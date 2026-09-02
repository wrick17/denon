"""Config flow for Denon App Volume."""

from __future__ import annotations

from collections.abc import Mapping
from typing import Any

import voluptuous as vol

from homeassistant.config_entries import ConfigFlow, ConfigFlowResult
from homeassistant.const import CONF_ENTITY_ID, CONF_HOST, CONF_PORT, CONF_TOKEN
from homeassistant.helpers.aiohttp_client import async_get_clientsession
from homeassistant.helpers.selector import EntitySelector, EntitySelectorConfig
from homeassistant.helpers.service_info.zeroconf import ZeroconfServiceInfo

from .api import (
    CannotConnect,
    CannotPair,
    DeviceInfo,
    async_get_info,
    async_pair,
    device_url,
)
from .const import DEFAULT_PORT, DOMAIN

_APP_SOURCE_SELECTOR = EntitySelector(
    EntitySelectorConfig(
        filter=[
            {"domain": "media_player", "integration": "apple_tv"},
            {"domain": "sensor", "integration": "mqtt"},
        ]
    )
)


class DenonAppVolumeConfigFlow(ConfigFlow, domain=DOMAIN):
    """Set up a discovered or manually entered ESP32."""

    VERSION = 1

    _host: str | None = None
    _port: int = DEFAULT_PORT
    _device: DeviceInfo | None = None

    async def async_step_user(
        self, user_input: dict[str, Any] | None = None
    ) -> ConfigFlowResult:
        """Handle manual setup."""
        errors: dict[str, str] = {}
        if user_input is not None:
            host = user_input[CONF_HOST].strip().rstrip(".")
            entity_id = user_input[CONF_ENTITY_ID]
            try:
                device = await self._async_probe(host, DEFAULT_PORT)
                await self.async_set_unique_id(
                    device.device_id, raise_on_progress=False
                )
                self._abort_if_unique_id_configured(
                    updates={CONF_HOST: host, CONF_PORT: DEFAULT_PORT},
                    reload_on_update=True,
                )
                if device.paired:
                    errors["base"] = "token_reset_required"
                    return self._show_user_form(errors)
                return await self._async_pair_and_create(
                    host, DEFAULT_PORT, device, entity_id
                )
            except CannotConnect:
                errors["base"] = "cannot_connect"
            except CannotPair:
                errors["base"] = "cannot_pair"

        return self._show_user_form(errors)

    def _show_user_form(self, errors: dict[str, str]) -> ConfigFlowResult:
        """Show manual setup without embedding any installation defaults."""
        return self.async_show_form(
            step_id="user",
            data_schema=vol.Schema(
                {
                    vol.Required(CONF_HOST): str,
                    vol.Required(CONF_ENTITY_ID): _APP_SOURCE_SELECTOR,
                }
            ),
            errors=errors,
        )

    async def async_step_zeroconf(
        self, discovery_info: ZeroconfServiceInfo
    ) -> ConfigFlowResult:
        """Handle ESP32 discovery and update an existing entry's address."""
        port = discovery_info.port or DEFAULT_PORT
        try:
            device = await self._async_probe(discovery_info.host, port)
        except CannotConnect:
            return self.async_abort(reason="cannot_connect")

        await self.async_set_unique_id(device.device_id)
        self._abort_if_unique_id_configured(
            updates={
                CONF_HOST: discovery_info.host,
                CONF_PORT: port,
            },
            reload_on_update=True,
        )

        self._host = discovery_info.host
        self._port = port
        self._device = device
        self.context.update(
            {
                "title_placeholders": {"name": device.name},
                "configuration_url": str(
                    device_url(discovery_info.host, port)
                ),
            }
        )
        return await self.async_step_zeroconf_confirm()

    async def async_step_zeroconf_confirm(
        self, user_input: dict[str, Any] | None = None
    ) -> ConfigFlowResult:
        """Confirm a discovered ESP32 and choose its Apple TV."""
        if self._host is None or self._device is None:
            return self.async_abort(reason="cannot_connect")

        errors: dict[str, str] = {}
        if user_input is not None:
            if self._device.paired:
                errors["base"] = "token_reset_required"
                return self._show_zeroconf_form(errors)
            try:
                return await self._async_pair_and_create(
                    self._host,
                    self._port,
                    self._device,
                    user_input[CONF_ENTITY_ID],
                )
            except CannotPair:
                errors["base"] = "cannot_pair"

        return self._show_zeroconf_form(errors)

    def _show_zeroconf_form(self, errors: dict[str, str]) -> ConfigFlowResult:
        """Show discovery confirmation with its single Apple TV choice."""
        assert self._device is not None
        return self.async_show_form(
            step_id="zeroconf_confirm",
            data_schema=vol.Schema(
                {vol.Required(CONF_ENTITY_ID): _APP_SOURCE_SELECTOR}
            ),
            description_placeholders={"name": self._device.name},
            errors=errors,
        )

    async def async_step_reconfigure(
        self, user_input: dict[str, Any] | None = None
    ) -> ConfigFlowResult:
        """Change the app identity source without replacing the device token."""
        entry = self._get_reconfigure_entry()
        if user_input is not None:
            return self.async_update_reload_and_abort(
                entry,
                data_updates={CONF_ENTITY_ID: user_input[CONF_ENTITY_ID]},
            )

        return self.async_show_form(
            step_id="reconfigure",
            data_schema=self.add_suggested_values_to_schema(
                vol.Schema({vol.Required(CONF_ENTITY_ID): _APP_SOURCE_SELECTOR}),
                entry.data,
            ),
        )

    async def async_step_reauth(
        self, _entry_data: Mapping[str, Any]
    ) -> ConfigFlowResult:
        """Start token recovery after the ESP32 rejects its saved token."""
        return await self.async_step_reauth_confirm()

    async def async_step_reauth_confirm(
        self, user_input: dict[str, Any] | None = None
    ) -> ConfigFlowResult:
        """Pair again after the user performs the physical token reset."""
        errors: dict[str, str] = {}
        entry = self._get_reauth_entry()
        if user_input is not None:
            try:
                device = await self._async_probe(
                    entry.data[CONF_HOST], entry.data[CONF_PORT]
                )
                if device.device_id != entry.unique_id:
                    errors["base"] = "wrong_device"
                elif device.paired:
                    errors["base"] = "token_reset_required"
                else:
                    token = await async_pair(
                        async_get_clientsession(self.hass),
                        entry.data[CONF_HOST],
                        entry.data[CONF_PORT],
                    )
                    return self.async_update_reload_and_abort(
                        entry, data_updates={CONF_TOKEN: token}
                    )
            except CannotConnect:
                errors["base"] = "cannot_connect"
            except CannotPair:
                errors["base"] = "cannot_pair"

        return self.async_show_form(
            step_id="reauth_confirm",
            data_schema=vol.Schema({}),
            errors=errors,
        )

    async def _async_probe(self, host: str, port: int) -> DeviceInfo:
        """Fetch device information with Home Assistant's shared session."""
        return await async_get_info(async_get_clientsession(self.hass), host, port)

    async def _async_pair_and_create(
        self, host: str, port: int, device: DeviceInfo, entity_id: str
    ) -> ConfigFlowResult:
        """Pair and persist only the narrow device credential."""
        token = await async_pair(async_get_clientsession(self.hass), host, port)
        return self.async_create_entry(
            title=device.name,
            data={
                CONF_ENTITY_ID: entity_id,
                CONF_HOST: host,
                CONF_PORT: port,
                CONF_TOKEN: token,
            },
        )
