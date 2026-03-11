/**
 * @file color_pipeline.cpp
 * @brief Color Processing Pipeline Implementation
 */

#include "color_pipeline.h"
#include "color_math.h"
#include "tcs_glue.h"
#include <cstring>
#include <cstdio>
#include <cmath>

static const char* TAG = "color_pipe";

// Confidence thresholds for color description
static const float CONFIDENCE_HIGH = 0.8f;    //< High confidence - "This is..."
static const float CONFIDENCE_MEDIUM = 0.5f;  //< Medium confidence - "This looks like..."

// Luminance threshold below which a sample is considered "near-black" and eligible
// for a gain-boost retry to improve SNR in low-illumination conditions.
static const float NEAR_BLACK_LUMINANCE_THRESHOLD = 25.0f;

// Number of gain steps to boost during low-light / near-black retries.
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
    .use_white_balance = false,
    .white_reference_led = {D65_X, D65_Y, D65_Z},
    .white_reference_ambient = {D65_X, D65_Y, D65_Z},
    .black_level = {0.0f, 0.0f, 0.0f},
    .has_led_calibration = false,
    .has_ambient_calibration = false,
    .has_black_calibration = false,
    .num_samples = 3,
    .sample_delay_ms = 50,
    .gray_threshold = 5.0f,
    .color_threshold = 60.0f,
    .saturation_boost = 1.5f
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

// Statistics
static uint32_t s_total_identifications = 0;
static float s_total_processing_ms = 0.0f;
static float s_total_confidence = 0.0f;

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

static esp_err_t apply_sensor_correction(const sensor_reading_t* reading, xyz_t* xyz);

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

    float sum_x = 0.0f;
    float sum_y = 0.0f;
    float sum_z = 0.0f;
    float sum_x2 = 0.0f;
    float sum_y2 = 0.0f;
    float sum_z2 = 0.0f;

    sensor_reading_t last_reading = {};
    bool has_last = false;
    xyz_t best_low_signal_xyz = {};
    float best_low_signal_y = -1.0f;
    bool has_best_low_signal = false;

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

        sum_x += corrected_xyz.x;
        sum_y += corrected_xyz.y;
        sum_z += corrected_xyz.z;
        sum_x2 += corrected_xyz.x * corrected_xyz.x;
        sum_y2 += corrected_xyz.y * corrected_xyz.y;
        sum_z2 += corrected_xyz.z * corrected_xyz.z;
        stats->accepted_samples++;

        if (stats->accepted_samples == 1)
        {
            stats->gain_code = reading.gain;
            stats->integration_ms = reading.integration_ms;
            stats->status2 = reading.status2;
            stats->status6 = reading.status6;
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

        return ESP_ERR_INVALID_STATE;
    }

    const float n = static_cast<float>(stats->accepted_samples);
    stats->mean_xyz.x = sum_x / n;
    stats->mean_xyz.y = sum_y / n;
    stats->mean_xyz.z = sum_z / n;

    float var_x = (sum_x2 / n) - (stats->mean_xyz.x * stats->mean_xyz.x);
    float var_y = (sum_y2 / n) - (stats->mean_xyz.y * stats->mean_xyz.y);
    float var_z = (sum_z2 / n) - (stats->mean_xyz.z * stats->mean_xyz.z);

    stats->stddev_xyz.x = sqrtf(fmaxf(0.0f, var_x));
    stats->stddev_xyz.y = sqrtf(fmaxf(0.0f, var_y));
    stats->stddev_xyz.z = sqrtf(fmaxf(0.0f, var_z));

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
    if (s_params.has_black_calibration)
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

    // APPLY D65 SCALING - Scale to D65 white point before PCCM
    // PCCM was calibrated with D65-normalized inputs (0.95:1.00:1.08 ratio)
    // RESP normalization produces ~1:1:1, so we apply D65 scaling here
    TCS_LOGD(TAG, "D65 constants: X=%.3f Y=%.3f Z=%.3f", D65_X, D65_Y, D65_Z);
    TCS_LOGD(TAG, "D65 scaling factors: X/100=%.3f Y/100=%.3f Z/100=%.3f", 
             D65_X / 100.0f, D65_Y / 100.0f, D65_Z / 100.0f);
    
    x_in *= (D65_X / 100.0f);
    y_in *= (D65_Y / 100.0f);
    z_in *= (D65_Z / 100.0f);
    
    TCS_LOGD(TAG, "After D65 scaling: x_in=%.3f y_in=%.3f z_in=%.3f", x_in, y_in, z_in);

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
    *xyz = color_math_apply_pccm(&xyz_in, s_params.pccm);

    // Soft floor for Z prevents total blue loss (essential when using subtraction matrices)
    xyz->z = fmaxf(MIN_Z_VALUE, xyz->z);

    // Scale to output range
    xyz->x *= XYZ_OUTPUT_SCALE;
    xyz->y *= XYZ_OUTPUT_SCALE;
    xyz->z *= XYZ_OUTPUT_SCALE;

    TCS_LOGD(TAG, "SensorCorr: after PCCM+scale: X=%.4f Y=%.4f Z=%.4f",
             xyz->x, xyz->y, xyz->z);

    // IR COMPENSATION
    if (reading->ir > 0 && reading->clear > 0)
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

    TCS_LOGD(TAG, "After CCM+IR: X=%.1f Y=%.1f Z=%.1f", xyz->x, xyz->y, xyz->z);

    return ESP_OK;
}

esp_err_t color_pipeline_init(const color_pipeline_config_t* config)
{
    if (config)
    {
        s_config = *config;
    }

    color_database_init();
    if (color_matcher_init() != ESP_OK)
    {
        ESP_LOGE(TAG, "Failed to initialize color matcher");
        return ESP_FAIL;
    }

    // Attempt to load auto-calibration params
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

    uint64_t start_time = tcs_time_us();
    memset(result, 0, sizeof(color_result_t));

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

    s_total_identifications++;
    s_total_processing_ms += (tcs_time_us() - start_time) / 1000.0f;
    s_total_confidence += result->confidence;

    return ret;
}

#ifdef ESP_PLATFORM
esp_err_t color_pipeline_capture_averaged(TCS3530* sensor,
                                          bool led_enabled,
                                          color_capture_stats_t* stats)
{
    return capture_averaged_xyz(sensor, led_enabled, stats);
}

esp_err_t color_pipeline_capture_csv(TCS3530* sensor,
                                     bool led_enabled,
                                     const char* swatch_id,
                                     const char* swatch_name,
                                     color_result_t* result)
{
    if (!sensor)
    {
        return ESP_ERR_INVALID_ARG;
    }

    color_capture_stats_t stats = {};
    esp_err_t ret = color_pipeline_capture_averaged(sensor, led_enabled, &stats);
    if (ret != ESP_OK)
    {
        return ret;
    }

    color_result_t local_result = {};
    color_result_t* out = result ? result : &local_result;
    out->saturated = stats.any_saturated;
    out->flicker_detected = stats.flicker_detected;
    out->xyz = stats.mean_xyz;
    clamp_xyz_floor(&out->xyz);
    out->timestamp_us = tcs_time_us();

    ret = color_pipeline_process_xyz(&out->xyz, led_enabled, out);
    if (ret != ESP_OK)
    {
        return ret;
    }

    // CSV column order:
    // swatch_id,swatch_name,led_enabled,gain_code,integration_ms,status2,status6,
    // requested_samples,accepted_samples,rejected_saturated,rejected_low_signal,
    // mean_x,mean_y,mean_z,stddev_x,stddev_y,stddev_z,mean_L,mean_a,mean_b,timestamp_us
    ESP_LOGI("KONA_SCAN_CSV",
             "%s,%s,%u,%u,%u,0x%02X,0x%02X,%u,%u,%u,%u,%.4f,%.4f,%.4f,%.4f,%.4f,%.4f,%.4f,%.4f,%.4f,%llu",
             swatch_id ? swatch_id : "",
             swatch_name ? swatch_name : "",
             (unsigned int)led_enabled,
             (unsigned int)stats.gain_code,
             (unsigned int)stats.integration_ms,
             stats.status2,
             stats.status6,
             (unsigned int)stats.requested_samples,
             (unsigned int)stats.accepted_samples,
             (unsigned int)stats.rejected_saturated,
             (unsigned int)stats.rejected_low_signal,
             stats.mean_xyz.x,
             stats.mean_xyz.y,
             stats.mean_xyz.z,
             stats.stddev_xyz.x,
             stats.stddev_xyz.y,
             stats.stddev_xyz.z,
             stats.mean_lab.l,
             stats.mean_lab.a,
             stats.mean_lab.b,
             (unsigned long long)out->timestamp_us);

    return ESP_OK;
}

esp_err_t color_pipeline_identify(TCS3530* sensor, color_result_t* result)
{
    if (!sensor || !result)
    {
        return ESP_ERR_INVALID_ARG;
    }

    uint64_t start_time = tcs_time_us();
    memset(result, 0, sizeof(color_result_t));

    bool led_enabled = false;
    sensor->getLed(&led_enabled);

    color_capture_stats_t capture_stats = {};
    esp_err_t ret = color_pipeline_capture_averaged(sensor, led_enabled, &capture_stats);
    if (ret != ESP_OK)
    {
        return ret;
    }

    result->saturated = capture_stats.any_saturated;
    result->flicker_detected = capture_stats.flicker_detected;
    result->xyz = capture_stats.mean_xyz;
    clamp_xyz_floor(&result->xyz);
    result->timestamp_us = tcs_time_us();

    ESP_LOGI(TAG, "Measure XYZ (IR+Black Cor): X=%.2f Y=%.2f Z=%.2f",
             result->xyz.x, result->xyz.y, result->xyz.z);

    ret = color_pipeline_process_xyz(&result->xyz, led_enabled, result);

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

    // Retry with higher gain for near-black samples that are above the low_light threshold.
    // When LED illumination is insufficient, dark-but-colored fabrics may appear featureless
    // black because the color signal is buried in noise. Boosting gain amplifies the signal
    // above the sensor noise floor, improving color discrimination for dark surfaces.
    if (ret == ESP_OK && !result->low_light && !result->saturated &&
        result->luminance < NEAR_BLACK_LUMINANCE_THRESHOLD)
    {
        TCS3530Gain original_gain = sensor->gain();
        uint8_t gain_code = static_cast<uint8_t>(original_gain);
        uint8_t retry_gain_code = gain_code + DARK_RETRY_GAIN_BOOST_STEPS;
        if (retry_gain_code > TCS3530_MAX_GAIN_CODE)
        {
            retry_gain_code = TCS3530_MAX_GAIN_CODE;
        }

        if (retry_gain_code > gain_code)
        {
            TCS3530Gain retry_gain = static_cast<TCS3530Gain>(retry_gain_code);
            ESP_LOGI(TAG, "Near-black retry: luminance=%.1f < %.1f, boosting gain from %.0fx to %.0fx",
                     result->luminance, NEAR_BLACK_LUMINANCE_THRESHOLD,
                     tcs3530_gain_code_to_multiplier(gain_code),
                     tcs3530_gain_code_to_multiplier(retry_gain_code));

            esp_err_t set_ret = sensor->setGain(retry_gain);
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
                    // Accept the gain-boosted result if it is not saturated. Even if still near-black,
                    // the boosted measurement has better SNR and will give more accurate color hue.
                    if (set_ret == ESP_OK && !retry_result.saturated)
                    {
                        *result = retry_result;
                    }
                }

                esp_err_t restore_ret = sensor->setGain(original_gain);
                if (restore_ret != ESP_OK)
                {
                    ESP_LOGW(TAG, "Failed to restore gain: %s", esp_err_to_name(restore_ret));
                }
            }
        }
    }

    // Stats update
    s_total_identifications++;
    s_total_processing_ms += (tcs_time_us() - start_time) / 1000.0f;
    s_total_confidence += result->confidence;

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
    result->lab = color_math_xyz_to_lab(corrected);

        
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

    float enhanced_chroma = color_math_enhance_saturation(&result->lab,
                            s_params.gray_threshold,
                            s_params.color_threshold,
                            s_params.saturation_boost);

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
        result->color_name = color_matcher_find_closest(&result->lab, &result->delta_e);
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

    // Build description based on confidence
    const char* prefix = (result->confidence > CONFIDENCE_HIGH) ? "This is " :
                         (result->confidence > CONFIDENCE_MEDIUM) ? "This looks like " :
                         "This might be ";

    len += snprintf(buffer + len, buffer_size - len, "%s%s. ", prefix, result->color_name);

    // Add description if available
    if (result->description && len < (int)buffer_size - 1)
    {
        len += snprintf(buffer + len, buffer_size - len, "%s ", result->description);
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

void color_pipeline_get_stats(uint32_t* total_identifications,
                              float* avg_processing_ms,
                              float* avg_confidence)
{
    if (total_identifications)
    {
        *total_identifications = s_total_identifications;
    }

    if (avg_processing_ms && s_total_identifications > 0)
    {
        *avg_processing_ms = s_total_processing_ms / s_total_identifications;
    }

    if (avg_confidence && s_total_identifications > 0)
    {
        *avg_confidence = s_total_confidence / s_total_identifications;
    }
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
