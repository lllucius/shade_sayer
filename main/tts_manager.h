/**
 * @file tts_manager.h
 * @brief Text-to-Speech Manager for Color Detector
 *
 * Manages speech synthesis for color descriptions using the picotts library.
 * Audio output is via I2S to a MAX98357A digital amplifier.
 *
 * @section Features
 * - Text-to-speech conversion using the picotts engine
 * - Printf-style format string support for dynamic speech
 * - Synchronous (tts_speak) and asynchronous (tts_speak_async) options
 * - Speech synthesis runs in a dedicated FreeRTOS task
 * - Audio chime feedback before speech begins
 * - Configurable volume and speed (speed not yet applied)
 *
 * @section Hardware
 * - Adafruit I2S Amplifier BFF with MAX98357A
 * - I2S interface: BCLK, LRCLK (word select), DIN (data)
 * - Optional SD_MODE pin for amplifier enable/disable
 *
 * @section Dependencies
 * - picotts library: https://github.com/lllucius/picotts
 * - audio_renderer module must be initialized before tts_init()
 *
 * @section Usage Example
 * @code
 * // Initialize audio renderer first
 * audio_renderer_config_t audio_cfg = { ... };
 * audio_renderer_init(&audio_cfg);
 *
 * // Initialize TTS
 * tts_config_t tts_cfg = {
 *     .volume = 80,
 *     .speed = 1.0f,
 *     .sample_rate = 16000
 * };
 * tts_init(&tts_cfg);
 *
 * // Speak text synchronously (blocks until complete)
 * tts_speak("The color is %s", "red");
 *
 * // Or speak asynchronously (returns immediately)
 * tts_speak_async("Hello, world!");
 * // ... do other work ...
 * tts_wait_for_completion(portMAX_DELAY);  // Wait when needed
 *
 * @endcode
 *
 * @note Speech synthesis runs in a dedicated FreeRTOS task with its own stack.
 * @note The picotts engine requires approximately 2.5MB of memory (PSRAM preferred).
 */

#ifndef TTS_MANAGER_H
#define TTS_MANAGER_H

#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief TTS configuration structure
 *
 * Specifies audio settings for the TTS engine.
 *
 * @note Audio output is handled by audio_renderer which must be
 *       initialized separately before calling tts_init().
 */
typedef struct
{
    uint8_t volume;             /**< Volume level (0-100), default 100 */
    float speed;                /**< Speech speed multiplier (0.5-2.0), default 1.0 */
    uint32_t sample_rate;       /**< Sample rate in Hz (default 16000 if 0) */
} tts_config_t;

/**
 * @brief Initialize the TTS engine
 *
 * Initializes the picotts speech synthesis engine and creates a dedicated
 * FreeRTOS task for audio generation. The engine allocates approximately
 * 2.5MB of memory from PSRAM if available, otherwise from the heap.
 *
 * @param config Pointer to TTS configuration structure
 * @return ESP_OK on success
 * @return ESP_ERR_INVALID_ARG if config is NULL
 * @return ESP_ERR_NO_MEM if memory allocation fails (need ~2.5MB)
 * @return ESP_FAIL if picotts engine initialization fails
 *
 * @pre audio_renderer_init() must be called before this function
 * @post Speech can be queued using tts_speak() or tts_speak_async()
 *
 * @note Calling tts_init() when already initialized returns ESP_OK without reinitializing
 * @note The speech task runs at priority 5 with a 4KB stack
 */
esp_err_t tts_init(const tts_config_t* config);

/**
 * @brief Speak formatted text synchronously
 *
 * Queues text for speech synthesis using printf-style format strings
 * and blocks until speech completes. An audio chime is played before
 * the speech begins.
 *
 * The text is formatted using the provided format string and arguments,
 * queued for processing by the speech task, then waits for completion.
 *
 * @param fmt printf-style format string
 * @param ... Variable arguments for the format string
 * @return ESP_OK on success
 * @return ESP_ERR_INVALID_STATE if not initialized
 * @return ESP_ERR_INVALID_ARG if fmt is NULL
 * @return ESP_ERR_NO_MEM if text formatting fails
 * @return ESP_ERR_TIMEOUT if the speech queue is full (after 1 second)
 *
 * @note This function blocks until the speech completes.
 * @note For non-blocking speech, use tts_speak_async().
 * @note Maximum queue depth is 16 speech events
 *
 * @par Example
 * @code
 * tts_speak("Hello, world!");  // Returns after speech completes
 * tts_speak("The color is %s with %d%% confidence", "red", 95);
 * @endcode
 */
esp_err_t tts_speak(const char *fmt, ...);

/**
 * @brief Speak formatted text asynchronously
 *
 * Queues text for speech synthesis using printf-style format strings
 * and returns immediately. An audio chime is played before the speech begins.
 *
 * The text is formatted using the provided format string and arguments,
 * then queued for asynchronous processing by the speech task.
 *
 * @param fmt printf-style format string
 * @param ... Variable arguments for the format string
 * @return ESP_OK on success
 * @return ESP_ERR_INVALID_STATE if not initialized
 * @return ESP_ERR_INVALID_ARG if fmt is NULL
 * @return ESP_ERR_NO_MEM if text formatting fails
 * @return ESP_ERR_TIMEOUT if the speech queue is full (after 1 second)
 *
 * @note This function returns immediately after queuing; it does not block
 *       until speech completes. Use tts_wait_for_completion() to wait.
 * @note Maximum queue depth is 16 speech events
 *
 * @par Example
 * @code
 * tts_speak_async("Hello, world!");  // Returns immediately
 * // ... do other work ...
 * tts_wait_for_completion(portMAX_DELAY);  // Wait for speech to finish
 * @endcode
 */
esp_err_t tts_speak_async(const char *fmt, ...);

/**
 * @brief Wait for all queued speech to complete
 *
 * Blocks until all queued speech events have been processed and the TTS
 * engine is idle. Uses an event group for efficient signaling instead of
 * polling.
 *
 * @param timeout Maximum time to wait in ticks (portMAX_DELAY for infinite)
 * @return ESP_OK when all speech has completed
 * @return ESP_ERR_INVALID_STATE if TTS is not initialized
 * @return ESP_ERR_TIMEOUT if the timeout expired before completion
 *
 * @par Example
 * @code
 * tts_speak("Hello, world!");
 * tts_speak("How are you?");
 * // Wait for both phrases to complete
 * tts_wait_for_completion(portMAX_DELAY);
 * @endcode
 */
esp_err_t tts_wait_for_completion(TickType_t timeout);

/**
 * @brief Stop current speech and clear the queue
 *
 * Immediately stops any speech synthesis in progress, clears all pending
 * speech events from the queue, and disables the audio amplifier.
 *
 * Dynamically allocated text in queued events is properly freed to prevent
 * memory leaks.
 *
 * @return ESP_OK on success
 * @return ESP_OK if not initialized (no-op)
 *
 * @note This function is synchronous and returns after speech is stopped
 * @note Safe to call even if no speech is in progress
 */
esp_err_t tts_stop(void);

/**
 * @brief Set the volume level
 *
 * Sets the volume level for subsequent speech output.
 * Values above 100 are clamped to 100.
 *
 * @param volume Volume level from 0 (mute) to 100 (maximum)
 * @return ESP_OK on success
 * @return ESP_ERR_INVALID_STATE if not initialized
 *
 * @note The MAX98357A amplifier does not have hardware volume control.
 *       Volume is applied by scaling audio samples in software during playback.
 */
esp_err_t tts_set_volume(uint8_t volume);

/**
 * @brief Set the speech speed
 *
 * Sets the speed multiplier for speech synthesis.
 * Values outside the 0.5-2.0 range are clamped.
 *
 * @param speed Speed multiplier:
 *              - 0.5 = half speed (slower)
 *              - 1.0 = normal speed
 *              - 2.0 = double speed (faster)
 * @return ESP_OK on success
 * @return ESP_ERR_INVALID_STATE if not initialized
 *
 * @warning Speech speed control is not currently implemented.
 *          The value is stored but has no effect on synthesis rate.
 */
esp_err_t tts_set_speed(float speed);

#ifdef __cplusplus
}
#endif

#endif /* TTS_MANAGER_H */
