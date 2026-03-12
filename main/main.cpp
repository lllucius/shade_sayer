/**
 * @file main.cpp
 * @brief Color Detection Device for Visually Impaired Users
 * 
 * Hardware Configuration (Unexpected Maker TinyS3D):
 * ==================================================
 * GPIO Assignment Table:
 * ----------------------
 * GPIO 1        - Button input (RTC GPIO, ext1 wakeup source)
 * GPIO 2        - MAX98357A SD_MODE (RTC GPIO, deep sleep hold enabled)
 * GPIO 6        - POWER_ENABLE (AO3401 P-MOSFET gate, RTC GPIO, deep sleep hold enabled)
 * GPIO 7        - USB VBUS detection (RTC GPIO, ext1 wakeup, voltage divider: 5V->1M->GPIO7->1M->GND, 100nF to GND)
 * GPIO 8        - I2C SDA (TCS3530 + MAX17048 shared bus, 2N7002 isolated)
 * GPIO 9        - I2C SCL (TCS3530 + MAX17048 shared bus, 2N7002 isolated)
 * GPIO 10       - MAX17048 ALRT (optional, not used for deep sleep wakeup)
 * GPIO 34       - I2S LRCLK (MAX98357A)
 * GPIO 36       - I2S BCLK (MAX98357A)
 * GPIO 37       - I2S DOUT (MAX98357A)
 * 
 * GPIO 35       - Secondary sensor illumination LED (active low)
 * 
 * TCS3530 INT:  Not connected (driver uses I2C polling)
 * TCS3530 LED:  Internal LED controlled via VSYNC/GPIO pin
 * 
 * RTC GPIO Notes:
 * ---------------
 * ESP32-S3 RTC GPIOs (0-21) support:
 *   - ext1 wakeup from deep sleep
 *   - rtc_gpio_hold_en() for per-pin state retention during sleep
 * 
 * GPIO 1 (Button):  RTC GPIO required for ext1 deep sleep wakeup
 * GPIO 2 (SD_MODE): RTC GPIO allows deep sleep hold to keep amp disabled
 * 
 * Power Management Flow:
 * =====================
 * 1. User presses button -> action performed
 * 2. After action, enter light sleep with 30-second timer
 * 3. If timer expires -> deep sleep
 * 4. If button pressed -> wake, perform action, goto step 2
 * 
 * USB Wake Handling:
 * ==================
 * - Device wakes from deep sleep when USB cable is plugged in (ext1 wakeup on GPIO 7 HIGH)
 * - On USB wake, TCS3530 sensor remains off, no measurement is taken
 * - Device announces charging status and battery level
 * - Enters light sleep with 30-second timer, then deep sleep if no button press
 * - During charging, single button press announces charge level
 */

#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <new>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_system.h"
#include "nvs_flash.h"
#include "esp_sleep.h"

#include "tcs3530_driver.h"
#include "color_pipeline.h"
#include "color_description.h"
#include "user_interface.h"
#include "tts_manager.h"
#include "power_manager.h"
#include "audio_renderer.h"
#include "auto_calibrate.h"
#include "i2c_bus_manager.h"
#include "hardware_pins.h"
#include "console_logger.h"
#include "kona_metadata.h"

static const char* TAG = "main";

// Firmware version
#define FIRMWARE_VERSION "1.0.2"

// Default integration time if configured value is invalid (0)
#define DEFAULT_INTEGRATION_TIME_MS 100

// I2S sample rate
#define I2S_SAMPLE_RATE         CONFIG_I2S_SAMPLE_RATE

// Integration time
#define ALS_INTEGRATION_TIME_MS CONFIG_ALS_INTEGRATION_TIME_MS

#ifdef CONFIG_POWER_SAVE_TIMEOUT_S
#define POWER_SAVE_TIMEOUT_S    CONFIG_POWER_SAVE_TIMEOUT_S
#else
#define POWER_SAVE_TIMEOUT_S    (30000)
#endif

// Button event detection timeout (ms)
#define BUTTON_EVENT_TIMEOUT_MS 5000

// Kona swatch scanning
#define KONA_SWATCH_TOTAL            365
#define KONA_SWATCH_ID_BUFFER_SIZE   24
#define KONA_SWATCH_NAME_BUFFER_SIZE 48

#if CONFIG_KONA_SCAN_STRIDE < 1
#define KONA_SCAN_STRIDE 1
#else
#define KONA_SCAN_STRIDE CONFIG_KONA_SCAN_STRIDE
#endif

// Global handles
static TCS3530* s_sensor = nullptr;

// Buffer size for spoken color descriptions
#define TTS_DESCRIPTION_BUFFER_SIZE  512

// Text buffer for spoken descriptions
static char s_description_buffer[TTS_DESCRIPTION_BUFFER_SIZE];

// Last measured color result for double-click description
static color_result_t s_last_result;
static bool s_has_last_result = false;

// Next swatch index for Kona scan workflow
static uint16_t s_kona_scan_index = 0;
static bool s_kona_scan_active = false;

static void speak_kona_swatch_prompt(uint16_t index)
{
    if (index >= KONA_SWATCH_METADATA_COUNT)
    {
        ESP_LOGW(TAG, "Kona metadata index out of range: %u", index);
        return;
    }

    const kona_swatch_info_t& info = KONA_SWATCH_METADATA[index];
    tts_speak("Capture swatch %d of %d. Panel %s, index %d, ID %d, name %s. Sampling every %d. Press button to capture, or double click to cancel.",
              index + 1,
              KONA_SWATCH_TOTAL,
              info.panel,
              info.panel_index,
              info.id,
              info.name,
              KONA_SCAN_STRIDE);
}

/**
 * @brief Announce battery level and estimated time to full
 */
static void speak_battery_status(void)
{
    // Check if battery is connected
    if (!power_is_battery_connected())
    {
        ESP_LOGW(TAG, "No battery detected");
        tts_speak("No battery detected");
        return;
    }

    // Get battery level
    int battery_level = power_get_battery_level();
    if (battery_level < 0)
    {
        ESP_LOGW(TAG, "Battery level unavailable");
        tts_speak("Battery level unavailable");
        return;
    }

    // Get estimated time to full charge
    int time_to_full = power_get_time_to_full_charge_minutes();
    
    int battery_mv = power_get_battery_voltage_mv();
    ESP_LOGI(TAG, "Battery level: %d%% (%dmV), estimated %d minutes to full", 
             battery_level, battery_mv, time_to_full);
    
    // Announce current level
    tts_speak("Battery at %d percent", battery_level);
    
    // Announce estimated time to full if available
    if (time_to_full > 0)
    {
        int hours = time_to_full / 60;
        int minutes = time_to_full % 60;

        if (hours > 0 && minutes > 0)
        {
            tts_speak("Estimated %d hour%s and %d minute%s to full charge",
                      hours, (hours == 1) ? "" : "s",
                      minutes, (minutes == 1) ? "" : "s");
        }
        else if (hours > 0)
        {
            tts_speak("Estimated %d hour%s to full charge",
                      hours, (hours == 1) ? "" : "s");
        }
        else
        {
            tts_speak("Estimated %d minute%s to full charge",
                      time_to_full, (time_to_full == 1) ? "" : "s");
        }
    }
}

/**
 * @brief Speak the generated description for the last measured color
 */
static void speak_color_description(void)
{
    if (!s_has_last_result)
    {
        ESP_LOGI(TAG, "No color measured yet");
        tts_speak("No color has been measured yet. Press the button to take a measurement.");
        return;
    }

    char desc_buffer[TTS_DESCRIPTION_BUFFER_SIZE];
    int len = color_description_generate(&s_last_result.lab, desc_buffer, sizeof(desc_buffer));

    if (len > 0)
    {
        ESP_LOGI(TAG, "Color description: %s", desc_buffer);
        tts_speak("%s is %s", s_last_result.color_name, desc_buffer);
    }
    else
    {
        ESP_LOGW(TAG, "Failed to generate color description");
        tts_speak("Could not generate a description for this color.");
    }
}

/**
 * @brief Auto-calibration callback for user feedback
 */
static void auto_cal_callback(const char* message)
{
    if (message)
    {
        ESP_LOGI(TAG, "Auto-cal: %s", message);
        tts_speak(message);
    }
}

/**
 * @brief Perform automatic calibration using reference colors
 */
static void perform_auto_calibration(void)
{
    ESP_LOGI(TAG, "Starting automatic calibration");
    tts_speak("Starting automatic color calibration.  Please wait.");

    // Initialize calibration context
    auto_cal_ctx_t* cal_ctx = nullptr;
    esp_err_t ret = auto_cal_init(&cal_ctx);
    if (ret != ESP_OK || ! cal_ctx)
    {
        ESP_LOGE(TAG, "Failed to initialize calibration context");
        tts_speak("Calibration initialization failed.");
        return;
    }
    
    auto_cal_set_callback(cal_ctx, auto_cal_callback);

    // Dark Gray reference FIRST - captures sensor offset before any other measurements
    // Measured before White to get raw RESP-normalized values (no white normalization)
    // 
    // NOTE: We use a dark gray swatch (RGB 35,31,32, L*≈11.5) instead of pure black because:
    // 1. Pure black has nearly zero reflectance, so sensor mostly measures noise and LED reflections
    // 2. Dark gray has enough reflectance (~1.4%) for stable, repeatable measurements
    // 3. Still captures sensor offset (LED reflections, noise, stray light)
    // 4. The calibration code scales the measurement to extract only the sensor offset
    auto_cal_add_reference_rgb(cal_ctx, "Dark Gray", 35, 31, 32, 
                                CAL_REF_FLAG_IS_BLACK | CAL_REF_FLAG_REQUIRED);

    // The function converts RGB→Lab internally
    auto_cal_add_reference_rgb(cal_ctx, "White",  241, 241, 242, 
                                CAL_REF_FLAG_IS_WHITE | CAL_REF_FLAG_REQUIRED);
    auto_cal_add_reference_rgb(cal_ctx, "Brights Red",    237,  28,  36, 
                                CAL_REF_FLAG_REQUIRED);
    auto_cal_add_reference_rgb(cal_ctx, "Brights Green",    0, 161,  75, 
                                CAL_REF_FLAG_REQUIRED);
    auto_cal_add_reference_rgb(cal_ctx, "Brights Blue",    33,  63, 153, 
                                CAL_REF_FLAG_REQUIRED);
    auto_cal_add_reference_rgb(cal_ctx, "Brights Yellow", 255, 221,  23, 
                                CAL_REF_FLAG_REQUIRED);
    auto_cal_add_reference_rgb(cal_ctx, "Brights Orange", 241, 101,  33, 
                                CAL_REF_FLAG_REQUIRED);
    auto_cal_add_reference_rgb(cal_ctx, "Cyan",     0, 173, 239, 
                                CAL_REF_FLAG_REQUIRED);
    auto_cal_add_reference_rgb(cal_ctx, "Skin",   194, 180, 154, 
                                CAL_REF_FLAG_REQUIRED);

    // Dark chromatic references to improve dark color identification
    // Fills the L*≈20-40 gap between Dark Gray (L*≈12) and mid-tone references (L*≈50+)
    // CAL_REF_FLAG_DARK_CHROMATIC gives them reduced optimizer weight (0.75×) so they
    // improve dark-color identification without distorting the bright-color calibration.
    auto_cal_add_reference_rgb(cal_ctx, "Dark Brown",  96,  56,  19, 
                                CAL_REF_FLAG_REQUIRED | CAL_REF_FLAG_DARK_CHROMATIC);
    auto_cal_add_reference_rgb(cal_ctx, "Dark Taupe",  89,  74,  65, 
                                CAL_REF_FLAG_REQUIRED | CAL_REF_FLAG_DARK_CHROMATIC);
    auto_cal_add_reference_rgb(cal_ctx, "Dark Green",   0, 103,  56, 
                                CAL_REF_FLAG_REQUIRED | CAL_REF_FLAG_DARK_CHROMATIC);

    // Gray reference for lightness/gamma calibration
    // Using single Gray 50 reference - simpler optimization works better
    // Multiple gray references over-constrained the optimizer (degraded from ΔE=3.35 to ΔE=5.18)
    auto_cal_add_reference_rgb(cal_ctx, "Gray 50", 147, 149, 151,
                                CAL_REF_FLAG_GRAY | CAL_REF_FLAG_IS_NEUTRAL | CAL_REF_FLAG_REQUIRED);
    auto_cal_add_reference_rgb(cal_ctx, "Gray 20", 209, 210, 212,
                                CAL_REF_FLAG_GRAY | CAL_REF_FLAG_IS_NEUTRAL | CAL_REF_FLAG_REQUIRED);
    auto_cal_add_reference_rgb(cal_ctx, "Gray 80", 88, 88, 91,
                                CAL_REF_FLAG_GRAY | CAL_REF_FLAG_IS_NEUTRAL | CAL_REF_FLAG_REQUIRED);

    // Browns are chromatic warm references, not neutral grays.
    auto_cal_add_reference_rgb(cal_ctx, "Brown 1", 59, 35, 20,
                                CAL_REF_FLAG_REQUIRED | CAL_REF_FLAG_DARK_CHROMATIC);
    auto_cal_add_reference_rgb(cal_ctx, "Brown 2", 138, 93, 59,
                                CAL_REF_FLAG_REQUIRED);
    auto_cal_add_reference_rgb(cal_ctx, "Brown 3", 195, 165, 107,
                                CAL_REF_FLAG_REQUIRED);
    auto_cal_add_reference_rgb(cal_ctx, "Mid Green", 0, 147, 68, CAL_REF_FLAG_REQUIRED);
    
    ret = auto_cal_start(cal_ctx);
    if (ret != ESP_OK)
    {
        ESP_LOGE(TAG, "Failed to start calibration");
        tts_speak("Failed to start calibration.");
        auto_cal_deinit(&cal_ctx);
        return;
    }
    
    // Guide user through measuring each reference color
    while (true)
    {
        const char* ref_name = auto_cal_get_current_ref_name(cal_ctx);
        if (!ref_name)
        {
            break;  // All measurements collected
        }
        
        ESP_LOGI(TAG, "Place %s reference and press button", ref_name);
        tts_speak("Place %s reference and press button", ref_name);
        
        ui_event_t event = ui_wait_event(60000);
        
        if (event != UI_EVENT_BUTTON_PRESS)
        {
            ESP_LOGW(TAG, "Calibration timeout or cancelled");
            tts_speak("Calibration cancelled.");
            auto_cal_deinit(&cal_ctx);
            return;
        }
        
        // Take measurement with LED on
        s_sensor->setLed(true);
        ESP_LOGI(TAG, "LED: Illumination ON for calibration measurement");
        
        vTaskDelay(pdMS_TO_TICKS(100));
        
        sensor_reading_t reading;
        ret = s_sensor->measure(&reading);
        
        s_sensor->setLed(false);
        ESP_LOGI(TAG, "LED: Illumination OFF");
        
        if (ret != ESP_OK)
        {
            ESP_LOGE(TAG, "Failed to read sensor");
            tts_speak("Sensor read failed.  Try again.");
            continue;
        }
        
        // Normalize raw counts using RESP factors
        float gain_multiplier = tcs3530_gain_code_to_multiplier(reading.gain);
        float integration_scale = (float)reading.integration_ms / 100.0f;
        
        if (gain_multiplier < 0.1f) gain_multiplier = 1.0f;
        if (integration_scale < 0.01f) integration_scale = 1.0f;
        
        // RESP-normalized values (sensor-relative)
        float resp_x = (float)reading.x / (TCS3530_RESP_X * gain_multiplier * integration_scale);
        float resp_y = (float)reading.y / (TCS3530_RESP_Y * gain_multiplier * integration_scale);
        float resp_z = (float)reading.z / (TCS3530_RESP_Z * gain_multiplier * integration_scale);
        
        ESP_LOGI(TAG, "Raw: X=%lu Y=%lu Z=%lu (gain=%.1fx, int=%dms)",
                 (unsigned long)reading.x, (unsigned long)reading.y, 
                 (unsigned long)reading.z, gain_multiplier, reading.integration_ms);
        ESP_LOGI(TAG, "RESP normalized: X=%.2f Y=%.2f Z=%.2f", resp_x, resp_y, resp_z);
        
        xyz_t xyz;
        xyz.x = resp_x;
        xyz.y = resp_y;
        xyz.z = resp_z;

        ret = auto_cal_submit_measurement(cal_ctx, &xyz);
        if (ret != ESP_OK)
        {
            ESP_LOGE(TAG, "Failed to submit measurement");
            tts_speak("Failed to record measurement.");
            auto_cal_deinit(&cal_ctx);
            return;
        }
        
        tts_speak("Recorded.");
    }
    
    // Run optimization
    ESP_LOGI(TAG, "Running calibration optimization");
    tts_speak("Optimizing.  Please wait.");
    ret = auto_cal_run_optimization(cal_ctx);
    if (ret != ESP_OK)
    {
        ESP_LOGE(TAG, "Optimization failed");
        tts_speak("Optimization failed.");
        auto_cal_deinit(&cal_ctx);
        return;
    }
    
    const cal_status_t* status = auto_cal_get_status(cal_ctx);
    if (status && status->state == CAL_STATE_COMPLETE)
    {
        ESP_LOGI(TAG, "Calibration complete.  Error: %.2f -> %.2f",
                 status->initial_error, status->best_error);
        
        tts_speak("Calibration complete. Average error %.1f", status->best_error);
        
        ret = auto_cal_save_to_nvs(cal_ctx, nullptr);
        if (ret == ESP_OK)
        {
            ESP_LOGI(TAG, "Calibration saved to NVS");
        }
    }
    
    auto_cal_apply_results(cal_ctx);  // This calls color_pipeline_set_params()

    auto_cal_deinit(&cal_ctx);
    ESP_LOGI(TAG, "Auto-calibration complete");

}

/**
 * @brief Perform color measurement and speak the result
 */
static void perform_measurement(void)
{
    color_result_t result;

    ESP_LOGI(TAG, "Starting color measurement");
    ESP_LOGI(TAG, "UI: Indicating measurement in progress");

    s_sensor->setLed(true);  // Turn on LED
    ESP_LOGI(TAG, "LED: Illumination ON");

    tts_speak_async("Measuring");

    ESP_LOGI(TAG, "Sensor: Using Fixed Balanced Gains (X16/Y128/Z512)");

    ESP_LOGI(TAG, "Sensor: Taking XYZ measurement...");
    esp_err_t ret = color_pipeline_identify(s_sensor, &result);

    s_sensor->setLed(false);  // Turn off LED
    ESP_LOGI(TAG, "LED: Illumination OFF");

    if (ret != ESP_OK)
    {
        ESP_LOGE(TAG, "Measurement failed: %s", esp_err_to_name(ret));
        tts_speak("An error occurred");
        return;
    }

    s_last_result = result;
    s_has_last_result = true;

    // Log measurement results
    const char* display_name = result.color_name;
    if (result.confidence <= 0.01f)
    {
        display_name = "No Match";
    }

    if (result.kona_matched)
    {
        ESP_LOGI(TAG, "Result: %s [Kona %u] (category: %s, dE=%.2f, conf=%.0f%%)",
                 display_name,
                 (unsigned int)result.kona_id,
                 result.category,
                 result.delta_e,
                 result.confidence * 100);
    }
    else
    {
        ESP_LOGI(TAG, "Result: %s (category: %s, dE=%.2f, conf=%.0f%%)",
                 display_name, result.category, result.delta_e, result.confidence * 100);
    }
    ESP_LOGI(TAG, "XYZ: X=%.2f Y=%.2f Z=%.2f", result.xyz.x, result.xyz.y, result.xyz.z);
    ESP_LOGI(TAG, "Lab: L=%.1f a=%.1f b=%.1f", result.lab.l, result.lab.a, result.lab.b);
    ESP_LOGI(TAG, "RGB: R=%d G=%d B=%d", result.rgb[0], result.rgb[1], result.rgb[2]);

    int len = color_pipeline_describe(&result, s_description_buffer, sizeof(s_description_buffer));
    if (len > 0)
    {
        ESP_LOGI(TAG, "Description: %s", s_description_buffer);
        tts_speak(s_description_buffer);
    }

    ESP_LOGI(TAG, "Measurement complete");
}

/**
 * @brief Capture one Kona swatch record and emit KONA_SCAN_CSV log row.
 */
static void capture_kona_swatch(void)
{
    if (!s_sensor)
    {
        ESP_LOGE(TAG, "Sensor not initialized for Kona scan");
        tts_speak("Sensor not ready");
        return;
    }

    if (s_kona_scan_index >= KONA_SWATCH_TOTAL || s_kona_scan_index >= KONA_SWATCH_METADATA_COUNT)
    {
        ESP_LOGI(TAG, "Kona scan sequence complete (%u swatches)", KONA_SWATCH_TOTAL);
        tts_speak("Kona scan is complete");
        s_kona_scan_active = false;
        return;
    }

    const uint16_t swatch_number = static_cast<uint16_t>(s_kona_scan_index + 1);
    const kona_swatch_info_t& info = KONA_SWATCH_METADATA[s_kona_scan_index];

    char swatch_id[KONA_SWATCH_ID_BUFFER_SIZE] = {0};
    char swatch_name[KONA_SWATCH_NAME_BUFFER_SIZE] = {0};
    snprintf(swatch_id, sizeof(swatch_id), "idx_%03u", swatch_number);
    snprintf(swatch_name, sizeof(swatch_name), "%s", info.name);

    ESP_LOGI(TAG, "Kona scan capture %u/%u (%s, panel=%s panel_index=%u id=%u name=%s)",
             swatch_number, KONA_SWATCH_TOTAL, swatch_id, info.panel, info.panel_index, info.id, info.name);

    color_result_t scan_result = {};
    esp_err_t ret = color_pipeline_capture_csv(s_sensor,
                                               true,
                                               swatch_id,
                                               swatch_name,
                                               &scan_result);
    if (ret != ESP_OK)
    {
        ESP_LOGE(TAG, "Kona scan capture failed: %s", esp_err_to_name(ret));
        if (ret == ESP_ERR_INVALID_STATE)
        {
            tts_speak("Scan failed, retry");
        }
        else
        {
            tts_speak("Scan failed");
        }
        return;
    }

    s_last_result = scan_result;
    s_has_last_result = true;

    s_kona_scan_index = static_cast<uint16_t>(s_kona_scan_index + KONA_SCAN_STRIDE);
    tts_speak("Captured %s, ID %d", info.name, info.id);

    if (s_kona_scan_index < KONA_SWATCH_TOTAL && s_kona_scan_index < KONA_SWATCH_METADATA_COUNT)
    {
        speak_kona_swatch_prompt(s_kona_scan_index);
    }
    else
    {
        tts_speak("All swatches captured");
        s_kona_scan_active = false;
    }
}

extern "C" void app_main(void)
{
    ESP_LOGI(TAG, "Color Detector for Visually Impaired");
    ESP_LOGI(TAG, "Firmware version: %s", FIRMWARE_VERSION);
    ESP_LOGI(TAG, "Free heap: %lu bytes", (unsigned long)esp_get_free_heap_size());

    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND)
    {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    // Initialize console logger to capture all output
    // This must be done early to capture initialization logs
    ret = console_logger_init();
    if (ret != ESP_OK)
    {
        ESP_LOGW(TAG, "Console logger initialization failed: %s", esp_err_to_name(ret));
    }

    // Disable onboard RGB LED on first boot only
    // If waking from deep sleep, the GPIO hold is already active
    if (esp_sleep_get_wakeup_cause() == ESP_SLEEP_WAKEUP_UNDEFINED)
    {
        power_disable_onboard_led();
    }

    // Restore audio GPIO from RTC mode after deep sleep
    // This must be done before audio_renderer_init() since SD_MODE GPIO
    // is held in RTC mode during deep sleep
    power_restore_audio_gpio();

    // Initialize audio early for all wake paths
    audio_renderer_config_t audio_config = {};
    audio_config.bclk_io_num = I2S_BCLK_GPIO;
    audio_config.ws_io_num = I2S_LRCLK_GPIO;
    audio_config.dout_io_num = I2S_DOUT_GPIO;
    audio_config.sd_mode_io_num = I2S_SD_MODE_GPIO;
    audio_config.sample_rate = I2S_SAMPLE_RATE;
    audio_renderer_init(&audio_config);

    tts_config_t tts_config = {};
    tts_config.volume = 100;
    tts_config.speed = 1.0f;
    tts_config.sample_rate = I2S_SAMPLE_RATE;
    tts_init(&tts_config);

    // Initialize I2C bus for TCS3530 sensor and MAX17048 fuel gauge
    ESP_LOGI(TAG, "Initializing shared I2C bus (TCS3530 + MAX17048)...");
    ret = i2c_bus_manager_init();
    if (ret != ESP_OK)
    {
        ESP_LOGE(TAG, "I2C initialization failed: %s", esp_err_to_name(ret));
        // Continue without I2C - sensor and battery monitoring will be disabled
    }

    // Setup power configuration and initialize power manager
    // This determines wake cause and initializes fuel gauge
    power_config_t power_config = {};
    power_config.wakeup_gpio = BUTTON_GPIO;
    power_config.sleep_timeout_ms = POWER_SAVE_TIMEOUT_S * 1000;
    power_config.i2c_bus = i2c_bus_manager_get();
    power_config.usb_detect_gpio = USB_DETECT_GPIO;
    power_config.max17048_alrt_gpio = MAX17048_ALRT_GPIO;
    
    // Initialize power manager
    power_init(&power_config);
    
    // Check wake cause
    power_wake_cause_t wake_cause = power_get_wake_cause();
    ESP_LOGI(TAG, "Wake cause: %d", wake_cause);

    // Handle spurious wakeup - immediately re-enter deep sleep
    if (wake_cause == POWER_WAKE_SPURIOUS)
    {
        ESP_LOGI(TAG, "Spurious wakeup detected - re-entering deep sleep");
        power_enter_deep_sleep();
        // power_enter_deep_sleep() never returns
    }

    ESP_LOGI(TAG, "Initializing TCS3530 color sensor...");
    TCS3530Config sensor_config = {};
    sensor_config.i2c_bus = i2c_bus_manager_get();

    // Hardware Gain Balancing Configuration:
    // Set initial_gain to X16 (base gain) and disable auto_gain.
    // Per-channel gains will be applied after initialization.
    sensor_config.initial_gain = TCS3530Gain::X16;

    uint16_t integration_time = ALS_INTEGRATION_TIME_MS;
    if (integration_time == 0)
    {
        integration_time = DEFAULT_INTEGRATION_TIME_MS;
    }
    sensor_config.integration_time_ms = integration_time;

    // Disable auto-gain for hardware gain balancing
    sensor_config.auto_gain = false;
    sensor_config.enable_flicker = true;
    sensor_config.secondary_led_gpio = SENSOR_LED2_GPIO;

#if CONFIG_LOG_DEFAULT_LEVEL >= ESP_LOG_DEBUG
    TCS3530::setDebugMode(true);
#endif

    s_sensor = new (std::nothrow) TCS3530();
    if (!s_sensor)
    {
        return;
    }

    ret = s_sensor->init(sensor_config);
    if (ret != ESP_OK)
    {
        ESP_LOGE(TAG, "TCS3530 initialization failed: %s", esp_err_to_name(ret));
        delete s_sensor;
        s_sensor = nullptr;
        return;
    }

    // Use Uniform Gain for stability. The Color Pipeline Matrix will handle the
    // specific channel boosting.
    ret = s_sensor->setGain(TCS3530Gain::X16);
    
    if (ret != ESP_OK)
    {
        ESP_LOGE(TAG, "Failed to set gain: %s", esp_err_to_name(ret));
        return;
    }
    ESP_LOGI(TAG, "Sensor configured for Uniform Gain (X16)");

    // Initialize color processing pipeline
    ESP_LOGI(TAG, "Initializing color processing pipeline...");
    color_pipeline_config_t pipeline_config = {};
    pipeline_config.min_luminance = 5.0f;
    pipeline_config.max_delta_e = 20.0f;
    pipeline_config.use_white_balance = true;
    pipeline_config.num_samples = 3;
    pipeline_config.sample_delay_ms = 50;

    // CRITICAL FIX: Color Tuning Defaults
    pipeline_config.gray_threshold = 2.0f;
    pipeline_config.color_threshold = 60.0f;

    // Default White Reference for Cool White LED
    pipeline_config.white_reference_led = {95.0f, 100.0f, 280.0f};
    pipeline_config.white_reference_ambient = {95.047f, 100.0f, 108.883f};

    ret = color_pipeline_init(&pipeline_config);
    if (ret != ESP_OK)
    {
        ESP_LOGE(TAG, "Color pipeline initialization failed");
        return;
    }

    ESP_LOGI(TAG, "Initializing user interface...");
    ui_config_t ui_config = {};
    ui_config.button_gpio = BUTTON_GPIO;
    ret = ui_init(&ui_config, wake_cause == POWER_WAKE_BUTTON);

    // Set sensor pointer for power manager sleep control
    power_set_sensor(s_sensor);

    // Check battery status and log (for both USB and normal modes)
    int battery_level = power_get_battery_level();
    if (battery_level >= 0)
    {
        int battery_mv = power_get_battery_voltage_mv();
        ESP_LOGI(TAG, "Battery: %d%% (%dmV)", battery_level, battery_mv);
        if (!power_is_usb_connected() && battery_level <= 10)
        {
            tts_speak_async("Battery low");
        }
    }

    // Initial action based on wake cause
    // Ready tone and message
    audio_renderer_tone_ready();
    ESP_LOGI(TAG, "Color detector ready!");
    tts_speak_async("Ready");

    // If woken by button press (not USB), go straight into measurement
    if (wake_cause == POWER_WAKE_BUTTON)
    {
        ESP_LOGI(TAG, "Woke from button press - taking immediate measurement");
        perform_measurement();
    }
    else if (wake_cause == POWER_WAKE_USB)
    {
        // USB mode: announce charging and battery level
        ESP_LOGI(TAG, "USB wake detected - announcing charging status");

        // Turn off the power LED when USB is connected
        power_disable_onboard_led();

        speak_battery_status();
    }
    else
    {
        // Fresh boot on battery - this is like a button wake, take measurement
        // The user pressed the button to turn on the device
        ESP_LOGI(TAG, "Fresh boot on battery - taking initial measurement");
        perform_measurement();
    }

    // Unified main loop: handles button events consistently
    while (1)
    {
        // Small delay after speech completes before entering sleep
        // This ensures audio has fully finished and gives a buffer for the user
        vTaskDelay(pdMS_TO_TICKS(100));

        // Enter light sleep with timer
        // Will return on button press, or enter deep sleep on timer
        ESP_LOGI(TAG, "Entering sleep mode...");
        power_wake_cause_t sleep_wake = power_enter_sleep();

        if (sleep_wake == POWER_WAKE_USB)
        {
            // USB was plugged in during light sleep - announce charging status
            ESP_LOGI(TAG, "USB connected during sleep - announcing charging status");

            // Turn off the power LED when USB is connected
            power_disable_onboard_led();

            audio_renderer_tone_ready();
            speak_battery_status();
        }
        else if (sleep_wake == POWER_WAKE_BUTTON)
        {
            // Wait for button event to determine action type
            ui_event_t event = ui_wait_event(BUTTON_EVENT_TIMEOUT_MS);

            switch (event)
            {
            case UI_EVENT_BUTTON_PRESS:
                if (s_kona_scan_active)
                {
                    ESP_LOGI(TAG, "Button press while Kona scan active - capture swatch");
                    capture_kona_swatch();
                }
                else
                {
                    ESP_LOGI(TAG, "Button pressed - taking measurement");
                    perform_measurement();
                }
                break;

            case UI_EVENT_BUTTON_LONG_PRESS:
                ESP_LOGI(TAG, "Long press - starting automatic calibration");
                perform_auto_calibration();
                break;

            case UI_EVENT_BUTTON_DOUBLE:
                if (s_kona_scan_active)
                {
                    ESP_LOGI(TAG, "Double press - canceling Kona scan");
                    s_kona_scan_active = false;
                    tts_speak("Kona scan canceled");
                }
                else
                {
                    ESP_LOGI(TAG, "Double press - speaking color description");
                    speak_color_description();
                }
                break;

            case UI_EVENT_BUTTON_TRIPLE:
                ESP_LOGI(TAG, "Triple press - announcing battery status");
                speak_battery_status();
                break;

            case UI_EVENT_BUTTON_QUAD:
                if (!power_is_usb_connected())
                {
                    ESP_LOGI(TAG, "Quad press ignored (not on USB power)");
                    tts_speak("Kona scan requires USB power");
                    break;
                }

                if (!s_kona_scan_active)
                {
                    ESP_LOGI(TAG, "Quad press on USB power - start Kona scan session");
                    s_kona_scan_active = true;
                    s_kona_scan_index = 0;
                    tts_speak("Starting Kona swatch scan, every %d", KONA_SCAN_STRIDE);
                    speak_kona_swatch_prompt(s_kona_scan_index);
                }
                else
                {
                    ESP_LOGI(TAG, "Quad press ignored, Kona scan already active");
                    tts_speak("Kona scan already active");
                }
                break;

            case UI_EVENT_NONE:
                // Timeout waiting for button classification
                // Just do a single press action
                ESP_LOGI(TAG, "Button event timeout - taking measurement");
                perform_measurement();
                break;

            default:
                ESP_LOGW(TAG, "Unhandled UI event: %d", event);
                break;
            }
        }
        // If power_enter_sleep returned for any other reason, loop back
    }
}
