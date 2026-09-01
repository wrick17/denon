"""Tests for the Denon App Volume runtime bridge."""

from types import SimpleNamespace
from unittest.mock import AsyncMock, MagicMock, Mock, patch

from aiohttp import ClientResponseError

from homeassistant.const import (
    CONF_ENTITY_ID,
    CONF_HOST,
    CONF_PORT,
    CONF_TOKEN,
    STATE_PAUSED,
    STATE_UNAVAILABLE,
)
from homeassistant.core import HomeAssistant, State
from pytest_homeassistant_custom_component.common import MockConfigEntry

from custom_components.denon_app_volume import (
    AppRelay,
    _app_payload,
    async_remove_entry,
)
from custom_components.denon_app_volume.api import async_unpair
from custom_components.denon_app_volume.const import DOMAIN

APPLE_TV_ENTITY_ID = "media_player.example_apple_tv"


def test_paused_app_is_valid() -> None:
    """Paused playback still identifies the app that should own the volume."""
    state = State(
        APPLE_TV_ENTITY_ID,
        STATE_PAUSED,
        {"app_id": "com.netflix.Netflix", "app_name": "Netflix"},
    )

    assert _app_payload(state) == ("com.netflix.Netflix", "Netflix")


def test_unavailable_is_ignored_but_missing_app_clears() -> None:
    """Availability controls whether missing app metadata clears ownership."""
    unavailable = State(
        APPLE_TV_ENTITY_ID,
        STATE_UNAVAILABLE,
        {"app_id": "com.netflix.Netflix", "app_name": "Netflix"},
    )
    idle = State(APPLE_TV_ENTITY_ID, "idle", {})

    assert _app_payload(unavailable) is None
    assert _app_payload(idle) == ("", "")


def test_missing_app_name_falls_back_to_identifier() -> None:
    """The ESP32 contract always receives a non-empty display name."""
    state = State(
        APPLE_TV_ENTITY_ID,
        "playing",
        {"app_id": "org.videolan.vlc-ios"},
    )

    assert _app_payload(state) == ("org.videolan.vlc-ios", "org.videolan.vlc-ios")


async def test_relay_coalesces_changes_but_heartbeat_resends() -> None:
    """Repeated HA events are cheap while a heartbeat can recover an ESP reboot."""
    state = State(
        APPLE_TV_ENTITY_ID,
        "playing",
        {"app_id": "com.netflix.Netflix", "app_name": "Netflix"},
    )
    hass = SimpleNamespace(states=SimpleNamespace(get=Mock(return_value=state)))
    entry = SimpleNamespace(
        data={
            CONF_ENTITY_ID: APPLE_TV_ENTITY_ID,
            CONF_HOST: "denon-volume.local",
            CONF_PORT: 80,
            CONF_TOKEN: "d" * 64,
        }
    )

    with patch(
        "custom_components.denon_app_volume.async_get_clientsession",
        return_value=object(),
    ):
        relay = AppRelay(hass, entry)

    with patch(
        "custom_components.denon_app_volume.async_send_app", new_callable=AsyncMock
    ) as send:
        await relay._async_send_current(force=False)
        await relay._async_send_current(force=False)
        assert send.await_count == 1

        await relay._async_send_current(force=True)
        assert send.await_count == 2


async def test_relay_sends_and_heartbeats_clear_payload() -> None:
    """An available Apple TV without app metadata clears ESP32 ownership."""
    state = State(APPLE_TV_ENTITY_ID, "idle", {})
    hass = SimpleNamespace(states=SimpleNamespace(get=Mock(return_value=state)))
    entry = SimpleNamespace(
        data={
            CONF_ENTITY_ID: APPLE_TV_ENTITY_ID,
            CONF_HOST: "denon-volume.local",
            CONF_PORT: 80,
            CONF_TOKEN: "e" * 64,
        }
    )

    with patch(
        "custom_components.denon_app_volume.async_get_clientsession",
        return_value=object(),
    ):
        relay = AppRelay(hass, entry)

    with patch(
        "custom_components.denon_app_volume.async_send_app", new_callable=AsyncMock
    ) as send:
        await relay._async_send_current(force=False)
        await relay._async_send_current(force=False)
        assert send.await_count == 1
        assert send.await_args.args[-2:] == ("", "")

        await relay._async_send_current(force=True)
        assert send.await_count == 2


async def test_rejected_token_starts_one_reauth_flow() -> None:
    """A rejected token prompts recovery once instead of looping flows."""
    state = State(
        APPLE_TV_ENTITY_ID,
        "playing",
        {"app_id": "com.example.video", "app_name": "Video"},
    )
    hass = SimpleNamespace(states=SimpleNamespace(get=Mock(return_value=state)))
    entry = SimpleNamespace(
        data={
            CONF_ENTITY_ID: APPLE_TV_ENTITY_ID,
            CONF_HOST: "denon-volume.local",
            CONF_PORT: 80,
            CONF_TOKEN: "f" * 64,
        },
        async_start_reauth=Mock(),
    )
    error = ClientResponseError(Mock(), (), status=401)

    with patch(
        "custom_components.denon_app_volume.async_get_clientsession",
        return_value=object(),
    ):
        relay = AppRelay(hass, entry)

    with patch(
        "custom_components.denon_app_volume.async_send_app",
        AsyncMock(side_effect=error),
    ):
        await relay._async_send_current(force=True)
        await relay._async_send_current(force=True)

    entry.async_start_reauth.assert_called_once_with(hass)


async def test_unpair_uses_bearer_token() -> None:
    """The unpair endpoint receives only the device-scoped bearer token."""
    session = MagicMock()
    response = MagicMock()
    request = MagicMock()
    request.__aenter__ = AsyncMock(return_value=response)
    request.__aexit__ = AsyncMock(return_value=None)
    session.post.return_value = request

    await async_unpair(session, "denon-volume.local", 80, "a" * 64)

    url = session.post.call_args.args[0]
    assert url.path == "/api/unpair"
    assert session.post.call_args.kwargs["headers"] == {
        "Authorization": f"Bearer {'a' * 64}"
    }
    response.raise_for_status.assert_called_once_with()


async def test_entry_removal_unpairs_device(hass: HomeAssistant) -> None:
    """Deleting the HA entry releases the ESP32 for another pairing."""
    entry = MockConfigEntry(
        domain=DOMAIN,
        unique_id="device-1",
        data={
            CONF_HOST: "denon-volume.local",
            CONF_PORT: 80,
            CONF_TOKEN: "b" * 64,
            CONF_ENTITY_ID: APPLE_TV_ENTITY_ID,
        },
    )
    with patch(
        "custom_components.denon_app_volume.async_unpair", new_callable=AsyncMock
    ) as unpair:
        await async_remove_entry(hass, entry)

    unpair.assert_awaited_once()
