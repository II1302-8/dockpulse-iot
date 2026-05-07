"""Shared helpers between factory-flash.py, verify-device.py, print-qr.py.

factory-flash.py uses a dash so it can't be imported as a module — keep
the cross-tool helpers here so verify-device and print-qr can reuse them
without subprocessing factory-flash.
"""
from __future__ import annotations

import os
import shutil
import subprocess
import sys
import time
from pathlib import Path


# matches the marker dp_prov_get_dev_uuid appends after the 6-byte MAC
MARKER_HEX = b"DOCKPULSE".hex() + "01"

# warn at factory + verify time so operator doesn't ship a sticker that
# rots in 2 weeks
EXPIRY_WARN_DAYS = 30


def fail(msg: str, code: int = 1) -> None:
    sys.stderr.write(f"{Path(sys.argv[0]).name}: {msg}\n")
    sys.exit(code)


def default_port() -> str | None:
    for pattern in ("cu.usbmodem*", "ttyACM*", "ttyUSB*"):
        candidates = sorted(Path("/dev").glob(pattern))
        if candidates:
            return str(candidates[0])
    return None


def activate_idf() -> None:
    # nvs_partition_gen.py + esptool.py both live under $IDF_PATH
    if os.environ.get("IDF_PATH") and shutil.which("esptool.py"):
        return
    idf_path = os.environ.get("IDF_PATH") or str(Path.home() / "esp/esp-idf")
    export_sh = Path(idf_path) / "export.sh"
    if not export_sh.is_file():
        fail(f"ESP-IDF not found at {idf_path}. Set IDF_PATH or install per CONTRIBUTING.md.")
    out = subprocess.run(
        ["bash", "-c", f". {export_sh} >/dev/null && env"],
        capture_output=True,
        text=True,
        check=True,
    )
    for line in out.stdout.splitlines():
        if "=" in line:
            k, v = line.split("=", 1)
            os.environ[k] = v


def read_mac(port: str) -> bytes:
    activate_idf()
    out = subprocess.run(
        [
            "esptool.py",
            "--chip",
            "esp32c3",
            "--port",
            port,
            "--before",
            "default_reset",
            "--after",
            "hard_reset",
            "read_mac",
        ],
        capture_output=True,
        text=True,
        check=True,
    )
    for line in out.stdout.splitlines():
        # "MAC: aa:bb:cc:dd:ee:ff"
        if line.startswith("MAC:"):
            mac_str = line.split(":", 1)[1].strip()
            return bytes.fromhex(mac_str.replace(":", ""))
    fail(f"could not parse MAC from esptool output:\n{out.stdout}")
    return b""  # unreachable


def compose_uuid(mac: bytes) -> str:
    if len(mac) != 6:
        fail(f"expected 6-byte MAC, got {len(mac)}")
    return mac.hex() + MARKER_HEX


def days_until(epoch_exp: int | float) -> float:
    return (epoch_exp - time.time()) / 86400.0
