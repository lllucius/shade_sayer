#include "tcs_glue.h"

#ifdef ESP_PLATFORM
#include "nvs.h"
#include "esp_timer.h"
#else
#include <chrono>
#include <thread>
#include <string>
#include <fstream>
#include <cstdlib>

const char* esp_err_to_name(esp_err_t err)
{
    switch (err)
    {
        case ESP_OK: return "ESP_OK";
        case ESP_FAIL: return "ESP_FAIL";
        case ESP_ERR_INVALID_ARG: return "ESP_ERR_INVALID_ARG";
        case ESP_ERR_INVALID_STATE: return "ESP_ERR_INVALID_STATE";
        case ESP_ERR_NOT_FOUND: return "ESP_ERR_NOT_FOUND";
        case ESP_ERR_NO_MEM: return "ESP_ERR_NO_MEM";
        default: return "ESP_ERR_UNKNOWN";
    }
}

void tcs_glue_log(const char* level, const char* tag, const char* fmt, ...)
{
    fprintf(stderr, "%s (%s): ", level, tag);
    va_list args;
    va_start(args, fmt);
    vfprintf(stderr, fmt, args);
    va_end(args);
    fprintf(stderr, "\n");
}

static std::string storage_path(const char* name_space, const char* key)
{
    const char* base = getenv("TCS_HOST_STORAGE_DIR");
    std::string root = base ? base : "host";
    return root + "/" + name_space + "_" + key + ".bin";
}
#endif

uint64_t tcs_time_us(void)
{
#ifdef ESP_PLATFORM
    return esp_timer_get_time();
#else
    auto now = std::chrono::steady_clock::now().time_since_epoch();
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(now).count());
#endif
}

void tcs_delay_ms(uint32_t delay_ms)
{
#ifdef ESP_PLATFORM
    vTaskDelay(pdMS_TO_TICKS(delay_ms));
#else
    std::this_thread::sleep_for(std::chrono::milliseconds(delay_ms));
#endif
}

esp_err_t tcs_storage_load_blob(const char* name_space,
                                const char* key,
                                void* out_blob,
                                size_t* inout_size)
{
#ifdef ESP_PLATFORM
    nvs_handle_t nvs_handle;
    esp_err_t err = nvs_open(name_space, NVS_READONLY, &nvs_handle);
    if (err != ESP_OK)
    {
        return err;
    }
    err = nvs_get_blob(nvs_handle, key, out_blob, inout_size);
    nvs_close(nvs_handle);
    return err;
#else
    if (!inout_size || !out_blob)
    {
        return ESP_ERR_INVALID_ARG;
    }
    std::ifstream in(storage_path(name_space, key), std::ios::binary);
    if (!in.good())
    {
        return ESP_ERR_NOT_FOUND;
    }
    in.seekg(0, std::ios::end);
    const size_t sz = static_cast<size_t>(in.tellg());
    in.seekg(0, std::ios::beg);
    if (*inout_size < sz)
    {
        *inout_size = sz;
        return ESP_ERR_INVALID_SIZE;
    }
    in.read(static_cast<char*>(out_blob), sz);
    *inout_size = sz;
    return in.good() ? ESP_OK : ESP_FAIL;
#endif
}

esp_err_t tcs_storage_save_blob(const char* name_space,
                                const char* key,
                                const void* blob,
                                size_t blob_size)
{
#ifdef ESP_PLATFORM
    nvs_handle_t nvs_handle;
    esp_err_t err = nvs_open(name_space, NVS_READWRITE, &nvs_handle);
    if (err != ESP_OK)
    {
        return err;
    }
    err = nvs_set_blob(nvs_handle, key, blob, blob_size);
    if (err == ESP_OK)
    {
        err = nvs_commit(nvs_handle);
    }
    nvs_close(nvs_handle);
    return err;
#else
    std::ofstream out(storage_path(name_space, key), std::ios::binary | std::ios::trunc);
    if (!out.good())
    {
        return ESP_FAIL;
    }
    out.write(static_cast<const char*>(blob), static_cast<std::streamsize>(blob_size));
    return out.good() ? ESP_OK : ESP_FAIL;
#endif
}
