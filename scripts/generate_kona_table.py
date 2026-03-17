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
import pathlib
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


def render_cpp(entries: List[KonaEntry], source_path: pathlib.Path, source_script: str = "generate_kona_table.py") -> str:
    """Render the C++ source file containing the kona_table_t struct."""
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

    generated_at = dt.datetime.now(dt.timezone.utc).isoformat()
    return f'''// Auto-generated by {source_script}
// Source: {source_path}
// Generated at: {generated_at}
// Entry count: {len(sorted_entries)}

#include "konaref.h"

const kona_table_t kona_reference = {{
    .version = KONA_REF_SCHEMA_VERSION,
    .entry_count = {len(sorted_entries)},
    .crc32 = 0x{crc:08X}u,
    .entries = {{
{chr(10).join(rows)}
    }},
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
