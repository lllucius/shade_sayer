/**
 * @file tcs3530_driver.h
 * @brief TCS3530 Color Sensor Driver for ESP-IDF
 *
 * This driver provides high-level control for the AMS TCS3530
 * true color ambient light sensor. It handles I2C communication,
 * sensor configuration, and data reading.
 *
 * Key Features:
 * - XYZ tristimulus color sensing
 * - Configurable integration time and gain
 * - I2C polling-based operation (interrupts not used)
 * - Internal LED control via VSYNC/GPIO pin
 * - Flicker detection (50/60Hz AC light)
 * - Power management with RAII
 *
 * NOTE: This driver uses the new ESP-IDF v5.x I2C master driver
 * (driver/i2c_master.h) instead of the legacy driver (driver/i2c.h).
 */

#ifndef TCS3530_DRIVER_H
#define TCS3530_DRIVER_H

#include <stdint.h>
#include <stdbool.h>
#include "driver/i2c_master.h"
#include "driver/gpio.h"
#include "esp_err.h"
#include "color_types.h"

#ifdef __cplusplus

//===========================================================================
// C++ Strong-typed Enums and Classes (Modern API)
//===========================================================================

/**
 * @brief TCS3530 channel indices (C++ enum class)
 */
enum class TCS3530Channel : uint8_t
{
    X = 0,       //< X (red-weighted) channel
    Y = 1,       //< Y (green/luminance) channel
    Z = 2,       //< Z (blue-weighted) channel
    IR = 3,      //< Infrared channel
    HGL = 4,     //< Mercury line low (519nm)
    HGH = 5,     //< Mercury line high (545nm)
    Clear = 6,   //< Clear (broadband) channel
    Flicker = 7, //< Flicker detection channel
    Count = 8
};

/**
 * @brief TCS3530 gain settings (C++ enum class)
 */
enum class TCS3530Gain : uint8_t
{
    X0_5   = 0,   //< 0.5x gain
    X1     = 1,   //< 1x gain
    X2     = 2,   //< 2x gain
    X4     = 3,   //< 4x gain
    X8     = 4,   //< 8x gain
    X16    = 5,   //< 16x gain
    X32    = 6,   //< 32x gain
    X64    = 7,   //< 64x gain
    X128   = 8,   //< 128x gain (default)
    X256   = 9,   //< 256x gain
    X512   = 10,  //< 512x gain
    X1024  = 11,  //< 1024x gain
    X2048  = 12,  //< 2048x gain
    X4096  = 13,  //< 4096x gain
};

/**
 * @brief TCS3530 status information (C++ struct)
 */
struct TCS3530Status
{
    bool pon;             //< Power ON bit enabled
    bool aen;             //< ALS engine enabled
    bool fden;            //< Flicker detection enabled
    bool als_data_valid;  //< ALS data is valid (new reading available)
    bool asat_digital;    //< Digital saturation detected
    bool asat_analog;     //< Analog saturation detected
    bool sai_active;      //< Sleep After Interrupt is active
    bool meas_complete;   //< Measurement sequencer completed
};

/**
 * @brief TCS3530 driver configuration (C++ struct)
 */
struct TCS3530Config
{
    i2c_master_bus_handle_t i2c_bus;  //< I2C master bus handle (new API)
    TCS3530Gain initial_gain;         //< Initial gain setting
    uint16_t integration_time_ms;     //< Integration time in ms (1-1000)
    bool auto_gain;                   //< Enable automatic gain control
    bool enable_flicker;              //< Enable flicker detection
    gpio_num_t secondary_led_gpio;    //< Secondary LED GPIO (active low), or GPIO_NUM_NC to disable
};

/**
 * @brief TCS3530 Color Sensor Driver Class
 *
 * RAII-based C++ class for the TCS3530 color sensor.
 * Manages I2C resources automatically.
 */
class TCS3530
{
public:
    /**
     * @brief Construct TCS3530 driver (does not initialize hardware)
     */
    TCS3530();

    /**
     * @brief Destructor - releases I2C device handle if initialized
     *
     * @note In typical usage, the device enters deep sleep without calling
     *       destructors. This is provided for completeness and testing.
     */
    ~TCS3530();

    // Disable copy
    TCS3530(const TCS3530&) = delete;
    TCS3530& operator=(const TCS3530&) = delete;

    // Enable move
    TCS3530(TCS3530&& other) noexcept;
    TCS3530& operator=(TCS3530&& other) noexcept;

    /**
     * @brief Initialize the sensor
     * @param config Configuration struct
     * @return ESP_OK on success
     */
    esp_err_t init(const TCS3530Config& config);

    /**
     * @brief Check if sensor is initialized
     */
    bool isInitialized() const
    {
        return m_initialized;
    }

    /**
     * @brief Get device ID and revision
     * @param device_id Output device ID (should be 0x68)
     * @param revision_id Output revision ID (optional, can be nullptr)
     * @return ESP_OK on success
     */
    esp_err_t getId(uint8_t* device_id, uint8_t* revision_id = nullptr);

    /**
     * @brief Enable or disable the sensor
     * @param enable True to enable, false to disable
     * @return ESP_OK on success
     */
    esp_err_t enable(bool enable);

    /**
     * @brief Set sensor gain
     * @param gain Gain setting
     * @return ESP_OK on success
     */
    esp_err_t setGain(TCS3530Gain gain);

    /**
     * @brief Set per-channel sequencer gains for hardware gain balancing
     *
     * This function sets individual gains for the X, Y, Z, and IR channels
     * in a specific sequencer step. This allows hardware-level gain balancing
     * to equalize signal levels on the sensor silicon before ADC, improving
     * signal-to-noise ratio on weaker channels (e.g., Blue/Z).
     *
     * @param step Sequencer step (0-3)
     * @param gain_x Gain for X (Red) channel
     * @param gain_y Gain for Y (Green) channel
     * @param gain_z Gain for Z (Blue) channel
     * @param gain_ir Gain for IR channel
     * @return ESP_OK on success
     */
    esp_err_t setSequencerGains(uint8_t step, TCS3530Gain gain_x, TCS3530Gain gain_y,
                                 TCS3530Gain gain_z, TCS3530Gain gain_ir);

    /**
     * @brief Get current sensor gain
     */
    TCS3530Gain gain() const
    {
        return m_current_gain;
    }

    /**
     * @brief Set integration time
     * @param time_ms Integration time in milliseconds (1-1000)
     * @return ESP_OK on success
     */
    esp_err_t setIntegrationTime(uint16_t time_ms);

    /**
     * @brief Get current integration time
     */
    uint16_t integrationTime() const
    {
        return m_integration_time_ms;
    }

    /**
     * @brief Check if new data is available
     * @param ready Output: true if data is ready
     * @return ESP_OK on success
     */
    esp_err_t dataReady(bool* ready);

    /**
     * @brief Trigger a single-shot measurement
     * @return ESP_OK on success
     */
    esp_err_t triggerMeasurement();

    /**
     * @brief Wait for measurement completion
     * @param timeout_ms Maximum time to wait
     * @return ESP_OK on success, ESP_ERR_TIMEOUT on timeout
     */
    esp_err_t waitForMeasurement(uint32_t timeout_ms);

    /**
     * @brief Read raw channel data
     * @param reading Output sensor reading structure
     * @return ESP_OK on success
     */
    esp_err_t readRaw(sensor_reading_t* reading);

    /**
     * @brief Perform a complete single-shot measurement
     * @param reading Output sensor reading structure
     * @return ESP_OK on success
     */
    esp_err_t measure(sensor_reading_t* reading);

    /**
     * @brief Perform automatic gain adjustment
     * @return ESP_OK on success
     */
    esp_err_t autoGain();

    /**
     * @brief Put sensor in low-power sleep mode
     * @return ESP_OK on success
     */
    esp_err_t sleep();

    /**
     * @brief Wake sensor from sleep mode
     * @return ESP_OK on success
     */
    esp_err_t wake();

    /**
     * @brief Control the internal LED/GPIO
     * @param on True to turn LED on, false to turn off
     * @return ESP_OK on success
     */
    esp_err_t setLed(bool on);

    /**
     * @brief Get internal LED state
     * @param on Output LED state (true = on, false = off)
     * @return ESP_OK on success
     */
    esp_err_t getLed(bool* on) const;

    /**
     * @brief Get comprehensive device status
     * @param status Output status structure
     * @return ESP_OK on success
     */
    esp_err_t getStatus(TCS3530Status* status);

    /**
     * @brief Enable/disable debug mode
     */
    static void setDebugMode(bool enable);
    static bool debugMode();

private:
    esp_err_t writeReg(uint8_t reg, uint8_t value);
    esp_err_t readReg(uint8_t reg, uint8_t* value) const;
    esp_err_t readRegs(uint8_t start_reg, uint8_t* buffer, size_t length) const;
    esp_err_t writeRegs(uint8_t start_reg, const uint8_t* data, size_t length);
    esp_err_t readFifo(uint8_t* buffer, size_t length) const;
    esp_err_t getFifoLevel(uint16_t* level) const;

    i2c_master_dev_handle_t m_i2c_dev;
    TCS3530Gain m_current_gain;
    uint16_t m_integration_time_ms;
    bool m_auto_gain;
    bool m_enable_flicker;
    bool m_enabled;
    bool m_initialized;
    bool m_led_on;
    gpio_num_t m_secondary_led_gpio;  //< Secondary LED GPIO (active low), or GPIO_NUM_NC

    static bool s_debug_mode;
};

#endif // __cplusplus

#endif // TCS3530_DRIVER_H
