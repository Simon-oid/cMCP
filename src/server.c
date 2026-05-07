#include "cmcp.h"
#include "cmcp_server.h"
#include "cmcp_json.h"
#include "cmcp_schema.h"
#include "cmcp_types.h"
#include "cmcp_transport.h"

#include <pthread.h>
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

/* ====================================================================== */
/* Internal registry entries                                               */
/* ====================================================================== */

typedef struct {
    char                 *name;
    char                 *description;
    cmcp_json_t          *input_schema;     /* may be NULL */
    cmcp_tool_handler_fn  handler;
    void                 *userdata;
} server_tool_t;

typedef struct {
    char                  *uri;
    char                  *name;
    char                  *description;
    char                  *mime_type;
    cmcp_resource_read_fn  read;
    void                  *userdata;
} server_resource_t;

typedef struct {
    char                   *name;
    char                   *description;
    cmcp_json_t            *arguments;     /* JSON array, may be NULL */
    cmcp_prompt_handler_fn  handler;
    void                   *userdata;
} server_prompt_t;

struct cmcp_server {
    char                       *name;
    char                       *version;
    cmcp_server_capabilities_t  caps;

    server_state_t              state;

    /* Tool registry. Linear array — N is tiny in practice (< 50). */
    server_tool_t              *tools;
    size_t                      n_tools;
    size_t                      cap_tools;

    /* Resource registry — same shape. */
    server_resource_t          *resources;
    size_t                      n_resources;
    size_t                      cap_resources;

    /* Prompt registry — same shape. */
    server_prompt_t            *prompts;
    size_t                      n_prompts;
    size_t                      cap_prompts;

    /* Resource subscriptions: list of URIs the (single) peer subscribed
     * to. Notification emit lands in Phase 2.4; we already store the
     * set so subscribe/unsubscribe are real round-trips. */
    char                      **sub_uris;
    size_t                      n_subs;
    size_t                      cap_subs;

    /* Negotiated peer info, set on initialize. */
    char                        *peer_name;
    char                        *peer_version;
    cmcp_client_capabilities_t   peer_caps;

    /* Active transport, valid only during cmcp_server_run. notify_mu
     * serialises pointer access between the run loop (which sets +
     * clears it) and external threads calling cmcp_server_notify. */
    cmcp_transport_t            *active_transport;
    pthread_mutex_t              notify_mu;
    int                          notify_mu_init;
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
    if (pthread_mutex_init(&s->notify_mu, NULL) != 0) {
        free(s->name); free(s->version); free(s);
        return NULL;
    }
    s->notify_mu_init = 1;
    return s;
}

static void tool_clear(server_tool_t *t) {
    free(t->name);
    free(t->description);
    cmcp_json_free(t->input_schema);
}

static void resource_clear(server_resource_t *r) {
    free(r->uri);
    free(r->name);
    free(r->description);
    free(r->mime_type);
}

static void prompt_clear(server_prompt_t *p) {
    free(p->name);
    free(p->description);
    cmcp_json_free(p->arguments);
}

void cmcp_server_free(cmcp_server_t *s) {
    if (!s) return;
    free(s->name);
    free(s->version);
    free(s->peer_name);
    free(s->peer_version);
    for (size_t i = 0; i < s->n_tools; i++) tool_clear(&s->tools[i]);
    free(s->tools);
    for (size_t i = 0; i < s->n_resources; i++) resource_clear(&s->resources[i]);
    free(s->resources);
    for (size_t i = 0; i < s->n_prompts; i++) prompt_clear(&s->prompts[i]);
    free(s->prompts);
    for (size_t i = 0; i < s->n_subs; i++) free(s->sub_uris[i]);
    free(s->sub_uris);
    if (s->notify_mu_init) pthread_mutex_destroy(&s->notify_mu);
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
/* Tool registry                                                           */
/* ====================================================================== */

static server_tool_t *tool_find(cmcp_server_t *s, const char *name) {
    if (!name) return NULL;
    for (size_t i = 0; i < s->n_tools; i++) {
        if (strcmp(s->tools[i].name, name) == 0) return &s->tools[i];
    }
    return NULL;
}

int cmcp_server_add_tool(cmcp_server_t *s, const cmcp_tool_t *tool) {
    if (!s || !tool || !tool->name || !tool->handler) return CMCP_EINVAL;
    if (tool_find(s, tool->name)) return CMCP_EPROTOCOL;

    cmcp_json_t *schema = NULL;
    if (tool->input_schema) {
        schema = cmcp_json_parse(tool->input_schema, strlen(tool->input_schema));
        if (!schema) return CMCP_EPARSE;
        if (schema->type != CMCP_JSON_OBJECT) {
            cmcp_json_free(schema);
            return CMCP_EPARSE;
        }
    }

    /* Grow the array if needed. */
    if (s->n_tools == s->cap_tools) {
        size_t newcap = s->cap_tools ? s->cap_tools * 2 : 4;
        server_tool_t *nt = (server_tool_t *)realloc(
            s->tools, newcap * sizeof *nt);
        if (!nt) { cmcp_json_free(schema); return CMCP_ENOMEM; }
        s->tools = nt;
        s->cap_tools = newcap;
    }

    server_tool_t *t = &s->tools[s->n_tools];
    memset(t, 0, sizeof *t);
    t->name        = xstrdup(tool->name);
    t->description = tool->description ? xstrdup(tool->description) : NULL;
    t->input_schema = schema;
    t->handler     = tool->handler;
    t->userdata    = tool->userdata;
    if (!t->name || (tool->description && !t->description)) {
        tool_clear(t);
        return CMCP_ENOMEM;
    }
    s->n_tools++;
    return CMCP_OK;
}

cmcp_json_t *cmcp_tool_text_content(const char *text) {
    if (!text) return NULL;
    cmcp_json_t *arr = cmcp_json_new_array();
    if (!arr) return NULL;
    cmcp_json_t *item = cmcp_json_new_object();
    if (!item) { cmcp_json_free(arr); return NULL; }
    cmcp_json_object_set(item, "type", cmcp_json_new_string("text"));
    cmcp_json_object_set(item, "text", cmcp_json_new_string(text));
    cmcp_json_array_append(arr, item);
    return arr;
}

/* ====================================================================== */
/* Resource registry                                                       */
/* ====================================================================== */

static server_resource_t *resource_find(cmcp_server_t *s, const char *uri) {
    if (!uri) return NULL;
    for (size_t i = 0; i < s->n_resources; i++) {
        if (strcmp(s->resources[i].uri, uri) == 0) return &s->resources[i];
    }
    return NULL;
}

int cmcp_server_add_resource(cmcp_server_t *s, const cmcp_resource_t *r) {
    if (!s || !r || !r->uri || !r->name || !r->read) return CMCP_EINVAL;
    if (resource_find(s, r->uri)) return CMCP_EPROTOCOL;

    if (s->n_resources == s->cap_resources) {
        size_t newcap = s->cap_resources ? s->cap_resources * 2 : 4;
        server_resource_t *nr = (server_resource_t *)realloc(
            s->resources, newcap * sizeof *nr);
        if (!nr) return CMCP_ENOMEM;
        s->resources = nr;
        s->cap_resources = newcap;
    }

    server_resource_t *e = &s->resources[s->n_resources];
    memset(e, 0, sizeof *e);
    e->uri         = xstrdup(r->uri);
    e->name        = xstrdup(r->name);
    e->description = r->description ? xstrdup(r->description) : NULL;
    e->mime_type   = r->mime_type ? xstrdup(r->mime_type) : NULL;
    e->read        = r->read;
    e->userdata    = r->userdata;
    if (!e->uri || !e->name ||
        (r->description && !e->description) ||
        (r->mime_type && !e->mime_type)) {
        resource_clear(e);
        memset(e, 0, sizeof *e);
        return CMCP_ENOMEM;
    }
    s->n_resources++;
    return CMCP_OK;
}

cmcp_json_t *cmcp_resource_text_contents(const char *uri,
                                          const char *mime_type,
                                          const char *text) {
    if (!uri || !text) return NULL;
    cmcp_json_t *arr = cmcp_json_new_array();
    if (!arr) return NULL;
    cmcp_json_t *item = cmcp_json_new_object();
    if (!item) { cmcp_json_free(arr); return NULL; }
    cmcp_json_object_set(item, "uri", cmcp_json_new_string(uri));
    if (mime_type)
        cmcp_json_object_set(item, "mimeType", cmcp_json_new_string(mime_type));
    cmcp_json_object_set(item, "text", cmcp_json_new_string(text));
    cmcp_json_array_append(arr, item);
    return arr;
}

/* ====================================================================== */
/* Prompt registry                                                         */
/* ====================================================================== */

static server_prompt_t *prompt_find(cmcp_server_t *s, const char *name) {
    if (!name) return NULL;
    for (size_t i = 0; i < s->n_prompts; i++) {
        if (strcmp(s->prompts[i].name, name) == 0) return &s->prompts[i];
    }
    return NULL;
}

int cmcp_server_add_prompt(cmcp_server_t *s, const cmcp_prompt_t *p) {
    if (!s || !p || !p->name || !p->handler) return CMCP_EINVAL;
    if (prompt_find(s, p->name)) return CMCP_EPROTOCOL;

    cmcp_json_t *args = NULL;
    if (p->arguments) {
        args = cmcp_json_parse(p->arguments, strlen(p->arguments));
        if (!args) return CMCP_EPARSE;
        if (args->type != CMCP_JSON_ARRAY) {
            cmcp_json_free(args);
            return CMCP_EPARSE;
        }
    }

    if (s->n_prompts == s->cap_prompts) {
        size_t newcap = s->cap_prompts ? s->cap_prompts * 2 : 4;
        server_prompt_t *np = (server_prompt_t *)realloc(
            s->prompts, newcap * sizeof *np);
        if (!np) { cmcp_json_free(args); return CMCP_ENOMEM; }
        s->prompts = np;
        s->cap_prompts = newcap;
    }

    server_prompt_t *e = &s->prompts[s->n_prompts];
    memset(e, 0, sizeof *e);
    e->name        = xstrdup(p->name);
    e->description = p->description ? xstrdup(p->description) : NULL;
    e->arguments   = args;
    e->handler     = p->handler;
    e->userdata    = p->userdata;
    if (!e->name || (p->description && !e->description)) {
        prompt_clear(e);
        memset(e, 0, sizeof *e);
        return CMCP_ENOMEM;
    }
    s->n_prompts++;
    return CMCP_OK;
}

cmcp_json_t *cmcp_prompt_text_messages(const char *role, const char *text) {
    if (!role || !text) return NULL;
    cmcp_json_t *arr = cmcp_json_new_array();
    if (!arr) return NULL;
    cmcp_json_t *msg = cmcp_json_new_object();
    cmcp_json_t *content = cmcp_json_new_object();
    if (!msg || !content) {
        cmcp_json_free(arr); cmcp_json_free(msg); cmcp_json_free(content);
        return NULL;
    }
    cmcp_json_object_set(content, "type", cmcp_json_new_string("text"));
    cmcp_json_object_set(content, "text", cmcp_json_new_string(text));
    cmcp_json_object_set(msg, "role", cmcp_json_new_string(role));
    cmcp_json_object_set(msg, "content", content);
    cmcp_json_array_append(arr, msg);
    return arr;
}

/* ====================================================================== */
/* JSON helpers for capability ↔ object encoding                           */
/* ====================================================================== */

/* Build the server's capabilities sub-object. The presence of a
 * top-level key (e.g. "tools": {}) signals that the capability is
 * offered; subkeys advertise optional sub-capabilities. */
static cmcp_json_t *server_caps_to_json(const cmcp_server_t *s) {
    const cmcp_server_capabilities_t *c = &s->caps;
    cmcp_json_t *root = cmcp_json_new_object();
    if (!root) return NULL;

    /* tools — declared if any tool is registered, OR explicitly via
     * tools_list_changed. */
    if (s->n_tools > 0 || c->tools_list_changed) {
        cmcp_json_t *tools = cmcp_json_new_object();
        if (c->tools_list_changed)
            cmcp_json_object_set(tools, "listChanged", cmcp_json_new_bool(1));
        cmcp_json_object_set(root, "tools", tools);
    }
    if (s->n_resources > 0 || c->resources_list_changed || c->resources_subscribe) {
        cmcp_json_t *res = cmcp_json_new_object();
        if (c->resources_subscribe)
            cmcp_json_object_set(res, "subscribe", cmcp_json_new_bool(1));
        if (c->resources_list_changed)
            cmcp_json_object_set(res, "listChanged", cmcp_json_new_bool(1));
        cmcp_json_object_set(root, "resources", res);
    }
    if (s->n_prompts > 0 || c->prompts_list_changed) {
        cmcp_json_t *pr = cmcp_json_new_object();
        if (c->prompts_list_changed)
            cmcp_json_object_set(pr, "listChanged", cmcp_json_new_bool(1));
        cmcp_json_object_set(root, "prompts", pr);
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
    cmcp_json_object_set(result, "capabilities", server_caps_to_json(s));
    cmcp_json_t *si = cmcp_json_new_object();
    cmcp_json_object_set(si, "name",    cmcp_json_new_string(s->name));
    cmcp_json_object_set(si, "version", cmcp_json_new_string(s->version));
    cmcp_json_object_set(result, "serverInfo", si);

    cmcp_rpc_make_response(resp, &req->id, result);
    s->state = SS_HANDSHAKE;
}

/* ====================================================================== */
/* tools/list                                                              */
/* ====================================================================== */

static cmcp_json_t *tool_to_descriptor(const server_tool_t *t) {
    cmcp_json_t *o = cmcp_json_new_object();
    if (!o) return NULL;
    cmcp_json_object_set(o, "name", cmcp_json_new_string(t->name));
    if (t->description)
        cmcp_json_object_set(o, "description",
                              cmcp_json_new_string(t->description));
    /* Spec requires inputSchema; default to {"type":"object"} if the
     * tool didn't supply one so clients can still validate against the
     * empty-args case. */
    if (t->input_schema) {
        cmcp_json_object_set(o, "inputSchema", cmcp_json_clone(t->input_schema));
    } else {
        cmcp_json_t *def = cmcp_json_new_object();
        cmcp_json_object_set(def, "type", cmcp_json_new_string("object"));
        cmcp_json_object_set(o, "inputSchema", def);
    }
    return o;
}

static void handle_tools_list(cmcp_server_t *s,
                               const cmcp_rpc_message_t *req,
                               cmcp_rpc_message_t *resp) {
    cmcp_json_t *result = cmcp_json_new_object();
    cmcp_json_t *arr    = cmcp_json_new_array();
    for (size_t i = 0; i < s->n_tools; i++) {
        cmcp_json_array_append(arr, tool_to_descriptor(&s->tools[i]));
    }
    cmcp_json_object_set(result, "tools", arr);
    /* No pagination in v0.1: omit nextCursor. */
    cmcp_rpc_make_response(resp, &req->id, result);
}

/* ====================================================================== */
/* tools/call                                                              */
/* ====================================================================== */

static void handle_tools_call(cmcp_server_t *s,
                               const cmcp_rpc_message_t *req,
                               cmcp_rpc_message_t *resp) {
    if (!req->params || req->params->type != CMCP_JSON_OBJECT) {
        reply_invalid_params(resp, &req->id,
                              "tools/call requires params object", NULL);
        return;
    }
    const cmcp_json_t *name_v = cmcp_json_object_get(req->params, "name");
    if (!name_v || name_v->type != CMCP_JSON_STRING) {
        reply_invalid_params(resp, &req->id,
                              "tools/call requires `name` string", NULL);
        return;
    }
    server_tool_t *t = tool_find(s, name_v->str.s);
    if (!t) {
        cmcp_json_t *data = cmcp_json_new_object();
        cmcp_json_object_set(data, "name",
                              cmcp_json_new_string(name_v->str.s));
        reply_invalid_params(resp, &req->id, "Unknown tool", data);
        return;
    }

    /* arguments is optional; default to absent. */
    const cmcp_json_t *args = cmcp_json_object_get(req->params, "arguments");

    /* Validate arguments against the registered input schema. NULL
     * arguments are treated as a JSON null by the validator; that
     * fails any `type: object` schema with required keys, which is
     * the right answer. Tools without an input_schema skip this step.
     *
     * Failures map to JSON-RPC -32602 INVALID_PARAMS with structured
     * data identifying path / keyword / message — clients can
     * surface or programmatically react to it. */
    if (t->input_schema) {
        cmcp_schema_error_t serr;
        int vr = cmcp_schema_validate(t->input_schema, args, &serr);
        if (vr == CMCP_ESCHEMA) {
            cmcp_json_t *data = cmcp_schema_error_to_json(&serr);
            cmcp_schema_error_clear(&serr);
            reply_invalid_params(resp, &req->id,
                                  "arguments failed schema validation", data);
            return;
        }
        if (vr != CMCP_OK) {
            /* CMCP_EINVAL (broken schema) or CMCP_ENOMEM. The schema
             * was parsed at registration time, so EINVAL here means
             * something else went wrong — treat as internal. */
            cmcp_schema_error_clear(&serr);
            cmcp_rpc_make_error(resp, &req->id, CMCP_RPC_INTERNAL_ERROR,
                                 "Schema validation failed internally", NULL);
            return;
        }
    }

    cmcp_json_t *content = NULL;
    int is_error = 0;
    int rc = t->handler(args, t->userdata, &content, &is_error);
    if (rc != CMCP_OK) {
        cmcp_json_free(content);
        cmcp_rpc_make_error(resp, &req->id, CMCP_RPC_INTERNAL_ERROR,
                             "Tool handler failed", NULL);
        return;
    }

    /* Wrap. NULL content → empty array (spec allows). */
    cmcp_json_t *result = cmcp_json_new_object();
    if (!content) content = cmcp_json_new_array();
    cmcp_json_object_set(result, "content", content);
    cmcp_json_object_set(result, "isError", cmcp_json_new_bool(is_error ? 1 : 0));
    cmcp_rpc_make_response(resp, &req->id, result);
}

/* ====================================================================== */
/* resources/list                                                          */
/* ====================================================================== */

static cmcp_json_t *resource_to_descriptor(const server_resource_t *r) {
    cmcp_json_t *o = cmcp_json_new_object();
    if (!o) return NULL;
    cmcp_json_object_set(o, "uri",  cmcp_json_new_string(r->uri));
    cmcp_json_object_set(o, "name", cmcp_json_new_string(r->name));
    if (r->description)
        cmcp_json_object_set(o, "description",
                              cmcp_json_new_string(r->description));
    if (r->mime_type)
        cmcp_json_object_set(o, "mimeType",
                              cmcp_json_new_string(r->mime_type));
    return o;
}

static void handle_resources_list(cmcp_server_t *s,
                                   const cmcp_rpc_message_t *req,
                                   cmcp_rpc_message_t *resp) {
    cmcp_json_t *result = cmcp_json_new_object();
    cmcp_json_t *arr    = cmcp_json_new_array();
    for (size_t i = 0; i < s->n_resources; i++) {
        cmcp_json_array_append(arr, resource_to_descriptor(&s->resources[i]));
    }
    cmcp_json_object_set(result, "resources", arr);
    cmcp_rpc_make_response(resp, &req->id, result);
}

/* ====================================================================== */
/* resources/read                                                          */
/* ====================================================================== */

static void handle_resources_read(cmcp_server_t *s,
                                   const cmcp_rpc_message_t *req,
                                   cmcp_rpc_message_t *resp) {
    if (!req->params || req->params->type != CMCP_JSON_OBJECT) {
        reply_invalid_params(resp, &req->id,
                              "resources/read requires params object", NULL);
        return;
    }
    const cmcp_json_t *uri_v = cmcp_json_object_get(req->params, "uri");
    if (!uri_v || uri_v->type != CMCP_JSON_STRING) {
        reply_invalid_params(resp, &req->id,
                              "resources/read requires `uri` string", NULL);
        return;
    }
    server_resource_t *r = resource_find(s, uri_v->str.s);
    if (!r) {
        cmcp_json_t *data = cmcp_json_new_object();
        cmcp_json_object_set(data, "uri", cmcp_json_new_string(uri_v->str.s));
        reply_invalid_params(resp, &req->id, "Unknown resource", data);
        return;
    }

    cmcp_json_t *contents = NULL;
    int is_error = 0;
    int rc = r->read(r->uri, r->userdata, &contents, &is_error);
    if (rc != CMCP_OK) {
        cmcp_json_free(contents);
        cmcp_rpc_make_error(resp, &req->id, CMCP_RPC_INTERNAL_ERROR,
                             "Resource read handler failed", NULL);
        return;
    }

    cmcp_json_t *result = cmcp_json_new_object();
    if (!contents) contents = cmcp_json_new_array();
    cmcp_json_object_set(result, "contents", contents);
    if (is_error)
        cmcp_json_object_set(result, "isError", cmcp_json_new_bool(1));
    cmcp_rpc_make_response(resp, &req->id, result);
}

/* ====================================================================== */
/* resources/subscribe + unsubscribe                                       */
/* ====================================================================== */

static int sub_index(cmcp_server_t *s, const char *uri) {
    for (size_t i = 0; i < s->n_subs; i++) {
        if (strcmp(s->sub_uris[i], uri) == 0) return (int)i;
    }
    return -1;
}

static int sub_add(cmcp_server_t *s, const char *uri) {
    if (sub_index(s, uri) >= 0) return CMCP_OK;        /* already subscribed */
    if (s->n_subs == s->cap_subs) {
        size_t newcap = s->cap_subs ? s->cap_subs * 2 : 4;
        char **n = (char **)realloc(s->sub_uris, newcap * sizeof *n);
        if (!n) return CMCP_ENOMEM;
        s->sub_uris = n;
        s->cap_subs = newcap;
    }
    char *dup = xstrdup(uri);
    if (!dup) return CMCP_ENOMEM;
    s->sub_uris[s->n_subs++] = dup;
    return CMCP_OK;
}

static void sub_remove(cmcp_server_t *s, const char *uri) {
    int idx = sub_index(s, uri);
    if (idx < 0) return;
    free(s->sub_uris[idx]);
    /* Compact: shift the tail. */
    for (size_t i = (size_t)idx + 1; i < s->n_subs; i++) {
        s->sub_uris[i - 1] = s->sub_uris[i];
    }
    s->n_subs--;
}

static void handle_resources_subscribe(cmcp_server_t *s,
                                        const cmcp_rpc_message_t *req,
                                        cmcp_rpc_message_t *resp,
                                        int subscribing) {
    if (!req->params || req->params->type != CMCP_JSON_OBJECT) {
        reply_invalid_params(resp, &req->id,
                              "expected params object with `uri`", NULL);
        return;
    }
    const cmcp_json_t *uri_v = cmcp_json_object_get(req->params, "uri");
    if (!uri_v || uri_v->type != CMCP_JSON_STRING) {
        reply_invalid_params(resp, &req->id, "missing `uri` string", NULL);
        return;
    }
    /* Subscribe is allowed against any URI — including ones the server
     * could later mint. The spec doesn't require the URI exist at
     * subscribe time. */
    if (subscribing) {
        if (sub_add(s, uri_v->str.s) != CMCP_OK) {
            cmcp_rpc_make_error(resp, &req->id, CMCP_RPC_INTERNAL_ERROR,
                                 "subscription tracking failed", NULL);
            return;
        }
    } else {
        sub_remove(s, uri_v->str.s);
    }
    cmcp_rpc_make_response(resp, &req->id, cmcp_json_new_object());
}

/* ====================================================================== */
/* prompts/list                                                            */
/* ====================================================================== */

static cmcp_json_t *prompt_to_descriptor(const server_prompt_t *p) {
    cmcp_json_t *o = cmcp_json_new_object();
    if (!o) return NULL;
    cmcp_json_object_set(o, "name", cmcp_json_new_string(p->name));
    if (p->description)
        cmcp_json_object_set(o, "description",
                              cmcp_json_new_string(p->description));
    if (p->arguments)
        cmcp_json_object_set(o, "arguments", cmcp_json_clone(p->arguments));
    return o;
}

static void handle_prompts_list(cmcp_server_t *s,
                                 const cmcp_rpc_message_t *req,
                                 cmcp_rpc_message_t *resp) {
    cmcp_json_t *result = cmcp_json_new_object();
    cmcp_json_t *arr    = cmcp_json_new_array();
    for (size_t i = 0; i < s->n_prompts; i++) {
        cmcp_json_array_append(arr, prompt_to_descriptor(&s->prompts[i]));
    }
    cmcp_json_object_set(result, "prompts", arr);
    cmcp_rpc_make_response(resp, &req->id, result);
}

/* ====================================================================== */
/* prompts/get                                                             */
/* ====================================================================== */

/* Walk the prompt's argument descriptor array and reject if any entry
 * marked `required: true` is absent from `args`. Soft validation —
 * full schemas are not modelled for prompt args (the spec uses a flat
 * descriptor list, not JSON Schema). */
static int check_required_prompt_args(const server_prompt_t *p,
                                       const cmcp_json_t *args,
                                       char **missing_name) {
    *missing_name = NULL;
    if (!p->arguments || p->arguments->type != CMCP_JSON_ARRAY) return 1;
    for (size_t i = 0; i < p->arguments->arr.len; i++) {
        const cmcp_json_t *desc = p->arguments->arr.items[i];
        if (!desc || desc->type != CMCP_JSON_OBJECT) continue;
        const cmcp_json_t *req = cmcp_json_object_get(desc, "required");
        if (!req || req->type != CMCP_JSON_BOOL || !req->b) continue;
        const cmcp_json_t *name = cmcp_json_object_get(desc, "name");
        if (!name || name->type != CMCP_JSON_STRING) continue;
        if (!args || args->type != CMCP_JSON_OBJECT ||
            !cmcp_json_object_get(args, name->str.s)) {
            *missing_name = name->str.s;       /* borrowed pointer */
            return 0;
        }
    }
    return 1;
}

static void handle_prompts_get(cmcp_server_t *s,
                                const cmcp_rpc_message_t *req,
                                cmcp_rpc_message_t *resp) {
    if (!req->params || req->params->type != CMCP_JSON_OBJECT) {
        reply_invalid_params(resp, &req->id,
                              "prompts/get requires params object", NULL);
        return;
    }
    const cmcp_json_t *name_v = cmcp_json_object_get(req->params, "name");
    if (!name_v || name_v->type != CMCP_JSON_STRING) {
        reply_invalid_params(resp, &req->id,
                              "prompts/get requires `name` string", NULL);
        return;
    }
    server_prompt_t *p = prompt_find(s, name_v->str.s);
    if (!p) {
        cmcp_json_t *data = cmcp_json_new_object();
        cmcp_json_object_set(data, "name",
                              cmcp_json_new_string(name_v->str.s));
        reply_invalid_params(resp, &req->id, "Unknown prompt", data);
        return;
    }

    const cmcp_json_t *args = cmcp_json_object_get(req->params, "arguments");

    char *missing = NULL;
    if (!check_required_prompt_args(p, args, &missing)) {
        cmcp_json_t *data = cmcp_json_new_object();
        cmcp_json_object_set(data, "name", cmcp_json_new_string(missing));
        reply_invalid_params(resp, &req->id,
                              "missing required prompt argument", data);
        return;
    }

    cmcp_json_t *messages = NULL;
    int rc = p->handler(args, p->userdata, &messages);
    if (rc != CMCP_OK) {
        cmcp_json_free(messages);
        cmcp_rpc_make_error(resp, &req->id, CMCP_RPC_INTERNAL_ERROR,
                             "Prompt handler failed", NULL);
        return;
    }

    cmcp_json_t *result = cmcp_json_new_object();
    if (p->description)
        cmcp_json_object_set(result, "description",
                              cmcp_json_new_string(p->description));
    if (!messages) messages = cmcp_json_new_array();
    cmcp_json_object_set(result, "messages", messages);
    cmcp_rpc_make_response(resp, &req->id, result);
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

    if (strcmp(msg->method, "tools/list") == 0) {
        handle_tools_list(s, msg, resp);
        return;
    }
    if (strcmp(msg->method, "tools/call") == 0) {
        handle_tools_call(s, msg, resp);
        return;
    }
    if (strcmp(msg->method, "resources/list") == 0) {
        handle_resources_list(s, msg, resp);
        return;
    }
    if (strcmp(msg->method, "resources/read") == 0) {
        handle_resources_read(s, msg, resp);
        return;
    }
    if (strcmp(msg->method, "resources/subscribe") == 0) {
        handle_resources_subscribe(s, msg, resp, 1);
        return;
    }
    if (strcmp(msg->method, "resources/unsubscribe") == 0) {
        handle_resources_subscribe(s, msg, resp, 0);
        return;
    }
    if (strcmp(msg->method, "prompts/list") == 0) {
        handle_prompts_list(s, msg, resp);
        return;
    }
    if (strcmp(msg->method, "prompts/get") == 0) {
        handle_prompts_get(s, msg, resp);
        return;
    }

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

    /* Make the transport available to cmcp_server_notify callers
     * (handlers + external threads). */
    pthread_mutex_lock(&s->notify_mu);
    s->active_transport = t;
    pthread_mutex_unlock(&s->notify_mu);

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

    pthread_mutex_lock(&s->notify_mu);
    s->active_transport = NULL;
    pthread_mutex_unlock(&s->notify_mu);

    s->state = SS_CLOSED;
    return CMCP_OK;
}

int cmcp_server_run_stdio(cmcp_server_t *s) {
    cmcp_transport_t *t = cmcp_transport_stdio_new();
    if (!t) return CMCP_ENOMEM;
    int rc = cmcp_server_run(s, t);
    cmcp_transport_close(t);
    return rc;
}

/* ====================================================================== */
/* Server-initiated notifications                                          */
/* ====================================================================== */

int cmcp_server_notify(cmcp_server_t *s,
                        const char *method,
                        cmcp_json_t *params) {
    if (!s || !method) {
        cmcp_json_free(params);
        return CMCP_EINVAL;
    }

    cmcp_rpc_message_t m;
    cmcp_rpc_message_init(&m);
    int rc = cmcp_rpc_make_notification(&m, method, params);
    if (rc != CMCP_OK) {
        cmcp_rpc_message_clear(&m);
        return rc;
    }
    char *wire = cmcp_rpc_emit(&m);
    cmcp_rpc_message_clear(&m);
    if (!wire) return CMCP_ENOMEM;

    /* Acquire a transport pointer atomically. */
    pthread_mutex_lock(&s->notify_mu);
    cmcp_transport_t *t = s->active_transport;
    pthread_mutex_unlock(&s->notify_mu);
    if (!t) {
        free(wire);
        return CMCP_EINVAL;
    }

    /* The transport's own write_fn is internally mutex-guarded against
     * concurrent writers (stdio's write mutex; HTTP's slot mutex). For
     * HTTP, write_fn classifies the body and routes notifications to
     * the SSE channel. */
    rc = cmcp_transport_write(t, wire, strlen(wire));
    free(wire);
    return rc;
}

int cmcp_server_notify_tools_changed(cmcp_server_t *s) {
    if (!s) return CMCP_EINVAL;
    if (!s->caps.tools_list_changed) return CMCP_EPROTOCOL;
    return cmcp_server_notify(s, "notifications/tools/list_changed", NULL);
}

int cmcp_server_notify_resources_changed(cmcp_server_t *s) {
    if (!s) return CMCP_EINVAL;
    if (!s->caps.resources_list_changed) return CMCP_EPROTOCOL;
    return cmcp_server_notify(s, "notifications/resources/list_changed", NULL);
}

int cmcp_server_notify_prompts_changed(cmcp_server_t *s) {
    if (!s) return CMCP_EINVAL;
    if (!s->caps.prompts_list_changed) return CMCP_EPROTOCOL;
    return cmcp_server_notify(s, "notifications/prompts/list_changed", NULL);
}

int cmcp_server_notify_resource_updated(cmcp_server_t *s, const char *uri) {
    if (!s || !uri) return CMCP_EINVAL;
    if (!s->caps.resources_subscribe) return CMCP_EPROTOCOL;
    /* Skip if no peer subscribed to this URI. */
    if (sub_index(s, uri) < 0) return CMCP_OK;

    cmcp_json_t *params = cmcp_json_new_object();
    if (!params) return CMCP_ENOMEM;
    cmcp_json_object_set(params, "uri", cmcp_json_new_string(uri));
    return cmcp_server_notify(s, "notifications/resources/updated", params);
}
