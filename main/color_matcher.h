/**
 * @file color_matcher.h
 * @brief Exact Color Matching via exhaustive CIEDE2000 scan
 *
 * This module provides nearest-neighbor color matching using a brute-force
 * linear scan over the color database with CIEDE2000 as the distance
 * function.
 *
 * A VP-Tree was previously used here, but VP-tree pruning requires the
 * triangle inequality and CIEDE2000 is not a metric — the tree returned
 * wrong nearest neighbors for ~4% of queries. At ~950 entries a linear
 * scan is fast enough (~1 ms on ESP32-S3) and always exact.
 */

#ifndef COLOR_MATCHER_H
#define COLOR_MATCHER_H

#include "tcs_glue.h"

#include "color_types.h"
#include "color_math.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Initialize color matcher
 * Counts the database entries; no tree construction, zero heap allocations.
 * @return ESP_OK on success
 */
esp_err_t color_matcher_init(void);

/**
 * Find closest color via exhaustive linear scan
 * Uses CIEDE2000 as the distance function; always returns the true
 * nearest neighbor.
 * @param lab Lab color to match
 * @param delta_e Pointer to store color difference (can be NULL)
 * @return Color name, or NULL on error
 */
const char* color_matcher_find_closest(const lab_t *lab, float *delta_e);

/**
 * Get statistics about color matching performance
 * @param searches Total searches performed
 * @param avg_comparisons Average CIEDE2000 comparisons per search
 */
void color_matcher_get_stats(uint32_t *searches, float *avg_comparisons);

/**
 * Get search statistics
 * @param nodes_visited_out Total number of database entries examined
 */
void color_matcher_get_filter_stats(uint64_t *nodes_visited_out);

#ifdef __cplusplus
}
#endif

#endif // COLOR_MATCHER_H
