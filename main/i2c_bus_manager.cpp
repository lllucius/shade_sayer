/**
 * @file i2c_bus_manager.cpp
 * @brief I2C Bus Manager Implementation
 *
 * This module centralizes I2C bus management to provide:
 * 1. Single initialization point for shared I2C bus configuration
 * 2. Consistent I2C settings (pins, clock, pullups, glitch filtering)
 * 3. I2C device tracking to ensure proper cleanup
 * 4. Proper deep sleep preparation (device removal, bus deletion, GPIO isolation)
 *
 * The I2C bus is shared between multiple devices:
 * - TCS3530 color sensor (I2C address 0x39)
 * - MAX17048 fuel gauge (I2C address 0x36)
 *
 * Device Tracking:
 * The bus manager maintains a list of registered I2C device handles. Devices should
 * register themselves after calling i2c_master_bus_add_device(). During deep sleep
 * preparation, the bus manager automatically removes all registered devices before
 * deleting the bus, ensuring proper cleanup even if device destructors haven't run.
 *
 * Deep Sleep Power Optimization:
 * Before entering deep sleep, the I2C bus must be properly shut down to prevent
 * current leakage:
 * 1. Remove all I2C devices from the bus (handled automatically by i2c_bus_manager_deinit)
 * 2. Delete the I2C master bus (i2c_bus_manager_deinit)
 * 3. Set I2C GPIOs to input with no pulls (i2c_bus_manager_isolate_gpios_for_sleep)
 * 4. Turn off the switched peripheral rail (AO3401 gate HIGH via power_manager)
 *
 * The 2N7002 I2C isolation MOSFETs (one per SDA, one per SCL) have their gates
 * tied to the switched VCC rail in a star topology.  When the rail is OFF the
 * MOSFETs disconnect the ESP32 side from the sensor side, preventing phantom
 * powering of the TCS3530 through the I2C lines.
 */

#include "i2c_bus_manager.h"
#include "hardware_pins.h"
#include "driver/gpio.h"
#include "driver/rtc_io.h"
#include "esp_log.h"
#include <vector>

static const char* TAG = "i2c_bus_mgr";

//===========================================================================
// I2C Bus Configuration
//===========================================================================

/** @brief I2C port number (ESP32-S3 supports I2C_NUM_0 and I2C_NUM_1) */
#define I2C_MASTER_PORT I2C_NUM_0

/** @brief Shared I2C bus handle (singleton pattern) */
static i2c_master_bus_handle_t s_i2c_bus = nullptr;

/** @brief List of registered I2C device handles for cleanup */
static std::vector<i2c_master_dev_handle_t> s_i2c_devices;

esp_err_t i2c_bus_manager_init(void)
{
    // Check if already initialized (idempotent - safe to call multiple times)
    if (s_i2c_bus != nullptr)
    {
        ESP_LOGW(TAG, "I2C bus manager already initialized");
        return ESP_OK;
    }

    // Configure I2C master bus settings
    i2c_master_bus_config_t bus_cfg = {};
    bus_cfg.clk_source = I2C_CLK_SRC_DEFAULT;  // Use default clock (typically APB)
    bus_cfg.i2c_port = I2C_MASTER_PORT;
    bus_cfg.scl_io_num = I2C_SCL_GPIO;
    bus_cfg.sda_io_num = I2C_SDA_GPIO;
    
    // Glitch filter: Ignore pulses shorter than 7 APB clock cycles.
    // This helps filter electrical noise on the I2C bus, improving reliability.
    // Value of 7 is a good balance between noise immunity and signal responsiveness.
    bus_cfg.glitch_ignore_cnt = 7;
    
    bus_cfg.intr_priority = 0;      // Use default interrupt priority
    bus_cfg.trans_queue_depth = 0;  // No async queue - use blocking transfers only
                                     // This simplifies the driver and is sufficient
                                     // for infrequent sensor reads
    
    // Enable internal pullups for proper I2C operation during normal use.
    // Note: The I2C GPIOs are properly isolated as high-impedance inputs before
    // entering deep sleep (via i2c_bus_manager_isolate_gpios_for_sleep), which
    // prevents any current leakage through these internal pullups during sleep.
    bus_cfg.flags.enable_internal_pullup = true;

    // Create the I2C master bus
    esp_err_t ret = i2c_new_master_bus(&bus_cfg, &s_i2c_bus);
    if (ret != ESP_OK)
    {
        ESP_LOGE(TAG, "Failed to create I2C bus: %s", esp_err_to_name(ret));
        s_i2c_bus = nullptr;
        return ret;
    }

    ESP_LOGI(TAG, "I2C bus initialized on port %d (SCL: GPIO%d, SDA: GPIO%d)",
             I2C_MASTER_PORT, static_cast<int>(I2C_SCL_GPIO), static_cast<int>(I2C_SDA_GPIO));

    return ESP_OK;
}

i2c_master_bus_handle_t i2c_bus_manager_get(void)
{
    return s_i2c_bus;
}

esp_err_t i2c_bus_manager_register_device(i2c_master_dev_handle_t dev_handle)
{
    if (dev_handle == nullptr)
    {
        ESP_LOGW(TAG, "Cannot register NULL device handle");
        return ESP_ERR_INVALID_ARG;
    }

    // Check if device is already registered
    for (const auto& dev : s_i2c_devices)
    {
        if (dev == dev_handle)
        {
            ESP_LOGW(TAG, "Device handle %p already registered", dev_handle);
            return ESP_OK;  // Already registered, not an error
        }
    }

    // Add to the device list
    try
    {
        s_i2c_devices.push_back(dev_handle);
        ESP_LOGI(TAG, "Registered I2C device %p (total: %d)", dev_handle, s_i2c_devices.size());
        return ESP_OK;
    }
    catch (const std::bad_alloc&)
    {
        ESP_LOGE(TAG, "Failed to register device: out of memory");
        return ESP_ERR_NO_MEM;
    }
}

esp_err_t i2c_bus_manager_unregister_device(i2c_master_dev_handle_t dev_handle)
{
    if (dev_handle == nullptr)
    {
        ESP_LOGW(TAG, "Cannot unregister NULL device handle");
        return ESP_ERR_INVALID_ARG;
    }

    // Find and remove the device from the list
    for (auto it = s_i2c_devices.begin(); it != s_i2c_devices.end(); ++it)
    {
        if (*it == dev_handle)
        {
            s_i2c_devices.erase(it);
            ESP_LOGI(TAG, "Unregistered I2C device %p (remaining: %d)", dev_handle, s_i2c_devices.size());
            return ESP_OK;
        }
    }

    ESP_LOGW(TAG, "Device handle %p not found in registry", dev_handle);
    return ESP_ERR_NOT_FOUND;
}

void i2c_bus_manager_deinit(void)
{
    // First, remove all registered I2C devices from the bus
    if (!s_i2c_devices.empty())
    {
        ESP_LOGI(TAG, "Removing %d registered I2C device(s) before deleting bus", s_i2c_devices.size());
        
        // Iterate through all registered devices and remove them
        for (auto dev_handle : s_i2c_devices)
        {
            if (dev_handle != nullptr)
            {
                ESP_LOGI(TAG, "Removing I2C device %p from bus", dev_handle);
                esp_err_t ret = i2c_master_bus_rm_device(dev_handle);
                if (ret != ESP_OK)
                {
                    ESP_LOGW(TAG, "Failed to remove device %p: %s", dev_handle, esp_err_to_name(ret));
                }
            }
        }
        
        // Clear the device list
        s_i2c_devices.clear();
        ESP_LOGI(TAG, "All I2C devices removed from bus");
    }
    
    // Then, delete the I2C master bus itself
    if (s_i2c_bus != nullptr)
    {
        ESP_LOGI(TAG, "Deleting I2C bus");
        
        // Delete the I2C master bus.
        // All devices must be removed first (which we just did above).
        esp_err_t ret = i2c_del_master_bus(s_i2c_bus);
        if (ret != ESP_OK)
        {
            ESP_LOGW(TAG, "Failed to delete I2C bus: %s", esp_err_to_name(ret));
        }
        else
        {
            ESP_LOGI(TAG, "I2C bus deleted successfully");
        }
        
        // Clear the handle to prevent use-after-free
        s_i2c_bus = nullptr;
    }
}

void i2c_bus_manager_isolate_gpios_for_sleep(void)
{
    //=======================================================================
    // Configure I2C SDA/SCL as high-impedance inputs with no pulls.
    //
    // The 2N7002 I2C isolation MOSFETs (one per line) have their gates
    // tied to the switched VCC rail (AO3401 drain).  When the power rail
    // is turned OFF the MOSFET gates collapse to 0 V, disconnecting the
    // ESP32 side from the sensor side.  This eliminates phantom powering
    // of the TCS3530 through SDA/SCL.
    //
    // We still configure the ESP32 GPIOs as inputs with no pulls so they
    // do not source or sink any current through their own pads.
    //=======================================================================

    // SDA
    gpio_config_t io_conf = {};
    io_conf.pin_bit_mask = (1ULL << I2C_SDA_GPIO);
    io_conf.mode = GPIO_MODE_INPUT;
    io_conf.pull_up_en = GPIO_PULLUP_DISABLE;
    io_conf.pull_down_en = GPIO_PULLDOWN_DISABLE;
    io_conf.intr_type = GPIO_INTR_DISABLE;
    gpio_config(&io_conf);
    ESP_LOGI(TAG, "I2C SDA GPIO%d set to input, no pulls", I2C_SDA_GPIO);

    // SCL
    io_conf.pin_bit_mask = (1ULL << I2C_SCL_GPIO);
    gpio_config(&io_conf);
    ESP_LOGI(TAG, "I2C SCL GPIO%d set to input, no pulls", I2C_SCL_GPIO);

    ESP_LOGI(TAG, "I2C GPIOs configured for deep sleep (2N7002 isolation active)");
}
