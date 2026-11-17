// Minimal SHA-256 / HMAC-SHA256 / HKDF-SHA256 (RFC 6234 / 2104 / 5869).
// The Flipper SDK does not export mbedtls to apps, so we carry our own.
#include "sha256.h"
#include <string.h>

typedef struct {
    uint32_t h[8];
    uint8_t buf[64];
    uint64_t len;
    size_t fill;
} Sha256;

static const uint32_t K[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
    0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
    0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
    0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
    0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2,
};

#define ROR(x, n) (((x) >> (n)) | ((x) << (32 - (n))))

static void sha256_block(Sha256* s, const uint8_t* p) {
    uint32_t w[64];
    for(int i = 0; i < 16; i++)
        w[i] = (uint32_t)p[i * 4] << 24 | (uint32_t)p[i * 4 + 1] << 16 | (uint32_t)p[i * 4 + 2] << 8 |
               p[i * 4 + 3];
    for(int i = 16; i < 64; i++) {
        uint32_t s0 = ROR(w[i - 15], 7) ^ ROR(w[i - 15], 18) ^ (w[i - 15] >> 3);
        uint32_t s1 = ROR(w[i - 2], 17) ^ ROR(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    uint32_t a = s->h[0], b = s->h[1], c = s->h[2], d = s->h[3];
    uint32_t e = s->h[4], f = s->h[5], g = s->h[6], h = s->h[7];
    for(int i = 0; i < 64; i++) {
        uint32_t t1 = h + (ROR(e, 6) ^ ROR(e, 11) ^ ROR(e, 25)) + ((e & f) ^ (~e & g)) + K[i] + w[i];
        uint32_t t2 = (ROR(a, 2) ^ ROR(a, 13) ^ ROR(a, 22)) + ((a & b) ^ (a & c) ^ (b & c));
        h = g; g = f; f = e; e = d + t1;
        d = c; c = b; b = a; a = t1 + t2;
    }
    s->h[0] += a; s->h[1] += b; s->h[2] += c; s->h[3] += d;
    s->h[4] += e; s->h[5] += f; s->h[6] += g; s->h[7] += h;
}

static void sha256_init(Sha256* s) {
    static const uint32_t iv[8] = {
        0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
    memcpy(s->h, iv, sizeof(iv));
    s->len = 0;
    s->fill = 0;
}

static void sha256_update(Sha256* s, const uint8_t* d, size_t n) {
    s->len += n;
    while(n--) {
        s->buf[s->fill++] = *d++;
        if(s->fill == 64) {
            sha256_block(s, s->buf);
            s->fill = 0;
        }
    }
}

static void sha256_final(Sha256* s, uint8_t out[32]) {
    uint64_t bits = s->len * 8;
    uint8_t pad = 0x80;
    sha256_update(s, &pad, 1);
    pad = 0;
    while(s->fill != 56) sha256_update(s, &pad, 1);
    uint8_t lenb[8];
    for(int i = 0; i < 8; i++) lenb[i] = (uint8_t)(bits >> (56 - 8 * i));
    sha256_update(s, lenb, 8);
    for(int i = 0; i < 8; i++) {
        out[i * 4] = s->h[i] >> 24;
        out[i * 4 + 1] = s->h[i] >> 16;
        out[i * 4 + 2] = s->h[i] >> 8;
        out[i * 4 + 3] = s->h[i];
    }
}

void sha256(const uint8_t* data, size_t len, uint8_t out[32]) {
    Sha256 s;
    sha256_init(&s);
    sha256_update(&s, data, len);
    sha256_final(&s, out);
}

// HMAC with the message given as up to three concatenated parts (enough for HKDF).
static void hmac3(
    const uint8_t* key, size_t klen,
    const uint8_t* m1, size_t n1,
    const uint8_t* m2, size_t n2,
    const uint8_t* m3, size_t n3,
    uint8_t out[32]) {
    uint8_t k[64] = {0};
    if(klen > 64)
        sha256(key, klen, k);
    else
        memcpy(k, key, klen);
    uint8_t ipad[64], opad[64];
    for(int i = 0; i < 64; i++) {
        ipad[i] = k[i] ^ 0x36;
        opad[i] = k[i] ^ 0x5c;
    }
    Sha256 s;
    uint8_t inner[32];
    sha256_init(&s);
    sha256_update(&s, ipad, 64);
    if(n1) sha256_update(&s, m1, n1);
    if(n2) sha256_update(&s, m2, n2);
    if(n3) sha256_update(&s, m3, n3);
    sha256_final(&s, inner);
    sha256_init(&s);
    sha256_update(&s, opad, 64);
    sha256_update(&s, inner, 32);
    sha256_final(&s, out);
}

void hmac_sha256(const uint8_t* key, size_t klen, const uint8_t* msg, size_t n, uint8_t out[32]) {
    hmac3(key, klen, msg, n, NULL, 0, NULL, 0, out);
}

void hkdf_sha256(
    const uint8_t* salt, size_t salt_len,
    const uint8_t* ikm, size_t ikm_len,
    const uint8_t* info, size_t info_len,
    uint8_t* out, size_t out_len) {
    uint8_t prk[32], t[32];
    hmac_sha256(salt, salt_len, ikm, ikm_len, prk);
    size_t done = 0, tlen = 0;
    for(uint8_t ctr = 1; done < out_len; ctr++) {
        hmac3(prk, 32, t, tlen, info, info_len, &ctr, 1, t);
        tlen = 32;
        size_t take = out_len - done < 32 ? out_len - done : 32;
        memcpy(out + done, t, take);
        done += take;
    }
}
