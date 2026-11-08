/* xheader.h — internal `x-mcp-header` rules (MCP 2026-07-28).
 *
 * Not a public header: it lives in src/ and is linked into the core.
 *
 * A tool may mark properties of its inputSchema with
 * `"x-mcp-header": "<Name>"`. Over Streamable HTTP the client then
 * mirrors that argument into an `Mcp-Param-<Name>` header, so a gateway
 * can route on it without parsing the body (spec: server/tools
 * "x-mcp-header", basic/transports/streamable-http "Custom Headers").
 *
 *   lint   at registration: the annotation must be a non-empty HTTP
 *          token, unique case-insensitively, on a property of type
 *          string / integer / boolean that is reachable from the root
 *          through `properties` keys only.
 *   check  per HTTP call: each annotated argument present in the body
 *          must arrive in its header with the same value (base64
 *          sentinel decoded; integers compared numerically). */
#ifndef CMCP_XHEADER_H
#define CMCP_XHEADER_H

#include "cmcp_json.h"
#include "cmcp_schema.h"

#define CMCP_XHEADER_MAX 64   /* annotations per inputSchema */

/* CMCP_OK, or CMCP_EINVAL with *err filled (err may be NULL). */
int cmcp_xheader_lint(const cmcp_json_t *input_schema, cmcp_schema_error_t *err);

/* Header lookup for the request being checked; NULL when absent. */
typedef const char *(*cmcp_header_get_fn)(void *ctx, const char *name);

/* 0 when every annotated argument matches its header. Otherwise 1 with
 * *out_msg a malloc'd "Header mismatch: ..." message for the -32020
 * error (or NULL on allocation failure). */
int cmcp_xheader_check(const cmcp_json_t *input_schema, const cmcp_json_t *args,
                       cmcp_header_get_fn get, void *get_ctx, char **out_msg);

#endif
