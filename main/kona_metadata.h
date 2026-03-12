#pragma once

#include <cstddef>
#include <cstdint>

struct kona_swatch_info_t {
    const char* panel;
    uint16_t panel_index;
    uint16_t id;
    const char* name;
};

extern const kona_swatch_info_t KONA_SWATCH_METADATA[];
extern const size_t KONA_SWATCH_METADATA_COUNT;

const kona_swatch_info_t* kona_metadata_find_by_id(uint16_t id);
