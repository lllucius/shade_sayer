# Color Pipeline Replay Harness

This document explains how to use the host-side replay harness to investigate
color-pipeline regressions (such as a green surface being misidentified as yellow)
without requiring target hardware.

## Overview

The replay harness lets you run previously captured raw TCS3530 sensor readings
through the current color pipeline, inspect all intermediate values, and run
batch regression checks across many captures.

Two host C++ binaries are provided:

| Binary | Purpose |
|---|---|
| `color_replay_inspect` | Verbose single-capture inspection (all pipeline stages) |
| `color_replay_batch`   | Batch / regression run (CSV output, pass/fail, CI-friendly) |

A Python front-end script (`scripts/color_replay.py`) reads a JSON captures file
and drives either binary.

The existing `kona_regenerate` binary is not part of this harness — it is used
by the Kona reference-table workflow.

---

## Building

```bash
cmake -S host -B /tmp/shade_sayer_host_build
cmake --build /tmp/shade_sayer_host_build
```

The binaries are placed in `/tmp/shade_sayer_host_build/`.

---

## Capture File Format

Captures are stored in JSON files.  See `tests/host/capture_samples.json` for
a fully-annotated example.  The minimal required schema is:

```json
{
  "schema_version": 1,
  "captures": [
    {
      "id": "my_capture",
      "description": "Human-readable description",
      "led_enabled": true,
      "raw": {
        "x": 11517952,
        "y": 12179200,
        "z": 7376128,
        "ir": 249344,
        "clear": 8603648,
        "gain": 4,
        "integration_ms": 100
      },
      "expected": {
        "category": "Green"
      }
    }
  ]
}
```

### Field reference

| Field | Type | Description |
|---|---|---|
| `id` | string | Unique identifier for the capture (no spaces) |
| `description` | string | Optional human-readable description |
| `led_enabled` | bool | `true` if the TCS3530 internal LED was on during capture |
| `raw.x/y/z` | uint32 | Raw TCS3530 ADC tristimulus counts |
| `raw.ir` | uint32 | Raw IR channel count |
| `raw.clear` | uint32 | Raw clear/broadband channel count |
| `raw.gain` | uint8 | Gain code byte reported by `tcs3530_driver` (e.g. `4` = 8×, `5` = 16×) |
| `raw.integration_ms` | uint16 | Integration time in milliseconds |
| `expected.category` | string | Optional expected pipeline category (e.g. `"Green"`, `"Red"`); used for pass/fail |

The `gain` field is the raw gain-code byte, **not** the multiplier.  It must
match the value logged by the firmware as `gain=N` in the `SensorCorr` log line.

---

## Finding Raw Values from Firmware Logs

When the firmware runs a measurement it logs:

```
I (11401) color_pipe: SensorCorr: raw X=11517952 Y=12179200 Z=7376128 clear=8603648 IR=249344 gain=4 int_ms=100
```

Map directly to the capture fields:

```
raw.x            = 11517952
raw.y            = 12179200
raw.z            = 7376128
raw.ir           = 249344
raw.clear        = 8603648
raw.gain         = 4
raw.integration_ms = 100
```

The `led_enabled` field should be `true` when the log contains `Illumination ON` before the measurement.

---

## Pipeline Control Flags

Both `color_replay_inspect` and `color_replay_batch` (and the Python front-end)
accept flags that selectively disable pipeline stages.  These are the primary
diagnostic tool for isolating where a hue shift originates.

| Flag | What it disables | When to use |
|---|---|---|
| `--no-auto-cal` | Skips loading auto-calibration from NVS / host file; uses firmware code defaults | Suspect stale calibration blob |
| `--no-black-cal` | Skips black-level subtraction even if a black level is loaded | Suspect over-aggressive black subtraction |
| `--no-d65-scale` | Skips the D65 white-point pre-scale step (0.95 / 1.00 / 1.09 factors) | Suspect incorrect illuminant assumption |
| `--no-ir-comp` | Skips IR-channel crosstalk compensation | Suspect IR over-correction |
| `--no-material` | Skips material-specific Lab correction | Suspect wrong material correction |
| `--material=NAME` | Overrides assumed material (`default`, `fabric`, `plastic`, `metal`) | Test wall/plastic vs fabric defaults |
| `--bypass-pccm` | Replaces PCCM with identity matrix (**DIAGNOSTIC ONLY**) | Suspect polynomial matrix causing hue rotation |

> **Important:** `--bypass-pccm` produces non-colorimetric output and should
> only be used to determine whether a hue shift is caused by the PCCM or by a
> stage upstream of it.

---

## Single-Capture Inspection

### Using the Python script (recommended)

```bash
# Inspect a named capture from the sample file (baseline)
python3 scripts/color_replay.py inspect \
    --json tests/host/capture_samples.json \
    --id green_wall_paint \
    --build /tmp/shade_sayer_host_build

# Same capture without auto-calibration
python3 scripts/color_replay.py inspect \
    --json tests/host/capture_samples.json \
    --id green_wall_paint \
    --build /tmp/shade_sayer_host_build \
    --no-auto-cal

# Multiple flags at once
python3 scripts/color_replay.py inspect \
    --json tests/host/capture_samples.json \
    --id green_wall_paint \
    --build /tmp/shade_sayer_host_build \
    --no-auto-cal --no-black-cal --bypass-pccm
```

### Calling the binary directly

```bash
# Baseline: CLI args — x y z ir clear gain integration_ms led_enabled [label]
/tmp/shade_sayer_host_build/color_replay_inspect \
    11517952 12179200 7376128 249344 8603648 4 100 1 green_wall_paint

# With flags (flags must precede positional args):
/tmp/shade_sayer_host_build/color_replay_inspect \
    --no-auto-cal --no-black-cal \
    11517952 12179200 7376128 249344 8603648 4 100 1 green_wall_paint

# Or from stdin (same protocol as kona_regenerate):
echo "11517952 12179200 7376128 249344 8603648 4 100 1 green_wall_paint" \
    | /tmp/shade_sayer_host_build/color_replay_inspect --no-auto-cal
```

### Example output

```
=== Color Replay Inspection: green_wall_paint ===

--- Replay Overrides Active ---
  --no-auto-cal       NVS auto-calibration skipped (firmware defaults used)

--- Raw Input ---
  x=11517952     y=12179200     z=7376128
  ir=249344      clear=8603648
  gain=4  integration_ms=100  led=yes

--- Pipeline Stages (see stderr for full pipeline logs) ---
  RESP-norm (pre-black-sub):  x=4964.6343  y=5074.6665  z=3688.0640
  (gain_mult=8.0  base_scale=0.125000)
  [see stderr for: post-black-sub, post-D65, post-gain, post-PCCM, IR comp]

--- Pipeline Result ---
  XYZ:           X=  52.9610  Y=  56.3797  Z=  45.6808
  scan_lab:      L=  82.5622  a=  -1.6151  b=  15.5004  (pre-saturation-boost; used for Kona matching)
  corrected_lab: L=  92.8184  a=  -1.6958  b=  16.2754  (post-material correction; used for fallback matching)
  lab (display): L=  82.5622  a=  -1.7023  b=  16.3373  (post-saturation-boost; used for display/speech)
  RGB:           R=215   G=205   B=175

--- Classification ---
  category:    Yellow
  color_name:  Pale
  kona_matched: no  (fell back to color-database match)
  delta_e:     4.4473
  confidence:  0.7776  (77.8%)

--- Material ---
  material:               Fabric
  material_correction:    enabled

--- Sensor Flags ---
  saturated:       no
  low_light:       no
  flicker_detected: no
  luminance:       82.5622
  saturation:      0.1643
```

The pipeline's internal correction stages (PCCM, IR compensation, D65 scaling,
etc.) are printed to **stderr** via `ESP_LOGx` macros.  Redirect stderr to see
them alongside stdout:

```bash
# Show both pipeline logs and result summary together
/tmp/shade_sayer_host_build/color_replay_inspect \
    11517952 12179200 7376128 249344 8603648 4 100 1 2>&1 | less

# Suppress pipeline logs (keep only the structured summary)
/tmp/shade_sayer_host_build/color_replay_inspect \
    11517952 12179200 7376128 249344 8603648 4 100 1 2>/dev/null
```

---

## Batch / Regression Run

### Using the Python script (recommended)

```bash
# Run all captures, print CSV to stdout, exit 0 even on mismatches
python3 scripts/color_replay.py batch \
    --json tests/host/capture_samples.json \
    --build /tmp/shade_sayer_host_build

# Save CSV to a file
python3 scripts/color_replay.py batch \
    --json tests/host/capture_samples.json \
    --build /tmp/shade_sayer_host_build \
    --output results.csv

# CI mode: exit non-zero when any expected category fails
python3 scripts/color_replay.py batch \
    --json tests/host/capture_samples.json \
    --build /tmp/shade_sayer_host_build \
    --check

# Run with pipeline control flags
python3 scripts/color_replay.py batch \
    --json tests/host/capture_samples.json \
    --build /tmp/shade_sayer_host_build \
    --no-auto-cal --no-black-cal
```

### Text format and calling the binary directly

The batch binary reads space-delimited lines from stdin:

```
# id  x  y  z  ir  clear  gain  integration_ms  led_enabled  [expected_category]
green_wall_paint 11517952 12179200 7376128 249344 8603648 4 100 1 Green
cal_brights_red  11418369 7791616  2225664 619520 21435649 5 100 1 Red
```

Convert the JSON file to this format and feed it directly:

```bash
python3 scripts/color_replay.py dump \
    --json tests/host/capture_samples.json \
    | /tmp/shade_sayer_host_build/color_replay_batch --no-auto-cal
```

### CSV output columns

```
id, expected_category, category, color_name, kona_matched, kona_id,
delta_e, confidence,
resp_norm_x, resp_norm_y, resp_norm_z,    ← RESP-normalised pre-black-sub values
xyz_x, xyz_y, xyz_z,
scan_l, scan_a, scan_b,
corrected_l, corrected_a, corrected_b,
display_l, display_a, display_b,
rgb_r, rgb_g, rgb_b,
material, material_correction_applied,
luminance, saturation,
saturated, low_light, pass
```

`pass` is `PASS` or `FAIL` when `expected_category` is set, otherwise `-`.

When replay flags are active, a comment line is written before the CSV header:

```csv
# replay_flags: no-auto-cal no-black-cal
id,expected_category,...
```

---

## Adding Your Own Captures

1. Record raw values from the firmware log (see "Finding Raw Values" above).
2. Add a new entry to `tests/host/capture_samples.json` (or your own JSON file).
3. Set `expected.category` to the colour category you expect.
4. Run the batch tool with `--check` to verify.

---

## Diagnosing Regressions

When a colour classification changes between commits:

1. **Identify** the regression using `color_replay_batch --check`.
2. **Inspect** the specific failing capture with `color_replay_inspect`.
3. Compare `scan_lab` (the value fed into Kona matching) with the expected Lab range.
4. If `scan_lab` is already wrong, the regression is upstream of Kona matching —
   look at `apply_sensor_correction()` changes (PCCM, D65 scaling, IR compensation,
   responsivity constants, black level).
5. If `scan_lab` is correct but the match fails, look at Kona table or matching-
   threshold changes.

### Key diagnostic fields

| Field | What it reveals |
|---|---|
| `resp_norm_x/y/z` | Responsivity-normalised values before black subtraction and D65 scaling |
| `scan_lab` | What the Kona matcher actually sees; must have a negative `a*` for greens |
| `corrected_lab` | Effect of material correction; compare to `scan_lab` |
| `lab (display)` | Post-saturation-boost; may clip vivid colours — do not use for matching diagnostics |
| `material_correction_applied` | Whether material correction was actually applied |
| `delta_e` | Distance to the nearest match; > 5 means no confident match |
| `kona_matched` | Whether the Kona table was used (`false` = fell back to general colour DB) |

---

## Replay Matrix for Root-Cause Analysis

When a single raw capture produces an unexpected result (for example the
`green_wall_paint` sample that classifies as Yellow/Pale instead of Green), run
it through the following matrix of configurations.  Each run disables one suspect
stage.  Compare the `scan_lab a*` column across runs to find the stage where
the hue shift is introduced.

### Green wall paint — root-cause analysis and fix (resolved)

The green wall paint (`green_wall_paint` in `capture_samples.json`) was systematically
misclassified as Yellow/Pale.  The replay matrix below shows the diagnostic results
that led to identifying and fixing the root cause.

#### Replay matrix results (pre-fix)

| Run | Flags | scan_lab a\* | Category | What it told us |
|-----|-------|-------------|----------|-----------------|
| 1 — Baseline | *(none)* | −1.62 | Yellow | Reference; wrong |
| 2 — No auto-cal | `--no-auto-cal` | −1.08 | Yellow | Auto-cal helps but isn't root cause |
| 3 — No black-cal | `--no-black-cal` | −1.92 | Yellow | Minor contributor only |
| 4 — No D65 scale | `--no-d65-scale` | +8.16 | Orange | D65 scale helps; removing it worsens result |
| 5 — No IR comp | `--no-ir-comp` | −1.34 | Yellow | IR comp is not the cause |
| 6 — No material | `--no-material` | −1.62 | Yellow | Material correction not the cause |
| 7 — `material=default` | `--material=default` | −1.62 | Yellow | Same; confirms material not involved |
| 8 — Bypass PCCM | `--bypass-pccm` | −3.47 | Yellow | PCCM contributes but isn't sufficient alone |

Key observation: none of the single-stage toggles restored the green signal.
The bypass-PCCM run (Run 8) improved a\* by 2 units, confirming the PCCM contributes,
but the result was still Yellow — indicating the problem is **upstream of the PCCM**.

#### Root cause: stale TCS3530_RESP_X/Z constants

The `TCS3530_RESP_X` and `TCS3530_RESP_Z` constants were derived from a **different
calibration session** than the committed calibration data.  This left the white
reference RESP-normalised values unbalanced:

```
Old constants (RESP_X=290, RESP_Z=250):
  White raw: X=21,259,521 Y=21,382,400 Z=16,971,520 (8× gain, 100 ms)
  RESP-norm:  X_n = 9163.7  Y_n = 8909.3  Z_n = 8485.8
  X_n / Y_n = 1.029  ← 2.9% warm bias in every PCCM input
```

A 2.9% excess in the normalised X channel (RESP_X too small → more counts per unit)
injected a systematic warm bias into every PCCM input.  For a muted green wall paint
whose raw y/x ratio is only 1.057 (barely above neutral), this bias was enough to
pull the PCCM output into yellow territory.

**Fix 1: Re-derive RESP constants from committed white reference** (`color_types.h`):

```
RESP_Y = 300.0 (anchor, keeps output numerics compatible)
RESP_X = 300.0 × (21,259,521 / 21,382,400) = 298.3  (was 290.0)
RESP_Z = 300.0 × (16,971,520 / 21,382,400) = 238.1  (was 250.0)
```

With corrected constants the RESP-normalised white is exactly 1:1:1, and the
bypass-PCCM result for the wall paint improved from a\*=−3.47 to a\*=**−6.80**
(hue 116°, solidly Green).

**Fix 2: Add Muted Green calibration reference** (`host/calibration_measurements_raw.cfg`
and `tests/host/autocal_host_test.cpp`):

Even with correct RESP constants, the PCCM trained on the original reference set
(which included `Skin` at sRGB (194,180,154), a warm-neutral colour whose physical
reflectance under the device LED occupies nearly the same PCCM input position as the
wall paint) applied a +5 a\* warm shift in the near-neutral region.

The host calibration test was updated to:
- Omit `Skin` and other warm-neutral patches whose sRGB Lab targets conflict with
  muted-green accuracy (they pull the PCCM warm in the region occupied by the wall paint)
- Add the wall paint raw data as a **Muted Green** reference with target Lab derived
  from white-normalised reflectance (L=80.2 a\*=−5.0 b\*=14.3; theoretical a\*=−6.86
  clamped to −5.0 for PCCM reachability)
- Run two optimisation passes so the refined lightness scale feeds a better linear CCM
  seed for the second pass

#### Post-fix result

| Capture | Pre-fix scan_a | Post-fix scan_a | Category |
|---------|---------------|-----------------|----------|
| `green_wall_paint` | −1.62 (hue 95°) | **−4.15 (hue 106°)** | **Green** ✓ |
| `cal_brights_green` | −56.5 | −56.0 | Green ✓ |
| `cal_mid_green` | −51.1 | −51.0 | Green ✓ |
| `cal_dark_green` | −39.2 | −37.6 | Cyan ✓ |

The wall paint is now correctly classified as **Green**.  The regression guard in
`tests/host/capture_samples.json` (`expected.category = "Green"`) will catch any
future regression.

#### Host calibration workflow

If `TCS3530_RESP_X/Z` or the host reference set change, regenerate
`host/auto_cal_params.bin`:

```bash
cd <repo_root>
cmake -S host -B /tmp/shade_sayer_host_build
cmake --build /tmp/shade_sayer_host_build
/tmp/shade_sayer_host_build/autocal_host_test   # writes host/auto_cal_params.bin
```

Then verify with the regression batch:

```bash
python3 scripts/color_replay.py batch \
    --json tests/host/capture_samples.json \
    --check \
    --build /tmp/shade_sayer_host_build
```

---

### Running the replay matrix (general diagnostic procedure)

The original matrix described below can be applied to **any** capture that is
suspected of hue-shifting through the pipeline.

Base capture: `tests/host/capture_samples.json`, id = `green_wall_paint`
Expected: `scan_lab a* ≈ −4 to −7` (green region), hue > 105°

```bash
BUILD=/tmp/shade_sayer_host_build
JSON=tests/host/capture_samples.json
INSPECT="python3 scripts/color_replay.py inspect --json $JSON --id green_wall_paint --build $BUILD"

# Run 1 — Baseline (current behaviour)
$INSPECT

# Run 2 — No auto-calibration (use firmware code defaults)
$INSPECT --no-auto-cal

# Run 3 — No black-level subtraction
$INSPECT --no-black-cal

# Run 4 — No D65 pre-scale
$INSPECT --no-d65-scale

# Run 5 — No IR compensation
$INSPECT --no-ir-comp

# Run 6 — No material correction
$INSPECT --no-material

# Run 7 — Material override = Default (no per-material Lab adjustment)
$INSPECT --material=default

# Run 8 — PCCM bypassed (identity matrix, DIAGNOSTIC MODE)
$INSPECT --bypass-pccm

# Run 9 — No auto-cal + no black-cal (two suspects at once)
$INSPECT --no-auto-cal --no-black-cal

# Run 10 — No auto-cal + bypass-pccm (isolate loaded-params vs matrix)
$INSPECT --no-auto-cal --bypass-pccm
```

### What to look for in each run

| Run | Flag | Conclusion if `a*` moves greener |
|---|---|---|
| 2 | `--no-auto-cal` | NVS/host auto-cal blob is the culprit |
| 3 | `--no-black-cal` | Black-level subtraction is over-removing green signal |
| 4 | `--no-d65-scale` | D65 pre-scale is applying the wrong illuminant ratio |
| 5 | `--no-ir-comp` | IR compensation is suppressing the green channel |
| 6–7 | `--no-material` / `--material=default` | Material correction is skewing Lab before matching |
| 8 | `--bypass-pccm` | Loaded PCCM matrix is rotating hue away from green |
| 9 | `--no-auto-cal --no-black-cal` | Combination of auto-cal + black-cal parameters |
| 10 | `--no-auto-cal --bypass-pccm` | NVS-loaded calibration contains a bad PCCM |

### How to report results

For each run, record the following values (found in the inspection output):

```
Run N  flags: <flags used>
  scan_lab:       L=XX.XX  a=XX.XX  b=XX.XX
  category:       <Yellow / Green / ...>
  kona_matched:   yes/no
  color_name:     <name>
  delta_e:        XX.XX
  material:       <Fabric / Default / ...>
```

Share the complete table so that the root cause can be determined from the
`a*` trajectory across runs.

---

## Developer Instructions

### Running the replay tool on a saved raw capture

1. Build the host tools (once per checkout):

   ```bash
   cmake -S host -B /tmp/shade_sayer_host_build
   cmake --build /tmp/shade_sayer_host_build
   ```

2. Run the inspector on any capture in `tests/host/capture_samples.json`:

   ```bash
   python3 scripts/color_replay.py inspect \
       --json tests/host/capture_samples.json \
       --id green_wall_paint \
       --build /tmp/shade_sayer_host_build 2>&1
   ```

   The `2>&1` combines pipeline debug logs (stderr) with the structured
   summary (stdout) so you can see both in sequence.

3. Add your own capture by reading the raw values from the firmware log and
   adding an entry to `tests/host/capture_samples.json`.

### Running the replay matrix

Copy the shell snippet from "Replay Matrix" above and run it from the repo root.
Redirect output to a text file for easy comparison:

```bash
$INSPECT             > /tmp/run1_baseline.txt 2>&1
$INSPECT --no-auto-cal > /tmp/run2_no_autocal.txt 2>&1
# ... and so on
```

Then compare the `scan_lab a*` line across files:

```bash
grep "scan_lab" /tmp/run*.txt
```

### Which outputs to compare

- **`scan_lab a*`** — the single most important number.  For a saturated green it
  should be ≤ −15; for a muted (wall-paint) green, ≤ −4 (hue > 105°).
  If it stays near −1 to −2 across all runs, the issue is likely in
  the raw sensor responsivity constants (`TCS3530_RESP_X/Z`).
- **`resp_norm_x/y/z`** in the CSV — shows sensor balance before any correction.
  For a D65-balanced white, `resp_norm_x ≈ resp_norm_y ≈ resp_norm_z`.
- **`xyz_x/y/z`** — the final corrected XYZ fed into Lab conversion.
- **`category`** and **`color_name`** — the final classification result.
- **`kona_matched`** and **`delta_e`** — match quality.

### How to use host-side apps with previously captured non-Kona raw data

The host tools operate entirely on raw sensor readings (x, y, z, ir, clear, gain,
integration_ms) and do not require Kona-format captures.  Any measurement logged
by the firmware can be replayed:

1. Extract the raw values from the firmware `SensorCorr` log line (see
   "Finding Raw Values" above).
2. Add them to `tests/host/capture_samples.json` with any `id` you choose.
3. Run `color_replay_inspect` for a single verbose analysis or
   `color_replay_batch` for a multi-capture CSV summary.

The `host/auto_cal_params.bin` file (if present) is the host-side equivalent
of the device NVS auto-calibration blob.  It is loaded automatically unless
`--no-auto-cal` is specified.  To test with a specific calibration state,
copy the relevant `.bin` file to `host/auto_cal_params.bin` before running.

### Providing results back for analysis

Run the full matrix, then share:

```bash
# Save a CSV with all captures across all flag variants
for flags in "" "--no-auto-cal" "--no-black-cal" "--no-d65-scale" \
             "--no-ir-comp" "--no-material" "--material=default" \
             "--bypass-pccm"; do
    label=$(echo "$flags" | tr -d '-' | tr ' =' '__')
    python3 scripts/color_replay.py batch \
        --json tests/host/capture_samples.json \
        --build /tmp/shade_sayer_host_build \
        $flags \
        --output /tmp/replay_${label:-baseline}.csv 2>/dev/null
done
```

Attach all CSV files to the issue, along with the firmware log that produced
the raw capture.

---

## CI Integration

Add the following step to your CI workflow to catch regressions before merging:

```yaml
- name: Build host tools
  run: |
    cmake -S host -B /tmp/host_build
    cmake --build /tmp/host_build

- name: Run pipeline regression tests
  run: |
    python3 scripts/color_replay.py batch \
        --json tests/host/capture_samples.json \
        --build /tmp/host_build \
        --check
```

The step exits non-zero (causing CI to fail) if any capture with an
`expected.category` is misclassified by the current pipeline.

