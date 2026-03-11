#pragma once

/**
 * @file hardware_pins.h
 * @brief Centralized hardware GPIO definitions for all modules
 *
 * ALL GPIO assignments should be defined here so that pin usage is
 * visible in a single location.  Values come from Kconfig where the
 * pin is user-configurable, or are hard-coded for board-fixed pins.
 */

#include "driver/gpio.h"
#include "sdkconfig.h"

// ── I2C bus (shared by TCS3530 + MAX17048) ──────────────────────────
inline constexpr gpio_num_t I2C_SDA_GPIO = static_cast<gpio_num_t>(CONFIG_I2C_MASTER_SDA_IO);
inline constexpr gpio_num_t I2C_SCL_GPIO = static_cast<gpio_num_t>(CONFIG_I2C_MASTER_SCL_IO);

// ── I2S audio (MAX98357A amplifier) ─────────────────────────────────
inline constexpr gpio_num_t I2S_BCLK_GPIO    = static_cast<gpio_num_t>(CONFIG_I2S_BCLK_GPIO);
inline constexpr gpio_num_t I2S_LRCLK_GPIO   = static_cast<gpio_num_t>(CONFIG_I2S_LRCLK_GPIO);
inline constexpr gpio_num_t I2S_DOUT_GPIO     = static_cast<gpio_num_t>(CONFIG_I2S_DOUT_GPIO);
inline constexpr gpio_num_t I2S_SD_MODE_GPIO  = static_cast<gpio_num_t>(CONFIG_I2S_SD_MODE_GPIO);

// ── Sensor power rail (AO3401 P-MOSFET gate) ───────────────────────
inline constexpr gpio_num_t POWER_ENABLE_GPIO = static_cast<gpio_num_t>(CONFIG_POWER_ENABLE_GPIO);

// ── User button (RTC GPIO, ext1 deep-sleep wakeup) ─────────────────
inline constexpr gpio_num_t BUTTON_GPIO = static_cast<gpio_num_t>(CONFIG_BUTTON_GPIO);

// ── USB VBUS detection (voltage divider, RTC GPIO) ─────────────────
#ifdef CONFIG_USB_DETECT_GPIO
inline constexpr gpio_num_t USB_DETECT_GPIO = static_cast<gpio_num_t>(CONFIG_USB_DETECT_GPIO);
#else
inline constexpr gpio_num_t USB_DETECT_GPIO = GPIO_NUM_NC;
#endif

// ── MAX17048 fuel-gauge ALRT pin ────────────────────────────────────
#ifdef CONFIG_MAX17048_ALRT_GPIO
inline constexpr gpio_num_t MAX17048_ALRT_GPIO = static_cast<gpio_num_t>(CONFIG_MAX17048_ALRT_GPIO);
#else
inline constexpr gpio_num_t MAX17048_ALRT_GPIO = GPIO_NUM_NC;
#endif

// ── TinyS3D onboard RGB LED (board-fixed) ──────────────────────────
inline constexpr gpio_num_t RGB_LED_PWR_GPIO  = GPIO_NUM_17;
inline constexpr gpio_num_t RGB_LED_DATA_GPIO = GPIO_NUM_18;

// ── Secondary sensor illumination LED (active low) ──────────────────
inline constexpr gpio_num_t SENSOR_LED2_GPIO = GPIO_NUM_35;
