/**
 * @file tcs3530_driver.cpp
 * @brief TCS3530 Color Sensor Driver Implementation
 *
 * This driver implements I2C communication and high-level control
 * for the AMS TCS3530 true color ambient light sensor.
 *
 * Modern C++ implementation with RAII semantics.
 * Uses the ESP-IDF v5.x I2C master driver.
 */

#include "tcs3530_driver.h"
#include "tcs3530.h"
#include "i2c_bus_manager.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_timer.h"

#include <cstring>
#include <cstdlib>
#include <cmath>
#include <new>

#define TCS_LOGD ESP_LOGI

static const char* TAG = "tcs3530";

// I2C timeout in milliseconds
static constexpr int I2C_TIMEOUT_MS = 100;

// I2C clock speed for TCS3530 (400 kHz fast mode)
static constexpr uint32_t TCS3530_I2C_SPEED_HZ = 400000;

// MOD_FIFO_DATA_CFGx value: bit 7 = enabled, bits 3:0 = no compression
static constexpr uint8_t TCS3530_FIFO_CFG_ENABLED_NO_COMPRESSION = 0x80u;

// Timeout buffer added to integration time for measurement completion
static constexpr uint32_t TCS3530_MEASUREMENT_TIMEOUT_BUFFER_MS = 100u;

// Auto-gain thresholds for 32-bit sample values
static constexpr uint32_t AUTO_GAIN_TARGET_LOW      = 0x40000000UL;
static constexpr uint32_t AUTO_GAIN_TARGET_HIGH     = 0xC0000000UL;
static constexpr uint32_t AUTO_GAIN_SATURATION_THR  = 0xF3333333UL;

// Maximum size for block I2C writes (includes 1 byte for register address)
static constexpr size_t TCS3530_MAX_BLOCK_WRITE_SIZE = 16;
// Maximum data payload size (excludes register address byte)
static constexpr size_t TCS3530_MAX_DATA_SIZE = TCS3530_MAX_BLOCK_WRITE_SIZE - 1;

//===========================================================================
// TCS3530 Timing Constants
//
// These timing delays are required by the TCS3530 sensor for proper
// initialization and operation per the datasheet.
//===========================================================================

/** Power-on initialization delay (ms) - time for internal oscillator startup */
static constexpr uint32_t TCS3530_POWER_ON_INIT_DELAY_MS = 1;

/** Software reset delay (ms) - time for internal registers to reset */
static constexpr uint32_t TCS3530_SOFT_RESET_DELAY_MS = 100;

/** Power-on (PON) settle delay (ms) - time for power subsystem to stabilize */
static constexpr uint32_t TCS3530_PON_SETTLE_DELAY_MS = 2;

/** Enable settle delay (ms) - time for ALS/Flicker engine to start */
static constexpr uint32_t TCS3530_ENABLE_SETTLE_DELAY_MS = 3;

/** Status polling interval (ms) - interval for checking measurement completion */
static constexpr uint32_t TCS3530_STATUS_POLL_INTERVAL_MS = 5;

/** I2C retry delay (ms) - delay between I2C retry attempts */
static constexpr uint32_t TCS3530_I2C_RETRY_DELAY_MS = 1;

/**
 * @brief Register configuration entry for table-driven initialization
 */
struct RegConfig
{
    uint8_t reg;
    uint8_t val;
};

/**
 * @brief Basic initialization register configuration table
 */
static const RegConfig s_init_config[] =
{
    { TCS3530_REG_MEAS_MODE0, TCS3530_REG_MEAS_MODE0_RESET },
    { TCS3530_REG_MEAS_MODE1, TCS3530_REG_MEAS_MODE1_RESET },
    { TCS3530_REG_MEAS_SEQR_STEP0_ALS, 0xFF },
    { TCS3530_REG_CONTROL, TCS3530_CONTROL_FIFO_CLR_Msk },
};

/**
 * @brief FIFO data configuration for all 8 modulators
 */
static const uint8_t s_fifo_config[8] =
{
    TCS3530_FIFO_CFG_ENABLED_NO_COMPRESSION,
    TCS3530_FIFO_CFG_ENABLED_NO_COMPRESSION,
    TCS3530_FIFO_CFG_ENABLED_NO_COMPRESSION,
    TCS3530_FIFO_CFG_ENABLED_NO_COMPRESSION,
    TCS3530_FIFO_CFG_ENABLED_NO_COMPRESSION,
    TCS3530_FIFO_CFG_ENABLED_NO_COMPRESSION,
    TCS3530_FIFO_CFG_ENABLED_NO_COMPRESSION,
    TCS3530_FIFO_CFG_ENABLED_NO_COMPRESSION,
};

//===========================================================================
// TCS3530 Class Implementation
//===========================================================================

bool TCS3530::s_debug_mode = false;

TCS3530::TCS3530()
    : m_i2c_dev(nullptr)
    , m_current_gain(TCS3530Gain::X64)
    , m_integration_time_ms(100)
    , m_auto_gain(false)
    , m_enable_flicker(false)
    , m_enabled(false)
    , m_initialized(false)
    , m_led_on(false)
    , m_secondary_led_gpio(GPIO_NUM_NC)
{
}

TCS3530::~TCS3530()
{
    if (m_initialized && m_i2c_dev)
    {
        // Unregister from bus manager first
        i2c_bus_manager_unregister_device(m_i2c_dev);
        
        // Then remove device from bus
        i2c_master_bus_rm_device(m_i2c_dev);
        m_i2c_dev = nullptr;
        m_initialized = false;
    }
}

TCS3530::TCS3530(TCS3530&& other) noexcept
    : m_i2c_dev(other.m_i2c_dev)
    , m_current_gain(other.m_current_gain)
    , m_integration_time_ms(other.m_integration_time_ms)
    , m_auto_gain(other.m_auto_gain)
    , m_enable_flicker(other.m_enable_flicker)
    , m_enabled(other.m_enabled)
    , m_initialized(other.m_initialized)
    , m_led_on(other.m_led_on)
    , m_secondary_led_gpio(other.m_secondary_led_gpio)
{
    other.m_i2c_dev = nullptr;
    other.m_initialized = false;
}

TCS3530& TCS3530::operator=(TCS3530&& other) noexcept
{
    if (this != &other)
    {
        if (m_initialized && m_i2c_dev)
        {
            i2c_master_bus_rm_device(m_i2c_dev);
        }
        m_i2c_dev = other.m_i2c_dev;
        m_current_gain = other.m_current_gain;
        m_integration_time_ms = other.m_integration_time_ms;
        m_auto_gain = other.m_auto_gain;
        m_enable_flicker = other.m_enable_flicker;
        m_enabled = other.m_enabled;
        m_initialized = other.m_initialized;
        m_led_on = other.m_led_on;
        m_secondary_led_gpio = other.m_secondary_led_gpio;

        other.m_i2c_dev = nullptr;
        other.m_initialized = false;
    }
    return *this;
}

void TCS3530::setDebugMode(bool enable)
{
    s_debug_mode = enable;
    ESP_LOGI(TAG, "Debug mode %s", enable ? "ENABLED" : "DISABLED");
}

bool TCS3530::debugMode()
{
    return s_debug_mode;
}

void TCS3530::setGainScalingFactors(const float* factors, size_t count)
{
    tcs3530_set_gain_scaling_factors(factors, count);
}

esp_err_t TCS3530::writeReg(uint8_t reg, uint8_t value)
{
    uint8_t buf[2] = {reg, value};
    return i2c_master_transmit(m_i2c_dev, buf, sizeof(buf), I2C_TIMEOUT_MS);
}

esp_err_t TCS3530::readReg(uint8_t reg, uint8_t* value) const
{
    return i2c_master_transmit_receive(m_i2c_dev, &reg, 1, value, 1, I2C_TIMEOUT_MS);
}

esp_err_t TCS3530::readRegs(uint8_t start_reg, uint8_t* buffer, size_t length) const
{
    return i2c_master_transmit_receive(m_i2c_dev, &start_reg, 1, buffer, length, I2C_TIMEOUT_MS);
}

esp_err_t TCS3530::readStatusRegs(uint8_t* status2, uint8_t* status6) const
{
    if (!status2 || !status6)
    {
        return ESP_ERR_INVALID_ARG;
    }

    uint8_t status_block[5] = {0};
    esp_err_t ret = readRegs(TCS3530_REG_STATUS2, status_block, sizeof(status_block));
    if (ret != ESP_OK)
    {
        return ret;
    }

    *status2 = status_block[0];
    *status6 = status_block[4];
    return ESP_OK;
}

esp_err_t TCS3530::writeRegs(uint8_t start_reg, const uint8_t* data, size_t length)
{
    if (length > TCS3530_MAX_DATA_SIZE)
    {
        ESP_LOGE(TAG, "Block write size %zu exceeds maximum %zu",
                 length, TCS3530_MAX_DATA_SIZE);
        return ESP_ERR_INVALID_SIZE;
    }

    uint8_t buf[TCS3530_MAX_BLOCK_WRITE_SIZE];
    buf[0] = start_reg;
    memcpy(&buf[1], data, length);
    return i2c_master_transmit(m_i2c_dev, buf, length + 1, I2C_TIMEOUT_MS);
}

esp_err_t TCS3530::readFifo(uint8_t* buffer, size_t length) const
{
    uint8_t reg = TCS3530_REG_FIFO_DATA;
    return i2c_master_transmit_receive(m_i2c_dev, &reg, 1, buffer, length, I2C_TIMEOUT_MS);
}

esp_err_t TCS3530::getFifoLevel(uint16_t* level) const
{
    uint8_t fifo_status[2];
    esp_err_t ret = readRegs(TCS3530_REG_FIFO_STATUS0, fifo_status, 2);
    if (ret != ESP_OK)
    {
        return ret;
    }

    *level = ((uint16_t)(fifo_status[1] & 0x03) << 8) | fifo_status[0];
    return ESP_OK;
}

/**
 * @brief RAII helper for automatic I2C device cleanup on init failure
 */
class I2CDeviceGuard
{
public:
    explicit I2CDeviceGuard(i2c_master_dev_handle_t* dev) : m_dev(dev), m_armed(true) {}
    
    ~I2CDeviceGuard()
    {
        if (m_armed && m_dev && *m_dev)
        {
            i2c_master_bus_rm_device(*m_dev);
            *m_dev = nullptr;
        }
    }
    
    void disarm() { m_armed = false; }
    
    // Disable copy and move (rule of five)
    I2CDeviceGuard(const I2CDeviceGuard&) = delete;
    I2CDeviceGuard& operator=(const I2CDeviceGuard&) = delete;
    I2CDeviceGuard(I2CDeviceGuard&&) = delete;
    I2CDeviceGuard& operator=(I2CDeviceGuard&&) = delete;
    
private:
    i2c_master_dev_handle_t* m_dev;
    bool m_armed;
};

esp_err_t TCS3530::init(const TCS3530Config& config)
{
    if (!config.i2c_bus)
    {
        return ESP_ERR_INVALID_ARG;
    }

    ESP_LOGI(TAG, "Initializing TCS3530 driver...");
    ESP_LOGI(TAG, "  Config: gain=%d, integration=%dms, auto_gain=%d",
             static_cast<int>(config.initial_gain), config.integration_time_ms, config.auto_gain);

    // Configure and add the TCS3530 device to the I2C bus
    i2c_device_config_t dev_cfg = {};
    dev_cfg.dev_addr_length = I2C_ADDR_BIT_LEN_7;
    dev_cfg.device_address = TCS3530_I2C_ADDR;
    dev_cfg.scl_speed_hz = TCS3530_I2C_SPEED_HZ;
    dev_cfg.scl_wait_us = 0;
    dev_cfg.flags.disable_ack_check = false;

    ESP_LOGI(TAG, "Adding I2C device at address 0x%02X, speed=%luHz",
             TCS3530_I2C_ADDR, (unsigned long)TCS3530_I2C_SPEED_HZ);

    esp_err_t ret = i2c_master_bus_add_device(config.i2c_bus, &dev_cfg, &m_i2c_dev);
    if (ret != ESP_OK)
    {
        ESP_LOGE(TAG, "Failed to add I2C device: %s", esp_err_to_name(ret));
        return ret;
    }

    // Register device with I2C bus manager for automatic cleanup
    ret = i2c_bus_manager_register_device(m_i2c_dev);
    if (ret != ESP_OK)
    {
        ESP_LOGW(TAG, "Failed to register device with bus manager: %s", esp_err_to_name(ret));
        // Not a fatal error - device can still operate, just won't be auto-cleaned up
    }

    // RAII guard: automatically cleans up I2C device on early return
    I2CDeviceGuard cleanup_guard(&m_i2c_dev);

    m_current_gain = config.initial_gain;
    m_integration_time_ms = config.integration_time_ms;
    m_auto_gain = config.auto_gain;
    m_enable_flicker = config.enable_flicker;
    m_secondary_led_gpio = config.secondary_led_gpio;
    m_enabled = false;

    // Wait for power-on init
    ESP_LOGI(TAG, "Waiting for power-on initialization...");
    vTaskDelay(pdMS_TO_TICKS(TCS3530_POWER_ON_INIT_DELAY_MS));

    // Verify device ID
    uint8_t device_id, rev_id;
    ret = getId(&device_id, &rev_id);
    if (ret != ESP_OK)
    {
        ESP_LOGE(TAG, "Failed to read device ID: %s", esp_err_to_name(ret));
        return ret;
    }

    if (device_id != TCS3530_REG_ID_RESET)
    {
        ESP_LOGE(TAG, "Invalid device ID: 0x%02X (expected 0x%02X)", device_id, TCS3530_REG_ID_RESET);
        return ESP_ERR_NOT_FOUND;
    }

    ESP_LOGI(TAG, "TCS3530 detected (ID=0x%02X, Rev=0x%02X)", device_id, rev_id);

    // Perform software reset
    ESP_LOGI(TAG, "Performing software reset...");
    ret = writeReg(TCS3530_REG_CONTROL_SCL, TCS3530_CONTROL_SCL_SOFT_RESET_Msk);
    if (ret != ESP_OK)
    {
        ESP_LOGE(TAG, "Failed to reset device: %s", esp_err_to_name(ret));
        return ret;
    }
    vTaskDelay(pdMS_TO_TICKS(TCS3530_SOFT_RESET_DELAY_MS));

    ret = enable(true);
    if (ret != ESP_OK)
    {
        ESP_LOGE(TAG, "Failed to enable device: %s", esp_err_to_name(ret));
        return ret;
    }

    vTaskDelay(pdMS_TO_TICKS(TCS3530_PON_SETTLE_DELAY_MS));

    // Apply basic register configuration
    ESP_LOGI(TAG, "Applying basic register configuration...");
    for (size_t i = 0; i < sizeof(s_init_config) / sizeof(s_init_config[0]); i++)
    {
        ret = writeReg(s_init_config[i].reg, s_init_config[i].val);
        if (ret != ESP_OK)
        {
            ESP_LOGE(TAG, "Failed to write reg 0x%02X: %s", s_init_config[i].reg, esp_err_to_name(ret));
            return ret;
        }
    }

    // Set initial timing
    ESP_LOGI(TAG, "Setting integration time to %dms...", config.integration_time_ms);
    ret = setIntegrationTime(config.integration_time_ms);
    if (ret != ESP_OK)
    {
        ESP_LOGE(TAG, "Failed to set integration time: %s", esp_err_to_name(ret));
        return ret;
    }

    // Set initial gain
    ESP_LOGI(TAG, "Setting gain to %dx...",
             static_cast<int>(tcs3530_gain_code_to_multiplier(static_cast<uint8_t>(config.initial_gain))));
    ret = setGain(config.initial_gain);
    if (ret != ESP_OK)
    {
        ESP_LOGE(TAG, "Failed to set gain: %s", esp_err_to_name(ret));
        return ret;
    }

    // Enable Flicker measurement if requested
    if (config.enable_flicker)
    {
        ESP_LOGI(TAG, "Enabling Flicker measurement for MOD7 in step 0...");
        ret = writeReg(TCS3530_REG_MEAS_SEQR_STEP0_FD, 0x80);
        if (ret != ESP_OK)
        {
            ESP_LOGE(TAG, "Failed to enable Flicker channel: %s", esp_err_to_name(ret));
            return ret;
        }
    }

    // Configure ALS data format for 32-bit output
    ESP_LOGI(TAG, "Configuring 32-bit ALS data format...");
    const int cfg4_max_retries = 3;
    bool cfg4_success = false;

    for (int attempt = 0; attempt < cfg4_max_retries && !cfg4_success; attempt++)
    {
        uint8_t cfg4;
        ret = readReg(TCS3530_REG_CFG4, &cfg4);
        if (ret != ESP_OK)
        {
            if (attempt == cfg4_max_retries - 1)
            {
                return ret;
            }
            vTaskDelay(pdMS_TO_TICKS(TCS3530_I2C_RETRY_DELAY_MS));
            continue;
        }

        cfg4 = (cfg4 & ~TCS3530_CFG4_MOD_ALS_FIFO_DATA_FORMAT_Msk) | TCS3530_ALS_FMT_32BIT;
        ret = writeReg(TCS3530_REG_CFG4, cfg4);
        if (ret != ESP_OK)
        {
            if (attempt == cfg4_max_retries - 1)
            {
                return ret;
            }
            vTaskDelay(pdMS_TO_TICKS(TCS3530_I2C_RETRY_DELAY_MS));
            continue;
        }

        uint8_t cfg4_verify;
        ret = readReg(TCS3530_REG_CFG4, &cfg4_verify);
        if (ret != ESP_OK || (cfg4_verify & TCS3530_CFG4_MOD_ALS_FIFO_DATA_FORMAT_Msk) != TCS3530_ALS_FMT_32BIT)
        {
            vTaskDelay(pdMS_TO_TICKS(TCS3530_I2C_RETRY_DELAY_MS));
        }
        else
        {
            ESP_LOGI(TAG, "CFG4 32-bit format verified successfully");
            cfg4_success = true;
        }
    }

    if (!cfg4_success)
    {
        ESP_LOGE(TAG, "Failed to configure 32-bit ALS format");
        return ESP_ERR_INVALID_STATE;
    }

    // Disable coherence buffer
    ESP_LOGI(TAG, "Ensuring coherence buffer is disabled...");
    uint8_t cfg7;
    ret = readReg(TCS3530_REG_CFG7, &cfg7);
    if (ret == ESP_OK)
    {
        cfg7 &= ~TCS3530_CFG7_ALS_CB_ENABLE_Msk;
        ret = writeReg(TCS3530_REG_CFG7, cfg7);
    }
    if (ret != ESP_OK)
    {
        ESP_LOGE(TAG, "Failed to configure CFG7: %s", esp_err_to_name(ret));
        return ret;
    }

    // Configure FIFO using block write
    ESP_LOGI(TAG, "Configuring FIFO for all 8 modulators (block write)...");
    ret = writeRegs(TCS3530_REG_MOD_FIFO_DATA_CFG0, s_fifo_config, sizeof(s_fifo_config));
    if (ret != ESP_OK)
    {
        ESP_LOGE(TAG, "Failed to configure FIFO: %s", esp_err_to_name(ret));
        return ret;
    }

    // Configure SAI single-shot mode
    ESP_LOGI(TAG, "Configuring SAI single-shot mode...");

    uint8_t cfg0;
    ret = readReg(TCS3530_REG_CFG0, &cfg0);
    if (ret != ESP_OK)
    {
        return ret;
    }
    cfg0 |= TCS3530_CFG0_SAI_Msk;
    ret = writeReg(TCS3530_REG_CFG0, cfg0);
    if (ret != ESP_OK)
    {
        return ret;
    }

    uint8_t meas_mode0;
    ret = readReg(TCS3530_REG_MEAS_MODE0, &meas_mode0);
    if (ret != ESP_OK)
    {
        return ret;
    }
    meas_mode0 |= TCS3530_MEAS_MODE0_MEAS_SEQR_SINGLE_SHOT_Msk;
    ret = writeReg(TCS3530_REG_MEAS_MODE0, meas_mode0);
    if (ret != ESP_OK)
    {
        return ret;
    }

    uint8_t sien;
    ret = readReg(TCS3530_REG_SIEN, &sien);
    if (ret != ESP_OK)
    {
        return ret;
    }
    sien |= TCS3530_SIEN_MEASUREMENT_SEQUENCER_Msk;
    ret = writeReg(TCS3530_REG_SIEN, sien);
    if (ret != ESP_OK)
    {
        return ret;
    }

    ESP_LOGI(TAG, "SAI single-shot mode configured");

    /*
     * Configure TCS3530's internal LED/GPIO pin for output mode with LED off.
     * The GPIO is controlled via the VSYNC_GPIO_INT register (0xB0):
     * - Bit 2 (GPIO_INPUT_ENABLE_BIT): Clear for output mode
     * - Bit 1 (GPIO_OUTPUT_LEVEL_BIT): Clear for LED off initially
     */
    ESP_LOGI(TAG, "Configuring TCS3530 internal LED/GPIO (output mode, LED off)...");
    uint8_t gpio_reg;
    ret = readReg(TCS3530_REG_VSYNC_GPIO_INT, &gpio_reg);
    if (ret != ESP_OK)
    {
        ESP_LOGE(TAG, "Failed to read VSYNC_GPIO_INT register: %s", esp_err_to_name(ret));
        return ret;
    }
    
    // Clear both output level (bit 1) and input enable (bit 2) for output mode with LED off
    gpio_reg &= ~(TCS3530_GPIO_OUTPUT_LEVEL_BIT | TCS3530_GPIO_INPUT_ENABLE_BIT);
    ret = writeReg(TCS3530_REG_VSYNC_GPIO_INT, gpio_reg);
    if (ret != ESP_OK)
    {
        ESP_LOGE(TAG, "Failed to configure internal LED/GPIO: %s", esp_err_to_name(ret));
        return ret;
    }
    
    m_led_on = false;
    ESP_LOGI(TAG, "TCS3530 internal LED/GPIO configured (output mode, LED off)");

    // Initialize secondary LED GPIO (active low) if configured.
    // Drive HIGH initially to keep the LED off.
    if (m_secondary_led_gpio != GPIO_NUM_NC)
    {
        gpio_config_t led2_cfg = {};
        led2_cfg.pin_bit_mask = (1ULL << m_secondary_led_gpio);
        led2_cfg.mode = GPIO_MODE_OUTPUT;
        led2_cfg.pull_up_en = GPIO_PULLUP_DISABLE;
        led2_cfg.pull_down_en = GPIO_PULLDOWN_DISABLE;
        led2_cfg.intr_type = GPIO_INTR_DISABLE;
        ret = gpio_config(&led2_cfg);
        if (ret != ESP_OK)
        {
            ESP_LOGE(TAG, "Failed to configure secondary LED GPIO%d: %s",
                     m_secondary_led_gpio, esp_err_to_name(ret));
            return ret;
        }
        // Active low: HIGH = LED off
        gpio_set_level(m_secondary_led_gpio, 1);
        ESP_LOGI(TAG, "Secondary LED GPIO%d configured (active low, LED off)", m_secondary_led_gpio);
    }

    // Perform warm-up measurement to complete auto-zero calibration.
    //
    // The TCS3530 performs auto-zero calibration "only once at start" (MOD_CALIB_CFG0=0xFF).
    // This calibration runs during the first measurement cycle after each AEN 0→1
    // transition, producing multiple FIFO samples of corrupted calibration data before
    // the actual measurement data. By performing a warm-up measurement here, we complete
    // the calibration during initialization and then disable future calibrations so that
    // subsequent measurements produce only valid data in the FIFO.
    ESP_LOGI(TAG, "Performing warm-up measurement for auto-zero calibration...");
    ret = triggerMeasurement();
    if (ret != ESP_OK)
    {
        ESP_LOGW(TAG, "Warm-up measurement trigger failed: %s", esp_err_to_name(ret));
        // Non-fatal: continue initialization, first user measurement may be affected
    }
    else
    {
        // Wait for measurement completion with timeout
        uint32_t warmup_timeout_ms = m_integration_time_ms + TCS3530_MEASUREMENT_TIMEOUT_BUFFER_MS;
        ret = waitForMeasurement(warmup_timeout_ms);
        if (ret != ESP_OK)
        {
            ESP_LOGW(TAG, "Warm-up measurement wait failed: %s", esp_err_to_name(ret));
            // Non-fatal: continue initialization
        }
        else
        {
            // Read and discard the warm-up data to clear FIFO
            uint16_t fifo_bytes = 0;
            ret = getFifoLevel(&fifo_bytes);
            ESP_LOGI(TAG, "Warm-up FIFO level: %d bytes", fifo_bytes);
            if (ret == ESP_OK && fifo_bytes > 0)
            {
                // Drain ALL warm-up data from FIFO (may include calibration iterations).
                // Use chunks of any size so partial trailing bytes are also consumed,
                // leaving the FIFO completely empty after warm-up.
                uint8_t discard_buffer[32];
                uint16_t remaining = fifo_bytes;
                while (remaining > 0)
                {
                    uint16_t chunk = (remaining >= 32) ? 32 : remaining;
                    ret = readFifo(discard_buffer, chunk);
                    if (ret != ESP_OK)
                    {
                        ESP_LOGW(TAG, "Warm-up FIFO read failed: %s", esp_err_to_name(ret));
                        break;
                    }
                    remaining -= chunk;
                }
            }
            ESP_LOGI(TAG, "Warm-up measurement complete (auto-zero calibration done)");
        }
    }

    // Disable auto-zero calibration for subsequent measurements.
    //
    // The warm-up measurement above completed the initial auto-zero calibration,
    // populating the MOD_OFFSETx registers with correct offset values. These offset
    // registers are retained across PON/AEN toggles (only cleared by chip reset).
    //
    // Without this, each AEN 0→1 transition (in triggerMeasurement()) would trigger
    // a new calibration cycle, filling the FIFO with multiple corrupted calibration
    // samples before the real measurement data. Since readRaw() consumes the last
    // 32-byte sample, disabling recalibration avoids repeated FIFO flooding and keeps
    // measurements stable, which helps prevent the "first measurement returns Black" bug.
    ret = writeReg(TCS3530_REG_MOD_CALIB_CFG0, 0x00);
    if (ret != ESP_OK)
    {
        ESP_LOGW(TAG, "Failed to disable auto-zero recalibration: %s", esp_err_to_name(ret));
    }
    else
    {
        uint8_t readback = 0xFF;
        readReg(TCS3530_REG_MOD_CALIB_CFG0, &readback);
        ESP_LOGI(TAG, "MOD_CALIB_CFG0: wrote 0x00, readback=0x%02X", readback);
    }

    // Put sensor in IDLE state (PON on, AEN off) rather than fully off.
    //
    // enable(false) would disable OSCEN, making ALL clocked registers
    // inaccessible. Any subsequent register writes (e.g., setGain(),
    // setLed()) before the next triggerMeasurement() would silently fail,
    // causing the "first measurement always returns Black" bug:
    //   - setLed(true) doesn't turn on the illumination LED
    //   - setGain()/setSequencerGains() don't configure gains
    //   - MOD_CALIB_CFG0=0x00 is lost, re-enabling auto-zero calibration
    //
    // By keeping OSCEN and PON active (IDLE state), all registers remain
    // accessible and the analog frontend stays warm. The power consumption
    // increase (~70µA) during the brief period before the first measurement
    // is negligible for a battery-operated device.
    ret = writeReg(TCS3530_REG_CONTROL, TCS3530_CONTROL_CLEAR_SAI_ACTIVE_Msk);
    if (ret != ESP_OK)
    {
        ESP_LOGW(TAG, "Failed to clear SAI_ACTIVE: %s", esp_err_to_name(ret));
    }
    ret = writeReg(TCS3530_REG_ENABLE, TCS3530_ENABLE_PON_Msk);
    if (ret != ESP_OK)
    {
        ESP_LOGW(TAG, "Failed to set IDLE state: %s", esp_err_to_name(ret));
    }

    m_initialized = true;
    
    // Disarm the cleanup guard - initialization succeeded
    cleanup_guard.disarm();

    ESP_LOGI(TAG, "TCS3530 initialized (gain=%dx, integration=%dms)",
             static_cast<int>(tcs3530_gain_code_to_multiplier(static_cast<uint8_t>(config.initial_gain))),
             config.integration_time_ms);

    return ESP_OK;
}

esp_err_t TCS3530::getId(uint8_t* device_id, uint8_t* revision_id)
{
    esp_err_t ret;

    if (device_id)
    {
        ret = readReg(TCS3530_REG_ID, device_id);
        if (ret != ESP_OK)
        {
            return ret;
        }
    }

    if (revision_id)
    {
        ret = readReg(TCS3530_REG_REV_ID, revision_id);
        if (ret != ESP_OK)
        {
            return ret;
        }
    }

    return ESP_OK;
}

esp_err_t TCS3530::enable(bool en)
{
    uint8_t enable_val = 0;
    esp_err_t ret;

    if (en)
    {
        ret = writeReg(TCS3530_REG_OSCEN, TCS3530_OSCEN_OSCEN_Msk);
        if (ret != ESP_OK)
        {
            return ret;
        }

        enable_val = TCS3530_ENABLE_PON_Msk | TCS3530_ENABLE_AEN_Msk;
        if (m_enable_flicker)
        {
            enable_val |= TCS3530_ENABLE_FDEN_Msk;
        }
        ret = writeReg(TCS3530_REG_ENABLE, enable_val);
    }
    else
    {
        ret = writeReg(TCS3530_REG_ENABLE, TCS3530_REG_ENABLE_RESET);
        if (ret != ESP_OK)
        {
            return ret;
        }

        ret = writeReg(TCS3530_REG_OSCEN, TCS3530_REG_OSCEN_RESET);
    }
  
    return ret;
}

esp_err_t TCS3530::setGain(TCS3530Gain gain)
{
    if (static_cast<uint8_t>(gain) > static_cast<uint8_t>(TCS3530Gain::X4096))
    {
        return ESP_ERR_INVALID_ARG;
    }

    // Set gain for all modulators using block write
    uint8_t packed = (static_cast<uint8_t>(gain) << 4) | (static_cast<uint8_t>(gain) & 0x0F);
    uint8_t gain_data[4] = {packed, packed, packed, packed};

    esp_err_t ret = writeRegs(TCS3530_REG_MEAS_SEQR_STEP0_MOD_GAINX_0, gain_data, sizeof(gain_data));
    if (ret != ESP_OK)
    {
        return ret;
    }

    m_current_gain = gain;
    return ESP_OK;
}

esp_err_t TCS3530::setSequencerGains(uint8_t step, TCS3530Gain gain_x, TCS3530Gain gain_y,
                                      TCS3530Gain gain_z, TCS3530Gain gain_ir)
{
    if (step > 3)
    {
        ESP_LOGE(TAG, "Invalid sequencer step: %d (max 3)", step);
        return ESP_ERR_INVALID_ARG;
    }

    if (static_cast<uint8_t>(gain_x) > static_cast<uint8_t>(TCS3530Gain::X4096) ||
        static_cast<uint8_t>(gain_y) > static_cast<uint8_t>(TCS3530Gain::X4096) ||
        static_cast<uint8_t>(gain_z) > static_cast<uint8_t>(TCS3530Gain::X4096) ||
        static_cast<uint8_t>(gain_ir) > static_cast<uint8_t>(TCS3530Gain::X4096))
    {
        ESP_LOGE(TAG, "Invalid gain value");
        return ESP_ERR_INVALID_ARG;
    }

    /*
     * TCS3530 sequencer gain register layout (per step, 4 bytes):
     *   Reg+0: MOD1 (Y) [7:4], MOD0 (X) [3:0]
     *   Reg+1: MOD3 (IR) [7:4], MOD2 (Z) [3:0]
     *   Reg+2: MOD5 (HGH) [7:4], MOD4 (HGL) [3:0]
     *   Reg+3: MOD7 (Flicker) [7:4], MOD6 (Clear) [3:0]
     *
     * We set X, Y, Z, IR with the specified gains and use X gain for the rest.
     */
    uint8_t gain_data[4];

    // Reg+0: Y in upper nibble, X in lower nibble
    gain_data[0] = (static_cast<uint8_t>(gain_y) << 4) | (static_cast<uint8_t>(gain_x) & 0x0F);

    // Reg+1: IR in upper nibble, Z in lower nibble
    gain_data[1] = (static_cast<uint8_t>(gain_ir) << 4) | (static_cast<uint8_t>(gain_z) & 0x0F);

    // Reg+2 and Reg+3: Use X gain for HGL, HGH, Clear, Flicker
    uint8_t default_packed = (static_cast<uint8_t>(gain_x) << 4) | (static_cast<uint8_t>(gain_x) & 0x0F);
    gain_data[2] = default_packed;
    gain_data[3] = default_packed;

    // Calculate register base address for the step (each step is 4 bytes apart)
    uint8_t reg_base = TCS3530_REG_MEAS_SEQR_STEP0_MOD_GAINX_0 + (step * 4);

    esp_err_t ret = writeRegs(reg_base, gain_data, sizeof(gain_data));
    if (ret != ESP_OK)
    {
        ESP_LOGE(TAG, "Failed to write sequencer gains for step %d: %s", step, esp_err_to_name(ret));
        return ret;
    }

    /*
     * Update m_current_gain to the X channel gain as the base reference.
     * With hardware gain balancing, there's no single "current gain" since
     * channels have different gains. We use X (Red) as the reference since
     * the software normalization in color_pipeline.cpp compensates for the
     * Y and Z channel boosts relative to X.
     */
    m_current_gain = gain_x;

    ESP_LOGI(TAG, "Sequencer step %d gains: X=%dx Y=%dx Z=%dx IR=%dx",
             step,
             static_cast<int>(tcs3530_gain_code_to_multiplier(static_cast<uint8_t>(gain_x))),
             static_cast<int>(tcs3530_gain_code_to_multiplier(static_cast<uint8_t>(gain_y))),
             static_cast<int>(tcs3530_gain_code_to_multiplier(static_cast<uint8_t>(gain_z))),
             static_cast<int>(tcs3530_gain_code_to_multiplier(static_cast<uint8_t>(gain_ir))));

    return ESP_OK;
}

esp_err_t TCS3530::setIntegrationTime(uint16_t time_ms)
{
    if (time_ms < 1 || time_ms > 1000)
    {
        return ESP_ERR_INVALID_ARG;
    }

    const uint16_t sample_time = 179;
    const double step_us = (sample_time + 1) * 1.388889;
    const double target_us = time_ms * 1000.0;
    int als_nr = static_cast<int>((target_us / step_us) - 1.0 + 0.5);
    if (als_nr < 0)
    {
        als_nr = 0;
    }
    if (als_nr > 2047)
    {
        als_nr = 2047;
    }

    double actual_us = (als_nr + 1) * step_us;
    double actual_ms = actual_us / 1000.0;

    if (s_debug_mode)
    {
        ESP_LOGI(TAG, "Integration time: requested=%dms, actual=%.2fms, ALS_NR_SAMPLES=%d",
                 time_ms, actual_ms, als_nr);
    }

    uint8_t st0 = static_cast<uint8_t>((sample_time & TCS3530_SAMPLE_TIME_LSB_Msk) << 5);
    uint8_t st1 = static_cast<uint8_t>((sample_time >> 3) & TCS3530_SAMPLE_TIME_MSB_Msk);

    esp_err_t ret = writeReg(TCS3530_REG_SAMPLE_TIME0, st0);
    if (ret != ESP_OK)
    {
        return ret;
    }
    ret = writeReg(TCS3530_REG_SAMPLE_TIME1, st1);
    if (ret != ESP_OK)
    {
        return ret;
    }

    uint8_t als0 = static_cast<uint8_t>(als_nr & TCS3530_ALS_NR_SAMPLES0_LSB_Msk);
    uint8_t als1 = static_cast<uint8_t>((als_nr >> 8) & TCS3530_ALS_NR_SAMPLES1_MSB_Msk);

    ret = writeReg(TCS3530_REG_ALS_NR_SAMPLES0, als0);
    if (ret != ESP_OK)
    {
        return ret;
    }
    ret = writeReg(TCS3530_REG_ALS_NR_SAMPLES1, als1);
    if (ret != ESP_OK)
    {
        return ret;
    }

    m_integration_time_ms = time_ms;

    ESP_LOGI(TAG, "Integration time set to %dms (actual: %.2fms)", time_ms, actual_ms);

    return ESP_OK;
}

esp_err_t TCS3530::dataReady(bool* ready)
{
    if (!ready)
    {
        return ESP_ERR_INVALID_ARG;
    }

    *ready = false;

    // Block read status registers
    uint8_t als_status, status5;
    esp_err_t ret = readReg(TCS3530_REG_ALS_DATA_STATUS, &als_status);
    if (ret != ESP_OK)
    {
        return ret;
    }

    ret = readReg(TCS3530_REG_STATUS5, &status5);
    if (ret != ESP_OK)
    {
        return ret;
    }

    bool als_valid = (als_status & TCS3530_ALS_DATA_STATUS_VALID_Msk) != 0;
    bool meas_complete = (status5 & TCS3530_STATUS5_SINT_MEASUREMENT_SEQUENCER_Msk) != 0;

    uint16_t fifo_bytes;
    ret = getFifoLevel(&fifo_bytes);
    if (ret != ESP_OK)
    {
        return ret;
    }

    bool fifo_ready = (fifo_bytes >= 32);
    *ready = (als_valid || meas_complete) && fifo_ready;

    return ESP_OK;
}

esp_err_t TCS3530::triggerMeasurement()
{
    esp_err_t ret;

    // Step 1: Enable oscillator first (required before any other operations).
    // The oscillator must be running for FIFO and status registers to respond.
    ret = writeReg(TCS3530_REG_OSCEN, TCS3530_OSCEN_OSCEN_Msk);
    if (ret != ESP_OK)
    {
        return ret;
    }

    // Step 2: Power on with PON only (NOT AEN).
    //
    // Key insight: If we enable AEN here, the sensor immediately starts
    // integrating. For the first measurement after init(), this causes
    // initialization-phase data to be captured during the settle delay,
    // contaminating the FIFO with near-zero "black" values even after FIFO_CLR.
    //
    // By enabling only PON first, we power up the sensor subsystems without
    // starting the ALS measurement engine. This allows proper FIFO clearing.
    ret = writeReg(TCS3530_REG_ENABLE, TCS3530_ENABLE_PON_Msk);
    if (ret != ESP_OK)
    {
        return ret;
    }

    // Step 3: Wait for oscillator and power subsystems to stabilize.
    //
    // After power-off (PON=0, OSCEN=0), the TCS3530 needs ~300µs for PON_INIT.
    // A 2ms delay provides ample margin for the oscillator to stabilize and
    // for any internal initialization to complete before we clear the FIFO.
    vTaskDelay(pdMS_TO_TICKS(TCS3530_PON_SETTLE_DELAY_MS));

    // Step 3a: Ensure auto-zero calibration is disabled.
    //
    // After sleep()/wake() cycles (used by the power manager for light sleep),
    // enable(false) disables OSCEN, causing clocked registers to become
    // inaccessible and potentially reverting MOD_CALIB_CFG0 to its reset
    // value of 0xFF ("only once at start"). Re-writing it here after OSCEN
    // is re-enabled prevents auto-zero calibration from running on the
    // AEN 0→1 transition.
    ret = writeReg(TCS3530_REG_MOD_CALIB_CFG0, 0x00);
    if (ret != ESP_OK)
    {
        ESP_LOGW(TAG, "Failed to disable auto-zero calibration: %s", esp_err_to_name(ret));
        // Non-fatal: continue, but first measurement may be affected
    }

    // Diagnostic: Check FIFO level before clearing
    if (s_debug_mode)
    {
        uint16_t pre_fifo = 0;
        getFifoLevel(&pre_fifo);
        ESP_LOGI(TAG, "FIFO before clear: %d bytes", pre_fifo);
    }

    // Step 4: Clear FIFO, SAI_ACTIVE, and status flags BEFORE enabling AEN.
    //
    // Now that the sensor is powered (PON=1) but not measuring (AEN=0),
    // FIFO_CLR will properly discard any stale initialization data.
    //
    // CLEAR_SAI_ACTIVE is essential for subsequent measurements: after a
    // measurement completes in SAI mode, the sensor sets SAI_ACTIVE and enters
    // sleep. If we don't clear it here, waitForMeasurement() will immediately
    // see the stale SAI_ACTIVE flag from the previous measurement and return
    // before any new data is collected, causing FIFO underflow.
    ret = writeReg(TCS3530_REG_CONTROL,
                   TCS3530_CONTROL_FIFO_CLR_Msk | TCS3530_CONTROL_CLEAR_SAI_ACTIVE_Msk);
    if (ret != ESP_OK)
    {
        return ret;
    }

    // Diagnostic: Verify FIFO was actually cleared
    if (s_debug_mode)
    {
        uint16_t post_fifo = 0;
        getFifoLevel(&post_fifo);
        ESP_LOGI(TAG, "FIFO after clear: %d bytes", post_fifo);
    }

    // Clear status flags to prevent false "measurement complete" detection.
    ret = writeReg(TCS3530_REG_STATUS, 0xFF);
    if (ret != ESP_OK)
    {
        return ret;
    }

    ret = writeReg(TCS3530_REG_STATUS5, 0xFF);
    if (ret != ESP_OK)
    {
        return ret;
    }

    // Step 5: Now enable AEN (and FDEN if needed) to start the measurement.
    //
    // With the FIFO cleared and status flags reset, enabling AEN will start
    // a fresh measurement cycle. For subsequent measurements (after SAI sleep),
    // this is functionally similar but ensures consistent behavior.
    uint8_t enable_val = TCS3530_ENABLE_PON_Msk | TCS3530_ENABLE_AEN_Msk;
    if (m_enable_flicker)
    {
        enable_val |= TCS3530_ENABLE_FDEN_Msk;
    }
    ret = writeReg(TCS3530_REG_ENABLE, enable_val);
    if (ret != ESP_OK)
    {
        return ret;
    }

    if (s_debug_mode)
    {
        ESP_LOGI(TAG, "Measurement triggered");
    }

    return ESP_OK;
}

esp_err_t TCS3530::waitForMeasurement(uint32_t timeout_ms)
{
    uint32_t start_time = static_cast<uint32_t>(esp_timer_get_time() / 1000);
    uint32_t elapsed_ms = 0;

    while (elapsed_ms < timeout_ms)
    {
        // Block read all 6 status registers
        uint8_t status_block[6];
        esp_err_t ret = readRegs(TCS3530_REG_STATUS, status_block, sizeof(status_block));
        if (ret != ESP_OK)
        {
            return ret;
        }

        // Check SAI_ACTIVE in STATUS4 (index 3)
        if (status_block[3] & TCS3530_STATUS4_SAI_ACTIVE_Msk)
        {
            if (s_debug_mode)
            {
                ESP_LOGI(TAG, "SAI_ACTIVE detected (elapsed: %lu ms)", (unsigned long)elapsed_ms);
            }
            return ESP_OK;
        }

        // Check SINT_MEASUREMENT_SEQUENCER in STATUS5 (index 4)
        if (status_block[4] & TCS3530_STATUS5_SINT_MEASUREMENT_SEQUENCER_Msk)
        {
            if (s_debug_mode)
            {
                ESP_LOGI(TAG, "SINT_MEASUREMENT_SEQUENCER detected (elapsed: %lu ms)",
                         (unsigned long)elapsed_ms);
            }
            return ESP_OK;
        }

        vTaskDelay(pdMS_TO_TICKS(TCS3530_STATUS_POLL_INTERVAL_MS));
        elapsed_ms = static_cast<uint32_t>(esp_timer_get_time() / 1000) - start_time;
    }

    ESP_LOGW(TAG, "Measurement timeout after %lu ms", (unsigned long)timeout_ms);
    return ESP_ERR_TIMEOUT;
}

esp_err_t TCS3530::readRaw(sensor_reading_t* reading)
{
    if (!reading)
    {
        return ESP_ERR_INVALID_ARG;
    }

    memset(reading, 0, sizeof(sensor_reading_t));
    reading->timestamp_us = static_cast<uint32_t>(esp_timer_get_time());
    reading->gain = static_cast<uint8_t>(m_current_gain);
    reading->integration_ms = m_integration_time_ms;

    // Read saturation status from STATUS2/STATUS6.
    // STATUS2 carries digital saturation + analog-saturation-any summary,
    // STATUS6 carries per-modulator analog saturation bits.
    uint8_t status2 = 0;
    uint8_t status6 = 0;
    esp_err_t ret = readStatusRegs(&status2, &status6);
    if (ret != ESP_OK)
    {
        return ret;
    }

    reading->status2 = status2;
    reading->status6 = status6;

    const bool digital_sat = (status2 & TCS3530_STATUS2_ASAT_DIGITAL_Msk) != 0;
    const bool analog_sat_any = (status2 & TCS3530_STATUS2_ASAT_ANALOG_ANY_Msk) != 0;
    const bool analog_sat_mod = (status6 & TCS3530_STATUS6_ASAT_ANALOG_MOD_Msk) != 0;

    reading->saturated = digital_sat || analog_sat_any || analog_sat_mod;

    if (digital_sat)
    {
        ESP_LOGW(TAG, "Digital saturation detected (STATUS2=0x%02x)", status2);
    }
    if (analog_sat_any || analog_sat_mod)
    {
        ESP_LOGW(TAG, "Analog saturation detected (STATUS2=0x%02x, STATUS6=0x%02x)", status2, status6);
    }

    // Check FIFO level
    uint16_t fifo_bytes = 0;
    ret = getFifoLevel(&fifo_bytes);
    if (ret == ESP_OK && s_debug_mode)
    {
        ESP_LOGI(TAG, "FIFO Status: %d bytes available", fifo_bytes);
    }

    if (fifo_bytes < 32)
    {
        ESP_LOGE(TAG, "FIFO underflow: only %d bytes", fifo_bytes);
        writeReg(TCS3530_REG_CONTROL, TCS3530_CONTROL_FIFO_CLR_Msk);
        return ESP_ERR_INVALID_STATE;
    }

    #if 0
    // Read the FIRST 32 bytes from FIFO — this is the valid MOD0-MOD7 measurement
    // block written immediately when the integration completes.  Any remaining
    // bytes are internal accumulation artifacts produced by the auto-zero
    // calibration or the TCS3530's accumulation engine; they follow the real
    // measurement in the FIFO and must be discarded rather than used.
    //
    // Previous attempts to read the LAST 32 bytes caused all-zero readings
    // because those trailing bytes are either FIFO-underflow zeros or
    // calibration-phase data, not the measured channel values.
    if (fifo_bytes > 32)
    {
        ESP_LOGW(TAG, "FIFO excess: %d bytes (expected 32), reading first block and draining %d bytes",
                 fifo_bytes, fifo_bytes - 32);
    }
    #endif

    uint8_t buffer[32] = {0};
    ret = readFifo(buffer, sizeof(buffer));
    if (ret != ESP_OK)
    {
        return ret;
    }

    // Drain any remaining FIFO bytes so the next triggerMeasurement starts clean.
    // We tolerate FIFO_UNDERFLOW here if getFifoLevel overstated the level;
    // FIFO_CLR in the next triggerMeasurement clears the underflow flag.
    if (fifo_bytes > 32)
    {
        uint16_t bytes_to_drain = fifo_bytes - 32;
        uint8_t drain_buf[32];
        while (bytes_to_drain > 0)
        {
            uint16_t chunk = (bytes_to_drain >= sizeof(drain_buf))
                                 ? sizeof(drain_buf)
                                 : bytes_to_drain;
            esp_err_t drain_ret = readFifo(drain_buf, chunk);
            if (drain_ret != ESP_OK && s_debug_mode)
            {
                ESP_LOGD(TAG, "FIFO drain read returned: %s (expected on underflow)",
                         esp_err_to_name(drain_ret));
            }
            bytes_to_drain -= chunk;
        }
    }

    // Parse 32-bit values (little-endian)
    uint32_t channels[8];
    for (int ch = 0; ch < 8; ch++)
    {
        int offset = ch * 4;
        channels[ch] = static_cast<uint32_t>(buffer[offset]) |
                       (static_cast<uint32_t>(buffer[offset + 1]) << 8) |
                       (static_cast<uint32_t>(buffer[offset + 2]) << 16) |
                       (static_cast<uint32_t>(buffer[offset + 3]) << 24);
    }

    reading->x = channels[static_cast<int>(TCS3530Channel::X)];
    reading->y = channels[static_cast<int>(TCS3530Channel::Y)];
    reading->z = channels[static_cast<int>(TCS3530Channel::Z)];
    reading->ir = channels[static_cast<int>(TCS3530Channel::IR)];
    reading->hgl = channels[static_cast<int>(TCS3530Channel::HGL)];
    reading->hgh = channels[static_cast<int>(TCS3530Channel::HGH)];
    reading->clear = channels[static_cast<int>(TCS3530Channel::Clear)];
    reading->flicker = channels[static_cast<int>(TCS3530Channel::Flicker)];

    if (s_debug_mode)
    {
        ESP_LOGI(TAG, "Raw: X=%lu Y=%lu Z=%lu IR=%lu",
                 (unsigned long)reading->x, (unsigned long)reading->y,
                 (unsigned long)reading->z, (unsigned long)reading->ir);
    }

    return ESP_OK;
}

esp_err_t TCS3530::measure(sensor_reading_t* reading)
{
    if (!reading)
    {
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t ret = triggerMeasurement();
    if (ret != ESP_OK)
    {
        return ret;
    }

    uint32_t timeout_ms = m_integration_time_ms + TCS3530_MEASUREMENT_TIMEOUT_BUFFER_MS;
    ret = waitForMeasurement(timeout_ms);
    if (ret != ESP_OK)
    {
        return ret;
    }

    ret = readRaw(reading);
    if (ret != ESP_OK)
    {
        return ret;
    }

    if (s_debug_mode)
    {
        ESP_LOGI(TAG, "Measurement complete (gain=%dx, int=%dms, sat=%d)",
                 static_cast<int>(tcs3530_gain_code_to_multiplier(static_cast<uint8_t>(m_current_gain))),
                 m_integration_time_ms, reading->saturated);
    }

    return ESP_OK;
}

esp_err_t TCS3530::autoGain()
{
    sensor_reading_t raw;
    esp_err_t ret = measure(&raw);
    if (ret != ESP_OK)
    {
        return ret;
    }

    uint32_t channels[] = {raw.x, raw.y, raw.z, raw.ir, raw.hgl, raw.hgh, raw.clear};
    uint32_t max_val = 0;
    for (int ch = 0; ch < 7; ch++)
    {
        if (channels[ch] > max_val)
        {
            max_val = channels[ch];
        }
    }

    TCS3530Gain new_gain = m_current_gain;

    if (raw.saturated || max_val > AUTO_GAIN_SATURATION_THR)
    {
        if (new_gain > TCS3530Gain::X0_5)
        {
            new_gain = static_cast<TCS3530Gain>(static_cast<int>(new_gain) - 1);
        }
        else
        {
            return ESP_ERR_INVALID_STATE;
        }
    }
    else if (max_val < AUTO_GAIN_TARGET_LOW && new_gain < TCS3530Gain::X4096)
    {
        new_gain = static_cast<TCS3530Gain>(static_cast<int>(new_gain) + 1);
    }
    else if (max_val > AUTO_GAIN_TARGET_HIGH && new_gain > TCS3530Gain::X0_5)
    {
        new_gain = static_cast<TCS3530Gain>(static_cast<int>(new_gain) - 1);
    }

    if (new_gain != m_current_gain)
    {
        ESP_LOGI(TAG, "Auto-gain: %dx -> %dx (max_val=0x%08lX)",
                 static_cast<int>(tcs3530_gain_code_to_multiplier(static_cast<uint8_t>(m_current_gain))),
                 static_cast<int>(tcs3530_gain_code_to_multiplier(static_cast<uint8_t>(new_gain))),
                 (unsigned long)max_val);
        ret = setGain(new_gain);
    }

    return ret;
}

esp_err_t TCS3530::sleep()
{
    if (s_debug_mode)
    {
        TCS3530Status status = {};
        getStatus(&status);
        TCS_LOGD(TAG, "STATUS: PON=%d AEN=%d FDEN=%d ALS_VALID=%d DSAT=%d ASAT=%d SAI=%d MEAS_DONE=%d",
                 status.pon, status.aen, status.fden, status.als_data_valid,
                 status.asat_digital, status.asat_analog, status.sai_active, status.meas_complete);
    }

    return enable(false);
}

esp_err_t TCS3530::wake()
{
    return enable(true);
}

esp_err_t TCS3530::setLed(bool on)
{
#if 0
    uint8_t gpio_reg;
    esp_err_t ret = readReg(TCS3530_REG_VSYNC_GPIO_INT, &gpio_reg);
    if (ret != ESP_OK)
    {
        ESP_LOGE(TAG, "Failed to read VSYNC_GPIO_INT register: %s", esp_err_to_name(ret));
        return ret;
    }
    
    // Clear input enable bit to ensure output mode
    gpio_reg &= ~TCS3530_GPIO_INPUT_ENABLE_BIT;
    
    // Set or clear output level bit based on LED state
    if (on)
    {
        gpio_reg |= TCS3530_GPIO_OUTPUT_LEVEL_BIT;
    }
    else
    {
        gpio_reg &= ~TCS3530_GPIO_OUTPUT_LEVEL_BIT;
    }
    
    ret = writeReg(TCS3530_REG_VSYNC_GPIO_INT, gpio_reg);
    if (ret != ESP_OK)
    {
        ESP_LOGE(TAG, "Failed to set LED state: %s", esp_err_to_name(ret));
        return ret;
    }
    
#endif    

    // Control secondary LED GPIO if configured (active low: on=LOW, off=HIGH)
    if (m_secondary_led_gpio != GPIO_NUM_NC)
    {
        gpio_set_level(m_secondary_led_gpio, on ? 0 : 1);
        m_led_on = on;
    }

    if (s_debug_mode)
    {
        ESP_LOGI(TAG, "Internal LED turned %s", on ? "ON" : "OFF");
    }
    
    return ESP_OK;
}

esp_err_t TCS3530::getLed(bool* on) const
{
    if (!on)
    {
        return ESP_ERR_INVALID_ARG;
    }

    // Return the cached LED state
    *on = m_led_on;
    return ESP_OK;
}

esp_err_t TCS3530::getStatus(TCS3530Status* status)
{
    if (!status)
    {
        return ESP_ERR_INVALID_ARG;
    }

    memset(status, 0, sizeof(TCS3530Status));

    uint8_t reg_val = 0;
    esp_err_t ret;

    uint8_t status2 = 0;
    uint8_t status6 = 0;
    ret = readStatusRegs(&status2, &status6);
    if (ret != ESP_OK)
    {
        return ret;
    }
    TCS_LOGD(TAG, "STATUS2 0x%02x", status2);
    status->asat_digital = (status2 & TCS3530_STATUS2_ASAT_DIGITAL_Msk) != 0;
    status->asat_analog = (status2 & TCS3530_STATUS2_ASAT_ANALOG_ANY_Msk) != 0;

    TCS_LOGD(TAG, "STATUS6 0x%02x", status6);
    status->asat_analog = status->asat_analog ||
                          ((status6 & TCS3530_STATUS6_ASAT_ANALOG_MOD_Msk) != 0);

    reg_val = 0;
    ret = readReg(TCS3530_REG_STATUS4, &reg_val);
    if (ret != ESP_OK)
    {
        return ret;
    }
    TCS_LOGD(TAG, "STATUS4 0x%02x", reg_val);
    status->sai_active = (reg_val & TCS3530_STATUS4_SAI_ACTIVE_Msk) != 0;

    reg_val = 0;
    ret = readReg(TCS3530_REG_STATUS5, &reg_val);
    if (ret != ESP_OK)
    {
        return ret;
    }
    TCS_LOGD(TAG, "STATUS5 0x%02x", reg_val);
    status->meas_complete = (reg_val & TCS3530_STATUS5_SINT_MEASUREMENT_SEQUENCER_Msk) != 0;

    reg_val = 0;
    ret = readReg(TCS3530_REG_ALS_DATA_STATUS, &reg_val);
    if (ret != ESP_OK)
    {
        return ret;
    }
    TCS_LOGD(TAG, "ALS_DATA_STATUS 0x%02x", reg_val);
    status->als_data_valid = (reg_val & TCS3530_ALS_DATA_STATUS_VALID_Msk) != 0;

    reg_val = 0;
    ret = readReg(TCS3530_REG_ENABLE, &reg_val);
    if (ret != ESP_OK)
    {
        return ret;
    }
    TCS_LOGD(TAG, "ENABLE 0x%02x", reg_val);
    status->pon = (reg_val & TCS3530_ENABLE_PON_Msk) != 0;
    status->aen = (reg_val & TCS3530_ENABLE_AEN_Msk) != 0;
    status->fden = (reg_val & TCS3530_ENABLE_FDEN_Msk) != 0;

    reg_val = 0;
    ret = readReg(TCS3530_REG_OSCEN, &reg_val);
    if (ret != ESP_OK)
    {
        return ret;
    }
    TCS_LOGD(TAG, "OSCEN 0x%02x", reg_val);

    return ESP_OK;
}
