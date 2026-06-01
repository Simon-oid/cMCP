/* _POSIX_C_SOURCE: clock_gettime, CLOCK_MONOTONIC, pthread_condattr_setclock
 * — used by the handler-timeout watchdog. */
#define _POSIX_C_SOURCE 200809L

#include "cmcp.h"
#include "cmcp_server.h"
#include "cmcp_json.h"
#include "cmcp_schema.h"
#include "cmcp_types.h"
#include "cmcp_transport.h"
#include "worker.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/resource.h>   /* getrlimit/setrlimit — F.4 RLIMIT_AS insurance */

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
    char                 *title;            /* may be NULL */
    char                 *description;
    cmcp_json_t          *input_schema;     /* may be NULL */
    cmcp_json_t          *output_schema;    /* may be NULL */
    cmcp_json_t          *icons;            /* JSON array, may be NULL */
    cmcp_tool_handler_fn  handler;
    void                 *userdata;
} server_tool_t;

typedef struct {
    char                  *uri;
    char                  *name;
    char                  *title;            /* may be NULL */
    char                  *description;
    char                  *mime_type;
    cmcp_json_t           *icons;            /* JSON array, may be NULL */
    cmcp_resource_read_fn  read;
    void                  *userdata;
} server_resource_t;

typedef struct {
    char                   *name;
    char                   *title;          /* may be NULL */
    char                   *description;
    cmcp_json_t            *arguments;     /* JSON array, may be NULL */
    cmcp_json_t            *icons;         /* JSON array, may be NULL */
    cmcp_prompt_handler_fn  handler;
    void                   *userdata;
} server_prompt_t;

/* One in-flight pool request: its id, the handler ctx whose cancel flag
 * to flip, and a monotonic deadline for the timeout watchdog. */
typedef struct {
    cmcp_id_t            id;        /* owned copy of the request id */
    cmcp_handler_ctx_t  *ctx;       /* borrowed: lives in the work_item */
    struct timespec      deadline;  /* CLOCK_MONOTONIC; {0,0} = no timeout */
} inflight_entry_t;

/* A server→client request awaiting its response. Created on the worker
 * thread that called cmcp_server_send_request, parked on cv until the
 * run loop routes the matching RESPONSE frame (sets done=1) or the
 * peer dies (cancelled=1, broadcast by outgoing_cancel_all). */
typedef struct outgoing_pending {
    long long                  id;
    pthread_mutex_t            mu;
    pthread_cond_t             cv;
    int                        done;        /* response arrived */
    int                        cancelled;   /* transport gone, give up */
    cmcp_rpc_message_t         response;    /* valid iff done && !cancelled */
    struct outgoing_pending   *next, *prev;
} outgoing_pending_t;

struct cmcp_server {
    char                       *name;
    char                       *version;
    char                       *description;       /* optional, MCP 2025-11-25 */
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
    char                        *peer_description;   /* optional, MCP 2025-11-25 */
    cmcp_client_capabilities_t   peer_caps;

    /* Active transport, valid only during cmcp_server_run. notify_mu
     * serialises pointer access between the run loop (which sets +
     * clears it) and external threads calling cmcp_server_notify. */
    cmcp_transport_t            *active_transport;
    pthread_mutex_t              notify_mu;
    int                          notify_mu_init;

    /* In-flight pool requests, for cancellation + the timeout watchdog.
     * Registered at enqueue, removed when the worker finishes. inflight_mu
     * guards the table AND each ctx's `cancelled` flag. */
    inflight_entry_t            *inflight;
    size_t                       n_inflight;
    size_t                       cap_inflight;
    pthread_mutex_t              inflight_mu;
    int                          inflight_mu_init;
    long                         handler_timeout_ms;  /* 0 = watchdog off */

    /* Server→client requests awaiting their response. outgoing_mu
     * guards both the linked list and the monotonic id counter; each
     * record then has its own mu/cv for the worker thread to park on. */
    pthread_mutex_t              outgoing_mu;
    int                          outgoing_mu_init;
    long long                    outgoing_id_counter;
    outgoing_pending_t          *outgoing_head;

    /* `notifications/message` floor. Set by the client via
     * logging/setLevel; messages below this level are dropped by
     * cmcp_server_log. Default is CMCP_LOG_LEVEL_DEBUG (no filter) —
     * the host is expected to dial it down. log_mu serialises level
     * reads against setLevel writes from the loop thread. */
    pthread_mutex_t              log_mu;
    int                          log_mu_init;
    cmcp_log_level_t             log_min_level;
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
    if (pthread_mutex_init(&s->inflight_mu, NULL) != 0) {
        pthread_mutex_destroy(&s->notify_mu);
        free(s->name); free(s->version); free(s);
        return NULL;
    }
    s->inflight_mu_init = 1;
    if (pthread_mutex_init(&s->outgoing_mu, NULL) != 0) {
        pthread_mutex_destroy(&s->notify_mu);
        pthread_mutex_destroy(&s->inflight_mu);
        free(s->name); free(s->version); free(s);
        return NULL;
    }
    s->outgoing_mu_init = 1;
    s->outgoing_id_counter = 0;
    if (pthread_mutex_init(&s->log_mu, NULL) != 0) {
        pthread_mutex_destroy(&s->notify_mu);
        pthread_mutex_destroy(&s->inflight_mu);
        pthread_mutex_destroy(&s->outgoing_mu);
        free(s->name); free(s->version); free(s);
        return NULL;
    }
    s->log_mu_init    = 1;
    s->log_min_level  = CMCP_LOG_LEVEL_DEBUG;
    return s;
}

static void tool_clear(server_tool_t *t) {
    free(t->name);
    free(t->title);
    free(t->description);
    cmcp_json_free(t->input_schema);
    cmcp_json_free(t->output_schema);
    cmcp_json_free(t->icons);
}

static void resource_clear(server_resource_t *r) {
    free(r->uri);
    free(r->name);
    free(r->title);
    free(r->description);
    free(r->mime_type);
    cmcp_json_free(r->icons);
}

static void prompt_clear(server_prompt_t *p) {
    free(p->name);
    free(p->title);
    free(p->description);
    cmcp_json_free(p->arguments);
    cmcp_json_free(p->icons);
}

void cmcp_server_free(cmcp_server_t *s) {
    if (!s) return;
    free(s->name);
    free(s->version);
    free(s->description);
    free(s->peer_name);
    free(s->peer_version);
    free(s->peer_description);
    for (size_t i = 0; i < s->n_tools; i++) tool_clear(&s->tools[i]);
    free(s->tools);
    for (size_t i = 0; i < s->n_resources; i++) resource_clear(&s->resources[i]);
    free(s->resources);
    for (size_t i = 0; i < s->n_prompts; i++) prompt_clear(&s->prompts[i]);
    free(s->prompts);
    for (size_t i = 0; i < s->n_subs; i++) free(s->sub_uris[i]);
    free(s->sub_uris);
    /* The run loop drains the pool before returning, so no in-flight
     * entry should survive; clear any stray id copies defensively. */
    for (size_t i = 0; i < s->n_inflight; i++)
        cmcp_id_clear(&s->inflight[i].id);
    free(s->inflight);
    if (s->notify_mu_init) pthread_mutex_destroy(&s->notify_mu);
    if (s->inflight_mu_init) pthread_mutex_destroy(&s->inflight_mu);
    if (s->outgoing_mu_init) pthread_mutex_destroy(&s->outgoing_mu);
    if (s->log_mu_init) pthread_mutex_destroy(&s->log_mu);
    free(s);
}

void cmcp_server_set_capabilities(cmcp_server_t *s,
                                   const cmcp_server_capabilities_t *caps) {
    if (!s || !caps) return;
    s->caps = *caps;
}

int cmcp_server_set_description(cmcp_server_t *s, const char *description) {
    if (!s) return CMCP_EINVAL;
    char *copy = NULL;
    if (description) {
        copy = xstrdup(description);
        if (!copy) return CMCP_ENOMEM;
    }
    free(s->description);
    s->description = copy;
    return CMCP_OK;
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
const char *cmcp_server_client_description(const cmcp_server_t *s) {
    return s ? s->peer_description : NULL;
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

    cmcp_json_t *in_schema = NULL;
    if (tool->input_schema) {
        in_schema = cmcp_json_parse(tool->input_schema,
                                     strlen(tool->input_schema));
        if (!in_schema) return CMCP_EPARSE;
        if (in_schema->type != CMCP_JSON_OBJECT) {
            cmcp_json_free(in_schema);
            return CMCP_EPARSE;
        }
    }
    cmcp_json_t *out_schema = NULL;
    if (tool->output_schema) {
        out_schema = cmcp_json_parse(tool->output_schema,
                                      strlen(tool->output_schema));
        if (!out_schema) { cmcp_json_free(in_schema); return CMCP_EPARSE; }
        if (out_schema->type != CMCP_JSON_OBJECT) {
            cmcp_json_free(in_schema);
            cmcp_json_free(out_schema);
            return CMCP_EPARSE;
        }
    }
    cmcp_json_t *icons = NULL;
    if (tool->icons) {
        icons = cmcp_json_parse(tool->icons, strlen(tool->icons));
        if (!icons || icons->type != CMCP_JSON_ARRAY) {
            cmcp_json_free(in_schema);
            cmcp_json_free(out_schema);
            cmcp_json_free(icons);
            return CMCP_EPARSE;
        }
    }

    /* Grow the array if needed. */
    if (s->n_tools == s->cap_tools) {
        size_t newcap = s->cap_tools ? s->cap_tools * 2 : 4;
        server_tool_t *nt = (server_tool_t *)realloc(
            s->tools, newcap * sizeof *nt);
        if (!nt) {
            cmcp_json_free(in_schema); cmcp_json_free(out_schema);
            cmcp_json_free(icons);
            return CMCP_ENOMEM;
        }
        s->tools = nt;
        s->cap_tools = newcap;
    }

    server_tool_t *t = &s->tools[s->n_tools];
    memset(t, 0, sizeof *t);
    t->name          = xstrdup(tool->name);
    t->title         = tool->title       ? xstrdup(tool->title)       : NULL;
    t->description   = tool->description ? xstrdup(tool->description) : NULL;
    t->input_schema  = in_schema;
    t->output_schema = out_schema;
    t->icons         = icons;
    t->handler       = tool->handler;
    t->userdata      = tool->userdata;
    if (!t->name ||
        (tool->title       && !t->title) ||
        (tool->description && !t->description)) {
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

cmcp_json_t *cmcp_tool_resource_link_content(const char *uri,
                                              const char *name,
                                              const char *description,
                                              const char *mime_type) {
    if (!uri || !name) return NULL;
    cmcp_json_t *arr = cmcp_json_new_array();
    if (!arr) return NULL;
    cmcp_json_t *item = cmcp_json_new_object();
    if (!item) { cmcp_json_free(arr); return NULL; }
    cmcp_json_object_set(item, "type", cmcp_json_new_string("resource_link"));
    cmcp_json_object_set(item, "uri",  cmcp_json_new_string(uri));
    cmcp_json_object_set(item, "name", cmcp_json_new_string(name));
    if (description)
        cmcp_json_object_set(item, "description",
                              cmcp_json_new_string(description));
    if (mime_type)
        cmcp_json_object_set(item, "mimeType",
                              cmcp_json_new_string(mime_type));
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

    cmcp_json_t *icons = NULL;
    if (r->icons) {
        icons = cmcp_json_parse(r->icons, strlen(r->icons));
        if (!icons || icons->type != CMCP_JSON_ARRAY) {
            cmcp_json_free(icons);
            return CMCP_EPARSE;
        }
    }

    if (s->n_resources == s->cap_resources) {
        size_t newcap = s->cap_resources ? s->cap_resources * 2 : 4;
        server_resource_t *nr = (server_resource_t *)realloc(
            s->resources, newcap * sizeof *nr);
        if (!nr) { cmcp_json_free(icons); return CMCP_ENOMEM; }
        s->resources = nr;
        s->cap_resources = newcap;
    }

    server_resource_t *e = &s->resources[s->n_resources];
    memset(e, 0, sizeof *e);
    e->uri         = xstrdup(r->uri);
    e->name        = xstrdup(r->name);
    e->title       = r->title       ? xstrdup(r->title)       : NULL;
    e->description = r->description ? xstrdup(r->description) : NULL;
    e->mime_type   = r->mime_type   ? xstrdup(r->mime_type)   : NULL;
    e->icons       = icons;
    e->read        = r->read;
    e->userdata    = r->userdata;
    if (!e->uri || !e->name ||
        (r->title       && !e->title) ||
        (r->description && !e->description) ||
        (r->mime_type   && !e->mime_type)) {
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
    cmcp_json_t *icons = NULL;
    if (p->icons) {
        icons = cmcp_json_parse(p->icons, strlen(p->icons));
        if (!icons || icons->type != CMCP_JSON_ARRAY) {
            cmcp_json_free(args);
            cmcp_json_free(icons);
            return CMCP_EPARSE;
        }
    }

    if (s->n_prompts == s->cap_prompts) {
        size_t newcap = s->cap_prompts ? s->cap_prompts * 2 : 4;
        server_prompt_t *np = (server_prompt_t *)realloc(
            s->prompts, newcap * sizeof *np);
        if (!np) {
            cmcp_json_free(args); cmcp_json_free(icons);
            return CMCP_ENOMEM;
        }
        s->prompts = np;
        s->cap_prompts = newcap;
    }

    server_prompt_t *e = &s->prompts[s->n_prompts];
    memset(e, 0, sizeof *e);
    e->name        = xstrdup(p->name);
    e->title       = p->title       ? xstrdup(p->title)       : NULL;
    e->description = p->description ? xstrdup(p->description) : NULL;
    e->arguments   = args;
    e->icons       = icons;
    e->handler     = p->handler;
    e->userdata    = p->userdata;
    if (!e->name ||
        (p->title       && !e->title) ||
        (p->description && !e->description)) {
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
    const cmcp_json_t *sampling = cmcp_json_object_get(o, "sampling");
    if (sampling) {
        out->sampling = 1;
        if (sampling->type == CMCP_JSON_OBJECT) {
            if (cmcp_json_object_get(sampling, "tools"))
                out->sampling_tools = 1;
        }
    }
    const cmcp_json_t *elicit = cmcp_json_object_get(o, "elicitation");
    if (elicit) {
        out->elicitation = 1;
        if (elicit->type == CMCP_JSON_OBJECT) {
            if (cmcp_json_object_get(elicit, "form"))
                out->elicitation_form = 1;
            if (cmcp_json_object_get(elicit, "url"))
                out->elicitation_url = 1;
        }
    }
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

    /* Protocol version negotiation. Per MCP spec: if the client
     * requests a version we don't support, we MUST still respond
     * (with our own supported version below) and let the client
     * decide whether to disconnect. Only a missing or malformed
     * `protocolVersion` field is a -32602. */
    const cmcp_json_t *pv = cmcp_json_object_get(req->params, "protocolVersion");
    if (!pv || pv->type != CMCP_JSON_STRING) {
        reply_invalid_params(resp, &req->id,
                              "initialize requires protocolVersion (string)",
                              NULL);
        return;
    }

    /* Capture client info + caps. */
    const cmcp_json_t *ci = cmcp_json_object_get(req->params, "clientInfo");
    if (ci && ci->type == CMCP_JSON_OBJECT) {
        const cmcp_json_t *n = cmcp_json_object_get(ci, "name");
        const cmcp_json_t *v = cmcp_json_object_get(ci, "version");
        const cmcp_json_t *d = cmcp_json_object_get(ci, "description");
        if (n && n->type == CMCP_JSON_STRING) {
            free(s->peer_name);
            s->peer_name = xstrdup(n->str.s);
        }
        if (v && v->type == CMCP_JSON_STRING) {
            free(s->peer_version);
            s->peer_version = xstrdup(v->str.s);
        }
        if (d && d->type == CMCP_JSON_STRING) {
            free(s->peer_description);
            s->peer_description = xstrdup(d->str.s);
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
    if (s->description) {
        cmcp_json_object_set(si, "description",
                              cmcp_json_new_string(s->description));
    }
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
    if (t->title)
        cmcp_json_object_set(o, "title", cmcp_json_new_string(t->title));
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
    if (t->output_schema)
        cmcp_json_object_set(o, "outputSchema",
                              cmcp_json_clone(t->output_schema));
    if (t->icons)
        cmcp_json_object_set(o, "icons", cmcp_json_clone(t->icons));
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
/* Handler context                                                         */
/* ====================================================================== */

/* Per-call handle passed to every handler. Created on the worker (or
 * loop) stack for the duration of one handler call; the borrowed
 * pointers (server, progress_token) are valid only that long.
 *
 * `structured` is OWNED — set via cmcp_handler_set_structured. The
 * tools/call dispatcher reads it after the handler returns, attaches
 * it to the response, and clears it. Resource/prompt dispatchers
 * ignore the field (set_structured no-ops for them). */
struct cmcp_handler_ctx {
    cmcp_server_t      *server;          /* for emitting progress */
    const cmcp_json_t  *progress_token;  /* params._meta.progressToken,
                                          * NULL if the caller sent none */
    int                 cancelled;       /* set by notifications/cancelled;
                                          * wired in Phase 3.4 Step 3 */
    cmcp_json_t        *structured;      /* owned; attached to tool result */
    int                 is_tool_call;    /* set_structured no-ops elsewhere */
};

/* params._meta.progressToken — a string or int per spec — or NULL. The
 * returned pointer is borrowed from `params`. */
static const cmcp_json_t *meta_progress_token(const cmcp_json_t *params) {
    if (!params || params->type != CMCP_JSON_OBJECT) return NULL;
    const cmcp_json_t *meta = cmcp_json_object_get(params, "_meta");
    if (!meta || meta->type != CMCP_JSON_OBJECT) return NULL;
    const cmcp_json_t *tok = cmcp_json_object_get(meta, "progressToken");
    if (tok && (tok->type == CMCP_JSON_STRING || tok->type == CMCP_JSON_INT))
        return tok;
    return NULL;
}

int cmcp_handler_cancelled(const cmcp_handler_ctx_t *hctx) {
    if (!hctx) return 0;
    /* `cancelled` is written by the cancel / watchdog paths under
     * inflight_mu; read it under the same lock. */
    pthread_mutex_lock(&hctx->server->inflight_mu);
    int c = hctx->cancelled;
    pthread_mutex_unlock(&hctx->server->inflight_mu);
    return c;
}

const char *cmcp_handler_get_header(const cmcp_handler_ctx_t *hctx,
                                    const char *name) {
    if (!hctx || !name) return NULL;
    cmcp_server_t *s = hctx->server;
    /* active_transport is stable for the whole run; notify_mu just
     * guards the pointer read (it's set/cleared under that lock). The
     * returned value's lifetime is governed by the transport (valid for
     * this handler call) — see cmcp_transport request_header_fn. */
    pthread_mutex_lock(&s->notify_mu);
    cmcp_transport_t *t = s->active_transport;
    pthread_mutex_unlock(&s->notify_mu);
    return cmcp_transport_request_header(t, name);
}

void cmcp_handler_set_structured(cmcp_handler_ctx_t *hctx,
                                  cmcp_json_t *value) {
    if (!hctx || !hctx->is_tool_call) {
        /* Non-tool ctx (resource/prompt) or NULL — the value would
         * never be consumed. Free it so the caller doesn't leak. */
        cmcp_json_free(value);
        return;
    }
    /* Replace any prior value. NULL clears. */
    cmcp_json_free(hctx->structured);
    hctx->structured = value;
}

int cmcp_handler_progress(cmcp_handler_ctx_t *hctx,
                          double progress, double total,
                          const char *message) {
    if (!hctx || !hctx->progress_token) return CMCP_OK;
    cmcp_json_t *params = cmcp_json_new_object();
    if (!params) return CMCP_ENOMEM;
    cmcp_json_object_set(params, "progressToken",
                         cmcp_json_clone(hctx->progress_token));
    cmcp_json_object_set(params, "progress", cmcp_json_new_double(progress));
    if (total >= 0)
        cmcp_json_object_set(params, "total", cmcp_json_new_double(total));
    if (message)
        cmcp_json_object_set(params, "message",
                             cmcp_json_new_string(message));
    return cmcp_server_notify(hctx->server, "notifications/progress", params);
}

/* ====================================================================== */
/* In-flight registry + cancellation                                       */
/* ====================================================================== */

/* Compare an owned request id against the JSON `requestId` of a
 * notifications/cancelled frame. Returns 1 on a match. */
static int id_equals_json(const cmcp_id_t *id, const cmcp_json_t *j) {
    if (!id || !j) return 0;
    if (id->kind == CMCP_ID_INT && j->type == CMCP_JSON_INT)
        return id->i == j->i;
    if (id->kind == CMCP_ID_STRING && j->type == CMCP_JSON_STRING)
        return j->str.s && strcmp(id->s, j->str.s) == 0;
    return 0;
}

/* Register a pool request before it is submitted, so a cancel arriving
 * while the request is still queued is not lost. Copies the id; the
 * `ctx` pointer is borrowed (it lives in the work_item). Stamps a
 * monotonic deadline when the timeout watchdog is enabled. */
static int inflight_register(cmcp_server_t *s, const cmcp_id_t *id,
                             cmcp_handler_ctx_t *ctx) {
    pthread_mutex_lock(&s->inflight_mu);
    if (s->n_inflight == s->cap_inflight) {
        size_t newcap = s->cap_inflight ? s->cap_inflight * 2 : 8;
        inflight_entry_t *ni = (inflight_entry_t *)realloc(
            s->inflight, newcap * sizeof *ni);
        if (!ni) { pthread_mutex_unlock(&s->inflight_mu); return CMCP_ENOMEM; }
        s->inflight     = ni;
        s->cap_inflight = newcap;
    }
    inflight_entry_t *e = &s->inflight[s->n_inflight];
    memset(e, 0, sizeof *e);
    if (cmcp_id_copy(&e->id, id) != CMCP_OK) {
        pthread_mutex_unlock(&s->inflight_mu);
        return CMCP_ENOMEM;
    }
    e->ctx = ctx;
    if (s->handler_timeout_ms > 0) {
        struct timespec d;
        clock_gettime(CLOCK_MONOTONIC, &d);
        long ms = s->handler_timeout_ms;
        d.tv_sec  += ms / 1000;
        d.tv_nsec += (ms % 1000) * 1000000L;
        if (d.tv_nsec >= 1000000000L) { d.tv_sec++; d.tv_nsec -= 1000000000L; }
        e->deadline = d;
    }
    s->n_inflight++;
    pthread_mutex_unlock(&s->inflight_mu);
    return CMCP_OK;
}

/* Remove the entry for `ctx` and report, under the same lock, whether
 * it had been cancelled — so the caller's decision can't race a cancel
 * that lands a moment later. Once removed, no cancel/watchdog thread can
 * reach `ctx`, which is about to be freed with its work_item. */
static int inflight_finish(cmcp_server_t *s, const cmcp_handler_ctx_t *ctx) {
    int cancelled = 0;
    pthread_mutex_lock(&s->inflight_mu);
    for (size_t i = 0; i < s->n_inflight; i++) {
        if (s->inflight[i].ctx == ctx) {
            cancelled = s->inflight[i].ctx->cancelled;
            cmcp_id_clear(&s->inflight[i].id);
            for (size_t j = i + 1; j < s->n_inflight; j++)
                s->inflight[j - 1] = s->inflight[j];
            s->n_inflight--;
            break;
        }
    }
    pthread_mutex_unlock(&s->inflight_mu);
    return cancelled;
}

/* Flag the in-flight request whose id matches `request_id` (a cancel
 * notification's payload). A no-op if it already finished. */
static void inflight_cancel(cmcp_server_t *s, const cmcp_json_t *request_id) {
    pthread_mutex_lock(&s->inflight_mu);
    for (size_t i = 0; i < s->n_inflight; i++) {
        if (id_equals_json(&s->inflight[i].id, request_id)) {
            s->inflight[i].ctx->cancelled = 1;
            break;
        }
    }
    pthread_mutex_unlock(&s->inflight_mu);
}

/* Flag every in-flight request — used at shutdown, when the peer is
 * gone and any response would only fail on a dead transport. */
static void inflight_cancel_all(cmcp_server_t *s) {
    pthread_mutex_lock(&s->inflight_mu);
    for (size_t i = 0; i < s->n_inflight; i++)
        s->inflight[i].ctx->cancelled = 1;
    pthread_mutex_unlock(&s->inflight_mu);
}

/* ====================================================================== */
/* Handler-timeout watchdog                                                */
/* ====================================================================== */

/* Per-handler timeout from $CMCP_HANDLER_TIMEOUT_MS (default 30000).
 * A value of 0 disables the watchdog entirely. */
static long resolve_handler_timeout(void) {
    const char *e = getenv("CMCP_HANDLER_TIMEOUT_MS");
    if (e && *e) {
        char *end;
        long v = strtol(e, &end, 10);
        if (*end == '\0' && v >= 0) return v;
    }
    return 30000;
}

/* A background thread that periodically flags in-flight requests whose
 * deadline has passed. It only sets the cooperative `cancelled` bit —
 * it never kills a thread; a handler that ignores the bit still runs to
 * completion (its response is then dropped, like any cancellation). */
typedef struct {
    cmcp_server_t  *server;
    pthread_t       thread;
    pthread_mutex_t mu;
    pthread_cond_t  cv;       /* CLOCK_MONOTONIC */
    int             stop;
    int             started;
} watchdog_t;

static void watchdog_sweep(cmcp_server_t *s) {
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    pthread_mutex_lock(&s->inflight_mu);
    for (size_t i = 0; i < s->n_inflight; i++) {
        inflight_entry_t *e = &s->inflight[i];
        if (e->deadline.tv_sec == 0 && e->deadline.tv_nsec == 0) continue;
        if (now.tv_sec > e->deadline.tv_sec ||
            (now.tv_sec == e->deadline.tv_sec &&
             now.tv_nsec >= e->deadline.tv_nsec)) {
            e->ctx->cancelled = 1;
        }
    }
    pthread_mutex_unlock(&s->inflight_mu);
}

static void *watchdog_main(void *arg) {
    watchdog_t *wd = (watchdog_t *)arg;
    pthread_mutex_lock(&wd->mu);
    while (!wd->stop) {
        struct timespec ts;
        clock_gettime(CLOCK_MONOTONIC, &ts);
        ts.tv_nsec += 200L * 1000 * 1000;   /* 200ms poll */
        if (ts.tv_nsec >= 1000000000L) { ts.tv_sec++; ts.tv_nsec -= 1000000000L; }
        pthread_cond_timedwait(&wd->cv, &wd->mu, &ts);
        if (wd->stop) break;
        pthread_mutex_unlock(&wd->mu);
        watchdog_sweep(wd->server);
        pthread_mutex_lock(&wd->mu);
    }
    pthread_mutex_unlock(&wd->mu);
    return NULL;
}

/* Start the watchdog. A no-op (leaving wd->started == 0) when the
 * timeout is disabled. */
static int watchdog_start(watchdog_t *wd, cmcp_server_t *s) {
    memset(wd, 0, sizeof *wd);
    wd->server = s;
    if (s->handler_timeout_ms <= 0) return CMCP_OK;   /* disabled */

    pthread_condattr_t ca;
    pthread_condattr_init(&ca);
    pthread_condattr_setclock(&ca, CLOCK_MONOTONIC);
    if (pthread_mutex_init(&wd->mu, NULL) != 0) {
        pthread_condattr_destroy(&ca);
        return CMCP_ENOMEM;
    }
    if (pthread_cond_init(&wd->cv, &ca) != 0) {
        pthread_condattr_destroy(&ca);
        pthread_mutex_destroy(&wd->mu);
        return CMCP_ENOMEM;
    }
    pthread_condattr_destroy(&ca);
    if (pthread_create(&wd->thread, NULL, watchdog_main, wd) != 0) {
        pthread_mutex_destroy(&wd->mu);
        pthread_cond_destroy(&wd->cv);
        return CMCP_ENOMEM;
    }
    wd->started = 1;
    return CMCP_OK;
}

static void watchdog_stop(watchdog_t *wd) {
    if (!wd->started) return;
    pthread_mutex_lock(&wd->mu);
    wd->stop = 1;
    pthread_cond_signal(&wd->cv);
    pthread_mutex_unlock(&wd->mu);
    pthread_join(wd->thread, NULL);
    pthread_mutex_destroy(&wd->mu);
    pthread_cond_destroy(&wd->cv);
    wd->started = 0;
}

/* ====================================================================== */
/* Outgoing-request pending table                                          */
/* ====================================================================== */
/* Server-initiated requests (sampling/elicitation/etc.). A worker-thread
 * handler calls cmcp_server_send_request, which allocates a fresh
 * outgoing id, links a completion record, sends the request, then parks
 * on the record's cv. The run-loop thread reads the matching RESPONSE
 * frame, moves it into the record, signals the cv. Shutdown unblocks
 * any still-parked waiter via outgoing_cancel_all. */

static outgoing_pending_t *outgoing_pending_new(long long id) {
    outgoing_pending_t *p = (outgoing_pending_t *)calloc(1, sizeof *p);
    if (!p) return NULL;
    p->id = id;
    pthread_mutex_init(&p->mu, NULL);
    pthread_cond_init(&p->cv, NULL);
    cmcp_rpc_message_init(&p->response);
    return p;
}

static void outgoing_pending_free(outgoing_pending_t *p) {
    if (!p) return;
    cmcp_rpc_message_clear(&p->response);
    pthread_mutex_destroy(&p->mu);
    pthread_cond_destroy(&p->cv);
    free(p);
}

static void outgoing_link(cmcp_server_t *s, outgoing_pending_t *p) {
    pthread_mutex_lock(&s->outgoing_mu);
    p->prev = NULL;
    p->next = s->outgoing_head;
    if (s->outgoing_head) s->outgoing_head->prev = p;
    s->outgoing_head = p;
    pthread_mutex_unlock(&s->outgoing_mu);
}

static void outgoing_unlink(cmcp_server_t *s, outgoing_pending_t *p) {
    pthread_mutex_lock(&s->outgoing_mu);
    if (p->prev) p->prev->next = p->next;
    else if (s->outgoing_head == p) s->outgoing_head = p->next;
    if (p->next) p->next->prev = p->prev;
    p->prev = p->next = NULL;
    pthread_mutex_unlock(&s->outgoing_mu);
}

/* Move `resp` into the matching completion's record and signal the
 * waiter. `resp` is moved on success (caller frees the now-empty
 * shell); left untouched on miss (caller must clear). */
static void deliver_outgoing_response(cmcp_server_t *s,
                                       cmcp_rpc_message_t *resp) {
    if (resp->id.kind != CMCP_ID_INT) return;
    pthread_mutex_lock(&s->outgoing_mu);
    outgoing_pending_t *p = s->outgoing_head;
    while (p && p->id != resp->id.i) p = p->next;
    if (p) {
        pthread_mutex_lock(&p->mu);
        p->response = *resp;
        cmcp_rpc_message_init(resp);
        p->done = 1;
        pthread_cond_broadcast(&p->cv);
        pthread_mutex_unlock(&p->mu);
    }
    pthread_mutex_unlock(&s->outgoing_mu);
}

/* Wake every still-pending waiter with cancelled=1. Called at run-loop
 * shutdown so a worker parked in cmcp_server_send_request doesn't
 * deadlock when the peer is gone. */
static void outgoing_cancel_all(cmcp_server_t *s) {
    pthread_mutex_lock(&s->outgoing_mu);
    outgoing_pending_t *p = s->outgoing_head;
    while (p) {
        pthread_mutex_lock(&p->mu);
        if (!p->done) {
            p->cancelled = 1;
            p->done = 1;
            pthread_cond_broadcast(&p->cv);
        }
        pthread_mutex_unlock(&p->mu);
        p = p->next;
    }
    pthread_mutex_unlock(&s->outgoing_mu);
}

/* ====================================================================== */
/* tools/call                                                              */
/* ====================================================================== */

static void handle_tools_call(cmcp_server_t *s,
                               const cmcp_rpc_message_t *req,
                               cmcp_rpc_message_t *resp,
                               cmcp_handler_ctx_t *hctx) {
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
     * MCP 2025-11-25 (Minor 5) flips this from a JSON-RPC protocol
     * error to a tool-execution error so the model can self-correct.
     * The path/keyword/message from the validator is rendered into a
     * human-readable text content item — the wire shape is
     *   { isError: true, content: [{ type: "text", text: "..." }] }
     * Non-validation protocol errors (missing `name`, unknown tool,
     * bad params shape) above stay as JSON-RPC errors. */
    if (t->input_schema) {
        cmcp_schema_error_t serr;
        int vr = cmcp_schema_validate(t->input_schema, args, &serr);
        if (vr == CMCP_ESCHEMA) {
            char buf[512];
            const char *path = (serr.path && *serr.path) ? serr.path : "/";
            const char *kw   = serr.keyword ? serr.keyword : "?";
            const char *msg  = serr.message ? serr.message : "validation failed";
            snprintf(buf, sizeof buf,
                     "Invalid arguments for tool '%s': %s (path: %s, keyword: %s)",
                     t->name, msg, path, kw);
            cmcp_schema_error_clear(&serr);
            cmcp_json_t *content = cmcp_tool_text_content(buf);
            cmcp_json_t *result  = cmcp_json_new_object();
            cmcp_json_object_set(result, "content", content);
            cmcp_json_object_set(result, "isError",
                                  cmcp_json_new_bool(1));
            cmcp_rpc_make_response(resp, &req->id, result);
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

    /* The pool path supplies a ctx (registered for cancel + timeout);
     * a NULL hctx — only the inline safety-net path — gets a transient
     * one with no cancellation wiring. */
    cmcp_handler_ctx_t local;
    if (!hctx) {
        local.server         = s;
        local.progress_token = meta_progress_token(req->params);
        local.cancelled      = 0;
        local.structured     = NULL;
        local.is_tool_call   = 1;
        hctx = &local;
    } else {
        hctx->is_tool_call = 1;
    }
    cmcp_json_t *content = NULL;
    int is_error = 0;
    int rc = t->handler(args, t->userdata, hctx, &content, &is_error);
    if (rc != CMCP_OK) {
        cmcp_json_free(content);
        cmcp_json_free(hctx->structured);
        hctx->structured = NULL;
        cmcp_rpc_make_error(resp, &req->id, CMCP_RPC_INTERNAL_ERROR,
                             "Tool handler failed", NULL);
        return;
    }

    /* If the handler attached structuredContent, validate it against
     * output_schema (spec: "server MUST provide structuredContent that
     * matches the schema") and surface validation failure as -32603. */
    if (hctx->structured && t->output_schema) {
        cmcp_schema_error_t serr;
        int vr = cmcp_schema_validate(t->output_schema, hctx->structured, &serr);
        cmcp_schema_error_clear(&serr);
        if (vr != CMCP_OK) {
            cmcp_json_free(content);
            cmcp_json_free(hctx->structured);
            hctx->structured = NULL;
            cmcp_rpc_make_error(resp, &req->id, CMCP_RPC_INTERNAL_ERROR,
                                 "structuredContent failed output_schema validation",
                                 NULL);
            return;
        }
    }

    /* Wrap. NULL content → empty array (spec allows). If structuredContent
     * is set and content is still empty, drop in a JSON-serialised
     * rendering so legacy clients that only read `content` still get
     * something — the spec calls for backwards-compat text. */
    cmcp_json_t *result = cmcp_json_new_object();
    if (!content) {
        if (hctx->structured) {
            char *rendered = cmcp_json_emit(hctx->structured);
            content = rendered ? cmcp_tool_text_content(rendered)
                                : cmcp_json_new_array();
            free(rendered);
        } else {
            content = cmcp_json_new_array();
        }
    }
    cmcp_json_object_set(result, "content", content);
    if (hctx->structured) {
        cmcp_json_object_set(result, "structuredContent", hctx->structured);
        hctx->structured = NULL;     /* moved into result */
    }
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
    if (r->title)
        cmcp_json_object_set(o, "title", cmcp_json_new_string(r->title));
    if (r->description)
        cmcp_json_object_set(o, "description",
                              cmcp_json_new_string(r->description));
    if (r->mime_type)
        cmcp_json_object_set(o, "mimeType",
                              cmcp_json_new_string(r->mime_type));
    if (r->icons)
        cmcp_json_object_set(o, "icons", cmcp_json_clone(r->icons));
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
                                   cmcp_rpc_message_t *resp,
                                   cmcp_handler_ctx_t *hctx) {
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

    cmcp_handler_ctx_t local;
    if (!hctx) {
        local.server         = s;
        local.progress_token = meta_progress_token(req->params);
        local.cancelled      = 0;
        local.structured     = NULL;
        local.is_tool_call   = 0;
        hctx = &local;
    } else {
        hctx->is_tool_call = 0;
    }
    cmcp_json_t *contents = NULL;
    int is_error = 0;
    int rc = r->read(r->uri, r->userdata, hctx, &contents, &is_error);
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
    if (p->title)
        cmcp_json_object_set(o, "title", cmcp_json_new_string(p->title));
    if (p->description)
        cmcp_json_object_set(o, "description",
                              cmcp_json_new_string(p->description));
    if (p->arguments)
        cmcp_json_object_set(o, "arguments", cmcp_json_clone(p->arguments));
    if (p->icons)
        cmcp_json_object_set(o, "icons", cmcp_json_clone(p->icons));
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
                                cmcp_rpc_message_t *resp,
                                cmcp_handler_ctx_t *hctx) {
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

    cmcp_handler_ctx_t local;
    if (!hctx) {
        local.server         = s;
        local.progress_token = meta_progress_token(req->params);
        local.cancelled      = 0;
        local.structured     = NULL;
        local.is_tool_call   = 0;
        hctx = &local;
    } else {
        hctx->is_tool_call = 0;
    }
    cmcp_json_t *messages = NULL;
    int rc = p->handler(args, p->userdata, hctx, &messages);
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
/* logging/setLevel                                                        */
/* ====================================================================== */

/* Cap-gated: if caps.logging wasn't opted in, fall through to the
 * outer -32601 path so an unprepared server isn't pretending to honour
 * the request. Otherwise stash the new floor under log_mu. */
static void handle_logging_set_level(cmcp_server_t *s,
                                      const cmcp_rpc_message_t *req,
                                      cmcp_rpc_message_t *resp) {
    if (!req->params || req->params->type != CMCP_JSON_OBJECT) {
        reply_invalid_params(resp, &req->id,
                              "logging/setLevel requires params object", NULL);
        return;
    }
    const cmcp_json_t *lv = cmcp_json_object_get(req->params, "level");
    if (!lv || lv->type != CMCP_JSON_STRING) {
        reply_invalid_params(resp, &req->id,
                              "logging/setLevel requires `level` string", NULL);
        return;
    }
    cmcp_log_level_t parsed;
    if (cmcp_log_level_from_name(lv->str.s, &parsed) != 0) {
        cmcp_json_t *data = cmcp_json_new_object();
        cmcp_json_object_set(data, "level",
                              cmcp_json_new_string(lv->str.s));
        reply_invalid_params(resp, &req->id, "unknown log level", data);
        return;
    }
    pthread_mutex_lock(&s->log_mu);
    s->log_min_level = parsed;
    pthread_mutex_unlock(&s->log_mu);
    cmcp_rpc_make_response(resp, &req->id, cmcp_json_new_object());
}

/* ====================================================================== */
/* Per-frame handler                                                       */
/* ====================================================================== */

/* Dispatch one parsed message. For requests, *resp is initialised by
 * this function and the caller must clear it. `hctx` is the per-call
 * handle for handler-invoking requests (cancellation + progress); it is
 * NULL for the inline path, where no handler runs. */
static void server_handle_message(cmcp_server_t *s,
                                   const cmcp_rpc_message_t *msg,
                                   cmcp_rpc_message_t *resp,
                                   cmcp_handler_ctx_t *hctx) {
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
        } else if (msg->method &&
                   strcmp(msg->method, "notifications/cancelled") == 0) {
            /* Cooperative cancellation: flag the in-flight request so
             * its handler can bail and its response is dropped. */
            const cmcp_json_t *rid = msg->params
                ? cmcp_json_object_get(msg->params, "requestId") : NULL;
            if (rid) inflight_cancel(s, rid);
        }
        /* All other notifications: drop silently per JSON-RPC spec. */
        return;
    }

    /* Request path. */
    if (msg->method && strcmp(msg->method, "initialize") == 0) {
        handle_initialize(s, msg, resp);
        return;
    }

    /* `ping` — a liveness probe. Either party may send it at any time;
     * the receiver MUST respond with an empty result. Answered before
     * the readiness gate so it works pre-handshake too. */
    if (msg->method && strcmp(msg->method, "ping") == 0) {
        cmcp_rpc_make_response(resp, &msg->id, cmcp_json_new_object());
        return;
    }

    if (s->state != SS_READY) {
        cmcp_rpc_make_error(resp, &msg->id, CMCP_RPC_INVALID_REQUEST,
                             "Server is not ready: send `initialize` first",
                             NULL);
        return;
    }

    /* JSON-RPC parse rejects method-less requests, so this is structurally
     * unreachable — but the analyzer doesn't see that constraint, and a
     * defensive guard is cheap. */
    if (!msg->method) {
        cmcp_rpc_make_error(resp, &msg->id, CMCP_RPC_INVALID_REQUEST,
                             "Request missing `method`", NULL);
        return;
    }

    if (strcmp(msg->method, "tools/list") == 0) {
        handle_tools_list(s, msg, resp);
        return;
    }
    if (strcmp(msg->method, "tools/call") == 0) {
        handle_tools_call(s, msg, resp, hctx);
        return;
    }
    if (strcmp(msg->method, "resources/list") == 0) {
        handle_resources_list(s, msg, resp);
        return;
    }
    if (strcmp(msg->method, "resources/read") == 0) {
        handle_resources_read(s, msg, resp, hctx);
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
        handle_prompts_get(s, msg, resp, hctx);
        return;
    }
    if (strcmp(msg->method, "logging/setLevel") == 0 && s->caps.logging) {
        handle_logging_set_level(s, msg, resp);
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
/* Worker-pool dispatch                                                     */
/* ====================================================================== */

/* Methods whose handling invokes a user-supplied handler. These are
 * dispatched to the worker pool so a slow handler can't stall the run
 * loop. Every other method (the handshake, the list queries,
 * subscribe/unsubscribe) is cheap and either read-only or touches the
 * lifecycle FSM, so it stays inline on the loop thread — that keeps the
 * FSM single-threaded and lock-free. */
static int is_pool_method(const char *method) {
    return method && (
        strcmp(method, "tools/call")     == 0 ||
        strcmp(method, "resources/read") == 0 ||
        strcmp(method, "prompts/get")    == 0);
}

/* Worker-thread count from $CMCP_WORKERS, clamped to [1,64], default 4. */
static size_t resolve_worker_count(void) {
    const char *e = getenv("CMCP_WORKERS");
    if (e && *e) {
        char *end;
        long v = strtol(e, &end, 10);
        if (*end == '\0' && v >= 1 && v <= 64) return (size_t)v;
    }
    return 4;
}

/* F.4 cheap insurance: if $CMCP_HANDLER_RLIMIT_AS_MB is a positive
 * integer, lower the process address-space soft limit so a runaway
 * handler hits malloc-returns-NULL instead of OOM-killing the box.
 * Coarse and process-wide (NOT per-handler isolation — see the contract
 * note on cmcp_server_add_tool). Best-effort: we never RAISE an existing
 * limit, never exceed the hard limit, and silently no-op on any parse or
 * setrlimit failure — a misconfigured knob must not stop the server from
 * running. Snapshotted once; applied at the first cmcp_server_run(). */
static pthread_once_t g_rlimit_once = PTHREAD_ONCE_INIT;

static void rlimit_as_init(void) {
#ifdef RLIMIT_AS
    const char *e = getenv("CMCP_HANDLER_RLIMIT_AS_MB");
    if (!e || !*e) return;
    char *end;
    long mb = strtol(e, &end, 10);
    if (*end != '\0' || mb <= 0) return;          /* unset/invalid → no-op */

    struct rlimit rl;
    if (getrlimit(RLIMIT_AS, &rl) != 0) return;

    /* Cap in bytes, guarding against overflow on the multiply. The MiB
     * factor is rlim_t-wide so the multiply happens in the wide type,
     * not int (clang-tidy bugprone-implicit-widening-of-multiplication). */
    const rlim_t MiB = (rlim_t)1024 * 1024;
    rlim_t want = (rlim_t)mb * MiB;
    if ((long)(want / MiB) != mb) return;  /* overflowed */

    /* Never raise above the current hard limit; never raise an already
     * lower soft limit. We only ever TIGHTEN. */
    if (rl.rlim_max != RLIM_INFINITY && want > rl.rlim_max)
        want = rl.rlim_max;
    if (rl.rlim_cur != RLIM_INFINITY && want >= rl.rlim_cur)
        return;                                   /* already at/below want */

    rl.rlim_cur = want;
    (void)setrlimit(RLIMIT_AS, &rl);              /* best-effort */
#endif
}

static void maybe_apply_rlimit_as(void) {
    pthread_once(&g_rlimit_once, rlimit_as_init);
}

/* A request handed to the pool. It owns `msg` and embeds the `ctx`
 * that the in-flight table points at; `server` and `transport` are
 * borrowed — both outlive the pool, because the run loop joins the pool
 * (cmcp_pool_free) before it returns. */
typedef struct {
    cmcp_server_t      *server;
    cmcp_transport_t   *transport;
    cmcp_rpc_message_t  msg;
    cmcp_handler_ctx_t  ctx;
} work_item_t;

/* Worker entry point: run one request to completion and reply. The
 * transport's write_fn is internally mutex-guarded, so concurrent
 * workers (and cmcp_server_notify) never interleave frames. */
static void process_work(void *arg) {
    work_item_t *w = (work_item_t *)arg;
    cmcp_rpc_message_t resp;
    server_handle_message(w->server, &w->msg, &resp, &w->ctx);
    /* Deregister before the reply decision (and before freeing the
     * work_item that holds `ctx`): inflight_finish removes the entry
     * and reports the final cancelled state under one lock. */
    int cancelled = inflight_finish(w->server, &w->ctx);
    /* Per the MCP spec a cancelled request SHOULD NOT get a response. */
    if (!cancelled)
        send_message(w->transport, &resp);
    cmcp_rpc_message_clear(&resp);
    cmcp_rpc_message_clear(&w->msg);
    free(w);
}

/* ====================================================================== */
/* Run loop                                                                */
/* ====================================================================== */

int cmcp_server_run(cmcp_server_t *s, cmcp_transport_t *t) {
    if (!s || !t) return CMCP_EINVAL;

    /* F.4 opt-in: tighten RLIMIT_AS once, before any handler can run. */
    maybe_apply_rlimit_as();

    cmcp_pool_t *pool = cmcp_pool_new(resolve_worker_count());
    if (!pool) return CMCP_ENOMEM;

    /* Handler-timeout watchdog — disabled when the timeout resolves
     * to 0. Started before the loop so the first request is covered. */
    s->handler_timeout_ms = resolve_handler_timeout();
    watchdog_t wd;
    if (watchdog_start(&wd, s) != CMCP_OK) {
        cmcp_pool_free(pool);
        return CMCP_ENOMEM;
    }

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
            /* Batch: MCP 2025-11-25 disallows. */
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

        /* Exactly one well-formed message. Responses to server→client
         * requests (sampling/elicitation/etc.) are routed to the
         * outgoing pending table — the worker that issued the request
         * is parked on its cv. Handler-invoking requests, once the
         * handshake is complete, go to the worker pool. Everything
         * else — the handshake itself, list/subscribe queries,
         * notifications — is handled inline, keeping the lifecycle FSM
         * on this one thread. */
        if (msgs[0].kind == CMCP_MSG_RESPONSE) {
            deliver_outgoing_response(s, &msgs[0]);
            cmcp_rpc_message_clear(&msgs[0]);
            free(msgs);
            continue;
        }
        if (msgs[0].kind == CMCP_MSG_REQUEST &&
            s->state == SS_READY &&
            is_pool_method(msgs[0].method)) {
            work_item_t *w = (work_item_t *)malloc(sizeof *w);
            if (!w) {
                /* Can't queue it — answer -32603 inline so the caller
                 * is not left waiting forever. */
                cmcp_rpc_message_t e;
                if (cmcp_rpc_make_error(&e, &msgs[0].id,
                        CMCP_RPC_INTERNAL_ERROR,
                        "Server out of memory", NULL) == CMCP_OK) {
                    send_message(t, &e);
                    cmcp_rpc_message_clear(&e);
                }
                cmcp_rpc_messages_free(msgs, n);
                continue;
            }
            w->server    = s;
            w->transport = t;
            w->msg       = msgs[0];   /* move: w->msg now owns the fields */
            free(msgs);               /* free the array container only   */
            w->ctx.server         = s;
            w->ctx.progress_token = meta_progress_token(w->msg.params);
            w->ctx.cancelled      = 0;
            w->ctx.structured     = NULL;
            w->ctx.is_tool_call   = 0;   /* re-set by handle_tools_call */
            /* Register before submit so a cancel racing in while the
             * request is still queued is not lost. */
            if (inflight_register(s, &w->msg.id, &w->ctx) != CMCP_OK) {
                cmcp_rpc_message_t e;
                if (cmcp_rpc_make_error(&e, &w->msg.id,
                        CMCP_RPC_INTERNAL_ERROR,
                        "Server out of memory", NULL) == CMCP_OK) {
                    send_message(t, &e);
                    cmcp_rpc_message_clear(&e);
                }
                cmcp_rpc_message_clear(&w->msg);
                free(w);
                continue;
            }
            if (cmcp_pool_submit(pool, process_work, w) != CMCP_OK) {
                /* Pool not accepting — only happens at shutdown, which
                 * can't occur mid-loop. Run inline as a safety net. */
                process_work(w);
            }
            continue;
        }

        cmcp_rpc_message_t resp;
        server_handle_message(s, &msgs[0], &resp, NULL);

        /* Only requests get replies. */
        if (msgs[0].kind == CMCP_MSG_REQUEST) {
            send_message(t, &resp);
        }
        cmcp_rpc_message_clear(&resp);
        cmcp_rpc_messages_free(msgs, n);
    }

    /* The peer is gone: flag every in-flight handler so cooperative
     * ones bail early and their undeliverable responses are dropped.
     * Also wake any worker parked in cmcp_server_send_request — its
     * response is never coming. */
    inflight_cancel_all(s);
    outgoing_cancel_all(s);

    /* Drain + join the pool BEFORE detaching the transport: a draining
     * worker may still call send_message / cmcp_server_notify, both of
     * which need the transport to still be live. */
    cmcp_pool_free(pool);

    /* Pool is joined — the in-flight table is now empty — so stop the
     * watchdog; it has nothing left to sweep. */
    watchdog_stop(&wd);

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

/* ====================================================================== */
/* Server → client requests                                                */
/* ====================================================================== */

int cmcp_server_send_request(cmcp_server_t *s,
                              cmcp_handler_ctx_t *hctx,
                              const char *method,
                              cmcp_json_t *params,
                              cmcp_rpc_message_t *out_response) {
    if (!s || !method || !out_response) {
        cmcp_json_free(params);
        return CMCP_EINVAL;
    }
    cmcp_rpc_message_init(out_response);

    pthread_mutex_lock(&s->notify_mu);
    cmcp_transport_t *t = s->active_transport;
    pthread_mutex_unlock(&s->notify_mu);
    if (!t) {
        cmcp_json_free(params);
        return CMCP_EINVAL;
    }

    pthread_mutex_lock(&s->outgoing_mu);
    long long id = ++s->outgoing_id_counter;
    pthread_mutex_unlock(&s->outgoing_mu);

    outgoing_pending_t *p = outgoing_pending_new(id);
    if (!p) { cmcp_json_free(params); return CMCP_ENOMEM; }
    outgoing_link(s, p);

    cmcp_rpc_message_t req;
    int rc = cmcp_rpc_make_request(&req, id, method, params);
    if (rc != CMCP_OK) {
        outgoing_unlink(s, p);
        outgoing_pending_free(p);
        cmcp_json_free(params);
        return rc;
    }
    rc = send_message(t, &req);
    cmcp_rpc_message_clear(&req);
    if (rc != CMCP_OK) {
        outgoing_unlink(s, p);
        outgoing_pending_free(p);
        return rc;
    }

    /* Park until the response arrives, the transport dies, or the
     * handler is cancelled. The 50ms tick is the cancellation poll
     * interval; a slow peer doesn't penalise it because the response
     * broadcast wakes us instantly. */
    int cancelled_by_handler = 0;
    pthread_mutex_lock(&p->mu);
    while (!p->done) {
        struct timespec ts;
        clock_gettime(CLOCK_REALTIME, &ts);
        ts.tv_nsec += 50L * 1000 * 1000;
        if (ts.tv_nsec >= 1000000000L) { ts.tv_sec++; ts.tv_nsec -= 1000000000L; }
        pthread_cond_timedwait(&p->cv, &p->mu, &ts);
        if (p->done) break;
        if (cmcp_handler_cancelled(hctx)) {
            cancelled_by_handler = 1;
            break;
        }
    }
    int peer_gone = p->cancelled;
    if (p->done && !p->cancelled) {
        *out_response = p->response;
        cmcp_rpc_message_init(&p->response);
    }
    pthread_mutex_unlock(&p->mu);

    outgoing_unlink(s, p);
    outgoing_pending_free(p);

    if (cancelled_by_handler) return CMCP_ECANCELLED;
    if (peer_gone)            return CMCP_EIO;
    return CMCP_OK;
}

int cmcp_handler_elicit(cmcp_handler_ctx_t *hctx,
                         const char *message,
                         cmcp_json_t *requested_schema,
                         cmcp_json_t **out_result) {
    if (!hctx || !hctx->server || !message || !out_result) {
        cmcp_json_free(requested_schema);
        return CMCP_EINVAL;
    }
    *out_result = NULL;

    cmcp_server_t *s = hctx->server;
    if (!s->peer_caps.elicitation) {
        cmcp_json_free(requested_schema);
        return CMCP_EUNSUPPORTED;
    }

    cmcp_json_t *params = cmcp_json_new_object();
    if (!params) {
        cmcp_json_free(requested_schema);
        return CMCP_ENOMEM;
    }
    cmcp_json_object_set(params, "message", cmcp_json_new_string(message));
    if (requested_schema) {
        cmcp_json_object_set(params, "requestedSchema", requested_schema);
    } else {
        cmcp_json_t *def = cmcp_json_new_object();
        cmcp_json_object_set(def, "type", cmcp_json_new_string("object"));
        cmcp_json_object_set(params, "requestedSchema", def);
    }

    cmcp_rpc_message_t resp;
    cmcp_rpc_message_init(&resp);
    int rc = cmcp_server_send_request(s, hctx, "elicitation/create",
                                       params, &resp);
    if (rc != CMCP_OK) return rc;

    if (resp.error) {
        cmcp_rpc_message_clear(&resp);
        return CMCP_EPROTOCOL;
    }
    /* Move the result out of the response shell. */
    *out_result = resp.result;
    resp.result = NULL;
    cmcp_rpc_message_clear(&resp);
    return CMCP_OK;
}

int cmcp_handler_elicit_url(cmcp_handler_ctx_t *hctx,
                             const char *message,
                             const char *url,
                             cmcp_json_t **out_result) {
    if (!hctx || !hctx->server || !message || !url || !out_result) {
        return CMCP_EINVAL;
    }
    *out_result = NULL;

    cmcp_server_t *s = hctx->server;
    if (!s->peer_caps.elicitation_url) return CMCP_EUNSUPPORTED;

    cmcp_json_t *params = cmcp_json_new_object();
    if (!params) return CMCP_ENOMEM;
    cmcp_json_object_set(params, "message", cmcp_json_new_string(message));
    cmcp_json_object_set(params, "mode",    cmcp_json_new_string("url"));
    cmcp_json_object_set(params, "url",     cmcp_json_new_string(url));

    cmcp_rpc_message_t resp;
    cmcp_rpc_message_init(&resp);
    int rc = cmcp_server_send_request(s, hctx, "elicitation/create",
                                       params, &resp);
    if (rc != CMCP_OK) return rc;

    if (resp.error) {
        cmcp_rpc_message_clear(&resp);
        return CMCP_EPROTOCOL;
    }
    *out_result = resp.result;
    resp.result = NULL;
    cmcp_rpc_message_clear(&resp);
    return CMCP_OK;
}

/* ====================================================================== */
/* Structured logging                                                       */
/* ====================================================================== */

/* Tier 6 axis 6.5.4: scrub sensitive values from outgoing wire logs.
 * Snapshotted once via pthread_once; default on, `CMCP_LOG_REDACT=0`
 * disables. Centralised so tests can reason about the toggle. */
static int g_log_redact = 1;
static pthread_once_t g_log_redact_once = PTHREAD_ONCE_INIT;
static void log_redact_init(void) {
    const char *r = getenv("CMCP_LOG_REDACT");
    if (r && *r) {
        char *end; long v = strtol(r, &end, 10);
        if (end != r && *end == '\0') g_log_redact = (v != 0);
    }
}

int cmcp_server_log(cmcp_server_t *s,
                     cmcp_log_level_t level,
                     const char *logger,
                     cmcp_json_t *data) {
    if (!s) { cmcp_json_free(data); return CMCP_EINVAL; }
    if (!s->caps.logging) { cmcp_json_free(data); return CMCP_EPROTOCOL; }
    const char *lname = cmcp_log_level_to_name(level);
    if (!lname) { cmcp_json_free(data); return CMCP_EINVAL; }

    /* Filter against the current floor. Dropping is success — callers
     * don't want a non-zero return to mean "your trace was just too
     * verbose for this peer". */
    pthread_mutex_lock(&s->log_mu);
    cmcp_log_level_t floor = s->log_min_level;
    pthread_mutex_unlock(&s->log_mu);
    if ((int)level < (int)floor) { cmcp_json_free(data); return CMCP_OK; }

    /* Scrub credential-shaped values from `data` before it leaves
     * the process. The host on the receiving end may persist or
     * forward this payload (file logs, ops pipelines) — redact at
     * the source, not the sink. */
    pthread_once(&g_log_redact_once, log_redact_init);
    if (g_log_redact && data) cmcp_json_redact(data);

    cmcp_json_t *params = cmcp_json_new_object();
    if (!params) { cmcp_json_free(data); return CMCP_ENOMEM; }
    cmcp_json_object_set(params, "level", cmcp_json_new_string(lname));
    if (logger)
        cmcp_json_object_set(params, "logger", cmcp_json_new_string(logger));
    cmcp_json_object_set(params, "data",
                          data ? data : cmcp_json_new_object());
    return cmcp_server_notify(s, "notifications/message", params);
}
