#!/usr/bin/env python3
"""Run the production archive with a fake clock and real SQLite transactions."""

import contextlib
import importlib.util
import io
import json
import os
from pathlib import Path
import queue
import sqlite3
import sys
import tempfile
import threading
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from unittest.mock import patch

path = Path(__file__).parents[1] / "tools" / "rolling_logs" / "recorder.py"
spec = importlib.util.spec_from_file_location("rolling_recorder", path)
recorder = importlib.util.module_from_spec(spec)
sys.modules[spec.name] = recorder
spec.loader.exec_module(recorder)


with tempfile.TemporaryDirectory() as temporary:
    clock = [1_800_000_000.0]
    db_path = Path(temporary) / "logs.sqlite3"
    store = recorder.Store(db_path, now=lambda: clock[0], min_free=0)
    boundary = clock[0] - recorder.RETENTION
    secrets = {"password": "password-secret", "nested": [{"api_key": "api-secret"}],
               "pairing_number": "pairing-secret", "pin": "pin-secret", "wifi_psk": "psk-secret",
               "message": "Bearer bearer-secret https://user:url-secret@example.test/ "
                          "password=text-secret token=query-secret "
                          + ".".join(("eyJhbGciOiJIUzI1NiJ9", "eyJzdWIiOiIxIn0", "jwt-signature")),
               "embedded": json.dumps({"refresh_token": "refresh-secret"}),
               "ordinary": "volume=20 source=Netflix"}
    secrets["spaced"] = "password: my secret phrase"
    secrets["prefixed"] = "mqtt_password=prefixed-secret volume=20"
    secrets["legacy_pin"] = "Input pin code: legacy-pin-secret"
    store.write([
        recorder.Event("journal", {"message": "too old"}, boundary - 0.001,
                       key="old", checkpoint=("journal", "cursor-old")),
        recorder.Event("journal", secrets, boundary, original_time=recorder.utc(boundary),
                       key="boundary", checkpoint=("journal", "cursor-boundary")),
        recorder.Event("esp32", {"uptime": 10}, clock[0]),
        recorder.Event("journal", {"message": "broken clock"}, clock[0] + 10**9,
                       original_time="future-source-time", key="future"),
        recorder.Event("docker:sample", {"message": "first"}, clock[0] - 3,
                       key="docker-first", checkpoint=("docker:sample", {"ts": clock[0] - 3, "value": "later"})),
        recorder.Event("docker:sample", {"message": "second"}, clock[0] - 4,
                       key="docker-second", checkpoint=("docker:sample", {"ts": clock[0] - 4, "value": "earlier"})),
    ])
    rows = store.db.execute("SELECT timestamp,source,payload,original_time FROM events").fetchall()
    assert len(rows) == 5, rows
    saved = " ".join(row[2] for row in rows)
    for secret in ("password-secret", "api-secret", "bearer-secret", "url-secret",
                   "text-secret", "query-secret", "refresh-secret", "jwt-signature",
                   "pairing-secret", "pin-secret", "psk-secret"):
        assert secret not in saved, secret
    assert "my secret phrase" not in saved and "prefixed-secret" not in saved
    assert "legacy-pin-secret" not in saved
    assert "mqtt_password=[REDACTED] volume=20" in saved
    assert "volume=20 source=Netflix" in saved
    binary_journal = recorder.redact(recorder.journal_payload({"MESSAGE": list(
        b"password=binary-secret\x00full message")}))
    assert "binary-secret" not in json.dumps(binary_journal)
    assert "full message" in json.dumps(binary_journal)
    future = next(row for row in rows if row[3] == "future-source-time")
    assert future[0] == clock[0]
    assert store.checkpoint("journal") == "cursor-boundary"
    assert store.checkpoint("docker:sample")["value"] == "later"
    assert db_path.stat().st_mode & 0o777 == 0o600
    store.close()

    # Durable checkpoint and dedup survive opening the recorder after a crash/restart.
    store = recorder.Store(db_path, now=lambda: clock[0], min_free=0)
    assert store.checkpoint("journal") == "cursor-boundary"
    store.write([recorder.Event("journal", secrets, boundary, key="boundary",
                               checkpoint=("journal", "cursor-boundary"))])
    assert store.db.execute("SELECT COUNT(*) FROM events").fetchone()[0] == 5

    # Failed insertion must not advance its cursor, either on disk or in memory.
    store.db.execute("CREATE TRIGGER reject_bad BEFORE INSERT ON events "
                     "WHEN NEW.source='bad' BEGIN SELECT RAISE(ABORT,'test failure'); END")
    acknowledgements = []
    try:
        store.write([recorder.Event("bad", {}, key="bad",
                                   checkpoint=("journal", "cursor-uncommitted"),
                                   after_commit=lambda: acknowledgements.append("bad"))])
    except sqlite3.IntegrityError:
        pass
    else:
        raise AssertionError("failed insertion succeeded")
    assert store.checkpoint("journal") == "cursor-boundary"
    assert not acknowledgements
    store.write([recorder.Event("mqtt", {"payload": "message"},
                               after_commit=lambda: acknowledgements.append("durable"))])
    assert acknowledgements == ["durable"]
    assert json.loads(store.db.execute("SELECT value FROM checkpoints WHERE name='journal'").fetchone()[0]) == "cursor-boundary"

    # Idle maintenance, not new traffic, expires rows. The exact boundary survives.
    clock[0] += 61
    store.maintain()
    assert not store.db.execute("SELECT 1 FROM events WHERE event_key='boundary'").fetchone()
    clock[0] += recorder.RETENTION
    store.maintain()
    assert store.db.execute("SELECT COUNT(*) FROM events").fetchone()[0] == 0
    store.close()

    # Explicit budget failures retain fresh logs instead of shortening the window.
    clock[0] = 1_800_000_000.0
    store = recorder.Store(db_path, now=lambda: clock[0], min_free=0)
    store.write([recorder.Event("fresh", {"message": "keep me"}, key="fresh")])
    store.max_bytes = 1
    try:
        store.write([recorder.Event("new", {}, checkpoint=("journal", "cursor-lost"))])
    except recorder.ArchiveError:
        pass
    else:
        raise AssertionError("archive budget was ignored")
    assert store.db.execute("SELECT COUNT(*) FROM events WHERE event_key='fresh'").fetchone()[0] == 1
    assert store.checkpoint("journal") == "cursor-boundary"
    store.max_bytes = recorder.ARCHIVE_BYTES
    store.min_free = 10**30
    try:
        store.check_capacity()
    except recorder.ArchiveError:
        pass
    else:
        raise AssertionError("free-space reserve was ignored")
    store.close()

    # Expiration reclaims allocated pages so a previously full budget can resume.
    store = recorder.Store(Path(temporary) / "vacuum.sqlite3", now=lambda: clock[0], min_free=0)
    assert store.db.execute("PRAGMA auto_vacuum").fetchone()[0] == 2
    store.write([recorder.Event("large", {"message": "x" * 4096}) for _ in range(50)])
    store.db.execute("PRAGMA wal_checkpoint(TRUNCATE)")
    allocated = store.path.stat().st_size
    store.max_bytes = allocated + 1
    try:
        store.write([recorder.Event("fresh", {"message": "new"})])
    except recorder.ArchiveError:
        pass
    else:
        raise AssertionError("test budget did not fill")
    clock[0] += recorder.RETENTION + 61
    store.maintain()
    assert store.path.stat().st_size < allocated
    store.write([recorder.Event("fresh", {"message": "resumed"})])
    assert store.db.execute("SELECT COUNT(*) FROM events").fetchone()[0] == 1
    store.close()

    # Exercise the actual journal reader's expired-cursor fallback and atomic resume.
    store = recorder.Store(Path(temporary) / "readers.sqlite3", min_free=0)
    store.write([recorder.Event("setup", {}, checkpoint=("journal", "expired-cursor"))])
    reader = recorder.Recorder(store)
    commands = []
    def journal_lines(command, source):
        commands.append(command)
        if len(commands) == 1:
            yield "Failed to seek to cursor: expired"
        else:
            yield json.dumps({"__REALTIME_TIMESTAMP": str(int(store.now() * 1_000_000)),
                              "__CURSOR": "new-cursor", "MESSAGE": "actual journal message"})
            reader.stop.set()
    with patch.object(reader, "lines", journal_lines), patch.object(reader.stop, "wait", return_value=False):
        reader.journal()
    assert "--after-cursor=expired-cursor" in commands[0]
    assert "--since=-72h" in commands[1]
    while not reader.events.queue.empty():
        store.write([reader.events.get(timeout=0)])
    assert store.checkpoint("journal") == "new-cursor"

    # The real Docker parser retains repeated same-timestamp lines once across replay.
    reader = recorder.Recorder(store)
    original = recorder.utc(store.now() - 1)
    docker_commands = []
    def docker_lines(command, source):
        docker_commands.append(command)
        yield original + " stdout and stderr both arrive here"
        yield original + " stdout and stderr both arrive here"
    with patch.object(reader, "lines", docker_lines):
        for _ in range(2):
            reader.docker_reader("sample", "example")
            while not reader.events.queue.empty():
                store.write([reader.events.get(timeout=0)])
    assert store.db.execute("SELECT COUNT(*) FROM events WHERE source='docker:sample'").fetchone()[0] == 2
    assert "--timestamps" in docker_commands[0]
    assert recorder.timestamp(docker_commands[1][docker_commands[1].index("--since") + 1]) <= recorder.timestamp(original)

    # Real child-process pipes include stderr and terminate when SIGTERM-style stop arrives.
    reader = recorder.Recorder(store)
    child = [sys.executable, "-u", "-c",
             "import sys,time; print('stdout'); print('stderr',file=sys.stderr); time.sleep(60)"]
    stream = reader.lines(child, "test-child")
    assert {next(stream), next(stream)} == {"stdout", "stderr"}
    reader.shutdown()
    stream.close()
    assert not reader.processes

    # Exercise authenticated ESP text paging, overflow, boot changes and durable cursors.
    reader = recorder.Recorder(store)
    def page(boot, records, *, oldest=1, next_seq=2, dropped=0):
        return {"boot_id": boot, "oldest_seq": oldest, "next_seq": next_seq,
                "dropped": dropped, "records": records}
    first_page = page("aa", [{"seq": 2, "uptime_ms": 200, "text": "first"},
                             {"seq": 3, "uptime_ms": 300, "text": "second"}],
                      oldest=2, next_seq=5, dropped=1)
    responses = [first_page, first_page,
                 page("aa", [{"seq": 4, "uptime_ms": 400, "text": "third"}],
                      oldest=2, next_seq=5, dropped=1),
                 page("bb", []),
                 page("bb", [{"seq": 1, "uptime_ms": 10, "text": "mqtt_password=esp-secret volume=20"}])]
    requests = []
    class LogOpener:
        def open(self, request, timeout):
            requests.append(request)
            response = io.BytesIO(json.dumps(responses.pop(0)).encode())
            response.headers = {"Content-Type": "application/json"}
            return response
    def finish_pages(seconds):
        if not responses:
            reader.stop.set()
        return reader.stop.is_set()
    with patch.dict(os.environ, {"ESP32_URL": "http://example.test", "ESP32_TOKEN": "auth-secret", "ESP32_TEXT_ENABLED": "1"}), \
         patch.object(recorder.urllib.request, "build_opener", return_value=LogOpener()), \
         patch.object(reader.stop, "wait", side_effect=finish_pages):
        reader.esp32_text()
    assert all(request.get_header("Authorization") == "Bearer auth-secret" for request in requests)
    assert "after=3" in requests[2].full_url
    assert "after=4" in requests[3].full_url and "after=0" in requests[4].full_url
    while not reader.events.queue.empty():
        store.write([reader.events.get(timeout=0)])
    assert store.checkpoint("esp32_text") == {"boot_id": "bb", "after": 1, "dropped": 0}
    text_rows = store.db.execute("SELECT payload FROM events WHERE source='esp32_text'").fetchall()
    assert len(text_rows) == 4 and "esp-secret" not in json.dumps(text_rows)
    health = " ".join(row[0] for row in store.db.execute("SELECT payload FROM events WHERE source='recorder'"))
    assert '"status":"gap"' in health and '"status":"ring_overwrite"' in health

    reader = recorder.Recorder(store)
    with patch.dict(os.environ, {"ESP32_URL": "http://example.test", "ESP32_TOKEN": "auth-secret", "ESP32_TEXT_ENABLED": "1"}), \
         patch.object(recorder.urllib.request, "build_opener") as factory, \
         patch.object(reader.stop, "wait", side_effect=lambda seconds: reader.stop.set()) as wait:
        factory.return_value.open.side_effect = recorder.urllib.error.HTTPError(
            "http://user:private-error@example.test", 404, "private-error", {}, None)
        reader.esp32_text()
        assert "after=1" in factory.return_value.open.call_args.args[0].full_url
        wait.assert_called_once_with(60)
    while not reader.events.queue.empty():
        store.write([reader.events.get(timeout=0)])
    health = " ".join(row[0] for row in store.db.execute("SELECT payload FROM events WHERE source='recorder'"))
    assert "private-error" not in health and '"status":"not_supported"' in health
    reader = recorder.Recorder(store)
    html = io.BytesIO(b"<html>must not read this body</html>")
    html.headers = {"Content-Type": "text/html"}
    with patch.dict(os.environ, {"ESP32_URL": "http://example.test", "ESP32_TOKEN": "auth-secret", "ESP32_TEXT_ENABLED": "1"}), \
         patch.object(recorder.urllib.request, "build_opener") as factory, \
         patch.object(html, "read", side_effect=AssertionError("HTML body was read")), \
         patch.object(reader.stop, "wait", side_effect=lambda seconds: reader.stop.set()) as wait:
        factory.return_value.open.return_value = html
        reader.esp32_text()
        wait.assert_called_once_with(60)
    reader = recorder.Recorder(store)
    with patch.dict(os.environ, {"ESP32_TEXT_ENABLED": "0"}), \
         patch.object(recorder.urllib.request, "build_opener") as factory, \
         patch.object(reader.stop, "wait", side_effect=lambda: reader.stop.set()):
        reader.esp32_text()
        factory.assert_not_called()
    # Ring overwrite during a page can skip sequences inside that page too.
    reader = recorder.Recorder(store)
    jumped_page = page("cc", [{"seq": 1, "uptime_ms": 10, "text": "before"},
                              {"seq": 3, "uptime_ms": 30, "text": "after"}], next_seq=4)
    responses = [jumped_page, jumped_page]
    with patch.dict(os.environ, {"ESP32_URL": "http://example.test", "ESP32_TOKEN": "auth-secret", "ESP32_TEXT_ENABLED": "1"}), \
         patch.object(recorder.urllib.request, "build_opener", return_value=LogOpener()), \
         patch.object(reader.stop, "wait", side_effect=finish_pages):
        reader.esp32_text()
    while not reader.events.queue.empty():
        store.write([reader.events.get(timeout=0)])
    gaps = [json.loads(row[0]) for row in store.db.execute("SELECT payload FROM events WHERE source='recorder'")]
    assert [item for item in gaps if item.get("boot_id") == "cc" and item.get("status") == "gap"] == [
        {"reader": "esp32_text", "status": "gap", "boot_id": "cc",
         "first_missing": 2, "last_missing": 2, "dropped": 0}]
    store.close()

    # Queue overflow is explicit, queue storage is bounded, and a stop unblocks producers.
    stop = threading.Event()
    events = recorder.EventQueue(stop, max_bytes=2048)
    assert events.put(recorder.Event("test", {"message": "small"}))
    blocked = threading.Thread(target=lambda: events.put(recorder.Event("test", {"message": "small"})))
    blocked.start()
    blocked.join(timeout=0.05)
    assert blocked.is_alive()
    stop.set()
    blocked.join(timeout=2)
    assert not blocked.is_alive()
    assert events.bytes <= events.max_bytes and events.queue.qsize() == 1
    assert events.get().payload["message"] == "small"
    try:
        events.put(recorder.Event("large", {"message": "x" * 4096}))
    except recorder.ArchiveError:
        pass
    else:
        raise AssertionError("oversized event was accepted")

    # Service errors cannot leak credentials embedded in exception URLs/text.
    output = io.StringIO()
    with patch.dict(os.environ, {"ESP32_URL": "http://example.test", "MQTT_HOST": "example.test",
                                 "MQTT_USERNAME": "test", "MQTT_PASSWORD": "test"}), \
         patch.object(recorder, "Store", side_effect=RuntimeError("https://user:exception-secret@example.test token=exception-secret")), \
         contextlib.redirect_stdout(output):
        assert recorder.main() == 1
    assert "exception-secret" not in output.getvalue()

    # Actual urllib must reject the old firmware's redirect without reading its Web UI.
    visits = []
    class OldFirmware(BaseHTTPRequestHandler):
        def do_GET(self):
            visits.append(self.path)
            if self.path.startswith(("/api/logs", "/api/state")):
                self.send_response(302)
                self.send_header("Location", "/")
                self.end_headers()
            else:
                self.send_response(200)
                self.send_header("Content-Type", "text/html")
                self.end_headers()
                self.wfile.write(b"<html>old firmware</html>")
        def log_message(self, *args):
            pass
    server = ThreadingHTTPServer(("127.0.0.1", 0), OldFirmware)
    server_thread = threading.Thread(target=server.serve_forever)
    server_thread.start()
    try:
        base = f"http://127.0.0.1:{server.server_port}"
        # Reproduce the unsafe stock urllib behavior as the regression baseline.
        with recorder.urllib.request.build_opener(recorder.urllib.request.ProxyHandler({})).open(base + "/api/logs") as response:
            assert response.read().startswith(b"<html>")
        visits.clear()
        store = recorder.Store(Path(temporary) / "redirect.sqlite3", min_free=0)
        reader = recorder.Recorder(store)
        with patch.dict(os.environ, {"ESP32_URL": base, "ESP32_TOKEN": "auth-secret", "ESP32_TEXT_ENABLED": "1"}), \
             patch.object(reader.stop, "wait", side_effect=lambda seconds: reader.stop.set()) as wait:
            reader.esp32_text()
            wait.assert_called_once_with(60)
        assert len(visits) == 1 and visits[0].startswith("/api/logs")
        visits.clear()
        reader = recorder.Recorder(store)
        with patch.dict(os.environ, {"ESP32_URL": base}), \
             patch.object(reader.stop, "wait", side_effect=lambda seconds: reader.stop.set()):
            reader.esp32()
        assert visits == ["/api/state"]
        store.close()
    finally:
        server.shutdown()
        server.server_close()
        server_thread.join(timeout=2)

print("Rolling log checks passed: retention, resume, dedup, redaction, permissions, bounds and safe failures")
