# Rolling server logs

`recorder.py` keeps the latest 72 hours in a private SQLite archive on persistent
server storage. It reads logs and status only; it does not change volume, input,
playback, pairing, or the services it observes.

| Source | Recorded data |
| --- | --- |
| System journal | Full available entries and metadata, including kernel, networking, Home Assistant, Apple TV foreground/tunnel, MQTT broker, and containers using the journald driver |
| Other Docker containers | Full available timestamped stdout/stderr; discovery runs every 30 seconds |
| MQTT | All messages visible to the read-only account on `#` and `$SYS/#`, including topic, payload, QoS and retained status |
| ESP32 | Complete `/api/state` every two seconds, plus request failures |
| ESP32 text | Authenticated `/api/logs` cursor pages when enabled; boot changes and missing/overwritten chunks are recorded |
| Recorder | Reader connection failures, restarts, and health checkpoints |

The journal and container readers import available history from the preceding
72 hours, then follow new output. MQTT and ESP32 capture starts at installation;
there is no historical source to import for them. This collects existing output
at each application's configured logging level. It does not enable global debug
logging or recover messages an application never emitted.

Rows older than 72 hours are deleted at startup and once per minute. Journal
cursors and Docker checkpoints resume capture after recorder restarts without
repeating archived entries. SQLite reuses freed pages. Database, WAL and shared
memory files are restricted to root. Common credential fields and text patterns
are redacted before storage; raw installation logs stay out of Git.

The archive has a 4 GiB budget and reserves 1 GiB of free disk space. If either
limit is reached, the recorder fails explicitly instead of deleting recent
records silently. Check `systemctl status denon-logs` and the recorder journal
if capture stops. Full text logs cannot guarantee detection of every possible
unlabelled secret; treat the archive as private.

## Install

The service uses the existing Apple TV collector virtual environment and broker
settings. That environment must include `paho-mqtt` 2.x. Install
`recorder.py` at `/opt/denon-logs/recorder.py`, the supplied service at
`/etc/systemd/system/denon-logs.service`, and a private copy of
`denon-logs.env.example` at `/etc/denon-logs.env` with the actual ESP32 address.
The environment file must be mode `0600`. Then run:

```sh
sudo systemctl daemon-reload
sudo systemctl enable --now denon-logs.service
sudo systemctl status denon-logs.service
```

Only the new recorder starts. The existing Home Assistant, Apple TV, MQTT and
container services need no restart. Its database defaults to
`/var/lib/denon-logs/logs.sqlite3`, outside DietPi's volatile `/var/log` filesystem.

Use a dedicated MQTT reader with `topic read #` and `topic read $SYS/#` ACLs.
The foreground collector's account may be restricted to its own topics. Override
its username/password in the private recorder environment, preserving the
collector's existing credentials. The supplied `mosquitto-logs.conf` mirrors
broker diagnostics to the journal; reload the broker after backing up and
updating its configuration and ACLs. Existing client permissions stay intact.

Leave `ESP32_TEXT_ENABLED=0` until the authenticated firmware log endpoint is
installed, then set it to `1`. The client rejects redirects and HTML responses;
it never follows an unsupported endpoint to the ESP32 web page. Text capture
uses the existing device token in the private environment, not command arguments.
The firmware captures existing Arduino Serial, SDK stdout/stderr, and runtime ROM
output in a 32-record ring with 128 bytes per chunk. Authenticated pages contain
at most 16 records and 3 KiB. USB output remains available. The ring cannot retain
every message through long server outages, resets, or before its hooks start.
Sequence gaps identify missing records; ring overwrite counts alone do not imply
archive loss when records were already collected.

MQTT QoS 1 messages are acknowledged after their SQLite transaction commits, and
the subscriber has a persistent session. QoS 0 traffic during disconnection and
messages from before installation cannot be recovered. Applications that log
unlabelled secrets or split credentials across text chunks still require private
handling of the archive.

## Read the archive

Source coverage and latest capture times:

```sh
sudo python3 - <<'PY'
import sqlite3
db = sqlite3.connect('file:/var/lib/denon-logs/logs.sqlite3?mode=ro', uri=True)
for row in db.execute("SELECT source, count(*), datetime(max(timestamp), 'unixepoch') FROM events GROUP BY source"):
    print(row)
PY
```

ESP32 snapshots from the last 30 minutes:

```sh
sudo python3 - <<'PY'
import sqlite3
db = sqlite3.connect('file:/var/lib/denon-logs/logs.sqlite3?mode=ro', uri=True)
query = "SELECT datetime(timestamp, 'unixepoch'), payload FROM events WHERE source='esp32' AND timestamp >= strftime('%s','now','-30 minutes') ORDER BY timestamp"
for recorded_at, payload in db.execute(query):
    print(recorded_at, payload)
PY
```

Times are UTC. Journal records retain their unit and container metadata inside
`payload`, so the same interval can be compared across the whole system.

Run the retention, resume and redaction regression locally with
`python test/rolling_logs_check.py`.
