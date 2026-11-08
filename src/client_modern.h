/* client_modern.h — internal client-side rules for MCP 2026-07-28.
 *
 * Not a public header: it lives in src/ and is linked into the client.
 *
 * A modern client keeps no session: every request carries its protocol
 * version, capabilities and identity in `params._meta`, and a request a
 * server cannot finish without client input comes back as
 * `resultType: "input_required"`. The client then answers the embedded
 * input requests (elicitation, sampling, roots) and retries the same
 * request with `inputResponses` and the server's opaque `requestState`
 * (spec: basic/patterns/mrtr, "Client Requirements").
 *
 * These helpers are pure functions of their arguments; client.c owns
 * the state (mode, identity, handlers) and the wire. */
#ifndef CMCP_CLIENT_MODERN_H
#define CMCP_CLIENT_MODERN_H

#include "cmcp_json.h"
#include "cmcp_types.h"

/* Retry rounds before a call that keeps asking for input gives up. */
#define CMCP_CLIENT_MRTR_MAX_ROUNDS 8

/* The per-request `_meta`: protocolVersion, clientCapabilities (consumed),
 * clientInfo {name, version, description?} and, when `log_level` is
 * non-NULL, logLevel. NULL on allocation failure. */
cmcp_json_t *cmcp_cmodern_meta(const char *name, const char *version,
                               const char *description, cmcp_json_t *caps,
                               const char *log_level);

/* Merge `meta` (consumed) into params._meta, creating params (NULL) or
 * _meta as needed; keys already in params._meta (e.g. progressToken) are
 * kept. Consumes params; returns the result or NULL on failure. */
cmcp_json_t *cmcp_cmodern_attach(cmcp_json_t *params, cmcp_json_t *meta);

/* The requests a server may answer with input_required. */
int cmcp_cmodern_supports_mrtr(const char *method);

/* Non-zero when `result` is an InputRequiredResult. */
int cmcp_cmodern_input_required(const cmcp_json_t *result);

/* Answer one embedded input request (`method` is elicitation/create,
 * sampling/createMessage or roots/list). On CMCP_OK *out is the owned
 * result object. */
typedef int (*cmcp_cmodern_answer_fn)(const char *method,
                                      const cmcp_json_t *params, void *ctx,
                                      cmcp_json_t **out);

/* Params for the retry of a request whose result asked for input: a copy
 * of `orig` (the params as first sent, without _meta) plus
 * `inputResponses` built by calling `answer` for every entry of
 * result.inputRequests, plus result.requestState echoed verbatim when
 * present (and absent when not). CMCP_OK with *out owned, or the first
 * failing answer's code, or CMCP_EPROTOCOL for a malformed result. */
int cmcp_cmodern_retry_params(const cmcp_json_t *orig,
                              const cmcp_json_t *result,
                              cmcp_cmodern_answer_fn answer, void *ctx,
                              cmcp_json_t **out);

/* Non-zero when a server/discover result lists 2026-07-28. */
int cmcp_cmodern_discover_ok(const cmcp_json_t *result);

#endif
