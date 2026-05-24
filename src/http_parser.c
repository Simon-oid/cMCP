/* HTTP/1.x request line + headers parser.
 *
 * Pure, hand-rolled, no I/O — the socket-driven read loop lives in
 * transport_http.c and calls into here. Extracted so the parser can be
 * fuzzed in isolation (libFuzzer harness in fuzz/fuzz_http.c) without
 * spinning up sockets. Behaviour is unchanged from the original
 * static implementation inside transport_http.c. */

#define _POSIX_C_SOURCE 200809L

#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "cmcp.h"
#include "cmcp_http_parser.h"

void cmcp_http_request_clear(cmcp_http_request_t *r) {
    if (!r) return;
    free(r->method);
    free(r->target);
    for (size_t i = 0; i < r->n_headers; i++) {
        free(r->headers[i].name);
        free(r->headers[i].value);
    }
    free(r->body);
    memset(r, 0, sizeof *r);
}

const char *cmcp_http_header_get(const cmcp_http_request_t *r,
                                  const char *name) {
    if (!r || !name) return NULL;
    for (size_t i = 0; i < r->n_headers; i++) {
        if (strcasecmp(r->headers[i].name, name) == 0)
            return r->headers[i].value;
    }
    return NULL;
}

static int starts_with_ci(const char *s, const char *prefix) {
    while (*prefix) {
        if (!*s) return 0;
        char a = *s++, b = *prefix++;
        if (a >= 'A' && a <= 'Z') a = (char)(a + 32);
        if (b >= 'A' && b <= 'Z') b = (char)(b + 32);
        if (a != b) return 0;
    }
    return 1;
}

/* Trim ASCII whitespace in place (returns possibly-shifted start). */
static char *trim_inplace(char *s) {
    while (*s == ' ' || *s == '\t') s++;
    char *end = s + strlen(s);
    while (end > s && (end[-1] == ' ' || end[-1] == '\t' ||
                        end[-1] == '\r' || end[-1] == '\n'))
        *--end = '\0';
    return s;
}

int cmcp_http_parse_head(char *block, size_t body_offset,
                          cmcp_http_request_t *out) {
    memset(out, 0, sizeof *out);
    /* Replace the empty line with a NUL so we work in the head only. */
    block[body_offset >= 2 ? body_offset - 2 : 0] = '\0';

    char *line = block;
    char *next = strchr(line, '\n');
    if (!next) return CMCP_EPROTOCOL;
    *next = '\0';
    if (next > line && next[-1] == '\r') next[-1] = '\0';

    /* Request line: METHOD SP TARGET SP HTTP/1.x */
    char *sp1 = strchr(line, ' ');
    if (!sp1) return CMCP_EPROTOCOL;
    *sp1 = '\0';
    char *target = sp1 + 1;
    char *sp2 = strchr(target, ' ');
    if (!sp2) return CMCP_EPROTOCOL;
    *sp2 = '\0';
    /* sp2+1 is the version; we don't validate beyond the prefix. */
    if (!starts_with_ci(sp2 + 1, "HTTP/1.")) return CMCP_EPROTOCOL;

    out->method = strdup(line);
    out->target = strdup(target);
    if (!out->method || !out->target) return CMCP_ENOMEM;

    /* Headers: Name: Value lines until empty. */
    line = next + 1;
    while (line && *line) {
        char *eol = strchr(line, '\n');
        if (eol) {
            *eol = '\0';
            if (eol > line && eol[-1] == '\r') eol[-1] = '\0';
        }
        if (*line == '\0') break;
        char *colon = strchr(line, ':');
        if (!colon) return CMCP_EPROTOCOL;
        *colon = '\0';
        char *name  = trim_inplace(line);
        char *value = trim_inplace(colon + 1);
        if (out->n_headers >= CMCP_HTTP_MAX_HEADERS) return CMCP_EPROTOCOL;

        out->headers[out->n_headers].name  = strdup(name);
        out->headers[out->n_headers].value = strdup(value);
        if (!out->headers[out->n_headers].name ||
            !out->headers[out->n_headers].value) return CMCP_ENOMEM;
        out->n_headers++;

        line = eol ? eol + 1 : NULL;
    }
    return CMCP_OK;
}
