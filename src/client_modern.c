/* client_modern.c — client-side MCP 2026-07-28 rules (see client_modern.h). */
#include "client_modern.h"

#include "cmcp.h"
#include "era.h"

#include <stdlib.h>
#include <string.h>

static int set_owned(cmcp_json_t *o, const char *k, cmcp_json_t *v) {
    if (!v || cmcp_json_object_set(o, k, v) != 0) {
        cmcp_json_free(v);
        return -1;
    }
    return 0;
}

static const cmcp_json_t *field(const cmcp_json_t *o, const char *k) {
    return (o && o->type == CMCP_JSON_OBJECT) ? cmcp_json_object_get(o, k)
                                              : NULL;
}

static cmcp_json_t *client_info(const char *name, const char *version,
                                const char *description) {
    cmcp_json_t *ci = cmcp_json_new_object();
    if (!ci ||
        set_owned(ci, "name", cmcp_json_new_string(name ? name : "")) != 0 ||
        set_owned(ci, "version", cmcp_json_new_string(version ? version : "")) != 0 ||
        (description &&
         set_owned(ci, "description", cmcp_json_new_string(description)) != 0)) {
        cmcp_json_free(ci);
        return NULL;
    }
    return ci;
}

cmcp_json_t *cmcp_cmodern_meta(const char *name, const char *version,
                               const char *description, cmcp_json_t *caps,
                               const char *log_level) {
    cmcp_json_t *m = cmcp_json_new_object();
    if (!m) { cmcp_json_free(caps); return NULL; }
    int ok = set_owned(m, CMCP_META_PROTOCOL_VERSION,
                       cmcp_json_new_string(CMCP_PROTOCOL_VERSION_MODERN)) == 0;
    /* set_owned consumes caps on every path, so only call it once. */
    if (ok) ok = set_owned(m, CMCP_META_CLIENT_CAPS,
                           caps ? caps : cmcp_json_new_object()) == 0;
    else cmcp_json_free(caps);
    ok = ok && set_owned(m, CMCP_META_CLIENT_INFO,
                         client_info(name, version, description)) == 0;
    ok = ok && (!log_level ||
                set_owned(m, CMCP_META_LOG_LEVEL,
                          cmcp_json_new_string(log_level)) == 0);
    if (!ok) { cmcp_json_free(m); return NULL; }
    return m;
}

cmcp_json_t *cmcp_cmodern_attach(cmcp_json_t *params, cmcp_json_t *meta) {
    if (!meta) { cmcp_json_free(params); return NULL; }
    if (!params) params = cmcp_json_new_object();
    if (!params || params->type != CMCP_JSON_OBJECT) {
        cmcp_json_free(params);
        cmcp_json_free(meta);
        return NULL;
    }
    const cmcp_json_t *old = cmcp_json_object_get(params, "_meta");
    if (old && old->type == CMCP_JSON_OBJECT) {
        /* Keep the caller's keys (progressToken, ...) next to ours. */
        for (size_t i = 0; i < old->obj.len; i++) {
            if (cmcp_json_object_get(meta, old->obj.keys[i])) continue;
            if (set_owned(meta, old->obj.keys[i],
                          cmcp_json_clone(old->obj.values[i])) != 0) {
                cmcp_json_free(meta);
                cmcp_json_free(params);
                return NULL;
            }
        }
    }
    if (set_owned(params, "_meta", meta) != 0) {   /* replaces the old one */
        cmcp_json_free(params);
        return NULL;
    }
    return params;
}

int cmcp_cmodern_supports_mrtr(const char *method) {
    return method && (strcmp(method, "tools/call") == 0 ||
                      strcmp(method, "resources/read") == 0 ||
                      strcmp(method, "prompts/get") == 0);
}

int cmcp_cmodern_input_required(const cmcp_json_t *result) {
    const cmcp_json_t *rt = field(result, "resultType");
    return rt && rt->type == CMCP_JSON_STRING &&
           strcmp(cmcp_json_string(rt), "input_required") == 0;
}

/* Answer every entry of `requests` into a new inputResponses object. */
static int build_responses(const cmcp_json_t *requests,
                           cmcp_cmodern_answer_fn answer, void *ctx,
                           cmcp_json_t **out) {
    *out = cmcp_json_new_object();
    if (!*out) return CMCP_ENOMEM;
    for (size_t i = 0; requests && i < requests->obj.len; i++) {
        const cmcp_json_t *req = requests->obj.values[i];
        const cmcp_json_t *m = field(req, "method");
        if (!m || m->type != CMCP_JSON_STRING) {
            cmcp_json_free(*out);
            *out = NULL;
            return CMCP_EPROTOCOL;
        }
        cmcp_json_t *res = NULL;
        int rc = answer(cmcp_json_string(m), field(req, "params"), ctx, &res);
        if (rc == CMCP_OK && !res) rc = CMCP_EPROTOCOL;
        if (rc != CMCP_OK) cmcp_json_free(res);
        else if (set_owned(*out, requests->obj.keys[i], res) != 0) rc = CMCP_ENOMEM;
        if (rc != CMCP_OK) {
            cmcp_json_free(*out);
            *out = NULL;
            return rc;
        }
    }
    return CMCP_OK;
}

/* Copy of `p` without `key` (the JSON API has no object_remove). */
static cmcp_json_t *without_key(const cmcp_json_t *p, const char *key) {
    cmcp_json_t *q = cmcp_json_new_object();
    for (size_t i = 0; q && i < p->obj.len; i++) {
        if (strcmp(p->obj.keys[i], key) == 0) continue;
        if (set_owned(q, p->obj.keys[i], cmcp_json_clone(p->obj.values[i])) != 0) {
            cmcp_json_free(q);
            q = NULL;
        }
    }
    return q;
}

int cmcp_cmodern_retry_params(const cmcp_json_t *orig,
                              const cmcp_json_t *result,
                              cmcp_cmodern_answer_fn answer, void *ctx,
                              cmcp_json_t **out) {
    *out = NULL;
    const cmcp_json_t *requests = field(result, "inputRequests");
    const cmcp_json_t *state = field(result, "requestState");
    if ((requests && requests->type != CMCP_JSON_OBJECT) ||
        (state && state->type != CMCP_JSON_STRING) || (!requests && !state))
        return CMCP_EPROTOCOL;           /* the spec requires one of them */

    cmcp_json_t *responses = NULL;
    int rc = build_responses(requests, answer, ctx, &responses);
    if (rc != CMCP_OK) return rc;

    /* Start from the params as first sent: a state is echoed only when
     * this result carried one (spec MUST NOT otherwise). */
    cmcp_json_t *p = (orig && orig->type == CMCP_JSON_OBJECT)
                         ? without_key(orig, "requestState")
                         : cmcp_json_new_object();
    if (!p ||
        set_owned(p, "inputResponses", responses) != 0 ||
        (state && set_owned(p, "requestState", cmcp_json_clone(state)) != 0)) {
        if (!p) cmcp_json_free(responses);
        cmcp_json_free(p);
        return CMCP_ENOMEM;
    }
    *out = p;
    return CMCP_OK;
}

int cmcp_cmodern_discover_ok(const cmcp_json_t *result) {
    const cmcp_json_t *sv = field(result, "supportedVersions");
    if (!sv || sv->type != CMCP_JSON_ARRAY) return 0;
    for (size_t i = 0; i < cmcp_json_array_len(sv); i++) {
        const cmcp_json_t *v = cmcp_json_array_at(sv, i);
        if (v->type == CMCP_JSON_STRING &&
            strcmp(cmcp_json_string(v), CMCP_PROTOCOL_VERSION_MODERN) == 0)
            return 1;
    }
    return 0;
}
