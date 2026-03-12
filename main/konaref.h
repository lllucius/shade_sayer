#pragma once

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

#define KONA_REF_SCHEMA_VERSION 1u
#define KONA_REF_MAX_ENTRIES 365u

typedef struct {
    uint16_t kona_id;
    float l;
    float a;
    float b;
    float sigma_l;
    float sigma_a;
    float sigma_b;
} kona_ref_t;

typedef struct {
    uint16_t version;
    uint16_t entry_count;
    uint32_t crc32;
    kona_ref_t entries[KONA_REF_MAX_ENTRIES];
} kona_table_t;

extern const kona_table_t kona_reference;

bool kona_ref_validate(void);
size_t kona_ref_entry_count(void);
const kona_ref_t* kona_ref_entries(void);

#ifdef __cplusplus
}
#endif
