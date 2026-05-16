#!/usr/bin/env python3
"""Per-device factory flash for DockPulse sensor and gateway nodes.

Reads the device MAC, composes a mesh UUID matching dp_prov_get_dev_uuid
(MAC + "DOCKPULSE" + 0x01), generates a random 16-byte static OOB, signs
a COSE_Sign1 claim (Ed25519) with the factory key, builds an NVS
partition image holding the OOB, flashes bootloader + ptable + app +
factory_nvs over USB, and renders a base45-encoded QR PNG that fits on
a 25mm sticker. After flashing the board, the script PUTs uuid+oob to
the backend's /api/admin/factory-devices/{serial} so adoption can look
them up by serial (the QR no longer carries them).

Run after `tools/build.sh` for the role you want, with the board
attached:

    tools/factory-flash.py --serial DP-N-000123
    tools/factory-flash.py --serial DP-N-000123 --role gateway
    tools/factory-flash.py --serial DP-N-000123 --nvs-only   # rotate OOB on already-flashed board

Prereqs:
- ESP-IDF env activated (we activate it ourselves if you have $IDF_PATH set)
- `tools/factory-keygen.sh` already ran once on this host
- `qrencode` on $PATH for the sticker PNG (brew install qrencode)
- `cryptography`, `cbor2`, `base45` Python packages (pip install if missing)
"""
from __future__ import annotations

import argparse
import binascii
import json
import os
import secrets
import shutil
import subprocess
import sys
import time
import urllib.error
import urllib.request
import uuid as _uuid
from pathlib import Path

try:
    import base45
    import cbor2
except ImportError:
    sys.stderr.write(
        "factory-flash: missing dep. pip install cbor2 base45 (both small, no native code).\n"
    )
    sys.exit(1)

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


# COSE constants — keep in sync with backend/app/adoption/cose.py
_COSE_TAG_SIGN1 = 18
_COSE_HDR_ALG = 1
_COSE_ALG_EDDSA = -8


def _sig_structure(protected: bytes, payload: bytes) -> bytes:
    return cbor2.dumps(["Signature1", protected, b"", payload])


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
    serial: str, exp_days: int, priv_pem: bytes
) -> tuple[str, dict]:
    """Build a base45 COSE_Sign1 (Ed25519) claim.

    Returns (qr_text, claim_meta). claim_meta carries jti+exp so the
    caller can write device.json + POST to the backend; uuid + oob are
    NOT signed any more, the backend resolves them by serial.
    """
    from cryptography.hazmat.primitives.serialization import load_pem_private_key

    priv = load_pem_private_key(priv_pem, password=None)
    now = int(time.time())
    exp = now + exp_days * 86400
    jti = _uuid.uuid4()
    # compact CBOR payload: integer keys keep the QR tiny.
    # 1=serial, 2=jti(16 raw bytes), 3=exp(unix int)
    payload = cbor2.dumps({1: serial, 2: jti.bytes, 3: exp})
    protected = cbor2.dumps({_COSE_HDR_ALG: _COSE_ALG_EDDSA})
    signature = priv.sign(_sig_structure(protected, payload))
    cose_blob = cbor2.dumps(
        cbor2.CBORTag(_COSE_TAG_SIGN1, [protected, {}, payload, signature])
    )
    qr_text = base45.b45encode(cose_blob).decode()
    return qr_text, {"jti": str(jti), "iat": now, "exp": exp}


def register_with_backend(
    *,
    backend_url: str,
    serial: str,
    uuid_hex: str,
    oob_hex: str,
    claim_jti: str,
    claim_exp: int,
    cf_token: str | None,
) -> None:
    """PUT device row to backend so adoption can resolve serial->uuid+oob.

    Cloudflare Access service token via CF-Access-Client-Id/Secret headers
    (operator sets DP_CF_ACCESS_CLIENT_ID and DP_CF_ACCESS_CLIENT_SECRET);
    raw bearer also accepted via --backend-token if cf access is open.
    """
    url = backend_url.rstrip("/") + f"/api/admin/factory-devices/{serial}"
    body = json.dumps(
        {
            "serial_number": serial,
            "mesh_uuid": uuid_hex,
            "oob_hex": oob_hex,
            "claim_jti": claim_jti,
            "claim_exp": claim_exp,
        }
    ).encode()
    headers = {"content-type": "application/json"}
    cf_id = os.environ.get("DP_CF_ACCESS_CLIENT_ID")
    cf_secret = os.environ.get("DP_CF_ACCESS_CLIENT_SECRET")
    if cf_id and cf_secret:
        headers["CF-Access-Client-Id"] = cf_id
        headers["CF-Access-Client-Secret"] = cf_secret
    if cf_token:
        headers["authorization"] = f"Bearer {cf_token}"
    req = urllib.request.Request(url, data=body, headers=headers, method="PUT")
    try:
        with urllib.request.urlopen(req, timeout=15) as resp:
            sys.stderr.write(
                f"factory-flash: registered {serial} at backend ({resp.status})\n"
            )
    except urllib.error.HTTPError as err:
        fail(f"backend register failed: {err.code} {err.reason} body={err.read()!r}")
    except urllib.error.URLError as err:
        fail(f"backend register failed: {err.reason}")


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


def erase_chip(port: str) -> None:
    # full chip erase wipes any stale factory_nvs / mesh prov state from prior runs
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
            "erase_flash",
        ],
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
        "--no-erase",
        action="store_true",
        help="skip full chip erase before flashing. default erases to wipe stale "
        "NVS / mesh prov state. ignored for --nvs-only and --reflash-nvs.",
    )
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
    ap.add_argument(
        "--backend-url",
        default=os.environ.get("DP_BACKEND_URL"),
        help="backend base url (default $DP_BACKEND_URL). when set, registers "
        "uuid+oob via PUT /api/admin/factory-devices/{serial} so adoption can "
        "resolve them by serial.",
    )
    ap.add_argument(
        "--backend-token",
        default=os.environ.get("DP_BACKEND_TOKEN"),
        help="optional bearer token for the backend admin endpoint when CF "
        "Access service-token env vars aren't set",
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
    qr_text, claim = sign_claim(args.serial, args.exp_days, priv_pem)
    warn_if_expiring(claim["exp"], label=f"new claim (--exp-days={args.exp_days})")

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
        if args.backend_url:
            register_with_backend(
                backend_url=args.backend_url,
                serial=args.serial,
                uuid_hex=uuid_hex,
                oob_hex=oob_hex,
                claim_jti=claim["jti"],
                claim_exp=claim["exp"],
                cf_token=args.backend_token,
            )
        return 0

    if not args.nvs_only:
        if not args.no_erase:
            erase_chip(port)
        flash_app(port, build_dir)
    flash_factory_nvs(port, nvs_bin)

    if args.backend_url:
        register_with_backend(
            backend_url=args.backend_url,
            serial=args.serial,
            uuid_hex=uuid_hex,
            oob_hex=oob_hex,
            claim_jti=claim["jti"],
            claim_exp=claim["exp"],
            cf_token=args.backend_token,
        )
    else:
        print(
            "register manually: curl -X PUT $DP_BACKEND_URL/api/admin/factory-devices/"
            f"{args.serial} -H 'CF-Access-Client-Id: ...' "
            "-H 'CF-Access-Client-Secret: ...' "
            f"-d '{{\"serial_number\":\"{args.serial}\",\"mesh_uuid\":\"{uuid_hex}\","
            f"\"oob_hex\":\"{oob_hex}\",\"claim_jti\":\"{claim['jti']}\","
            f"\"claim_exp\":{claim['exp']}}}'"
        )

    print("flash complete")
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except subprocess.CalledProcessError as e:
        fail(f"command failed: {e.cmd}")
    except (binascii.Error, ValueError) as e:
        fail(str(e))
