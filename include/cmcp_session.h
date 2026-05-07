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
