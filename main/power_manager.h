/**
 * @file power_manager.h
 * @brief Power Management for Color Detector
 *
 * Implements a simple sleep/wake cycle:
 * 1. User presses button -> action performed
 * 2. After action completes, enter light sleep with 30-second timer
 * 3. If timer expires -> deep sleep
 * 4. If button pressed -> wake and perform action (go to step 2)
 *
 * USB wakeup handling:
 * - If device wakes due to USB connection, announce battery level
 * - Wait 30 seconds, then enter deep sleep
 */

#ifndef POWER_MANAGER_H
#define POWER_MANAGER_H

#include "esp_err.h"
#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "tcs3530_driver.h"

/**
 * @brief Wakeup cause from light/deep sleep
 */
typedef enum
{
    POWER_WAKE_BUTTON = 1,  /**< Woken by button press (starts at 1 to avoid == 0 bugs) */
    POWER_WAKE_TIMER,       /**< Woken by timer expiry */
    POWER_WAKE_USB,         /**< Woken by USB connection */
    POWER_WAKE_SPURIOUS,    /**< Spurious wakeup (no actual state change) */
    POWER_WAKE_UNKNOWN      /**< Unknown or first boot */
} power_wake_cause_t;

/**
 * @brief Power configuration
 */
typedef struct
{
    gpio_num_t wakeup_gpio;         /**< GPIO for wakeup from sleep (button) */
    uint32_t sleep_timeout_ms;      /**< Time in light sleep before deep sleep (default 30000) */
    i2c_master_bus_handle_t i2c_bus; /**< Shared I2C bus handle for MAX17048 */
    gpio_num_t usb_detect_gpio;     /**< GPIO for USB VBUS detection (GPIO_NUM_NC to disable) */
    gpio_num_t max17048_alrt_gpio;  /**< GPIO for MAX17048 ALRT pin (GPIO_NUM_NC to disable) */
} power_config_t;

/**
 * @brief Initialize power manager
 *
 * Single initialization function that:
 * 1. Determines wake cause from deep sleep or reset
 * 2. Initializes fuel gauge and other power hardware
 * 3. Configures sleep wakeup sources
 *
 * @param config Power configuration
 * @param sensor TCS3530 sensor pointer (for sleep control, can be NULL initially)
 * @return ESP_OK on success
 */
esp_err_t power_init(const power_config_t* config, TCS3530* sensor);

/**
 * @brief Set sensor pointer for sleep control
 *
 * Call this after sensor initialization if sensor was NULL in power_init().
 *
 * @param sensor TCS3530 sensor pointer
 */
void power_set_sensor(TCS3530* sensor);

/**
 * @brief Get the boot/wakeup cause
 *
 * Determines what caused the device to boot or wake from sleep.
 * Call this early in initialization to determine startup behavior.
 *
 * @return Wakeup cause enumeration value
 */
power_wake_cause_t power_get_wake_cause(void);

/**
 * @brief Enter light sleep mode with timer
 *
 * Enters light sleep mode. Will wake on:
 * - Button press (returns POWER_WAKE_BUTTON)
 * - Timer expiry after sleep_timeout_ms (returns POWER_WAKE_TIMER)
 *
 * If timer expires, this function automatically enters deep sleep
 * and does not return.
 *
 * @return Wake cause (only returns on button wake)
 */
power_wake_cause_t power_enter_sleep(void);

/**
 * @brief Enter deep sleep mode immediately
 *
 * Puts device into deep sleep mode. This function does not return.
 * Device will wake on button press.
 */
void power_enter_deep_sleep(void);

/**
 * @brief Disable and isolate the onboard RGB LED to save power
 *
 * Drives the TinyS3D onboard RGB LED power pin LOW and sets the data
 * pin to high-impedance, then enables GPIO hold to maintain state
 * during deep sleep.
 *
 * Call only on first boot (wakeup cause UNDEFINED).  During subsequent
 * wakes from deep sleep the hold is already active.
 */
void power_disable_onboard_led(void);

/**
 * @brief Get estimated battery level
 *
 * Reads battery state-of-charge from MAX17048 fuel gauge.
 *
 * @return Battery percentage (0-100), or -1 if not available
 */
int power_get_battery_level(void);

/**
 * @brief Get battery voltage in millivolts
 *
 * Reads battery voltage from MAX17048 fuel gauge.
 *
 * @return Battery voltage in mV, or -1 if not available
 */
int power_get_battery_voltage_mv(void);

/**
 * @brief Check if running on USB power
 *
 * @return True if USB VBUS detected, false if on battery
 */
bool power_is_usb_connected(void);

/**
 * @brief Check if battery is connected
 *
 * Determines if a battery is physically connected to the fuel gauge.
 * A battery is considered connected if the voltage is above the minimum
 * threshold (MIN_BATTERY_VOLTAGE_MV).
 *
 * @return True if battery is connected, false otherwise
 */
bool power_is_battery_connected(void);

/**
 * @brief Get estimated time to full charge in minutes
 *
 * Calculates the estimated time remaining to reach 100% charge based on
 * current SOC. This is a rough estimation assuming constant charging rate.
 * 
 * Typical LiPo charging profiles:
 * - 0-80%: Constant current (CC) phase, ~1-2 hours for 500mAh battery
 * - 80-100%: Constant voltage (CV) phase, slower charging
 * 
 * @return Estimated minutes to full charge, or -1 if not available or not charging
 */
int power_get_time_to_full_charge_minutes(void);

/**
 * @brief Restore SD_MODE GPIO after deep sleep
 *
 * Releases the RTC hold on SD_MODE and reconfigures it as a regular GPIO output.
 * This must be called after waking from deep sleep before initializing the
 * audio renderer, as the SD_MODE GPIO is held in RTC mode during deep sleep.
 *
 * This function is safe to call multiple times and on first boot.
 */
void power_restore_audio_gpio(void);

#endif /* POWER_MANAGER_H */
