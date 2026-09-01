# Denon App Volume

Denon App Volume remembers a separate receiver volume for each Apple TV app.
When Home Assistant reports a new app, the ESP32 saves the outgoing app's
volume and restores the last volume recorded for the incoming app. Manual
volume changes become that app's new saved value.

The project has two parts:

```text
Apple TV -> Home Assistant custom integration -> local HTTP -> ESP32
ESP32 -> Bluetooth Classic SPP -> Denon receiver
```

The ESP32 owns the volume table and the restore loop. Home Assistant only sends
the current Apple TV `app_id` and display name. There is no cloud service or
external database.

## Requirements

- An original ESP32 with Bluetooth Classic SPP. ESP32-S2, S3, C2, C3, C5, C6,
  and H2 boards are not compatible.
- A supported Denon receiver and its remote control.
- A 2.4 GHz Wi-Fi network shared with Home Assistant.
- Home Assistant with the official Apple TV integration already working.
- PlatformIO for building and flashing the firmware.
- HACS, or file access to the Home Assistant configuration directory, for the
  custom integration.

## Build and flash

Replace every bracketed placeholder below with the value for your installation
before running a command.

```sh
git clone <REPOSITORY_URL> <REPOSITORY_DIRECTORY>
cd <REPOSITORY_DIRECTORY>
python3 -m venv .venv
.venv/bin/python -m pip install platformio==6.1.19
cp include/config.h.example include/config.h
PLATFORMIO_CORE_DIR="$PWD/.pio-home" .venv/bin/pio run -t upload
```

Leave the values in `include/config.h` empty for guided Wi-Fi and receiver
setup. The file is ignored by Git. Compile-time defaults are optional, but a
populated file must never be committed.

## Guided setup

### 1. Connect the ESP32 to Wi-Fi

1. After flashing, join the access point named
   `Denon-Setup-<DEVICE_ID_PREFIX>` from a phone or computer.
2. Open the captive portal notification. If it does not appear, open the
   gateway address shown in the connected network's details.
3. Enter the home Wi-Fi name and password. The ESP32 saves them in its local
   NVS storage, restarts, and obtains an address through DHCP.
4. Rejoin the home network. The ESP32 web interface is available at
   `http://denon-volume-<DEVICE_ID_PREFIX>.local/` when mDNS works on the
   network.

If the saved network later becomes unreachable, the setup access point returns
after about 15 seconds so Wi-Fi can be configured again.

### 2. Pair the Denon receiver

1. Disconnect phones or other sources using the Denon's Bluetooth connection.
2. Hold **Bluetooth** on the Denon remote for at least three seconds until the
   receiver enters pairing mode.
3. Open the ESP32 web interface, select **Find receiver**, and choose the Denon.
4. Verify that the web page and receiver show the same six-digit number, then
   press **ENTER** on the Denon remote. The ESP32 confirms its side
   automatically.
5. Return the receiver to **TV Audio**.

After the first bond, the ESP32 reconnects to the control service without
repeating pairing. Set **Bluetooth > Auto-Select** to **Off** so control does not
switch the receiver to Bluetooth audio. Set **Bluetooth Standby** to **On** if
the receiver should reconnect after a restart.

### 3. Install the Home Assistant integration

HACS custom repository:

1. In HACS, add `<REPOSITORY_URL>` as a custom repository with category
   **Integration**.
2. Download **Denon App Volume**.
3. Restart Home Assistant.

This repository does not claim a listing in the HACS default catalog.

Manual install:

```sh
mkdir -p <HOME_ASSISTANT_CONFIG>/custom_components
cp -R custom_components/denon_app_volume \
  <HOME_ASSISTANT_CONFIG>/custom_components/
```

Restart Home Assistant after copying the directory.

### 4. Add the discovered device

1. Open **Settings > Devices & services** in Home Assistant.
2. Find the discovered **Denon App Volume** device and select **Configure**.
3. Choose the Apple TV media player to follow and confirm.

Confirmation claims a device-generated API token. An unpaired ESP32 accepts
this claim from its setup network or for ten minutes after joining or restarting
on the home network. If setup took longer, restart the ESP32 immediately before
confirming it in Home Assistant. The token is returned once, then stored in the
ESP32 NVS and the Home Assistant config entry.

If discovery does not appear, add **Denon App Volume** manually and enter
`denon-volume-<DEVICE_ID_PREFIX>.local` or the ESP32 address reported by the
router.
Manual host entry also helps when mDNS is blocked across VLANs.

## Normal operation

Home Assistant sends the usable Apple TV app identity on state changes and as a
five-second heartbeat. The ESP32 waits briefly for the identity to settle, then:

- saves the current Denon volume for the outgoing app;
- restores the incoming app's saved volume one Denon step at a time, waiting
  for receiver feedback between steps;
- ignores intermediate restore values so they do not overwrite the saved value;
- records later manual changes as the active app's new volume.

A restore stops instead of issuing blind commands if receiver feedback stalls,
does not move toward the target, exceeds 196 steps, or runs for 30 seconds.
`GET /api/state` and `GET /api/apps` expose `restore_state` (`idle`,
`restoring`, or `error`) and a stable `restore_error` value when it stops.

The ESP32 keeps up to 16 app entries in NVS. When full, it reuses the oldest
entry. Writes are delayed briefly to reduce flash wear.

Open `http://denon-volume-<DEVICE_ID_PREFIX>.local/` to see the live connection
state, current receiver volume, pairing controls, and the per-app volume table.

### Apple TV app identity limitation

This depends on the Apple TV integration's `app_id` attribute. Apple TV commonly
reports it while an app is playing media, not merely because the app is open in
the foreground. An empty ID means no app is playing and clears the active app
without changing its stored row. `unknown`, `unavailable`, and other transient
identities are ignored, so opening an app without starting playback may not
trigger a volume switch.

## Local API

Discovery and status:

- `GET /api/info` returns the product, API version, generated device ID, name,
  hostname, and pairing state.
- `GET /api/state` returns Wi-Fi, receiver, Bluetooth, volume, active app, and
  restore state.
- `GET /api/apps` returns the bounded per-app volume table.
- `GET /api/discover` scans for nearby Bluetooth devices for eight seconds.

Pairing and app updates:

- `POST /api/pair` with an empty body or `{}` claims an unpaired device during
  its pairing window. It returns `200` with
  `{"token":"<64_LOWERCASE_HEX_CHARACTERS>"}`.
  Later claims return a conflict; claims outside the window are forbidden.
- `POST /api/app` accepts
  `{"app_id":"<APP_ID>","app_name":"<APP_NAME>"}` and requires
  `Authorization: Bearer <DEVICE_TOKEN>`. A string `app_id` of `""` is the valid
  no-playing-app state: it cancels pending work, clears current app ownership,
  leaves stored rows untouched, and returns
  `202 {"accepted":true,"cleared":true}`. A non-empty accepted ID returns
  `202`, an ignored transient identity returns `204`, malformed JSON or
  non-string fields return `400`, and a missing or invalid token returns `401`.
- `POST /api/unpair` requires the same bearer token, clears only that token,
  opens a new ten-minute claim window, and returns `204`.

Receiver and provisioning controls:

- `POST /api/volume/up` and `POST /api/volume/down` change one Denon step.
- `POST /api/denon` with form field `mac` saves the selected receiver.
- `POST /api/denon/reconnect` retries the saved receiver.
- `DELETE /api/denon` removes only the saved receiver bond and restarts the
  ESP32. It retains Wi-Fi, app memory, and the Home Assistant API token.
- `POST /api/wifi` with form fields `ssid` and `password` is accepted only while
  the setup access point is active, then restarts the ESP32.

Receiver discovery, selection, and forgetting are accepted only on the setup
access point, during an unpaired device's ten-minute claim window, or with the
bearer token. The receiver reconnect and volume buttons remain available on the
trusted LAN. Denon raw volume values outside `0..196` (display `0.0..98.0`) are
rejected from both receiver reports and persisted app rows.

The device uses local plain HTTP. The bearer token protects app updates and
provisioning mutations, but the web UI, status, reconnect, and volume controls
remain available to the local network. Keep the ESP32 and Home Assistant on a
trusted LAN. Do not forward its HTTP port or expose it through a public reverse
proxy. The first client to call the pairing endpoint during an open claim
window receives the token.

## Reset and re-pair

- For a normal Home Assistant removal, call authenticated `POST /api/unpair`
  before deleting the integration entry. It clears only the API token and opens
  a new ten-minute claim window.
- If the Home Assistant token is lost, leave the powered ESP32 running and hold
  its **BOOT** button for at least ten seconds. Release it after the serial log
  says pairing was reset; release triggers the reboot. This clears only the API
  token. Wi-Fi, the Denon bond reference, app volumes, and device identity are
  retained. Do not hold BOOT while powering on or resetting the board, because
  that enters the ESP32 bootloader instead of running this recovery action.
- To change the Denon receiver after Home Assistant is paired, authenticate the
  receiver request with its bearer token. If using only the web UI, first use
  the BOOT recovery above, then use **Forget receiver** and repeat receiver
  pairing within the new ten-minute window.

## Tests

Firmware build and protocol checks:

```sh
PLATFORMIO_CORE_DIR="$PWD/.pio-home" .venv/bin/pio run
.venv/bin/python test/protocol_check.py
```

Repository privacy checks:

```sh
.venv/bin/python test/secret_check.py --self-test
.venv/bin/python test/secret_check.py
git diff --check
```

Home Assistant integration tests:

```sh
.venv/bin/python -m pip install -r requirements-dev.txt
.venv/bin/python -m pytest -q tests
```

Live state checks require an explicit local URL and do not contain a default
deployment address:

```sh
DENON_URL=http://<ESP32_HOST> .venv/bin/python test/live_acceptance.py ready \
  --label <TEST_LABEL> --evidence <EVIDENCE_PATH>
```

## Compatibility and privacy

The Denon protocol is unpublished. This firmware uses fixed RFCOMM channel 2
and packets captured from the Denon 500-series AVR-X580BT. Other receivers may
pair but are not supported until the protocol is verified on real hardware.

The repository contains no deployment IP address, MAC address, Home Assistant
entity ID, access token, or Wi-Fi credential. Runtime values live only in the
ESP32 NVS or the Home Assistant config entry. CI runs both the repository secret
scanner and gitleaks to catch accidental additions.
