#include "cmcp.h"
#include "cmcp_client.h"
#include "cmcp_json.h"
#include "cmcp_types.h"
#include "cmcp_transport.h"

#include <stdlib.h>
#include <string.h>

struct cmcp_client {
    char                       *name;
    char                       *version;
    cmcp_client_capabilities_t  caps;

    cmcp_transport_t           *transport;       /* borrowed */
    cmcp_rpc_pending_t         *pending;
    int                         initialized;

    /* Server identity captured during handshake. */
    char                        *server_name;
    char                        *server_version;
    cmcp_server_capabilities_t   server_caps;
};

/* ---------------------------------------------------------------------- */

static char *xstrdup(const char *s) {
    if (!s) return NULL;
    size_t n = strlen(s);
    char *p = (char *)malloc(n + 1);
    if (!p) return NULL;
    memcpy(p, s, n + 1);
    return p;
}

cmcp_client_t *cmcp_client_new(const char *name, const char *version) {
    if (!name || !version) return NULL;
    cmcp_client_t *c = (cmcp_client_t *)calloc(1, sizeof *c);
    if (!c) return NULL;
    c->name    = xstrdup(name);
    c->version = xstrdup(version);
    c->pending = cmcp_rpc_pending_new();
    if (!c->name || !c->version || !c->pending) {
        cmcp_client_free(c);
        return NULL;
    }
    return c;
}

void cmcp_client_free(cmcp_client_t *c) {
    if (!c) return;
    free(c->name);
    free(c->version);
    free(c->server_name);
    free(c->server_version);
    cmcp_rpc_pending_free(c->pending);
    free(c);
}

void cmcp_client_set_capabilities(cmcp_client_t *c,
                                   const cmcp_client_capabilities_t *caps) {
    if (!c || !caps) return;
    c->caps = *caps;
}

const cmcp_server_capabilities_t *cmcp_client_server_caps(const cmcp_client_t *c) {
    return c ? &c->server_caps : NULL;
}
const char *cmcp_client_server_name(const cmcp_client_t *c) {
    return c ? c->server_name : NULL;
}
const char *cmcp_client_server_version(const cmcp_client_t *c) {
    return c ? c->server_version : NULL;
}

/* ---------------------------------------------------------------------- */
/* Capability helpers                                                      */
/* ---------------------------------------------------------------------- */

static cmcp_json_t *client_caps_to_json(const cmcp_client_capabilities_t *c) {
    cmcp_json_t *root = cmcp_json_new_object();
    if (!root) return NULL;
    if (c->sampling) {
        cmcp_json_object_set(root, "sampling", cmcp_json_new_object());
    }
    if (c->roots_list_changed) {
        cmcp_json_t *roots = cmcp_json_new_object();
        cmcp_json_object_set(roots, "listChanged", cmcp_json_new_bool(1));
        cmcp_json_object_set(root, "roots", roots);
    }
    return root;
}

static void server_caps_from_json(const cmcp_json_t *o,
                                   cmcp_server_capabilities_t *out) {
    memset(out, 0, sizeof *out);
    if (!o || o->type != CMCP_JSON_OBJECT) return;
    const cmcp_json_t *tools = cmcp_json_object_get(o, "tools");
    if (tools && tools->type == CMCP_JSON_OBJECT) {
        const cmcp_json_t *lc = cmcp_json_object_get(tools, "listChanged");
        if (lc && lc->type == CMCP_JSON_BOOL && lc->b) {
            out->tools_list_changed = 1;
        }
    }
    const cmcp_json_t *res = cmcp_json_object_get(o, "resources");
    if (res && res->type == CMCP_JSON_OBJECT) {
        const cmcp_json_t *sub = cmcp_json_object_get(res, "subscribe");
        const cmcp_json_t *lc  = cmcp_json_object_get(res, "listChanged");
        if (sub && sub->type == CMCP_JSON_BOOL && sub->b) out->resources_subscribe = 1;
        if (lc  && lc->type  == CMCP_JSON_BOOL && lc->b)  out->resources_list_changed = 1;
    }
    const cmcp_json_t *prompts = cmcp_json_object_get(o, "prompts");
    if (prompts && prompts->type == CMCP_JSON_OBJECT) {
        const cmcp_json_t *lc = cmcp_json_object_get(prompts, "listChanged");
        if (lc && lc->type == CMCP_JSON_BOOL && lc->b) {
            out->prompts_list_changed = 1;
        }
    }
    if (cmcp_json_object_get(o, "logging")) out->logging = 1;
}

/* ---------------------------------------------------------------------- */
/* Wire helpers                                                            */
/* ---------------------------------------------------------------------- */

static int send_message(cmcp_transport_t *t, const cmcp_rpc_message_t *m) {
    char *wire = cmcp_rpc_emit(m);
    if (!wire) return CMCP_ENOMEM;
    int rc = cmcp_transport_write(t, wire, strlen(wire));
    free(wire);
    return rc;
}

/* Read frames until one matches the expected response ID. Drops any
 * server-initiated notifications encountered along the way. The
 * matched response is moved into *out (zero-copy of the parsed
 * message struct). */
static int read_until_response(cmcp_transport_t *t, long long want_id,
                               cmcp_rpc_message_t *out) {
    for (;;) {
        char *frame = NULL; size_t flen = 0;
        int rc = cmcp_transport_read(t, &frame, &flen);
        if (rc != CMCP_OK) return rc;

        cmcp_rpc_message_t *msgs = NULL; size_t n = 0;
        int prc = cmcp_rpc_parse(frame, flen, &msgs, &n);
        free(frame);
        if (prc != CMCP_OK || n != 1) {
            cmcp_rpc_messages_free(msgs, n);
            continue;        /* skip malformed frames */
        }

        if (msgs[0].kind == CMCP_MSG_RESPONSE &&
            msgs[0].id.kind == CMCP_ID_INT &&
            msgs[0].id.i == want_id) {
            /* Move msgs[0] into *out without copying. */
            *out = msgs[0];
            cmcp_rpc_message_init(&msgs[0]);   /* prevent double free */
            cmcp_rpc_messages_free(msgs, n);
            return CMCP_OK;
        }

        /* Notification or unrelated response — drop. */
        cmcp_rpc_messages_free(msgs, n);
    }
}

/* ---------------------------------------------------------------------- */
/* Public API                                                              */
/* ---------------------------------------------------------------------- */

int cmcp_client_handshake(cmcp_client_t *c, cmcp_transport_t *t) {
    if (!c || !t) return CMCP_EINVAL;
    if (c->initialized) return CMCP_EPROTOCOL;
    c->transport = t;

    /* Build initialize request. */
    cmcp_json_t *params = cmcp_json_new_object();
    cmcp_json_object_set(params, "protocolVersion",
                          cmcp_json_new_string(CMCP_PROTOCOL_VERSION));
    cmcp_json_object_set(params, "capabilities",
                          client_caps_to_json(&c->caps));
    cmcp_json_t *ci = cmcp_json_new_object();
    cmcp_json_object_set(ci, "name",    cmcp_json_new_string(c->name));
    cmcp_json_object_set(ci, "version", cmcp_json_new_string(c->version));
    cmcp_json_object_set(params, "clientInfo", ci);

    long long id = cmcp_rpc_pending_register(c->pending, NULL);
    if (id == 0) { cmcp_json_free(params); return CMCP_ENOMEM; }

    cmcp_rpc_message_t req;
    int rc = cmcp_rpc_make_request(&req, id, "initialize", params);
    if (rc != CMCP_OK) { cmcp_json_free(params); return rc; }

    rc = send_message(t, &req);
    cmcp_rpc_message_clear(&req);
    if (rc != CMCP_OK) return rc;

    /* Wait for response. */
    cmcp_rpc_message_t resp;
    rc = read_until_response(t, id, &resp);
    cmcp_rpc_pending_take(c->pending, id, NULL);
    if (rc != CMCP_OK) return rc;

    if (resp.error) {
        cmcp_rpc_message_clear(&resp);
        return CMCP_EPROTOCOL;
    }
    if (!resp.result || resp.result->type != CMCP_JSON_OBJECT) {
        cmcp_rpc_message_clear(&resp);
        return CMCP_EPROTOCOL;
    }

    /* Validate negotiated protocol version. */
    const cmcp_json_t *pv = cmcp_json_object_get(resp.result, "protocolVersion");
    if (!pv || pv->type != CMCP_JSON_STRING ||
        strcmp(pv->str.s, CMCP_PROTOCOL_VERSION) != 0) {
        cmcp_rpc_message_clear(&resp);
        return CMCP_EPROTOCOL;
    }

    /* Capture server identity + caps. */
    const cmcp_json_t *si = cmcp_json_object_get(resp.result, "serverInfo");
    if (si && si->type == CMCP_JSON_OBJECT) {
        const cmcp_json_t *n = cmcp_json_object_get(si, "name");
        const cmcp_json_t *v = cmcp_json_object_get(si, "version");
        if (n && n->type == CMCP_JSON_STRING) {
            free(c->server_name);
            c->server_name = xstrdup(n->str.s);
        }
        if (v && v->type == CMCP_JSON_STRING) {
            free(c->server_version);
            c->server_version = xstrdup(v->str.s);
        }
    }
    server_caps_from_json(cmcp_json_object_get(resp.result, "capabilities"),
                           &c->server_caps);
    cmcp_rpc_message_clear(&resp);

    /* Send notifications/initialized. */
    rc = cmcp_client_notify(c, "notifications/initialized", NULL);
    if (rc != CMCP_OK) return rc;

    c->initialized = 1;
    return CMCP_OK;
}

int cmcp_client_notify(cmcp_client_t *c, const char *method,
                        cmcp_json_t *params) {
    if (!c || !c->transport || !method) return CMCP_EINVAL;
    cmcp_rpc_message_t n;
    int rc = cmcp_rpc_make_notification(&n, method, params);
    if (rc != CMCP_OK) { cmcp_json_free(params); return rc; }
    rc = send_message(c->transport, &n);
    cmcp_rpc_message_clear(&n);
    return rc;
}

int cmcp_client_request(cmcp_client_t *c, const char *method,
                         cmcp_json_t *params,
                         cmcp_rpc_message_t *out_response) {
    if (!c || !c->transport || !method || !out_response) return CMCP_EINVAL;

    long long id = cmcp_rpc_pending_register(c->pending, NULL);
    if (id == 0) { cmcp_json_free(params); return CMCP_ENOMEM; }

    cmcp_rpc_message_t req;
    int rc = cmcp_rpc_make_request(&req, id, method, params);
    if (rc != CMCP_OK) {
        cmcp_json_free(params);
        cmcp_rpc_pending_take(c->pending, id, NULL);
        return rc;
    }
    rc = send_message(c->transport, &req);
    cmcp_rpc_message_clear(&req);
    if (rc != CMCP_OK) {
        cmcp_rpc_pending_take(c->pending, id, NULL);
        return rc;
    }

    rc = read_until_response(c->transport, id, out_response);
    cmcp_rpc_pending_take(c->pending, id, NULL);
    return rc;
}
