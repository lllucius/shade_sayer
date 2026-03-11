#!/usr/bin/env python3
"""Collect Kona scan CSV rows from device serial logs.

Expected device lines contain the tag `KONA_SCAN_CSV:` followed by CSV payload.
"""

import argparse
import csv
import datetime as dt
import re
import sys

try:
    import serial
except ImportError:
    print("pyserial is required: pip install pyserial", file=sys.stderr)
    raise

CSV_HEADER = [
    "host_timestamp_iso",
    "swatch_id",
    "swatch_name",
    "led_enabled",
    "gain_code",
    "integration_ms",
    "status2",
    "status6",
    "raw_x",
    "raw_y",
    "raw_z",
    "raw_ir",
    "raw_clear",
    "raw_hgl",
    "raw_hgh",
    "xyz_x",
    "xyz_y",
    "xyz_z",
    "lab_l",
    "lab_a",
    "lab_b",
    "timestamp_us",
]

PATTERN = re.compile(r"KONA_SCAN_CSV:\s*(.*)$")


def parse_args() -> argparse.Namespace:
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--port", required=True, help="Serial port (e.g. /dev/ttyACM0)")
    p.add_argument("--baud", type=int, default=115200)
    p.add_argument("--output", default="kona_raw_captures.csv")
    return p.parse_args()


def main() -> int:
    args = parse_args()
    with serial.Serial(args.port, args.baud, timeout=1) as ser, open(args.output, "a", newline="") as f:
        writer = csv.writer(f)
        if f.tell() == 0:
            writer.writerow(CSV_HEADER)
        print(f"Listening on {args.port} @ {args.baud}, writing to {args.output}")
        while True:
            line = ser.readline().decode("utf-8", errors="replace").strip()
            if not line:
                continue
            m = PATTERN.search(line)
            if not m:
                continue
            payload = [x.strip() for x in m.group(1).split(",")]
            if len(payload) != len(CSV_HEADER) - 1:
                print(f"Skipping malformed payload ({len(payload)} fields): {line}", file=sys.stderr)
                continue
            writer.writerow([dt.datetime.now(dt.timezone.utc).isoformat()] + payload)
            f.flush()
            print(f"Captured {payload[0]} {payload[1]} @ gain {payload[3]} int {payload[4]}ms")


if __name__ == "__main__":
    raise SystemExit(main())
