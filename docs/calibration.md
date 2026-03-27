# Calibration Guide

This document explains how the Shade Sayer colour calibration system works
and how to perform calibration with physical reference swatches.

## Overview

The calibration system optimises the following parameters:

* **Polynomial Colour Correction Matrix (PCCM)** — a 3×10 matrix that maps
  raw sensor XYZ into a calibrated colour space.
* **Black-level offset** — sensor noise measured against a dark reference.
* **White balance** — chromatic adaptation from the measured illuminant to
  the D65 standard, with separate profiles for LED and ambient lighting.
* **Piecewise gamma correction** — independent gamma curves for dark and
  light tones with a configurable transition point.

All parameters are persisted in NVS and loaded automatically at boot.

## When to Calibrate

* First use of the device.
* After changing the internal LED or sensor module.
* When switching between very different lighting environments.
* If colours are consistently identified incorrectly.

## Quick White-Balance Calibration

1. Place a white reference (bright white paper or Kona "White" swatch)
   under the sensor.
2. Long-press the button (hold for ≥ 2 seconds).
3. The device speaks "Calibration complete" when finished.
4. Both LED and ambient white profiles are stored in NVS.

## Full Automatic Calibration

Full calibration uses a set of physical reference swatches with known RGB
values.  The firmware guides the user through each swatch by name.

### Reference Set

The default reference set is defined in `main.cpp` inside
`perform_auto_calibration()`:

| # | Name | RGB | Flags |
|---|------|-----|-------|
| 1 | Dark Gray | (35, 31, 32) | IS_BLACK, REQUIRED |
| 2 | White | (241, 241, 242) | IS_WHITE, REQUIRED |
| 3 | Brights Red | (237, 28, 36) | REQUIRED |
| 4 | Brights Green | (0, 161, 75) | REQUIRED |
| 5 | Brights Blue | (33, 63, 153) | REQUIRED |
| 6 | Brights Yellow | (255, 221, 23) | REQUIRED |
| 7 | Brights Orange | (241, 101, 33) | REQUIRED |
| 8 | Cyan | (0, 173, 239) | REQUIRED |
| 9 | Skin | (194, 180, 154) | REQUIRED |
| 10 | Dark Brown | (96, 56, 19) | REQUIRED, DARK_CHROMATIC |
| 11 | Dark Taupe | (89, 74, 65) | REQUIRED, DARK_CHROMATIC |
| 12 | Dark Green | (0, 103, 56) | REQUIRED, DARK_CHROMATIC |
| 13 | Gray 50 | (147, 149, 151) | GRAY, IS_NEUTRAL, REQUIRED |
| 14 | Gray 20 | (209, 210, 212) | GRAY, IS_NEUTRAL, REQUIRED |
| 15 | Gray 80 | (88, 88, 91) | GRAY, IS_NEUTRAL, REQUIRED |
| 16 | Brown 1 | (59, 35, 20) | REQUIRED, DARK_CHROMATIC |
| 17 | Brown 2 | (138, 93, 59) | REQUIRED |
| 18 | Brown 3 | (195, 165, 107) | REQUIRED |
| 19 | Mid Green | (0, 147, 68) | REQUIRED |

### Procedure

1. Power the device over USB (calibration requires stable power).
2. Trigger full calibration via the firmware (typically a triple-press or
   menu-initiated command, depending on build).
3. When prompted, place the named swatch under the sensor and press the
   button.
4. Repeat for each reference.  The device announces each name via TTS.
5. After all references are captured, the optimiser runs (10–30 seconds).
6. The resulting parameters are applied to the pipeline and saved to NVS.

### Reference Flags

| Flag | Meaning |
|------|---------|
| `CAL_REF_FLAG_IS_WHITE` | White reference for white-balance |
| `CAL_REF_FLAG_IS_BLACK` | Dark reference for black-level offset |
| `CAL_REF_FLAG_IS_NEUTRAL` | Neutral (achromatic) reference |
| `CAL_REF_FLAG_REQUIRED` | Must be measured (calibration fails if skipped) |
| `CAL_REF_FLAG_GRAY` | Gray reference for lightness/gamma calibration |
| `CAL_REF_FLAG_DARK_CHROMATIC` | Dark chromatic (reduced optimiser weight, 0.75×) |

## Dual White-Balance Profiles

Two independent white-balance profiles are maintained:

* **LED profile** — captured when the TCS3530 internal LED is active.
* **Ambient profile** — captured under ambient room lighting.

The pipeline automatically selects the correct profile based on whether the
LED was on during the measurement.  This eliminates the need to
recalibrate when switching between illumination modes.

## Calibration Parameters (NVS)

All parameters are stored as a single NVS blob under the namespace
`"color_cal"`.  The `color_calib_params_t` structure includes:

| Field | Type | Description |
|-------|------|-------------|
| `pccm[3][10]` | float | Polynomial Colour Correction Matrix |
| `lightness_scale` | float | Global lightness scaling factor |
| `lightness_offset` | float | Global lightness offset |
| `gamma` | float | Gamma exponent |
| `gamma_dark` | float | Gamma for dark tones (piecewise) |
| `gamma_transition` | float | L\* transition between dark/light gamma |
| `ir_factor_x/y/z` | float | Per-channel IR compensation factors |
| `black_offset_*` | float | Black-level XYZ offsets |
| `white_led_*` / `white_ambient_*` | float | White-balance XYZ profiles |

## Host-Side Replay

After changing calibration parameters you can replay captured sensor
readings through the pipeline on your host machine to verify the effect.
See [Replay Harness](replay-harness.md) and [Testing Guide](testing.md).

## Troubleshooting

### "Calibration initialization failed"

* Ensure the device is powered (USB preferred).
* Check that NVS has been initialised (first-boot flash erases NVS).

### Colours worse after calibration

* Verify the reference swatches are the correct colours and not faded.
* Ensure consistent lighting during the entire calibration sequence.
* Try resetting to defaults via `auto_cal_reset_defaults()` and
  recalibrating.

### Dark colours inaccurate, bright colours fine

* Add more DARK_CHROMATIC references to the calibration set.
* Verify the dark-gray reference is not too close to pure black (pure
  black has near-zero reflectance and produces noisy readings).
