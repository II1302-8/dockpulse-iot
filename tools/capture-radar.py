#!/usr/bin/env python3
"""Stream RADAR,... lines from a sensor's serial port into a clean CSV.

Bypasses idf.py monitor (no ANSI escapes, no chip reset on connect).
Extracts only the RADAR,... portion of each log line so the output is
directly importable with pandas.read_csv(...).

Usage:
    tools/capture-radar.py /dev/cu.usbmodem2101 [-o field_test.csv]

Stops on Ctrl-C.
"""

import argparse
import re
import sys
import time

try:
    import serial
except ImportError:
    sys.exit("pyserial not installed — `pip install pyserial` or activate ESP-IDF.")


HEADER = (
    "ts_ms,presence,distance_cm,"
    + ",".join(f"gate{i}" for i in range(16))
)

# Match RADAR,... up to end of line (after stripping ANSI). The leading
# log prefix `I (12345) sensor: ` is dropped by anchoring on RADAR.
PATTERN = re.compile(rb"RADAR,[0-9,-]+")


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("port", help="serial device, e.g. /dev/cu.usbmodem2101")
    ap.add_argument("-b", "--baud", type=int, default=115200)
    ap.add_argument(
        "-o", "--output", default="field_test.csv",
        help="output CSV path (default: field_test.csv)",
    )
    ap.add_argument(
        "--append", action="store_true",
        help="append to output instead of overwriting",
    )
    args = ap.parse_args()

    mode = "ab" if args.append else "wb"
    with serial.Serial(args.port, args.baud, timeout=1) as ser, \
         open(args.output, mode) as out:
        if not args.append:
            out.write((HEADER + "\n").encode())
            out.flush()

        sys.stderr.write(f"capturing {args.port} → {args.output} (Ctrl-C to stop)\n")
        try:
            while True:
                line = ser.readline()
                m = PATTERN.search(line)
                if not m:
                    continue
                # Drop the literal "RADAR," prefix; keep the comma-sep values.
                row = m.group(0)[len(b"RADAR,"):]
                out.write(row + b"\n")
                out.flush()
        except KeyboardInterrupt:
            sys.stderr.write("\nstopped\n")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
