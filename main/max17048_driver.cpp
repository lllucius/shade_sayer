/**
 * @file max17048_driver.cpp
 * @brief Implementation of MAX17048G+T10 Fuel Gauge Driver
 */

#include "max17048_driver.h"
#include "i2c_bus_manager.h"
#include "esp_log.h"
#include <cstring>

static const char* TAG = "max17048";

/**
 * @brief I2C timeout in milliseconds
 */
#define I2C_TIMEOUT_MS 1000

MAX17048::MAX17048()
    : m_dev_handle(nullptr)
    , m_i2c_bus(nullptr)
    , m_owns_bus(false)
    , m_initialized(false)
{
}

MAX17048::~MAX17048()
{
    if (m_dev_handle)
    {
        // Unregister from bus manager first
        i2c_bus_manager_unregister_device(m_dev_handle);
        
        // Then remove device from bus
        i2c_master_bus_rm_device(m_dev_handle);
        m_dev_handle = nullptr;
    }
    
    // Delete the I2C bus if we created it
    if (m_owns_bus && m_i2c_bus)
    {
        i2c_del_master_bus(m_i2c_bus);
        m_i2c_bus = nullptr;
    }
    
    m_initialized = false;
}

esp_err_t MAX17048::init(const max17048_config_t* config)
{
    if (!config)
    {
        ESP_LOGE(TAG, "Invalid configuration");
        return ESP_ERR_INVALID_ARG;
    }

    if (m_initialized)
    {
        ESP_LOGW(TAG, "Already initialized");
        return ESP_OK;
    }

    i2c_master_bus_handle_t i2c_bus = config->i2c_bus;
    
    // If no I2C bus provided, create our own
    if (!i2c_bus)
    {
        // Validate GPIO numbers (ESP32-S3 has GPIOs 0-48, but some are not available)
        // We do a basic range check here
        const int MAX_GPIO_NUM = 48;  // ESP32-S3 specific
        if (config->scl_io_num < 0 || config->scl_io_num > MAX_GPIO_NUM)
        {
            ESP_LOGE(TAG, "Invalid SCL GPIO number: %d", config->scl_io_num);
            return ESP_ERR_INVALID_ARG;
        }
        if (config->sda_io_num < 0 || config->sda_io_num > MAX_GPIO_NUM)
        {
            ESP_LOGE(TAG, "Invalid SDA GPIO number: %d", config->sda_io_num);
            return ESP_ERR_INVALID_ARG;
        }
        
        ESP_LOGI(TAG, "Creating dedicated I2C bus for MAX17048");
        
        i2c_master_bus_config_t bus_cfg = {};
        bus_cfg.clk_source = I2C_CLK_SRC_DEFAULT;
        // Use provided I2C port or default to I2C_NUM_1
        bus_cfg.i2c_port = (config->i2c_port >= 0) ? config->i2c_port : I2C_NUM_1;
        bus_cfg.scl_io_num = static_cast<gpio_num_t>(config->scl_io_num);
        bus_cfg.sda_io_num = static_cast<gpio_num_t>(config->sda_io_num);
        bus_cfg.glitch_ignore_cnt = 7;
        bus_cfg.intr_priority = 0;      // Use default interrupt priority.
        bus_cfg.trans_queue_depth = 0;  // No async queue (blocking transfers only).
        bus_cfg.flags.enable_internal_pullup = true;
        
        esp_err_t ret = i2c_new_master_bus(&bus_cfg, &m_i2c_bus);
        if (ret != ESP_OK)
        {
            ESP_LOGE(TAG, "Failed to create I2C bus: %s", esp_err_to_name(ret));
            return ret;
        }
        
        i2c_bus = m_i2c_bus;
        m_owns_bus = true;
        ESP_LOGI(TAG, "I2C bus created on port %d (SCL: GPIO%d, SDA: GPIO%d)", 
                 bus_cfg.i2c_port, config->scl_io_num, config->sda_io_num);
    }

    // Configure I2C device
    i2c_device_config_t dev_cfg = {};
    dev_cfg.dev_addr_length = I2C_ADDR_BIT_LEN_7;
    dev_cfg.device_address = MAX17048_I2C_ADDR;
    dev_cfg.scl_speed_hz = 100000;  // 100kHz for MAX17048

    esp_err_t ret = i2c_master_bus_add_device(i2c_bus, &dev_cfg, &m_dev_handle);
    if (ret != ESP_OK)
    {
        ESP_LOGE(TAG, "Failed to add I2C device: %s", esp_err_to_name(ret));
        if (m_owns_bus && m_i2c_bus)
        {
            i2c_del_master_bus(m_i2c_bus);
            m_i2c_bus = nullptr;
            m_owns_bus = false;
        }
        return ret;
    }

    // Register device with I2C bus manager for automatic cleanup
    ret = i2c_bus_manager_register_device(m_dev_handle);
    if (ret != ESP_OK)
    {
        ESP_LOGW(TAG, "Failed to register device with bus manager: %s", esp_err_to_name(ret));
        // Not a fatal error - device can still operate, just won't be auto-cleaned up
    }

    // Verify device is present by reading version register
    uint16_t version = 0;
    ret = readRegister16(MAX17048_REG_VERSION, &version);
    if (ret != ESP_OK)
    {
        ESP_LOGE(TAG, "Failed to communicate with MAX17048");
        // Mirror destructor cleanup order: unregister from bus manager first
        // so it does not retain a dangling handle, then remove from the bus.
        i2c_bus_manager_unregister_device(m_dev_handle);
        i2c_master_bus_rm_device(m_dev_handle);
        m_dev_handle = nullptr;
        if (m_owns_bus && m_i2c_bus)
        {
            i2c_del_master_bus(m_i2c_bus);
            m_i2c_bus = nullptr;
            m_owns_bus = false;
        }
        return ret;
    }

    ESP_LOGI(TAG, "MAX17048 detected, version: 0x%04X", version);

    // Perform quick start to ensure fresh calculation
    ret = quickStart();
    if (ret != ESP_OK)
    {
        ESP_LOGW(TAG, "Quick start failed, continuing anyway");
    }

    m_initialized = true;
    ESP_LOGI(TAG, "MAX17048 initialized successfully");

    return ESP_OK;
}

esp_err_t MAX17048::getSOC(float* soc)
{
    if (!m_initialized || !m_dev_handle)
    {
        return ESP_ERR_INVALID_STATE;
    }

    if (!soc)
    {
        return ESP_ERR_INVALID_ARG;
    }

    uint16_t raw_soc = 0;
    esp_err_t ret = readRegister16(MAX17048_REG_SOC, &raw_soc);
    if (ret != ESP_OK)
    {
        return ret;
    }

    // SOC register format:
    // Upper byte: percentage (0-100)
    // Lower byte: fractional part (1/256)
    uint8_t percent = (raw_soc >> 8) & 0xFF;
    uint8_t fraction = raw_soc & 0xFF;

    *soc = percent + (fraction / 256.0f);

    return ESP_OK;
}

esp_err_t MAX17048::getVoltage(int* voltage_mv)
{
    if (!m_initialized || !m_dev_handle)
    {
        return ESP_ERR_INVALID_STATE;
    }

    if (!voltage_mv)
    {
        return ESP_ERR_INVALID_ARG;
    }

    uint16_t raw_vcell = 0;
    esp_err_t ret = readRegister16(MAX17048_REG_VCELL, &raw_vcell);
    if (ret != ESP_OK)
    {
        return ret;
    }

    // VCELL register format: 78.125µV per LSB
    // Formula: voltage_mV = raw_vcell * 78.125 / 1000 = raw_vcell * 0.078125
    // To avoid floating point: voltage_mV = (raw_vcell * 78125) / 1000000
    // Simplified for better precision: voltage_mV = (raw_vcell * 5) / 64 (approximation)
    // Better: voltage_mV = (raw_vcell * 78125) / 1000000
    *voltage_mv = (int)(((uint32_t)raw_vcell * 78125UL) / 1000000UL);

    return ESP_OK;
}

esp_err_t MAX17048::quickStart()
{
    if (!m_dev_handle)
    {
        return ESP_ERR_INVALID_STATE;
    }

    // Read current MODE register value
    uint16_t mode_value = 0;
    esp_err_t ret = readRegister16(MAX17048_REG_MODE, &mode_value);
    if (ret != ESP_OK)
    {
        return ret;
    }

    // Set QuickStart bit (bit 14)
    mode_value |= 0x4000;

    // Write back to MODE register
    ret = writeRegister16(MAX17048_REG_MODE, mode_value);
    if (ret != ESP_OK)
    {
        ESP_LOGE(TAG, "Failed to write QuickStart command");
        return ret;
    }

    ESP_LOGI(TAG, "QuickStart command sent");
    return ESP_OK;
}

bool MAX17048::isPresent()
{
    if (!m_dev_handle)
    {
        return false;
    }

    uint16_t version = 0;
    esp_err_t ret = readRegister16(MAX17048_REG_VERSION, &version);
    return (ret == ESP_OK);
}

bool MAX17048::isBatteryConnected()
{
    if (!m_initialized || !m_dev_handle)
    {
        return false;
    }

    // Read battery voltage
    int voltage_mv = 0;
    esp_err_t ret = getVoltage(&voltage_mv);
    if (ret != ESP_OK)
    {
        ESP_LOGW(TAG, "Failed to read voltage for battery detection");
        return false;
    }

    // Battery is connected if voltage is above minimum threshold
    // LiPo cells have nominal 3.7V; readings below 2.5V indicate no battery
    bool connected = (voltage_mv >= MIN_BATTERY_VOLTAGE_MV);
    
    ESP_LOGD(TAG, "Battery voltage: %dmV, connected: %s", 
             voltage_mv, connected ? "yes" : "no");
    
    return connected;
}

esp_err_t MAX17048::enableAlert()
{
    if (!m_initialized || !m_dev_handle)
    {
        return ESP_ERR_INVALID_STATE;
    }

    // Read current CONFIG register
    uint16_t config = 0;
    esp_err_t ret = readRegister16(MAX17048_REG_CONFIG, &config);
    if (ret != ESP_OK)
    {
        ESP_LOGE(TAG, "Failed to read CONFIG register");
        return ret;
    }

    // Enable ALSC (Alert on SOC change) to trigger ALRT pin on any SOC change
    // This will detect both charging and discharging events
    config |= MAX17048_CONFIG_ALSC;

    // Write back to CONFIG register
    ret = writeRegister16(MAX17048_REG_CONFIG, config);
    if (ret != ESP_OK)
    {
        ESP_LOGE(TAG, "Failed to enable alert");
        return ret;
    }

    ESP_LOGI(TAG, "Alert enabled (ALSC set, CONFIG=0x%04X)", config);
    return ESP_OK;
}

esp_err_t MAX17048::isAlertSet(bool* is_set)
{
    if (!m_initialized || !m_dev_handle)
    {
        return ESP_ERR_INVALID_STATE;
    }

    if (!is_set)
    {
        return ESP_ERR_INVALID_ARG;
    }

    // Read CONFIG register
    uint16_t config = 0;
    esp_err_t ret = readRegister16(MAX17048_REG_CONFIG, &config);
    if (ret != ESP_OK)
    {
        return ret;
    }

    // Check ALRT bit (bit 5)
    *is_set = (config & MAX17048_CONFIG_ALRT) != 0;

    return ESP_OK;
}

esp_err_t MAX17048::clearAlert()
{
    if (!m_initialized || !m_dev_handle)
    {
        return ESP_ERR_INVALID_STATE;
    }

    // Read current CONFIG register
    uint16_t config = 0;
    esp_err_t ret = readRegister16(MAX17048_REG_CONFIG, &config);
    if (ret != ESP_OK)
    {
        ESP_LOGE(TAG, "Failed to read CONFIG register");
        return ret;
    }

    // Clear ALRT bit by writing 0 to it
    config &= ~MAX17048_CONFIG_ALRT;

    // Write back to CONFIG register
    ret = writeRegister16(MAX17048_REG_CONFIG, config);
    if (ret != ESP_OK)
    {
        ESP_LOGE(TAG, "Failed to clear alert");
        return ret;
    }

    ESP_LOGD(TAG, "Alert cleared (CONFIG=0x%04X)", config);
    return ESP_OK;
}

esp_err_t MAX17048::hibernate()
{
    if (!m_initialized || !m_dev_handle)
    {
        return ESP_ERR_INVALID_STATE;
    }

    // Set the HIBRT register to always hibernate
    uint16_t threshold = 0xffff;
    esp_err_t ret = writeRegister16(MAX17048_REG_HIBRT, threshold);
    if (ret != ESP_OK)
    {
        ESP_LOGE(TAG, "Failed to enable hibernation");
        return ret;
    }

    ESP_LOGI(TAG, "Hibernation enabled");
    return ESP_OK;
}

esp_err_t MAX17048::readRegister16(uint8_t reg, uint16_t* value)
{
    if (!m_dev_handle || !value)
    {
        return ESP_ERR_INVALID_ARG;
    }

    uint8_t data[2] = {0};
    
    esp_err_t ret = i2c_master_transmit_receive(
        m_dev_handle,
        &reg,
        1,
        data,
        2,
        I2C_TIMEOUT_MS
    );

    if (ret != ESP_OK)
    {
        ESP_LOGE(TAG, "Failed to read register 0x%02X: %s", reg, esp_err_to_name(ret));
        return ret;
    }

    // MAX17048 sends MSB first
    *value = (data[0] << 8) | data[1];

    return ESP_OK;
}

esp_err_t MAX17048::writeRegister16(uint8_t reg, uint16_t value)
{
    if (!m_dev_handle)
    {
        return ESP_ERR_INVALID_ARG;
    }

    uint8_t data[3];
    data[0] = reg;
    data[1] = (value >> 8) & 0xFF;  // MSB first
    data[2] = value & 0xFF;

    esp_err_t ret = i2c_master_transmit(
        m_dev_handle,
        data,
        3,
        I2C_TIMEOUT_MS
    );

    if (ret != ESP_OK)
    {
        ESP_LOGE(TAG, "Failed to write register 0x%02X: %s", reg, esp_err_to_name(ret));
        return ret;
    }

    return ESP_OK;
}
