#!/usr/bin/env python3
"""Collect Kona scan CSV rows from a console log text file.

Expected device lines contain the tag `KONA_SCAN_CSV:` followed by CSV payload.
If `kona_365_sensor_ready.csv` metadata is available, `idx_###` placeholders are
resolved to real swatch identifiers and names.
"""

import argparse
import csv
import datetime as dt
import os
import re
import pathlib
import sys
from typing import Dict, List, Optional

CSV_HEADER = [
    "host_timestamp_iso",
    "panel",
    "panel_index",
    "kona_id",
    "kona_name",
    "led_enabled",
    "gain_code",
    "integration_ms",
    "status2",
    "status6",
    "requested_samples",
    "accepted_samples",
    "rejected_saturated",
    "rejected_low_signal",
    "mean_xyz_x",
    "mean_xyz_y",
    "mean_xyz_z",
    "stddev_xyz_x",
    "stddev_xyz_y",
    "stddev_xyz_z",
    "mean_lab_l",
    "mean_lab_a",
    "mean_lab_b",
    "timestamp_us",
]

PATTERN = re.compile(r"KONA_SCAN_CSV:\s*(.*)$")
IDX_PATTERN = re.compile(r"^idx_(\d{3})$")


def parse_args() -> argparse.Namespace:
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument(
        "--input-log",
        default="console.txt",
        help="Path to captured console log text file (or '-' for stdin)",
    )
    p.add_argument("--output", default="kona_avg_captures.csv")
    p.add_argument(
        "--metadata",
        default="kona_365_sensor_ready.csv",
        help="Path to Kona swatch metadata CSV (optional)",
    )
    return p.parse_args()


def load_metadata(path: str) -> Optional[List[Dict[str, str]]]:
    if not os.path.exists(path):
        print(f"Metadata CSV not found: {path} (continuing with idx placeholders)")
        return None

    with open(path, newline="") as f:
        rows = list(csv.DictReader(f))

    print(f"Loaded {len(rows)} metadata rows from {path}")
    return rows


def resolve_metadata(swatch_id: str, swatch_name: str, metadata: Optional[List[Dict[str, str]]]):
    panel = ""
    panel_index = ""
    kona_id = swatch_id
    kona_name = swatch_name

    if metadata:
        m = IDX_PATTERN.match(swatch_id)
        if m:
            idx_1based = int(m.group(1))
            if 1 <= idx_1based <= len(metadata):
                row = metadata[idx_1based - 1]
                panel = row.get("panel", "")
                panel_index = row.get("panel_index", "")
                kona_id = row.get("id", swatch_id)
                kona_name = row.get("name", swatch_name)

    return panel, panel_index, kona_id, kona_name


def main() -> int:
    args = parse_args()
    metadata = load_metadata(args.metadata)

    with open(args.output, "a", newline="") as f:
        writer = csv.writer(f)
        if f.tell() == 0:
            writer.writerow(CSV_HEADER)
        print(f"Reading log lines from {args.input_log}, writing to {args.output}")

        if args.input_log == "-":
            line_iter = sys.stdin
        else:
            log_path = pathlib.Path(args.input_log)
            if not log_path.exists():
                print(f"Input log not found: {log_path}", file=sys.stderr)
                return 1
            line_iter = log_path.open("r", encoding="utf-8", errors="replace")

        for raw_line in line_iter:
            line = raw_line.strip()
            if not line:
                continue

            m = PATTERN.search(line)
            if not m:
                continue

            payload = [x.strip() for x in m.group(1).split(",")]
            # Device payload: swatch_id, swatch_name + 19 averaged/stat fields = 21
            if len(payload) != 21:
                print(f"Skipping malformed payload ({len(payload)} fields): {line}", file=sys.stderr)
                continue

            swatch_id, swatch_name = payload[0], payload[1]
            panel, panel_index, kona_id, kona_name = resolve_metadata(swatch_id, swatch_name, metadata)

            out_row = [
                dt.datetime.now(dt.timezone.utc).isoformat(),
                panel,
                panel_index,
                kona_id,
                kona_name,
            ] + payload[2:]

            writer.writerow(out_row)
            f.flush()
            print(f"Captured {kona_id} {kona_name} (panel={panel} idx={panel_index})")

        if args.input_log != "-":
            line_iter.close()

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
