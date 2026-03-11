/**
 * @file color_matcher.h
 * @brief Fast Color Matching using VP-Tree
 *
 * This module provides efficient nearest-neighbor color matching using
 * a pre-computed Vantage Point Tree (VP-Tree) stored in Flash memory.
 *
 * Key Features:
 * - O(log n) search complexity instead of O(n) linear search
 * - VP-Tree stored in Flash - zero heap allocations
 * - CIEDE2000 distance metric for perceptually accurate matching
 * - Fast startup - no runtime tree construction
 *
 * For the database of ~950 colors:
 * - Linear search: ~950 CIEDE2000 calculations per query
 * - VP-Tree search: ~10-15 CIEDE2000 calculations per query
 *
 * The VP-Tree is generated at build time by scripts/generate_vptree.py
 * and stored as static const data in vptree_data.h.
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
 * Uses pre-computed VP-Tree stored in Flash for O(log n) search.
 * Zero heap allocations - fast startup.
 * @return ESP_OK on success
 */
esp_err_t color_matcher_init(void);

/**
 * Find closest color using VP-Tree search
 * Uses CIEDE2000 as the distance metric.
 * Average complexity: O(log n) instead of O(n).
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
 * Get VP-Tree traversal statistics
 * @param nodes_visited_out Total number of tree nodes visited
 */
void color_matcher_get_filter_stats(uint64_t *nodes_visited_out);

#ifdef __cplusplus
}
#endif

#endif // COLOR_MATCHER_H
