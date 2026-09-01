#!/usr/bin/env python3
"""Record and check live Denon ESP32 state without changing the device."""

import argparse
import json
import os
import time
import urllib.error
import urllib.request
from datetime import datetime, timezone
from pathlib import Path


def read_state(url: str) -> dict:
    with urllib.request.urlopen(f"{url.rstrip('/')}/api/state", timeout=2) as response:
        if response.status != 200:
            raise RuntimeError(f"GET /api/state returned HTTP {response.status}")
        return json.load(response)


def matches(condition: str, state: dict, raw: int | None = None) -> bool:
    if condition == "ready":
        raw = state.get("volume_raw")
        return (
            state.get("receiver_configured") is True
            and state.get("receiver_bonded") is True
            and state.get("connected") is True
            and isinstance(raw, int)
            and state.get("volume") == raw / 2
            and state.get("volume_db") == raw / 2 - 80
        )
    if condition == "paired-ready":
        return matches("ready", state) and state.get("a2dp_connected") is True
    if condition == "background-ready":
        return matches("ready", state)
    if condition == "pairing":
        number = state.get("pairing_number")
        return (
            state.get("receiver_configured") is True
            and state.get("connecting") is True
            and state.get("pairing_status") == "confirm_on_denon"
            and isinstance(number, str)
            and len(number) == 6
            and number.isdigit()
        )
    if condition == "forgotten":
        return (
            state.get("receiver_configured") is False
            and state.get("receiver_bonded") is False
            and state.get("a2dp_connected") is False
            and state.get("connected") is False
            and state.get("receiver_mac") is None
            and state.get("setup_ap") is False
        )
    if condition == "receiver-down":
        return state.get("receiver_configured") is True and state.get("connected") is False
    if condition == "volume":
        return matches("ready", state) and state.get("volume_raw") == raw
    if condition == "network-dhcp":
        return state.get("network_mode") == "dhcp"
    if condition == "network-static":
        return state.get("network_mode") == "static"
    raise ValueError(condition)


def record(path: Path, label: str, event: str, state: dict | None = None) -> None:
    item = {
        "at": datetime.now(timezone.utc).isoformat(),
        "label": label,
        "event": event,
    }
    if state is not None:
        item["state"] = state
    line = json.dumps(item, separators=(",", ":"), sort_keys=True)
    with path.open("a") as output:
        output.write(line + "\n")
    print(line, flush=True)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument(
        "condition",
        choices=(
            "ready",
            "paired-ready",
            "background-ready",
            "pairing",
            "forgotten",
            "volume",
            "network-dhcp",
            "network-static",
            "esp-cycle",
            "denon-cycle",
        ),
    )
    parser.add_argument("--url", default=os.environ.get("DENON_URL"))
    parser.add_argument("--label", required=True)
    parser.add_argument("--evidence", type=Path, required=True)
    parser.add_argument("--timeout", type=float, default=120)
    parser.add_argument("--interval", type=float, default=0.25)
    parser.add_argument("--raw", type=int)
    args = parser.parse_args()
    if not args.url:
        parser.error("--url or DENON_URL is required")
    if args.condition == "volume" and args.raw is None:
        parser.error("volume requires --raw")

    deadline = time.monotonic() + args.timeout
    saw_baseline = False
    saw_outage = False
    consecutive_unreachable = 0
    previous = object()
    while time.monotonic() < deadline:
        try:
            state = read_state(args.url)
            if state != previous:
                record(args.evidence, args.label, "state", state)
                previous = state
            if args.condition in (
                "ready",
                "paired-ready",
                "background-ready",
                "pairing",
                "forgotten",
                "volume",
                "network-dhcp",
                "network-static",
            ) and matches(args.condition, state, args.raw):
                return 0
            saw_baseline = saw_baseline or matches("ready", state)
            if args.condition == "esp-cycle" and saw_outage and matches("ready", state):
                return 0
            if args.condition == "esp-cycle" and saw_baseline and matches("receiver-down", state):
                saw_outage = True
            if args.condition == "denon-cycle":
                saw_outage = saw_outage or (saw_baseline and matches("receiver-down", state))
                if saw_outage and matches("ready", state):
                    return 0
            consecutive_unreachable = 0
        except (OSError, RuntimeError, json.JSONDecodeError, urllib.error.URLError) as error:
            if previous != "unreachable":
                record(args.evidence, args.label, "unreachable", {"error": str(error)})
                previous = "unreachable"
            if args.condition == "esp-cycle" and saw_baseline:
                consecutive_unreachable += 1
                saw_outage = consecutive_unreachable >= 2
        time.sleep(args.interval)

    record(args.evidence, args.label, "timeout")
    return 1


if __name__ == "__main__":
    raise SystemExit(main())
