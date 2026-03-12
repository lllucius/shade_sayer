#include "konaref.h"

#include <stddef.h>
#include <stdint.h>

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

bool kona_ref_validate(void)
{
    if (kona_reference.version != KONA_REF_SCHEMA_VERSION)
    {
        return false;
    }

    if (kona_reference.entry_count > KONA_REF_MAX_ENTRIES)
    {
        return false;
    }

    const size_t bytes = static_cast<size_t>(kona_reference.entry_count) * sizeof(kona_ref_t);
    const uint8_t* raw = reinterpret_cast<const uint8_t*>(kona_reference.entries);
    return kona_crc32(raw, bytes) == kona_reference.crc32;
}

size_t kona_ref_entry_count(void)
{
    return static_cast<size_t>(kona_reference.entry_count);
}

const kona_ref_t* kona_ref_entries(void)
{
    return kona_reference.entries;
}
