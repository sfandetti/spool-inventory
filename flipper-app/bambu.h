#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define BAMBU_SECTORS 16

// Blocks we read from a Bambu Lab spool tag (all in sectors 0-4).
// Order here = order in BambuRaw.blocks.
#define BAMBU_BLOCK_LIST {1, 2, 4, 5, 9, 12, 14, 16}
#define BAMBU_BLOCK_COUNT 8

typedef struct {
    uint8_t uid[10];
    uint8_t uid_len;
    uint8_t blocks[BAMBU_BLOCK_COUNT][16];
} BambuRaw;

typedef struct {
    char tag_uid[21]; // hex
    char tray_uid[33]; // hex of block 9, unique per spool
    char variant_id[9];
    char material_id[9];
    char type[17]; // "PLA"
    char detail[17]; // "PLA Basic"
    uint8_t rgba[4];
    uint8_t rgba2[4]; // second color for multicolor spools (0 if none)
    uint16_t color_count;
    uint16_t weight_g;
    float diameter_mm;
    uint16_t length_m;
    char produced[17]; // "2024_05_17_09_31"
} BambuSpool;

/** Derive the 16 sector A-keys (6 bytes each) from the tag UID. */
void bambu_derive_keys(const uint8_t* uid, size_t uid_len, uint8_t keys[BAMBU_SECTORS][6]);

/** Decode raw blocks. Returns false if it doesn't look like a Bambu spool. */
bool bambu_parse(const BambuRaw* raw, BambuSpool* out);

/** Nearest plain-English color name, e.g. "Red". */
const char* bambu_color_name(const uint8_t rgba[4]);

/** One CSV line (with trailing newline) for the spool. Returns length. */
size_t bambu_csv_line(const BambuSpool* s, uint32_t scanned_unix, char* buf, size_t cap);

#define BAMBU_CSV_HEADER                                                                     \
    "tray_uid,tag_uid,material_id,variant_id,type,detail,color_rgba,color2_rgba,color_count," \
    "weight_g,diameter_mm,length_m,produced,scanned_unix\n"
