"""Tiny regression checks for Denon framing and UI string serialization."""

from pathlib import Path


def volume_reports(chunks: list[bytes]) -> list[int]:
    buffer = bytearray()
    reports: list[int] = []
    for chunk in chunks:
        buffer.extend(chunk)
        consumed = True
        while consumed:
            consumed = False
            for start in range(len(buffer) - 8):
                packet = buffer[start : start + 9]
                if (
                    packet[:2] == b"AT"
                    and packet[2] in (0x07, 0x57)
                    and packet[3:6] == b"\x02\x03\xc5"
                    and packet[7] == 0
                ):
                    reports.append(packet[6])
                    del buffer[: start + 9]
                    consumed = True
                    break
    return reports


def mute_reports(chunks: list[bytes]) -> list[bool]:
    buffer = bytearray()
    reports: list[bool] = []
    for chunk in chunks:
        buffer.extend(chunk)
        consumed = True
        while consumed:
            consumed = False
            for start in range(len(buffer) - 6):
                packet = buffer[start : start + 7]
                if (
                    packet[:2] == b"AT"
                    and packet[2] in (0x07, 0x57)
                    and packet[3:5] == b"\x1d\x01"
                    and packet[5] in (0, 1)
                    and packet[6] == 0xFF - packet[5]
                ):
                    reports.append(packet[5] == 1)
                    del buffer[: start + 7]
                    consumed = True
                    break
    return reports


captured_50 = bytes.fromhex("41 54 07 02 03 c5 64 00 d4")
captured_50_5 = bytes.fromhex("41 54 07 02 03 c5 65 00 d3")
captured_51 = bytes.fromhex("41 54 07 02 03 c5 66 00 d2")
captured_tv_audio_50 = bytes.fromhex("41 54 57 02 03 c5 64 00 d4")
wrong_category = bytes.fromhex("41 54 06 02 03 c5 64 00 d4")
wrong_header = bytes.fromhex("42 54 57 02 03 c5 64 00 d4")
captured_mute_on = bytes.fromhex("41 54 07 1d 01 01 fe")
captured_mute_off = bytes.fromhex("41 54 07 1d 01 00 ff")
captured_x580_mute_on = bytes.fromhex("41 54 57 1d 01 01 fe")
captured_x580_mute_off = bytes.fromhex("41 54 57 1d 01 00 ff")
wrong_mute_checksum = bytes.fromhex("41 54 07 1d 01 01 ff")

assert volume_reports([captured_50]) == [0x64]
assert volume_reports([captured_tv_audio_50]) == [0x64]
assert volume_reports(
    [b"noise" + captured_tv_audio_50[:4], captured_tv_audio_50[4:7], captured_tv_audio_50[7:] + captured_51]
) == [0x64, 0x66]
assert volume_reports([captured_50_5 + captured_51]) == [0x65, 0x66]
assert volume_reports([wrong_category]) == []
assert volume_reports([wrong_header]) == []
assert volume_reports([captured_tv_audio_50[:-1]]) == []
assert mute_reports([captured_mute_on, captured_mute_off]) == [True, False]
assert mute_reports([captured_x580_mute_on, captured_x580_mute_off]) == [True, False]
assert mute_reports([b"noise" + captured_mute_on[:3], captured_mute_on[3:]]) == [True]
assert mute_reports([wrong_mute_checksum]) == []
source = (Path(__file__).parents[1] / "src" / "main.cpp").read_text()
assert "escaped += static_cast<char>(c);" in source
assert "escaped += c;" not in source
assert '<meta charset=utf-8>' in source
assert 'text/html; charset=utf-8' in source
assert "ESP_BT_IO_CAP_IN" not in source
assert "address, 2, ESP_SPP_SEC_ENCRYPT | ESP_SPP_SEC_AUTHENTICATE" in source
assert "#include <esp_a2dp_api.h>" in source
assert "esp_a2d_source_init()" in source
assert "esp_a2d_source_connect(denonMac)" in source
assert "esp_a2d_source_register_data_callback(provideSilentAudio)" in source
assert "esp_avrc" not in source
assert "constexpr uint8_t kControlHandshake[] = {0x41, 0x54, 0x07, 0x25," in source
assert "0x01, 0x20, 0xDF};" in source
assert "constexpr uint8_t kGetSources[] = {0x41, 0x54, 0x00, 0x04, 0x00, 0x00};" in source
assert "constexpr uint8_t kGetStatus[] = {0x41, 0x54, 0x00, 0x06, 0x00, 0x00};" in source
maintain = source[source.index("void maintainDenon()") : source.index("void readDenon()")]
assert maintain.index("serialBt.connected()") < maintain.index("if (!a2dpConnected)")
assert "startA2dpConnect();" in maintain
bonded_reconnect = maintain[
    maintain.index("if (denonBonded)") : maintain.index("if (!a2dpConnected)")
]
assert "startDenonConnect();" in bonded_reconnect
assert "startA2dpConnect();" not in bonded_reconnect
assert "esp_a2d_source_connect" not in bonded_reconnect
assert maintain.count("sendDenon(kControlHandshake, sizeof(kControlHandshake))") == 1
assert maintain.count("sendDenon(kGetSources, sizeof(kGetSources))") == 1
session_init = maintain[
    maintain.index("if (!sessionInitialized)") : maintain.index(
        "nextStatusAt = now + kStatusIntervalMs;"
    )
]
assert session_init.index("sendDenon(kControlHandshake") < session_init.index(
    "sendDenon(kGetSources"
) < session_init.index("sendDenon(kGetStatus")
assert "if (!sessionInitialized)" in maintain
assert maintain.count("sessionInitialized = false;") == 2
assert maintain.count("sessionInitialized = true;") == 1
assert "nextStatusAt = now + 300;" in maintain
parser = source[source.index("bool parseVolumePacket(") : source.index("bool protocolSelfCheck()")]
assert "length != 9" in parser
assert "packet[2] != 0x07 && packet[2] != 0x57" in parser
assert "packet[7] != 0" in parser
assert "packet[8]" not in parser
assert "bool parseMutePacket(" in parser
assert "packet[2] != 0x07 && packet[2] != 0x57" in parser
assert "packet[5] > 1" in parser
assert "0xFF - packet[5]" in parser
reader = source[source.index("void readDenon()") : source.index("void sendPage()")]
assert "parseVolumePacket(buffer + i, 9, raw)" in reader
assert "parseMutePacket(buffer + i, 7, muted)" in reader
assert "Mute frame:" not in reader
assert reader.index("observeMute(muted);") < reader.index("observeVolume(raw);")
assert "serialBt.onConfirmRequest(confirmDenonPairing);" in source
confirm = source[source.index("void confirmDenonPairing(") : source.index("int32_t provideSilentAudio(")]
assert "a2dpConnecting" in confirm
assert "forgetInProgress" in confirm
assert "esp_bt_gap_ssp_confirm_reply(denonMac, true)" in confirm
assert "serialBt.confirmReply" not in confirm
a2dp_event = source[source.index("void a2dpEvent(") : source.index("bool startA2dp()")]
assert "isDenonAddress(param->conn_stat.remote_bda)" in a2dp_event
assert "esp_a2d_source_disconnect(param->conn_stat.remote_bda)" in a2dp_event
assert "pairingTimedOut = !forgetInProgress && a2dpConnecting;" in a2dp_event
a2dp_start = source[source.index("void startA2dpConnect()") : source.index("void connectDenonTask(")]
assert "connectInProgress" in a2dp_start
assert "forgetInProgress" in a2dp_start
assert "denonBonded" in a2dp_start
spp_task = source[source.index("void connectDenonTask(") : source.index("void maintainDenon()")]
assert "(!denonBonded && !a2dpConnected)" in spp_task
assert spp_task.count("forgetInProgress") >= 3
assert r'\"receiver_bonded\":' in source
assert "s.receiver_bonded?'Retry connection':'Retry pairing'" in source
assert "const bool pairing = !denonBonded && a2dpConnecting;" in source
setup = source[source.index("void setup()") : source.index("void loop()")]
assert setup.index('serialBt.begin("Denon ESP32", true)') < setup.index(
    "refreshDenonBondState()"
)
assert '"confirm_on_denon"' in source
assert r'\"pairing_number\":' in source
assert '"%06lu"' in source
assert "Press ENTER on the Denon now" in source
assert source.index("App volume memory") < source.index("id=receiver")
assert "Retry pairing" in source
assert "async function retryReceiver()" in source
assert 'server.on("/api/denon/reconnect", HTTP_POST, reconnectDenon);' in source
reconnect = source[source.index("void reconnectDenon()") : source.index("void discoverDenon()")]
assert "!hasDenonMac" in reconnect
assert "connectInProgress" in reconnect
assert "serialBt.connected()" in reconnect
assert "startDenonConnect();" in reconnect
assert "startA2dpConnect();" in reconnect
assert 'server.send(202, "application/json", "{\\\"connecting\\\":true}");' in reconnect
assert 'server.send(202, "text/plain", "Connecting");' not in reconnect
assert "/api/passkey" not in source
assert "submitPasskey" not in source
assert "parsePasskey" not in source
assert "passkeySubmitted" not in source
assert "esp_bt_gap_ssp_passkey_reply" not in source
assert "pairing-code" not in source
assert "esp_bt_gap_remove_bond_device(denonMac)" in source
assert "esp_a2d_source_disconnect(denonMac)" in source
forget = source[source.index("void forgetFailed(") : source.index("void saveWifi()")]
assert "forgetInProgress = true;" in forget
assert "forgetInProgress = false;" in forget
assert "state = bondState(denonMac)" in source
assert 'preferences.remove("denon_mac")' in source
assert source.index("esp_bt_gap_remove_bond_device(denonMac)") < source.index(
    'preferences.remove("denon_mac")'
)
assert source.index("esp_bt_gap_remove_bond_device(denonMac)") < source.index(
    "denonBonded = false;", source.index("void forgetDenon()")
)
assert "[" + "DEBUG-BT]" not in source
network_check = source[
    source.index("bool networkSelfCheck()") : source.index("bool isHexText(")
]
assert 'parseIpv4("203.0.113.7", parsed)' in network_check
assert '!parseIpv4("203.0.113", parsed)' in network_check
assert '!parseIpv4("203.0.113.256", parsed)' in network_check
assert "isSetupApIngress(true, true, false)" in network_check
assert "!isSetupApIngress(true, false, true)" in network_check
assert "!isSetupApIngress(true, true, true)" in network_check
assert "isStationIngress(true, true, false)" in network_check
assert "!isStationIngress(true, false, true)" in network_check
static_storage = source[
    source.index("enum NetworkStorageKey") : source.index("void loadStaticNetwork()")
]
for key in (
    "wifi_ssid",
    "wifi_password",
    "static_ip",
    "static_gw",
    "static_mask",
    "static_dns",
    "pending_ip",
):
    assert f'"{key}"' in static_storage
assert "const NetworkStorageState previous = readNetworkStorage();" in static_storage
assert "if (writeNetworkStorage(desired)) return true;" in static_storage
assert "writeNetworkStorage(previous)" in static_storage
assert "preferences.isKey(kNetworkStorageKeys[i])" in static_storage
assert "preferences.getString(kNetworkStorageKeys[i]" in static_storage
assert "commitNetworkStorage(desired)" in static_storage
station_server = source[
    source.index("class StationAwareWebServer") : source.index(
        "StationAwareWebServer server"
    )
]
assert "_currentClient.localIP()" in station_server
assert "isStationIngress(WiFi.status() == WL_CONNECTED" in station_server
assert "localAddress == WiFi.localIP()" in station_server
assert "localAddress == WiFi.softAPIP()" in station_server
assert "stationHttpClaimed = true;" in station_server
assert station_server.count("markStationRequest();") == 2
assert station_server.count("if (length > 0 && written == length)") == 2
write_ram = station_server[
    station_server.index("size_t _currentClientWrite(") : station_server.index(
        "size_t _currentClientWrite_P("
    )
]
write_flash = station_server[
    station_server.index("size_t _currentClientWrite_P(") : station_server.index(
        " private:"
    )
]
for write, base_call in (
    (write_ram, "WebServer::_currentClientWrite(buffer, length)"),
    (write_flash, "WebServer::_currentClientWrite_P(buffer, length)"),
):
    assert write.index(base_call) < write.index("markStationRequest();")
    assert write.index("written == length") < write.index("markStationRequest();")
    assert "return written;" in write
request_auth = source[
    source.index("bool currentRequestUsesSetupAp()") : source.index("String macString()")
]
assert "server.client().localIP()" in request_auth
assert "isSetupApIngress(" in request_auth
assert "currentRequestUsesSetupAp() ||" in request_auth
assert "hasAppAuthorization()" in request_auth
wifi_start = source[
    source.index("void startWifi()") : source.index("void maintainWifi()")
]
assert "stationHttpClaimed = false;" in wifi_start
assert "if (staticNetworkEnabled" in wifi_start
assert "WiFi.config(staticIp, staticGateway, staticSubnet, staticDns, staticDns)" in wifi_start
assert "if (staticNetworkEnabled) startSetupAp();" in wifi_start
assert "WiFi.begin(ssid.c_str(), password.c_str());" in wifi_start
wifi_maintain = source[
    source.index("void maintainWifi()") : source.index("bool sendDenon(")
]
assert "shouldStopSetupAp(staticNetworkEnabled, stationHttpClaimed)" in wifi_maintain
assert wifi_maintain.index("stopSetupAp();") < wifi_maintain.index("startSetupAp();")
assert "if (staticNetworkEnabled) stationHttpClaimed = false;" in wifi_maintain
wifi_save = source[source.index("void saveWifi()") : source.index("void saveNetwork()")]
assert "if (!currentRequestUsesSetupAp())" in wifi_save
assert "requireProvisioningAuthorization()" not in wifi_save
assert "apiClaimWindowOpen()" not in wifi_save
assert wifi_save.index("currentRequestUsesSetupAp()") < wifi_save.index('server.arg("ssid")')
assert 'server.arg("preferred_ip")' in wifi_save
assert "NetworkStorageState desired = readNetworkStorage();" in wifi_save
assert "desired.value[kPendingIpKey] = preferred.toString();" in wifi_save
assert "commitNetworkStorage(desired)" in wifi_save
assert "previous settings restored" in wifi_save
network_save = source[source.index("void saveNetwork()") : source.index("void clearNetwork()")]
assert "requireProvisioningAuthorization()" in network_save
assert 'parseIpv4(server.arg("ip"), preferred)' in network_save
for runtime_value in ("WiFi.gatewayIP()", "WiFi.subnetMask()", "WiFi.dnsIP(0)"):
    assert runtime_value in network_save
assert "validStaticNetwork(preferred, gateway, subnet, dns)" in network_save
assert "persistStaticNetwork(preferred, gateway, subnet, dns)" in network_save
network_clear = source[source.index("void clearNetwork()") : source.index("void setupWeb()")]
assert "requireProvisioningAuthorization()" in network_clear
assert "if (!clearStaticNetworkStorage())" in network_clear
assert "previous settings restored" in network_clear
assert 'constexpr char kMdnsService[] = "denon-volume";' in source
assert 'MDNS.addServiceTxt(kMdnsService, "tcp", "id", deviceId.c_str());' in source
web = source[source.index("void setupWeb()") : source.index("void setup()")]
for route in (
    "/api/info",
    "/api/pair",
    "/api/app",
    "/api/apps",
    "/api/backup",
    "/api/network",
):
    assert f'server.on("{route}"' in web
assert r'\"network_mode\":' in source
assert (
    'const char *headers[] = {"Authorization", "If-Match", "Origin", "Host"};'
    in web
)
assert "server.collectHeaders(headers, 4);" in web
backup = source[source.index("bool appTableEmpty()") : source.index("void sendInfo()")]
assert "hasAppAuthorization()" in backup
assert "requireProvisioningAuthorization()" not in backup
assert 'server.header("If-Match") != backupEtag()' in backup
assert 'server.sendHeader("Cache-Control", "no-store")' in backup
assert backup.index("writeBackupBank(targetBank") < backup.index(
    'preferences.putUChar("app_bank", targetBank)'
)
assert backup.index('preferences.putUChar("app_bank", targetBank)') < backup.index(
    "memcpy(appVolumes, apps"
)
assert "preferences.getBytes(key, &readback" in backup
assert "memcmp(&readback, &apps[i]" in backup
receive_app = source[source.index("void receiveApp()") : source.index("void sendApps()")]
assert "hasAppAuthorization()" in receive_app
assert 'parseAppJson(server.arg("plain"), appId, appName, eventId' in receive_app
assert "queueAppCandidate(appId, appName, playbackKnown, playbackActive, eventId);" in receive_app
assert receive_app.index("if (appId.isEmpty())") < receive_app.index(
    "if (isIgnoredAppId(appId))"
) < receive_app.index("queueAppCandidate(appId, appName, playbackKnown")
assert "validPlaybackEventId(eventId.c_str(), eventId.length())" in receive_app
assert "playbackIdleEventReady(" in receive_app
assert 'server.send(503, "text/plain"' in receive_app
assert receive_app.index("playbackIdleEventReady(") < receive_app.index(
    'server.send(503, "text/plain"'
) < receive_app.index("queueAppCandidate(appId, appName, playbackKnown")
app_switch = source[
    source.index("void queueAppCandidate(") : source.index("void observeVolume(")
]
clear_app = source[
    source.index("void clearCurrentApp(") : source.index("void queueAppCandidate(")
]
assert "currentAppId = \"\";" in clear_app
assert "appClearCancelsVolumeTarget(restoreTargetRaw, restoreAutomatic)" in clear_app
assert "if (restoreTargetRaw < 0 || restoreAutomatic)" not in clear_app
activate_app = app_switch[
    app_switch.index("void activatePendingApp(") : app_switch.index(
        "void maintainAppSwitch("
    )
]
assert activate_app.index("rememberAppVolume(currentAppId") < activate_app.index(
    "currentAppId = pendingAppId;"
)
assert activate_app.index("findApp(currentAppId)") < activate_app.index(
    "startRestoreForCurrentApp();"
)
assert "rememberAppVolume(currentAppId, currentAppName" in activate_app
assert activate_app.count("appVolumeLearningAllowed(") == 2
maintain_app_switch = app_switch[app_switch.index("void maintainAppSwitch(") :]
assert "restoreTargetRaw >= 0 && !restoreAutomatic" in maintain_app_switch
assert "millis() - pendingAppAt >= kAppStableMs" in maintain_app_switch
queue_app = app_switch[
    app_switch.index("void queueAppCandidate(") : app_switch.index(
        "void activatePendingApp("
    )
]
assert "if (restoreTargetRaw < 0 || restoreAutomatic) cancelVolumeRestore();" in queue_app
assert "appVolumeLearningAllowed(" in queue_app
assert "currentPlaybackIdleAuthorized = idleAuthorized;" in queue_app
assert "if (!idleAuthorized && restoreAutomaticMuteCycle) cancelVolumeRestore();" in queue_app
assert "validPlaybackEventId(eventId.c_str(), eventId.length())" in queue_app
assert "playbackIdleEventGrants(" in queue_app
assert "copyText(lastPlaybackIdleEventId" in queue_app
same_current = queue_app[
    queue_app.index("if (appId == currentAppId)") :
    queue_app.index("if (appId == pendingAppId)")
]
assert same_current.index("if (!duplicateIdleEvent)") < (
    same_current.index("currentPlaybackAt = now;")
)
assert "!duplicateIdleEvent && (hadPendingSwitch || idleAuthorized)" in same_current
same_pending = queue_app[
    queue_app.index("if (appId == pendingAppId)") :
    queue_app.index("if (!currentAppId.isEmpty()")
]
assert same_pending.index("if (!duplicateIdleEvent)") < (
    same_pending.index("pendingPlaybackAt = now;")
)
assert "pendingRestoreAllowed = !duplicateIdleEvent;" in queue_app
assert "if (restoreAllowed) startRestoreForCurrentApp();" in activate_app
volume_restore = source[
    source.index("void resetVolumeRestore(") : source.index("bool parseVolumePacket(")
]
assert "void armVolumeTargetSession(unsigned long now)" in volume_restore
assert "void pauseVolumeTargetSession()" in volume_restore
assert "advanceVolumeTargetGeneration();" in volume_restore
for reset_name, next_name in (
    ("void resetVolumeRestore(", "void advanceVolumeTargetGeneration("),
    ("void armVolumeTargetSession(", "void pauseVolumeTargetSession("),
    ("void pauseVolumeTargetSession(", "void cancelVolumeRestore("),
):
    reset_block = volume_restore[
        volume_restore.index(reset_name) : volume_restore.index(next_name)
    ]
    assert "restoreBurstActive = false;" in reset_block
    assert "restoreBurstExtensionPending = false;" in reset_block
    assert "restoreBurstStartRaw = -1;" in reset_block
    assert "restoreBurstClicksPlanned = 0;" in reset_block
    assert "restoreBurstClicksSent = 0;" in reset_block
arm_target = volume_restore[
    volume_restore.index("void armVolumeTargetSession(") : volume_restore.index(
        "void pauseVolumeTargetSession("
    )
]
assert "restoreBurstMeasuredDeltaRaw = 0;" in arm_target
assert "restoreBurstMeasuredClicks = 0;" in arm_target
cancel_restore = volume_restore[
    volume_restore.index("void cancelVolumeRestore()") : volume_restore.index(
        "void completeVolumeRestore()"
    )
]
assert "volumeTargetCancellationAdvancesGeneration(restoreTargetRaw)" in cancel_restore
assert cancel_restore.index("volumeTargetCancellationAdvancesGeneration(") < (
    cancel_restore.index("resetVolumeRestore(true);")
)
assert "bool setVolume(int targetRaw, bool automatic)" in volume_restore
assert "!automatic &&" in volume_restore
assert "!volumeCommandAllowed(muteStateKnown, denonMuted, manualMuteLocked)" in (
    volume_restore
)
assert "const bool mutedIdleRestore" in volume_restore
assert "currentPlaybackIdleFresh(millis())" in volume_restore
assert "!mutedIdleRestore" in volume_restore
assert "if (automaticRemuteRequired) return false;" in volume_restore
assert "setVolume(appVolumes[index].raw, true);" in volume_restore
clear_remute = volume_restore[
    volume_restore.index("bool clearAutomaticRemuteJournal(") :
    volume_restore.index("void advanceVolumeTargetGeneration(")
]
assert clear_remute.index('preferences.remove("remute")') < (
    clear_remute.index("automaticRemuteRequired = false;")
)
assert clear_remute.index("automaticRemuteRequired = true;") < (
    clear_remute.index('preferences.putBool("remute", true)')
)
observe_mute = volume_restore[
    volume_restore.index("void observeMute(") : volume_restore.index(
        "void observeVolume("
    )
]
assert observe_mute.index("manualMuteLocked = true;") < observe_mute.index(
    "cancelVolumeRestore();"
)
assert "const bool becameMuted = !muteStateKnown || !denonMuted;" in observe_mute
assert "if (!becameMuted) return;" in observe_mute
assert "if (automaticRemuteRequired)" in observe_mute
assert "restoreAutomaticUnmuteObserved = true;" in observe_mute
assert "automaticRemuteConfirmed(" in observe_mute
assert "clearAutomaticRemuteJournal()" in observe_mute
assert "!restoreAutomaticUnmuteObserved" in observe_mute
assert "restoreAutomatic && restoreTargetRaw >= 0 && restoreSteps > 0" in observe_mute
assert "muteAutomaticFeedbackUntil" in observe_mute
assert observe_mute.index("armAutomaticRemuteRecovery(millis())") < (
    observe_mute.index("cancelVolumeRestore();")
)
mute_cancel = observe_mute[
    observe_mute.index("if (cancelTarget)") : observe_mute.index(
        "void maintainManualMuteLock("
    )
]
assert mute_cancel.index("cancelVolumeRestore();") < mute_cancel.index(
    "restoreLearningSuppressed = true;"
)
assert "restoreFailureRaw = volumeRaw;" in mute_cancel
assert "restoreLearningResumeAt = millis();" in mute_cancel
clear_mute = volume_restore[
    volume_restore.index("void clearManualMuteLock(") : volume_restore.index(
        "void observeMute("
    )
]
assert "denonMuted = false" not in clear_mute
maintain_mute = volume_restore[
    volume_restore.index("void maintainManualMuteLock(") : volume_restore.index(
        "void observeVolume("
    )
]
assert "timeReached(millis(), muteAutomaticFeedbackUntil)" in maintain_mute
assert "muteAutomaticFeedbackPending = false;" in maintain_mute
assert "manualMuteLockRaw = volumeRaw;" in maintain_mute
manual_feedback = volume_restore[
    volume_restore.index("void acceptManualVolumeFeedback(") : volume_restore.index(
        "void observeVolume("
    )
]
assert "muteAutomaticFeedbackPending = false;" in manual_feedback
assert "manualMuteLockRaw = volumeRaw;" in manual_feedback
assert "muteManualFeedbackDirection = direction;" in manual_feedback
observe_volume = volume_restore[
    volume_restore.index("void observeVolume(") : volume_restore.index(
        "void maintainVolumeRestore("
    )
]
assert "!appVolumeLearningAllowed(" in observe_volume
assert "manualVolumeFeedbackMatches(" in observe_volume
assert "!restoreAutomaticMuteCycle && !automaticRemuteRequired" in observe_volume
assert observe_volume.index("manualVolumeFeedbackMatches(") < observe_volume.index(
    "manualMuteLockClearsOnVolume("
)
pending_feedback_guard = observe_volume[
    observe_volume.index("if (restoreLearningSuppressed)") : observe_volume.index(
        "const bool cooldownElapsed"
    )
]
assert "if (muteAutomaticFeedbackPending)" in pending_feedback_guard
assert "restoreFailureRaw = raw;" in pending_feedback_guard
assert "return;" in pending_feedback_guard
set_volume = volume_restore[
    volume_restore.index("bool setVolume(") : volume_restore.index(
        "void failVolumeRestore("
    )
]
muted_automatic_rejection = set_volume[
    set_volume.index("if (automatic &&") : set_volume.index(
        "advanceVolumeTargetGeneration();"
    )
]
assert "restoreLearningSuppressed = true;" in muted_automatic_rejection
assert "restoreFailureRaw = volumeRaw;" in muted_automatic_rejection
assert "restoreLearningResumeAt = millis();" in muted_automatic_rejection
assert muted_automatic_rejection.index("restoreLearningResumeAt = millis();") < (
    muted_automatic_rejection.index("return false;")
)
assert "restoreTargetRaw = targetRaw;" not in muted_automatic_rejection
assert "completeVolumeRestore" not in set_volume
maintain_restore = volume_restore[
    volume_restore.index("void maintainVolumeRestore()") :
]
assert maintain_restore.index(
    "!restoreSessionArmed || !serialBt.connected() || !sessionInitialized"
) < maintain_restore.index(
    "timeReached(now, restoreDeadlineAt)"
)
assert maintain_restore.index("timeReached(now, restoreDeadlineAt)") < (
    maintain_restore.index("automaticRestoreCommandAllowed(")
)
assert maintain_restore.index("VolumeTargetPhase::waitFreshStatus") < (
    maintain_restore.index("automaticRestoreCommandAllowed(")
)
playback_expiry = maintain_restore[
    maintain_restore.index("if (restoreAutomaticMuteCycle && !currentPlaybackIdleFresh") :
    maintain_restore.index("if (restoreBurstExtensionPending")
]
assert "playbackAuthorizationExpiryFails(volumeRaw, restoreTargetRaw)" in playback_expiry
assert 'failVolumeRestore("playback_authorization_expired")' in playback_expiry
assert "else {\n      completeVolumeRestore();" in playback_expiry
regular_restore = maintain_restore[
    maintain_restore.index("if (restoreBurstExtensionPending") :
]
assert regular_restore.index("automaticRestoreCommandAllowed(") < (
    regular_restore.index("if (restorePhase == VolumeTargetPhase::waitBurstSecond)")
)
for phase in (
    "needFreshStatus",
    "waitFreshStatus",
    "waitAutomaticUnmute",
    "waitBurstSecond",
    "waitMovement",
    "waitMovementStatus",
    "settling",
    "waitConfirmation",
):
    assert f"VolumeTargetPhase::{phase}" in volume_restore
automatic_unmute = maintain_restore[
    maintain_restore.index("VolumeTargetPhase::waitAutomaticUnmute") :
    maintain_restore.index("if (restoreAutomaticMuteCycle && !currentPlaybackIdleFresh")
]
assert automatic_unmute.index("armAutomaticRemuteRecovery(now)") < (
    automatic_unmute.index("sendDenon(command, length)")
)
assert automatic_unmute.index("sendDenon(command, length)") < (
    automatic_unmute.index("muteStateKnown = false;")
)
assert "kMuteOff" not in source
automatic_remute = volume_restore[
    volume_restore.index("void maintainAutomaticRemute(") :
    volume_restore.index("void maintainVolumeRestore(")
]
assert "sendDenon(kMuteOn, sizeof(kMuteOn))" in automatic_remute
assert "automaticMuteConfirmationPending = true;" in automatic_remute
assert automatic_remute.index("automaticMuteConfirmationPending = true;") < (
    automatic_remute.index("muteStateKnown = false;")
)
assert "clearAutomaticRemuteJournal" not in automatic_remute
assert "if (automaticRemuteRequired && !restoreAutomaticMuteCycle) return;" in (
    maintain_restore
)
burst_guard = volume_restore[
    volume_restore.index("bool validateActiveBurstFeedback(") :
    volume_restore.index("void observeVolume(")
]
assert "volumeRapidFeedbackValid(" in burst_guard
assert "restoreObservedRaw, raw" in burst_guard
assert '"rapid_gain_exceeded"' in burst_guard
assert '"wrong_direction"' in burst_guard
assert "failVolumeRestore(reason);" in burst_guard
observe_volume_target = volume_restore[
    volume_restore.index("if (restoreTargetRaw >= 0)") :
    volume_restore.index("if (restoreLearningSuppressed)")
]
assert observe_volume_target.index("validateActiveBurstFeedback(raw)") < (
    observe_volume_target.index("VolumeTargetPhase::waitFreshStatus")
)
assert observe_volume_target.index("validateActiveBurstFeedback(raw)") < (
    observe_volume_target.index("restoreObservedRaw = raw;")
)
wait_movement_feedback = volume_restore[
    volume_restore.index("if (restorePhase == VolumeTargetPhase::waitMovement ||") :
    volume_restore.index("if (restorePhase == VolumeTargetPhase::waitBurstSecond)")
]
assert "restoreBurstActive ? restoreObservedRaw : restoreCommandRaw" in (
    wait_movement_feedback
)
assert "volumeMovementInDirection(feedbackBaseline, raw" in wait_movement_feedback
assert "if (!restoreBurstActive &&" in wait_movement_feedback
assert wait_movement_feedback.index("if (raw == feedbackBaseline) return;") < (
    wait_movement_feedback.index("volumeRapidExtensionAllowed(")
)
assert "restoreBurstMeasuredDeltaRaw = abs(raw - restoreBurstStartRaw);" in (
    wait_movement_feedback
)
assert "restoreBurstMeasuredClicks = restoreBurstClicksSent;" in (
    wait_movement_feedback
)
assert "automaticRestoreCommandAllowed()" in wait_movement_feedback
assert "timeReached(now, restoreDeadlineAt)" in wait_movement_feedback
assert "restoreSteps," in wait_movement_feedback
assert "kRestoreMaxSteps" in wait_movement_feedback
assert "restoreBurstActive = false;" in wait_movement_feedback
assert "restoreBurstExtensionPending = volumeRapidExtensionAllowed(" in (
    wait_movement_feedback
)
assert "restoreBurstStartRaw = raw;" not in wait_movement_feedback
assert "restoreBurstClicksPlanned = 1;" not in wait_movement_feedback
assert "restoreBurstClicksSent = 0;" not in wait_movement_feedback
wait_burst_feedback = volume_restore[
    volume_restore.index("if (restorePhase == VolumeTargetPhase::waitBurstSecond)") :
    volume_restore.index("if (restorePhase == VolumeTargetPhase::settling)")
]
assert "restoreBurstClicksPlanned = restoreBurstClicksSent;" in wait_burst_feedback
assert "remainingDirection == 0 ||" in wait_burst_feedback
assert "remainingDirection != restoreCommandDirection" in wait_burst_feedback
assert wait_burst_feedback.index("restoreObservedRaw = raw;") < (
    wait_burst_feedback.index("if (restoreBurstExtensionPending)")
)
assert "const bool continuedInDirection = volumeMovementInDirection(" in (
    wait_burst_feedback
)
assert "if (!continuedInDirection ||" in wait_burst_feedback
assert "volumeRapidExtensionAllowed(" in wait_burst_feedback
assert "restoreBurstExtensionPending = false;" in wait_burst_feedback
regular_burst_feedback = wait_burst_feedback[
    wait_burst_feedback.index("const int remainingDirection") :
]
assert regular_burst_feedback.index("restoreCoarseEnabled = false;") < (
    regular_burst_feedback.index("restorePhase = VolumeTargetPhase::settling;")
)
wait_burst_send = maintain_restore[
    maintain_restore.index("if (restorePhase == VolumeTargetPhase::waitBurstSecond)") :
    maintain_restore.index("if (restorePhase == VolumeTargetPhase::waitMovement")
]
assert "kVolumeRapidIntervalMs" in wait_burst_send
assert "restoreBurstClicksSent >= restoreBurstClicksPlanned" in wait_burst_send
assert "++restoreBurstClicksSent;" in wait_burst_send
assert "restoreCommandRaw = restoreObservedRaw;" not in wait_burst_send
assert "const bool sendExtension = restoreBurstExtensionPending;" in wait_burst_send
assert "restoreBurstStartRaw = restoreObservedRaw;" in wait_burst_send
assert "restoreBurstClicksPlanned = 1;" in wait_burst_send
assert "restoreBurstClicksSent = 0;" in wait_burst_send
assert "restoreBurstExtensionPending = false;" in wait_burst_send
assert wait_burst_send.index("volumeRapidExtensionAllowed(") < (
    wait_burst_send.index("restoreBurstStartRaw = restoreObservedRaw;")
)
assert wait_burst_send.index("restoreBurstStartRaw = restoreObservedRaw;") < (
    wait_burst_send.index("sendDenon(command, length)")
)
assert wait_burst_send.index("restoreBurstExtensionPending = false;") < (
    wait_burst_send.index("sendDenon(command, length)")
)
settling = maintain_restore[
    maintain_restore.index("if (restorePhase == VolumeTargetPhase::settling)") :
    maintain_restore.index("if (restorePhase == VolumeTargetPhase::waitConfirmation)")
]
assert "volumeSettleWindowElapsed(" in settling
assert "remainingDirection == restoreCommandDirection" in settling
assert "if (!restoreBurstActive &&" in settling
assert settling.index("volumeSettleWindowElapsed(") < settling.index(
    "restorePhase = VolumeTargetPhase::ready;"
)
assert settling.index("restorePhase = VolumeTargetPhase::ready;") < settling.index(
    "sendDenon(kGetStatus"
)
assert "volumeMovementInDirection(" in volume_restore
assert "volumeSettleWindowElapsed(" in volume_restore
assert "volumeRapidClickCount(" in volume_restore
assert "volumeRapidGainWithinBound(" in volume_restore
assert '"rapid_gain_exceeded"' in volume_restore
assert "restoreBurstMeasuredDeltaRaw = movedRaw;" in volume_restore
assert "restoreBurstMeasuredClicks = restoreBurstClicksSent;" in volume_restore
assert "restoreBurstClicksPlanned = static_cast<uint8_t>(rapidClicks);" in volume_restore
assert "restoreCoarseEnabled = false;" in volume_restore
assert "restoreCorrectiveReversalUsed" in volume_restore
assert "corrective_reversal_exceeded" in volume_restore
assert "if (restoreTargetRaw < 0) sendDenon(kGetStatus" in maintain
assert "volume_target_id" in source
assert "target_id" in source
assert "s.volume_target_id!==targetRequest.id" in source
assert "b.disabled=!s.connected||s.restoring" in source
assert '<p id=mute-status class=mute-status role=status aria-live=polite>' in source
assert "s.mute_known" in source
assert "s.muted" in source
assert "s.manual_mute_lock" in source
assert "mute.classList.toggle('is-muted'" in source
assert "mute.classList.toggle('is-unmuted'" in source
assert '<label for=target-volume>Target volume</label><input id=target-volume' in source
assert "#target{display:flex;align-items:center}" in source
assert "@media(max-width:22rem)" in source
assert "name==='MemoryPoster'||name==='com.apple.IdleScreen.MemoryPoster'" in source
assert "name='Screensaver'" in source
send_volume = source[source.index("void sendVolume(") : source.index("void sendTargetVolume(")]
send_target = source[source.index("void sendTargetVolume(") : source.index("void reconnectDenon(")]
restore_state = source[source.index("const char *restoreStateText(") : source.index("void sendApps(")]
assert 'return "remuting";' in restore_state
assert "restoreTargetRaw >= 0 || automaticRemuteRequired" in restore_state
for handler in (send_volume, send_target):
    assert "requireManualVolumeAuthorization()" in handler
    assert "volumeCommandAllowed(muteStateKnown, denonMuted, manualMuteLocked)" in handler
    assert 'server.send(423, "text/plain"' in handler
    assert handler.index("server.send(423") < handler.index("server.send(409")
    assert handler.index("restoreTargetRaw >= 0") < handler.index("server.send(409")
    assert "Volume target is already in progress" in handler
    assert "automaticRemuteRequired ||" in handler
assert send_volume.index("server.send(409") < send_volume.index("sendDenon(")
assert send_target.index("server.send(409") < send_target.index("setVolume(")
assert send_volume.index("server.send(423") < send_volume.index("sendDenon(")
assert send_volume.index("server.send(423") < send_volume.index(
    "acceptManualVolumeFeedback(direction);"
)
assert send_target.index("server.send(423") < send_target.index(
    "acceptManualVolumeFeedback(volumeDirection(volumeRaw, targetRaw));"
)
assert send_target.index("server.send(423") < send_target.index("setVolume(")
assert send_volume.index("sendDenon(") < send_volume.index(
    "acceptManualVolumeFeedback(direction);"
)
assert (
    send_target.index("acceptManualVolumeFeedback(volumeDirection(volumeRaw, targetRaw));")
    < send_target.index("setVolume(")
)
assert "setVolume(targetRaw, false)" in send_target
manual_auth = source[
    source.index("bool isLocalControlHost(") : source.index(
        "bool apiClaimWindowOpen()"
    )
]
assert 'constantTimeEquals(origin, "http://" + host)' in manual_auth
assert "isLocalControlHost(host)" in manual_auth
assert "host == hostName + \".local\"" in manual_auth
assert "host == WiFi.localIP().toString()" in manual_auth
assert "currentRequestUsesSetupAp() || hasAppAuthorization()" in manual_auth
assert "if (restoreTargetRaw >= 0) armVolumeTargetSession(now);" in maintain
assert "pauseVolumeTargetSession();" in maintain
assert "automaticMuteConfirmationPending = false;" in maintain
assert setup.index("networkSelfCheck()") < setup.index('preferences.begin("denon", false)')
assert 'automaticRemuteRequired = preferences.getBool("remute", false);' in setup
assert setup.index("loadDeviceIdentity();") < setup.index("loadAppVolumes();") < setup.index(
    "startWifi();"
)
loop = source[source.index("void loop()") :]
assert loop.index("readDenon();") < loop.index("maintainManualMuteLock();")
assert loop.index("maintainAppSwitch();") < loop.index("maintainVolumeRestore();") < loop.index(
    "maintainAppStorage();"
)
