/**
 * @file power_manager.cpp
 * @brief Power Management Implementation (Simplified)
 *
 * Implements a simple sleep/wake cycle for the color detector:
 * 1. After action completes, enter light sleep with 30-second timer
 * 2. On button wake: return to main loop for next action
 * 3. On timer wake: enter deep sleep
 *
 * Features:
 * - Battery monitoring via MAX17048G+T10 fuel gauge IC
 * - USB VBUS detection to determine power source at boot
 * - ext1 wakeup for both button (active high) and USB detection during deep sleep
 * - GPIO wakeup for button detection during light sleep
 */

#include "power_manager.h"
#include "max17048_driver.h"
#include "audio_renderer.h"
#include "tts_manager.h"
#include "i2c_bus_manager.h"
#include "hardware_pins.h"
#include "console_logger.h"
#include "esp_log.h"
#include "esp_sleep.h"
#include "driver/gpio.h"
#include "driver/rtc_io.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_rom_sys.h"
#include "esp_private/periph_ctrl.h"
#include "soc/soc_caps.h"

#include <cstring>
#include <new>

static const char* TAG = "power";

/** @brief Default sleep timeout in milliseconds (30 seconds) */
#define DEFAULT_SLEEP_TIMEOUT_MS 30000

// RTC memory to persist USB state across deep sleep
// This allows detecting USB state changes (plug/unplug) during deep sleep
static RTC_DATA_ATTR int rtc_usb_state_before_sleep = -1;
static RTC_DATA_ATTR int rtc_spurious_wakeup_count = 0;

// Power manager state
static struct
{
    power_config_t config;
    TCS3530* sensor;
    bool initialized;
    MAX17048* fuel_gauge;
    bool fuel_gauge_available;
    power_wake_cause_t boot_cause;
} s_power;

/**
 * @brief Initialize MAX17048 fuel gauge
 */
static esp_err_t init_fuel_gauge(i2c_master_bus_handle_t i2c_bus)
{
    s_power.fuel_gauge = new (std::nothrow) MAX17048();
    if (!s_power.fuel_gauge)
    {
        ESP_LOGE(TAG, "Failed to allocate MAX17048 instance");
        return ESP_ERR_NO_MEM;
    }

    max17048_config_t fg_config = {};
    fg_config.i2c_bus = i2c_bus;
    fg_config.scl_io_num = -1;
    fg_config.sda_io_num = -1;
    fg_config.i2c_port = -1;

    esp_err_t ret = s_power.fuel_gauge->init(&fg_config);
    if (ret != ESP_OK)
    {
        ESP_LOGW(TAG, "MAX17048 initialization failed: %s", esp_err_to_name(ret));
        delete s_power.fuel_gauge;
        s_power.fuel_gauge = nullptr;
        s_power.fuel_gauge_available = false;
        return ret;
    }

    s_power.fuel_gauge_available = true;
    ESP_LOGI(TAG, "MAX17048 fuel gauge initialized");

    return ESP_OK;
}

/**
 * @brief Initialize button GPIO as regular input
 * 
 * De-initializes GPIO from RTC mode (if needed) and configures it as
 * a regular input with active HIGH logic and external pulldown.
 * 
 * @param button_gpio Button GPIO to initialize
 */
static void init_button_gpio(gpio_num_t button_gpio)
{
    if (button_gpio == GPIO_NUM_NC)
    {
        return;
    }
    
    // De-initialize from RTC mode first (required for gpio_config to work)
    if (rtc_gpio_is_valid_gpio(button_gpio))
    {
        rtc_gpio_hold_dis(button_gpio);
        rtc_gpio_deinit(button_gpio);
    }
    
    // Configure as regular GPIO input (active HIGH, external pulldown)
    gpio_config_t io_conf = {};
    io_conf.pin_bit_mask = (1ULL << button_gpio);
    io_conf.mode = GPIO_MODE_INPUT;
    io_conf.pull_up_en = GPIO_PULLUP_DISABLE;
    io_conf.pull_down_en = GPIO_PULLDOWN_DISABLE;  // External pulldown provided
    io_conf.intr_type = GPIO_INTR_DISABLE;
    gpio_config(&io_conf);
}

/**
 * @brief Enable the sensor peripheral power rail
 *
 * Drives the AO3401 P-MOSFET gate LOW to turn ON the switched VCC rail
 * powering the TCS3530 and the 2N7002 I2C isolation MOSFET gates.
 */
static void sensor_rail_on(void)
{
    // Release any RTC hold from deep sleep
    if (rtc_gpio_is_valid_gpio(POWER_ENABLE_GPIO))
    {
        rtc_gpio_hold_dis(POWER_ENABLE_GPIO);
        rtc_gpio_deinit(POWER_ENABLE_GPIO);
    }

    gpio_config_t io_conf = {};
    io_conf.pin_bit_mask = (1ULL << POWER_ENABLE_GPIO);
    io_conf.mode = GPIO_MODE_OUTPUT;
    io_conf.pull_up_en = GPIO_PULLUP_DISABLE;
    io_conf.pull_down_en = GPIO_PULLDOWN_DISABLE;
    io_conf.intr_type = GPIO_INTR_DISABLE;
    gpio_config(&io_conf);

    // Gate LOW = P-MOSFET ON = sensor powered
    gpio_set_level(POWER_ENABLE_GPIO, 0);
    ESP_LOGI(TAG, "Sensor power rail ON (GPIO%d LOW)", POWER_ENABLE_GPIO);
}

/**
 * @brief Disable the sensor peripheral power rail and hold for deep sleep
 *
 * Drives the AO3401 P-MOSFET gate HIGH to turn OFF the switched VCC rail.
 * Enables gpio_hold_en() and gpio_deep_sleep_hold_en() so the rail stays
 * OFF during deep sleep, preventing phantom powering through SDA/SCL.
 */
static void sensor_rail_off_for_sleep(void)
{
    // Drive gate HIGH = P-MOSFET OFF = sensor unpowered
    gpio_set_level(POWER_ENABLE_GPIO, 1);

    // Enable hold so the HIGH state survives deep sleep
    gpio_hold_en(POWER_ENABLE_GPIO);
    gpio_deep_sleep_hold_en();

    ESP_LOGI(TAG, "Sensor power rail OFF (GPIO%d HIGH, hold enabled)", POWER_ENABLE_GPIO);
}

/**
 * @brief Configure wakeup sources for deep sleep
 * 
 * Configures ext1 for both button (active high, external pulldown) and 
 * USB VBUS (active high) wakeup sources for deep sleep.
 * 
 * @param wakeup_pin Button GPIO for wakeup (active high, ext1)
 * @param usb_detect_pin USB VBUS detect GPIO for wakeup (active high, ext1)
 */
static void configure_deep_sleep_wakeup(gpio_num_t wakeup_pin, gpio_num_t usb_detect_pin)
{
    uint64_t ext1_mask = 0;
    
    // Configure button wakeup pin (ext1, active HIGH with external pulldown)
    if (wakeup_pin != GPIO_NUM_NC && rtc_gpio_is_valid_gpio(wakeup_pin))
    {
        // Clear any previous RTC configuration
        rtc_gpio_hold_dis(wakeup_pin);
        rtc_gpio_deinit(wakeup_pin);
        
        // Configure RTC GPIO for ext1 wakeup
        rtc_gpio_init(wakeup_pin);
        rtc_gpio_set_direction(wakeup_pin, RTC_GPIO_MODE_INPUT_ONLY);
        // No internal pulls - external pulldown provided
        rtc_gpio_pullup_dis(wakeup_pin);
        rtc_gpio_pulldown_dis(wakeup_pin);
        
        ext1_mask |= (1ULL << wakeup_pin);
        
        int gpio_level = rtc_gpio_get_level(wakeup_pin);
        ESP_LOGI(TAG, "Wake button GPIO%d configured for ext1 (level=%d)", wakeup_pin, gpio_level);
        
        if (gpio_level == 1)
        {
            ESP_LOGW(TAG, "WARNING: Button GPIO is HIGH - device may wake immediately");
        }
    }

    // Configure USB VBUS detect pin (ext1, active HIGH)
    if (usb_detect_pin != GPIO_NUM_NC && rtc_gpio_is_valid_gpio(usb_detect_pin))
    {
        // Clear any previous RTC configuration
        rtc_gpio_hold_dis(usb_detect_pin);
        rtc_gpio_deinit(usb_detect_pin);
        
        // Configure RTC GPIO for ext1 wakeup
        rtc_gpio_init(usb_detect_pin);
        rtc_gpio_set_direction(usb_detect_pin, RTC_GPIO_MODE_INPUT_ONLY);
        // USB detect has external voltage divider, no internal pulls needed
        rtc_gpio_pullup_dis(usb_detect_pin);
        rtc_gpio_pulldown_dis(usb_detect_pin);
        
        // Read initial level
        int gpio_level_1 = rtc_gpio_get_level(usb_detect_pin);
        ESP_LOGI(TAG, "USB detect GPIO%d initial level=%d", usb_detect_pin, gpio_level_1);
        
        // Delay and re-read to check stability (voltage divider settling)
        esp_rom_delay_us(10000); // 10ms delay
        int gpio_level_2 = rtc_gpio_get_level(usb_detect_pin);
        
        if (gpio_level_1 != gpio_level_2)
        {
            ESP_LOGW(TAG, "WARNING: USB detect GPIO%d unstable (was %d, now %d) - voltage divider issue?",
                     usb_detect_pin, gpio_level_1, gpio_level_2);
        }
        else
        {
            ESP_LOGI(TAG, "USB detect GPIO%d stable at level=%d", usb_detect_pin, gpio_level_2);
        }

        // Always add USB to wakeup mask to detect state changes (plug AND unplug).
        // We store the current state in RTC memory and verify state actually changed
        // on wakeup to avoid spurious wakes from already-high GPIO.
        ext1_mask |= (1ULL << usb_detect_pin);
        ESP_LOGI(TAG, "USB GPIO%d added to ext1 wakeup mask (current level=%d)", 
                 usb_detect_pin, gpio_level_2);
    }

    // Enable ext1 wakeup if any pins configured (trigger on ANY_HIGH)
    if (ext1_mask != 0)
    {
        esp_err_t ret = esp_sleep_enable_ext1_wakeup(ext1_mask, ESP_EXT1_WAKEUP_ANY_HIGH);
        if (ret != ESP_OK)
        {
            ESP_LOGE(TAG, "Failed to enable ext1 wakeup: %s", esp_err_to_name(ret));
        }
        else
        {
            ESP_LOGI(TAG, "ext1 wakeup enabled (mask=0x%llx, trigger=ANY_HIGH)", ext1_mask);
        }
    }
}

/**
 * @brief Determine boot cause from deep sleep or reset
 */
static power_wake_cause_t determine_boot_cause(const power_config_t* config)
{
    esp_sleep_wakeup_cause_t esp_cause = esp_sleep_get_wakeup_cause();

    switch (esp_cause)
    {
    case ESP_SLEEP_WAKEUP_EXT0:
        // ext0 is no longer used - both button and USB use ext1 now
        ESP_LOGW(TAG, "Unexpected ext0 wakeup - treating as button");
        return POWER_WAKE_BUTTON;
    
    case ESP_SLEEP_WAKEUP_EXT1:
    {
        // Both button and USB now use ext1 - check which GPIO triggered wakeup
        uint64_t wakeup_status = esp_sleep_get_ext1_wakeup_status();
        
        // Check if USB VBUS triggered the wakeup
        if (config->usb_detect_gpio != GPIO_NUM_NC &&
            (wakeup_status & (1ULL << config->usb_detect_gpio)))
        {
            // Verify USB state actually changed to avoid spurious wakeups
            int current_usb_state = gpio_get_level(config->usb_detect_gpio);
            ESP_LOGI(TAG, "USB GPIO triggered ext1 wakeup (before=%d, now=%d, spurious_count=%d)", 
                     rtc_usb_state_before_sleep, current_usb_state, rtc_spurious_wakeup_count);
            
            if (rtc_usb_state_before_sleep != -1 && 
                rtc_usb_state_before_sleep == current_usb_state &&
                current_usb_state == 1)  // Both HIGH
            {
                // USB state didn't change and is still HIGH - likely spurious wakeup
                // This happens when USB was plugged in before sleep and ext1 ANY_HIGH
                // triggers immediately. Increment counter to prevent infinite loops.
                rtc_spurious_wakeup_count++;
                
                if (rtc_spurious_wakeup_count < 3)
                {
                    // Try re-entering deep sleep a few times in case USB gets unplugged
                    ESP_LOGW(TAG, "Spurious USB wakeup (attempt %d) - state unchanged, re-entering deep sleep", 
                             rtc_spurious_wakeup_count);
                    return POWER_WAKE_SPURIOUS;
                }
                else
                {
                    // Too many spurious wakeups - USB is likely stable HIGH
                    // Treat as a USB wake event to avoid infinite loop
                    ESP_LOGW(TAG, "Max spurious wakeups reached - treating as USB wake event");
                    rtc_spurious_wakeup_count = 0;  // Reset for next sleep cycle
                    return POWER_WAKE_USB;
                }
            }
            
            // State changed or went from LOW to HIGH - this is a real wake event
            rtc_spurious_wakeup_count = 0;  // Reset counter
            ESP_LOGI(TAG, "Woken by USB state change (ext1, GPIO%d)", config->usb_detect_gpio);
            return POWER_WAKE_USB;
        }
        
        // Check if button triggered the wakeup
        if (config->wakeup_gpio != GPIO_NUM_NC &&
            (wakeup_status & (1ULL << config->wakeup_gpio)))
        {
            // Trust the ext1 wakeup event - hardware already has external pulldown
            // for debouncing, and requiring button to be held through boot (~2s)
            // creates poor user experience for momentary button presses
            ESP_LOGI(TAG, "Woken by button press (ext1, GPIO%d)", config->wakeup_gpio);
            return POWER_WAKE_BUTTON;
        }
        
        // Unknown ext1 source
        ESP_LOGW(TAG, "Unknown ext1 wakeup source (status=0x%llx)", wakeup_status);
        return POWER_WAKE_UNKNOWN;
    }
    
    case ESP_SLEEP_WAKEUP_GPIO:
        ESP_LOGI(TAG, "Woken by button press (GPIO)");
        return POWER_WAKE_BUTTON;

    case ESP_SLEEP_WAKEUP_TIMER:
        ESP_LOGI(TAG, "Woken by timer");
        return POWER_WAKE_TIMER;

    case ESP_SLEEP_WAKEUP_UNDEFINED:
    default:
        // First boot or reset - check USB first
        if (config->usb_detect_gpio != GPIO_NUM_NC)
        {
            gpio_config_t io_conf = {};
            io_conf.pin_bit_mask = (1ULL << config->usb_detect_gpio);
            io_conf.mode = GPIO_MODE_INPUT;
            io_conf.pull_up_en = GPIO_PULLUP_DISABLE;
            io_conf.pull_down_en = GPIO_PULLDOWN_DISABLE;
            io_conf.intr_type = GPIO_INTR_DISABLE;
            gpio_config(&io_conf);

            if (gpio_get_level(config->usb_detect_gpio) == 1)
            {
                ESP_LOGI(TAG, "USB VBUS detected at boot");
                return POWER_WAKE_USB;
            }
        }
        
        // Check if button is currently pressed (active high with external pulldown)
        if (config->wakeup_gpio != GPIO_NUM_NC)
        {
            gpio_config_t io_conf = {};
            io_conf.pin_bit_mask = (1ULL << config->wakeup_gpio);
            io_conf.mode = GPIO_MODE_INPUT;
            io_conf.pull_up_en = GPIO_PULLUP_DISABLE;
            io_conf.pull_down_en = GPIO_PULLDOWN_DISABLE;  // External pulldown provided
            io_conf.intr_type = GPIO_INTR_DISABLE;
            gpio_config(&io_conf);
            
            vTaskDelay(pdMS_TO_TICKS(10));
            
            if (gpio_get_level(config->wakeup_gpio) == 1)
            {
                ESP_LOGI(TAG, "Button pressed at boot");
                return POWER_WAKE_BUTTON;
            }
        }
        
        ESP_LOGI(TAG, "Unknown boot cause");
        return POWER_WAKE_UNKNOWN;
    }
}

/**
 * @brief Isolate I2C GPIOs to prevent current leakage during deep sleep
 * 
 * This function is now a wrapper that calls the I2C bus manager's
 * deinit and GPIO isolation functions to properly clean up the I2C bus
 * and prevent current draw during deep sleep.
 */
static void isolate_i2c_gpios(void)
{
    // First, deinitialize the I2C bus (deletes bus, which removes devices)
    i2c_bus_manager_deinit();
    
    // Then isolate the GPIOs
    i2c_bus_manager_isolate_gpios_for_sleep();
}

/**
 * @brief Prepare hardware for sleep
 */
static void prepare_for_sleep(void)
{
    // Put sensor to sleep
    if (s_power.sensor)
    {
        s_power.sensor->setLed(false);
        s_power.sensor->sleep();
    }
    
    // Ensure MAX98357A amplifier is shut down (SD_MODE LOW)
    // Use RTC GPIO functions to enable hold during deep sleep
    if (rtc_gpio_is_valid_gpio(I2S_SD_MODE_GPIO))
    {
        // Initialize as RTC GPIO
        rtc_gpio_init(I2S_SD_MODE_GPIO);
        rtc_gpio_set_direction(I2S_SD_MODE_GPIO, RTC_GPIO_MODE_OUTPUT_ONLY);
        
        // Disable pulls (external circuit doesn't need them)
        rtc_gpio_pullup_dis(I2S_SD_MODE_GPIO);
        rtc_gpio_pulldown_dis(I2S_SD_MODE_GPIO);
        
        // Drive LOW to shutdown amplifier
        rtc_gpio_set_level(I2S_SD_MODE_GPIO, 0);
        
        // Enable hold to maintain LOW during deep sleep
        rtc_gpio_hold_en(I2S_SD_MODE_GPIO);
    }
    else
    {
        // Fallback to regular GPIO (won't maintain during deep sleep)
        gpio_set_level(I2S_SD_MODE_GPIO, 0);
        ESP_LOGW(TAG, "SD_MODE GPIO%d is not RTC GPIO - cannot use hold (may leak current)", I2S_SD_MODE_GPIO);
    }
}

/**
 * @brief Restore SD_MODE GPIO after sleep
 *
 * Counterpart to prepare_for_sleep() — releases the RTC hold on
 * SD_MODE and reconfigures it as a regular GPIO output so that
 * audio_renderer_set_enable() can drive it again.
 *
 * Safe to call multiple times and on first boot.
 */
void power_restore_audio_gpio(void)
{
    if (rtc_gpio_is_valid_gpio(I2S_SD_MODE_GPIO))
    {
        rtc_gpio_hold_dis(I2S_SD_MODE_GPIO);
        rtc_gpio_deinit(I2S_SD_MODE_GPIO);

        gpio_config_t io_conf = {};
        io_conf.pin_bit_mask = (1ULL << I2S_SD_MODE_GPIO);
        io_conf.mode = GPIO_MODE_OUTPUT;
        io_conf.pull_up_en = GPIO_PULLUP_DISABLE;
        io_conf.pull_down_en = GPIO_PULLDOWN_DISABLE;
        io_conf.intr_type = GPIO_INTR_DISABLE;
        gpio_config(&io_conf);

        // Keep amplifier muted until audio_renderer_set_enable(true)
        gpio_set_level(I2S_SD_MODE_GPIO, 0);
    }
}

/**
 * @brief Wake sensor from light sleep
 *
 * Counterpart to prepare_for_sleep() - wakes the TCS3530 sensor
 * after returning from light sleep on button press.
 */
static void wake_sensor(void)
{
    if (s_power.sensor)
    {
        s_power.sensor->wake();
    }
}

/**
 * @brief Shutdown audio peripherals before deep sleep
 */
static void shutdown_audio_for_deep_sleep(void)
{
    esp_err_t ret = tts_stop();
    if (ret != ESP_OK)
    {
        ESP_LOGW(TAG, "Failed to stop TTS before deep sleep: %s", esp_err_to_name(ret));
    }

    audio_renderer_prepare_for_sleep();
}

/**
 * @brief Disable fuel gauge alert GPIO before deep sleep
 */
static void prepare_fuel_gauge_gpio_for_sleep(void)
{
    if (s_power.fuel_gauge_available && s_power.fuel_gauge)
    {
        s_power.fuel_gauge->clearAlert();
        s_power.fuel_gauge->hibernate();
    }

    if (s_power.config.max17048_alrt_gpio == GPIO_NUM_NC)
    {
        return;
    }

    gpio_num_t alrt_gpio = s_power.config.max17048_alrt_gpio;
    const bool use_rtc_gpio = rtc_gpio_is_valid_gpio(alrt_gpio);
    if (use_rtc_gpio)
    {
        rtc_gpio_hold_dis(alrt_gpio);
        rtc_gpio_deinit(alrt_gpio);
        rtc_gpio_init(alrt_gpio);
        rtc_gpio_set_direction(alrt_gpio, RTC_GPIO_MODE_INPUT_ONLY);
        rtc_gpio_pullup_dis(alrt_gpio);
        rtc_gpio_pulldown_dis(alrt_gpio);
        rtc_gpio_hold_en(alrt_gpio);
    }
    else
    {
        gpio_config_t io_conf = {};
        io_conf.pin_bit_mask = (1ULL << alrt_gpio);
        io_conf.mode = GPIO_MODE_INPUT;
        io_conf.pull_up_en = GPIO_PULLUP_DISABLE;
        io_conf.pull_down_en = GPIO_PULLDOWN_DISABLE;
        io_conf.intr_type = GPIO_INTR_DISABLE;
        gpio_config(&io_conf);
    }

    ESP_LOGI(TAG, "MAX17048 ALRT GPIO%d configured for deep sleep (%s)", alrt_gpio,
             use_rtc_gpio ? "RTC input + hold" : "GPIO input");
}

/**
 * @brief Speak power down message
 */
static void speak_power_down(void)
{
    audio_renderer_tone_powering_off();
    tts_speak("Bye bye");
    vTaskDelay(pdMS_TO_TICKS(100));
}

/**
 * @brief Wait for button release with timeout
 *
 * Also monitors USB insertion during the wait - if USB is inserted,
 * breaks early to avoid false button press detection from electrical noise.
 */
static void wait_for_button_release(gpio_num_t gpio)
{
    if (gpio == GPIO_NUM_NC)
    {
        return;
    }

    int wait_count = 0;
    const int max_wait_ms = 5000;
    const int poll_interval_ms = 50;

    // Get initial USB state
    bool usb_connected_initial = false;
    if (s_power.config.usb_detect_gpio != GPIO_NUM_NC)
    {
        usb_connected_initial = (gpio_get_level(s_power.config.usb_detect_gpio) == 1);
    }

    while (gpio_get_level(gpio) == 1 && wait_count < max_wait_ms)  // Wait while pressed (active high)
    {
        vTaskDelay(pdMS_TO_TICKS(poll_interval_ms));
        wait_count += poll_interval_ms;

        // Check if USB was just inserted during the wait
        if (s_power.config.usb_detect_gpio != GPIO_NUM_NC)
        {
            bool usb_connected_now = (gpio_get_level(s_power.config.usb_detect_gpio) == 1);
            if (!usb_connected_initial && usb_connected_now)
            {
                ESP_LOGI(TAG, "USB inserted during button release wait - breaking early to avoid false detection");
                return;
            }
        }
    }

    if (wait_count >= max_wait_ms)
    {
        ESP_LOGW(TAG, "Button still pressed after %dms", max_wait_ms);
    }
    else if (wait_count > 0)
    {
        ESP_LOGI(TAG, "Button released after %dms", wait_count);
    }
}

// ============================================================================
// Public API
// ============================================================================

void power_disable_onboard_led(void)
{
    esp_err_t ret;

    // Configure RGB_LED_PWR_GPIO as OUTPUT and set LOW
    gpio_config_t pwr_cfg = {};
    pwr_cfg.pin_bit_mask = (1ULL << RGB_LED_PWR_GPIO);
    pwr_cfg.mode = GPIO_MODE_OUTPUT;
    pwr_cfg.pull_up_en = GPIO_PULLUP_DISABLE;
    pwr_cfg.pull_down_en = GPIO_PULLDOWN_DISABLE;
    pwr_cfg.intr_type = GPIO_INTR_DISABLE;

    ret = gpio_config(&pwr_cfg);
    if (ret != ESP_OK)
    {
        ESP_LOGW(TAG, "Failed to configure RGB LED power GPIO: %s", esp_err_to_name(ret));
        return;
    }

    gpio_set_level(RGB_LED_PWR_GPIO, 0);

    ret = gpio_hold_en(RGB_LED_PWR_GPIO);
    if (ret != ESP_OK)
    {
        ESP_LOGW(TAG, "Failed to enable hold on RGB LED power GPIO: %s", esp_err_to_name(ret));
    }

    // Configure RGB_LED_DATA_GPIO as INPUT (High-Z)
    gpio_config_t data_cfg = {};
    data_cfg.pin_bit_mask = (1ULL << RGB_LED_DATA_GPIO);
    data_cfg.mode = GPIO_MODE_INPUT;
    data_cfg.pull_up_en = GPIO_PULLUP_DISABLE;
    data_cfg.pull_down_en = GPIO_PULLDOWN_DISABLE;
    data_cfg.intr_type = GPIO_INTR_DISABLE;

    ret = gpio_config(&data_cfg);
    if (ret != ESP_OK)
    {
        ESP_LOGW(TAG, "Failed to configure RGB LED data GPIO: %s", esp_err_to_name(ret));
        return;
    }

    ret = gpio_hold_en(RGB_LED_DATA_GPIO);
    if (ret != ESP_OK)
    {
        ESP_LOGW(TAG, "Failed to enable hold on RGB LED data GPIO: %s", esp_err_to_name(ret));
    }

    ESP_LOGI(TAG, "Onboard RGB LED disabled (GPIO %d LOW, GPIO %d High-Z, hold enabled)",
             RGB_LED_PWR_GPIO, RGB_LED_DATA_GPIO);
}

esp_err_t power_init(const power_config_t* config, TCS3530* sensor)
{
    if (!config)
    {
        return ESP_ERR_INVALID_ARG;
    }

    // Guard against double-initialization
    if (s_power.initialized)
    {
        ESP_LOGW(TAG, "Power manager already initialized");
        s_power.sensor = sensor;
        return ESP_OK;
    }

    // Initialize state
    memset(&s_power, 0, sizeof(s_power));
    s_power.config = *config;
    s_power.sensor = sensor;

    // Set default sleep timeout
    if (s_power.config.sleep_timeout_ms == 0)
    {
        s_power.config.sleep_timeout_ms = DEFAULT_SLEEP_TIMEOUT_MS;
    }

    // Determine boot cause
    s_power.boot_cause = determine_boot_cause(config);
    ESP_LOGI(TAG, "Boot cause: %d", s_power.boot_cause);
    
    // Release I2C GPIO holds from deep sleep (if any)
    if (rtc_gpio_is_valid_gpio(I2C_SDA_GPIO))
    {
        rtc_gpio_hold_dis(I2C_SDA_GPIO);
        rtc_gpio_deinit(I2C_SDA_GPIO);
        ESP_LOGD(TAG, "I2C SDA GPIO%d released from RTC mode", I2C_SDA_GPIO);
    }
    
    if (rtc_gpio_is_valid_gpio(I2C_SCL_GPIO))
    {
        rtc_gpio_hold_dis(I2C_SCL_GPIO);
        rtc_gpio_deinit(I2C_SCL_GPIO);
        ESP_LOGD(TAG, "I2C SCL GPIO%d released from RTC mode", I2C_SCL_GPIO);
    }
    
    // Release POWER_ENABLE hold and turn on sensor power rail
    // This must happen before I2C bus init so the TCS3530 and 2N7002
    // isolation MOSFETs are powered before any I2C communication.
    sensor_rail_on();
    
    // Release wakeup GPIO from RTC mode and configure as regular GPIO
    if (config->wakeup_gpio != GPIO_NUM_NC)
    {
        ESP_LOGI(TAG, "Initializing button GPIO%d as regular input", config->wakeup_gpio);
        init_button_gpio(config->wakeup_gpio);
        ESP_LOGI(TAG, "Button GPIO%d initialized", config->wakeup_gpio);
    }

    // Release USB detect GPIO from RTC mode and initialize as regular GPIO
    if (config->usb_detect_gpio != GPIO_NUM_NC)
    {
        // De-initialize from RTC mode if it was configured for deep sleep wakeup
        if (rtc_gpio_is_valid_gpio(config->usb_detect_gpio))
        {
            rtc_gpio_hold_dis(config->usb_detect_gpio);
            rtc_gpio_deinit(config->usb_detect_gpio);
        }
        
        // Configure as regular GPIO input
        gpio_config_t io_conf = {};
        io_conf.pin_bit_mask = (1ULL << config->usb_detect_gpio);
        io_conf.mode = GPIO_MODE_INPUT;
        io_conf.pull_up_en = GPIO_PULLUP_DISABLE;
        io_conf.pull_down_en = GPIO_PULLDOWN_DISABLE;
        io_conf.intr_type = GPIO_INTR_DISABLE;
        gpio_config(&io_conf);
        ESP_LOGI(TAG, "USB detect GPIO%d initialized", config->usb_detect_gpio);
    }

    // Release MAX17048 ALRT GPIO from RTC hold (if configured for deep sleep)
    if (config->max17048_alrt_gpio != GPIO_NUM_NC &&
        rtc_gpio_is_valid_gpio(config->max17048_alrt_gpio))
    {
        rtc_gpio_hold_dis(config->max17048_alrt_gpio);
        rtc_gpio_deinit(config->max17048_alrt_gpio);
        ESP_LOGD(TAG, "MAX17048 ALRT GPIO%d released from RTC mode", config->max17048_alrt_gpio);
    }

    // Initialize MAX17048 fuel gauge
    if (config->i2c_bus)
    {
        init_fuel_gauge(config->i2c_bus);
    }
    else
    {
        ESP_LOGI(TAG, "Battery monitoring disabled (no I2C bus)");
    }
    
    // Enable alert interrupt on fuel gauge
    if (s_power.fuel_gauge_available && config->max17048_alrt_gpio != GPIO_NUM_NC)
    {
        if (s_power.fuel_gauge->enableAlert() == ESP_OK)
        {
            ESP_LOGI(TAG, "MAX17048 ALRT enabled on GPIO%d", config->max17048_alrt_gpio);
            s_power.fuel_gauge->clearAlert();
        }
    }

    // Configure GPIO wakeup for light sleep (active high button)
    if (config->wakeup_gpio != GPIO_NUM_NC)
    {
        gpio_wakeup_enable(config->wakeup_gpio, GPIO_INTR_HIGH_LEVEL);
        esp_sleep_enable_gpio_wakeup();
    }

    s_power.initialized = true;
    ESP_LOGI(TAG, "Power manager initialized (timeout=%ldms)", (long)s_power.config.sleep_timeout_ms);

    return ESP_OK;
}

void power_set_sensor(TCS3530* sensor)
{
    s_power.sensor = sensor;
}

power_wake_cause_t power_get_wake_cause(void)
{
    return s_power.boot_cause;
}

power_wake_cause_t power_enter_sleep(void)
{
    if (!s_power.initialized)
    {
        return POWER_WAKE_UNKNOWN;
    }

    // Debounce delay
//    vTaskDelay(pdMS_TO_TICKS(SLEEP_DEBOUNCE_DELAY_MS));
    wait_for_button_release(s_power.config.wakeup_gpio);

    // Prepare hardware for sleep
    prepare_for_sleep();

    // Check USB state before sleep for both real and simulated paths
    bool usb_connected_before_sleep = false;
    if (s_power.config.usb_detect_gpio != GPIO_NUM_NC)
    {
        usb_connected_before_sleep = (gpio_get_level(s_power.config.usb_detect_gpio) == 1);
    }

#ifdef CONFIG_DEBUG_SIMULATE_SLEEP
    // Simulated light sleep: poll button and timer using vTaskDelay
    // This keeps USB serial console and all peripherals active.
    ESP_LOGI(TAG, "Simulated light sleep (%lu ms) - polling for button/USB",
             (unsigned long)s_power.config.sleep_timeout_ms);

    const uint32_t poll_ms = 50;
    uint32_t elapsed_ms = 0;
    bool button_woke = false;
    bool usb_woke = false;

    while (elapsed_ms < s_power.config.sleep_timeout_ms)
    {
        vTaskDelay(pdMS_TO_TICKS(poll_ms));
        elapsed_ms += poll_ms;

        // Check button press
        if (s_power.config.wakeup_gpio != GPIO_NUM_NC &&
            gpio_get_level(s_power.config.wakeup_gpio) == 1)
        {
            button_woke = true;
            break;
        }

        // Check USB connection
        if (!usb_connected_before_sleep &&
            s_power.config.usb_detect_gpio != GPIO_NUM_NC &&
            gpio_get_level(s_power.config.usb_detect_gpio) == 1)
        {
            usb_woke = true;
            break;
        }
    }

    // Restore SD_MODE GPIO so audio renderer can drive the amplifier
    power_restore_audio_gpio();

    if (usb_woke)
    {
        ESP_LOGI(TAG, "Simulated wake (USB connected)");
        return POWER_WAKE_USB;
    }

    if (button_woke)
    {
        ESP_LOGI(TAG, "Simulated wake (button)");
        wake_sensor();
        return POWER_WAKE_BUTTON;
    }

    // Timer expired - check if USB is connected
    bool usb_connected_now = false;
    if (s_power.config.usb_detect_gpio != GPIO_NUM_NC)
    {
        usb_connected_now = (gpio_get_level(s_power.config.usb_detect_gpio) == 1);
    }

    if (usb_connected_now)
    {
        // USB present - stay in charging-mode light sleep instead of deep sleep
        ESP_LOGI(TAG, "Simulated light sleep timer expired - USB present, staying in charging-mode light sleep");
        return POWER_WAKE_TIMER;  // Return timer wake to indicate we should loop back to light sleep
    }
    else
    {
        // USB not present - proceed to deep sleep as before
        ESP_LOGI(TAG, "Simulated light sleep timer expired - USB absent, entering deep sleep");
        power_enter_deep_sleep();
        // power_enter_deep_sleep() never returns (deep sleep or infinite loop)
    }

    return POWER_WAKE_UNKNOWN;

#else
    // Real light sleep
    esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_ALL);

    // Enable GPIO wakeup for button (active HIGH with external pulldown)
    if (s_power.config.wakeup_gpio != GPIO_NUM_NC)
    {
        gpio_wakeup_enable(s_power.config.wakeup_gpio, GPIO_INTR_HIGH_LEVEL);
    }

    // Enable GPIO wakeup for USB VBUS detection (active HIGH)
    // Always enable USB GPIO wakeup in light sleep to detect state changes
    // (both plug and unplug events)
    if (s_power.config.usb_detect_gpio != GPIO_NUM_NC)
    {
        gpio_wakeup_enable(s_power.config.usb_detect_gpio, GPIO_INTR_HIGH_LEVEL);
    }

    // Enable GPIO wakeup if any wakeup sources were configured
    if (s_power.config.wakeup_gpio != GPIO_NUM_NC || s_power.config.usb_detect_gpio != GPIO_NUM_NC)
    {
        esp_sleep_enable_gpio_wakeup();
    }

    uint64_t sleep_us = (uint64_t)s_power.config.sleep_timeout_ms * 1000ULL;
    esp_sleep_enable_timer_wakeup(sleep_us);
    ESP_LOGI(TAG, "Entering light sleep (%ld sec)", (long)(s_power.config.sleep_timeout_ms / 1000));
    esp_light_sleep_start();

    esp_sleep_wakeup_cause_t cause = esp_sleep_get_wakeup_cause();

    // Check if woken by GPIO - need to determine if it was button or USB
    if (cause == ESP_SLEEP_WAKEUP_GPIO || cause == ESP_SLEEP_WAKEUP_EXT1)
    {
        // Check current USB state
        bool usb_connected_now = (s_power.config.usb_detect_gpio != GPIO_NUM_NC && 
                                  gpio_get_level(s_power.config.usb_detect_gpio) == 1);
        
        // Restore SD_MODE GPIO so audio renderer can drive the amplifier
        power_restore_audio_gpio();

        // Check if USB state changed (either plugged or unplugged)
        if (usb_connected_before_sleep != usb_connected_now)
        {
            if (usb_connected_now)
            {
                // USB was plugged in (state changed from LOW to HIGH)
                ESP_LOGI(TAG, "Woke from light sleep (USB connected)");
                // Don't wake sensor for USB - just report charging status
                return POWER_WAKE_USB;
            }
            else
            {
                // USB was unplugged (state changed from HIGH to LOW)
                // This shouldn't wake us with GPIO_INTR_HIGH_LEVEL, but handle it anyway
                ESP_LOGI(TAG, "Woke from light sleep (USB disconnected) - staying in light sleep");
                return POWER_WAKE_TIMER;  // Loop back to light sleep without announcement
            }
        }
        
        // USB state didn't change - check if it was button or spurious USB wake
        // If USB is connected and button is NOT pressed, it's likely a USB replug (HIGH->LOW->HIGH)
        bool button_pressed = (s_power.config.wakeup_gpio != GPIO_NUM_NC &&
                              gpio_get_level(s_power.config.wakeup_gpio) == 1);
        
        if (usb_connected_now && !button_pressed)
        {
            // USB is high but button is not pressed - likely USB replug while in charging mode
            ESP_LOGI(TAG, "Woke from light sleep (USB replug detected) - staying in charging-mode light sleep");
            return POWER_WAKE_TIMER;  // Loop back to light sleep without announcement
        }
        
        // Button wake
        ESP_LOGI(TAG, "Woke from light sleep (button)");
        wake_sensor();
        return POWER_WAKE_BUTTON;
    }
    
    if (cause == ESP_SLEEP_WAKEUP_TIMER)
    {
        // Timer expired - check if USB is connected
        bool usb_connected_now = false;
        if (s_power.config.usb_detect_gpio != GPIO_NUM_NC)
        {
            usb_connected_now = (gpio_get_level(s_power.config.usb_detect_gpio) == 1);
        }

        if (usb_connected_now)
        {
            // USB present - stay in charging-mode light sleep instead of deep sleep
            ESP_LOGI(TAG, "Light sleep timer expired - USB present, staying in charging-mode light sleep");
            
            // Restore SD_MODE GPIO so audio can work if needed
            power_restore_audio_gpio();
            
            return POWER_WAKE_TIMER;  // Return timer wake to indicate we should loop back to light sleep
        }
        else
        {
            // USB not present - proceed to deep sleep as before
            ESP_LOGI(TAG, "Light sleep timer expired - USB absent, entering deep sleep");
            power_enter_deep_sleep();
            // Does not return
        }
    }

    // Unexpected wake - enter deep sleep
    ESP_LOGW(TAG, "Unexpected wake cause: %d - entering deep sleep", cause);
    power_enter_deep_sleep();
    // Does not return

    return POWER_WAKE_UNKNOWN;
#endif // CONFIG_DEBUG_SIMULATE_SLEEP
}

void power_enter_deep_sleep(void)
{
    if (!s_power.initialized)
    {
        ESP_LOGE(TAG, "Power manager not initialized");
        return;
    }

    ESP_LOGI(TAG, "========== ENTERING DEEP SLEEP ==========");

    // Step 1: Speak power down message
    ESP_LOGI(TAG, "Step 1: Speaking power down message");
    speak_power_down();

    // Step 1.5: Save console log to NVS if errors occurred (only on battery power)
    if (!power_is_usb_connected())
    {
        ESP_LOGI(TAG, "Step 1.5: Checking for errors to save console log");
        esp_err_t log_err = console_logger_save_if_errors();
        if (log_err == ESP_OK)
        {
            ESP_LOGI(TAG, "Console log saved to NVS (errors detected)");
        }
        else if (log_err == ESP_ERR_INVALID_STATE)
        {
            ESP_LOGI(TAG, "No errors in this session, log not saved");
        }
        else
        {
            ESP_LOGW(TAG, "Failed to save console log: %s", esp_err_to_name(log_err));
        }
    }
    else
    {
        ESP_LOGI(TAG, "Step 1.5: Skipped log save (on USB power)");
    }

    // Step 2: Prepare hardware for sleep
    ESP_LOGI(TAG, "Step 2: Preparing hardware for sleep");
    shutdown_audio_for_deep_sleep();
    prepare_fuel_gauge_gpio_for_sleep();
    prepare_for_sleep();

    // Disable all wakeup sources
    esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_ALL);
    
    if (s_power.config.wakeup_gpio != GPIO_NUM_NC)
    {
        gpio_wakeup_disable(s_power.config.wakeup_gpio);
    }

    wait_for_button_release(s_power.config.wakeup_gpio);

    // Step 3: Save USB state to RTC memory for spurious wakeup detection
    ESP_LOGI(TAG, "Step 3: Saving USB state to RTC memory");
    if (s_power.config.usb_detect_gpio != GPIO_NUM_NC)
    {
        rtc_usb_state_before_sleep = gpio_get_level(s_power.config.usb_detect_gpio);
        ESP_LOGI(TAG, "USB state before sleep: %d", rtc_usb_state_before_sleep);
    }
    else
    {
        rtc_usb_state_before_sleep = -1; // No USB detection configured
    }

    // Step 4: Configure wakeup sources
    ESP_LOGI(TAG, "Step 4: Configuring deep sleep wakeup sources");
    configure_deep_sleep_wakeup(s_power.config.wakeup_gpio, s_power.config.usb_detect_gpio);

    // Step 5: Turn off sensor power rail (I2C bus must be deleted first)
    ESP_LOGI(TAG, "Step 5: Isolating I2C GPIOs and turning off sensor power rail");
    isolate_i2c_gpios();
    sensor_rail_off_for_sleep();
    
    // Step 6: Isolate unused RTC GPIOs to prevent current leakage.
    // Skip GPIOs that have active holds or specific configurations that
    // must be preserved during deep sleep.
    for (int i = 1; i < GPIO_NUM_MAX; i++)
    {
        gpio_num_t gpio = (gpio_num_t)i;

        if (!rtc_gpio_is_valid_gpio(gpio))
        {
            continue;
        }

        // Skip GPIOs with active holds that must retain state
        if (gpio == POWER_ENABLE_GPIO ||  // Held HIGH - sensor rail OFF
            gpio == I2S_SD_MODE_GPIO)      // Held LOW - amplifier shutdown
        {
            continue;
        }

        // Skip wakeup GPIOs - configured for ext1 deep sleep wakeup
        if (gpio == s_power.config.wakeup_gpio ||
            gpio == s_power.config.usb_detect_gpio)
        {
            continue;
        }

        // Skip I2C GPIOs - already configured as inputs by i2c_bus_manager
        if (gpio == I2C_SDA_GPIO || gpio == I2C_SCL_GPIO)
        {
            continue;
        }

        // Skip MAX17048 ALRT GPIO if configured with hold
        if (s_power.config.max17048_alrt_gpio != GPIO_NUM_NC &&
            gpio == s_power.config.max17048_alrt_gpio)
        {
            continue;
        }

        // Skip RGB LED GPIOs - held to keep onboard LED disabled
        if (gpio == RGB_LED_PWR_GPIO || gpio == RGB_LED_DATA_GPIO)
        {
            continue;
        }

        rtc_gpio_isolate(gpio);
    }
    esp_sleep_config_gpio_isolate();

    ESP_LOGI(TAG, "Entering deep sleep NOW");
    esp_deep_sleep_start();
}

int power_get_battery_voltage_mv(void)
{
    if (!s_power.fuel_gauge_available || !s_power.fuel_gauge)
    {
        return -1;
    }

    int voltage_mv = 0;
    esp_err_t ret = s_power.fuel_gauge->getVoltage(&voltage_mv);
    if (ret != ESP_OK)
    {
        ESP_LOGD(TAG, "Failed to read battery voltage: %s", esp_err_to_name(ret));
        return -1;
    }

    return voltage_mv;
}

int power_get_battery_level(void)
{
    if (!s_power.fuel_gauge_available || !s_power.fuel_gauge)
    {
        return -1;
    }

    float soc = 0.0f;
    esp_err_t ret = s_power.fuel_gauge->getSOC(&soc);
    if (ret != ESP_OK)
    {
        ESP_LOGD(TAG, "Failed to read battery SOC: %s", esp_err_to_name(ret));
        return -1;
    }

    if (soc > 100.0f) soc = 100.0f;
    if (soc < 0.0f) soc = 0.0f;

    return (int)(soc + 0.5f);
}

bool power_is_usb_connected(void)
{
    if (s_power.config.usb_detect_gpio == GPIO_NUM_NC)
    {
        return false;
    }
    return gpio_get_level(s_power.config.usb_detect_gpio) == 1;
}

bool power_is_battery_connected(void)
{
    if (!s_power.fuel_gauge_available || !s_power.fuel_gauge)
    {
        return false;
    }
    return s_power.fuel_gauge->isBatteryConnected();
}

int power_get_time_to_full_charge_minutes(void)
{
    const float CC_PHASE_MINUTES_PER_PERCENT = 1.25f;
    const float CV_PHASE_MINUTES_PER_PERCENT = 1.5f;
    const int SAFETY_BUFFER_MINUTES = 5;
    const int CC_TO_CV_TRANSITION_PERCENT = 80;
    
    if (!power_is_usb_connected())
    {
        return -1;
    }
    
    int battery_level = power_get_battery_level();
    if (battery_level < 0 || battery_level >= 100)
    {
        return -1;
    }
    
    int estimated_minutes = 0;
    
    if (battery_level < CC_TO_CV_TRANSITION_PERCENT)
    {
        int cc_percent_remaining = CC_TO_CV_TRANSITION_PERCENT - battery_level;
        estimated_minutes += (int)(cc_percent_remaining * CC_PHASE_MINUTES_PER_PERCENT);
        
        int cv_percent_remaining = 100 - CC_TO_CV_TRANSITION_PERCENT;
        estimated_minutes += (int)(cv_percent_remaining * CV_PHASE_MINUTES_PER_PERCENT);
    }
    else
    {
        int cv_percent_remaining = 100 - battery_level;
        estimated_minutes = (int)(cv_percent_remaining * CV_PHASE_MINUTES_PER_PERCENT);
    }
    
    estimated_minutes += SAFETY_BUFFER_MINUTES;
    
    return estimated_minutes;
}
