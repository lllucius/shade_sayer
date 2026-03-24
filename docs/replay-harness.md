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

## Single-Capture Inspection

### Using the Python script (recommended)

```bash
# Inspect a named capture from the sample file
python3 scripts/color_replay.py inspect \
    --json tests/host/capture_samples.json \
    --id green_wall_paint \
    --build /tmp/shade_sayer_host_build
```

### Calling the binary directly

```bash
# CLI args: x y z ir clear gain integration_ms led_enabled [label]
/tmp/shade_sayer_host_build/color_replay_inspect \
    11517952 12179200 7376128 249344 8603648 4 100 1 green_wall_paint

# Or from stdin (same protocol as kona_regenerate):
echo "11517952 12179200 7376128 249344 8603648 4 100 1 green_wall_paint" \
    | /tmp/shade_sayer_host_build/color_replay_inspect
```

### Example output

```
=== Color Replay Inspection: green_wall_paint ===

--- Raw Input ---
  x=11517952     y=12179200     z=7376128
  ir=249344      clear=8603648
  gain=4  integration_ms=100  led=yes

--- Pipeline Result ---
  XYZ:           X=  49.5400  Y=  52.3700  Z=  42.5200
  scan_lab:      L=  80.6000  a=  -0.6400  b=  15.0200  (pre-saturation-boost; used for Kona matching)
  corrected_lab: L=  90.6600  a=  -0.6720  b=  15.7710  (post-material correction)
  lab (display): L=  80.6000  a=  -0.7000  b=  15.3000  (post-saturation-boost; used for display/speech)
  RGB:           R=210   G=199   B=172

--- Classification ---
  category:    Yellow
  color_name:  Dark OYSTER
  kona_matched: yes
  kona_id:     22681
  delta_e:     4.4155
  confidence:  0.3100  (31.0%)

--- Sensor Flags ---
  saturated:       no
  low_light:       no
  flicker_detected: no
  luminance:       80.6000
  saturation:      0.0700

--- Material ---
  material:    Fabric
```

The pipeline's internal correction stages (PCCM, IR compensation, D65 scaling,
etc.) are printed to **stderr** via `ESP_LOGx` macros.  Redirect stderr to see
them alongside stdout, or suppress them to get cleaner output:

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
    | /tmp/shade_sayer_host_build/color_replay_batch
```

### CSV output columns

```
id, expected_category, category, color_name, kona_matched, kona_id,
delta_e, confidence,
xyz_x, xyz_y, xyz_z,
scan_l, scan_a, scan_b,
corrected_l, corrected_a, corrected_b,
display_l, display_a, display_b,
rgb_r, rgb_g, rgb_b,
material, luminance, saturation,
saturated, low_light, pass
```

`pass` is `PASS` or `FAIL` when `expected_category` is set, otherwise `-`.

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
| `scan_lab` | What the Kona matcher actually sees; must have a negative `a*` for greens |
| `corrected_lab` | Effect of material correction; compare to `scan_lab` |
| `lab (display)` | Post-saturation-boost; may clip vivid colours — do not use for matching diagnostics |
| `delta_e` | Distance to the nearest match; > 5 means no confident match |
| `kona_matched` | Whether the Kona table was used (`false` = fell back to general colour DB) |

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
