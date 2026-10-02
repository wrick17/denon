#!/usr/bin/env python3
"""Read-only 72-hour journal, Docker, MQTT and ESP32 state archive."""

from __future__ import annotations

import hashlib
import json
import math
import os
from pathlib import Path
import queue
import re
import shutil
import signal
import sqlite3
import subprocess
import threading
import time
import urllib.error
import urllib.request
from dataclasses import dataclass
from datetime import datetime, timezone

RETENTION = 72 * 3600
PRUNE_SECONDS = 60
QUEUE_BYTES = 32 * 1024 * 1024
ESP_TEXT_BYTES = 16 * 1024
ARCHIVE_BYTES = 4 * 1024**3
MIN_FREE_BYTES = 1024**3
SECRET_KEY = re.compile(
    r"(?:password|passwd|pwd|secret|token|authorization|api[_-]?key|"
    r"access[_-]?key|private[_-]?key|credential|cookie|pairing[_-]?number|"
    r"(?:^|[_-])(?:pin|psk)(?:$|[_-]))", re.I
)
TEXT_SECRET = re.compile(
    r'''(?i)(\b(?:[a-z][a-z0-9]*[_-])?(?:password|passwd|pwd|secret|(?:access[_-]?|refresh[_-]?)?token|'''
    r'''api[_-]?key|authorization|cookie|pairing[_-]?number|pin(?:[ _-]?code)?|psk)\b["']?\s*[:=]\s*)'''
    r'''(?:"[^"\n]*"|'[^'\n]*'|[^\n&,;\x00]+?'''
    r'''(?=\s+[\w.-]+\s*[:=]|[\n&,;\x00]|$))'''
)
BEARER = re.compile(r"(?i)\b(Bearer|Basic)\s+[A-Za-z0-9._~+/=-]+")
JWT = re.compile(r"\beyJ[A-Za-z0-9_-]+\.[A-Za-z0-9_-]+\.[A-Za-z0-9_-]+\b")
URL_USERINFO = re.compile(r"(\b[a-zA-Z][a-zA-Z0-9+.-]*://)[^\s/@]+@")


class ArchiveError(RuntimeError):
    """Fixed, non-sensitive fatal diagnostics safe for the service journal."""


class NoRedirect(urllib.request.HTTPRedirectHandler):
    def redirect_request(self, request, fp, code, message, headers, new_url):
        fp.close()
        raise urllib.error.HTTPError(request.full_url, code, "source redirect rejected", headers, None)


def redact(value):
    if isinstance(value, dict):
        return {key: "[REDACTED]" if SECRET_KEY.search(str(key)) else redact(item)
                for key, item in value.items()}
    if isinstance(value, list):
        return [redact(item) for item in value]
    if isinstance(value, str):
        # MQTT and journal MESSAGE fields often embed another JSON document.
        try:
            embedded = json.loads(value)
        except (ValueError, TypeError):
            embedded = None
        if isinstance(embedded, (dict, list)):
            return json.dumps(redact(embedded), ensure_ascii=True, separators=(",", ":"))
        value = URL_USERINFO.sub(r"\1[REDACTED]@", value)
        value = BEARER.sub(r"\1 [REDACTED]", value)
        value = JWT.sub("[REDACTED]", value)
        return TEXT_SECRET.sub(r"\1[REDACTED]", value)
    return value


def utc(timestamp=None):
    return datetime.fromtimestamp(time.time() if timestamp is None else timestamp,
                                  timezone.utc).isoformat().replace("+00:00", "Z")


def timestamp(value):
    return datetime.fromisoformat(value.replace("Z", "+00:00")).timestamp()


def journal_payload(record):
    # journalctl represents binary fields as byte arrays. Decode them reversibly
    # so embedded credentials receive the same redaction as ordinary MESSAGEs.
    return {key: {"encoding": "utf8-backslashreplace", "value": bytes(value).decode(
                    "utf-8", errors="backslashreplace")}
            if isinstance(value, list) and value and
               all(type(item) is int and 0 <= item <= 255 for item in value)
            else value for key, value in record.items()}


@dataclass
class Event:
    source: str
    payload: object
    occurred: float | None = None
    original_time: str | None = None
    key: str | None = None
    checkpoint: tuple[str, object] | None = None
    queue_bytes: int = 0
    after_commit: object = None


class Store:
    """Only the main thread writes; reader checkpoints are committed with their rows."""

    def __init__(self, path, *, now=time.time, min_free=MIN_FREE_BYTES,
                 max_bytes=ARCHIVE_BYTES):
        self.path = Path(path)
        self.path.parent.mkdir(parents=True, exist_ok=True, mode=0o700)
        self.now, self.min_free, self.max_bytes = now, min_free, max_bytes
        self.lock = threading.Lock()
        self.checkpoints = {}
        # Create with private permissions before SQLite can write any content.
        fd = os.open(self.path, os.O_CREAT | os.O_RDWR, 0o600)
        os.fchmod(fd, 0o600)
        os.close(fd)
        self.db = sqlite3.connect(self.path)
        # New archives can reclaim old pages without copying the entire database.
        self.db.execute("PRAGMA auto_vacuum=INCREMENTAL")
        self.db.execute("PRAGMA journal_mode=WAL")
        self.db.execute("PRAGMA synchronous=FULL")
        self.db.execute("PRAGMA busy_timeout=5000")
        self.db.executescript("""
            CREATE TABLE IF NOT EXISTS events (
                id INTEGER PRIMARY KEY, timestamp REAL NOT NULL,
                observed REAL NOT NULL, source TEXT NOT NULL,
                original_time TEXT, payload TEXT NOT NULL, event_key TEXT UNIQUE
            );
            CREATE INDEX IF NOT EXISTS events_timestamp ON events(timestamp);
            CREATE INDEX IF NOT EXISTS events_source ON events(source, timestamp);
            CREATE TABLE IF NOT EXISTS checkpoints (name TEXT PRIMARY KEY, value TEXT NOT NULL);
        """)
        self.checkpoints = {name: json.loads(value) for name, value in
                            self.db.execute("SELECT name, value FROM checkpoints")}
        self.next_prune = 0
        self.maintain()
        self.private_files()

    def private_files(self):
        for suffix in ("", "-wal", "-shm"):
            path = Path(str(self.path) + suffix)
            if path.exists():
                path.chmod(0o600)

    def check_capacity(self, incoming=0):
        size = sum(path.stat().st_size for path in
                   (self.path, Path(str(self.path) + "-wal"), Path(str(self.path) + "-shm"))
                   if path.exists())
        if size + incoming > self.max_bytes:
            raise ArchiveError("archive budget exhausted; fresh records were not deleted")
        if shutil.disk_usage(self.path.parent).free - incoming < self.min_free:
            raise ArchiveError("archive free-space reserve exhausted")

    def checkpoint(self, name):
        with self.lock:
            return self.checkpoints.get(name)

    def write(self, events):
        now = self.now()
        rows, updates = [], {}
        for event in events:
            occurred = now if event.occurred is None else event.occurred
            # Keep the original timestamp, but don't let a broken source clock
            # retain future-dated rows forever.
            if not math.isfinite(occurred) or occurred > now + 300:
                occurred = now
            payload = json.dumps(redact(event.payload), ensure_ascii=True,
                                 separators=(",", ":"))
            if occurred >= now - RETENTION:
                rows.append((occurred, now, event.source, event.original_time,
                             payload, event.key))
            if event.checkpoint:
                name, value = event.checkpoint
                if isinstance(value, dict) and "ts" in value and (not math.isfinite(value["ts"]) or value["ts"] > now + 300):
                    value = {**value, "ts": now}
                previous = updates.get(name, self.checkpoint(name))
                if not isinstance(value, dict) or "ts" not in value or not previous or value["ts"] >= previous["ts"]:
                    updates[name] = value
        self.maintain()
        self.check_capacity(sum(len(row[4].encode()) * 2 + 1024 for row in rows))
        with self.db:
            self.db.executemany("INSERT OR IGNORE INTO events "
                                "(timestamp,observed,source,original_time,payload,event_key) "
                                "VALUES (?,?,?,?,?,?)", rows)
            self.db.executemany("INSERT OR REPLACE INTO checkpoints VALUES (?,?)",
                                [(name, json.dumps(value)) for name, value in updates.items()])
        with self.lock:
            self.checkpoints.update(updates)
        self.private_files()
        for event in events:
            if event.after_commit:
                event.after_commit()

    def maintain(self):
        now = self.now()
        if now >= self.next_prune:
            with self.db:
                self.db.execute("DELETE FROM events WHERE timestamp < ?", (now - RETENTION,))
                self.db.execute("PRAGMA incremental_vacuum").fetchall()
            # Reclaim expired pages and WAL space before checking the archive budget.
            self.db.execute("PRAGMA wal_checkpoint(TRUNCATE)")
            self.next_prune = now + PRUNE_SECONDS
            self.check_capacity()

    def close(self):
        self.db.close()


class EventQueue:
    """Backpressure rather than dropped messages; memory is bounded by encoded bytes."""

    def __init__(self, stop, max_bytes=QUEUE_BYTES):
        self.stop, self.max_bytes = stop, max_bytes
        self.queue = queue.Queue(maxsize=256)
        self.condition = threading.Condition()
        self.bytes = 0

    def put(self, event):
        # Redact before both queueing and serializing to disk.
        event.payload = redact(event.payload)
        size = len(json.dumps(event.payload, ensure_ascii=True).encode()) + 1024
        event.queue_bytes = size
        if size > self.max_bytes:
            raise ArchiveError("source event exceeds bounded archive queue")
        with self.condition:
            while self.bytes + size > self.max_bytes and not self.stop.is_set():
                self.condition.wait(0.5)
            if self.stop.is_set():
                return False
            self.bytes += size
        while not self.stop.is_set():
            try:
                self.queue.put((event, size), timeout=0.5)
                return True
            except queue.Full:
                pass
        with self.condition:
            self.bytes -= size
            self.condition.notify_all()
        return False

    def get(self, timeout=0.5):
        event, size = self.queue.get(timeout=timeout)
        with self.condition:
            self.bytes -= size
            self.condition.notify_all()
        return event


class Recorder:
    def __init__(self, store):
        self.store = store
        self.stop = threading.Event()
        self.events = EventQueue(self.stop)
        self.processes = set()
        self.process_lock = threading.Lock()
        self.threads = []
        self.thread_lock = threading.Lock()
        self.failure = None

    def emit(self, source, payload, **kwargs):
        return self.events.put(Event(source, payload, **kwargs))

    def health(self, source, status, **details):
        self.emit("recorder", {"reader": source, "status": status, **details})

    def start(self, name, function, *args):
        def run():
            try:
                function(*args)
            except Exception as error:
                # Never print exception text: urllib and libraries can include credentials.
                detail = str(error) if isinstance(error, ArchiveError) else type(error).__name__
                self.health(name, "fatal_reader_error", error_type=type(error).__name__)
                self.failure = f"{name}: {detail}"
                self.stop.set()
        thread = threading.Thread(target=run, name=name)
        with self.thread_lock:
            self.threads = [item for item in self.threads if item.is_alive()]
            self.threads.append(thread)
            thread.start()
        return thread

    def lines(self, command, source):
        process = subprocess.Popen(command, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
        with self.process_lock:
            self.processes.add(process)
        try:
            assert process.stdout is not None
            while not self.stop.is_set():
                # An oversized message fails explicitly instead of truncating full logs.
                line = process.stdout.readline(QUEUE_BYTES + 1)
                if not line:
                    break
                if len(line) > QUEUE_BYTES:
                    raise ArchiveError("source line exceeds bounded archive queue")
                yield line.decode("utf-8", errors="backslashreplace").rstrip("\n")
            if not self.stop.is_set():
                self.health(source, "reader_exit", exit_code=process.wait())
        finally:
            if process.poll() is None:
                process.terminate()
            try:
                process.wait(timeout=3)
            except subprocess.TimeoutExpired:
                process.kill()
                process.wait()
            process.stdout.close()
            with self.process_lock:
                self.processes.discard(process)

    def journal(self):
        fallback = False
        while not self.stop.is_set():
            cursor = None if fallback else self.store.checkpoint("journal")
            command = ["journalctl", "--follow", "--all", "--output=json", "--no-pager"]
            command += [f"--after-cursor={cursor}"] if cursor else ["--since=-72h"]
            self.health("journal", "starting", resume=bool(cursor))
            try:
                for line in self.lines(command, "journal"):
                    try:
                        record = json.loads(line)
                        occurred = int(record["__REALTIME_TIMESTAMP"]) / 1_000_000
                        event_cursor = record["__CURSOR"]
                    except (ValueError, KeyError, TypeError):
                        self.health("journal", "reader_message", message=line)
                        continue
                    self.emit("journal", journal_payload(record), occurred=occurred, original_time=utc(occurred),
                              key="journal:" + event_cursor,
                              checkpoint=("journal", event_cursor))
            except OSError as error:
                self.health("journal", "reader_error", error_type=type(error).__name__)
            # Expired/invalid cursors fall back to the retained time window.
            fallback = bool(cursor)
            if not self.stop.is_set():
                self.health("journal", "retry", time_window_fallback=fallback)
            self.stop.wait(5)

    def docker_reader(self, container_id, name):
        source = "docker:" + container_id
        try:
            saved = self.store.checkpoint(source)
            since = max(time.time() - RETENTION, saved["ts"] - 5 if saved else 0)
            counts, high, cleaned = {}, since, since
            self.health(source, "starting", container_name=name, resume=bool(saved))
            for line in self.lines(["docker", "logs", "--follow", "--timestamps",
                                    "--since", utc(since), container_id], source):
                original, separator, message = line.partition(" ")
                try:
                    occurred = timestamp(original)
                    if not separator:
                        raise ValueError
                except ValueError:
                    self.health(source, "reader_message", message=line)
                    continue
                high = max(high, occurred)
                # Replay five seconds on reconnect; hash+occurrence keeps identical
                # messages with identical timestamps while deduplicating backfill.
                if high >= cleaned + 1:
                    counts = {key: value for key, value in counts.items() if key[0] >= high - 5}
                    cleaned = high
                digest = hashlib.sha256(line.encode()).hexdigest()
                identity = (occurred, digest)
                counts[identity] = counts.get(identity, 0) + 1
                if len(counts) > 100_000:
                    raise ArchiveError("Docker replay deduplication window exhausted")
                self.emit(source, {"container_name": name, "message": message},
                          occurred=occurred, original_time=original,
                          key=f"{source}:{digest}:{counts[identity]}",
                          checkpoint=(source, {"ts": occurred, "value": original}))
        except OSError as error:
            self.health(source, "reader_error", error_type=type(error).__name__)

    def docker(self):
        readers = {}
        stopped_seen = set()
        while not self.stop.is_set():
            try:
                result = subprocess.run(["docker", "ps", "--all", "--no-trunc", "--format",
                                         "{{.ID}}\t{{.Names}}"], capture_output=True,
                                        text=True, timeout=10)
                if result.returncode:
                    self.health("docker", "discovery_error", exit_code=result.returncode,
                                message=result.stderr)
                else:
                    containers = [line.split("\t", 1) for line in result.stdout.splitlines()]
                    if not containers:
                        self.stop.wait(30)
                        continue
                    inspected = subprocess.run(["docker", "inspect", "--format",
                                                "{{.Id}}\t{{.HostConfig.LogConfig.Type}}\t{{.State.Running}}",
                                                *[item[0] for item in containers]],
                                               capture_output=True, text=True, timeout=10)
                    if inspected.returncode:
                        self.health("docker", "discovery_error", message=inspected.stderr,
                                    exit_code=inspected.returncode)
                        self.stop.wait(5)
                        continue
                    drivers = {fields[0]: fields[1:] for fields in
                               (line.split("\t") for line in inspected.stdout.splitlines())}
                    for container_id, name in containers:
                        driver, running = drivers[container_id]
                        if driver == "journald":
                            continue  # Full system journal already includes these containers.
                        if running == "false" and container_id in stopped_seen:
                            continue
                        if container_id not in readers or not readers[container_id].is_alive():
                            readers[container_id] = self.start("docker:" + container_id,
                                                               self.docker_reader, container_id, name)
                            if running == "false":
                                stopped_seen.add(container_id)
                    stopped_seen.intersection_update(item[0] for item in containers)
                    readers = {key: value for key, value in readers.items() if value.is_alive()}
            except (OSError, subprocess.TimeoutExpired, ValueError) as error:
                self.health("docker", "discovery_error", error_type=type(error).__name__)
            self.stop.wait(30)

    def esp32(self):
        url = os.environ["ESP32_URL"].rstrip("/") + "/api/state"
        if not url.startswith(("http://", "https://")):
            raise ValueError("ESP32_URL must be HTTP or HTTPS")
        # Ignore proxy environment variables for this private, read-only endpoint.
        opener = urllib.request.build_opener(urllib.request.ProxyHandler({}), NoRedirect())
        while not self.stop.is_set():
            started = time.monotonic()
            try:
                with opener.open(url, timeout=3) as response:
                    raw = response.read(QUEUE_BYTES + 1)
                if len(raw) > QUEUE_BYTES:
                    raise ArchiveError("ESP32 state exceeds bounded archive queue")
                self.emit("esp32", json.loads(raw))
            except (OSError, ValueError) as error:
                self.health("esp32", "request_error", error_type=type(error).__name__)
            self.stop.wait(max(0, 2 - (time.monotonic() - started)))

    def esp32_text(self):
        if os.environ.get("ESP32_TEXT_ENABLED", "0") != "1":
            self.health("esp32_text", "disabled")
            self.stop.wait()
            return
        base = os.environ["ESP32_URL"].rstrip("/")
        token = os.environ.get("ESP32_TOKEN")
        if not token:
            self.health("esp32_text", "authentication_not_configured")
            self.stop.wait()
            return
        opener = urllib.request.build_opener(urllib.request.ProxyHandler({}), NoRedirect())
        saved = self.store.checkpoint("esp32_text") or {"boot_id": None, "after": 0, "dropped": 0}
        while not self.stop.is_set():
            try:
                request = urllib.request.Request(
                    f"{base}/api/logs?after={saved['after']}&limit=16",
                    headers={"Authorization": "Bearer " + token})
                with opener.open(request, timeout=3) as response:
                    content_type = response.headers.get("Content-Type", "").split(";", 1)[0].strip().lower()
                    raw = response.read(ESP_TEXT_BYTES + 1) if content_type == "application/json" else None
                if raw is None:
                    self.health("esp32_text", "not_supported", reason="non_json_response")
                    self.stop.wait(60)
                    continue
                if len(raw) > ESP_TEXT_BYTES:
                    self.health("esp32_text", "not_supported", reason="oversized_response")
                    self.stop.wait(60)
                    continue
                page = json.loads(raw)
                if (not isinstance(page, dict) or not isinstance(page.get("boot_id"), str)
                        or not re.fullmatch(r"[0-9a-fA-F]{1,32}", page["boot_id"])
                        or any(type(page.get(key)) is not int or not 0 <= page[key] <= 0xFFFFFFFF
                               for key in ("oldest_seq", "next_seq", "dropped"))
                        or not isinstance(page.get("records"), list) or len(page["records"]) > 16):
                    raise ValueError("invalid ESP32 log page")
                if page["boot_id"] != saved["boot_id"]:
                    saved = {"boot_id": page["boot_id"], "after": 0, "dropped": 0}
                    self.emit("recorder", {"reader": "esp32_text", "status": "boot",
                                            "boot_id": page["boot_id"]},
                              checkpoint=("esp32_text", saved.copy()))
                    # A request with the previous boot's high cursor can be empty.
                    # Refetch from zero rather than treating it as a quiet source.
                    continue
                if page["oldest_seq"] > saved["after"] + 1:
                    self.health("esp32_text", "gap", boot_id=page["boot_id"],
                                first_missing=saved["after"] + 1,
                                last_missing=page["oldest_seq"] - 1, dropped=page["dropped"])
                if page["dropped"] != saved["dropped"]:
                    self.health("esp32_text", "ring_overwrite", dropped=page["dropped"],
                                boot_id=page["boot_id"])
                previous_seq = max(saved["after"], page["oldest_seq"] - 1)
                for record in page["records"]:
                    if (not isinstance(record, dict) or type(record.get("seq")) is not int
                            or not previous_seq < record["seq"] < page["next_seq"]
                            or type(record.get("uptime_ms")) is not int
                            or not 0 <= record["uptime_ms"] <= 0xFFFFFFFF
                            or not isinstance(record.get("text"), str)):
                        raise ValueError("invalid ESP32 log record")
                    if record["seq"] > previous_seq + 1:
                        self.health("esp32_text", "gap", boot_id=page["boot_id"],
                                    first_missing=previous_seq + 1,
                                    last_missing=record["seq"] - 1, dropped=page["dropped"])
                    previous_seq = record["seq"]
                    saved = {"boot_id": page["boot_id"], "after": previous_seq,
                             "dropped": page["dropped"]}
                    if not self.emit("esp32_text", {**page, "records": [record]},
                                     key=f"esp32_text:{page['boot_id']}:{previous_seq}",
                                     checkpoint=("esp32_text", saved.copy())):
                        return
                if not page["records"]:
                    saved = {**saved, "after": previous_seq, "dropped": page["dropped"]}
                if not page["records"] and previous_seq < page["next_seq"] - 1:
                    raise ValueError("ESP32 log page omitted available records")
                if previous_seq < page["next_seq"] - 1:
                    continue  # Drain a full ring page immediately before returning to polling.
                self.stop.wait(2)
            except urllib.error.HTTPError as error:
                unsupported = 300 <= error.code < 400 or error.code in (401, 403, 404)
                self.health("esp32_text", "not_supported" if unsupported else "http_error",
                            status_code=error.code)
                self.stop.wait(60 if unsupported else 5)
            except (OSError, ValueError, KeyError, TypeError) as error:
                self.health("esp32_text", "request_error", error_type=type(error).__name__)
                self.stop.wait(5)

    def mqtt(self):
        import paho.mqtt.client as mqtt

        client = mqtt.Client(mqtt.CallbackAPIVersion.VERSION2,
                             client_id="denon-rolling-logs", protocol=mqtt.MQTTv311,
                             clean_session=False, manual_ack=True)
        client.connect_timeout = 3
        client.username_pw_set(os.environ.get("MQTT_USERNAME"), os.environ.get("MQTT_PASSWORD"))
        client.reconnect_delay_set(min_delay=1, max_delay=30)

        def connected(client, userdata, flags, reason, properties):
            self.health("mqtt", "connected" if reason == 0 else "connect_error", reason=str(reason))
            if reason == 0:
                client.subscribe([("#", 1), ("$SYS/#", 1)])

        def disconnected(client, userdata, flags, reason, properties):
            self.health("mqtt", "disconnected", reason=str(reason))

        def message(client, userdata, item):
            # Preserve undecodable bytes as reversible backslash escapes, rather
            # than storing opaque base64 that would bypass text secret redaction.
            try:
                self.emit("mqtt", {"topic": item.topic,
                                   "payload": item.payload.decode("utf-8", errors="backslashreplace"),
                                   "qos": item.qos, "retain": item.retain, "duplicate": item.dup},
                          after_commit=(lambda: client.ack(item.mid, item.qos)) if item.qos else None)
            except Exception as error:
                self.failure = f"mqtt: {type(error).__name__}"
                self.stop.set()

        client.on_connect, client.on_disconnect, client.on_message = connected, disconnected, message
        client.on_connect_fail = lambda *_: self.health("mqtt", "connect_failed")
        client.on_subscribe = lambda client, userdata, mid, reasons, properties: self.health(
            "mqtt", "subscription_error" if any(reason.is_failure for reason in reasons)
            else "subscribed", reasons=[str(reason) for reason in reasons])
        client.connect_async(os.environ["MQTT_HOST"], int(os.environ.get("MQTT_PORT", "1883")), 60)
        client.loop_start()
        try:
            self.stop.wait()
        finally:
            client.disconnect()
            client.loop_stop()

    def shutdown(self):
        self.stop.set()
        with self.process_lock:
            for process in list(self.processes):
                if process.poll() is None:
                    process.terminate()
        with self.thread_lock:
            threads = list(self.threads)
        for thread in threads:
            thread.join(timeout=12)

    def run(self):
        self.health("recorder", "started")
        for name in ("journal", "docker", "esp32", "esp32_text", "mqtt"):
            self.start(name, getattr(self, name))
        try:
            next_health = time.monotonic() + 60
            while not self.stop.is_set():
                if time.monotonic() >= next_health:
                    self.store.write([Event("recorder", {"status": "running",
                                                        "queued_records": self.events.queue.qsize()})])
                    next_health = time.monotonic() + 60
                try:
                    batch = [self.events.get()]
                    batch_bytes = batch[0].queue_bytes
                    while len(batch) < 64 and batch_bytes < 1024 * 1024:
                        try:
                            event = self.events.get(timeout=0)
                            batch.append(event)
                            batch_bytes += event.queue_bytes
                        except queue.Empty:
                            break
                    self.store.write(batch)
                except queue.Empty:
                    self.store.maintain()
            if self.failure:
                self.store.write([Event("recorder", {"status": "reader_failure",
                                                     "error": self.failure})])
                raise ArchiveError(self.failure)
        except Exception:
            self.failure = self.failure or "writer failed"
            raise
        finally:
            self.shutdown()
            # Flush accepted records on orderly termination. A writer failure
            # deliberately leaves checkpoints behind, so backfill can retry them.
            if not self.failure:
                while not self.events.queue.empty():
                    self.store.write([self.events.get(timeout=0)])
                self.store.write([Event("recorder", {"status": "stopped"})])


def main():
    os.umask(0o077)
    store = None
    try:
        for name in ("ESP32_URL", "MQTT_HOST", "MQTT_USERNAME", "MQTT_PASSWORD"):
            if not os.environ.get(name):
                raise ValueError(f"missing {name}")
        store = Store(os.environ.get("LOG_DB", "/var/lib/denon-logs/logs.sqlite3"))
        recorder = Recorder(store)
        signal.signal(signal.SIGTERM, lambda *_: recorder.stop.set())
        signal.signal(signal.SIGINT, lambda *_: recorder.stop.set())
        recorder.run()
        return 0
    except Exception as error:
        # Fixed messages for our own guard failures; unknown exceptions expose
        # only their class, never payloads, endpoints, or credentials.
        detail = str(error) if isinstance(error, ArchiveError) else type(error).__name__
        print("rolling log recorder stopped: " + detail, flush=True)
        return 1
    finally:
        if store:
            store.close()


if __name__ == "__main__":
    raise SystemExit(main())
