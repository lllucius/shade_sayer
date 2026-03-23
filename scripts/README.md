# Color Database Generation Scripts

This directory contains Python scripts for generating and managing the xkcd color survey database.

## Scripts

### kona_scanner_gui.py

**GUI Application** for managing Kona Cotton swatch scanning sessions.

**Usage:**
```bash
# Launch GUI with default settings (opens kona_captures.json)
python3 kona_scanner_gui.py

# Specify serial port and data file
python3 kona_scanner_gui.py --port /dev/ttyACM0 --data ../kona_captures.json
```

**Features:**
- Display all 365 Kona swatches in a sortable, filterable treeview
- Show color sample and Lab/RGB info for selected swatches
- Support single or multi-swatch scanning via EXTENDED selection
- Bidirectional serial communication with device
- Export to C++ header file for firmware use
- Filter by panel, name, ID, or scan status
- Save scan results to kona_captures.json

**Device Setup:**
1. Connect device via USB cable
2. Press button 5 times quickly to enter serial scan mode
3. Launch GUI and click "Connect"
4. Select swatches to scan and click "Scan Selected"

**Requirements:**
- Python 3.8+
- Tkinter (usually included with Python)
- pyserial (`pip install pyserial`)

### generate_synthetic_tints.py

**Standalone script** that generates synthetic tint and shade variants from the
365 measured Kona swatches in `kona_captures.json`.  The resulting
`kona_synthetic_tints.json` expands coverage into lighter and darker regions
where Kona has no real swatches.

**Usage:**
```bash
# Generate with default steps (-30, -15, +15, +30 L* offsets)
python3 generate_synthetic_tints.py

# Specify paths explicitly
python3 generate_synthetic_tints.py \
    --input ../kona_captures.json \
    --output ../kona_synthetic_tints.json

# Custom L* offset steps
python3 generate_synthetic_tints.py --steps -30,-15,15,30

# Preview without writing output
python3 generate_synthetic_tints.py --dry-run
```

**Options:**
| Option | Default | Description |
|--------|---------|-------------|
| `--input` | `../kona_captures.json` | Source of measured Lab values |
| `--output` | `../kona_synthetic_tints.json` | Destination JSON file |
| `--steps` | `-30,-15,15,30` | Comma-separated L* offsets to apply |
| `--min-l` | `5.0` | Minimum allowed L* for output entries |
| `--max-l` | `95.0` | Maximum allowed L* for output entries |
| `--dry-run` | off | Print summary without writing file |

**Algorithm — tint/shade generation:**

For each measured swatch (L, a, b), a variant is generated at
`target_L = L + offset`:

- **Tints** (positive offset, lighter): chroma is reduced proportionally to how
  far the variant moves toward white.  At `L*=100` chroma would reach zero.
- **Shades** (negative offset, darker): chroma undergoes a smaller reduction as
  the variant approaches black.

Variants are skipped when they would be too close to the original
(|ΔL*| < 5) or when the source is already in the target region (e.g., a
"light" variant of an already very light color).

**Naming:**

The four default step positions map to the following prefixes:

| Step | Prefix | Example |
|------|--------|---------|
| −30  | Deep   | Deep CORAL |
| −15  | Dark   | Dark CORAL |
| +15  | Light  | Light CORAL |
| +30  | Pale   | Pale CORAL |

**Synthetic IDs:**

Synthetic entries use IDs ≥ 10000, computed as:

```
synthetic_id = 10000 + source_kona_id × 10 + variant_index
```

This keeps IDs deterministic, unique, and traceable back to the source swatch.
Real Kona IDs (7–1898) never collide with this range.

**Typical output:**

```
Reading source swatches from: ../kona_captures.json
  Found 365 measured swatches
Generating synthetic variants with L* steps: [-30, -15, 15, 30]
  Generated 1420 synthetic entries (710 shades + 710 tints) from 365 source swatches
Wrote 1420 synthetic entries to: ../kona_synthetic_tints.json
```

**Workflow:**

```bash
cd scripts

# 1. Generate synthetic tints from existing scans
python3 generate_synthetic_tints.py

# 2. Next firmware build automatically picks them up
#    (CMakeLists.txt passes --synthetic if the file exists)
idf.py build
```

The build system passes `--synthetic ../kona_synthetic_tints.json` to
`generate_kona_table.py` automatically when the file exists.  Real scanned
entries always take priority over synthetic entries in case of ID collision.

### generate_kona_table.py

Generates a Kona table source file (default: `main/konaref_generated.cpp`) from
`kona_captures.json`.  Optionally merges synthetic tint/shade entries from
`kona_synthetic_tints.json` (produced by `generate_synthetic_tints.py`).

Also provides the shared `render_cpp`, `KonaEntry`, and `crc32_entries` helpers
used by `kona_scanner_gui.py` to export C++ from the GUI.

**Usage:**
```bash
# Normal build (real swatches only)
python3 generate_kona_table.py --input ../kona_captures.json --output ../main/konaref_generated.cpp

# With synthetic tints merged in
python3 generate_kona_table.py \
    --input ../kona_captures.json \
    --output ../main/konaref_generated.cpp \
    --synthetic ../kona_synthetic_tints.json
```

**Features:**
- Reads cached `lab` values from the `swatches` array in kona_captures.json
- Only includes entries where `measured=true` and all Lab values are present
- Optionally merges synthetic entries from `kona_synthetic_tints.json`; real entries always win
- Deduplicates by `id` (last entry wins), sorted by numeric swatch id
- Emits `kona_table_t kona_reference` with schema version, entry count, and CRC32
- **Generates a VP-tree** alongside the reference table for O(log n) matching (vs O(n) linear scan)
- Supports up to 2048 entries (365 real + up to ~1,460 synthetic)
- Shared by `kona_scanner_gui.py` for C++ export

**VP-tree details:**

The generator builds a Vantage Point Tree over all entries using CIEDE2000 as the
distance metric.  The tree is emitted as two additional symbols in the generated
C++ file:

| Symbol | Description |
|--------|-------------|
| `kona_vptree_node_count` | Number of VP-tree nodes (= entry count) |
| `kona_vptree_nodes[]` | Flat node array; root at index 0 |

Each node stores an `entry_index` into `kona_reference.entries[]`, a
`median_distance` partitioning threshold, and left/right child indices.
Search complexity is **O(log n)** (~10–15 CIEDE2000 calculations for 1,141
entries, vs ~1,141 for a linear scan).

### regenerate_kona_lab.py *(new)*

**Pipeline replay script** — rebuilds cached scan_lab values in `kona_captures.json` from
raw sensor data after the color pipeline changes.

**Usage:**
```bash
# Build host binary and regenerate (from scripts/ directory)
python3 regenerate_kona_lab.py

# Specify paths explicitly
python3 regenerate_kona_lab.py --json ../kona_captures.json --host-build ../host/build

# Dry-run: show what would change without writing
python3 regenerate_kona_lab.py --dry-run

# Use a pre-built binary
python3 regenerate_kona_lab.py --binary ../host/build/kona_regenerate
```

**Workflow after a pipeline change:**
1. Edit pipeline parameters (PCCM, responsivity, IR compensation, etc.)
2. Run `python3 regenerate_kona_lab.py` — it builds the host binary and replays raw data
3. Review the printed summary of scan_lab changes (ΔE per swatch)
4. Commit the updated `kona_captures.json`
5. Normal builds pick up the new Lab values automatically

**Requirements:**
- CMake and a C++ compiler (for building the `kona_regenerate` host binary)
- `kona_captures.json` with swatches containing raw sensor data in the `raw` field

**Important:** replay regenerates the same `scan_lab` values returned by the firmware
`SCAN` command (pre-Z-floor, pre-saturation-boost Lab). For multi-sample scans, the
stored `raw` ADC values describe the representative winning measurement context and are
not a lossless encoding of the final averaged scan result.

## JSON Capture Format: kona_captures.json

The `kona_captures.json` file at the repository root is the **single source of truth**
for Kona swatch capture data.  It stores both raw sensor readings (for pipeline replay)
and cached Lab values (for fast builds).

### Top-level Structure

```json
{
  "schema_version": 1,
  "capture_date": "2026-01-01T00:00:00Z",
  "device": {
    "firmware_version": "1.0.0",
    "firmware_commit": "aa55fb9d"
  },
  "pipeline_config_snapshot": {},
  "swatches": [ ... ]
}
```

### Per-Swatch Entry

```json
{
  "panel": "yellow_orange_red",
  "panel_index": 1,
  "id": 449,
  "name": "SUNNY",
  "measured": true,
  "raw": {
    "x": 32200000,
    "y": 33400000,
    "z": 27900000,
    "ir": 619520,
    "clear": 21435649,
    "gain": 5,
    "integration_ms": 100
  },
  "lab": {
    "l": 98.5,
    "a": 26.0264,
    "b": 110.0
  },
  "rgb": {
    "r": 255,
    "g": 228,
    "b": 0
  },
  "notes": ""
}
```

### Fields

| Field | Description |
|---|---|
| `panel` / `panel_index` / `id` / `name` | Metadata matching the Kona 365 swatch catalog |
| `measured` | `true` once a scan has been captured for this swatch |
| `raw.x/y/z` | Raw TCS3530 ADC counts for the X/Y/Z tristimulus channels |
| `raw.ir` | Raw infrared channel count |
| `raw.clear` | Raw broadband clear channel count |
| `raw.gain` | TCS3530 gain code used during capture |
| `raw.integration_ms` | Integration time in milliseconds |
| `lab.l/a/b` | Cached Lab values from the current pipeline (used by normal builds) |
| `rgb.r/g/b` | Display RGB values (informational) |

### Normal vs. Regeneration Build Paths

```
Normal build (99% of the time):
  kona_captures.json (cached lab) → generate_kona_table.py → konaref_generated.cpp

After pipeline change:
  python3 regenerate_kona_lab.py
    → builds kona_regenerate (host C++ binary)
    → feeds raw data through color_pipeline_identify_from_reading()
    → updates lab values in kona_captures.json
    → commit updated JSON
  Next build: picks up new Lab values automatically
```

### Why Not Replay on Every Build?

- Avoids a circular dependency (the host binary needs `konaref_generated.cpp`)
- Calibration decisions (which config to use) require human judgement
- Makes Lab changes explicit and auditable via git history

## Legacy Scripts

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


### kona_scan_collect.py

Collects `KONA_SCAN_CSV` log lines emitted by firmware scan mode and appends them to a capture CSV.

**Usage:**
```bash
python3 kona_scan_collect.py --input-log ../console.txt --output kona_avg_captures.csv

# Optional metadata mapping (default: kona_cotton_solids_k001.csv)
python3 kona_scan_collect.py --input-log ../console.txt --metadata ../kona_cotton_solids_k001.csv

# Or stream from stdin
cat ../console.txt | python3 kona_scan_collect.py --input-log - --output kona_avg_captures.csv
```

**Features:**
- Reads a saved firmware console log text file (or stdin)
- Filters lines tagged with `KONA_SCAN_CSV:`
- Resolves firmware `idx_###` placeholders against `kona_cotton_solids_k001.csv` when available
- Writes panel/panel_index/id/name plus averaged XYZ/Lab and quality counters
- Skips malformed lines with a warning

**Requirements:**
- Python 3.8+

### Legacy Scripts

- **import_resene_colors.py**: Previous import script for Resene paint colors from Excel.
  This script is kept for reference but is no longer used in the main build.
- **analyze_calibration_log.py**: Parses `console.txt` auto-calibration logs and compares optimizer ΔE with post-calibration measurement ΔE.

  **Usage:**
  ```bash
  python3 analyze_calibration_log.py ../console.txt
  ```

## Data Files

### kona_captures.json *(new – preferred)*

Hybrid raw + cached Lab capture file.  Scan once; rebuild Lab from raw data after
pipeline changes.  See [JSON Capture Format](#json-capture-format-kona_capturesjson) above.

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
