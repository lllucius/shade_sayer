/**
 * @file konaref_default.cpp
 * @brief Fallback Empty Kona Reference Table
 *
 * This file provides an empty Kona reference table used when no scan
 * capture data (kona_avg_captures.csv) is available at build time.
 *
 * When this fallback is used:
 * - kona_ref_validate() returns true (empty but valid)
 * - kona_ref_entry_count() returns 0
 * - Color matching falls back to the general VP-Tree matcher
 *
 * To generate a populated table, run a Kona swatch scan session and then:
 *   python3 scripts/kona_scan_collect.py --input-log console.txt
 *   python3 scripts/generate_kona_table.py --input kona_avg_captures.csv
 *
 * @see scripts/generate_kona_table.py for table generation
 * @see color_pipeline.cpp::try_match_kona_reference() for matching logic
 */

#include "konaref.h"

const kona_table_t kona_reference = {
    .version = KONA_REF_SCHEMA_VERSION,
    .entry_count = 0,
    .crc32 = 0,
    .entries = {},
};
