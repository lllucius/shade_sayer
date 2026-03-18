#ifndef COLOR_MATH_H
#define COLOR_MATH_H

#include <stdint.h>
#include "color_types.h"
#include "tcs_glue.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Initialize color math module (no-op for backward compatibility)
 *
 * This function is now a no-op as modern FPUs handle direct computation
 * efficiently. LUT-based optimization has been removed to reduce cache
 * pressure and memory usage. Retained for API compatibility.
 *
 * @return ESP_OK always
 */
static inline esp_err_t color_math_init(void)
{
    return ESP_OK;
}

/**
 * Convert XYZ to CIE L*a*b* color space
 * Uses D65 illuminant
 */
lab_t color_math_xyz_to_lab(xyz_t xyz);

/**
 * Convert XYZ to RGB (0-255)
 * Uses sRGB D65 color space
 */
void color_math_xyz_to_rgb(xyz_t xyz, uint8_t *r, uint8_t *g, uint8_t *b);

/**
 * Convert CIE L*a*b* to XYZ color space
 * Uses D65 illuminant
 */
xyz_t color_math_lab_to_xyz(lab_t lab);

/**
 * Convert LAB directly to RGB
 */
void color_math_lab_to_rgb(lab_t lab, uint8_t *r, uint8_t *g, uint8_t *b);

/**
 * Calculate CIEDE2000 color difference (DeltaE2000)
 * Returns perceptual color distance between two LAB colors
 * DeltaE < 1.0: Not perceptible by human eyes
 * DeltaE 1-2: Perceptible through close observation
 * DeltaE 2-10: Perceptible at a glance
 * DeltaE > 10: Colors are more different than similar
 */
float color_math_delta_e_ciede2000(const lab_t *lab1, const lab_t *lab2);

/**
 * Apply chromatic adaptation from one illuminant to another
 * Uses Bradford chromatic adaptation transform
 * @param xyz Source color in XYZ
 * @param src_white Source illuminant XYZ
 * @param dst_white Destination illuminant XYZ
 * @return Adapted XYZ color
 */
xyz_t color_math_chromatic_adapt(xyz_t xyz, xyz_t src_white, xyz_t dst_white);

/**
 * @brief Enhance saturation using adaptive thresholds
 *
 * Applies saturation enhancement to Lab color using LCH conversion.
 * Colors below gray_threshold are forced to pure gray.
 * Colors between gray_threshold and color_threshold are ramped.
 * Colors above color_threshold get full boost_factor applied.
 *
 * @param lab Input/output Lab color (modified in place)
 * @param gray_threshold Chroma below this is snapped to gray (e.g., 5.0)
 * @param color_threshold Chroma above this gets full boost (e.g., 60.0)
 * @param boost_factor Saturation multiplier for vivid colors (e.g., 2.5)
 * @return Final chroma value after enhancement
 */
float color_math_enhance_saturation(lab_t* lab, float gray_threshold,
                                    float color_threshold, float boost_factor);

/**
 * @brief Convert XYZ to Lab with specific illuminant
 *
 * @param xyz Input XYZ color
 * @param lab Output Lab color
 * @param illuminant Reference illuminant for conversion (can be NULL for D65)
 * @return ESP_OK on success
 */
esp_err_t color_math_xyz_to_lab_illuminant(const xyz_t* xyz, lab_t* lab,
        const illuminant_t* illuminant);

/**
 * @brief Convert Lab to LCH (cylindrical coordinates)
 *
 * Returns LCH in a struct (value semantics) for easier use in math operations.
 * Hue is in degrees (0-360).
 *
 * @param lab Input Lab color
 * @return LCH color
 */
lch_t color_math_lab_to_lch(lab_t lab);

/**
 * @brief Convert LCH back to Lab
 *
 * @param lch Input LCH color
 * @return Lab color
 */
lab_t color_math_lch_to_lab(lch_t lch);

/**
 * @brief Convert XYZ to sRGB (float 0.0-1.0 range)
 *
 * This should only be used for displaying colors, not for color matching.
 * Includes gamma correction.
 *
 * @param xyz Input XYZ color
 * @param rgb Output RGB color (0.0-1.0 range)
 * @return ESP_OK on success
 */
esp_err_t color_math_xyz_to_rgb_float(const xyz_t* xyz, rgb_t* rgb);

/**
 * @brief Apply Color Correction Matrix to XYZ values
 *
 * Transforms XYZ tristimulus values using a 3x3 matrix.
 * Commonly used to correct sensor response to match standard observer.
 *
 * @param xyz Input XYZ color
 * @param ccm 3x3 Color Correction Matrix
 * @return Corrected XYZ color
 */
xyz_t color_math_apply_ccm(const xyz_t* xyz, const float ccm[3][3]);

/**
 * @brief Apply Polynomial Color Correction Matrix to XYZ values
 *
 * Transforms XYZ tristimulus values using a 3x10 polynomial matrix.
 * This allows for non-linear correction to improve accuracy with saturated colors.
 * 10 terms: R, G, B, R², G², B², RG, RB, GB, const
 *
 * Note: The constant term (index 9) is always forced to 0 by apply_constraints()
 * to prevent dark-color identification issues. Black-level subtraction is the
 * correct mechanism for additive offsets.
 *
 * @param xyz Input XYZ color
 * @param pccm 3x10 Polynomial Color Correction Matrix
 * @return Corrected XYZ color
 */
xyz_t color_math_apply_pccm(const xyz_t* xyz, const float pccm[3][10]);

/**
 * @brief Linear interpolation helper function
 *
 * Renamed from lerp to avoid conflict with C++20 std::lerp.
 *
 * @param a Start value
 * @param b End value
 * @param t Interpolation factor (0.0-1.0)
 * @return Interpolated value: a + t * (b - a)
 */
static inline float math_lerp(float a, float b, float t)
{
    return a + t * (b - a);
}

/**
 * @brief Apply lightness correction with blue compensation and gamma
 *
 * Applies linear lightness correction plus Gaussian blue region correction,
 * followed by gamma correction for non-linear mid-tone adjustment.
 * Formula: L_out = 100 * pow((scale * L_in + offset + blue_correction) / 100, gamma)
 * where blue_correction = magnitude * exp(-(L-center)²/width²)
 *
 * @param L_measured Input lightness value (0-100)
 * @param params Calibration parameters containing correction coefficients
 * @return Corrected lightness value (clamped to 0-100)
 */
float color_math_correct_lightness(float L_measured, const color_calib_params_t* params);

//===========================================================================
// BRADFORD CHROMATIC ADAPTATION MATRICES
//===========================================================================

/**
 * @brief Bradford chromatic adaptation matrix (XYZ -> LMS cone response)
 *
 * This is the standard Bradford transformation matrix from CIE color science.
 * Use with bradford_ma_inv for chromatic adaptation calculations.
 */
extern const float BRADFORD_MA[3][3];

/**
 * @brief Inverse Bradford matrix (LMS cone response -> XYZ)
 *
 * Inverse of BRADFORD_MA, used in chromatic adaptation calculations.
 */
extern const float BRADFORD_MA_INV[3][3];

//===========================================================================
// MATERIAL-AWARE COLOR CORRECTION
//===========================================================================

/**
 * @brief Get the default correction parameters for a material type
 * 
 * Returns the pre-defined correction factors for compensating measurement
 * bias introduced by different surface physics:
 * 
 * - FABRIC: L' = L*1.10 + 2, a' = a*1.05, b' = b*1.05
 *   (lifts shadows, restores muted chroma from fiber scattering)
 * 
 * - PLASTIC: L' = L*0.98
 *   (mild correction for semi-gloss specular bias)
 * 
 * - METAL: L' = L*0.90, a' = a*0.95, b' = b*0.95
 *   (suppresses specular reflection bias)
 * 
 * - DEFAULT/UNKNOWN: identity (no correction)
 * 
 * @param material Material type to get correction for
 * @return Correction factors for the specified material
 */
material_correction_t color_math_get_material_correction(material_type_t material);

/**
 * @brief Apply material-specific correction to Lab values
 * 
 * Transforms Lab values using material-specific correction factors to
 * compensate for systematic measurement bias introduced by surface physics.
 * 
 * @param lab Input Lab color to correct
 * @param correction Correction factors to apply
 * @return Corrected Lab color
 */
lab_t color_math_apply_material_correction(const lab_t* lab, const material_correction_t* correction);

/**
 * @brief Classify material type from raw sensor data
 * 
 * Uses heuristics based on sensor signal characteristics to infer
 * material type without requiring machine learning:
 * 
 * - Variance across samples: Fabrics have high variance due to thread texture
 * - Intensity vs saturation ratio: Metals have high reflectance, low saturation
 * - Channel ratios: Different materials have characteristic R/G/B balance
 * 
 * @param reading Raw sensor reading with X, Y, Z, IR, Clear channels
 * @param variance_xyz Optional XYZ variance from multiple samples (can be NULL)
 * @return Classified material type
 */
material_type_t color_math_classify_material(const sensor_reading_t* reading,
                                             const xyz_t* variance_xyz);

/**
 * @brief Get the name of a material type as a string
 * 
 * @param material Material type
 * @return Human-readable material name
 */
const char* color_math_material_name(material_type_t material);

#ifdef __cplusplus
}
#endif

#endif // COLOR_MATH_H
