#include "konaref.h"

const kona_table_t kona_reference = {
    .version = KONA_REF_SCHEMA_VERSION,
    .entry_count = 0,
    .crc32 = 0,
    .entries = {},
};

const uint16_t kona_vptree_node_count = 0;

const kona_vptree_node_t kona_vptree_nodes[1] = {
    { 0, 0.0f, -1, -1 },  // placeholder — never accessed when count == 0
};

const uint16_t kona_synthetic_name_count = 0;

const kona_synthetic_name_t kona_synthetic_names[1] = {
    { 0, nullptr },  // placeholder — never accessed when count == 0
};

const uint16_t kona_description_count = 0;

const kona_description_t kona_descriptions[1] = {
    { 0, nullptr },  // placeholder — never accessed when count == 0
};
