/**
 * @file max17048_driver.h
 * @brief Driver for MAX17048G+T10 Fuel Gauge IC
 * 
 * The MAX17048 is a fuel gauge IC that provides state-of-charge (SOC)
 * and voltage information for single-cell lithium-ion batteries.
 * 
 * Features:
 * - I2C interface (7-bit address: 0x36)
 * - Direct SOC percentage reading (0-100%)
 * - Battery voltage reading
 * - Low-power operation
 * - No calibration required for typical Li-ion cells
 */

#ifndef MAX17048_DRIVER_H
#define MAX17048_DRIVER_H

#include "esp_err.h"
#include "driver/i2c_master.h"

/**
 * @brief MAX17048 I2C address (7-bit)
 */
#define MAX17048_I2C_ADDR 0x36

/**
 * @brief MAX17048 register addresses
 */
#define MAX17048_REG_VCELL    0x02  /**< Battery voltage (16-bit) */
#define MAX17048_REG_SOC      0x04  /**< State of charge (16-bit) */
#define MAX17048_REG_MODE     0x06  /**< Mode register */
#define MAX17048_REG_VERSION  0x08  /**< Version register */
#define MAX17048_REG_HIBRT    0x0A  /**< Hibernation register */
#define MAX17048_REG_CONFIG   0x0C  /**< Configuration register */
#define MAX17048_REG_VALRT    0x14  /**< Voltage alert threshold register */
#define MAX17048_REG_COMMAND  0xFE  /**< Command register */

/**
 * @brief MAX17048 CONFIG register bit definitions
 */
#define MAX17048_CONFIG_ALRT   0x0020  /**< Alert bit (read/write to clear) */
#define MAX17048_CONFIG_ALSC   0x0040  /**< Alert on SOC change */

/**
 * @brief Minimum battery voltage (mV) to consider battery connected
 * 
 * LiPo cells have a nominal voltage of 3.7V and discharge cutoff around 3.0V.
 * A reading below 2.5V indicates no battery is connected.
 */
#define MIN_BATTERY_VOLTAGE_MV 2500

/**
 * @brief MAX17048 configuration
 */
typedef struct
{
    i2c_master_bus_handle_t i2c_bus;  /**< I2C bus handle (if NULL, driver will create its own bus) */
    int scl_io_num;                    /**< I2C SCL GPIO number (required if i2c_bus is NULL) */
    int sda_io_num;                    /**< I2C SDA GPIO number (required if i2c_bus is NULL) */
    int i2c_port;                      /**< I2C port number (default: I2C_NUM_1, ignored if i2c_bus provided) */
} max17048_config_t;

/**
 * @brief MAX17048 driver class
 */
class MAX17048
{
public:
    /**
     * @brief Constructor
     */
    MAX17048();

    /**
     * @brief Destructor
     */
    ~MAX17048();

    /**
     * @brief Initialize the MAX17048 fuel gauge
     * 
     * @param config Configuration structure
     * @return ESP_OK on success, error code otherwise
     */
    esp_err_t init(const max17048_config_t* config);

    /**
     * @brief Get battery state of charge percentage
     * 
     * @param soc Pointer to store SOC value (0-100%)
     * @return ESP_OK on success, error code otherwise
     */
    esp_err_t getSOC(float* soc);

    /**
     * @brief Get battery voltage in millivolts
     * 
     * @param voltage_mv Pointer to store voltage in mV
     * @return ESP_OK on success, error code otherwise
     */
    esp_err_t getVoltage(int* voltage_mv);

    /**
     * @brief Quick start - forces a restart of the fuel gauge calculations
     * 
     * Use this after significant battery changes or on first power-up.
     * 
     * @return ESP_OK on success, error code otherwise
     */
    esp_err_t quickStart();

    /**
     * @brief Check if chip is present and responding
     * 
     * @return true if chip detected, false otherwise
     */
    bool isPresent();

    /**
     * @brief Check if battery is connected to the fuel gauge
     * 
     * Determines battery presence by checking cell voltage.
     * A voltage below MIN_BATTERY_VOLTAGE_MV indicates no battery.
     * 
     * @return true if battery is connected, false otherwise
     */
    bool isBatteryConnected();

    /**
     * @brief Enable alert interrupt for SOC changes
     * 
     * Configures the MAX17048 to trigger the ALRT pin when:
     * - Battery voltage changes (charging/discharging)
     * - SOC percentage changes
     * 
     * This is useful for detecting when charging starts.
     * 
     * @return ESP_OK on success, error code otherwise
     */
    esp_err_t enableAlert();

    /**
     * @brief Check if alert flag is set
     * 
     * @param is_set Pointer to store alert status (true if alert is active)
     * @return ESP_OK on success, error code otherwise
     */
    esp_err_t isAlertSet(bool* is_set);

    /**
     * @brief Clear alert flag
     * 
     * Clears the ALRT bit in the CONFIG register, which also resets
     * the ALRT pin to inactive state.
     * 
     * @return ESP_OK on success, error code otherwise
     */
    esp_err_t clearAlert();

    /**
     * @brief Enable hiberation mode
     * 
     * Sets the hibernation mode for deep sleep.
     * 
     * @return ESP_OK on success, error code otherwise
     */
    esp_err_t hibernate();


private:
    i2c_master_dev_handle_t m_dev_handle;
    i2c_master_bus_handle_t m_i2c_bus;
    bool m_owns_bus;  // True if we created our own I2C bus
    bool m_initialized;

    /**
     * @brief Read 16-bit register
     * 
     * @param reg Register address
     * @param value Pointer to store value
     * @return ESP_OK on success, error code otherwise
     */
    esp_err_t readRegister16(uint8_t reg, uint16_t* value);

    /**
     * @brief Write 16-bit register
     * 
     * @param reg Register address
     * @param value Value to write
     * @return ESP_OK on success, error code otherwise
     */
    esp_err_t writeRegister16(uint8_t reg, uint16_t value);
};

#endif /* MAX17048_DRIVER_H */
