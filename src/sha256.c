/* sha256.c — SHA-256 (FIPS 180-4 §6.2) and HMAC-SHA256 (RFC 2104).
 *
 * Straight transcription of the standard: a 64-round compression
 * function over 512-bit blocks, big-endian message words, and the
 * usual 0x80-then-zeros-then-64-bit-length padding. */
#include "sha256.h"

#include <string.h>

/* First 32 bits of the fractional parts of the cube roots of the first
 * 64 primes (FIPS 180-4 §4.2.2). */
static const uint32_t K[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1,
    0x923f82a4, 0xab1c5ed5, 0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3,
    0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174, 0xe49b69c1, 0xefbe4786,
    0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147,
    0x06ca6351, 0x14292967, 0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13,
    0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85, 0xa2bfe8a1, 0xa81a664b,
    0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a,
    0x5b9cca4f, 0x682e6ff3, 0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208,
    0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2,
};

static uint32_t rotr(uint32_t x, unsigned n) {
    return (x >> n) | (x << (32u - n));
}

static uint32_t load_be32(const unsigned char *p) {
    return ((uint32_t)p[0] << 24u) | ((uint32_t)p[1] << 16u) |
           ((uint32_t)p[2] << 8u)  |  (uint32_t)p[3];
}

static void store_be32(unsigned char *p, uint32_t v) {
    p[0] = (unsigned char)(v >> 24u);
    p[1] = (unsigned char)(v >> 16u);
    p[2] = (unsigned char)(v >> 8u);
    p[3] = (unsigned char)v;
}

/* Process one 64-byte block (FIPS 180-4 §6.2.2). */
static void compress(uint32_t h[8], const unsigned char block[64]) {
    uint32_t w[64];
    for (size_t t = 0; t < 16; t++) w[t] = load_be32(block + (size_t)4 * t);
    for (unsigned t = 16; t < 64; t++) {
        uint32_t s0 = rotr(w[t - 15], 7) ^ rotr(w[t - 15], 18) ^ (w[t - 15] >> 3u);
        uint32_t s1 = rotr(w[t - 2], 17) ^ rotr(w[t - 2], 19) ^ (w[t - 2] >> 10u);
        w[t] = w[t - 16] + s0 + w[t - 7] + s1;
    }

    uint32_t a = h[0], b = h[1], c = h[2], d = h[3];
    uint32_t e = h[4], f = h[5], g = h[6], k = h[7];
    for (unsigned t = 0; t < 64; t++) {
        uint32_t S1 = rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25);
        uint32_t ch = (e & f) ^ (~e & g);
        uint32_t t1 = k + S1 + ch + K[t] + w[t];
        uint32_t S0 = rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22);
        uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
        uint32_t t2 = S0 + maj;
        k = g; g = f; f = e; e = d + t1;
        d = c; c = b; b = a; a = t1 + t2;
    }
    h[0] += a; h[1] += b; h[2] += c; h[3] += d;
    h[4] += e; h[5] += f; h[6] += g; h[7] += k;
}

void cmcp_sha256_init(cmcp_sha256_t *c) {
    /* Fractional parts of the square roots of the first 8 primes. */
    static const uint32_t H0[8] = {
        0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
        0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19,
    };
    memcpy(c->h, H0, sizeof H0);
    c->total_len = 0;
    c->buf_len = 0;
}

void cmcp_sha256_update(cmcp_sha256_t *c, const void *data, size_t len) {
    const unsigned char *p = (const unsigned char *)data;
    c->total_len += len;
    while (len > 0) {
        size_t take = CMCP_SHA256_BLOCK - c->buf_len;
        if (take > len) take = len;
        memcpy(c->buf + c->buf_len, p, take);
        c->buf_len += take;
        p += take;
        len -= take;
        if (c->buf_len == CMCP_SHA256_BLOCK) {
            compress(c->h, c->buf);
            c->buf_len = 0;
        }
    }
}

void cmcp_sha256_final(cmcp_sha256_t *c, unsigned char out[CMCP_SHA256_LEN]) {
    /* Capture the length before padding: update() keeps counting. */
    uint64_t bits = c->total_len * 8u;
    static const unsigned char pad = 0x80;
    static const unsigned char zero[CMCP_SHA256_BLOCK] = {0};

    /* 0x80, then zeros until 8 bytes short of a block boundary. */
    cmcp_sha256_update(c, &pad, 1);
    size_t fill = (c->buf_len <= 56) ? 56 - c->buf_len
                                     : CMCP_SHA256_BLOCK + 56 - c->buf_len;
    cmcp_sha256_update(c, zero, fill);

    unsigned char len_be[8];
    for (unsigned i = 0; i < 8; i++)
        len_be[i] = (unsigned char)(bits >> (56u - 8u * i));
    cmcp_sha256_update(c, len_be, 8);   /* completes the last block */

    for (size_t i = 0; i < 8; i++) store_be32(out + (size_t)4 * i, c->h[i]);
    memset(c, 0, sizeof *c);
}

void cmcp_sha256(const void *data, size_t len,
                 unsigned char out[CMCP_SHA256_LEN]) {
    cmcp_sha256_t c;
    cmcp_sha256_init(&c);
    cmcp_sha256_update(&c, data, len);
    cmcp_sha256_final(&c, out);
}

void cmcp_hmac_sha256(const void *key, size_t key_len,
                      const void *msg, size_t msg_len,
                      unsigned char out[CMCP_SHA256_LEN]) {
    unsigned char k[CMCP_SHA256_BLOCK] = {0};
    if (key_len > CMCP_SHA256_BLOCK) {
        cmcp_sha256(key, key_len, k);
    } else if (key_len > 0) {
        memcpy(k, key, key_len);
    }

    unsigned char ipad[CMCP_SHA256_BLOCK], opad[CMCP_SHA256_BLOCK];
    for (unsigned i = 0; i < CMCP_SHA256_BLOCK; i++) {
        ipad[i] = (unsigned char)(k[i] ^ 0x36u);
        opad[i] = (unsigned char)(k[i] ^ 0x5cu);
    }

    unsigned char inner[CMCP_SHA256_LEN];
    cmcp_sha256_t c;
    cmcp_sha256_init(&c);
    cmcp_sha256_update(&c, ipad, sizeof ipad);
    cmcp_sha256_update(&c, msg, msg_len);
    cmcp_sha256_final(&c, inner);

    cmcp_sha256_init(&c);
    cmcp_sha256_update(&c, opad, sizeof opad);
    cmcp_sha256_update(&c, inner, sizeof inner);
    cmcp_sha256_final(&c, out);

    memset(k, 0, sizeof k);
    memset(ipad, 0, sizeof ipad);
    memset(opad, 0, sizeof opad);
}

int cmcp_ct_equal(const void *a, const void *b, size_t len) {
    const volatile unsigned char *x = (const volatile unsigned char *)a;
    const volatile unsigned char *y = (const volatile unsigned char *)b;
    unsigned char diff = 0;
    for (size_t i = 0; i < len; i++) diff |= (unsigned char)(x[i] ^ y[i]);
    return diff == 0;
}
