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
