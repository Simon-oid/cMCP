#ifndef CMCP_SERVER_H
#define CMCP_SERVER_H

#include "cmcp_json.h"
#include "cmcp_types.h"
#include "cmcp_transport.h"

typedef struct cmcp_server cmcp_server_t;

/* Allocate a server. name/version are copied. */
cmcp_server_t *cmcp_server_new(const char *name, const char *version);
void           cmcp_server_free(cmcp_server_t *s);

/* Declare what this server can do. Caller's struct is copied. If never
 * called, defaults are all-zero (no optional capabilities).
 *
 * NOTE: the `tools` capability is *also* implicitly declared whenever
 * one or more tools have been registered via cmcp_server_add_tool().
 * Set `tools_list_changed = 1` only if you intend to actually emit
 * `notifications/tools/list_changed` (Phase 2.4). */
void cmcp_server_set_capabilities(cmcp_server_t *s,
                                   const cmcp_server_capabilities_t *caps);

/* ====================================================================== */
/* Handler context                                                         */
/* ====================================================================== */

/* Opaque per-call handle the library passes to every handler. It is the
 * handler's channel back to the run loop for two things: cooperative
 * cancellation and progress reporting. Valid only for the duration of
 * the handler call — do not retain the pointer past it. */
typedef struct cmcp_handler_ctx cmcp_handler_ctx_t;

/* Non-zero once the peer has asked to cancel this call (sent a
 * `notifications/cancelled` for this request id) or the handler timeout
 * has elapsed. A long-running handler should poll this and return early
 * when set; cancellation is cooperative — a handler that never polls
 * simply runs to completion. NULL-safe (returns 0). */
int cmcp_handler_cancelled(const cmcp_handler_ctx_t *hctx);

/* Emit a `notifications/progress` update for this call. `progress` is
 * the amount done so far; `total` is the expected total, or negative if
 * unknown. `message` is an optional human-readable status (may be
 * NULL). If the caller attached no progressToken this is a no-op
 * returning CMCP_OK — a handler never has to branch on that. NULL-safe. */
int cmcp_handler_progress(cmcp_handler_ctx_t *hctx,
                          double progress, double total,
                          const char *message);

/* Attach a structured result to this tool call. The library wraps it
 * as `structuredContent` in the tools/call response. If the tool has
 * an `output_schema`, the value is validated against it before send;
 * a validation failure surfaces as -32603 INTERNAL_ERROR per spec
 * ("server MUST provide structuredContent that matches the schema").
 *
 * `value` is consumed (caller transfers ownership). Pass NULL to
 * clear a previously-set value. Calling on a non-tool handler ctx
 * (resource/prompt) is a no-op — only tool results carry
 * structuredContent. NULL-safe on hctx. */
void cmcp_handler_set_structured(cmcp_handler_ctx_t *hctx,
                                  cmcp_json_t *value);

/* ====================================================================== */
/* Tool registry                                                           */
/* ====================================================================== */

/* Handler for a `tools/call` invocation.
 *
 *   `arguments`      The parsed `params.arguments` object from the
 *                    request. May be NULL if the caller sent no args.
 *                    Borrowed — handler must NOT free.
 *   `userdata`       Whatever was passed at registration time.
 *   `hctx`           Per-call handle — cancellation + progress. See
 *                    cmcp_handler_cancelled / cmcp_handler_progress.
 *   `out_content`    OUT. Handler must set to an owned cmcp_json_t array
 *                    of content items, e.g. [{"type":"text","text":...}].
 *                    Library wraps NULL as an empty array. The library
 *                    takes ownership on success.
 *   `out_is_error`   OUT. Set to non-zero to mark this as a *tool-level*
 *                    error (the call ran, but the operation failed —
 *                    e.g. file-not-found). The response still carries
 *                    `content`; the spec says clients render it as the
 *                    error message. JSON-RPC errors are reserved for
 *                    *protocol* failures and bypass this path entirely.
 *
 * Return CMCP_OK on success. Any non-zero return is treated as an
 * INTERNAL_ERROR (-32603) and `out_*` values are ignored. */
typedef int (*cmcp_tool_handler_fn)(const cmcp_json_t *arguments,
                                     void *userdata,
                                     cmcp_handler_ctx_t *hctx,
                                     cmcp_json_t **out_content,
                                     int *out_is_error);

/* Tool descriptor (caller-owned, copied by add_tool). */
typedef struct {
    const char            *name;            /* required, unique per server */
    const char            *title;           /* optional, human display name */
    const char            *description;     /* optional, may be NULL */
    /* JSON Schema for the input as JSON text. May be NULL. Phase 1.6
     * will validate inbound `arguments` against this. */
    const char            *input_schema;
    /* JSON Schema for the structured output as JSON text. May be NULL.
     * When set, any value the handler attaches via
     * cmcp_handler_set_structured is validated against this schema
     * before being sent — a mismatch surfaces as a -32603 internal
     * error per spec ("server MUST provide structuredContent that
     * matches"). Independent of `input_schema`. */
    const char            *output_schema;
    cmcp_tool_handler_fn   handler;         /* required */
    void                  *userdata;        /* opaque, kept verbatim */
} cmcp_tool_t;

/* Register a tool. Strings/schema are deep-copied. The schema string
 * is parsed eagerly: malformed JSON returns CMCP_EPARSE.
 *
 * Returns:
 *   CMCP_OK         on success
 *   CMCP_EINVAL     on missing required fields
 *   CMCP_EPARSE     if `input_schema` is non-NULL and not valid JSON
 *   CMCP_EPROTOCOL  if a tool with the same name is already registered
 *   CMCP_ENOMEM     on allocation failure
 *
 * MUST be called BEFORE `cmcp_server_run()`. Adding tools concurrently
 * with the run loop is unsupported in v0.1. */
int cmcp_server_add_tool(cmcp_server_t *s, const cmcp_tool_t *tool);

/* Convenience: build a content-array containing a single text item.
 * Caller assigns the result into `*out_content` from a tool handler;
 * library will free it after emitting the response. Returns NULL on
 * allocation failure. */
cmcp_json_t *cmcp_tool_text_content(const char *text);

/* Convenience: build a content-array containing a single resource_link
 * item — `{type: "resource_link", uri, name, description?, mimeType?}`
 * per the 2025-06-18 spec. Use when a tool wants to point at a
 * resource instead of inlining its content. Multiple items can be
 * appended to a single array via cmcp_json_array_append; use this
 * helper for the first one and build subsequent ones inline. NULL
 * uri/name → returns NULL. */
cmcp_json_t *cmcp_tool_resource_link_content(const char *uri,
                                              const char *name,
                                              const char *description,
                                              const char *mime_type);

/* ====================================================================== */
/* Resource registry                                                       */
/* ====================================================================== */

/* Handler for a `resources/read` invocation.
 *
 *   `uri`           The URI the caller asked for. Always equals the
 *                   resource's registered URI (the dispatcher matches
 *                   before invoking). Borrowed.
 *   `userdata`      Whatever was passed at registration time.
 *   `hctx`          Per-call handle — cancellation + progress. See
 *                   cmcp_handler_cancelled / cmcp_handler_progress.
 *   `out_contents`  OUT. Owned cmcp_json_t array of content items, e.g.
 *                   [{"uri":..., "mimeType":..., "text":...}]. The
 *                   library takes ownership on success. NULL is treated
 *                   as an empty array.
 *   `out_is_error`  OUT. Set to non-zero to mark this as a resource-
 *                   level error (analogous to tool-level error). The
 *                   wire still carries `contents`; clients render it
 *                   as the error message.
 *
 * Return CMCP_OK on success. Any non-zero return is treated as an
 * INTERNAL_ERROR (-32603) and `out_*` values are ignored. */
typedef int (*cmcp_resource_read_fn)(const char *uri,
                                      void *userdata,
                                      cmcp_handler_ctx_t *hctx,
                                      cmcp_json_t **out_contents,
                                      int *out_is_error);

/* Resource descriptor (caller-owned, copied by add_resource). */
typedef struct {
    const char            *uri;          /* required, unique per server */
    const char            *name;         /* required, programmatic id */
    const char            *title;        /* optional, human display name —
                                          * distinct from `name`, which
                                          * the spec scopes to identifier
                                          * use; `title` is what UIs show */
    const char            *description;  /* optional */
    const char            *mime_type;    /* optional, e.g. "text/plain" */
    cmcp_resource_read_fn  read;         /* required */
    void                  *userdata;
} cmcp_resource_t;

/* Register a resource. Strings are deep-copied. Duplicate URIs rejected
 * with CMCP_EPROTOCOL.
 *
 * MUST be called BEFORE cmcp_server_run(). */
int cmcp_server_add_resource(cmcp_server_t *s, const cmcp_resource_t *r);

/* Convenience: build a contents-array containing a single text item.
 * mime_type may be NULL. Returns NULL on allocation failure. */
cmcp_json_t *cmcp_resource_text_contents(const char *uri,
                                          const char *mime_type,
                                          const char *text);

/* ====================================================================== */
/* Prompt registry                                                         */
/* ====================================================================== */

/* Handler for a `prompts/get` invocation.
 *
 *   `arguments`    The parsed `params.arguments` object from the
 *                  request. May be NULL. Borrowed.
 *   `userdata`     Whatever was passed at registration time.
 *   `hctx`         Per-call handle — cancellation + progress. See
 *                  cmcp_handler_cancelled / cmcp_handler_progress.
 *   `out_messages` OUT. Owned cmcp_json_t array of message objects:
 *                  [{"role":"user|assistant", "content":{...}}]. The
 *                  library takes ownership on success. NULL is treated
 *                  as an empty array.
 *
 * Return CMCP_OK on success. Any non-zero return is treated as an
 * INTERNAL_ERROR (-32603). */
typedef int (*cmcp_prompt_handler_fn)(const cmcp_json_t *arguments,
                                       void *userdata,
                                       cmcp_handler_ctx_t *hctx,
                                       cmcp_json_t **out_messages);

/* Prompt descriptor (caller-owned, copied by add_prompt). */
typedef struct {
    const char             *name;         /* required, unique per server */
    const char             *title;        /* optional, human display name */
    const char             *description;  /* optional */
    /* JSON array text describing arguments, each: {name, description?,
     * required?}. May be NULL. Server validates required fields are
     * present at prompts/get time; full schema validation is the
     * handler's responsibility. */
    const char             *arguments;
    cmcp_prompt_handler_fn  handler;      /* required */
    void                   *userdata;
} cmcp_prompt_t;

int cmcp_server_add_prompt(cmcp_server_t *s, const cmcp_prompt_t *p);

/* Convenience: build a single-element messages array containing one
 * text-content message. role is "user" or "assistant". */
cmcp_json_t *cmcp_prompt_text_messages(const char *role, const char *text);

/* ====================================================================== */
/* Run loop                                                                */
/* ====================================================================== */

/* Drive the server on a transport until the transport closes.
 *
 * The server reads frames in a loop, parses them as JSON-RPC, and
 * dispatches them. The handshake (`initialize` request, `initialized`
 * notification), `tools/list` and `tools/call` are built in. Other
 * request methods get -32601.
 *
 * Does NOT take ownership of the transport — caller closes it after
 * cmcp_server_run() returns. Returns CMCP_OK on clean shutdown
 * (transport closed), or a negative cmcp_err_t on misuse. */
int cmcp_server_run(cmcp_server_t *s, cmcp_transport_t *t);

/* Sugar: open a stdio transport over the calling process's stdin/stdout,
 * run the server, then close the transport. Equivalent of:
 *
 *   cmcp_transport_t *t = cmcp_transport_stdio_new();
 *   cmcp_server_run(s, t);
 *   cmcp_transport_close(t);
 *
 * Returns CMCP_ENOMEM if the transport can't be allocated; otherwise
 * propagates the cmcp_server_run() return. */
int cmcp_server_run_stdio(cmcp_server_t *s);

/* ====================================================================== */
/* Negotiated peer state                                                   */
/* ====================================================================== */

/* Negotiated client capabilities and identity, valid after the
 * handshake completes. Pointers returned are owned by the server. */
const cmcp_client_capabilities_t *cmcp_server_client_caps(const cmcp_server_t *s);
const char *cmcp_server_client_name(const cmcp_server_t *s);
const char *cmcp_server_client_version(const cmcp_server_t *s);

/* ====================================================================== */
/* Server-initiated notifications                                          */
/* ====================================================================== */

/* Emit a JSON-RPC notification on the active transport.
 *
 * Valid only between cmcp_server_run() entry and exit; calling before
 * or after returns CMCP_EINVAL. May be called from a tool/resource/
 * prompt handler (runs on the run-loop thread) OR from a background
 * thread; both paths are serialised by the transport's writer mutex.
 *
 * For HTTP transports, notifications are routed to held-open SSE
 * connections; if no client has opened the SSE channel, the
 * notification is dropped on the floor (the spec allows this — a
 * client that wants to receive notifications is expected to open
 * `GET /mcp` first).
 *
 * `params` is consumed (may be NULL). */
int cmcp_server_notify(cmcp_server_t *s,
                        const char *method,
                        cmcp_json_t *params);

/* Capability-gated convenience wrappers for the standard MCP
 * list-changed and updated notifications.
 *
 * Each wrapper checks the corresponding capability flag was opted in
 * via cmcp_server_set_capabilities(); if not, returns CMCP_EPROTOCOL
 * (you said you wouldn't emit this — peers may not even be listening).
 * Otherwise emits the spec-defined method with the spec-defined
 * (empty or single-key) params shape. */
int cmcp_server_notify_tools_changed(cmcp_server_t *s);
int cmcp_server_notify_resources_changed(cmcp_server_t *s);
int cmcp_server_notify_prompts_changed(cmcp_server_t *s);

/* Emit `notifications/resources/updated` for the given URI. Requires
 * `caps.resources_subscribe = 1` (CMCP_EPROTOCOL otherwise). Silently
 * no-ops (returns CMCP_OK) if no peer has subscribed to this URI —
 * keeps the wire quiet for resources nobody cares about. */
int cmcp_server_notify_resource_updated(cmcp_server_t *s, const char *uri);

/* ====================================================================== */
/* Server → client requests                                                */
/* ====================================================================== */

/* Send a JSON-RPC request from the server to the client and block the
 * calling thread until the response arrives.
 *
 * Intended to be called from a tool/resource/prompt handler running on
 * the worker pool — the run-loop thread reads the response off the
 * transport and routes it to this caller's completion record. Calling
 * from the run-loop thread itself would deadlock (the loop would never
 * get back to reading frames) so don't do that.
 *
 * Valid only between cmcp_server_run() entry and exit. `params` is
 * consumed (may be NULL).
 *
 * On success, *out_response is initialised and owns its fields —
 * caller must cmcp_rpc_message_clear() it. Inspect ->result vs
 * ->error to distinguish a successful peer response from a peer-side
 * JSON-RPC error.
 *
 * If `hctx` is non-NULL, this call polls its cancellation flag on a
 * ~50ms tick and returns CMCP_ECANCELLED if the handler has been
 * cancelled — so a server-initiated request inside a cancellable tool
 * call doesn't strand the worker on a peer that's no longer answering.
 *
 * Returns:
 *   CMCP_OK         response received (success or peer error)
 *   CMCP_EINVAL     bad args, or no active transport
 *   CMCP_ENOMEM     allocation failure
 *   CMCP_EIO        transport closed before the response arrived
 *   CMCP_ECANCELLED hctx was cancelled while waiting */
int cmcp_server_send_request(cmcp_server_t *s,
                              cmcp_handler_ctx_t *hctx,
                              const char *method,
                              cmcp_json_t *params,
                              cmcp_rpc_message_t *out_response);

/* Convenience: ask the peer (the host) for structured input via the
 * `elicitation/create` request.
 *
 *   `message`           Human-readable prompt (required).
 *   `requested_schema`  A flat JSON object describing the shape of the
 *                       answer per the MCP spec (string/number/boolean/
 *                       enum properties only — no nesting). Consumed on
 *                       success; freed on failure. May be NULL — the
 *                       library then sends an empty `{"type":"object"}`.
 *   `out_result`        OUT. Owned cmcp_json_t object shaped
 *                       {"action": "accept"|"decline"|"cancel",
 *                        "content"?: ...}. Caller frees via
 *                       cmcp_json_free.
 *
 * Cap-gated: returns CMCP_EUNSUPPORTED if the peer didn't advertise
 * `elicitation` in its initialize capabilities.
 *
 * On CMCP_OK, `*out_result` is set. On any other return, `*out_result`
 * is NULL. Forwards the underlying CMCP_ECANCELLED / CMCP_EIO /
 * CMCP_EINVAL semantics from cmcp_server_send_request. A peer-side
 * JSON-RPC error response is surfaced as CMCP_EPROTOCOL. */
int cmcp_handler_elicit(cmcp_handler_ctx_t *hctx,
                         const char *message,
                         cmcp_json_t *requested_schema,
                         cmcp_json_t **out_result);

#endif
