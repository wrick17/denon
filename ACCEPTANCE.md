# Physical end-to-end acceptance

The concurrent Apple TV, Home Assistant, ESP32, and Denon session was confirmed
stopped before live acceptance began. Assigned hardware and remote owners perform
the mutations; the acceptance owner records evidence only. These checks require
a human to operate the Apple TV and Denon. A unit test, dashboard claim, Home
Assistant entity state, or HTTP `202` alone cannot pass a physical row.

## Evidence contract

Keep one timestamped evidence directory per run. Do not commit it. Record:

- the human action and the Denon display or mute indicator;
- the foreground event ID and source/receipt timestamps;
- the MQTT publish acknowledgement and Home Assistant state timestamp;
- read-only `GET /api/state` and `GET /api/apps` samples at 250 ms intervals;
- the final Denon display, `volume_raw`, `restore_state`, `restore_error`,
  foreground MQTT `app_id`, official Apple TV media-player state and `app_id`,
  ESP32 `current_app_id`, `mute_known`, `muted`, `manual_mute_lock`, and
  `volume_target_id`.

The receiver report and its physical display must agree. `volume_raw / 2` is
the displayed volume. A requested target, HTTP success, MQTT state, or helper
state does not prove receiver movement or mute state.

Start the existing read-only receiver observer with a private evidence file:

```sh
acceptance_dir="$(mktemp -d "${TMPDIR:-/tmp}/denon-acceptance.XXXXXX")"
chmod 700 "$acceptance_dir"
export DENON_URL='http://<esp32-host>'
.venv/bin/python test/live_acceptance.py ready \
  --label preflight --evidence "$acceptance_dir/receiver.jsonl" --timeout 10
```

For each manual action, append its UTC time, then keep state samples while the
action runs:

```sh
timestamp() {
  python3 -c 'from datetime import datetime, timezone; print(datetime.now(timezone.utc).isoformat(timespec="milliseconds").replace("+00:00", "Z"))'
}
printf '%s %s\n' "$(timestamp)" '<describe physical action>' \
  | tee -a "$acceptance_dir/actions.log"
while sleep 0.25; do
  state="$(curl -fsS "$DENON_URL/api/state")" || break
  jq -cn --arg at "$(timestamp)" --argjson state "$state" \
    '{at:$at,state:$state}' >> "$acceptance_dir/state.jsonl" || break
done
```

Poll both Home Assistant entities from the same observation host. Keep the
bearer token only in the environment and out of evidence:

```sh
export HA_URL='http://<home-assistant-host>:<port>'
export HA_TOKEN='<runtime-only-token>'
export FOREGROUND_ENTITY_ID='<mqtt-foreground-entity>'
export PLAYBACK_ENTITY_ID='<official-apple-tv-media-player>'
timestamp() {
  python3 -c 'from datetime import datetime, timezone; print(datetime.now(timezone.utc).isoformat(timespec="milliseconds").replace("+00:00", "Z"))'
}
while sleep 0.25; do
  at="$(timestamp)"
  for entity in "$FOREGROUND_ENTITY_ID" "$PLAYBACK_ENTITY_ID"; do
    state="$(curl -fsS -H "Authorization: Bearer $HA_TOKEN" \
      "$HA_URL/api/states/$entity")" || exit
    jq -cn --arg at "$at" --argjson state "$state" \
      '{at:$at,state:$state}' \
      >> "$acceptance_dir/home-assistant.jsonl" || exit
  done
done
```

Capture the matching collector, MQTT, and Home Assistant records without
credentials in the output. Redact hostnames, entity IDs, MAC addresses, tokens,
and user names before sharing evidence. Compute an elapsed time only between
timestamps taken by the same observation host or process. Do not compare clocks
from separate machines unless their measured offset is included.

## Grounded timing limits

These are hard safety limits from the current executable paths, not desired
performance targets:

| Segment | Pass limit | Basis |
|---|---:|---|
| Home Assistant state event to ESP32 HTTP result | 5.00 s | Integration request timeout |
| Accepted new app to app activation | 1.50 s minimum, 1.75 s evidence ceiling | Firmware debounce plus 250 ms evidence sampling |
| Armed or rearmed target to exact settled volume | 30.25 s | Firmware 30 s deadline plus 250 ms evidence sampling |
| Home Assistant state event to exact settled volume | 36.75 s | 5 s request, 1.5 s debounce, 30 s target, 250 ms sampling |
| Receiver disconnect to ready again | 120 s | Existing live observer timeout; prior abrupt-link recovery was about 92 s |
| Reconnect to exact resumed target | 30.25 s | Target deadline restarts only after the SPP session is ready |
| Short collector EOF before reconnect attempt | 0.50 s | Deployed initial reconnect delay |
| Foreground source record to collector receipt | unresolved | Latest physical Netflix and Home lifecycle records took 9.02 s and 7.27 s; an earlier sequence also missed a lifecycle record |
| Collector receipt to MQTT acknowledgement | unresolved | Latest acknowledgement was 1.5 ms; prior samples were 1.7 to 2.7 ms |
| Physical transition to live DVT state | unresolved | Screen saver→Home and Home→screen saver reached authoritative DVT state in 395.1 ms and 515.1 ms; two samples are not a bound |
| DVT state to MQTT acknowledgement | unresolved | The two transitions received MQTT acknowledgement in 5.2 ms and 4.7 ms; two samples are not a bound |
| Physical Apple TV action to exact settled volume | unresolved | A complete lossless source-to-receiver run has not been measured |

Any missing foreground transition is an immediate functional failure, even if
later state happens to match. The project cannot claim real-time acceptance
until a clean run supplies the unresolved end-to-end bounds and the user
accepts them. Do not turn the 1.5 to 2.7 ms MQTT subsegment into an end-to-end
claim; the latest source-to-host lifecycle delivery alone took 7.27 to 9.02 s.

## Test matrix

The user selected and authorized Spotify 45.0 and Netflix 60.0 through 65.0 for
this run, then selected exact Netflix target `N = 60.0`, raw 120. The first live
restore reached that exact value without playback, but did not sustain the
required acceleration. Use Spotify 45.0, raw 90, and Netflix 60.0, raw 120, in
every rerun so an accidental no-op cannot pass. A stored YouTube value of 50.0
remains a normal eligible value.
Automation may temporarily unmute only for the bounded, authoritatively idle
M05 transaction, and it must finish muted. Active, unknown, stale, off, or
unavailable playback authority remains a hard zero-command block. Automation
never starts playback; playback begins only after the user's deliberate action.
Restore the user's preferred final volume after the run.

| ID | Preconditions and physical action | Required authoritative result | Timing | Current gate |
|---|---|---|---|---|
| P00 | Before changing anything, start all observers and inspect current HA, MQTT, ESP32, and Denon state. | Both HA entities are available, ESP32 is connected and idle with no restore error, receiver volume matches the physical display, mute is known, and the expected stored rows exist. Any missing evidence field fails preflight. | Ten-second read-only preflight. | **PASS — final scoped-v1 bracket.** Exact live HA `8cd5ed19`/`ea728597` and firmware `bcefdec0`, source main `9841`, finished Home at raw 90, target 2, muted, locked, connected, idle and error-free for 45.573 s. Rows Home 90, Netflix 130, Spotify 90 and Settings 90 were intact. |
| F01 | No app owns playback. Put Home in front, then open Netflix with its recorded target 60.0 without starting playback. | Foreground event and HA state identify Netflix once. ESP32 activates Netflix after the debounce, sustains the reviewed acceleration profile, settles at raw 120 with `restore_state=idle` and no error, and the Denon displays 60.0. Playback remains stopped until the user deliberately starts it. | HA event to exact state at most 36.75 s. Activation must not occur before 1.50 s. | **PASS on the current build through M05 chronology.** With playback authoritatively idle, Netflix foreground at `19:42:22.876` reached ESP after 1.526 s, satisfying debounce. Firmware `0513` reached exact raw 120 without overshoot after 4.468 s and returned idle after the bounded transaction; no playback started. HA build was `01eb`/`faa`. |
| F02 | Start Spotify playback and set its learned value to 45.0. While Spotify audio continues, put Home or the screen saver in foreground. | Foreground MQTT identifies Home or the screen saver. The official media player remains `playing` with Spotify `app_id`. ESP32 `current_app_id` remains Spotify. Denon stays at 45.0, raw 90. No automatic target starts, `volume_target_id` does not change, and neither stored row changes. | Observe for at least 36.75 s after the HA foreground event, covering the complete automatic path. Any owner, target, receiver or stored-row change fails immediately. | **PASS — equivalent physical ownership.** Home became foreground at `16:50:27.023` while the official player remained `playing` with Spotify; screen saver/unavailable followed at `16:51:22.794`. Across 46.9 seconds and 42 samples, the official player and relay stayed Spotify. Raw 90, target 0, generation 0, mute and lock, idle state, Spotify 90, Home 90 and Netflix 90 all remained unchanged with zero errors, transients, unsafe Home/clear send or actuation. Netflix's startup tone may release Spotify, so this Home/screen-saver overlap is the accepted ownership proof. |
| F03 | After F02, deliberately release Spotify playback, confirm the official player is no longer Spotify-owned, then open Netflix. | The release event does not apply a stale foreground target. Once Netflix becomes the authoritative eligible app, ESP32 changes from Spotify to Netflix, one target settles at raw 120, and the Denon displays 60.0. Predictive control covers most movement; only the final one or two display points, two to four raw units, may use 0.5-point fine steps. Measured gain aims for that tail, and liability bounds prevent overshoot. Automation never sends unmute or starts playback. | Eligible event to exact state at most 36.75 s. User-visible Home 45 to Netflix 60 and return legs must complete within 4.5 s from first visible movement to exact, with at most two to four raw units of fine tail. | **PASS — bidirectional movement accepted and algorithm frozen.** Upward Home raw 90 to Netflix raw 120 followed 90, 91, 97, 110 and exact 120 with no visible fine tail; Home Assistant measured exact within 1.05 s and idle within 1.64 s, telemetry observed 1.241 s and full movement under 2.5 s, and the user called it perfect. Reverse Netflix raw 120 to Home raw 90 first observed 94, then 92 and exact 90; exact and idle took 2.457 s, full movement stayed under 3.7 s, and only two raw or one display point used the fine tail. Both legs had no overshoot, error, mute change or disconnect, and Home 90 plus Netflix 120 remained preserved. The user accepted the current set-volume algorithm; latency and further fine tuning are deferred, non-release-blocking, and must not weaken the 3-second wake and reconnect guard. |
| F04 | With Spotify playing at 45.0, browse Home, YouTube, Netflix, and back to Home without taking playback ownership. | Every foreground transition appears once and in order. Official media-player ownership and ESP32 `current_app_id` stay Spotify. Denon remains raw 90 and `volume_target_id` never changes. | Hold each app for at least 36.75 s. No missing event is allowed. | Blocked by lossy lifecycle records and pending ownership-gate acceptance. |
| M01 | Historical absolute-lock case: while unmuted and idle at a known raw value, mute with the Denon remote, then switch foreground apps without starting playback. | Historical requirement: mute stays on and no automatic volume or unmute command occurs. | Historical observation window was at least 36.75 s. | **SUPERSEDED — evidence preserved.** Firmware `c1bcfeb4` passed this old contract after a fix and manual Netflix raw 120 reseed: muted Netflix-to-Home held actual raw 120, target 2, mute and lock, idle and no-error state while Netflix 120 and Home 90 remained unchanged for more than five seconds. The user now permits a temporary unmute and restore only when playback is authoritatively idle. Use M05 through M10 for current acceptance. |
| M01-MEM | Historical absolute-lock memory case: with Netflix stored raw 120 and the receiver muted at raw 90, switch foreground to Netflix without playback. | Historical requirement: preserve Netflix 120, raw 90 and target identity with no automatic command. | Historical observation covered debounce and delayed persistence. | **SUPERSEDED — historical pass preserved.** Home raw 90 to Netflix stored 120 held more than 17 seconds with Home 90 and Netflix 120 unchanged and no command or error. Current idle-muted behavior is M05; stored-row preservation remains mandatory there. |
| M01-PLAY | Historical absolute-lock playback case. | Historical requirement: playback never bypasses the mute lock. | Historical observation window was at least 36.75 s. | **SUPERSEDED before execution.** Active playback remains a hard block under M06, while playback starting during an idle transaction is now the explicit abort-and-re-mute case M07. |
| M02 | Historical absolute-lock case: mute during a long automatic restore. | Historical requirement: cancel before another step and never automatically unmute. | Historical observation window was at least 36.75 s. | **SUPERSEDED before physical execution.** The current contract permits a bounded temporary unmute only for authoritatively idle playback and requires re-mute. Playback takeover, ambiguity, errors and reconnect are covered explicitly by M07 through M09. |
| M03 | After the 1.8-second automatic-feedback quarantine ends, change volume with the physical Denon remote. | Denon volume frames inherently unmute as a direct result of the user's action. The mute-off and manual volume feedback clear the lock, and the active app learns the new raw value. Automatic feedback outstanding when mute arrived must not count as manual release. A movement during quarantine is deliberately absorbed and requires another remote movement afterward. This user-driven path is separate from M05's bounded automatic transaction. | Start the physical remote action after the 1.8-second quarantine, then observe row persistence for at least 2.25 s and one eligible app event. | **Manual release and learning path passed for Home.** Home started raw 120 muted and locked. The user's physical remote change inherently unmuted the receiver and cleared the lock; feedback moved monotonically to raw 90, Home persisted 90 for more than five seconds, Netflix stayed 120, generation stayed 1, and the controller remained idle, connected and error-free. The switch-away-and-back restore half passed under L01. |
| M04 | Manually unmute on the Denon, then create one eligible foreground or playback-owner event. | The repeatedly captured X580BT mute-off report `AT 57 1D 01 00 FF` must produce `mute_known=true`, `muted=false`, and `manual_mute_lock=false`. The physical indicator turns off from that manual action, then automation may restore the eligible app exactly. M05 separately governs the new bounded automatic unmute and re-mute transaction. | Eligible event to exact state at most 36.75 s. | **PASS through combined physical evidence.** M03/L01 proved manual receiver-volume intervention cleared mute and lock and learned Home 90; later eligible Netflix and Home events restored exact learned rows. M05 and the final bcef bracket separately proved authoritative automatic re-mute. |
| M05 | Receiver is authoritatively muted and playback is authoritatively idle. Switch to a known app with a different stored target. | Before the first automatic volume command, persist `remute_pending=true`. Automation may then unmute only for this bounded transaction, restore the new app exactly with the accepted controller, send mute, confirm `mute_known=true` and `muted=true`, then clear the pending marker. Both stored rows remain unchanged and no playback starts. | Eligible event through exact target and confirmed re-mute at most 36.75 s; visible volume movement still uses the accepted 4.5 s bound. | **PASS — physical happy path on HA `01eb`/`faa` and firmware `0513`.** Netflix foreground was observed at `19:42:22.876`; ESP accepted after 1.526 s; first unmute or movement followed after 2.062 s. Raw moved 90, 96, 108, 116, 118, 119 and exact 120 without overshoot by 4.468 s. Re-mute began by 4.881 s and authoritative muted, locked, idle and no-error state returned by 5.294 s. Connected state and Home 90, Netflix 120 and Spotify 90 remained stable for more than 29 seconds. |
| M06 | Receiver is muted while Spotify authoritatively owns active playback. Browse Home, screen saver or another app. | The ownership gate remains Spotify. Automation sends zero volume, mute or unmute commands; raw volume, target identity and all stored rows remain unchanged. | Observe at least 36.75 s after each foreground event. Any Denon command fails immediately. | **PASS — valid physical overlap on the current build.** Baseline was Spotify continuously playing and muted with effective owner Spotify, actual raw 120, target 4, Spotify stored 90 and Home stored 90. Home became foreground at `19:48:33.062` while the official player remained Spotify; relay stayed Spotify with `playback_active=true`. More than 35 s of HA evidence and more than 17 s of 250 ms ESP samples showed zero transitions in raw 120, target 4, mute, lock, idle, error, rows or owner, with no Home target or unmute. An earlier Netflix attempt is invalid, not failed, because Spotify stopped before Netflix became foreground. |
| M07 | Start M05, then make playback authoritative and active before the target transaction completes. | On the first authoritative active-playback state, abort the restore. Send no further volume command, re-mute if the bounded transaction had unmuted, confirm authoritative mute, and preserve both app rows. Playback ownership wins. | From the playback-state event through confirmed mute and at least 36.75 s of stable observation. | **Deferred pending a second playback control.** No cue was issued and no result exists; do not label this pass or fail. |
| M08 | While muted, present unknown, stale, off, unavailable or contradictory playback authority during an app switch. | Fail closed. Send zero volume, mute or unmute commands; remain muted; preserve current owner, target identity and all rows. Only fresh authoritative idle may enable M05. | Observe each state for at least 6.75 s. Any Denon command fails immediately. | **Deferred after the production-ready v1 milestone.** No controlled sentinel result exists; coordinate the eventual run with U02. |
| M09 | Interrupt M05 with a controller error, ESP power loss, Bluetooth loss or receiver disconnect before completion. | `remute_pending` was persisted before the first volume command. If the link drops while temporarily unmuted, the accepted limitation is that Denon may remain unmuted until reconnect. On reconnect, the first state-changing command must be mute-on before any volume command; then obtain fresh receiver and playback state. Clear `remute_pending` only after authoritative mute confirmation. Stored rows remain unchanged and an abandoned transaction never resumes unmuted. | Historical reconnect bound is at most 120 s; after reconnect, observe authoritative muted state for at least 36.75 s. | **Deferred, non-blocking for scoped v1.** R05 proves offline event delivery, not loss during an already unmuted target transaction. |
| M10-A | During M05, physically mute from the Denon while the automatic volume rise is active. | Treat authoritative physical mute as immediate cancellation. No later volume movement may run, re-mute recovery must remain locked through delayed feedback, both stored rows must remain unchanged, and `remute_pending` clears only after fresh confirmed mute. | From the physical mute through at least 36.75 s of stable observation. | **PASS — fixed physical rerun on exact live firmware `5fe3` and HA `01eb`.** Home raw 90 to Netflix target 4 was interrupted by physical Mute accepted at raw 108/display 54.0. Target advanced 4 to 5 for re-mute; a delayed `mute=false` transient occurred, but lock and recovery remained engaged. Fresh mute-on was authoritative within 2.523 s. Final state was raw 108, target 5, muted, locked, idle and error-free; Netflix stored raw 120 remained intact and no later movement occurred for more than 34 s. The earlier firmware `0513` run remains historical failure evidence: it ended unmuted at raw 106 and overwrote Netflix 120 to 106. |
| M10-B | During M05, change volume with the physical Denon remote before automation finishes. | Treat the authoritative manual volume as user intent. Cancel or rebase automatic work without replaying stale feedback, do not overwrite the wrong app row, and finish in a state consistent with the receiver's authoritative mute and volume reports. | From the manual volume report through at least 36.75 s of stable observation. | **Deferred after the production-ready v1 milestone.** No physical result exists. |
| W01 | While mute is unknown, on, or manually locked, submit a WebUI exact-target request. Repeat after authoritative manual unmute. | Each muted or unknown request returns HTTP 423 and sends no Denon volume or unmute packet; raw volume, target generation and stored rows stay fixed. After manual unmute, the same user request may proceed. Physical Denon remote volume remains allowed and learns because it is a direct user action. | HTTP result immediately; observe state and packets for at least 6.75 s per rejected request. | **PASS — physical muted rejection.** Target 60 at `11:47:23Z` and direct-up at `11:47:33Z` each returned HTTP 423 with the exact manual-unmute response. More than five seconds after each, raw 90, target and generation 0, null app, mute and lock, Home 90, Netflix 120, Spotify 90, idle and error-free state all remained unchanged. There was no command feedback, movement, unmute, disconnect or read error. Manual-unmute success behavior remains part of the next restoration sequence. |
| L01 | With one known app authoritative, unmuted and unlocked, and no automatic target active, change volume using the physical Denon remote to a distinct value. After persistence, switch away and back. Do not use the WebUI target control for this row. | Receiver feedback changes the active app's stored raw value automatically. No inactive row changes. The value survives delayed NVS persistence, and the eligible return restores that exact raw value with matching physical display, idle state and no error. Automation does not start playback. | Observe stored readback for at least 2.25 s after the last manual step; eligible return to exact state at most 36.75 s. | **PASS — physical remote learning and restoration.** Home started raw 120 muted and locked. The user's physical Denon remote change inherently unmuted and cleared the lock; feedback moved monotonically to raw 90, Home tracked and persisted 90 for more than five seconds, Netflix stayed 120, generation stayed 1, and state remained idle, connected and error-free. The eligible Netflix switch restored its raw 120 row, and the reverse Home switch restored the learned raw 90 value exactly while both rows remained preserved. |
| U01 | With no playback owner, open well-formed unseen bundle `com.apple.TVSettings` while muted; then manually unmute and repeat. | Muted phase: preserve the exact ID but create no row and send no volume or unmute command. Unmuted phase: create exactly one Settings row at receiver raw 90; do not change any existing row, raw volume or target identity. | Activation not before 1.50 s and observed within 1.80 s; cover delayed persistence and hold each phase at least 35 s. | **PASS — both physical phases.** Muted Settings was accepted after 1.774 s and held more than 44 s with raw 90, target 5, mute, lock, idle and no-error state, no row and no actuation. Repeated unmuted, it created exactly one Settings raw-90 row, increasing app count from five to six, while raw 90, target 5, unmuted, unlocked, idle, no-error state and every existing row stayed unchanged for more than 35 s. An intermediate Home lifecycle record was lossy but is not part of this gate. |
| U02 | Inject sentinel app IDs `unknown`, `unavailable`, `none`, and `null`; publish one observed foreground clear; then end the collector stream. | Sentinel app IDs never become rows or targets. Empty/off inputs must leave `current_app_id` and `volume_target_id` stable. An observed online clear must publish nonempty JSON state `none` with `event_kind=foreground_clear`, which HA maps to one explicit empty ownership update. EOF or stop must publish availability offline only, retain the last state, and send no clear to ESP. | Observe each input for at least 6.75 s. | **Deferred after the production-ready v1 milestone.** The deployed guard previously held `current_app_id=null`, `volume_target_id=0`, raw 90 and muted/locked state over 26 samples in 30 seconds, but controlled sentinel, explicit-clear and EOF evidence through MQTT/HA still does not exist. |
| R01 | Start a known-app restore with a target different from the current raw value, then physically interrupt the receiver link. Restore the link without changing app identity. | While disconnected, state reports disconnected, the target identity is retained, and no volume command is sent. After reconnect, firmware obtains fresh receiver status before movement, keeps the same target identity, then settles exactly with no error. | Ready again within 120 s; reconnect to exact target at most 30.25 s. | **Deferred, non-blocking for scoped v1.** R05 interrupted delivery before target start; it did not interrupt an active target. |
| R02 | After learning two distinct app rows, power-cycle the ESP32. Do not clear NVS or change pairing. Reopen each app in turn. | Both rows survive with exact raw values. Bond and app memory remain present. Each eligible app restores exactly, the physical display agrees, and the final state is connected, idle, and error-free. | Each HA event to exact state at most 36.75 s; device ready within 120 s. | **Pending, non-blocking for scoped v1.** App-only flashes preserved NVS, but a deliberate full power-cycle followed by reopening every learned app has not run. |
| R03 | Cause one collector EOF while an app is visibly foreground, then leave the Apple TV unchanged through reconnect. | EOF publishes availability offline without changing retained foreground state, HA becomes `unavailable`, and ESP ownership remains unchanged. Reconnect must not claim online until an authoritative app or explicit-clear record arrives, and it must never publish an empty tombstone or cause a false restore. | Reconnect attempt starts after 0.50 s. Authoritative post-reconnect state latency remains unresolved and must be measured. | This preserves the legacy syslog-only contract for traceability. That runtime has been superseded by the live DVT-authority collector, so the old row does not pass the current deployment; use R03-DVT for acceptance. |
| R03-DVT | With the deployed DVT-authority collector, observe aggregate state while separately interrupting syslog and DVT. | DVT is the sole state authority and syslog is telemetry only. With DVT healthy and authoritative, syslog EOF leaves availability online and preserves current app. DVT failure or ambiguity forces availability offline even if lossy syslog remains live. Neither case replays stale retained state, sends an empty clear to ESP, or starts a restore. | Aggregate failure and recovery bounds are unresolved and must be measured from one observation host. | **Deferred partial row.** Collector 1b passed live hash, ordering and service checks, a 128-second muted no-actuation bracket, and the physical two-action wake rerun with no transient Netflix publish and exactly one Home publish. Deliberate DVT failure, ambiguity, syslog EOF and lossy-lifecycle robustness remain unaccepted. |
| R04 | During a muted idle restore transaction, interrupt and restore the receiver link. | `remute_pending` must already be durable. No command occurs while disconnected or while authority is unknown. The accepted limitation is that Denon may remain temporarily unmuted until reconnect. Mute-on must be the first state-changing post-reconnect command before any volume, followed by fresh state; the abandoned restore cannot resume. | Historical reconnect bound is at most 120 s; observe authoritative muted state for another 36.75 s. | **Deferred, non-blocking for scoped v1.** M10-A and R05 cover adjacent cases, but neither interrupts the live receiver link during temporary unmute. |
| R05 | Make Settings authoritative, interrupt ESP32 network availability, then make Home authoritative with playback paused while the endpoint is down. Restore ESP32 availability without repeating the Apple TV action. | A false pre-ready app request returns HTTP 503 and does not consume its event ID. HA retries that same ID after receiver readiness; ESP accepts Home exactly once and receiver behavior follows Home's stored row. Netflix's intentional raw-130 row remains intact. | Use the existing 5.00 s POST timeout; after endpoint recovery, record retry-to-ESP acceptance and exact settled timing from one observation host. | **PASS — exact live HA `8cd5ed19`/`ea728597` and firmware `bcefdec0`/main `9841`.** The offline Home event was retained and, after reconnect, one target restored 65.0 to 45.0 and re-muted with rows preserved and no duplicate. Final confirmation removed the prior build-10d expiry defect: Netflix `59756dce` ACK 2.2 ms used one target, reached exact raw 130 at `15:37:41.293`, and was remuted, idle and error-free at `15:37:43.305`, stable 19.081 s. Home `66a1d8ea` ACK 1.7 ms used one target, followed raw 129, 116, 96 and exact 90 at `15:38:05.376`, and was remuted, idle and error-free at `15:38:08.421`, stable 45.573 s. Official playback was paused for both; no duplicate, overshoot, read error or row corruption occurred. |

## Run order and completion rule

The user accepted exact live HA `8cd5ed19`/`ea728597` and firmware `bcefdec0`,
source main `9841`, as the scoped production-ready v1 milestone after the final
R05 and bidirectional physical bracket passed. The whole project remains active,
not complete: M07, M08/U02 sentinel behavior, M10-B manual-volume intervention,
M09/R01/R04 active-target link-loss recovery, R02 full power-cycle and
reopen-each-app persistence, and broader collector lossy-lifecycle and DVT fault
robustness are explicitly deferred or unaccepted. Before any later physical row,
recheck connected, authoritative mute, volume, target and restore state. Never
reuse an older sample as that safety gate.

Run `P00`, `F01`, `U01`, and `U02` first. Then run `F02` through `F04`. Keep
`M01`, `M01-MEM`, `M01-PLAY`, and `M02` only as historical evidence; they are
superseded. Run the current contract in `M05` through `M10-B`, with `M03` and
`M04` retained for manual-control behavior. Run R05 before closeout, then finish
the deferred recovery work in `R01` through `R04`
because they interrupt links or power. Return the receiver to its recorded
starting volume and muted state.
`R03-DVT` is now required because the DVT-authority collector is deployed. The
legacy syslog-only R03 remains for traceability and cannot pass current runtime.

Every row needs its own evidence slice and a plain `PASS` or `FAIL`. `U02` may
use controlled Home Assistant state injection because sentinel identities are
not physical apps. All other rows require the physical action shown. No row can
pass from code inspection, simulated tests, or a matching final value without
the correlated intermediate events. Project completion requires every row to
pass. Latency and fine tuning beyond the stated row bounds are deferred and are
not release blockers.

Use `jq -e` to make the receiver part fail closed. These examples assume a
single row's samples have been copied to `slice.jsonl`:

```sh
# Exact settled target with healthy receiver state.
jq -se 'last.state | .connected == true and .volume_raw == 90 and
  .restore_state == "idle" and .restore_error == null' slice.jsonl

# Ownership or mute suppression.
jq -se 'map(.state) as $s |
  ($s | all(.volume_raw == 72)) and
  (($s | map(.volume_target_id) | unique | length) == 1)' slice.jsonl

# Mute rows fail until the validated receiver signal and latch are exposed.
jq -se 'last.state | .mute_known == true and .muted == true and
  .manual_mute_lock == true' slice.jsonl

# One exact stored row. Capture `/api/apps` as one JSON object per line.
jq -se 'last.apps | any(.app_id == "com.netflix.Netflix" and
  .volume_raw == 90)' apps.jsonl
```
