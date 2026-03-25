/**
 * @file konaref.cpp
 * @brief Kona Reference Table Validation, Access, and VP-tree Search
 *
 * Provides runtime validation of the Kona reference table (CRC32 checksum),
 * accessor functions for table data, and O(log n) nearest-neighbour search
 * via a pre-computed VP-tree.
 */

#include "konaref.h"
#include "color_math.h"

#include <float.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

/**
 * @brief Compute CRC32 checksum using IEEE 802.3 polynomial.
 *
 * Uses the standard CRC32 polynomial (0xEDB88320) with bit-reversed
 * algorithm. This matches Python's zlib.crc32() for compatibility
 * with the build-time table generator.
 *
 * @param data Pointer to data buffer
 * @param len Length of data in bytes
 * @return CRC32 checksum value
 */
static uint32_t kona_crc32(const uint8_t* data, size_t len)
{
    uint32_t crc = 0xFFFFFFFFu;
    for (size_t i = 0; i < len; ++i)
    {
        crc ^= data[i];
        for (int bit = 0; bit < 8; ++bit)
        {
            crc = (crc >> 1) ^ (0xEDB88320u & static_cast<uint32_t>(-(static_cast<int32_t>(crc & 1u))));
        }
    }
    return crc ^ 0xFFFFFFFFu;
}

/**
 * @brief Validate a kona table's integrity.
 *
 * @param table Pointer to the table to validate
 * @return true if table is valid, false otherwise
 */
static bool validate_table(const kona_table_t* table)
{
    if (!table)
    {
        return false;
    }

    // Check schema version compatibility
    if (table->version != KONA_REF_SCHEMA_VERSION)
    {
        return false;
    }

    // Check entry count is within bounds
    if (table->entry_count > KONA_REF_MAX_ENTRIES)
    {
        return false;
    }

    // Verify CRC32 checksum over entry data
    const size_t bytes = static_cast<size_t>(table->entry_count) * sizeof(kona_ref_t);
    const uint8_t* raw = reinterpret_cast<const uint8_t*>(table->entries);
    return kona_crc32(raw, bytes) == table->crc32;
}

bool kona_ref_validate(void)
{
    return validate_table(&kona_reference);
}

size_t kona_ref_entry_count(void)
{
    return static_cast<size_t>(kona_reference.entry_count);
}

const kona_ref_t* kona_ref_entries(void)
{
    return kona_reference.entries;
}


// ---------------------------------------------------------------------------
// VP-tree nearest-neighbour search
// ---------------------------------------------------------------------------

/** @brief Recursive VP-tree search state. */
typedef struct
{
    float query_l;
    float query_a;
    float query_b;
    int32_t best_index;       ///< index into kona_reference.entries[]
    float   best_distance;    ///< CIEDE2000 to best match so far
} kona_vp_search_t;

/**
 * @brief Recursively search the Kona VP-tree for the nearest entry.
 *
 * The algorithm mirrors color_matcher.cpp's vptree_search_recursive():
 * 1. Compute CIEDE2000 distance to this node's entry.
 * 2. Update best if closer.
 * 3. Prune subtrees that cannot improve the current best.
 */
static void kona_vp_search(int16_t node_idx, kona_vp_search_t* state)
{
    if (node_idx < 0 || node_idx >= static_cast<int16_t>(kona_vptree_node_count))
    {
        return;
    }

    const kona_vptree_node_t* node = &kona_vptree_nodes[node_idx];
    const int16_t ei = node->entry_index;
    if (ei < 0 || ei >= static_cast<int16_t>(kona_reference.entry_count))
    {
        return;
    }

    const kona_ref_t* entry = &kona_reference.entries[ei];
    const lab_t query_lab = {state->query_l, state->query_a, state->query_b};
    const lab_t entry_lab = {entry->l, entry->a, entry->b};
    const float dist = color_math_delta_e_ciede2000(&query_lab, &entry_lab);

    if (dist < state->best_distance)
    {
        state->best_distance = dist;
        state->best_index = ei;
    }

    // Leaf — nothing more to search
    if (node->left_child < 0 && node->right_child < 0)
    {
        return;
    }

    const float median = node->median_distance;
    const bool has_left  = (node->left_child  >= 0);
    const bool has_right = (node->right_child >= 0);

    if (dist <= median)
    {
        // Closer to vantage point → search left (closer) first
        if (has_left)
        {
            kona_vp_search(node->left_child, state);
        }
        // Right subtree may still contain a better match
        if (has_right && (dist + state->best_distance > median))
        {
            kona_vp_search(node->right_child, state);
        }
    }
    else
    {
        // Farther from vantage point → search right (farther) first
        if (has_right)
        {
            kona_vp_search(node->right_child, state);
        }
        // Left subtree may still contain a better match
        if (has_left && (dist - state->best_distance <= median))
        {
            kona_vp_search(node->left_child, state);
        }
    }
}

const kona_ref_t* kona_ref_find_closest(float query_l, float query_a, float query_b,
                                        float* delta_e, size_t* best_idx)
{
    const size_t count = static_cast<size_t>(kona_reference.entry_count);
    if (count == 0)
    {
        return nullptr;
    }

    // Use VP-tree when available
    if (kona_vptree_node_count > 0)
    {
        kona_vp_search_t state = {};
        state.query_l = query_l;
        state.query_a = query_a;
        state.query_b = query_b;
        state.best_index = -1;
        state.best_distance = FLT_MAX;

        kona_vp_search(0, &state);

        if (state.best_index >= 0)
        {
            if (delta_e)
            {
                *delta_e = state.best_distance;
            }
            if (best_idx)
            {
                *best_idx = static_cast<size_t>(state.best_index);
            }
            return &kona_reference.entries[state.best_index];
        }
    }

    // Fallback: linear scan (empty VP-tree or no match found)
    const lab_t query_lab = {query_l, query_a, query_b};
    float best_de = FLT_MAX;
    size_t best_i = 0;
    for (size_t i = 0; i < count; ++i)
    {
        const lab_t entry_lab = {kona_reference.entries[i].l,
                                 kona_reference.entries[i].a,
                                 kona_reference.entries[i].b};
        const float de = color_math_delta_e_ciede2000(&query_lab, &entry_lab);
        if (de < best_de)
        {
            best_de = de;
            best_i = i;
        }
    }

    if (delta_e)
    {
        *delta_e = best_de;
    }
    if (best_idx)
    {
        *best_idx = best_i;
    }
    return &kona_reference.entries[best_i];
}

const char* kona_ref_synthetic_name(uint16_t kona_id)
{
    for (uint16_t i = 0; i < kona_synthetic_name_count; ++i)
    {
        if (kona_synthetic_names[i].kona_id == kona_id)
        {
            return kona_synthetic_names[i].nearest_name;
        }
    }
    return nullptr;
}
