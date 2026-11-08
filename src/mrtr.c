/* mrtr.c — Multi Round-Trip Requests for the modern server path
 * (MCP 2026-07-28 basic/patterns/mrtr). See mrtr.h. */
#include "mrtr.h"

#include "cmcp.h"
#include "era.h"
#include "seal.h"

#include <stdlib.h>
#include <string.h>
#include <time.h>

/* Requests that may answer with an InputRequiredResult (spec table
 * "Supported Requests"). Every other method MUST NOT. */
static int supports_input_required(const char *method) {
    return method && (strcmp(method, "tools/call") == 0 ||
                      strcmp(method, "resources/read") == 0 ||
                      strcmp(method, "prompts/get") == 0);
}

static const cmcp_json_t *field(const cmcp_json_t *o, const char *k) {
    return (o && o->type == CMCP_JSON_OBJECT) ? cmcp_json_object_get(o, k)
                                              : NULL;
}

static const char *str_field(const cmcp_json_t *o, const char *k) {
    const cmcp_json_t *v = field(o, k);
    return (v && v->type == CMCP_JSON_STRING) ? cmcp_json_string(v) : NULL;
}

static int bad_params(cmcp_rpc_message_t *resp, const cmcp_rpc_message_t *req,
                      const char *msg) {
    cmcp_rpc_make_error(resp, &req->id, CMCP_RPC_INVALID_PARAMS, msg, NULL);
    return CMCP_EPROTOCOL;
}

/* Verify and unseal the client's requestState, if it sent one. */
static int begin_state(cmcp_mrtr_t *m, const cmcp_rpc_message_t *req,
                       const unsigned char *key, size_t key_len,
                       cmcp_rpc_message_t *resp) {
    const cmcp_json_t *rs = field(req->params, "requestState");
    if (!rs) return CMCP_OK;
    if (rs->type != CMCP_JSON_STRING)
        return bad_params(resp, req, "requestState must be a string");
    if (!key || key_len == 0)
        return bad_params(resp, req, "Invalid requestState");

    int rc = cmcp_seal_open(key, key_len, cmcp_json_string(rs), m->method,
                            m->name, (long long)time(NULL), &m->state_in);
    if (rc == CMCP_ENOMEM) {
        cmcp_rpc_make_error(resp, &req->id, CMCP_RPC_INTERNAL_ERROR,
                            "Server out of memory", NULL);
        return CMCP_EPROTOCOL;
    }
    /* Forged, expired, or minted for another request: treat it as the
     * attacker-controlled input the spec says it is. */
    return rc == CMCP_OK ? CMCP_OK
                         : bad_params(resp, req, "Invalid requestState");
}

int cmcp_mrtr_begin(cmcp_mrtr_t *m, const cmcp_rpc_message_t *req,
                    const unsigned char *key, size_t key_len,
                    cmcp_rpc_message_t *resp) {
    memset(m, 0, sizeof *m);
    m->modern = 1;
    m->log_level = -1;
    m->method = req->method;
    m->name = str_field(req->params, "name");
    if (!m->name) m->name = str_field(req->params, "uri");

    const cmcp_json_t *meta = field(req->params, "_meta");
    cmcp_era_parse_client_caps(field(meta, CMCP_META_CLIENT_CAPS), &m->caps);

    const cmcp_json_t *lv = field(meta, CMCP_META_LOG_LEVEL);
    if (lv) {
        cmcp_log_level_t parsed;
        if (lv->type != CMCP_JSON_STRING ||
            cmcp_log_level_from_name(cmcp_json_string(lv), &parsed) != 0)
            return bad_params(resp, req, "Invalid logLevel");
        m->log_level = (int)parsed;
    }

    const cmcp_json_t *ir = field(req->params, "inputResponses");
    if (ir && ir->type != CMCP_JSON_OBJECT)
        return bad_params(resp, req, "inputResponses must be an object");
    m->input_responses = ir;

    return begin_state(m, req, key, key_len, resp);
}

/* {"<cap>": {}} or {"<cap>": {"<sub>": {}}} */
static cmcp_json_t *cap_shape(const char *cap, const char *sub) {
    cmcp_json_t *root = cmcp_json_new_object();
    cmcp_json_t *inner = cmcp_json_new_object();
    cmcp_json_t *leaf = sub ? cmcp_json_new_object() : NULL;
    if (!root || !inner || (sub && !leaf)) goto fail;
    if (sub) {
        if (cmcp_json_object_set(inner, sub, leaf) != 0) goto fail;
        leaf = NULL;
    }
    if (cmcp_json_object_set(root, cap, inner) != 0) goto fail;
    return root;
fail:
    cmcp_json_free(leaf);
    cmcp_json_free(inner);
    cmcp_json_free(root);
    return NULL;
}

cmcp_json_t *cmcp_mrtr_missing_cap(const cmcp_client_capabilities_t *caps,
                                   const char *method,
                                   const cmcp_json_t *params, int *known) {
    *known = 1;
    if (method && strcmp(method, "elicitation/create") == 0) {
        const char *mode = str_field(params, "mode");
        if (mode && strcmp(mode, "url") == 0)
            return caps->elicitation_url ? NULL : cap_shape("elicitation", "url");
        return caps->elicitation ? NULL : cap_shape("elicitation", NULL);
    }
    if (method && strcmp(method, "sampling/createMessage") == 0)
        return caps->sampling ? NULL : cap_shape("sampling", NULL);
    if (method && strcmp(method, "roots/list") == 0)
        return caps->roots ? NULL : cap_shape("roots", NULL);
    *known = 0;
    return NULL;
}

/* Fold one missing capability into m->missing_caps (consumes `need`). */
static void note_missing(cmcp_mrtr_t *m, cmcp_json_t *need) {
    if (!need) return;
    if (!m->missing_caps) { m->missing_caps = need; return; }
    /* need has exactly one key; move it across. */
    static const char *const keys[] = {"elicitation", "sampling", "roots"};
    for (size_t i = 0; i < sizeof keys / sizeof keys[0]; i++) {
        const cmcp_json_t *v = cmcp_json_object_get(need, keys[i]);
        cmcp_json_t *copy = v ? cmcp_json_clone(v) : NULL;
        if (copy && cmcp_json_object_set(m->missing_caps, keys[i], copy) != 0)
            cmcp_json_free(copy);
    }
    cmcp_json_free(need);
}

/* Append {key: {method, params}} to the queued input requests. */
static int queue_request(cmcp_mrtr_t *m, const char *key, const char *method,
                         cmcp_json_t *params) {
    if (!m->input_requests) {
        m->input_requests = cmcp_json_new_object();
        if (!m->input_requests) { cmcp_json_free(params); return CMCP_ENOMEM; }
    }
    if (cmcp_json_object_get(m->input_requests, key)) {
        cmcp_json_free(params);         /* keys MUST be unique per request */
        return CMCP_EINVAL;
    }
    cmcp_json_t *req = cmcp_json_new_object();
    cmcp_json_t *mth = cmcp_json_new_string(method);
    if (!params) params = cmcp_json_new_object();
    if (!req || !mth || !params || cmcp_json_object_set(req, "method", mth) != 0) {
        cmcp_json_free(req); cmcp_json_free(mth); cmcp_json_free(params);
        return CMCP_ENOMEM;
    }
    if (cmcp_json_object_set(req, "params", params) != 0) {
        cmcp_json_free(req); cmcp_json_free(params);
        return CMCP_ENOMEM;
    }
    if (cmcp_json_object_set(m->input_requests, key, req) != 0) {
        cmcp_json_free(req);
        return CMCP_ENOMEM;
    }
    return CMCP_EINPUT_REQUIRED;
}

int cmcp_mrtr_input(cmcp_mrtr_t *m, const char *key, const char *method,
                    cmcp_json_t *params, cmcp_json_t **out_result) {
    *out_result = NULL;
    if (!key || !*key || !method) { cmcp_json_free(params); return CMCP_EINVAL; }

    int known = 0;
    cmcp_json_t *need = cmcp_mrtr_missing_cap(&m->caps, method, params, &known);
    if (!known) { cmcp_json_free(params); return CMCP_EINVAL; }
    if (need) {
        /* Servers MUST NOT send input requests the client did not
         * declare; remember why, so a failing handler maps to -32021. */
        note_missing(m, need);
        cmcp_json_free(params);
        return CMCP_EUNSUPPORTED;
    }

    const cmcp_json_t *answer = field(m->input_responses, key);
    if (answer) {
        cmcp_json_free(params);
        if (answer->type != CMCP_JSON_OBJECT) {
            m->bad_response = 1;
            return CMCP_EPROTOCOL;
        }
        *out_result = cmcp_json_clone(answer);
        return *out_result ? CMCP_OK : CMCP_ENOMEM;
    }
    return queue_request(m, key, method, params);
}

int cmcp_mrtr_set_state(cmcp_mrtr_t *m, const char *state) {
    if (!m->modern) return CMCP_EUNSUPPORTED;
    char *copy = NULL;
    if (state) {
        size_t n = strlen(state);
        copy = (char *)malloc(n + 1);
        if (!copy) return CMCP_ENOMEM;
        memcpy(copy, state, n + 1);
    }
    free(m->state_out);
    m->state_out = copy;
    return CMCP_OK;
}

/* Seal m->state_out into r.requestState. */
static int attach_state(cmcp_mrtr_t *m, cmcp_json_t *r,
                        const unsigned char *key, size_t key_len) {
    char *tok = NULL;
    long long expiry = (long long)time(NULL) + CMCP_MRTR_STATE_TTL_S;
    cmcp_json_t *v = NULL;
    if (cmcp_seal_make(key, key_len, m->method, m->name, m->state_out,
                       expiry, &tok) == CMCP_OK)
        v = cmcp_json_new_string(tok);
    free(tok);
    if (!v || cmcp_json_object_set(r, "requestState", v) != 0) {
        cmcp_json_free(v);
        return CMCP_ENOMEM;
    }
    return CMCP_OK;
}

/* Build {resultType:"input_required", inputRequests?, requestState?}. */
static cmcp_json_t *input_required_result(cmcp_mrtr_t *m,
                                          const unsigned char *key,
                                          size_t key_len) {
    cmcp_json_t *r = cmcp_json_new_object();
    cmcp_json_t *rt = cmcp_json_new_string("input_required");
    if (!r || !rt || cmcp_json_object_set(r, "resultType", rt) != 0) {
        cmcp_json_free(r); cmcp_json_free(rt);
        return NULL;
    }
    if (m->input_requests) {
        if (cmcp_json_object_set(r, "inputRequests", m->input_requests) != 0) {
            cmcp_json_free(r);
            return NULL;
        }
        m->input_requests = NULL;          /* moved */
    }
    if (m->state_out && attach_state(m, r, key, key_len) != CMCP_OK) {
        cmcp_json_free(r);
        return NULL;
    }
    return r;
}

static void replace_with_error(cmcp_rpc_message_t *resp,
                               const cmcp_rpc_message_t *req, int code,
                               const char *msg, cmcp_json_t *data) {
    cmcp_rpc_message_clear(resp);
    cmcp_rpc_make_error(resp, &req->id, code, msg, data);
}

/* The handler failed because the client lacks a capability: -32021
 * with data.requiredCapabilities (spec: MissingRequiredClientCapability). */
static void missing_cap_error(cmcp_mrtr_t *m, const cmcp_rpc_message_t *req,
                              cmcp_rpc_message_t *resp) {
    cmcp_json_t *data = cmcp_json_new_object();
    if (!data || cmcp_json_object_set(data, "requiredCapabilities",
                                      m->missing_caps) != 0) {
        cmcp_json_free(data);
        return;                    /* keep the handler's own error */
    }
    m->missing_caps = NULL;        /* moved */
    replace_with_error(resp, req, CMCP_RPC_MISSING_CLIENT_CAPABILITY,
                       "Client lacks a capability this request requires",
                       data);
}

void cmcp_mrtr_finish(cmcp_mrtr_t *m, const cmcp_rpc_message_t *req,
                      const unsigned char *key, size_t key_len,
                      cmcp_rpc_message_t *resp) {
    if (!m->modern || !supports_input_required(req->method)) return;

    if (m->bad_response) {
        replace_with_error(resp, req, CMCP_RPC_INVALID_PARAMS,
                           "Invalid inputResponses entry", NULL);
        return;
    }
    if (m->input_requests || m->state_out) {
        cmcp_json_t *r = input_required_result(m, key, key_len);
        cmcp_rpc_message_clear(resp);
        if (r) {
            cmcp_rpc_make_response(resp, &req->id, r);
        } else {
            cmcp_rpc_make_error(resp, &req->id, CMCP_RPC_INTERNAL_ERROR,
                                "Server out of memory", NULL);
        }
        return;
    }
    if (m->missing_caps && resp->error) missing_cap_error(m, req, resp);
}

void cmcp_mrtr_clear(cmcp_mrtr_t *m) {
    free(m->state_in);
    free(m->state_out);
    cmcp_json_free(m->input_requests);
    cmcp_json_free(m->missing_caps);
    memset(m, 0, sizeof *m);
}
