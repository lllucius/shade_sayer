#!/usr/bin/env python3
"""Generate synthetic tint, shade, and tone references from existing Kona swatch scans.

This script reads kona_captures.json, generates synthetic lighter, darker,
and desaturated variants of each measured swatch by shifting L* up/down
(tints/shades) or reducing chroma at the same L* (tones), and writes the
result to kona_synthetic_tints.json.

The generated entries use IDs >= 10000 so they never collide with real Kona
IDs (which range from 7 to 1898). Each variant ID is deterministic:

    synthetic_id = 10000 + original_kona_id * 10 + variant_index

Where variant_index corresponds to the step's position in the ordered step list
(0 = most negative offset, increasing for each step).  Tone variants occupy
indices after the tint/shade entries (by default indices 4 and 5).

Usage:
    python3 scripts/generate_synthetic_tints.py \\
        --input kona_captures.json \\
        --output kona_synthetic_tints.json \\
        --steps -30,-15,15,30 \\
        --tone-scales 0.6,0.3 \\
        --dry-run
"""

import argparse
import dataclasses
import datetime as dt
import json
import math
import pathlib
from typing import List, Optional, Tuple

# Default L* offset steps (configurable via --steps)
DEFAULT_STEPS = [-30, -15, 15, 30]

# Variant name prefixes ordered from most-negative to most-positive step
VARIANT_PREFIXES = ["Deep", "Dark", "Light", "Pale"]

# Default chroma scale factors for tone generation (configurable via --tone-scales).
# Sorted largest-first so that the least reduced tone comes first.
DEFAULT_TONE_SCALES = [0.6, 0.3]

# Tone variant prefixes ordered from least to most desaturated
TONE_PREFIXES = ["Muted", "Dusty"]

# Tone variant indices start after the 4 tint/shade slots
TONE_VARIANT_INDEX_BASE = 4

# Minimum chroma (sqrt(a² + b²)) required for a tone variant to be meaningful
MIN_CHROMA_FOR_TONE = 5.0


@dataclasses.dataclass(frozen=True)
class SourceSwatch:
    """A measured Kona swatch read from kona_captures.json."""
    kona_id: int
    name: str
    l: float
    a: float
    b: float


@dataclasses.dataclass
class SyntheticEntry:
    """A single synthetic tint, shade, or tone entry."""
    synth_id: int
    name: str
    source_id: int
    source_name: str
    variant: str       # "deep"|"dark"|"light"|"pale"|"muted"|"dusty"
    l_offset: int
    l: float
    a: float
    b: float
    notes: str
    chroma_scale: Optional[float] = None  # For tones: scale factor applied


def generate_variant(
    original_l: float,
    original_a: float,
    original_b: float,
    target_l: float,
    min_l: float,
    max_l: float,
) -> Optional[Tuple[float, float, float]]:
    """Generate a synthetic tint/shade Lab value from an original.

    Tints (lighter) desaturate proportionally to how far they move toward
    white (L*=100).  Shades (darker) undergo a smaller chroma reduction as
    they approach black (L*=0).

    @param original_l   Source L* value.
    @param original_a   Source a* value.
    @param original_b   Source b* value.
    @param target_l     Desired L* for the new variant (before clamping).
    @param min_l        Minimum allowed L* for output (default 5.0).
    @param max_l        Maximum allowed L* for output (default 95.0).
    @return (L, a, b) tuple for the variant, or None if the variant should
            be skipped.
    """
    # Clamp target L* to valid range
    target_l = max(min_l, min(max_l, target_l))

    # Skip if variant would be too close to the original
    if abs(target_l - original_l) < 5.0:
        return None

    # Skip tints of already-very-light colors
    if target_l > original_l and original_l > 85.0:
        return None

    # Skip shades of already-very-dark colors
    if target_l < original_l and original_l < 15.0:
        return None

    # Chroma scaling
    if target_l > original_l:
        # TINT (lighter): desaturate proportionally toward white
        desaturation = (
            (target_l - original_l) / (100.0 - original_l)
            if original_l < 100.0
            else 1.0
        )
        chroma_scale = max(0.1, 1.0 - desaturation * 0.7)
    else:
        # SHADE (darker): slight chroma reduction toward black
        darkening = (
            (original_l - target_l) / original_l
            if original_l > 0.0
            else 1.0
        )
        chroma_scale = max(0.2, 1.0 - darkening * 0.3)

    new_a = original_a * chroma_scale
    new_b = original_b * chroma_scale

    return (target_l, new_a, new_b)


def generate_tone(
    original_l: float,
    original_a: float,
    original_b: float,
    chroma_scale: float,
) -> Optional[Tuple[float, float, float]]:
    """Generate a synthetic tone Lab value by reducing chroma at the same L*.

    A tone is produced by mixing gray into the original color, which reduces
    saturation (chroma) while keeping lightness essentially unchanged.

    @param original_l     Source L* value.
    @param original_a     Source a* value.
    @param original_b     Source b* value.
    @param chroma_scale   Multiplicative factor for chroma (0.0–1.0).
    @return (L, a, b) tuple for the tone, or None if the variant should
            be skipped (e.g. source chroma too low to be meaningful).
    """
    chroma = math.sqrt(original_a * original_a + original_b * original_b)

    # Skip near-achromatic colours — desaturating them further is meaningless
    if chroma < MIN_CHROMA_FOR_TONE:
        return None

    new_a = original_a * chroma_scale
    new_b = original_b * chroma_scale

    return (original_l, new_a, new_b)


def load_source_swatches(input_path: pathlib.Path) -> List[SourceSwatch]:
    """Load measured swatches with valid Lab values from kona_captures.json.

    @param input_path  Path to kona_captures.json.
    @return List of SourceSwatch objects sorted by kona_id.
    """
    with input_path.open(encoding="utf-8") as f:
        data = json.load(f)

    swatches: List[SourceSwatch] = []
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

        name = str(swatch.get("name", "")).strip()
        swatches.append(SourceSwatch(
            kona_id=int(kona_id_raw),
            name=name,
            l=float(l),
            a=float(a),
            b=float(b),
        ))

    # Sort for deterministic output
    swatches.sort(key=lambda s: s.kona_id)
    return swatches


def build_variant_name(prefix: str, source_name: str) -> str:
    """Build a human-readable variant name.

    @param prefix       Prefix word (e.g. "Dark", "Pale").
    @param source_name  Original Kona color name (e.g. "SEA GLASS").
    @return Combined name like "Dark SEA GLASS".
    """
    return f"{prefix} {source_name}"


def generate_synthetics(
    swatches: List[SourceSwatch],
    steps: List[int],
    min_l: float,
    max_l: float,
    tone_scales: Optional[List[float]] = None,
) -> List[SyntheticEntry]:
    """Generate all synthetic tint, shade, and tone entries.

    The step list is sorted ascending so that variant_index 0 always
    corresponds to the most negative (darkest) offset and the highest index
    to the most positive (lightest).  Tone entries (if enabled) occupy
    variant indices after the tint/shade entries.

    @param swatches      Measured Kona swatches to generate variants for.
    @param steps         L* offsets to apply (e.g. [-30, -15, 15, 30]).
    @param min_l         Minimum allowed L* for variants.
    @param max_l         Maximum allowed L* for variants.
    @param tone_scales   Chroma scale factors for tone generation (e.g.
                         [0.6, 0.3]).  None or empty disables tones.
    @return List of SyntheticEntry objects.
    """
    # Sort steps ascending so indices are stable and predictable
    sorted_steps = sorted(steps)
    num_steps = len(sorted_steps)

    # Build prefix list — use VARIANT_PREFIXES if step count matches,
    # otherwise fall back to generic labels
    if num_steps == len(VARIANT_PREFIXES):
        prefixes = VARIANT_PREFIXES
    else:
        # Generic prefixes for non-standard step counts
        prefixes = []
        for step in sorted_steps:
            if step < -20:
                prefixes.append("Deep")
            elif step < 0:
                prefixes.append("Dark")
            elif step < 20:
                prefixes.append("Light")
            else:
                prefixes.append("Pale")

    results: List[SyntheticEntry] = []

    for swatch in swatches:
        # Track clamped target L* values to avoid duplicate entries when
        # multiple steps clamp to the same boundary (e.g. both +15 and +30
        # clamping to max_l for colors near the L* ceiling)
        seen_target_l: set[float] = set()

        for variant_index, step in enumerate(sorted_steps):
            target_l = swatch.l + step
            variant = generate_variant(
                swatch.l, swatch.a, swatch.b, target_l, min_l, max_l
            )
            if variant is None:
                continue

            vl, va, vb = variant

            # Skip if a previous step already produced this clamped L*
            # (matches the rounding precision used for output on line 256)
            rounded_l = round(vl, 6)
            if rounded_l in seen_target_l:
                continue
            seen_target_l.add(rounded_l)

            prefix = prefixes[variant_index]

            # Determine variant type label
            variant_label = prefix.lower()

            synth_id = 10000 + swatch.kona_id * 10 + variant_index
            name = build_variant_name(prefix, swatch.name)
            notes = (
                f"Synthetic {'tint' if step > 0 else 'shade'}: "
                f"L* {step:+d} from {swatch.name} (L*={swatch.l:.1f})"
            )

            results.append(SyntheticEntry(
                synth_id=synth_id,
                name=name,
                source_id=swatch.kona_id,
                source_name=swatch.name,
                variant=variant_label,
                l_offset=step,
                l=round(vl, 6),
                a=round(va, 6),
                b=round(vb, 6),
                notes=notes,
            ))

    # ── Tone generation ──────────────────────────────────────────────
    if tone_scales:
        # Sort largest-first so the least-reduced tone gets the lowest index
        sorted_tone_scales = sorted(tone_scales, reverse=True)
        num_tones = len(sorted_tone_scales)

        # Build tone prefix list
        if num_tones == len(TONE_PREFIXES):
            t_prefixes = TONE_PREFIXES
        else:
            t_prefixes = [f"Tone{i}" for i in range(num_tones)]

        for swatch in swatches:
            for tone_index, scale in enumerate(sorted_tone_scales):
                tone = generate_tone(
                    swatch.l, swatch.a, swatch.b, scale
                )
                if tone is None:
                    continue

                vl, va, vb = tone
                variant_index = TONE_VARIANT_INDEX_BASE + tone_index
                prefix = t_prefixes[tone_index]
                variant_label = prefix.lower()

                synth_id = 10000 + swatch.kona_id * 10 + variant_index
                name = build_variant_name(prefix, swatch.name)
                notes = (
                    f"Synthetic tone: chroma \u00d7{scale:.1f} "
                    f"from {swatch.name} (L*={swatch.l:.1f})"
                )

                results.append(SyntheticEntry(
                    synth_id=synth_id,
                    name=name,
                    source_id=swatch.kona_id,
                    source_name=swatch.name,
                    variant=variant_label,
                    l_offset=0,
                    l=round(vl, 6),
                    a=round(va, 6),
                    b=round(vb, 6),
                    notes=notes,
                    chroma_scale=scale,
                ))

    return results


def render_output(
    entries: List[SyntheticEntry],
    source_path: pathlib.Path,
    steps: List[int],
    tone_scales: Optional[List[float]] = None,
) -> dict:
    """Render the output JSON structure.

    @param entries      List of generated synthetic entries.
    @param source_path  Path to the source kona_captures.json.
    @param steps        The L* offset steps used.
    @param tone_scales  Chroma scale factors used for tone generation (or None).
    @return dict suitable for json.dump().
    """
    swatches = []
    for e in entries:
        entry_dict = {
            "id": e.synth_id,
            "name": e.name,
            "source_id": e.source_id,
            "source_name": e.source_name,
            "synthetic": True,
            "variant": e.variant,
            "l_offset": e.l_offset,
            "measured": False,
            "lab": {"l": e.l, "a": e.a, "b": e.b},
            "notes": e.notes,
        }
        if e.chroma_scale is not None:
            entry_dict["chroma_scale"] = e.chroma_scale
        swatches.append(entry_dict)

    result = {
        "schema_version": 1,
        "generator": "generate_synthetic_tints.py",
        "source": str(source_path),
        "generated_date": dt.datetime.now(dt.timezone.utc).isoformat(),
        "steps": sorted(steps),
        "swatches": swatches,
    }

    if tone_scales:
        result["tone_scales"] = sorted(tone_scales, reverse=True)

    return result


def parse_steps(steps_str: str) -> List[int]:
    """Parse a comma-separated list of integer L* offsets.

    @param steps_str  String like "-30,-15,15,30".
    @return List of integers.
    @raises ValueError if any token is not a valid integer.
    """
    result = []
    for token in steps_str.split(","):
        token = token.strip().lstrip("+")
        result.append(int(token))
    return result


def parse_tone_scales(scales_str: str) -> List[float]:
    """Parse a comma-separated list of chroma scale factors.

    @param scales_str  String like "0.6,0.3".
    @return List of floats in (0.0, 1.0).
    @raises ValueError if any token is not a valid float or out of range.
    """
    result = []
    for token in scales_str.split(","):
        val = float(token.strip())
        if val <= 0.0 or val >= 1.0:
            raise ValueError(
                f"tone scale {val} must be between 0.0 and 1.0 exclusive"
            )
        result.append(val)
    return result


def parse_args() -> argparse.Namespace:
    p = argparse.ArgumentParser(description=__doc__,
                                formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument(
        "--input",
        default="../kona_captures.json",
        type=pathlib.Path,
        help="Path to kona_captures.json (default: ../kona_captures.json)",
    )
    p.add_argument(
        "--output",
        default="../kona_synthetic_tints.json",
        type=pathlib.Path,
        help="Path to output JSON file (default: ../kona_synthetic_tints.json)",
    )
    p.add_argument(
        "--steps",
        default=",".join(str(s) for s in DEFAULT_STEPS),
        type=str,
        help="Comma-separated L* offsets, e.g. -30,-15,15,30 (default: -30,-15,15,30)",
    )
    p.add_argument(
        "--min-l",
        default=5.0,
        type=float,
        help="Minimum L* for generated variants (default: 5.0)",
    )
    p.add_argument(
        "--max-l",
        default=95.0,
        type=float,
        help="Maximum L* for generated variants (default: 95.0)",
    )
    p.add_argument(
        "--tone-scales",
        default=",".join(str(s) for s in DEFAULT_TONE_SCALES),
        type=str,
        help=(
            "Comma-separated chroma scale factors for tone variants "
            "(0.0–1.0 exclusive, default: 0.6,0.3)"
        ),
    )
    p.add_argument(
        "--no-tones",
        action="store_true",
        help="Disable tone generation (only produce tints and shades)",
    )
    p.add_argument(
        "--dry-run",
        action="store_true",
        help="Print summary without writing the output file",
    )
    return p.parse_args()


def main() -> int:
    args = parse_args()

    try:
        steps = parse_steps(args.steps)
    except ValueError as exc:
        print(f"Error: invalid --steps value: {exc}")
        return 1

    tone_scales: Optional[List[float]] = None
    if not args.no_tones:
        try:
            tone_scales = parse_tone_scales(args.tone_scales)
        except ValueError as exc:
            print(f"Error: invalid --tone-scales value: {exc}")
            return 1

    if not args.input.exists():
        print(f"Error: input file not found: {args.input}")
        return 1

    print(f"Reading source swatches from: {args.input}")
    swatches = load_source_swatches(args.input)
    print(f"  Found {len(swatches)} measured swatches")

    print(f"Generating synthetic variants with L* steps: {sorted(steps)}")
    if tone_scales:
        print(f"  Tone chroma scales: "
              f"{sorted(tone_scales, reverse=True)}")
    entries = generate_synthetics(
        swatches, steps, args.min_l, args.max_l, tone_scales
    )

    # Summary statistics
    tints = sum(1 for e in entries if e.l_offset > 0)
    shades = sum(1 for e in entries if e.l_offset < 0)
    tones = sum(1 for e in entries if e.chroma_scale is not None)
    print(f"  Generated {len(entries)} synthetic entries "
          f"({shades} shades + {tints} tints + {tones} tones) "
          f"from {len(swatches)} source swatches")

    if args.dry_run:
        print("Dry run — not writing output file.")
        # Print a sample of entries
        sample = entries[:5]
        if sample:
            print("\nSample entries:")
            for e in sample:
                print(f"  id={e.synth_id} name='{e.name}' "
                      f"source_id={e.source_id} offset={e.l_offset:+d} "
                      f"Lab=({e.l:.2f}, {e.a:.2f}, {e.b:.2f})")
        return 0

    output = render_output(entries, args.input, steps, tone_scales)
    args.output.parent.mkdir(parents=True, exist_ok=True)
    with args.output.open("w", encoding="utf-8") as f:
        json.dump(output, f, indent=2, ensure_ascii=False)
        f.write("\n")

    print(f"Wrote {len(entries)} synthetic entries to: {args.output}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
