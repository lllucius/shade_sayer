# Color Detection Device for Visually Impaired Users

An ESP-IDF based color detection and identification device designed to assist visually impaired individuals in identifying colors using the TCS3530 true color ambient light sensor.

## Features

- **High-Accuracy Color Detection**: Uses the TCS3530 XYZ color sensor for true color measurement
- **Perceptual Color Matching**: CIEDE2000 color distance metric with VP-Tree for O(log n) search
- **Extensive Color Database**: 949 xkcd color survey colors with intuitive, commonly-used names
- **Text-to-Speech**: Spoken color names and descriptions using picotts library
- **I2S Audio Output**: High-quality digital audio via MAX98357A amplifier
- **Button-Triggered Measurement**: Simple single-button operation with multi-click support
- **White Balance Calibration**: Long-press to calibrate against a white reference
- **Power Management**: Deep sleep mode with button wake (< 20µA sleep current)
- **Fast Startup**: Pre-computed VP-Tree and color values eliminate expensive calculations

## Hardware Requirements

### Essential Components
- **Unexpected Maker TinyS3D** (ESP32-S3 with 8MB Flash, 8MB PSRAM)
- **TCS3530 Color Sensor** - AMS true color ambient light sensor (uses internal LED)
- **MAX98357A I2S Amplifier** - I2S Class-D amplifier for audio output
- **Push Button** - For triggering measurements
- **Speaker** - 4-8Ω, 1W minimum

### Optional Components
- **Battery** - 3.7V LiPo for portable operation
- **MAX17048G+T10** - Fuel gauge IC for accurate battery monitoring (I2C address 0x36)

### Pin Connections (Unexpected Maker TinyS3D)

| Function | GPIO | Notes |
|----------|------|-------|
| I2C SDA | 8 | TCS3530 and MAX17048 data |
| I2C SCL | 9 | TCS3530 and MAX17048 clock |
| Button | 1 | Active low, external pull-up, RTC GPIO |
| I2S BCLK | 36 | MAX98357A bit clock |
| I2S LRCLK | 34 | MAX98357A word select |
| I2S DOUT | 37 | MAX98357A data in |
| I2S SD_MODE | 2 | MAX98357A shutdown (RTC GPIO) |
| MAX17048 ALRT | 10 | Battery alert/charging detection, ext1 wakeup (RTC GPIO) |
| USB VBUS Detect | 7 | USB power detection via voltage divider (RTC GPIO) |

**Note**: The TCS3530 uses its internal LED for illumination - no external LED required.

## Building and Flashing

### Prerequisites
- ESP-IDF v5.5.2 or later
- Python 3.8+

### Build Steps

```bash
# Set up ESP-IDF environment
. $IDF_PATH/export.sh

# Configure project (optional - uses defaults from sdkconfig.defaults)
idf.py menuconfig

# Build
idf.py build

# Flash to ESP32
idf.py -p /dev/ttyUSB0 flash monitor
```

### Configuration Options

Use `idf.py menuconfig` to configure:
- GPIO pin assignments
- Integration time (default 100ms)
- I2S audio settings
- Power management timeouts
- Battery monitoring (if equipped)
- **Debug mode**: Simulate sleep instead of entering actual sleep modes (see below)

### Debug Mode: Sleep Simulation

For development and debugging, you can enable sleep simulation mode which prevents the device from entering actual light or deep sleep states. This is useful for:
- Maintaining serial console output during development
- Debugging power management logic without losing connectivity
- Testing sleep/wake behavior without actual sleep transitions

To enable debug mode:
1. Run `idf.py menuconfig`
2. Navigate to: `Color Detector Configuration` → `Debug: Simulate Sleep Instead of Entering Sleep Modes`
3. Enable the option and save
4. Rebuild and flash the firmware

**Debug Mode Behavior:**
- **Simulated Light Sleep**: Uses a FreeRTOS timer instead of `esp_light_sleep_start()`
  - GPIO interrupts remain active for button detection
  - Timer resets on button press (just like real light sleep)
  - Serial console output continues working
- **Simulated Deep Sleep**: Turns off devices but enters an infinite loop
  - Hardware is prepared for sleep (sensor LED off, sensor in sleep mode)
  - Device remains awake waiting for button press
  - In production, this would trigger a reboot; in debug mode, it just logs the event

**Note:** Debug mode should only be used during development. Disable it for production builds to achieve proper low-power operation.

## Usage

### Basic Operation

1. **Power On**: Device announces "Ready" when initialized
2. **Single Press**: Measure and identify the color under the sensor
3. **Double Press**: Repeat last color description with more detail
4. **Triple Press**: Announce battery status
5. **Long Press (2+ seconds)**: Calibrate white reference (point at white paper)

### Understanding Results

The device provides:
- **Color Name**: The closest matching named color from xkcd survey
- **Description**: Natural language description with tone, hue, and associations

### Confidence Indicators

The description includes tone descriptors that indicate color characteristics:
- "pale", "light", "bright", "dark", "deep" - Lightness indicators
- "vivid", "vibrant", "muted", "dull" - Saturation indicators

## Project Structure

```
shade_sayer/
├── CMakeLists.txt               # Project CMake configuration
├── sdkconfig.defaults           # Default SDK configuration
├── partitions.csv               # Flash partition table
├── CONTRIBUTING.md              # Coding conventions and contribution guide
├── HARDWARE.md                  # Hardware reference (schematics, BOM, PCB)
├── LICENSE
├── main/
│   ├── CMakeLists.txt           # Main component configuration
│   ├── Kconfig.projbuild        # Menu configuration options
│   ├── main.cpp                 # Application entry point
│   │
│   │   # Colour Processing
│   ├── color_types.h/.cpp       # Unified type system (XYZ, Lab, LCH, RGB)
│   ├── color_math.h/.cpp        # Colour space conversions, CIEDE2000
│   ├── color_pipeline.h/.cpp    # 13-stage processing pipeline
│   ├── color_database.h/.cpp    # xkcd colour database (generated)
│   ├── color_matcher.h/.cpp     # VP-Tree O(log n) colour matching
│   ├── color_description.h/.cpp # Natural language description generator
│   ├── vptree_data.h            # Pre-computed VP-Tree (generated)
│   │
│   │   # Kona Reference System
│   ├── konaref.h/.cpp           # Kona table schema, validation, VP-tree search
│   ├── konaref_default.cpp      # Fallback empty Kona table
│   ├── kona_metadata.h/.cpp     # 365 Kona Cotton Solids swatch metadata
│   │
│   │   # Hardware Drivers
│   ├── tcs3530.h                # TCS3530 register map & bitfields
│   ├── tcs3530_driver.h/.cpp    # TCS3530 I2C sensor driver
│   ├── max17048_driver.h/.cpp   # MAX17048 battery fuel gauge driver
│   ├── i2c_bus_manager.h/.cpp   # Shared I2C bus management
│   ├── hardware_pins.h          # GPIO pin definitions (from Kconfig)
│   ├── tcs_glue.h/.cpp          # ESP-IDF / host portability layer
│   │
│   │   # Audio & TTS
│   ├── audio_renderer.h/.cpp    # I2S output to MAX98357A
│   ├── tts_manager.h/.cpp       # picotts speech synthesis queue
│   │
│   │   # UI & Power
│   ├── user_interface.h/.cpp    # Button polling, multi-click detection
│   ├── power_manager.h/.cpp     # Deep/light sleep, battery, USB detect
│   │
│   │   # Calibration
│   └── auto_calibrate.h/.cpp    # Guided CCM calibration with optimiser
│
├── components/
│   └── picotts/                 # picotts TTS library (third-party)
│
├── scripts/                     # Python utilities (see scripts/README.md)
│   ├── generate_kona_table.py   # Generate Kona reference C++ source
│   ├── generate_vptree.py       # Generate VP-Tree for xkcd database
│   ├── generate_color_database.py # Generate color_database.cpp
│   ├── generate_synthetic_tints.py # Create tint/shade/tone variants
│   ├── kona_scanner_gui.py      # GUI for scanning Kona swatches
│   ├── annotate_nearest_colors.py # Nearest colour annotation
│   ├── regenerate_kona_lab.py   # Pipeline replay for Lab regeneration
│   ├── color_replay.py          # Batch pipeline replay driver
│   ├── import_xkcd_colors.py    # Import xkcd colour CSV
│   ├── import_resene_colors.py  # Import Resene paint colours
│   ├── analyze_calibration_log.py # Calibration analysis
│   ├── extract_calibration_config.py # Config extraction from logs
│   └── seed_kona_json.py        # JSON initialisation utility
│
├── tests/host/                  # Host-side test programs
│   ├── test_ciede2000.cpp       # CIEDE2000 regression (30 reference pairs)
│   ├── test_delta_e.cpp         # Delta-E edge case tests
│   ├── color_pipeline_unit_tests.cpp # Pipeline unit tests
│   ├── color_match_host_test.cpp # Colour matching test
│   ├── autocal_host_test.cpp    # Calibration optimiser test
│   ├── color_replay_inspect.cpp # Single-capture verbose inspection
│   ├── color_replay_batch.cpp   # Batch/regression replay (CI-friendly)
│   └── kona_regenerate.cpp      # Raw data replay for Kona table regen
│
├── host/                        # Host build configuration
│   ├── CMakeLists.txt           # Host CMake config (8 test executables)
│   └── calibration_measurements_raw.cfg
│
├── docs/                        # Documentation
│   ├── architecture.md          # System architecture overview
│   ├── color-science.md         # Colour science reference
│   ├── calibration.md           # Calibration procedures guide
│   ├── testing.md               # Host test and validation guide
│   ├── replay-harness.md        # Replay tool documentation
│   └── console-log-troubleshooting.md # Debug log interpretation
│
├── kona_captures.json           # Measured Kona swatch data
├── kona_synthetic_tints.json    # Generated tint/shade/tone variants
├── kona_cotton_solids_k001.csv  # Kona 365 swatch metadata (source)
└── TCS3530_DS.txt               # TCS3530 sensor datasheet extract
```

### Kona scan reference table flow

```bash
# Generate from kona_captures.json (requires measured=true entries with Lab values)
python3 scripts/generate_kona_table.py --input kona_captures.json --output main/konaref_generated.cpp

# Or use the GUI to scan swatches and save/export
python3 scripts/kona_scanner_gui.py
```

The firmware validates the generated Kona table via schema version + CRC32 at startup. If the table is
invalid or no entry passes the Kona ΔE threshold, identification falls back to the legacy matcher.

## Color Processing Pipeline

1. **Sensor Reading**: Raw XYZ tristimulus values from TCS3530
2. **Responsivity Normalization**: Convert raw readings using TCS3530 responsivity constants
3. **Color Correction Matrix**: Apply 3x3 CCM for sensor calibration
4. **IR Compensation**: Adaptive correction based on Clear/IR ratio
5. **Integration Time Normalization**: Normalize to 100ms baseline
6. **Black Level Subtraction**: Remove sensor crosstalk/noise
7. **White Balance**: Optional chromatic adaptation to D65 (Bradford transform)
8. **XYZ to Lab**: Convert to perceptually uniform Lab color space
9. **Lightness Correction**: Non-linear correction for blue measurements
10. **Saturation Enhancement**: Adaptive enhancement for vivid colors
11. **Color Matching**: Find nearest named color using VP-Tree with CIEDE2000
12. **Description Generation**: Create natural language description
13. **TTS Output**: Speak the result

## TCS3530 Sensor

The TCS3530 is a true color XYZ ambient light sensor with:
- 8 concurrent sensing channels (X, Y, Z, IR, Clear, HgL, HgH, Flicker)
- Programmable gain (0.5x to 4096x)
- Programmable integration time (1-1000ms)
- Flicker detection up to 7kHz
- I²C interface up to 1MHz

### Uniform Gain Configuration

The driver uses uniform gain (128x) across all channels for stability:
- **X (Red)**: 128x gain
- **Y (Green)**: 128x gain
- **Z (Blue)**: 128x gain
- **IR**: 128x gain

Channel sensitivity differences are handled by the Color Correction Matrix (CCM)
in the color pipeline, which normalizes sensor responsivity.

## Color Science

### Color Spaces Used
- **XYZ**: Device-independent tristimulus (sensor native)
- **Lab**: Perceptually uniform (for matching and processing)
- **LCH**: Cylindrical Lab (for saturation enhancement)
- **sRGB**: Display reference (output only)

### CIEDE2000 Color Difference

The CIEDE2000 formula provides perceptually accurate color differences:
- **ΔE < 1.0**: Not perceptible by human eyes
- **ΔE 1-2**: Perceptible through close observation
- **ΔE 2-10**: Perceptible at a glance
- **ΔE > 10**: Colors are more different than similar

### VP-Tree Fast Matching

Color matching uses a pre-computed Vantage Point Tree (VP-Tree):
- **Search complexity**: O(log n) instead of O(n)
- **For 949 colors**: ~12-20 CIEDE2000 calculations per query vs ~949 for linear search
- **Memory**: ~9.3 KB in Flash (zero heap allocations)

## Calibration

### White Balance Calibration
1. Point the sensor at a white reference (white paper in good lighting)
2. Press and hold the button for 2+ seconds
3. Wait for "Calibration complete" message
4. The device stores both LED and ambient white references in NVS

### When to Calibrate
- First use with LED illumination
- First use with ambient (no LED) measurement
- When changing lighting conditions significantly
- If colors seem consistently wrong

### Dual Calibration Profiles

The system maintains two white balance profiles:
- **LED Profile**: Used when the internal TCS3530 LED is active
- **Ambient Profile**: Used when measuring without LED illumination

This allows accurate measurements in both modes without re-calibration.

## Power Management

The device uses ESP32 deep sleep for ultra-low power consumption:

1. **Active**: Full operation during measurement
2. **Idle**: Sensor sleeping, 30-second inactivity timeout
3. **Deep Sleep**: < 20µA consumption, button press wakes via ext1 wakeup

### Battery Monitoring

The device uses the MAX17048G+T10 fuel gauge IC for accurate battery monitoring:
- Direct state-of-charge (SOC) percentage reading (0-100%)
- Battery voltage measurement
- Shares I2C bus with TCS3530 sensor (address 0x36)
- No additional GPIO or ADC pins required
- Automatic detection when available
- Periodic battery level announcements
- Low battery warnings
- USB vs battery power detection

## Troubleshooting

### "Device not found" Error
- Check I2C connections (SDA, SCL)
- Verify TCS3530 has correct power (1.8V VDD, 1.8V VBUS)
- Check I2C address (should be 0x39)

### Inaccurate Colors
- Perform white balance calibration (long press)
- Ensure adequate lighting
- Check for saturated readings (LED too close or too bright)
- Allow 100ms+ integration time for stable readings

### No Audio Output
- Check I2S connections to MAX98357A
- Verify speaker connection
- Check I2S SD_MODE pin configuration
- Confirm picotts library is properly linked

### Colors Too Muted or Too Vivid
- System uses adaptive saturation enhancement
- Calibration affects this - try recalibrating
- Check gray_threshold and color_threshold settings in code

### "Button still pressed after XXXms" Warning
**Symptoms:** Warning message appears when device tries to sleep, may cause immediate wake-up.

**Cause:** Button GPIO is reading LOW (pressed) when trying to enter sleep mode. This can happen due to:
1. User still physically holding the button down
2. Button bouncing without proper hardware debounce
3. GPIO noise or floating without external components

**Solution:**
1. **Hardware Fix (REQUIRED):** Implement proper button circuit with:
   - 47kΩ external pull-up resistor (3.3V to GPIO1)
   - 100nF ceramic capacitor (GPIO1 to GND) for RC debounce
   - 1kΩ series protection resistor (GPIO1 to button)
   - See HARDWARE.md for complete circuit diagram
2. **Release Button Faster:** Try releasing the button immediately after pressing
3. **Check Button Wiring:** 
   - Button should pull GPIO1 to GND through 1kΩ resistor when pressed
   - GPIO1 should be HIGH (3.3V via 47kΩ pull-up) when not pressed
   - Verify no short circuits or stuck button
4. **Valid GPIO:** Ensure button is on a valid RTC GPIO (0-21 on ESP32-S3)
   - Default: GPIO 1 on Unexpected Maker TinyS3D
   - Any RTC GPIO (0-21) can be used

## Documentation

Detailed guides are available in the `docs/` directory:

* **[Architecture](docs/architecture.md)** — system block diagram, data flow, source file map
* **[Colour Science](docs/color-science.md)** — colour spaces, CIEDE2000, pipeline stages
* **[Calibration](docs/calibration.md)** — white balance, full auto-calibration procedure
* **[Testing](docs/testing.md)** — host build, test executables, Python tests
* **[Replay Harness](docs/replay-harness.md)** — batch pipeline replay for debugging
* **[Console Troubleshooting](docs/console-log-troubleshooting.md)** — interpreting log output

See also: **[CONTRIBUTING.md](CONTRIBUTING.md)** for coding conventions.

## License

This project is open source. See LICENSE file for details.

## Acknowledgments

- Color database based on the xkcd color survey (https://xkcd.com/color/rgb/)
- CIEDE2000 implementation per CIE Technical Report
- TCS3530 register definitions from AMS datasheet
- picotts library from https://github.com/lllucius/picotts
