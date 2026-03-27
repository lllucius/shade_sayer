/**
 * @file tcs_glue.h
 * @brief Platform abstraction layer for ESP-IDF / host portability
 *
 * Provides a thin shim so that colour-processing code compiled on the
 * host (for unit tests, replay tools, etc.) can use the same logging,
 * timing, and NVS storage APIs as the firmware running on the ESP32.
 *
 * On ESP-IDF the macros simply forward to the native SDK functions.
 * On the host they are implemented in tcs_glue.cpp using the C++ standard
 * library and the local filesystem.
 */

#ifndef TCS_GLUE_H
#define TCS_GLUE_H

#include <stddef.h>
#include <stdint.h>

#ifdef ESP_PLATFORM
#include "esp_err.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#else
#include <stdio.h>
#include <stdarg.h>

typedef int esp_err_t;
#define ESP_OK 0
#define ESP_FAIL -1
#define ESP_ERR_INVALID_ARG 0x102
#define ESP_ERR_INVALID_STATE 0x103
#define ESP_ERR_NOT_FOUND 0x105
#define ESP_ERR_NO_MEM 0x101
#define ESP_ERR_INVALID_SIZE 0x106

/** @brief Convert an esp_err_t code to a human-readable string (host shim). */
const char* esp_err_to_name(esp_err_t err);

/** @brief Log a message to stderr in the format "LEVEL (TAG): message" (host shim). */
void tcs_glue_log(const char* level, const char* tag, const char* fmt, ...);
#define ESP_LOGI(tag, fmt, ...) tcs_glue_log("I", tag, fmt, ##__VA_ARGS__)
#define ESP_LOGW(tag, fmt, ...) tcs_glue_log("W", tag, fmt, ##__VA_ARGS__)
#define ESP_LOGE(tag, fmt, ...) tcs_glue_log("E", tag, fmt, ##__VA_ARGS__)
#endif

/** @name Portable logging aliases used by driver code */
///@{
#define TCS_LOGI ESP_LOGI
#define TCS_LOGW ESP_LOGW
#define TCS_LOGE ESP_LOGE
#define TCS_LOGD ESP_LOGI
///@}

/**
 * @brief Return the current time in microseconds
 *
 * Uses esp_timer_get_time() on ESP-IDF and std::chrono on the host.
 */
uint64_t tcs_time_us(void);

/**
 * @brief Busy-wait / sleep for @p delay_ms milliseconds
 *
 * Uses vTaskDelay() on ESP-IDF and std::this_thread::sleep_for() on the host.
 */
void tcs_delay_ms(uint32_t delay_ms);

/**
 * @brief Load a binary blob from persistent storage
 *
 * On ESP-IDF this wraps NVS; on the host it reads from a local file under
 * the directory pointed to by TCS_HOST_STORAGE_DIR (default: "host/").
 *
 * @param name_space  NVS namespace (or filename prefix on host)
 * @param key         NVS key (or filename suffix on host)
 * @param[out] out_blob  Destination buffer
 * @param[in,out] inout_size  On entry the buffer size; on exit the bytes read
 * @return ESP_OK on success, or an error code
 */
esp_err_t tcs_storage_load_blob(const char* name_space,
                                const char* key,
                                void* out_blob,
                                size_t* inout_size);

/**
 * @brief Save a binary blob to persistent storage
 *
 * On ESP-IDF this wraps NVS (with immediate commit).  On the host it
 * writes a file under TCS_HOST_STORAGE_DIR.
 *
 * @param name_space  NVS namespace (or filename prefix on host)
 * @param key         NVS key (or filename suffix on host)
 * @param blob        Source data
 * @param blob_size   Number of bytes to write
 * @return ESP_OK on success, or an error code
 */
esp_err_t tcs_storage_save_blob(const char* name_space,
                                const char* key,
                                const void* blob,
                                size_t blob_size);

#endif
