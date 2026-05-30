#include "cmcp.h"
#include "cmcp_json.h"
#include "cmcp_types.h"

#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ====================================================================== */
/* Small allocation helpers                                                */
/* ====================================================================== */

static char *dup_n(const char *s, size_t n) {
    char *out = (char *)malloc(n + 1);
    if (!out) return NULL;
    memcpy(out, s, n);
    out[n] = '\0';
    return out;
}

static char *dup_cstr(const char *s) {
    return dup_n(s, strlen(s));
}

/* ====================================================================== */
/* cmcp_id_t                                                               */
/* ====================================================================== */

void cmcp_id_init_none(cmcp_id_t *id) {
    if (!id) return;
    id->kind = CMCP_ID_NONE;
    id->i = 0;
    id->s = NULL;
    id->s_len = 0;
}

void cmcp_id_init_null(cmcp_id_t *id) {
    if (!id) return;
    cmcp_id_init_none(id);
    id->kind = CMCP_ID_NULL;
}

void cmcp_id_init_int(cmcp_id_t *id, long long i) {
    if (!id) return;
    cmcp_id_init_none(id);
    id->kind = CMCP_ID_INT;
    id->i = i;
}

int cmcp_id_init_string(cmcp_id_t *id, const char *s, size_t n) {
    if (!id || !s) return CMCP_EINVAL;
    cmcp_id_init_none(id);
    char *copy = dup_n(s, n);
    if (!copy) return CMCP_ENOMEM;
    id->kind = CMCP_ID_STRING;
    id->s = copy;
    id->s_len = n;
    return CMCP_OK;
}

int cmcp_id_copy(cmcp_id_t *dst, const cmcp_id_t *src) {
    if (!dst || !src) return CMCP_EINVAL;
    cmcp_id_clear(dst);
    switch (src->kind) {
    case CMCP_ID_NONE:   cmcp_id_init_none(dst);                 return CMCP_OK;
    case CMCP_ID_NULL:   cmcp_id_init_null(dst);                 return CMCP_OK;
    case CMCP_ID_INT:    cmcp_id_init_int(dst, src->i);          return CMCP_OK;
    case CMCP_ID_STRING: return cmcp_id_init_string(dst, src->s, src->s_len);
    }
    return CMCP_EINVAL;
}

void cmcp_id_clear(cmcp_id_t *id) {
    if (!id) return;
    if (id->kind == CMCP_ID_STRING) free(id->s);
    cmcp_id_init_none(id);
}

int cmcp_id_equal(const cmcp_id_t *a, const cmcp_id_t *b) {
    if (!a || !b) return 0;
    if (a->kind != b->kind) return 0;
    switch (a->kind) {
    case CMCP_ID_NONE:
    case CMCP_ID_NULL:   return 1;
    case CMCP_ID_INT:    return a->i == b->i;
    case CMCP_ID_STRING: return a->s_len == b->s_len &&
                                memcmp(a->s, b->s, a->s_len) == 0;
    }
    return 0;
}

/* ====================================================================== */
/* cmcp_rpc_error_t                                                        */
/* ====================================================================== */

static cmcp_rpc_error_t *rpc_error_new(int code, const char *message,
                                       cmcp_json_t *data) {
    cmcp_rpc_error_t *e = (cmcp_rpc_error_t *)calloc(1, sizeof *e);
    if (!e) return NULL;
    e->code = code;
    if (message) {
        e->message = dup_cstr(message);
        if (!e->message) { free(e); return NULL; }
    }
    e->data = data;
    return e;
}

void cmcp_rpc_error_free(cmcp_rpc_error_t *e) {
    if (!e) return;
    free(e->message);
    cmcp_json_free(e->data);
    free(e);
}

/* ====================================================================== */
/* cmcp_rpc_message_t                                                      */
/* ====================================================================== */

void cmcp_rpc_message_init(cmcp_rpc_message_t *m) {
    if (!m) return;
    m->kind = CMCP_MSG_REQUEST;
    cmcp_id_init_none(&m->id);
    m->method = NULL;
    m->params = NULL;
    m->result = NULL;
    m->error  = NULL;
}

void cmcp_rpc_message_clear(cmcp_rpc_message_t *m) {
    if (!m) return;
    cmcp_id_clear(&m->id);
    free(m->method);          m->method = NULL;
    cmcp_json_free(m->params); m->params = NULL;
    cmcp_json_free(m->result); m->result = NULL;
    cmcp_rpc_error_free(m->error);  m->error  = NULL;
}

/* ====================================================================== */
/* JSON-RPC encoding                                                       */
/* ====================================================================== */

static cmcp_json_t *id_to_json(const cmcp_id_t *id) {
    switch (id->kind) {
    case CMCP_ID_NULL:   return cmcp_json_new_null();
    case CMCP_ID_INT:    return cmcp_json_new_int(id->i);
    case CMCP_ID_STRING: return cmcp_json_new_string_n(id->s, id->s_len);
    case CMCP_ID_NONE:   return NULL;
    }
    return NULL;
}

static cmcp_json_t *error_to_json(const cmcp_rpc_error_t *e) {
    cmcp_json_t *o = cmcp_json_new_object();
    if (!o) return NULL;
    cmcp_json_t *code = cmcp_json_new_int(e->code);
    cmcp_json_t *msg  = cmcp_json_new_string(e->message ? e->message : "");
    if (!code || !msg ||
        cmcp_json_object_set(o, "code", code) != CMCP_OK ||
        cmcp_json_object_set(o, "message", msg) != CMCP_OK) {
        cmcp_json_free(code); cmcp_json_free(msg); cmcp_json_free(o);
        return NULL;
    }
    if (e->data) {
        cmcp_json_t *data = cmcp_json_clone(e->data);
        if (!data || cmcp_json_object_set(o, "data", data) != CMCP_OK) {
            cmcp_json_free(data); cmcp_json_free(o);
            return NULL;
        }
    }
    return o;
}

cmcp_json_t *cmcp_rpc_to_json(const cmcp_rpc_message_t *m) {
    if (!m) return NULL;
    cmcp_json_t *o = cmcp_json_new_object();
    if (!o) return NULL;

    cmcp_json_t *jver = cmcp_json_new_string("2.0");
    if (!jver || cmcp_json_object_set(o, "jsonrpc", jver) != CMCP_OK) {
        cmcp_json_free(jver); cmcp_json_free(o);
        return NULL;
    }

    if (m->kind == CMCP_MSG_REQUEST || m->kind == CMCP_MSG_RESPONSE) {
        cmcp_json_t *jid = id_to_json(&m->id);
        /* requests must have a non-NONE id; responses too (NULL allowed) */
        if (!jid) {
            if (m->kind == CMCP_MSG_RESPONSE && m->id.kind == CMCP_ID_NONE) {
                /* tolerate by emitting null */
                jid = cmcp_json_new_null();
            }
        }
        if (jid && cmcp_json_object_set(o, "id", jid) != CMCP_OK) {
            cmcp_json_free(jid); cmcp_json_free(o);
            return NULL;
        }
    }

    if (m->kind == CMCP_MSG_REQUEST || m->kind == CMCP_MSG_NOTIFICATION) {
        cmcp_json_t *jmethod = cmcp_json_new_string(m->method ? m->method : "");
        if (!jmethod || cmcp_json_object_set(o, "method", jmethod) != CMCP_OK) {
            cmcp_json_free(jmethod); cmcp_json_free(o);
            return NULL;
        }
        if (m->params) {
            cmcp_json_t *p = cmcp_json_clone(m->params);
            if (!p || cmcp_json_object_set(o, "params", p) != CMCP_OK) {
                cmcp_json_free(p); cmcp_json_free(o);
                return NULL;
            }
        }
    }

    if (m->kind == CMCP_MSG_RESPONSE) {
        if (m->error) {
            cmcp_json_t *je = error_to_json(m->error);
            if (!je || cmcp_json_object_set(o, "error", je) != CMCP_OK) {
                cmcp_json_free(je); cmcp_json_free(o);
                return NULL;
            }
        } else {
            /* success response: result key required, even if value is null */
            cmcp_json_t *r = m->result ? cmcp_json_clone(m->result)
                                       : cmcp_json_new_null();
            if (!r || cmcp_json_object_set(o, "result", r) != CMCP_OK) {
                cmcp_json_free(r); cmcp_json_free(o);
                return NULL;
            }
        }
    }

    return o;
}

char *cmcp_rpc_emit(const cmcp_rpc_message_t *m) {
    cmcp_json_t *j = cmcp_rpc_to_json(m);
    if (!j) return NULL;
    char *s = cmcp_json_emit_stable(j);
    cmcp_json_free(j);
    return s;
}

char *cmcp_rpc_emit_batch(const cmcp_rpc_message_t *msgs, size_t count) {
    if (!msgs && count > 0) return NULL;
    cmcp_json_t *arr = cmcp_json_new_array();
    if (!arr) return NULL;
    for (size_t i = 0; i < count; i++) {
        cmcp_json_t *j = cmcp_rpc_to_json(&msgs[i]);
        if (!j || cmcp_json_array_append(arr, j) != CMCP_OK) {
            cmcp_json_free(j); cmcp_json_free(arr);
            return NULL;
        }
    }
    char *s = cmcp_json_emit_stable(arr);
    cmcp_json_free(arr);
    return s;
}

/* ====================================================================== */
/* JSON-RPC decoding                                                       */
/* ====================================================================== */

static int parse_id_field(const cmcp_json_t *jid, cmcp_id_t *out) {
    if (!jid) { cmcp_id_init_none(out); return CMCP_OK; }
    switch (jid->type) {
    case CMCP_JSON_NULL:   cmcp_id_init_null(out); return CMCP_OK;
    case CMCP_JSON_INT:    cmcp_id_init_int(out, jid->i); return CMCP_OK;
    case CMCP_JSON_STRING: return cmcp_id_init_string(out, jid->str.s,
                                                      jid->str.len);
    /* JSON-RPC technically allows numeric (non-integer) IDs but
     * discourages fractional parts. We reject them. */
    default:               return CMCP_EPROTOCOL;
    }
}

int cmcp_rpc_from_json(const cmcp_json_t *json, cmcp_rpc_message_t *out) {
    if (!json || !out) return CMCP_EINVAL;
    cmcp_rpc_message_init(out);
    if (json->type != CMCP_JSON_OBJECT) return CMCP_EPROTOCOL;

    /* Validate jsonrpc == "2.0". */
    const cmcp_json_t *jver = cmcp_json_object_get(json, "jsonrpc");
    if (!jver || jver->type != CMCP_JSON_STRING ||
        jver->str.len != 3 || memcmp(jver->str.s, "2.0", 3) != 0) {
        return CMCP_EPROTOCOL;
    }

    const cmcp_json_t *jid     = cmcp_json_object_get(json, "id");
    const cmcp_json_t *jmethod = cmcp_json_object_get(json, "method");
    const cmcp_json_t *jparams = cmcp_json_object_get(json, "params");
    const cmcp_json_t *jresult = cmcp_json_object_get(json, "result");
    const cmcp_json_t *jerror  = cmcp_json_object_get(json, "error");

    /* Classify the message. JSON-RPC 2.0:
     *   - request:      jsonrpc + method + id   (params optional)
     *   - notification: jsonrpc + method        (no id; params optional)
     *   - response:     jsonrpc + id + (result XOR error)
     */
    if (jmethod) {
        if (jmethod->type != CMCP_JSON_STRING) return CMCP_EPROTOCOL;
        if (jresult || jerror) return CMCP_EPROTOCOL;
        out->method = dup_n(jmethod->str.s, jmethod->str.len);
        if (!out->method) return CMCP_ENOMEM;
        if (jparams) {
            if (jparams->type != CMCP_JSON_OBJECT &&
                jparams->type != CMCP_JSON_ARRAY) {
                return CMCP_EPROTOCOL;
            }
            out->params = cmcp_json_clone(jparams);
            if (!out->params) return CMCP_ENOMEM;
        }
        if (jid) {
            out->kind = CMCP_MSG_REQUEST;
            int rc = parse_id_field(jid, &out->id);
            if (rc != CMCP_OK) return rc;
        } else {
            out->kind = CMCP_MSG_NOTIFICATION;
        }
        return CMCP_OK;
    }

    /* No method → must be a response. */
    if (!jid) return CMCP_EPROTOCOL;
    if ((jresult && jerror) || (!jresult && !jerror)) return CMCP_EPROTOCOL;

    out->kind = CMCP_MSG_RESPONSE;
    int rc = parse_id_field(jid, &out->id);
    if (rc != CMCP_OK) return rc;

    if (jresult) {
        out->result = cmcp_json_clone(jresult);
        if (!out->result) return CMCP_ENOMEM;
    } else {
        if (jerror->type != CMCP_JSON_OBJECT) return CMCP_EPROTOCOL;
        const cmcp_json_t *jcode = cmcp_json_object_get(jerror, "code");
        const cmcp_json_t *jmsg  = cmcp_json_object_get(jerror, "message");
        const cmcp_json_t *jdata = cmcp_json_object_get(jerror, "data");
        if (!jcode || jcode->type != CMCP_JSON_INT) return CMCP_EPROTOCOL;
        if (!jmsg  || jmsg->type  != CMCP_JSON_STRING) return CMCP_EPROTOCOL;
        cmcp_json_t *data_clone = NULL;
        if (jdata) {
            data_clone = cmcp_json_clone(jdata);
            if (!data_clone) return CMCP_ENOMEM;
        }
        char *msg_dup = dup_n(jmsg->str.s, jmsg->str.len);
        if (!msg_dup) { cmcp_json_free(data_clone); return CMCP_ENOMEM; }
        cmcp_rpc_error_t *e = (cmcp_rpc_error_t *)calloc(1, sizeof *e);
        if (!e) { free(msg_dup); cmcp_json_free(data_clone); return CMCP_ENOMEM; }
        e->code = (int)jcode->i;
        e->message = msg_dup;
        e->data = data_clone;
        out->error = e;
    }
    return CMCP_OK;
}

int cmcp_rpc_parse(const char *text, size_t len,
                   cmcp_rpc_message_t **out_msgs, size_t *out_count) {
    if (!text || !out_msgs || !out_count) return CMCP_EINVAL;
    *out_msgs = NULL;
    *out_count = 0;

    cmcp_json_t *root = cmcp_json_parse(text, len);
    if (!root) return CMCP_EPARSE;

    int rc = CMCP_OK;
    cmcp_rpc_message_t *msgs = NULL;
    size_t n = 0;

    if (root->type == CMCP_JSON_ARRAY) {
        n = cmcp_json_array_len(root);
        if (n == 0) { rc = CMCP_EPROTOCOL; goto out; }
        msgs = (cmcp_rpc_message_t *)calloc(n, sizeof *msgs);
        if (!msgs) { rc = CMCP_ENOMEM; goto out; }
        for (size_t i = 0; i < n; i++) {
            const cmcp_json_t *elem = cmcp_json_array_at(root, i);
            rc = cmcp_rpc_from_json(elem, &msgs[i]);
            if (rc != CMCP_OK) {
                for (size_t j = 0; j <= i; j++) cmcp_rpc_message_clear(&msgs[j]);
                free(msgs); msgs = NULL; n = 0;
                goto out;
            }
        }
    } else if (root->type == CMCP_JSON_OBJECT) {
        msgs = (cmcp_rpc_message_t *)calloc(1, sizeof *msgs);
        if (!msgs) { rc = CMCP_ENOMEM; goto out; }
        rc = cmcp_rpc_from_json(root, &msgs[0]);
        if (rc != CMCP_OK) {
            cmcp_rpc_message_clear(&msgs[0]);
            free(msgs); msgs = NULL;
            goto out;
        }
        n = 1;
    } else {
        rc = CMCP_EPROTOCOL;
    }

out:
    cmcp_json_free(root);
    if (rc == CMCP_OK) {
        *out_msgs = msgs;
        *out_count = n;
    }
    return rc;
}

void cmcp_rpc_messages_free(cmcp_rpc_message_t *msgs, size_t count) {
    if (!msgs) return;
    for (size_t i = 0; i < count; i++) cmcp_rpc_message_clear(&msgs[i]);
    free(msgs);
}

/* ====================================================================== */
/* Construction helpers                                                    */
/* ====================================================================== */

int cmcp_rpc_make_request(cmcp_rpc_message_t *m, long long id,
                          const char *method, cmcp_json_t *params) {
    if (!m || !method) return CMCP_EINVAL;
    cmcp_rpc_message_init(m);
    m->kind = CMCP_MSG_REQUEST;
    cmcp_id_init_int(&m->id, id);
    m->method = dup_cstr(method);
    if (!m->method) { cmcp_rpc_message_clear(m); return CMCP_ENOMEM; }
    m->params = params;
    return CMCP_OK;
}

int cmcp_rpc_make_request_str(cmcp_rpc_message_t *m,
                              const char *id_str, size_t id_len,
                              const char *method, cmcp_json_t *params) {
    if (!m || !id_str || !method) return CMCP_EINVAL;
    cmcp_rpc_message_init(m);
    m->kind = CMCP_MSG_REQUEST;
    int rc = cmcp_id_init_string(&m->id, id_str, id_len);
    if (rc != CMCP_OK) return rc;
    m->method = dup_cstr(method);
    if (!m->method) { cmcp_rpc_message_clear(m); return CMCP_ENOMEM; }
    m->params = params;
    return CMCP_OK;
}

int cmcp_rpc_make_notification(cmcp_rpc_message_t *m,
                               const char *method, cmcp_json_t *params) {
    if (!m || !method) return CMCP_EINVAL;
    cmcp_rpc_message_init(m);
    m->kind = CMCP_MSG_NOTIFICATION;
    m->method = dup_cstr(method);
    if (!m->method) { cmcp_rpc_message_clear(m); return CMCP_ENOMEM; }
    m->params = params;
    return CMCP_OK;
}

int cmcp_rpc_make_response(cmcp_rpc_message_t *m, const cmcp_id_t *id,
                           cmcp_json_t *result) {
    if (!m || !id) return CMCP_EINVAL;
    cmcp_rpc_message_init(m);
    m->kind = CMCP_MSG_RESPONSE;
    int rc = cmcp_id_copy(&m->id, id);
    if (rc != CMCP_OK) return rc;
    m->result = result;
    return CMCP_OK;
}

int cmcp_rpc_make_error(cmcp_rpc_message_t *m, const cmcp_id_t *id,
                        int code, const char *message, cmcp_json_t *data) {
    if (!m || !id || !message) return CMCP_EINVAL;
    cmcp_rpc_message_init(m);
    m->kind = CMCP_MSG_RESPONSE;
    int rc = cmcp_id_copy(&m->id, id);
    if (rc != CMCP_OK) return rc;
    m->error = rpc_error_new(code, message, data);
    if (!m->error) { cmcp_rpc_message_clear(m); return CMCP_ENOMEM; }
    return CMCP_OK;
}

/* ====================================================================== */
/* In-flight pending request table                                         */
/* ====================================================================== */

#define PENDING_INIT_CAP   16
#define PENDING_TOMBSTONE  ((long long)-1)

/* In-flight cap (Tier 6 axis 6.5.3). Bounds the number of concurrent
 * unresolved registrations so a peer that never replies can't drive
 * the hash table to arbitrary size. 0 = unbounded; default 1024
 * (env: CMCP_RPC_MAX_INFLIGHT, snapshotted at table construction). */
#define CMCP_RPC_MAX_INFLIGHT_DEFAULT 1024

typedef struct {
    long long id;        /* 0 = empty, -1 = tombstone, else live */
    void     *userdata;
} pending_slot_t;

struct cmcp_rpc_pending {
    pending_slot_t *slots;
    size_t          cap;          /* power of two */
    size_t          len;          /* live entries */
    size_t          tomb;         /* tombstones */
    long long       next_id;      /* monotonic */
    size_t          max_inflight; /* 0 = unbounded */
    pthread_mutex_t mu;
};

static size_t pending_hash(long long id, size_t cap) {
    /* Knuth multiplicative hash on the low 64 bits. */
    uint64_t h = (uint64_t)id * 2654435761ull;
    return (size_t)h & (cap - 1);
}

static int pending_resize(cmcp_rpc_pending_t *t, size_t new_cap) {
    pending_slot_t *ns = (pending_slot_t *)calloc(new_cap, sizeof *ns);
    if (!ns) return CMCP_ENOMEM;
    for (size_t i = 0; i < t->cap; i++) {
        long long id = t->slots[i].id;
        if (id <= 0) continue; /* empty or tombstone */
        size_t idx = pending_hash(id, new_cap);
        while (ns[idx].id != 0) idx = (idx + 1) & (new_cap - 1);
        ns[idx] = t->slots[i];
    }
    free(t->slots);
    t->slots = ns;
    t->cap = new_cap;
    t->tomb = 0;
    return CMCP_OK;
}

cmcp_rpc_pending_t *cmcp_rpc_pending_new(void) {
    cmcp_rpc_pending_t *t = (cmcp_rpc_pending_t *)calloc(1, sizeof *t);
    if (!t) return NULL;
    t->slots = (pending_slot_t *)calloc(PENDING_INIT_CAP, sizeof *t->slots);
    if (!t->slots) { free(t); return NULL; }
    t->cap = PENDING_INIT_CAP;
    t->next_id = 1;
    /* Snapshot env once at construction; setter overrides after. */
    t->max_inflight = CMCP_RPC_MAX_INFLIGHT_DEFAULT;
    const char *mi = getenv("CMCP_RPC_MAX_INFLIGHT");
    if (mi && *mi) {
        char *end; long v = strtol(mi, &end, 10);
        if (end != mi && *end == '\0' && v >= 0) t->max_inflight = (size_t)v;
    }
    if (pthread_mutex_init(&t->mu, NULL) != 0) {
        free(t->slots); free(t);
        return NULL;
    }
    return t;
}

void cmcp_rpc_pending_free(cmcp_rpc_pending_t *t) {
    if (!t) return;
    pthread_mutex_destroy(&t->mu);
    free(t->slots);
    free(t);
}

long long cmcp_rpc_pending_register(cmcp_rpc_pending_t *t, void *userdata) {
    if (!t) return 0;
    pthread_mutex_lock(&t->mu);

    if (t->max_inflight > 0 && t->len >= t->max_inflight) {
        pthread_mutex_unlock(&t->mu);
        return -1;  /* CMCP_EAGAIN — caller surfaces capacity error */
    }

    if ((t->len + t->tomb) * 4 >= t->cap * 3) {
        size_t new_cap = (t->len * 2 >= t->cap) ? t->cap * 2 : t->cap;
        if (pending_resize(t, new_cap) != CMCP_OK) {
            pthread_mutex_unlock(&t->mu);
            return 0;
        }
    }

    long long id = t->next_id++;
    size_t idx = pending_hash(id, t->cap);
    size_t first_tomb = (size_t)-1;
    while (t->slots[idx].id != 0) {
        if (t->slots[idx].id == PENDING_TOMBSTONE && first_tomb == (size_t)-1) {
            first_tomb = idx;
        }
        idx = (idx + 1) & (t->cap - 1);
    }
    if (first_tomb != (size_t)-1) { idx = first_tomb; t->tomb--; }
    t->slots[idx].id = id;
    t->slots[idx].userdata = userdata;
    t->len++;
    pthread_mutex_unlock(&t->mu);
    return id;
}

int cmcp_rpc_pending_take(cmcp_rpc_pending_t *t, long long id,
                          void **out_userdata) {
    if (!t || id <= 0) return 0;
    pthread_mutex_lock(&t->mu);
    size_t idx = pending_hash(id, t->cap);
    for (size_t probes = 0; probes < t->cap; probes++) {
        long long sid = t->slots[idx].id;
        if (sid == 0) break;
        if (sid == id) {
            if (out_userdata) *out_userdata = t->slots[idx].userdata;
            t->slots[idx].id = PENDING_TOMBSTONE;
            t->slots[idx].userdata = NULL;
            t->len--;
            t->tomb++;
            pthread_mutex_unlock(&t->mu);
            return 1;
        }
        idx = (idx + 1) & (t->cap - 1);
    }
    pthread_mutex_unlock(&t->mu);
    return 0;
}

size_t cmcp_rpc_pending_count(cmcp_rpc_pending_t *t) {
    if (!t) return 0;
    pthread_mutex_lock(&t->mu);
    size_t n = t->len;
    pthread_mutex_unlock(&t->mu);
    return n;
}

void cmcp_rpc_pending_set_max_inflight(cmcp_rpc_pending_t *t, size_t cap) {
    if (!t) return;
    pthread_mutex_lock(&t->mu);
    t->max_inflight = cap;
    pthread_mutex_unlock(&t->mu);
}

size_t cmcp_rpc_pending_max_inflight(cmcp_rpc_pending_t *t) {
    if (!t) return 0;
    pthread_mutex_lock(&t->mu);
    size_t n = t->max_inflight;
    pthread_mutex_unlock(&t->mu);
    return n;
}

/* ====================================================================== */
/* Dispatch                                                                */
/* ====================================================================== */

static int set_error(cmcp_rpc_message_t *resp, int code, const char *msg) {
    resp->error = rpc_error_new(code, msg, NULL);
    return resp->error ? CMCP_OK : CMCP_ENOMEM;
}

int cmcp_rpc_dispatch(const cmcp_rpc_message_t *in,
                      const cmcp_rpc_route_t *routes, size_t n_routes,
                      cmcp_rpc_message_t *out_response) {
    if (!in) return CMCP_EINVAL;
    if (in->kind == CMCP_MSG_RESPONSE) return CMCP_EINVAL;

    const cmcp_rpc_route_t *match = NULL;
    if (in->method && routes) {
        for (size_t i = 0; i < n_routes; i++) {
            if (routes[i].method &&
                strcmp(routes[i].method, in->method) == 0) {
                match = &routes[i];
                break;
            }
        }
    }

    if (in->kind == CMCP_MSG_NOTIFICATION) {
        if (match && match->handler) {
            match->handler(in, NULL, match->userdata);
        }
        return CMCP_OK;
    }

    /* REQUEST */
    if (!out_response) return CMCP_EINVAL;
    cmcp_rpc_message_init(out_response);
    out_response->kind = CMCP_MSG_RESPONSE;
    cmcp_id_copy(&out_response->id, &in->id);

    if (!match || !match->handler) {
        char buf[256];
        snprintf(buf, sizeof buf, "Method not found: %s",
                 in->method ? in->method : "(null)");
        return set_error(out_response, CMCP_RPC_METHOD_NOT_FOUND, buf);
    }

    int rc = match->handler(in, out_response, match->userdata);
    if (rc != CMCP_OK && !out_response->result && !out_response->error) {
        set_error(out_response, CMCP_RPC_INTERNAL_ERROR, "Handler error");
    }
    return CMCP_OK;
}
