"""Tests for the Denon App Volume config flow."""

from ipaddress import IPv4Address
from unittest.mock import AsyncMock, patch

from homeassistant import config_entries
from homeassistant.config_entries import ConfigEntryState
from homeassistant.const import CONF_ENTITY_ID, CONF_HOST, CONF_PORT, CONF_TOKEN
from homeassistant.core import HomeAssistant
from homeassistant.data_entry_flow import FlowResultType
from homeassistant.helpers.service_info.zeroconf import ZeroconfServiceInfo
from pytest_homeassistant_custom_component.common import MockConfigEntry

from custom_components.denon_app_volume.api import DeviceInfo
from custom_components.denon_app_volume.const import DOMAIN

APPLE_TV_ENTITY_ID = "media_player.example_apple_tv"


def _discovery(ip: str = "192.0.2.5") -> ZeroconfServiceInfo:
    """Build one mDNS discovery record."""
    address = IPv4Address(ip)
    return ZeroconfServiceInfo(
        ip_address=address,
        ip_addresses=[address],
        port=80,
        hostname="denon-volume.local.",
        type="_denon-volume._tcp.local.",
        name="Denon Volume._denon-volume._tcp.local.",
        properties={"id": "device-1"},
    )


async def test_manual_setup_pairs_and_stores_device_token(
    hass: HomeAssistant,
) -> None:
    """Manual setup pairs once and stores the returned narrow token."""
    with (
        patch(
            "custom_components.denon_app_volume.config_flow.async_get_info",
            AsyncMock(return_value=DeviceInfo("device-1", "Living Room", False)),
        ),
        patch(
            "custom_components.denon_app_volume.config_flow.async_pair",
            AsyncMock(return_value="a" * 64),
        ) as pair,
    ):
        result = await hass.config_entries.flow.async_init(
            DOMAIN, context={"source": config_entries.SOURCE_USER}
        )
        assert result["type"] is FlowResultType.FORM

        result = await hass.config_entries.flow.async_configure(
            result["flow_id"],
            {
                CONF_HOST: "denon-volume.local",
                CONF_ENTITY_ID: APPLE_TV_ENTITY_ID,
            },
        )

    assert result["type"] is FlowResultType.CREATE_ENTRY
    assert result["data"][CONF_TOKEN] == "a" * 64
    assert result["data"][CONF_ENTITY_ID] == APPLE_TV_ENTITY_ID
    pair.assert_awaited_once()


async def test_manual_setup_rejects_existing_device(hass: HomeAssistant) -> None:
    """Manual rediscovery updates and reloads the active existing entry."""
    entry = MockConfigEntry(
        domain=DOMAIN,
        unique_id="device-1",
        state=ConfigEntryState.LOADED,
        data={
            CONF_HOST: "192.0.2.4",
            CONF_PORT: 80,
            CONF_TOKEN: "d" * 64,
            CONF_ENTITY_ID: APPLE_TV_ENTITY_ID,
        },
    )
    entry.add_to_hass(hass)

    with (
        patch(
            "custom_components.denon_app_volume.config_flow.async_get_info",
            AsyncMock(return_value=DeviceInfo("device-1", "Living Room", True)),
        ),
        patch.object(hass.config_entries, "async_schedule_reload") as reload_entry,
    ):
        result = await hass.config_entries.flow.async_init(
            DOMAIN,
            context={"source": config_entries.SOURCE_USER},
            data={
                CONF_HOST: "denon-volume.local",
                CONF_ENTITY_ID: APPLE_TV_ENTITY_ID,
            },
        )

    assert result["type"] is FlowResultType.ABORT
    assert result["reason"] == "already_configured"
    assert entry.data[CONF_HOST] == "denon-volume.local"
    reload_entry.assert_called_once_with(entry.entry_id)


async def test_discovery_confirms_then_pairs(hass: HomeAssistant) -> None:
    """Discovery needs only Apple TV selection and hardware confirmation."""
    with (
        patch(
            "custom_components.denon_app_volume.config_flow.async_get_info",
            AsyncMock(return_value=DeviceInfo("device-1", "Living Room", False)),
        ),
        patch(
            "custom_components.denon_app_volume.config_flow.async_pair",
            AsyncMock(return_value="b" * 64),
        ),
    ):
        result = await hass.config_entries.flow.async_init(
            DOMAIN,
            context={"source": config_entries.SOURCE_ZEROCONF},
            data=_discovery(),
        )
        assert result["type"] is FlowResultType.FORM
        assert result["step_id"] == "zeroconf_confirm"

        result = await hass.config_entries.flow.async_configure(
            result["flow_id"],
            {CONF_ENTITY_ID: APPLE_TV_ENTITY_ID},
        )

    assert result["type"] is FlowResultType.CREATE_ENTRY
    assert result["data"][CONF_TOKEN] == "b" * 64


async def test_rediscovery_updates_only_address(hass: HomeAssistant) -> None:
    """A stable device ID follows DHCP changes without replacing its token."""
    entry = MockConfigEntry(
        domain=DOMAIN,
        unique_id="device-1",
        data={
            CONF_HOST: "192.0.2.4",
            CONF_PORT: 80,
            CONF_TOKEN: "c" * 64,
            CONF_ENTITY_ID: APPLE_TV_ENTITY_ID,
        },
    )
    entry.add_to_hass(hass)

    with patch(
        "custom_components.denon_app_volume.config_flow.async_get_info",
        AsyncMock(return_value=DeviceInfo("device-1", "Living Room", True)),
    ):
        result = await hass.config_entries.flow.async_init(
            DOMAIN,
            context={"source": config_entries.SOURCE_ZEROCONF},
            data=_discovery(),
        )

    assert result["type"] is FlowResultType.ABORT
    assert result["reason"] == "already_configured"
    assert entry.data[CONF_HOST] == "192.0.2.5"
    assert entry.data[CONF_TOKEN] == "c" * 64


async def test_reauth_pairs_after_physical_token_reset(hass: HomeAssistant) -> None:
    """Token recovery updates the entry once the ESP32 reports unpaired."""
    entry = MockConfigEntry(
        domain=DOMAIN,
        unique_id="device-1",
        data={
            CONF_HOST: "denon-volume.local",
            CONF_PORT: 80,
            CONF_TOKEN: "e" * 64,
            CONF_ENTITY_ID: APPLE_TV_ENTITY_ID,
        },
    )
    entry.add_to_hass(hass)

    with (
        patch(
            "custom_components.denon_app_volume.config_flow.async_get_info",
            AsyncMock(return_value=DeviceInfo("device-1", "Living Room", False)),
        ),
        patch(
            "custom_components.denon_app_volume.config_flow.async_pair",
            AsyncMock(return_value="f" * 64),
        ),
    ):
        result = await entry.start_reauth_flow(hass)
        assert result["type"] is FlowResultType.FORM
        assert result["step_id"] == "reauth_confirm"

        result = await hass.config_entries.flow.async_configure(
            result["flow_id"], {}
        )

    assert result["type"] is FlowResultType.ABORT
    assert result["reason"] == "reauth_successful"
    assert entry.data[CONF_TOKEN] == "f" * 64
