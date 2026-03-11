/**
 * @file i2c_bus_manager.h
 * @brief I2C Bus Manager - Centralized I2C bus initialization and management
 *
 * This module provides a single source of truth for:
 * - I2C bus creation/configuration (pins, port, pullups, glitch filter, queue depth)
 * - Access to the shared i2c_master_bus_handle_t
 * - I2C device tracking and cleanup
 * - Deep-sleep preparation behavior for I2C GPIOs (isolate SDA/SCL)
 *
 * The shared I2C bus is used by:
 * - TCS3530 color sensor (I2C address 0x39)
 * - MAX17048 fuel gauge (I2C address 0x36)
 *
 * Device Management:
 * Devices should register themselves with i2c_bus_manager_register_device() after
 * calling i2c_master_bus_add_device(). This allows the bus manager to properly
 * clean up all devices before deleting the bus during deep sleep preparation.
 */

#pragma once

#include "driver/i2c_master.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Initialize the I2C bus manager and create the shared I2C master bus
 * 
 * Creates and configures the I2C bus using settings from Kconfig:
 * - CONFIG_I2C_MASTER_SDA_IO
 * - CONFIG_I2C_MASTER_SCL_IO
 * 
 * Internal pullups are enabled for proper I2C operation during normal use.
 * Before deep sleep, i2c_bus_manager_deinit() and 
 * i2c_bus_manager_isolate_gpios_for_sleep() should be called to prevent
 * current leakage.
 * 
 * @return ESP_OK on success, error code otherwise
 */
esp_err_t i2c_bus_manager_init(void);

/**
 * @brief Get the shared I2C bus handle
 * 
 * Use this handle when calling i2c_master_bus_add_device() to add devices
 * to the shared I2C bus.
 * 
 * @return i2c_master_bus_handle_t The shared I2C bus handle, or NULL if not initialized
 */
i2c_master_bus_handle_t i2c_bus_manager_get(void);

/**
 * @brief Register an I2C device with the bus manager
 * 
 * Call this after successfully adding a device to the bus with i2c_master_bus_add_device().
 * The bus manager will track this device and ensure it is properly removed before
 * the bus is deleted during deep sleep preparation.
 * 
 * @param dev_handle Device handle returned by i2c_master_bus_add_device()
 * @return ESP_OK on success, ESP_ERR_NO_MEM if device list is full
 */
esp_err_t i2c_bus_manager_register_device(i2c_master_dev_handle_t dev_handle);

/**
 * @brief Unregister an I2C device from the bus manager
 * 
 * Call this before removing a device from the bus with i2c_master_bus_rm_device()
 * (e.g., in a device destructor). This removes the device from the manager's
 * tracking list.
 * 
 * @param dev_handle Device handle to unregister
 * @return ESP_OK on success, ESP_ERR_NOT_FOUND if device was not registered
 */
esp_err_t i2c_bus_manager_unregister_device(i2c_master_dev_handle_t dev_handle);

/**
 * @brief Deinitialize the I2C bus before deep sleep
 * 
 * Removes all registered I2C devices from the bus and then deletes the I2C master
 * bus to free resources before entering deep sleep. This should be called before
 * i2c_bus_manager_isolate_gpios_for_sleep().
 * 
 * All registered devices are automatically removed via i2c_master_bus_rm_device()
 * before the bus is deleted. This ensures proper cleanup even if device destructors
 * haven't been called (e.g., for static or global objects).
 */
void i2c_bus_manager_deinit(void);

/**
 * @brief Isolate I2C GPIOs for deep sleep to prevent current leakage
 * 
 * Configures I2C GPIOs (SDA and SCL) as inputs with no pulls.  The 2N7002
 * I2C isolation MOSFETs disconnect the ESP32 side from the sensor side when
 * the switched peripheral rail is turned OFF.
 * 
 * This should be called after i2c_bus_manager_deinit() as part of the deep
 * sleep preparation sequence, before the sensor power rail is turned off.
 */
void i2c_bus_manager_isolate_gpios_for_sleep(void);

#ifdef __cplusplus
}
#endif
