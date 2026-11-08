/* sha256.h — internal SHA-256 (FIPS 180-4) and HMAC-SHA256 (RFC 2104).
 *
 * Not a public header: it lives in src/ and is linked into the core.
 * cMCP has no crypto dependency, and MCP 2026-07-28 requires a server
 * to integrity-protect any `requestState` that drives its logic, so the
 * one primitive that needs is implemented here from the standards.
 * Verified against the NIST FIPS 180-4 examples and RFC 4231 vectors
 * in tests/test_seal.c. */
#ifndef CMCP_SHA256_H
#define CMCP_SHA256_H

#include <stddef.h>
#include <stdint.h>

#define CMCP_SHA256_LEN   32u  /* digest size in bytes */
#define CMCP_SHA256_BLOCK 64u  /* block size in bytes  */

typedef struct {
    uint32_t h[8];
    uint64_t total_len;               /* bytes absorbed so far */
    unsigned char buf[CMCP_SHA256_BLOCK];
    size_t   buf_len;
} cmcp_sha256_t;

void cmcp_sha256_init(cmcp_sha256_t *c);
void cmcp_sha256_update(cmcp_sha256_t *c, const void *data, size_t len);
void cmcp_sha256_final(cmcp_sha256_t *c, unsigned char out[CMCP_SHA256_LEN]);

/* One-shot digest. */
void cmcp_sha256(const void *data, size_t len,
                 unsigned char out[CMCP_SHA256_LEN]);

/* HMAC-SHA256 of `msg` under `key` (any length; keys longer than one
 * block are hashed first, per RFC 2104). */
void cmcp_hmac_sha256(const void *key, size_t key_len,
                      const void *msg, size_t msg_len,
                      unsigned char out[CMCP_SHA256_LEN]);

/* Constant-time equality of two equal-length buffers: the running time
 * does not depend on where they differ, so a MAC check leaks nothing
 * about how many leading bytes an attacker guessed. 1 if equal. */
int cmcp_ct_equal(const void *a, const void *b, size_t len);

#endif
