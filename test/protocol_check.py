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


captured_50 = bytes.fromhex("41 54 07 02 03 c5 64 00 d4")
captured_50_5 = bytes.fromhex("41 54 07 02 03 c5 65 00 d3")
captured_51 = bytes.fromhex("41 54 07 02 03 c5 66 00 d2")
captured_tv_audio_50 = bytes.fromhex("41 54 57 02 03 c5 64 00 d4")
wrong_category = bytes.fromhex("41 54 06 02 03 c5 64 00 d4")
wrong_header = bytes.fromhex("42 54 57 02 03 c5 64 00 d4")

assert volume_reports([captured_50]) == [0x64]
assert volume_reports([captured_tv_audio_50]) == [0x64]
assert volume_reports(
    [b"noise" + captured_tv_audio_50[:4], captured_tv_audio_50[4:7], captured_tv_audio_50[7:] + captured_51]
) == [0x64, 0x66]
assert volume_reports([captured_50_5 + captured_51]) == [0x65, 0x66]
assert volume_reports([wrong_category]) == []
assert volume_reports([wrong_header]) == []
assert volume_reports([captured_tv_audio_50[:-1]]) == []
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
reader = source[source.index("void readDenon()") : source.index("void sendPage()")]
assert "parseVolumePacket(buffer + i, 9, raw)" in reader
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
assert "WiFi.config(" not in source
assert 'constexpr char kMdnsService[] = "denon-volume";' in source
assert 'MDNS.addServiceTxt(kMdnsService, "tcp", "id", deviceId.c_str());' in source
web = source[source.index("void setupWeb()") : source.index("void setup()")]
for route in ("/api/info", "/api/pair", "/api/app", "/api/apps"):
    assert f'server.on("{route}"' in web
assert "server.collectHeaders(headers, 1);" in web
receive_app = source[source.index("void receiveApp()") : source.index("void sendApps()")]
assert "hasAppAuthorization()" in receive_app
assert 'parseAppJson(server.arg("plain"), appId, appName)' in receive_app
assert "queueAppCandidate(appId, appName);" in receive_app
assert setup.index("appStateSelfCheck()") < setup.index('preferences.begin("denon", false)')
assert setup.index("loadDeviceIdentity();") < setup.index("loadAppVolumes();") < setup.index(
    "startWifi();"
)
loop = source[source.index("void loop()") :]
assert loop.index("maintainAppSwitch();") < loop.index("maintainVolumeRestore();") < loop.index(
    "maintainAppStorage();"
)
