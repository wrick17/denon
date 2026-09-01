"""Home Assistant bridge for Denon App Volume."""

from __future__ import annotations

import asyncio
import logging
from typing import Any

from aiohttp import ClientError, ClientResponseError

from homeassistant.config_entries import ConfigEntry
from homeassistant.const import (
    CONF_ENTITY_ID,
    CONF_HOST,
    CONF_PORT,
    CONF_TOKEN,
    STATE_OFF,
    STATE_UNAVAILABLE,
    STATE_UNKNOWN,
)
from homeassistant.core import Event, HomeAssistant, State, callback
from homeassistant.helpers.aiohttp_client import async_get_clientsession
from homeassistant.helpers.device_registry import async_get
from homeassistant.helpers.event import async_track_state_change_event

from .api import async_send_app, async_unpair, device_url
from .const import ATTR_APP_ID, ATTR_APP_NAME, DOMAIN, HEARTBEAT_SECONDS

_LOGGER = logging.getLogger(__name__)
_INVALID_STATES = {STATE_OFF, STATE_UNAVAILABLE, STATE_UNKNOWN}


def _app_payload(state: State | None) -> tuple[str, str] | None:
    """Return app ownership, an explicit clear, or None when unavailable."""
    if state is None or state.state in _INVALID_STATES:
        return None

    app_id = state.attributes.get(ATTR_APP_ID)
    if not isinstance(app_id, str) or not (app_id := app_id.strip()):
        return "", ""

    app_name = state.attributes.get(ATTR_APP_NAME)
    if not isinstance(app_name, str) or not (app_name := app_name.strip()):
        app_name = app_id
    return app_id, app_name


class AppRelay:
    """Coalesce Apple TV changes and heartbeat the current app to the ESP32."""

    def __init__(self, hass: HomeAssistant, entry: ConfigEntry) -> None:
        """Initialize the relay from one config entry."""
        self._hass = hass
        self._entry = entry
        self._session = async_get_clientsession(hass)
        self._entity_id: str = entry.data[CONF_ENTITY_ID]
        self._host: str = entry.data[CONF_HOST]
        self._port: int = entry.data[CONF_PORT]
        self._token: str = entry.data[CONF_TOKEN]
        self._changed = asyncio.Event()
        self._last_sent: tuple[str, str] | None = None
        self._available = True
        self._reauth_started = False

    @callback
    def async_notify(self, _event: Event[Any]) -> None:
        """Wake the relay after a relevant Apple TV state change."""
        self._changed.set()

    async def async_run(self) -> None:
        """Send immediately, on changes, and every five seconds."""
        force = True
        while True:
            self._changed.clear()
            await self._async_send_current(force=force)
            try:
                await asyncio.wait_for(
                    self._changed.wait(), timeout=HEARTBEAT_SECONDS
                )
            except TimeoutError:
                force = True
            else:
                force = False

    async def _async_send_current(self, *, force: bool) -> None:
        """Send the latest usable app without treating a restore as user input."""
        payload = _app_payload(self._hass.states.get(self._entity_id))
        if payload is None:
            # Force a resend when a temporarily unavailable Apple TV returns.
            self._last_sent = None
            return
        if not force and payload == self._last_sent:
            return

        try:
            await async_send_app(
                self._session,
                self._host,
                self._port,
                self._token,
                *payload,
            )
        except ClientResponseError as err:
            if err.status == 401:
                if not self._reauth_started:
                    _LOGGER.warning(
                        "ESP32 rejected its token; starting reauthentication"
                    )
                    self._entry.async_start_reauth(self._hass)
                    self._reauth_started = True
                self._available = False
                return
            if self._available:
                _LOGGER.warning("Denon App Volume device became unavailable: %s", err)
            self._available = False
            return
        except (ClientError, TimeoutError) as err:
            if self._available:
                _LOGGER.warning("Denon App Volume device became unavailable: %s", err)
            self._available = False
            return

        if not self._available:
            _LOGGER.info("Denon App Volume device is available again")
        self._available = True
        self._last_sent = payload


async def async_setup_entry(hass: HomeAssistant, entry: ConfigEntry) -> bool:
    """Set up one local ESP32 bridge."""
    assert entry.unique_id is not None
    relay = AppRelay(hass, entry)
    entry.runtime_data = relay

    registry = async_get(hass)
    registry.async_get_or_create(
        config_entry_id=entry.entry_id,
        identifiers={(DOMAIN, entry.unique_id)},
        name=entry.title,
        configuration_url=str(device_url(entry.data[CONF_HOST], entry.data[CONF_PORT])),
    )

    entry.async_on_unload(
        async_track_state_change_event(
            hass, entry.data[CONF_ENTITY_ID], relay.async_notify
        )
    )
    entry.async_create_background_task(
        hass,
        relay.async_run(),
        f"{DOMAIN}-{entry.entry_id}",
    )
    return True


async def async_unload_entry(_hass: HomeAssistant, _entry: ConfigEntry) -> bool:
    """Unload the bridge; config-entry lifecycle cancels its listener and task."""
    return True


async def async_remove_entry(hass: HomeAssistant, entry: ConfigEntry) -> None:
    """Release the device token when this config entry is deleted."""
    try:
        await async_unpair(
            async_get_clientsession(hass),
            entry.data[CONF_HOST],
            entry.data[CONF_PORT],
            entry.data[CONF_TOKEN],
        )
    except (ClientError, TimeoutError) as err:
        _LOGGER.warning(
            "Could not clear the ESP32 token during removal (%s). Hold its "
            "GPIO0/BOOT button for 10 seconds, then release it to reset the token",
            err,
        )
