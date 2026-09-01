#!/usr/bin/env python3
"""Reject private deployment values from tracked working-tree files."""

from __future__ import annotations

import argparse
import re
import subprocess
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
IPV4 = re.compile(r"(?<![\d.])(?:\d{1,3}\.){3}\d{1,3}(?![\d.])")
MAC = re.compile(
    r"(?i)(?<![0-9a-f])(?:[0-9a-f]{2}([:-]))(?:[0-9a-f]{2}\1){4}"
    r"[0-9a-f]{2}(?![0-9a-f])"
)
ENTITY_ID = re.compile(r"\bmedia_player\.([a-z0-9_]+)\b")
JWT = re.compile(
    r"\beyJ[A-Za-z0-9_-]{8,}\.[A-Za-z0-9_-]{8,}\.[A-Za-z0-9_-]{8,}\b"
)
BEARER = re.compile(r"(?i)\bBearer\s+[A-Za-z0-9._~-]{20,}\b")
TOKEN_VALUE = re.compile(
    r'''(?ix)["']?(?:ha|home_assistant|access)[_-]?token["']?\s*[:=]\s*'''
    r'''["']([^"']+)["']'''
)
WIFI_DEFAULT = re.compile(
    r'''(?i)#define\s+WIFI_(?:SSID|PASSWORD)\s+["']([^"']+)["']'''
)
PLACEHOLDER_PREFIXES = ("<", "${", "{{", "example", "replace_", "your_")


def tracked_files() -> list[Path]:
    result = subprocess.run(
        ["git", "-C", str(ROOT), "ls-files", "-z"],
        check=True,
        capture_output=True,
    )
    return [ROOT / item.decode() for item in result.stdout.split(b"\0") if item]


def is_private_ipv4(value: str) -> bool:
    try:
        octets = tuple(int(part) for part in value.split("."))
    except ValueError:
        return False
    if len(octets) != 4 or any(part > 255 for part in octets):
        return False
    if octets[0] == 127:
        return False
    return (
        octets[0] == 10
        or octets[:2] == (192, 168)
        or (octets[0] == 172 and 16 <= octets[1] <= 31)
    )


def is_placeholder(value: str) -> bool:
    return value.strip().lower().startswith(PLACEHOLDER_PREFIXES)


def findings(path: Path, text: str) -> list[str]:
    found: list[str] = []
    for line_number, line in enumerate(text.splitlines(), 1):
        for match in IPV4.finditer(line):
            if is_private_ipv4(match.group()):
                found.append(f"{path.relative_to(ROOT)}:{line_number}: private IPv4 literal")
        if MAC.search(line):
            found.append(f"{path.relative_to(ROOT)}:{line_number}: MAC address literal")
        entity_id = ENTITY_ID.search(line)
        if entity_id and not entity_id.group(1).startswith("example_"):
            found.append(f"{path.relative_to(ROOT)}:{line_number}: media_player entity ID")
        if JWT.search(line) or BEARER.search(line):
            found.append(f"{path.relative_to(ROOT)}:{line_number}: token literal")
        token = TOKEN_VALUE.search(line)
        if token and not is_placeholder(token.group(1)):
            found.append(f"{path.relative_to(ROOT)}:{line_number}: non-placeholder token")
        wifi = WIFI_DEFAULT.search(line)
        if wifi and not is_placeholder(wifi.group(1)):
            found.append(f"{path.relative_to(ROOT)}:{line_number}: non-empty Wi-Fi default")
    return found


def scan() -> list[str]:
    found: list[str] = []
    for path in tracked_files():
        try:
            found.extend(findings(path, path.read_text(encoding="utf-8")))
        except (UnicodeDecodeError, OSError):
            continue
    return found


def self_test() -> None:
    private_ip = ".".join(str(part) for part in (10, 1, 2, 3))
    loopback = ".".join(str(part) for part in (127, 0, 0, 1))
    mac = ":".join(("AA", "BB", "CC", "DD", "EE", "FF"))
    entity = "media_" + "player.living_room"
    jwt = "eyJ" + "a" * 8 + "." + "b" * 8 + "." + "c" * 8
    wifi = '#define WIFI_' + 'SSID "real-network"'
    sample = "\n".join((private_ip, mac, entity, jwt, wifi))
    reasons = findings(ROOT / "sample", sample)
    assert len(reasons) == 5, reasons
    assert not findings(ROOT / "sample", loopback)
    assert not findings(ROOT / "sample", '#define WIFI_SSID ""')
    assert not findings(ROOT / "sample", 'access_token: "<HOME_ASSISTANT_TOKEN>"')
    assert not findings(ROOT / "sample", "media_player.example_apple_tv")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--self-test", action="store_true")
    args = parser.parse_args()
    if args.self_test:
        self_test()
        print("secret scanner self-test passed")
        return 0
    found = scan()
    if found:
        print("\n".join(found))
        return 1
    print("tracked-file secret scan passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
