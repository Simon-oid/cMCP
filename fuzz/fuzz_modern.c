/* libFuzzer harness for the MCP 2026-07-28 request surface.
 *
 * The input is an HTTP request: a head up to the first blank line, and
 * a JSON body after it. Everything a modern request feeds to the
 * library is driven from those bytes:
 *
 *   - Streamable HTTP rules: modern detection, the Mcp-* header checks,
 *     and the header value sentinel (decode(encode(v)) must equal v);
 *   - the era and MRTR checks of the request's _meta, inputResponses
 *     and requestState;
 *   - the subscriptions/listen filter;
 *   - the body as a tool schema: cmcp_schema_check and the x-mcp-header
 *     lint, then the per-call x-mcp-header comparison against the same
 *     headers. */

#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "cmcp.h"
#include "cmcp_http_parser.h"
#include "cmcp_json.h"
#include "cmcp_schema.h"
#include "cmcp_types.h"
#include "../src/era.h"
#include "../src/http_modern.h"
#include "../src/mrtr.h"
#include "../src/subscriptions.h"
#include "../src/xheader.h"

static const unsigned char KEY[32] = "fuzz-key-0123456789abcdef-01234";

static const char *get_header(void *ctx, const char *name) {
    return cmcp_http_header_get((const cmcp_http_request_t *)ctx, name);
}

/* decode(encode(v)) == v for every header value (NUL-free by nature). */
static void check_sentinel_roundtrip(const cmcp_http_request_t *r) {
    for (size_t i = 0; i < r->n_headers; i++) {
        const char *v = r->headers[i].value;
        if (!v) continue;
        char *enc = cmcp_http_header_encode(v);
        char *dec = enc ? cmcp_http_header_decode(enc) : NULL;
        if (enc && (!dec || strcmp(dec, v) != 0)) abort();
        free(enc);
        free(dec);
        free(cmcp_http_header_decode(v));   /* raw value as a sentinel */
    }
}

static void fuzz_request(const cmcp_json_t *body) {
    cmcp_rpc_message_t *msgs = NULL;
    size_t n = 0;
    char *text = cmcp_json_emit(body);
    if (text && cmcp_rpc_parse(text, strlen(text), &msgs, &n) == CMCP_OK &&
        n == 1 && msgs[0].kind == CMCP_MSG_REQUEST) {
        cmcp_rpc_message_t resp;
        cmcp_rpc_message_init(&resp);
        cmcp_era_check_request(&msgs[0], &resp);
        cmcp_rpc_message_clear(&resp);

        cmcp_mrtr_t m;
        cmcp_rpc_message_init(&resp);
        cmcp_mrtr_begin(&m, &msgs[0], KEY, sizeof KEY, &resp);
        cmcp_mrtr_finish(&m, &msgs[0], KEY, sizeof KEY, &resp);
        cmcp_mrtr_clear(&m);
        cmcp_rpc_message_clear(&resp);
    }
    cmcp_rpc_messages_free(msgs, n);
    free(text);

    cmcp_subs_t *subs = cmcp_subs_new();
    cmcp_json_t *id = cmcp_json_new_int(1);
    cmcp_json_t *ack = NULL;
    if (subs && id)
        cmcp_subs_listen(subs, id, cmcp_json_object_get(body, "params"), 0xFu, &ack);
    cmcp_json_free(ack);
    cmcp_json_free(cmcp_subs_match(subs, 0x8u, "a://x"));
    cmcp_json_free(id);
    cmcp_subs_free(subs);
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    size_t head_len = 0;
    for (size_t i = 0; i + 3 < size; i++) {
        if (memcmp(data + i, "\r\n\r\n", 4) == 0) { head_len = i + 4; break; }
    }
    if (head_len < 4) return 0;

    char *head = (char *)malloc(head_len + 1);
    if (!head) return 0;
    memcpy(head, data, head_len);
    head[head_len] = '\0';
    cmcp_http_request_t r;
    memset(&r, 0, sizeof r);
    if (cmcp_http_parse_head(head, head_len, &r) == CMCP_OK) {
        size_t blen = size - head_len;
        r.body = (char *)malloc(blen + 1);
        if (r.body) {
            memcpy(r.body, data + head_len, blen);
            r.body[blen] = '\0';
            r.body_len = blen;
        }
        check_sentinel_roundtrip(&r);
        cmcp_json_t *body = r.body ? cmcp_json_parse(r.body, r.body_len) : NULL;

        cmcp_http_modern_is(body, &r);
        char *err = NULL;
        cmcp_http_modern_check(body, &r, &err);
        if (err) cmcp_http_modern_status(err, strlen(err));
        free(err);

        if (body) {
            fuzz_request(body);
            cmcp_schema_error_t e;
            cmcp_schema_check(body, &e);
            cmcp_schema_error_clear(&e);
            if (cmcp_xheader_lint(body, &e) == CMCP_OK) {
                char *msg = NULL;
                cmcp_xheader_check(body, body, get_header, &r, &msg);
                free(msg);
            }
            cmcp_schema_error_clear(&e);
        }
        cmcp_json_free(body);
    }
    cmcp_http_request_clear(&r);
    free(head);
    return 0;
}
