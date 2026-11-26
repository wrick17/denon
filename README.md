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

### Moving an older install to the update layout

Firmware before November 21, 2026 used a single-app partition table
(`huge_app.csv`). The current firmware uses `min_spiffs.csv`:

| Partition | Offset | Size |
|---|---|---|
| `nvs` | `0x9000` | `0x5000` (unchanged) |
| `otadata` | `0xe000` | `0x2000` |
| `app0`, `app1` | `0x10000`, `0x1f0000` | `0x1e0000` each |
| `coredump` | `0x3f0000` | `0x10000` |

A partition table cannot change over Wi-Fi, so an older install needs one USB
flash. `pio run -t upload` writes the bootloader, partition table,
`boot_app0.bin` and the application in one pass. Do not erase flash: NVS holds
the receiver bond, the app volume table, the Wi-Fi settings and the API token,
and the write leaves it in place. Every later update goes over Wi-Fi.

### Wi-Fi firmware updates

Wi-Fi and Bluetooth share one radio, and uploads to the ESP32 run at a few
KB/s while Bluetooth runs, so an image can hit the 120-second upload limit.
Start a maintenance boot first:

1. Request maintenance from the home network with the setup access point
   closed:

   ```sh
   curl -X POST http://<ESP32_HOST>/api/firmware/maintenance \
     -H 'Authorization: Bearer <DEVICE_TOKEN>'
   ```

   It returns `202`, closes the Bluetooth links and restarts into a boot that
   never starts Bluetooth and turns Wi-Fi power save off. Wait until
   `GET /api/state` reports `maintenance` as `true`;
   `maintenance_remaining_ms` counts down the 10-minute window.
2. Upload the image as a raw body with the device token:

   ```sh
   curl -X POST http://<ESP32_HOST>/api/firmware \
     -H 'Authorization: Bearer <DEVICE_TOKEN>' \
     -H 'Content-Type: application/octet-stream' \
     --data-binary @.pio/build/esp32dev/firmware.bin
   ```

   The ESP32 returns `202`, closes its Bluetooth links and restarts into the
   other slot. That boot is a normal one, with Bluetooth.
3. Wait until `GET /api/state` reports `ota_state` as `trial` and the new
   `ota_slot`.
4. Within 10 minutes of that boot, confirm the image from the home network with
   the setup access point closed:

   ```sh
   curl -X POST http://<ESP32_HOST>/api/firmware/confirm \
     -H 'Authorization: Bearer <DEVICE_TOKEN>'
   ```

   It returns `200 {"confirmed":true}`.

Maintenance rules:

- The bearer token is required, and the request returns `403` through the
  setup access point or while it runs, and `409` while the running image is
  still unconfirmed.
- Any restart leaves maintenance: a successful upload, the end of the
  10-minute window, a crash or a watchdog reset all boot normally. Another
  request during maintenance restarts into a fresh window.
- Ending the receiver link has the usual cost: the Denon accepts the next
  link only after a power cycle, which the
  [link alert](#link-alert-in-home-assistant) automation performs.
- The maintenance restart is not a Bluetooth fault and is not a network
  watchdog restart, so the next normal boot starts Bluetooth at once.
- An upload must finish inside the window: in a maintenance boot the
  120-second upload limit is cut to the window's end, and an upload that
  starts after the window has ended returns `409`.
- Uploading without maintenance still works when the link is fast enough.

Upload rules:

- The bearer token is always required. The setup access point never authorizes
  an upload.
- The ESP32 answers before reading the body and closes the connection on
  rejection: `401` for a missing token, `409` while the running image is still
  unconfirmed, `413` for an image larger than the slot, `415` for multipart or
  a body that is not an ESP32 application image, `408` after 120 seconds.
- The boot slot changes only after the whole image is written and its checksum
  and SHA-256 verify; a failed check returns `422` and keeps the running image.

Confirmation and rollback:

- Confirm returns `403` through the setup access point or while it runs, and
  `409` when nothing awaits confirmation or the firmware self-check failed.
- An image that is not confirmed within 10 minutes restarts. The bootloader
  then boots the previous image. If the bootloader does not roll back, the
  firmware counts trial boots in NVS and switches back itself after three.
- `ota_last_result` in `GET /api/state` reports `ok`, `rolled_back`,
  `fallback` or `fallback_failed`.
- If the firmware self-check fails, the ESP32 serves only info, pairing, state,
  logs, firmware upload and maintenance, restart and coredump. Bluetooth,
  controls and confirmation stay off, so a fixed image can still be installed.

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

The Wi-Fi recovery firmware retries the saved station connection every 30
seconds while disconnected from the access point, including when the framework
stops automatic retries. It leaves an existing association alone while DHCP
finishes. It retains the setup access point and saved network settings. It does not reboot
the device or alter Bluetooth pairing and app volumes. The production-base
network patch was installed and passed a controlled station-recovery test on
September 26, 2026; see `ACCEPTANCE.md` for evidence and remaining limits.

`GET /api/state` also reports uptime, free and minimum free
heap, Wi-Fi signal strength, fallback retry count, and the last disconnect
reason. Serial output records boot reset reason and disconnect/retry events.
Counters and the last disconnect reason are volatile and reset on reboot;
capture serial output before restarting a failed device when possible.

Bluetooth link and recovery fields are described in
[Bluetooth link and recovery](#bluetooth-link-and-recovery).

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
repeating pairing; see
[Bluetooth link and recovery](#bluetooth-link-and-recovery). Set
**Bluetooth > Auto-Select** to **Off** so control does not switch the receiver
to Bluetooth audio. Set **Bluetooth Standby** to **On** if
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

If the collector cannot find the configured Apple TV for 60 seconds while
tunneld stays reachable and lists no tunnels at all, the collector asks tunneld to exit through its loopback `/shutdown`
endpoint, and `Restart=always` starts a fresh daemon. Repeat attempts back off
from 2 minutes to 1 hour and reset after the next successful DVT snapshot. An
unreachable daemon, a listed but unusable tunnel, and DVT errors never trigger
this restart; each one restarts the 60-second count without resetting the
backoff. Because tunneld keys Wi-Fi tunnels by pairing identifier rather than
the configured UDID, any listed tunnel blocks the restart. This recovery
therefore covers a single-device tunneld only.

With the default DVT polling enabled, DVT is the sole state authority and the
unused syslog reader is not started. Setting `DVT_POLL_SECONDS=0` selects the
legacy syslog mode. A failed or ambiguous DVT snapshot makes the MQTT sensor
unavailable and preserves the last retained identity rather than publishing a
false clear.

A brief ambiguous reading during an ordinary app switch does not add a recovery
delay: if the previous accepted sample is at most one second old and a unique
app returns within 1.5 seconds of the first ambiguous reading, it is published
immediately. Availability remains offline until that fresh reading arrives.
Screensaver clears, connection failures, malformed snapshots, and longer gaps
retain the three-second stable-app guard. The screensaver itself never selects
a volume; returning to the same app at its saved volume sends no volume steps.

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
750 ms identity debounce, the ESP32:

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

### Bluetooth link and recovery

The firmware drives the ESP-IDF Bluedroid SPP and GAP APIs directly. Bluetooth
callbacks only copy events and received bytes into queues; the main loop makes
every state change and connection call. Handshake, source and status requests
go out one at a time from a bounded queue. A volume step is never queued: it is
sent only when no write is in flight and the link is not congested, otherwise
the restore or manual request retries from fresh receiver feedback.

The controller runs in Classic mode only; the BLE controller memory goes
back to the heap at boot (about 5 KB). Before the controller starts, the
firmware sets Wi-Fi power save back to `min`, because the coexistence layer
aborts when the controller starts with it off; if that fails, the start fails
with `wifi_ps_restore_failed` and is retried.

The ESP32 stays connectable but hidden, accepts a control link from the bonded
receiver, and connects at once over an ACL link the receiver opens. Its own
reconnect attempts follow this ladder:

| Consecutive failures | Retry delay |
|---|---|
| 1 to 6 | 5 s |
| 7 to 12 | 15 s |
| 13 or more | 60 s |

- An attempt with no result after 30 seconds counts as a failure.
- After 10 minutes of failures the firmware restarts the Bluetooth stack, then
  again every 30 minutes. Two attempts in a row with no response restart it at
  once. None of this reboots the board.
- A stack that fails to start is retried after 1 minute, doubling up to
  10 minutes. HTTP and updates stay available meanwhile.
- Every intentional restart closes the control and A2DP links, waits up to
  2 seconds, then disables Bluedroid, so the receiver sees a normal disconnect.
- A 60-second loop watchdog resets a hung main loop. If the board resets while
  Bluetooth runs, the next boot waits 3 minutes before starting Bluetooth so
  HTTP and updates are reachable first. A stack start that hangs for 45 seconds
  or a failed stack teardown reboots into the same delay.
- A network watchdog covers a loop that keeps running while Wi-Fi stops
  carrying replies and still reports a connection. It arms once station
  clients have been answered for 5 minutes with no 5-minute gap. Then
  5 minutes with no complete reply reconnects Wi-Fi once, and 10 minutes
  restarts the board. While Wi-Fi is down, the normal Wi-Fi retries own
  recovery, and after each reassociation the watchdog waits 1 minute. Home Assistant polls every few seconds, so a
  Home Assistant outage longer than 10 minutes also causes one restart; the
  watchdog then stays unarmed until polling resumes for 5 minutes.

`GET /api/state` reports the link and recovery state:

| Field | Meaning |
|---|---|
| `bt_recovery_stage` | `connected`, `connecting`, `reconnecting`, `retrying`, `backoff`, `passive`, `idle`, `stack_starting` or `bluetooth_down` |
| `bt_page_timeouts` | consecutive page timeouts (HCI status `260`) |
| `bt_connect_failures` | consecutive failed attempts |
| `bt_stack_restarts` | Bluetooth stack restarts since boot |
| `bt_start_failures` | consecutive failed stack starts |
| `bt_since_connected_ms` | time since the link was last up; `0` while connected, `null` if never connected this boot |
| `bt_last_error` | last start or recovery error, such as `spp_init_timeout` or `recovery_mode` |
| `bt_controller_enabled` | whether the Bluetooth controller runs |
| `bt_event_drops`, `bt_rx_drops` | queue overflows; a lost event drops and reconnects the link |
| `reset_reason` | ESP-IDF reset reason of this boot |
| `network_watchdog_reconnects` | Wi-Fi reconnects forced by the network watchdog since boot |
| `network_watchdog_restarted` | whether the network watchdog restarted the board before this boot |
| `ota_slot`, `ota_state`, `ota_trial_boots`, `ota_last_result` | running slot and update state |
| `maintenance`, `maintenance_remaining_ms` | whether this is a maintenance boot, and the time left in its window (`null` otherwise) |
| `wifi_ps` | Wi-Fi power save the driver reports: `none`, `min` or `max` |
| `free_heap_bytes`, `min_free_heap_bytes`, `largest_free_block_bytes` | free heap now, its low mark since boot, and the largest allocatable block |

The passive diagnostics fields `bt_acl_connect_count`,
`bt_acl_last_connect_status`, `bt_acl_last_connect_at_ms`,
`bt_acl_last_disconnect_reason`, and `bt_acl_last_disconnect_at_ms` remain. A
disconnect reason is kept only for a previously established link; failed
attempts do not replace it. Times are milliseconds since boot, and unobserved
values are `null`. All of these counters reset on reboot.

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
  restore state, mute state and lock, target generation, `network_mode`
  (`dhcp` or `static`), and the Bluetooth recovery and update fields listed in
  [Bluetooth link and recovery](#bluetooth-link-and-recovery).
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

Maintenance, all with `Authorization: Bearer <DEVICE_TOKEN>` and never
authorized by the setup access point:

- `POST /api/firmware/maintenance`, `POST /api/firmware` and
  `POST /api/firmware/confirm` prepare, install and confirm an update; see
  [Wi-Fi firmware updates](#wi-fi-firmware-updates).
- `POST /api/restart` with form field `mode=clean` closes the Bluetooth links
  first; `mode=abrupt` restarts at once to simulate a crash. Both return `202`.
- `GET /api/coredump` streams the stored crash dump as `coredump.bin`, or
  returns `404` when none is stored. It is the raw ESP-IDF flash dump; decode
  it against the ELF of the firmware that crashed:

  ```sh
  esp-coredump info_corefile --core coredump.bin --core-format raw \
    <FIRMWARE_ELF>
  ```

- `DELETE /api/coredump` erases the stored dump and returns `204`.

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

For unattended fault history, [rolling server logs](tools/rolling_logs/README.md)
documents the read-only 72-hour journal, container, MQTT and ESP32 archive.

The tested AVR-X580BT stops accepting connections after its link to the ESP32
ends, however the link ends: every page then fails with HCI status `260`, and
firmware retries or a Bluetooth stack restart do not clear it. Turn the
receiver off and on with its remote's power button, or cycle its mains power;
the link returns within seconds. Holding the remote's Bluetooth button for
three seconds (pairing mode) also restored the existing bond in September
tests; return to TV Audio afterward. See `ACCEPTANCE.md`.

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

### Link alert in Home Assistant

`homeassistant/packages/denon_link_alert.yaml` polls `GET /api/state` every
10 seconds. After the link has been down for 30 seconds it switches the
Denon's smart socket off and back on after 1 second, unless the socket is
already off; an unreachable ESP32 does not cycle the Denon. It rechecks every
minute while the link is off, cycles at most once per 10 minutes, and hourly
after the link has been down for an hour. After each cycle it makes sure the
socket turned back on. It sends a phone notification after the link has been
down, or the ESP32 unreachable, for 1 minute, and again when the link returns.

1. Copy the file to `<HOME_ASSISTANT_CONFIG>/packages/` and enable packages in
   `configuration.yaml` with `homeassistant: packages: !include_dir_named
   packages`.
2. Add `denon_state_url: http://<ESP32_HOST>/api/state` to `secrets.yaml`.
3. Replace the `notify.` action in both alert automations with a notify
   service of your installation, and `switch.denon_power` with the switch
   entity of the Denon's smart socket, then restart Home Assistant.

## Tests

Firmware build and protocol checks:

```sh
PLATFORMIO_CORE_DIR="$PWD/.pio-home" .venv/bin/pio run
.venv/bin/python test/protocol_check.py
.venv/bin/python test/wifi_recovery_check.py
c++ -std=c++17 -Wall -Wextra -Werror -pedantic \
  test/volume_target_check.cpp -o /tmp/volume-target-check
/tmp/volume-target-check
c++ -std=c++17 -Wall -Wextra -Werror -pedantic \
  test/recovery_policy_check.cpp -o /tmp/recovery-policy-check
/tmp/recovery-policy-check
c++ -std=c++17 -Wall -Wextra -Werror -pedantic -Iinclude \
  test/bt_diagnostics_check.cpp -o /tmp/bt-diagnostics-check
/tmp/bt-diagnostics-check
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
