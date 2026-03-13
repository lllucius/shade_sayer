/**
 * @file konaref.cpp
 * @brief Kona Reference Table Validation and Access Implementation
 *
 * Provides runtime validation of the Kona reference table (CRC32 checksum)
 * and accessor functions for table data. Supports loading temporary tables
 * from the GUI application for testing purposes.
 */

#include "konaref.h"

#include <stddef.h>
#include <stdint.h>
#include <string.h>

/**
 * @brief Storage for temporarily loaded Kona table.
 *
 * When a temporary table is loaded via kona_ref_load_temp(), it is copied
 * here and takes precedence over the built-in table. The temporary table
 * is cleared on device reset or by calling kona_ref_clear_temp().
 */
static kona_table_t s_temp_table = {};

/**
 * @brief Flag indicating whether a temporary table is active.
 */
static bool s_temp_table_active = false;

/**
 * @brief Flag indicating whether the temporary table has been validated.
 */
static bool s_temp_table_valid = false;

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
    // Prefer temporary table if active and valid
    if (s_temp_table_active && s_temp_table_valid)
    {
        return s_temp_table.entries;
    }
    return kona_reference.entries;
}

bool kona_ref_load_temp(const kona_table_t* table)
{
    if (!table)
    {
        return false;
    }

    // Validate before loading
    if (!validate_table(table))
    {
        return false;
    }

    // Copy table to internal storage
    memcpy(&s_temp_table, table, sizeof(kona_table_t));
    s_temp_table_valid = true;
    s_temp_table_active = true;

    return true;
}

void kona_ref_clear_temp(void)
{
    s_temp_table_active = false;
    s_temp_table_valid = false;
    memset(&s_temp_table, 0, sizeof(kona_table_t));
}

bool kona_ref_is_temp_active(void)
{
    return s_temp_table_active && s_temp_table_valid;
}

size_t kona_ref_active_entry_count(void)
{
    // Prefer temporary table if active and valid
    if (s_temp_table_active && s_temp_table_valid)
    {
        return static_cast<size_t>(s_temp_table.entry_count);
    }

    // Fall back to built-in table if valid
    if (validate_table(&kona_reference))
    {
        return static_cast<size_t>(kona_reference.entry_count);
    }

    return 0;
}
