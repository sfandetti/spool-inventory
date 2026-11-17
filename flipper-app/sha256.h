#pragma once
#include <stddef.h>
#include <stdint.h>

void sha256(const uint8_t* data, size_t len, uint8_t out[32]);
void hmac_sha256(const uint8_t* key, size_t klen, const uint8_t* msg, size_t n, uint8_t out[32]);
void hkdf_sha256(
    const uint8_t* salt, size_t salt_len,
    const uint8_t* ikm, size_t ikm_len,
    const uint8_t* info, size_t info_len,
    uint8_t* out, size_t out_len);
