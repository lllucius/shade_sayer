#!/usr/bin/env python3
"""Regenerate stored scan-space Lab values in kona_captures.json from raw sensor data.

Reads the representative raw sensor readings stored in kona_captures.json,
passes each one through the host build's color pipeline (kona_regenerate
binary), and writes the updated scan-space Lab values back to the JSON file.

Run this after any change to the color pipeline (PCCM coefficients, responsivity
constants, IR compensation, lightness correction, etc.) to update the cached
scan-space Lab values without requiring a physical rescan of all 365 swatches.

Note: the stored RAWDATA values are representative capture data from the scan,
not a byte-for-byte serialization of every accepted sample. Replay therefore
tracks the captured scan closely, but it is still an approximation for scans
that used averaging or retry logic in firmware.

Workflow:
    1.  Edit pipeline parameters in main/color_pipeline.cpp or calibration headers.
    2.  Run this script: python3 regenerate_kona_lab.py
    3.  Review the summary of Lab changes printed to stdout.
    4.  Commit the updated kona_captures.json.
    5.  Normal builds pick up the new Lab values automatically.

Requirements:
    - CMake and a C++ compiler (for building the kona_regenerate binary)
    - kona_captures.json with at least some swatches containing raw sensor data

Usage:
    python3 regenerate_kona_lab.py [--json ../kona_captures.json] [--host-build ../host/build]
    python3 regenerate_kona_lab.py --dry-run   # Show what would change without writing
"""

import argparse
import json
import math
import os
import pathlib
import subprocess
import sys
import tempfile
from typing import Optional


def _repo_root() -> pathlib.Path:
    """Return the repository root (parent of the scripts directory)."""
    return pathlib.Path(__file__).resolve().parent.parent


def _default_json_path() -> pathlib.Path:
    return _repo_root() / "kona_captures.json"


def _default_host_build() -> pathlib.Path:
    return _repo_root() / "host" / "build"


def build_host_binary(host_build: pathlib.Path) -> pathlib.Path:
    """Build the kona_regenerate binary if necessary.

    Runs cmake and then cmake --build targeting the kona_regenerate executable.
    Returns the path to the built binary.
    """
    host_src = host_build.parent  # host/ directory
    host_build.mkdir(parents=True, exist_ok=True)

    binary = host_build / "kona_regenerate"

    # Configure if CMakeCache.txt is absent (first run)
    cmake_cache = host_build / "CMakeCache.txt"
    if not cmake_cache.exists():
        print(f"Configuring host build in {host_build} ...")
        result = subprocess.run(
            ["cmake", str(host_src)],
            cwd=host_build,
            capture_output=False,
        )
        if result.returncode != 0:
            raise RuntimeError(f"cmake configure failed (exit {result.returncode})")

    # Build only the kona_regenerate target
    print(f"Building kona_regenerate ...")
    result = subprocess.run(
        ["cmake", "--build", ".", "--target", "kona_regenerate"],
        cwd=host_build,
        capture_output=False,
    )
    if result.returncode != 0:
        raise RuntimeError(f"cmake build failed (exit {result.returncode})")

    if not binary.exists():
        raise RuntimeError(f"Build succeeded but binary not found: {binary}")

    return binary


def _delta_e_approx(lab1: tuple, lab2: tuple) -> float:
    """Quick Euclidean delta-E for summary reporting (not CIEDE2000)."""
    return math.sqrt(sum((a - b) ** 2 for a, b in zip(lab1, lab2)))


def regenerate(json_path: pathlib.Path, binary: pathlib.Path, dry_run: bool = False) -> int:
    """Run the regeneration pipeline.

    Feeds swatches with raw sensor data through the kona_regenerate binary and
    updates the stored scan-space Lab values in the JSON file.

    Returns the number of swatches updated.
    """
    with json_path.open(encoding="utf-8") as f:
        data = json.load(f)

    swatches = data.get("swatches", [])

    # Build the input for the binary: one line per swatch with raw data
    eligible = []
    for sw in swatches:
        if not sw.get("measured", False):
            continue
        raw = sw.get("raw") or {}
        if any(raw.get(k) is None for k in ("x", "y", "z", "ir", "clear", "gain", "integration_ms")):
            continue
        eligible.append(sw)

    if not eligible:
        print("No swatches with raw sensor data found. Nothing to regenerate.")
        print("Capture raw data via the GUI (when the device protocol is extended to "
              "return raw sensor values) or via the firmware kona scan mode.")
        return 0

    print(f"Processing {len(eligible)} swatches with raw data ...")

    # Build stdin payload for the binary
    lines = []
    for sw in eligible:
        raw = sw["raw"]
        led = 1  # Assume LED-illuminated captures
        lines.append(
            f"{sw['id']} {raw['x']} {raw['y']} {raw['z']} "
            f"{raw['ir']} {raw['clear']} {raw['gain']} {raw['integration_ms']} {led}"
        )
    stdin_data = "\n".join(lines) + "\n"

    # Run the binary
    result = subprocess.run(
        [str(binary)],
        input=stdin_data,
        capture_output=True,
        text=True,
    )
    if result.returncode != 0:
        print(f"kona_regenerate exited with {result.returncode}", file=sys.stderr)
        if result.stderr:
            print(result.stderr, file=sys.stderr)
        return 0

    if result.stderr:
        print(result.stderr, end="", file=sys.stderr)

    # Parse output: "OK <id> <l> <a> <b>" or "ERR <id>"
    new_lab: dict = {}
    errors = []
    for line in result.stdout.splitlines():
        parts = line.split()
        if not parts:
            continue
        if parts[0] == "OK" and len(parts) == 5:
            sid = int(parts[1])
            new_lab[sid] = (float(parts[2]), float(parts[3]), float(parts[4]))
        elif parts[0] == "ERR" and len(parts) >= 2:
            errors.append(int(parts[1]))

    if errors:
        print(f"Pipeline errors for swatch IDs: {errors}", file=sys.stderr)

    # Update swatches and compute deltas for summary
    updated = 0
    max_delta = 0.0
    max_delta_name = ""

    # Build a lookup by swatch id for fast access
    by_id = {sw["id"]: sw for sw in swatches}

    for sid, (l, a, b) in new_lab.items():
        sw = by_id.get(sid)
        if sw is None:
            continue

        old_lab = sw.get("lab") or {}
        old_l = old_lab.get("l")
        old_a = old_lab.get("a")
        old_b = old_lab.get("b")

        if old_l is not None and old_a is not None and old_b is not None:
            de = _delta_e_approx((old_l, old_a, old_b), (l, a, b))
            if de > max_delta:
                max_delta = de
                max_delta_name = sw.get("name", str(sid))
            if de > 0.01:
                print(f"  {sw.get('name', sid):20s} (id={sid:3d}): "
                      f"L={old_l:.2f}→{l:.2f}  a={old_a:.2f}→{a:.2f}  b={old_b:.2f}→{b:.2f}  "
                      f"ΔE≈{de:.2f}")
        else:
            print(f"  {sw.get('name', sid):20s} (id={sid:3d}): new Lab "
                  f"L={l:.2f}  a={a:.2f}  b={b:.2f}")

        if not dry_run:
            if sw.get("lab") is None:
                sw["lab"] = {}
            sw["lab"]["l"] = round(l, 6)
            sw["lab"]["a"] = round(a, 6)
            sw["lab"]["b"] = round(b, 6)
            sw["measured"] = True
        updated += 1

    print(f"\nSummary: {updated} swatches updated, max ΔE ≈ {max_delta:.2f} ({max_delta_name})")

    if dry_run:
        print("Dry-run mode: no changes written to disk.")
        return updated

    # Write updated JSON back to file
    with json_path.open("w", encoding="utf-8") as f:
        json.dump(data, f, indent=2)
        f.write("\n")

    print(f"Updated {json_path}")
    return updated


def parse_args() -> argparse.Namespace:
    p = argparse.ArgumentParser(description=__doc__,
                                formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("--json", default=str(_default_json_path()), type=pathlib.Path,
                   help="Path to kona_captures.json (default: %(default)s)")
    p.add_argument("--host-build", default=str(_default_host_build()), type=pathlib.Path,
                   dest="host_build",
                   help="Host build directory (default: %(default)s)")
    p.add_argument("--binary", default=None, type=pathlib.Path,
                   help="Path to pre-built kona_regenerate binary (skips cmake build step)")
    p.add_argument("--dry-run", action="store_true",
                   help="Show changes without writing to disk")
    return p.parse_args()


def main() -> int:
    args = parse_args()

    if not args.json.exists():
        print(f"Error: JSON file not found: {args.json}", file=sys.stderr)
        return 1

    if args.binary is not None:
        binary = args.binary
        if not binary.exists():
            print(f"Error: specified binary not found: {binary}", file=sys.stderr)
            return 1
    else:
        try:
            binary = build_host_binary(args.host_build)
        except RuntimeError as e:
            print(f"Error: {e}", file=sys.stderr)
            return 1

    try:
        updated = regenerate(args.json, binary, dry_run=args.dry_run)
    except Exception as e:
        print(f"Error during regeneration: {e}", file=sys.stderr)
        return 1

    return 0 if updated >= 0 else 1


if __name__ == "__main__":
    raise SystemExit(main())
