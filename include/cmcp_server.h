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
/* Tool registry                                                           */
/* ====================================================================== */

/* Handler for a `tools/call` invocation.
 *
 *   `arguments`      The parsed `params.arguments` object from the
 *                    request. May be NULL if the caller sent no args.
 *                    Borrowed — handler must NOT free.
 *   `userdata`       Whatever was passed at registration time.
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
                                     cmcp_json_t **out_content,
                                     int *out_is_error);

/* Tool descriptor (caller-owned, copied by add_tool). */
typedef struct {
    const char            *name;            /* required, unique per server */
    const char            *description;     /* optional, may be NULL */
    /* JSON Schema for the input as JSON text. May be NULL. Phase 1.6
     * will validate inbound `arguments` against this. */
    const char            *input_schema;
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

/* ====================================================================== */
/* Resource registry                                                       */
/* ====================================================================== */

/* Handler for a `resources/read` invocation.
 *
 *   `uri`           The URI the caller asked for. Always equals the
 *                   resource's registered URI (the dispatcher matches
 *                   before invoking). Borrowed.
 *   `userdata`      Whatever was passed at registration time.
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
                                      cmcp_json_t **out_contents,
                                      int *out_is_error);

/* Resource descriptor (caller-owned, copied by add_resource). */
typedef struct {
    const char            *uri;          /* required, unique per server */
    const char            *name;         /* required, human display name */
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
 *   `out_messages` OUT. Owned cmcp_json_t array of message objects:
 *                  [{"role":"user|assistant", "content":{...}}]. The
 *                  library takes ownership on success. NULL is treated
 *                  as an empty array.
 *
 * Return CMCP_OK on success. Any non-zero return is treated as an
 * INTERNAL_ERROR (-32603). */
typedef int (*cmcp_prompt_handler_fn)(const cmcp_json_t *arguments,
                                       void *userdata,
                                       cmcp_json_t **out_messages);

/* Prompt descriptor (caller-owned, copied by add_prompt). */
typedef struct {
    const char             *name;         /* required, unique per server */
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

#endif
