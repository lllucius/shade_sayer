#!/usr/bin/env python3
"""
Import official Resene colors from the Excel spreadsheet.

This script:
1. Reads color data from the official Resene Excel file (resene.xls)
2. Extracts unique color names with their RGB values
3. Converts RGB to CIELAB using sRGB D65 color space
4. Generates natural language descriptions
5. Outputs to JSON format for use with generate_color_database.py

Usage:
    python import_resene_colors.py [--input FILE] [--output FILE]

Arguments:
    --input FILE    Input XLS file (default: ../resene.xls)
    --output FILE   Output JSON file (default: resene_colors.json)

Requirements:
    xlrd package: pip install xlrd
    (Note: xlrd only supports .xls format, not .xlsx)
"""

import argparse
import json
import math
import os
import sys

try:
    import xlrd
except ImportError:
    print("Error: xlrd package required.")
    print("Install with: pip install xlrd")
    print("Note: xlrd only supports legacy .xls format, not .xlsx")
    sys.exit(1)

# CIE LAB conversion constants
# Threshold for linear vs cubic root in XYZ to LAB conversion
# Derived from (24/116)^3 ≈ 0.008856
CIE_EPSILON = 0.008856
# Constant kappa = 903.3 is derived from (116/16)^3 * epsilon
CIE_KAPPA = 903.3

# Color description constants
# Chroma threshold for neutral color detection (below this is gray)
NEUTRAL_CHROMA_THRESHOLD = 10
# Chroma threshold for pure neutral (completely achromatic)
PURE_NEUTRAL_CHROMA_THRESHOLD = 2
# Max lightness value (101 to handle floating point precision at L*=100)
MAX_LIGHTNESS = 101
# Lightness thresholds for pure black and white
BLACK_LIGHTNESS_THRESHOLD = 5
WHITE_LIGHTNESS_THRESHOLD = 95


def srgb_to_linear(c):
    """Convert sRGB component (0-255) to linear RGB (0-1)."""
    c = c / 255.0
    if c <= 0.04045:
        return c / 12.92
    else:
        return pow((c + 0.055) / 1.055, 2.4)


def rgb_to_xyz(r, g, b):
    """Convert sRGB to XYZ using D65 illuminant."""
    # Linearize sRGB
    r_lin = srgb_to_linear(r)
    g_lin = srgb_to_linear(g)
    b_lin = srgb_to_linear(b)
    
    # sRGB to XYZ matrix (D65)
    x = r_lin * 0.4124564 + g_lin * 0.3575761 + b_lin * 0.1804375
    y = r_lin * 0.2126729 + g_lin * 0.7151522 + b_lin * 0.0721750
    z = r_lin * 0.0193339 + g_lin * 0.1191920 + b_lin * 0.9503041
    
    return x, y, z


def xyz_to_lab(x, y, z):
    """Convert XYZ to CIELAB using D65 reference white."""
    # D65 reference white
    ref_x = 0.95047
    ref_y = 1.00000
    ref_z = 1.08883
    
    x = x / ref_x
    y = y / ref_y
    z = z / ref_z
    
    def f(t):
        if t > CIE_EPSILON:
            return pow(t, 1/3)
        else:
            return (CIE_KAPPA * t + 16) / 116
    
    fx = f(x)
    fy = f(y)
    fz = f(z)
    
    L = 116 * fy - 16
    a = 500 * (fx - fy)
    b = 200 * (fy - fz)
    
    return L, a, b


def rgb_to_lab(r, g, b):
    """Convert sRGB to CIELAB."""
    x, y, z = rgb_to_xyz(r, g, b)
    return xyz_to_lab(x, y, z)


# Hue names based on CIELAB hue angle (degrees)
# More specific distinctions for natural color naming
HUE_NAMES = [
    (0, 10, "red"),
    (10, 25, "vermilion"),
    (25, 45, "orange"),
    (45, 60, "amber"),
    (60, 80, "gold"),
    (80, 95, "yellow"),
    (95, 110, "lime"),
    (110, 130, "chartreuse"),
    (130, 155, "green"),
    (155, 175, "teal"),
    (175, 195, "cyan"),
    (195, 215, "azure"),
    (215, 235, "cerulean"),
    (235, 260, "blue"),
    (260, 280, "indigo"),
    (280, 300, "violet"),
    (300, 320, "purple"),
    (320, 340, "magenta"),
    (340, 355, "crimson"),
    (355, 360, "red"),
]

# Color associations for descriptions - expanded with more variety
COLOR_ASSOCIATIONS = {
    "red": ["roses", "cherries", "ruby", "cardinals", "poppies"],
    "vermilion": ["autumn leaves", "persimmons", "cinnabar", "ripe tomatoes"],
    "orange": ["oranges", "pumpkins", "marigolds", "tangerines", "sunsets"],
    "amber": ["honey", "maple syrup", "topaz", "autumn foliage"],
    "gold": ["ripe wheat", "sunlight", "bullion", "canary feathers"],
    "yellow": ["sunflowers", "lemons", "bananas", "buttercups", "daffodils"],
    "lime": ["spring buds", "new leaves", "granny smith apples", "limeade"],
    "chartreuse": ["absinthe", "fireflies", "spring grass", "new growth"],
    "green": ["emeralds", "grass", "forest", "jade", "clover"],
    "teal": ["peacock feathers", "sea glass", "mallard plumage", "lagoons"],
    "cyan": ["turquoise", "tropical waters", "glacial ice", "swimming pools"],
    "azure": ["clear skies", "forget-me-nots", "robin eggs", "powder blue"],
    "cerulean": ["ocean depths", "twilight skies", "morning glories"],
    "blue": ["sapphires", "cornflowers", "denim", "blueberries"],
    "indigo": ["midnight skies", "deep water", "ink", "dark denim"],
    "violet": ["lavender", "amethyst", "wisteria", "irises"],
    "purple": ["plums", "grapes", "eggplants", "royalty robes"],
    "magenta": ["orchids", "fuchsia flowers", "bougainvillea", "lipstick"],
    "crimson": ["blood oranges", "garnets", "wine", "poinsettias"],
}

# Neutral color associations - expanded for smart neutral handling
# Note: Last range uses MAX_LIGHTNESS (101) to capture L=100.x floating point values
NEUTRAL_ASSOCIATIONS = {
    (0, 5): ["jet", "obsidian", "coal"],
    (5, 15): ["charcoal", "soot", "onyx"],
    (15, 30): ["graphite", "slate", "dark stone"],
    (30, 50): ["pewter", "concrete", "storm clouds"],
    (50, 70): ["dove", "ash", "silver"],
    (70, 85): ["pearl", "cloud", "mist"],
    (85, 95): ["snow", "cream", "ivory"],
    (95, MAX_LIGHTNESS): ["chalk", "fresh snow", "linen"],
}

# Warm and cool hue sets for tinted neutral detection
WARM_HUES = {"red", "vermilion", "orange", "amber", "gold", "yellow", "crimson", "magenta"}
COOL_HUES = {"lime", "chartreuse", "green", "teal", "cyan", "azure", "cerulean", "blue", "indigo", "violet", "purple"}


def get_hue_name(hue_degrees):
    """Get the hue name for a given hue angle in degrees."""
    h = hue_degrees % 360
    
    for start, end, name in HUE_NAMES:
        if start <= h < end:
            return name
    
    return "red"


def get_tone_descriptor(L, chroma):
    """
    Get a combined tone descriptor based on lightness and chroma.
    
    This provides more natural descriptions than simple concatenation.
    Returns a single tone word string (e.g., "pale", "vibrant", "deep").
    """
    # High lightness (L >= 70)
    if L >= 70:
        if chroma < 20:
            return "pale"
        elif chroma < 40:
            return "pastel"
        elif chroma < 65:
            return "bright"
        else:
            return "vibrant"
    
    # Medium-high lightness (L 50-70)
    elif L >= 50:
        if chroma < 20:
            return "soft"
        elif chroma < 40:
            return "moderate"
        elif chroma < 65:
            return "vivid"
        else:
            return "electric"
    
    # Medium-low lightness (L 30-50)
    elif L >= 30:
        if chroma < 20:
            return "muted"
        elif chroma < 40:
            return "dusty"
        elif chroma < 65:
            return "rich"
        else:
            return "saturated"
    
    # Low lightness (L < 30)
    else:
        if chroma < 20:
            return "dark"
        elif chroma < 40:
            return "deep"
        elif chroma < 65:
            return "intense"
        else:
            return "bold"


def get_neutral_description(L, a, b, chroma):
    """
    Generate description for neutral or near-neutral colors.
    
    Handles:
    - True neutrals (chroma < PURE_NEUTRAL_CHROMA_THRESHOLD): pure black, white, or gray
    - Tinted neutrals (chroma up to NEUTRAL_CHROMA_THRESHOLD): warm gray or cool gray
    """
    # True neutrals (completely achromatic)
    if chroma < PURE_NEUTRAL_CHROMA_THRESHOLD:
        if L < BLACK_LIGHTNESS_THRESHOLD:
            return "Pure black"
        elif L > WHITE_LIGHTNESS_THRESHOLD:
            return "Pure white"
        else:
            # Find matching neutral association
            for (start, end), items in NEUTRAL_ASSOCIATIONS.items():
                if start <= L < end:
                    return f"A pure gray like {items[0]}"
            return "A pure neutral gray"
    
    # Tinted neutrals (slightly chromatic grays) - calculate hue
    hue_rad = math.atan2(b, a)
    hue_deg = math.degrees(hue_rad)
    if hue_deg < 0:
        hue_deg += 360
    hue_name = get_hue_name(hue_deg)
    
    if hue_name in WARM_HUES:
        temp_desc = "warm"
    elif hue_name in COOL_HUES:
        temp_desc = "cool"
    else:
        temp_desc = "neutral"
    
    # Get lightness-based association
    for (start, end), items in NEUTRAL_ASSOCIATIONS.items():
        if start <= L < end:
            return f"A {temp_desc} gray like {items[0]}"
    
    return f"A {temp_desc} gray"


def generate_description(L, a, b):
    """
    Generate a human-readable description of a color based on its LAB values.
    
    NOTE: This function is no longer used in the main build. The description
    generation has been migrated to the C++ module main/color_description.cpp
    for runtime generation on double-click. This Python implementation is kept
    for reference and for offline color database tooling.
    """
    chroma = math.sqrt(a * a + b * b)
    
    # Handle neutral and near-neutral colors
    if chroma < NEUTRAL_CHROMA_THRESHOLD:
        return get_neutral_description(L, a, b, chroma)
    
    # Chromatic colors
    hue_rad = math.atan2(b, a)
    hue_deg = math.degrees(hue_rad)
    if hue_deg < 0:
        hue_deg += 360
    
    hue_name = get_hue_name(hue_deg)
    tone_desc = get_tone_descriptor(L, chroma)
    
    # Get color associations with fallback
    associations = COLOR_ASSOCIATIONS.get(hue_name, ["objects of this hue"])
    assoc_str = associations[0]
    
    # Determine warm/cool temperature note
    if hue_name in WARM_HUES:
        temp = " - a warm color"
    elif hue_name in COOL_HUES:
        temp = " - a cool color"
    else:
        temp = ""
    
    # Use "an" for tones starting with a vowel
    article = "An" if tone_desc[0] in "aeiou" else "A"
    
    return f"{article} {tone_desc} {hue_name} like {assoc_str}{temp}"


def is_missing_value(value):
    """Check if a cell value represents a missing/empty value."""
    if value is None:
        return True
    if isinstance(value, str):
        stripped = value.strip()
        return stripped == '' or stripped == '-' or stripped.lower() == 'nan'
    # Check for NaN (Not a Number) in floats
    if isinstance(value, float) and math.isnan(value):
        return True
    return False


def load_colors_from_xls(filename):
    """Load official Resene colors from the Excel file."""
    workbook = xlrd.open_workbook(filename)
    sheet = workbook.sheet_by_index(0)
    
    colors = {}
    
    # Data starts from row 9 (index 9)
    # Columns: 0=name, 7=R, 8=G, 9=B
    for i in range(9, sheet.nrows):
        name = sheet.cell_value(i, 0)
        r_val = sheet.cell_value(i, 7)
        g_val = sheet.cell_value(i, 8)
        b_val = sheet.cell_value(i, 9)
        
        # Skip if name is empty or RGB values are missing
        if not name or is_missing_value(r_val) or is_missing_value(g_val) or is_missing_value(b_val):
            continue
        
        try:
            r = int(r_val)
            g = int(g_val)
            b = int(b_val)
        except (ValueError, TypeError):
            continue
        
        # Validate RGB range
        if not (0 <= r <= 255 and 0 <= g <= 255 and 0 <= b <= 255):
            continue
        
        # Strip whitespace from name
        name = name.strip()
        
        # Only keep the first occurrence of each color name
        if name not in colors:
            colors[name] = (r, g, b)
    
    return colors


def main():
    parser = argparse.ArgumentParser(
        description='Import official Resene colors from Excel spreadsheet.'
    )
    parser.add_argument(
        '--input', '-i',
        default=None,
        help='Input XLS file (default: ../resene.xls)'
    )
    parser.add_argument(
        '--output', '-o',
        default='resene_colors.json',
        help='Output JSON file (default: resene_colors.json)'
    )
    
    args = parser.parse_args()
    
    # Default input path
    script_dir = os.path.dirname(os.path.abspath(__file__))
    if args.input is None:
        args.input = os.path.join(script_dir, '..', 'resene.xls')
    
    # Default output path
    if not os.path.isabs(args.output):
        args.output = os.path.join(script_dir, args.output)
    
    # Load colors from XLS
    print(f"Loading colors from {args.input}...")
    rgb_colors = load_colors_from_xls(args.input)
    print(f"Loaded {len(rgb_colors)} unique colors")
    
    # Convert to LAB and generate descriptions
    print("Converting RGB to LAB and generating descriptions...")
    colors = []
    for name, (r, g, b) in rgb_colors.items():
        L, a, b_val = rgb_to_lab(r, g, b)
        description = generate_description(L, a, b_val)
        
        colors.append({
            'name': name,
            'L': round(L, 2),
            'a': round(a, 2),
            'b': round(b_val, 2),
            'description': description
        })
    
    # Sort by lightness
    colors.sort(key=lambda c: c['L'])
    
    # Output
    output = {
        'colors': colors,
        'metadata': {
            'source': 'Official Resene color palette (Jul 2016)',
            'color_space': 'CIELAB D65',
            'total_colors': len(colors),
            'conversion': 'RGB (sRGB) -> XYZ -> CIELAB (D65 illuminant)'
        }
    }
    
    with open(args.output, 'w', encoding='utf-8') as f:
        json.dump(output, f, indent=2)
    
    print(f"Wrote {len(colors)} colors to {args.output}")
    
    # Print some stats
    if colors:
        print(f"\nLightness range: {colors[0]['L']} to {colors[-1]['L']}")
        print(f"First color: {colors[0]['name']} (L={colors[0]['L']})")
        print(f"Last color: {colors[-1]['name']} (L={colors[-1]['L']})")
    
    return 0


if __name__ == '__main__':
    sys.exit(main())
