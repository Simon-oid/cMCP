#include "cmcp.h"
#include "cmcp_server.h"
#include "cmcp_json.h"
#include "cmcp_types.h"
#include "cmcp_transport.h"

#include <stdlib.h>
#include <string.h>

/* ====================================================================== */
/* Lifecycle states                                                        */
/* ====================================================================== */

typedef enum {
    SS_UNINIT = 0,        /* before initialize */
    SS_HANDSHAKE,         /* initialize sent its response, awaiting
                           * notifications/initialized */
    SS_READY,             /* handshake done; can process other methods */
    SS_CLOSED,            /* terminal */
} server_state_t;

struct cmcp_server {
    char                       *name;
    char                       *version;
    cmcp_server_capabilities_t  caps;

    server_state_t              state;

    /* Negotiated peer info, set on initialize. */
    char                        *peer_name;
    char                        *peer_version;
    cmcp_client_capabilities_t   peer_caps;
};

/* ====================================================================== */
/* Allocation                                                              */
/* ====================================================================== */

static char *xstrdup(const char *s) {
    if (!s) return NULL;
    size_t n = strlen(s);
    char *out = (char *)malloc(n + 1);
    if (!out) return NULL;
    memcpy(out, s, n + 1);
    return out;
}

cmcp_server_t *cmcp_server_new(const char *name, const char *version) {
    if (!name || !version) return NULL;
    cmcp_server_t *s = (cmcp_server_t *)calloc(1, sizeof *s);
    if (!s) return NULL;
    s->name    = xstrdup(name);
    s->version = xstrdup(version);
    if (!s->name || !s->version) {
        free(s->name); free(s->version); free(s);
        return NULL;
    }
    s->state = SS_UNINIT;
    return s;
}

void cmcp_server_free(cmcp_server_t *s) {
    if (!s) return;
    free(s->name);
    free(s->version);
    free(s->peer_name);
    free(s->peer_version);
    free(s);
}

void cmcp_server_set_capabilities(cmcp_server_t *s,
                                   const cmcp_server_capabilities_t *caps) {
    if (!s || !caps) return;
    s->caps = *caps;
}

const cmcp_client_capabilities_t *cmcp_server_client_caps(const cmcp_server_t *s) {
    return s ? &s->peer_caps : NULL;
}
const char *cmcp_server_client_name(const cmcp_server_t *s) {
    return s ? s->peer_name : NULL;
}
const char *cmcp_server_client_version(const cmcp_server_t *s) {
    return s ? s->peer_version : NULL;
}

/* ====================================================================== */
/* JSON helpers for capability ↔ object encoding                           */
/* ====================================================================== */

/* Build the server's capabilities sub-object. Empty inner objects mean
 * "the capability is offered, no extra options" per MCP convention. */
static cmcp_json_t *server_caps_to_json(const cmcp_server_capabilities_t *c) {
    cmcp_json_t *root = cmcp_json_new_object();
    if (!root) return NULL;

    if (c->tools_list_changed || c->resources_list_changed ||
        c->resources_subscribe || c->prompts_list_changed) {
        /* tools — declared if list_changed is set. The presence of
         * "tools" in the object signals tool support; subkeys advertise
         * optional sub-capabilities. */
        if (c->tools_list_changed) {
            cmcp_json_t *tools = cmcp_json_new_object();
            cmcp_json_object_set(tools, "listChanged", cmcp_json_new_bool(1));
            cmcp_json_object_set(root, "tools", tools);
        }
        if (c->resources_list_changed || c->resources_subscribe) {
            cmcp_json_t *res = cmcp_json_new_object();
            if (c->resources_subscribe)
                cmcp_json_object_set(res, "subscribe", cmcp_json_new_bool(1));
            if (c->resources_list_changed)
                cmcp_json_object_set(res, "listChanged", cmcp_json_new_bool(1));
            cmcp_json_object_set(root, "resources", res);
        }
        if (c->prompts_list_changed) {
            cmcp_json_t *pr = cmcp_json_new_object();
            cmcp_json_object_set(pr, "listChanged", cmcp_json_new_bool(1));
            cmcp_json_object_set(root, "prompts", pr);
        }
    }
    if (c->logging) cmcp_json_object_set(root, "logging", cmcp_json_new_object());
    return root;
}

static void client_caps_from_json(const cmcp_json_t *o,
                                   cmcp_client_capabilities_t *out) {
    memset(out, 0, sizeof *out);
    if (!o || o->type != CMCP_JSON_OBJECT) return;
    if (cmcp_json_object_get(o, "sampling")) out->sampling = 1;
    const cmcp_json_t *roots = cmcp_json_object_get(o, "roots");
    if (roots && roots->type == CMCP_JSON_OBJECT) {
        const cmcp_json_t *lc = cmcp_json_object_get(roots, "listChanged");
        if (lc && lc->type == CMCP_JSON_BOOL && lc->b) {
            out->roots_list_changed = 1;
        }
    }
}

/* ====================================================================== */
/* Handshake handlers                                                      */
/* ====================================================================== */

/* Build a structured -32602 invalid params error with extra detail. */
static int reply_invalid_params(cmcp_rpc_message_t *resp,
                                const cmcp_id_t *id, const char *msg,
                                cmcp_json_t *data) {
    return cmcp_rpc_make_error(resp, id, CMCP_RPC_INVALID_PARAMS, msg, data);
}

/* Handle `initialize`. On success, populates *resp with the
 * initialize result and sets server state to HANDSHAKE. */
static void handle_initialize(cmcp_server_t *s,
                              const cmcp_rpc_message_t *req,
                              cmcp_rpc_message_t *resp) {
    if (s->state != SS_UNINIT) {
        cmcp_rpc_make_error(resp, &req->id, CMCP_RPC_INVALID_REQUEST,
                            "Server is already initialised", NULL);
        return;
    }
    if (!req->params || req->params->type != CMCP_JSON_OBJECT) {
        reply_invalid_params(resp, &req->id, "initialize requires params object", NULL);
        return;
    }

    /* Pin protocol version. */
    const cmcp_json_t *pv = cmcp_json_object_get(req->params, "protocolVersion");
    if (!pv || pv->type != CMCP_JSON_STRING ||
        strcmp(pv->str.s, CMCP_PROTOCOL_VERSION) != 0) {
        cmcp_json_t *data = cmcp_json_new_object();
        cmcp_json_object_set(data, "supported",
                              cmcp_json_new_string(CMCP_PROTOCOL_VERSION));
        if (pv && pv->type == CMCP_JSON_STRING) {
            cmcp_json_object_set(data, "requested",
                                  cmcp_json_new_string(pv->str.s));
        }
        reply_invalid_params(resp, &req->id,
                              "Unsupported protocol version", data);
        return;
    }

    /* Capture client info + caps. */
    const cmcp_json_t *ci = cmcp_json_object_get(req->params, "clientInfo");
    if (ci && ci->type == CMCP_JSON_OBJECT) {
        const cmcp_json_t *n = cmcp_json_object_get(ci, "name");
        const cmcp_json_t *v = cmcp_json_object_get(ci, "version");
        if (n && n->type == CMCP_JSON_STRING) {
            free(s->peer_name);
            s->peer_name = xstrdup(n->str.s);
        }
        if (v && v->type == CMCP_JSON_STRING) {
            free(s->peer_version);
            s->peer_version = xstrdup(v->str.s);
        }
    }
    const cmcp_json_t *cc = cmcp_json_object_get(req->params, "capabilities");
    client_caps_from_json(cc, &s->peer_caps);

    /* Build result. */
    cmcp_json_t *result = cmcp_json_new_object();
    cmcp_json_object_set(result, "protocolVersion",
                          cmcp_json_new_string(CMCP_PROTOCOL_VERSION));
    cmcp_json_object_set(result, "capabilities", server_caps_to_json(&s->caps));
    cmcp_json_t *si = cmcp_json_new_object();
    cmcp_json_object_set(si, "name",    cmcp_json_new_string(s->name));
    cmcp_json_object_set(si, "version", cmcp_json_new_string(s->version));
    cmcp_json_object_set(result, "serverInfo", si);

    cmcp_rpc_make_response(resp, &req->id, result);
    s->state = SS_HANDSHAKE;
}

/* ====================================================================== */
/* Per-frame handler                                                       */
/* ====================================================================== */

/* Dispatch one parsed message. For requests, *resp is initialised by
 * this function and the caller must clear it. */
static void server_handle_message(cmcp_server_t *s,
                                   const cmcp_rpc_message_t *msg,
                                   cmcp_rpc_message_t *resp) {
    cmcp_rpc_message_init(resp);

    if (msg->kind == CMCP_MSG_RESPONSE) {
        /* Servers don't send requests in v0.1, so they shouldn't see
         * responses. Drop silently — no reply. */
        return;
    }

    /* Notification path. */
    if (msg->kind == CMCP_MSG_NOTIFICATION) {
        if (msg->method &&
            strcmp(msg->method, "notifications/initialized") == 0) {
            if (s->state == SS_HANDSHAKE) s->state = SS_READY;
            /* Otherwise: stray initialized; ignore. */
        }
        /* All other notifications: drop silently per JSON-RPC spec. */
        return;
    }

    /* Request path. */
    if (msg->method && strcmp(msg->method, "initialize") == 0) {
        handle_initialize(s, msg, resp);
        return;
    }

    if (s->state != SS_READY) {
        cmcp_rpc_make_error(resp, &msg->id, CMCP_RPC_INVALID_REQUEST,
                             "Server is not ready: send `initialize` first",
                             NULL);
        return;
    }

    /* No tool registry yet — Phase 1.5. */
    cmcp_rpc_make_error(resp, &msg->id, CMCP_RPC_METHOD_NOT_FOUND,
                         msg->method ? msg->method : "(null)", NULL);
}

/* Send a response (or error) frame. */
static int send_message(cmcp_transport_t *t, const cmcp_rpc_message_t *m) {
    char *wire = cmcp_rpc_emit(m);
    if (!wire) return CMCP_ENOMEM;
    int rc = cmcp_transport_write(t, wire, strlen(wire));
    free(wire);
    return rc;
}

/* Send a -32700 parse error with id=null. */
static void send_parse_error(cmcp_transport_t *t) {
    cmcp_id_t nid; cmcp_id_init_null(&nid);
    cmcp_rpc_message_t e;
    if (cmcp_rpc_make_error(&e, &nid, CMCP_RPC_PARSE_ERROR,
                             "Parse error", NULL) == CMCP_OK) {
        send_message(t, &e);
        cmcp_rpc_message_clear(&e);
    }
    cmcp_id_clear(&nid);
}

/* ====================================================================== */
/* Run loop                                                                */
/* ====================================================================== */

int cmcp_server_run(cmcp_server_t *s, cmcp_transport_t *t) {
    if (!s || !t) return CMCP_EINVAL;

    for (;;) {
        char *frame = NULL; size_t flen = 0;
        int rc = cmcp_transport_read(t, &frame, &flen);
        if (rc != CMCP_OK) break;        /* EOF or read error → done */

        cmcp_rpc_message_t *msgs = NULL; size_t n = 0;
        int prc = cmcp_rpc_parse(frame, flen, &msgs, &n);
        free(frame);

        if (prc == CMCP_EPARSE) {
            send_parse_error(t);
            continue;
        }
        if (prc != CMCP_OK || n == 0) {
            /* Malformed JSON-RPC: -32600 invalid request, no id. */
            cmcp_id_t nid; cmcp_id_init_null(&nid);
            cmcp_rpc_message_t e;
            if (cmcp_rpc_make_error(&e, &nid, CMCP_RPC_INVALID_REQUEST,
                                     "Invalid Request", NULL) == CMCP_OK) {
                send_message(t, &e);
                cmcp_rpc_message_clear(&e);
            }
            cmcp_id_clear(&nid);
            cmcp_rpc_messages_free(msgs, n);
            continue;
        }

        if (n != 1) {
            /* Batch: MCP 2025-06-18 disallows. */
            cmcp_id_t nid; cmcp_id_init_null(&nid);
            cmcp_rpc_message_t e;
            cmcp_rpc_make_error(&e, &nid, CMCP_RPC_INVALID_REQUEST,
                                 "Batch requests are not supported", NULL);
            send_message(t, &e);
            cmcp_rpc_message_clear(&e);
            cmcp_id_clear(&nid);
            cmcp_rpc_messages_free(msgs, n);
            continue;
        }

        cmcp_rpc_message_t resp;
        server_handle_message(s, &msgs[0], &resp);

        /* Only requests get replies. */
        if (msgs[0].kind == CMCP_MSG_REQUEST) {
            send_message(t, &resp);
        }
        cmcp_rpc_message_clear(&resp);
        cmcp_rpc_messages_free(msgs, n);
    }

    s->state = SS_CLOSED;
    return CMCP_OK;
}
