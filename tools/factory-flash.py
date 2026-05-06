#!/usr/bin/env python3
"""Per-device factory flash for DockPulse sensor and gateway nodes.

Reads the device MAC, composes a mesh UUID matching dp_prov_get_dev_uuid
(MAC + "DOCKPULSE" + 0x01), generates a random 16-byte static OOB, signs
an Ed25519 claim JWT with the factory key, builds an NVS partition image
holding the OOB, flashes bootloader + ptable + app + factory_nvs over
USB, and renders a QR PNG that the operator sticks on the enclosure.

Run after `tools/build.sh` for the role you want, with the board
attached:

    tools/factory-flash.py --serial DP-N-000123
    tools/factory-flash.py --serial DP-N-000123 --role gateway
    tools/factory-flash.py --serial DP-N-000123 --nvs-only   # rotate OOB on already-flashed board

Prereqs:
- ESP-IDF env activated (we activate it ourselves if you have $IDF_PATH set)
- `tools/factory-keygen.sh` already ran once on this host
- `qrencode` on $PATH for the sticker PNG (brew install qrencode)
- `cryptography` Python package (already in IDF's pyenv)
"""
from __future__ import annotations

import argparse
import base64
import binascii
import json
import os
import secrets
import shutil
import subprocess
import sys
import time
import uuid as _uuid
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent

# matches the marker dp_prov_get_dev_uuid appends after the 6-byte MAC
MARKER_HEX = b"DOCKPULSE".hex() + "01"

# factory_nvs partition. keep in sync with partitions.csv
FACTORY_NVS_OFFSET = 0x200000
FACTORY_NVS_SIZE = 0x10000
FACTORY_NAMESPACE = "factory"
FACTORY_KEY_OOB = "oob"

# flash layout for app+bootloader+ptable. keep in sync with build_*/flasher_args.json
APP_FLASH_OFFSETS = {
    "bootloader": 0x0,
    "partition_table": 0x8000,
    "app": 0x10000,
}


def fail(msg: str, code: int = 1) -> None:
    sys.stderr.write(f"factory-flash: {msg}\n")
    sys.exit(code)


def b64u(data: bytes) -> str:
    return base64.urlsafe_b64encode(data).rstrip(b"=").decode()


def activate_idf() -> None:
    # nvs_partition_gen.py lives under $IDF_PATH, so even if esptool.py is
    # already on the path we still need the env populated. running export.sh
    # is idempotent
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


def default_port() -> str | None:
    candidates = sorted(Path("/dev").glob("cu.usbmodem*"))
    return str(candidates[0]) if candidates else None


def read_mac(port: str) -> bytes:
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


def sign_claim(uuid_hex: str, serial: str, exp_days: int, priv_pem: bytes) -> tuple[str, dict]:
    # avoid pyjwt dep — IDF's pyenv already has cryptography
    from cryptography.hazmat.primitives.serialization import load_pem_private_key

    priv = load_pem_private_key(priv_pem, password=None)
    now = int(time.time())
    claim = {
        "iss": "factory",
        "sub": serial,
        "uuid": uuid_hex,
        "jti": str(_uuid.uuid4()),
        "iat": now,
        "exp": now + exp_days * 86400,
    }
    header = {"alg": "EdDSA", "typ": "JWT"}
    h = b64u(json.dumps(header, separators=(",", ":")).encode())
    p = b64u(json.dumps(claim, separators=(",", ":")).encode())
    signing_input = f"{h}.{p}".encode()
    signature = priv.sign(signing_input)
    return f"{h}.{p}.{b64u(signature)}", claim


def build_qr_payload(uuid_hex: str, oob_hex: str, serial: str, jwt_token: str) -> str:
    qr = {
        "v": 1,
        "uuid": uuid_hex,
        "oob": oob_hex,
        "sn": serial,
        "jwt": jwt_token,
    }
    return b64u(json.dumps(qr, separators=(",", ":")).encode())


def gen_nvs_image(oob_hex: str, out_bin: Path) -> None:
    idf_path = Path(os.environ["IDF_PATH"])
    gen = idf_path / "components/nvs_flash/nvs_partition_generator/nvs_partition_gen.py"
    if not gen.is_file():
        fail(f"nvs_partition_gen.py not found at {gen}")

    csv_path = out_bin.with_suffix(".csv")
    csv_path.write_text(
        "key,type,encoding,value\n"
        f"{FACTORY_NAMESPACE},namespace,,\n"
        f"{FACTORY_KEY_OOB},data,hex2bin,{oob_hex}\n"
    )
    # nvs_partition_gen ships its dependencies as a separate module
    # (esp_idf_nvs_partition_gen) inside IDF's pyenv, not in the system
    # interpreter. resolve via PATH after activate_idf
    py = shutil.which("python") or shutil.which("python3")
    if not py:
        fail("no python on PATH after activate_idf")
    subprocess.run(
        [
            py,
            str(gen),
            "generate",
            str(csv_path),
            str(out_bin),
            f"0x{FACTORY_NVS_SIZE:x}",
        ],
        check=True,
    )


def render_qr_png(qr_text: str, out_png: Path) -> None:
    if not shutil.which("qrencode"):
        sys.stderr.write(
            "warning: qrencode not on PATH, skipping QR PNG. Install with `brew install qrencode`.\n"
        )
        return
    subprocess.run(
        ["qrencode", "-l", "M", "-s", "8", "-o", str(out_png), qr_text],
        check=True,
    )


def flash_app(port: str, build_dir: Path) -> None:
    flasher = build_dir / "flasher_args.json"
    if not flasher.is_file():
        fail(f"build artefacts missing at {build_dir}. Run tools/build.sh first.")
    with flasher.open() as f:
        args = json.load(f)
    cmd = [
        "esptool.py",
        "--chip",
        args["extra_esptool_args"]["chip"],
        "--port",
        port,
        "--baud",
        "460800",
        "--before",
        args["extra_esptool_args"]["before"],
        "--after",
        args["extra_esptool_args"]["after"],
        "write_flash",
        *args["write_flash_args"],
    ]
    for offset_str, fname in args["flash_files"].items():
        cmd += [offset_str, str(build_dir / fname)]
    subprocess.run(cmd, check=True)


def flash_factory_nvs(port: str, nvs_bin: Path) -> None:
    subprocess.run(
        [
            "esptool.py",
            "--chip",
            "esp32c3",
            "--port",
            port,
            "--baud",
            "460800",
            "--before",
            "default_reset",
            "--after",
            "hard_reset",
            "write_flash",
            f"0x{FACTORY_NVS_OFFSET:x}",
            str(nvs_bin),
        ],
        check=True,
    )


def main() -> int:
    ap = argparse.ArgumentParser(description="DockPulse per-device factory flash")
    ap.add_argument("--serial", required=True, help="human-readable SN, e.g. DP-N-000123")
    ap.add_argument("--port", default=default_port(), help="serial port (default: first cu.usbmodem*)")
    ap.add_argument("--role", choices=["sensor", "gateway"], default="sensor")
    ap.add_argument("--key", default=str(REPO / "factory/private/factory.pem"))
    ap.add_argument("--out", default=None, help="artefact dir (default: factory/devices/<serial>)")
    ap.add_argument("--exp-days", type=int, default=365)
    ap.add_argument("--nvs-only", action="store_true", help="skip app/bootloader flash")
    ap.add_argument("--no-flash", action="store_true", help="generate artefacts only")
    args = ap.parse_args()

    if not args.port and not args.no_flash:
        fail("no /dev/cu.usbmodem* found. Plug in a board or pass --port, or use --no-flash.")
    priv_path = Path(args.key)
    if not priv_path.is_file():
        fail(f"factory private key missing at {priv_path}. Run tools/factory-keygen.sh.")

    out_dir = Path(args.out) if args.out else REPO / "factory/devices" / args.serial
    out_dir.mkdir(parents=True, exist_ok=True)

    build_dir = REPO / ("build" if args.role == "gateway" else "build_sensor")

    activate_idf()

    if args.no_flash:
        # bench mode: caller supplied a MAC out-of-band via env or we punt
        mac_env = os.environ.get("DP_FACTORY_MAC")
        if not mac_env:
            fail("--no-flash requires DP_FACTORY_MAC=<12 hex> env (board not attached)")
        mac = bytes.fromhex(mac_env.replace(":", ""))
    else:
        mac = read_mac(args.port)

    uuid_hex = compose_uuid(mac)
    oob = secrets.token_bytes(16)
    oob_hex = oob.hex()

    priv_pem = priv_path.read_bytes()
    jwt_token, claim = sign_claim(uuid_hex, args.serial, args.exp_days, priv_pem)
    qr_text = build_qr_payload(uuid_hex, oob_hex, args.serial, jwt_token)

    nvs_bin = out_dir / "factory_nvs.bin"
    gen_nvs_image(oob_hex, nvs_bin)

    qr_png = out_dir / "qr.png"
    render_qr_png(qr_text, qr_png)

    (out_dir / "claim.json").write_text(json.dumps(claim, indent=2) + "\n")
    (out_dir / "qr.txt").write_text(qr_text + "\n")
    (out_dir / "device.json").write_text(
        json.dumps(
            {
                "serial": args.serial,
                "role": args.role,
                "mac": mac.hex(),
                "uuid": uuid_hex,
                "oob_hex": oob_hex,
                "claim_jti": claim["jti"],
                "claim_exp": claim["exp"],
            },
            indent=2,
        )
        + "\n"
    )

    print(f"serial={args.serial} role={args.role} mac={mac.hex()} uuid={uuid_hex}")
    print(f"artefacts in {out_dir}")

    if args.no_flash:
        print("--no-flash set, skipping flash")
        return 0

    if not args.nvs_only:
        flash_app(args.port, build_dir)
    flash_factory_nvs(args.port, nvs_bin)

    print("flash complete")
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except subprocess.CalledProcessError as e:
        fail(f"command failed: {e.cmd}")
    except (binascii.Error, ValueError) as e:
        fail(str(e))
