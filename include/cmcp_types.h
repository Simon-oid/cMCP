/**
 * @file cmcp_types.h
 * @brief JSON-RPC 2.0 message shapes, capability structs, dispatch
 *        types. Shared between server and client.
 *
 * The "schema" of cMCP at the C-struct level: how a JSON-RPC
 * request/response/notification looks in memory, what the server-
 * and client-side capability objects carry, the JSON-RPC error-code
 * constants. Most callers reach these through `cmcp_server.h` /
 * `cmcp_client.h` rather than allocating these types directly.
 */
#ifndef CMCP_TYPES_H
#define CMCP_TYPES_H

#include <stddef.h>
#include "cmcp_json.h"

/* ------------------------------------------------------------------ */
/* MCP capability declarations                                          */
/* ------------------------------------------------------------------ */
/* These are the subset of MCP capabilities cMCP currently models. Each
 * field is a boolean: 1 if the peer offers this capability, 0 if not.
 * Negotiated at initialize-time and never re-negotiated within a
 * session. Phase 1.4 wires the structs and the handshake; later phases
 * fill in the behavior gated by each flag. */

typedef struct {
    int tools_list_changed;       /* tools/list_changed notifications */
    int resources_subscribe;      /* resources/subscribe + updated */
    int resources_list_changed;   /* resources/list_changed */
    int prompts_list_changed;     /* prompts/list_changed */
    int logging;                  /* logging methods */
} cmcp_server_capabilities_t;

typedef struct {
    int sampling;                 /* sampling/createMessage */
    int sampling_tools;           /* sampling.tools sub-cap (MCP 2025-11-25
                                   * SEP-1577): host's model can invoke tools
                                   * the server hands it during sampling */
    int elicitation;              /* elicitation/create */
    int elicitation_form;         /* elicitation.form sub-cap (MCP 2025-11-25
                                   * SEP-1036): host accepts schema-driven
                                   * form elicitations (the legacy default) */
    int elicitation_url;          /* elicitation.url sub-cap (SEP-1036): host
                                   * accepts URL-redirect elicitations */
    int roots_list_changed;       /* roots/list_changed */
} cmcp_client_capabilities_t;

/* ------------------------------------------------------------------ */
/* MCP log severity levels (RFC 5424 syslog, per spec)                 */
/* ------------------------------------------------------------------ */
/* Numerically ordered: a message with level >= the server-side floor
 * is emitted; below it is dropped. `debug` is the most verbose,
 * `emergency` the most severe. Wire names are the lowercase strings
 * below; cmcp_log_level_from_name / _to_name convert. */
typedef enum {
    CMCP_LOG_LEVEL_DEBUG     = 0,
    CMCP_LOG_LEVEL_INFO      = 1,
    CMCP_LOG_LEVEL_NOTICE    = 2,
    CMCP_LOG_LEVEL_WARNING   = 3,
    CMCP_LOG_LEVEL_ERROR     = 4,
    CMCP_LOG_LEVEL_CRITICAL  = 5,
    CMCP_LOG_LEVEL_ALERT     = 6,
    CMCP_LOG_LEVEL_EMERGENCY = 7,
} cmcp_log_level_t;

/* Returns the wire string ("debug", "info", ...) for `lvl`, or NULL
 * if `lvl` is out of range. Pointer is to static storage. */
const char *cmcp_log_level_to_name(cmcp_log_level_t lvl);

/* Parses `name` into a level. Returns 0 on success and writes
 * *out_level; returns -1 on unknown/NULL name. */
int cmcp_log_level_from_name(const char *name, cmcp_log_level_t *out_level);

/* JSON-RPC 2.0 standard error codes. Parenthesised so they survive
 * use in macro-expansion contexts where the surrounding tokens could
 * otherwise re-bind the leading `-` (e.g. `x - CMCP_RPC_PARSE_ERROR`
 * would otherwise expand to `x - -32700`, which still parses but is
 * the kind of thing static analysers, rightly, flag). */
#define CMCP_RPC_PARSE_ERROR       (-32700)
#define CMCP_RPC_INVALID_REQUEST   (-32600)
#define CMCP_RPC_METHOD_NOT_FOUND  (-32601)
#define CMCP_RPC_INVALID_PARAMS    (-32602)
#define CMCP_RPC_INTERNAL_ERROR    (-32603)

/* JSON-RPC reserves -32000..-32099 for implementation-defined server
 * errors. cMCP and MCP application errors live in this range. */
#define CMCP_RPC_SERVER_ERROR_MIN  (-32099)
#define CMCP_RPC_SERVER_ERROR_MAX  (-32000)

/* ------------------------------------------------------------------ */
/* JSON-RPC ID                                                         */
/* ------------------------------------------------------------------ */

typedef enum {
    CMCP_ID_NONE,    /* no id field (notification) */
    CMCP_ID_NULL,    /* explicit JSON null (parse-error responses) */
    CMCP_ID_INT,
    CMCP_ID_STRING,
} cmcp_id_kind_t;

typedef struct {
    cmcp_id_kind_t kind;
    long long      i;        /* CMCP_ID_INT */
    char          *s;        /* CMCP_ID_STRING — owned */
    size_t         s_len;
} cmcp_id_t;

void cmcp_id_init_none(cmcp_id_t *id);
void cmcp_id_init_null(cmcp_id_t *id);
void cmcp_id_init_int(cmcp_id_t *id, long long i);
int  cmcp_id_init_string(cmcp_id_t *id, const char *s, size_t n);
int  cmcp_id_copy(cmcp_id_t *dst, const cmcp_id_t *src);
void cmcp_id_clear(cmcp_id_t *id);
int  cmcp_id_equal(const cmcp_id_t *a, const cmcp_id_t *b);

/* ------------------------------------------------------------------ */
/* JSON-RPC error object                                               */
/* ------------------------------------------------------------------ */

typedef struct {
    int          code;
    char        *message;     /* owned */
    cmcp_json_t *data;        /* owned, may be NULL */
} cmcp_rpc_error_t;

/* ------------------------------------------------------------------ */
/* JSON-RPC message (discriminated union)                              */
/* ------------------------------------------------------------------ */

typedef enum {
    CMCP_MSG_REQUEST,
    CMCP_MSG_RESPONSE,
    CMCP_MSG_NOTIFICATION,
} cmcp_msg_kind_t;

typedef struct {
    cmcp_msg_kind_t   kind;
    cmcp_id_t         id;       /* REQUEST, RESPONSE */
    char             *method;   /* REQUEST, NOTIFICATION — owned */
    cmcp_json_t      *params;   /* REQUEST, NOTIFICATION — owned, may be NULL */
    cmcp_json_t      *result;   /* RESPONSE success — owned, may be NULL */
    cmcp_rpc_error_t *error;    /* RESPONSE error — owned, may be NULL */
} cmcp_rpc_message_t;

void cmcp_rpc_message_init(cmcp_rpc_message_t *m);
void cmcp_rpc_message_clear(cmcp_rpc_message_t *m);

/* ------------------------------------------------------------------ */
/* Encode / decode                                                     */
/* ------------------------------------------------------------------ */

/* Parse JSON-RPC text into one or more messages.
 *
 * On success, returns CMCP_OK; *out_msgs is a malloc'd array of length
 * *out_count (1 for a single message, N for a JSON-RPC batch). Free
 * with cmcp_rpc_messages_free().
 *
 * NOTE: MCP (since 2025-06-18, still the case in 2025-11-25) removes
 * batch support at the protocol layer, but this framing layer parses
 * batches for completeness; higher layers should reject batches with
 * INVALID_REQUEST when applicable. */
int  cmcp_rpc_parse(const char *text, size_t len,
                    cmcp_rpc_message_t **out_msgs, size_t *out_count);
void cmcp_rpc_messages_free(cmcp_rpc_message_t *msgs, size_t count);

/* Convert one parsed JSON value into a message. Caller must clear *out
 * even on failure (init it first via cmcp_rpc_message_init). */
int  cmcp_rpc_from_json(const cmcp_json_t *json, cmcp_rpc_message_t *out);

/* Convert a message into a fresh cmcp_json_t tree. */
cmcp_json_t *cmcp_rpc_to_json(const cmcp_rpc_message_t *m);

/* Emit one message as a JSON string with stable key ordering
 * (caller frees with free()). */
char *cmcp_rpc_emit(const cmcp_rpc_message_t *m);
char *cmcp_rpc_emit_batch(const cmcp_rpc_message_t *msgs, size_t count);

/* ------------------------------------------------------------------ */
/* Construction helpers                                                */
/* ------------------------------------------------------------------ */

/* All make_* helpers take ownership of params/result/data on success. */
int cmcp_rpc_make_request(cmcp_rpc_message_t *m, long long id,
                          const char *method, cmcp_json_t *params);
int cmcp_rpc_make_request_str(cmcp_rpc_message_t *m,
                              const char *id_str, size_t id_len,
                              const char *method, cmcp_json_t *params);
int cmcp_rpc_make_notification(cmcp_rpc_message_t *m,
                               const char *method, cmcp_json_t *params);
int cmcp_rpc_make_response(cmcp_rpc_message_t *m, const cmcp_id_t *id,
                           cmcp_json_t *result);
int cmcp_rpc_make_error(cmcp_rpc_message_t *m, const cmcp_id_t *id,
                        int code, const char *message, cmcp_json_t *data);

/* ------------------------------------------------------------------ */
/* In-flight request table (client-side ID matching)                   */
/* ------------------------------------------------------------------ */

typedef struct cmcp_rpc_pending cmcp_rpc_pending_t;

cmcp_rpc_pending_t *cmcp_rpc_pending_new(void);
void                cmcp_rpc_pending_free(cmcp_rpc_pending_t *t);

/* Reserve a fresh monotonic positive integer ID and associate it with
 * userdata. Returns the new ID, or 0 on failure. */
long long cmcp_rpc_pending_register(cmcp_rpc_pending_t *t, void *userdata);

/* Look up an ID and remove it. Returns 1 and writes *out_userdata on
 * hit; returns 0 if not found. out_userdata may be NULL. */
int cmcp_rpc_pending_take(cmcp_rpc_pending_t *t, long long id,
                          void **out_userdata);

size_t cmcp_rpc_pending_count(cmcp_rpc_pending_t *t);

/* ------------------------------------------------------------------ */
/* Dispatch                                                            */
/* ------------------------------------------------------------------ */

/* Handler for an incoming request or notification.
 *
 * For REQUEST: out_response is pre-initialised as an empty response
 * mirroring the request's ID. Handler must set out_response->result
 * OR out_response->error before returning.
 *
 * For NOTIFICATION: out_response is NULL.
 *
 * Return CMCP_OK on success; non-zero on failure (treated as
 * INTERNAL_ERROR for requests, ignored for notifications). */
typedef int (*cmcp_rpc_handler_fn)(const cmcp_rpc_message_t *in,
                                    cmcp_rpc_message_t *out_response,
                                    void *userdata);

typedef struct {
    const char          *method;
    cmcp_rpc_handler_fn  handler;
    void                *userdata;
} cmcp_rpc_route_t;

/* Dispatch one message against the route table.
 *
 *   REQUEST: looks up route by method, invokes handler, populates
 *     *out_response with success or error. If method is unknown,
 *     populates out_response with a -32601 error. Returns CMCP_OK.
 *
 *   NOTIFICATION: looks up route and invokes handler (out_response is
 *     NULL). Returns CMCP_OK whether or not the method was found.
 *     out_response (if non-NULL) is left untouched.
 *
 *   RESPONSE: returns CMCP_EINVAL — match via pending table instead. */
int cmcp_rpc_dispatch(const cmcp_rpc_message_t *in,
                      const cmcp_rpc_route_t *routes, size_t n_routes,
                      cmcp_rpc_message_t *out_response);

#endif
