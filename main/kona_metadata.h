/**
 * @file kona_metadata.h
 * @brief Kona Cotton Swatch Metadata Definitions
 *
 * This module provides static metadata for the 365 Kona Cotton quilting
 * fabric swatches including panel location, swatch name, and unique ID.
 *
 * The metadata is used to:
 * - Display human-readable names when a Kona swatch is matched
 * - Guide users through the swatch scanning process
 * - Map between internal IDs and panel/index locations
 *
 * @note The metadata array is sorted by panel then panel_index order
 *       (the physical layout of the Kona color card).
 * @note IDs range from 7 to 1898 (not sequential, assigned by manufacturer).
 */

#pragma once

#include <cstddef>
#include <cstdint>

/**
 * @brief Metadata for a single Kona swatch.
 */
struct kona_swatch_info_t
{
    const char* panel;      //< Panel name (e.g., "yellow_orange_red", "neutrals_greys")
    uint16_t panel_index;   //< 1-based index within the panel (1-65)
    uint16_t id;            //< Manufacturer-assigned swatch ID (unique, non-sequential)
    const char* name;       //< Human-readable swatch name (e.g., "SUNNY", "PAPAYA")
};

/**
 * @brief Array of all 365 Kona swatch metadata entries.
 *
 * Sorted by panel then panel_index (physical color card order).
 */
extern const kona_swatch_info_t KONA_SWATCH_METADATA[];

/**
 * @brief Number of entries in KONA_SWATCH_METADATA array.
 */
extern const size_t KONA_SWATCH_METADATA_COUNT;

/**
 * @brief Find Kona swatch metadata by manufacturer ID.
 *
 * Performs a linear search through the metadata array.
 *
 * @param id Manufacturer-assigned swatch ID to find
 * @return Pointer to matching metadata, or nullptr if not found
 *
 * @note O(n) complexity - consider caching results if called frequently.
 */
const kona_swatch_info_t* kona_metadata_find_by_id(uint16_t id);
