#!/usr/bin/env python3
import re
from pathlib import Path

src = Path('calibration_run.txt')
out_xyz = Path('host/calibration_measurements.cfg')
out_raw = Path('host/calibration_measurements_raw.cfg')

pat_captured = re.compile(r"Captured (.+?) \(\d+/\d+\): XYZ\(([-\d.]+), ([-\d.]+), ([-\d.]+)\)")
pat_place = re.compile(r"Place (.+?) reference and press button")
pat_raw = re.compile(r"Raw: X=(\d+) Y=(\d+) Z=(\d+) \(gain=([\d.]+)x, int=(\d+)ms\)")

xyz_lines = []
raw_lines = []
current_ref = None

for line in src.read_text().splitlines():
    m = pat_captured.search(line)
    if m:
        xyz_lines.append(f"{m.group(1)}|{m.group(2)}|{m.group(3)}|{m.group(4)}")
        continue

    m = pat_place.search(line)
    if m:
        current_ref = m.group(1)
        continue

    m = pat_raw.search(line)
    if m and current_ref:
        raw_lines.append(f"{current_ref}|{m.group(1)}|{m.group(2)}|{m.group(3)}|{m.group(4)}|{m.group(5)}")
        current_ref = None

out_xyz.write_text("\n".join(xyz_lines) + "\n")
out_raw.write_text("\n".join(raw_lines) + "\n")
print(f"wrote {len(xyz_lines)} XYZ entries to {out_xyz}")
print(f"wrote {len(raw_lines)} RAW entries to {out_raw}")
