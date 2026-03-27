# Testing Guide

This document explains how to build and run the Shade Sayer host-side test
suite, and how to use the Python test scripts.

## Host Build

The host build compiles the core colour-processing code as native
executables (no ESP-IDF or embedded toolchain required).  The `tcs_glue`
portability layer provides stub implementations of ESP-IDF APIs (logging,
NVS, timing) so the same source files compile on both targets.

### Prerequisites

* A C++17 compiler (GCC ≥ 10, Clang ≥ 12).
* CMake ≥ 3.16.
* Python 3.8+ (for Python-based tests and code generation).

### Building

```bash
# Configure (once)
cmake -S host -B /tmp/shade_sayer_host_build

# Build all test executables
cmake --build /tmp/shade_sayer_host_build
```

The executables are placed in `/tmp/shade_sayer_host_build/`.

## Test Executables

### test_ciede2000

Regression test for the CIEDE2000 colour difference implementation
(`color_math_delta_e_ciede2000`).  Runs 30 reference pairs from the CIE
technical report and verifies the results to four decimal places.

```bash
/tmp/shade_sayer_host_build/test_ciede2000
```

### test_delta_e

Additional delta-E regression tests covering edge cases (identical
colours, achromatic pairs, wrap-around hue differences).

```bash
/tmp/shade_sayer_host_build/test_delta_e
```

### color_pipeline_unit_tests

Comprehensive pipeline tests including:

* Basic XYZ → colour identification round-trip.
* `scan_lab` no-saturation-boost verification.
* Raw sensor reading processing.
* Material correction (fabric, metal, default).
* Auto-detect material heuristic.
* Kona matching correctness.

```bash
/tmp/shade_sayer_host_build/color_pipeline_unit_tests
```

### color_match_host_test

End-to-end colour matching test: feeds known XYZ values through the
pipeline and verifies the matched colour name.

```bash
/tmp/shade_sayer_host_build/color_match_host_test
```

### autocal_host_test

Tests the automatic calibration subsystem by loading raw calibration
measurements from `host/calibration_measurements_raw.cfg` and running
the optimiser.  **Must be run from the repository root** so it can find
the config file:

```bash
cd /path/to/shade_sayer
/tmp/shade_sayer_host_build/autocal_host_test
```

### color_replay_inspect

Single-capture verbose pipeline inspection tool.  Accepts raw sensor
values (space-delimited on stdin or as CLI arguments) and prints every
intermediate pipeline value.  Useful for debugging colour misidentification.

```bash
echo "test 11517952 7791616 2225664 619520 21435649 5 100 1" | \
  /tmp/shade_sayer_host_build/color_replay_inspect
```

See [Replay Harness](replay-harness.md) for the full input format and
available bypass flags.

### color_replay_batch

Batch / regression replay tool.  Reads multiple captures from stdin,
outputs a CSV of intermediate and final values, and returns a non-zero
exit code if any expected-category assertion fails.  Suitable for CI.

```bash
/tmp/shade_sayer_host_build/color_replay_batch < captures.txt
```

### kona_regenerate

Replays raw sensor readings and writes updated L\*a\*b\* values to
stdout.  Used by the Kona reference table regeneration workflow
(`scripts/regenerate_kona_lab.py`) after pipeline changes.

## Python Tests

### test_generate_kona_table.py

Validates the `generate_kona_table.py` code-generation script:

* Table generation from test JSON fixtures.
* CRC32 calculation correctness.
* VP-Tree node structure.
* Synthetic tint handling.

```bash
cd /path/to/shade_sayer
python3 tests/host/test_generate_kona_table.py
```

### test_kona_json_descriptions.py

Validates that Kona capture and synthetic JSON files contain correctly
formatted descriptions (one-sentence, comparison-based wording).

```bash
python3 tests/host/test_kona_json_descriptions.py
```

### test_kona_scanner_gui.py

Unit tests for the Kona scanning GUI (`kona_scanner_gui.py`):

* SwatchData model validation.
* JSON serialisation round-trip.
* Nearest-name display formatting.

```bash
python3 tests/host/test_kona_scanner_gui.py
```

## Replay Bypass Flags

Both `color_replay_inspect` and `color_replay_batch` support stage-bypass
flags for isolating pipeline regressions:

| Flag | Effect |
|------|--------|
| `--no-auto-cal` | Skip NVS/host-file calibration load; use defaults |
| `--no-black-cal` | Skip black-level subtraction |
| `--no-d65-scale` | Skip D65 white-point pre-scale |
| `--no-ir-comp` | Skip IR-channel crosstalk compensation |
| `--no-material` | Skip material-specific Lab correction |
| `--material=NAME` | Force a specific material type |
| `--bypass-pccm` | Replace loaded PCCM with identity matrix |

The Python driver `scripts/color_replay.py` exposes the same flags:

```bash
python3 scripts/color_replay.py inspect --no-ir-comp < capture.txt
python3 scripts/color_replay.py batch   --bypass-pccm < captures.txt
```

## Running All Tests

```bash
# Build
cmake -S host -B /tmp/shade_sayer_host_build
cmake --build /tmp/shade_sayer_host_build

# C++ tests
/tmp/shade_sayer_host_build/test_ciede2000
/tmp/shade_sayer_host_build/test_delta_e
/tmp/shade_sayer_host_build/color_pipeline_unit_tests
/tmp/shade_sayer_host_build/color_match_host_test
cd /path/to/shade_sayer && /tmp/shade_sayer_host_build/autocal_host_test

# Python tests
python3 tests/host/test_generate_kona_table.py
python3 tests/host/test_kona_json_descriptions.py
python3 tests/host/test_kona_scanner_gui.py
```

All tests should complete with zero exit code and print a summary line
(e.g. "CIEDE2000 regression passed (30 cases)").
