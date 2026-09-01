"""Tests for the Denon App Volume runtime bridge."""

from types import SimpleNamespace
from unittest.mock import AsyncMock, MagicMock, Mock, patch

from aiohttp import ClientResponseError
import pytest

from homeassistant.const import (
    CONF_ENTITY_ID,
    CONF_HOST,
    CONF_PORT,
    CONF_TOKEN,
    STATE_OFF,
    STATE_PAUSED,
    STATE_UNAVAILABLE,
    STATE_UNKNOWN,
)
from homeassistant.core import HomeAssistant, State
from homeassistant.exceptions import HomeAssistantError
from pytest_homeassistant_custom_component.common import MockConfigEntry

from custom_components.denon_app_volume import (
    AppRelay,
    _app_payload,
    async_remove_entry,
)
from custom_components.denon_app_volume.api import (
    AppVolume,
    BackupSnapshot,
    async_get_backup,
    async_put_backup,
    async_unpair,
    parse_backup_apps,
)
from custom_components.denon_app_volume.const import DOMAIN

APPLE_TV_ENTITY_ID = "media_player.example_apple_tv"


@pytest.mark.parametrize(
    ("media_state", "expected"),
    [
        (STATE_UNAVAILABLE, None),
        (STATE_UNKNOWN, None),
        ("idle", ("", "")),
        (STATE_OFF, ("", "")),
    ],
)
def test_missing_app_clears_only_for_responsive_states(
    media_state: str, expected: tuple[str, str] | None
) -> None:
    """Only a responsive Apple TV can safely end playback app ownership."""
    assert _app_payload(State(APPLE_TV_ENTITY_ID, media_state, {})) == expected


def test_off_clears_stale_app_attributes() -> None:
    """An off Apple TV must not keep attributing volume to its previous app."""
    state = State(
        APPLE_TV_ENTITY_ID,
        STATE_OFF,
        {"app_id": "com.netflix.Netflix", "app_name": "Netflix"},
    )

    assert _app_payload(state) == ("", "")


def test_missing_app_name_falls_back_to_identifier() -> None:
    """The ESP32 contract always receives a non-empty display name."""
    state = State(
        APPLE_TV_ENTITY_ID,
        "playing",
        {"app_id": "org.videolan.vlc-ios"},
    )

    assert _app_payload(state) == ("org.videolan.vlc-ios", "org.videolan.vlc-ios")


@pytest.mark.parametrize("media_state", ["playing", STATE_PAUSED])
async def test_relay_delivers_playback_app_and_coalesces(media_state: str) -> None:
    """Playing and paused metadata both reach the ESP32 app endpoint."""
    state = State(
        APPLE_TV_ENTITY_ID,
        media_state,
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
        assert send.await_args.args[-2:] == ("com.netflix.Netflix", "Netflix")

        await relay._async_send_current(force=True)
        assert send.await_count == 2


async def test_relay_clears_once_when_playback_metadata_disappears() -> None:
    """Leaving Netflix stops later volume changes from being learned as Netflix."""
    known = State(
        APPLE_TV_ENTITY_ID,
        "playing",
        {"app_id": "com.netflix.Netflix", "app_name": "Netflix"},
    )
    missing = State(APPLE_TV_ENTITY_ID, "idle", {})
    hass = SimpleNamespace(
        states=SimpleNamespace(get=Mock(side_effect=[known, missing, missing]))
    )
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
        await relay._async_send_current(force=False)

    assert send.await_count == 2
    assert send.await_args_list[0].args[-2:] == (
        "com.netflix.Netflix",
        "Netflix",
    )
    assert send.await_args_list[1].args[-2:] == ("", "")


async def test_relay_does_not_clear_when_apple_tv_becomes_unavailable() -> None:
    """A transport outage is not evidence that playback ownership ended."""
    known = State(
        APPLE_TV_ENTITY_ID,
        "playing",
        {"app_id": "com.netflix.Netflix", "app_name": "Netflix"},
    )
    unavailable = State(APPLE_TV_ENTITY_ID, STATE_UNAVAILABLE, {})
    hass = SimpleNamespace(
        states=SimpleNamespace(get=Mock(side_effect=[known, unavailable]))
    )
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

    send.assert_awaited_once()
    assert send.await_args.args[-2:] == ("com.netflix.Netflix", "Netflix")


async def test_store_load_failure_does_not_stop_playback_forwarding(caplog) -> None:
    """A broken HA store degrades backup without taking down the app relay."""
    state = State(
        APPLE_TV_ENTITY_ID,
        "playing",
        {"app_id": "com.netflix.Netflix", "app_name": "Netflix"},
    )
    store = MagicMock()
    store.async_load = AsyncMock(side_effect=HomeAssistantError("store failed"))
    hass = SimpleNamespace(states=SimpleNamespace(get=Mock(return_value=state)))
    entry = SimpleNamespace(
        data={
            CONF_ENTITY_ID: APPLE_TV_ENTITY_ID,
            CONF_HOST: "denon-volume.local",
            CONF_PORT: 80,
            CONF_TOKEN: "9" * 64,
        }
    )

    with (
        patch(
            "custom_components.denon_app_volume.async_get_clientsession",
            return_value=object(),
        ),
        patch("custom_components.denon_app_volume.Store", return_value=store),
        patch(
            "custom_components.denon_app_volume.async_send_app",
            new_callable=AsyncMock,
        ) as send,
    ):
        relay = AppRelay(hass, entry)
        await relay._async_sync_backup()
        await relay._async_send_current(force=True)
        await relay._async_sync_backup()

    send.assert_awaited_once()
    assert send.await_args.args[-2:] == ("com.netflix.Netflix", "Netflix")
    assert caplog.text.count("Could not sync the app-volume backup") == 1


async def test_empty_device_restores_ha_backup_with_readback() -> None:
    """A replacement ESP32 receives the HA copy before app updates begin."""
    apps = (AppVolume("com.netflix.Netflix", "Netflix", 130),)
    empty = BackupSnapshot(0, '"0"', ())
    restored = BackupSnapshot(1, '"1"', apps)
    store = MagicMock()
    store.async_load = AsyncMock(
        return_value={
            "schema": 1,
            "apps": [
                {
                    "app_id": "com.netflix.Netflix",
                    "app_name": "Netflix",
                    "volume_raw": 130,
                }
            ],
        }
    )
    hass = SimpleNamespace(
        states=SimpleNamespace(get=Mock(return_value=None)),
    )
    entry = SimpleNamespace(
        data={
            CONF_ENTITY_ID: APPLE_TV_ENTITY_ID,
            CONF_HOST: "denon-volume.local",
            CONF_PORT: 80,
            CONF_TOKEN: "1" * 64,
        }
    )

    with (
        patch(
            "custom_components.denon_app_volume.async_get_clientsession",
            return_value=object(),
        ),
        patch("custom_components.denon_app_volume.Store", return_value=store),
        patch(
            "custom_components.denon_app_volume.async_get_backup",
            AsyncMock(side_effect=[empty, restored]),
        ) as get_backup,
        patch(
            "custom_components.denon_app_volume.async_put_backup",
            new_callable=AsyncMock,
        ) as put_backup,
    ):
        relay = AppRelay(hass, entry)
        await relay._async_sync_backup()

    put_backup.assert_awaited_once_with(
        relay._session,
        "denon-volume.local",
        80,
        "1" * 64,
        '"0"',
        apps,
    )
    assert get_backup.await_count == 2
    store.async_delay_save.assert_not_called()


async def test_nonempty_device_wins_and_only_changed_content_is_saved() -> None:
    """The ESP32 remains primary and unchanged heartbeat snapshots are cheap."""
    apps = (AppVolume("com.netflix.Netflix", "Netflix", 130),)
    snapshot = BackupSnapshot(4, '"4"', apps)
    store = MagicMock()
    store.async_load = AsyncMock(return_value=None)
    hass = SimpleNamespace(states=SimpleNamespace(get=Mock(return_value=None)))
    entry = SimpleNamespace(
        data={
            CONF_ENTITY_ID: APPLE_TV_ENTITY_ID,
            CONF_HOST: "denon-volume.local",
            CONF_PORT: 80,
            CONF_TOKEN: "2" * 64,
        }
    )

    with (
        patch(
            "custom_components.denon_app_volume.async_get_clientsession",
            return_value=object(),
        ),
        patch("custom_components.denon_app_volume.Store", return_value=store),
        patch(
            "custom_components.denon_app_volume.async_get_backup",
            AsyncMock(return_value=snapshot),
        ),
        patch(
            "custom_components.denon_app_volume.async_put_backup",
            new_callable=AsyncMock,
        ) as put_backup,
    ):
        relay = AppRelay(hass, entry)
        await relay._async_sync_backup()
        await relay._async_sync_backup()

    put_backup.assert_not_awaited()
    store.async_delay_save.assert_called_once()
    assert store.async_delay_save.call_args.args[0]() == {
        "schema": 1,
        "apps": [
            {
                "app_id": "com.netflix.Netflix",
                "app_name": "Netflix",
                "volume_raw": 130,
            }
        ],
    }


async def test_backup_api_uses_bearer_and_opaque_etag() -> None:
    """Backup restore passes the authenticated GET validator back unchanged."""
    session = MagicMock()
    get_response = MagicMock()
    get_response.headers = {"ETag": 'W/"7"'}
    get_response.json = AsyncMock(
        return_value={
            "schema": 1,
            "revision": 7,
            "apps": [
                {
                    "app_id": "com.netflix.Netflix",
                    "app_name": "Netflix",
                    "volume_raw": 130,
                }
            ],
        }
    )
    get_request = MagicMock()
    get_request.__aenter__ = AsyncMock(return_value=get_response)
    get_request.__aexit__ = AsyncMock(return_value=None)
    session.get.return_value = get_request

    snapshot = await async_get_backup(
        session, "denon-volume.local", 80, "3" * 64
    )

    assert snapshot.etag == 'W/"7"'
    assert snapshot.apps == (
        AppVolume("com.netflix.Netflix", "Netflix", 130),
    )
    assert session.get.call_args.kwargs["headers"] == {
        "Authorization": f"Bearer {'3' * 64}"
    }

    put_response = MagicMock(status=204)
    put_request = MagicMock()
    put_request.__aenter__ = AsyncMock(return_value=put_response)
    put_request.__aexit__ = AsyncMock(return_value=None)
    session.put.return_value = put_request
    await async_put_backup(
        session,
        "denon-volume.local",
        80,
        "3" * 64,
        snapshot.etag,
        snapshot.apps,
    )
    assert session.put.call_args.kwargs["headers"]["If-Match"] == 'W/"7"'


def test_backup_validation_rejects_duplicates_and_boolean_volumes() -> None:
    """Malformed device or storage data never reaches ESP32 persistence."""
    duplicate = {
        "schema": 1,
        "apps": [
            {"app_id": "same", "app_name": "One", "volume_raw": 10},
            {"app_id": "same", "app_name": "Two", "volume_raw": 20},
        ],
    }
    boolean = {
        "schema": 1,
        "apps": [{"app_id": "app", "app_name": "App", "volume_raw": True}],
    }

    for payload in (duplicate, boolean):
        try:
            parse_backup_apps(payload)
        except ValueError:
            pass
        else:
            raise AssertionError("invalid backup was accepted")


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
