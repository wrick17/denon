"""Shared Home Assistant test setup."""

from unittest.mock import AsyncMock, patch

import pytest

from custom_components.denon_app_volume.api import BackupSnapshot


@pytest.fixture(autouse=True)
def auto_enable_custom_integrations(enable_custom_integrations):
    """Allow Home Assistant to load this repository's custom integration."""
    yield


@pytest.fixture(autouse=True)
def mock_device_backup_api():
    """Keep config-entry background tasks off the network in unit tests."""
    with patch(
        "custom_components.denon_app_volume.async_get_backup",
        AsyncMock(return_value=BackupSnapshot(0, '"0"', ())),
    ):
        yield
