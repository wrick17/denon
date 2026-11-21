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
    STATE_IDLE,
    STATE_OFF,
    STATE_PAUSED,
    STATE_PLAYING,
    STATE_UNAVAILABLE,
    STATE_UNKNOWN,
)
from homeassistant.core import Event, HomeAssistant, State, callback
from homeassistant.exceptions import HomeAssistantError
from homeassistant.helpers import entity_registry as er
from homeassistant.helpers.aiohttp_client import async_get_clientsession
from homeassistant.helpers.device_registry import async_get
from homeassistant.helpers.event import async_track_state_change_event
from homeassistant.helpers.storage import Store

from .api import (
    AppVolume,
    async_get_backup,
    async_put_backup,
    async_send_app,
    async_unpair,
    backup_payload,
    device_url,
    parse_backup_apps,
)
from .const import (
    ATTR_APP_ID,
    ATTR_APP_NAME,
    BACKUP_SAVE_DELAY_SECONDS,
    DOMAIN,
    HEARTBEAT_SECONDS,
)

_LOGGER = logging.getLogger(__name__)
_UNAVAILABLE_STATES = {STATE_UNAVAILABLE, STATE_UNKNOWN}
type AppHandoff = tuple[str, str, bool | None, str | None]


def _app_payload(state: State | None) -> tuple[str, str] | None:
    """Return known ownership, an available-state clear, or unavailable."""
    if state is None or state.state in _UNAVAILABLE_STATES:
        return None
    if state.state == STATE_OFF:
        return "", ""

    app_id = state.attributes.get(ATTR_APP_ID)
    if not isinstance(app_id, str) or not (app_id := app_id.strip()):
        return "", ""

    app_name = state.attributes.get(ATTR_APP_NAME)
    if not isinstance(app_name, str) or not (app_name := app_name.strip()):
        app_name = app_id
    return app_id, app_name


def _event_id(state: State | None) -> str | None:
    """Return the collector's stable foreground event identifier."""
    value = state.attributes.get("event_id") if state is not None else None
    if (
        isinstance(value, str)
        and len(value) == 32
        and all(character in "0123456789abcdef" for character in value)
    ):
        return value
    return None


class AppRelay:
    """Coalesce Apple TV changes and heartbeat the current app to the ESP32."""

    def __init__(
        self,
        hass: HomeAssistant,
        entry: ConfigEntry,
        backup_key: str | None = None,
        playback_entity_id: str | None = None,
    ) -> None:
        """Initialize the relay from one config entry."""
        self._hass = hass
        self._entry = entry
        self._session = async_get_clientsession(hass)
        self._entity_id: str = entry.data[CONF_ENTITY_ID]
        self._playback_entity_id = playback_entity_id
        if self._entity_id.startswith("media_player."):
            self._playback_entity_id = self._entity_id
        self._deferred_app_id: str | None = None
        self._host: str = entry.data[CONF_HOST]
        self._port: int = entry.data[CONF_PORT]
        self._token: str = entry.data[CONF_TOKEN]
        self._changed = asyncio.Event()
        self._foreground_generation = 0
        self._foreground_delivered_generation = 0
        self._last_sent: AppHandoff | None = None
        self._available = True
        self._reauth_started = False
        self._backup_key = backup_key or f"{DOMAIN}.backup.{self._entity_id}"
        self._backup_store: Store[dict[str, Any]] | None = None
        self._stored_apps: tuple[AppVolume, ...] = ()
        self._backup_loaded = False
        self._backup_available = True

    @callback
    def async_notify(self, event: Event[Any]) -> None:
        """Wake the relay after a relevant Apple TV state change."""
        if event.data.get("entity_id") == self._entity_id:
            self._foreground_generation += 1
        self._changed.set()

    async def async_run(self) -> None:
        """Send immediately, on changes, and every five seconds."""
        force = True
        while True:
            self._changed.clear()
            if force:
                await self._async_sync_backup()
            await self._async_send_pending(force=force)
            try:
                await asyncio.wait_for(
                    self._changed.wait(), timeout=HEARTBEAT_SECONDS
                )
            except TimeoutError:
                force = True
            else:
                force = False

    async def _async_send_pending(self, *, force: bool) -> bool | None:
        """Deliver the newest foreground generation without dropping races."""
        generation = self._foreground_generation
        fresh_foreground = generation != self._foreground_delivered_generation
        delivered = await self._async_send_current(
            force=force,
            fresh_foreground=fresh_foreground,
            foreground_pending=fresh_foreground,
        )
        if delivered is True and generation == self._foreground_generation:
            self._foreground_delivered_generation = generation
        return delivered

    async def _async_send_current(
        self,
        *,
        force: bool,
        fresh_foreground: bool = True,
        foreground_pending: bool = False,
    ) -> bool | None:
        """Send the latest usable app without treating a restore as user input."""
        source = self._hass.states.get(self._entity_id)
        foreground = _app_payload(source)
        payload = self._gated_payload(
            foreground,
            source,
            fresh_foreground=fresh_foreground,
            foreground_pending=foreground_pending,
        )
        if payload is None:
            return None
        if (
            payload == self._last_sent
            and (not force or not payload[0])
            and (not fresh_foreground or payload[2] is not False)
        ):
            return True

        try:
            await async_send_app(
                self._session,
                self._host,
                self._port,
                self._token,
                payload[0],
                payload[1],
                playback_active=payload[2],
                event_id=payload[3],
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
                return None
            if self._available:
                _LOGGER.warning("Denon App Volume device became unavailable: %s", err)
            self._available = False
            return False
        except (ClientError, TimeoutError) as err:
            if self._available:
                _LOGGER.warning("Denon App Volume device became unavailable: %s", err)
            self._available = False
            return False

        if not self._available:
            _LOGGER.info("Denon App Volume device is available again")
        self._available = True
        self._last_sent = payload
        return True

    def _gated_payload(
        self,
        foreground: tuple[str, str] | None,
        source: State | None,
        *,
        fresh_foreground: bool,
        foreground_pending: bool,
    ) -> AppHandoff | None:
        """Prefer active playback and defer a browsed app until it plays."""
        event_id = _event_id(source)
        if self._playback_entity_id is None:
            self._deferred_app_id = foreground[0] if foreground else None
            return None

        playback = (
            source
            if self._playback_entity_id == self._entity_id
            else self._hass.states.get(self._playback_entity_id)
        )
        if playback is None or playback.state in _UNAVAILABLE_STATES:
            if foreground_pending:
                return None
            return self._revocation()
        if playback.state == STATE_OFF:
            if self._playback_entity_id == self._entity_id:
                return "", "", None, event_id
            if foreground_pending:
                return None
            return self._revocation()
        if playback.state == STATE_PLAYING:
            owner = _app_payload(playback)
            if owner is None or not owner[0]:
                self._deferred_app_id = foreground[0] if foreground else None
                if foreground_pending:
                    return None
                return self._revocation()
            if foreground is not None and owner[0] != foreground[0]:
                self._deferred_app_id = foreground[0] or None
            else:
                self._deferred_app_id = None
            return owner[0], owner[1], True, event_id
        if playback.state not in {STATE_IDLE, STATE_PAUSED}:
            if foreground_pending:
                return None
            return self._revocation()
        if not fresh_foreground:
            return self._revocation()
        if foreground is None:
            return self._revocation()
        if foreground[0] and foreground[0] == self._deferred_app_id:
            return self._revocation()
        self._deferred_app_id = None
        return (
            foreground[0],
            foreground[1],
            False if foreground[0] else None,
            event_id,
        )

    def _revocation(self) -> AppHandoff | None:
        """Revoke a mute-restore lease without changing app ownership."""
        if self._last_sent is None or not self._last_sent[0]:
            return None
        return self._last_sent[0], self._last_sent[1], None, self._last_sent[3]

    async def _async_load_backup(self) -> None:
        """Load the last HA snapshot once before talking to the ESP32."""
        if self._backup_loaded:
            return
        self._backup_store = Store(self._hass, 1, self._backup_key)
        saved = await self._backup_store.async_load()
        if saved is not None:
            try:
                self._stored_apps = parse_backup_apps(saved)
            except ValueError:
                _LOGGER.warning("Ignoring an invalid stored app-volume backup")
        self._backup_loaded = True

    async def _async_sync_backup(self) -> None:
        """Mirror ESP data to HA, or restore HA data to an empty ESP."""
        try:
            await self._async_load_backup()
            assert self._backup_store is not None
            snapshot = await async_get_backup(
                self._session,
                self._host,
                self._port,
                self._token,
            )
            if snapshot.apps:
                if snapshot.apps != self._stored_apps:
                    self._stored_apps = snapshot.apps
                    saved = backup_payload(snapshot.apps)
                    self._backup_store.async_delay_save(
                        lambda: saved, BACKUP_SAVE_DELAY_SECONDS
                    )
            elif self._stored_apps:
                await async_put_backup(
                    self._session,
                    self._host,
                    self._port,
                    self._token,
                    snapshot.etag,
                    self._stored_apps,
                )
                restored = await async_get_backup(
                    self._session,
                    self._host,
                    self._port,
                    self._token,
                )
                if restored.apps != self._stored_apps:
                    raise ValueError("backup restore readback did not match")
        except ClientResponseError as err:
            if err.status == 401 and not self._reauth_started:
                _LOGGER.warning("ESP32 rejected its token; starting reauthentication")
                self._entry.async_start_reauth(self._hass)
                self._reauth_started = True
            if self._backup_available:
                _LOGGER.warning("Could not sync the app-volume backup: %s", err)
            self._backup_available = False
            return
        except (
            ClientError,
            HomeAssistantError,
            OSError,
            TimeoutError,
            ValueError,
        ) as err:
            if self._backup_available:
                _LOGGER.warning("Could not sync the app-volume backup: %s", err)
            self._backup_available = False
            return

        if not self._backup_available:
            _LOGGER.info("App-volume backup sync is available again")
        self._backup_available = True


async def async_setup_entry(hass: HomeAssistant, entry: ConfigEntry) -> bool:
    """Set up one local ESP32 bridge."""
    assert entry.unique_id is not None
    entity_registry = er.async_get(hass)
    source = entity_registry.async_get(entry.data[CONF_ENTITY_ID])
    source_id = source.id if source is not None else entry.data[CONF_ENTITY_ID]
    playback_ids = [
        candidate.entity_id
        for candidate in entity_registry.entities.values()
        if candidate.platform == "apple_tv"
        and candidate.entity_id.startswith("media_player.")
        and candidate.disabled_by is None
    ]
    playback_entity_id = (
        entry.data[CONF_ENTITY_ID]
        if source is not None and source.platform == "apple_tv"
        else playback_ids[0]
        if len(playback_ids) == 1
        else None
    )
    # ponytail: one volume controller per Apple TV; add a user-selected backup
    # slot if simultaneous controllers for one Apple TV become a real use case.
    relay = AppRelay(
        hass,
        entry,
        f"{DOMAIN}.backup.{source_id}",
        playback_entity_id,
    )
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
            hass,
            {
                entity_id
                for entity_id in (
                    entry.data[CONF_ENTITY_ID],
                    playback_entity_id,
                )
                if entity_id is not None
            },
            relay.async_notify,
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
