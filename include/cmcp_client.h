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
/* Server identity (post-handshake)                                        */
/* ====================================================================== */

const cmcp_server_capabilities_t *cmcp_client_server_caps(const cmcp_client_t *c);
const char *cmcp_client_server_name(const cmcp_client_t *c);
const char *cmcp_client_server_version(const cmcp_client_t *c);

#endif
