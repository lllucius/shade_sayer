/**
 * @file color_pipeline.h
 * @brief Color Processing Pipeline for TCS3530
 *
 * This module provides the complete color processing pipeline from
 * raw sensor readings to identified color names with descriptions.
 *
 * Pipeline stages:
 * 1. Raw sensor data acquisition
 * 2. Calibration and normalization
 * 3. XYZ to Lab color space conversion
 * 4. Color database lookup
 * 5. Description generation
 */

#ifndef COLOR_PIPELINE_H
#define COLOR_PIPELINE_H

#include "tcs_glue.h"
#ifdef ESP_PLATFORM
#include "tcs3530_driver.h"
#endif
#include "color_types.h"
#include "color_math.h"
#include "color_database.h"
#include "color_matcher.h"

/**
 * @brief Color identification result
 */
typedef struct
{
    // Identified color
    const char* color_name;         //< Name of the identified color
    const char* description;        //< Human-readable description

    // Color values in different spaces
    xyz_t xyz;                       //< CIE XYZ tristimulus values
    lab_t lab;                       //< CIE L*a*b* color space
    uint8_t rgb[3];                  //< sRGB for display/reference

    // Quality metrics
    float confidence;                //< Match confidence (0.0-1.0)
    float delta_e;                   //< CIEDE2000 distance to nearest color
    bool saturated;                  //< True if sensor was saturated
    bool low_light;                  //< True if light level was too low
    bool flicker_detected;           //< True if lighting flicker was detected

    // Additional info
    float luminance;                 //< Relative luminance (0-100)
    float saturation;                //< Color saturation (0-1)
    const char* category;            //< Basic color category

    // Timestamp
    uint64_t timestamp_us;           //< Reading timestamp (64-bit to avoid overflow)
} color_result_t;

/**
 * @brief Pipeline configuration
 */
typedef struct
{
    // Quality thresholds
    float min_luminance;             //< Minimum acceptable luminance
    float max_delta_e;               //< Maximum acceptable DeltaE for "good" match

    // Calibration - Dual white balance profiles + Black Level
    bool use_white_balance;          //< Apply white point correction
    xyz_t white_reference_led;       //< Reference white point for LED illumination
    xyz_t white_reference_ambient;   //< Reference white point for ambient lighting
    xyz_t black_level;               //< Black level offset (crosstalk/noise)
    bool has_led_calibration;        //< True if LED calibration is available
    bool has_ambient_calibration;    //< True if ambient calibration is available
    bool has_black_calibration;      //< True if black level calibration is available

    // Averaging
    uint8_t num_samples;             //< Number of readings to average
    uint16_t sample_delay_ms;        //< Delay between samples

    // Color tuning parameters
    float gray_threshold;            //< Chroma below this is gray (default 5.0)
    float color_threshold;           //< Chroma above this is vivid (default 60.0)
    float saturation_boost;          //< Boost factor for vivid colors (default 2.5)
} color_pipeline_config_t;

/**
 * @brief Set calibration parameters
 *
 * Updates the color pipeline's calibration parameters.
 * Used by auto-calibration system to apply optimized parameters.
 *
 * @param params Calibration parameters to apply
 * @return ESP_OK on success
 */
esp_err_t color_pipeline_set_params(const color_calib_params_t* params);

/**
 * @brief Initialize color processing pipeline
 *
 * @param config Pipeline configuration
 * @return ESP_OK on success
 */
esp_err_t color_pipeline_init(const color_pipeline_config_t* config);

#if defined(ESP_PLATFORM) && defined(__cplusplus)
/**
 * @brief Process a sensor reading and identify the color
 *
 * @param sensor TCS3530 driver pointer
 * @param result Output color identification result
 * @return ESP_OK on success
 */
esp_err_t color_pipeline_identify(TCS3530* sensor, color_result_t* result);
#endif // defined(ESP_PLATFORM) && defined(__cplusplus)


/**
 * @brief Process a pre-captured raw sensor reading and identify color.
 *
 * This applies the same sensor correction and color identification stages used by
 * the runtime identify path, but without calling sensor->measure().
 * Useful for host replay/testing with recorded raw captures.
 *
 * @param reading Raw sensor reading (counts + gain + integration metadata)
 * @param led_enabled True if measurement was taken with LED illumination
 * @param result Output color identification result
 * @return ESP_OK on success
 */
esp_err_t color_pipeline_identify_from_reading(const sensor_reading_t* reading,
                                               bool led_enabled,
                                               color_result_t* result);

/**
 * @brief Process XYZ values (for testing without sensor)
 *
 * @param xyz Input XYZ color values
 * @param use_led_cal If true, use LED calibration; if false, use ambient calibration
 * @param result Output color identification result
 * @return ESP_OK on success
 */
esp_err_t color_pipeline_process_xyz(const xyz_t* xyz, bool use_led_cal, color_result_t* result);

/**
 * @brief Get a simple color category from Lab values
 *
 * Returns basic categories like "Red", "Green", "Blue", "Yellow", etc.
 *
 * @param lab Lab color
 * @return Category string
 */
const char* color_pipeline_get_category(const lab_t* lab);

/**
 * @brief Generate spoken description of a color result
 *
 * Creates a natural language description suitable for TTS.
 *
 * @param result Color result
 * @param buffer Output buffer
 * @param buffer_size Buffer size
 * @return Length of generated string
 */
int color_pipeline_describe(const color_result_t* result, char* buffer, size_t buffer_size);

/**
 * @brief Get pipeline statistics
 *
 * @param total_identifications Total colors identified
 * @param avg_processing_ms Average processing time
 * @param avg_confidence Average match confidence
 */
void color_pipeline_get_stats(uint32_t* total_identifications,
                              float* avg_processing_ms,
                              float* avg_confidence);

#endif // COLOR_PIPELINE_H
