#!/usr/bin/env python3
"""Generate a Kona reference data C++ table from kona_captures.json.

This script reads Lab measurements from a kona_captures.json file and produces a
C++ source file containing a kona_table_t struct for firmware use.  Only entries
with measured=true and complete L/a/b values are included.

The generated table includes:
- Schema version for compatibility checking
- Entry count and CRC32 checksum for validation
- Lab values for each measured Kona swatch

Usage:
    python3 generate_kona_table.py --input ../kona_captures.json \\
                                   --output ../main/konaref_generated.cpp
"""

import argparse
import dataclasses
import datetime as dt
import json
import math
import pathlib
import random
import struct
import zlib
from typing import Iterable, List, Optional

SCHEMA_VERSION = 1
MAX_ENTRIES = 2048


@dataclasses.dataclass(frozen=True)
class KonaEntry:
    """A single Kona swatch reference entry."""
    kona_id: int
    l: float
    a: float
    b: float
    name: str = ""


def _float_or_none(row: dict, key: str) -> Optional[float]:
    """Parse a float from a dict, returning None for missing/empty/None values."""
    value = row.get(key)
    if value is None or str(value).strip() == "":
        return None
    return float(value)


def parse_json_captures(path: pathlib.Path) -> List[KonaEntry]:
    """Parse Lab measurements from a kona_captures.json file.

    Reads the 'swatches' array and extracts entries where measured=true and
    the 'lab' object has all three values (l, a, b).
    Duplicate IDs are deduplicated (last entry wins).
    Output is sorted by kona_id.
    """
    with path.open(encoding="utf-8") as f:
        data = json.load(f)

    entries = {}
    for swatch in data.get("swatches", []):
        if not swatch.get("measured", False):
            continue

        lab = swatch.get("lab") or {}
        l = lab.get("l")
        a = lab.get("a")
        b = lab.get("b")
        if l is None or a is None or b is None:
            continue

        kona_id_raw = swatch.get("id")
        if kona_id_raw is None:
            continue

        kona_id = int(kona_id_raw)
        entries[kona_id] = KonaEntry(
            kona_id=kona_id,
            l=float(l),
            a=float(a),
            b=float(b),
            name=str(swatch.get("name", "")).strip(),
        )

    ordered = sorted(entries.values(), key=lambda e: e.kona_id)
    if len(ordered) > MAX_ENTRIES:
        raise ValueError(f"Too many entries ({len(ordered)}), max is {MAX_ENTRIES}")
    return ordered


def parse_captures(path: pathlib.Path) -> List[KonaEntry]:
    """Parse Kona capture data from a kona_captures.json file."""
    return parse_json_captures(path)


def parse_json_synthetic(path: pathlib.Path) -> List[KonaEntry]:
    """Parse synthetic tint/shade entries from a kona_synthetic_tints.json file.

    Reads the 'swatches' array and extracts entries with valid Lab values.
    Synthetic entries are expected to have 'synthetic': true and IDs >= 10000.
    Duplicate IDs are deduplicated (last entry wins).
    Output is sorted by id.
    """
    with path.open(encoding="utf-8") as f:
        data = json.load(f)

    entries = {}
    for swatch in data.get("swatches", []):
        lab = swatch.get("lab") or {}
        l = lab.get("l")
        a = lab.get("a")
        b = lab.get("b")
        if l is None or a is None or b is None:
            continue

        swatch_id_raw = swatch.get("id")
        if swatch_id_raw is None:
            continue

        swatch_id = int(swatch_id_raw)
        entries[swatch_id] = KonaEntry(
            kona_id=swatch_id,
            l=float(l),
            a=float(a),
            b=float(b),
            name=str(swatch.get("name", "")).strip(),
        )

    return sorted(entries.values(), key=lambda e: e.kona_id)


def crc32_entries(entries: Iterable[KonaEntry]) -> int:
    """Compute CRC32 checksum of entry data matching firmware struct layout.
    
    The binary format must match kona_ref_t in konaref.h:
    - uint16_t kona_id (2 bytes)
    - 2 bytes padding (for 4-byte float alignment)
    - float l, a, b (4 bytes each = 12 bytes)
    - Total: 16 bytes per entry, little-endian
    
    Note: The padding is required because C/C++ struct alignment rules
    insert 2 bytes of padding after kona_id to align the float members.
    """
    payload = bytearray()
    for e in entries:
        # Pack as: uint16_t + 2 padding bytes + 3 floats (little-endian)
        # This must match sizeof(kona_ref_t) = 16 bytes
        payload.extend(struct.pack("<H2x3f", e.kona_id, e.l, e.a, e.b))
    return zlib.crc32(payload) & 0xFFFFFFFF


# ---------------------------------------------------------------------------
# CIEDE2000 — Python implementation for VP-tree construction.
# The runtime C version in color_math.cpp is used for actual device matching.
# ---------------------------------------------------------------------------

def _ciede2000(lab1_l: float, lab1_a: float, lab1_b: float,
               lab2_l: float, lab2_a: float, lab2_b: float) -> float:
    """Calculate CIEDE2000 colour difference between two Lab triples."""
    kL = kC = kH = 1.0

    C1 = math.sqrt(lab1_a * lab1_a + lab1_b * lab1_b)
    C2 = math.sqrt(lab2_a * lab2_a + lab2_b * lab2_b)
    C_bar = (C1 + C2) / 2.0
    C_bar_7 = C_bar ** 7
    G = 0.5 * (1.0 - math.sqrt(C_bar_7 / (C_bar_7 + 25.0 ** 7)))

    a1p = lab1_a * (1.0 + G)
    a2p = lab2_a * (1.0 + G)
    C1p = math.sqrt(a1p * a1p + lab1_b * lab1_b)
    C2p = math.sqrt(a2p * a2p + lab2_b * lab2_b)

    def _hp(ap: float, b: float) -> float:
        if abs(ap) < 1e-10 and abs(b) < 1e-10:
            return 0.0
        h = math.degrees(math.atan2(b, ap))
        return h + 360.0 if h < 0 else h

    h1p = _hp(a1p, lab1_b)
    h2p = _hp(a2p, lab2_b)

    dLp = lab2_l - lab1_l
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

    Lbp = (lab1_l + lab2_l) / 2.0
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
# VP-tree construction — produces a flattened node array for firmware use.
# ---------------------------------------------------------------------------

@dataclasses.dataclass
class _VPNode:
    entry_index: int      # index into the *sorted entries list*
    median_distance: float
    left_child: int       # node-list index, -1 if none
    right_child: int      # node-list index, -1 if none


def _build_vptree(entries: List[KonaEntry], indices: List[int],
                  nodes: List[_VPNode]) -> int:
    """Recursively build a VP-tree from *indices* into *entries*.

    Returns the index of the created node in *nodes*, or -1 if empty.
    """
    if not indices:
        return -1

    if len(indices) == 1:
        nodes.append(_VPNode(indices[0], 0.0, -1, -1))
        return len(nodes) - 1

    vp_idx = indices[0]
    vp = entries[vp_idx]
    rest = indices[1:]

    dists = []
    for idx in rest:
        e = entries[idx]
        d = _ciede2000(vp.l, vp.a, vp.b, e.l, e.a, e.b)
        dists.append((idx, d))
    dists.sort(key=lambda x: x[1])

    median_pos = len(dists) // 2
    median_distance = dists[median_pos - 1][1] if median_pos > 0 else (dists[0][1] if dists else 0.0)

    left_idx_list = [i for i, d in dists if d <= median_distance]
    right_idx_list = [i for i, d in dists if d > median_distance]

    # Avoid infinite recursion when all distances are identical
    if len(left_idx_list) == len(dists):
        left_idx_list = [i for i, _ in dists[:median_pos]]
        right_idx_list = [i for i, _ in dists[median_pos:]]

    node_pos = len(nodes)
    nodes.append(_VPNode(vp_idx, median_distance, -1, -1))

    nodes[node_pos].left_child = _build_vptree(entries, left_idx_list, nodes)
    nodes[node_pos].right_child = _build_vptree(entries, right_idx_list, nodes)
    return node_pos


def build_vptree(entries: List[KonaEntry]) -> List[_VPNode]:
    """Build a VP-tree over *entries* (already sorted by kona_id).

    Returns the flat list of nodes; node 0 is the root.
    """
    if not entries:
        return []
    indices = list(range(len(entries)))
    random.seed(42)          # deterministic for reproducible builds
    random.shuffle(indices)
    nodes: List[_VPNode] = []
    _build_vptree(entries, indices, nodes)
    return nodes


def render_cpp(entries: List[KonaEntry], source_path: pathlib.Path, source_script: str = "generate_kona_table.py") -> str:
    """Render the C++ source file containing the kona_table_t struct and VP-tree."""
    sorted_entries = sorted(entries, key=lambda e: e.kona_id)
    if len(sorted_entries) > MAX_ENTRIES:
        raise ValueError(f"Too many entries ({len(sorted_entries)}), max is {MAX_ENTRIES}")
    crc = crc32_entries(sorted_entries)
    rows = []
    for e in sorted_entries:
        comment = f" // {e.kona_id} {e.name}".rstrip() if e.name else f" // {e.kona_id}"
        rows.append(
            "        { %d, %.6ff, %.6ff, %.6ff },%s" % (
                e.kona_id,
                e.l,
                e.a,
                e.b,
                comment,
            )
        )
    if len(sorted_entries) < MAX_ENTRIES:
        rows.append("        // Remaining entries are zero-initialized.")

    # Build VP-tree for O(log n) matching
    vp_nodes = build_vptree(sorted_entries)
    vp_rows = []
    for i, n in enumerate(vp_nodes):
        vp_rows.append(
            "    { %d, %.6ff, %d, %d }," % (
                n.entry_index,
                n.median_distance,
                n.left_child,
                n.right_child,
            )
        )

    generated_at = dt.datetime.now(dt.timezone.utc).isoformat()
    tree_depth = 0
    if vp_nodes:
        def _depth(idx: int, d: int) -> int:
            if idx < 0:
                return d
            nd = vp_nodes[idx]
            return max(_depth(nd.left_child, d + 1), _depth(nd.right_child, d + 1))
        tree_depth = _depth(0, 0)

    tree_kb = len(vp_nodes) * 12 / 1024.0  # 12 bytes per node with alignment (2+2pad+4+2+2)

    return f'''// Auto-generated by {source_script}
// Source: {source_path}
// Generated at: {generated_at}
// Entry count: {len(sorted_entries)}
// VP-tree nodes: {len(vp_nodes)}, depth: {tree_depth}, Flash: {tree_kb:.2f} KB

#include "konaref.h"

const kona_table_t kona_reference = {{
    .version = KONA_REF_SCHEMA_VERSION,
    .entry_count = {len(sorted_entries)},
    .crc32 = 0x{crc:08X}u,
    .entries = {{
{chr(10).join(rows)}
    }},
}};

const uint16_t kona_vptree_node_count = {len(vp_nodes)};

const kona_vptree_node_t kona_vptree_nodes[{max(1, len(vp_nodes))}] = {{
{chr(10).join(vp_rows) if vp_rows else "    { 0, 0.0f, -1, -1 },  // placeholder"}
}};
'''


def parse_args() -> argparse.Namespace:
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--input", default="../kona_captures.json", type=pathlib.Path,
                   help="Input kona_captures.json with measured Lab values")
    p.add_argument("--output", default=pathlib.Path("../main/konaref_generated.cpp"), type=pathlib.Path,
                   help="Output C++ source file path")
    p.add_argument("--synthetic", default=None, type=pathlib.Path,
                   help="Optional kona_synthetic_tints.json to merge (real entries take priority)")
    return p.parse_args()


def main() -> int:
    args = parse_args()
    real_entries = parse_captures(args.input)

    synthetic_entries: List[KonaEntry] = []
    if args.synthetic is not None:
        if not args.synthetic.exists():
            print(f"Warning: synthetic file not found: {args.synthetic} — skipping")
        else:
            synthetic_entries = parse_json_synthetic(args.synthetic)

    # Merge: real entries take priority over synthetic entries with the same ID
    real_ids = {e.kona_id for e in real_entries}
    merged = list(real_entries)
    skipped = 0
    for e in synthetic_entries:
        if e.kona_id in real_ids:
            skipped += 1
        else:
            merged.append(e)

    merged.sort(key=lambda e: e.kona_id)

    source_script = "generate_kona_table.py"
    if synthetic_entries:
        source_script = "generate_kona_table.py (with synthetic tints)"

    output_text = render_cpp(merged, args.input, source_script)
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(output_text)

    if synthetic_entries:
        used_synthetic = len(merged) - len(real_entries)
        print(
            f"Wrote {len(real_entries)} real + {used_synthetic} synthetic"
            f" = {len(merged)} entries to {args.output}"
        )
        if skipped:
            print(f"  (Skipped {skipped} synthetic entries that collided with real IDs)")
    else:
        print(f"Wrote {len(real_entries)} entries to {args.output}")

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
