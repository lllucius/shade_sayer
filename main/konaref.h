/**
 * @file konaref.h
 * @brief Kona Swatch Reference Table Schema and Access API
 *
 * This module provides a reference table of CIE L*a*b* color values for
 * Kona Cotton quilting fabric swatches. The table is used for high-accuracy
 * color matching when identifying fabric colors.
 *
 * The table is generated at build time from scan captures using
 * scripts/generate_kona_table.py. If no scan data is available, an empty
 * fallback table (konaref_default.cpp) is used and matching falls back
 * to the general color database.
 *
 * @note CIEDE2000 is used for matching (perceptually uniform color difference).
 * @note Default match threshold is 2.0 ΔE2000 units (just noticeable difference).
 */

#pragma once

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Schema version for the Kona reference table format.
 *
 * Increment when changing the struct layout to detect incompatible tables.
 */
#define KONA_REF_SCHEMA_VERSION 1u

/**
 * @brief Maximum number of Kona swatch entries supported.
 *
 * The Kona Cotton collection has 365 colors, so this is the upper bound.
 */
#define KONA_REF_MAX_ENTRIES 365u

/**
 * @brief A single Kona swatch reference entry.
 *
 * Contains the swatch ID and its measured CIE L*a*b* color coordinates.
 * These values are averages from multiple device sensor readings.
 *
 * @note The struct has 2 bytes of padding after kona_id for float alignment.
 *       Total size is 16 bytes (not 14). The build-time table generator
 *       (scripts/generate_kona_table.py) must use the same layout for CRC32.
 */
typedef struct
{
    uint16_t kona_id;   ///< Kona swatch identifier (matches KONA_SWATCH_METADATA[].id)
    // 2 bytes padding inserted by compiler for float alignment
    float l;            ///< CIE L* (lightness): 0-100
    float a;            ///< CIE a* (green-red axis): typically -128 to +127
    float b;            ///< CIE b* (blue-yellow axis): typically -128 to +127
} kona_ref_t;

/**
 * @brief Expected size of kona_ref_t struct in bytes.
 *
 * This includes 2 bytes of padding after kona_id for float alignment:
 * - uint16_t kona_id: 2 bytes
 * - padding: 2 bytes
 * - float l, a, b: 12 bytes (3 × 4 bytes)
 * - Total: 16 bytes
 */
#define KONA_REF_T_SIZE 16u

/**
 * @brief Container for the complete Kona reference table.
 *
 * Includes metadata for validation (version, count, CRC32) and the
 * array of reference entries. CRC32 covers only the entry data.
 */
typedef struct
{
    uint16_t version;       ///< Schema version (must match KONA_REF_SCHEMA_VERSION)
    uint16_t entry_count;   ///< Number of valid entries (0 to KONA_REF_MAX_ENTRIES)
    uint32_t crc32;         ///< CRC32 checksum of entries[] data (IEEE 802.3 polynomial)
    kona_ref_t entries[KONA_REF_MAX_ENTRIES]; ///< Swatch reference data
} kona_table_t;

#ifdef __cplusplus
static_assert(sizeof(kona_ref_t) == KONA_REF_T_SIZE,
              "kona_ref_t size mismatch - update KONA_REF_T_SIZE and generate_kona_table.py");
#endif

/**
 * @brief Global Kona reference table instance.
 *
 * Defined in konaref_generated.cpp (build-time generated) or
 * konaref_default.cpp (empty fallback).
 */
extern const kona_table_t kona_reference;

/**
 * @brief Validate the Kona reference table integrity.
 *
 * Checks schema version, entry count bounds, and CRC32 checksum.
 *
 * @return true if table is valid and ready for matching, false otherwise.
 */
bool kona_ref_validate(void);

/**
 * @brief Get the number of entries in the Kona reference table.
 *
 * @return Entry count (0 if table is empty or invalid).
 */
size_t kona_ref_entry_count(void);

/**
 * @brief Get a pointer to the Kona reference entries array.
 *
 * @return Pointer to the entries array (valid up to kona_ref_entry_count() entries).
 */
const kona_ref_t* kona_ref_entries(void);

/**
 * @brief Load a temporary Kona reference table into RAM.
 *
 * The temporary table takes precedence over the built-in table until
 * kona_ref_clear_temp() is called or the device is reset.
 *
 * @param table Pointer to the table data to load (copied to internal storage)
 * @return true if table was loaded successfully, false if validation failed
 *
 * @note The table is copied, so the input buffer can be freed after this call.
 * @note Validates version, entry count, and CRC32 before accepting.
 */
bool kona_ref_load_temp(const kona_table_t* table);

/**
 * @brief Clear the temporary Kona reference table.
 *
 * After calling this function, kona_ref_* functions will return data from
 * the built-in table (if valid) instead of the temporary table.
 */
void kona_ref_clear_temp(void);

/**
 * @brief Check if a temporary table is currently active.
 *
 * @return true if a temporary table is loaded and active, false otherwise
 */
bool kona_ref_is_temp_active(void);

/**
 * @brief Get the entry count of the currently active table (temp or built-in).
 *
 * @return Number of entries in the active table, or 0 if no valid table
 */
size_t kona_ref_active_entry_count(void);

#ifdef __cplusplus
}
#endif
