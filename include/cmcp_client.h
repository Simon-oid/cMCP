/**
 * @file cmcp_client.h
 * @brief Asynchronous MCP client: handshake, list/call/read/subscribe,
 *        cancellation, progress, sampling, roots, elicitation.
 *
 * The client owns a transport and a reader thread that demultiplexes
 * responses by id against an in-flight table. Every call is async at
 * the wire level — `cmcp_client_call_async` returns immediately with
 * a handle, and `cmcp_client_wait` blocks for completion; many calls
 * can be in flight concurrently and they complete in any order.
 * `cmcp_client_request` is sync sugar over the async pair.
 *
 * Client-side handlers (notification routing, sampling, elicitation)
 * run on the reader thread; they must not call back into the same
 * client. `cmcp_client_set_roots` advertises filesystem scope to the
 * server; `cmcp_client_cancel` issues a `notifications/cancelled` for
 * an in-flight call.
 *
 * For multi-server agents see `cmcp_session.h`, which aggregates
 * several already-handshaken clients under a single primitive
 * surface.
 */
#ifndef CMCP_CLIENT_H
#define CMCP_CLIENT_H

#include "cmcp_types.h"
#include "cmcp_transport.h"

typedef struct cmcp_client cmcp_client_t;

/* Single-client typed helpers (below) re-use the session-layer record
 * shapes from cmcp_session.h, so a host pulls in one struct definition
 * and one set of *_free destructors regardless of whether it talks to
 * one server or N. The include sits after the cmcp_client_t typedef so
 * cmcp_session.h's references to cmcp_client_t resolve cleanly when a
 * consumer #includes cmcp_client.h before (or instead of) cmcp_session.h. */
#include "cmcp_session.h"

/* ====================================================================== */
/* Lifecycle                                                               */
/* ====================================================================== */

cmcp_client_t *cmcp_client_new(const char *name, const char *version);

/* Tears down the client. If the client owns a spawned child (via
 * cmcp_client_connect_stdio) the child is asked to exit (transport
 * close → SIGTERM fallback) and reaped before this returns. Cancels
 * any in-flight calls with CMCP_ECANCELLED. */
void cmcp_client_free(cmcp_client_t *c);

void cmcp_client_set_capabilities(cmcp_client_t *c,
                                   const cmcp_client_capabilities_t *caps);

/* Optional human-readable description, echoed in the handshake's
 * `clientInfo.description` (MCP 2025-11-25 Minor 2). Pass NULL to
 * clear. Returns CMCP_OK or CMCP_ENOMEM. */
int cmcp_client_set_description(cmcp_client_t *c, const char *description);

/* ====================================================================== */
/* Notification routing                                                    */
/* ====================================================================== */

/* Invoked on the client's reader thread for each server-to-client
 * notification. The handler MUST NOT call back into the same client
 * (would deadlock). params may be NULL. */
typedef void (*cmcp_notification_fn)(const char *method,
                                      const cmcp_json_t *params,
                                      void *userdata);

void cmcp_client_set_notification_handler(cmcp_client_t *c,
                                           cmcp_notification_fn fn,
                                           void *userdata);

/* ====================================================================== */
/* Handshake & transport                                                   */
/* ====================================================================== */

/* Drive the initialize → notifications/initialized handshake on a
 * caller-provided transport. The client borrows the transport for its
 * lifetime; caller still closes it. Starts the reader thread before
 * sending the initialize request. Returns CMCP_OK; CMCP_EPROTOCOL on
 * spec violation; CMCP_EIO on transport failure. */
int cmcp_client_handshake(cmcp_client_t *c, cmcp_transport_t *t);

/* Spawn a child process (path + argv, optional envp) connected via a
 * stdio pipe pair, run the handshake, and start the reader thread.
 * The client takes ownership of both the child process and the
 * transport — cmcp_client_free closes them.
 *
 * argv[0] should typically equal `path`; argv must end with NULL.
 * envp may be NULL to inherit the parent environment.
 *
 * Returns CMCP_OK; CMCP_EIO if fork/exec/handshake failed. */
int cmcp_client_connect_stdio(cmcp_client_t *c,
                               const char *path,
                               char *const argv[],
                               char *const envp[]);

/* ====================================================================== */
/* Requests                                                                */
/* ====================================================================== */

/* Async request. Sends method+params and returns immediately. *out_id
 * receives the in-flight request ID; pass it to cmcp_client_wait.
 * Every async call must be matched by exactly one wait (or the
 * client must be freed, which cancels all in-flight calls).
 *
 * params is consumed (ownership transferred). */
int cmcp_client_call_async(cmcp_client_t *c,
                            const char *method,
                            cmcp_json_t *params,
                            long long *out_id);

/* Block the calling thread until the response for id arrives, or
 * until the client is shut down. On success *out_response is
 * initialised and owns its fields — caller must
 * cmcp_rpc_message_clear() it.
 *
 * Returns CMCP_OK, CMCP_ECANCELLED on shutdown, or CMCP_ENOTFOUND if
 * the id is not in the pending table (already consumed by callback or
 * never registered). */
int cmcp_client_wait(cmcp_client_t *c, long long id,
                      cmcp_rpc_message_t *out_response);

/* Synchronous convenience: call_async + wait. params is consumed. */
int cmcp_client_request(cmcp_client_t *c, const char *method,
                         cmcp_json_t *params,
                         cmcp_rpc_message_t *out_response);

/* Fire-and-forget notification. params is consumed. */
int cmcp_client_notify(cmcp_client_t *c, const char *method,
                        cmcp_json_t *params);

/* Cancel an in-flight call. Emits `notifications/cancelled`
 * `{requestId, reason?}` on the wire AND unblocks any thread parked in
 * cmcp_client_wait(id) — that wait then returns CMCP_ECANCELLED. The
 * pending entry is dropped, so a server response arriving after the
 * cancel is silently discarded.
 *
 * `reason` may be NULL (the field is then omitted from the wire). The
 * server MAY honour the cancel cooperatively; a slow handler that
 * doesn't poll cmcp_handler_cancelled will still run to completion,
 * but its response is dropped per spec.
 *
 * Returns:
 *   CMCP_OK         cancel signalled + wire frame sent (or attempted)
 *   CMCP_EINVAL     id is unknown or already completed
 *   CMCP_ENOMEM     allocation failure */
int cmcp_client_cancel(cmcp_client_t *c, long long id, const char *reason);

/* Per-call progress callback. Invoked on the client's reader thread for
 * each `notifications/progress` frame whose `_meta.progressToken`
 * matches the token attached by cmcp_client_call_async_progress.
 *
 *   progress    The amount of work done so far.
 *   total       Expected total, or negative if the server didn't send
 *               one (the spec marks `total` optional).
 *   message     Optional human-readable status, NULL if absent.
 *   userdata    The userdata pointer registered with the call.
 *
 * The handler MUST NOT call back into the same client (would deadlock)
 * and should not block — slow handlers stall inbound frames. */
typedef void (*cmcp_progress_fn)(double progress, double total,
                                  const char *message, void *userdata);

/* Async request with a per-call progress callback. The library generates
 * a monotonically-unique integer progress token, attaches it to
 * `_meta.progressToken` in `params` (replacing any caller-supplied
 * value at that path), and routes matching `notifications/progress`
 * frames to `fn` for the lifetime of the call.
 *
 * Otherwise identical to cmcp_client_call_async — pair with
 * cmcp_client_wait. When the call completes (wait returns), the
 * subscription is torn down with the completion record — no late
 * progress notification will fire `fn` after wait returns.
 *
 * `params` is consumed (NULL is upgraded to an empty object so the
 * library has somewhere to attach `_meta`). `fn` must not be NULL.
 * Progress notifications carrying tokens that don't match any
 * call-attached subscription still reach the generic notification
 * handler (set via cmcp_client_set_notification_handler). */
int cmcp_client_call_async_progress(cmcp_client_t *c,
                                     const char *method,
                                     cmcp_json_t *params,
                                     cmcp_progress_fn fn,
                                     void *userdata,
                                     long long *out_id);

/* ====================================================================== */
/* Single-client typed helpers                                              */
/* ====================================================================== */

/* These helpers exist so a host talking to ONE server doesn't have to
 * drop to raw cmcp_json_object_get walking, and doesn't have to wrap
 * the single client in a cmcp_session_t just to get list-iteration and
 * pagination handling for free. They mirror the cmcp_session_t shapes:
 * the record types and *_free destructors live in cmcp_session.h.
 *
 * For records produced by these helpers, the `server` and `qualified`
 * fields (where present on the session-layer struct) are populated
 * with NULL — there's only one client, so there's no namespacing to
 * carry. Pair every list helper with the matching cmcp_session_*_free
 * (which is NULL-safe on those fields). */

/* Send `tools/list` and accumulate every page into a flat array. On
 * CMCP_OK *out_tools is a malloc'd array of length *out_n (free with
 * cmcp_session_tools_free) and the .server / .qualified fields on
 * each entry are NULL.
 *
 * Returns CMCP_OK on success (including the empty-list case); the
 * standard transport errors from cmcp_client_request on a wire failure;
 * CMCP_EPROTOCOL if the server returned a JSON-RPC error for the first
 * page. */
int cmcp_client_tools_list(cmcp_client_t *c,
                            cmcp_session_tool_t **out_tools,
                            size_t *out_n);

/* Send `resources/list` and accumulate every page. Same ownership and
 * error model as cmcp_client_tools_list. Free with
 * cmcp_session_resources_free. */
int cmcp_client_resources_list(cmcp_client_t *c,
                                cmcp_session_resource_t **out_resources,
                                size_t *out_n);

/* Send `prompts/list` and accumulate every page. Same ownership and
 * error model as cmcp_client_tools_list. Free with
 * cmcp_session_prompts_free. */
int cmcp_client_prompts_list(cmcp_client_t *c,
                              cmcp_session_prompt_t **out_prompts,
                              size_t *out_n);

/* Send `resources/read` for `uri` and surface the first content item's
 * text body. On CMCP_OK *out_text is a malloc'd NUL-terminated string
 * (caller frees with free()) and *out_n is its byte length (excluding
 * the NUL).
 *
 * Decision on binary `blob` content (per v0.6.0 acceptance doc rule 1):
 * we keep the helper text-only. If the server returns a `blob` content
 * item the helper returns CMCP_EUNSUPPORTED — a host that needs binary
 * resources drops to cmcp_client_request("resources/read", ...) and
 * walks the result tree itself.
 *
 * Returns CMCP_OK; CMCP_EINVAL on bad arguments; CMCP_EPROTOCOL if the
 * server returned a JSON-RPC error or a structurally invalid result;
 * CMCP_ENOTFOUND if `result.contents` is an empty array;
 * CMCP_EUNSUPPORTED if the first content item is a `blob`; the
 * standard transport errors on a wire failure. */
int cmcp_client_resource_read(cmcp_client_t *c, const char *uri,
                               char **out_text, size_t *out_n);

/* Send `prompts/get` for `name` with optional `args` (consumed; may be
 * NULL) and surface the messages array. On CMCP_OK *out_messages is an
 * owned cmcp_json_t whose `type` is CMCP_JSON_ARRAY — caller frees
 * with cmcp_json_free.
 *
 * Returns CMCP_OK; CMCP_EINVAL on bad arguments; CMCP_EPROTOCOL if the
 * server returned a JSON-RPC error or a structurally invalid result;
 * the standard transport errors on a wire failure. */
int cmcp_client_prompt_get(cmcp_client_t *c, const char *name,
                            cmcp_json_t *args,
                            cmcp_json_t **out_messages);

/* Outcome of a cmcp_client_tool_call. The MCP `tools/call` method can
 * fail in two distinct ways the host has to reason about separately:
 *
 *   - a JSON-RPC error on the channel (peer rejected the call,
 *     transport failed mid-flight, schema rejection surfaced as
 *     -32602, unknown tool surfaced as -32602 with structured data),
 *   - a tool-level error (handler succeeded at the channel level but
 *     reported a failure in result.isError + result.content[]).
 *
 * Hosts that try to flatten this into a single resp.error check miss
 * the tool-level case; hosts that only check result.isError miss the
 * protocol case. The 3-way outcome puts both into a single switch. */
typedef enum {
    CMCP_TOOL_OK             = 0, /* success, *out_result is owned by caller */
    CMCP_TOOL_ERR_TOOL_LEVEL = 1, /* tool said no — *out_text is owned by caller */
    CMCP_TOOL_ERR_PROTOCOL   = 2, /* channel said no — *out_rpc_err is owned by caller */
} cmcp_tool_outcome_t;

/* Call a tool, flatten the two-channel error model into one outcome.
 *
 * `args` is consumed (may be NULL — the helper sends `{}` in that case
 * to keep the wire well-formed). On return EXACTLY ONE of *out_result
 * / *out_text / *out_rpc_err is populated, matching the outcome enum.
 *
 *   CMCP_TOOL_OK              *out_result   = result object (owned;
 *                                              free with cmcp_json_free).
 *                                              Includes content[],
 *                                              structuredContent (if
 *                                              the tool produced one),
 *                                              and isError == false.
 *
 *   CMCP_TOOL_ERR_TOOL_LEVEL  *out_text     = first content[].text as a
 *                                              malloc'd NUL-terminated
 *                                              string (owned; free with
 *                                              free()). Empty string if
 *                                              the server set isError
 *                                              without any text content.
 *
 *   CMCP_TOOL_ERR_PROTOCOL    *out_rpc_err  = owned cmcp_rpc_error_t
 *                                              (free with
 *                                              cmcp_rpc_error_free).
 *                                              Carries the wire error
 *                                              (e.g. -32602 schema
 *                                              rejection with
 *                                              structured data, -32601
 *                                              method not found,
 *                                              -32603 internal error),
 *                                              OR a synthesized error
 *                                              when the failure was
 *                                              transport-side (the
 *                                              call never reached the
 *                                              peer or the response
 *                                              never came back).
 *
 * Any of the three out_* may be NULL — passing NULL just discards that
 * branch of the result. Whichever pointer would have received the
 * payload is silently freed.
 *
 * A NULL `c` or NULL `name` is reported as CMCP_TOOL_ERR_PROTOCOL with
 * a synthesized -32602 error so the caller's `switch` still handles
 * it without falling through to a default arm. */
cmcp_tool_outcome_t cmcp_client_tool_call(cmcp_client_t *c,
                                            const char *name,
                                            cmcp_json_t *args,
                                            cmcp_json_t **out_result,
                                            char **out_text,
                                            cmcp_rpc_error_t **out_rpc_err);

/* Typed async pair, A4 (v0.7 candidate surfaced by the v0.6.0 dogfood
 * rewrite). cmcp_client_tool_call is the sync sugar; this pair lets
 * the host fan out N concurrent tool calls and reap them in any order
 * without dropping back to the raw cmcp_client_call_async +
 * cmcp_client_wait surface (which would re-expose the two-channel
 * error model A2's outcome enum eliminated).
 *
 * Wire shape, ownership, and error semantics are identical to
 * cmcp_client_tool_call — the split is purely about scheduling.
 *
 * cmcp_client_tool_call_async: builds {name, arguments} (NULL args
 * becomes {}), dispatches via the async core, stores the in-flight
 * id in `*out_id`. `args` is consumed in every code path (including
 * caller-misuse: NULL c / NULL name / NULL out_id). Returns CMCP_OK
 * on success, or one of CMCP_EINVAL / CMCP_ENOMEM / CMCP_EAGAIN
 * (in-flight cap) / transport error from the writer.
 *
 * cmcp_client_tool_wait: blocks until the response arrives, then
 * processes it through the same three branches as
 * cmcp_client_tool_call. Always returns one of the three
 * cmcp_tool_outcome_t values. Failures that cannot arrive on the
 * wire (caller passed an unknown id; the call was cancelled
 * mid-flight; the response was malformed) surface as
 * CMCP_TOOL_ERR_PROTOCOL with a synthesized cmcp_rpc_error_t so the
 * caller's switch has no default arm. */
int cmcp_client_tool_call_async(cmcp_client_t *c,
                                  const char *name,
                                  cmcp_json_t *args,
                                  long long *out_id);

cmcp_tool_outcome_t cmcp_client_tool_wait(cmcp_client_t *c,
                                            long long id,
                                            cmcp_json_t **out_result,
                                            char **out_text,
                                            cmcp_rpc_error_t **out_rpc_err);

/* Content-shortcut helper, A5 (v0.7 candidate surfaced by the v0.6.0
 * dogfood rewrite). Many host call sites want one thing from a
 * `tools/call`: the LLM-facing text. Both the success branch
 * (result.content[0].text) and the tool-error branch
 * (isError:true + result.content[0].text) produce that text — but
 * cmcp_client_tool_call hands the success branch back as a raw
 * cmcp_json_t the host then has to walk. A5 flattens both:
 *
 *   On CMCP_OK:
 *      *out_text is the first content[].text from the result, as
 *      an owned malloc-d C string. Empty string if the result had
 *      no content items or the first item had no text. *out_rpc_err
 *      stays NULL. The success-vs-tool-error distinction is
 *      DELIBERATELY collapsed; if you need it, use
 *      cmcp_client_tool_call.
 *
 *   On negative return (CMCP_EPROTOCOL):
 *      *out_rpc_err is an owned cmcp_rpc_error_t (free with
 *      cmcp_rpc_error_free). *out_text stays NULL.
 *
 * `args` is consumed in every code path. NULL out_text or NULL
 * out_rpc_err is silently freed. NULL c / NULL name / NULL out_text
 * is reported as CMCP_EPROTOCOL with a synthesized -32602. */
int cmcp_client_tool_call_text(cmcp_client_t *c,
                                const char *name,
                                cmcp_json_t *args,
                                char **out_text,
                                cmcp_rpc_error_t **out_rpc_err);

/* ====================================================================== */
/* Sampling (server → host LLM call)                                        */
/* ====================================================================== */

/* Handler for `sampling/createMessage`. Servers invoke this when they
 * want the host to run a completion through its configured LLM (e.g.
 * a tool that wants the model to summarise its raw output before
 * surfacing it).
 *
 *   `params`     The full params object: messages array + optional
 *                modelPreferences, systemPrompt, includeContext,
 *                temperature, maxTokens, stopSequences, metadata.
 *                Borrowed.
 *   `userdata`   What was passed to set_sampling_handler.
 *   `out_result` OUT. Owned cmcp_json_t object, e.g.
 *                {"role":"assistant","content":{...},"model":"...",
 *                 "stopReason":"endTurn"}.
 *                The library takes ownership on success.
 *
 * Return CMCP_OK on success, non-zero for INTERNAL_ERROR (-32603).
 *
 * Authorisation note: cMCP does NOT impose its own allow-list. The
 * host (openclawd) decides per-server whether to register a handler;
 * a server with no handler attached gets the default -32601 response.
 * That's the trust gate. Don't register a handler for a server you
 * don't trust to spend tokens. */
typedef int (*cmcp_sampling_handler_fn)(const cmcp_json_t *params,
                                          void *userdata,
                                          cmcp_json_t **out_result);

/* Register a sampling handler. Pass fn=NULL to clear. Setting the
 * handler does NOT automatically advertise the `sampling` capability
 * — call cmcp_client_set_capabilities to opt in to the wire signal
 * (and do so BEFORE cmcp_client_handshake; the cap travels in the
 * initialize request and isn't re-negotiated). */
void cmcp_client_set_sampling_handler(cmcp_client_t *c,
                                       cmcp_sampling_handler_fn fn,
                                       void *userdata);

/* Convenience: build a sampling result with a single-text-content
 * assistant message. stop_reason is one of "endTurn", "stopSequence",
 * "maxTokens" per spec. Returns NULL on allocation failure. */
cmcp_json_t *cmcp_sampling_text_result(const char *text,
                                        const char *model,
                                        const char *stop_reason);

/* ====================================================================== */
/* Elicitation (server → host structured-input prompt)                      */
/* ====================================================================== */

/* Handler for `elicitation/create`. Servers invoke this mid-tool-call
 * when they need additional structured input from the user — e.g. a
 * confirmation, a missing argument, a credential.
 *
 *   `params`     `{message: string, requestedSchema: object}`.
 *                `requestedSchema` is a restricted flat object of
 *                primitive properties per the MCP spec (string / number
 *                / boolean / enum — no nesting). Borrowed.
 *   `userdata`   What was passed to set_elicitation_handler.
 *   `out_result` OUT. Owned cmcp_json_t object built via
 *                cmcp_elicitation_result(action, content). The library
 *                takes ownership on success.
 *
 * Return CMCP_OK on success, non-zero for INTERNAL_ERROR (-32603).
 *
 * Trust note: same per-client model as sampling — register a handler
 * only on clients whose servers are allowed to interrupt the user. */
typedef int (*cmcp_elicitation_handler_fn)(const cmcp_json_t *params,
                                             void *userdata,
                                             cmcp_json_t **out_result);

/* Register an elicitation handler. Pass fn=NULL to clear. Setting the
 * handler does NOT automatically advertise the `elicitation` capability
 * — call cmcp_client_set_capabilities to opt in to the wire signal
 * (and do so BEFORE cmcp_client_handshake). */
void cmcp_client_set_elicitation_handler(cmcp_client_t *c,
                                           cmcp_elicitation_handler_fn fn,
                                           void *userdata);

/* Convenience: build an elicitation response envelope.
 *
 *   action   "accept", "decline", or "cancel" — required.
 *   content  Required for "accept" — owned cmcp_json_t OBJECT shaped per
 *            the request's `requestedSchema`. Ignored (freed) for
 *            "decline"/"cancel" since the spec omits content there.
 *
 * Returns NULL on bad input; on failure, any passed-in content is
 * freed so the caller never leaks. */
cmcp_json_t *cmcp_elicitation_result(const char *action,
                                       cmcp_json_t *content);

/* ====================================================================== */
/* Roots (host → server filesystem scoping)                                 */
/* ====================================================================== */

/* Roots tell servers which paths the host considers in-scope. A
 * filesystem-shaped server (or any server that operates on URIs) is
 * expected to read this list before doing anything that touches the
 * outside world. Roots are advisory — the server is the one that
 * enforces the boundary. cMCP just carries the list. */

typedef struct {
    const char *uri;        /* required, typically `file:///abs/path` */
    const char *name;       /* optional human-readable display name */
} cmcp_root_t;

/* Replace the current roots set with a deep-copy of the caller's
 * array. Pass roots=NULL,n=0 to clear. After this call, the client's
 * `roots` capability is advertised (presence of the key signals
 * support for `roots/list`); if you also set
 * `caps.roots_list_changed = 1` via cmcp_client_set_capabilities,
 * `listChanged` is added.
 *
 * Safe to call before OR after handshake. Calling after handshake
 * does NOT re-negotiate caps, but the new list takes effect
 * immediately for subsequent server `roots/list` requests; pair with
 * cmcp_client_notify_roots_changed if your server cares.
 *
 * Returns CMCP_OK / CMCP_EINVAL / CMCP_ENOMEM. */
int cmcp_client_set_roots(cmcp_client_t *c,
                           const cmcp_root_t *roots, size_t n);

/* Emit `notifications/roots/list_changed`. Requires
 * caps.roots_list_changed = 1 (CMCP_EPROTOCOL otherwise). Caller is
 * responsible for the order: typically you set the new roots first,
 * then call this so the server's re-list sees the new state. */
int cmcp_client_notify_roots_changed(cmcp_client_t *c);

/* ====================================================================== */
/* Logging                                                                 */
/* ====================================================================== */

/* Ask the server to raise/lower its `notifications/message` floor.
 * Sends a `logging/setLevel` request with `{level: "<name>"}` and
 * blocks until the response arrives. Levels below the floor will be
 * dropped server-side after this returns.
 *
 * Requires the server to have advertised the `logging` capability;
 * otherwise the peer answers -32601 and this returns CMCP_EPROTOCOL.
 *
 * Returns CMCP_OK on success, CMCP_EPROTOCOL on a peer-side error or
 * if the server doesn't speak logging, or the standard transport
 * errors from cmcp_client_request. */
int cmcp_client_set_log_level(cmcp_client_t *c, cmcp_log_level_t level);

/* ====================================================================== */
/* Server identity (post-handshake)                                        */
/* ====================================================================== */

const cmcp_server_capabilities_t *cmcp_client_server_caps(const cmcp_client_t *c);
const char *cmcp_client_server_name(const cmcp_client_t *c);
const char *cmcp_client_server_version(const cmcp_client_t *c);
/* Server-advertised description (Minor 2 / MCP 2025-11-25). NULL if
 * the server didn't send one or the handshake hasn't completed. */
const char *cmcp_client_server_description(const cmcp_client_t *c);

/* The protocol version the server advertised at handshake. Per the MCP
 * spec this may differ from CMCP_PROTOCOL_VERSION — negotiation is
 * non-fatal, the handshake still succeeds. A host that requires an
 * exact match should inspect this and disconnect on mismatch. Returns
 * NULL before the handshake completes. */
const char *cmcp_client_server_protocol(const cmcp_client_t *c);

#endif
