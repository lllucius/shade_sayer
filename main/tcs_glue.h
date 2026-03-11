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

const char* esp_err_to_name(esp_err_t err);

void tcs_glue_log(const char* level, const char* tag, const char* fmt, ...);
#define ESP_LOGI(tag, fmt, ...) tcs_glue_log("I", tag, fmt, ##__VA_ARGS__)
#define ESP_LOGW(tag, fmt, ...) tcs_glue_log("W", tag, fmt, ##__VA_ARGS__)
#define ESP_LOGE(tag, fmt, ...) tcs_glue_log("E", tag, fmt, ##__VA_ARGS__)
#endif

#define TCS_LOGI ESP_LOGI
#define TCS_LOGW ESP_LOGW
#define TCS_LOGE ESP_LOGE
#define TCS_LOGD ESP_LOGI

uint64_t tcs_time_us(void);
void tcs_delay_ms(uint32_t delay_ms);

esp_err_t tcs_storage_load_blob(const char* name_space,
                                const char* key,
                                void* out_blob,
                                size_t* inout_size);
esp_err_t tcs_storage_save_blob(const char* name_space,
                                const char* key,
                                const void* blob,
                                size_t blob_size);

#endif
