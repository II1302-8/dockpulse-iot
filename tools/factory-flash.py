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

# share helpers with verify-device.py and print-qr.py
sys.path.insert(0, str(Path(__file__).resolve().parent))
from factory_flash_helpers import (  # noqa: E402
    EXPIRY_WARN_DAYS,
    activate_idf,
    compose_uuid,
    days_until,
    default_port,
    fail,
    read_mac,
)

REPO = Path(__file__).resolve().parent.parent

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


def b64u(data: bytes) -> str:
    return base64.urlsafe_b64encode(data).rstrip(b"=").decode()


def assert_path_gitignored(path: Path, what: str) -> None:
    """Refuse to read/write secrets at a path git wouldn't ignore.

    Belt-and-braces: the repo ships a .gitignore for factory/private/ and
    factory/devices/ but a future contributor might rearrange paths. Run
    from a git checkout and ask git directly — the source of truth.
    """
    try:
        out = subprocess.run(
            ["git", "check-ignore", "-q", str(path)],
            cwd=REPO,
            check=False,
        )
    except FileNotFoundError:
        # not a git checkout (zip download, etc), let the user decide
        sys.stderr.write(
            f"factory-flash: warning, git not available, can't verify "
            f"{path} is gitignored\n"
        )
        return
    if out.returncode != 0:
        fail(
            f"refusing to use {what} at {path}: not in .gitignore "
            "(would be committed on next git add). Add the parent dir to "
            ".gitignore first."
        )


def sign_claim(
    uuid_hex: str, oob_hex: str, serial: str, exp_days: int, priv_pem: bytes
) -> tuple[str, dict]:
    # avoid pyjwt dep — IDF's pyenv already has cryptography
    from cryptography.hazmat.primitives.serialization import load_pem_private_key

    priv = load_pem_private_key(priv_pem, password=None)
    now = int(time.time())
    # both uuid and oob in the signed payload, tampering invalidates sig
    claim = {
        "iss": "factory",
        "sub": serial,
        "uuid": uuid_hex,
        "oob": oob_hex,
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


def build_qr_payload(serial: str, jwt_token: str) -> str:
    # v=2 minimal envelope, uuid + oob live in the JWT.
    # sn kept for human-readable logging before signature verify
    qr = {"v": 2, "sn": serial, "jwt": jwt_token}
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


def warn_if_expiring(claim_exp: int, label: str = "JWT") -> None:
    days_left = days_until(claim_exp)
    if days_left <= 0:
        sys.stderr.write(f"factory-flash: warning, {label} EXPIRED {-days_left:.0f}d ago\n")
    elif days_left <= EXPIRY_WARN_DAYS:
        sys.stderr.write(
            f"factory-flash: warning, {label} expires in {days_left:.0f}d "
            f"(< {EXPIRY_WARN_DAYS}d). Consider --force to re-roll before shipping.\n"
        )


def main() -> int:
    ap = argparse.ArgumentParser(description="DockPulse per-device factory flash")
    ap.add_argument("--serial", required=True, help="human-readable SN, e.g. DP-N-000123")
    ap.add_argument("--port", default=None, help="serial port (default: first attached usb-serial)")
    ap.add_argument("--role", choices=["sensor", "gateway"], default="sensor")
    ap.add_argument("--key", default=str(REPO / "factory/private/factory.pem"))
    ap.add_argument("--out", default=None, help="artefact dir (default: factory/devices/<serial>)")
    ap.add_argument("--exp-days", type=int, default=365)
    ap.add_argument("--nvs-only", action="store_true", help="skip app/bootloader flash")
    ap.add_argument("--no-flash", action="store_true", help="generate artefacts only")
    ap.add_argument(
        "--force",
        action="store_true",
        help="overwrite existing device artefacts. invalidates the previously printed sticker.",
    )
    ap.add_argument(
        "--reflash-nvs",
        action="store_true",
        help="re-flash factory_nvs.bin from existing artefacts without regenerating "
        "OOB or JWT. recovers a board after esptool erase_flash wiped factory_nvs.",
    )
    args = ap.parse_args()

    # resolve port lazily so plugging in after script start still works
    port = args.port or default_port()
    if not port and not args.no_flash:
        fail("no usb-serial device found. Plug in a board or pass --port, or use --no-flash.")
    priv_path = Path(args.key)
    if not args.reflash_nvs and not priv_path.is_file():
        fail(f"factory private key missing at {priv_path}. Run tools/factory-keygen.sh.")
    if not args.reflash_nvs:
        assert_path_gitignored(priv_path, "private key")

    out_dir = Path(args.out) if args.out else REPO / "factory/devices" / args.serial
    out_dir.mkdir(parents=True, exist_ok=True)
    assert_path_gitignored(out_dir, "per-device artefact dir")

    build_dir = REPO / ("build" if args.role == "gateway" else "build_sensor")
    device_json = out_dir / "device.json"
    nvs_bin = out_dir / "factory_nvs.bin"

    activate_idf()

    # write existing nvs blob without regen, sticker JWT + OOB stay valid
    if args.reflash_nvs:
        if not nvs_bin.is_file() or not device_json.is_file():
            fail(
                f"--reflash-nvs needs existing artefacts in {out_dir}. "
                "Run a regular factory-flash first."
            )
        existing = json.loads(device_json.read_text())
        warn_if_expiring(existing["claim_exp"])
        flash_factory_nvs(port, nvs_bin)
        print(f"reflashed factory_nvs for {args.serial} from {nvs_bin}")
        return 0

    # refuse to clobber an existing flashed device without --force
    if device_json.is_file() and not args.force:
        existing = json.loads(device_json.read_text())
        days_left = days_until(existing["claim_exp"])
        msg = (
            f"{args.serial} already factory-flashed (uuid={existing['uuid']}, "
            f"jwt valid {days_left:+.0f}d).\n"
            "  --force          regenerate OOB+JWT, invalidates current sticker\n"
            "  --reflash-nvs    re-flash existing OOB to device (recovery)\n"
            "  tools/print-qr.py --serial " + args.serial + "  reprint sticker"
        )
        warn_if_expiring(existing["claim_exp"])
        fail(msg)

    if args.no_flash:
        # bench mode: caller supplied a MAC out-of-band via env or we punt
        mac_env = os.environ.get("DP_FACTORY_MAC")
        if not mac_env:
            fail("--no-flash requires DP_FACTORY_MAC=<12 hex> env (board not attached)")
        mac = bytes.fromhex(mac_env.replace(":", ""))
    else:
        mac = read_mac(port)

    uuid_hex = compose_uuid(mac)
    oob = secrets.token_bytes(16)
    oob_hex = oob.hex()

    priv_pem = priv_path.read_bytes()
    jwt_token, claim = sign_claim(
        uuid_hex, oob_hex, args.serial, args.exp_days, priv_pem
    )
    qr_text = build_qr_payload(args.serial, jwt_token)
    warn_if_expiring(claim["exp"], label=f"new JWT (--exp-days={args.exp_days})")

    gen_nvs_image(oob_hex, nvs_bin)

    qr_png = out_dir / "qr.png"
    render_qr_png(qr_text, qr_png)

    (out_dir / "claim.json").write_text(json.dumps(claim, indent=2) + "\n")
    (out_dir / "qr.txt").write_text(qr_text + "\n")
    device_json.write_text(
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
        flash_app(port, build_dir)
    flash_factory_nvs(port, nvs_bin)

    print("flash complete")
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except subprocess.CalledProcessError as e:
        fail(f"command failed: {e.cmd}")
    except (binascii.Error, ValueError) as e:
        fail(str(e))
