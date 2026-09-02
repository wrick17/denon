# Denon App Volume

Denon App Volume remembers a separate receiver volume for each Apple TV app.
A local foreground collector identifies the app on screen, Home Assistant
combines that identity with the official Apple TV playback state, and the ESP32
saves or restores the appropriate Denon volume. Manual receiver-volume changes
become the active app's new saved value.

The runtime path is:

```text
Apple TV -> pymobiledevice3 DVT collector -> loopback MQTT -> Home Assistant
Apple TV -> official Home Assistant media player -----------^
Home Assistant custom integration -> authenticated local HTTP -> ESP32
ESP32 -> Bluetooth Classic SPP -> Denon receiver
```

The ESP32 owns the volume table, safety state, and restore loop. Home Assistant
sends the selected app plus a tri-state playback authorization and keeps a local
backup of the table. The collector's DVT snapshot is the foreground authority;
its syslog stream is telemetry only. There is no cloud service or external
database.

## Requirements

- An original ESP32 with Bluetooth Classic SPP. ESP32-S2, S3, C2, C3, C5, C6,
  and H2 boards are not compatible.
- A supported Denon receiver and its remote control.
- A 2.4 GHz Wi-Fi network shared with Home Assistant.
- Home Assistant with the official Apple TV integration already working.
- For idle foreground-app switching, an always-on Linux host paired with the
  Apple TV through `pymobiledevice3`, plus a loopback-only Mosquitto broker.
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
3. Enter the home Wi-Fi name and password. **Preferred IP address** is optional:
   leave it blank to use DHCP, or enter an unused address from the home network
   to keep the ESP32 at that address across restarts. Reserving the same address
   in the router avoids another DHCP client receiving it.
4. The ESP32 saves the choice in local NVS and restarts. For a preferred
   address, it first takes one DHCP lease to learn the network's gateway, subnet,
   and DNS settings, saves those runtime values, then restarts once more using
   the preferred address.
5. Rejoin the home network. The ESP32 web interface is available at
   `http://denon-volume-<DEVICE_ID_PREFIX>.local/` when mDNS works on the
   network.

If the saved network later becomes unreachable, the setup access point returns
after about 15 seconds so Wi-Fi can be configured again.

When starting with saved static settings, the recovery access point is available
immediately and remains available until the ESP32 finishes an HTTP response to a
request received through its station/static address. Requests made through the
setup access point do not close it. This proves the saved address is actually
reachable before removing the recovery path. If a later router or network change
strands the static address after the access point has closed, power-cycle the
ESP32 to reopen it.

To change an installed device's preferred address, send the new `ip` to the
protected local network endpoint. To return to DHCP, delete that configuration:

```sh
curl -X POST http://<ESP32_HOST>/api/network \
  -H 'Authorization: Bearer <DEVICE_TOKEN>' \
  --data-urlencode 'ip=<PREFERRED_IP>'

curl -X DELETE http://<ESP32_HOST>/api/network \
  -H 'Authorization: Bearer <DEVICE_TOKEN>'
```

Both operations return `202` and restart the ESP32. If a saved address cannot
reach the home network, join the recovery access point, use the gateway address
shown by the phone or computer, and issue `DELETE /api/network` there. While the
setup access point is running, provisioning requests are allowed without the
bearer token; the delete clears only fixed and pending network settings. Invalid
stored settings are ignored and the firmware falls back to DHCP.

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

### 4. Run the foreground collector

This step is required for switching volumes when an app is open but not
playing. A playback-only installation may instead select the official Apple TV
media player in step 5.

The files in `tools/appletv_foreground/` are the deployed collector, pinned
dependencies, environment template, and systemd units. The supplied units
expect the collector and virtual environment in `/opt/appletv-foreground`, the
private environment file at `/etc/appletv-foreground.env`, and an existing
persistent `pymobiledevice3` pairing for the Apple TV. Configure Mosquitto to
listen only on loopback, give the collector a dedicated account restricted to
its topic prefix, and never commit the populated environment file.

Keep the service directory, virtual environment, script, and their parent paths
root-owned and not writable by the unprivileged service account. The tunnel
service runs as root because `pymobiledevice3` requires it; the collector itself
runs unprivileged. After installing the files, enable `appletv-tunneld.service`
and `appletv-foreground.service`. A healthy collector publishes retained MQTT
discovery, foreground state, and availability at QoS 1.

With the default DVT polling enabled, DVT is the sole state authority. Syslog
can add diagnostic lifecycle records but cannot keep the sensor online or
change its state. A failed or ambiguous DVT snapshot makes the MQTT sensor
unavailable and preserves the last retained identity rather than publishing a
false clear.

### 5. Add the discovered device

1. Open **Settings > Devices & services** in Home Assistant.
2. Find the discovered **Denon App Volume** device and select **Configure**.
3. Choose the MQTT **Apple TV Foreground App** sensor to support idle app
   switching, or choose the official Apple TV media player for playback-only
   behavior, and confirm.

When the MQTT sensor is selected, the current integration requires exactly one
enabled official Apple TV media-player entity. It uses that entity only for
playback ownership and safety. If Home Assistant has zero or multiple enabled
Apple TV media players, foreground updates fail closed until the ambiguity is
removed.

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

The foreground collector publishes the app visibly on screen. Home Assistant
combines it with the official Apple TV media player before sending an update to
the ESP32:

- active playback with a valid app identity owns the receiver, even while a
  different app is in front;
- a fresh foreground event may restore a volume only while playback is
  authoritatively idle or paused;
- off, unavailable, unknown, contradictory, or missing playback authority
  revokes the temporary restore permission and sends no new foreground target.

Each collector transition carries a stable 32-character lowercase hexadecimal
`event_id`. Home Assistant keeps the latest transition pending across a failed
ESP32 request and retries it after the normal five-second wait; a newer event
supersedes the pending one. Repeating the same event is idempotent in firmware,
so a transport retry cannot renew its short idle authorization or start a
second target. Off or unavailable playback fails closed without consuming the
pending foreground event. The ESP32 keeps the last granted ID in RAM, not NVS;
after a restart, a retained state or heartbeat is not treated as a fresh
foreground transition.

A fresh idle event is accepted only after the receiver link, session, volume,
and mute state are authoritative and no manual target or re-mute recovery is in
progress. Until then, the ESP32 returns `503` without consuming the `event_id`,
so Home Assistant can deliver that same transition after readiness.

This means Spotify can keep playing at its own volume while the user browses
another app. The browsed app is applied only after it becomes eligible. After a
1.5-second identity debounce, the ESP32:

- saves the current Denon volume for the outgoing app;
- restores the incoming app's saved volume with feedback-bounded accelerated
  steps and a fine exact finish;
- ignores intermediate restore values so they do not overwrite the saved value;
- records later manual changes as the active app's new volume.

An unseen well-formed app is learned at the current receiver volume only when
the receiver is unmuted. Sentinel identities such as `unknown`, `unavailable`,
`none`, and `null` never create rows or targets.

If the user has muted the Denon and playback is freshly and authoritatively
idle, automation may perform one bounded known-app restore. Denon volume frames
inherently unmute the receiver, so the ESP32 first persists a re-mute recovery
marker, restores the exact target, sends mute-on, waits for authoritative mute
confirmation, and only then clears the marker. If another app owns active audio,
or playback authority is stale or unknown, automation neither changes volume
nor unmutes. A physical Denon volume change or manual unmute remains user intent
and resumes normal learning.

If Bluetooth, the ESP32, or the receiver is interrupted after that bounded
transaction has temporarily unmuted the Denon, the accepted limitation is that
the receiver may remain unmuted until the control link returns. The persisted
marker makes mute-on the first state-changing recovery command before any later
volume command. Physical acceptance of this interrupted-recovery path remains
deferred; see `ACCEPTANCE.md`.

A restore stops instead of issuing blind commands if receiver feedback stalls,
does not move toward the target, exceeds 196 steps, runs for 30 seconds, or
violates an acceleration-liability bound. If the five-second idle authorization
expires during a muted transaction, an unfinished target stops and re-mutes;
an already exact target completes and re-mutes without leaving a stale error.
`GET /api/state` and `GET /api/apps` expose `restore_state` (`idle`,
`restoring`, `remuting`, or `error`) and a stable `restore_error` value when it
stops. They also expose authoritative `mute_known`, `muted`, and
`manual_mute_lock` fields. `GET /api/state` includes `volume_target_id`, which
lets a client detect that another request replaced or cancelled its target.

The ESP32 keeps up to 16 app entries in NVS, which survives power loss and
restart. When full, it reuses the oldest entry. Writes are delayed briefly to
reduce flash wear.

The Home Assistant integration also mirrors the table to its native persistent
`Store`. The ESP32 remains the primary copy: a non-empty device table replaces
the Home Assistant backup, while an empty or replacement ESP32 receives the
existing backup. Unchanged five-second polls do not write to Home Assistant's
disk; changed snapshots are saved after a short delay.

Open `http://denon-volume-<DEVICE_ID_PREFIX>.local/` to see the live connection
state, current receiver volume, pairing controls, and the per-app volume table.

### Apple TV app identity and ownership limitations

The official [Home Assistant Apple TV
integration](https://www.home-assistant.io/integrations/apple_tv/) supplies the
playback owner; it does not identify an idle foreground app. The local
`pymobiledevice3` DVT collector supplies that separate foreground identity.
Home Assistant gives active playback precedence and sends `playback_active`
only when the value is authoritative: `true` for active ownership, `false` with
a new valid `event_id` for one fresh idle foreground handoff, and no field to
revoke that temporary permission without clearing the remembered owner.

The collector deliberately fails closed. A confirmed screen-saver/foreground
clear is a nonempty retained JSON event; an EOF, service stop, DVT failure, or
ambiguous snapshot changes availability to offline without publishing an empty
retained tombstone. Full deliberate-failure, reconnect, and lossy-lifecycle
physical acceptance is still deferred.

## Local API

Discovery and status:

- `GET /api/info` returns the product, API version, generated device ID, name,
  hostname, and pairing state.
- `GET /api/state` returns Wi-Fi, receiver, Bluetooth, volume, active app,
  restore state, mute state and lock, target generation, and `network_mode`
  (`dhcp` or `static`).
- `GET /api/apps` returns the bounded per-app volume table plus current app,
  restore state, mute state, and lock.
- `GET /api/backup` returns a versioned app-volume snapshot and `ETag`. It
  requires `Authorization: Bearer <DEVICE_TOKEN>` and sends `Cache-Control:
  no-store`.
- `GET /api/discover` scans for nearby Bluetooth devices for eight seconds.

Pairing and app updates:

- `POST /api/pair` with an empty body or `{}` claims an unpaired device during
  its pairing window. It returns `200` with
  `{"token":"<64_LOWERCASE_HEX_CHARACTERS>"}`.
  Later claims return a conflict; claims outside the window are forbidden.
- `POST /api/app` accepts
  `{"app_id":"<APP_ID>","app_name":"<APP_NAME>","playback_active":<BOOLEAN>,"event_id":"<32_LOWERCASE_HEX>"}`
  and requires
  `Authorization: Bearer <DEVICE_TOKEN>`. A string `app_id` of `""` represents
  an explicit observed foreground clear: it cancels pending work, clears current
  app ownership, leaves stored rows untouched, and returns
  `202 {"accepted":true,"cleared":true}`. A non-empty accepted ID returns
  `202`, an ignored transient identity returns `204`, malformed JSON or
  non-string fields return `400`, and a missing or invalid token returns `401`.
  `playback_active=true` marks active playback ownership. A
  `playback_active=false` request with a new valid `event_id` grants one fresh
  muted-idle restore lease; a retry with the same ID is idempotent. Omitting the
  field revokes a prior lease without granting automation permission. A valid
  fresh idle event returns `503` without consuming its ID while receiver state
  is not ready; Home Assistant may retry the same request after readiness.
- `PUT /api/backup` restores a validated versioned snapshot only when the
  device table is empty. It requires the bearer token and the `ETag` from a
  preceding `GET /api/backup` in `If-Match`, rejects concurrent changes, and
  commits to NVS before switching the active table.
- `POST /api/unpair` requires the same bearer token, clears only that token,
  opens a new ten-minute claim window, and returns `204`.

Receiver and provisioning controls:

- `POST /api/volume/up` and `POST /api/volume/down` change one Denon step.
- `POST /api/volume` with form field `volume` requests an exact 0.5-step display
  value. Manual volume endpoints return `423` while mute is unknown, on, locked,
  or awaiting recovery, and `409` while another target is active.
- `POST /api/denon` with form field `mac` saves the selected receiver.
- `POST /api/denon/reconnect` retries the saved receiver.
- `DELETE /api/denon` removes only the saved receiver bond and restarts the
  ESP32. It retains Wi-Fi, app memory, and the Home Assistant API token.
- `POST /api/wifi` with form fields `ssid` and `password` is accepted only while
  the setup access point is active. Its optional `preferred_ip` field is blank
  for DHCP or contains a complete unicast IPv4 address. A preferred address is
  combined with gateway, subnet, and DNS values learned from the first DHCP
  lease, persisted in NVS, and applied after an automatic second restart.
- `POST /api/network` with form field `ip` validates a new preferred address
  against the current network, persists that address plus the live gateway,
  subnet, and DNS settings, then returns `202` and restarts.
- `DELETE /api/network` clears only fixed and pending network settings, returns
  `202`, and restarts using DHCP.

Network-setting writes are transactional across Wi-Fi credentials, static
address, gateway, subnet, DNS, and a pending preferred address. Every write and
removal is read back. If a commit fails, firmware attempts to restore the prior
complete snapshot, returns `500`, and does not restart with a knowingly partial
configuration.

Receiver discovery, selection, forgetting, and runtime network changes are
accepted when the request arrives through the setup-access-point interface,
during an unpaired device's ten-minute claim window, or with the bearer token.
Merely running the recovery access point does not authorize a simultaneous
station-interface request; that path still needs the bearer token. If interface
addresses ever collide, setup bypass fails closed. The receiver reconnect and
volume buttons remain available on the trusted LAN. Denon raw volume values
outside `0..196` (display `0.0..98.0`) are rejected from both receiver reports
and persisted app rows.

The device uses local plain HTTP. The bearer token protects app updates and
provisioning mutations, but the web UI, status, reconnect, and volume controls
remain available to the local network. Keep the ESP32 and Home Assistant on a
trusted LAN. Do not forward its HTTP port or expose it through a public reverse
proxy. The first client to call the pairing endpoint during an open claim
window receives the token.

## Production-ready v1 and deferred acceptance

The user accepted the physically verified foreground restore, playback
ownership, exact accelerated targeting, manual learning, bounded idle-muted
restore, and re-mute behavior as the production-ready v1 milestone. The final
readiness/retry build is physically accepted: a pre-ready request returned `503`
without consuming its event, the retry applied exactly once after readiness,
and the receiver restored exact app targets, re-muted, stayed idle and
error-free, and preserved every stored row without duplicate targets.

Production-ready v1 is green. The whole project is not complete; the remaining
deferred physical work is:

- playback beginning midway through a muted idle restore;
- controlled sentinel, stale, off, unavailable, explicit-clear, and EOF cases;
- physical volume intervention during an automatic restore;
- active-target receiver or Bluetooth interruption and reconnect;
- interrupted re-mute recovery across ESP32, Bluetooth, or receiver loss;
- a full ESP32 power-cycle followed by reopening each saved app;
- a complete foreground browse sequence during uninterrupted background audio;
- deliberate DVT failure, ambiguity, syslog EOF, restart, sleep, wake, and
  lossy-lifecycle robustness with correlated MQTT, Home Assistant, ESP32, and
  receiver evidence.

Treat these as deferred acceptance, not completed behavior. The exact evidence
contract, timing bounds, historical failures, superseded requirements, and
authoritative pass/fail rows are in `ACCEPTANCE.md`.

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
c++ -std=c++17 -Wall -Wextra -Werror -pedantic \
  test/volume_target_check.cpp -o /tmp/volume-target-check
/tmp/volume-target-check
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

Foreground collector tests:

```sh
.venv/bin/python -m unittest tools.appletv_foreground.test_parser
```

Live state checks require an explicit local URL and do not contain a default
deployment address:

```sh
DENON_URL=http://<ESP32_HOST> .venv/bin/python test/live_acceptance.py ready \
  --label <TEST_LABEL> --evidence <EVIDENCE_PATH>

DENON_URL=http://<ESP32_HOST> .venv/bin/python test/live_acceptance.py \
  network-static --label <TEST_LABEL> --evidence <EVIDENCE_PATH>
```

## Compatibility and privacy

The Denon protocol is unpublished. This firmware uses fixed RFCOMM channel 2
and packets captured from the Denon 500-series AVR-X580BT. Other receivers may
pair but are not supported until the protocol is verified on real hardware.

The repository contains no deployment IP address, MAC address, Home Assistant
entity ID, access token, or Wi-Fi credential. Runtime values live only in the
ESP32 NVS or Home Assistant's private config and storage data. CI runs both the
repository secret scanner and gitleaks to catch accidental additions.
