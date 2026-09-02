import asyncio
import json
import os
import re
import threading
import unittest
from datetime import datetime, timedelta, timezone
from unittest.mock import patch

from tools.appletv_foreground.collector import (
    Config,
    DvtSettleGuard,
    ForegroundState,
    ForegroundTracker,
    _VALUE_TEMPLATE,
    _clear_payload,
    _dvt_loop,
    _dvt_session,
    _next_failure_count,
    _reconnect_delay,
    _snapshot_foreground,
    _source_datetime,
    _source_is_authoritative,
    _source_timestamp,
    _state_payload,
)


class ForegroundTrackerTest(unittest.TestCase):
    def test_post_overlay_settle_drops_underlying_app_pulse(self) -> None:
        guard = DvtSettleGuard(3.0)
        published = [""]
        guard.arm()

        self.assertFalse(guard.accepts("com.netflix.Netflix", 10.0))
        self.assertFalse(guard.accepts("com.netflix.Netflix", 12.091))
        guard.arm()  # The ambiguous DVT snapshot keeps availability offline.
        self.assertFalse(guard.accepts("com.apple.HeadBoard", 12.5))
        self.assertFalse(guard.accepts("com.apple.HeadBoard", 15.499))
        self.assertTrue(guard.accepts("com.apple.HeadBoard", 15.5))
        published.append("com.apple.HeadBoard")

        self.assertEqual(published, ["", "com.apple.HeadBoard"])

    def test_post_overlay_settle_allows_sustained_resume_once(self) -> None:
        guard = DvtSettleGuard(3.0)
        published: list[str] = []
        guard.arm()

        for now in (20.0, 20.5, 21.0, 22.999):
            if guard.accepts("com.netflix.Netflix", now):
                published.append("com.netflix.Netflix")
        if guard.accepts("com.netflix.Netflix", 23.0):
            published.append("com.netflix.Netflix")

        self.assertEqual(published, ["com.netflix.Netflix"])
        self.assertTrue(guard.accepts("com.apple.HeadBoard", 23.1))

    def test_post_overlay_settle_has_no_delayed_callback(self) -> None:
        guard = DvtSettleGuard(3.0)
        published: list[str] = []
        guard.arm()
        self.assertFalse(guard.accepts("com.netflix.Netflix", 30.0))

        # Merely advancing time cannot publish; a fresh DVT snapshot is required.
        self.assertEqual(published, [])
        guard.arm()  # A stop, ambiguity, or reconnect discards the pending app.
        self.assertFalse(guard.accepts("com.netflix.Netflix", 40.0))
        self.assertEqual(published, [])

    def test_normal_dvt_app_switch_is_not_delayed(self) -> None:
        guard = DvtSettleGuard(3.0)
        self.assertTrue(guard.accepts("com.netflix.Netflix", 50.0))
        self.assertTrue(guard.accepts("com.apple.HeadBoard", 50.1))

    def test_outage_preserves_retained_state_but_never_replays_it_online(self) -> None:
        state = ForegroundState()
        self.assertIsNone(state.replay_payload)
        self.assertFalse(state.online)

        state.observe("current")
        self.assertEqual(state.replay_payload, "current")
        self.assertTrue(state.online)

        state.outage()
        self.assertEqual(state.payload, "current")
        self.assertIsNone(state.replay_payload)
        self.assertFalse(state.online)

    def test_availability_stays_online_while_either_source_is_live(self) -> None:
        state = ForegroundState()
        state.observe("syslog", source="syslog")
        state.observe("dvt", source="dvt")
        state.outage("syslog")
        self.assertTrue(state.online)
        self.assertEqual(state.replay_payload, "dvt")
        state.outage("dvt")
        self.assertFalse(state.online)

    def test_dvt_is_required_when_it_is_the_authoritative_source(self) -> None:
        state = ForegroundState(required_source="dvt")
        state.observe("netflix", source="syslog")
        self.assertFalse(state.online)
        self.assertIsNone(state.replay_payload)

        state.observe("netflix", source="dvt")
        self.assertTrue(state.online)
        self.assertEqual(state.replay_payload, "netflix")

        state.outage("dvt")
        self.assertFalse(state.online)
        self.assertIsNone(state.replay_payload)
        self.assertIn("syslog", state.online_sources)

    def test_state_and_availability_publications_are_serialized(self) -> None:
        state = ForegroundState()
        state_published = threading.Event()
        release_online = threading.Event()
        outage_waiting = threading.Event()
        actions: list[tuple[str, str]] = []

        def observation() -> None:
            with state.lock:
                state.observe("current")
                actions.append(("state", "current"))
                state_published.set()
                self.assertTrue(release_online.wait(1))
                actions.append(("availability", "online"))

        def outage() -> None:
            self.assertTrue(state_published.wait(1))
            outage_waiting.set()
            with state.lock:
                state.outage()
                actions.append(("availability", "offline"))

        first = threading.Thread(target=observation)
        second = threading.Thread(target=outage)
        first.start()
        self.assertTrue(state_published.wait(1))
        second.start()
        self.assertTrue(outage_waiting.wait(1))
        self.assertEqual(actions, [("state", "current")])
        release_online.set()
        first.join(1)
        second.join(1)

        self.assertFalse(first.is_alive())
        self.assertFalse(second.is_alive())
        self.assertEqual(
            actions,
            [
                ("state", "current"),
                ("availability", "online"),
                ("availability", "offline"),
            ],
        )
        self.assertFalse(state.online)

    def test_empty_retained_state_has_a_safe_template_fallback(self) -> None:
        self.assertIn("value_json is defined", _VALUE_TEMPLATE)
        self.assertIn("else 'unknown'", _VALUE_TEMPLATE)

    def test_proven_transition_signatures_and_deduplication(self) -> None:
        tracker = ForegroundTracker()

        def feed(process: str, message: str) -> str | None:
            return tracker.feed(
                {"filename": f"/usr/libexec/{process}", "message": message}
            )

        self.assertIsNone(feed("symptomsd", "com.netflix.Netflix: Foreground: false"))
        self.assertEqual(
            feed("symptomsd", "com.netflix.Netflix: Foreground: true"),
            "com.netflix.Netflix",
        )
        self.assertIsNone(feed("symptomsd", "com.netflix.Netflix: Foreground: true"))
        self.assertIsNone(feed("symptomsd", "com.netflix.Netflix: Foreground: false"))
        self.assertIsNone(tracker.active_app)

        home = (
            "[0x1:(FBSceneManager):com.apple.HeadBoard-default] "
            "Scene lifecycle state did change: Foreground"
        )
        self.assertEqual(feed("PineBoard", home), "com.apple.HeadBoard")
        self.assertIsNone(feed("PineBoard", home))
        netflix = (
            "[0x2:(FBSceneManager):com.netflix.Netflix-default] "
            "Scene lifecycle state did change: Foreground"
        )
        self.assertEqual(feed("PineBoard", netflix), "com.netflix.Netflix")
        youtube = (
            "[0x3:(FBSceneManager):com.google.ios.youtube-default] "
            "Scene lifecycle state did change: Foreground"
        )
        self.assertEqual(feed("PineBoard", youtube), "com.google.ios.youtube")
        self.assertIsNone(
            feed(
                "PineBoard",
                "Including scene com.netflix.Netflix-default "
                "(bundle: com.netflix.Netflix) — process foreground: YES, "
                "scene settings foreground: YES",
            )
        )
        self.assertEqual(feed("PineBoard", home), "com.apple.HeadBoard")
        self.assertIsNone(
            feed("symptomsd", "com.google.ios.youtube: Foreground: false")
        )
        self.assertEqual(tracker.active_app, "com.apple.HeadBoard")

    def test_dvt_snapshot_selects_one_foreground_app_or_fails_closed(self) -> None:
        def process(bundle_id: str, *, foreground: bool = True) -> dict:
            return {
                "bundleIdentifier": bundle_id,
                "isApplication": True,
                "foregroundRunning": foreground,
            }

        infrastructure = [
            process("com.apple.PineBoard"),
            process("com.apple.TVSystemUIService"),
        ]
        self.assertEqual(
            _snapshot_foreground(
                [*infrastructure, process("com.netflix.Netflix")]
            ),
            "com.netflix.Netflix",
        )
        self.assertEqual(
            _snapshot_foreground(
                [*infrastructure, process("com.apple.HeadBoard")]
            ),
            "com.apple.HeadBoard",
        )
        self.assertIsNone(_snapshot_foreground(infrastructure))
        self.assertIsNone(
            _snapshot_foreground(
                [
                    *infrastructure,
                    process("com.netflix.Netflix"),
                    process("com.google.ios.youtube"),
                ]
            )
        )
        self.assertEqual(
            _snapshot_foreground(
                [
                    *infrastructure,
                    process("com.netflix.Netflix"),
                    process("com.apple.IdleScreen"),
                ]
            ),
            "",
        )
        self.assertEqual(
            _snapshot_foreground(
                [
                    *infrastructure,
                    process("com.netflix.Netflix"),
                    {
                        "foregroundRunning": True,
                        "name": "MemoryPoster",
                        "realAppName": "/System/MemoryPoster",
                    },
                ]
            ),
            "",
        )

    def test_cross_source_merge_rejects_delayed_lifecycle_records(self) -> None:
        tracker = ForegroundTracker()
        now = datetime(2026, 9, 2, 8, tzinfo=timezone.utc)

        self.assertEqual(
            tracker.apply("com.apple.HeadBoard", True, now), "changed"
        )
        self.assertEqual(
            tracker.apply("com.netflix.Netflix", True, now + timedelta(seconds=2)),
            "changed",
        )
        self.assertEqual(
            tracker.apply("com.apple.HeadBoard", True, now + timedelta(seconds=1)),
            "ignored",
        )
        self.assertEqual(
            tracker.apply("com.netflix.Netflix", True, now + timedelta(seconds=2)),
            "same",
        )
        self.assertEqual(tracker.active_app, "com.netflix.Netflix")

    def test_syslog_cannot_override_dvt_when_polling_is_enabled(self) -> None:
        tracker = ForegroundTracker()
        now = datetime(2026, 9, 2, 8, tzinfo=timezone.utc)
        self.assertTrue(_source_is_authoritative("dvt", True))
        self.assertFalse(_source_is_authoritative("syslog", True))
        self.assertTrue(_source_is_authoritative("syslog", False))

        self.assertEqual(
            tracker.apply("com.netflix.Netflix", True, now), "changed"
        )
        if _source_is_authoritative("syslog", True):
            tracker.apply(
                "com.apple.HeadBoard", True, now + timedelta(seconds=10)
            )
        self.assertEqual(tracker.active_app, "com.netflix.Netflix")

    def test_first_authoritative_overlay_publishes_clear_before_online(self) -> None:
        tracker = ForegroundTracker()
        state = ForegroundState(required_source="dvt")
        now = datetime(2026, 9, 2, 8, tzinfo=timezone.utc)
        actions: list[str] = []

        with state.lock:
            self.assertEqual(tracker.clear(now), "cleared")
            payload = _clear_payload("", "2026-09-02T08:00:00.000Z", "c" * 32)
            state.observe(payload, source="dvt")
            actions.append("state_clear")
            if state.online:
                actions.append("online")

        self.assertEqual(actions, ["state_clear", "online"])
        self.assertEqual(tracker.clear(now), "same")
        self.assertEqual(json.loads(payload)["previous_app_id"], "")

    def test_tracker_merge_and_publication_share_one_lock(self) -> None:
        tracker = ForegroundTracker()
        state = ForegroundState(required_source="dvt")
        now = datetime(2026, 9, 2, 8, tzinfo=timezone.utc)
        self.assertEqual(
            tracker.apply("com.apple.HeadBoard", True, now), "changed"
        )
        entered = threading.Event()
        release = threading.Event()
        actions: list[str] = []

        def dvt() -> None:
            with state.lock:
                self.assertEqual(
                    tracker.apply(
                        "com.netflix.Netflix",
                        True,
                        now + timedelta(seconds=2),
                    ),
                    "changed",
                )
                entered.set()
                self.assertTrue(release.wait(1))
                state.observe("netflix", source="dvt")
                actions.append("netflix")

        def delayed_syslog() -> None:
            self.assertTrue(entered.wait(1))
            with state.lock:
                self.assertEqual(
                    tracker.apply(
                        "com.apple.HeadBoard",
                        True,
                        now + timedelta(seconds=1),
                    ),
                    "ignored",
                )
                actions.append("syslog_ignored")

        first = threading.Thread(target=dvt)
        second = threading.Thread(target=delayed_syslog)
        first.start()
        second.start()
        self.assertTrue(entered.wait(1))
        release.set()
        first.join(1)
        second.join(1)
        self.assertEqual(actions, ["netflix", "syslog_ignored"])
        self.assertEqual(tracker.active_app, "com.netflix.Netflix")

    def test_dvt_disconnect_recreates_the_session(self) -> None:
        async def run() -> None:
            stop = threading.Event()
            calls = 0
            outages = 0

            async def session(
                _udid, _emit, _interval, _timeout, *, stop_event
            ):
                nonlocal calls
                calls += 1
                if calls == 1:
                    raise ConnectionError("disconnected")
                stop_event.set()

            def outage() -> None:
                nonlocal outages
                outages += 1

            await _dvt_loop(
                "target",
                lambda *_args: None,
                outage,
                stop,
                0.5,
                0,
                3,
                session=session,
            )
            self.assertEqual(calls, 2)
            self.assertEqual(outages, 2)

        asyncio.run(run())

    def test_dvt_request_start_time_cannot_overwrite_newer_lifecycle(self) -> None:
        async def run() -> None:
            stop = threading.Event()
            tracker = ForegroundTracker()
            results: list[str] = []

            class Rsd:
                udid = "target"

                async def close(self) -> None:
                    pass

            class Context:
                async def __aenter__(self):
                    return self

                async def __aexit__(self, *_args) -> None:
                    pass

            class Info(Context):
                async def proclist(self):
                    await asyncio.sleep(0.001)
                    lifecycle_at = datetime.now(timezone.utc)
                    self_outer.assertEqual(
                        tracker.apply(
                            "com.apple.HeadBoard", True, lifecycle_at
                        ),
                        "changed",
                    )
                    stop.set()
                    return [
                        {
                            "bundleIdentifier": "com.netflix.Netflix",
                            "isApplication": True,
                            "foregroundRunning": True,
                        }
                    ]

            async def devices():
                return [Rsd()]

            self_outer = self

            def emit(app_id: str | None, event_at: datetime) -> None:
                assert app_id is not None
                results.append(tracker.apply(app_id, True, event_at))

            await _dvt_session(
                "target",
                emit,
                0.5,
                3,
                stop_event=stop,
                get_devices=devices,
                dvt_factory=lambda _rsd: Context(),
                info_factory=lambda _dvt: Info(),
            )
            self.assertEqual(results, ["ignored"])
            self.assertEqual(tracker.active_app, "com.apple.HeadBoard")

        asyncio.run(run())

    def test_dvt_process_snapshot_timeout_closes_the_session(self) -> None:
        async def run() -> None:
            stop = threading.Event()
            closed = 0

            class Rsd:
                udid = "target"

                async def close(self) -> None:
                    nonlocal closed
                    closed += 1

            class Context:
                async def __aenter__(self):
                    return self

                async def __aexit__(self, *_args) -> None:
                    pass

            class Info(Context):
                async def proclist(self):
                    await asyncio.Event().wait()

            async def devices():
                return [Rsd()]

            with self.assertRaises(TimeoutError):
                await _dvt_session(
                    "target",
                    lambda *_args: None,
                    0.5,
                    0.001,
                    stop_event=stop,
                    get_devices=devices,
                    dvt_factory=lambda _rsd: Context(),
                    info_factory=lambda _dvt: Info(),
                )
            self.assertEqual(closed, 1)

        asyncio.run(run())

    def test_dvt_poll_default_is_nonzero(self) -> None:
        environment = {
            "APPLE_TV_UDID": "target",
            "MQTT_HOST": "127.0.0.1",
        }
        with patch.dict(os.environ, environment, clear=True):
            config = Config.from_env()
            self.assertEqual(config.dvt_poll_seconds, 0.5)
            self.assertEqual(config.dvt_timeout_seconds, 3)
            os.environ["DVT_POLL_SECONDS"] = "0"
            self.assertEqual(Config.from_env().dvt_poll_seconds, 0)

    def test_nonfinite_or_negative_intervals_are_rejected(self) -> None:
        environment = {
            "APPLE_TV_UDID": "target",
            "MQTT_HOST": "127.0.0.1",
        }
        cases = (
            ("DVT_POLL_SECONDS", "-1"),
            ("DVT_POLL_SECONDS", "nan"),
            ("DVT_TIMEOUT_SECONDS", "inf"),
            ("DVT_TIMEOUT_SECONDS", "-inf"),
            ("RECONNECT_SECONDS", "nan"),
            ("RECONNECT_SECONDS", "-1"),
        )
        for name, value in cases:
            with self.subTest(name=name, value=value):
                with patch.dict(os.environ, environment, clear=True):
                    os.environ[name] = value
                    with self.assertRaises(ValueError):
                        Config.from_env()

    def test_observable_state_payload(self) -> None:
        event_id = "a" * 32
        observed_at = "2026-09-02T05:35:01.234Z"
        payload = json.loads(
            _state_payload("com.spotify.client", observed_at, event_id)
        )
        self.assertEqual(payload["state"], "Spotify")
        self.assertEqual(payload["app_id"], "com.spotify.client")
        self.assertEqual(payload["app_name"], "Spotify")
        self.assertEqual(payload["observed_at"], observed_at)
        self.assertEqual(payload["event_id"], event_id)
        self.assertEqual(
            _source_timestamp({"timestamp": "2026-09-02 11:05:01.123\nspoof"}),
            "2026-09-02 11:05:01.123_spoof",
        )
        self.assertRegex(observed_at, re.compile(r"Z$"))

    def test_syslog_source_timestamp_is_normalized_to_utc(self) -> None:
        india = timezone(timedelta(hours=5, minutes=30))
        self.assertEqual(
            _source_datetime(
                {"timestamp": "2026-09-02 13:30:01.125"}, india
            ),
            datetime(2026, 9, 2, 8, 0, 1, 125000, tzinfo=timezone.utc),
        )
        for record in ({}, {"timestamp": None}, {"timestamp": "invalid"}):
            self.assertIsNone(_source_datetime(record, india))

    def test_online_clear_is_not_an_mqtt_tombstone(self) -> None:
        payload = json.loads(
            _clear_payload(
                "com.netflix.Netflix",
                "2026-09-02T05:35:01.234Z",
                "b" * 32,
            )
        )
        self.assertEqual(payload["state"], "none")
        self.assertEqual(payload["app_id"], "")
        self.assertEqual(payload["app_name"], "")
        self.assertEqual(payload["event_kind"], "foreground_clear")
        self.assertEqual(payload["previous_app_id"], "com.netflix.Netflix")

    def test_reconnect_delay_is_fast_then_bounded(self) -> None:
        self.assertEqual(_reconnect_delay(0.5, 1), 0.5)
        self.assertEqual(_reconnect_delay(0.5, 2), 1.0)
        self.assertEqual(_reconnect_delay(0.5, 3), 2.0)
        self.assertEqual(_reconnect_delay(0.5, 100), 30.0)

    def test_stable_stream_resets_repeated_failure_count(self) -> None:
        self.assertEqual(_next_failure_count(0, 1.0), 1)
        self.assertEqual(_next_failure_count(1, 1.0), 2)
        self.assertEqual(_next_failure_count(7, 30.0), 1)


if __name__ == "__main__":
    unittest.main()
