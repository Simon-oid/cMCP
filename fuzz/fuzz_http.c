/* libFuzzer harness for cmcp_http_parse_head.
 *
 * The pure parser was extracted from transport_http.c (it used to be a
 * static helper) into src/http_parser.c specifically so this harness
 * could exist — no sockets, no listen, just bytes-in -> struct-out.
 *
 * The parser mutates `block` in-place (writes NULs to terminate
 * substrings) and reads byte at `body_offset - 2` when body_offset >= 2.
 * We hand it a writable copy of the libFuzzer-provided buffer with one
 * extra slack byte for NUL termination. body_offset is the full input
 * size — the harness intentionally biases toward "no body" since the
 * point is to stress the head parser. */

#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "cmcp_http_parser.h"

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    /* Parser writes at block[body_offset-2]; require room. */
    if (size < 2) return 0;

    char *buf = (char *)malloc(size + 1);
    if (!buf) return 0;
    memcpy(buf, data, size);
    buf[size] = '\0';

    cmcp_http_request_t r;
    cmcp_http_parse_head(buf, size, &r);
    /* clear is safe whether parse_head returned OK or an error
     * partway through — it walks n_headers and frees whatever was set. */
    cmcp_http_request_clear(&r);

    free(buf);
    return 0;
}
