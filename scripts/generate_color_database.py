#!/usr/bin/env python3
"""
Generate color_database.cpp from xkcd colors JSON data.

This script:
1. Reads color data from a JSON file containing xkcd color survey colors
2. Computes chroma (C*) and hue angle (h*) for each color from L*a*b* values
3. Sorts colors by lightness for efficient binary search
4. Balances hue distribution when limiting the number of colors
5. Outputs a C++ source file with pre-computed values

Usage:
    python generate_color_database.py [--colors N] [--input FILE] [--output FILE]

Arguments:
    --colors N      Number of colors to include (default: all)
    --input FILE    Input JSON file (default: xkcd_colors.json)
    --output FILE   Output C++ file (default: color_database.cpp)
"""

import argparse
import json
import math
import os
import sys
from dataclasses import dataclass
from typing import List, Optional


@dataclass
class Color:
    """Represents a color with name, LAB values, and derived values."""
    name: str
    l: float  # Lightness (0-100)
    a: float  # Green-red axis
    b: float  # Blue-yellow axis
    chroma: float = 0.0  # sqrt(a² + b²)
    hue_angle: float = 0.0  # atan2(b, a) in radians
    
    def __post_init__(self):
        """Compute chroma and hue angle from a* and b*."""
        self.chroma = math.sqrt(self.a * self.a + self.b * self.b)
        if self.chroma > 0.001:  # Avoid atan2 issues for neutral colors
            self.hue_angle = math.atan2(self.b, self.a)
        else:
            self.hue_angle = 0.0
    
    def hue_degrees(self) -> float:
        """Get hue angle in degrees (0-360)."""
        h = math.degrees(self.hue_angle)
        if h < 0:
            h += 360
        return h


def load_colors_from_json(filename: str) -> List[Color]:
    """Load colors from a JSON file."""
    with open(filename, 'r', encoding='utf-8') as f:
        data = json.load(f)
    
    colors = []
    for item in data.get('colors', data):
        # Support both formats: {"name": ..., "lab": [L, a, b]} and {"name": ..., "L": ..., "a": ..., "b": ...}
        if 'lab' in item:
            lab = item['lab']
            l, a, b = lab[0], lab[1], lab[2]
        else:
            l, a, b = item['L'], item['a'], item['b']
        
        colors.append(Color(
            name=item['name'],
            l=l,
            a=a,
            b=b
        ))
    
    return colors


def balance_hue_distribution(colors: List[Color], target_count: int) -> List[Color]:
    """
    Select colors to maintain a balanced hue distribution.
    
    Strategy:
    1. Divide hue circle into 12 bins (30° each)
    2. Divide lightness into 10 bins (10 L* units each)
    3. Select proportionally from each bin to maintain diversity
    4. Prioritize named Resene colors (non-synthetic)
    """
    if len(colors) <= target_count:
        return colors
    
    # Number of hue bins and lightness bins
    HUE_BINS = 12
    LIGHTNESS_BINS = 10
    
    # Create bins: [hue_bin][lightness_bin] = list of colors
    bins = [[[] for _ in range(LIGHTNESS_BINS)] for _ in range(HUE_BINS)]
    neutral_bin = []  # For achromatic colors (low chroma)
    
    CHROMA_THRESHOLD = 5.0  # Below this, consider color neutral/gray
    
    for color in colors:
        l_bin = min(int(color.l / 10), LIGHTNESS_BINS - 1)
        
        if color.chroma < CHROMA_THRESHOLD:
            neutral_bin.append(color)
        else:
            h_deg = color.hue_degrees()
            h_bin = int(h_deg / 30) % HUE_BINS
            bins[h_bin][l_bin].append(color)
    
    # Calculate how many colors to take from each area
    # Reserve ~10% for neutral colors
    neutral_target = max(int(target_count * 0.1), 10)
    chromatic_target = target_count - min(neutral_target, len(neutral_bin))
    
    # Count total chromatic colors
    total_chromatic = sum(len(bins[h][l]) for h in range(HUE_BINS) for l in range(LIGHTNESS_BINS))
    
    selected = []
    
    # Select chromatic colors proportionally from each bin
    for h in range(HUE_BINS):
        for l in range(LIGHTNESS_BINS):
            bin_colors = bins[h][l]
            if not bin_colors:
                continue
            
            # Calculate proportional allocation
            proportion = len(bin_colors) / total_chromatic
            allocation = max(1, int(chromatic_target * proportion))
            
            # Sort by uniqueness: prefer named colors, diverse chromas
            bin_colors.sort(key=lambda c: (
                # Prefer actual Resene names (not synthetic like "Dark Vivid Red")
                0 if ' ' not in c.name or any(x in c.name for x in ['Vivid', 'Medium', 'Pale', 'Soft', 'Light', 'Dark', 'Deep', 'Mid']) else 1,
                -c.chroma  # Higher chroma more distinctive
            ))
            
            selected.extend(bin_colors[:allocation])
    
    # Add neutral colors
    neutral_bin.sort(key=lambda c: c.l)  # Sort by lightness
    step = max(1, len(neutral_bin) // neutral_target)
    selected.extend(neutral_bin[::step][:neutral_target])
    
    # If we have too few or too many, adjust
    if len(selected) < target_count:
        # Add more colors from bins with remaining colors
        remaining = []
        for h in range(HUE_BINS):
            for l in range(LIGHTNESS_BINS):
                for color in bins[h][l]:
                    if color not in selected:
                        remaining.append(color)
        
        remaining.sort(key=lambda c: c.chroma, reverse=True)
        selected.extend(remaining[:target_count - len(selected)])
    
    elif len(selected) > target_count:
        # Remove excess, preferring to keep diverse hues
        selected.sort(key=lambda c: (int(c.hue_degrees() / 30), c.l))
        selected = selected[:target_count]
    
    return selected


def generate_cpp_file(colors: List[Color], output_path: str):
    """Generate the C++ color_database.cpp file."""
    
    # Sort by lightness for binary search
    colors.sort(key=lambda c: c.l)
    
    cpp_content = '''#include "color_database.h"
#include "color_math.h"
#include <math.h>
#include <limits.h>
#include <float.h>
#include <string.h>

// xkcd Color Survey Database
// Colors from the xkcd color survey (https://xkcd.com/color/rgb/)
// Colors stored directly in LAB format for perceptual accuracy
// LAB values pre-computed from RGB using sRGB D65 color space
// SORTED BY LIGHTNESS (L*) from darkest to brightest for efficient binary search
// Each entry includes pre-computed chroma and hue angle for fast color matching
// Generated by generate_color_database.py
static const color_entry_t xkcd_colors[] =
{
'''
    
    for i, color in enumerate(colors):
        # Escape special characters in name
        name = color.name.replace('"', '\\"')
        
        # Format the entry with pre-computed chroma and hue (no description)
        cpp_content += f'    {{"{name}", {{{color.l:.2f}f, {color.a:.2f}f, {color.b:.2f}f}}, {color.chroma:.2f}f, {color.hue_angle:.4f}f}}'
        
        if i < len(colors) - 1:
            cpp_content += ','
        cpp_content += '\n'
    
    cpp_content += '''};

static const int num_colors = sizeof(xkcd_colors) / sizeof(color_entry_t);

/* The database is pre-sorted by lightness (L*) in the source code */
static const bool database_sorted = true;

void color_database_init(void)
{
    // Colors are already in LAB format and pre-sorted by lightness - no work needed!
    // Chroma and hue angle are pre-computed - no runtime calculation needed!
    // This function is kept for API compatibility
}

const char* find_closest_color_lab(const lab_t *lab, float *out_delta_e)
{
    float min_delta_e = FLT_MAX;
    int closest_index = 0;

    for (int i = 0; i < num_colors; i++)
    {
        float delta_e = color_math_delta_e_ciede2000(lab, &xkcd_colors[i].lab);
        if (delta_e < min_delta_e)
        {
            min_delta_e = delta_e;
            closest_index = i;
        }
    }

    if (out_delta_e != nullptr)
    {
        *out_delta_e = min_delta_e;
    }

    return xkcd_colors[closest_index].name;
}

const char* find_closest_color_lab_with_runner_up(const lab_t *lab, 
                                                   float *best_delta_e, 
                                                   float *second_best_delta_e)
{
    float min_delta_e = FLT_MAX;
    float second_min_delta_e = FLT_MAX;
    int closest_index = 0;

    for (int i = 0; i < num_colors; i++)
    {
        float delta_e = color_math_delta_e_ciede2000(lab, &xkcd_colors[i].lab);
        if (delta_e < min_delta_e)
        {
            // New best match found
            second_min_delta_e = min_delta_e;  // Previous best becomes second best
            min_delta_e = delta_e;
            closest_index = i;
        }
        else if (delta_e < second_min_delta_e)
        {
            // New second best match found
            second_min_delta_e = delta_e;
        }
    }

    if (best_delta_e != nullptr)
    {
        *best_delta_e = min_delta_e;
    }
    
    if (second_best_delta_e != nullptr)
    {
        *second_best_delta_e = second_min_delta_e;
    }

    return xkcd_colors[closest_index].name;
}

const char* find_closest_color(uint8_t r, uint8_t g, uint8_t b)
{
    // Convert RGB to LAB for matching
    // Note: This function is maintained for backward compatibility,
    // but direct LAB matching with find_closest_color_lab() is preferred
    lab_t lab = color_math_rgb_to_lab(r, g, b);
    return find_closest_color_lab(&lab, nullptr);
}

esp_err_t get_color_lab(const char *color_name, lab_t *out_lab)
{
    if (color_name == nullptr)
    {
        return ESP_ERR_INVALID_ARG;
    }

    // Linear search through the color database
    for (int i = 0; i < num_colors; i++)
    {
        if (strcasecmp(xkcd_colors[i].name, color_name) == 0)
        {
            if (out_lab != nullptr)
            {
                *out_lab = xkcd_colors[i].lab;
            }
            return ESP_OK;
        }
    }

    return ESP_ERR_NOT_FOUND;
}

uint32_t color_database_get_count(void)
{
    return num_colors;
}

esp_err_t color_database_get_entry(uint32_t index, char *name, size_t name_size, lab_t *lab)
{
    if (index >= num_colors)
    {
        return ESP_ERR_INVALID_ARG;
    }
    
    /* Array is pre-sorted by lightness, so direct indexing works */
    if (name && name_size > 0)
    {
        strncpy(name, xkcd_colors[index].name, name_size - 1);
        name[name_size - 1] = '\\0';
    }
    
    if (lab)
    {
        *lab = xkcd_colors[index].lab;
    }
    
    return ESP_OK;
}

const char* color_database_get_name(uint32_t index)
{
    if (index >= num_colors)
    {
        return NULL;
    }
    
    /* Array is pre-sorted by lightness, so direct indexing works */
    return xkcd_colors[index].name;
}

void color_database_sort_by_lightness(void)
{
    /* Array is pre-sorted in source code - nothing to do */
    /* This function is kept for API compatibility */
}

bool color_database_is_sorted(void)
{
    return database_sorted;
}

float color_database_get_lightness(uint32_t index)
{
    if (index >= num_colors)
    {
        return -1.0f;
    }
    
    /* Array is pre-sorted by lightness, so direct indexing works */
    return xkcd_colors[index].lab.l;
}

float color_database_get_chroma(uint32_t index)
{
    if (index >= num_colors)
    {
        return -1.0f;
    }
    
    return xkcd_colors[index].chroma;
}

float color_database_get_hue(uint32_t index)
{
    if (index >= num_colors)
    {
        return -1000.0f;  // Invalid hue marker
    }
    
    return xkcd_colors[index].hue;
}
'''
    
    with open(output_path, 'w', encoding='utf-8') as f:
        f.write(cpp_content)
    
    print(f"Generated {output_path} with {len(colors)} colors")


def main():
    parser = argparse.ArgumentParser(
        description='Generate color_database.cpp from xkcd colors JSON data.'
    )
    parser.add_argument(
        '--colors', '-n',
        type=int,
        default=None,
        help='Number of colors to include (default: all)'
    )
    parser.add_argument(
        '--input', '-i',
        default='xkcd_colors.json',
        help='Input JSON file (default: xkcd_colors.json)'
    )
    parser.add_argument(
        '--output', '-o',
        default=None,
        help='Output C++ file (default: ../main/color_database.cpp)'
    )
    
    args = parser.parse_args()
    
    # Default output path
    script_dir = os.path.dirname(os.path.abspath(__file__))
    if args.output is None:
        args.output = os.path.join(script_dir, '..', 'main', 'color_database.cpp')
    
    # Default input path
    if not os.path.isabs(args.input):
        args.input = os.path.join(script_dir, args.input)
    
    # Load colors
    print(f"Loading colors from {args.input}...")
    colors = load_colors_from_json(args.input)
    print(f"Loaded {len(colors)} colors")
    
    # Balance hue distribution if limiting colors
    if args.colors is not None and args.colors < len(colors):
        print(f"Selecting {args.colors} colors with balanced hue distribution...")
        colors = balance_hue_distribution(colors, args.colors)
        print(f"Selected {len(colors)} colors")
    
    # Generate output file
    generate_cpp_file(colors, args.output)
    
    return 0


if __name__ == '__main__':
    sys.exit(main())
