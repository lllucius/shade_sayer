/**
 * Color Database - xkcd Color Survey Matching
 *
 * This module provides color name lookup using the xkcd color survey palette.
 * The xkcd color survey (https://xkcd.com/color/rgb/) contains 949 named colors
 * based on a survey of over 200,000 people, providing more understandable
 * and commonly-used color names than traditional paint color databases.
 *
 * COLOR SPACE AND MATCHING:
 * =========================
 * - All colors are stored as LAB values (L*, a*, b*)
 * - LAB values assume D65 reference illuminant (daylight, 6504K)
 * - LAB color space is perceptually uniform (equal distances ~ equal perceived differences)
 * - Matching uses CIEDE2000 (DeltaE2000) color difference metric
 *
 * CIEDE2000 METRIC:
 * =================
 * - Industry standard for perceptual color difference
 * - Accounts for perceptual non-uniformities in LAB space
 * - Includes corrections for lightness, chroma, and hue differences
 * - DeltaE2000 interpretation:
 *   * <1.0:  Not perceptible by human eyes
 *   * 1-2:   Perceptible through close observation
 *   * 2-10:  Perceptible at a glance
 *   * >10:   Colors are more different than similar
 *
 * COMPATIBILITY WITH COLOR PIPELINE:
 * ==================================
 * The color database assumes all input LAB values have been properly adapted
 * to D65 via either:
 *   1. White point calibration (Von Kries diagonal chromatic adaptation), OR
 *   2. Bradford chromatic adaptation from source illuminant to D65
 *
 * This ensures consistent color matching regardless of the sensor's illuminant.
 * See color_pipeline.cpp for details on the chromatic adaptation pipeline.
 *
 */

#ifndef COLOR_DATABASE_H
#define COLOR_DATABASE_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include "color_math.h"
#include "tcs_glue.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct
{
    const char *name;
    lab_t lab;              // Colors stored directly in LAB format for perceptual accuracy
    float chroma;           // Pre-computed chroma: sqrt(a*a + b*b)
    float hue;              // Pre-computed hue angle in radians: atan2(b, a)
} color_entry_t;

/**
 * Initialize color database.
 *
 * The table is generated offline and already contains LAB/chroma/hue data,
 * so this function is intentionally a no-op kept for API compatibility.
 */
void color_database_init(void);

/**
 * Find the closest matching color using LAB color space
 * @param lab LAB color to match
 * @param out_delta_e If not nullptr, will be filled with the DeltaE2000 value
 * @return Pointer to the color name
 */
const char* find_closest_color_lab(const lab_t *lab, float *out_delta_e);

/**
 * Get the LAB values for a given color name
 * Uses O(n) lookup in the color database
 * @param color_name Name of the color to lookup
 * @param out_lab Output LAB values (can be NULL if not needed)
 * @return ESP_OK if found, ESP_ERR_NOT_FOUND if not found
 */
esp_err_t get_color_lab(const char *color_name, lab_t *out_lab);

/**
 * Get total number of colors in database
 * @return Number of colors
 */
uint32_t color_database_get_count(void);

/**
 * Get color entry by index.
 * @param index Index in database (0 to count-1)
 * @param name Output buffer for name (can be NULL)
 * @param name_size Size of name buffer
 * @param lab Output LAB color (can be NULL)
 * @return ESP_OK on success, ESP_ERR_INVALID_ARG if index out of range
 */
esp_err_t color_database_get_entry(uint32_t index, char *name, size_t name_size, lab_t *lab);

/**
 * Get direct pointer to color name by index (no copy)
 * @param index Index in database (0 to count-1)
 * @return Pointer to color name string, or NULL if index out of range
 */
const char* color_database_get_name(uint32_t index);

#ifdef __cplusplus
}
#endif

#endif // COLOR_DATABASE_H
