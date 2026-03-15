/**
 * @file konaref.cpp
 * @brief Kona Reference Table Validation and Access Implementation
 *
 * Provides runtime validation of the Kona reference table (CRC32 checksum)
 * and accessor functions for table data.
 */

#include "konaref.h"

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
