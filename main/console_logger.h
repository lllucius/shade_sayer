/**
 * @file console_logger.h
 * @brief Console Log History Manager for Battery Diagnostics
 *
 * Captures all console output in memory from power-up to deep sleep.
 * Stores last 5 log sessions in NVS only when errors occur during battery operation.
 * Allows retrieval and playback of stored logs for debugging.
 */

#ifndef CONSOLE_LOGGER_H
#define CONSOLE_LOGGER_H

#include "esp_err.h"
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Initialize console logger
 * 
 * Sets up memory buffer for capturing console output and registers
 * custom log output handler with ESP-IDF logging system.
 * 
 * @return ESP_OK on success
 */
esp_err_t console_logger_init(void);

/**
 * @brief Check if any errors occurred during this session
 * 
 * @return true if ESP_LOGE was called at least once
 */
bool console_logger_has_errors(void);

/**
 * @brief Save current log buffer to NVS (only if errors occurred)
 * 
 * Should be called before entering deep sleep. Will only write to NVS
 * if console_logger_has_errors() returns true.
 * Maintains circular buffer of last 5 log sessions.
 * 
 * @return ESP_OK on success, ESP_ERR_INVALID_STATE if no errors to save
 */
esp_err_t console_logger_save_if_errors(void);

/**
 * @brief Get count of stored log sessions in NVS
 * 
 * @return Number of stored log sessions (0-5)
 */
int console_logger_get_stored_count(void);

/**
 * @brief Retrieve a stored log session
 * 
 * @param index Log index (0 = most recent, 4 = oldest)
 * @param buffer Buffer to receive log text
 * @param buffer_size Size of buffer
 * @param out_len Pointer to receive actual bytes read (optional, can be NULL)
 * @return ESP_OK on success
 * @return ESP_ERR_INVALID_ARG if buffer is NULL, buffer_size is 0, or index is out of range
 * @return ESP_ERR_NOT_FOUND if the requested log index doesn't exist
 */
esp_err_t console_logger_get_stored_log(int index, char* buffer, size_t buffer_size, size_t* out_len);

/**
 * @brief Clear all stored logs from NVS
 * 
 * @return ESP_OK on success
 */
esp_err_t console_logger_clear_all(void);

#ifdef __cplusplus
}
#endif

#endif /* CONSOLE_LOGGER_H */
