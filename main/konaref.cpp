/**
 * @file konaref.cpp
 * @brief Kona Reference Table Validation, Access, and Nearest-Neighbour Search
 *
 * Provides runtime validation of the Kona reference table (CRC32 checksum),
 * accessor functions for table data, and exact nearest-neighbour search via
 * an exhaustive CIEDE2000 linear scan (see kona_ref_find_closest for why a
 * VP-tree is deliberately not used).
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
// Nearest-neighbour search (exhaustive CIEDE2000 linear scan)
//
// A VP-tree search was previously used here, but VP-tree pruning relies on
// the triangle inequality, which CIEDE2000 does not satisfy — the tree could
// return a non-nearest swatch and even flip a match across the acceptance
// threshold. With a few hundred entries, a full linear scan is on the order
// of a millisecond and always exact, so the tree traversal was removed.
// ---------------------------------------------------------------------------

const kona_ref_t* kona_ref_find_closest(float query_l, float query_a, float query_b,
                                        float* delta_e, size_t* best_idx)
{
    const size_t count = static_cast<size_t>(kona_reference.entry_count);
    if (count == 0)
    {
        return nullptr;
    }

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
    // Binary search — the table is sorted by kona_id at build time.
    int lo = 0;
    int hi = static_cast<int>(kona_synthetic_name_count) - 1;
    while (lo <= hi)
    {
        int mid = lo + (hi - lo) / 2;
        uint16_t mid_id = kona_synthetic_names[mid].kona_id;
        if (mid_id == kona_id)
        {
            return kona_synthetic_names[mid].nearest_name;
        }
        else if (mid_id < kona_id)
        {
            lo = mid + 1;
        }
        else
        {
            hi = mid - 1;
        }
    }
    return nullptr;
}

const char* kona_ref_description(uint16_t kona_id)
{
    // Binary search — the table is sorted by kona_id at build time.
    int lo = 0;
    int hi = static_cast<int>(kona_description_count) - 1;
    while (lo <= hi)
    {
        int mid = lo + (hi - lo) / 2;
        uint16_t mid_id = kona_descriptions[mid].kona_id;
        if (mid_id == kona_id)
        {
            return kona_descriptions[mid].description;
        }
        else if (mid_id < kona_id)
        {
            lo = mid + 1;
        }
        else
        {
            hi = mid - 1;
        }
    }
    return nullptr;
}
