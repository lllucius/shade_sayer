/**
 * @file audio_renderer.h
 * @brief I2S Audio Output Handler for MAX98357A
 *
 * Manages audio output via I2S to MAX98357A digital amplifier.
 * Provides functionality for:
 * - I2S channel initialization and configuration
 * - Audio sample writing to I2S DMA buffer
 * - Amplifier enable/disable via SD_MODE pin
 * - Audio feedback sounds (chime)
 *
 * Hardware: Adafruit I2S Amplifier BFF with MAX98357A
 */

#ifndef AUDIO_RENDERER_H
#define AUDIO_RENDERER_H

#include <stdint.h>
#include <stddef.h>
#include "esp_err.h"
#include "driver/gpio.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Audio renderer configuration
 */
typedef struct
{
    gpio_num_t bclk_io_num;     /**< Bit Clock Pin */
    gpio_num_t ws_io_num;       /**< Word Select (LRCLK) Pin */
    gpio_num_t dout_io_num;     /**< Data Out Pin */
    gpio_num_t sd_mode_io_num;  /**< SD Mode / Enable Pin (GPIO_NUM_NC if unused) */
    uint32_t sample_rate;       /**< Sample rate in Hz */
} audio_renderer_config_t;

/**
 * @brief Initialize the audio renderer
 *
 * Sets up the I2S channel in master mode for mono 16-bit audio output.
 * Configures the SD_MODE GPIO for amplifier enable/disable control if provided.
 *
 * @param config Pointer to audio renderer configuration structure
 * @return ESP_OK on success, ESP_ERR_INVALID_ARG if config is NULL,
 *         or I2S driver error codes on failure
 */
esp_err_t audio_renderer_init(const audio_renderer_config_t* config);

/**
 * @brief Write audio samples to the I2S output
 *
 * Writes 16-bit mono audio samples to the I2S DMA buffer.
 * This function blocks until all samples are written.
 *
 * @param samples Pointer to array of 16-bit audio samples
 * @param count Number of samples to write
 * @return ESP_OK on success, ESP_ERR_INVALID_STATE if not initialized,
 *         or I2S driver error codes on failure
 */
esp_err_t audio_renderer_write(const int16_t *samples, size_t count);

/**
 * @brief Enable or disable the audio amplifier
 *
 * Controls the MAX98357A amplifier via the SD_MODE pin.
 * When disabled, the amplifier enters shutdown mode for power savings.
 *
 * @param enable true to enable amplifier, false to disable
 */
void audio_renderer_set_enable(bool enable);

/**
 * @brief Prepare the audio renderer for deep sleep
 *
 * Disables the I2S channel, releases I2S GPIOs to inputs, and ensures the
 * amplifier is muted before the device enters deep sleep. This is intended
 * to be called just before deep sleep and does not support resuming without
 * reinitialization.
 */
void audio_renderer_prepare_for_sleep(void);

/**
 * @brief Play device ready attention tone
 *
 * Generates and plays an ascending two-note chime (C5->E5) to indicate
 * the device is ready for use. Produces 16-bit mono at 16000Hz with
 * normalized volume.
 *
 * This function enables the amplifier before playing and disables it after.
 */
void audio_renderer_tone_ready(void);

/**
 * @brief Play measurement done attention tone
 *
 * Generates and plays a short confirmation beep (G5) to indicate
 * a measurement has completed successfully. Produces 16-bit mono at 16000Hz
 * with normalized volume.
 *
 * This function enables the amplifier before playing and disables it after.
 */
void audio_renderer_tone_measurement_done(void);

/**
 * @brief Play powering off attention tone
 *
 * Generates and plays a descending two-note chime (E5->C5) to indicate
 * the device is powering off. Produces 16-bit mono at 16000Hz with
 * normalized volume.
 *
 * This function enables the amplifier before playing and disables it after.
 */
void audio_renderer_tone_powering_off(void);

/**
 * @brief Play error attention tone
 *
 * Generates and plays two short low-frequency beeps (A4) to indicate
 * an error has occurred. Produces 16-bit mono at 16000Hz with
 * normalized volume.
 *
 * This function enables the amplifier before playing and disables it after.
 */
void audio_renderer_tone_error(void);

#ifdef __cplusplus
}
#endif

#endif // AUDIO_RENDERER_H
