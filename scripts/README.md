# Color Database Generation Scripts

This directory contains Python scripts for generating and managing the xkcd color survey database.

## Scripts

### import_xkcd_colors.py

**Primary script** that imports xkcd color survey colors from the CSV file.

**Usage:**
```bash
# Import from the xkcd CSV file
python3 import_xkcd_colors.py

# Specify input/output files
python3 import_xkcd_colors.py --input xkcd.csv --output xkcd_colors.json
```

**Features:**
- Reads xkcd color survey colors from CSV file (xkcd.csv)
- Extracts color names with hex RGB values
- Converts RGB to CIELAB using sRGB D65 color space
- Title-cases color names for better readability
- Outputs to JSON format

**Requirements:**
- Python 3.8+

### generate_color_database.py

Generates `color_database.cpp` from a JSON color file.

**Usage:**
```bash
# Generate with all colors
python3 generate_color_database.py --input xkcd_colors.json

# Generate with limited colors (balanced hue distribution)
python3 generate_color_database.py --colors 500 --input xkcd_colors.json

# Specify output file
python3 generate_color_database.py --output /path/to/color_database.cpp
```

**Features:**
- Reads CIELAB color data from JSON
- Computes chroma (C*) and hue angle (h*) for each color
- Sorts colors by lightness for efficient binary search
- Balances hue distribution when limiting colors
- Generates C++ source with pre-computed values

### generate_vptree.py

Generates a pre-computed VP-Tree (Vantage Point Tree) for O(log n) color matching.

**Usage:**
```bash
# Generate VP-Tree header
python3 generate_vptree.py

# Specify input/output files
python3 generate_vptree.py --input xkcd_colors.json --output ../main/vptree_data.h
```

**Features:**
- Builds VP-Tree using CIEDE2000 as the distance metric
- Outputs const data structure stored in Flash (zero runtime allocations)
- Average search complexity: O(log n) instead of O(n)
- For 949 colors: ~10 CIEDE2000 calculations per query vs ~949 for linear search

**Generated Output:**
- `vptree_data.h`: Header file with const VP-Tree nodes
- Flash usage: ~9.3 KB for 949 colors

### Legacy Scripts

- **import_resene_colors.py**: Previous import script for Resene paint colors from Excel.
  This script is kept for reference but is no longer used in the main build.
- **analyze_calibration_log.py**: Parses `console.txt` auto-calibration logs and compares optimizer ΔE with post-calibration measurement ΔE.

  **Usage:**
  ```bash
  python3 analyze_calibration_log.py ../console.txt
  ```

## Data Files

### xkcd_colors.json

Complete color database with xkcd color survey colors. Each entry contains:
- `name`: Color name from the survey (Title Cased)
- `L`, `a`, `b`: CIELAB color coordinates

### xkcd.csv

Source CSV file with xkcd color survey data. Format: `name,#hexcolor,`

### resene_colors.json

Legacy Resene color database (kept for reference).

## Regenerating the Database

To regenerate the color database from the xkcd CSV file:

```bash
cd scripts

# Step 1: Import xkcd colors from CSV
python3 import_xkcd_colors.py

# Step 2: Generate the C++ database
python3 generate_color_database.py

# Step 3: Generate the VP-Tree for fast matching
python3 generate_vptree.py
```

## Color Entry Format

The generated C++ code uses this structure:

```cpp
typedef struct {
    const char *name;
    lab_t lab;        // L*, a*, b* coordinates
    float chroma;     // Pre-computed sqrt(a*a + b*b)
    float hue;        // Pre-computed atan2(b, a) in radians
} color_entry_t;
```

## VP-Tree Node Format

The VP-Tree uses this structure (stored in Flash):

```cpp
typedef struct {
    int16_t color_index;       // Index into color database
    float median_distance;     // CIEDE2000 distance threshold
    int16_t left_child;        // Left child index (-1 if none)
    int16_t right_child;       // Right child index (-1 if none)
} vptree_node_t;
```

## Dependencies

- Python 3.8+
