#!/usr/bin/env python3
"""Verify a flashed board matches its factory artefacts.

Connects to an attached board, reads the eFuse MAC via esptool, composes
the expected mesh UUID, and matches against factory/devices/*/device.json.
Prints PASS/FAIL with uuid + JWT expiry status. Useful before shipping a
device to make sure the printed sticker still matches the board.

Usage:
    tools/verify-device.py                              # auto-discover by mac
    tools/verify-device.py --serial DP-N-000123         # check one specific
    tools/verify-device.py --port /dev/cu.usbmodem21
"""
from __future__ import annotations

import argparse
import json
import sys
import time
from pathlib import Path

# factory-flash sits next to us so import works
sys.path.insert(0, str(Path(__file__).resolve().parent))
from factory_flash_helpers import (  # type: ignore[import-not-found]
    EXPIRY_WARN_DAYS,
    compose_uuid,
    days_until,
    default_port,
    read_mac,
)


REPO = Path(__file__).resolve().parent.parent
DEVICES_DIR = REPO / "factory" / "devices"


def find_device_by_uuid(uuid_hex: str) -> dict | None:
    if not DEVICES_DIR.is_dir():
        return None
    for device_json in DEVICES_DIR.glob("*/device.json"):
        try:
            data = json.loads(device_json.read_text())
        except (OSError, json.JSONDecodeError):
            continue
        if data.get("uuid") == uuid_hex:
            return data
    return None


def report(device: dict) -> int:
    days_left = days_until(device["claim_exp"])
    print(f"PASS device {device['serial']}")
    print(f"  uuid    {device['uuid']}")
    print(f"  mac     {device['mac']}")
    print(f"  role    {device['role']}")
    if days_left <= 0:
        print(f"  jwt     EXPIRED {-days_left:.0f}d ago — re-flash with --force before shipping")
        return 2
    if days_left <= EXPIRY_WARN_DAYS:
        print(f"  jwt     expires in {days_left:.0f}d — within {EXPIRY_WARN_DAYS}d warn window")
        return 1
    print(f"  jwt     valid for {days_left:.0f}d")
    return 0


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--port", default=None)
    ap.add_argument(
        "--serial",
        default=None,
        help="check this specific device.json instead of looking up by mac",
    )
    args = ap.parse_args()

    port = args.port or default_port()
    if not port:
        print("FAIL no usb-serial device found. Plug in a board or pass --port.", file=sys.stderr)
        return 2

    mac = read_mac(port)
    expected_uuid = compose_uuid(mac)

    if args.serial:
        device_json = DEVICES_DIR / args.serial / "device.json"
        if not device_json.is_file():
            print(f"FAIL no factory artefacts at {device_json}", file=sys.stderr)
            return 2
        device = json.loads(device_json.read_text())
        if device["uuid"] != expected_uuid:
            print(
                f"FAIL board mac {mac.hex()} (uuid {expected_uuid}) does not match "
                f"{args.serial} (uuid {device['uuid']}). Wrong board for this sticker.",
                file=sys.stderr,
            )
            return 2
        return report(device)

    device = find_device_by_uuid(expected_uuid)
    if device is None:
        print(
            f"FAIL board mac {mac.hex()} (uuid {expected_uuid}) has no factory record "
            f"under {DEVICES_DIR}. Run tools/factory-flash.py to provision.",
            file=sys.stderr,
        )
        return 2
    return report(device)


if __name__ == "__main__":
    raise SystemExit(main())
