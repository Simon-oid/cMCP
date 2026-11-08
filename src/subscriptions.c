/* subscriptions.c — registry of MCP 2026-07-28 subscriptions (see
 * subscriptions.h). A short linked list under one mutex: a server has a
 * handful of listeners, not thousands. */
#include "subscriptions.h"

#include "cmcp.h"

#include <pthread.h>
#include <stdlib.h>
#include <string.h>

typedef struct sub {
    cmcp_json_t *id;          /* owned clone of the listen request's id */
    unsigned     kinds;       /* CMCP_SUB_* bits the server agreed to   */
    cmcp_json_t *uris;        /* owned array of strings, or NULL        */
    struct sub  *next;
} sub_t;

struct cmcp_subs {
    pthread_mutex_t mu;
    sub_t          *head;
};

/* Filter fields for the three boolean kinds. */
static const struct {
    const char *field;
    unsigned    kind;
} BOOL_KINDS[] = {
    {"toolsListChanged",     CMCP_SUB_TOOLS},
    {"promptsListChanged",   CMCP_SUB_PROMPTS},
    {"resourcesListChanged", CMCP_SUB_RESOURCES},
};
#define N_BOOL_KINDS (sizeof BOOL_KINDS / sizeof BOOL_KINDS[0])

static int set_owned(cmcp_json_t *o, const char *k, cmcp_json_t *v) {
    if (!v || cmcp_json_object_set(o, k, v) != 0) {
        cmcp_json_free(v);
        return -1;
    }
    return 0;
}

static void sub_free(sub_t *s) {
    if (!s) return;
    cmcp_json_free(s->id);
    cmcp_json_free(s->uris);
    free(s);
}

cmcp_subs_t *cmcp_subs_new(void) {
    cmcp_subs_t *subs = (cmcp_subs_t *)calloc(1, sizeof *subs);
    if (!subs) return NULL;
    if (pthread_mutex_init(&subs->mu, NULL) != 0) {
        free(subs);
        return NULL;
    }
    return subs;
}

void cmcp_subs_free(cmcp_subs_t *subs) {
    if (!subs) return;
    sub_t *s = subs->head;
    while (s) {
        sub_t *next = s->next;
        sub_free(s);
        s = next;
    }
    pthread_mutex_destroy(&subs->mu);
    free(subs);
}

/* Read the filter into *kinds / *uris. CMCP_EPROTOCOL when malformed. */
static int parse_filter(const cmcp_json_t *f, unsigned supported,
                        unsigned *kinds, cmcp_json_t **uris) {
    *kinds = 0;
    *uris = NULL;
    if (!f) return CMCP_OK;                    /* every field is optional */
    if (f->type != CMCP_JSON_OBJECT) return CMCP_EPROTOCOL;

    for (size_t i = 0; i < N_BOOL_KINDS; i++) {
        const cmcp_json_t *v = cmcp_json_object_get(f, BOOL_KINDS[i].field);
        if (!v) continue;
        if (v->type != CMCP_JSON_BOOL) return CMCP_EPROTOCOL;
        if (cmcp_json_bool(v) && (supported & BOOL_KINDS[i].kind))
            *kinds |= BOOL_KINDS[i].kind;
    }

    const cmcp_json_t *rs = cmcp_json_object_get(f, "resourceSubscriptions");
    if (!rs) return CMCP_OK;
    if (rs->type != CMCP_JSON_ARRAY) return CMCP_EPROTOCOL;
    for (size_t i = 0; i < cmcp_json_array_len(rs); i++) {
        if (cmcp_json_array_at(rs, i)->type != CMCP_JSON_STRING)
            return CMCP_EPROTOCOL;
    }
    if ((supported & CMCP_SUB_UPDATES) && cmcp_json_array_len(rs) > 0) {
        *uris = cmcp_json_clone(rs);
        if (!*uris) return CMCP_ENOMEM;
        *kinds |= CMCP_SUB_UPDATES;
    }
    return CMCP_OK;
}

/* The `notifications` object of the acknowledgment: what was kept. */
static cmcp_json_t *accepted_filter(unsigned kinds, const cmcp_json_t *uris) {
    cmcp_json_t *o = cmcp_json_new_object();
    if (!o) return NULL;
    for (size_t i = 0; i < N_BOOL_KINDS; i++) {
        if ((kinds & BOOL_KINDS[i].kind) &&
            set_owned(o, BOOL_KINDS[i].field, cmcp_json_new_bool(1)) != 0) {
            cmcp_json_free(o);
            return NULL;
        }
    }
    if ((kinds & CMCP_SUB_UPDATES) &&
        set_owned(o, "resourceSubscriptions", cmcp_json_clone(uris)) != 0) {
        cmcp_json_free(o);
        return NULL;
    }
    return o;
}

cmcp_json_t *cmcp_subs_tag(const cmcp_json_t *id, cmcp_json_t *extra) {
    cmcp_json_t *p = extra ? extra : cmcp_json_new_object();
    cmcp_json_t *meta = cmcp_json_new_object();
    if (!p || !meta ||
        set_owned(meta, CMCP_META_SUBSCRIPTION_ID, cmcp_json_clone(id)) != 0) {
        cmcp_json_free(meta);
        cmcp_json_free(p);
        return NULL;
    }
    if (set_owned(p, "_meta", meta) != 0) {
        cmcp_json_free(p);
        return NULL;
    }
    return p;
}

/* Acknowledgment params: {"_meta": {subscriptionId}, "notifications": kept}. */
static cmcp_json_t *make_ack(const cmcp_json_t *id, unsigned kinds,
                             const cmcp_json_t *uris) {
    cmcp_json_t *body = cmcp_json_new_object();
    if (!body) return NULL;
    if (set_owned(body, "notifications", accepted_filter(kinds, uris)) != 0) {
        cmcp_json_free(body);
        return NULL;
    }
    return cmcp_subs_tag(id, body);          /* consumes body */
}

/* Unlink and free the subscription with this id. Caller holds mu. */
static int remove_locked(cmcp_subs_t *subs, const cmcp_json_t *id) {
    for (sub_t **pp = &subs->head; *pp; pp = &(*pp)->next) {
        if (cmcp_json_equal((*pp)->id, id)) {
            sub_t *dead = *pp;
            *pp = dead->next;
            sub_free(dead);
            return 1;
        }
    }
    return 0;
}

int cmcp_subs_listen(cmcp_subs_t *subs, const cmcp_json_t *id,
                     const cmcp_json_t *params, unsigned supported,
                     cmcp_json_t **out_ack) {
    *out_ack = NULL;
    if (!subs || !id) return CMCP_EINVAL;
    const cmcp_json_t *f = (params && params->type == CMCP_JSON_OBJECT)
                               ? cmcp_json_object_get(params, "notifications")
                               : NULL;
    unsigned kinds = 0;
    cmcp_json_t *uris = NULL;
    int rc = parse_filter(f, supported, &kinds, &uris);
    if (rc != CMCP_OK) return rc;

    sub_t *s = (sub_t *)calloc(1, sizeof *s);
    cmcp_json_t *ack = make_ack(id, kinds, uris);
    if (s) s->id = cmcp_json_clone(id);
    if (!s || !s->id || !ack) {
        sub_free(s);
        cmcp_json_free(uris);
        cmcp_json_free(ack);
        return CMCP_ENOMEM;
    }
    s->kinds = kinds;
    s->uris = uris;

    pthread_mutex_lock(&subs->mu);
    remove_locked(subs, id);
    s->next = subs->head;
    subs->head = s;
    pthread_mutex_unlock(&subs->mu);
    *out_ack = ack;
    return CMCP_OK;
}

int cmcp_subs_cancel(cmcp_subs_t *subs, const cmcp_json_t *id) {
    if (!subs || !id) return 0;
    pthread_mutex_lock(&subs->mu);
    int removed = remove_locked(subs, id);
    pthread_mutex_unlock(&subs->mu);
    return removed;
}

static int has_uri(const cmcp_json_t *uris, const char *uri) {
    for (size_t i = 0; uris && i < cmcp_json_array_len(uris); i++) {
        if (strcmp(cmcp_json_string(cmcp_json_array_at(uris, i)), uri) == 0)
            return 1;
    }
    return 0;
}

cmcp_json_t *cmcp_subs_match(cmcp_subs_t *subs, unsigned kind,
                             const char *uri) {
    if (!subs) return NULL;
    cmcp_json_t *ids = NULL;
    pthread_mutex_lock(&subs->mu);
    for (sub_t *s = subs->head; s; s = s->next) {
        if (!(s->kinds & kind)) continue;
        if (kind == CMCP_SUB_UPDATES && (!uri || !has_uri(s->uris, uri)))
            continue;
        if (!ids) ids = cmcp_json_new_array();
        cmcp_json_t *copy = cmcp_json_clone(s->id);
        if (!ids || !copy || cmcp_json_array_append(ids, copy) != 0) {
            cmcp_json_free(copy);
            break;
        }
    }
    pthread_mutex_unlock(&subs->mu);
    if (ids && cmcp_json_array_len(ids) == 0) {
        cmcp_json_free(ids);
        ids = NULL;
    }
    return ids;
}
