/* cmcp_session_t — multi-server tool aggregator.
 *
 * A session holds N already-handshaken clients under host-supplied
 * server names. tools/list is fanned out to every client in parallel
 * (call_async + wait), so a single slow server doesn't stall the
 * whole catalog. tools/call is dispatched by parsing the qualified
 * "<server>:<tool>" name.
 *
 * Ownership: cmcp_session_add takes ownership of the client. Freeing
 * the session frees all clients, which closes their transports and
 * reaps spawned children.
 */

#include "cmcp.h"
#include "cmcp_client.h"
#include "cmcp_json.h"
#include "cmcp_session.h"
#include "cmcp_types.h"

#include <stdlib.h>
#include <string.h>

/* ====================================================================== */
/* Session struct                                                          */
/* ====================================================================== */

typedef struct {
    char           *server_name;
    cmcp_client_t  *client;
} entry_t;

struct cmcp_session {
    entry_t *entries;
    size_t   len;
    size_t   cap;
};

/* ====================================================================== */
/* Helpers                                                                 */
/* ====================================================================== */

static char *xstrdup(const char *s) {
    if (!s) return NULL;
    size_t n = strlen(s);
    char *p = (char *)malloc(n + 1);
    if (!p) return NULL;
    memcpy(p, s, n + 1);
    return p;
}

static int grow_entries(cmcp_session_t *s) {
    size_t new_cap = s->cap ? s->cap * 2 : 4;
    entry_t *new_entries = (entry_t *)realloc(s->entries,
                                                new_cap * sizeof *new_entries);
    if (!new_entries) return CMCP_ENOMEM;
    s->entries = new_entries;
    s->cap = new_cap;
    return CMCP_OK;
}

static entry_t *find_by_name(cmcp_session_t *s, const char *server_name) {
    for (size_t i = 0; i < s->len; i++) {
        if (strcmp(s->entries[i].server_name, server_name) == 0) {
            return &s->entries[i];
        }
    }
    return NULL;
}

static char *qualified_name(const char *server, const char *tool) {
    size_t sn = strlen(server), tn = strlen(tool);
    char *out = (char *)malloc(sn + tn + 2);
    if (!out) return NULL;
    memcpy(out, server, sn);
    out[sn] = ':';
    memcpy(out + sn + 1, tool, tn);
    out[sn + 1 + tn] = '\0';
    return out;
}

/* ====================================================================== */
/* Lifecycle                                                               */
/* ====================================================================== */

cmcp_session_t *cmcp_session_new(void) {
    cmcp_session_t *s = (cmcp_session_t *)calloc(1, sizeof *s);
    return s;
}

void cmcp_session_free(cmcp_session_t *s) {
    if (!s) return;
    for (size_t i = 0; i < s->len; i++) {
        free(s->entries[i].server_name);
        cmcp_client_free(s->entries[i].client);
    }
    free(s->entries);
    free(s);
}

int cmcp_session_add(cmcp_session_t *s,
                      const char *server_name,
                      cmcp_client_t *c) {
    if (!s || !server_name || !c) return CMCP_EINVAL;
    if (server_name[0] == '\0' || strchr(server_name, ':')) return CMCP_EINVAL;
    if (find_by_name(s, server_name)) return CMCP_EPROTOCOL;

    if (s->len == s->cap) {
        int rc = grow_entries(s);
        if (rc != CMCP_OK) return rc;
    }
    char *dup = xstrdup(server_name);
    if (!dup) return CMCP_ENOMEM;
    s->entries[s->len].server_name = dup;
    s->entries[s->len].client      = c;
    s->len++;
    return CMCP_OK;
}

cmcp_client_t *cmcp_session_get(cmcp_session_t *s, const char *server_name) {
    if (!s || !server_name) return NULL;
    entry_t *e = find_by_name(s, server_name);
    return e ? e->client : NULL;
}

size_t cmcp_session_count(const cmcp_session_t *s) {
    return s ? s->len : 0;
}

/* ====================================================================== */
/* Aggregated tools/list                                                   */
/* ====================================================================== */

static void absorb_one_tools_list(const char *server_name,
                                   const cmcp_rpc_message_t *resp,
                                   cmcp_session_tool_t **out_arr,
                                   size_t *out_len, size_t *out_cap) {
    if (!resp || !resp->result || resp->result->type != CMCP_JSON_OBJECT) return;
    const cmcp_json_t *tools = cmcp_json_object_get(resp->result, "tools");
    if (!tools || tools->type != CMCP_JSON_ARRAY) return;

    for (size_t i = 0; i < tools->arr.len; i++) {
        const cmcp_json_t *t = tools->arr.items[i];
        if (!t || t->type != CMCP_JSON_OBJECT) continue;
        const cmcp_json_t *name = cmcp_json_object_get(t, "name");
        if (!name || name->type != CMCP_JSON_STRING) continue;
        const cmcp_json_t *desc = cmcp_json_object_get(t, "description");
        const cmcp_json_t *sch  = cmcp_json_object_get(t, "inputSchema");

        if (*out_len == *out_cap) {
            size_t new_cap = *out_cap ? *out_cap * 2 : 8;
            cmcp_session_tool_t *na = (cmcp_session_tool_t *)realloc(*out_arr,
                                       new_cap * sizeof *na);
            if (!na) return;
            *out_arr = na;
            *out_cap = new_cap;
        }
        cmcp_session_tool_t *e = &(*out_arr)[*out_len];
        memset(e, 0, sizeof *e);
        e->server      = xstrdup(server_name);
        e->name        = xstrdup(name->str.s);
        e->qualified   = qualified_name(server_name, name->str.s);
        e->description = (desc && desc->type == CMCP_JSON_STRING)
                            ? xstrdup(desc->str.s) : NULL;
        e->input_schema = sch ? cmcp_json_clone(sch) : NULL;
        if (!e->server || !e->name || !e->qualified) {
            free(e->server); free(e->name); free(e->qualified);
            free(e->description); cmcp_json_free(e->input_schema);
            continue;
        }
        (*out_len)++;
    }
}

int cmcp_session_tools_list(cmcp_session_t *s,
                             cmcp_session_tool_t **out_tools,
                             size_t *out_n) {
    if (!s || !out_tools || !out_n) return CMCP_EINVAL;
    *out_tools = NULL;
    *out_n = 0;
    if (s->len == 0) return CMCP_OK;

    /* Fan out: one async call per server, all flying simultaneously. */
    long long *ids = (long long *)calloc(s->len, sizeof *ids);
    int       *ok  = (int *)calloc(s->len, sizeof *ok);
    if (!ids || !ok) { free(ids); free(ok); return CMCP_ENOMEM; }

    for (size_t i = 0; i < s->len; i++) {
        int rc = cmcp_client_call_async(s->entries[i].client,
                                          "tools/list", NULL, &ids[i]);
        ok[i] = (rc == CMCP_OK);
    }

    /* Fan in. */
    cmcp_session_tool_t *arr = NULL;
    size_t arr_len = 0, arr_cap = 0;

    for (size_t i = 0; i < s->len; i++) {
        if (!ok[i]) continue;
        cmcp_rpc_message_t resp;
        cmcp_rpc_message_init(&resp);
        int wr = cmcp_client_wait(s->entries[i].client, ids[i], &resp);
        if (wr != CMCP_OK) continue;
        if (!resp.error) {
            absorb_one_tools_list(s->entries[i].server_name, &resp,
                                    &arr, &arr_len, &arr_cap);
        }
        cmcp_rpc_message_clear(&resp);
    }

    free(ids); free(ok);
    *out_tools = arr;
    *out_n     = arr_len;
    return CMCP_OK;
}

void cmcp_session_tools_free(cmcp_session_tool_t *tools, size_t n) {
    if (!tools) return;
    for (size_t i = 0; i < n; i++) {
        free(tools[i].server);
        free(tools[i].name);
        free(tools[i].qualified);
        free(tools[i].description);
        cmcp_json_free(tools[i].input_schema);
    }
    free(tools);
}

/* ====================================================================== */
/* Routed tools/call                                                       */
/* ====================================================================== */

int cmcp_session_tool_call(cmcp_session_t *s,
                            const char *qualified,
                            cmcp_json_t *args,
                            cmcp_rpc_message_t *out_response) {
    if (!s || !qualified || !out_response) {
        cmcp_json_free(args);
        return CMCP_EINVAL;
    }
    const char *colon = strchr(qualified, ':');
    if (!colon || colon == qualified || colon[1] == '\0') {
        cmcp_json_free(args);
        return CMCP_EINVAL;
    }

    size_t srv_len = (size_t)(colon - qualified);
    char srv[256];
    if (srv_len >= sizeof srv) { cmcp_json_free(args); return CMCP_EINVAL; }
    memcpy(srv, qualified, srv_len);
    srv[srv_len] = '\0';

    entry_t *e = find_by_name(s, srv);
    if (!e) { cmcp_json_free(args); return CMCP_ENOTFOUND; }

    /* Build the tools/call params: {"name":<tool>, "arguments":<args>}. */
    cmcp_json_t *params = cmcp_json_new_object();
    if (!params) { cmcp_json_free(args); return CMCP_ENOMEM; }
    cmcp_json_object_set(params, "name", cmcp_json_new_string(colon + 1));
    if (args) cmcp_json_object_set(params, "arguments", args);

    return cmcp_client_request(e->client, "tools/call", params, out_response);
}
