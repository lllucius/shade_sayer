/**
 * @file console_logger.cpp
 * @brief Console Log History Manager Implementation
 */

#include "console_logger.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include <cstring>
#include <cstdio>
#include <cstdarg>
#include <cstdlib>

static const char* TAG = "console_logger";

// NVS namespace for log storage
#define NVS_NAMESPACE "console_log"

// Maximum size of a single log session in memory
// Note: NVS partition is 156KB With calibration data also stored,
// we allocate 16KB per log to allow ~5 logs to fit in NVS alongside other data.
// This is a FIFO buffer that wraps around when full, keeping the most recent logs.
#define LOG_BUFFER_SIZE (16 * 1024)

// Minimum space required before wrapping (1 char + null terminator)
#define MIN_WRAP_SPACE 2

// Maximum number of log sessions to keep in NVS
#define MAX_STORED_LOGS 5

// State
static struct
{
    char* buffer;                   // Memory buffer for current session
    size_t buffer_pos;              // Current position in buffer (wraps at LOG_BUFFER_SIZE)
    bool buffer_wrapped;            // Whether buffer has wrapped (FIFO mode)
    bool has_errors;                // Whether any errors occurred this session
    bool initialized;               // Whether logger is initialized
    vprintf_like_t original_vprintf; // Original vprintf function
} s_logger;

/**
 * @brief Custom vprintf function that captures output to memory buffer
 */
static int console_logger_vprintf(const char* fmt, va_list args)
{
    // First, output to original handler (usually UART console)
    int ret = 0;
    if (s_logger.original_vprintf)
    {
        va_list args_copy;
        va_copy(args_copy, args);
        ret = s_logger.original_vprintf(fmt, args_copy);
        va_end(args_copy);
    }

    // Capture to memory buffer if initialized
    if (s_logger.initialized && s_logger.buffer)
    {
        // Calculate available space for vsnprintf
        size_t available = LOG_BUFFER_SIZE - s_logger.buffer_pos;
        
        if (available < MIN_WRAP_SPACE)
        {
            // Not enough space for meaningful data, wrap now
            s_logger.buffer_pos = 0;
            s_logger.buffer_wrapped = true;
            available = LOG_BUFFER_SIZE;
        }
        
        // Remember where we start writing for error detection
        size_t write_start = s_logger.buffer_pos;
        
        // Format directly into buffer at current position
        // Note: va_copy is used because vsnprintf will consume the va_list
        va_list args_copy;
        va_copy(args_copy, args);
        int written = vsnprintf(s_logger.buffer + s_logger.buffer_pos, available, fmt, args_copy);
        va_end(args_copy);
        
        if (written < 0)
        {
            // vsnprintf encoding error - log it
            ESP_LOGE(TAG, "vsnprintf encoding error");
        }
        else if (written > 0)
        {
            // Determine how much was actually written (limited by available space)
            // vsnprintf returns the number of characters that would have been written
            // If written >= available, output was truncated and available-1 chars were written
            size_t actual_written = ((size_t)written < available) ? (size_t)written : (available - 1);
            
            // Update position
            s_logger.buffer_pos += actual_written;
            
            // Null-terminate at current position to make strstr safe
            if (s_logger.buffer_pos < LOG_BUFFER_SIZE)
            {
                s_logger.buffer[s_logger.buffer_pos] = '\0';
            }
            
            // Check for error pattern in the newly written content
            // strstr is safe because we null-terminated at buffer_pos
            // Note: After wrap, write_start is 0 and we're checking the new content
            if (strstr(s_logger.buffer + write_start, "E (") != NULL)
            {
                s_logger.has_errors = true;
            }
        }
    }

    return ret;
}

esp_err_t console_logger_init(void)
{
    if (s_logger.initialized)
    {
        return ESP_OK;
    }

    // Allocate memory buffer
    s_logger.buffer = (char*)malloc(LOG_BUFFER_SIZE);
    if (!s_logger.buffer)
    {
        ESP_LOGE(TAG, "Failed to allocate log buffer");
        return ESP_ERR_NO_MEM;
    }

    s_logger.buffer[0] = '\0';
    s_logger.buffer_pos = 0;
    s_logger.buffer_wrapped = false;
    s_logger.has_errors = false;
    s_logger.initialized = true;

    // Install custom vprintf handler
    s_logger.original_vprintf = esp_log_set_vprintf(console_logger_vprintf);

    ESP_LOGI(TAG, "Console logger initialized (buffer size: %d bytes)", LOG_BUFFER_SIZE);

    return ESP_OK;
}

bool console_logger_has_errors(void)
{
    return s_logger.has_errors;
}

esp_err_t console_logger_save_if_errors(void)
{
    if (!s_logger.initialized)
    {
        return ESP_ERR_INVALID_STATE;
    }

    // Only save if errors occurred
    if (!s_logger.has_errors)
    {
        ESP_LOGI(TAG, "No errors in this session, not saving log");
        return ESP_ERR_INVALID_STATE;
    }

    // Prepare buffer for saving
    // If buffer hasn't wrapped, we can save directly from start to buffer_pos
    // If buffer has wrapped, we need to rearrange to maintain chronological order
    char* save_buffer = s_logger.buffer;
    size_t save_size = s_logger.buffer_pos;
    
    if (s_logger.buffer_wrapped)
    {
        // Buffer has wrapped - rearrange to chronological order
        // After wrapping, oldest data is at buffer_pos (where we'll write next),
        // and newest data is at buffer_pos-1 (what we just wrote)
        save_buffer = (char*)malloc(LOG_BUFFER_SIZE);
        if (!save_buffer)
        {
            ESP_LOGE(TAG, "Failed to allocate rearrange buffer");
            return ESP_ERR_NO_MEM;
        }
        
        // Copy oldest part first (from buffer_pos to end)
        size_t old_part_size = LOG_BUFFER_SIZE - s_logger.buffer_pos;
        if (old_part_size > 0)
        {
            memcpy(save_buffer, s_logger.buffer + s_logger.buffer_pos, old_part_size);
        }
        
        // Copy newer part (from start to buffer_pos)
        if (s_logger.buffer_pos > 0)
        {
            memcpy(save_buffer + old_part_size, s_logger.buffer, s_logger.buffer_pos);
        }
        
        // Use full buffer size
        save_size = LOG_BUFFER_SIZE;
        
        // Ensure null termination at end for C string safety
        // Note: If the last byte is not already null, this may overwrite
        // the last character of log data, which is acceptable for string safety
        if (save_buffer[LOG_BUFFER_SIZE - 1] != '\0')
        {
            save_buffer[LOG_BUFFER_SIZE - 1] = '\0';
        }
    }
    else
    {
        // Ensure null termination
        if (s_logger.buffer_pos > 0 && s_logger.buffer_pos < LOG_BUFFER_SIZE)
        {
            s_logger.buffer[s_logger.buffer_pos] = '\0';
        }
        // Size includes the content up to buffer_pos
        save_size = s_logger.buffer_pos;
    }

    ESP_LOGI(TAG, "Saving log to NVS (size: %zu bytes, wrapped: %s)", 
             save_size, s_logger.buffer_wrapped ? "yes" : "no");

    // Open NVS
    nvs_handle_t nvs_handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs_handle);
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "Failed to open NVS: %s", esp_err_to_name(err));
        if (s_logger.buffer_wrapped && save_buffer != s_logger.buffer)
        {
            free(save_buffer);
        }
        return err;
    }

    // Read current count
    int32_t count = 0;
    nvs_get_i32(nvs_handle, "count", &count);
    
    // Circular buffer: if count >= MAX_STORED_LOGS, wrap to 0
    if (count >= MAX_STORED_LOGS)
    {
        count = 0;
    }

    // Store log with index-based key
    char key[16];
    snprintf(key, sizeof(key), "log_%d", (int)count);
    
    // Calculate bytes to save:
    // - When wrapped: save_size is LOG_BUFFER_SIZE (full buffer)
    // - When not wrapped: save_size is buffer_pos, add 1 for null terminator at buffer_pos
    size_t bytes_to_save = s_logger.buffer_wrapped ? save_size : (save_size + 1);
    err = nvs_set_blob(nvs_handle, key, save_buffer, bytes_to_save);
    if (err != ESP_OK)
    {
        if (err == ESP_ERR_NVS_NOT_ENOUGH_SPACE)
        {
            ESP_LOGW(TAG, "NVS partition full - cannot save log (consider increasing partition size)");
        }
        else
        {
            ESP_LOGE(TAG, "Failed to write log to NVS: %s", esp_err_to_name(err));
        }
        nvs_close(nvs_handle);
        if (s_logger.buffer_wrapped && save_buffer != s_logger.buffer)
        {
            free(save_buffer);
        }
        return err;
    }

    // Update count
    count++;
    if (count > MAX_STORED_LOGS)
    {
        count = MAX_STORED_LOGS;
    }
    
    err = nvs_set_i32(nvs_handle, "count", count);
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "Failed to update count: %s", esp_err_to_name(err));
    }

    // Commit immediately (no shutdown process before deep sleep)
    err = nvs_commit(nvs_handle);
    nvs_close(nvs_handle);

    // Free temporary buffer if allocated
    if (s_logger.buffer_wrapped && save_buffer != s_logger.buffer)
    {
        free(save_buffer);
    }

    if (err == ESP_OK)
    {
        ESP_LOGI(TAG, "Log saved successfully as log_%d (total logs: %d)", (int)(count - 1), (int)count);
    }

    return err;
}

int console_logger_get_stored_count(void)
{
    nvs_handle_t nvs_handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READONLY, &nvs_handle);
    if (err != ESP_OK)
    {
        return 0;
    }

    int32_t count = 0;
    nvs_get_i32(nvs_handle, "count", &count);
    nvs_close(nvs_handle);

    return (int)count;
}

esp_err_t console_logger_get_stored_log(int index, char* buffer, size_t buffer_size, size_t* out_len)
{
    if (!buffer || buffer_size == 0)
    {
        return ESP_ERR_INVALID_ARG;
    }

    if (index < 0 || index >= MAX_STORED_LOGS)
    {
        return ESP_ERR_INVALID_ARG;
    }

    nvs_handle_t nvs_handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READONLY, &nvs_handle);
    if (err != ESP_OK)
    {
        return err;
    }

    // Get total count to determine actual index mapping
    int32_t count = 0;
    nvs_get_i32(nvs_handle, "count", &count);

    if (index >= count)
    {
        nvs_close(nvs_handle);
        return ESP_ERR_NOT_FOUND;
    }

    // Map logical index to physical NVS key index:
    // Logical index: 0 = most recent log, 4 = oldest log
    // Physical index: NVS key "log_N" where N is 0-4
    //
    // When count < MAX_STORED_LOGS (buffer not full yet):
    //   - Logs are stored sequentially: log_0, log_1, log_2, ...
    //   - Most recent is at (count-1), oldest at 0
    //   - Physical index = count - 1 - logical_index
    //
    // When count >= MAX_STORED_LOGS (buffer has wrapped):
    //   - Logs wrap around: newest overwrites oldest
    //   - Most recent is at (count-1) % MAX_STORED_LOGS
    //   - Physical index = (newest_idx - logical_index + MAX_STORED_LOGS) % MAX_STORED_LOGS
    int physical_index;
    if (count < MAX_STORED_LOGS)
    {
        physical_index = (int)count - 1 - index;
    }
    else
    {
        // Circular buffer: most recent is at (count - 1) % MAX_STORED_LOGS
        int newest_idx = ((int)count - 1) % MAX_STORED_LOGS;
        physical_index = (newest_idx - index + MAX_STORED_LOGS) % MAX_STORED_LOGS;
    }

    char key[16];
    snprintf(key, sizeof(key), "log_%d", physical_index);

    // Read log size first
    size_t required_size = 0;
    err = nvs_get_blob(nvs_handle, key, NULL, &required_size);
    if (err != ESP_OK)
    {
        nvs_close(nvs_handle);
        return err;
    }

    // Read log data
    size_t read_size = (required_size < buffer_size) ? required_size : buffer_size;
    err = nvs_get_blob(nvs_handle, key, buffer, &read_size);
    nvs_close(nvs_handle);

    if (err == ESP_OK && out_len)
    {
        *out_len = read_size;
        // Ensure null termination
        if (read_size > 0 && read_size <= buffer_size)
        {
            buffer[read_size - 1] = '\0';
        }
    }

    return err;
}

esp_err_t console_logger_clear_all(void)
{
    nvs_handle_t nvs_handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs_handle);
    if (err != ESP_OK)
    {
        return err;
    }

    // Erase all keys in namespace
    err = nvs_erase_all(nvs_handle);
    if (err == ESP_OK)
    {
        err = nvs_commit(nvs_handle);
    }

    nvs_close(nvs_handle);
    
    ESP_LOGI(TAG, "All stored logs cleared");
    
    return err;
}
