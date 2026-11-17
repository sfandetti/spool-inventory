#include "bambu.h"
#include "sha256.h"
#include <stdio.h>
#include <string.h>

// Key derivation published by the Bambu Research Group (RFID-Tag-Guide, deriveKeys.py):
// HKDF-SHA256(ikm = UID, salt = below, info = "RFID-A\0", 96 bytes) -> 16 six-byte A-keys.
static const uint8_t SALT[16] = {
    0x9a, 0x75, 0x9c, 0xf2, 0xc4, 0xf7, 0xca, 0xff, 0x22, 0x2c, 0xb9, 0x76, 0x9b, 0x41, 0xbc, 0x96};
static const uint8_t INFO[7] = {'R', 'F', 'I', 'D', '-', 'A', 0};

void bambu_derive_keys(const uint8_t* uid, size_t uid_len, uint8_t keys[BAMBU_SECTORS][6]) {
    hkdf_sha256(SALT, sizeof(SALT), uid, uid_len, INFO, sizeof(INFO), &keys[0][0], BAMBU_SECTORS * 6);
}

static void hex(const uint8_t* d, size_t n, char* out) {
    static const char* H = "0123456789ABCDEF";
    for(size_t i = 0; i < n; i++) {
        out[i * 2] = H[d[i] >> 4];
        out[i * 2 + 1] = H[d[i] & 15];
    }
    out[n * 2] = 0;
}

// Copy a fixed-width tag string: stop at NUL, keep only plain printable ASCII.
static void str(const uint8_t* d, size_t n, char* out) {
    size_t j = 0;
    for(size_t i = 0; i < n && d[i]; i++)
        if(d[i] >= 0x20 && d[i] < 0x7f && d[i] != ',' && d[i] != '"') out[j++] = (char)d[i];
    while(j && out[j - 1] == ' ') j--;
    out[j] = 0;
}

static uint16_t u16(const uint8_t* d) {
    return (uint16_t)(d[0] | d[1] << 8);
}

bool bambu_parse(const BambuRaw* raw, BambuSpool* s) {
    memset(s, 0, sizeof(*s));
    const uint8_t* b1 = raw->blocks[0];
    const uint8_t* b2 = raw->blocks[1];
    const uint8_t* b4 = raw->blocks[2];
    const uint8_t* b5 = raw->blocks[3];
    const uint8_t* b9 = raw->blocks[4];
    const uint8_t* b12 = raw->blocks[5];
    const uint8_t* b14 = raw->blocks[6];
    const uint8_t* b16 = raw->blocks[7];

    hex(raw->uid, raw->uid_len, s->tag_uid);
    hex(b9, 16, s->tray_uid);
    str(b1, 8, s->variant_id);
    str(b1 + 8, 8, s->material_id);
    str(b2, 16, s->type);
    str(b4, 16, s->detail);
    memcpy(s->rgba, b5, 4);
    s->weight_g = u16(b5 + 4);
    memcpy(&s->diameter_mm, b5 + 8, 4); // little-endian float, same as the MCU
    str(b12, 16, s->produced);
    s->length_m = u16(b14 + 4);
    if(u16(b16) == 2) { // format 2 = color info
        s->color_count = u16(b16 + 2);
        // stored reversed (ABGR)
        s->rgba2[0] = b16[7];
        s->rgba2[1] = b16[6];
        s->rgba2[2] = b16[5];
        s->rgba2[3] = b16[4];
    }
    if(!(s->diameter_mm > 0.5f && s->diameter_mm < 5.0f)) s->diameter_mm = 0;

    // A real Bambu spool always has a filament type and a non-zero tray UID.
    bool tray_nonzero = false;
    for(int i = 0; i < 16; i++) tray_nonzero |= b9[i] != 0;
    return s->type[0] != 0 && tray_nonzero;
}

typedef struct {
    const char* name;
    uint8_t r, g, b;
} Named;

static const Named NAMES[] = {
    {"Black", 0x10, 0x10, 0x10},  {"White", 0xF5, 0xF5, 0xF5},  {"Gray", 0x80, 0x80, 0x80},
    {"Silver", 0xC0, 0xC0, 0xC0}, {"Red", 0xD0, 0x10, 0x20},    {"Dark Red", 0x80, 0x10, 0x10},
    {"Orange", 0xFF, 0x70, 0x10}, {"Yellow", 0xF8, 0xE0, 0x20}, {"Gold", 0xC8, 0xA0, 0x40},
    {"Green", 0x20, 0xA0, 0x40},  {"Dark Green", 0x10, 0x50, 0x20}, {"Lime", 0xA0, 0xE0, 0x30},
    {"Teal", 0x00, 0x80, 0x80},   {"Cyan", 0x30, 0xD0, 0xE0},   {"Blue", 0x10, 0x50, 0xD0},
    {"Navy", 0x10, 0x20, 0x60},   {"Purple", 0x70, 0x30, 0xA0}, {"Pink", 0xF0, 0x90, 0xC0},
    {"Magenta", 0xE0, 0x20, 0xA0}, {"Brown", 0x70, 0x40, 0x20}, {"Beige", 0xE8, 0xD8, 0xB0},
};

const char* bambu_color_name(const uint8_t c[4]) {
    const char* best = "?";
    long bd = 1L << 30;
    for(size_t i = 0; i < sizeof(NAMES) / sizeof(NAMES[0]); i++) {
        long dr = c[0] - NAMES[i].r, dg = c[1] - NAMES[i].g, db = c[2] - NAMES[i].b;
        long d = 3 * dr * dr + 4 * dg * dg + 2 * db * db;
        if(d < bd) {
            bd = d;
            best = NAMES[i].name;
        }
    }
    return best;
}

size_t bambu_csv_line(const BambuSpool* s, uint32_t ts, char* buf, size_t cap) {
    char c1[9], c2[9];
    hex(s->rgba, 4, c1);
    hex(s->rgba2, 4, c2);
    int d100 = (int)(s->diameter_mm * 100.0f + 0.5f);
    int n = snprintf(
        buf, cap, "%s,%s,%s,%s,%s,%s,%s,%s,%u,%u,%d.%02d,%u,%s,%lu\n",
        s->tray_uid, s->tag_uid, s->material_id, s->variant_id, s->type, s->detail, c1, c2,
        s->color_count, s->weight_g, d100 / 100, d100 % 100, s->length_m, s->produced,
        (unsigned long)ts);
    return n < 0 ? 0 : ((size_t)n >= cap ? cap - 1 : (size_t)n);
}
