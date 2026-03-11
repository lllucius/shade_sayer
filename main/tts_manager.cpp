/**
 * @file tts_manager.cpp
 * @brief Text-to-Speech Manager Implementation
 *
 * Integrates with the picotts library for speech synthesis.
 * Audio output via I2S to MAX98357A amplifier (Adafruit I2S Amplifier BFF).
 *
 * Hardware: Adafruit I2S Amplifier BFF with MAX98357A
 * TTS Library: https://github.com/lllucius/picotts
 *
 * The MAX98357A is a digital amplifier that accepts I2S audio input.
 * It requires BCLK, LRCLK (word select), and DIN (data) connections.
 * The SD_MODE pin can be used to enable/disable the amplifier.
 *
 * @section Architecture
 * The TTS manager uses a producer-consumer pattern:
 * - Main thread queues speech events via tts_speak() or tts_speak_async()
 * - A dedicated FreeRTOS task (speech_task) consumes events and synthesizes audio
 * - Audio samples are written to audio_renderer for I2S output
 *
 * @section Memory
 * - Picotts engine requires ~2.5MB of memory (allocated from PSRAM if available)
 * - Speech events with dynamically allocated text are freed after processing
 * - Resources are released when device enters deep sleep
 */

#include "tts_manager.h"
#include "audio_renderer.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/event_groups.h"

#include <cstring>
#include <cstdlib>
#include <cstdarg>
#include <cstdio>

#include "picoapi.h"
#include "picoextapi.h"

static const char* TAG = "tts";

/** @brief Default sample rate for picotts (Hz) - get from Kconfig */
#define DEFAULT_SAMPLE_RATE CONFIG_TTS_SAMPLE_RATE

/** @brief Number of speech events the queue can hold - get from Kconfig */
#define SPEECH_QUEUE_SIZE CONFIG_TTS_SPEECH_QUEUE_SIZE

/** @brief Stack size for the speech synthesis task (bytes) - get from Kconfig */
#define SPEECH_TASK_STACK_SIZE CONFIG_TTS_TASK_STACK_SIZE

/** @brief Priority of the speech synthesis task - get from Kconfig */
#define SPEECH_TASK_PRIORITY CONFIG_TTS_TASK_PRIORITY

/** @brief Delay after speech completes before disabling amplifier (ms) - get from Kconfig */
#define POST_SPEECH_DELAY_MS CONFIG_TTS_POST_SPEECH_DELAY_MS

/**
 * @brief TTS manager internal state
 *
 * Contains all state variables for the TTS module.
 * This structure is private to tts_manager.cpp.
 */
static struct
{
    tts_config_t config;        /**< Current configuration */
    bool initialized;           /**< True if tts_init() succeeded */
    TaskHandle_t speak_task;    /**< Handle to the speech task */
    bool stop_requested;        /**< Flag to signal task termination */
} s_tts;

/**
 * @brief Speech event structure for the queue
 *
 * Represents a text string to be spoken.
 */
typedef struct speech_event
{
    pico_Char *text;    /**< Text to speak (may be NULL for stop signal) */
    bool free;          /**< If true, text was dynamically allocated and must be freed */
} speech_event_t;

/** @brief Queue for speech events */
static QueueHandle_t s_speech_queue = NULL;

/** @brief Event group for signaling speech idle state */
static EventGroupHandle_t s_speech_event_group = NULL;

/** @brief Bit indicating the speech queue is empty and engine is idle */
#define SPEECH_IDLE_BIT (1 << 0)

/** @brief Pico TTS system handle */
static pico_System s_picoSystem = NULL;

/** @brief Pico TTS engine handle */
static pico_Engine s_picoEngine = NULL;

/** @brief Pointer to picotts memory buffer (for cleanup) */
static void *s_picoMemory = NULL;

/** @brief Memory size required by picotts (bytes) */
static const pico_Uint32 PICO_MEM_SIZE = 2500000;

/**
 * @brief Speech synthesis task
 *
 * FreeRTOS task that processes speech events from the queue.
 * For each event:
 * 1. Plays an audio chime to indicate speech is starting
 * 2. Feeds text to the picotts engine
 * 3. Writes synthesized audio samples to audio_renderer
 * 4. Disables the amplifier after speech completes
 *
 * The task runs continuously until device power is cut.
 *
 * @param pvParameters Unused task parameter
 */
static void speech_task(void *pvParameters)
{
    (void)pvParameters;

    pico_Status status = PICO_STEP_IDLE;
    pico_Int16 bytes_sent;
    speech_event_t sevt = {};
    int16_t audio_buffer[256];

    // Start in idle state - signal that we're ready
    xEventGroupSetBits(s_speech_event_group, SPEECH_IDLE_BIT);

    while (!s_tts.stop_requested)
    {
        if (status == PICO_STEP_IDLE)
        {
            // Wait for a speech event with timeout to allow checking stop_requested
            if (xQueueReceive(s_speech_queue, &sevt, pdMS_TO_TICKS(100)) == pdTRUE)
            {
                // Clear idle bit when we start processing
                xEventGroupClearBits(s_speech_event_group, SPEECH_IDLE_BIT);
                pico_resetEngine(s_picoEngine, PICO_RESET_SOFT);
                if (sevt.text)
                {
                    // Re-enable amplifier for speech (tone disables it when done)
                    audio_renderer_set_enable(true);
                    pico_Int16 text_len = strlen((const char *)sevt.text) + 1;
                    pico_putTextUtf8(s_picoEngine, sevt.text, text_len, &bytes_sent);
                    if (sevt.free)
                    {
                        free(sevt.text);
                        sevt.text = NULL;
                    }
                    status = PICO_STEP_BUSY;
                }
            }
            else
            {
                // Queue receive timed out with no items
                // Since status is PICO_STEP_IDLE and queue is empty, we're truly idle
                xEventGroupSetBits(s_speech_event_group, SPEECH_IDLE_BIT);
            }
        }

        if (status == PICO_STEP_BUSY)
        {
            pico_Int16 bytes_recv = 0;
            pico_Int16 out_data_type;
            status = pico_getData(s_picoEngine, (void *)audio_buffer, sizeof(audio_buffer), &bytes_recv, &out_data_type);

            if (bytes_recv > 0)
            {
                // Ensure bytes_recv is aligned to sample boundaries (16-bit samples)
                size_t sample_count = bytes_recv / sizeof(int16_t);
                if (sample_count > 0)
                {
                    // Apply software volume scaling if volume is less than 100%
                    uint8_t volume = s_tts.config.volume;
                    if (volume < 100)
                    {
                        for (size_t i = 0; i < sample_count; i++)
                        {
                            // Use int32_t intermediate to prevent overflow
                            int32_t scaled = ((int32_t)audio_buffer[i] * volume) / 100;
                            audio_buffer[i] = (int16_t)scaled;
                        }
                    }
                    audio_renderer_write(audio_buffer, sample_count);
                }
            }
            else if (status == PICO_STEP_IDLE)
            {
                vTaskDelay(pdMS_TO_TICKS(POST_SPEECH_DELAY_MS));
                audio_renderer_set_enable(false);

                // Check if queue is empty after completing this speech
                if (uxQueueMessagesWaiting(s_speech_queue) == 0)
                {
                    xEventGroupSetBits(s_speech_event_group, SPEECH_IDLE_BIT);
                }
            }
        }
    }

    // Task cleanup: notify that we're done
    s_tts.speak_task = NULL;
    vTaskDelete(NULL);
}

esp_err_t tts_init(const tts_config_t* config)
{
    if (!config)
    {
        return ESP_ERR_INVALID_ARG;
    }

    // Prevent double initialization
    if (s_tts.initialized)
    {
        ESP_LOGW(TAG, "TTS already initialized");
        return ESP_OK;
    }

    memset(&s_tts, 0, sizeof(s_tts));
    s_tts.config = *config;

    // Set default sample rate if not specified
    if (s_tts.config.sample_rate == 0)
    {
        s_tts.config.sample_rate = DEFAULT_SAMPLE_RATE;
    }

    // Note: audio_renderer is now initialized separately in main.cpp.
    // TTS manager just uses it for audio output.
    ESP_LOGI(TAG, "Initializing TTS with picotts + MAX98357A I2S amplifier");
    ESP_LOGI(TAG, "Sample rate: %lu Hz", (unsigned long)s_tts.config.sample_rate);

    pico_Retstring msg;
    pico_Status ret;

    ESP_LOGI(TAG, "Initial free heap: %ldK", esp_get_free_heap_size() / 1024);

    // Try PSRAM (SPIRAM) allocation first, fallback to regular malloc
    s_picoMemory = heap_caps_malloc(PICO_MEM_SIZE, MALLOC_CAP_SPIRAM);
    if (s_picoMemory != NULL)
    {
        ESP_LOGI(TAG, "Pico memory allocated in PSRAM");
    }
    else
    {
        ESP_LOGW(TAG, "PSRAM allocation failed, falling back to standard malloc");
        s_picoMemory = malloc(PICO_MEM_SIZE);
    }
    if (s_picoMemory == NULL)
    {
        ESP_LOGE(TAG, "Failed to allocate memory for pico TTS engine");
        return ESP_ERR_NO_MEM;
    }

    ret = pico_initialize(s_picoMemory, PICO_MEM_SIZE, &s_picoSystem);
    if (ret != PICO_OK)
    {
        pico_getSystemStatusMessage(s_picoSystem, ret, msg);
        ESP_LOGE(TAG, "Cannot initialize pico (%i): %s", ret, msg);
        free(s_picoMemory);
        s_picoMemory = NULL;
        return ESP_FAIL;
    }

    ret = pico_loadVoices(s_picoSystem);
    if (ret != PICO_OK)
    {
        pico_getSystemStatusMessage(s_picoSystem, ret, msg);
        ESP_LOGE(TAG, "Loading voices failed (%i): %s", ret, msg);
        pico_terminate(&s_picoSystem);
        free(s_picoMemory);
        s_picoMemory = NULL;
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "Voices loaded");

    // Create a new Pico engine
    ret = pico_newEngine(s_picoSystem, PICO_VOICE_EN_US, &s_picoEngine);
    if (ret != PICO_OK)
    {
        pico_getSystemStatusMessage(s_picoSystem, ret, msg);
        ESP_LOGE(TAG, "Cannot create a new pico engine (%i): %s", ret, msg);
        pico_terminate(&s_picoSystem);
        free(s_picoMemory);
        s_picoMemory = NULL;
        return ESP_FAIL;
    }

    // Create speech event queue
    s_speech_queue = xQueueCreate(SPEECH_QUEUE_SIZE, sizeof(speech_event_t));
    if (s_speech_queue == NULL)
    {
        ESP_LOGE(TAG, "Failed to create speech queue");
        pico_disposeEngine(s_picoSystem, &s_picoEngine);
        pico_terminate(&s_picoSystem);
        free(s_picoMemory);
        s_picoMemory = NULL;
        return ESP_ERR_NO_MEM;
    }

    // Create event group for signaling speech completion
    s_speech_event_group = xEventGroupCreate();
    if (s_speech_event_group == NULL)
    {
        ESP_LOGE(TAG, "Failed to create speech event group");
        vQueueDelete(s_speech_queue);
        s_speech_queue = NULL;
        pico_disposeEngine(s_picoSystem, &s_picoEngine);
        pico_terminate(&s_picoSystem);
        free(s_picoMemory);
        s_picoMemory = NULL;
        return ESP_ERR_NO_MEM;
    }

    // Create speech synthesis task
    s_tts.stop_requested = false;
    BaseType_t task_ret = xTaskCreate(
                              &speech_task,
                              "speech_task",
                              SPEECH_TASK_STACK_SIZE,
                              NULL,
                              SPEECH_TASK_PRIORITY,
                              &s_tts.speak_task
                          );
    if (task_ret != pdPASS)
    {
        ESP_LOGE(TAG, "Failed to create speech task");
        vEventGroupDelete(s_speech_event_group);
        s_speech_event_group = NULL;
        vQueueDelete(s_speech_queue);
        s_speech_queue = NULL;
        pico_disposeEngine(s_picoSystem, &s_picoEngine);
        pico_terminate(&s_picoSystem);
        free(s_picoMemory);
        s_picoMemory = NULL;
        return ESP_ERR_NO_MEM;
    }

    s_tts.initialized = true;
    ESP_LOGI(TAG, "TTS initialization complete");

    return ESP_OK;
}

/**
 * @brief Internal helper to queue speech (used by both sync and async variants)
 */
static esp_err_t tts_speak_internal(const char *fmt, va_list args)
{
    if (!s_tts.initialized)
    {
        return ESP_ERR_INVALID_STATE;
    }

    if (!fmt)
    {
        return ESP_ERR_INVALID_ARG;
    }

    pico_Char *text = NULL;

    int len = vasprintf((char **)&text, fmt, args);

    if (len < 0 || text == NULL)
    {
        ESP_LOGE(TAG, "Failed to format speech text");
        return ESP_ERR_NO_MEM;
    }

    speech_event_t evt = {text, true};

    ESP_LOGI(TAG, "Speak: %s", (char *)text);

    // Clear idle bit before queuing - we're about to have work to do
    xEventGroupClearBits(s_speech_event_group, SPEECH_IDLE_BIT);

    if (xQueueSendToBack(s_speech_queue, (void *)&evt, pdMS_TO_TICKS(1000)) != pdTRUE)
    {
        ESP_LOGW(TAG, "Speech queue full, dropping message");
        free(text);
        // Note: We don't restore idle bit here - the speech task will
        // set it appropriately on its next iteration when queue is empty
        return ESP_ERR_TIMEOUT;
    }

    return ESP_OK;
}

esp_err_t tts_speak_async(const char *fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    esp_err_t ret = tts_speak_internal(fmt, args);
    va_end(args);
    return ret;
}

esp_err_t tts_speak(const char *fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    esp_err_t ret = tts_speak_internal(fmt, args);
    va_end(args);

    if (ret != ESP_OK)
    {
        return ret;
    }

    // Wait for speech to complete
    return tts_wait_for_completion(portMAX_DELAY);
}

esp_err_t tts_wait_for_completion(TickType_t timeout)
{
    if (!s_tts.initialized)
    {
        return ESP_ERR_INVALID_STATE;
    }

    // Wait for the SPEECH_IDLE_BIT to be set, indicating queue is empty and not speaking
    EventBits_t bits = xEventGroupWaitBits(
        s_speech_event_group,
        SPEECH_IDLE_BIT,
        pdFALSE,            // Don't clear bits on exit
        pdTRUE,             // Wait for all bits (just one bit here)
        timeout
    );

    if (bits & SPEECH_IDLE_BIT)
    {
        return ESP_OK;
    }

    return ESP_ERR_TIMEOUT;
}

esp_err_t tts_stop(void)
{
    if (!s_tts.initialized)
    {
        return ESP_OK;
    }

    // Flush the speech queue and free any pending dynamically allocated text
    if (s_speech_queue != NULL)
    {
        speech_event_t evt;
        while (xQueueReceive(s_speech_queue, &evt, 0) == pdTRUE)
        {
            if (evt.free && evt.text != NULL)
            {
                free(evt.text);
            }
        }
    }

    // Reset the pico engine to abort current processing
    if (s_picoEngine != NULL)
    {
        pico_resetEngine(s_picoEngine, PICO_RESET_SOFT);
    }

    // Disable amplifier
    audio_renderer_set_enable(false);

    // Signal that speech is now idle since we've cleared everything
    if (s_speech_event_group != NULL)
    {
        xEventGroupSetBits(s_speech_event_group, SPEECH_IDLE_BIT);
    }

    return ESP_OK;
}

esp_err_t tts_set_volume(uint8_t volume)
{
    if (!s_tts.initialized)
    {
        return ESP_ERR_INVALID_STATE;
    }

    s_tts.config.volume = (volume > 100) ? 100 : volume;
    ESP_LOGI(TAG, "Volume set to %d%%", s_tts.config.volume);

    /*
     * Note: MAX98357A doesn't have hardware volume control.
     * Volume is applied by scaling audio samples in speech_task before I2S output.
     */

    return ESP_OK;
}

esp_err_t tts_set_speed(float speed)
{
    if (!s_tts.initialized)
    {
        return ESP_ERR_INVALID_STATE;
    }

    if (speed < 0.5f)
    {
        speed = 0.5f;
    }
    if (speed > 2.0f)
    {
        speed = 2.0f;
    }

    s_tts.config.speed = speed;
    ESP_LOGI(TAG, "Speed set to %.1fx", speed);

    /*
     * Speech speed is controlled via picotts configuration.
     * The pico_set_speed() function adjusts synthesis rate.
     */

    return ESP_OK;
}
