"""Small HTTP client for the ESP32 service."""

from dataclasses import dataclass
import re
from typing import Any

from aiohttp import ClientError, ClientSession, ClientTimeout
from yarl import URL

from .const import (
    API_VERSION,
    BACKUP_SCHEMA,
    MAX_APPS,
    MAX_APP_ID_LENGTH,
    MAX_APP_NAME_LENGTH,
    MAX_VOLUME_RAW,
    REQUEST_TIMEOUT_SECONDS,
)

_TOKEN_PATTERN = re.compile(r"[0-9a-f]{64}")
_TIMEOUT = ClientTimeout(total=REQUEST_TIMEOUT_SECONDS)


class CannotConnect(Exception):
    """Raised when the ESP32 cannot be reached or identified."""


class CannotPair(Exception):
    """Raised when the ESP32 rejects or fails pairing."""


@dataclass(frozen=True, slots=True)
class DeviceInfo:
    """Identity returned by the ESP32."""

    device_id: str
    name: str
    paired: bool


@dataclass(frozen=True, slots=True)
class AppVolume:
    """One persisted per-app receiver volume."""

    app_id: str
    app_name: str
    volume_raw: int


@dataclass(frozen=True, slots=True)
class BackupSnapshot:
    """Validated ESP32 app-volume snapshot."""

    revision: int
    etag: str
    apps: tuple[AppVolume, ...]


def device_url(host: str, port: int, path: str = "") -> URL:
    """Build a safe HTTP URL for IPv4, IPv6, or a hostname."""
    return URL.build(scheme="http", host=host, port=port).with_path(path)


async def async_get_info(
    session: ClientSession, host: str, port: int
) -> DeviceInfo:
    """Fetch and validate the public device identity endpoint."""
    try:
        async with session.get(
            device_url(host, port, "/api/info"), timeout=_TIMEOUT
        ) as response:
            response.raise_for_status()
            payload: Any = await response.json(content_type=None)
    except (ClientError, TimeoutError, ValueError, TypeError) as err:
        raise CannotConnect from err

    if not isinstance(payload, dict):
        raise CannotConnect

    device_id = payload.get("id")
    name = payload.get("name")
    api_version = payload.get("api_version")
    paired = payload.get("paired")
    if (
        not isinstance(device_id, str)
        or not device_id.strip()
        or not isinstance(name, str)
        or not name.strip()
        or not isinstance(api_version, int)
        or isinstance(api_version, bool)
        or api_version != API_VERSION
        or not isinstance(paired, bool)
    ):
        raise CannotConnect

    return DeviceInfo(device_id.strip(), name.strip(), paired)


async def async_pair(session: ClientSession, host: str, port: int) -> str:
    """Pair during the device's claim window and return its token."""
    try:
        async with session.post(
            device_url(host, port, "/api/pair"), json={}, timeout=_TIMEOUT
        ) as response:
            response.raise_for_status()
            payload: Any = await response.json(content_type=None)
    except (ClientError, TimeoutError, ValueError, TypeError) as err:
        raise CannotPair from err

    token = payload.get("token") if isinstance(payload, dict) else None
    if not isinstance(token, str) or _TOKEN_PATTERN.fullmatch(token) is None:
        raise CannotPair
    return token


async def async_send_app(
    session: ClientSession,
    host: str,
    port: int,
    token: str,
    app_id: str,
    app_name: str,
    *,
    playback_active: bool | None = None,
    event_id: str | None = None,
) -> None:
    """Send the current Apple TV app to the paired ESP32."""
    payload: dict[str, str | bool] = {
        "app_id": app_id,
        "app_name": app_name,
    }
    if playback_active is not None:
        payload["playback_active"] = playback_active
    if event_id is not None:
        payload["event_id"] = event_id
    async with session.post(
        device_url(host, port, "/api/app"),
        headers={"Authorization": f"Bearer {token}"},
        json=payload,
        timeout=_TIMEOUT,
    ) as response:
        response.raise_for_status()


def backup_payload(apps: tuple[AppVolume, ...]) -> dict[str, Any]:
    """Convert validated app volumes to the versioned wire/storage format."""
    return {
        "schema": BACKUP_SCHEMA,
        "apps": [
            {
                "app_id": app.app_id,
                "app_name": app.app_name,
                "volume_raw": app.volume_raw,
            }
            for app in apps
        ],
    }


def parse_backup_apps(payload: Any) -> tuple[AppVolume, ...]:
    """Validate an app-volume payload from the device or HA storage."""
    if (
        not isinstance(payload, dict)
        or not isinstance(payload.get("schema"), int)
        or isinstance(payload.get("schema"), bool)
        or payload["schema"] != BACKUP_SCHEMA
    ):
        raise ValueError("unsupported backup schema")
    raw_apps = payload.get("apps")
    if not isinstance(raw_apps, list) or len(raw_apps) > MAX_APPS:
        raise ValueError("invalid backup app list")

    apps: list[AppVolume] = []
    seen: set[str] = set()
    for item in raw_apps:
        if not isinstance(item, dict):
            raise ValueError("invalid backup app")
        app_id = item.get("app_id")
        app_name = item.get("app_name")
        volume_raw = item.get("volume_raw")
        if (
            not isinstance(app_id, str)
            or not app_id
            or "\0" in app_id
            or len(app_id.encode()) > MAX_APP_ID_LENGTH
            or app_id in seen
            or not isinstance(app_name, str)
            or "\0" in app_name
            or len(app_name.encode()) > MAX_APP_NAME_LENGTH
            or not isinstance(volume_raw, int)
            or isinstance(volume_raw, bool)
            or not 0 <= volume_raw <= MAX_VOLUME_RAW
        ):
            raise ValueError("invalid backup app")
        seen.add(app_id)
        apps.append(AppVolume(app_id, app_name, volume_raw))
    return tuple(apps)


async def async_get_backup(
    session: ClientSession, host: str, port: int, token: str
) -> BackupSnapshot:
    """Read and validate the authenticated ESP32 backup snapshot."""
    async with session.get(
        device_url(host, port, "/api/backup"),
        headers={"Authorization": f"Bearer {token}"},
        timeout=_TIMEOUT,
    ) as response:
        response.raise_for_status()
        payload: Any = await response.json(content_type=None)
        etag = response.headers.get("ETag")

    revision = payload.get("revision") if isinstance(payload, dict) else None
    if (
        not isinstance(revision, int)
        or isinstance(revision, bool)
        or not 0 <= revision <= 0xFFFFFFFF
        or not isinstance(etag, str)
        or not etag
    ):
        raise ValueError("invalid backup revision")
    return BackupSnapshot(revision, etag, parse_backup_apps(payload))


async def async_put_backup(
    session: ClientSession,
    host: str,
    port: int,
    token: str,
    etag: str,
    apps: tuple[AppVolume, ...],
) -> None:
    """Restore a backup only if the empty snapshot has not changed."""
    async with session.put(
        device_url(host, port, "/api/backup"),
        headers={
            "Authorization": f"Bearer {token}",
            "If-Match": etag,
        },
        json=backup_payload(apps),
        timeout=_TIMEOUT,
    ) as response:
        response.raise_for_status()
        if response.status != 204:
            raise ValueError("unexpected backup restore response")


async def async_unpair(
    session: ClientSession, host: str, port: int, token: str
) -> None:
    """Remove this Home Assistant token from the ESP32."""
    async with session.post(
        device_url(host, port, "/api/unpair"),
        headers={"Authorization": f"Bearer {token}"},
        timeout=_TIMEOUT,
    ) as response:
        response.raise_for_status()
