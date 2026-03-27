# System Architecture

This document describes the overall architecture of the Shade Sayer colour
detection device.  It is aimed at developers who need to understand the
firmware layout, data flow, and hardware/software interactions before making
changes.

## High-Level Block Diagram

```
┌──────────────────────────────────────────────────────────────────┐
│                   APPLICATION (main.cpp)                         │
│                                                                  │
│  ┌────────────┐  ┌────────────────┐  ┌───────────────────────┐  │
│  │   User     │  │  Color         │  │  Text-to-Speech       │  │
│  │ Interface  │  │  Pipeline      │  │  Manager              │  │
│  └─────┬──────┘  └───────┬────────┘  └──────────┬────────────┘  │
│        │                 │                       │               │
│  ┌─────┴──────┐  ┌───────┴────────┐  ┌──────────┴────────────┐  │
│  │  Power     │  │  Color         │  │  Audio                │  │
│  │  Manager   │  │  Matcher       │  │  Renderer             │  │
│  └────────────┘  └───────┬────────┘  └────────────────────────┘  │
│                          │                                       │
│  ┌───────────────────────┴────────────────────────────────────┐  │
│  │                    Driver Layer                             │  │
│  │  TCS3530  │  MAX17048  │  I2C Bus Manager  │  tcs_glue     │  │
│  └────────────────────────────────────────────────────────────┘  │
└──────────────────────────────────────────────────────────────────┘
         │               │                │               │
     ┌───┴───┐     ┌─────┴────┐     ┌────┴────┐    ┌─────┴──────┐
     │TCS3530│     │ MAX17048 │     │ Button  │    │ MAX98357A  │
     │Sensor │     │ Fuel     │     │ (GPIO1) │    │ I2S Amp    │
     │ (I2C) │     │ Gauge    │     └─────────┘    │ + Speaker  │
     └───────┘     │ (I2C)    │                    └────────────┘
                   └──────────┘
```

## Layer Overview

### 1. Application Layer — `main.cpp`

Entry point (`app_main`).  Responsible for:

* Boot-time hardware initialisation (NVS, I2C, I2S, TCS3530, MAX17048).
* Determining wake cause (button press, USB plug-in, timer, spurious).
* Running the main event loop: wait for a UI event, dispatch it, and
  re-enter light sleep until the idle timeout triggers deep sleep.
* Serial scan mode (quintuple-press on USB power) for the scanning GUI.

### 2. Colour Processing — `color_pipeline.*`, `color_math.*`, `color_types.*`

A 13-stage pipeline converts raw TCS3530 XYZ readings into a named colour
with a spoken description.  See [Color Science](color-science.md) for
details on each stage.

### 3. Colour Matching — `color_matcher.*`, `konaref.*`, `vptree_data.h`

Two-tier match strategy:

1. **Kona reference** — 365 measured Kona Cotton Solids plus up to ~1 460
   synthetic tint/shade/tone variants, searched with a VP-Tree in O(log n).
2. **xkcd fallback** — 949 commonly-known colour names, also VP-Tree
   indexed.

### 4. Calibration — `auto_calibrate.*`

Interactive guided calibration using physical colour reference cards.
Optimises a 3×10 polynomial Colour Correction Matrix (PCCM), black-level
offsets, and piecewise gamma correction via gradient-descent.  Results are
persisted in NVS.  See [Calibration Guide](calibration.md).

### 5. Text-to-Speech — `tts_manager.*`, `audio_renderer.*`

Speech synthesis is performed by the **picotts** library (under
`components/picotts`).  `tts_manager` queues text from a FreeRTOS task and
feeds PCM samples to `audio_renderer`, which writes them via DMA to the
MAX98357A I2S amplifier.

### 6. User Interface — `user_interface.*`

Synchronous button polling with software debounce.  Recognises single,
double, triple, quadruple, and quintuple clicks plus a long-press (≥ 2 s).
Events are returned to `main.cpp` as `ui_event_t` values.

### 7. Power Management — `power_manager.*`

Manages the full lifecycle of the device:

| State | Description | Current Draw |
|-------|-------------|-------------|
| Active | Measurement + TTS in progress | ~80 mA |
| Light Sleep | 30 s idle timer, button wake | ~2 mA |
| Deep Sleep | RTC GPIO wake only | < 20 µA |

USB plug-in is detected via a voltage divider on GPIO 7 and used as an
additional ext1 wake source.

### 8. Hardware Abstraction — `i2c_bus_manager.*`, `tcs3530_driver.*`, `max17048_driver.*`, `tcs_glue.*`

* **I2C Bus Manager** — creates a single shared I2C master bus used by both
  the TCS3530 and the MAX17048.
* **TCS3530 Driver** — C++ class wrapping all I2C register access to the
  colour sensor, including gain/integration control, LED switching, and
  sleep.
* **MAX17048 Driver** — C++ class for the battery fuel gauge (SOC, voltage,
  alert, hibernate).
* **tcs_glue** — thin portability shim so that colour-processing code can
  compile identically on ESP-IDF and on the host (for unit tests and replay
  tools).

## Data Flow: Button Press → Spoken Colour

```
User presses button
        │
        ▼
  ui_wait_event()           ← user_interface.cpp
        │
        ▼
  TCS3530::measure()        ← tcs3530_driver.cpp
        │ raw XYZ + IR + Clear
        ▼
  color_pipeline_identify() ← color_pipeline.cpp (13 stages)
        │ color_result_t
        ├──► color_name    (from Kona / xkcd)
        ├──► lab / rgb     (processed values)
        └──► description   (from Kona table or generated)
        │
        ▼
  tts_speak_async()         ← tts_manager.cpp
        │ PCM samples via picotts
        ▼
  audio_renderer_write()    ← audio_renderer.cpp → I2S DMA → speaker
        │
        ▼
  power_enter_sleep()       ← power_manager.cpp (30 s idle timer)
```

## Source File Map

### Core Colour Processing

| File | Purpose |
|------|---------|
| `color_types.h/.cpp` | Unified type system: XYZ, Lab, LCH, RGB structs; gain tables |
| `color_math.h/.cpp` | Colour space conversions, CIEDE2000, chromatic adaptation |
| `color_pipeline.h/.cpp` | 13-stage processing pipeline from raw sensor → named colour |
| `color_database.h/.cpp` | 949 xkcd colour entries sorted by lightness (generated) |
| `color_matcher.h/.cpp` | VP-Tree O(log n) colour search |
| `color_description.h/.cpp` | Natural-language colour description generator |
| `vptree_data.h` | Pre-computed VP-Tree nodes (generated) |

### Kona Reference System

| File | Purpose |
|------|---------|
| `konaref.h/.cpp` | Kona table schema, CRC32 validation, VP-tree search |
| `konaref_default.cpp` | Fallback empty table (used when generated table absent) |
| `kona_metadata.h/.cpp` | Static metadata for 365 Kona Cotton Solids swatches |

### Drivers & Hardware

| File | Purpose |
|------|---------|
| `tcs3530.h` | TCS3530 register map, bitfields, inline helpers |
| `tcs3530_driver.h/.cpp` | TCS3530 C++ driver (I2C, gain, LED, sleep) |
| `max17048_driver.h/.cpp` | MAX17048 battery fuel gauge driver |
| `i2c_bus_manager.h/.cpp` | Shared I2C bus creation & teardown |
| `hardware_pins.h` | GPIO pin definitions (from Kconfig) |
| `tcs_glue.h/.cpp` | ESP-IDF / host portability layer |

### Audio & TTS

| File | Purpose |
|------|---------|
| `audio_renderer.h/.cpp` | I2S output to MAX98357A amplifier |
| `tts_manager.h/.cpp` | picotts speech synthesis queue |

### UI & Power

| File | Purpose |
|------|---------|
| `user_interface.h/.cpp` | Button polling, multi-click detection |
| `power_manager.h/.cpp` | Deep/light sleep, battery monitoring, USB detect |

### Calibration

| File | Purpose |
|------|---------|
| `auto_calibrate.h/.cpp` | Guided CCM calibration with gradient-descent optimiser |

## Build Systems

Two independent build configurations exist:

1. **ESP-IDF firmware** — the root `CMakeLists.txt` plus `main/CMakeLists.txt`.
   Built with `idf.py build`.  The Kona reference table is generated at
   build time by `scripts/generate_kona_table.py`.

2. **Host test harness** — `host/CMakeLists.txt`.  Compiles the same colour
   processing code on a POSIX host (via `tcs_glue`), producing eight test
   executables.  See [Testing Guide](testing.md).

## GPIO Assignments (Default)

| GPIO | Function | Notes |
|------|----------|-------|
| 1 | Button | RTC GPIO, ext1 wake source |
| 2 | MAX98357A SD_MODE | RTC GPIO, deep-sleep hold |
| 6 | POWER_ENABLE | AO3401 P-MOSFET gate, RTC GPIO |
| 7 | USB VBUS detect | Voltage divider, RTC GPIO |
| 8 | I2C SDA | Shared bus (TCS3530 + MAX17048), 2N7002 isolated |
| 9 | I2C SCL | Shared bus, 2N7002 isolated |
| 10 | MAX17048 ALRT | Optional |
| 34 | I2S LRCLK | MAX98357A word select |
| 35 | Secondary LED | Active low |
| 36 | I2S BCLK | MAX98357A bit clock |
| 37 | I2S DOUT | MAX98357A data |

All GPIO numbers are configurable via `idf.py menuconfig` under
*Color Detector Configuration*.
