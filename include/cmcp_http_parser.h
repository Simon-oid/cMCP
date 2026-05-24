#ifndef CMCP_HTTP_PARSER_H
#define CMCP_HTTP_PARSER_H

#include <stddef.h>

#define CMCP_HTTP_MAX_HEADERS  64

typedef struct {
    char *name;     /* trimmed, owned */
    char *value;    /* trimmed, owned */
} cmcp_http_header_t;

typedef struct {
    char               *method;    /* "POST", "GET", ... — owned */
    char               *target;    /* "/mcp", "/mcp?foo=bar", ... — owned */
    cmcp_http_header_t  headers[CMCP_HTTP_MAX_HEADERS];
    size_t              n_headers;
    char               *body;      /* NULL when Content-Length absent / 0 */
    size_t              body_len;
} cmcp_http_request_t;

/* Free all owned strings in `r` and zero the struct. Safe on a zeroed
 * struct and on a struct populated only partway through parsing. */
void cmcp_http_request_clear(cmcp_http_request_t *r);

/* Case-insensitive header lookup. Returns NULL if absent. */
const char *cmcp_http_header_get(const cmcp_http_request_t *r,
                                  const char *name);

/* Parse a request line + headers section in-place.
 *
 *   block         caller-owned writable buffer holding the head bytes.
 *                 MUST remain valid until the call returns. The parser
 *                 writes NULs into it to terminate parsed substrings.
 *   body_offset   byte offset of the start of the body (i.e. one past
 *                 the terminating "\r\n\r\n" or "\n\n"). Used to bound
 *                 the head's working range.
 *   out           OUT. On success, populated with owned strings.
 *                 On error, partially populated — caller must always
 *                 call cmcp_http_request_clear() to release whatever
 *                 was set.
 *
 * Returns 0 on success or a negative cmcp_err_t (CMCP_EPROTOCOL on
 * malformed input, CMCP_ENOMEM on allocation failure). */
int cmcp_http_parse_head(char *block, size_t body_offset,
                          cmcp_http_request_t *out);

#endif
