/**
 * @file user_interface.cpp
 * @brief User Interface Implementation
 *
 * Custom synchronous button handler with polling-based detection.
 * Supports single click, double click, triple click, and long press.
 * Handles wake-from-sleep cleanly without background state machines.
 * 
 * Note: Hardware debounce is provided by a 100nF capacitor on the button GPIO.
 * No software debounce sampling is required.
 */

#include "user_interface.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <cstring>

static const char* TAG = "ui";

// Button timing configuration - get from Kconfig
#define LONG_PRESS_MS               CONFIG_BUTTON_LONG_PRESS_MS
#define MULTI_CLICK_WINDOW_MS       400   // Window for detecting multiple clicks
#define POLL_INTERVAL_MS            10    // Polling interval for button state checks
#define BUTTON_RELEASE_TIMEOUT_MS   5000  // Timeout for waiting for button release on wake

// UI state
static struct
{
    ui_config_t config;
    bool initialized;
    gpio_num_t button_gpio;
} s_ui;

/**
 * @brief Read button state (hardware debounce via capacitor)
 * 
 * Returns immediate button state. Hardware debounce is provided by
 * a 100nF capacitor on the button GPIO, eliminating the need for
 * software debounce sampling.
 * 
 * @return true if button is pressed (active high = 1), false otherwise
 */
static inline bool read_button_state(void)
{
    return gpio_get_level(s_ui.button_gpio) == 1;  // Active high (external pulldown)
}

/**
 * @brief Wait for button to be released
 * 
 * @param timeout_ms Maximum time to wait in milliseconds
 * @return true if released, false if timeout
 */
static bool wait_for_release(uint32_t timeout_ms)
{
    TickType_t start_tick = xTaskGetTickCount();
    TickType_t timeout_ticks = pdMS_TO_TICKS(timeout_ms);
    
    while (gpio_get_level(s_ui.button_gpio) == 1)  // Wait while pressed (active high)
    {
        TickType_t elapsed_ticks = xTaskGetTickCount() - start_tick;
        if (elapsed_ticks >= timeout_ticks)
        {
            return false;  // Timeout
        }
        vTaskDelay(pdMS_TO_TICKS(POLL_INTERVAL_MS));
    }
    
    return true;  // Released
}

/**
 * @brief Wait for button to be pressed
 * 
 * @param timeout_ms Maximum time to wait in milliseconds (0 = wait forever)
 * @return true if pressed, false if timeout
 */
static bool wait_for_press(uint32_t timeout_ms)
{
    TickType_t start_tick = xTaskGetTickCount();
    TickType_t timeout_ticks = (timeout_ms == 0) ? portMAX_DELAY : pdMS_TO_TICKS(timeout_ms);
    
    while (gpio_get_level(s_ui.button_gpio) != 1)  // Wait while not pressed (active high)
    {
        if (timeout_ms != 0)
        {
            TickType_t elapsed_ticks = xTaskGetTickCount() - start_tick;
            if (elapsed_ticks >= timeout_ticks)
            {
                return false;  // Timeout
            }
        }
        vTaskDelay(pdMS_TO_TICKS(POLL_INTERVAL_MS));
    }
    
    return true;  // Pressed
}

/**
 * @brief Detect button event synchronously
 * 
 * Blocks until a button event is detected or timeout occurs.
 * Detects: single click, double click, triple click, long press.
 * 
 * Note: Hardware debounce is provided by a 100nF capacitor on the button GPIO,
 * so software debounce sampling is not required.
 * 
 * @param timeout_ms Maximum time to wait for initial button press (0 = wait forever)
 * @return Detected UI event or UI_EVENT_NONE on timeout
 */
static ui_event_t detect_button_event(uint32_t timeout_ms)
{
    // Wait for initial button press
    if (!wait_for_press(timeout_ms))
    {
        return UI_EVENT_NONE;  // Timeout waiting for press
    }
    
    // Track press start time for long press detection
    TickType_t press_start = xTaskGetTickCount();
    
    // Wait for release or long press timeout
    while (read_button_state())  // While pressed
    {
        TickType_t press_duration = xTaskGetTickCount() - press_start;
        if (press_duration >= pdMS_TO_TICKS(LONG_PRESS_MS))
        {
            // Long press detected - wait for release before returning
            ESP_LOGI(TAG, "Long press detected");
            wait_for_release(BUTTON_RELEASE_TIMEOUT_MS);
            return UI_EVENT_BUTTON_LONG_PRESS;
        }
        vTaskDelay(pdMS_TO_TICKS(POLL_INTERVAL_MS));
    }
    
    // Button was released - count clicks
    int click_count = 1;
    TickType_t last_release = xTaskGetTickCount();
    
    // Look for additional clicks within the multi-click window
    while (click_count < 5)  // Support up to quintuple click
    {
        TickType_t elapsed = xTaskGetTickCount() - last_release;
        TickType_t remaining = pdMS_TO_TICKS(MULTI_CLICK_WINDOW_MS) - elapsed;
        
        if (elapsed >= pdMS_TO_TICKS(MULTI_CLICK_WINDOW_MS))
        {
            break;  // Multi-click window expired
        }
        
        // Wait for next press within remaining window
        if (wait_for_press(pdTICKS_TO_MS(remaining)))
        {
            click_count++;
            
            // Wait for release
            if (!wait_for_release(BUTTON_RELEASE_TIMEOUT_MS))
            {
                ESP_LOGW(TAG, "Button release timeout during multi-click");
                break;
            }
            
            last_release = xTaskGetTickCount();
        }
        else
        {
            break;  // No additional press within window
        }
    }
    
    // Return appropriate event based on click count
    switch (click_count)
    {
        case 1:
            ESP_LOGI(TAG, "Single click detected");
            return UI_EVENT_BUTTON_PRESS;
        case 2:
            ESP_LOGI(TAG, "Double click detected");
            return UI_EVENT_BUTTON_DOUBLE;
        case 3:
            ESP_LOGI(TAG, "Triple click detected");
            return UI_EVENT_BUTTON_TRIPLE;
        case 4:
            ESP_LOGI(TAG, "Quadruple click detected");
            return UI_EVENT_BUTTON_QUAD;
        case 5:
            ESP_LOGI(TAG, "Quintuple click detected");
            return UI_EVENT_BUTTON_QUINT;
        default:
            return UI_EVENT_NONE;
    }
}

esp_err_t ui_init(const ui_config_t* config, bool from_button_wake)
{
    if (!config)
    {
        return ESP_ERR_INVALID_ARG;
    }

    memset(&s_ui, 0, sizeof(s_ui));
    s_ui.config = *config;

    // Store button GPIO (already configured by power_manager during power_init)
    if (config->button_gpio >= 0)
    {
        s_ui.button_gpio = config->button_gpio;

        // If we woke from button press, wait for release before accepting new input.
        // This prevents the wake button press from being counted toward a new event.
        if (from_button_wake)
        {
            ESP_LOGI(TAG, "Waiting for wake button release...");
            
            if (!wait_for_release(BUTTON_RELEASE_TIMEOUT_MS))
            {
                ESP_LOGW(TAG, "Button release timeout - proceeding anyway");
            }
            else
            {
                ESP_LOGI(TAG, "Wake button released");
            }
        }

        ESP_LOGI(TAG, "Button initialized on GPIO%d (custom synchronous handler)",
                 config->button_gpio);
    }

    s_ui.initialized = true;
    ESP_LOGI(TAG, "UI initialized (button only - audio via I2S TTS)");

    return ESP_OK;
}

ui_event_t ui_wait_event(uint32_t timeout_ms)
{
    if (!s_ui.initialized)
    {
        return UI_EVENT_NONE;
    }

    // Call synchronous event detection
    return detect_button_event(timeout_ms);
}

