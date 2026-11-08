/* era.c — the modern (2026-07-28) request rules. See era.h. */

#include "era.h"
#include "cmcp.h"

#include <stddef.h>
#include <string.h>

/* Cache hints for cacheable modern results (spec: server/utilities/
 * caching). A ttl of 0 means "immediately stale" — always valid and
 * never wrong, so it is the safe default until hosts can configure it.
 * "private" keeps shared intermediaries from caching per-caller data. */
#define CACHE_TTL_MS_DEFAULT 0
#define CACHE_SCOPE_DEFAULT  "private"

/* Methods the server answers on the modern path. */
static const char *const MODERN_METHODS[] = {
    "server/discover",
    "tools/list",     "tools/call",
    "resources/list", "resources/read",
    "prompts/list",   "prompts/get",
};

/* Results the spec marks cacheable (CacheableResult). */
static const char *const CACHEABLE_METHODS[] = {
    "server/discover", "tools/list", "prompts/list",
    "resources/list",  "resources/read", "resources/templates/list",
};

#define COUNT(a) (sizeof(a) / sizeof((a)[0]))

static int in_list(const char *s, const char *const *list, size_t n) {
    if (!s) return 0;
    for (size_t i = 0; i < n; i++) {
        if (strcmp(s, list[i]) == 0) return 1;
    }
    return 0;
}

static const cmcp_json_t *meta_of(const cmcp_json_t *params) {
    if (!params || params->type != CMCP_JSON_OBJECT) return NULL;
    const cmcp_json_t *m = cmcp_json_object_get(params, "_meta");
    return (m && m->type == CMCP_JSON_OBJECT) ? m : NULL;
}

/* object_set / array_append leave `v` with the caller on failure; these
 * wrappers free it so every call site stays leak-free in one line. */
static int set_owned(cmcp_json_t *obj, const char *key, cmcp_json_t *v) {
    if (!v) return CMCP_ENOMEM;
    if (cmcp_json_object_set(obj, key, v) != 0) {
        cmcp_json_free(v);
        return CMCP_ENOMEM;
    }
    return CMCP_OK;
}

static int append_owned(cmcp_json_t *arr, cmcp_json_t *v) {
    if (!v) return CMCP_ENOMEM;
    if (cmcp_json_array_append(arr, v) != 0) {
        cmcp_json_free(v);
        return CMCP_ENOMEM;
    }
    return CMCP_OK;
}

int cmcp_era_is_modern(const cmcp_json_t *params) {
    const cmcp_json_t *m = meta_of(params);
    return m && (cmcp_json_object_get(m, CMCP_META_PROTOCOL_VERSION) ||
                 cmcp_json_object_get(m, CMCP_META_CLIENT_CAPS));
}

int cmcp_era_modern_method_allowed(const char *method) {
    return in_list(method, MODERN_METHODS, COUNT(MODERN_METHODS));
}

cmcp_json_t *cmcp_era_supported_versions(void) {
    cmcp_json_t *arr = cmcp_json_new_array();
    if (!arr) return NULL;
    if (append_owned(arr, cmcp_json_new_string(CMCP_PROTOCOL_VERSION_MODERN)) ||
        append_owned(arr, cmcp_json_new_string(CMCP_PROTOCOL_VERSION))) {
        cmcp_json_free(arr);
        return NULL;
    }
    return arr;
}

cmcp_json_t *cmcp_era_discover_result(cmcp_json_t *caps) {
    cmcp_json_t *result = cmcp_json_new_object();
    if (!result) {
        cmcp_json_free(caps);
        return NULL;
    }
    /* Two steps, not `a || b`: if the first set fails, `caps` has not
     * been handed over yet and must still be freed here. */
    if (set_owned(result, "supportedVersions", cmcp_era_supported_versions())) {
        cmcp_json_free(caps);
        cmcp_json_free(result);
        return NULL;
    }
    if (set_owned(result, "capabilities", caps)) {
        cmcp_json_free(result);
        return NULL;
    }
    return result;
}

/* Fill *resp with a -32022 error naming what we serve and what was
 * asked. Falls back to an error without data if building it fails, so
 * the client still gets the right code. */
static void reject_version(const cmcp_rpc_message_t *req,
                           cmcp_rpc_message_t *resp, const char *requested) {
    cmcp_json_t *data = cmcp_json_new_object();
    if (data &&
        (set_owned(data, "supported", cmcp_era_supported_versions()) ||
         set_owned(data, "requested", cmcp_json_new_string(requested)))) {
        cmcp_json_free(data);
        data = NULL;
    }
    cmcp_rpc_make_error(resp, &req->id, CMCP_RPC_UNSUPPORTED_PROTOCOL_VERSION,
                        "Unsupported protocol version", data);
}

int cmcp_era_check_request(const cmcp_rpc_message_t *req,
                           cmcp_rpc_message_t *resp) {
    const cmcp_json_t *m = meta_of(req->params);
    const cmcp_json_t *pv =
        m ? cmcp_json_object_get(m, CMCP_META_PROTOCOL_VERSION) : NULL;
    const cmcp_json_t *caps =
        m ? cmcp_json_object_get(m, CMCP_META_CLIENT_CAPS) : NULL;

    if (!pv || pv->type != CMCP_JSON_STRING) {
        cmcp_rpc_make_error(resp, &req->id, CMCP_RPC_INVALID_PARAMS,
            "Missing or invalid _meta[\"" CMCP_META_PROTOCOL_VERSION
            "\"] (string)", NULL);
        return CMCP_EPROTOCOL;
    }
    if (!caps || caps->type != CMCP_JSON_OBJECT) {
        cmcp_rpc_make_error(resp, &req->id, CMCP_RPC_INVALID_PARAMS,
            "Missing or invalid _meta[\"" CMCP_META_CLIENT_CAPS
            "\"] (object)", NULL);
        return CMCP_EPROTOCOL;
    }
    /* The legacy revision is listed in `supported` but is only served
     * through `initialize`, so in modern _meta it is unsupported too. */
    if (strcmp(cmcp_json_string(pv), CMCP_PROTOCOL_VERSION_MODERN) != 0) {
        reject_version(req, resp, cmcp_json_string(pv));
        return CMCP_EPROTOCOL;
    }
    return CMCP_OK;
}

/* Rebuild `_meta` with serverInfo added, keeping any keys a handler
 * already put there. cmcp_json_object_get only lends a const view, so
 * the existing object is cloned, extended and swapped in. */
static int add_server_info(cmcp_json_t *result, const char *name,
                           const char *version) {
    const cmcp_json_t *old = cmcp_json_object_get(result, "_meta");
    cmcp_json_t *meta = (old && old->type == CMCP_JSON_OBJECT)
                            ? cmcp_json_clone(old)
                            : cmcp_json_new_object();
    cmcp_json_t *info = cmcp_json_new_object();
    if (!meta || !info ||
        set_owned(info, "name", cmcp_json_new_string(name ? name : "")) ||
        set_owned(info, "version",
                  cmcp_json_new_string(version ? version : ""))) {
        cmcp_json_free(meta);
        cmcp_json_free(info);
        return CMCP_ENOMEM;
    }
    if (set_owned(meta, CMCP_META_SERVER_INFO, info)) {
        cmcp_json_free(meta);
        return CMCP_ENOMEM;
    }
    return set_owned(result, "_meta", meta);
}

int cmcp_era_finish_result(cmcp_json_t *result, const char *method,
                           const char *server_name,
                           const char *server_version) {
    if (!result || result->type != CMCP_JSON_OBJECT) return CMCP_EINVAL;

    if (!cmcp_json_object_get(result, "resultType") &&
        set_owned(result, "resultType", cmcp_json_new_string("complete")))
        return CMCP_ENOMEM;

    if (add_server_info(result, server_name, server_version) != CMCP_OK)
        return CMCP_ENOMEM;

    /* Interim input_required results are never cacheable and carry no
     * hints (spec: server/utilities/caching). */
    const cmcp_json_t *rt = cmcp_json_object_get(result, "resultType");
    if (rt && rt->type == CMCP_JSON_STRING &&
        strcmp(cmcp_json_string(rt), "input_required") == 0)
        return CMCP_OK;
    if (!in_list(method, CACHEABLE_METHODS, COUNT(CACHEABLE_METHODS)))
        return CMCP_OK;
    if (!cmcp_json_object_get(result, "ttlMs") &&
        set_owned(result, "ttlMs", cmcp_json_new_int(CACHE_TTL_MS_DEFAULT)))
        return CMCP_ENOMEM;
    if (!cmcp_json_object_get(result, "cacheScope") &&
        set_owned(result, "cacheScope",
                  cmcp_json_new_string(CACHE_SCOPE_DEFAULT)))
        return CMCP_ENOMEM;
    return CMCP_OK;
}

/* Moved from server.c so the modern path can parse per-request
 * capabilities with the same rules as the legacy handshake. */
void cmcp_era_parse_client_caps(const cmcp_json_t *o,
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
        out->roots = 1;
        const cmcp_json_t *lc = cmcp_json_object_get(roots, "listChanged");
        if (lc && lc->type == CMCP_JSON_BOOL && lc->b) {
            out->roots_list_changed = 1;
        }
    }
}
