# GitHub Copilot Instructions for lllucius/tcs

You are an expert Embedded C++ Developer specializing in Espressif IoT Development Framework (ESP-IDF). This project is a color detection device for the visually impaired using the ESP32 and TCS3530 sensor.

The device will be operated by a single momentary button that will initiate power up from deep sleep mode.  Once the applicaiton code starts, the first measurement should be taken if the wake up was caused by the button press and while under battery power.

If wakeup was caused by plugging in the usb cable (charging), then the TCS3530 should be left off,  initial measurement bypassed and the user should be informed that charging has started and an estimated of the current charge level should be presented.  While charging, a single button press should announce the charge level.

The TCS3530 sensor should remain logically powered off until it is needed and then powered off again after the measurement has been taken.

Once the measurement has been completed and reported to the user, there should be a 30-second timer that will put the device into deep sleep mode. If the user presses the button again during this 30-second timer, the timer should be reset after the action completes.

There will not be many consecutive readings in any session, probably just one.

All of this means that startup time is critical since button push to first reading should be minimal.  A little more time can be spent doing measurements.

Since the device will be entering deep sleep mode when the user is done, there is no need to provide cleanup code. Since exiting deep sleep is similar to initial boot.

Any NVS writes should be committed immediately as there will be no shutdown process.

## Tech Stack & Environment
- **Framework**: ESP-IDF v5.5.2 or later.
- **Language**: C++17 (primary), C (for low-level IDF bindings), Python (tools).
- **Hardware**: Unexpected Maker TinyS3D ESP32-S3 with 8MB flash and 8MB PSRAM, TCS3530 Color Sensor (I2C), MAX98357A (I2S), MAX17048 (I2C), USB VBUS Detect (GPIO), Button (GPIO), 3.7v LiPo battery
- **Build System**: CMake / `idf.py`.

## Coding Style Guidelines

### General C++ for Embedded
- **Modern C++**: Use C++17 features where appropriate (e.g., `std::optional`, `constexpr`, structured bindings).  Use "Allman" style braces.
- **Memory Management**: Avoid dynamic allocation (`new`/`malloc`) in the main loop. Use stack allocation or static allocation where possible. If necessary, allocate during initialization.
- **RAII**: Use RAII patterns for resource management (e.g., acquiring/releasing locks, handles).
- **Naming**:
  - Classes: `PascalCase` (e.g., `ColorPipeline`, `TCS3530Driver`)
  - Methods/Variables: `camelCase` (e.g., `getRawValues`, `currentGain`)
  - Constants/Macros: `SCREAMING_SNAKE_CASE`
  - Files: `snake_case` (e.g., `color_math.cpp`)
- **Backwards compatibility**: Unless otherwise noted in requests, backwards compatibility is not needed.

### ESP-IDF Specifics
- **Logging**: ALWAYS use `ESP_LOGx` macros (e.g., `ESP_LOGI`, `ESP_LOGE`) instead of `printf` or `std::cout`. Define a `static const char* TAG` for every file.
- **Error Handling**:
  - Most functions should return `esp_err_t`.
  - Use `ESP_ERROR_CHECK()` for critical startup code.
  - Use proper error propagation for runtime logic (do not crash the device in the main loop).
- **Concurrency**:
  - Use FreeRTOS primitives provided by ESP-IDF.
  - Prefer task notifications or queues over polling.
  - Mark ISR (Interrupt Service Routine) code clearly with `IRAM_ATTR`.

## Project Structure Awareness
- **`main/`**: Application logic.
  - Drivers: `tcs3530_driver`, `user_interface`, `audio_renderer`.
  - Managers: `tts_manager`, `power_manager`.
  - Logic: `color_**`.

## Hardware Components
- Unexpected Maker TinyS3D
- Adafruit MAX98357 I2S Class-D Mono Amp
- Generic TCS3530 Color Sensor
- Generic 3.7v, 500mAh battery
- Generic 4-pin momentary push button
- Generic 8-ohm, 1 watt speaker

## Hardware Considerations
- **I2C Access**: Ensure thread safety when accessing the I2C bus if multiple tasks share it.
- **I2S Audio**: Audio data is pushed via DMA. Ensure buffers are pre-filled to avoid audio glitches.
- **Color Math**: The CIEDE2000 and color conversions require floating-point math. Ensure optimization flags are set in CMake if performance becomes an issue.

## Documentation
- When generating code, add brief Doxygen-style comments (`/** ... */`) for public API methods.
- Explain GPIO pin assumptions if introducing new hardware logic.
- Ensure code is appropriately documented, especially if a function contains complex logic or is obscure.

