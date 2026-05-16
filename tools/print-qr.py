#!/usr/bin/env python3
"""Reprint a QR sticker from existing factory artefacts.

Wraps qrencode against the qr.txt that tools/factory-flash.py writes.
Useful when a sticker falls off in the field — operator runs this from
the same host that has factory/devices/<serial>/ and gets a fresh PNG.
The board itself doesn't have to be attached.

Usage:
    tools/print-qr.py --serial DP-N-000123
    tools/print-qr.py --serial DP-N-000123 -o /tmp/sticker.png
    tools/print-qr.py --serial DP-N-000123 --terminal   # ascii to stdout
"""
from __future__ import annotations

import argparse
import json
import shutil
import subprocess
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from factory_flash_helpers import (  # noqa: E402
    EXPIRY_WARN_DAYS,
    days_until,
    fail,
)


REPO = Path(__file__).resolve().parent.parent
DEVICES_DIR = REPO / "factory" / "devices"


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--serial", required=True, help="device serial, e.g. DP-N-000123")
    ap.add_argument(
        "-o",
        "--output",
        default=None,
        help="output PNG path (default: factory/devices/<serial>/qr.png, overwrites)",
    )
    ap.add_argument(
        "--terminal",
        action="store_true",
        help="render ascii QR to stdout instead of a PNG. handy in remote shells",
    )
    args = ap.parse_args()

    device_dir = DEVICES_DIR / args.serial
    qr_txt = device_dir / "qr.txt"
    device_json = device_dir / "device.json"

    if not qr_txt.is_file():
        fail(f"no qr.txt at {qr_txt}. Run tools/factory-flash.py --serial {args.serial} first.")

    # warn early so operator doesn't print a sticker that's already rotted
    if device_json.is_file():
        try:
            data = json.loads(device_json.read_text())
            days_left = days_until(data["claim_exp"])
            if days_left <= 0:
                fail(
                    f"claim EXPIRED {-days_left:.0f}d ago for {args.serial}. "
                    "Re-roll first: tools/factory-flash.py --force --serial " + args.serial,
                    code=2,
                )
            if days_left <= EXPIRY_WARN_DAYS:
                sys.stderr.write(
                    f"print-qr: warning, claim expires in {days_left:.0f}d "
                    f"(< {EXPIRY_WARN_DAYS}d). Consider --force re-roll before printing.\n"
                )
        except (json.JSONDecodeError, KeyError) as e:
            sys.stderr.write(f"print-qr: warning, can't read {device_json}: {e}\n")

    if not shutil.which("qrencode"):
        fail("qrencode not on PATH. Install with `brew install qrencode` (mac) or apt.")

    qr_text = qr_txt.read_text().strip()

    if args.terminal:
        # -t ANSIUTF8 fits in 80 cols, scannable from screen
        subprocess.run(["qrencode", "-t", "ANSIUTF8", qr_text], check=True)
        return 0

    out_png = Path(args.output) if args.output else device_dir / "qr.png"
    subprocess.run(
        ["qrencode", "-l", "M", "-s", "8", "-o", str(out_png), qr_text],
        check=True,
    )
    print(f"wrote {out_png}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
