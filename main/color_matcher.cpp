/**
 * @file color_matcher.cpp
 * @brief Color matching using exhaustive CIEDE2000 linear scan
 *
 * This implementation performs a brute-force linear scan over the color
 * database using CIEDE2000 as the distance function.
 *
 * WHY NOT A VP-TREE: an earlier implementation searched a pre-computed
 * VP-Tree with CIEDE2000 as the tree metric. VP-tree pruning relies on the
 * triangle inequality, which CIEDE2000 does not satisfy (its hue rotation
 * term and S-weightings break metricity). Empirically the tree returned a
 * non-nearest color for ~4% of Lab-grid queries with gaps up to 6 ΔE, and
 * could flip a match across the acceptance threshold. With only ~950
 * entries, a full linear scan costs on the order of a millisecond on the
 * ESP32-S3 and is exactly correct, so the tree was removed.
 *
 * MEMORY: All data resides in Flash/RODATA. No heap allocations needed.
 */

#include <float.h>
#include <math.h>
#include <inttypes.h>


#include "color_matcher.h"
#include "tcs_glue.h"
#include "color_database.h"
#include "color_math.h"

static const char *TAG = "color_matcher";

static uint32_t s_color_count = 0;
static bool s_initialized = false;

// Statistics
static uint32_t total_searches = 0;
static uint64_t total_comparisons = 0;

esp_err_t color_matcher_init(void)
{
    ESP_LOGI(TAG, "Initializing color matcher (linear CIEDE2000 scan)");

    if (s_initialized)
    {
        ESP_LOGI(TAG, "Color matcher already initialized with %" PRIu32 " colors", s_color_count);
        return ESP_OK;
    }

    // Get the total number of colors from database
    s_color_count = color_database_get_count();

    s_initialized = true;
    ESP_LOGI(TAG, "Color matcher initialized: %" PRIu32 " colors", s_color_count);

    return ESP_OK;
}

const char* color_matcher_find_closest(const lab_t *lab, float *delta_e)
{
    if (!lab)
    {
        return NULL;
    }

    total_searches++;

    // Exhaustive linear scan — guaranteed to return the true CIEDE2000
    // nearest neighbor (see file header for why a VP-tree is not used).
    const char *result = find_closest_color_lab(lab, delta_e);
    total_comparisons += color_database_get_count();
    return result;
}

void color_matcher_get_stats(uint32_t *searches, float *avg_comparisons)
{
    if (searches)
    {
        *searches = total_searches;
    }
    if (avg_comparisons && total_searches > 0)
    {
        *avg_comparisons = (float)total_comparisons / (float)total_searches;
    }
}

void color_matcher_get_filter_stats(uint64_t *nodes_visited_out)
{
    // Linear scan visits every database entry once per search.
    if (nodes_visited_out)
    {
        *nodes_visited_out = total_comparisons;
    }
}
