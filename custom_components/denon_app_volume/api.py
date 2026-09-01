"""Small HTTP client for the ESP32 service."""

from dataclasses import dataclass
import re
from typing import Any

from aiohttp import ClientError, ClientSession, ClientTimeout
from yarl import URL

from .const import API_VERSION, REQUEST_TIMEOUT_SECONDS

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
) -> None:
    """Send the current Apple TV app to the paired ESP32."""
    async with session.post(
        device_url(host, port, "/api/app"),
        headers={"Authorization": f"Bearer {token}"},
        json={"app_id": app_id, "app_name": app_name},
        timeout=_TIMEOUT,
    ) as response:
        response.raise_for_status()


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
