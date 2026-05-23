#ifndef CMCP_CLIENT_H
#define CMCP_CLIENT_H

#include "cmcp_types.h"
#include "cmcp_transport.h"

typedef struct cmcp_client cmcp_client_t;

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
/* Server identity (post-handshake)                                        */
/* ====================================================================== */

const cmcp_server_capabilities_t *cmcp_client_server_caps(const cmcp_client_t *c);
const char *cmcp_client_server_name(const cmcp_client_t *c);
const char *cmcp_client_server_version(const cmcp_client_t *c);

/* The protocol version the server advertised at handshake. Per the MCP
 * spec this may differ from CMCP_PROTOCOL_VERSION — negotiation is
 * non-fatal, the handshake still succeeds. A host that requires an
 * exact match should inspect this and disconnect on mismatch. Returns
 * NULL before the handshake completes. */
const char *cmcp_client_server_protocol(const cmcp_client_t *c);

#endif
