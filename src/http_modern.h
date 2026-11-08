/* http_modern.h — internal Streamable HTTP rules for MCP 2026-07-28.
 *
 * Not a public header: it lives in src/ and is linked into the core.
 *
 * In 2026-07-28 Streamable HTTP has no sessions, and every POST mirrors
 * selected body fields into headers so intermediaries can route on them
 * without parsing JSON (spec: basic/transports/streamable-http, "Request
 * Metadata"). A server that reads the body MUST check the two agree:
 *
 *   MCP-Protocol-Version  == params._meta["io.modelcontextprotocol/protocolVersion"]
 *   Mcp-Method            == method
 *   Mcp-Name              == params.name or params.uri
 *                            (tools/call, resources/read, prompts/get)
 *
 * and answer a mismatch with 400 and JSON-RPC -32020 (HeaderMismatch).
 * These helpers hold those rules, kept apart from the socket code so the
 * tests can drive them directly. */
#ifndef CMCP_HTTP_MODERN_H
#define CMCP_HTTP_MODERN_H

#include "cmcp_http_parser.h"
#include "cmcp_json.h"

#include <stddef.h>

/* Non-zero when this POST is a modern request: the body carries modern
 * `_meta`, or the MCP-Protocol-Version header names the modern revision
 * (a modern header on a body without `_meta` is then a mismatch, not a
 * legacy request). `body` may be NULL (unparseable). */
int cmcp_http_modern_is(const cmcp_json_t *body,
                        const cmcp_http_request_t *req);

/* Validate the mirrored headers of a modern request. Returns 0 when the
 * request may proceed. Otherwise returns the HTTP status to send (400)
 * and sets *out_body to a malloc'd JSON-RPC error frame: -32020 for a
 * missing, malformed or mismatched header, -32022 for an
 * MCP-Protocol-Version this build does not serve. */
int cmcp_http_modern_check(const cmcp_json_t *body,
                           const cmcp_http_request_t *req, char **out_body);

/* HTTP status for a modern JSON-RPC response body: 404 for -32601,
 * 400 for -32020/-32021/-32022, 200 for everything else (results, and
 * errors the spec gives no status for). */
int cmcp_http_modern_status(const char *resp, size_t len);

/* Decode a header value that may use the `=?base64?...?=` sentinel
 * (standard alphabet, padded). Plain values are copied as-is. Returns a
 * malloc'd NUL-terminated string, or NULL when the sentinel's payload is
 * not valid base64 (or decodes to bytes containing NUL). */
char *cmcp_http_header_decode(const char *value);

/* The client side of the same rule: `value` as is when it is a plain
 * header value (visible ASCII and inner spaces, no leading or trailing
 * whitespace, not itself shaped like the sentinel), otherwise
 * `=?base64?<base64 of the UTF-8 bytes>?=`. Malloc'd, or NULL. */
char *cmcp_http_header_encode(const char *value);

/* Client side: the request-metadata headers for a modern request body —
 * MCP-Protocol-Version (from _meta), Mcp-Method, Mcp-Name for
 * tools/call / resources/read / prompts/get, and Mcp-Param-<Name> for
 * each x-mcp-header argument of `tool_schema` (the tool's inputSchema,
 * may be NULL). Values are encoded with cmcp_http_header_encode. Each
 * header goes to add(ctx, "Name: value"); returns CMCP_ENOMEM or the
 * first non-zero add result, else CMCP_OK. */
typedef int (*cmcp_http_line_fn)(void *ctx, const char *line);
int cmcp_http_modern_request_headers(const cmcp_json_t *body,
                                     const cmcp_json_t *tool_schema,
                                     cmcp_http_line_fn add, void *ctx);

#endif
