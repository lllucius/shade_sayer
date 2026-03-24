/**
 * @file color_pipeline.cpp
 * @brief Color Processing Pipeline Implementation
 */

#include "color_pipeline.h"
#include "color_math.h"
#include "konaref.h"
#include "kona_metadata.h"
#include "tcs_glue.h"
#include <cstring>
#include <cstdio>
#include <cmath>
#include <cfloat>

static const char* TAG = "color_pipe";

// Confidence thresholds for color description
static const float CONFIDENCE_HIGH = 0.8f;    //< High confidence - "This is..."
static const float CONFIDENCE_MEDIUM = 0.5f;  //< Medium confidence - "This looks like..."

// Progressive auto-gain for dark surfaces.
// Luminance (L*) threshold below which progressive auto-gain kicks in.
// L*=25 captures most "dark" colors without triggering on medium tones.
static const float DARK_AUTO_GAIN_LUMINANCE_THRESHOLD = 25.0f;

// Target luminance (L*) at which the auto-gain loop considers the signal
// adequate and stops boosting. L*=15 is well above the noise floor.
static const float DARK_AUTO_GAIN_TARGET_LUMINANCE = 15.0f;

// Maximum gain code for progressive auto-gain (256× = code 9).
// Higher values (512×, 1024×, etc.) risk saturating even dark surfaces
// when the LED is on. 256× gives a 32× boost over the 8× default,
// which testing showed provides good dark-color discrimination.
static const uint8_t AUTO_GAIN_MAX_CODE = 9u;  // 256×

// Number of gain steps to boost during low-light retries.
// Each step doubles the gain, so 2 steps = 4x more signal.
static const uint8_t DARK_RETRY_GAIN_BOOST_STEPS = 2u;

// Result strings
static const char* COLOR_NAME_UNKNOWN = "Unknown";
static const char* COLOR_DESC_LOW_LIGHT = "Insufficient light for accurate color identification";
static const char* COLOR_DESC_NO_MATCH = "Could not identify the color";

// Default configuration
static color_pipeline_config_t s_config =
{
    .min_luminance = 5.0f,
    .max_delta_e = 10.0f,
    .kona_max_delta_e = 5.0f,
    .use_white_balance = false,
    .white_reference_led = {D65_X, D65_Y, D65_Z},
    .white_reference_ambient = {D65_X, D65_Y, D65_Z},
    .black_level = {0.0f, 0.0f, 0.0f},
    .has_led_calibration = false,
    .has_ambient_calibration = false,
    .has_black_calibration = false,
    .num_samples = 5,
    .sample_delay_ms = 80,
    .gray_threshold = 5.0f,
    .color_threshold = 60.0f,
    .saturation_boost = 1.5f,
    // Material correction: enabled by default, assume fabric (primary use case)
    .use_material_correction = true,
    .assumed_material = MATERIAL_FABRIC,
    .auto_detect_material = false
};

static color_calib_params_t s_params = {
    // Initialize PCCM as a linear approximation
    // Terms: R, G, B, R², G², B², RG, RB, GB, 1
    .pccm = {
        // X channel
        {  1.15f, -0.05f, -0.10f,  0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f },
        // Y channel
        { -0.20f,  1.10f,  0.10f,  0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f },
        // Z channel
        { -0.15f, -0.15f,  1.30f,  0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f }
    },
    .lightness_scale = 0.93f,
    .lightness_offset = -1.5f,
    .lightness_gamma = 1.0f,  // Start with no gamma correction
    
    // Piecewise gamma parameters (disabled by default with transition=0)
    .lightness_gamma_dark = 1.0f,
    .lightness_gamma_light = 1.0f,
    .lightness_transition = 0.0f,  // 0 = disabled, use single gamma mode
    
    .blue_correction_magnitude = -8.0f,
    .blue_correction_center = 43.0f,
    .blue_correction_width = 10.0f,
    .saturation_boost = 1.5f, // Matched your main.cpp manual override
    .gray_threshold = 4.0f,
    .color_threshold = 60.0f,
    .black_level = {0.0f, 0.0f, 0.0f},
    .has_black_calibration = false
};

/**
 * @brief Cached chromatic adaptation matrix
 *
 * Pre-computed matrix that combines Bradford transform with source/destination
 * white point scaling. This avoids recalculating the full chromatic adaptation
 * for each color sample, providing significant performance improvement.
 *
 * The matrix maps XYZ input directly to adapted XYZ output:
 *   adapted_xyz = s_cached_adaptation_matrix * input_xyz
 */
static float s_cached_adaptation_matrix[3][3] = {{1.0f, 0.0f, 0.0f},
    {0.0f, 1.0f, 0.0f},
    {0.0f, 0.0f, 1.0f}
};
static bool s_matrix_valid = false;
static bool s_matrix_for_led = true;  //< True if matrix is for LED calibration
static bool s_kona_reference_ready = false;

// Cached sensor reading for auto-detect material feature.
// When auto_detect_material is enabled, color_pipeline_identify_from_reading stores
// the sensor reading here so color_pipeline_process_xyz can classify the material.
static sensor_reading_t s_last_sensor_reading;
static bool s_has_sensor_reading = false;

// Cached capture variance for auto-detect material feature.
// When color_pipeline_identify captures multiple samples, the stddev is converted
// to variance and stored here so that color_math_classify_material can use it
// for variance-based heuristics (e.g., fabric detection via high-variance reads).
static xyz_t s_capture_variance;
static bool s_has_capture_variance = false;

// Categories struct (unchanged) ...
typedef struct
{
    float hue_min;
    float hue_max;
    const char* name;
} color_category_t;

static const color_category_t CATEGORIES[] =
{
    {   0,  40, "Red" }, {  40,  70, "Orange" }, {  70, 105, "Yellow" },
    { 105, 160, "Green" }, { 160, 195, "Cyan" }, { 195, 300, "Blue" },
    { 300, 320, "Purple" }, { 320, 340, "Magenta" }, { 340, 360, "Red" }
};

// Brown detection thresholds: low-lightness warm colors (dark orange/yellow hues)
static const float BROWN_MAX_LIGHTNESS = 45.0f;
static const float BROWN_MAX_CHROMA    = 55.0f;
static const float BROWN_HUE_MIN      = 40.0f;
static const float BROWN_HUE_MAX      = 85.0f;

// Reference integration time for normalization (100ms baseline)
#define REFERENCE_INTEGRATION_MS 100.0f

/**
 * @brief Minimum Z value soft floor
 *
 * Prevents total loss of blue channel data when clamping negative values.
 * A small positive value (0.01) is safer than hard clamping to 0.0 which
 * can cause green colors to be misidentified as red.
 */
#define MIN_Z_VALUE 0.01f

// Note: XYZ_OUTPUT_SCALE is now defined in color_types.h to ensure consistency
// between runtime color pipeline and calibration optimizer

/**
 * @brief IR compensation coefficient structure for matrix-based correction
 *
 * These coefficients define the IR crosstalk for each XYZ channel.
 * Future enhancement: Could be extended to a full 3x4 matrix for
 * more sophisticated color correction.
 */
typedef struct
{
    float x;    //< X channel IR crosstalk factor
    float y;    //< Y channel IR crosstalk factor
    float z;    //< Z channel IR crosstalk factor
} ir_coefficients_t;

// IR compensation coefficients for different illumination types
static ir_coefficients_t s_ir_coeff_led = {0.35f, 0.25f, 0.05f};           // LED-dominant
static ir_coefficients_t s_ir_coeff_incandescent = {0.60f, 0.45f, 0.15f};  // Incandescent

/**
 * @brief Active diagnostic replay flags.
 *
 * Set via color_pipeline_set_replay_flags() before color_pipeline_init().
 * Zero by default — no effect on normal device operation.
 * Each bit maps to a REPLAY_* constant defined in color_pipeline.h.
 */
static uint32_t s_replay_flags = 0;

static esp_err_t apply_sensor_correction(const sensor_reading_t* reading, xyz_t* xyz);

/**
 * @brief Attempt to match input color against Kona reference swatches.
 *
 * Performs a linear search through the Kona reference table to find the
 * closest matching swatch using CIEDE2000 color difference. If a match
 * is found within the configured threshold, the result is updated with
 * the Kona swatch information.
 *
 * @param lab Input color in CIE L*a*b* space
 * @param result Output structure to populate with match information
 * @return true if a match was found within kona_max_delta_e threshold
 *
 * @note Uses CIEDE2000 for perceptually uniform matching.
 * @note Default threshold is 2.0 ΔE (just noticeable difference).
 * @note Falls through to legacy VP-Tree matcher if no Kona match found.
 */

// Synthetic entry ID threshold: real Kona IDs are 7–1898, synthetics use 10000+
static constexpr uint32_t KONA_SYNTHETIC_ID_BASE = 10000u;

// Wider ΔE acceptance window for synthetic entries: they are Lab approximations
// derived from real scans, not actual measurements, so a 40% wider window
// prevents them from being rejected when the perceptual distance is slightly
// larger than the nominal threshold.
static constexpr float SYNTHETIC_THRESHOLD_MULTIPLIER = 1.4f;

// Confidence penalty for synthetic matches relative to real scanned matches.
// A value of 0.85 means a synthetic match at ΔE=0 scores 0.85 vs 1.0 for a
// real match, reflecting the inherent approximation error.
static constexpr float SYNTHETIC_CONFIDENCE_FACTOR = 0.85f;

static bool try_match_kona_reference(const lab_t* lab, color_result_t* result)
{
    if (!s_kona_reference_ready)
    {
        return false;
    }
    
    if (!lab || !result)
    {
        return false;
    }

    const size_t count = kona_ref_entry_count();

    // Diagnostic: log input Lab values for debugging CIEDE2000 discrepancy
    ESP_LOGI(TAG, "Kona input: measured Lab L=%.4f a=%.4f b=%.4f (count=%zu)",
             lab->l, lab->a, lab->b, count);

    // VP-tree search — O(log n) for up to 2048 entries
    float best_delta_e = FLT_MAX;
    size_t best_index = 0;
    const kona_ref_t* best_entry = kona_ref_find_closest(
        lab->l, lab->a, lab->b, &best_delta_e, &best_index);

    // Diagnostic: log the best match entry details for debugging
    if (best_entry)
    {
        ESP_LOGI(TAG, "Kona best: [%zu] id=%u ref Lab L=%.4f a=%.4f b=%.4f dE=%.4f",
                 best_index, best_entry->kona_id, best_entry->l, best_entry->a, best_entry->b,
                 best_delta_e);
    }

    // Use a wider threshold for synthetic entries (IDs >= KONA_SYNTHETIC_ID_BASE) because
    // synthetic references are approximations and need a broader acceptance window.
    const bool is_synthetic = best_entry && (best_entry->kona_id >= KONA_SYNTHETIC_ID_BASE);
    const float effective_threshold = is_synthetic
        ? s_config.kona_max_delta_e * SYNTHETIC_THRESHOLD_MULTIPLIER
        : s_config.kona_max_delta_e;

    ESP_LOGI(TAG, "Kona result: best_entry %p best_delta %f max delta %f%s",
             best_entry, best_delta_e, effective_threshold,
             is_synthetic ? " (synthetic)" : "");

    // Reject if no match or distance exceeds threshold
    if (!best_entry || best_delta_e >= effective_threshold)
    {
        return false;
    }

    // Look up swatch metadata (name, panel info)
    const kona_swatch_info_t* info = kona_metadata_find_by_id(best_entry->kona_id);

    if (!info && is_synthetic)
    {
        // Synthetic tint/shade entry — derive name from the source swatch.
        // Synthetic IDs encode: synthetic_id = KONA_SYNTHETIC_ID_BASE + source_id * 10 + variant_index
        // variant_index is positional in the sorted steps list:
        //   0 = "Deep"  (L* -30), 1 = "Dark"  (L* -15)
        //   2 = "Light" (L* +15), 3 = "Pale"  (L* +30)
        const uint32_t encoded = static_cast<uint32_t>(best_entry->kona_id) - KONA_SYNTHETIC_ID_BASE;
        const uint16_t source_id = static_cast<uint16_t>(encoded / 10u);
        const uint8_t  variant   = static_cast<uint8_t>(encoded % 10u);

        const kona_swatch_info_t* source_info = kona_metadata_find_by_id(source_id);

        const char* prefix;
        switch (variant)
        {
            case 0:  prefix = "Deep";     break;
            case 1:  prefix = "Dark";     break;
            case 2:  prefix = "Light";    break;
            case 3:  prefix = "Pale";     break;
            default: prefix = "Shade of"; break;
        }

        // Safe to use a static buffer here: color measurements are single-threaded
        // (driven by a single button press), so this function is never re-entered
        // concurrently. The result->color_name pointer is consumed before the next
        // call to try_match_kona_reference().
        static char s_synthetic_name[64];
        if (source_info && source_info->name)
        {
            snprintf(s_synthetic_name, sizeof(s_synthetic_name),
                     "%s %s", prefix, source_info->name);
        }
        else
        {
            snprintf(s_synthetic_name, sizeof(s_synthetic_name),
                     "%s Kona #%u", prefix, static_cast<unsigned>(source_id));
        }

        result->kona_matched = true;
        result->kona_id      = best_entry->kona_id;
        result->delta_e      = best_delta_e;
        result->color_name   = s_synthetic_name;

        // Apply SYNTHETIC_CONFIDENCE_FACTOR multiplier for synthetic matches — they are
        // approximations and should rank below real scanned matches.
        // Uses quadratic scaling consistent with real Kona confidence formula.
        {
            float ratio = best_delta_e / effective_threshold;
            result->confidence = fmaxf(0.0f, 1.0f - (ratio * ratio)) * SYNTHETIC_CONFIDENCE_FACTOR;
        }

        return true;
    }

    // Populate result with match information
    result->kona_matched = true;
    result->kona_id = best_entry->kona_id;
    result->delta_e = best_delta_e;
    result->color_name = info ? info->name : COLOR_NAME_UNKNOWN;

    // Confidence: quadratic scaling from 1.0 (exact match) to 0.0 (at threshold)
    // Formula: confidence = 1.0 - (deltaE / max_deltaE)²
    // Quadratic scaling better reflects CIEDE2000 perceptual reality: small ΔE
    // values (< 2.0) are barely noticeable and should yield high confidence,
    // while confidence drops steeply as ΔE approaches the acceptance threshold.
    {
        float ratio = best_delta_e / s_config.kona_max_delta_e;
        result->confidence = fmaxf(0.0f, 1.0f - (ratio * ratio));
    }

    return true;
}

// Clear/IR ratio thresholds for interpolation
static const float IR_RATIO_LED_THRESHOLD = 5.0f;           // Above this = mostly LED
static const float IR_RATIO_INCANDESCENT_THRESHOLD = 2.0f;  // Below this = mostly incandescent


// Minimum corrected luminance (Y, CIE 0-100) required for an inner sample to
// contribute to averaged scan/identify statistics.
// Default is 0.5, valid configurable range is 0.0-100.0.
#ifdef ESP_PLATFORM
static constexpr float k_capture_min_accepted_y = static_cast<float>(CONFIG_CAPTURE_MIN_ACCEPTED_Y_X100) / 100.0f;
#else
#ifndef CAPTURE_MIN_ACCEPTED_Y
#define CAPTURE_MIN_ACCEPTED_Y 0.5f
#endif
static constexpr float k_capture_min_accepted_y = CAPTURE_MIN_ACCEPTED_Y;
#endif

static void clamp_xyz_floor(xyz_t* xyz)
{
    if (!xyz)
    {
        return;
    }
    xyz->x = fmaxf(0.01f, xyz->x);
    xyz->y = fmaxf(0.01f, xyz->y);
    xyz->z = fmaxf(0.01f, xyz->z);
}

#ifdef ESP_PLATFORM
static esp_err_t capture_averaged_xyz(TCS3530* sensor,
                                      bool led_enabled,
                                      color_capture_stats_t* stats)
{
    if (!sensor || !stats)
    {
        return ESP_ERR_INVALID_ARG;
    }

    memset(stats, 0, sizeof(*stats));
    stats->requested_samples = (s_config.num_samples > 0) ? s_config.num_samples : 1;

    // Collect accepted XYZ samples in a fixed-size array for trimmed mean computation.
    // Sized at 12 to comfortably hold the default num_samples (5) plus a full texture
    // retry round (another 5), with headroom for any user override up to this cap.
    static const int MAX_ACCEPTED_SAMPLES = 12;
    xyz_t accepted_xyz[MAX_ACCEPTED_SAMPLES];
    sensor_reading_t first_accepted_reading = {};
    bool has_first_accepted = false;

    sensor_reading_t last_reading = {};
    bool has_last = false;
    xyz_t best_low_signal_xyz = {};
    float best_low_signal_y = -1.0f;
    bool has_best_low_signal = false;
    xyz_t best_saturated_xyz = {};
    float best_saturated_y = FLT_MAX;  // Track minimum Y (closest to saturation threshold)
    bool has_best_saturated = false;

    for (uint8_t i = 0; i < stats->requested_samples; ++i)
    {
        sensor_reading_t reading = {};
        esp_err_t ret = sensor->measure(&reading);
        if (ret != ESP_OK)
        {
            ESP_LOGE(TAG, "Measurement failed: %s", esp_err_to_name(ret));
            return ret;
        }

        last_reading = reading;
        has_last = true;

        if (reading.saturated)
        {
            stats->any_saturated = true;
            stats->rejected_saturated++;
        }

        if (!led_enabled && reading.flicker > 0)
        {
            stats->flicker_detected = true;
        }

        xyz_t corrected_xyz;
        ret = apply_sensor_correction(&reading, &corrected_xyz);
        if (ret != ESP_OK)
        {
            ESP_LOGE(TAG, "apply_sensor_correction failed: %s", esp_err_to_name(ret));
            return ret;
        }

        if (reading.saturated)
        {
            // Track the lowest-Y saturated sample as a fallback.
            // Lowest-Y is preferred because it's closest to the saturation threshold
            // and likely the most accurate among over-exposed samples.
            if (!has_best_saturated || corrected_xyz.y < best_saturated_y)
            {
                best_saturated_xyz = corrected_xyz;
                best_saturated_y = corrected_xyz.y;
                has_best_saturated = true;
            }
            goto sample_delay;
        }

        if (corrected_xyz.y < k_capture_min_accepted_y)
        {
            stats->rejected_low_signal++;

            if (!has_best_low_signal || corrected_xyz.y > best_low_signal_y)
            {
                best_low_signal_xyz = corrected_xyz;
                best_low_signal_y = corrected_xyz.y;
                has_best_low_signal = true;
            }

            goto sample_delay;
        }

        // Store accepted sample in the fixed-size collection array.
        if (stats->accepted_samples < MAX_ACCEPTED_SAMPLES)
        {
            accepted_xyz[stats->accepted_samples] = corrected_xyz;
        }
        stats->accepted_samples++;

        if (!has_first_accepted)
        {
            has_first_accepted = true;
            first_accepted_reading = reading;
        }

sample_delay:
        if (i + 1 < stats->requested_samples && s_config.sample_delay_ms > 0)
        {
            tcs_delay_ms(s_config.sample_delay_ms);
        }
    }

    if (stats->accepted_samples == 0)
    {
        if (has_last)
        {
            stats->gain_code = last_reading.gain;
            stats->integration_ms = last_reading.integration_ms;
            stats->status2 = last_reading.status2;
            stats->status6 = last_reading.status6;
            // All samples were rejected (saturated or below signal floor); fall back to
            // the last reading's raw ADC counts so pipeline replay can still be attempted.
            stats->raw_x = last_reading.x;
            stats->raw_y = last_reading.y;
            stats->raw_z = last_reading.z;
            stats->raw_ir = last_reading.ir;
            stats->raw_clear = last_reading.clear;
        }

        // If all samples were below the luminance acceptance floor, keep the
        // best low-signal sample instead of failing the full capture. This
        // avoids aborting scan workflows on very dark swatches while still
        // preserving rejected_low_signal diagnostics.
        if (has_best_low_signal)
        {
            stats->accepted_samples = 1;
            stats->mean_xyz = best_low_signal_xyz;
            stats->stddev_xyz = {0.0f, 0.0f, 0.0f};

            xyz_t lab_xyz = stats->mean_xyz;
            clamp_xyz_floor(&lab_xyz);
            stats->mean_lab = color_math_xyz_to_lab(lab_xyz);

            ESP_LOGW(TAG,
                     "All samples below Y floor %.2f; using best low-signal sample (Y=%.4f)",
                     k_capture_min_accepted_y,
                     best_low_signal_y);
            return ESP_OK;
        }

        // If all samples were saturated, use the lowest-Y saturated sample as
        // a fallback. This allows retry-with-reduced-exposure logic to trigger
        // rather than aborting entirely.
        if (has_best_saturated)
        {
            stats->accepted_samples = 1;
            stats->mean_xyz = best_saturated_xyz;
            stats->stddev_xyz = {0.0f, 0.0f, 0.0f};

            xyz_t lab_xyz = stats->mean_xyz;
            clamp_xyz_floor(&lab_xyz);
            stats->mean_lab = color_math_xyz_to_lab(lab_xyz);

            ESP_LOGW(TAG,
                     "All samples saturated; using best saturated sample (Y=%.4f)",
                     best_saturated_y);
            return ESP_OK;
        }

        return ESP_ERR_INVALID_STATE;
    }

    // Populate the stats fields from the first accepted reading.
    stats->gain_code = first_accepted_reading.gain;
    stats->integration_ms = first_accepted_reading.integration_ms;
    stats->status2 = first_accepted_reading.status2;
    stats->status6 = first_accepted_reading.status6;
    // Capture the raw ADC counts from the first accepted reading. These values
    // are used for kona pipeline replay via regenerate_kona_lab.py.
    stats->raw_x = first_accepted_reading.x;
    stats->raw_y = first_accepted_reading.y;
    stats->raw_z = first_accepted_reading.z;
    stats->raw_ir = first_accepted_reading.ir;
    stats->raw_clear = first_accepted_reading.clear;

    // Clamp the usable count in case accepted_samples exceeded the array bound.
    int n_usable = (stats->accepted_samples < MAX_ACCEPTED_SAMPLES)
                   ? stats->accepted_samples : MAX_ACCEPTED_SAMPLES;

    // === TRIMMED MEAN ===
    // Sort the accepted samples by Y (luminance) using a simple insertion sort.
    // With at most MAX_ACCEPTED_SAMPLES (12) elements this is negligible overhead.
    // Sorting by Y lets us discard the extreme high/low luminance outliers that
    // are caused by micro-shadows and specular highlights on textured fabrics.
    for (int a = 1; a < n_usable; ++a)
    {
        xyz_t key = accepted_xyz[a];
        int j = a - 1;
        while (j >= 0 && accepted_xyz[j].y > key.y)
        {
            accepted_xyz[j + 1] = accepted_xyz[j];
            --j;
        }
        accepted_xyz[j + 1] = key;
    }

    // With 4+ samples, discard the lowest and highest Y outliers (1 from each end).
    int trim_start = 0;
    int trim_end = n_usable;
    if (n_usable >= 4)
    {
        trim_start = 1;
        trim_end = n_usable - 1;
    }

    // Compute mean from the trimmed (core) set.
    float sum_x = 0.0f;
    float sum_y = 0.0f;
    float sum_z = 0.0f;
    int core_count = trim_end - trim_start;
    for (int k = trim_start; k < trim_end; ++k)
    {
        sum_x += accepted_xyz[k].x;
        sum_y += accepted_xyz[k].y;
        sum_z += accepted_xyz[k].z;
    }

    const float fn = static_cast<float>(core_count);
    stats->mean_xyz.x = sum_x / fn;
    stats->mean_xyz.y = sum_y / fn;
    stats->mean_xyz.z = sum_z / fn;

    // Compute stddev from the FULL (untrimmed) set for diagnostics.
    float full_sum_x2 = 0.0f, full_sum_y2 = 0.0f, full_sum_z2 = 0.0f;
    float full_sum_x = 0.0f, full_sum_y = 0.0f, full_sum_z = 0.0f;
    for (int k = 0; k < n_usable; ++k)
    {
        full_sum_x += accepted_xyz[k].x;
        full_sum_y += accepted_xyz[k].y;
        full_sum_z += accepted_xyz[k].z;
        full_sum_x2 += accepted_xyz[k].x * accepted_xyz[k].x;
        full_sum_y2 += accepted_xyz[k].y * accepted_xyz[k].y;
        full_sum_z2 += accepted_xyz[k].z * accepted_xyz[k].z;
    }
    const float fn_full = static_cast<float>(n_usable);
    float mean_x_full = full_sum_x / fn_full;
    float mean_y_full = full_sum_y / fn_full;
    float mean_z_full = full_sum_z / fn_full;
    float var_x = (full_sum_x2 / fn_full) - (mean_x_full * mean_x_full);
    float var_y = (full_sum_y2 / fn_full) - (mean_y_full * mean_y_full);
    float var_z = (full_sum_z2 / fn_full) - (mean_z_full * mean_z_full);

    stats->stddev_xyz.x = sqrtf(fmaxf(0.0f, var_x));
    stats->stddev_xyz.y = sqrtf(fmaxf(0.0f, var_y));
    stats->stddev_xyz.z = sqrtf(fmaxf(0.0f, var_z));

    if (n_usable >= 4)
    {
        TCS_LOGD(TAG, "Trimmed mean: %d/%d core samples (dropped Y min=%.2f max=%.2f)",
                 core_count, n_usable, accepted_xyz[0].y, accepted_xyz[n_usable - 1].y);
    }

    xyz_t lab_xyz = stats->mean_xyz;
    clamp_xyz_floor(&lab_xyz);
    stats->mean_lab = color_math_xyz_to_lab(lab_xyz);

    return ESP_OK;
}
#endif

/**
 * @brief Update the cached chromatic adaptation matrix
 *
 * Pre-computes the Bradford transform matrix from source to destination white point.
 * This avoids recalculating the full chromatic adaptation for each color sample.
 * Uses public Bradford matrices (BRADFORD_MA and BRADFORD_MA_INV) from color_math.h
 *
 * @param src_white Source white point in XYZ
 * @param dst_white Destination white point in XYZ (typically D65)
 * @param use_led True if matrix is for LED calibration, false for ambient
 */
static void update_adaptation_matrix(const xyz_t* src_white, const xyz_t* dst_white, bool use_led)
{
    const float EPSILON = 1e-10f;

    // Transform source and destination white points to cone response domain
    float src_cone[3], dst_cone[3];
    for (int i = 0; i < 3; i++)
    {
        src_cone[i] = BRADFORD_MA[i][0] * src_white->x +
                      BRADFORD_MA[i][1] * src_white->y +
                      BRADFORD_MA[i][2] * src_white->z;
        dst_cone[i] = BRADFORD_MA[i][0] * dst_white->x +
                      BRADFORD_MA[i][1] * dst_white->y +
                      BRADFORD_MA[i][2] * dst_white->z;
    }

    // Calculate scale factors with epsilon protection
    //
    // When the source cone response is near-zero, division would produce
    // undefined or extreme values. Using scale=1.0 preserves the original
    // color channel, which is mathematically safer than allowing division
    // by near-zero. This matches the behavior in color_math_chromatic_adapt().
    float scale[3];
    for (int i = 0; i < 3; i++)
    {
        if (fabsf(src_cone[i]) < EPSILON)
        {
            scale[i] = 1.0f;
            ESP_LOGI(TAG, "Near-zero cone response[%d]=%.6f, using scale=1.0", i, src_cone[i]);
        }
        else
        {
            scale[i] = dst_cone[i] / src_cone[i];
        }
    }

    // Build the combined adaptation matrix: M_inv * diag(scale) * M
    // This is computed as: result[i][j] = sum_k(M_inv[i][k] * scale[k] * M[k][j])
    for (int i = 0; i < 3; i++)
    {
        for (int j = 0; j < 3; j++)
        {
            float sum = 0.0f;
            for (int k = 0; k < 3; k++)
            {
                sum += BRADFORD_MA_INV[i][k] * scale[k] * BRADFORD_MA[k][j];
            }
            s_cached_adaptation_matrix[i][j] = sum;
        }
    }

    s_matrix_valid = true;
    s_matrix_for_led = use_led;

    ESP_LOGI(TAG, "Cached adaptation matrix updated (%s)",
             use_led ? "LED" : "ambient");
}

/**
 * @brief Apply sensor correction to raw readings
 *
 * Performs the complete sensor correction pipeline:
 * 1. Hardware gain normalization
 * 2. Integration time normalization to 100ms baseline
 * 3. Responsivity scaling for XYZ tristimulus values
 * 4. Black level subtraction (if calibrated) - removes sensor offset
 * 5. D65 white point scaling
 * 6. Color Correction Matrix (CCM) application
 * 7. IR compensation using adaptive interpolation
 *
 * @param reading Raw sensor reading with metadata
 * @param xyz Output corrected XYZ tristimulus values
 * @return ESP_OK on success
 */
static esp_err_t apply_sensor_correction(const sensor_reading_t* reading, xyz_t* xyz)
{
    if (!reading || !xyz) return ESP_ERR_INVALID_ARG;

    // HARDWARE GAIN NORMALIZATION - Removed custom factors, standard only
    float raw_x = (float)reading->x;         
    float raw_y = (float)reading->y;  
    float raw_z = (float)reading->z;  

    TCS_LOGD(TAG, "SensorCorr: raw X=%lu Y=%lu Z=%lu clear=%lu IR=%lu gain=%u int_ms=%u",
             (unsigned long)reading->x, (unsigned long)reading->y, (unsigned long)reading->z,
             (unsigned long)reading->clear, (unsigned long)reading->ir,
             (unsigned)reading->gain, (unsigned)reading->integration_ms);

    // Calculate global scaling
    float gain_mult = tcs3530_gain_code_to_multiplier(reading->gain);
    float time_scale = reading->integration_ms / REFERENCE_INTEGRATION_MS;
    if (gain_mult == 0.0f) gain_mult = 1.0f;
    float base_scale = 1.0f / (gain_mult * time_scale);

    TCS_LOGD(TAG, "SensorCorr: gain_mult=%.1f time_scale=%.4f base_scale=%.6f",
             gain_mult, time_scale, base_scale);

    // BASE IRRADIANCE (Normalized by RESP constants)
    // For White light, these three values will now be approximately equal
    float x_in = (raw_x * base_scale) / TCS3530_RESP_X;
    float y_in = (raw_y * base_scale) / TCS3530_RESP_Y;
    float z_in = (raw_z * base_scale) / TCS3530_RESP_Z;

    TCS_LOGD(TAG, "SensorCorr: RESP-norm x_in=%.4f y_in=%.4f z_in=%.4f (pre-black-sub)",
             x_in, y_in, z_in);

    // BLACK LEVEL SUBTRACTION - Remove sensor offset/crosstalk
    // Black level is stored in raw RESP-normalized form (before D65 scaling)
    // Subtract before D65 scaling and CCM to remove systematic offset
    if (s_params.has_black_calibration && !(s_replay_flags & REPLAY_NO_BLACK_CAL))
    {
        // Guard against over-subtraction from stale or aggressive black calibration.
        // Keep at least a small fraction of each channel so normal surfaces are not
        // collapsed to near-zero ("too dark") after calibration.
        const float MAX_BLACK_SUBTRACTION_FRACTION = 0.40f;
        float black_x = fminf(s_params.black_level.x, x_in * MAX_BLACK_SUBTRACTION_FRACTION);
        float black_y = fminf(s_params.black_level.y, y_in * MAX_BLACK_SUBTRACTION_FRACTION);
        float black_z = fminf(s_params.black_level.z, z_in * MAX_BLACK_SUBTRACTION_FRACTION);

        TCS_LOGD(TAG, "SensorCorr: black subtraction: black_x=%.4f black_y=%.4f black_z=%.4f (clamped to 40%%)",
                 black_x, black_y, black_z);

        x_in -= black_x;
        y_in -= black_y;
        z_in -= black_z;
        
        // Clamp to zero to prevent negative values
        x_in = fmaxf(0.0f, x_in);
        y_in = fmaxf(0.0f, y_in);
        z_in = fmaxf(0.0f, z_in);
        
        TCS_LOGD(TAG, "SensorCorr: after black sub: x_in=%.4f y_in=%.4f z_in=%.4f", x_in, y_in, z_in);
    }
    else if (s_replay_flags & REPLAY_NO_BLACK_CAL)
    {
        TCS_LOGD(TAG, "SensorCorr: black subtraction SKIPPED (REPLAY_NO_BLACK_CAL)");
    }

    // APPLY D65 SCALING - Scale to D65 white point before PCCM
    // PCCM was calibrated with D65-normalized inputs (0.95:1.00:1.08 ratio)
    // RESP normalization produces ~1:1:1, so we apply D65 scaling here
    if (!(s_replay_flags & REPLAY_NO_D65_SCALE))
    {
        TCS_LOGD(TAG, "D65 constants: X=%.3f Y=%.3f Z=%.3f", D65_X, D65_Y, D65_Z);
        TCS_LOGD(TAG, "D65 scaling factors: X/100=%.3f Y/100=%.3f Z/100=%.3f", 
                 D65_X / 100.0f, D65_Y / 100.0f, D65_Z / 100.0f);
    
        x_in *= (D65_X / 100.0f);
        y_in *= (D65_Y / 100.0f);
        z_in *= (D65_Z / 100.0f);
    
        TCS_LOGD(TAG, "After D65 scaling: x_in=%.3f y_in=%.3f z_in=%.3f", x_in, y_in, z_in);
    }
    else
    {
        TCS_LOGD(TAG, "D65 pre-scale SKIPPED (REPLAY_NO_D65_SCALE): x_in=%.3f y_in=%.3f z_in=%.3f",
                 x_in, y_in, z_in);
    }

    // APPLY GLOBAL GAIN (lightness_scale) in linear space before PCCM
    // This ensures the polynomial operates on properly scaled data
    float gain = s_params.lightness_scale;
    TCS_LOGD(TAG, "lightness_scale (gain): %.3f", gain);
    
    x_in *= gain;
    y_in *= gain;
    z_in *= gain;
    
    TCS_LOGD(TAG, "After gain scaling: x_in=%.3f y_in=%.3f z_in=%.3f", x_in, y_in, z_in);

    // APPLY 3x10 POLYNOMIAL COLOR CORRECTION MATRIX using shared function
    xyz_t xyz_in = {x_in, y_in, z_in};

    if (s_replay_flags & REPLAY_BYPASS_PCCM)
    {
        // Identity PCCM: pass XYZ through unchanged for diagnostic purposes.
        // Terms: [R, G, B, R², G², B², RG, RB, GB, 1]
        static const float identity_pccm[3][10] =
        {
            { 1.0f, 0.0f, 0.0f,  0.0f, 0.0f, 0.0f,  0.0f, 0.0f, 0.0f,  0.0f },
            { 0.0f, 1.0f, 0.0f,  0.0f, 0.0f, 0.0f,  0.0f, 0.0f, 0.0f,  0.0f },
            { 0.0f, 0.0f, 1.0f,  0.0f, 0.0f, 0.0f,  0.0f, 0.0f, 0.0f,  0.0f }
        };
        *xyz = color_math_apply_pccm(&xyz_in, identity_pccm);
        TCS_LOGD(TAG, "SensorCorr: PCCM BYPASSED (identity matrix, REPLAY_BYPASS_PCCM)");
    }
    else
    {
        *xyz = color_math_apply_pccm(&xyz_in, s_params.pccm);
    }

    // Soft floor for Z prevents total blue loss (essential when using subtraction matrices)
    xyz->z = fmaxf(MIN_Z_VALUE, xyz->z);

    // Scale to output range
    xyz->x *= XYZ_OUTPUT_SCALE;
    xyz->y *= XYZ_OUTPUT_SCALE;
    xyz->z *= XYZ_OUTPUT_SCALE;

    TCS_LOGD(TAG, "SensorCorr: after PCCM+scale: X=%.4f Y=%.4f Z=%.4f",
             xyz->x, xyz->y, xyz->z);

    // IR COMPENSATION
    if (reading->ir > 0 && reading->clear > 0 && !(s_replay_flags & REPLAY_NO_IR_COMP))
    {
        float ir_val = (float)reading->ir; 
        float ir_normalized = (ir_val * base_scale) / TCS3530_RESP_Y * XYZ_OUTPUT_SCALE;
        // Keep IR compensation in the same linear scale domain as XYZ before subtraction.
        // Without this, calibrated low exposure gains can over-subtract and force XYZ to zero.
        ir_normalized *= gain;

        float clear_ir_ratio = (float)reading->clear / (float)reading->ir;
        float t;
        
        if (clear_ir_ratio >= IR_RATIO_LED_THRESHOLD) t = 1.0f;
        else if (clear_ir_ratio <= IR_RATIO_INCANDESCENT_THRESHOLD) t = 0.0f;
        else t = (clear_ir_ratio - IR_RATIO_INCANDESCENT_THRESHOLD) /
                 (IR_RATIO_LED_THRESHOLD - IR_RATIO_INCANDESCENT_THRESHOLD);

        float ir_factor_x = math_lerp(s_ir_coeff_incandescent.x, s_ir_coeff_led.x, t);
        float ir_factor_y = math_lerp(s_ir_coeff_incandescent.y, s_ir_coeff_led.y, t);
        float ir_factor_z = math_lerp(s_ir_coeff_incandescent.z, s_ir_coeff_led.z, t);

        TCS_LOGD(TAG, "IRComp: clear_ir_ratio=%.3f t=%.3f ir_normalized=%.4f (x gain=%.3f)",
                 clear_ir_ratio, t, ir_normalized, gain);
        TCS_LOGD(TAG, "IRComp: ir_factor_x=%.4f ir_factor_y=%.4f ir_factor_z=%.4f",
                 ir_factor_x, ir_factor_y, ir_factor_z);
        TCS_LOGD(TAG, "IRComp: sub_x=%.4f sub_y=%.4f sub_z=%.4f",
                 ir_normalized * ir_factor_x, ir_normalized * ir_factor_y, ir_normalized * ir_factor_z);
        TCS_LOGD(TAG, "IRComp: XYZ pre-IR: X=%.4f Y=%.4f Z=%.4f",
                 xyz->x, xyz->y, xyz->z);

        xyz->x -= ir_normalized * ir_factor_x;
        xyz->y -= ir_normalized * ir_factor_y;
        xyz->z -= ir_normalized * ir_factor_z;

        float xyz_post_sub_x = xyz->x, xyz_post_sub_y = xyz->y, xyz_post_sub_z = xyz->z;

        xyz->x = fmaxf(0.0f, xyz->x);
        xyz->y = fmaxf(0.0f, xyz->y);
        xyz->z = fmaxf(MIN_Z_VALUE, xyz->z);  // Soft floor prevents total blue loss

        TCS_LOGD(TAG, "IRComp: XYZ post-sub=%.4f/%.4f/%.4f post-clamp=%.4f/%.4f/%.4f%s%s%s",
                 xyz_post_sub_x, xyz_post_sub_y, xyz_post_sub_z,
                 xyz->x, xyz->y, xyz->z,
                 xyz_post_sub_x != xyz->x ? " [X clamped]" : "",
                 xyz_post_sub_y != xyz->y ? " [Y clamped]" : "",
                 xyz_post_sub_z != xyz->z ? " [Z clamped]" : "");
    }
    else if (s_replay_flags & REPLAY_NO_IR_COMP)
    {
        TCS_LOGD(TAG, "IRComp: SKIPPED (REPLAY_NO_IR_COMP)");
    }

    TCS_LOGD(TAG, "After CCM+IR: X=%.1f Y=%.1f Z=%.1f", xyz->x, xyz->y, xyz->z);

    return ESP_OK;
}

esp_err_t color_pipeline_init(const color_pipeline_config_t* config)
{
    if (config)
    {
        s_config = *config;
    }

    if (s_config.kona_max_delta_e <= 0.0f)
    {
        s_config.kona_max_delta_e = 5.0f;
    }

    color_database_init();
    if (color_matcher_init() != ESP_OK)
    {
        ESP_LOGE(TAG, "Failed to initialize color matcher");
        return ESP_FAIL;
    }

    // Attempt to load auto-calibration params (skipped when REPLAY_NO_AUTO_CAL is set)
    if (s_replay_flags & REPLAY_NO_AUTO_CAL)
    {
        ESP_LOGI(TAG, "Auto-calibration load SKIPPED (REPLAY_NO_AUTO_CAL) — using firmware defaults");
    }
    else
    {
        size_t size = sizeof(color_calib_params_t);
        color_calib_params_t loaded_params;
        esp_err_t blob_err = tcs_storage_load_blob("auto_cal", "params", &loaded_params, &size);
        if (blob_err == ESP_OK && size == sizeof(color_calib_params_t))
        {
            loaded_params.lightness_offset = 0.0f;
            s_params = loaded_params;
            ESP_LOGI(TAG, "Loaded auto-calibration params");
            ESP_LOGI(TAG, "  lightness: scale=%.4f offset=%.2f gamma=%.4f",
                     loaded_params.lightness_scale, loaded_params.lightness_offset, loaded_params.lightness_gamma);
            ESP_LOGI(TAG, "  saturation_boost=%.3f, has_black_cal=%d",
                     loaded_params.saturation_boost, loaded_params.has_black_calibration);
        }
        else
        {
            ESP_LOGI(TAG, "Using default calibration parameters");
        }
    }

    // Pre-compute the chromatic adaptation matrix if calibration is available
    if (s_config.has_led_calibration)
    {
        xyz_t d65 = {D65_X, D65_Y, D65_Z};
        update_adaptation_matrix(&s_config.white_reference_led, &d65, true);
    }
    else if (s_config.has_ambient_calibration)
    {
        xyz_t d65 = {D65_X, D65_Y, D65_Z};
        update_adaptation_matrix(&s_config.white_reference_ambient, &d65, false);
    }

    s_kona_reference_ready = kona_ref_validate();
    if (s_kona_reference_ready)
    {
        ESP_LOGI(TAG,
                 "Kona reference table ready: version=%u entries=%u",
                 (unsigned int)kona_reference.version,
                 (unsigned int)kona_reference.entry_count);

        // Diagnostic: Verify CIEDE2000 implementation with a known test case.
        // This helps identify if the CIEDE2000 calculation is broken on the device.
        // Test case from CIE Technical Report 142-2001 (CIEDE2000 Color-Difference Formula):
        // Reference pair #1: (50, 2.6772, -79.7751) vs (50, 0, -82.7485) = 2.0425
        lab_t test1 = {50.0f, 2.6772f, -79.7751f};
        lab_t test2 = {50.0f, 0.0f, -82.7485f};
        float test_de = color_math_delta_e_ciede2000(&test1, &test2);
        ESP_LOGI(TAG, "CIEDE2000 selftest: dE=%.4f (expected=2.0425, err=%.4f)",
                 test_de, fabsf(test_de - 2.0425f));

        // Also test with first Kona entry as a sanity check using small Lab offsets
        if (kona_reference.entry_count > 0)
        {
            // Small offset to verify the dE calculation produces reasonable values
            // for nearly identical colors (should give dE < 0.1 for offsets of 0.1)
            static constexpr float SMALL_LAB_OFFSET = 0.1f;
            
            const kona_ref_t* first = &kona_reference.entries[0];
            lab_t first_lab = {first->l, first->a, first->b};
            lab_t test_similar = {first->l, first->a + SMALL_LAB_OFFSET, first->b - SMALL_LAB_OFFSET};
            float similar_de = color_math_delta_e_ciede2000(&first_lab, &test_similar);
            ESP_LOGI(TAG, "Kona entry[0] selftest: id=%u L=%.2f a=%.4f b=%.4f; dE vs +/-%.1f = %.4f",
                     first->kona_id, first->l, first->a, first->b, SMALL_LAB_OFFSET, similar_de);
        }
    }
    else
    {
        ESP_LOGW(TAG,
                 "Kona reference table invalid/unavailable (version=%u entries=%u); using legacy matcher",
                 (unsigned int)kona_reference.version,
                 (unsigned int)kona_reference.entry_count);
    }

    return ESP_OK;
}


esp_err_t color_pipeline_identify_from_reading(const sensor_reading_t* reading,
                                               bool led_enabled,
                                               color_result_t* result)
{
    if (!reading || !result)
    {
        return ESP_ERR_INVALID_ARG;
    }

    memset(result, 0, sizeof(color_result_t));

    // Cache sensor reading for auto-detect material feature
    if (s_config.auto_detect_material)
    {
        s_last_sensor_reading = *reading;
        s_has_sensor_reading = true;
    }

    xyz_t corrected_xyz;
    esp_err_t ret = apply_sensor_correction(reading, &corrected_xyz);
    if (ret != ESP_OK)
    {
        ESP_LOGE(TAG, "apply_sensor_correction failed: %s", esp_err_to_name(ret));
        return ret;
    }

    corrected_xyz.x = fmaxf(0.01f, corrected_xyz.x);
    corrected_xyz.y = fmaxf(0.01f, corrected_xyz.y);
    corrected_xyz.z = fmaxf(0.01f, corrected_xyz.z);

    result->saturated = reading->saturated;
    result->flicker_detected = (!led_enabled && reading->flicker > 0);
    result->xyz = corrected_xyz;
    result->timestamp_us = tcs_time_us();

    ESP_LOGI(TAG, "Measure XYZ (IR+Black Cor): X=%.2f Y=%.2f Z=%.2f",
             result->xyz.x, result->xyz.y, result->xyz.z);

    ret = color_pipeline_process_xyz(&result->xyz, led_enabled, result);

    return ret;
}

#ifdef ESP_PLATFORM
esp_err_t color_pipeline_capture_averaged(TCS3530* sensor,
                                          bool led_enabled,
                                          color_capture_stats_t* stats)
{
    return capture_averaged_xyz(sensor, led_enabled, stats);
}

esp_err_t color_pipeline_identify(TCS3530* sensor, color_result_t* result,
                                  color_capture_stats_t* stats_out)
{
    if (!sensor || !result)
    {
        return ESP_ERR_INVALID_ARG;
    }

    memset(result, 0, sizeof(color_result_t));

    bool led_enabled = false;
    sensor->getLed(&led_enabled);

    color_capture_stats_t capture_stats = {};
    esp_err_t ret = color_pipeline_capture_averaged(sensor, led_enabled, &capture_stats);
    if (ret != ESP_OK)
    {
        return ret;
    }

    // Track which capture produced the accepted result so raw values can be reported.
    color_capture_stats_t winning_stats = capture_stats;

    result->saturated = capture_stats.any_saturated;
    result->flicker_detected = capture_stats.flicker_detected;
    result->xyz = capture_stats.mean_xyz;
    clamp_xyz_floor(&result->xyz);
    result->timestamp_us = tcs_time_us();

    ESP_LOGI(TAG, "Measure XYZ (IR+Black Cor): X=%.2f Y=%.2f Z=%.2f",
             result->xyz.x, result->xyz.y, result->xyz.z);

    ret = color_pipeline_process_xyz(&result->xyz, led_enabled, result);

    // === TEXTURE VARIANCE RETRY ===
    // If the coefficient of variation (CoV) of luminance across the captured samples
    // is high, the surface is likely textured (micro-shadows and specular highlights
    // cause high Y variance). Take a second round of samples and merge the two rounds
    // via a weighted average to reduce the influence of outlier readings.
    // A CoV of 15% was chosen as the texture detection threshold based on the
    // observed variance difference between flat swatches (typically < 5% CoV) and
    // textured fabrics (typically 20–40% CoV). At 15% the trigger is conservative
    // enough to avoid spurious retries on flat surfaces while reliably catching
    // textured materials that would benefit from additional spatial averaging.
    static const float TEXTURE_CV_THRESHOLD = 0.15f;
    if (ret == ESP_OK && !result->low_light && !result->saturated &&
        capture_stats.accepted_samples >= 2 && capture_stats.mean_xyz.y > 0.1f)
    {
        float cv_y = capture_stats.stddev_xyz.y / capture_stats.mean_xyz.y;
        if (cv_y > TEXTURE_CV_THRESHOLD)
        {
            ESP_LOGI(TAG,
                     "High variance detected (CoV=%.2f), taking additional samples for textured surface",
                     cv_y);

            color_capture_stats_t extra_stats = {};
            esp_err_t extra_ret = color_pipeline_capture_averaged(sensor, led_enabled, &extra_stats);
            if (extra_ret == ESP_OK && extra_stats.accepted_samples > 0)
            {
                float n1 = static_cast<float>(capture_stats.accepted_samples);
                float n2 = static_cast<float>(extra_stats.accepted_samples);
                float total = n1 + n2;
                xyz_t merged_xyz;
                merged_xyz.x = (capture_stats.mean_xyz.x * n1 + extra_stats.mean_xyz.x * n2) / total;
                merged_xyz.y = (capture_stats.mean_xyz.y * n1 + extra_stats.mean_xyz.y * n2) / total;
                merged_xyz.z = (capture_stats.mean_xyz.z * n1 + extra_stats.mean_xyz.z * n2) / total;

                // Rebuild the result from the merged XYZ.
                color_result_t merged_result = {};
                merged_result.xyz = merged_xyz;
                clamp_xyz_floor(&merged_result.xyz);
                merged_result.saturated = capture_stats.any_saturated || extra_stats.any_saturated;
                merged_result.flicker_detected = capture_stats.flicker_detected || extra_stats.flicker_detected;
                merged_result.timestamp_us = tcs_time_us();

                esp_err_t merge_ret = color_pipeline_process_xyz(&merged_result.xyz, led_enabled, &merged_result);
                if (merge_ret == ESP_OK)
                {
                    *result = merged_result;
                    // Update capture_stats to reflect the merged total sample count.
                    capture_stats.accepted_samples += extra_stats.accepted_samples;
                    // Keep winning_stats pointing at the first capture round for raw ADC replay.
                }
            }
        }
    }

    // Retry with longer integration time and higher gain if the result is too dark.
    // Combining both gives up to 8x more signal (2x integration × 4x gain), which
    // helps extremely low-reflectance surfaces that are barely above the sensor noise floor.
    if (ret == ESP_OK && result->low_light)
    {
        uint16_t original_integration_ms = sensor->integrationTime();
        uint16_t retry_integration_ms = static_cast<uint16_t>(original_integration_ms * 2);
        if (retry_integration_ms > 1000)
        {
            retry_integration_ms = 1000;
        }

        TCS3530Gain original_gain = sensor->gain();
        uint8_t gain_code = static_cast<uint8_t>(original_gain);
        uint8_t retry_gain_code = gain_code + DARK_RETRY_GAIN_BOOST_STEPS;
        if (retry_gain_code > TCS3530_MAX_GAIN_CODE)
        {
            retry_gain_code = TCS3530_MAX_GAIN_CODE;
        }
        TCS3530Gain retry_gain = static_cast<TCS3530Gain>(retry_gain_code);

        bool boost_integration = (retry_integration_ms > original_integration_ms);
        bool boost_gain = (retry_gain_code > gain_code);

        if (boost_integration || boost_gain)
        {
            ESP_LOGI(TAG, "Low-light retry: int=%ums->%ums, gain=%.0fx->%.0fx",
                     original_integration_ms, boost_integration ? retry_integration_ms : original_integration_ms,
                     tcs3530_gain_code_to_multiplier(gain_code),
                     boost_gain ? tcs3530_gain_code_to_multiplier(retry_gain_code)
                                : tcs3530_gain_code_to_multiplier(gain_code));

            esp_err_t set_ret = ESP_OK;
            if (boost_integration)
            {
                set_ret = sensor->setIntegrationTime(retry_integration_ms);
            }
            if (set_ret == ESP_OK && boost_gain)
            {
                set_ret = sensor->setGain(retry_gain);
            }

            if (set_ret == ESP_OK)
            {
                color_capture_stats_t retry_stats = {};
                set_ret = color_pipeline_capture_averaged(sensor, led_enabled, &retry_stats);
                if (set_ret == ESP_OK)
                {
                    color_result_t retry_result = {};
                    retry_result.xyz = retry_stats.mean_xyz;
                    clamp_xyz_floor(&retry_result.xyz);
                    retry_result.saturated = retry_stats.any_saturated;
                    retry_result.flicker_detected = retry_stats.flicker_detected;
                    retry_result.timestamp_us = tcs_time_us();

                    set_ret = color_pipeline_process_xyz(&retry_result.xyz, led_enabled, &retry_result);
                    if (set_ret == ESP_OK && (!retry_result.low_light || retry_result.luminance > result->luminance))
                    {
                        *result = retry_result;
                        winning_stats = retry_stats;
                    }
                }
            }

            if (boost_integration)
            {
                esp_err_t restore_ret = sensor->setIntegrationTime(original_integration_ms);
                if (restore_ret != ESP_OK)
                {
                    ESP_LOGW(TAG, "Failed to restore integration time: %s", esp_err_to_name(restore_ret));
                }
            }
            if (boost_gain)
            {
                esp_err_t restore_ret = sensor->setGain(original_gain);
                if (restore_ret != ESP_OK)
                {
                    ESP_LOGW(TAG, "Failed to restore gain: %s", esp_err_to_name(restore_ret));
                }
            }
        }
    }

// Retry once with shorter integration time if the result is saturated.
    // This helps glossy/specular objects by reducing clipping.
    if (ret == ESP_OK && result->saturated)
    {
        uint16_t original_integration_ms = sensor->integrationTime();
        uint16_t retry_integration_ms = static_cast<uint16_t>(original_integration_ms / 2);
        if (retry_integration_ms < 1)
        {
            retry_integration_ms = 1;
        }

        if (retry_integration_ms < original_integration_ms)
        {
            esp_err_t set_ret = sensor->setIntegrationTime(retry_integration_ms);
            if (set_ret == ESP_OK)
            {
                color_capture_stats_t retry_stats = {};
                set_ret = color_pipeline_capture_averaged(sensor, led_enabled, &retry_stats);
                if (set_ret == ESP_OK)
                {
                    color_result_t retry_result = {};
                    retry_result.xyz = retry_stats.mean_xyz;
                    clamp_xyz_floor(&retry_result.xyz);
                    retry_result.saturated = retry_stats.any_saturated;
                    retry_result.flicker_detected = retry_stats.flicker_detected;
                    retry_result.timestamp_us = tcs_time_us();

                    set_ret = color_pipeline_process_xyz(&retry_result.xyz, led_enabled, &retry_result);
                    if (set_ret == ESP_OK && (!retry_result.saturated || retry_result.luminance < result->luminance))
                    {
                        *result = retry_result;
                        winning_stats = retry_stats;
                    }
                }

                esp_err_t restore_ret = sensor->setIntegrationTime(original_integration_ms);
                if (restore_ret != ESP_OK)
                {
                    ESP_LOGW(TAG, "Failed to restore integration time: %s", esp_err_to_name(restore_ret));
                }
            }
        }
    }

    // Progressive auto-gain for dark surfaces that are above the low_light threshold.
    // When the initial measurement at default gain produces a low luminance result,
    // iteratively step gain upward until the signal is well above the noise floor,
    // the measurement saturates, or the gain ceiling is reached. This significantly
    // improves hue discrimination for dark fabrics (corduroy, dark knits, dark solids).
    if (ret == ESP_OK && !result->low_light && !result->saturated &&
        result->luminance < DARK_AUTO_GAIN_LUMINANCE_THRESHOLD)
    {
        TCS3530Gain original_gain = sensor->gain();
        uint8_t gain_code = static_cast<uint8_t>(original_gain);

        // Track the best non-saturated result found so far.
        color_result_t best_result = *result;
        color_capture_stats_t best_stats = winning_stats;
        uint8_t best_gain_code = gain_code;
        int steps_taken = 0;

        while (gain_code < AUTO_GAIN_MAX_CODE)
        {
            uint8_t next_gain_code = gain_code + 1u;
            steps_taken++;

            TCS3530Gain step_gain = static_cast<TCS3530Gain>(next_gain_code);
            esp_err_t set_ret = sensor->setGain(step_gain);
            if (set_ret != ESP_OK)
            {
                ESP_LOGW(TAG, "Auto-gain step %d: failed to set gain code %u: %s",
                         steps_taken, next_gain_code, esp_err_to_name(set_ret));
                break;
            }

            color_capture_stats_t step_stats = {};
            set_ret = color_pipeline_capture_averaged(sensor, led_enabled, &step_stats);
            if (set_ret != ESP_OK)
            {
                ESP_LOGW(TAG, "Auto-gain step %d: capture failed: %s",
                         steps_taken, esp_err_to_name(set_ret));
                break;
            }

            color_result_t step_result = {};
            step_result.xyz = step_stats.mean_xyz;
            clamp_xyz_floor(&step_result.xyz);
            step_result.saturated = step_stats.any_saturated;
            step_result.flicker_detected = step_stats.flicker_detected;
            step_result.timestamp_us = tcs_time_us();

            set_ret = color_pipeline_process_xyz(&step_result.xyz, led_enabled, &step_result);
            if (set_ret != ESP_OK)
            {
                ESP_LOGW(TAG, "Auto-gain step %d: process_xyz failed: %s",
                         steps_taken, esp_err_to_name(set_ret));
                break;
            }

            ESP_LOGI(TAG, "Auto-gain step %d: gain=%.0fx, luminance=%.1f, saturated=%d",
                     steps_taken,
                     tcs3530_gain_code_to_multiplier(next_gain_code),
                     step_result.luminance,
                     static_cast<int>(step_result.saturated));

            if (step_result.saturated)
            {
                // Saturated — discard this step; the previous best is the highest usable gain.
                break;
            }

            // This step is non-saturated — it has better SNR than the previous, adopt it.
            gain_code = next_gain_code;
            best_result = step_result;
            best_stats = step_stats;
            best_gain_code = gain_code;

            if (step_result.luminance >= DARK_AUTO_GAIN_TARGET_LUMINANCE)
            {
                // Signal is adequate — stop boosting.
                break;
            }
        }

        ESP_LOGI(TAG, "Auto-gain complete: %d steps, final gain=%.0fx, luminance=%.1f",
                 steps_taken,
                 tcs3530_gain_code_to_multiplier(best_gain_code),
                 best_result.luminance);

        *result = best_result;
        winning_stats = best_stats;

        esp_err_t restore_ret = sensor->setGain(original_gain);
        if (restore_ret != ESP_OK)
        {
            ESP_LOGW(TAG, "Auto-gain: failed to restore gain: %s", esp_err_to_name(restore_ret));
        }
    }

    // Cache the capture variance for the material auto-detect classifier.
    // Convert stddev to variance (stddev²) since the classifier uses variance_xyz.
    if (s_config.auto_detect_material && winning_stats.accepted_samples >= 2)
    {
        s_capture_variance.x = winning_stats.stddev_xyz.x * winning_stats.stddev_xyz.x;
        s_capture_variance.y = winning_stats.stddev_xyz.y * winning_stats.stddev_xyz.y;
        s_capture_variance.z = winning_stats.stddev_xyz.z * winning_stats.stddev_xyz.z;
        s_has_capture_variance = true;
    }

    if (stats_out)
    {
        *stats_out = winning_stats;
    }

    return ret;
}
#endif


esp_err_t color_pipeline_process_xyz(const xyz_t* xyz, bool use_led_cal, color_result_t* result)
{
    if (!xyz || !result)
    {
        return ESP_ERR_INVALID_ARG;
    }

    xyz_t corrected = *xyz;
    if (s_config.use_white_balance)
    {
        bool has_cal = use_led_cal ? s_config.has_led_calibration
                       : s_config.has_ambient_calibration;

        if (has_cal)
        {
            // Use cached adaptation matrix if available and matches calibration type.
            //
            // The cache is updated by:
            // - color_pipeline_init() when loading calibrations from NVS
            // - color_pipeline_calibrate() after successful white calibration
            //
            // If the white reference is modified through other means (which is not
            // supported by the public API), the fallback path ensures correct results.
            if (s_matrix_valid && s_matrix_for_led == use_led_cal)
            {
                // Apply cached matrix directly: result = matrix * input
                corrected.x = s_cached_adaptation_matrix[0][0] * xyz->x +
                              s_cached_adaptation_matrix[0][1] * xyz->y +
                              s_cached_adaptation_matrix[0][2] * xyz->z;
                corrected.y = s_cached_adaptation_matrix[1][0] * xyz->x +
                              s_cached_adaptation_matrix[1][1] * xyz->y +
                              s_cached_adaptation_matrix[1][2] * xyz->z;
                corrected.z = s_cached_adaptation_matrix[2][0] * xyz->x +
                              s_cached_adaptation_matrix[2][1] * xyz->y +
                              s_cached_adaptation_matrix[2][2] * xyz->z;
            }
            else
            {
                // Fallback: compute chromatic adaptation on the fly
                xyz_t white_ref = use_led_cal ? s_config.white_reference_led
                                  : s_config.white_reference_ambient;
                corrected = color_math_chromatic_adapt(*xyz, white_ref, (xyz_t)
                {
                    D65_X, D65_Y, D65_Z
                });
            }

            // Clamp negatives
            corrected.x = fmaxf(0.0f, corrected.x);
            corrected.y = fmaxf(0.0f, corrected.y);
            corrected.z = fmaxf(0.0f, corrected.z);
        }
    }

    result->xyz = corrected;

    // For display Lab values, normalize XYZ to prevent extreme b* values for display.
    // The Z floor is applied for display only — it is NOT applied when computing scan_lab
    // (the Lab values captured by SCAN and stored in kona_captures.json) to preserve the
    // natural b* variation between similarly-saturated swatches.
    xyz_t lab_xyz = corrected;

    // Normalize XYZ values when luminance exceeds D65 white reference.
    // This prevents Lab values from going out of gamut (b* > 200) when the sensor
    // reports colors brighter than reference white (e.g., saturated yellows).
    // We scale all channels proportionally to preserve chromaticity (hue).
    // Applied to BOTH display and scan paths.
    if (lab_xyz.y > D65_Y)
    {
        float scale = D65_Y / lab_xyz.y;
        lab_xyz.x *= scale;
        lab_xyz.y *= scale;
        lab_xyz.z *= scale;
        TCS_LOGD(TAG, "XYZ normalized: Y=%.1f -> %.1f (scale=%.4f)", corrected.y, lab_xyz.y, scale);
    }

    // === SCAN LAB PATH (no Z floor, no saturation boost) ===
    // Compute scan_lab from the Y-normalized XYZ WITHOUT applying the Z floor or
    // saturation boost. This produces the raw perceptual Lab value for each swatch
    // that is stored in kona_captures.json and used to build the Kona reference table.
    //
    // Why no saturation boost: the 1.5× boost + ±110 clamp collapses deeply-saturated
    // swatches (e.g., CARROT, TORCH, ORANGEADE) to identical a*=110 b*=110, making
    // them impossible to distinguish.  Pre-boost Lab values naturally differ in both
    // a* and b* (driven by the PCCM-corrected XYZ) and remain distinct.
    //
    // Why no Z floor: the Z floor forces a minimum Z of 20% of Y, which gives all
    // saturated yellows/oranges the same b*≈85 before boost. Without the Z floor the
    // natural b* variation (CANTALOUPE b*≈60 vs SUNNY b*≈73) is preserved.
    {
        // CRITICAL: Use scale=1.0 to prevent double-scaling (same as display path below).
        // s_params.lightness_scale is already applied in apply_sensor_correction() in XYZ
        // space (it scales the whole XYZ triplet). Re-applying it here in Lab space would
        // scale L a second time. Only apply gamma and offset in Lab space.
        color_calib_params_t scan_lightness_params = s_params;
        scan_lightness_params.lightness_scale = 1.0f;

        lab_t scan_lab = color_math_xyz_to_lab(lab_xyz);
        scan_lab.l = color_math_correct_lightness(scan_lab.l, &scan_lightness_params);

        // No saturation boost and no clamp — preserve natural Lab values so that
        // highly-saturated swatches remain distinguishable in kona_captures.json.
        result->scan_lab = scan_lab;

        // === MATERIAL-AWARE CORRECTION ===
        // Apply per-material Lab correction to compensate for systematic measurement
        // bias introduced by different surface physics (fabric vs plastic vs metal).
        // This correction is applied BEFORE ΔE2000 matching to bring measurements
        // closer to ideal viewing conditions.
        //
        // Determine material type:
        // - If auto_detect_material is enabled and we have sensor data, classify from heuristics
        // - Otherwise use assumed_material (default: FABRIC for quilting fabric use case)
        if (s_config.auto_detect_material && s_has_sensor_reading)
        {
            result->material = color_math_classify_material(
                &s_last_sensor_reading,
                s_has_capture_variance ? &s_capture_variance : nullptr);
            TCS_LOGD(TAG, "Auto-detected material: %s", color_math_material_name(result->material));
        }
        else
        {
            result->material = s_config.assumed_material;
        }
        
        if ((s_config.use_material_correction) && !(s_replay_flags & REPLAY_NO_MATERIAL_COR))
        {
            material_correction_t correction = color_math_get_material_correction(result->material);
            result->corrected_lab = color_math_apply_material_correction(&scan_lab, &correction);
            
            TCS_LOGD(TAG, "Material correction (%s): scan_lab L=%.2f a=%.2f b=%.2f -> corrected L=%.2f a=%.2f b=%.2f",
                     color_math_material_name(result->material),
                     scan_lab.l, scan_lab.a, scan_lab.b,
                     result->corrected_lab.l, result->corrected_lab.a, result->corrected_lab.b);
        }
        else
        {
            // No material correction: use scan_lab directly
            result->corrected_lab = scan_lab;
        }
    }

    // === DISPLAY LAB PATH (with Z floor) ===
    // Apply minimum Z floor relative to luminance to prevent extreme b* values on display.
    // When the PCCM collapses Z to near-zero (e.g., for saturated yellows), the Lab
    // b* calculation explodes because b* = 200*(fy - fz) and fz becomes very small.
    // A minimum Z of 20% of Y caps b* at roughly 86 pre-boost (at Y=100, Z_floor=20
    // gives f(20/108.883)=0.573, so b=200*(1.0-0.573)=85.4). This prevents runaway
    // display values while still allowing saturation boost to bring b* up to 110 for
    // fully saturated colors.
    // The Z floor only affects the display path (result->lab); scan_lab uses the
    // natural Z (above) to maintain distinct b* values across different swatches.
    const float MIN_Z_TO_Y_RATIO = 0.20f;  // 20% of Y → pre-boost b* cap ≈ 86
    float min_z = lab_xyz.y * MIN_Z_TO_Y_RATIO;
    if (lab_xyz.z < min_z)
    {
        TCS_LOGD(TAG, "Z floor applied: Z=%.4f -> %.4f (%.1f%% of Y=%.1f)",
                 lab_xyz.z, min_z, MIN_Z_TO_Y_RATIO * 100.0f, lab_xyz.y);
        lab_xyz.z = min_z;
    }

    result->lab = color_math_xyz_to_lab(lab_xyz);

    // Log raw and corrected lightness values for diagnostics
    float raw_L = result->lab.l;

    // CRITICAL: Use scale=1.0 to prevent double-scaling.
    // lightness_scale was already applied in apply_sensor_correction() (XYZ space).
    // Only apply gamma/offset/blue_correction here.
    color_calib_params_t lightness_params = s_params;
    lightness_params.lightness_scale = 1.0f;
    result->lab.l = color_math_correct_lightness(result->lab.l, &lightness_params);

    TCS_LOGD(TAG, "Lightness correction: raw_L=%.1f -> corrected_L=%.1f (gamma=%.4f, scale=%.4f, offset=%.2f)",
             raw_L, result->lab.l, s_params.lightness_gamma, s_params.lightness_scale, s_params.lightness_offset);

    result->luminance = result->lab.l;
    float effective_min_luminance = s_config.min_luminance * fmaxf(s_params.lightness_scale, 0.01f);
    result->low_light = result->lab.l < effective_min_luminance;

    TCS_LOGD(TAG, "Gate: raw_L=%.2f corrected_L=%.2f min_luminance=%.2f lightness_scale=%.4f effective_min=%.2f low_light=%d",
             raw_L, result->lab.l, s_config.min_luminance, s_params.lightness_scale,
             effective_min_luminance, (int)result->low_light);

    float raw_chroma = color_math_chroma(&result->lab);
    TCS_LOGD(TAG, "Pre-enhance Lab: L=%.1f a=%.1f b=%.1f chroma=%.1f",
             result->lab.l, result->lab.a, result->lab.b, raw_chroma);

    // Save pre-boost a*/b* so we can re-apply a confidence-scaled boost later.
    float pre_boost_a = result->lab.a;
    float pre_boost_b = result->lab.b;

    float enhanced_chroma = color_math_enhance_saturation(&result->lab,
                            s_config.gray_threshold,
                            s_config.color_threshold,
                            s_config.saturation_boost);

    result->saturation = enhanced_chroma / 100.0f;

    // Clamp extreme a* and b* values (handles edge cases like bright yellow)
    result->lab.a = fminf(fmaxf(result->lab.a, -110.0f), 110.0f);
    result->lab.b = fminf(fmaxf(result->lab.b, -110.0f), 110.0f);

    result->category = color_pipeline_get_category(&result->lab);

    if (result->low_light)
    {
        result->color_name = COLOR_NAME_UNKNOWN;
        result->description = COLOR_DESC_LOW_LIGHT;
    }
    else
    {
        // Use scan_lab (Y-normalized, no Z floor, no saturation boost) for Kona matching.
        // The Kona reference table is built from kona_captures.json values that were
        // captured using this same scan_lab computation (from actual fabric scans),
        // so matching against scan_lab ensures accurate deltaE between measured colors
        // and stored references. Material correction is NOT applied for Kona matching
        // since the reference table already reflects fabric measurement characteristics.
        // Using pre-boost values keeps highly saturated swatches (e.g., CARROT, TORCH)
        // distinguishable where boost+clamp would otherwise collapse them to identical
        // a*=110, b*=110 values.
        if (try_match_kona_reference(&result->scan_lab, result))
        {
            result->description = nullptr;
            TCS_LOGD(TAG, "Kona match: id=%u name=%s dE=%.3f",
                     (unsigned int)result->kona_id,
                     result->color_name,
                     result->delta_e);
        }
        else
        {
            // Use material-corrected Lab for fallback database matching.
            // The general color database contains theoretical/reference Lab values,
            // so material correction helps compensate for measurement bias.
            // If material correction is disabled, corrected_lab equals scan_lab.
            result->color_name = color_matcher_find_closest(&result->corrected_lab, &result->delta_e);
            if (result->color_name)
            {
                // Description is generated on-demand via color_description_generate()
                result->description = nullptr;
                if (result->delta_e < 1.0f)
                {
                    result->confidence = 1.0f;
                }
                else if (result->delta_e < s_config.max_delta_e)
                {
                    result->confidence = 1.0f - (result->delta_e / s_config.max_delta_e);
                }
                else
                {
                    result->confidence = 0.0f;
                }
            }
            else
            {
                result->color_name = COLOR_NAME_UNKNOWN;
                result->description = COLOR_DESC_NO_MATCH;
            }
        }

        // === CONFIDENCE-SCALED SATURATION BOOST ===
        // Reduce the saturation boost for low-confidence matches to prevent over-boosting
        // uncertain readings (e.g., from textured surfaces) into an incorrect vivid color name.
        // At confidence=1.0 the full boost is applied; at confidence=0.0 no boost is applied.
        // Uses s_config.saturation_boost (same source as the initial boost above) to ensure
        // consistent behavior regardless of whether auto-calibration loaded different params.
        float confidence_factor = fmaxf(0.0f, fminf(1.0f, result->confidence));
        float effective_boost = 1.0f + (s_config.saturation_boost - 1.0f) * confidence_factor;
        if (effective_boost < s_config.saturation_boost - 0.01f)
        {
            // Restore pre-boost a*/b* and reapply with the lower effective boost.
            result->lab.a = pre_boost_a;
            result->lab.b = pre_boost_b;
            enhanced_chroma = color_math_enhance_saturation(&result->lab,
                              s_config.gray_threshold,
                              s_config.color_threshold,
                              effective_boost);
            result->saturation = enhanced_chroma / 100.0f;
            result->lab.a = fminf(fmaxf(result->lab.a, -110.0f), 110.0f);
            result->lab.b = fminf(fmaxf(result->lab.b, -110.0f), 110.0f);
            TCS_LOGD(TAG,
                     "Confidence-scaled boost: confidence=%.2f factor=%.2f boost=%.2f->%.2f",
                     result->confidence, confidence_factor,
                     s_config.saturation_boost, effective_boost);
        }
    }

    // Clear cached sensor reading and variance after use to avoid stale data in next call
    s_has_sensor_reading = false;
    s_has_capture_variance = false;

    color_math_lab_to_rgb(result->lab, &result->rgb[0], &result->rgb[1], &result->rgb[2]);
    return ESP_OK;
}

const char* color_pipeline_get_category(const lab_t* lab)
{
    if (!lab)
    {
        return "Unknown";
    }

    // Check for grayscale (low chroma)
    float C = color_math_chroma(lab);

    if (C < 10.0f)
    {
        // Low chroma = achromatic
        if (lab->l < 20.0f)
        {
            return "Black";
        }
        if (lab->l < 40.0f)
        {
            return "Dark Gray";
        }
        if (lab->l < 60.0f)
        {
            return "Gray";
        }
        if (lab->l < 80.0f)
        {
            return "Light Gray";
        }
        return "White";
    }

    // Calculate hue angle
    float H = atan2f(lab->b, lab->a) * (180.0f / M_PI);
    if (H < 0)
    {
        H += 360.0f;
    }

    // Brown detection: low lightness + warm hue (orange/yellow range) + moderate chroma
    if (lab->l < BROWN_MAX_LIGHTNESS && C < BROWN_MAX_CHROMA &&
        H >= BROWN_HUE_MIN && H < BROWN_HUE_MAX)
    {
        return "Brown";
    }

    // Find category by hue
    for (size_t i = 0; i < sizeof(CATEGORIES)/sizeof(CATEGORIES[0]); i++)
    {
        if (H >= CATEGORIES[i].hue_min && H < CATEGORIES[i].hue_max)
        {
            return CATEGORIES[i].name;
        }
    }

    return "Unknown";
}

int color_pipeline_describe(const color_result_t* result, char* buffer, size_t buffer_size)
{
    if (!result || !buffer || buffer_size == 0)
    {
        return 0;
    }

    int len = 0;

    // Prepend flicker warning if detected
    if (result->flicker_detected)
    {
        len = snprintf(buffer, buffer_size,
                       "Light flickering detected. Result may be inaccurate. ");
        if (len >= (int)buffer_size)
        {
            return len;
        }
    }

    // Handle error conditions first
    if (result->low_light)
    {
        len += snprintf(buffer + len, buffer_size - len,
                        "It's too dark to identify the color accurately. "
                        "Please try with more light.");
        return len;
    }

    if (result->saturated)
    {
        len += snprintf(buffer + len, buffer_size - len,
                        "The light is too bright. "
                        "Please move further from the light source.");
        return len;
    }

    // Build description based on confidence and match type.
    // Kona matches always announce the swatch name since the match already
    // passed the ΔE acceptance threshold — the name is meaningful even at
    // lower confidence levels.  For non-Kona (color-database) matches, fall
    // back to the generic category when confidence is too low.
    if (!result->kona_matched &&
        result->confidence <= CONFIDENCE_MEDIUM && result->category &&
        result->category[0] != '\0')
    {
        len += snprintf(buffer + len, buffer_size - len,
                        "This appears to be a shade of %s. ", result->category);
    }
    else
    {
        const char* prefix = (result->confidence > CONFIDENCE_HIGH) ? "This is " :
                             (result->confidence > CONFIDENCE_MEDIUM) ? "This looks like " :
                             "This might be ";

        len += snprintf(buffer + len, buffer_size - len, "%s%s. ", prefix, result->color_name);

        // Add description if available
        if (result->description && len < (int)buffer_size - 1)
        {
            len += snprintf(buffer + len, buffer_size - len, "%s ", result->description);
        }
    }

    // Add lightness and saturation qualifiers in one pass
    const char* quality = (result->lab.l < 30.0f) ? "It's quite dark. " :
                          (result->lab.l > 80.0f) ? "It's quite light. " :
                          (result->saturation > 0.8f) ? "It's very vivid and saturated. " :
                          (result->saturation > 0.1f && result->saturation < 0.3f) ? "It's a muted, grayish tone. " : "";

    if (quality[0] && len < (int)buffer_size - 1)
    {
        len += snprintf(buffer + len, buffer_size - len, "%s", quality);
    }

    return len;
}

esp_err_t color_pipeline_set_params(const color_calib_params_t* params)
{
    if (!params) return ESP_ERR_INVALID_ARG;
    s_params = *params;

    // Clear any constant-offset bias in PCCM term[9] (see NVS load path for
    // full explanation).  This covers the path where calibration is applied
    // directly (e.g., from auto_calibrate) rather than loaded from NVS.
    for (int i = 0; i < 3; i++)
    {
        s_params.pccm[i][9] = 0.0f;
    }

    // Update config flag based on params (for backward compatibility)
    s_config.has_black_calibration = params->has_black_calibration;
    
    if (params->has_black_calibration)
    {
        ESP_LOGI(TAG, "Pipeline parameters updated (with black level calibration)");
    }
    else
    {
        ESP_LOGI(TAG, "Pipeline parameters updated (no black level calibration)");
    }
    
    return ESP_OK;
}

esp_err_t color_pipeline_set_material(material_type_t material)
{
    // Only FABRIC, PLASTIC, METAL, or DEFAULT are valid assumed materials
    // UNKNOWN should not be explicitly set as the assumed material
    if (material < MATERIAL_FABRIC || material > MATERIAL_DEFAULT)
    {
        return ESP_ERR_INVALID_ARG;
    }
    
    s_config.assumed_material = material;
    ESP_LOGI(TAG, "Material type set to: %s", color_math_material_name(material));
    
    return ESP_OK;
}

esp_err_t color_pipeline_set_material_auto_detect(bool enable)
{
    s_config.auto_detect_material = enable;
    ESP_LOGI(TAG, "Material auto-detection: %s", enable ? "enabled" : "disabled");
    
    return ESP_OK;
}

esp_err_t color_pipeline_set_material_correction(bool enable)
{
    s_config.use_material_correction = enable;
    ESP_LOGI(TAG, "Material correction: %s", enable ? "enabled" : "disabled");
    
    return ESP_OK;
}

esp_err_t color_pipeline_set_replay_flags(uint32_t flags)
{
    s_replay_flags = flags;
    if (flags)
    {
        ESP_LOGI(TAG, "Replay flags set: 0x%02x%s%s%s%s%s%s",
                 (unsigned)flags,
                 (flags & REPLAY_NO_AUTO_CAL)     ? " NO_AUTO_CAL"     : "",
                 (flags & REPLAY_NO_BLACK_CAL)    ? " NO_BLACK_CAL"    : "",
                 (flags & REPLAY_NO_D65_SCALE)    ? " NO_D65_SCALE"    : "",
                 (flags & REPLAY_NO_IR_COMP)      ? " NO_IR_COMP"      : "",
                 (flags & REPLAY_NO_MATERIAL_COR) ? " NO_MATERIAL_COR" : "",
                 (flags & REPLAY_BYPASS_PCCM)     ? " BYPASS_PCCM"     : "");
    }
    else
    {
        ESP_LOGI(TAG, "Replay flags cleared — normal pipeline behavior restored");
    }
    return ESP_OK;
}

uint32_t color_pipeline_get_replay_flags(void)
{
    return s_replay_flags;
}
