/**
 * @file audio_renderer.cpp
 * @brief I2S Audio Output Implementation for MAX98357A
 *
 * Implements audio output via I2S to MAX98357A digital amplifier.
 * Uses ESP-IDF v5.x I2S standard mode API.
 *
 * Hardware: Adafruit I2S Amplifier BFF with MAX98357A
 */

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "driver/i2s_std.h"
#include "esp_log.h"
#include <cstring>
#include <cmath>
#include <array>

#include "audio_renderer.h"

// Fallback definition for M_PI if not provided by cmath
#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

// Common audio buffer configuration - get from Kconfig
#define AUDIO_CHUNK_SIZE        CONFIG_AUDIO_CHUNK_SIZE

// Attention tone configuration
// Note: Per requirements, tones must produce 16-bit mono at 16000Hz
#define TONE_SAMPLE_RATE        CONFIG_TONE_SAMPLE_RATE
#define TONE_AMPLITUDE          8000    // Normalized volume for all tones

// Attention tone frequencies (musical notes)
#define FREQ_A4                 440     // A4 - Error tone
#define FREQ_C5                 523     // C5 - Base note for ready/power off
#define FREQ_E5                 659     // E5 - Second note for ready/power off
#define FREQ_G5                 784     // G5 - Measurement done

// Tone durations in seconds - get from Kconfig (convert ms to seconds)
#define TONE_NOTE_DURATION      (CONFIG_TONE_NOTE_DURATION_MS / 1000.0f)
#define TONE_GAP_DURATION       0.04f   // Gap between notes
#define TONE_FADE_SAMPLES       50      // Fade in/out samples

static const char* TAG = "audio_renderer";

// SD_MODE GPIO for amplifier enable/disable control
static gpio_num_t s_sd_mode_gpio = GPIO_NUM_NC;
static gpio_num_t s_bclk_gpio = GPIO_NUM_NC;
static gpio_num_t s_ws_gpio = GPIO_NUM_NC;
static gpio_num_t s_dout_gpio = GPIO_NUM_NC;

// Track I2S channel enabled state to prevent double-enable
static bool s_i2s_enabled = false;

static i2s_chan_handle_t tx_handle;

esp_err_t audio_renderer_init(const audio_renderer_config_t* config)
{
    if (!config)
    {
        return ESP_ERR_INVALID_ARG;
    }

    // Store audio GPIOs for later deep sleep preparation
    s_sd_mode_gpio = config->sd_mode_io_num;
    s_bclk_gpio = config->bclk_io_num;
    s_ws_gpio = config->ws_io_num;
    s_dout_gpio = config->dout_io_num;
    if (s_sd_mode_gpio != GPIO_NUM_NC)
    {
        gpio_config_t io_conf = {};
        io_conf.pin_bit_mask = (1ULL << s_sd_mode_gpio);
        io_conf.mode = GPIO_MODE_OUTPUT;
        io_conf.pull_up_en = GPIO_PULLUP_ENABLE;
        io_conf.pull_down_en = GPIO_PULLDOWN_DISABLE;
        io_conf.intr_type = GPIO_INTR_DISABLE;
        esp_err_t gpio_ret = gpio_config(&io_conf);
        if (gpio_ret != ESP_OK)
        {
            ESP_LOGE(TAG, "Failed to configure SD_MODE GPIO: %s", esp_err_to_name(gpio_ret));
            return gpio_ret;
        }
        // Set LOW to keep amplifier in shutdown during I2S initialization
        gpio_set_level(s_sd_mode_gpio, 0);
    }

    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_AUTO, I2S_ROLE_MASTER);
    chan_cfg.auto_clear = true;

    esp_err_t ret = i2s_new_channel(&chan_cfg, &tx_handle, NULL);
    if (ret != ESP_OK)
    {
        ESP_LOGE(TAG, "Failed to create I2S channel: %s", esp_err_to_name(ret));
        return ret;
    }

    i2s_std_config_t std_cfg =
    {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(16000),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_MONO),
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED,
            .bclk = config->bclk_io_num,
            .ws = config->ws_io_num,
            .dout = config->dout_io_num,
            .din = I2S_GPIO_UNUSED,
            .invert_flags =
            {
                .mclk_inv = false,
                .bclk_inv = false,
                .ws_inv = false
            },
        },
    };

    ret = i2s_channel_init_std_mode(tx_handle, &std_cfg);
    if (ret != ESP_OK)
    {
        i2s_del_channel(tx_handle);
        return ret;
    }

    ESP_LOGI(TAG, "Audio renderer initialized");
    return ESP_OK;
}

esp_err_t audio_renderer_write(const int16_t *samples, size_t count)
{
    if (!tx_handle || !samples || count == 0)
    {
        return ESP_ERR_INVALID_STATE;
    }

    size_t written = 0;
    esp_err_t ret = i2s_channel_write(tx_handle,
                                      samples,
                                      count * sizeof(int16_t),
                                      &written,
                                      portMAX_DELAY);
    if (ret != ESP_OK)
    {
        return ret;
    }

    return ESP_OK;
}

void audio_renderer_set_enable(bool enable)
{
    if (tx_handle)
    {
        if (enable)
        {
            // Check if already enabled to prevent double-enable error
            if (s_i2s_enabled)
            {
                ESP_LOGD(TAG, "I2S already enabled, skipping enable");
                return;
            }

            // 1. Enable I2S channel first to start clocks
            esp_err_t ret = i2s_channel_enable(tx_handle);
            if (ret != ESP_OK)
            {
                ESP_LOGE(TAG, "Failed to enable I2S channel: %s", esp_err_to_name(ret));
                return;
            }
            s_i2s_enabled = true;

            // 2. Write silence to prime DMA and stabilize clocks before enabling amp.
            //    At 16kHz sample rate, 256 samples = 16ms of silence.
            int16_t silence[AUDIO_CHUNK_SIZE] = {0};
            ret = audio_renderer_write(silence, AUDIO_CHUNK_SIZE);
            if (ret != ESP_OK)
            {
                ESP_LOGW(TAG, "Failed to write silence buffer: %s", esp_err_to_name(ret));
            }

            // 3. Unmute amplifier by setting SD_MODE HIGH
            if (s_sd_mode_gpio != GPIO_NUM_NC)
            {
                gpio_set_level(s_sd_mode_gpio, 1);
            }
        }
        else
        {
            // Check if already disabled
            if (!s_i2s_enabled)
            {
                ESP_LOGD(TAG, "I2S already disabled, skipping disable");
                return;
            }

            // 1. Mute amplifier by setting SD_MODE LOW
            if (s_sd_mode_gpio != GPIO_NUM_NC)
            {
                gpio_set_level(s_sd_mode_gpio, 0);
            }

            // 2. Wait for amplifier to fully shut down
            vTaskDelay(pdMS_TO_TICKS(10));

            // 3. Disable I2S channel
            esp_err_t ret = i2s_channel_disable(tx_handle);
            if (ret != ESP_OK)
            {
                ESP_LOGE(TAG, "Failed to disable I2S channel: %s", esp_err_to_name(ret));
                return;
            }
            s_i2s_enabled = false;
        }
    }
}

void audio_renderer_prepare_for_sleep(void)
{
    audio_renderer_set_enable(false);

    if (tx_handle)
    {
        esp_err_t ret = ESP_OK;
        if (s_i2s_enabled)
        {
            ret = i2s_channel_disable(tx_handle);
            if (ret != ESP_OK)
            {
                ESP_LOGW(TAG, "Failed to disable I2S channel: %s", esp_err_to_name(ret));
            }
            s_i2s_enabled = false;
        }

        ret = i2s_del_channel(tx_handle);
        if (ret != ESP_OK)
        {
            ESP_LOGW(TAG, "Failed to delete I2S channel: %s", esp_err_to_name(ret));
        }
        tx_handle = nullptr;
    }

    const std::array audio_gpios{ s_bclk_gpio, s_ws_gpio, s_dout_gpio };
    for (gpio_num_t gpio : audio_gpios)
    {
        if (gpio == GPIO_NUM_NC)
        {
            continue;
        }

        gpio_reset_pin(gpio);
        gpio_set_direction(gpio, GPIO_MODE_DISABLE);
    }

    ESP_LOGI(TAG, "Audio I2S GPIOs disabled for deep sleep");
}

/**
 * @brief Generate a single tone at the specified frequency
 *
 * @param freq_hz Frequency in Hz
 * @param duration_s Duration in seconds
 */
static void generate_tone(int freq_hz, float duration_s)
{
    int16_t mono_chunk[AUDIO_CHUNK_SIZE];
    int num_samples = (int)(TONE_SAMPLE_RATE * duration_s);

    for (int i = 0; i < num_samples; i += AUDIO_CHUNK_SIZE)
    {
        int batch = (num_samples - i > AUDIO_CHUNK_SIZE) ? AUDIO_CHUNK_SIZE : (num_samples - i);
        for (int j = 0; j < batch; ++j)
        {
            int current = i + j;
            float t = (float)current / TONE_SAMPLE_RATE;
            float fade = 1.0f;

            // Apply fade in/out for smooth transitions
            if (current < TONE_FADE_SAMPLES)
            {
                fade = (float)current / TONE_FADE_SAMPLES;
            }
            else if (current > num_samples - TONE_FADE_SAMPLES)
            {
                fade = (float)(num_samples - current) / TONE_FADE_SAMPLES;
            }

            float amp = TONE_AMPLITUDE * fade;
            mono_chunk[j] = (int16_t)(amp * sinf(2.0f * M_PI * freq_hz * t));
        }
        if (audio_renderer_write(mono_chunk, batch) != ESP_OK)
        {
            break;
        }
    }
}

/**
 * @brief Generate silence (gap between notes)
 *
 * @param duration_s Duration in seconds
 */
static void generate_silence(float duration_s)
{
    int16_t mono_chunk[AUDIO_CHUNK_SIZE] = {0};
    int num_samples = (int)(TONE_SAMPLE_RATE * duration_s);

    for (int i = 0; i < num_samples; i += AUDIO_CHUNK_SIZE)
    {
        int batch = (num_samples - i > AUDIO_CHUNK_SIZE) ? AUDIO_CHUNK_SIZE : (num_samples - i);
        if (audio_renderer_write(mono_chunk, batch) != ESP_OK)
        {
            break;
        }
    }
}

void audio_renderer_tone_ready(void)
{
    audio_renderer_set_enable(true);

    // Ascending two-note chime: C5 -> E5 (pleasant upward tone)
    generate_tone(FREQ_C5, TONE_NOTE_DURATION);
    generate_silence(TONE_GAP_DURATION);
    generate_tone(FREQ_E5, TONE_NOTE_DURATION);

    audio_renderer_set_enable(false);
}

void audio_renderer_tone_measurement_done(void)
{
    audio_renderer_set_enable(true);

    // Single confirmation beep: G5 (higher pitched, quick feedback)
    generate_tone(FREQ_G5, TONE_NOTE_DURATION);

    audio_renderer_set_enable(false);
}

void audio_renderer_tone_powering_off(void)
{
    audio_renderer_set_enable(true);

    // Descending two-note chime: E5 -> C5 (pleasant downward tone)
    generate_tone(FREQ_E5, TONE_NOTE_DURATION);
    generate_silence(TONE_GAP_DURATION);
    generate_tone(FREQ_C5, TONE_NOTE_DURATION);

    audio_renderer_set_enable(false);
}

void audio_renderer_tone_error(void)
{
    audio_renderer_set_enable(true);

    // Two short low-frequency beeps: A4 -> gap -> A4
    generate_tone(FREQ_A4, TONE_NOTE_DURATION);
    generate_silence(TONE_GAP_DURATION);
    generate_tone(FREQ_A4, TONE_NOTE_DURATION);

    audio_renderer_set_enable(false);
}
