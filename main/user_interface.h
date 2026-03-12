/**
 * @file user_interface.h
 * @brief User Interface for Color Detector
 *
 * Handles button input, LED feedback, and audio output coordination.
 * Designed for visually impaired users with emphasis on non-visual feedback.
 */

#ifndef USER_INTERFACE_H
#define USER_INTERFACE_H

#include "esp_err.h"
#include "driver/gpio.h"
#include "color_pipeline.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief UI event types
 */
typedef enum
{
    UI_EVENT_NONE = 0,
    UI_EVENT_BUTTON_PRESS,      /**< Single button press */
    UI_EVENT_BUTTON_LONG_PRESS, /**< Button held for > 2 seconds */
    UI_EVENT_BUTTON_DOUBLE,     /**< Double click */
    UI_EVENT_BUTTON_TRIPLE,     /**< Triple click */
    UI_EVENT_BUTTON_QUAD,       /**< Quadruple click */
    UI_EVENT_BUTTON_QUINT,      /**< Quintuple click (5 presses) */
    UI_EVENT_MEASUREMENT_START, /**< Measurement starting */
    UI_EVENT_MEASUREMENT_DONE,  /**< Measurement complete */
    UI_EVENT_CALIBRATION,       /**< Calibration requested */
    UI_EVENT_ERROR,             /**< Error occurred */
    UI_EVENT_LOW_BATTERY,       /**< Battery low warning */
} ui_event_t;

/**
 * @brief UI configuration
 */
typedef struct
{
    gpio_num_t button_gpio;         /**< Measurement button GPIO */
} ui_config_t;

/**
 * @brief Initialize user interface
 *
 * @param config UI configuration
 * @param from_button_wake True if woken from deep sleep via button press
 * @return ESP_OK on success
 */
esp_err_t ui_init(const ui_config_t* config, bool from_button_wake);

/**
 * @brief Wait for UI event with timeout (blocking)
 *
 * Blocks the calling task until a UI event occurs or the timeout expires.
 * This is more power-efficient than polling ui_process() in a loop.
 *
 * @param timeout_ms Timeout in milliseconds (0 = wait forever)
 * @return UI event that occurred, or UI_EVENT_NONE if timeout
 */
ui_event_t ui_wait_event(uint32_t timeout_ms);

#ifdef __cplusplus
}
#endif

#endif /* USER_INTERFACE_H */
