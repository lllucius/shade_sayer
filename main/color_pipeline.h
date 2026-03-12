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

#ifdef __cplusplus
extern "C" {
#endif

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
    uint16_t kona_id;                //< Matched Kona swatch identifier (0 when not matched)
    bool kona_matched;               //< True when match came from Kona reference table

    // Timestamp
    uint64_t timestamp_us;           //< Reading timestamp (64-bit to avoid overflow)
} color_result_t;

/**
 * @brief Pipeline configuration
 */
typedef struct
{
    // Quality thresholds
    float min_luminance;             //< Minimum acceptable luminance (L*) for valid reading
    float max_delta_e;               //< Maximum acceptable DeltaE for "good" general match
    float kona_max_delta_e;          //< Maximum CIEDE2000 ΔE for Kona swatch match (default: 2.0)
                                     //  2.0 ΔE is approximately the "just noticeable difference"
                                     //  for trained observers. Values 0.5-3.0 are typical.

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
 * @brief Averaged sensor capture statistics for scan/identify workflows.
 */
typedef struct
{
    uint8_t requested_samples;      //< Number of inner sensor measurements requested
    uint8_t accepted_samples;       //< Number of measurements accepted into average
    uint8_t rejected_saturated;     //< Rejected because sensor reported saturation
    uint8_t rejected_low_signal;    //< Rejected because corrected Y was below threshold
    bool any_saturated;             //< True if any inner sample reported saturation
    bool flicker_detected;           //< True if ambient flicker was detected in any sample
    uint8_t gain_code;              //< Representative gain code from accepted set (or last sample)
    uint16_t integration_ms;        //< Representative integration time from accepted set (or last sample)
    uint8_t status2;                //< Representative STATUS2 snapshot from accepted set (or last sample)
    uint8_t status6;                //< Representative STATUS6 snapshot from accepted set (or last sample)
    xyz_t mean_xyz;                 //< Mean corrected XYZ across accepted samples
    xyz_t stddev_xyz;               //< Standard deviation of corrected XYZ across accepted samples
    lab_t mean_lab;                 //< Lab derived from mean XYZ
} color_capture_stats_t;

/**
 * @brief Process a sensor reading and identify the color
 *
 * @param sensor TCS3530 driver pointer
 * @param result Output color identification result
 * @return ESP_OK on success
 */
esp_err_t color_pipeline_identify(TCS3530* sensor, color_result_t* result);

/**
 * @brief Capture and average multiple sensor readings with quality filtering.
 *
 * Uses pipeline-configured sample count/delay and returns averaged XYZ/Lab plus
 * acceptance/rejection counters.
 *
 * @param sensor TCS3530 driver pointer
 * @param led_enabled True if LED illumination is enabled for this capture
 * @param stats Output averaged capture statistics
 * @return ESP_OK on success, ESP_ERR_INVALID_STATE if all samples are rejected
 */
esp_err_t color_pipeline_capture_averaged(TCS3530* sensor,
                                          bool led_enabled,
                                          color_capture_stats_t* stats);

/**
 * @brief Capture averaged readings and emit a CSV log row for Kona scanning workflows.
 *
 * Runs an averaged capture measurement, processes it through the pipeline, and logs
 * a CSV row containing quality counts, status metadata, and averaged XYZ/Lab stats.
 * Column order: swatch_id, swatch_name, led_enabled, gain_code, integration_ms,
 * status2, status6, requested_samples, accepted_samples, rejected_saturated,
 * rejected_low_signal, mean_x, mean_y, mean_z, stddev_x, stddev_y, stddev_z,
 * mean_L, mean_a, mean_b, timestamp_us.
 *
 * @param sensor TCS3530 driver pointer
 * @param led_enabled True if LED illumination is enabled for this capture
 * @param swatch_id Swatch identifier string (nullable)
 * @param swatch_name Human-readable swatch name string (nullable)
 * @param result Optional output color result (nullable)
 * @return ESP_OK on success
 */
esp_err_t color_pipeline_capture_csv(TCS3530* sensor,
                                     bool led_enabled,
                                     const char* swatch_id,
                                     const char* swatch_name,
                                     color_result_t* result);

#endif

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

#ifdef __cplusplus
}
#endif

#endif // COLOR_PIPELINE_H
