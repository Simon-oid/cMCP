/* test_seal — SHA-256, HMAC-SHA256, base64url and the requestState seal.
 *
 * The crypto is checked against published vectors (NIST FIPS 180-4
 * examples, RFC 4231, RFC 4648), so a transcription slip in the round
 * function or the padding shows up as a wrong digest, not as a seal
 * that happens to round-trip with itself. */

/* strdup(3) — POSIX. */
#define _POSIX_C_SOURCE 200809L

#include "test.h"
#include "cmcp.h"
#include "../src/sha256.h"
#include "../src/seal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void hex(const unsigned char *d, size_t n, char *out) {
    for (size_t i = 0; i < n; i++) sprintf(out + 2 * i, "%02x", d[i]);
}

static int sha_is(const void *msg, size_t len, const char *want) {
    unsigned char d[CMCP_SHA256_LEN];
    char h[2 * CMCP_SHA256_LEN + 1];
    cmcp_sha256(msg, len, d);
    hex(d, sizeof d, h);
    return strcmp(h, want) == 0;
}

static int hmac_is(const void *key, size_t klen, const char *msg,
                   const char *want) {
    unsigned char d[CMCP_SHA256_LEN];
    char h[2 * CMCP_SHA256_LEN + 1];
    cmcp_hmac_sha256(key, klen, msg, strlen(msg), d);
    hex(d, sizeof d, h);
    return strcmp(h, want) == 0;
}

static void test_sha256_nist_vectors(void) {
    TEST_ASSERT(sha_is("", 0,
        "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"));
    TEST_ASSERT(sha_is("abc", 3,
        "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"));
    /* 56 bytes: the padding spills into a second block. */
    const char *two = "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq";
    TEST_ASSERT(sha_is(two, strlen(two),
        "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1"));
}

static void test_sha256_million_a_streamed(void) {
    /* One million 'a', fed in uneven chunks to exercise buffering. */
    unsigned char chunk[997];
    memset(chunk, 'a', sizeof chunk);
    cmcp_sha256_t c;
    cmcp_sha256_init(&c);
    size_t left = 1000000;
    while (left > 0) {
        size_t n = left < sizeof chunk ? left : sizeof chunk;
        cmcp_sha256_update(&c, chunk, n);
        left -= n;
    }
    unsigned char d[CMCP_SHA256_LEN];
    char h[2 * CMCP_SHA256_LEN + 1];
    cmcp_sha256_final(&c, d);
    hex(d, sizeof d, h);
    TEST_ASSERT(strcmp(h,
        "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0") == 0);
}

static void test_hmac_rfc4231(void) {
    unsigned char k1[20];
    memset(k1, 0x0b, sizeof k1);
    TEST_ASSERT(hmac_is(k1, sizeof k1, "Hi There",
        "b0344c61d8db38535ca8afceaf0bf12b881dc200c9833da726e9376c2e32cff7"));
    TEST_ASSERT(hmac_is("Jefe", 4, "what do ya want for nothing?",
        "5bdcc146bf60754e6a042426089575c75a003f089d2739839dec58b964ec3843"));
    /* Test case 6: a key longer than the block is hashed first. */
    unsigned char k6[131];
    memset(k6, 0xaa, sizeof k6);
    TEST_ASSERT(hmac_is(k6, sizeof k6,
        "Test Using Larger Than Block-Size Key - Hash Key First",
        "60e431591ee0b67f0d8a26aacbf5b77f8e0bc6213728c5140546040f0ee37f54"));
}

static void test_ct_equal(void) {
    TEST_ASSERT(cmcp_ct_equal("abcd", "abcd", 4) == 1);
    TEST_ASSERT(cmcp_ct_equal("abcd", "abce", 4) == 0);
    TEST_ASSERT(cmcp_ct_equal("xbcd", "abcd", 4) == 0);
}

static void test_b64url_rfc4648(void) {
    static const char *const in[]  = {"", "f", "fo", "foo", "foob", "fooba", "foobar"};
    static const char *const out[] = {"", "Zg", "Zm8", "Zm9v", "Zm9vYg", "Zm9vYmE", "Zm9vYmFy"};
    for (size_t i = 0; i < sizeof in / sizeof in[0]; i++) {
        char *e = cmcp_b64url_encode((const unsigned char *)in[i], strlen(in[i]));
        TEST_ASSERT(e && strcmp(e, out[i]) == 0);
        size_t n = 0;
        unsigned char *d = e ? cmcp_b64url_decode(e, strlen(e), &n) : NULL;
        TEST_ASSERT(d && n == strlen(in[i]) && memcmp(d, in[i], n) == 0);
        free(e);
        free(d);
    }
    /* URL-safe alphabet: 0xfb 0xff → "-_8" (not "+/8"). */
    const unsigned char hi[] = {0xfb, 0xff};
    char *e = cmcp_b64url_encode(hi, sizeof hi);
    TEST_ASSERT(e && strcmp(e, "-_8") == 0);
    free(e);
}

static void test_b64url_rejects_bad_input(void) {
    size_t n = 0;
    TEST_ASSERT(cmcp_b64url_decode("Z", 1, &n) == NULL);     /* len % 4 == 1 */
    TEST_ASSERT(cmcp_b64url_decode("Zg=", 3, &n) == NULL);   /* padding char */
    TEST_ASSERT(cmcp_b64url_decode("Z+8", 3, &n) == NULL);   /* std alphabet */
    TEST_ASSERT(cmcp_b64url_decode("Zh", 2, &n) == NULL);    /* stray bits */
}

/* ---------------------------------------------------------------- */
/* Seal                                                              */
/* ---------------------------------------------------------------- */

static const unsigned char KEY[CMCP_SEAL_KEY_LEN] = "0123456789abcdef0123456789abcde";
static const unsigned char OTHER[CMCP_SEAL_KEY_LEN] = "fedcba9876543210fedcba987654321";

#define NOW 1000000LL

static char *make(const char *state, long long expiry) {
    char *tok = NULL;
    int rc = cmcp_seal_make(KEY, sizeof KEY, "tools/call", "deploy", state,
                            expiry, &tok);
    return rc == CMCP_OK ? tok : NULL;
}

static int open_rc(const unsigned char *key, const char *tok,
                   const char *method, const char *name, long long now) {
    char *state = NULL;
    int rc = cmcp_seal_open(key, CMCP_SEAL_KEY_LEN, tok, method, name, now,
                            &state);
    free(state);
    return rc;
}

static void test_seal_round_trip(void) {
    char *tok = make("step=2;env=prod", NOW + 60);
    TEST_ASSERT(tok != NULL);
    TEST_ASSERT(tok && strncmp(tok, "v1.", 3) == 0);
    char *state = NULL;
    TEST_ASSERT(cmcp_seal_open(KEY, sizeof KEY, tok, "tools/call", "deploy",
                               NOW, &state) == CMCP_OK);
    TEST_ASSERT(state && strcmp(state, "step=2;env=prod") == 0);
    free(state);
    free(tok);
}

static void test_seal_rejects_forgery(void) {
    char *tok = make("ok", NOW + 60);
    TEST_ASSERT(tok != NULL);
    if (!tok) return;
    TEST_ASSERT(open_rc(OTHER, tok, "tools/call", "deploy", NOW) == CMCP_EPROTOCOL);

    /* Flip one payload character, then one MAC character. */
    char *bad = strdup(tok);
    TEST_ASSERT(bad != NULL);
    if (!bad) { free(tok); return; }
    bad[5] = (char)(bad[5] == 'A' ? 'B' : 'A');
    TEST_ASSERT(open_rc(KEY, bad, "tools/call", "deploy", NOW) == CMCP_EPROTOCOL);
    strcpy(bad, tok);
    size_t last = strlen(bad) - 2;
    bad[last] = (char)(bad[last] == 'A' ? 'B' : 'A');
    TEST_ASSERT(open_rc(KEY, bad, "tools/call", "deploy", NOW) == CMCP_EPROTOCOL);
    free(bad);
    free(tok);

    TEST_ASSERT(open_rc(KEY, "plain-text", "tools/call", "deploy", NOW) == CMCP_EPROTOCOL);
    TEST_ASSERT(open_rc(KEY, "v1.", "tools/call", "deploy", NOW) == CMCP_EPROTOCOL);
    TEST_ASSERT(open_rc(KEY, "v1.e30", "tools/call", "deploy", NOW) == CMCP_EPROTOCOL);
}

static void test_seal_binds_request(void) {
    char *tok = make("ok", NOW + 60);
    TEST_ASSERT(tok != NULL);
    if (!tok) return;
    TEST_ASSERT(open_rc(KEY, tok, "prompts/get", "deploy", NOW) == CMCP_EPROTOCOL);
    TEST_ASSERT(open_rc(KEY, tok, "tools/call", "rollback", NOW) == CMCP_EPROTOCOL);
    TEST_ASSERT(open_rc(KEY, tok, "tools/call", NULL, NOW) == CMCP_EPROTOCOL);
    free(tok);
}

static void test_seal_expiry(void) {
    char *tok = make("ok", NOW);
    TEST_ASSERT(tok != NULL);
    if (!tok) return;
    TEST_ASSERT(open_rc(KEY, tok, "tools/call", "deploy", NOW) == CMCP_OK);
    TEST_ASSERT(open_rc(KEY, tok, "tools/call", "deploy", NOW + 1) == CMCP_EPROTOCOL);
    free(tok);
}

static void test_seal_rejects_oversized_token(void) {
    size_t n = CMCP_SEAL_MAX_TOKEN + 10;
    char *big = (char *)malloc(n + 1);
    TEST_ASSERT(big != NULL);
    if (!big) return;
    memcpy(big, "v1.", 3);
    memset(big + 3, 'A', n - 3);
    big[n] = '\0';
    TEST_ASSERT(open_rc(KEY, big, "tools/call", "deploy", NOW) == CMCP_EPROTOCOL);
    free(big);
}

int main(void) {
    TEST_RUN(test_sha256_nist_vectors);
    TEST_RUN(test_sha256_million_a_streamed);
    TEST_RUN(test_hmac_rfc4231);
    TEST_RUN(test_ct_equal);
    TEST_RUN(test_b64url_rfc4648);
    TEST_RUN(test_b64url_rejects_bad_input);
    TEST_RUN(test_seal_round_trip);
    TEST_RUN(test_seal_rejects_forgery);
    TEST_RUN(test_seal_binds_request);
    TEST_RUN(test_seal_expiry);
    TEST_RUN(test_seal_rejects_oversized_token);
    TEST_DONE();
}
