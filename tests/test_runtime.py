"""Tests for the Denon App Volume runtime bridge."""

from types import SimpleNamespace
from unittest.mock import AsyncMock, MagicMock, Mock, patch

from aiohttp import ClientError, ClientResponseError
import pytest

from homeassistant.const import (
    CONF_ENTITY_ID,
    CONF_HOST,
    CONF_PORT,
    CONF_TOKEN,
    STATE_IDLE,
    STATE_OFF,
    STATE_PAUSED,
    STATE_UNAVAILABLE,
    STATE_UNKNOWN,
)
from homeassistant.core import HomeAssistant, State
from homeassistant.exceptions import HomeAssistantError
from homeassistant.helpers import entity_registry as er
from pytest_homeassistant_custom_component.common import MockConfigEntry

from custom_components.denon_app_volume import (
    AppRelay,
    _app_payload,
    async_remove_entry,
    async_setup_entry,
)
from custom_components.denon_app_volume.api import (
    AppVolume,
    BackupSnapshot,
    async_get_backup,
    async_put_backup,
    async_send_app,
    async_unpair,
    parse_backup_apps,
)
from custom_components.denon_app_volume.const import DOMAIN

APPLE_TV_ENTITY_ID = "media_player.example_apple_tv"
FOREGROUND_ENTITY_ID = "sensor.example_apple_tv_foreground"


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


async def test_relay_delivers_playback_app_and_coalesces() -> None:
    """Playing metadata reaches the ESP32 app endpoint."""
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
        assert send.await_args.args[-2:] == ("com.netflix.Netflix", "Netflix")
        assert send.await_args.kwargs["playback_active"] is True

        await relay._async_send_current(force=True)
        assert send.await_count == 2


async def test_relay_clears_once_when_playback_metadata_disappears() -> None:
    """Leaving Netflix stops later volume changes from being learned as Netflix."""
    known = State(
        APPLE_TV_ENTITY_ID,
        "playing",
        {"app_id": "com.netflix.Netflix", "app_name": "Netflix"},
    )
    missing = State(APPLE_TV_ENTITY_ID, STATE_OFF, {})
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


async def test_relay_does_not_heartbeat_a_redundant_clear_before_fresh_app() -> None:
    """A heartbeat cannot race a fresh MQTT foreground with a stale clear."""
    states = {
        FOREGROUND_ENTITY_ID: State(
            FOREGROUND_ENTITY_ID,
            "none",
            {"event_kind": "foreground_clear"},
        ),
        APPLE_TV_ENTITY_ID: State(APPLE_TV_ENTITY_ID, STATE_IDLE, {}),
    }
    hass = SimpleNamespace(
        states=SimpleNamespace(get=Mock(side_effect=states.get))
    )
    entry = SimpleNamespace(
        data={
            CONF_ENTITY_ID: FOREGROUND_ENTITY_ID,
            CONF_HOST: "denon-volume.local",
            CONF_PORT: 80,
            CONF_TOKEN: "e" * 64,
        }
    )

    with patch(
        "custom_components.denon_app_volume.async_get_clientsession",
        return_value=object(),
    ):
        relay = AppRelay(
            hass,
            entry,
            playback_entity_id=APPLE_TV_ENTITY_ID,
        )

    with patch(
        "custom_components.denon_app_volume.async_send_app",
        new_callable=AsyncMock,
    ) as send:
        await relay._async_send_current(force=True)
        await relay._async_send_current(force=True)
        states[FOREGROUND_ENTITY_ID] = State(
            FOREGROUND_ENTITY_ID,
            "Home",
            {"app_id": "com.apple.HeadBoard", "app_name": "Home"},
        )
        await relay._async_send_current(force=False)

    assert [call.args[-2:] for call in send.await_args_list] == [
        ("", ""),
        ("com.apple.HeadBoard", "Home"),
    ]


async def test_relay_does_not_clear_when_apple_tv_becomes_unavailable() -> None:
    """A transport outage revokes permission without changing ownership."""
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

    assert send.await_count == 2
    assert send.await_args_list[0].args[-2:] == (
        "com.netflix.Netflix",
        "Netflix",
    )
    assert send.await_args_list[0].kwargs["playback_active"] is True
    assert send.await_args_list[1].args[-2:] == (
        "com.netflix.Netflix",
        "Netflix",
    )
    assert send.await_args_list[1].kwargs["playback_active"] is None


async def test_mqtt_tombstone_preserves_owner_during_eof_race() -> None:
    """The tombstone preceding offline cannot safely prove an app departure."""
    states = {
        FOREGROUND_ENTITY_ID: State(FOREGROUND_ENTITY_ID, STATE_UNKNOWN, {}),
        APPLE_TV_ENTITY_ID: State(APPLE_TV_ENTITY_ID, "idle", {}),
    }
    hass = SimpleNamespace(states=SimpleNamespace(get=states.get))
    entry = SimpleNamespace(
        data={
            CONF_ENTITY_ID: FOREGROUND_ENTITY_ID,
            CONF_HOST: "denon-volume.local",
            CONF_PORT: 80,
            CONF_TOKEN: "e" * 64,
        }
    )

    with patch(
        "custom_components.denon_app_volume.async_get_clientsession",
        return_value=object(),
    ):
        relay = AppRelay(hass, entry, playback_entity_id=APPLE_TV_ENTITY_ID)

    with patch(
        "custom_components.denon_app_volume.async_send_app", new_callable=AsyncMock
    ) as send:
        await relay._async_send_current(force=False)
        send.assert_not_awaited()

        states[FOREGROUND_ENTITY_ID] = State(
            FOREGROUND_ENTITY_ID, STATE_UNAVAILABLE, {}
        )
        await relay._async_send_current(force=True)
        send.assert_not_awaited()

        states[FOREGROUND_ENTITY_ID] = State(
            FOREGROUND_ENTITY_ID,
            "none",
            {"event_kind": "foreground_clear"},
        )
        await relay._async_send_current(force=True)
        assert send.await_args.args[-2:] == ("", "")


async def test_active_playback_owner_overrides_foreground_until_handoff() -> None:
    """Browsing another app cannot steal volume ownership from active audio."""
    states = {
        FOREGROUND_ENTITY_ID: State(
            FOREGROUND_ENTITY_ID,
            "Netflix",
            {"app_id": "com.netflix.Netflix", "app_name": "Netflix"},
        ),
        APPLE_TV_ENTITY_ID: State(
            APPLE_TV_ENTITY_ID,
            "playing",
            {"app_id": "com.spotify.client", "app_name": "Spotify"},
        ),
    }
    hass = SimpleNamespace(states=SimpleNamespace(get=states.get))
    entry = SimpleNamespace(
        data={
            CONF_ENTITY_ID: FOREGROUND_ENTITY_ID,
            CONF_HOST: "denon-volume.local",
            CONF_PORT: 80,
            CONF_TOKEN: "e" * 64,
        }
    )

    with patch(
        "custom_components.denon_app_volume.async_get_clientsession",
        return_value=object(),
    ):
        relay = AppRelay(hass, entry, playback_entity_id=APPLE_TV_ENTITY_ID)

    with patch(
        "custom_components.denon_app_volume.async_send_app", new_callable=AsyncMock
    ) as send:
        await relay._async_send_current(force=False)
        assert send.await_args.args[-2:] == ("com.spotify.client", "Spotify")
        assert send.await_args.kwargs["playback_active"] is True

        states[FOREGROUND_ENTITY_ID] = State(
            FOREGROUND_ENTITY_ID,
            "YouTube",
            {"app_id": "com.google.ios.youtube", "app_name": "YouTube"},
        )
        await relay._async_send_current(force=False)
        assert send.await_count == 1

        states[APPLE_TV_ENTITY_ID] = State(APPLE_TV_ENTITY_ID, STATE_OFF, {})
        await relay._async_send_current(force=True)
        assert send.await_count == 2
        assert send.await_args.args[-2:] == ("com.spotify.client", "Spotify")
        assert send.await_args.kwargs["playback_active"] is None

        states[APPLE_TV_ENTITY_ID] = State(
            APPLE_TV_ENTITY_ID,
            "playing",
            {"app_id": "com.google.ios.youtube", "app_name": "YouTube"},
        )
        await relay._async_send_current(force=False)
        assert send.await_args.args[-2:] == (
            "com.google.ios.youtube",
            "YouTube",
        )
        assert send.await_args.kwargs["playback_active"] is True


async def test_disconnected_off_playback_fails_closed_for_mqtt_foreground() -> None:
    """A disconnected playback entity cannot authorize foreground restores."""
    states = {
        FOREGROUND_ENTITY_ID: State(
            FOREGROUND_ENTITY_ID,
            "MemoryPoster",
            {
                "app_id": "com.apple.IdleScreen.MemoryPoster",
                "app_name": "MemoryPoster",
            },
        ),
        APPLE_TV_ENTITY_ID: State(APPLE_TV_ENTITY_ID, STATE_OFF, {}),
    }
    hass = SimpleNamespace(states=SimpleNamespace(get=states.get))
    entry = SimpleNamespace(
        data={
            CONF_ENTITY_ID: FOREGROUND_ENTITY_ID,
            CONF_HOST: "denon-volume.local",
            CONF_PORT: 80,
            CONF_TOKEN: "e" * 64,
        }
    )

    with patch(
        "custom_components.denon_app_volume.async_get_clientsession",
        return_value=object(),
    ):
        relay = AppRelay(hass, entry, playback_entity_id=APPLE_TV_ENTITY_ID)

    with patch(
        "custom_components.denon_app_volume.async_send_app", new_callable=AsyncMock
    ) as send:
        await relay._async_send_current(force=True)

    send.assert_not_awaited()


async def test_unavailable_playback_fails_closed_until_connected_idle() -> None:
    """Startup gaps and disconnected off states fail closed without deferral."""
    foreground = State(
        FOREGROUND_ENTITY_ID,
        "Netflix",
        {"app_id": "com.netflix.Netflix", "app_name": "Netflix"},
    )
    states = {FOREGROUND_ENTITY_ID: foreground}
    hass = SimpleNamespace(states=SimpleNamespace(get=states.get))
    entry = SimpleNamespace(
        data={
            CONF_ENTITY_ID: FOREGROUND_ENTITY_ID,
            CONF_HOST: "denon-volume.local",
            CONF_PORT: 80,
            CONF_TOKEN: "e" * 64,
        }
    )

    with patch(
        "custom_components.denon_app_volume.async_get_clientsession",
        return_value=object(),
    ):
        relay = AppRelay(hass, entry, playback_entity_id=APPLE_TV_ENTITY_ID)

    with patch(
        "custom_components.denon_app_volume.async_send_app", new_callable=AsyncMock
    ) as send:
        await relay._async_send_current(force=True)
        send.assert_not_awaited()

        states[APPLE_TV_ENTITY_ID] = State(APPLE_TV_ENTITY_ID, STATE_OFF, {})
        await relay._async_send_current(force=True)
        send.assert_not_awaited()

        states[APPLE_TV_ENTITY_ID] = State(APPLE_TV_ENTITY_ID, "idle", {})
        await relay._async_send_current(force=True)
        assert send.await_args.args[-2:] == ("com.netflix.Netflix", "Netflix")
        assert send.await_args.kwargs["playback_active"] is False

        states[APPLE_TV_ENTITY_ID] = State(APPLE_TV_ENTITY_ID, "playing", {})
        await relay._async_send_current(force=True)
        assert send.await_count == 2
        assert send.await_args.args[-2:] == ("com.netflix.Netflix", "Netflix")
        assert send.await_args.kwargs["playback_active"] is None


@pytest.mark.parametrize("safe_state", [STATE_IDLE, STATE_PAUSED])
async def test_same_app_playback_start_revokes_safe_permission(
    safe_state: str,
) -> None:
    """Playback status participates in relay deduplication."""
    foreground = State(
        FOREGROUND_ENTITY_ID,
        "Netflix",
        {"app_id": "com.netflix.Netflix", "app_name": "Netflix"},
    )
    states = {
        FOREGROUND_ENTITY_ID: foreground,
        APPLE_TV_ENTITY_ID: State(APPLE_TV_ENTITY_ID, safe_state, {}),
    }
    hass = SimpleNamespace(states=SimpleNamespace(get=states.get))
    entry = SimpleNamespace(
        data={
            CONF_ENTITY_ID: FOREGROUND_ENTITY_ID,
            CONF_HOST: "denon-volume.local",
            CONF_PORT: 80,
            CONF_TOKEN: "e" * 64,
        }
    )

    with patch(
        "custom_components.denon_app_volume.async_get_clientsession",
        return_value=object(),
    ):
        relay = AppRelay(hass, entry, playback_entity_id=APPLE_TV_ENTITY_ID)

    with patch(
        "custom_components.denon_app_volume.async_send_app", new_callable=AsyncMock
    ) as send:
        await relay._async_send_current(force=False)
        states[APPLE_TV_ENTITY_ID] = State(
            APPLE_TV_ENTITY_ID,
            "playing",
            {"app_id": "com.netflix.Netflix", "app_name": "Netflix"},
        )
        await relay._async_send_current(force=False)

    assert [call.kwargs["playback_active"] for call in send.await_args_list] == [
        False,
        True,
    ]


async def test_heartbeat_does_not_renew_foreground_permission() -> None:
    """Only a fresh foreground event grants the short mute-restore lease."""
    states = {
        FOREGROUND_ENTITY_ID: State(
            FOREGROUND_ENTITY_ID,
            "Netflix",
            {"app_id": "com.netflix.Netflix", "app_name": "Netflix"},
        ),
        APPLE_TV_ENTITY_ID: State(
            APPLE_TV_ENTITY_ID,
            STATE_IDLE,
            {},
        ),
    }
    hass = SimpleNamespace(states=SimpleNamespace(get=states.get))
    entry = SimpleNamespace(
        data={
            CONF_ENTITY_ID: FOREGROUND_ENTITY_ID,
            CONF_HOST: "denon-volume.local",
            CONF_PORT: 80,
            CONF_TOKEN: "e" * 64,
        }
    )

    with patch(
        "custom_components.denon_app_volume.async_get_clientsession",
        return_value=object(),
    ):
        relay = AppRelay(hass, entry, playback_entity_id=APPLE_TV_ENTITY_ID)

    with patch(
        "custom_components.denon_app_volume.async_send_app", new_callable=AsyncMock
    ) as send:
        await relay._async_send_current(force=False, fresh_foreground=True)
        await relay._async_send_current(force=True, fresh_foreground=False)

    assert [call.kwargs["playback_active"] for call in send.await_args_list] == [
        False,
        None,
    ]


async def test_failed_foreground_delivery_retries_same_event() -> None:
    """A transport failure cannot consume the latest foreground event."""
    event_id = "1" * 32
    states = {
        FOREGROUND_ENTITY_ID: State(
            FOREGROUND_ENTITY_ID,
            "Home",
            {
                "app_id": "com.apple.HeadBoard",
                "app_name": "Home",
                "event_id": event_id,
            },
        ),
        APPLE_TV_ENTITY_ID: State(APPLE_TV_ENTITY_ID, STATE_PAUSED, {}),
    }
    hass = SimpleNamespace(states=SimpleNamespace(get=states.get))
    entry = SimpleNamespace(
        data={
            CONF_ENTITY_ID: FOREGROUND_ENTITY_ID,
            CONF_HOST: "denon-volume.local",
            CONF_PORT: 80,
            CONF_TOKEN: "e" * 64,
        }
    )

    with patch(
        "custom_components.denon_app_volume.async_get_clientsession",
        return_value=object(),
    ):
        relay = AppRelay(hass, entry, playback_entity_id=APPLE_TV_ENTITY_ID)
    relay.async_notify(
        SimpleNamespace(data={"entity_id": FOREGROUND_ENTITY_ID})
    )

    with patch(
        "custom_components.denon_app_volume.async_send_app",
        new_callable=AsyncMock,
        side_effect=[ClientError("down"), None],
    ) as send:
        assert await relay._async_send_pending(force=False) is False
        assert relay._foreground_delivered_generation == 0
        assert await relay._async_send_pending(force=False) is True

    assert relay._foreground_delivered_generation == 1
    assert [call.kwargs["event_id"] for call in send.await_args_list] == [
        event_id,
        event_id,
    ]
    assert [
        call.kwargs["playback_active"] for call in send.await_args_list
    ] == [False, False]


@pytest.mark.parametrize("first_succeeds", [False, True])
async def test_newer_foreground_survives_inflight_delivery(
    first_succeeds: bool,
) -> None:
    """An in-flight older request cannot consume a newer foreground event."""
    home_event_id = "1" * 32
    netflix_event_id = "2" * 32
    states = {
        FOREGROUND_ENTITY_ID: State(
            FOREGROUND_ENTITY_ID,
            "Home",
            {
                "app_id": "com.apple.HeadBoard",
                "app_name": "Home",
                "event_id": home_event_id,
            },
        ),
        APPLE_TV_ENTITY_ID: State(APPLE_TV_ENTITY_ID, STATE_PAUSED, {}),
    }
    hass = SimpleNamespace(states=SimpleNamespace(get=states.get))
    entry = SimpleNamespace(
        data={
            CONF_ENTITY_ID: FOREGROUND_ENTITY_ID,
            CONF_HOST: "denon-volume.local",
            CONF_PORT: 80,
            CONF_TOKEN: "e" * 64,
        }
    )

    with patch(
        "custom_components.denon_app_volume.async_get_clientsession",
        return_value=object(),
    ):
        relay = AppRelay(hass, entry, playback_entity_id=APPLE_TV_ENTITY_ID)
    relay.async_notify(
        SimpleNamespace(data={"entity_id": FOREGROUND_ENTITY_ID})
    )

    delivery_count = 0

    async def delivery(*_args: object, **_kwargs: object) -> None:
        nonlocal delivery_count
        delivery_count += 1
        if delivery_count == 1:
            states[FOREGROUND_ENTITY_ID] = State(
                FOREGROUND_ENTITY_ID,
                "Netflix",
                {
                    "app_id": "com.netflix.Netflix",
                    "app_name": "Netflix",
                    "event_id": netflix_event_id,
                },
            )
            relay.async_notify(
                SimpleNamespace(data={"entity_id": FOREGROUND_ENTITY_ID})
            )
            if not first_succeeds:
                raise ClientError("down")

    with patch(
        "custom_components.denon_app_volume.async_send_app",
        new_callable=AsyncMock,
        side_effect=delivery,
    ) as send:
        await relay._async_send_pending(force=False)
        assert relay._foreground_delivered_generation == 0
        assert await relay._async_send_pending(force=False) is True

    assert relay._foreground_delivered_generation == 2
    assert [call.args[-2:] for call in send.await_args_list] == [
        ("com.apple.HeadBoard", "Home"),
        ("com.netflix.Netflix", "Netflix"),
    ]
    assert [call.kwargs["event_id"] for call in send.await_args_list] == [
        home_event_id,
        netflix_event_id,
    ]


async def test_clear_supersedes_failed_foreground_delivery() -> None:
    """A newer clear is sent instead of replaying an older app."""
    states = {
        FOREGROUND_ENTITY_ID: State(
            FOREGROUND_ENTITY_ID,
            "Home",
            {
                "app_id": "com.apple.HeadBoard",
                "app_name": "Home",
                "event_id": "1" * 32,
            },
        ),
        APPLE_TV_ENTITY_ID: State(APPLE_TV_ENTITY_ID, STATE_PAUSED, {}),
    }
    hass = SimpleNamespace(states=SimpleNamespace(get=states.get))
    entry = SimpleNamespace(
        data={
            CONF_ENTITY_ID: FOREGROUND_ENTITY_ID,
            CONF_HOST: "denon-volume.local",
            CONF_PORT: 80,
            CONF_TOKEN: "e" * 64,
        }
    )

    with patch(
        "custom_components.denon_app_volume.async_get_clientsession",
        return_value=object(),
    ):
        relay = AppRelay(hass, entry, playback_entity_id=APPLE_TV_ENTITY_ID)
    relay.async_notify(
        SimpleNamespace(data={"entity_id": FOREGROUND_ENTITY_ID})
    )

    with patch(
        "custom_components.denon_app_volume.async_send_app",
        new_callable=AsyncMock,
        side_effect=[ClientError("down"), None],
    ) as send:
        assert await relay._async_send_pending(force=False) is False
        states[FOREGROUND_ENTITY_ID] = State(
            FOREGROUND_ENTITY_ID,
            "none",
            {"event_kind": "foreground_clear", "event_id": "2" * 32},
        )
        relay.async_notify(
            SimpleNamespace(data={"entity_id": FOREGROUND_ENTITY_ID})
        )
        assert await relay._async_send_pending(force=False) is True

    assert [call.args[-2:] for call in send.await_args_list] == [
        ("com.apple.HeadBoard", "Home"),
        ("", ""),
    ]
    assert send.await_args_list[1].kwargs["event_id"] == "2" * 32


async def test_playback_off_defers_failed_foreground_retry() -> None:
    """Playback loss blocks a retry until ownership is authoritative again."""
    netflix_event_id = "1" * 32
    home_event_id = "2" * 32
    states = {
        FOREGROUND_ENTITY_ID: State(
            FOREGROUND_ENTITY_ID,
            "Netflix",
            {
                "app_id": "com.netflix.Netflix",
                "app_name": "Netflix",
                "event_id": netflix_event_id,
            },
        ),
        APPLE_TV_ENTITY_ID: State(APPLE_TV_ENTITY_ID, STATE_PAUSED, {}),
    }
    hass = SimpleNamespace(states=SimpleNamespace(get=states.get))
    entry = SimpleNamespace(
        data={
            CONF_ENTITY_ID: FOREGROUND_ENTITY_ID,
            CONF_HOST: "denon-volume.local",
            CONF_PORT: 80,
            CONF_TOKEN: "e" * 64,
        }
    )

    with patch(
        "custom_components.denon_app_volume.async_get_clientsession",
        return_value=object(),
    ):
        relay = AppRelay(hass, entry, playback_entity_id=APPLE_TV_ENTITY_ID)
    relay.async_notify(
        SimpleNamespace(data={"entity_id": FOREGROUND_ENTITY_ID})
    )

    with patch(
        "custom_components.denon_app_volume.async_send_app",
        new_callable=AsyncMock,
        side_effect=[None, ClientError("down"), None],
    ) as send:
        assert await relay._async_send_pending(force=False) is True
        states[FOREGROUND_ENTITY_ID] = State(
            FOREGROUND_ENTITY_ID,
            "Home",
            {
                "app_id": "com.apple.HeadBoard",
                "app_name": "Home",
                "event_id": home_event_id,
            },
        )
        relay.async_notify(
            SimpleNamespace(data={"entity_id": FOREGROUND_ENTITY_ID})
        )
        assert await relay._async_send_pending(force=False) is False
        states[APPLE_TV_ENTITY_ID] = State(APPLE_TV_ENTITY_ID, STATE_OFF, {})
        assert await relay._async_send_pending(force=False) is None
        assert send.await_count == 2
        assert relay._foreground_delivered_generation == 1
        states[APPLE_TV_ENTITY_ID] = State(
            APPLE_TV_ENTITY_ID, STATE_PAUSED, {}
        )
        assert await relay._async_send_pending(force=False) is True

    assert relay._foreground_delivered_generation == 2
    assert send.await_count == 3
    assert send.await_args.kwargs == {
        "playback_active": False,
        "event_id": home_event_id,
    }


@pytest.mark.parametrize(
    "playback_state", ["buffering", "on", "standby", "other"]
)
async def test_other_playback_states_revoke_muted_restore(
    playback_state: str,
) -> None:
    """Every responsive but non-idle state revokes the short permission."""
    states = {
        FOREGROUND_ENTITY_ID: State(
            FOREGROUND_ENTITY_ID,
            "Netflix",
            {"app_id": "com.netflix.Netflix", "app_name": "Netflix"},
        ),
        APPLE_TV_ENTITY_ID: State(APPLE_TV_ENTITY_ID, STATE_IDLE, {}),
    }
    hass = SimpleNamespace(states=SimpleNamespace(get=states.get))
    entry = SimpleNamespace(
        data={
            CONF_ENTITY_ID: FOREGROUND_ENTITY_ID,
            CONF_HOST: "denon-volume.local",
            CONF_PORT: 80,
            CONF_TOKEN: "e" * 64,
        }
    )

    with patch(
        "custom_components.denon_app_volume.async_get_clientsession",
        return_value=object(),
    ):
        relay = AppRelay(hass, entry, playback_entity_id=APPLE_TV_ENTITY_ID)

    with patch(
        "custom_components.denon_app_volume.async_send_app", new_callable=AsyncMock
    ) as send:
        await relay._async_send_current(force=False)
        states[APPLE_TV_ENTITY_ID] = State(
            APPLE_TV_ENTITY_ID, playback_state, {}
        )
        await relay._async_send_current(force=False)

    assert [call.kwargs["playback_active"] for call in send.await_args_list] == [
        False,
        None,
    ]


async def test_setup_pairs_mqtt_foreground_with_only_apple_tv_player(
    hass: HomeAssistant,
) -> None:
    """The existing Apple TV entity supplies playback ownership."""
    registry = er.async_get(hass)
    foreground = registry.async_get_or_create(
        "sensor",
        "mqtt",
        "foreground",
        suggested_object_id="example_apple_tv_foreground",
    )
    playback = registry.async_get_or_create(
        "media_player",
        "apple_tv",
        "playback",
        suggested_object_id="example_apple_tv",
    )
    entry = MockConfigEntry(
        domain=DOMAIN,
        unique_id="device-1",
        data={
            CONF_ENTITY_ID: foreground.entity_id,
            CONF_HOST: "denon-volume.local",
            CONF_PORT: 80,
            CONF_TOKEN: "e" * 64,
        },
    )
    entry.add_to_hass(hass)
    relay = MagicMock()
    relay.async_run = AsyncMock()

    with patch(
        "custom_components.denon_app_volume.AppRelay", return_value=relay
    ) as relay_class:
        assert await async_setup_entry(hass, entry)
        await hass.async_block_till_done()

    assert relay_class.call_args.args[-1] == playback.entity_id


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


async def test_app_handoff_carries_only_known_playback_safety() -> None:
    """The ESP receives false permission or an omitted fail-closed value."""
    session = MagicMock()
    response = MagicMock()
    request = MagicMock()
    request.__aenter__ = AsyncMock(return_value=response)
    request.__aexit__ = AsyncMock(return_value=None)
    session.post.return_value = request

    await async_send_app(
        session,
        "denon-volume.local",
        80,
        "a" * 64,
        "com.netflix.Netflix",
        "Netflix",
        playback_active=False,
    )
    assert session.post.call_args.kwargs["json"] == {
        "app_id": "com.netflix.Netflix",
        "app_name": "Netflix",
        "playback_active": False,
    }

    await async_send_app(
        session,
        "denon-volume.local",
        80,
        "a" * 64,
        "com.netflix.Netflix",
        "Netflix",
    )
    assert session.post.call_args.kwargs["json"] == {
        "app_id": "com.netflix.Netflix",
        "app_name": "Netflix",
    }


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
