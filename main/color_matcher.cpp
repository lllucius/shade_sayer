/**
 * @file color_matcher.cpp
 * @brief Color matching using VP-Tree for O(log n) search performance
 *
 * This implementation uses a pre-computed VP-Tree (Vantage Point Tree) stored
 * in Flash memory for efficient nearest neighbor color matching:
 *
 * 1. VP-Tree is generated at build time by generate_vptree.py
 * 2. Tree is stored as const data in Flash - zero runtime allocations
 * 3. Search uses CIEDE2000 as the distance metric
 * 4. Average search complexity is O(log n) instead of O(n)
 *
 * For ~950 colors:
 * - Linear search: ~950 CIEDE2000 calculations per query
 * - VP-Tree search: ~10-15 CIEDE2000 calculations per query (log2(950) ≈ 10)
 *
 * MEMORY: All data resides in Flash/RODATA. No heap allocations needed.
 */

#include <string.h>
#include <float.h>
#include <math.h>
#include <inttypes.h>


#include "color_matcher.h"
#include "tcs_glue.h"
#include "color_database.h"
#include "color_math.h"
#include "vptree_data.h"

static const char *TAG = "color_matcher";

static uint32_t s_color_count = 0;
static bool s_initialized = false;

// Statistics
static uint32_t total_searches = 0;
static uint64_t total_comparisons = 0;
static uint64_t total_nodes_visited = 0;

/**
 * @brief VP-Tree search state
 *
 * Tracks the best match found so far during tree traversal.
 */
typedef struct
{
    const lab_t* query;         //< Query color
    int32_t best_index;         //< Best matching color index
    float best_distance;        //< Best CIEDE2000 distance found
    uint32_t nodes_visited;     //< Number of nodes visited
    uint32_t comparisons;       //< Number of CIEDE2000 calculations
} vptree_search_state_t;

/**
 * @brief Recursively search the VP-Tree for the nearest neighbor
 *
 * The VP-Tree search algorithm:
 * 1. Calculate distance from query to vantage point
 * 2. Update best match if this distance is smaller
 * 3. Determine which child subtree(s) could contain a better match
 * 4. Search the closer subtree first (better pruning)
 * 5. Only search farther subtree if it could contain a better match
 *
 * @param node_idx Index of current node in vptree_nodes array
 * @param state Search state (query, best match, statistics)
 */
static void vptree_search_recursive(int16_t node_idx, vptree_search_state_t* state)
{
    if (node_idx < 0 || node_idx >= VPTREE_NODE_COUNT)
    {
        return;
    }

    state->nodes_visited++;

    const vptree_node_t* node = &vptree_nodes[node_idx];

    // Get the Lab color for this node's color_index from the database
    lab_t node_lab;
    color_database_get_entry((uint32_t)node->color_index, NULL, 0, &node_lab);

    // Calculate CIEDE2000 distance from query to vantage point
    float dist = color_math_delta_e_ciede2000(state->query, &node_lab);
    state->comparisons++;

    // Update best match if this is closer
    if (dist < state->best_distance)
    {
        state->best_distance = dist;
        state->best_index = node->color_index;
    }

    // Leaf node - no children to search
    if (node->left_child < 0 && node->right_child < 0)
    {
        return;
    }

    float median = node->median_distance;

    // Determine which subtree to search first and whether to search both
    bool search_left = (node->left_child >= 0);
    bool search_right = (node->right_child >= 0);

    if (dist <= median)
    {
        // Query is closer to vantage point - search left (closer) subtree first
        if (search_left)
        {
            vptree_search_recursive(node->left_child, state);
        }

        // Search right subtree only if it could contain a closer match
        // The right subtree contains colors at distance > median from vantage point
        // We need to search if: dist + best_distance > median
        // (i.e., the "ball" around query with radius best_distance overlaps right subtree)
        if (search_right && (dist + state->best_distance > median))
        {
            vptree_search_recursive(node->right_child, state);
        }
    }
    else
    {
        // Query is farther from vantage point - search right (farther) subtree first
        if (search_right)
        {
            vptree_search_recursive(node->right_child, state);
        }

        // Search left subtree only if it could contain a closer match
        // The left subtree contains colors at distance <= median from vantage point
        // We need to search if: dist - best_distance <= median
        // (i.e., the "ball" around query overlaps left subtree)
        if (search_left && (dist - state->best_distance <= median))
        {
            vptree_search_recursive(node->left_child, state);
        }
    }
}

esp_err_t color_matcher_init(void)
{
    ESP_LOGI(TAG, "Initializing color matcher (VP-Tree in Flash, zero allocations)");

    if (s_initialized)
    {
        ESP_LOGI(TAG, "Color matcher already initialized with %" PRIu32 " colors", s_color_count);
        return ESP_OK;
    }

    // Get the total number of colors from database
    s_color_count = color_database_get_count();

    // Verify VP-Tree matches database
    if (VPTREE_NODE_COUNT != s_color_count)
    {
        ESP_LOGW(TAG, "VP-Tree node count (%d) differs from database (%" PRIu32 ")",
                 VPTREE_NODE_COUNT, s_color_count);
        // This is not fatal - we can still use the VP-Tree
    }

    s_initialized = true;
    ESP_LOGI(TAG, "VP-Tree initialized: %d nodes, depth ~%d, Flash usage: %.2f KB",
             VPTREE_NODE_COUNT,
             (int)(log2f((float)VPTREE_NODE_COUNT) + 0.5f),
             (float)(VPTREE_NODE_COUNT * sizeof(vptree_node_t)) / 1024.0f);

    return ESP_OK;
}

const char* color_matcher_find_closest(const lab_t *lab, float *delta_e)
{
    if (!lab)
    {
        return NULL;
    }

    total_searches++;

    // Fall back to linear search if not initialized or VP-Tree empty
    if (!s_initialized || VPTREE_NODE_COUNT == 0)
    {
        const char *result = find_closest_color_lab(lab, delta_e);
        total_comparisons += color_database_get_count();
        return result;
    }

    // Initialize search state
    vptree_search_state_t state =
    {
        .query = lab,
        .best_index = -1,
        .best_distance = FLT_MAX,
        .nodes_visited = 0,
        .comparisons = 0
    };

    // Search VP-Tree starting from root
    vptree_search_recursive(VPTREE_ROOT_INDEX, &state);

    // Update statistics
    total_comparisons += state.comparisons;
    total_nodes_visited += state.nodes_visited;

    // Return result
    if (state.best_index >= 0)
    {
        if (delta_e)
        {
            *delta_e = state.best_distance;
        }
        return color_database_get_name((uint32_t)state.best_index);
    }

    return NULL;
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
    if (nodes_visited_out)
    {
        *nodes_visited_out = total_nodes_visited;
    }
}
