/* era.h — internal helpers for the two MCP protocol eras.
 *
 * Not a public header: it lives in src/ and is linked into the core.
 *
 * MCP 2026-07-28 calls an implementation "dual-era" when it speaks both
 *   - legacy revisions (2025-11-25 and earlier): an `initialize`
 *     handshake opens a session that fixes version and capabilities;
 *   - modern revisions (2026-07-28 and later): no handshake, every
 *     request carries its protocol version and client capabilities in
 *     `params._meta`, and the server keeps no state between requests.
 *
 * These helpers hold the modern rules that do not depend on the
 * server's internals: recognising a modern request, validating its
 * `_meta`, the set of methods the modern path serves, and the fields
 * every modern result must carry. The dispatch itself stays in
 * server.c. */
#ifndef CMCP_ERA_H
#define CMCP_ERA_H

#include "cmcp_json.h"
#include "cmcp_types.h"

/* Reserved per-request / per-result `_meta` keys (spec: basic/index). */
#define CMCP_META_PROTOCOL_VERSION "io.modelcontextprotocol/protocolVersion"
#define CMCP_META_CLIENT_CAPS      "io.modelcontextprotocol/clientCapabilities"
#define CMCP_META_CLIENT_INFO      "io.modelcontextprotocol/clientInfo"
#define CMCP_META_SERVER_INFO      "io.modelcontextprotocol/serverInfo"

/* Non-zero when `params._meta` carries either required modern key.
 * Either key is enough: a modern request missing the other one must be
 * rejected with -32602, not mistaken for a legacy request. */
int cmcp_era_is_modern(const cmcp_json_t *params);

/* Check the required modern `_meta` fields and the requested version.
 * Returns CMCP_OK when the request can be served. Otherwise fills
 * *resp with the error the spec requires and returns CMCP_EPROTOCOL:
 *   -32602 (invalid params)  a required field is missing or mistyped;
 *   -32022 (unsupported)     the version is not one this build serves,
 *                            with data.supported and data.requested. */
int cmcp_era_check_request(const cmcp_rpc_message_t *req,
                           cmcp_rpc_message_t *resp);

/* Non-zero if the server's modern path serves `method`. Methods removed
 * in 2026-07-28 (ping, logging/setLevel, resources/(un)subscribe) and
 * the legacy handshake are not on the list. */
int cmcp_era_modern_method_allowed(const char *method);

/* New array of every protocol version this build serves, modern first.
 * Caller owns the result; NULL on allocation failure. */
cmcp_json_t *cmcp_era_supported_versions(void);

/* New `server/discover` result: supportedVersions plus `caps`, which
 * the result takes ownership of (it is freed on failure too). Fields
 * common to every modern result are added by cmcp_era_finish_result.
 * NULL on allocation failure. */
cmcp_json_t *cmcp_era_discover_result(cmcp_json_t *caps);

/* Complete a modern result in place:
 *   - `resultType: "complete"` unless the handler already set one;
 *   - `_meta["io.modelcontextprotocol/serverInfo"]` = {name, version},
 *     keeping any other `_meta` keys already present;
 *   - for cacheable methods, `ttlMs` and `cacheScope` unless present.
 * Returns CMCP_OK, CMCP_EINVAL on a non-object result, or CMCP_ENOMEM. */
int cmcp_era_finish_result(cmcp_json_t *result, const char *method,
                           const char *server_name,
                           const char *server_version);

#endif
