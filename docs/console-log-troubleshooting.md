# Console Log Troubleshooting: Saturation + Invalid State

## Observed warnings/errors

1. `Kona reference table invalid/unavailable ... using legacy matcher`
2. `Digital saturation detected (STATUS2=0x10)` (repeats on each sample)
3. `Measurement failed: ESP_ERR_INVALID_STATE`

## Root causes in firmware

### 1) Kona reference warning
The pipeline validates the generated Kona table at startup and warns/falls back if invalid.

- Log origin: `color_pipeline.cpp` (`Kona reference table invalid/unavailable ... using legacy matcher`).
- Expected behavior: use legacy matcher instead of Kona table when validation fails.

### 2) Digital saturation warning
The sensor driver marks a reading as saturated when STATUS2/STATUS6 indicate digital/analog saturation.

- Log origin: `tcs3530_driver.cpp` (`Digital saturation detected`).
- Impact: saturated samples are rejected by averaging capture logic.

### 3) Final `ESP_ERR_INVALID_STATE`
The averaging pipeline returns `ESP_ERR_INVALID_STATE` when all collected samples are rejected.

- In this log, all three captures are saturated, so accepted sample count remains zero.
- Log origin for return condition: `color_pipeline.cpp` (`if (stats->accepted_samples == 0) return ESP_ERR_INVALID_STATE;`).

## Fixes to apply

### A. Reduce sensor saturation probability (highest priority)
- Reduce sensor gain from current fixed mode (`X16/Y128/Z512` in app log path) to lower settings for bright surfaces.
- Shorten integration time from 500 ms to a lower value (e.g., 100-200 ms) when saturation is seen.
- Reduce LED illumination intensity/duty if configurable, or increase sensor/swatch distance.
- Add auto-exposure retry logic: on first saturated sample, re-measure with reduced gain/integration.

### B. Improve robustness when all samples saturate
- Instead of hard-failing on zero accepted samples, optionally:
  - return a specific "overexposed" result code, or
  - retry automatically with safer exposure parameters before returning error.
- Add explicit user feedback such as "Too bright, moving farther away".

### C. Regenerate/fix Kona reference table
- Rebuild Kona table from scan captures and ensure schema+CRC validation passes.
- Keep fallback behavior, but surface telemetry for why validation failed (schema mismatch vs CRC mismatch vs empty table).

## Recommended verification after fixes

1. Boot log no longer shows Kona invalid warning (or shows clear reason if intentionally disabled).
2. Measurement path shows no repeated saturation warnings under normal use.
3. `color_pipeline_identify()` completes without `ESP_ERR_INVALID_STATE` on representative bright swatches.
