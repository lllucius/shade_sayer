#!/usr/bin/env python3
"""Generate single-sentence color descriptions for Kona swatch JSON files.

This script reads kona_captures.json and kona_synthetic_tints.json, generates
a human-friendly one-sentence description for each color entry based on its
CIELAB values, and writes the results back with the new "description" field.

Descriptions use familiar real-world comparisons (sky, grass, clay, charcoal,
cream, rose, denim, sand, rust, snow, etc.) and are intended for a person who
previously had some vision and retains memory of everyday object colors.

Usage:
    python3 scripts/generate_descriptions.py [--dry-run]
"""

import argparse
import json
import math
import pathlib
from typing import Optional, Tuple

# ---------------------------------------------------------------------------
# Lab → colour-attribute helpers
# ---------------------------------------------------------------------------

def lab_to_lch(l: float, a: float, b: float) -> Tuple[float, float, float]:
    """Convert CIELAB to LCH (lightness, chroma, hue in degrees 0-360)."""
    c = math.sqrt(a * a + b * b)
    h = math.degrees(math.atan2(b, a)) % 360.0
    return l, c, h


def _lightness_word(l: float) -> str:
    """Return a simple lightness adjective."""
    if l >= 95:
        return "very light"
    if l >= 85:
        return "light"
    if l >= 70:
        return "medium-light"
    if l >= 50:
        return "medium"
    if l >= 35:
        return "medium-dark"
    if l >= 20:
        return "dark"
    return "very dark"


def _is_near_neutral(chroma: float) -> bool:
    return chroma < 8.0


def _is_low_chroma(chroma: float) -> bool:
    return 8.0 <= chroma < 20.0


# ---------------------------------------------------------------------------
# Hue-family classification
# ---------------------------------------------------------------------------

_HUE_FAMILIES = [
    # (start, end, family)  — hue angles in degrees.
    # Boundaries calibrated against Kona swatch measurements in CIE Lab space.
    # Convention: start is inclusive, end is exclusive (start <= h < end).
    # Red wraps around 0° and is split into two entries.
    (0, 35, "red"),
    (35, 63, "orange"),
    (63, 93, "yellow"),
    (93, 112, "yellow-green"),
    (112, 162, "green"),
    (162, 200, "teal"),
    (200, 295, "blue"),
    (295, 330, "violet"),
    (330, 350, "pink"),
    (350, 360, "red"),
]


def _hue_family(h: float) -> str:
    for lo, hi, fam in _HUE_FAMILIES:
        if lo <= h < hi:
            return fam
    return "red"


# ---------------------------------------------------------------------------
# Real-world comparison lookup tables
# ---------------------------------------------------------------------------

# Keys: (hue_family, lightness_bucket) where lightness_bucket is one of:
#   "very_light", "light", "medium", "dark", "very_dark"
# Values: list of (template, chroma_range) pairs.
# chroma_range: None means any chroma, otherwise (lo, hi) inclusive.

def _lightness_bucket(l: float) -> str:
    if l >= 92:
        return "very_light"
    if l >= 72:
        return "light"
    if l >= 45:
        return "medium"
    if l >= 22:
        return "dark"
    return "very_dark"


# Mapping from (hue_family, lightness_bucket) → list of comparison strings.
# The first one whose chroma condition matches is used.
# Each entry: (description_template, min_chroma, max_chroma)
# Use {name} for the swatch name (title-cased).

_COMPARISONS = {
    # ----- RED -----
    ("red", "very_light"): [
        ("a barely-there blush like the lightest pink rose petal", 0, 30),
        ("a pale pinkish white like the inside of a seashell", 0, 50),
        ("a light rosy tint like watered-down cranberry juice on white fabric", 50, 999),
    ],
    ("red", "light"): [
        ("a warm rosy tone like sun-faded brick or a dusty rose", 0, 35),
        ("a clear rose-pink like fresh watermelon flesh", 35, 65),
        ("a vivid reddish pink like a ripe strawberry", 65, 999),
    ],
    ("red", "medium"): [
        ("a muted earthy red like aged terracotta pottery", 0, 35),
        ("a solid warm red like a ripe tomato or red bell pepper", 35, 80),
        ("an intense fiery red like a fire truck or a cardinal bird", 80, 999),
    ],
    ("red", "dark"): [
        ("a deep muted red like old brick or dried clay", 0, 35),
        ("a rich dark red like cherry wine or cranberry sauce", 35, 80),
        ("a bold deep crimson like dark velvet or polished garnet", 80, 999),
    ],
    ("red", "very_dark"): [
        ("an almost-black red like very dark dried blood or dark mahogany", 0, 50),
        ("a near-black red with a deep wine or berry undertone", 50, 999),
    ],

    # ----- ORANGE -----
    ("orange", "very_light"): [
        ("a soft peachy white like the inside of a ripe peach", 0, 35),
        ("a warm peachy cream like apricot ice cream", 35, 65),
        ("a bright pale orange like diluted tangerine juice", 65, 999),
    ],
    ("orange", "light"): [
        ("a warm sandy tone like light desert sand at sunset", 0, 30),
        ("a soft warm peach like a ripe apricot or cantaloupe", 30, 60),
        ("a bright warm orange like a clementine peel", 60, 999),
    ],
    ("orange", "medium"): [
        ("a warm earthy brown like cinnamon bark or toffee", 0, 40),
        ("a warm medium orange like butternut squash or sweet potato", 40, 75),
        ("a vivid orange like a traffic cone or a ripe pumpkin", 75, 999),
    ],
    ("orange", "dark"): [
        ("a deep brownish orange like dark caramel or old leather", 0, 40),
        ("a warm dark orange like rust on old metal", 40, 75),
        ("a rich burnt orange like the deepest autumn leaves", 75, 999),
    ],
    ("orange", "very_dark"): [
        ("an almost-black warm brown like dark espresso or very dark chocolate", 0, 50),
        ("a near-black burnt orange like deeply charred wood with a warm tint", 50, 999),
    ],

    # ----- YELLOW -----
    ("yellow", "very_light"): [
        ("a barely-there warm tint like ivory or fresh cream", 0, 25),
        ("a soft buttery pale yellow like vanilla custard", 25, 55),
        ("a sunny warm yellow like the center of a daisy", 55, 85),
        ("a bright vivid yellow like a canary or daffodil in full bloom", 85, 999),
    ],
    ("yellow", "light"): [
        ("a warm golden tone like light honey or wheat straw", 0, 35),
        ("a sunny light yellow like a ripe banana peel", 35, 70),
        ("a rich warm golden yellow like honey or butterscotch in sunlight", 70, 90),
        ("a vivid bright yellow like a sunflower or a school bus", 90, 999),
    ],
    ("yellow", "medium"): [
        ("a warm brownish gold like whole-wheat bread crust or caramel", 0, 40),
        ("a rich golden yellow like an autumn maple leaf", 40, 75),
        ("an intense saturated yellow like a lemon rind or saffron", 75, 999),
    ],
    ("yellow", "dark"): [
        ("a deep olive-gold like dark mustard or aged brass", 0, 40),
        ("a warm dark gold like dark honey or old amber", 40, 75),
        ("a deep golden amber like dark molten caramel", 75, 999),
    ],
    ("yellow", "very_dark"): [
        ("an almost-black dark olive like very old bronze in shadow", 0, 50),
        ("a near-black deep gold like dark amber resin", 50, 999),
    ],

    # ----- YELLOW-GREEN -----
    ("yellow-green", "very_light"): [
        ("a soft pale chartreuse like the lightest spring leaf", 0, 40),
        ("a bright pale yellow-green like fresh lime zest diluted in water", 40, 999),
    ],
    ("yellow-green", "light"): [
        ("a light yellow-green like a fresh pear skin or celery heart", 0, 40),
        ("a bright lime green like a tennis ball or fresh kiwi flesh", 40, 999),
    ],
    ("yellow-green", "medium"): [
        ("a warm olive-green like unripe avocado flesh", 0, 40),
        ("a vivid chartreuse like a fresh lime slice", 40, 999),
    ],
    ("yellow-green", "dark"): [
        ("a deep olive green like a ripe olive or dark pickle", 0, 40),
        ("a rich dark lime like a dense forest seen at dusk", 40, 999),
    ],
    ("yellow-green", "very_dark"): [
        ("an almost-black olive like very dark seaweed", 0, 999),
    ],

    # ----- GREEN -----
    ("green", "very_light"): [
        ("a barely-there minty tint like pale sea glass or celadon", 0, 25),
        ("a soft pale green like mint ice cream mixed with cream", 25, 50),
        ("a bright pale green like the lightest spring grass or a green apple", 50, 999),
    ],
    ("green", "light"): [
        ("a soft sage green like dried sage leaves or eucalyptus", 0, 30),
        ("a fresh medium-light green like new spring leaves", 30, 60),
        ("a vivid green like a freshly mowed lawn", 60, 999),
    ],
    ("green", "medium"): [
        ("a muted earthy green like dried herbs or moss on a shaded rock", 0, 35),
        ("a true medium green like fresh basil leaves", 35, 70),
        ("an intense vivid green like a traffic light or fresh parsley", 70, 999),
    ],
    ("green", "dark"): [
        ("a deep muted green like pine needles in winter", 0, 35),
        ("a rich dark green like a dense forest canopy", 35, 70),
        ("a deep vivid green like an emerald gemstone or Christmas tree", 70, 999),
    ],
    ("green", "very_dark"): [
        ("an almost-black green like deep evergreen in moonlight", 0, 50),
        ("a near-black vivid green like the deepest jungle shade", 50, 999),
    ],

    # ----- TEAL -----
    ("teal", "very_light"): [
        ("a soft pale aqua like very shallow tropical water over white sand", 0, 30),
        ("a bright pale teal like sea foam on a clear day", 30, 999),
    ],
    ("teal", "light"): [
        ("a soft blue-green like polished sea glass", 0, 35),
        ("a clear aqua like a swimming pool on a sunny day", 35, 999),
    ],
    ("teal", "medium"): [
        ("a muted teal like weathered copper patina", 0, 35),
        ("a vivid teal like the underside of a peacock feather or jade", 35, 999),
    ],
    ("teal", "dark"): [
        ("a deep muted teal like dark ocean water on a cloudy day", 0, 35),
        ("a rich dark teal like deep tropical sea water", 35, 999),
    ],
    ("teal", "very_dark"): [
        ("an almost-black teal like the deep ocean at night", 0, 999),
    ],

    # ----- BLUE -----
    ("blue", "very_light"): [
        ("a barely-there blue tint like a very pale winter sky", 0, 18),
        ("a soft pale blue like a clear daytime sky near the horizon", 18, 40),
        ("a bright pale blue like cornflowers fading in the sun", 40, 999),
    ],
    ("blue", "light"): [
        ("a soft powder blue like a hazy afternoon sky or faded chambray", 0, 30),
        ("a clear light blue like a robin's eggshell or periwinkle", 30, 55),
        ("a vivid sky blue like a tropical lagoon on a bright day", 55, 999),
    ],
    ("blue", "medium"): [
        ("a dusty slate blue like well-worn denim jeans", 0, 30),
        ("a solid medium blue like classic denim or blue tile", 30, 55),
        ("an intense bright blue like a sapphire or cobalt pottery", 55, 999),
    ],
    ("blue", "dark"): [
        ("a deep muted blue like faded navy fabric or dark denim", 0, 30),
        ("a rich navy blue like a dark night sky just after sunset", 30, 55),
        ("a bold deep blue like indigo dye or a midnight ocean", 55, 999),
    ],
    ("blue", "very_dark"): [
        ("an almost-black blue like the sky in deep twilight", 0, 50),
        ("a near-black vivid blue like the darkest midnight ink", 50, 999),
    ],

    # ----- VIOLET / PURPLE -----
    ("violet", "very_light"): [
        ("a barely-there lavender tint like very pale lilac blooms", 0, 25),
        ("a soft pale purple like diluted grape juice on white cloth", 25, 50),
        ("a bright pale violet like light wisteria flowers", 50, 999),
    ],
    ("violet", "light"): [
        ("a soft dusty lavender like dried lavender flowers or soft lilac", 0, 30),
        ("a clear light purple like fresh lilac blossoms", 30, 55),
        ("a vivid light violet like a bright amethyst gem", 55, 999),
    ],
    ("violet", "medium"): [
        ("a muted plum-gray like dried plum skin", 0, 30),
        ("a rich medium purple like a ripe concord grape", 30, 55),
        ("an intense vivid purple like a fresh iris or purple cabbage", 55, 999),
    ],
    ("violet", "dark"): [
        ("a deep muted eggplant like dark plum jam", 0, 30),
        ("a rich dark purple like ripe blackberries or dark wine", 30, 55),
        ("a bold deep violet like dark grape juice or a pansy center", 55, 999),
    ],
    ("violet", "very_dark"): [
        ("an almost-black purple like a very dark eggplant interior", 0, 50),
        ("a near-black vivid purple like the deepest grape skin", 50, 999),
    ],

    # ----- PINK / MAGENTA -----
    ("pink", "very_light"): [
        ("a barely-there pink like the palest pink rose petal", 0, 25),
        ("a soft pale pink like cotton candy or pink lemonade", 25, 50),
        ("a bright pale magenta like diluted fuchsia", 50, 999),
    ],
    ("pink", "light"): [
        ("a soft dusty pink like a faded rose or blush makeup", 0, 30),
        ("a clear medium pink like a pink carnation", 30, 55),
        ("a vivid hot pink like a bright azalea bloom", 55, 999),
    ],
    ("pink", "medium"): [
        ("a muted mauve like dried rose petals", 0, 30),
        ("a warm medium pink like a deep pink tulip", 30, 55),
        ("an intense magenta like a bougainvillea flower", 55, 999),
    ],
    ("pink", "dark"): [
        ("a deep dusty rose like aged wine stains on linen", 0, 30),
        ("a rich dark magenta like crushed raspberries", 30, 55),
        ("a bold deep fuchsia like dark berry juice", 55, 999),
    ],
    ("pink", "very_dark"): [
        ("an almost-black berry tone like very dark plum skin", 0, 50),
        ("a near-black magenta like concentrated blackcurrant", 50, 999),
    ],
}

# ---------------------------------------------------------------------------
# Neutral / achromatic descriptions
# ---------------------------------------------------------------------------

def _neutral_description(l: float, a: float, b: float) -> str:
    """Generate a description for a near-neutral (gray/white/black) colour."""
    warm = b > 1.5 or a > 1.5
    cool = b < -1.5 or a < -1.5

    if l >= 96:
        if warm:
            return "A crisp near-white with a very faint warm ivory undertone, like fresh cream."
        if cool:
            return "A crisp near-white with a very faint cool tint, like fresh snow in shade."
        return "A clean bright white like fresh copy paper or untouched snow."
    if l >= 90:
        if warm:
            return "A very light warm off-white like unbleached cotton or old lace."
        if cool:
            return "A very light cool off-white like winter morning frost."
        return "A pale off-white like cream or parchment paper."
    if l >= 80:
        if warm:
            return "A light warm gray like natural linen or pale oatmeal."
        if cool:
            return "A light cool gray like morning fog or pale cement."
        return "A light silver-gray like a lightly overcast sky."
    if l >= 65:
        if warm:
            return "A medium warm gray like weathered driftwood."
        if cool:
            return "A medium cool gray like polished pewter or a cloudy day."
        return "A medium gray like brushed aluminum or concrete sidewalk."
    if l >= 50:
        if warm:
            return "A warm mid-gray like aged wood or dry clay."
        if cool:
            return "A cool mid-gray like wet slate or a storm cloud."
        return "A balanced mid-gray like graphite pencil lead."
    if l >= 35:
        if warm:
            return "A dark warm gray like worn leather or wet earth."
        if cool:
            return "A dark cool gray like dark slate stone."
        return "A dark gray like an asphalt road after rain."
    if l >= 20:
        if warm:
            return "A very dark warm gray like dark roasted coffee beans."
        if cool:
            return "A very dark cool gray like dark charcoal."
        return "A deep charcoal gray like soot or coal dust."
    if l >= 10:
        return "A near-black like fresh charcoal or a moonless night sky."
    return "A deep solid black like ink or onyx stone."


# ---------------------------------------------------------------------------
# Low-chroma (muted / dusty) descriptions
# ---------------------------------------------------------------------------

def _low_chroma_description(l: float, c: float, h: float) -> str:
    """Generate a description for a low-chroma but not neutral colour."""
    family = _hue_family(h)
    bucket = _lightness_bucket(l)

    _LOW_CHROMA_MAP = {
        ("red", "very_light"): "A very pale warm white with a faint rosy blush, like the inside of a conch shell.",
        ("red", "light"): "A soft dusty rose-gray like sun-faded terracotta tile.",
        ("red", "medium"): "A muted warm brownish tone like adobe clay or old brick.",
        ("red", "dark"): "A deep muted brownish red like dark chocolate with a cherry undertone.",
        ("red", "very_dark"): "A near-black with a faint warm reddish tone, like very dark mahogany wood.",

        ("orange", "very_light"): "A warm creamy white like vanilla bean ice cream.",
        ("orange", "light"): "A soft tan like light sand on a dry beach.",
        ("orange", "medium"): "A warm earthy brown like cinnamon bark or light leather.",
        ("orange", "dark"): "A deep brown like dark leather or roasted chestnut.",
        ("orange", "very_dark"): "A near-black warm brown like espresso or very dark walnut wood.",

        ("yellow", "very_light"): "A very pale warm white like fresh buttercream or antique ivory.",
        ("yellow", "light"): "A soft warm beige like dry sand or light parchment.",
        ("yellow", "medium"): "A warm khaki or golden brown like a whole-wheat cracker.",
        ("yellow", "dark"): "A deep earthy olive-brown like dark mustard or old brass.",
        ("yellow", "very_dark"): "A near-black dark olive like very old bronze in shadow.",

        ("yellow-green", "very_light"): "A very pale warm-tinted white like light parchment or soft chamomile tea.",
        ("yellow-green", "light"): "A soft muted sage like dried celery or pale pistachio shell.",
        ("yellow-green", "medium"): "A muted olive green like dried bay leaves.",
        ("yellow-green", "dark"): "A deep dark olive like pickled green olives.",
        ("yellow-green", "very_dark"): "A near-black dark olive like very dark seaweed.",

        ("green", "very_light"): "A very pale cool white with a faint minty or celadon tint.",
        ("green", "light"): "A soft sage green like dried sage leaves or light eucalyptus.",
        ("green", "medium"): "A muted earthy green like dried thyme or weathered copper.",
        ("green", "dark"): "A deep dark green-gray like dark moss on stone.",
        ("green", "very_dark"): "A near-black green like very dark pine bark in shadow.",

        ("teal", "very_light"): "A very pale blue-green tint like ice with a hint of sea glass.",
        ("teal", "light"): "A soft muted aqua like weathered sea glass.",
        ("teal", "medium"): "A muted blue-green like aged copper patina on an old statue.",
        ("teal", "dark"): "A deep dark teal-gray like deep ocean water on a cloudy day.",
        ("teal", "very_dark"): "A near-black blue-green like the deep sea at night.",

        ("blue", "very_light"): "A very pale icy blue like a frosty winter window.",
        ("blue", "light"): "A soft blue-gray like a hazy sky or faded chambray fabric.",
        ("blue", "medium"): "A muted slate blue like well-worn denim or a gray rainy sky.",
        ("blue", "dark"): "A deep dark navy like a night sky or dark denim in shadow.",
        ("blue", "very_dark"): "A near-black blue like the sky long after sunset.",

        ("violet", "very_light"): "A very pale lavender-gray like morning mist over a lavender field.",
        ("violet", "light"): "A soft dusty lilac like dried lavender or pale wisteria.",
        ("violet", "medium"): "A muted mauve-gray like dried purple sage or twilight haze.",
        ("violet", "dark"): "A deep dark plum-gray like dark dried figs.",
        ("violet", "very_dark"): "A near-black purple like the night sky with a faint violet cast.",

        ("pink", "very_light"): "A very pale warm white with a faint pink blush, like rose quartz.",
        ("pink", "light"): "A soft dusty pink-gray like faded rose petals or old pink marble.",
        ("pink", "medium"): "A muted warm mauve like dried roses or berry-stained linen.",
        ("pink", "dark"): "A deep dark berry-gray like dried blackberry stains.",
        ("pink", "very_dark"): "A near-black with a faint berry tone, like very dark plum skin.",
    }

    key = (family, bucket)
    return _LOW_CHROMA_MAP.get(key,
        f"A subtle muted tone with a faint {family} undertone.")


# ---------------------------------------------------------------------------
# Main description generator
# ---------------------------------------------------------------------------

def generate_description(lab: dict) -> str:
    """Generate a single-sentence colour description from CIELAB values."""
    l_val = lab["l"]
    a_val = lab["a"]
    b_val = lab["b"]

    _, chroma, hue = lab_to_lch(l_val, a_val, b_val)

    # Achromatic / near-neutral
    if _is_near_neutral(chroma):
        return _neutral_description(l_val, a_val, b_val)

    # Low-chroma (muted, dusty)
    if _is_low_chroma(chroma):
        return _low_chroma_description(l_val, chroma, hue)

    # Chromatic colours — use the lookup table
    family = _hue_family(hue)
    bucket = _lightness_bucket(l_val)
    key = (family, bucket)

    entries = _COMPARISONS.get(key)
    if not entries:
        # Fallback — should not normally happen
        lw = _lightness_word(l_val)
        return f"A {lw} {family} tone."

    for desc, min_c, max_c in entries:
        if min_c <= chroma <= max_c:
            # Capitalise properly — templates start lowercase after "a/an"
            if desc[0].islower():
                desc = desc[0].upper() + desc[1:]
            return desc + "."

    # If nothing matched (shouldn't happen with 999 upper bounds), use last
    desc = entries[-1][0]
    if desc[0].islower():
        desc = desc[0].upper() + desc[1:]
    return desc + "."


# ---------------------------------------------------------------------------
# File processing
# ---------------------------------------------------------------------------

def process_json_file(filepath: pathlib.Path, dry_run: bool = False) -> int:
    """Add 'description' field to every swatch in *filepath*.

    Returns the number of swatches processed.
    """
    with open(filepath, "r", encoding="utf-8") as fh:
        data = json.load(fh)

    swatches = data.get("swatches", [])
    count = 0
    for swatch in swatches:
        lab = swatch.get("lab")
        if lab is None:
            continue
        swatch["description"] = generate_description(lab)
        count += 1

    if not dry_run:
        with open(filepath, "w", encoding="utf-8") as fh:
            json.dump(data, fh, indent=2, ensure_ascii=False)
            fh.write("\n")

    return count


# ---------------------------------------------------------------------------
# CLI
# ---------------------------------------------------------------------------

def main() -> None:
    parser = argparse.ArgumentParser(
        description="Add colour descriptions to Kona swatch JSON files.",
    )
    parser.add_argument(
        "--dry-run",
        action="store_true",
        help="Print stats without modifying the files.",
    )
    parser.add_argument(
        "--captures",
        type=pathlib.Path,
        default=pathlib.Path("kona_captures.json"),
        help="Path to kona_captures.json (default: %(default)s)",
    )
    parser.add_argument(
        "--synthetic",
        type=pathlib.Path,
        default=pathlib.Path("kona_synthetic_tints.json"),
        help="Path to kona_synthetic_tints.json (default: %(default)s)",
    )
    args = parser.parse_args()

    mode = "DRY RUN" if args.dry_run else "WRITE"

    for label, path in [("captures", args.captures), ("synthetic", args.synthetic)]:
        if not path.exists():
            print(f"  SKIP {label}: {path} not found")
            continue
        n = process_json_file(path, dry_run=args.dry_run)
        print(f"  [{mode}] {label}: {n} descriptions in {path}")


if __name__ == "__main__":
    main()
