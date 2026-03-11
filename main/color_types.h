/**
 * @file color_types.h
 * @brief Unified color type system for TCS3530 color sensor
 *
 * This file defines the unified type system for color handling across
 * all color spaces (XYZ, Lab, LCH, RGB). It eliminates the previous
 * chaos of 7+ overlapping color structures.
 *
 * CANONICAL COLOR SCIENCE CONSTANTS:
 * All color processing code should use the constants defined here
 * to ensure consistency across the codebase.
 */

#ifndef COLOR_TYPES_H
#define COLOR_TYPES_H

#include <stdint.h>
#include <stdbool.h>
#include <math.h>

#ifdef __cplusplus
extern "C" {
#endif

//===========================================================================
// CANONICAL COLOR SCIENCE CONSTANTS
//
// These are the single source of truth for all color science constants.
// Do NOT redefine these values elsewhere in the codebase.
//===========================================================================

/**
 * @defgroup D65_Constants CIE D65 Standard Illuminant
 * @brief Noon daylight reference (6504K CCT)
 *
 * D65 represents average daylight and is the standard reference white
 * for sRGB color space and most computer graphics applications.
 * Values normalized to Y=100.
 * @{
 */
#define D65_X 95.047f
#define D65_Y 100.0f
#define D65_Z 108.883f
//@}

/**
 * @defgroup D50_Constants CIE D50 Standard Illuminant
 * @brief Horizon daylight reference (5003K CCT)
 *
 * D50 is commonly used in printing and prepress applications.
 * Values normalized to Y=100.
 * @{
 */
#define D50_X 96.422f
#define D50_Y 100.0f
#define D50_Z 82.521f
//@}

/**
 * @defgroup CIE_Lab_Constants CIE Lab Conversion Constants
 * @brief Constants for XYZ <-> Lab conversions per CIE standards
 *
 * These values come from the CIE 1976 L*a*b* color space specification.
 * - CIE_EPSILON = (6/29)^3 ~ 0.008856 (cube of the linear/nonlinear threshold)
 * - CIE_KAPPA = (29/3)^3 ~ 903.3 (slope of linear segment)
 * - CIE_DELTA = 6/29 ~ 0.20689655 (linear/nonlinear transition point)
 * @{
 */
#define CIE_EPSILON 0.008856f
#define CIE_KAPPA   903.3f
#define CIE_DELTA   0.20689655f
//@}

/**
 * @defgroup sRGB_Constants sRGB Gamma Constants
 * @brief Constants for sRGB gamma compression/expansion
 *
 * Per IEC 61966-2-1:1999 sRGB specification:
 * - Linear segment: C_linear <= 0.0031308, C_srgb = 12.92 * C_linear
 * - Gamma segment: C_linear > 0.0031308, C_srgb = 1.055 * C_linear^(1/2.4) - 0.055
 * @{
 */
#define SRGB_LINEAR_THRESHOLD 0.0031308f
#define SRGB_LINEAR_SCALE     12.92f
#define SRGB_GAMMA_OFFSET     0.055f
#define SRGB_GAMMA_SCALE      1.055f
#define SRGB_GAMMA_EXPONENT   2.4f
//@}

/**
 * @defgroup TCS3530_Responsivity TCS3530 Spectral Responsivity Constants
 * @brief Responsivity factors for TCS3530 sensor channels
 *
 * DERIVED FROM ACTUAL CALIBRATION DATA (White Reference Raw Values):
 *   X Raw = 32.2M  -> 290.0 (0.9667× Y anchor)
 *   Y Raw = 33.4M  -> 300.0 (Anchor)
 *   Z Raw = 27.9M  -> 250.0 (0.8333× Y anchor)
 *
 * These values ensure that normalized white light produces X≈Y≈Z (1:1:1 ratio).
 * Previous Z value of 1150.0 was too high, causing Z to be severely under-represented
 * and leading to green colors being misidentified as red (Z→0 after CCM clamping).
 * @{
 */
#define TCS3530_RESP_X 290.0f
#define TCS3530_RESP_Y 300.0f
#define TCS3530_RESP_Z 250.0f
//@}

/**
 * @defgroup XYZ_Processing XYZ Output Scaling Constants
 * @brief Output scale factor for XYZ values after color correction
 *
 * This scale factor is applied AFTER the Polynomial Color Correction Matrix (PCCM)
 * to normalize XYZ values to the expected range for Lab conversion.
 * The value is optimized for overall color accuracy with hardware gain balancing.
 *
 * CRITICAL: This value must be applied consistently in both:
 * - Runtime color pipeline (color_pipeline.cpp)
 * - Calibration optimizer (auto_calibrate.c)
 * @{
 */
#define XYZ_OUTPUT_SCALE 0.11f
//@}

/**
 * @brief XYZ color structure
 *
 * CIE XYZ tristimulus values (0-100 range typically)
 */
typedef struct
{
    float x;
    float y;
    float z;
} xyz_t;

/**
 * @brief Lab color structure
 *
 * CIE L*a*b* perceptual color space
 * L* (0-100), a* and b* (-128 to 127)
 */
typedef struct
{
    float l;
    float a;
    float b;
} lab_t;

/**
 * @brief Calculate chroma from Lab color
 *
 * Chroma is the colorfulness of a color, computed as sqrt(a² + b²).
 * Uses hypotf() for better numerical stability than manual sqrt(a*a + b*b).
 *
 * @param lab Pointer to Lab color structure
 * @return Chroma value (C*), or 0.0f if lab is NULL
 */
static inline float color_math_chroma(const lab_t* lab)
{
    if (!lab) return 0.0f;
    return hypotf(lab->a, lab->b);
}

/**
 * @brief LCH color structure
 *
 * Cylindrical Lab (Lightness, Chroma, Hue)
 * Hue in degrees (0-360)
 */
typedef struct
{
    float l;
    float c;
    float h;
} lch_t;

/**
 * @brief RGB color structure
 *
 * RGB color space (for display only)
 * Values in 0.0-1.0 range
 */
typedef struct
{
    float r;
    float g;
    float b;
} rgb_t;

/**
 * @brief Sensor-specific raw data (separate from color representation)
 *
 * Keeps raw sensor data separate from processed color values,
 * with metadata for quality assessment.
 *
 * Note: Channel data uses uint32_t for 32-bit sensor readings.
 */
typedef struct
{
    uint32_t x, y, z;        //< Tristimulus values (raw ADC counts, 32-bit)
    uint32_t ir;             //< Infrared channel
    uint32_t clear;          //< Broadband clear channel
    uint32_t hgl, hgh;       //< Mercury line detection channels
    uint32_t flicker;        //< Flicker detection value
    uint32_t timestamp_us;   //< Capture timestamp in microseconds
    uint8_t gain;            //< Applied gain setting
    uint16_t integration_ms; //< Integration time in milliseconds
    uint8_t status2;         //< STATUS2 snapshot (ALS digital/analog saturation summary)
    uint8_t status6;         //< STATUS6 snapshot (per-modulator analog saturation bits)
    bool saturated;          //< True if sensor was saturated during reading
} sensor_reading_t;

/**
 * @brief Illuminant type identifier
 */
typedef enum
{
    ILLUMINANT_UNKNOWN,
    ILLUMINANT_D50,     //< Horizon daylight (5000K)
    ILLUMINANT_D55,     //< Mid-morning daylight (5500K)
    ILLUMINANT_D65,     //< Noon daylight (6500K)
    ILLUMINANT_D75,     //< North sky daylight (7500K)
    ILLUMINANT_A,       //< Incandescent tungsten (2856K)
    ILLUMINANT_F2,      //< Cool white fluorescent
    ILLUMINANT_F7,      //< Daylight fluorescent
    ILLUMINANT_F11,     //< Narrow band fluorescent
    ILLUMINANT_LED_B3,  //< Warm white LED
    ILLUMINANT_LED_B5,  //< Neutral white LED
    ILLUMINANT_CUSTOM   //< User-defined illuminant
} illuminant_type_t;

/**
 * @brief Illuminant characteristics
 */
typedef struct
{
    illuminant_type_t type;
    float confidence;           //< Detection confidence (0.0-1.0)

    // Illuminant characteristics
    float cct;                  //< Correlated color temperature (K)
    float duv;                  //< Distance from blackbody locus
    xyz_t white_point;          //< XYZ white point

    // Chromatic adaptation matrix (Bradford transform)
    float adaptation_matrix[3][3];
} illuminant_t;

/**
 * @brief Unified calibration parameters structure
 * 
 * Shared between color pipeline and auto-calibration system.
 * Contains all parameters needed for color correction.
 */
typedef struct
{
    // Polynomial Color Correction Matrix (3x10)
    // 10 terms: R, G, B, R², G², B², RG, RB, GB, 1
    float pccm[3][10];

    // Lightness correction (L_corrected = scale * L + offset)
    float lightness_scale;
    float lightness_offset;

    // Lightness gamma correction (L_gamma = 100 * pow(L/100, gamma))
    // gamma > 1.0 compresses mid-tones (makes them darker)
    // gamma < 1.0 expands mid-tones (makes them brighter)
    float lightness_gamma;
    
    // Piecewise lightness gamma correction
    // Allows different gamma values for dark vs light regions to better fit sensor response
    // If lightness_transition > 0, piecewise gamma is enabled
    float lightness_gamma_dark;      // Gamma for L* < lightness_transition
    float lightness_gamma_light;     // Gamma for L* >= lightness_transition
    float lightness_transition;      // L* value where transition occurs (0 = disabled)

    // Blue region Gaussian correction
    float blue_correction_magnitude;
    float blue_correction_center;
    float blue_correction_width;

    // Saturation boost for vivid colors
    float saturation_boost;

    // Thresholds for color categorization
    float gray_threshold;
    float color_threshold;

    // Black level offset (raw XYZ values before normalization)
    xyz_t black_level;
    
    // Flag indicating if black level calibration was performed
    bool has_black_calibration;
} color_calib_params_t;

//===========================================================================
// TCS3530 GAIN MULTIPLIER CONSTANTS
//
// Centralized gain multiplier lookup for TCS3530 sensor.
// Used by both the driver and color processing code for consistent scaling.
//===========================================================================

/**
 * @brief Maximum gain code for TCS3530 (TCS3530_GAIN_4096X = 13)
 */
#define TCS3530_MAX_GAIN_CODE 13

/**
 * @brief Convert TCS3530 gain code to multiplier value
 *
 * Gain multipliers for TCS3530 sensor (index corresponds to gain code 0-13):
 *   0: 0.5x, 1: 1x, 2: 2x, 3: 4x, 4: 8x, 5: 16x, 6: 32x, 7: 64x,
 *   8: 128x, 9: 256x, 10: 512x, 11: 1024x, 12: 2048x, 13: 4096x
 *
 * @param gain_code Gain code (0-13)
 * @return Gain multiplier, or 128.0f (default) for invalid codes
 */
static inline float tcs3530_gain_code_to_multiplier(uint8_t gain_code)
{
    // Lookup table is declared inside the function to avoid separate copies
    // in each translation unit. The compiler will optimize this for inline.
    static const float gain_multipliers[] =
    {
        0.5f, 1.0f, 2.0f, 4.0f, 8.0f, 16.0f, 32.0f, 64.0f,
        128.0f, 256.0f, 512.0f, 1024.0f, 2048.0f, 4096.0f
    };

    if (gain_code <= TCS3530_MAX_GAIN_CODE)
    {
        return gain_multipliers[gain_code];
    }
    return 128.0f; // Default to 128x for invalid codes
}

#ifdef __cplusplus
}
#endif

#endif // COLOR_TYPES_H
