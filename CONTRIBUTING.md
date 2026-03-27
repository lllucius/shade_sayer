# Contributing to Shade Sayer

Thank you for your interest in contributing to Shade Sayer!  This document
describes the coding conventions, project structure, and workflow you
should follow.

## Development Environment

* **Framework**: ESP-IDF v5.5.2 or later.
* **Language**: C++17 (primary), C (low-level IDF bindings), Python 3.8+
  (scripts and tests).
* **Hardware**: Unexpected Maker TinyS3D (ESP32-S3 with 8 MB Flash,
  8 MB PSRAM).
* **Build**: CMake / `idf.py`.

## Coding Style

### C++ Naming

| Element | Convention | Example |
|---------|-----------|---------|
| Classes | PascalCase | `TCS3530`, `ColorPipeline` |
| Functions / methods | camelCase | `getRawValues`, `currentGain` |
| Variables | camelCase | `deltaE`, `bestMatch` |
| Constants / macros | SCREAMING_SNAKE_CASE | `D65_X`, `KONA_REF_MAX_ENTRIES` |
| File names | snake_case | `color_math.cpp`, `tcs3530_driver.h` |

### Brace Style

Use **Allman** (BSD) style: opening brace on a new line.

```cpp
void myFunction()
{
    if (condition)
    {
        doSomething();
    }
    else
    {
        doOther();
    }
}
```

### C++17 Features

Use modern C++ features where appropriate:

* `std::optional`, `constexpr`, structured bindings.
* RAII for resource management (locks, handles).
* Prefer stack or static allocation over `new`/`malloc` in the main loop.
* Allocate during initialisation if dynamic memory is necessary.

### Logging

**Always** use `ESP_LOGx` macros:

```cpp
static const char* TAG = "my_module";

ESP_LOGI(TAG, "Initialised with %d entries", count);
ESP_LOGW(TAG, "Unexpected value: %f", value);
ESP_LOGE(TAG, "Failed to read sensor: %s", esp_err_to_name(err));
```

Never use `printf` or `std::cout`.

### Error Handling

* Most functions return `esp_err_t`.
* Use `ESP_ERROR_CHECK()` for critical startup code.
* Propagate errors with `return err;` in runtime code — do not crash.

### Documentation

* Add **Doxygen-style** comments (`/** … */`) for all public API
  functions and types.
* Every `.h` and `.cpp` file should begin with a `@file` / `@brief`
  Doxygen block.
* Explain complex algorithms inline; simple code does not need comments.

```cpp
/**
 * @brief Convert XYZ to CIE L*a*b*
 *
 * Uses D65 illuminant as the reference white.
 *
 * @param xyz  Input XYZ tristimulus values
 * @return Converted L*a*b* values
 */
lab_t color_math_xyz_to_lab(xyz_t xyz);
```

### Python

* Module-level docstring in every script.
* Follow PEP 8 (enforced informally).
* Use `argparse` for CLI scripts.
* Type hints encouraged but not mandatory.

## Memory & Performance

* **No dynamic allocation in the main loop.**  Pre-allocate buffers during
  init or use stack allocation.
* **Startup time is critical** — button-press to first spoken colour should
  be minimal.  Avoid expensive computation at boot.
* **NVS writes must commit immediately** — there is no shutdown process
  (deep sleep simply cuts power).

## Concurrency

* Use FreeRTOS primitives (task notifications, queues) over polling.
* Mark ISR code with `IRAM_ATTR`.
* Ensure I2C bus access is thread-safe when multiple tasks share it.

## Testing

All changes should pass the existing host test suite:

```bash
cmake -S host -B /tmp/shade_sayer_host_build
cmake --build /tmp/shade_sayer_host_build

/tmp/shade_sayer_host_build/test_ciede2000
/tmp/shade_sayer_host_build/test_delta_e
/tmp/shade_sayer_host_build/color_pipeline_unit_tests
/tmp/shade_sayer_host_build/color_match_host_test
cd /path/to/shade_sayer && /tmp/shade_sayer_host_build/autocal_host_test

python3 tests/host/test_generate_kona_table.py
python3 tests/host/test_kona_json_descriptions.py
python3 tests/host/test_kona_scanner_gui.py
```

See [docs/testing.md](docs/testing.md) for details on each test.

## Project Layout

See [docs/architecture.md](docs/architecture.md) for a detailed source
file map.  The key directories are:

| Directory | Contents |
|-----------|----------|
| `main/` | Firmware source — drivers, pipeline, UI, power |
| `components/picotts/` | picotts TTS library |
| `scripts/` | Python utilities (code generation, GUI, analysis) |
| `tests/host/` | Host-side C++ test programs and Python tests |
| `host/` | Host build configuration and calibration data |
| `docs/` | Project documentation |

## Code Generation

Several source files are **auto-generated** and should not be edited
by hand:

| Generated File | Generator Script |
|----------------|-----------------|
| `main/color_database.cpp` | `scripts/generate_color_database.py` |
| `main/vptree_data.h` | `scripts/generate_vptree.py` |
| `konaref_generated.cpp` | `scripts/generate_kona_table.py` |

Regenerate them via:

```bash
python3 scripts/generate_color_database.py
python3 scripts/generate_vptree.py
python3 scripts/generate_kona_table.py --input kona_captures.json \
        --output main/konaref_generated.cpp
```

## Backwards Compatibility

Unless otherwise noted, backwards compatibility is **not** required.
The device enters deep sleep (equivalent to power-off) after each use,
so there is no need for graceful migration of runtime state.
