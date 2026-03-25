#!/usr/bin/env python3
"""Annotate kona_synthetic_tints.json swatches with nearest real color names.

This script reads ``kona_synthetic_tints.json``, matches each synthetic
swatch's Lab values against one or more reference color databases using the
CIEDE2000 perceptual distance metric, and writes three new fields back into
each swatch entry:

- ``nearest_name``      — the name of the closest color from any database
- ``nearest_source``    — which database it came from (``"resene"``,
                          ``"xkcd"``, or ``"meodai"``)
- ``nearest_delta_e``   — the CIEDE2000 distance (rounded to 2 decimal places)

Reference databases (searched in this priority order):
1. **Resene** — ``scripts/resene_colors.json`` (pre-computed Lab values)
2. **xkcd**   — ``scripts/xkcd_colors.json``  (pre-computed Lab values)
3. **meodai/color-names** (optional, fetched from
   https://unpkg.com/color-name-list/dist/colornames.json).  Hex values are
   converted to Lab via the same sRGB→XYZ→Lab D65 pipeline used by the other
   import scripts.  The result is cached locally so subsequent runs skip the
   download.  Pass ``--no-meodai`` to skip entirely.

Usage::

    # Annotate in place (overwrites kona_synthetic_tints.json)
    python3 scripts/annotate_nearest_colors.py

    # Dry run — print matches without modifying any file
    python3 scripts/annotate_nearest_colors.py --dry-run

    # Write to a separate output file
    python3 scripts/annotate_nearest_colors.py --output /tmp/annotated.json

    # Only accept matches better than ΔE 5.0
    python3 scripts/annotate_nearest_colors.py --max-delta-e 5.0

    # Skip the network download
    python3 scripts/annotate_nearest_colors.py --no-meodai
"""

import argparse
import json
import math
import os
import statistics
import sys
import urllib.request
from typing import Dict, List, Optional, Tuple

# ---------------------------------------------------------------------------
# CIE LAB conversion constants (D65)
# ---------------------------------------------------------------------------

# Threshold for linear vs cubic root in XYZ→Lab
# Derived from (24/116)^3 ≈ 0.008856
CIE_EPSILON = 0.008856
# kappa = 903.3 is derived from (116/16)^3 * epsilon
CIE_KAPPA = 903.3

# D65 reference white
REF_X = 0.95047
REF_Y = 1.00000
REF_Z = 1.08883

# URL for meodai/color-names database
MEODAI_URL = "https://unpkg.com/color-name-list/dist/colornames.json"

# ---------------------------------------------------------------------------
# sRGB → XYZ → Lab conversion (same as import_xkcd_colors.py)
# ---------------------------------------------------------------------------


def srgb_to_linear(c: int) -> float:
    """Convert an sRGB channel value (0–255) to linear light (0–1)."""
    v = c / 255.0
    if v <= 0.04045:
        return v / 12.92
    return pow((v + 0.055) / 1.055, 2.4)


def rgb_to_xyz(r: int, g: int, b: int) -> Tuple[float, float, float]:
    """Convert sRGB (0–255 per channel) to CIE XYZ using the D65 illuminant."""
    r_lin = srgb_to_linear(r)
    g_lin = srgb_to_linear(g)
    b_lin = srgb_to_linear(b)

    x = r_lin * 0.4124564 + g_lin * 0.3575761 + b_lin * 0.1804375
    y = r_lin * 0.2126729 + g_lin * 0.7151522 + b_lin * 0.0721750
    z = r_lin * 0.0193339 + g_lin * 0.1191920 + b_lin * 0.9503041
    return x, y, z


def xyz_to_lab(x: float, y: float, z: float) -> Tuple[float, float, float]:
    """Convert CIE XYZ to CIELAB using the D65 reference white."""
    xn = x / REF_X
    yn = y / REF_Y
    zn = z / REF_Z

    def f(t: float) -> float:
        if t > CIE_EPSILON:
            return pow(t, 1.0 / 3.0)
        return (CIE_KAPPA * t + 16.0) / 116.0

    fx = f(xn)
    fy = f(yn)
    fz = f(zn)

    L = 116.0 * fy - 16.0
    a = 500.0 * (fx - fy)
    b = 200.0 * (fy - fz)
    return L, a, b


def hex_to_lab(hex_color: str) -> Tuple[float, float, float]:
    """Convert a hex colour string (e.g. ``#ff8040``) to CIELAB D65."""
    hex_color = hex_color.lstrip('#')
    if len(hex_color) != 6:
        raise ValueError(f"Invalid hex color: '#{hex_color}' (expected 6 hex digits)")
    r = int(hex_color[0:2], 16)
    g = int(hex_color[2:4], 16)
    b = int(hex_color[4:6], 16)
    x, y, z = rgb_to_xyz(r, g, b)
    return xyz_to_lab(x, y, z)


# ---------------------------------------------------------------------------
# CIEDE2000 (copied from generate_kona_table.py)
# ---------------------------------------------------------------------------


def ciede2000(
    l1: float, a1: float, b1: float,
    l2: float, a2: float, b2: float,
) -> float:
    """Return the CIEDE2000 colour difference between two CIELAB triples."""
    kL = kC = kH = 1.0

    C1 = math.sqrt(a1 * a1 + b1 * b1)
    C2 = math.sqrt(a2 * a2 + b2 * b2)
    C_bar = (C1 + C2) / 2.0
    C_bar_7 = C_bar ** 7
    G = 0.5 * (1.0 - math.sqrt(C_bar_7 / (C_bar_7 + 25.0 ** 7)))

    a1p = a1 * (1.0 + G)
    a2p = a2 * (1.0 + G)
    C1p = math.sqrt(a1p * a1p + b1 * b1)
    C2p = math.sqrt(a2p * a2p + b2 * b2)

    def _hp(ap: float, b: float) -> float:
        if abs(ap) < 1e-10 and abs(b) < 1e-10:
            return 0.0
        h = math.degrees(math.atan2(b, ap))
        return h + 360.0 if h < 0 else h

    h1p = _hp(a1p, b1)
    h2p = _hp(a2p, b2)

    dLp = l2 - l1
    dCp = C2p - C1p

    if C1p * C2p < 1e-10:
        dhp = 0.0
    else:
        dhp = h2p - h1p
        if dhp > 180.0:
            dhp -= 360.0
        elif dhp < -180.0:
            dhp += 360.0

    dHp = 2.0 * math.sqrt(C1p * C2p) * math.sin(math.radians(dhp / 2.0))

    Lbp = (l1 + l2) / 2.0
    Cbp = (C1p + C2p) / 2.0

    if C1p * C2p < 1e-10:
        hbp = h1p + h2p
    else:
        hs = h1p + h2p
        if abs(h1p - h2p) > 180.0:
            hs += 360.0 if hs < 360.0 else -360.0
        hbp = hs / 2.0

    T = (1.0
         - 0.17 * math.cos(math.radians(hbp - 30.0))
         + 0.24 * math.cos(math.radians(2.0 * hbp))
         + 0.32 * math.cos(math.radians(3.0 * hbp + 6.0))
         - 0.20 * math.cos(math.radians(4.0 * hbp - 63.0)))

    dtheta = 30.0 * math.exp(-((hbp - 275.0) / 25.0) ** 2)
    Cbp7 = Cbp ** 7
    RC = 2.0 * math.sqrt(Cbp7 / (Cbp7 + 25.0 ** 7))

    Lbp50sq = (Lbp - 50.0) ** 2
    SL = 1.0 + 0.015 * Lbp50sq / math.sqrt(20.0 + Lbp50sq)
    SC = 1.0 + 0.045 * Cbp
    SH = 1.0 + 0.015 * Cbp * T
    RT = -math.sin(math.radians(2.0 * dtheta)) * RC

    return math.sqrt(
        (dLp / (kL * SL)) ** 2
        + (dCp / (kC * SC)) ** 2
        + (dHp / (kH * SH)) ** 2
        + RT * (dCp / (kC * SC)) * (dHp / (kH * SH))
    )


# ---------------------------------------------------------------------------
# Reference database loading
# ---------------------------------------------------------------------------


def load_lab_json(path: str, source_tag: str) -> List[Tuple[str, float, float, float, str]]:
    """Load a pre-computed Lab JSON database.

    Expected format: ``{"colors": [{"name": ..., "L": ..., "a": ..., "b": ...}, ...]}``

    @param path        Filesystem path to the JSON file.
    @param source_tag  Short label used for the ``nearest_source`` field
                       (e.g. ``"resene"`` or ``"xkcd"``).
    @returns           List of ``(name, L, a, b, source_tag)`` tuples.
    """
    with open(path, 'r', encoding='utf-8') as fh:
        data = json.load(fh)

    entries = []
    for c in data.get('colors', []):
        try:
            entries.append((c['name'], float(c['L']), float(c['a']), float(c['b']), source_tag))
        except (KeyError, TypeError, ValueError):
            continue

    print(f"  Loaded {len(entries):>6,} colours from {path}")
    return entries


def load_meodai(cache_path: str) -> List[Tuple[str, float, float, float, str]]:
    """Download (or load from cache) the meodai/color-names database.

    The downloaded JSON is ``[{"name": "...", "hex": "#rrggbb"}, ...]``.
    Hex values are converted to Lab using the D65 pipeline above.
    Results are cached to ``cache_path`` to avoid repeated downloads.

    @param cache_path  Path for the local cache file.
    @returns           List of ``(name, L, a, b, "meodai")`` tuples, or an
                       empty list if the download fails.
    """
    source_tag = "meodai"

    # Try cache first
    if os.path.exists(cache_path):
        print(f"  Loading meodai cache from {cache_path}")
        try:
            with open(cache_path, 'r', encoding='utf-8') as fh:
                cached = json.load(fh)
            entries = [
                (c['name'], float(c['L']), float(c['a']), float(c['b']), source_tag)
                for c in cached.get('colors', [])
            ]
            print(f"  Loaded {len(entries):>6,} colours from meodai cache")
            return entries
        except Exception as exc:
            print(f"  Warning: could not read meodai cache ({exc}); re-downloading")

    # Download
    print(f"  Downloading meodai/color-names from {MEODAI_URL} ...")
    try:
        req = urllib.request.Request(
            MEODAI_URL,
            headers={"User-Agent": "annotate_nearest_colors/1.0 (shade_sayer)"},
        )
        with urllib.request.urlopen(req, timeout=30) as resp:
            raw = resp.read().decode('utf-8')
        color_list = json.loads(raw)
    except Exception as exc:
        print(f"  Warning: meodai download failed ({exc}); skipping")
        return []

    print(f"  Converting {len(color_list):,} meodai hex colours to Lab ...")
    entries = []
    cache_data: List[Dict] = []
    for item in color_list:
        name = item.get('name', '').strip()
        hex_val = item.get('hex', '').strip()
        if not name or not hex_val:
            continue
        try:
            L, a, b = hex_to_lab(hex_val)
        except Exception:
            continue
        entries.append((name, L, a, b, source_tag))
        cache_data.append({'name': name, 'L': round(L, 4), 'a': round(a, 4), 'b': round(b, 4)})

    # Write cache
    try:
        with open(cache_path, 'w', encoding='utf-8') as fh:
            json.dump({'colors': cache_data}, fh, indent=2, ensure_ascii=False)
            fh.write('\n')
        print(f"  Cached {len(cache_data):,} meodai entries to {cache_path}")
    except Exception as exc:
        print(f"  Warning: could not write meodai cache ({exc})")

    print(f"  Loaded {len(entries):>6,} colours from meodai")
    return entries


# ---------------------------------------------------------------------------
# Nearest-colour search
# ---------------------------------------------------------------------------


def find_nearest(
    L: float, a: float, b: float,
    databases: List[List[Tuple[str, float, float, float, str]]],
    max_delta_e: Optional[float] = None,
    L_window: float = 20.0,
) -> Optional[Tuple[str, str, float]]:
    """Find the nearest named colour across all reference databases.

    Databases are searched in order; the globally best match (lowest CIEDE2000
    across all databases) is returned.  To keep performance acceptable for the
    large meodai database (~30 k entries), candidates are pre-filtered by L*
    proximity before the full CIEDE2000 calculation is performed.

    @param L           Query L* value.
    @param a           Query a* value.
    @param b           Query b* value.
    @param databases   List of database lists (each entry is a 5-tuple).
    @param max_delta_e If set, only return a match when ΔE ≤ max_delta_e.
    @param L_window    Pre-filter half-width in L* units (default 20.0).
    @returns           ``(name, source, delta_e)`` tuple, or ``None`` if no
                       match satisfies ``max_delta_e``.
    """
    best_name: Optional[str] = None
    best_source: Optional[str] = None
    best_de: float = float('inf')

    for db in databases:
        for (ref_name, ref_L, ref_a, ref_b, ref_source) in db:
            # Fast L* pre-filter to avoid most CIEDE2000 calls
            if abs(ref_L - L) > L_window:
                continue
            de = ciede2000(L, a, b, ref_L, ref_a, ref_b)
            if de < best_de:
                best_de = de
                best_name = ref_name
                best_source = ref_source

    if best_name is None:
        return None
    if max_delta_e is not None and best_de > max_delta_e:
        return None
    return best_name, best_source, best_de


# ---------------------------------------------------------------------------
# Main entry point
# ---------------------------------------------------------------------------


def main() -> int:
    """Script entry point."""
    script_dir = os.path.dirname(os.path.abspath(__file__))

    parser = argparse.ArgumentParser(
        description=(
            "Annotate kona_synthetic_tints.json swatches with the nearest "
            "real colour name from Resene, xkcd, and optionally "
            "meodai/color-names databases using CIEDE2000."
        )
    )
    parser.add_argument(
        '--input', '-i',
        default=os.path.join(script_dir, '..', 'kona_synthetic_tints.json'),
        metavar='FILE',
        help='Input synthetic tints JSON (default: ../kona_synthetic_tints.json)',
    )
    parser.add_argument(
        '--output', '-o',
        default=None,
        metavar='FILE',
        help='Output JSON file (default: same as --input, overwrite in place)',
    )
    parser.add_argument(
        '--resene',
        default=os.path.join(script_dir, 'resene_colors.json'),
        metavar='FILE',
        help='Resene colours JSON (default: resene_colors.json in script dir)',
    )
    parser.add_argument(
        '--xkcd',
        default=os.path.join(script_dir, 'xkcd_colors.json'),
        metavar='FILE',
        help='xkcd colours JSON (default: xkcd_colors.json in script dir)',
    )
    parser.add_argument(
        '--no-meodai',
        action='store_true',
        help='Skip downloading the meodai/color-names database',
    )
    parser.add_argument(
        '--meodai-cache',
        default=os.path.join(script_dir, 'meodai_colors_cache.json'),
        metavar='FILE',
        help='Cache file for downloaded meodai data (default: meodai_colors_cache.json in script dir)',
    )
    parser.add_argument(
        '--max-delta-e',
        type=float,
        default=None,
        metavar='N',
        help='Only annotate if best match ΔE ≤ N (default: no limit)',
    )
    parser.add_argument(
        '--dry-run',
        action='store_true',
        help='Print matches without modifying any file',
    )

    args = parser.parse_args()

    # Resolve paths
    input_path = os.path.realpath(args.input)
    output_path = os.path.realpath(args.output) if args.output else input_path

    # ------------------------------------------------------------------
    # Load input
    # ------------------------------------------------------------------
    print(f"Loading swatches from: {input_path}")
    if not os.path.exists(input_path):
        print(f"Error: input file not found: {input_path}", file=sys.stderr)
        return 1

    with open(input_path, 'r', encoding='utf-8') as fh:
        data = json.load(fh)

    swatches = data.get('swatches', [])
    print(f"  Found {len(swatches):,} swatches")

    # ------------------------------------------------------------------
    # Load reference databases
    # ------------------------------------------------------------------
    print("\nLoading reference colour databases:")
    databases: List[List[Tuple[str, float, float, float, str]]] = []

    if os.path.exists(args.resene):
        databases.append(load_lab_json(args.resene, 'resene'))
    else:
        print(f"  Warning: Resene database not found: {args.resene}")

    if os.path.exists(args.xkcd):
        databases.append(load_lab_json(args.xkcd, 'xkcd'))
    else:
        print(f"  Warning: xkcd database not found: {args.xkcd}")

    if not args.no_meodai:
        meodai_entries = load_meodai(args.meodai_cache)
        if meodai_entries:
            databases.append(meodai_entries)

    if not databases:
        print("Error: no reference databases could be loaded.", file=sys.stderr)
        return 1

    total_ref = sum(len(db) for db in databases)
    print(f"  Total reference colours: {total_ref:,}")

    # ------------------------------------------------------------------
    # Annotate swatches
    # ------------------------------------------------------------------
    print(f"\nAnnotating {len(swatches):,} swatches ...")

    annotated = 0
    skipped = 0
    source_counts: Dict[str, int] = {}
    delta_e_values: List[float] = []
    sample_matches: List[Tuple[str, str, str, float]] = []

    for swatch in swatches:
        lab = swatch.get('lab', {})
        try:
            L = float(lab['l'])
            a = float(lab['a'])
            b = float(lab['b'])
        except (KeyError, TypeError, ValueError):
            skipped += 1
            continue

        result = find_nearest(L, a, b, databases, max_delta_e=args.max_delta_e)
        if result is None:
            skipped += 1
            continue

        name, source, de = result
        swatch['nearest_name'] = name
        swatch['nearest_source'] = source
        swatch['nearest_delta_e'] = round(de, 2)

        annotated += 1
        source_counts[source] = source_counts.get(source, 0) + 1
        delta_e_values.append(de)

        # Collect up to 5 sample matches for console display
        if len(sample_matches) < 5:
            sample_matches.append((swatch.get('name', '?'), name, source, de))

    # ------------------------------------------------------------------
    # Write output
    # ------------------------------------------------------------------
    if not args.dry_run:
        with open(output_path, 'w', encoding='utf-8') as fh:
            json.dump(data, fh, indent=2, ensure_ascii=False)
            fh.write('\n')
        print(f"\nWrote annotated JSON to: {output_path}")
    else:
        print("\nDry run — output file not modified.")

    # ------------------------------------------------------------------
    # Summary statistics
    # ------------------------------------------------------------------
    print(f"\n{'=' * 60}")
    print(f"Summary")
    print(f"{'=' * 60}")
    print(f"  Total swatches processed : {len(swatches):,}")
    print(f"  Annotated                : {annotated:,}")
    print(f"  Skipped (no Lab / no hit): {skipped:,}")

    if source_counts:
        print(f"\n  Breakdown by source:")
        for src in ('resene', 'xkcd', 'meodai'):
            count = source_counts.get(src, 0)
            if count:
                print(f"    {src:<10}: {count:,}")

    if delta_e_values:
        print(f"\n  ΔE (CIEDE2000) statistics:")
        print(f"    min    : {min(delta_e_values):.2f}")
        print(f"    max    : {max(delta_e_values):.2f}")
        print(f"    mean   : {statistics.mean(delta_e_values):.2f}")
        print(f"    median : {statistics.median(delta_e_values):.2f}")

    if sample_matches:
        print(f"\n  Sample matches:")
        for swatch_name, match_name, match_src, de in sample_matches:
            print(f"    '{swatch_name}' → '{match_name}' [{match_src}] ΔE={de:.2f}")

    return 0


if __name__ == '__main__':
    sys.exit(main())
