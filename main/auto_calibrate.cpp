/**
 * @file auto_calibrate.cpp
 * @brief Automatic color sensor calibration implementation.
 *
 * The calibration routine alternates between:
 * 1) collecting XYZ measurements for a fixed set of reference swatches, and
 * 2) running constrained optimization to reduce average CIEDE2000 error.
 *
 * Parameters are persisted to NVS and consumed by color_pipeline at runtime.
 */

#include "auto_calibrate.h"
#include "color_math.h"
#include "color_pipeline.h"
#include "tcs_glue.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

static const char* TAG = "auto_cal";

//===========================================================================
// CONSTANTS
//===========================================================================

/** Maximum number of optimization iterations */
#define MAX_ITERATIONS 500              // Increased from 200 to ensure full convergence

/** Target average ΔE for convergence */
#define TARGET_DELTA_E 3.0f             // Aggressive target

/** Minimum improvement to continue (ΔE reduction per iteration) */
#define MIN_IMPROVEMENT 1.0e-5f         // 0.00001 - finer resolution for final polish

/** Consecutive sub-MIN_IMPROVEMENT iterations required before declaring convergence.
 *  Adam is momentum-based: single near-zero-improvement iterations are routine
 *  while the moment estimates warm up or when traversing a plateau, so breaking
 *  on the FIRST flat iteration would abort spuriously and bypass the
 *  stall/warm-restart machinery. */
#define MAX_FLAT_ITERATIONS 25

/** Maximum iterations without improvement before triggering a warm restart */
#define MAX_STALL_ITERATIONS 100        // Enough patience for Adam to warm up and converge

/** Number of warm restarts allowed after stall (each restarts Adam with α/5 from best params) */
#define MAX_WARM_RESTARTS 1

/** Adam optimizer step size */
#define ADAM_ALPHA  0.001f

/** Learning rate used after a warm restart (finer search from best-found params) */
#define ADAM_ALPHA_RESTART  (ADAM_ALPHA / 5.0f)

/** Learning-rate multiplier for scalar (non-PCCM) parameters.
 *  Scalar params (offset, gamma, transition, saturation) need to traverse much
 *  larger distances than PCCM terms — e.g. lightness_offset moves from −3 → −6
 *  while a CCM coefficient moves from 1.4 → 1.6.  Without this multiplier, Adam
 *  at α=0.001 cannot reach the global optimum in the iteration budget. */
#define SCALAR_LR_MULTIPLIER 5.0f

/** Adam first moment (momentum) decay factor */
#define ADAM_BETA1  0.9f

/** Adam second moment (RMS) decay factor */
#define ADAM_BETA2  0.999f

/** Adam numerical stability epsilon */
#define ADAM_EPS    1.0e-8f

/** Finite difference step for gradient estimation */
#define GRADIENT_EPSILON 0.001f

/** Guard against numerical gradient spikes near non-smooth objective regions. */
#define MAX_GRADIENT_MAGNITUDE 60.0f

/**
 * PCCM layout: each row has 10 terms [R, G, B, R², G², B², RG, RB, GB, const].
 * Index 9 is the constant/bias term, which is always forced to 0 by apply_constraints.
 */
#define PCCM_TERMS          10
#define PCCM_CONSTANT_INDEX  9

/** Number of scalar (non-PCCM) parameters optimized by Adam */
#define NON_PCCM_PARAM_COUNT 10

/** Weight multiplier for gray references in error calculation */
#define GRAY_REFERENCE_WEIGHT 1.0f

/** Weight for the white reference — elevated so the optimizer cannot sacrifice white
 *  accuracy to gain on chromatic references.  White defines the exposure level for the
 *  whole pipeline; if it clips or drifts the entire lightness mapping is wrong.
 *  1.5× is a balanced choice: 1.0× let white drift to ΔE=21 (sacrificed), 2.0× over-constrained
 *  white and blocked the PCCM from correcting Cyan's a* sign (wrong Z→Y cross-term). */
#define WHITE_REFERENCE_WEIGHT 1.5f

/** Weight for bright saturated color references (high chroma) */
#define SATURATED_REFERENCE_WEIGHT 1.5f

/** Weight for dark chromatic references - lower than bright to avoid distorting bright-color calibration */
#define DARK_CHROMATIC_REFERENCE_WEIGHT 0.75f

/** Default weight for other references */
#define DEFAULT_REFERENCE_WEIGHT 1.0f


//===========================================================================
// DEFAULT CONSTRAINTS
//===========================================================================

static const cal_constraints_t DEFAULT_CONSTRAINTS =
{
    .ccm_diag_min = 0.5f,
    .ccm_diag_max = 3.0f,
    
    .ccm_off_diag_min = -2.0f,
    .ccm_off_diag_max = 2.0f,
    
    .lightness_scale_min = 0.01f,   
    .lightness_scale_max = 2.0f,
    
    .lightness_offset_min = -50.0f,
    .lightness_offset_max = 50.0f,
    .lightness_gamma_min = 0.5f,
    .lightness_gamma_max = 2.5f,
    
    // Piecewise gamma constraints
    .lightness_gamma_dark_min = 0.3f,      // Allow stronger dark correction
    .lightness_gamma_dark_max = 2.5f,
    .lightness_gamma_light_min = 0.5f,
    .lightness_gamma_light_max = 2.0f,
    .lightness_transition_min = 15.0f,     // Must be > 0 to keep piecewise gamma active; 0 disables it causing Gray 50 ΔE to blow up
    .lightness_transition_max = 90.0f,
    
    .saturation_min = 0.9f,         
    .saturation_max = 3.0f,
    
    // ADD THESE NEW CONSTRAINTS:
    .blue_correction_magnitude_min = -20.0f,
    .blue_correction_magnitude_max = 20.0f,
    .blue_correction_center_min = 20.0f,
    .blue_correction_center_max = 70.0f,
    .blue_correction_width_min = 5.0f,
    .blue_correction_width_max = 30.0f
};

//===========================================================================
// DEFAULT PARAMETERS
//===========================================================================

static const color_calib_params_t DEFAULT_PARAMS =
{
    // Initialize PCCM as a linear approximation (first 3 terms are linear CCM)
    // Terms: R, G, B, R², G², B², RG, RB, GB, 1
    .pccm = {
        // X channel
        {  1.40f, -0.20f, -0.20f,  0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f },
        // Y channel
        { -0.20f,  1.40f, -0.20f,  0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f },
        // Z channel
        { -0.20f, -0.20f,  1.40f,  0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f }
    },
    .lightness_scale = 0.13f,       // Fallback default; auto_cal_run_optimization estimates a better value
                                    // from the white measurement before the optimizer runs.
    .lightness_offset = -3.0f,      // Python simulation shows optimal ≈ −6.4; starting at −3 is halfway there
    .lightness_gamma = 0.8f,        // Start closer to the known good value (old successful calibration used 0.5)
    
    // Piecewise gamma parameters - enabled by default with dark/light split at L*=40.
    // Allows the optimizer to independently tune gamma for dark colors (L*<25) and
    // light colors (L*>55), which is critical when dark chromatic references are present.
    .lightness_gamma_dark = 1.5f,   // Python simulation shows optimal ≈ 2.5; starting at 1.5 is reachable by Adam
    .lightness_gamma_light = 0.85f, // Python simulation shows optimal ≈ 0.78
    .lightness_transition = 30.0f,  // Python simulation shows optimal ≈ 27.6
    
    .blue_correction_magnitude = -8.0f,
    .blue_correction_center = 43.0f,
    .blue_correction_width = 10.0f,
    .saturation_boost = 1.15f,      // Python simulation shows optimal ≈ 1.14
    
    .gray_threshold = 4.0f,
    .color_threshold = 60.0f,
    .black_level = {0.0f, 0.0f, 0.0f},
    .has_black_calibration = false
};

//===========================================================================
// PRESET REFERENCE COLORS
//===========================================================================

// Default preset (6 colors) - User's available swatches
const cal_reference_t CAL_PRESET_DEFAULT[] =
{
    {"White",  {95.0f,   0.0f,   0.0f}, {}, CAL_REF_FLAG_IS_WHITE | CAL_REF_FLAG_REQUIRED, false},
    {"Red",    {47.5f,  69.5f,  47.8f}, {}, CAL_REF_FLAG_REQUIRED, false},
    {"Green",  {57.5f, -59.5f,  37.1f}, {}, CAL_REF_FLAG_REQUIRED, false},
    {"Blue",   {30.5f,  24.9f, -55.9f}, {}, CAL_REF_FLAG_REQUIRED, false},
    {"Yellow", {89.6f,  -9.4f,  93.2f}, {}, CAL_REF_FLAG_REQUIRED, false},
    {"Black",  {26.5f,   0.0f,   0.0f}, {}, CAL_REF_FLAG_IS_NEUTRAL, false}
};
const int CAL_PRESET_DEFAULT_COUNT = sizeof(CAL_PRESET_DEFAULT) / sizeof(CAL_PRESET_DEFAULT[0]);

// RGBW preset (4 colors)
const cal_reference_t CAL_PRESET_RGBW[] =
{
    {"White", {100.0f, 0.0f, 0.0f}, {}, CAL_REF_FLAG_IS_WHITE | CAL_REF_FLAG_REQUIRED, false},
    {"Red",   {47.5f, 68.0f, 48.0f}, {}, CAL_REF_FLAG_REQUIRED, false},
    {"Green", {57.5f, -46.0f, 43.0f}, {}, CAL_REF_FLAG_REQUIRED, false},
    {"Blue",  {30.5f, 24.0f, -60.0f}, {}, CAL_REF_FLAG_REQUIRED, false}
};
const int CAL_PRESET_RGBW_COUNT = sizeof(CAL_PRESET_RGBW) / sizeof(CAL_PRESET_RGBW[0]);

// RGBWY preset (5 colors)
const cal_reference_t CAL_PRESET_RGBWY[] =
{
    {"White",  {100.0f, 0.0f, 0.0f}, {}, CAL_REF_FLAG_IS_WHITE | CAL_REF_FLAG_REQUIRED, false},
    {"Red",    {47.5f, 68.0f, 48.0f}, {}, CAL_REF_FLAG_REQUIRED, false},
    {"Green",  {57.5f, -46.0f, 43.0f}, {}, CAL_REF_FLAG_REQUIRED, false},
    {"Blue",   {30.5f, 24.0f, -60.0f}, {}, CAL_REF_FLAG_REQUIRED, false},
    {"Yellow", {89.0f, -5.0f, 93.0f}, {}, CAL_REF_FLAG_REQUIRED, false}
};
const int CAL_PRESET_RGBWY_COUNT = sizeof(CAL_PRESET_RGBWY) / sizeof(CAL_PRESET_RGBWY[0]);

// Extended preset (8 colors)
const cal_reference_t CAL_PRESET_EXTENDED[] =
{
    {"White",   {100.0f, 0.0f, 0.0f}, {}, CAL_REF_FLAG_IS_WHITE | CAL_REF_FLAG_REQUIRED, false},
    {"Red",     {47.5f, 68.0f, 48.0f}, {}, CAL_REF_FLAG_REQUIRED, false},
    {"Green",   {57.5f, -46.0f, 43.0f}, {}, CAL_REF_FLAG_REQUIRED, false},
    {"Blue",    {30.5f, 24.0f, -60.0f}, {}, CAL_REF_FLAG_REQUIRED, false},
    {"Yellow",  {89.0f, -5.0f, 93.0f}, {}, CAL_REF_FLAG_REQUIRED, false},
    {"Cyan",    {88.0f, -50.0f, -14.0f}, {}, CAL_REF_FLAG_REQUIRED, false},
    {"Magenta", {60.0f, 93.0f, -60.0f}, {}, CAL_REF_FLAG_REQUIRED, false},
    {"Black",   {0.0f, 0.0f, 0.0f}, {}, CAL_REF_FLAG_IS_NEUTRAL | CAL_REF_FLAG_REQUIRED, false}
};
const int CAL_PRESET_EXTENDED_COUNT = sizeof(CAL_PRESET_EXTENDED) / sizeof(CAL_PRESET_EXTENDED[0]);

// X-Rite ColorChecker Classic (24 patches) - subset for embedded use
const cal_reference_t CAL_PRESET_COLORCHECKER[] =
{
    {"Dark Skin",       {37.99f, 13.56f, 14.06f}, {}, CAL_REF_FLAG_REQUIRED, false},
    {"Light Skin",      {65.71f, 18.13f, 17.81f}, {}, CAL_REF_FLAG_REQUIRED, false},
    {"Blue Sky",        {49.93f, -4.88f, -21.93f}, {}, CAL_REF_FLAG_REQUIRED, false},
    {"Foliage",         {43.14f, -13.10f, 21.91f}, {}, CAL_REF_FLAG_REQUIRED, false},
    {"Blue Flower",     {55.11f, 8.84f, -25.40f}, {}, CAL_REF_FLAG_REQUIRED, false},
    {"Bluish Green",    {70.72f, -33.40f, -0.20f}, {}, CAL_REF_FLAG_REQUIRED, false},
    {"Orange",          {62.66f, 36.07f, 57.10f}, {}, CAL_REF_FLAG_REQUIRED, false},
    {"Purplish Blue",   {40.02f, 10.41f, -45.96f}, {}, CAL_REF_FLAG_REQUIRED, false},
    {"Moderate Red",    {51.12f, 48.24f, 16.25f}, {}, CAL_REF_FLAG_REQUIRED, false},
    {"Purple",          {30.33f, 22.98f, -21.59f}, {}, CAL_REF_FLAG_REQUIRED, false},
    {"Yellow Green",    {72.53f, -23.71f, 57.26f}, {}, CAL_REF_FLAG_REQUIRED, false},
    {"Orange Yellow",   {71.94f, 19.36f, 67.86f}, {}, CAL_REF_FLAG_REQUIRED, false},
    {"Blue",            {28.78f, 14.18f, -50.30f}, {}, CAL_REF_FLAG_REQUIRED, false},
    {"Green",           {55.26f, -38.34f, 31.37f}, {}, CAL_REF_FLAG_REQUIRED, false},
    {"Red",             {42.10f, 53.38f, 28.19f}, {}, CAL_REF_FLAG_REQUIRED, false},
    {"Yellow",          {81.73f, 4.04f, 79.82f}, {}, CAL_REF_FLAG_REQUIRED, false},
    {"Magenta",         {51.94f, 49.99f, -14.57f}, {}, CAL_REF_FLAG_REQUIRED, false},
    {"Cyan",            {51.04f, -28.63f, -28.64f}, {}, CAL_REF_FLAG_REQUIRED, false},
    {"White",           {96.54f, -0.43f, 1.19f}, {}, CAL_REF_FLAG_IS_WHITE | CAL_REF_FLAG_REQUIRED, false},
    {"Neutral 8",       {81.26f, -0.64f, -0.34f}, {}, CAL_REF_FLAG_IS_NEUTRAL, false},
    {"Neutral 6.5",     {66.77f, -0.73f, -0.50f}, {}, CAL_REF_FLAG_IS_NEUTRAL, false},
    {"Neutral 5",       {50.87f, -0.15f, -0.27f}, {}, CAL_REF_FLAG_IS_NEUTRAL, false},
    {"Neutral 3.5",     {35.66f, -0.42f, -1.23f}, {}, CAL_REF_FLAG_IS_NEUTRAL, false},
    {"Black",           {20.46f, -0.08f, -0.97f}, {}, CAL_REF_FLAG_IS_NEUTRAL | CAL_REF_FLAG_REQUIRED, false}
};
const int CAL_PRESET_COLORCHECKER_COUNT = sizeof(CAL_PRESET_COLORCHECKER) / sizeof(CAL_PRESET_COLORCHECKER[0]);

//===========================================================================
// MEASUREMENT STORAGE
//===========================================================================

/** Stored measurement for a reference color */
typedef struct
{
    xyz_t measured_xyz;     ///< Measured XYZ values
    lab_t target_lab;       ///< Target Lab values
    bool valid;             ///< True if measurement was taken
} cal_measurement_t;

//===========================================================================
// CONTEXT STRUCTURE
//===========================================================================

struct auto_cal_ctx
{
    // Configuration
    cal_reference_t references[CAL_MAX_REFERENCES];
    int num_references;
    cal_constraints_t constraints;
    
    // Callback (simplified - only message parameter used)
    cal_callback_t callback;
    
    // State
    cal_status_t status;
    
    // Measurements
    cal_measurement_t measurements[CAL_MAX_REFERENCES];
    
    // Parameters (current and best) - uses unified structure from color_types.h
    color_calib_params_t params;
    color_calib_params_t best_params;
    
    // Adam optimizer state
    float adam_m_pccm[3][10];  ///< First moment (exp. moving avg. of gradients) for PCCM
    float adam_v_pccm[3][10];  ///< Second moment (exp. moving avg. of squared gradients) for PCCM
    float adam_m_other[NON_PCCM_PARAM_COUNT];     ///< First moment for non-PCCM parameters
    float adam_v_other[NON_PCCM_PARAM_COUNT];     ///< Second moment for non-PCCM parameters
    float adam_beta1_t;        ///< Running BETA1^t for bias correction (starts at BETA1^1)
    float adam_beta2_t;        ///< Running BETA2^t for bias correction (starts at BETA2^1)

    // Optimization state
    float learning_rate;
};

//===========================================================================
// HELPER FUNCTIONS
//===========================================================================

/**
 * @brief Apply constraints to parameters
 */
static void apply_constraints(color_calib_params_t* params, const cal_constraints_t* constraints)
{
    // Constrain PCCM linear terms (first 3 terms: R, G, B)
    // These act like the diagonal of a traditional CCM
    for (int i = 0; i < 3; i++)
    {
        // Linear diagonal-like terms (R->X, G->Y, B->Z)
        if (params->pccm[i][i] < constraints->ccm_diag_min)
            params->pccm[i][i] = constraints->ccm_diag_min;
        if (params->pccm[i][i] > constraints->ccm_diag_max)
            params->pccm[i][i] = constraints->ccm_diag_max;
            
        // Linear off-diagonal terms
        for (int j = 0; j < 3; j++)
        {
            if (i != j)
            {
                if (params->pccm[i][j] < constraints->ccm_off_diag_min)
                    params->pccm[i][j] = constraints->ccm_off_diag_min;
                if (params->pccm[i][j] > constraints->ccm_off_diag_max)
                    params->pccm[i][j] = constraints->ccm_off_diag_max;
            }
        }
    }
    
    // Constrain polynomial terms (indices 3-8: R², G², B², RG, RB, GB)
    // Use tighter constraints for polynomial terms to prevent instability
    const float poly_min = -1.0f;
    const float poly_max = 1.0f;
    for (int i = 0; i < 3; i++)
    {
        for (int j = 3; j < 9; j++)
        {
            if (params->pccm[i][j] < poly_min)
                params->pccm[i][j] = poly_min;
            if (params->pccm[i][j] > poly_max)
                params->pccm[i][j] = poly_max;
        }
    }
    
    // Force constant terms (index 9: bias/offset) to zero.
    // A non-zero constant offset in the PCCM compensates for the residual black
    // bias in bright-color calibration references, but the same offset destroys
    // the tiny Y/Z signal from dark (near-black) colors, causing them to be
    // misidentified as black.  Black-level subtraction is the correct place to
    // remove additive offsets; the PCCM should only apply linear/polynomial
    // colour corrections.
    for (int i = 0; i < 3; i++)
    {
        params->pccm[i][9] = 0.0f;
    }
    
    // Constrain lightness parameters
    if (params->lightness_scale < constraints->lightness_scale_min)
        params->lightness_scale = constraints->lightness_scale_min;
    if (params->lightness_scale > constraints->lightness_scale_max)
        params->lightness_scale = constraints->lightness_scale_max;
    if (params->lightness_offset < constraints->lightness_offset_min)
        params->lightness_offset = constraints->lightness_offset_min;
    if (params->lightness_offset > constraints->lightness_offset_max)
        params->lightness_offset = constraints->lightness_offset_max;
    if (params->lightness_gamma < constraints->lightness_gamma_min)
        params->lightness_gamma = constraints->lightness_gamma_min;
    if (params->lightness_gamma > constraints->lightness_gamma_max)
        params->lightness_gamma = constraints->lightness_gamma_max;
    
    // Constrain piecewise gamma parameters
    if (params->lightness_gamma_dark < constraints->lightness_gamma_dark_min)
        params->lightness_gamma_dark = constraints->lightness_gamma_dark_min;
    if (params->lightness_gamma_dark > constraints->lightness_gamma_dark_max)
        params->lightness_gamma_dark = constraints->lightness_gamma_dark_max;
        
    if (params->lightness_gamma_light < constraints->lightness_gamma_light_min)
        params->lightness_gamma_light = constraints->lightness_gamma_light_min;
    if (params->lightness_gamma_light > constraints->lightness_gamma_light_max)
        params->lightness_gamma_light = constraints->lightness_gamma_light_max;
        
    if (params->lightness_transition < constraints->lightness_transition_min)
        params->lightness_transition = constraints->lightness_transition_min;
    if (params->lightness_transition > constraints->lightness_transition_max)
        params->lightness_transition = constraints->lightness_transition_max;
    
    // Constrain saturation
    if (params->saturation_boost < constraints->saturation_min)
        params->saturation_boost = constraints->saturation_min;
    if (params->saturation_boost > constraints->saturation_max)
        params->saturation_boost = constraints->saturation_max;
    
    // Constrain blue correction parameters
    if (params->blue_correction_magnitude < constraints->blue_correction_magnitude_min)
        params->blue_correction_magnitude = constraints->blue_correction_magnitude_min;
    if (params->blue_correction_magnitude > constraints->blue_correction_magnitude_max)
        params->blue_correction_magnitude = constraints->blue_correction_magnitude_max;
        
    if (params->blue_correction_center < constraints->blue_correction_center_min)
        params->blue_correction_center = constraints->blue_correction_center_min;
    if (params->blue_correction_center > constraints->blue_correction_center_max)
        params->blue_correction_center = constraints->blue_correction_center_max;
        
    if (params->blue_correction_width < constraints->blue_correction_width_min)
        params->blue_correction_width = constraints->blue_correction_width_min;
    if (params->blue_correction_width > constraints->blue_correction_width_max)
        params->blue_correction_width = constraints->blue_correction_width_max;
}

/**
 * @brief Apply calibration parameters to XYZ measurement
 *
 * Mirrors the runtime SCAN/MATCHING path (the Lab space that
 * try_match_kona_reference and the color-database fallback actually consume):
 * 1. Black Level Subtraction
 * 2. D65 Scaling + Exposure Gain (Linear Space)
 * 3. PCCM (Polynomial Color Correction Matrix)
 * 4. Y normalization when luminance exceeds the D65 white reference
 * 5. Lightness Correction (Gamma/Offset)
 *
 * Deliberately NO saturation boost and NO Z display floor: the runtime
 * scan_lab computation omits both (see color_pipeline_process_xyz), so
 * including them here would let the optimizer meet chroma targets by
 * "spending" boost instead of fixing the PCCM, leaving the matcher with
 * under-corrected chroma.
 */
lab_t auto_cal_apply_calibration(const xyz_t* xyz, const color_calib_params_t* params)
{
    xyz_t input = *xyz;

    // 1. Apply Black Level Subtraction (if active)
    if (params->has_black_calibration)
    {
        const float MAX_BLACK_SUBTRACTION_FRACTION = 0.40f;
        float black_x = fminf(params->black_level.x, input.x * MAX_BLACK_SUBTRACTION_FRACTION);
        float black_y = fminf(params->black_level.y, input.y * MAX_BLACK_SUBTRACTION_FRACTION);
        float black_z = fminf(params->black_level.z, input.z * MAX_BLACK_SUBTRACTION_FRACTION);

        input.x -= black_x;
        input.y -= black_y;
        input.z -= black_z;
        
        // Clamp to 0
        if (input.x < 0.0f) input.x = 0.0f;
        if (input.y < 0.0f) input.y = 0.0f;
        if (input.z < 0.0f) input.z = 0.0f;
    }

    // 2. Apply D65 Scaling + Global Gain (Exposure)
    // We combine D65 scaling and the optimization gain parameter here in linear space.
    // This allows the PCCM to operate on properly scaled data.
    float gain = params->lightness_scale;
    input.x *= (D65_X / 100.0f) * gain;
    input.y *= (D65_Y / 100.0f) * gain;
    input.z *= (D65_Z / 100.0f) * gain;

    // 3. Apply PCCM
    xyz_t corrected_xyz = color_math_apply_pccm(&input, params->pccm);
    
    // Apply XYZ output scaling (CRITICAL - must match runtime pipeline)
    // This scale factor is applied in color_pipeline.cpp after PCCM and MUST be
    // applied here as well to ensure calibration optimizer sees the same Lab values
    // as the runtime pipeline for identical input XYZ values.
    corrected_xyz.x *= XYZ_OUTPUT_SCALE;
    corrected_xyz.y *= XYZ_OUTPUT_SCALE;
    corrected_xyz.z *= XYZ_OUTPUT_SCALE;

    // 4. Normalize XYZ when luminance exceeds the D65 white reference, exactly
    // as the runtime does before Lab conversion. Scales all channels
    // proportionally to preserve chromaticity.
    if (corrected_xyz.y > D65_Y)
    {
        float scale = D65_Y / corrected_xyz.y;
        corrected_xyz.x *= scale;
        corrected_xyz.y *= scale;
        corrected_xyz.z *= scale;
    }

    // 5. Convert to Lab
    lab_t lab = color_math_xyz_to_lab(corrected_xyz);

    // 6. Apply Lightness Correction (Gamma/Offset ONLY)
    // CRITICAL: We create a temp params struct with scale=1.0 to prevent double-scaling.
    // The scale was already applied in step 2 (linear space).
    color_calib_params_t temp_params = *params;
    temp_params.lightness_scale = 1.0f;
    lab.l = color_math_correct_lightness(lab.l, &temp_params);

    // No saturation enhancement here — the matching path consumes unboosted
    // Lab (scan_lab), so the optimizer objective must be computed in the same
    // space (see function doc comment).

    // Clamp L to valid range
    if (lab.l < 0.0f) lab.l = 0.0f;
    if (lab.l > 100.0f) lab.l = 100.0f;

    return lab;
}

/**
 * @brief Calculate average ΔE error for all measurements
 * 
 * Black reference is excluded from the average because after proper black level
 * subtraction it will always be near XYZ(0,0,0) regardless of CCM/gamma settings.
 * Including it would contribute meaningless error to the optimization.
 * 
 * Weighting strategy:
 * - White reference (CAL_REF_FLAG_IS_WHITE): 1.5x — defines exposure for the whole pipeline
 * - Saturated colors (non-neutral, non-white, high chroma): 1.5x weight to prioritize accuracy
 * - Gray references (CAL_REF_FLAG_GRAY): 1.0x weight for lightness/gamma calibration
 * - Neutral/other references: 1.0x default weight
 */
static float calculate_average_error(auto_cal_ctx_t* ctx)
{
    float total_error = 0.0f;
    float total_weight = 0.0f;
    
    for (int i = 0; i < ctx->num_references; i++)
    {
        if (ctx->measurements[i].valid)
        {
            // Skip black reference in optimization (it's used for offset calibration only)
            if (ctx->references[i].flags & CAL_REF_FLAG_IS_BLACK)
            {
                continue;
            }
            
            lab_t measured_lab = auto_cal_apply_calibration(&ctx->measurements[i].measured_xyz, &ctx->params);
            float error = color_math_delta_e_ciede2000(&measured_lab, &ctx->measurements[i].target_lab);
            
            // Apply weight based on color type
            float weight = DEFAULT_REFERENCE_WEIGHT;
            
            if (ctx->references[i].flags & CAL_REF_FLAG_IS_WHITE)
            {
                // White is the exposure anchor: give it elevated weight so the optimizer
                // cannot sacrifice it while chasing chromatic references.
                weight = WHITE_REFERENCE_WEIGHT;  // 1.5
            }
            else if (ctx->references[i].flags & CAL_REF_FLAG_GRAY)
            {
                weight = GRAY_REFERENCE_WEIGHT;  // 1.0
            }
            else if (ctx->references[i].flags & CAL_REF_FLAG_DARK_CHROMATIC)
            {
                // Dark chromatic references are harder to calibrate and conflict with bright
                // saturated colors in the optimizer.  Give them a reduced weight so they
                // improve dark-color identification without distorting the bright-color fit.
                weight = DARK_CHROMATIC_REFERENCE_WEIGHT;  // 0.75
            }
            else if (!(ctx->references[i].flags & (CAL_REF_FLAG_IS_NEUTRAL | CAL_REF_FLAG_IS_WHITE)))
            {
                // Non-neutral, non-white, non-dark-chromatic = bright saturated color
                weight = SATURATED_REFERENCE_WEIGHT;  // 1.5
            }
            total_error += error * weight;
            total_weight += weight;
        }
    }
    
    return (total_weight > 0.0f) ? (total_error / total_weight) : 0.0f;
}

/**
 * @brief Helper: get reference optimizer weight (same logic as calculate_average_error)
 */
static float get_reference_weight(const cal_reference_t* ref)
{
    if (ref->flags & CAL_REF_FLAG_IS_WHITE)        return WHITE_REFERENCE_WEIGHT;
    if (ref->flags & CAL_REF_FLAG_GRAY)            return GRAY_REFERENCE_WEIGHT;
    if (ref->flags & CAL_REF_FLAG_DARK_CHROMATIC)  return DARK_CHROMATIC_REFERENCE_WEIGHT;
    if (!(ref->flags & (CAL_REF_FLAG_IS_NEUTRAL | CAL_REF_FLAG_IS_WHITE)))
                                                    return SATURATED_REFERENCE_WEIGHT;
    return DEFAULT_REFERENCE_WEIGHT;
}

/**
 * @brief Invert the piecewise-gamma lightness correction.
 *
 * Finds L_xyz such that color_math_correct_lightness(L_xyz, params) = L_target,
 * using 60 iterations of binary search (error < 0.002 L* units).
 */
static float invert_lightness_correction(float L_target, const color_calib_params_t* params)
{
    // color_math_correct_lightness is monotonically increasing; binary search is safe.
    // Upper bound of 200 covers the full valid L* range (0–100) with margin for gamma > 1.
    // 60 iterations give sub-0.002 L* precision (2^-60 of a 200-unit search range).
    static constexpr float LIGHTNESS_SEARCH_UPPER_BOUND = 200.0f;
    static constexpr int   LIGHTNESS_INVERSION_ITERATIONS = 60;
    float lo = 0.0f, hi = LIGHTNESS_SEARCH_UPPER_BOUND;
    for (int k = 0; k < LIGHTNESS_INVERSION_ITERATIONS; k++)
    {
        float mid = (lo + hi) * 0.5f;
        // Evaluate with scale=1 (same as apply_calibration does via temp_params)
        color_calib_params_t tmp = *params;
        tmp.lightness_scale = 1.0f;
        float result = color_math_correct_lightness(mid, &tmp);
        if (result < L_target)
            lo = mid;
        else
            hi = mid;
    }
    return (lo + hi) * 0.5f;
}

/**
 * @brief Initialise the PCCM linear 3×3 terms analytically before Adam runs.
 *
 * Solves a per-output-channel weighted least-squares problem:
 *   pccm_row = argmin  Σ_i  w_i · (input_i · pccm_row - target_i)²
 * where input_i is the D65-scaled sensor XYZ (PCCM input space, /100 normalised)
 * and target_i is derived from the reference target Lab by inverting the complete
 * post-PCCM pipeline (lightness correction, saturation boost, XYZ_OUTPUT_SCALE).
 *
 * This replaces the hardcoded default CCM ([1.4,−0.2,−0.2] per channel) with a
 * measurement-derived starting point, cutting the Adam warm-up from ~130 to <30
 * iterations in practice.
 */
static void init_ccm_from_measurements(auto_cal_ctx_t* ctx)
{
    // Maximum references that contribute (all non-black valid ones)
    const int N_MAX = CAL_MAX_REFERENCES;
    float A[N_MAX][3];   // PCCM input vectors (normalised by /100)
    float B[N_MAX][3];   // Target PCCM output (normalised by /(100·XYZ_OUTPUT_SCALE))
    float wt[N_MAX];     // Optimizer weights
    int   n = 0;         // actual count

    const float MAX_BLACK_SUBTRACTION_FRACTION = 0.40f;
    float ls = ctx->params.lightness_scale;

    for (int i = 0; i < ctx->num_references; i++)
    {
        if (!ctx->measurements[i].valid) continue;
        if (ctx->references[i].flags & CAL_REF_FLAG_IS_BLACK) continue;

        // 1. Build PCCM input vector (mirrors apply_calibration steps 1+2)
        xyz_t xyz = ctx->measurements[i].measured_xyz;
        float bx = 0.0f, by = 0.0f, bz = 0.0f;
        if (ctx->params.has_black_calibration)
        {
            bx = fminf(ctx->params.black_level.x, xyz.x * MAX_BLACK_SUBTRACTION_FRACTION);
            by = fminf(ctx->params.black_level.y, xyz.y * MAX_BLACK_SUBTRACTION_FRACTION);
            bz = fminf(ctx->params.black_level.z, xyz.z * MAX_BLACK_SUBTRACTION_FRACTION);
        }
        // D65 scale + gain, then divide by 100 (as apply_pccm does internally)
        A[n][0] = fmaxf(xyz.x - bx, 0.0f) * (D65_X / 100.0f) * ls / 100.0f;
        A[n][1] = fmaxf(xyz.y - by, 0.0f) * (D65_Y / 100.0f) * ls / 100.0f;
        A[n][2] = fmaxf(xyz.z - bz, 0.0f) * (D65_Z / 100.0f) * ls / 100.0f;

        // 2. Invert pipeline to get target PCCM output XYZ.
        // No saturation-boost inversion: the objective (auto_cal_apply_calibration)
        // is computed in the unboosted scan/matching Lab space.
        lab_t tlab = ctx->measurements[i].target_lab;
        // Undo lightness correction: find the L value that xyz_to_lab must produce
        float L_xyz = invert_lightness_correction(tlab.l, &ctx->params);
        tlab.l = L_xyz;
        // Convert target Lab → XYZ (this is what the PCCM output must look like,
        // after XYZ_OUTPUT_SCALE; divide back to get the raw PCCM output)
        xyz_t tgt_xyz = color_math_lab_to_xyz(tlab);
        B[n][0] = tgt_xyz.x / (100.0f * XYZ_OUTPUT_SCALE);
        B[n][1] = tgt_xyz.y / (100.0f * XYZ_OUTPUT_SCALE);
        B[n][2] = tgt_xyz.z / (100.0f * XYZ_OUTPUT_SCALE);

        wt[n] = get_reference_weight(&ctx->references[i]);
        n++;
    }

    if (n < 3)
    {
        // Not enough references to constrain a 3×3 system; keep defaults.
        ESP_LOGW(TAG, "init_ccm_from_measurements: only %d references, skipping", n);
        return;
    }

    // 3. For each output channel k, solve: argmin Σ_i w_i·(A[i]·c - B[i][k])²
    //    Normal equations: (Aᵀ W A) c = Aᵀ W b   →   3×3 linear system
    for (int k = 0; k < 3; k++)
    {
        // Build 3×3 matrix M = Aᵀ W A  and  right-hand side rhs = Aᵀ W b
        float M[3][3] = {};
        float rhs[3] = {};
        for (int i = 0; i < n; i++)
        {
            float w = wt[i];
            for (int r = 0; r < 3; r++)
            {
                rhs[r] += w * A[i][r] * B[i][k];
                for (int c = 0; c < 3; c++)
                    M[r][c] += w * A[i][r] * A[i][c];
            }
        }

        // Solve 3×3 via Cramer's rule
        // det(M)
        float det = M[0][0]*(M[1][1]*M[2][2]-M[1][2]*M[2][1])
                  - M[0][1]*(M[1][0]*M[2][2]-M[1][2]*M[2][0])
                  + M[0][2]*(M[1][0]*M[2][1]-M[1][1]*M[2][0]);

        // Singularity threshold: if det is this small, the system is under-determined
        static constexpr float MATRIX_SINGULARITY_THRESHOLD = 1e-12f;
        if (fabsf(det) < MATRIX_SINGULARITY_THRESHOLD)
        {
            ESP_LOGW(TAG, "init_ccm_from_measurements: singular matrix for channel %d, skipping", k);
            continue;
        }

        float inv_det = 1.0f / det;
        // Cofactor expansion to get M⁻¹ (symmetric since Aᵀ W A is symmetric)
        float Minv[3][3];
        Minv[0][0] = (M[1][1]*M[2][2]-M[1][2]*M[2][1]) * inv_det;
        Minv[0][1] = (M[0][2]*M[2][1]-M[0][1]*M[2][2]) * inv_det;
        Minv[0][2] = (M[0][1]*M[1][2]-M[0][2]*M[1][1]) * inv_det;
        Minv[1][0] = (M[1][2]*M[2][0]-M[1][0]*M[2][2]) * inv_det;
        Minv[1][1] = (M[0][0]*M[2][2]-M[0][2]*M[2][0]) * inv_det;
        Minv[1][2] = (M[0][2]*M[1][0]-M[0][0]*M[1][2]) * inv_det;
        Minv[2][0] = (M[1][0]*M[2][1]-M[1][1]*M[2][0]) * inv_det;
        Minv[2][1] = (M[0][1]*M[2][0]-M[0][0]*M[2][1]) * inv_det;
        Minv[2][2] = (M[0][0]*M[1][1]-M[0][1]*M[1][0]) * inv_det;

        float sol[3] = {};
        for (int r = 0; r < 3; r++)
            for (int c = 0; c < 3; c++)
                sol[r] += Minv[r][c] * rhs[c];

        // Clamp to PCCM linear constraints before writing
        // Diagonal terms
        float diag_val = fmaxf(ctx->constraints.ccm_diag_min,
                               fminf(ctx->constraints.ccm_diag_max, sol[k]));
        ctx->params.pccm[k][k] = diag_val;
        // Off-diagonal terms
        for (int j = 0; j < 3; j++)
        {
            if (j == k) continue;
            ctx->params.pccm[k][j] = fmaxf(ctx->constraints.ccm_off_diag_min,
                                            fminf(ctx->constraints.ccm_off_diag_max, sol[j]));
        }
    }

    // Reset polynomial terms (indices 3–PCCM_CONSTANT_INDEX-1) to 0; Adam optimises them
    for (int i = 0; i < 3; i++)
        for (int j = 3; j < PCCM_CONSTANT_INDEX; j++)
            ctx->params.pccm[i][j] = 0.0f;

    ESP_LOGI(TAG, "Analytical CCM init (from %d refs):", n);
    ESP_LOGI(TAG, "  X: [%.3f, %.3f, %.3f]",
             ctx->params.pccm[0][0], ctx->params.pccm[0][1], ctx->params.pccm[0][2]);
    ESP_LOGI(TAG, "  Y: [%.3f, %.3f, %.3f]",
             ctx->params.pccm[1][0], ctx->params.pccm[1][1], ctx->params.pccm[1][2]);
    ESP_LOGI(TAG, "  Z: [%.3f, %.3f, %.3f]",
             ctx->params.pccm[2][0], ctx->params.pccm[2][1], ctx->params.pccm[2][2]);
}

/**
 * @brief Estimate gradient by finite differences
 *
 * Calculates numerical gradient for one parameter by perturbing it slightly
 * and measuring the change in error.
 */
static float estimate_gradient(auto_cal_ctx_t* ctx, float* param)
{
    float original_value = *param;

    // Use symmetric finite differences to reduce directional bias, then guard
    // against pathological spikes from non-smooth objective regions.
    *param = original_value + GRADIENT_EPSILON;
    float error_plus = calculate_average_error(ctx);

    *param = original_value - GRADIENT_EPSILON;
    float error_minus = calculate_average_error(ctx);

    *param = original_value;

    float gradient = (error_plus - error_minus) / (2.0f * GRADIENT_EPSILON);

    if (!isfinite(gradient))
    {
        return 0.0f;
    }

    if (gradient > MAX_GRADIENT_MAGNITUDE)
        gradient = MAX_GRADIENT_MAGNITUDE;
    else if (gradient < -MAX_GRADIENT_MAGNITUDE)
        gradient = -MAX_GRADIENT_MAGNITUDE;

    return gradient;
}

/**
 * @brief Invoke callback if set
 */
static void invoke_callback(auto_cal_ctx_t* ctx, const char* message)
{
    if (ctx->callback)
    {
        ctx->callback(message);
    }
}

//===========================================================================
// API IMPLEMENTATION
//===========================================================================

esp_err_t auto_cal_init(auto_cal_ctx_t** ctx)
{
    if (!ctx)
    {
        return ESP_ERR_INVALID_ARG;
    }
    
    auto_cal_ctx_t* new_ctx = (auto_cal_ctx_t*)calloc(1, sizeof(auto_cal_ctx_t));
    if (!new_ctx)
    {
        ESP_LOGE(TAG, "Failed to allocate calibration context");
        return ESP_ERR_NO_MEM;
    }
    
    // Initialize with defaults
    new_ctx->constraints = DEFAULT_CONSTRAINTS;
    new_ctx->params = DEFAULT_PARAMS;
    new_ctx->best_params = DEFAULT_PARAMS;
    new_ctx->status.state = CAL_STATE_IDLE;
    new_ctx->status.current_ref_index = -1;
    new_ctx->learning_rate = ADAM_ALPHA;
    
    *ctx = new_ctx;
    
    ESP_LOGI(TAG, "Auto-calibration initialized with %d max references", CAL_MAX_REFERENCES);
    ESP_LOGI(TAG, "Default constraints: CCM[%.2f-%.2f], Lightness[%.2f-%.2f], Gamma[%.2f-%.2f], Sat[%.2f-%.2f]",
             DEFAULT_CONSTRAINTS.ccm_diag_min, DEFAULT_CONSTRAINTS.ccm_diag_max,
             DEFAULT_CONSTRAINTS.lightness_scale_min, DEFAULT_CONSTRAINTS.lightness_scale_max,
             DEFAULT_CONSTRAINTS.lightness_gamma_min, DEFAULT_CONSTRAINTS.lightness_gamma_max,
             DEFAULT_CONSTRAINTS.saturation_min, DEFAULT_CONSTRAINTS.saturation_max);
    ESP_LOGI(TAG, "Piecewise gamma: dark[%.2f-%.2f], light[%.2f-%.2f], transition[%.1f-%.1f]",
             DEFAULT_CONSTRAINTS.lightness_gamma_dark_min, DEFAULT_CONSTRAINTS.lightness_gamma_dark_max,
             DEFAULT_CONSTRAINTS.lightness_gamma_light_min, DEFAULT_CONSTRAINTS.lightness_gamma_light_max,
             DEFAULT_CONSTRAINTS.lightness_transition_min, DEFAULT_CONSTRAINTS.lightness_transition_max);
    
    return ESP_OK;
}

void auto_cal_deinit(auto_cal_ctx_t** ctx)
{
    if (ctx && *ctx)
    {
        free(*ctx);
        *ctx = NULL;
        ESP_LOGI(TAG, "Calibration context freed");
    }
}

esp_err_t auto_cal_set_references_preset(auto_cal_ctx_t* ctx, const cal_reference_t* preset, int count)
{
    if (!ctx || !preset)
    {
        ESP_LOGE(TAG, "Invalid arguments to auto_cal_set_references_preset");
        return ESP_ERR_INVALID_ARG;
    }
    
    if (count < CAL_MIN_REFERENCES || count > CAL_MAX_REFERENCES)
    {
        ESP_LOGE(TAG, "Invalid reference count: %d (min=%d, max=%d)", count, CAL_MIN_REFERENCES, CAL_MAX_REFERENCES);
        return ESP_ERR_INVALID_ARG;
    }
    
    memcpy(ctx->references, preset, count * sizeof(cal_reference_t));
    ctx->num_references = count;
    
    // Find white reference index
    int white_index = -1;
    for (int i = 0; i < count; i++)
    {
        if (ctx->references[i].flags & CAL_REF_FLAG_IS_WHITE)
        {
            white_index = i;
            break;
        }
    }
    
    ESP_LOGI(TAG, "Set %d reference colors, white at index %d", count, white_index);
    for (int i = 0; i < count; i++)
    {
        ESP_LOGI(TAG, "Added reference '%s' at index %d: Lab(%.1f, %.1f, %.1f) flags=0x%02x",
                 ctx->references[i].name, i,
                 ctx->references[i].target_lab.l,
                 ctx->references[i].target_lab.a,
                 ctx->references[i].target_lab.b,
                 ctx->references[i].flags);
    }
    
    return ESP_OK;
}

esp_err_t auto_cal_set_references(auto_cal_ctx_t* ctx, const cal_reference_t* references, int count)
{
    return auto_cal_set_references_preset(ctx, references, count);
}

int auto_cal_add_reference(auto_cal_ctx_t* ctx, const char* name, const lab_t* target_lab, uint8_t flags)
{
    if (!ctx || !name || !target_lab)
    {
        ESP_LOGE(TAG, "Invalid arguments to auto_cal_add_reference");
        return -1;
    }
    
    if (ctx->num_references >= CAL_MAX_REFERENCES)
    {
        ESP_LOGE(TAG, "Maximum references (%d) reached", CAL_MAX_REFERENCES);
        return -1;
    }
    
    int idx = ctx->num_references;
    strncpy(ctx->references[idx].name, name, CAL_MAX_NAME_LENGTH - 1);
    ctx->references[idx].name[CAL_MAX_NAME_LENGTH - 1] = '\0';
    ctx->references[idx].target_lab = *target_lab;
    ctx->references[idx].flags = flags;
    ctx->references[idx].measured = false;
    ctx->num_references++;
    
    ESP_LOGI(TAG, "Added reference '%s' at index %d: Lab(%.1f, %.1f, %.1f) flags=0x%02x",
             name, idx, target_lab->l, target_lab->a, target_lab->b, flags);
    
    return idx;
}

/**
 * @brief Helper function for gamma expansion (sRGB to linear RGB)
 */
static inline float gamma_expand(float c)
{
    if (c <= 0.04045f)
        return c / 12.92f;
    else
        return powf((c + 0.055f) / 1.055f, 2.4f);
}

int auto_cal_add_reference_rgb(auto_cal_ctx_t* ctx, const char* name, uint8_t r, uint8_t g, uint8_t b, uint8_t flags)
{
    if (!ctx || !name)
    {
        return -1;
    }
    
    // Convert RGB to Lab (via XYZ)
    // sRGB to linear RGB
    float rf = gamma_expand(r / 255.0f);
    float gf = gamma_expand(g / 255.0f);
    float bf = gamma_expand(b / 255.0f);
    
    // sRGB to XYZ (D65)
    xyz_t xyz;
    xyz.x = 0.4124f * rf + 0.3576f * gf + 0.1805f * bf;
    xyz.y = 0.2126f * rf + 0.7152f * gf + 0.0722f * bf;
    xyz.z = 0.0193f * rf + 0.1192f * gf + 0.9505f * bf;
    
    // Scale to 0-100 range
    xyz.x *= 100.0f;
    xyz.y *= 100.0f;
    xyz.z *= 100.0f;
    
    // Convert to Lab
    lab_t lab = color_math_xyz_to_lab(xyz);
    
    return auto_cal_add_reference(ctx, name, &lab, flags);
}

void auto_cal_clear_references(auto_cal_ctx_t* ctx)
{
    if (ctx)
    {
        ctx->num_references = 0;
        ESP_LOGI(TAG, "Cleared all references");
    }
}

void auto_cal_set_callback(auto_cal_ctx_t* ctx, cal_callback_t cb)
{
    if (ctx)
    {
        ctx->callback = cb;
    }
}

esp_err_t auto_cal_start(auto_cal_ctx_t* ctx)
{
    if (!ctx)
    {
        return ESP_ERR_INVALID_ARG;
    }
    
    if (ctx->num_references < CAL_MIN_REFERENCES)
    {
        ESP_LOGE(TAG, "Need at least %d references (have %d)", CAL_MIN_REFERENCES, ctx->num_references);
        return ESP_ERR_INVALID_STATE;
    }
    
    // Reset state
    memset(ctx->measurements, 0, sizeof(ctx->measurements));
    ctx->status.state = CAL_STATE_MEASURING;
    ctx->status.current_ref_index = 0;
    ctx->status.total_references = ctx->num_references;
    ctx->status.measurements_collected = 0;
    ctx->status.iteration = 0;
    ctx->status.converged = false;
    ctx->status.stall_count = 0;
    
    ESP_LOGI(TAG, "Started calibration with %d references", ctx->num_references);
    invoke_callback(ctx, "Calibration started");
    
    return ESP_OK;
}

esp_err_t auto_cal_submit_measurement(auto_cal_ctx_t* ctx, const xyz_t* xyz)
{
    if (!ctx || !xyz)
    {
        return ESP_ERR_INVALID_ARG;
    }
    
    if (ctx->status.state != CAL_STATE_MEASURING)
    {
        ESP_LOGE(TAG, "Not in measuring state");
        return ESP_ERR_INVALID_STATE;
    }
    
    int idx = ctx->status.current_ref_index;
    if (idx < 0 || idx >= ctx->num_references)
    {
        ESP_LOGE(TAG, "Invalid reference index");
        return ESP_ERR_INVALID_STATE;
    }
    
    // Store measurement
    ctx->measurements[idx].measured_xyz = *xyz;
    ctx->measurements[idx].target_lab = ctx->references[idx].target_lab;
    ctx->measurements[idx].valid = true;
    ctx->references[idx].measured_xyz = *xyz;
    ctx->references[idx].measured = true;
    ctx->status.measurements_collected++;
    
    ESP_LOGI(TAG, "Captured %s (%d/%d): XYZ(%.2f, %.2f, %.2f)",
             ctx->references[idx].name,
             ctx->status.measurements_collected,
             ctx->num_references,
             xyz->x, xyz->y, xyz->z);
    
    // Capture black level if this is the black reference
    if (ctx->references[idx].flags & CAL_REF_FLAG_IS_BLACK)
    {
        // The dark gray swatch (RGB 35,31,32, L*≈11.5) has actual reflectance.
        // We need to subtract only the sensor offset, not the swatch reflectance.
        // 
        // The captured value includes:
        // - Sensor offset (what we want to subtract from measurements)
        // - Swatch's actual reflectance (what we should NOT subtract)
        //
        // To estimate the pure sensor offset, we scale down the captured value
        // by a factor that accounts for the swatch not being pure black.
        // 
        // A scaling factor of 0.75 works well empirically:
        // - Captures most of the sensor offset
        // - Leaves headroom for the swatch's actual dark gray color
        const float BLACK_LEVEL_SCALE = 0.75f;  // Empirical factor
        
        ctx->params.black_level.x = xyz->x * BLACK_LEVEL_SCALE;
        ctx->params.black_level.y = xyz->y * BLACK_LEVEL_SCALE;
        ctx->params.black_level.z = xyz->z * BLACK_LEVEL_SCALE;
        ctx->params.has_black_calibration = true;
        
        ctx->best_params.black_level = ctx->params.black_level;
        ctx->best_params.has_black_calibration = true;
        
        ESP_LOGI(TAG, "Black level captured (scaled %.0f%%): XYZ(%.2f, %.2f, %.2f)",
                 BLACK_LEVEL_SCALE * 100.0f,
                 ctx->params.black_level.x, 
                 ctx->params.black_level.y, 
                 ctx->params.black_level.z);
    }
    
    // Advance to next reference
    ctx->status.current_ref_index++;
    
    // Check if all measurements collected
    if (ctx->status.current_ref_index >= ctx->num_references)
    {
        ctx->status.current_ref_index = -1;
        ESP_LOGI(TAG, "All measurements collected");
        invoke_callback(ctx, "All measurements collected. Ready to optimize.");
    }
    
    return ESP_OK;
}

esp_err_t auto_cal_skip_current(auto_cal_ctx_t* ctx)
{
    if (!ctx)
    {
        return ESP_ERR_INVALID_ARG;
    }
    
    if (ctx->status.state != CAL_STATE_MEASURING)
    {
        return ESP_ERR_INVALID_STATE;
    }
    
    int idx = ctx->status.current_ref_index;
    if (idx < 0 || idx >= ctx->num_references)
    {
        return ESP_ERR_INVALID_STATE;
    }
    
    // Check if required
    if (ctx->references[idx].flags & CAL_REF_FLAG_REQUIRED)
    {
        ESP_LOGW(TAG, "%s is required and cannot be skipped", ctx->references[idx].name);
        return ESP_ERR_INVALID_STATE;
    }
    
    // Mark as invalid and advance
    ctx->measurements[idx].valid = false;
    ctx->references[idx].measured = false;
    ctx->status.current_ref_index++;
    
    ESP_LOGI(TAG, "Skipped optional reference '%s'", ctx->references[idx].name);
    
    // Check if done
    if (ctx->status.current_ref_index >= ctx->num_references)
    {
        ctx->status.current_ref_index = -1;
        invoke_callback(ctx, "All measurements collected. Ready to optimize.");
    }
    
    return ESP_OK;
}

esp_err_t auto_cal_run_optimization(auto_cal_ctx_t* ctx)
{
    if (!ctx)
    {
        return ESP_ERR_INVALID_ARG;
    }
    
    if (ctx->status.state != CAL_STATE_MEASURING || ctx->status.current_ref_index != -1)
    {
        ESP_LOGE(TAG, "Not ready for optimization");
        return ESP_ERR_INVALID_STATE;
    }
    
    // Count valid measurements
    int measured_count = 0;
    for (int i = 0; i < ctx->num_references; i++)
    {
        if (ctx->measurements[i].valid)
        {
            measured_count++;
        }
    }
    
    ctx->status.state = CAL_STATE_OPTIMIZING;
    ctx->learning_rate = ADAM_ALPHA;

    // Reset Adam optimizer state (previous optimization values must not carry over on restart)
    memset(ctx->adam_m_pccm, 0, sizeof(ctx->adam_m_pccm));
    memset(ctx->adam_v_pccm, 0, sizeof(ctx->adam_v_pccm));
    memset(ctx->adam_m_other, 0, sizeof(ctx->adam_m_other));
    memset(ctx->adam_v_other, 0, sizeof(ctx->adam_v_other));
    ctx->adam_beta1_t = ADAM_BETA1;   // BETA1^1 for first step's bias correction
    ctx->adam_beta2_t = ADAM_BETA2;   // BETA2^1 for first step's bias correction

    // Estimate lightness_scale from the white measurement so the optimizer starts
    // near the correct gain regardless of sensor illumination level.
    //
    // Derivation: CIE L* = 116·(Y/Yn)^(1/3) − 16  for Y/Yn > 0.008856
    //   → Y/Yn = ((L*+16)/116)³  ≈ 0.881 for L*=95.2
    // The optimizer pipeline applies:
    //   Y_out = Y_white_eff · (D65_Y/100) · ls · PCCM_Y_diag · XYZ_OUTPUT_SCALE
    // Solving for ls:
    //   ls = 0.881 · D65_Y / (Y_white_eff · PCCM_Y_diag · XYZ_OUTPUT_SCALE)
    // Black subtraction caps at MAX_BLACK_SUBTRACTION_FRACTION of the channel value.
    {
        // Target Y/Yn for white L*=95.2: ((95.2+16)/116)^3
        const float TARGET_Y_RATIO = 0.881f;
        // Must match DEFAULT_PARAMS.pccm[1][1] — the Y-channel diagonal coefficient
        const float DEFAULT_PCCM_Y_DIAG = DEFAULT_PARAMS.pccm[1][1];
        // Must match MAX_BLACK_SUBTRACTION_FRACTION used in apply_calibration()
        const float MAX_BLACK_SUBTRACTION_FRACTION = 0.40f;

        // Find white reference measurement
        float white_y = 0.0f;
        float black_y = ctx->params.has_black_calibration ? ctx->params.black_level.y : 0.0f;
        for (int i = 0; i < ctx->num_references; i++)
        {
            if ((ctx->references[i].flags & CAL_REF_FLAG_IS_WHITE) && ctx->measurements[i].valid)
            {
                white_y = ctx->measurements[i].measured_xyz.y;
                break;
            }
        }

        if (white_y > 0.0f)
        {
            float effective_black = fminf(black_y, white_y * MAX_BLACK_SUBTRACTION_FRACTION);
            float white_y_eff = white_y - effective_black;
            if (white_y_eff > 0.0f)
            {
                float ls_est = (TARGET_Y_RATIO * D65_Y) / (white_y_eff * DEFAULT_PCCM_Y_DIAG * XYZ_OUTPUT_SCALE);
                // Clamp to constraint range
                ls_est = fmaxf(ctx->constraints.lightness_scale_min,
                               fminf(ctx->constraints.lightness_scale_max, ls_est));
                ctx->params.lightness_scale = ls_est;
                ctx->best_params.lightness_scale = ls_est;
                ESP_LOGI(TAG, "White Y=%.2f (eff=%.2f) → estimated lightness_scale=%.4f",
                         white_y, white_y_eff, ls_est);
            }
        }
    }
    
    // Analytically initialize the PCCM linear 3×3 terms using weighted least squares.
    // This puts Adam in a region close to the true optimum before the first gradient step,
    // avoiding the ~130+ iterations wasted converging from the hardcoded default CCM.
    init_ccm_from_measurements(ctx);

    // Calculate initial error
    ctx->status.initial_error = calculate_average_error(ctx);
    ctx->status.current_error = ctx->status.initial_error;
    ctx->status.best_error = ctx->status.initial_error;
    ctx->best_params = ctx->params;
    
    ESP_LOGI(TAG, "=== Starting optimization with %d references ===", measured_count);
    ESP_LOGI(TAG, "Initial average ΔE: %.2f", ctx->status.initial_error);
    invoke_callback(ctx, "Optimizing calibration parameters...");
    
    // Gradient descent loop
    // Adam warm-restart counter — allows one finer search from best params after stall
    int warm_restarts_remaining = MAX_WARM_RESTARTS;
    // Consecutive flat (sub-MIN_IMPROVEMENT) iterations — see MAX_FLAT_ITERATIONS
    int flat_iterations = 0;

    for (int iter = 0; iter < MAX_ITERATIONS; iter++)
    {
        ctx->status.iteration = iter;
        
        // Estimate gradients for PCCM elements (3x10 = 30 parameters)
        float pccm_gradients[3][10] = {};
        
        // Optimize all PCCM parameters
        for (int i = 0; i < 3; i++)
        {
            for (int j = 0; j < PCCM_TERMS; j++)
            {
                // PCCM_CONSTANT_INDEX (j=9) is the bias/offset term.  apply_constraints
                // always clamps it to 0, so computing its gradient would be both wasteful
                // and would corrupt the Adam m/v accumulators with meaningless values.
                if (j == PCCM_CONSTANT_INDEX)
                {
                    pccm_gradients[i][j] = 0.0f;
                    continue;
                }
                pccm_gradients[i][j] = estimate_gradient(ctx, &ctx->params.pccm[i][j]);
            }
        }
        
        // Estimate gradients for lightness parameters
        float lightness_scale_grad = estimate_gradient(ctx, &ctx->params.lightness_scale);
        float lightness_offset_grad = estimate_gradient(ctx, &ctx->params.lightness_offset);
        float lightness_gamma_grad = estimate_gradient(ctx, &ctx->params.lightness_gamma);
        
        // Estimate gradients for piecewise gamma parameters (only if enabled)
        float lightness_gamma_dark_grad = 0.0f;
        float lightness_gamma_light_grad = 0.0f;
        float lightness_transition_grad = 0.0f;
        if (ctx->params.lightness_transition > 0.0f)
        {
            lightness_gamma_dark_grad = estimate_gradient(ctx, &ctx->params.lightness_gamma_dark);
            lightness_gamma_light_grad = estimate_gradient(ctx, &ctx->params.lightness_gamma_light);
            lightness_transition_grad = estimate_gradient(ctx, &ctx->params.lightness_transition);
        }
        
        // Saturation boost is display-only cosmetic: the objective
        // (auto_cal_apply_calibration) is computed in the unboosted matching
        // space, so its gradient is identically zero — skip the two objective
        // evaluations rather than feed a guaranteed-zero gradient to Adam.
        float saturation_grad = 0.0f;
        // Estimate gradients for blue-lightness correction
        float blue_correction_magnitude_grad = estimate_gradient(ctx, &ctx->params.blue_correction_magnitude);
        float blue_correction_center_grad = estimate_gradient(ctx, &ctx->params.blue_correction_center);
        float blue_correction_width_grad = estimate_gradient(ctx, &ctx->params.blue_correction_width);
        
        // Log gradients at debug level (sample of important terms)
        TCS_LOGD(TAG, "  PCCM linear gradients: X[%.4f,%.4f,%.4f] Y[%.4f,%.4f,%.4f] Z[%.4f,%.4f,%.4f]",
                 pccm_gradients[0][0], pccm_gradients[0][1], pccm_gradients[0][2],
                 pccm_gradients[1][0], pccm_gradients[1][1], pccm_gradients[1][2],
                 pccm_gradients[2][0], pccm_gradients[2][1], pccm_gradients[2][2]);
        TCS_LOGD(TAG, "  Adam alpha=%.4f bc1=%.6f bc2=%.6f",
                 ctx->learning_rate, 1.0f - ctx->adam_beta1_t, 1.0f - ctx->adam_beta2_t);
        
        // Update parameters using Adam optimizer.
        // Adam normalises each gradient by its running RMS, which prevents the oscillation
        // caused by conflicting bright/dark reference gradients that plagued plain SGD.
        float bc1 = 1.0f - ctx->adam_beta1_t;   // bias correction for first moment
        float bc2 = 1.0f - ctx->adam_beta2_t;   // bias correction for second moment
        
        for (int i = 0; i < 3; i++)
        {
            for (int j = 0; j < PCCM_TERMS; j++)
            {
                float g = pccm_gradients[i][j];
                ctx->adam_m_pccm[i][j] = ADAM_BETA1 * ctx->adam_m_pccm[i][j] + (1.0f - ADAM_BETA1) * g;
                ctx->adam_v_pccm[i][j] = ADAM_BETA2 * ctx->adam_v_pccm[i][j] + (1.0f - ADAM_BETA2) * g * g;
                float m_hat = ctx->adam_m_pccm[i][j] / bc1;
                float v_hat = ctx->adam_v_pccm[i][j] / bc2;
                ctx->params.pccm[i][j] -= ctx->learning_rate * m_hat / (sqrtf(v_hat) + ADAM_EPS);
            }
        }
        
        // Non-PCCM parameters: order matches adam_m_other/adam_v_other indices.
        float other_grads[NON_PCCM_PARAM_COUNT] =
        {
            lightness_scale_grad, lightness_offset_grad, lightness_gamma_grad,
            lightness_gamma_dark_grad, lightness_gamma_light_grad, lightness_transition_grad,
            saturation_grad,
            blue_correction_magnitude_grad, blue_correction_center_grad, blue_correction_width_grad
        };
        float* other_params[NON_PCCM_PARAM_COUNT] =
        {
            &ctx->params.lightness_scale, &ctx->params.lightness_offset, &ctx->params.lightness_gamma,
            &ctx->params.lightness_gamma_dark, &ctx->params.lightness_gamma_light, &ctx->params.lightness_transition,
            &ctx->params.saturation_boost,
            &ctx->params.blue_correction_magnitude, &ctx->params.blue_correction_center, &ctx->params.blue_correction_width
        };
        for (int k = 0; k < NON_PCCM_PARAM_COUNT; k++)
        {
            float g = other_grads[k];
            ctx->adam_m_other[k] = ADAM_BETA1 * ctx->adam_m_other[k] + (1.0f - ADAM_BETA1) * g;
            ctx->adam_v_other[k] = ADAM_BETA2 * ctx->adam_v_other[k] + (1.0f - ADAM_BETA2) * g * g;
            float m_hat = ctx->adam_m_other[k] / bc1;
            float v_hat = ctx->adam_v_other[k] / bc2;
            *other_params[k] -= ctx->learning_rate * SCALAR_LR_MULTIPLIER * m_hat / (sqrtf(v_hat) + ADAM_EPS);
        }
        
        // Advance Adam bias correction accumulators for next iteration
        ctx->adam_beta1_t *= ADAM_BETA1;
        ctx->adam_beta2_t *= ADAM_BETA2;
        
        // Apply constraints
        apply_constraints(&ctx->params, &ctx->constraints);
        
        // Calculate new error
        float new_error = calculate_average_error(ctx);
        float improvement = ctx->status.current_error - new_error;
        
        ctx->status.current_error = new_error;
        
        // Track best parameters
        if (new_error < ctx->status.best_error)
        {
            ctx->status.best_error = new_error;
            ctx->best_params = ctx->params;
            ctx->status.stall_count = 0;
        }
        else
        {
            ctx->status.stall_count++;
        }
        
        // Calculate per-color errors for detailed logging
        if (iter % 10 == 0)
        {
            // Find specific color errors for logging
            float de_red = 0.0f, de_green = 0.0f, de_blue = 0.0f, de_yellow = 0.0f;
            for (int i = 0; i < ctx->num_references; i++)
            {
                if (!ctx->measurements[i].valid) continue;
                
                // Skip black reference - only used for black level capture, not color accuracy
                if (ctx->references[i].flags & CAL_REF_FLAG_IS_BLACK) continue;
                
                lab_t measured_lab = auto_cal_apply_calibration(&ctx->measurements[i].measured_xyz, &ctx->params);
                float de = color_math_delta_e_ciede2000(&measured_lab, &ctx->measurements[i].target_lab);
                
                // Track the bright primary colors specifically (use exact "Brights" prefix
                // to avoid "Dark Green" overwriting "Brights Green" due to substring match)
                const char* name = ctx->references[i].name;
                if (strstr(name, "Brights Red") != NULL) de_red = de;
                else if (strstr(name, "Brights Green") != NULL) de_green = de;
                else if (strstr(name, "Brights Blue") != NULL) de_blue = de;
                else if (strstr(name, "Brights Yellow") != NULL) de_yellow = de;
            }
            
            ESP_LOGI(TAG, "Iter %3d: avg ΔE=%6.2f (best=%6.2f) [R:%.1f G:%.1f B:%.1f Y:%.1f]",
                     iter, new_error, ctx->status.best_error,
                     de_red, de_green, de_blue, de_yellow);
            TCS_LOGD(TAG, "  PCCM linear diag: [%.3f, %.3f, %.3f]",
                     ctx->params.pccm[0][0], ctx->params.pccm[1][1], ctx->params.pccm[2][2]);
            TCS_LOGD(TAG, "  Lightness: scale=%.4f offset=%.2f",
                     ctx->params.lightness_scale, ctx->params.lightness_offset);
            TCS_LOGD(TAG, "  Saturation boost: %.3f", ctx->params.saturation_boost);
        }
        
        // Check convergence
        if (ctx->status.best_error < TARGET_DELTA_E)
        {
            ESP_LOGI(TAG, "Converged! avg ΔE=%.2f after %d iterations (target was %.2f)",
                     ctx->status.best_error, iter, TARGET_DELTA_E);
            ctx->status.converged = true;
            break;
        }
        
        if (ctx->status.stall_count >= MAX_STALL_ITERATIONS)
        {
            if (warm_restarts_remaining > 0)
            {
                // Warm restart: reset Adam state and resume from the best params found so far
                // with a finer learning rate.  This lets the optimizer escape the momentum-driven
                // overshoot that caused the stall and do a more precise search around the best.
                warm_restarts_remaining--;
                ctx->params = ctx->best_params;
                memset(ctx->adam_m_pccm, 0, sizeof(ctx->adam_m_pccm));
                memset(ctx->adam_v_pccm, 0, sizeof(ctx->adam_v_pccm));
                memset(ctx->adam_m_other, 0, sizeof(ctx->adam_m_other));
                memset(ctx->adam_v_other, 0, sizeof(ctx->adam_v_other));
                ctx->adam_beta1_t = ADAM_BETA1;
                ctx->adam_beta2_t = ADAM_BETA2;
                ctx->learning_rate = ADAM_ALPHA_RESTART;
                ctx->status.stall_count = 0;
                flat_iterations = 0;
                ESP_LOGI(TAG, "Stalled at iter %d (avg ΔE=%.2f) — warm restart #%d with α=%.4f",
                         iter, ctx->status.best_error,
                         MAX_WARM_RESTARTS - warm_restarts_remaining,
                         ctx->learning_rate);
                continue;
            }
            ESP_LOGW(TAG, "Optimization stalled at iter %d, avg ΔE=%.2f (no improvement for %d iters)",
                     iter, ctx->status.best_error, MAX_STALL_ITERATIONS);
            break;
        }
        
        if (improvement < MIN_IMPROVEMENT && improvement >= 0)
        {
            flat_iterations++;
            if (flat_iterations >= MAX_FLAT_ITERATIONS)
            {
                ESP_LOGI(TAG, "Converged: improvement < %.6f for %d consecutive iterations (iter %d)",
                         (double)MIN_IMPROVEMENT, flat_iterations, iter);
                break;
            }
        }
        else
        {
            flat_iterations = 0;
        }
    }
    
    // Use best parameters
    ctx->params = ctx->best_params;
    ctx->status.current_error = ctx->status.best_error;
    ctx->status.state = CAL_STATE_COMPLETE;
    
    // Comprehensive final results logging
    ESP_LOGI(TAG, "========================================");
    ESP_LOGI(TAG, "=== AUTO-CALIBRATION COMPLETE ===");
    ESP_LOGI(TAG, "========================================");
    ESP_LOGI(TAG, "Final avg ΔE: %.2f (target: %.2f)", ctx->status.best_error, TARGET_DELTA_E);
    ESP_LOGI(TAG, "Iterations: %d", ctx->status.iteration);
    ESP_LOGI(TAG, "");
    ESP_LOGI(TAG, "Optimized CCM:");
    ESP_LOGI(TAG, "  | %7.3f %7.3f %7.3f |",
             ctx->params.pccm[0][0], ctx->params.pccm[0][1], ctx->params.pccm[0][2]);
    ESP_LOGI(TAG, "  | %7.3f %7.3f %7.3f |",
             ctx->params.pccm[1][0], ctx->params.pccm[1][1], ctx->params.pccm[1][2]);
    ESP_LOGI(TAG, "  | %7.3f %7.3f %7.3f |",
             ctx->params.pccm[2][0], ctx->params.pccm[2][1], ctx->params.pccm[2][2]);
    ESP_LOGI(TAG, "");
    ESP_LOGI(TAG, "Lightness: scale=%.4f offset=%.2f gamma=%.4f",
             ctx->params.lightness_scale, ctx->params.lightness_offset, ctx->params.lightness_gamma);
    ESP_LOGI(TAG, "Piecewise gamma: dark=%.4f light=%.4f transition=%.1f",
             ctx->params.lightness_gamma_dark, ctx->params.lightness_gamma_light, ctx->params.lightness_transition);
    ESP_LOGI(TAG, "Blue correction: magnitude=%.2f center=%.1f width=%.1f",
             ctx->params.blue_correction_magnitude,
             ctx->params.blue_correction_center,
             ctx->params.blue_correction_width);
    ESP_LOGI(TAG, "Saturation boost: %.3f", ctx->params.saturation_boost);
    ESP_LOGI(TAG, "Black level: X=%.2f Y=%.2f Z=%.2f",
             ctx->params.black_level.x, ctx->params.black_level.y, ctx->params.black_level.z);
    ESP_LOGI(TAG, "");
    ESP_LOGI(TAG, "Per-color results:");
    
    // Log per-color ΔE
    for (int i = 0; i < ctx->num_references; i++)
    {
        if (ctx->measurements[i].valid)
        {
            // Skip black reference - only used for black level capture, not color accuracy
            if (ctx->references[i].flags & CAL_REF_FLAG_IS_BLACK) continue;
            
            lab_t measured_lab = auto_cal_apply_calibration(&ctx->measurements[i].measured_xyz, &ctx->params);
            float de = color_math_delta_e_ciede2000(&measured_lab, &ctx->measurements[i].target_lab);
            ESP_LOGI(TAG, "  %-12s: ΔE=%5.2f (target L=%.1f a=%.1f b=%.1f)",
                     ctx->references[i].name, de,
                     ctx->measurements[i].target_lab.l,
                     ctx->measurements[i].target_lab.a,
                     ctx->measurements[i].target_lab.b);
        }
    }
    
    ESP_LOGI(TAG, "========================================");
    
    char msg[64];
    snprintf(msg, sizeof(msg), "Calibration complete. Average error: %.1f", ctx->status.best_error);
    invoke_callback(ctx, msg);
    
    return ESP_OK;
}

const cal_status_t* auto_cal_get_status(auto_cal_ctx_t* ctx)
{
    return ctx ? &ctx->status : NULL;
}

const char* auto_cal_get_current_ref_name(auto_cal_ctx_t* ctx)
{
    if (!ctx || ctx->status.current_ref_index < 0 || ctx->status.current_ref_index >= ctx->num_references)
    {
        return NULL;
    }
    
    return ctx->references[ctx->status.current_ref_index].name;
}

const cal_reference_t* auto_cal_get_current_ref(auto_cal_ctx_t* ctx)
{
    if (!ctx || ctx->status.current_ref_index < 0 || ctx->status.current_ref_index >= ctx->num_references)
    {
        return NULL;
    }
    
    return &ctx->references[ctx->status.current_ref_index];
}

const color_calib_params_t* auto_cal_get_params(auto_cal_ctx_t* ctx)
{
    return ctx ? &ctx->params : NULL;
}

esp_err_t auto_cal_apply_results(auto_cal_ctx_t* ctx)
{
    if (!ctx)
    {
        return ESP_ERR_INVALID_ARG;
    }
    
    if (ctx->status.state != CAL_STATE_COMPLETE)
    {
        ESP_LOGE(TAG, "Calibration not complete");
        return ESP_ERR_INVALID_STATE;
    }
    
    // Note: This would need to be integrated with the actual color_pipeline
    // configuration update mechanism. For now, just log what would be applied.
    ESP_LOGI(TAG, "Applying calibration results to color pipeline");
    ESP_LOGI(TAG, "  CCM diagonal: [%.3f, %.3f, %.3f]",
             ctx->params.pccm[0][0], ctx->params.pccm[1][1], ctx->params.pccm[2][2]);
    ESP_LOGI(TAG, "  Lightness: scale=%.3f, offset=%.3f, gamma=%.3f",
             ctx->params.lightness_scale, ctx->params.lightness_offset, ctx->params.lightness_gamma);
    ESP_LOGI(TAG, "  Piecewise gamma: dark=%.3f, light=%.3f, transition=%.1f",
             ctx->params.lightness_gamma_dark, ctx->params.lightness_gamma_light, ctx->params.lightness_transition);
    ESP_LOGI(TAG, "  Saturation boost: %.3f", ctx->params.saturation_boost);
    
    // Structures are now unified - no cast needed
    return color_pipeline_set_params(&ctx->params);
}

esp_err_t auto_cal_save_to_nvs(auto_cal_ctx_t* ctx, const char* ns)
{
    if (!ctx)
    {
        return ESP_ERR_INVALID_ARG;
    }
    const char* namespace_name = ns ? ns : CAL_NVS_NAMESPACE;
    return tcs_storage_save_blob(namespace_name, "params", &ctx->params, sizeof(color_calib_params_t));
}


esp_err_t auto_cal_load_from_nvs(auto_cal_ctx_t* ctx, const char* ns)
{
    if (!ctx)
    {
        return ESP_ERR_INVALID_ARG;
    }
    const char* namespace_name = ns ? ns : CAL_NVS_NAMESPACE;
    size_t required_size = sizeof(color_calib_params_t);
    esp_err_t err = tcs_storage_load_blob(namespace_name, "params", &ctx->params, &required_size);
    if (err != ESP_OK)
    {
        return err;
    }
    ctx->best_params = ctx->params;
    return ESP_OK;
}



esp_err_t auto_cal_submit_measurements_from_file(auto_cal_ctx_t* ctx, const char* path)
{
    if (!ctx || !path)
    {
        return ESP_ERR_INVALID_ARG;
    }
    FILE* f = fopen(path, "r");
    if (!f)
    {
        return ESP_ERR_NOT_FOUND;
    }

    char line[256];
    while (fgets(line, sizeof(line), f))
    {
        char name[CAL_MAX_NAME_LENGTH] = {0};
        float x = 0.0f, y = 0.0f, z = 0.0f;
        if (sscanf(line, " %63[^|]|%f|%f|%f", name, &x, &y, &z) != 4)
        {
            continue;
        }

        if (ctx->status.state != CAL_STATE_MEASURING)
        {
            break;
        }

        // auto_cal_submit_measurement() always stores at the CURRENT reference
        // index, so only accept the line matching the current reference name.
        // Submitting a name-matched line out of sequence would silently
        // attribute the measurement to the wrong swatch.
        const char* current = auto_cal_get_current_ref_name(ctx);
        if (!current || strcmp(current, name) != 0)
        {
            ESP_LOGW(TAG, "Skipping line for '%s' (expected '%s')", name, current ? current : "<none>");
            continue;
        }

        xyz_t xyz = {.x = x, .y = y, .z = z};
        esp_err_t ret = auto_cal_submit_measurement(ctx, &xyz);
        if (ret != ESP_OK)
        {
            fclose(f);
            return ret;
        }
    }
    fclose(f);
    return ESP_OK;
}


esp_err_t auto_cal_submit_raw_measurements_from_file(auto_cal_ctx_t* ctx, const char* path)
{
    if (!ctx || !path)
    {
        return ESP_ERR_INVALID_ARG;
    }

    FILE* f = fopen(path, "r");
    if (!f)
    {
        return ESP_ERR_NOT_FOUND;
    }

    char line[256];
    while (fgets(line, sizeof(line), f))
    {
        char name[CAL_MAX_NAME_LENGTH] = {0};
        unsigned long raw_x = 0, raw_y = 0, raw_z = 0;
        float gain_multiplier = 1.0f;
        unsigned int integration_ms = 100;

        if (sscanf(line, " %63[^|]|%lu|%lu|%lu|%f|%u",
                   name, &raw_x, &raw_y, &raw_z, &gain_multiplier, &integration_ms) != 6)
        {
            continue;
        }

        if (ctx->status.state != CAL_STATE_MEASURING)
        {
            break;
        }

        const char* current = auto_cal_get_current_ref_name(ctx);
        if (!current || strcmp(current, name) != 0)
        {
            ESP_LOGW(TAG, "Skipping raw line for '%s' (expected '%s')", name, current ? current : "<none>");
            continue;
        }

        float integration_scale = (float)integration_ms / 100.0f;
        if (gain_multiplier < 0.1f) gain_multiplier = 1.0f;
        if (integration_scale < 0.01f) integration_scale = 1.0f;

        xyz_t xyz = {
            .x = (float)raw_x / (TCS3530_RESP_X * gain_multiplier * integration_scale),
            .y = (float)raw_y / (TCS3530_RESP_Y * gain_multiplier * integration_scale),
            .z = (float)raw_z / (TCS3530_RESP_Z * gain_multiplier * integration_scale),
        };

        esp_err_t ret = auto_cal_submit_measurement(ctx, &xyz);
        if (ret != ESP_OK)
        {
            fclose(f);
            return ret;
        }
    }

    fclose(f);
    return ESP_OK;
}

void auto_cal_reset_defaults(auto_cal_ctx_t* ctx)
{
    if (ctx)
    {
        ctx->params = DEFAULT_PARAMS;
        ctx->best_params = DEFAULT_PARAMS;
        ESP_LOGI(TAG, "Reset parameters to defaults");
    }
}

const char* auto_cal_state_name(cal_state_t state)
{
    switch (state)
    {
        case CAL_STATE_IDLE:
            return "IDLE";
        case CAL_STATE_MEASURING:
            return "MEASURING";
        case CAL_STATE_OPTIMIZING:
            return "OPTIMIZING";
        case CAL_STATE_COMPLETE:
            return "COMPLETE";
        case CAL_STATE_ERROR:
            return "ERROR";
        default:
            return "UNKNOWN";
    }
}
