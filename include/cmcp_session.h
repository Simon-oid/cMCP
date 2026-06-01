/**
 * @file cmcp_session.h
 * @brief Multi-server primitive aggregator on top of N cMCP clients.
 *
 * A host that wants to talk to several MCP servers at once (the
 * common case for agent frameworks) creates one `cmcp_client_t` per
 * server, hands them all to a `cmcp_session_t`, and then uses the
 * session for all subsequent calls. The session fans out
 * `tools/list` / `resources/list` / `prompts/list` to every member
 * in parallel and merges the results; `tool_call`, `resource_read`,
 * and `prompt_get` route to the right backend either by qualified
 * name (`<server>:<tool>` for tools) or by an explicit `(server,
 * uri|name)` pair (for resources and prompts, since URIs already
 * contain colons).
 */
#ifndef CMCP_SESSION_H
#define CMCP_SESSION_H

#include "cmcp_client.h"
#include "cmcp_types.h"

/* cmcp_session_t aggregates many cmcp_client_t handles into a single
 * tool surface. Tool names are namespaced as "<server>:<tool>" so a
 * host (openclawd) can present a flat menu to its model while the
 * session routes calls to the right child server.
 *
 * The session takes ownership of clients added to it. Freeing the
 * session frees every client, which in turn closes its transport and
 * reaps any spawned child. */

typedef struct cmcp_session cmcp_session_t;

cmcp_session_t *cmcp_session_new(void);
void            cmcp_session_free(cmcp_session_t *s);

/* Add an already-handshaken client under the given server name. The
 * server name must be non-empty, must not contain ':', and must be
 * unique within the session. On success the session takes ownership
 * of c. On failure c is NOT freed and ownership stays with the
 * caller. */
int cmcp_session_add(cmcp_session_t *s,
                      const char *server_name,
                      cmcp_client_t *c);

/* ====================================================================== */
/* Aggregated tool listing                                                 */
/* ====================================================================== */

typedef struct {
    char        *server;        /* host-supplied server name */
    char        *name;          /* tool name as the server published it */
    char        *qualified;     /* "<server>:<name>" — pass to tool_call */
    char        *description;   /* may be NULL */
    cmcp_json_t *input_schema;  /* deep-copy of the tool's schema; may be NULL */
} cmcp_session_tool_t;

/* Collect tools/list from every client in registration order and
 * concatenate them. Returns CMCP_OK on full success or partial
 * success (tools from servers that responded; failed servers
 * skipped). On allocation failure returns CMCP_ENOMEM and sets
 * *out_tools = NULL, *out_n = 0. */
int cmcp_session_tools_list(cmcp_session_t *s,
                             cmcp_session_tool_t **out_tools,
                             size_t *out_n);

void cmcp_session_tools_free(cmcp_session_tool_t *tools, size_t n);

/* ====================================================================== */
/* Routed tool call                                                        */
/* ====================================================================== */

/* qualified is "<server>:<tool>". args is consumed (may be NULL).
 * On CMCP_OK, *out_response is initialised and owns its fields —
 * caller must cmcp_rpc_message_clear() it.
 * Returns CMCP_ENOTFOUND if no client matches the server prefix. */
int cmcp_session_tool_call(cmcp_session_t *s,
                            const char *qualified,
                            cmcp_json_t *args,
                            cmcp_rpc_message_t *out_response);

/* Async routed tool call (F3). The whole point of a multi-server session
 * is parallel tool calls, but cmcp_session_tool_call is synchronous —
 * a host wanting concurrency had to drop through cmcp_session_get() to
 * the raw clients and re-do the routing the session already knows (the
 * P6 host-probe's FRICTION 1). This pair keeps the routing in the
 * session and hands back the same cmcp_tool_handle_t the client-level
 * async call uses, so the result reaping is identical and the handle's
 * id→client binding still prevents cross-server mis-routing (F4).
 *
 * cmcp_session_tool_call_async: `qualified` is "<server>:<tool>"; args
 * is consumed (may be NULL). Resolves the server prefix, dispatches via
 * the routed client, and returns a handle bound to that client. On any
 * failure (bad session/qualified, unknown server, dispatch error) the
 * returned handle is invalid (cmcp_tool_handle_valid == 0) and args is
 * still consumed.
 *
 * cmcp_session_tool_wait: reap a handle from cmcp_session_tool_call_async
 * (or, equivalently, from cmcp_client_tool_call_async — the handle is
 * the same type). Thin forwarder to cmcp_client_tool_wait, provided for
 * call-site symmetry. Always free the result with
 * cmcp_tool_result_clear. */
cmcp_tool_handle_t cmcp_session_tool_call_async(cmcp_session_t *s,
                                                const char *qualified,
                                                cmcp_json_t *args);

cmcp_tool_result_t cmcp_session_tool_wait(cmcp_tool_handle_t h);

/* Look up a client by host-supplied server name. Returns NULL on miss.
 * Useful for sending notifications, custom requests, etc. */
cmcp_client_t *cmcp_session_get(cmcp_session_t *s, const char *server_name);

size_t cmcp_session_count(const cmcp_session_t *s);

/* ====================================================================== */
/* Aggregated resources                                                    */
/* ====================================================================== */

/* URIs may already contain colons (scheme separator) so we don't fold
 * server-name into them; resource_read takes an explicit (server, uri)
 * pair instead of a single qualified string. */

typedef struct {
    char *server;        /* host-supplied server name */
    char *uri;           /* resource URI as the server published it */
    char *name;          /* display name */
    char *description;   /* may be NULL */
    char *mime_type;     /* may be NULL */
} cmcp_session_resource_t;

int cmcp_session_resources_list(cmcp_session_t *s,
                                 cmcp_session_resource_t **out_resources,
                                 size_t *out_n);

void cmcp_session_resources_free(cmcp_session_resource_t *r, size_t n);

/* Read one resource. server names which client to route to; uri is sent
 * as params.uri verbatim. On CMCP_OK out_response owns its fields. */
int cmcp_session_resource_read(cmcp_session_t *s,
                                const char *server,
                                const char *uri,
                                cmcp_rpc_message_t *out_response);

/* ====================================================================== */
/* Aggregated prompts                                                      */
/* ====================================================================== */

typedef struct {
    char        *server;       /* host-supplied server name */
    char        *name;         /* prompt name as the server published it */
    char        *description;  /* may be NULL */
    cmcp_json_t *arguments;    /* deep-copy of the prompt's argument array; may be NULL */
} cmcp_session_prompt_t;

int cmcp_session_prompts_list(cmcp_session_t *s,
                               cmcp_session_prompt_t **out_prompts,
                               size_t *out_n);

void cmcp_session_prompts_free(cmcp_session_prompt_t *p, size_t n);

/* Get one prompt. args is consumed (may be NULL). */
int cmcp_session_prompt_get(cmcp_session_t *s,
                             const char *server,
                             const char *name,
                             cmcp_json_t *args,
                             cmcp_rpc_message_t *out_response);

#endif
