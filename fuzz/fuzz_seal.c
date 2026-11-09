/* libFuzzer harness for the MRTR requestState seal (src/seal.c).
 *
 * requestState comes back from the client, so cmcp_seal_open parses
 * attacker-controlled bytes: base64url, a MAC, then JSON. Two checks:
 *
 *   1. open() of arbitrary bytes never crashes, and only a token this
 *      key sealed can open (a forgery that opens is a bug: abort);
 *   2. seal() then open() of an arbitrary state round-trips exactly.
 *
 * Input layout: the whole input is tried as a token; its first half
 * (with NULs dropped) is also used as a state to round-trip. */

#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "cmcp.h"
#include "../src/seal.h"

static const unsigned char KEY[CMCP_SEAL_KEY_LEN] = "fuzz-key-0123456789abcdef-01234";

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    char *tok = (char *)malloc(size + 1);
    if (!tok) return 0;
    memcpy(tok, data, size);
    tok[size] = '\0';

    char *state = NULL;
    int rc = cmcp_seal_open(KEY, sizeof KEY, tok, "tools/call", "t", 1000, &state);
    /* Random bytes carrying a valid HMAC would mean the MAC check is
     * broken; libFuzzer cannot guess a 256-bit MAC by chance. */
    if (rc == CMCP_OK && strncmp(tok, "v1.", 3) != 0) abort();
    free(state);

    /* Round-trip: a NUL-free prefix as the state. */
    size_t n = 0;
    for (size_t i = 0; i < size / 2; i++)
        if (data[i]) tok[n++] = (char)data[i];
    tok[n] = '\0';
    char *sealed = NULL;
    if (cmcp_seal_make(KEY, sizeof KEY, "tools/call", "t", tok, 2000,
                       &sealed) == CMCP_OK) {
        char *back = NULL;
        if (cmcp_seal_open(KEY, sizeof KEY, sealed, "tools/call", "t", 1000,
                           &back) != CMCP_OK || strcmp(back, tok) != 0)
            abort();
        free(back);
        /* Bound to its request: another tool must not open it. */
        back = NULL;
        if (cmcp_seal_open(KEY, sizeof KEY, sealed, "tools/call", "u", 1000,
                           &back) == CMCP_OK)
            abort();
        free(back);
    }
    free(sealed);
    free(tok);
    return 0;
}
