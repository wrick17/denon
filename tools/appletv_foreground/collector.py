#!/usr/bin/env python3
"""Publish the foreground tvOS app from pymobiledevice3 syslog to MQTT."""

from __future__ import annotations

import asyncio
import json
import logging
import math
import os
from pathlib import Path
import re
import signal
import subprocess
import sys
import threading
import time
from datetime import datetime, timezone
from dataclasses import dataclass, field
from typing import Any
from uuid import uuid4


_SYMPTOMSD = re.compile(r"^([A-Za-z0-9._-]+): Foreground: (true|false)$")
_PINEBOARD_FOREGROUND = re.compile(
    r"^\[[^]]+:\(FBSceneManager\):([A-Za-z0-9._-]+)-default\] "
    r"Scene lifecycle state did change: Foreground$"
)
_APP_ID = re.compile(r"^[A-Za-z0-9._-]+$")
_DVT_INFRASTRUCTURE = {
    "com.apple.PineBoard",
    "com.apple.TVAirPlay",
    "com.apple.TVSystemUIService",
}
_DVT_OVERLAYS = {
    "com.apple.IdleScreen",
    "com.apple.IdleScreen.MemoryPoster",
}
_DVT_OVERLAY_NAMES = {"IdleScreen", "MemoryPoster"}
_NAMES = {
    "com.apple.HeadBoard": "Home",
    "com.google.ios.youtube": "YouTube",
    "com.netflix.Netflix": "Netflix",
    "com.spotify.client": "Spotify",
}
_VALUE_TEMPLATE = (
    "{{ value_json.state if value_json is defined else 'unknown' }}"
)
_RECONNECT_MAX_SECONDS = 30.0
_RECONNECT_STABLE_SECONDS = 30.0
_DVT_SETTLE_SECONDS = 3.0


def _app_name(app_id: str) -> str:
    return _NAMES.get(app_id, app_id.rsplit(".", 1)[-1])


def _observed_at() -> str:
    return datetime.now(timezone.utc).isoformat(timespec="milliseconds").replace(
        "+00:00", "Z"
    )


def _source_timestamp(record: dict[str, Any]) -> str:
    value = str(record.get("timestamp", "unknown")).strip()[:64]
    return re.sub(r"[^0-9A-Za-z:+.,_ /-]", "_", value) or "unknown"


def _source_datetime(
    record: dict[str, Any], local_timezone: Any = None
) -> datetime | None:
    try:
        value = datetime.fromisoformat(str(record["timestamp"]).strip())
    except (KeyError, TypeError, ValueError):
        return None
    if value.tzinfo is None:
        local_timezone = local_timezone or datetime.now().astimezone().tzinfo
        value = value.replace(tzinfo=local_timezone)
    return value.astimezone(timezone.utc)


def _reconnect_delay(base_seconds: float, failures: int) -> float:
    exponent = min(max(failures - 1, 0), 10)
    return min(base_seconds * (2**exponent), _RECONNECT_MAX_SECONDS)


def _next_failure_count(previous: int, runtime_seconds: float) -> int:
    if runtime_seconds >= _RECONNECT_STABLE_SECONDS:
        return 1
    return previous + 1


def _state_payload(app_id: str, observed_at: str, event_id: str) -> str:
    app_name = _app_name(app_id)
    return json.dumps(
        {
            "state": app_name,
            "app_id": app_id,
            "app_name": app_name,
            "observed_at": observed_at,
            "event_id": event_id,
        },
        separators=(",", ":"),
    )


def _clear_payload(previous_app_id: str, observed_at: str, event_id: str) -> str:
    return json.dumps(
        {
            "state": "none",
            "app_id": "",
            "app_name": "",
            "event_kind": "foreground_clear",
            "previous_app_id": previous_app_id,
            "observed_at": observed_at,
            "event_id": event_id,
        },
        separators=(",", ":"),
    )


@dataclass(frozen=True)
class Config:
    udid: str
    mqtt_host: str
    mqtt_port: int
    mqtt_username: str | None
    mqtt_password: str | None
    topic_prefix: str
    discovery_prefix: str
    reconnect_seconds: float
    dvt_poll_seconds: float
    dvt_timeout_seconds: float

    @classmethod
    def from_env(cls) -> "Config":
        def required(name: str) -> str:
            value = os.environ.get(name, "").strip()
            if not value:
                raise ValueError(f"{name} is required")
            return value

        port = int(os.environ.get("MQTT_PORT", "1883"))
        delay = float(os.environ.get("RECONNECT_SECONDS", "0.5"))
        poll = float(os.environ.get("DVT_POLL_SECONDS", "0.5"))
        timeout = float(os.environ.get("DVT_TIMEOUT_SECONDS", "3"))
        if (
            not 1 <= port <= 65535
            or not all(math.isfinite(value) for value in (delay, poll, timeout))
            or delay < 0
            or poll < 0
            or 0 < poll < 0.1
            or timeout < 0.1
        ):
            raise ValueError("MQTT_PORT or reconnect/poll interval is invalid")
        return cls(
            udid=required("APPLE_TV_UDID"),
            mqtt_host=required("MQTT_HOST"),
            mqtt_port=port,
            mqtt_username=os.environ.get("MQTT_USERNAME") or None,
            mqtt_password=os.environ.get("MQTT_PASSWORD") or None,
            topic_prefix=os.environ.get(
                "MQTT_TOPIC_PREFIX", "denon/apple_tv_foreground"
            ).strip("/"),
            discovery_prefix=os.environ.get(
                "MQTT_DISCOVERY_PREFIX", "homeassistant"
            ).strip("/"),
            reconnect_seconds=delay,
            dvt_poll_seconds=poll,
            dvt_timeout_seconds=timeout,
        )


@dataclass
class ForegroundState:
    payload: str | None = None
    online_sources: set[str] = field(default_factory=set)
    required_source: str | None = None
    lock: threading.Lock = field(default_factory=threading.Lock, repr=False)

    @property
    def online(self) -> bool:
        if self.required_source is not None:
            return self.required_source in self.online_sources
        return bool(self.online_sources)

    @property
    def replay_payload(self) -> str | None:
        return self.payload if self.online else None

    def observe(self, payload: str, source: str = "syslog") -> None:
        self.payload = payload
        self.online_sources.add(source)

    def outage(self, source: str = "syslog") -> None:
        self.online_sources.discard(source)


def _snapshot_foreground(processes: Any) -> str | None:
    if not isinstance(processes, list):
        return None
    for process in processes:
        if (
            not isinstance(process, dict)
            or process.get("foregroundRunning") is not True
        ):
            continue
        real_app_name = process.get("realAppName")
        if (
            process.get("bundleIdentifier") in _DVT_OVERLAYS
            or process.get("name") in _DVT_OVERLAY_NAMES
            or (
                isinstance(real_app_name, str)
                and Path(real_app_name).name in _DVT_OVERLAY_NAMES
            )
        ):
            return ""
    foreground = {
        process.get("bundleIdentifier")
        for process in processes
        if isinstance(process, dict)
        and process.get("isApplication") is True
        and process.get("foregroundRunning") is True
        and isinstance(process.get("bundleIdentifier"), str)
        and _APP_ID.fullmatch(process["bundleIdentifier"]) is not None
    }
    candidates = foreground - _DVT_INFRASTRUCTURE
    return next(iter(candidates)) if len(candidates) == 1 else None


def _syslog_event(record: dict[str, Any]) -> tuple[str, bool] | None:
    process = Path(str(record.get("filename", ""))).name
    message = str(record.get("message", ""))
    if process == "PineBoard" and (
        match := _PINEBOARD_FOREGROUND.fullmatch(message)
    ):
        return match.group(1), True
    if process == "symptomsd" and (match := _SYMPTOMSD.fullmatch(message)):
        return match.group(1), match.group(2) == "true"
    return None


def _source_is_authoritative(source: str, dvt_enabled: bool) -> bool:
    return source == ("dvt" if dvt_enabled else "syslog")


class DvtSettleGuard:
    """Hold the first stable app after an overlay clear or DVT outage."""

    def __init__(self, seconds: float = _DVT_SETTLE_SECONDS) -> None:
        self.seconds = seconds
        self.armed = False
        self.pending_app: str | None = None
        self.pending_since = 0.0

    def arm(self) -> None:
        self.armed = True
        self.pending_app = None
        self.pending_since = 0.0

    def accepts(self, app_id: str, now: float) -> bool:
        if not self.armed:
            return True
        if self.pending_app != app_id:
            self.pending_app = app_id
            self.pending_since = now
            return False
        if now - self.pending_since < self.seconds:
            return False
        self.armed = False
        self.pending_app = None
        self.pending_since = 0.0
        return True


class ForegroundTracker:
    """Turn relevant syslog records into deduplicated foreground app IDs."""

    def __init__(self) -> None:
        self.active_app: str | None = None
        self.last_event_at: datetime | None = None
        self.has_state = False

    def feed(
        self, record: dict[str, Any], event_at: datetime | None = None
    ) -> str | None:
        if (event := _syslog_event(record)) is None:
            return None
        app_id, foreground = event
        return app_id if self.apply(app_id, foreground, event_at) == "changed" else None

    def apply(
        self, app_id: str, foreground: bool, event_at: datetime | None
    ) -> str:
        if not foreground:
            if self.active_app != app_id or not self._accept_time(event_at):
                return "ignored"
            self.active_app = None
            self.has_state = True
            return "cleared"

        same = self.active_app == app_id
        if not self._accept_time(event_at, allow_equal=same):
            return "ignored"
        if same:
            return "same"
        self.active_app = app_id
        self.has_state = True
        return "changed"

    def clear(self, event_at: datetime | None) -> str:
        same = self.active_app is None
        if not self._accept_time(event_at, allow_equal=same):
            return "ignored"
        if same and self.has_state:
            return "same"
        self.active_app = None
        self.has_state = True
        return "cleared"

    def _accept_time(
        self, event_at: datetime | None, *, allow_equal: bool = False
    ) -> bool:
        if event_at is None:
            return self.last_event_at is None
        if event_at.tzinfo is None:
            return False
        if self.last_event_at is not None:
            if event_at < self.last_event_at:
                return False
            if event_at == self.last_event_at and not allow_equal:
                return False
        self.last_event_at = event_at
        return True


async def _wait_for_stop(
    stop_event: threading.Event, seconds: float
) -> None:
    deadline = time.monotonic() + seconds
    while not stop_event.is_set() and time.monotonic() < deadline:
        await asyncio.sleep(min(0.1, deadline - time.monotonic()))


async def _dvt_session(
    udid: str,
    emit: Any,
    poll_seconds: float,
    timeout_seconds: float,
    *,
    stop_event: threading.Event,
    get_devices: Any = None,
    dvt_factory: Any = None,
    info_factory: Any = None,
) -> None:
    if get_devices is None or dvt_factory is None or info_factory is None:
        from pymobiledevice3.services.dvt.instruments.device_info import DeviceInfo
        from pymobiledevice3.services.dvt.instruments.dvt_provider import DvtProvider
        from pymobiledevice3.tunneld.api import get_tunneld_devices

        get_devices = get_devices or get_tunneld_devices
        dvt_factory = dvt_factory or DvtProvider
        info_factory = info_factory or DeviceInfo

    rsds = await get_devices()
    rsd = next((item for item in rsds if item.udid == udid), None)
    for item in rsds:
        if item is not rsd:
            await item.close()
    if rsd is None:
        raise RuntimeError("configured Apple TV tunnel is unavailable")

    try:
        async with dvt_factory(rsd) as dvt:
            async with info_factory(dvt) as device_info:
                while not stop_event.is_set():
                    started_at = time.monotonic()
                    event_at = datetime.now(timezone.utc)
                    foreground = _snapshot_foreground(
                        await asyncio.wait_for(
                            device_info.proclist(), timeout_seconds
                        )
                    )
                    emit(foreground, event_at)
                    remaining = poll_seconds - (time.monotonic() - started_at)
                    if remaining > 0:
                        await _wait_for_stop(stop_event, remaining)
    finally:
        await rsd.close()


async def _dvt_loop(
    udid: str,
    emit: Any,
    outage: Any,
    stop_event: threading.Event,
    poll_seconds: float,
    reconnect_seconds: float,
    timeout_seconds: float,
    session: Any = _dvt_session,
) -> None:
    failures = 0
    while not stop_event.is_set():
        started_at = time.monotonic()
        try:
            await session(
                udid,
                emit,
                poll_seconds,
                timeout_seconds,
                stop_event=stop_event,
            )
        except Exception as error:
            logging.warning("DVT foreground poll failed: %s", error)
        outage()
        if stop_event.is_set():
            return
        failures = _next_failure_count(
            failures, time.monotonic() - started_at
        )
        delay = _reconnect_delay(reconnect_seconds, failures)
        logging.warning(
            "DVT foreground poll ended; reconnecting in %.2fs", delay
        )
        await _wait_for_stop(stop_event, delay)


def _dvt_worker(
    config: Config,
    emit: Any,
    outage: Any,
    stop_event: threading.Event,
) -> None:
    asyncio.run(
        _dvt_loop(
            config.udid,
            emit,
            outage,
            stop_event,
            config.dvt_poll_seconds,
            config.reconnect_seconds,
            config.dvt_timeout_seconds,
        )
    )


def _syslog_command(config: Config) -> list[str]:
    return [
        sys.executable,
        "-u",
        "-m",
        "pymobiledevice3",
        "syslog",
        "live",
        "--tunnel",
        config.udid,
        "--format",
        "json",
        "--insensitive-regex",
        r"Foreground: (true|false)",
        "--insensitive-regex",
        r"\(FBSceneManager\):[A-Za-z0-9._-]+-default\] "
        r"Scene lifecycle state did change: Foreground",
    ]


def main() -> int:
    import paho.mqtt.client as mqtt

    logging.basicConfig(
        level=logging.INFO,
        format="%(asctime)s %(levelname)s %(message)s",
        datefmt="%Y-%m-%dT%H:%M:%S%z",
    )
    try:
        config = Config.from_env()
    except (TypeError, ValueError) as error:
        logging.error("Invalid configuration: %s", error)
        return 2

    state_topic = f"{config.topic_prefix}/state"
    availability_topic = f"{config.topic_prefix}/availability"
    discovery_topic = (
        f"{config.discovery_prefix}/sensor/denon_apple_tv_foreground/config"
    )
    discovery = {
        "name": "Apple TV Foreground App",
        "unique_id": "denon_apple_tv_foreground",
        "state_topic": state_topic,
        "value_template": _VALUE_TEMPLATE,
        "json_attributes_topic": state_topic,
        "availability_topic": availability_topic,
        "device": {
            "identifiers": ["denon_apple_tv_foreground"],
            "name": "Apple TV",
        },
    }
    dvt_enabled = config.dvt_poll_seconds > 0
    foreground_state = ForegroundState(
        required_source="dvt" if dvt_enabled else None
    )
    pending: list[Any] = []
    publish_started: dict[int, tuple[float, str, str | None, str | None]] = {}
    early_acks: dict[int, float] = {}
    publish_lock = threading.Lock()
    client = mqtt.Client(
        mqtt.CallbackAPIVersion.VERSION2,
        client_id="denon-appletv-foreground",
    )
    if config.mqtt_username:
        client.username_pw_set(config.mqtt_username, config.mqtt_password)
    client.will_set(availability_topic, "offline", qos=1, retain=True)
    client.reconnect_delay_set(min_delay=1, max_delay=30)

    def log_publish_ack(
        acknowledged_at: float,
        started: tuple[float, str, str | None, str | None],
    ) -> None:
        received_at, kind, app_id, event_id = started
        logging.info(
            "mqtt_publish_ack kind=%s event_id=%s app_id=%s receipt_to_ack_ms=%.1f",
            kind,
            event_id or "none",
            app_id or "none",
            (acknowledged_at - received_at) * 1000,
        )

    def queue_publish(
        topic: str,
        payload: str | None,
        kind: str,
        app_id: str | None = None,
        received_at: float | None = None,
        event_id: str | None = None,
    ) -> None:
        started = (received_at or time.monotonic(), kind, app_id, event_id)
        message = client.publish(topic, payload, qos=1, retain=True)
        acknowledged_at: float | None = None
        with publish_lock:
            pending[:] = [item for item in pending if not item.is_published()]
            pending.append(message)
            acknowledged_at = early_acks.pop(message.mid, None)
            if acknowledged_at is None:
                publish_started[message.mid] = started
        if acknowledged_at is not None:
            log_publish_ack(acknowledged_at, started)

    def on_publish(
        _client: Any,
        _userdata: Any,
        message_id: int,
        _reason: Any,
        _properties: Any,
    ) -> None:
        acknowledged_at = time.monotonic()
        with publish_lock:
            started = publish_started.pop(message_id, None)
            if started is None:
                early_acks[message_id] = acknowledged_at
                return
        log_publish_ack(acknowledged_at, started)

    def on_connect(
        client: Any,
        _userdata: Any,
        _flags: Any,
        _reason: Any,
        _properties: Any,
    ) -> None:
        queue_publish(discovery_topic, json.dumps(discovery), "discovery")
        with foreground_state.lock:
            if foreground_state.replay_payload is not None:
                queue_publish(
                    state_topic, foreground_state.replay_payload, "state"
                )
            queue_publish(
                availability_topic,
                "online" if foreground_state.online else "offline",
                "availability",
            )

    client.on_connect = on_connect
    client.on_publish = on_publish
    client.connect(config.mqtt_host, config.mqtt_port, keepalive=60)
    client.loop_start()
    tracker = ForegroundTracker()
    dvt_settle = DvtSettleGuard()
    failure_count = 0
    dvt_stop = threading.Event()

    def source_outage(source: str) -> None:
        with foreground_state.lock:
            if source == "dvt":
                dvt_settle.arm()
            was_online = foreground_state.online
            foreground_state.outage(source)
            if was_online and not foreground_state.online:
                queue_publish(
                    availability_topic, "offline", "availability"
                )

    def accept_candidate(
        app_id: str,
        foreground: bool,
        event_at: datetime | None,
        source: str,
        source_record_at: str,
        availability_source: str,
        received_at: float,
    ) -> None:
        if not _source_is_authoritative(availability_source, dvt_enabled):
            return
        if event_at is None and dvt_enabled:
            return
        with foreground_state.lock:
            if availability_source == "dvt" and dvt_stop.is_set():
                return
            if availability_source == "dvt":
                if app_id == "":
                    dvt_settle.arm()
                elif not dvt_settle.accepts(app_id, received_at):
                    return
            previous_app = tracker.active_app
            status = (
                tracker.clear(event_at)
                if app_id == ""
                else tracker.apply(app_id, foreground, event_at)
            )
            if status == "ignored":
                return
            if status == "same":
                if foreground_state.payload is None:
                    return
                was_online = foreground_state.online
                foreground_state.observe(
                    foreground_state.payload, availability_source
                )
                if not was_online and foreground_state.online:
                    queue_publish(
                        state_topic, foreground_state.payload, "state"
                    )
                    queue_publish(
                        availability_topic, "online", "availability"
                    )
                return

            observed_at = _observed_at()
            event_id = uuid4().hex
            if status == "changed":
                logging.info(
                    "foreground_transition event_id=%s observed_at=%s "
                    "source_record_at=%s source=%s app_id=%s",
                    event_id,
                    observed_at,
                    source_record_at,
                    source,
                    app_id,
                )
                payload = _state_payload(app_id, observed_at, event_id)
                kind = "state"
                published_app = app_id
            else:
                assert status == "cleared"
                logging.info(
                    "foreground_clear event_id=%s observed_at=%s "
                    "source_record_at=%s source=%s app_id=%s",
                    event_id,
                    observed_at,
                    source_record_at,
                    source,
                    previous_app or "none",
                )
                payload = _clear_payload(
                    previous_app or "", observed_at, event_id
                )
                kind = "state_clear"
                published_app = previous_app

            was_online = foreground_state.online
            foreground_state.observe(payload, availability_source)
            queue_publish(
                state_topic,
                payload,
                kind,
                published_app,
                received_at,
                event_id,
            )
            if not was_online and foreground_state.online:
                queue_publish(
                    availability_topic,
                    "online",
                    "availability",
                    received_at=received_at,
                )

    def accept_dvt(app_id: str | None, event_at: datetime) -> None:
        if dvt_stop.is_set():
            return
        if app_id is None:
            source_outage("dvt")
            return
        accept_candidate(
            app_id,
            app_id != "",
            event_at,
            "DVT DeviceInfo",
            event_at.isoformat(),
            "dvt",
            time.monotonic(),
        )

    def stop(_signum: int, _frame: Any) -> None:
        raise SystemExit

    signal.signal(signal.SIGTERM, stop)
    signal.signal(signal.SIGINT, stop)

    dvt_thread: threading.Thread | None = None
    if dvt_enabled:
        dvt_thread = threading.Thread(
            target=_dvt_worker,
            args=(
                config,
                accept_dvt,
                lambda: source_outage("dvt"),
                dvt_stop,
            ),
            name="appletv-dvt-foreground",
            daemon=True,
        )
        dvt_thread.start()

    try:
        while True:
            logging.info("Connecting to Apple TV syslog")
            stream_started_at = time.monotonic()
            process = subprocess.Popen(
                _syslog_command(config),
                stdout=subprocess.PIPE,
                bufsize=1,
                text=True,
            )
            try:
                assert process.stdout is not None
                for line in process.stdout:
                    received_at = time.monotonic()
                    try:
                        record = json.loads(line)
                    except json.JSONDecodeError:
                        continue
                    if (event := _syslog_event(record)) is not None:
                        app_id, foreground = event
                        accept_candidate(
                            app_id,
                            foreground,
                            _source_datetime(record),
                            Path(
                                str(record.get("filename", ""))
                            ).name
                            or "unknown",
                            _source_timestamp(record),
                            "syslog",
                            received_at,
                        )
                source_outage("syslog")
                runtime_seconds = time.monotonic() - stream_started_at
                failure_count = _next_failure_count(
                    failure_count, runtime_seconds
                )
                reconnect_delay = _reconnect_delay(
                    config.reconnect_seconds, failure_count
                )
                logging.warning(
                    "Apple TV syslog ended after %.1fs; reconnecting in %.2fs "
                    "(consecutive_short_failures=%d)",
                    runtime_seconds,
                    reconnect_delay,
                    max(failure_count - 1, 0),
                )
            finally:
                if process.poll() is None:
                    process.terminate()
                try:
                    process.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    process.kill()
                    process.wait()
            time.sleep(reconnect_delay)
    finally:
        dvt_stop.set()
        if dvt_thread is not None:
            dvt_thread.join(timeout=2)
        with foreground_state.lock:
            foreground_state.outage("syslog")
            foreground_state.outage("dvt")
            queue_publish(availability_topic, "offline", "availability")
        deadline = time.monotonic() + 2
        for message in pending:
            if (remaining := deadline - time.monotonic()) <= 0:
                break
            try:
                message.wait_for_publish(timeout=remaining)
            except (RuntimeError, ValueError):
                break
        client.disconnect()
        client.loop_stop()


if __name__ == "__main__":
    raise SystemExit(main())
