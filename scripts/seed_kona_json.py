#!/usr/bin/env python3
"""
seed_kona_json.py — Seed kona_captures.json with all swatches from kona_cotton_solids_k001.csv.

This utility reads the CSV file that contains the master list of Kona Cotton solid swatches
and merges them into kona_captures.json, preserving any existing JSON metadata
(schema_version, device, pipeline_config_snapshot) and any already-captured raw sensor data.

Usage:
    python3 scripts/seed_kona_json.py [--csv <path>] [--json <path>]

Defaults:
    --csv   ../kona_cotton_solids_k001.csv   (relative to this script)
    --json  ../kona_captures.json            (relative to this script)
"""

import argparse
import csv
import json
import os
import sys


def load_csv(csv_path):
    """Read all swatch rows from the CSV file and return a list of dicts."""
    swatches = []
    with open(csv_path, newline="", encoding="utf-8") as f:
        reader = csv.DictReader(f)
        for row in reader:
            measured = row.get("measured", "false").strip().lower() == "true"

            lab = None
            rgb = None

            if measured:
                try:
                    l_val = float(row["L"]) if row.get("L", "").strip() else None
                    a_val = float(row["a"]) if row.get("a", "").strip() else None
                    b_val = float(row["b"]) if row.get("b", "").strip() else None
                    if l_val is not None and a_val is not None and b_val is not None:
                        lab = {"l": l_val, "a": a_val, "b": b_val}
                except (ValueError, KeyError) as e:
                    print(f"  Warning: could not parse Lab for id={row.get('id')}: {e}", file=sys.stderr)

                try:
                    r_val = int(row["R"]) if row.get("R", "").strip() else None
                    g_val = int(row["G"]) if row.get("G", "").strip() else None
                    b_rgb = int(row["B"]) if row.get("B", "").strip() else None
                    if r_val is not None and g_val is not None and b_rgb is not None:
                        rgb = {"r": r_val, "g": g_val, "b": b_rgb}
                except (ValueError, KeyError) as e:
                    print(f"  Warning: could not parse RGB for id={row.get('id')}: {e}", file=sys.stderr)

            swatches.append(
                {
                    "panel": row.get("panel", "").strip(),
                    "panel_index": int(row.get("panel_index", 0)),
                    "id": int(row.get("id", 0)),
                    "name": row.get("name", "").strip(),
                    "measured": measured,
                    "lab": lab,
                    "rgb": rgb,
                    "raw": None,
                    "notes": row.get("notes", "").strip(),
                }
            )
    return swatches


def load_json(json_path):
    """Load the existing JSON file, or return a fresh template if it doesn't exist."""
    if os.path.exists(json_path):
        with open(json_path, encoding="utf-8") as f:
            return json.load(f)
    return {
        "schema_version": 1,
        "capture_date": "",
        "device": {"firmware_version": "", "firmware_commit": ""},
        "pipeline_config_snapshot": {},
        "swatches": [],
    }


def merge_swatches(csv_swatches, existing_swatches):
    """
    Merge CSV swatches with existing JSON swatches.

    For each CSV swatch:
    - If an existing JSON entry has the same id, preserve its raw/lab/rgb/notes
      (but update panel/panel_index/name from the CSV master list).
    - Otherwise, use the CSV entry as-is.
    """
    existing_by_id = {s["id"]: s for s in existing_swatches if "id" in s}

    merged = []
    for csv_entry in csv_swatches:
        swatch_id = csv_entry["id"]
        if swatch_id in existing_by_id:
            existing = existing_by_id[swatch_id]
            merged.append(
                {
                    "panel": csv_entry["panel"],
                    "panel_index": csv_entry["panel_index"],
                    "id": swatch_id,
                    "name": csv_entry["name"],
                    "measured": existing.get("measured", csv_entry["measured"]),
                    "lab": existing.get("lab", csv_entry["lab"]),
                    "rgb": existing.get("rgb", csv_entry["rgb"]),
                    "raw": existing.get("raw", csv_entry["raw"]),
                    "notes": existing.get("notes", csv_entry["notes"]),
                }
            )
        else:
            merged.append(csv_entry)

    return merged


def main():
    script_dir = os.path.dirname(os.path.abspath(__file__))
    default_csv = os.path.join(script_dir, "..", "kona_cotton_solids_k001.csv")
    default_json = os.path.join(script_dir, "..", "kona_captures.json")

    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--csv", default=default_csv, help="Path to kona_cotton_solids_k001.csv")
    parser.add_argument("--json", default=default_json, help="Path to kona_captures.json")
    args = parser.parse_args()

    csv_path = os.path.realpath(args.csv)
    json_path = os.path.realpath(args.json)

    if not os.path.exists(csv_path):
        print(f"ERROR: CSV file not found: {csv_path}", file=sys.stderr)
        sys.exit(1)

    print(f"Reading swatches from: {csv_path}")
    csv_swatches = load_csv(csv_path)
    print(f"  Loaded {len(csv_swatches)} swatches from CSV")

    print(f"Loading existing JSON from: {json_path}")
    data = load_json(json_path)
    existing_count = len(data.get("swatches", []))
    print(f"  Found {existing_count} existing swatches in JSON")

    merged = merge_swatches(csv_swatches, data.get("swatches", []))
    data["swatches"] = merged

    with open(json_path, "w", encoding="utf-8") as f:
        json.dump(data, f, indent=2, ensure_ascii=False)
        f.write("\n")

    measured = sum(1 for s in merged if s.get("measured"))
    print(f"Wrote {len(merged)} swatches to: {json_path}")
    print(f"  Measured: {measured}, Unmeasured: {len(merged) - measured}")


if __name__ == "__main__":
    main()
