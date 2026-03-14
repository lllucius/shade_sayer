#!/usr/bin/env python3
"""
Import xkcd color survey colors from CSV file.

This script:
1. Reads color data from the xkcd color survey CSV file (xkcd.csv)
2. Extracts color names with their hex RGB values
3. Converts RGB to CIELAB using sRGB D65 color space
4. Outputs to JSON format for use with generate_color_database.py

The xkcd color survey (https://xkcd.com/color/rgb/) contains 949 named colors
based on a survey of over 200,000 people, with more understandable color names
than traditional paint color databases.

Usage:
    python import_xkcd_colors.py [--input FILE] [--output FILE]

Arguments:
    --input FILE    Input CSV file (default: xkcd.csv)
    --output FILE   Output JSON file (default: xkcd_colors.json)
"""

import argparse
import json
import os
import sys

# CIE LAB conversion constants
# Threshold for linear vs cubic root in XYZ to LAB conversion
# Derived from (24/116)^3 ≈ 0.008856
CIE_EPSILON = 0.008856
# Constant kappa = 903.3 is derived from (116/16)^3 * epsilon
CIE_KAPPA = 903.3


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


def hex_to_rgb(hex_color):
    """Convert hex color string to RGB tuple."""
    # Remove leading '#' if present
    hex_color = hex_color.lstrip('#')
    
    # Parse hex to RGB
    r = int(hex_color[0:2], 16)
    g = int(hex_color[2:4], 16)
    b = int(hex_color[4:6], 16)
    
    return r, g, b


def load_colors_from_csv(filename):
    """Load xkcd colors from the CSV file."""
    colors = {}
    
    with open(filename, 'r', encoding='utf-8') as f:
        for line in f:
            line = line.strip()
            if not line:
                continue
            
            # Parse CSV format: name,#hexcolor,
            parts = line.split(',')
            if len(parts) < 2:
                continue
            
            name = parts[0].strip()
            hex_color = parts[1].strip()
            
            if not name or not hex_color:
                continue
            
            # Convert hex to RGB
            try:
                r, g, b = hex_to_rgb(hex_color)
            except (ValueError, IndexError):
                print(f"Warning: Invalid hex color '{hex_color}' for '{name}', skipping")
                continue
            
            # Validate RGB range
            if not (0 <= r <= 255 and 0 <= g <= 255 and 0 <= b <= 255):
                continue
            
            # Title case the name for better readability
            # e.g., "cloudy blue" -> "Cloudy Blue"
            name = name.title()
            
            # Only keep the first occurrence of each color name
            if name not in colors:
                colors[name] = (r, g, b)
    
    return colors


def main():
    parser = argparse.ArgumentParser(
        description='Import xkcd color survey colors from CSV file.'
    )
    parser.add_argument(
        '--input', '-i',
        default=None,
        help='Input CSV file (default: xkcd.csv)'
    )
    parser.add_argument(
        '--output', '-o',
        default='xkcd_colors.json',
        help='Output JSON file (default: xkcd_colors.json)'
    )
    
    args = parser.parse_args()
    
    # Default input path
    script_dir = os.path.dirname(os.path.abspath(__file__))
    if args.input is None:
        args.input = os.path.join(script_dir, 'xkcd.csv')
    
    # Default output path
    if not os.path.isabs(args.output):
        args.output = os.path.join(script_dir, args.output)
    
    # Load colors from CSV
    print(f"Loading colors from {args.input}...")
    rgb_colors = load_colors_from_csv(args.input)
    print(f"Loaded {len(rgb_colors)} unique colors")
    
    # Convert to LAB
    print("Converting RGB to LAB...")
    colors = []
    for name, (r, g, b) in rgb_colors.items():
        L, a, b_val = rgb_to_lab(r, g, b)
        
        colors.append({
            'name': name,
            'L': round(L, 2),
            'a': round(a, 2),
            'b': round(b_val, 2)
        })
    
    # Sort by lightness
    colors.sort(key=lambda c: c['L'])
    
    # Output
    output = {
        'colors': colors,
        'metadata': {
            'source': 'xkcd color survey (https://xkcd.com/color/rgb/)',
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
