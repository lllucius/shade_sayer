#!/usr/bin/env python3
"""
Analyze an auto-calibration console log and compare:
1) optimizer-reported per-reference ΔE
2) post-calibration measured Lab values against the same references
"""

from __future__ import annotations

import argparse
import math
import re
from dataclasses import dataclass
from pathlib import Path


CAL_REF_FLAG_IS_WHITE = 1 << 0
CAL_REF_FLAG_IS_NEUTRAL = 1 << 1
CAL_REF_FLAG_IS_BLACK = 1 << 3
CAL_REF_FLAG_GRAY = 1 << 4
CAL_REF_FLAG_DARK_CHROMATIC = 1 << 5

SATURATED_REFERENCE_WEIGHT = 1.5
DARK_CHROMATIC_REFERENCE_WEIGHT = 0.75
DEFAULT_REFERENCE_WEIGHT = 1.0


@dataclass
class Lab:
    l: float
    a: float
    b: float


@dataclass
class Reference:
    name: str
    target: Lab
    flags: int


def ciede2000(lab1: Lab, lab2: Lab) -> float:
    l1, a1, b1 = lab1.l, lab1.a, lab1.b
    l2, a2, b2 = lab2.l, lab2.a, lab2.b

    avg_l = (l1 + l2) / 2.0
    c1 = math.sqrt(a1 * a1 + b1 * b1)
    c2 = math.sqrt(a2 * a2 + b2 * b2)
    avg_c = (c1 + c2) / 2.0

    g = 0.5 * (1.0 - math.sqrt((avg_c**7) / (avg_c**7 + 25.0**7)))
    a1p = (1.0 + g) * a1
    a2p = (1.0 + g) * a2
    c1p = math.sqrt(a1p * a1p + b1 * b1)
    c2p = math.sqrt(a2p * a2p + b2 * b2)
    avg_cp = (c1p + c2p) / 2.0

    h1p = math.degrees(math.atan2(b1, a1p)) % 360.0
    h2p = math.degrees(math.atan2(b2, a2p)) % 360.0
    if abs(h1p - h2p) > 180.0:
        avg_hp = (h1p + h2p + 360.0) / 2.0 if (h1p + h2p) < 360.0 else (h1p + h2p - 360.0) / 2.0
    else:
        avg_hp = (h1p + h2p) / 2.0

    t = (
        1.0
        - 0.17 * math.cos(math.radians(avg_hp - 30.0))
        + 0.24 * math.cos(math.radians(2.0 * avg_hp))
        + 0.32 * math.cos(math.radians(3.0 * avg_hp + 6.0))
        - 0.20 * math.cos(math.radians(4.0 * avg_hp - 63.0))
    )

    dhp = h2p - h1p
    if abs(dhp) > 180.0:
        dhp += 360.0 if dhp < 0.0 else -360.0

    dlp = l2 - l1
    dcp = c2p - c1p
    dhp_term = 2.0 * math.sqrt(c1p * c2p) * math.sin(math.radians(dhp / 2.0))

    sl = 1.0 + (0.015 * (avg_l - 50.0) * (avg_l - 50.0)) / math.sqrt(20.0 + (avg_l - 50.0) * (avg_l - 50.0))
    sc = 1.0 + 0.045 * avg_cp
    sh = 1.0 + 0.015 * avg_cp * t

    dtheta = 30.0 * math.exp(-((avg_hp - 275.0) / 25.0) ** 2.0)
    rc = 2.0 * math.sqrt((avg_cp**7) / (avg_cp**7 + 25.0**7))
    rt = -rc * math.sin(math.radians(2.0 * dtheta))

    return math.sqrt((dlp / sl) ** 2.0 + (dcp / sc) ** 2.0 + (dhp_term / sh) ** 2.0 + rt * (dcp / sc) * (dhp_term / sh))


def parse_log(text: str) -> tuple[list[Reference], dict[str, float], list[Lab]]:
    ref_re = re.compile(
        r"Added reference '([^']+)'.*Lab\(\s*([-0-9.]+),\s*([-0-9.]+),\s*([-0-9.]+)\)\s*flags=0x([0-9a-fA-F]+)"
    )
    final_ref_re = re.compile(r"auto_cal:\s+(.+?): ΔE=\s*([-0-9.]+)\s+\(target")
    lab_re = re.compile(r"main: Lab: L=\s*([-0-9.]+)\s+a=\s*([-0-9.]+)\s+b=\s*([-0-9.]+)")

    refs = [
        Reference(name=m.group(1), target=Lab(float(m.group(2)), float(m.group(3)), float(m.group(4))), flags=int(m.group(5), 16))
        for m in ref_re.finditer(text)
    ]

    final_report = {}
    for m in final_ref_re.finditer(text):
        final_report[m.group(1).strip()] = float(m.group(2))

    start_idx = text.find("Auto-calibration complete")
    search_text = text[start_idx:] if start_idx >= 0 else text
    measured_labs = [Lab(float(m.group(1)), float(m.group(2)), float(m.group(3))) for m in lab_re.finditer(search_text)]

    return refs, final_report, measured_labs


def weight_for_flags(flags: int) -> float:
    if flags & CAL_REF_FLAG_GRAY:
        return DEFAULT_REFERENCE_WEIGHT
    if flags & CAL_REF_FLAG_DARK_CHROMATIC:
        return DARK_CHROMATIC_REFERENCE_WEIGHT
    if flags & (CAL_REF_FLAG_IS_NEUTRAL | CAL_REF_FLAG_IS_WHITE):
        return DEFAULT_REFERENCE_WEIGHT
    return SATURATED_REFERENCE_WEIGHT


def main() -> None:
    parser = argparse.ArgumentParser(description="Analyze auto-calibration quality from console log")
    parser.add_argument("logfile", nargs="?", default="console.txt", help="Path to console log (default: console.txt)")
    args = parser.parse_args()

    log_path = Path(args.logfile)
    text = log_path.read_text(encoding="utf-8")
    refs, final_report, measured_labs = parse_log(text)

    if not refs:
        raise SystemExit("No calibration references found in log")

    if len(measured_labs) < len(refs):
        raise SystemExit(f"Found {len(measured_labs)} post-calibration measurements but expected at least {len(refs)}")

    print(f"Log: {log_path}")
    print(f"References: {len(refs)}")
    print(f"Post-calibration measurements used: {len(refs)}")
    print()
    print(f"{'Reference':<16} {'Opt ΔE':>7} {'Post ΔE':>8} {'Weight':>7}")
    print("-" * 44)

    weighted_sum = 0.0
    weighted_den = 0.0
    post_errors: list[tuple[str, float]] = []

    for ref, measured in zip(refs, measured_labs):
        post_de = ciede2000(ref.target, measured)
        opt_de = final_report.get(ref.name)
        weight = 0.0 if (ref.flags & CAL_REF_FLAG_IS_BLACK) else weight_for_flags(ref.flags)
        opt_text = f"{opt_de:7.2f}" if opt_de is not None else "   n/a "
        print(f"{ref.name:<16} {opt_text} {post_de:8.2f} {weight:7.2f}")
        post_errors.append((ref.name, post_de))

        if weight > 0.0:
            weighted_sum += post_de * weight
            weighted_den += weight

    if weighted_den == 0:
        raise SystemExit("No weighted references found")

    post_errors.sort(key=lambda p: p[1], reverse=True)
    weighted_avg = weighted_sum / weighted_den
    print("-" * 44)
    print(f"Post-cal weighted average ΔE: {weighted_avg:.2f}")
    print("Largest post-cal errors:", ", ".join(f"{name} ({de:.2f})" for name, de in post_errors[:4]))


if __name__ == "__main__":
    main()
