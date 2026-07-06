/**
 * @file color_math.cpp
 * @brief Core color math functions - the single source of truth for color conversions
 *
 * This file contains all fundamental color math operations using direct computation.
 * Modern FPUs handle these calculations efficiently, reducing cache pressure from
 * large lookup tables.
 */

#include "color_math.h"
#include "tcs_glue.h"
#include "color_types.h"
#include <math.h>
#include <float.h>
#include <string.h>

static const char* TAG = "color_math";

// Tolerance for gamma == 1.0 check to handle floating-point precision
#define GAMMA_UNITY_TOLERANCE 1e-6f

// Minimum base value for pow() to avoid issues with fractional exponents
#define MIN_POW_BASE 1e-10f

// Saturation enhancement thresholds
#define MIN_CHROMA_FOR_SATURATION 0.01f   // Skip saturation enhancement for achromatic colors
#define MIN_RANGE_THRESHOLD 0.01f         // Minimum range for saturation ramping

// Piecewise gamma blend width (L* units)
// Width of the smooth transition zone between dark and light gamma values
#define LIGHTNESS_GAMMA_BLEND_WIDTH 15.0f  // Blend over ±15 L* units from transition point

//===========================================================================
// sRGB <-> XYZ Conversion Matrices (D65 illuminant)
//
// These are the standard IEC 61966-2-1:1999 sRGB primaries matrices.
// RGB to XYZ matrix (linear RGB -> CIE XYZ D65)
// XYZ to RGB matrix is the inverse
//===========================================================================

// XYZ to sRGB (D65) - inverse of sRGB to XYZ matrix
static const float XYZ_TO_SRGB[3][3] =
{
    {  3.2404542f, -1.5371385f, -0.4985314f },
    { -0.9692660f,  1.8760108f,  0.0415560f },
    {  0.0556434f, -0.2040259f,  1.0572252f }
};

//===========================================================================
// sRGB Gamma Functions (using constants from color_types.h)
// Direct computation - modern FPUs handle powf() efficiently
//===========================================================================

/**
 * @brief sRGB gamma compression (linear -> sRGB)
 * Uses SRGB_* constants from color_types.h
 */
static inline float gamma_compress(float linear)
{
    if (linear <= SRGB_LINEAR_THRESHOLD)
    {
        return SRGB_LINEAR_SCALE * linear;
    }
    else
    {
        return SRGB_GAMMA_SCALE * powf(linear, 1.0f / SRGB_GAMMA_EXPONENT) - SRGB_GAMMA_OFFSET;
    }
}

/**
 * @brief CIE Lab f(t) function
 * Uses CIE_EPSILON and CIE_KAPPA from color_types.h
 * Direct computation using cbrtf() - efficient on modern FPUs
 */
static inline float lab_f(float t)
{
    if (t > CIE_EPSILON)
    {
        return cbrtf(t);
    }
    else
    {
        return (CIE_KAPPA * t + 16.0f) / 116.0f;
    }
}

/**
 * @brief Inverse of CIE Lab f(t) function
 */
static inline float lab_f_inv(float t)
{
    float t3 = t * t * t;
    if (t3 > CIE_EPSILON)
    {
        return t3;
    }
    else
    {
        return (116.0f * t - 16.0f) / CIE_KAPPA;
    }
}

//===========================================================================
// XYZ <-> Lab Conversion (using D65 constants from color_types.h)
//===========================================================================

lab_t color_math_xyz_to_lab(xyz_t xyz)
{
    // Use canonical D65 reference white from color_types.h
    float fx = lab_f(xyz.x / D65_X);
    float fy = lab_f(xyz.y / D65_Y);
    float fz = lab_f(xyz.z / D65_Z);

    lab_t lab;
    lab.l = 116.0f * fy - 16.0f;
    lab.a = 500.0f * (fx - fy);
    lab.b = 200.0f * (fy - fz);

    return lab;
}

xyz_t color_math_lab_to_xyz(lab_t lab)
{
    // Use canonical D65 reference white from color_types.h
    float fy = (lab.l + 16.0f) / 116.0f;
    float fx = lab.a / 500.0f + fy;
    float fz = fy - lab.b / 200.0f;

    xyz_t xyz;
    xyz.x = D65_X * lab_f_inv(fx);
    xyz.y = D65_Y * lab_f_inv(fy);
    xyz.z = D65_Z * lab_f_inv(fz);

    return xyz;
}

void color_math_xyz_to_rgb(xyz_t xyz, uint8_t *r, uint8_t *g, uint8_t *b)
{
    // Call the float version and convert to uint8
    rgb_t rgb;
    color_math_xyz_to_rgb_float(&xyz, &rgb);

    *r = (uint8_t)(rgb.r * 255.0f + 0.5f);
    *g = (uint8_t)(rgb.g * 255.0f + 0.5f);
    *b = (uint8_t)(rgb.b * 255.0f + 0.5f);
}

void color_math_lab_to_rgb(lab_t lab, uint8_t *r, uint8_t *g, uint8_t *b)
{
    xyz_t xyz = color_math_lab_to_xyz(lab);
    color_math_xyz_to_rgb(xyz, r, g, b);
}

// CIEDE2000 implementation based on Sharma et al. 2005
// Optimized: replaced powf(x, 7) with manual multiplication for performance

/**
 * @brief Compute x^7 using optimized manual multiplication
 *
 * powf(x, 7) is expensive. Using x2=x*x, x4=x2*x2, x7=x4*x2*x is faster.
 */
static inline float pow7(float x)
{
    float x2 = x * x;
    float x4 = x2 * x2;
    return x4 * x2 * x;  // x^4 * x^2 * x = x^7
}

float color_math_delta_e_ciede2000(const lab_t *lab1, const lab_t *lab2)
{
    // Validate inputs
    if (!lab1 || !lab2)
    {
        ESP_LOGE(TAG, "color_math_delta_e_ciede2000: null pointer argument");
        return FLT_MAX;  // Return maximum float to indicate error
    }
    
    float L1 = lab1->l, a1 = lab1->a, b1 = lab1->b;
    float L2 = lab2->l, a2 = lab2->a, b2 = lab2->b;

    // Weighting factors (set to 1.0 for standard CIEDE2000)
    static const float kL = 1.0f;
    static const float kC = 1.0f;
    static const float kH = 1.0f;

    // Pre-computed 25^7 constant (6103515625) - static const ensures compile-time computation
    static const float POW25_7 = 6103515625.0f;

    float C1 = sqrtf(a1*a1 + b1*b1);
    float C2 = sqrtf(a2*a2 + b2*b2);
    float C_ave = (C1 + C2) / 2.0f;

    float C_ave_pow7 = pow7(C_ave);
    float G = 0.5f * (1.0f - sqrtf(C_ave_pow7 / (C_ave_pow7 + POW25_7)));
    float a1p = a1 * (1.0f + G);
    float a2p = a2 * (1.0f + G);

    float C1p = sqrtf(a1p*a1p + b1*b1);
    float C2p = sqrtf(a2p*a2p + b2*b2);

    float h1p = atan2f(b1, a1p);
    if (h1p < 0)
    {
        h1p += 2.0f * M_PI;
    }
    float h2p = atan2f(b2, a2p);
    if (h2p < 0)
    {
        h2p += 2.0f * M_PI;
    }

    float dLp = L2 - L1;
    float dCp = C2p - C1p;

    float dhp = 0.0f;
    if (C1p * C2p == 0.0f)
    {
        dhp = 0.0f;
    }
    else if (fabsf(h2p - h1p) <= M_PI)
    {
        dhp = h2p - h1p;
    }
    else if (h2p - h1p > M_PI)
    {
        dhp = h2p - h1p - 2.0f * M_PI;
    }
    else
    {
        dhp = h2p - h1p + 2.0f * M_PI;
    }

    float dHp = 2.0f * sqrtf(C1p * C2p) * sinf(dhp / 2.0f);

    float Lp_ave = (L1 + L2) / 2.0f;
    float Cp_ave = (C1p + C2p) / 2.0f;

    float hp_ave = 0.0f;
    if (C1p * C2p == 0.0f)
    {
        hp_ave = h1p + h2p;
    }
    else if (fabsf(h1p - h2p) <= M_PI)
    {
        hp_ave = (h1p + h2p) / 2.0f;
    }
    else if (h1p + h2p < 2.0f * M_PI)
    {
        hp_ave = (h1p + h2p + 2.0f * M_PI) / 2.0f;
    }
    else
    {
        hp_ave = (h1p + h2p - 2.0f * M_PI) / 2.0f;
    }

    float T = 1.0f - 0.17f * cosf(hp_ave - M_PI/6.0f) +
              0.24f * cosf(2.0f * hp_ave) +
              0.32f * cosf(3.0f * hp_ave + M_PI/30.0f) -
              0.20f * cosf(4.0f * hp_ave - 21.0f * M_PI/60.0f);

    float hp_ave_term = (hp_ave * 180.0f/M_PI - 275.0f) / 25.0f;
    float dtheta = 30.0f * M_PI/180.0f * expf(-(hp_ave_term * hp_ave_term));

    float Cp_ave_pow7 = pow7(Cp_ave);
    float Rc = 2.0f * sqrtf(Cp_ave_pow7 / (Cp_ave_pow7 + POW25_7));

    float Lp_ave_term = Lp_ave - 50.0f;
    float Lp_ave_term_sq = Lp_ave_term * Lp_ave_term;
    float Sl = 1.0f + (0.015f * Lp_ave_term_sq) / sqrtf(20.0f + Lp_ave_term_sq);
    float Sc = 1.0f + 0.045f * Cp_ave;
    float Sh = 1.0f + 0.015f * Cp_ave * T;
    float Rt = -sinf(2.0f * dtheta) * Rc;

    float dLp_term = dLp / (kL * Sl);
    float dCp_term = dCp / (kC * Sc);
    float dHp_term = dHp / (kH * Sh);

    float dE = sqrtf(
                   dLp_term * dLp_term +
                   dCp_term * dCp_term +
                   dHp_term * dHp_term +
                   Rt * dCp_term * dHp_term
               );

    return dE;
}

// Bradford chromatic adaptation matrices - exposed as public constants
const float BRADFORD_MA[3][3] =
{
    { 0.8951f,  0.2664f, -0.1614f},
    {-0.7502f,  1.7135f,  0.0367f},
    { 0.0389f, -0.0685f,  1.0296f}
};

const float BRADFORD_MA_INV[3][3] =
{
    { 0.9870f, -0.1471f,  0.1600f},
    { 0.4323f,  0.5184f,  0.0493f},
    {-0.0085f,  0.0400f,  0.9685f}
};

xyz_t color_math_apply_ccm(const xyz_t* xyz, const float ccm[3][3])
{
    // Validate input
    if (!xyz || !ccm)
    {
        ESP_LOGE(TAG, "color_math_apply_ccm: null pointer argument");
        return {0.0f, 0.0f, 0.0f};
    }
    
    xyz_t result;
    result.x = ccm[0][0] * xyz->x + ccm[0][1] * xyz->y + ccm[0][2] * xyz->z;
    result.y = ccm[1][0] * xyz->x + ccm[1][1] * xyz->y + ccm[1][2] * xyz->z;
    result.z = ccm[2][0] * xyz->x + ccm[2][1] * xyz->y + ccm[2][2] * xyz->z;
    
    // Clamp negative values
    if (result.x < 0.0f) result.x = 0.0f;
    if (result.y < 0.0f) result.y = 0.0f;
    if (result.z < 0.0f) result.z = 0.0f;
    
    return result;
}

xyz_t color_math_apply_pccm(const xyz_t* xyz, const float pccm[3][10])
{
    // Validate input
    if (!xyz || !pccm)
    {
        ESP_LOGE(TAG, "color_math_apply_pccm: null pointer argument");
        return {0.0f, 0.0f, 0.0f};
    }
    
    // Input values (normalized to 0-1 range for polynomial terms)
    float r = xyz->x / 100.0f;
    float g = xyz->y / 100.0f;
    float b = xyz->z / 100.0f;
    
    // Polynomial terms: R, G, B, R², G², B², RG, RB, GB, 1
    float terms[10];
    terms[0] = r;
    terms[1] = g;
    terms[2] = b;
    terms[3] = r * r;
    terms[4] = g * g;
    terms[5] = b * b;
    terms[6] = r * g;
    terms[7] = r * b;
    terms[8] = g * b;
    terms[9] = 1.0f;
    
    // Apply polynomial transformation
    xyz_t result;
    result.x = 0.0f;
    result.y = 0.0f;
    result.z = 0.0f;
    
    for (int i = 0; i < 10; i++)
    {
        result.x += pccm[0][i] * terms[i];
        result.y += pccm[1][i] * terms[i];
        result.z += pccm[2][i] * terms[i];
    }
    
    // Scale back to 0-100 range
    result.x *= 100.0f;
    result.y *= 100.0f;
    result.z *= 100.0f;
    
    // Clamp negative values
    if (result.x < 0.0f) result.x = 0.0f;
    if (result.y < 0.0f) result.y = 0.0f;
    if (result.z < 0.0f) result.z = 0.0f;
    
    return result;
}

float color_math_correct_lightness(float L_measured, const color_calib_params_t* params)
{
    // Validate input
    if (!params)
    {
        ESP_LOGE(TAG, "color_math_correct_lightness: null params pointer");
        return L_measured;  // Return input unchanged if params is null
    }
    
    // Apply linear correction and blue correction
    float distance_from_blue = (L_measured - params->blue_correction_center) / params->blue_correction_width;
    float blue_correction = params->blue_correction_magnitude * expf(-distance_from_blue * distance_from_blue);
    
    float L_linear = params->lightness_scale * L_measured + params->lightness_offset + blue_correction;
    
    // Clamp to valid range before gamma
    L_linear = fmaxf(0.0f, fminf(100.0f, L_linear));
    
    // Apply gamma correction: L_gamma = 100 * pow(L_linear/100, gamma)
    // gamma > 1.0 compresses mid-tones (makes them darker)
    // gamma < 1.0 expands mid-tones (makes them brighter)
    // Supports both single gamma and piecewise gamma (for better fitting of sensor response)
    float L_corrected;
    
    // Check if piecewise gamma is enabled (transition > 0)
    // Also ensure L_linear is in a valid range for comparison
    if (params->lightness_transition > 0.0f && L_linear >= 0.0f && L_linear <= 100.0f)
    {
        // Piecewise gamma with smooth blend transition
        // Define blend width (how wide the transition zone is)
        
        float transition_center = params->lightness_transition;
        float blend_start = transition_center - LIGHTNESS_GAMMA_BLEND_WIDTH;
        float blend_end = transition_center + LIGHTNESS_GAMMA_BLEND_WIDTH;
        
        float blend_factor;
        if (L_linear <= blend_start)
        {
            // Fully in dark region
            blend_factor = 0.0f;
        }
        else if (L_linear >= blend_end)
        {
            // Fully in light region
            blend_factor = 1.0f;
        }
        else
        {
            // In transition zone - use smoothstep for smooth interpolation
            float t = (L_linear - blend_start) / (blend_end - blend_start);
            // Smoothstep: 3t² - 2t³ (has zero derivative at endpoints)
            blend_factor = t * t * (3.0f - 2.0f * t);
        }
        
        // Blend between the two gamma values
        float gamma_to_use = params->lightness_gamma_dark * (1.0f - blend_factor) 
                           + params->lightness_gamma_light * blend_factor;
        
        // Apply selected gamma (optimize for gamma ≈ 1.0)
        if (fabsf(gamma_to_use - 1.0f) < GAMMA_UNITY_TOLERANCE)
        {
            L_corrected = L_linear;
        }
        else
        {
            float normalized = fmaxf(L_linear / 100.0f, MIN_POW_BASE);
            L_corrected = 100.0f * powf(normalized, gamma_to_use);
        }
    }
    else
    {
        // Single gamma mode (legacy/fallback)
        // Optimize for the common case where gamma is approximately 1.0 (no correction)
        if (fabsf(params->lightness_gamma - 1.0f) < GAMMA_UNITY_TOLERANCE)
        {
            L_corrected = L_linear;
        }
        else
        {
            // Protect against pow(0, fractional) which can cause issues with gamma < 1
            float normalized = fmaxf(L_linear / 100.0f, MIN_POW_BASE);
            L_corrected = 100.0f * powf(normalized, params->lightness_gamma);
        }
    }
    
    // Final clamp to valid range
    return fmaxf(0.0f, fminf(100.0f, L_corrected));
}

xyz_t color_math_chromatic_adapt(xyz_t xyz, xyz_t src_white, xyz_t dst_white)
{
    // Epsilon to prevent division by zero in cone response domain
    const float EPSILON = 1e-10f;

    // Convert XYZ to cone response domain using Bradford matrix
    float src_rgb[3] =
    {
        BRADFORD_MA[0][0] * src_white.x + BRADFORD_MA[0][1] * src_white.y + BRADFORD_MA[0][2] * src_white.z,
        BRADFORD_MA[1][0] * src_white.x + BRADFORD_MA[1][1] * src_white.y + BRADFORD_MA[1][2] * src_white.z,
        BRADFORD_MA[2][0] * src_white.x + BRADFORD_MA[2][1] * src_white.y + BRADFORD_MA[2][2] * src_white.z
    };

    float dst_rgb[3] =
    {
        BRADFORD_MA[0][0] * dst_white.x + BRADFORD_MA[0][1] * dst_white.y + BRADFORD_MA[0][2] * dst_white.z,
        BRADFORD_MA[1][0] * dst_white.x + BRADFORD_MA[1][1] * dst_white.y + BRADFORD_MA[1][2] * dst_white.z,
        BRADFORD_MA[2][0] * dst_white.x + BRADFORD_MA[2][1] * dst_white.y + BRADFORD_MA[2][2] * dst_white.z
    };

    // Calculate scale factors with epsilon protection against division by zero
    float scale[3];
    for (int i = 0; i < 3; i++)
    {
        if (fabsf(src_rgb[i]) < EPSILON)
        {
            // Protect against division by zero - use 1.0 scale if source is near-zero
            scale[i] = 1.0f;
        }
        else
        {
            scale[i] = dst_rgb[i] / src_rgb[i];
        }
    }

    // Transform input color to cone response
    float rgb[3] =
    {
        BRADFORD_MA[0][0] * xyz.x + BRADFORD_MA[0][1] * xyz.y + BRADFORD_MA[0][2] * xyz.z,
        BRADFORD_MA[1][0] * xyz.x + BRADFORD_MA[1][1] * xyz.y + BRADFORD_MA[1][2] * xyz.z,
        BRADFORD_MA[2][0] * xyz.x + BRADFORD_MA[2][1] * xyz.y + BRADFORD_MA[2][2] * xyz.z
    };

    // Apply scaling
    rgb[0] *= scale[0];
    rgb[1] *= scale[1];
    rgb[2] *= scale[2];

    // Transform back to XYZ
    xyz_t result;
    result.x = BRADFORD_MA_INV[0][0] * rgb[0] + BRADFORD_MA_INV[0][1] * rgb[1] + BRADFORD_MA_INV[0][2] * rgb[2];
    result.y = BRADFORD_MA_INV[1][0] * rgb[0] + BRADFORD_MA_INV[1][1] * rgb[1] + BRADFORD_MA_INV[1][2] * rgb[2];
    result.z = BRADFORD_MA_INV[2][0] * rgb[0] + BRADFORD_MA_INV[2][1] * rgb[1] + BRADFORD_MA_INV[2][2] * rgb[2];

    return result;
}

//===========================================================================
// Additional Consolidated Functions (ported from color_conversion.cpp)
//===========================================================================

esp_err_t color_math_xyz_to_lab_illuminant(const xyz_t* xyz, lab_t* lab,
        const illuminant_t* illuminant)
{
    if (!xyz || !lab)
    {
        return ESP_ERR_INVALID_ARG;
    }

    // Use D65 if no illuminant specified
    float ref_x = D65_X;
    float ref_y = D65_Y;
    float ref_z = D65_Z;

    if (illuminant)
    {
        ref_x = illuminant->white_point.x;
        ref_y = illuminant->white_point.y;
        ref_z = illuminant->white_point.z;
    }

    // Normalize with reference white
    float x_rel = xyz->x / ref_x;
    float y_rel = xyz->y / ref_y;
    float z_rel = xyz->z / ref_z;

    // Apply nonlinear compression
    float fx = lab_f(x_rel);
    float fy = lab_f(y_rel);
    float fz = lab_f(z_rel);

    // Calculate L*a*b* values
    lab->l = 116.0f * fy - 16.0f;
    lab->a = 500.0f * (fx - fy);
    lab->b = 200.0f * (fy - fz);

    // Ensure L* is in valid range [0, 100]
    lab->l = fminf(fmaxf(lab->l, 0.0f), 100.0f);

    return ESP_OK;
}

lch_t color_math_lab_to_lch(lab_t lab)
{
    lch_t lch;

    lch.l = lab.l;
    lch.c = hypotf(lab.a, lab.b);
    lch.h = atan2f(lab.b, lab.a) * 180.0f / (float)M_PI;

    // Normalize hue to 0-360 degrees
    if (lch.h < 0)
    {
        lch.h += 360.0f;
    }

    return lch;
}

lab_t color_math_lch_to_lab(lch_t lch)
{
    lab_t lab;

    float h_rad = lch.h * (float)M_PI / 180.0f;

    lab.l = lch.l;
    lab.a = lch.c * cosf(h_rad);
    lab.b = lch.c * sinf(h_rad);

    return lab;
}

float color_math_enhance_saturation(lab_t* lab, float gray_threshold,
                                    float color_threshold, float boost_factor)
{
    if (!lab)
    {
        return 0.0f;
    }

    float C = hypotf(lab->a, lab->b);
    
    if (C < MIN_CHROMA_FOR_SATURATION) return 0.0f;  // Skip achromatic
    
    // Calculate saturation scale with smooth transition
    // No hue-dependent boost - use uniform boost factor for all colors
    float scale = 1.0f;
    float range = color_threshold - gray_threshold;
    
    if (C > color_threshold)
    {
        scale = boost_factor;
    }
    else if (C > gray_threshold && range > MIN_RANGE_THRESHOLD)
    {
        float t = (C - gray_threshold) / range;
        // Smoothstep for gradual transition
        float smooth_t = t * t * (3.0f - 2.0f * t);
        scale = 1.0f + smooth_t * (boost_factor - 1.0f);
    }
    else
    {
        // Below gray threshold - snap to neutral
        lab->a = 0.0f;
        lab->b = 0.0f;
        return 0.0f;
    }
    
    lab->a *= scale;
    lab->b *= scale;
    
    return C * scale;
}

esp_err_t color_math_xyz_to_rgb_float(const xyz_t* xyz, rgb_t* rgb)
{
    if (!xyz || !rgb)
    {
        return ESP_ERR_INVALID_ARG;
    }

    // Normalize XYZ to 0-1 range first
    float x = xyz->x / 100.0f;
    float y = xyz->y / 100.0f;
    float z = xyz->z / 100.0f;

    // XYZ to linear RGB using sRGB D65 matrix
    float r_linear = XYZ_TO_SRGB[0][0] * x + XYZ_TO_SRGB[0][1] * y + XYZ_TO_SRGB[0][2] * z;
    float g_linear = XYZ_TO_SRGB[1][0] * x + XYZ_TO_SRGB[1][1] * y + XYZ_TO_SRGB[1][2] * z;
    float b_linear = XYZ_TO_SRGB[2][0] * x + XYZ_TO_SRGB[2][1] * y + XYZ_TO_SRGB[2][2] * z;

    // Apply gamma compression and clamp to [0, 1] range
    rgb->r = fminf(fmaxf(gamma_compress(r_linear), 0.0f), 1.0f);
    rgb->g = fminf(fmaxf(gamma_compress(g_linear), 0.0f), 1.0f);
    rgb->b = fminf(fmaxf(gamma_compress(b_linear), 0.0f), 1.0f);

    return ESP_OK;
}

//===========================================================================
// MATERIAL-AWARE COLOR CORRECTION
//===========================================================================

/**
 * @brief Default correction factors for each material type
 * 
 * Empirically derived from the ChatGPT discussion on material-aware correction:
 * 
 * - FABRIC: Fibers scatter light (subsurface + directional), shadows between
 *   threads lead to darker and more desaturated readings than perceived.
 *   Correction: L *= 1.10, L += 2, a *= 1.05, b *= 1.05
 * 
 * - PLASTIC: Semi-gloss surfaces may have specular highlights biasing toward
 *   white, causing slightly elevated lightness readings.
 *   Correction: L *= 0.98 (mild reduction)
 * 
 * - METAL: Strong specular reflection makes colors read artificially bright
 *   or washed out.
 *   Correction: L *= 0.90, a *= 0.95, b *= 0.95
 */
static const material_correction_t MATERIAL_CORRECTIONS[] = {
    // MATERIAL_UNKNOWN - identity (no correction)
    { .l_scale = 1.0f, .l_offset = 0.0f, .a_scale = 1.0f, .b_scale = 1.0f },
    
    // MATERIAL_FABRIC - lift shadows, restore muted chroma
    { .l_scale = 1.10f, .l_offset = 2.0f, .a_scale = 1.05f, .b_scale = 1.05f },
    
    // MATERIAL_PLASTIC - mild lightness reduction
    { .l_scale = 0.98f, .l_offset = 0.0f, .a_scale = 1.0f, .b_scale = 1.0f },
    
    // MATERIAL_METAL - suppress specular bias
    { .l_scale = 0.90f, .l_offset = 0.0f, .a_scale = 0.95f, .b_scale = 0.95f },
    
    // MATERIAL_DEFAULT - identity (no correction)
    { .l_scale = 1.0f, .l_offset = 0.0f, .a_scale = 1.0f, .b_scale = 1.0f },
};

static const char* MATERIAL_NAMES[] = {
    "Unknown",
    "Fabric",
    "Plastic",
    "Metal",
    "Default"
};

material_correction_t color_math_get_material_correction(material_type_t material)
{
    if (material <= MATERIAL_DEFAULT)
    {
        return MATERIAL_CORRECTIONS[material];
    }
    // Fallback to identity if out of range
    return MATERIAL_CORRECTIONS[MATERIAL_DEFAULT];
}

lab_t color_math_apply_material_correction(const lab_t* lab, const material_correction_t* correction)
{
    lab_t result;
    
    if (!lab || !correction)
    {
        ESP_LOGE(TAG, "color_math_apply_material_correction: null pointer argument");
        return (lab_t){0.0f, 0.0f, 0.0f};
    }
    
    // Apply correction: L' = L * scale + offset, a' = a * scale, b' = b * scale
    result.l = lab->l * correction->l_scale + correction->l_offset;
    result.a = lab->a * correction->a_scale;
    result.b = lab->b * correction->b_scale;
    
    // Clamp lightness to valid range
    result.l = fminf(fmaxf(result.l, 0.0f), 100.0f);
    
    return result;
}

material_type_t color_math_classify_material(const sensor_reading_t* reading,
                                             const xyz_t* variance_xyz,
                                             const xyz_t* mean_xyz)
{
    if (!reading)
    {
        return MATERIAL_DEFAULT;
    }

    // Extract sensor characteristics for classification heuristics
    // Based on ChatGPT discussion: use variance, saturation, reflectance patterns

    // Minimum sensor value to prevent division by zero
    static const float MIN_SENSOR_VALUE = 1.0f;

    // Calculate approximate saturation from raw channel ratios
    // Higher X relative to Y suggests redder, higher Z relative to Y suggests bluer
    float x_norm = (float)reading->x;
    float y_norm = (float)reading->y;
    float z_norm = (float)reading->z;
    float clear = (float)reading->clear;

    // Avoid division by zero
    if (y_norm < MIN_SENSOR_VALUE) y_norm = MIN_SENSOR_VALUE;
    if (clear < MIN_SENSOR_VALUE) clear = MIN_SENSOR_VALUE;

    // Reflectance indicator: ratio of clear to sum of color channels, with all
    // channels normalized by their spectral responsivities so the comparison is
    // in a common irradiance domain (raw counts per channel are not comparable
    // because each channel has a different responsivity). The clear channel has
    // no dedicated RESP constant; RESP_Y is used as a proxy.
    // NOTE: the metal/plastic thresholds below are provisional pending
    // characterization with real material measurements.
    float total_color = (x_norm / TCS3530_RESP_X) +
                        (y_norm / TCS3530_RESP_Y) +
                        (z_norm / TCS3530_RESP_Z);
    float clear_ratio = (clear / TCS3530_RESP_Y) / total_color;

    // Chroma indicator: deviation from neutral (equal X:Y:Z)
    float x_ratio = x_norm / y_norm;
    float z_ratio = z_norm / y_norm;
    float max_ratio = fmaxf(x_ratio, z_ratio);
    float min_ratio = fminf(x_ratio, z_ratio);
    float chroma_spread = max_ratio - min_ratio;

    // Variance-based classification (if capture statistics are provided).
    // Compute a coefficient of variation entirely in the corrected-XYZ domain:
    // sqrt(mean channel variance) / mean Y. Both inputs must come from the
    // same capture statistics — dividing a corrected-domain variance by the
    // raw ADC count would produce a meaningless near-zero score.
    float variance_score = 0.0f;
    if (variance_xyz && mean_xyz && mean_xyz->y > 0.01f)
    {
        float mean_variance = (variance_xyz->x + variance_xyz->y + variance_xyz->z) / 3.0f;
        variance_score = sqrtf(fmaxf(0.0f, mean_variance)) / mean_xyz->y;
    }

    // Classification heuristics based on ChatGPT discussion:
    //
    // Fabric:
    //   - High variance across samples (fiber texture)
    //   - Lower saturation than perceived (muted by scattering)
    //   - Moderate clear ratio
    //
    // Metal:
    //   - Very high clear ratio (strong specular reflection)
    //   - Low chroma spread (washed out colors)
    //   - Low variance (smooth surface)
    //
    // Plastic:
    //   - Moderate to high clear ratio
    //   - Moderate chroma
    //   - Low variance

    // Thresholds (empirically tuned, may need adjustment based on real data)
    // VARIANCE_FABRIC_THRESHOLD matches the pipeline's texture-detection CoV
    // threshold (TEXTURE_CV_THRESHOLD in color_pipeline.cpp): flat swatches
    // measure < 5% CoV, textured fabrics 20-40%.
    const float VARIANCE_FABRIC_THRESHOLD = 0.15f;   // High CoV -> fabric
    const float CLEAR_METAL_THRESHOLD = 1.5f;        // Very high clear ratio -> metal (provisional)
    const float CLEAR_PLASTIC_THRESHOLD = 1.2f;      // Moderately high clear ratio -> plastic (provisional)
    const float CHROMA_METAL_THRESHOLD = 0.3f;       // Low chroma spread -> metal candidate

    // Priority-based classification:
    // 1. High variance strongly suggests fabric
    if (variance_score > VARIANCE_FABRIC_THRESHOLD)
    {
        TCS_LOGD(TAG, "Material classified as FABRIC (variance_score=%.4f)", variance_score);
        return MATERIAL_FABRIC;
    }

    // 2. Very high clear ratio + low chroma suggests metal
    if (clear_ratio > CLEAR_METAL_THRESHOLD && chroma_spread < CHROMA_METAL_THRESHOLD)
    {
        TCS_LOGD(TAG, "Material classified as METAL (clear_ratio=%.4f, chroma_spread=%.4f)",
                 clear_ratio, chroma_spread);
        return MATERIAL_METAL;
    }

    // 3. Moderately high clear ratio suggests plastic
    if (clear_ratio > CLEAR_PLASTIC_THRESHOLD)
    {
        TCS_LOGD(TAG, "Material classified as PLASTIC (clear_ratio=%.4f)", clear_ratio);
        return MATERIAL_PLASTIC;
    }

    // 4. Default: assume fabric for this quilting fabric device
    // Since the primary use case is Kona Cotton quilting fabrics,
    // defaulting to fabric correction is appropriate when uncertain
    ESP_LOGI(TAG, "Material classifier fell through to FABRIC default "
             "(variance_score=%.4f clear_ratio=%.4f chroma_spread=%.4f)",
             variance_score, clear_ratio, chroma_spread);
    return MATERIAL_FABRIC;
}

const char* color_math_material_name(material_type_t material)
{
    if (material <= MATERIAL_DEFAULT)
    {
        return MATERIAL_NAMES[material];
    }
    return "Unknown";
}
