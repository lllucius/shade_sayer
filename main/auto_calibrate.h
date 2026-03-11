/**
 * @file auto_calibrate.h
 * @brief Automatic Color Sensor Calibration System
 *
 * This module provides an automated calibration system for the TCS3530 color sensor
 * that uses gradient descent optimization to determine optimal calibration parameters
 * based on measurements of reference colors.
 *
 * The system guides users through measuring a set of reference colors, then automatically
 * optimizes the Color Correction Matrix (CCM), lightness correction, and saturation
 * boost parameters to minimize color error.
 *
 * Key Features:
 * - Variable reference color support (2-32 colors)
 * - Preset reference sets (RGBW, RGBWY, Extended, X-Rite ColorChecker)
 * - Custom reference colors by name + Lab or RGB values
 * - State machine for user guidance through measurement process
 * - Gradient descent optimization with constraints
 * - Convergence detection (target ΔE or improvement stall)
 * - NVS persistence for calibration parameters
 * - Callback integration for TTS/UI feedback
 */

#ifndef AUTO_CALIBRATE_H
#define AUTO_CALIBRATE_H

#include <stdint.h>
#include <stdbool.h>
#include "tcs_glue.h"
#include "color_types.h"

#ifdef __cplusplus
extern "C" {
#endif

//===========================================================================
// CONSTANTS
//===========================================================================

/** Maximum number of reference colors supported */
#define CAL_MAX_REFERENCES 32

/** Minimum number of reference colors required */
#define CAL_MIN_REFERENCES 2

/** Maximum name length for reference colors */
#define CAL_MAX_NAME_LENGTH 32

/** Default NVS namespace for calibration data */
#define CAL_NVS_NAMESPACE "auto_cal"

//===========================================================================
// REFERENCE COLOR FLAGS
//===========================================================================

/** This reference is the white reference (used for white balance) */
#define CAL_REF_FLAG_IS_WHITE   (1 << 0)

/** This reference is neutral (gray/black, low chroma) */
#define CAL_REF_FLAG_IS_NEUTRAL (1 << 1)

/** This reference must be measured (vs optional) */
#define CAL_REF_FLAG_REQUIRED   (1 << 2)

/** This reference is the black reference (used for black level offset) */
#define CAL_REF_FLAG_IS_BLACK   (1 << 3)

/** This reference is a gray (used for lightness/gamma calibration) */
#define CAL_REF_FLAG_GRAY       (1 << 4)

/** This reference is a dark chromatic color (lower optimizer weight to avoid distorting bright-color calibration) */
#define CAL_REF_FLAG_DARK_CHROMATIC (1 << 5)

//===========================================================================
// TYPES
//===========================================================================

/**
 * @brief Reference color for calibration
 *
 * Defines a target color that will be measured during calibration.
 * The system compares measured XYZ values against target Lab values
 * to compute calibration error.
 */
typedef struct
{
    char name[CAL_MAX_NAME_LENGTH];  ///< Human-readable name (e.g., "White", "Red")
    lab_t target_lab;                 ///< Target Lab color value
    xyz_t measured_xyz;               ///< Measured XYZ values (filled during calibration)
    uint8_t flags;                    ///< Combination of CAL_REF_FLAG_* bits
    bool measured;                    ///< True if measurement was taken
} cal_reference_t;

// Forward declare the calibration context structure
typedef struct auto_cal_ctx auto_cal_ctx_t;

/**
 * @brief Calibration parameter constraints
 *
 * Defines min/max bounds for each parameter to prevent divergence
 * during gradient descent optimization.
 */
typedef struct
{
    // CCM constraints
    float ccm_diag_min;         ///< Minimum diagonal element (e.g., 0.7)
    float ccm_diag_max;         ///< Maximum diagonal element (e.g., 2.2)
    float ccm_off_diag_min;     ///< Minimum off-diagonal (e.g., -0.4)
    float ccm_off_diag_max;     ///< Maximum off-diagonal (e.g., 0.4)

    // Lightness constraints
    float lightness_scale_min;  ///< Minimum scale (e.g., 0.85)
    float lightness_scale_max;  ///< Maximum scale (e.g., 1.05)
    float lightness_offset_min; ///< Minimum offset (e.g., -10.0)
    float lightness_offset_max; ///< Maximum offset (e.g., 10.0)
    float lightness_gamma_min;  ///< Minimum gamma (e.g., 0.8)
    float lightness_gamma_max;  ///< Maximum gamma (e.g., 1.5)
    
    // Piecewise lightness gamma constraints
    float lightness_gamma_dark_min;      ///< Minimum dark gamma (e.g., 0.5)
    float lightness_gamma_dark_max;      ///< Maximum dark gamma (e.g., 2.0)
    float lightness_gamma_light_min;     ///< Minimum light gamma (e.g., 0.5)
    float lightness_gamma_light_max;     ///< Maximum light gamma (e.g., 2.0)
    float lightness_transition_min;      ///< Minimum transition point (e.g., 20.0)
    float lightness_transition_max;      ///< Maximum transition point (e.g., 60.0)

    // Saturation constraints
    float saturation_min;       ///< Minimum boost (e.g., 1.0)
    float saturation_max;       ///< Maximum boost (e.g., 2.0)

    // Blue correction constraints
    float blue_correction_magnitude_min;
    float blue_correction_magnitude_max;
    float blue_correction_center_min;
    float blue_correction_center_max;
    float blue_correction_width_min;
    float blue_correction_width_max;
} cal_constraints_t;

/**
 * @brief Calibration state
 */
typedef enum
{
    CAL_STATE_IDLE,              ///< Not calibrating
    CAL_STATE_MEASURING,         ///< Collecting reference measurements
    CAL_STATE_OPTIMIZING,        ///< Running gradient descent
    CAL_STATE_COMPLETE,          ///< Calibration finished successfully
    CAL_STATE_ERROR              ///< Error occurred
} cal_state_t;

/**
 * @brief Calibration status information
 */
typedef struct
{
    cal_state_t state;           ///< Current state
    int current_ref_index;       ///< Index of reference being measured (-1 if none)
    int total_references;        ///< Total number of references
    int measurements_collected;  ///< Number of measurements collected so far
    
    // Optimization progress
    int iteration;               ///< Current optimization iteration
    float current_error;         ///< Current average ΔE error
    float initial_error;         ///< Initial error before optimization
    float best_error;            ///< Best error achieved so far
    
    // Convergence info
    bool converged;              ///< True if convergence criteria met
    int stall_count;             ///< Number of iterations without improvement
} cal_status_t;

/**
 * @brief Calibration callback function
 *
 * Called at various points during calibration to provide user feedback.
 *
 * @param status Current calibration status
 * @param message Human-readable message (may be NULL)
 * @param user_data User data pointer passed to auto_cal_set_callback()
 */
typedef void (*cal_callback_t)(const cal_status_t* status, const char* message, void* user_data);

/**
 * @brief Calibration context (opaque)
 *
 * Internal structure - do not access members directly.
 * Use API functions to interact with calibration context.
 */
typedef struct auto_cal_ctx auto_cal_ctx_t;

//===========================================================================
// PRESET REFERENCE COLORS
//===========================================================================

/** Default preset (6 colors): User's available swatches */
extern const cal_reference_t CAL_PRESET_DEFAULT[];
extern const int CAL_PRESET_DEFAULT_COUNT;

/** RGBW preset (4 colors): White, Red, Green, Blue */
extern const cal_reference_t CAL_PRESET_RGBW[];
extern const int CAL_PRESET_RGBW_COUNT;

/** RGBWY preset (5 colors): White, Red, Green, Blue, Yellow */
extern const cal_reference_t CAL_PRESET_RGBWY[];
extern const int CAL_PRESET_RGBWY_COUNT;

/** Extended preset (8 colors): Adds Cyan, Magenta, Black to RGBWY */
extern const cal_reference_t CAL_PRESET_EXTENDED[];
extern const int CAL_PRESET_EXTENDED_COUNT;

/** X-Rite ColorChecker Classic (24 patches) */
extern const cal_reference_t CAL_PRESET_COLORCHECKER[];
extern const int CAL_PRESET_COLORCHECKER_COUNT;

//===========================================================================
// API FUNCTIONS
//===========================================================================

/**
 * @brief Initialize calibration context
 *
 * Allocates and initializes a new calibration context with default parameters.
 *
 * @param ctx Pointer to receive context handle
 * @return ESP_OK on success, error code otherwise
 */
esp_err_t auto_cal_init(auto_cal_ctx_t** ctx);

/**
 * @brief Deinitialize and free calibration context
 *
 * @param ctx Context to free (set to NULL after freeing)
 */
void auto_cal_deinit(auto_cal_ctx_t** ctx);

/**
 * @brief Set reference colors from a preset
 *
 * Replaces current references with a preset array.
 *
 * @param ctx Calibration context
 * @param preset Preset reference array (e.g., CAL_PRESET_RGBW)
 * @param count Number of references in preset
 * @return ESP_OK on success, error code otherwise
 */
esp_err_t auto_cal_set_references_preset(auto_cal_ctx_t* ctx, const cal_reference_t* preset, int count);

/**
 * @brief Set custom reference colors
 *
 * Replaces current references with a custom array.
 *
 * @param ctx Calibration context
 * @param references Array of reference colors
 * @param count Number of references (CAL_MIN_REFERENCES to CAL_MAX_REFERENCES)
 * @return ESP_OK on success, error code otherwise
 */
esp_err_t auto_cal_set_references(auto_cal_ctx_t* ctx, const cal_reference_t* references, int count);

/**
 * @brief Add a reference color by Lab values
 *
 * @param ctx Calibration context
 * @param name Reference color name
 * @param target_lab Target Lab color
 * @param flags Combination of CAL_REF_FLAG_* bits
 * @return Index of added reference, or -1 on error
 */
int auto_cal_add_reference(auto_cal_ctx_t* ctx, const char* name, const lab_t* target_lab, uint8_t flags);

/**
 * @brief Add a reference color by RGB values
 *
 * Converts RGB to Lab internally using standard sRGB->XYZ->Lab conversion.
 *
 * @param ctx Calibration context
 * @param name Reference color name
 * @param r Red component (0-255)
 * @param g Green component (0-255)
 * @param b Blue component (0-255)
 * @param flags Combination of CAL_REF_FLAG_* bits
 * @return Index of added reference, or -1 on error
 */
int auto_cal_add_reference_rgb(auto_cal_ctx_t* ctx, const char* name, uint8_t r, uint8_t g, uint8_t b, uint8_t flags);

/**
 * @brief Clear all reference colors
 *
 * @param ctx Calibration context
 */
void auto_cal_clear_references(auto_cal_ctx_t* ctx);

/**
 * @brief Set callback for calibration events
 *
 * @param ctx Calibration context
 * @param cb Callback function (or NULL to disable)
 * @param user_data User data pointer passed to callback
 */
void auto_cal_set_callback(auto_cal_ctx_t* ctx, cal_callback_t cb, void* user_data);

/**
 * @brief Start calibration process
 *
 * Begins the calibration state machine. The system will guide the user
 * through measuring each reference color. After all measurements are
 * collected, call auto_cal_run_optimization().
 *
 * @param ctx Calibration context
 * @return ESP_OK on success, error code otherwise
 */
esp_err_t auto_cal_start(auto_cal_ctx_t* ctx);

/**
 * @brief Submit a measurement for the current reference color
 *
 * Call this after the user has placed the current reference color and
 * the sensor has taken a measurement.
 *
 * @param ctx Calibration context
 * @param xyz Measured XYZ values from sensor
 * @return ESP_OK on success, error code otherwise
 */
esp_err_t auto_cal_submit_measurement(auto_cal_ctx_t* ctx, const xyz_t* xyz);

/**
 * @brief Skip the current reference color
 *
 * Advances to the next reference without recording a measurement.
 * Only works for non-required references.
 *
 * @param ctx Calibration context
 * @return ESP_OK on success, ESP_ERR_INVALID_STATE if reference is required
 */
esp_err_t auto_cal_skip_current(auto_cal_ctx_t* ctx);

/**
 * @brief Run gradient descent optimization
 *
 * Optimizes calibration parameters based on collected measurements.
 * This is a blocking operation that may take 2-5 seconds.
 *
 * @param ctx Calibration context
 * @return ESP_OK on success, error code otherwise
 */
esp_err_t auto_cal_run_optimization(auto_cal_ctx_t* ctx);

/**
 * @brief Get current calibration status
 *
 * @param ctx Calibration context
 * @return Pointer to status structure (valid until next API call)
 */
const cal_status_t* auto_cal_get_status(auto_cal_ctx_t* ctx);

/**
 * @brief Get name of current reference being measured
 *
 * @param ctx Calibration context
 * @return Reference name, or NULL if not measuring
 */
const char* auto_cal_get_current_ref_name(auto_cal_ctx_t* ctx);

/**
 * @brief Get current reference structure
 *
 * @param ctx Calibration context
 * @return Pointer to current reference, or NULL if not measuring
 */
const cal_reference_t* auto_cal_get_current_ref(auto_cal_ctx_t* ctx);

/**
 * @brief Get optimized calibration parameters
 *
 * @param ctx Calibration context
 * @return Pointer to parameters (valid until next API call or context freed)
 */
const color_calib_params_t* auto_cal_get_params(auto_cal_ctx_t* ctx);

/**
 * @brief Apply calibration results to color pipeline
 *
 * Updates the global color pipeline configuration with optimized parameters.
 * This makes the calibration active for future color measurements.
 *
 * @param ctx Calibration context
 * @return ESP_OK on success, error code otherwise
 */
esp_err_t auto_cal_apply_results(auto_cal_ctx_t* ctx);

/**
 * @brief Save calibration to NVS
 *
 * @param ctx Calibration context
 * @param ns NVS namespace (NULL for default CAL_NVS_NAMESPACE)
 * @return ESP_OK on success, error code otherwise
 */
esp_err_t auto_cal_save_to_nvs(auto_cal_ctx_t* ctx, const char* ns);

/**
 * @brief Load calibration from NVS
 *
 * @param ctx Calibration context
 * @param ns NVS namespace (NULL for default CAL_NVS_NAMESPACE)
 * @return ESP_OK on success, ESP_ERR_NOT_FOUND if no calibration saved
 */
esp_err_t auto_cal_load_from_nvs(auto_cal_ctx_t* ctx, const char* ns);

/**
 * @brief Reset parameters to default values
 *
 * @param ctx Calibration context
 */
esp_err_t auto_cal_submit_measurements_from_file(auto_cal_ctx_t* ctx, const char* path);
esp_err_t auto_cal_submit_raw_measurements_from_file(auto_cal_ctx_t* ctx, const char* path);

void auto_cal_reset_defaults(auto_cal_ctx_t* ctx);

/**
 * @brief Get human-readable name for a calibration state
 *
 * @param state Calibration state
 * @return String representation of state
 */
const char* auto_cal_state_name(cal_state_t state);

#ifdef __cplusplus
}
#endif

#endif // AUTO_CALIBRATE_H
