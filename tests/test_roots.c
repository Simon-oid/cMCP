/* End-to-end tests for the Phase 2.6 roots surface (host-side).
 *
 * The library's `cmcp_server_t` doesn't initiate requests, so we
 * hand-roll a mini-server (same pattern as test_sampling) that:
 *   1. Completes the initialize handshake. Captures the client's
 *      advertised capabilities object.
 *   2. Sends a `roots/list` request and captures the response.
 *
 * Plus a separate test for the list-changed notification path.
 */

#define _POSIX_C_SOURCE 200809L

#include "test.h"
#include "cmcp.h"
#include "cmcp_client.h"
#include "cmcp_json.h"
#include "cmcp_transport.h"
#include "cmcp_types.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

/* ====================================================================== */
/* Pipe-pair scaffolding                                                   */
/* ====================================================================== */

typedef struct {
    cmcp_transport_t *client_t;
    cmcp_transport_t *server_t;
} transport_pair_t;

static int make_pair(transport_pair_t *out) {
    int c2s[2], s2c[2];
    if (pipe(c2s) != 0) return -1;
    if (pipe(s2c) != 0) { close(c2s[0]); close(c2s[1]); return -1; }
    out->client_t = cmcp_transport_stdio_new_fds(s2c[0], c2s[1]);
    out->server_t = cmcp_transport_stdio_new_fds(c2s[0], s2c[1]);
    if (!out->client_t || !out->server_t) {
        cmcp_transport_close(out->client_t);
        cmcp_transport_close(out->server_t);
        return -1;
    }
    return 0;
}

/* ====================================================================== */
/* Mini-server: handshake → roots/list → capture                            */
/* ====================================================================== */

typedef struct {
    pthread_mutex_t       mu;
    pthread_cond_t        cv;
    int                   done;
    /* Snapshot of `capabilities` from the initialize request. */
    int                   has_caps;
    int                   roots_advertised;
    int                   roots_list_changed;
    /* Captured response to roots/list. */
    cmcp_rpc_message_t    captured;
    /* Notification capture (separate path). */
    int                   notif_seen;
    char                 *notif_method;
} capture_t;

static void capture_init(capture_t *c) {
    pthread_mutex_init(&c->mu, NULL);
    pthread_cond_init(&c->cv, NULL);
    c->done = 0;
    c->has_caps = 0;
    c->roots_advertised = 0;
    c->roots_list_changed = 0;
    c->notif_seen = 0;
    c->notif_method = NULL;
    cmcp_rpc_message_init(&c->captured);
}

static void capture_clear(capture_t *c) {
    pthread_mutex_destroy(&c->mu);
    pthread_cond_destroy(&c->cv);
    cmcp_rpc_message_clear(&c->captured);
    free(c->notif_method);
}

typedef struct {
    cmcp_transport_t *t;
    capture_t        *cap;
    /* If non-zero, send a roots/list request after handshake. */
    int               send_roots_list;
    /* If non-zero, drain post-handshake frames into notif capture. */
    int               capture_notifs;
} miniserver_arg_t;

static void *miniserver_thread(void *arg) {
    miniserver_arg_t *a = (miniserver_arg_t *)arg;
    cmcp_transport_t *t = a->t;

    /* 1. Read initialize. Capture caps.roots presence + listChanged. */
    char *frame = NULL; size_t flen = 0;
    if (cmcp_transport_read(t, &frame, &flen) != CMCP_OK) return NULL;
    cmcp_rpc_message_t *msgs = NULL; size_t nmsgs = 0;
    int prc = cmcp_rpc_parse(frame, flen, &msgs, &nmsgs);
    free(frame);
    if (prc != CMCP_OK || nmsgs != 1) {
        cmcp_rpc_messages_free(msgs, nmsgs);
        return NULL;
    }

    pthread_mutex_lock(&a->cap->mu);
    if (msgs[0].params && msgs[0].params->type == CMCP_JSON_OBJECT) {
        const cmcp_json_t *cc = cmcp_json_object_get(msgs[0].params,
                                                       "capabilities");
        if (cc && cc->type == CMCP_JSON_OBJECT) {
            a->cap->has_caps = 1;
            const cmcp_json_t *r = cmcp_json_object_get(cc, "roots");
            if (r && r->type == CMCP_JSON_OBJECT) {
                a->cap->roots_advertised = 1;
                const cmcp_json_t *lc = cmcp_json_object_get(r, "listChanged");
                if (lc && lc->type == CMCP_JSON_BOOL && lc->b)
                    a->cap->roots_list_changed = 1;
            }
        }
    }
    pthread_mutex_unlock(&a->cap->mu);

    /* 2. Reply minimal initialize success. */
    cmcp_json_t *result = cmcp_json_new_object();
    cmcp_json_object_set(result, "protocolVersion",
                          cmcp_json_new_string(CMCP_PROTOCOL_VERSION));
    cmcp_json_object_set(result, "capabilities", cmcp_json_new_object());
    cmcp_json_t *si = cmcp_json_new_object();
    cmcp_json_object_set(si, "name",    cmcp_json_new_string("roots-srv"));
    cmcp_json_object_set(si, "version", cmcp_json_new_string("0.1.0"));
    cmcp_json_object_set(result, "serverInfo", si);

    cmcp_rpc_message_t resp;
    cmcp_rpc_message_init(&resp);
    cmcp_rpc_make_response(&resp, &msgs[0].id, result);
    char *wire = cmcp_rpc_emit(&resp);
    cmcp_transport_write(t, wire, strlen(wire));
    free(wire);
    cmcp_rpc_message_clear(&resp);
    cmcp_rpc_messages_free(msgs, nmsgs);

    /* 3. Read notifications/initialized. */
    if (cmcp_transport_read(t, &frame, &flen) != CMCP_OK) return NULL;
    free(frame);

    /* 4. Optionally send roots/list and capture the reply. */
    if (a->send_roots_list) {
        cmcp_rpc_message_t req;
        cmcp_rpc_message_init(&req);
        cmcp_rpc_make_request(&req, 7, "roots/list", NULL);
        wire = cmcp_rpc_emit(&req);
        cmcp_transport_write(t, wire, strlen(wire));
        free(wire);
        cmcp_rpc_message_clear(&req);

        if (cmcp_transport_read(t, &frame, &flen) != CMCP_OK) return NULL;
        cmcp_rpc_message_t *rsps = NULL; size_t nr = 0;
        int rrc = cmcp_rpc_parse(frame, flen, &rsps, &nr);
        free(frame);
        pthread_mutex_lock(&a->cap->mu);
        if (rrc == CMCP_OK && nr == 1) {
            a->cap->captured = rsps[0];
            cmcp_rpc_message_init(&rsps[0]);
        }
        a->cap->done = 1;
        pthread_cond_broadcast(&a->cap->cv);
        pthread_mutex_unlock(&a->cap->mu);
        cmcp_rpc_messages_free(rsps, nr);
    } else {
        /* Not sending a request — mark done immediately so the test
         * can proceed to check the captured caps. */
        pthread_mutex_lock(&a->cap->mu);
        a->cap->done = 1;
        pthread_cond_broadcast(&a->cap->cv);
        pthread_mutex_unlock(&a->cap->mu);
    }

    /* 5. Drain remaining frames; record notification methods. */
    while (cmcp_transport_read(t, &frame, &flen) == CMCP_OK) {
        if (a->capture_notifs) {
            cmcp_rpc_message_t *m2 = NULL; size_t n2 = 0;
            if (cmcp_rpc_parse(frame, flen, &m2, &n2) == CMCP_OK && n2 == 1 &&
                m2[0].kind == CMCP_MSG_NOTIFICATION && m2[0].method) {
                pthread_mutex_lock(&a->cap->mu);
                if (!a->cap->notif_seen) {
                    a->cap->notif_method = strdup(m2[0].method);
                    a->cap->notif_seen = 1;
                    pthread_cond_broadcast(&a->cap->cv);
                }
                pthread_mutex_unlock(&a->cap->mu);
            }
            cmcp_rpc_messages_free(m2, n2);
        }
        free(frame);
    }
    return NULL;
}

static int wait_done(capture_t *c) {
    pthread_mutex_lock(&c->mu);
    while (!c->done) {
        struct timespec ts;
        clock_gettime(CLOCK_REALTIME, &ts);
        ts.tv_sec += 2;
        if (pthread_cond_timedwait(&c->cv, &c->mu, &ts) != 0) break;
    }
    int hit = c->done;
    pthread_mutex_unlock(&c->mu);
    return hit;
}

static int wait_notif(capture_t *c) {
    pthread_mutex_lock(&c->mu);
    while (!c->notif_seen) {
        struct timespec ts;
        clock_gettime(CLOCK_REALTIME, &ts);
        ts.tv_sec += 2;
        if (pthread_cond_timedwait(&c->cv, &c->mu, &ts) != 0) break;
    }
    int hit = c->notif_seen;
    pthread_mutex_unlock(&c->mu);
    return hit;
}

/* ====================================================================== */
/* Tests                                                                    */
/* ====================================================================== */

static void test_no_set_roots_no_cap_and_method_not_found(void) {
    /* Host never calls set_roots. The `roots` cap must NOT be in the
     * initialize, and a server-sent roots/list request gets -32601. */
    transport_pair_t p;
    TEST_ASSERT(make_pair(&p) == 0);

    capture_t cap; capture_init(&cap);
    miniserver_arg_t marg = { p.server_t, &cap, /*send=*/1, /*notif=*/0 };
    pthread_t th;
    pthread_create(&th, NULL, miniserver_thread, &marg);

    cmcp_client_t *cli = cmcp_client_new("host", "0.0.1");
    TEST_ASSERT(cmcp_client_handshake(cli, p.client_t) == CMCP_OK);

    TEST_ASSERT(wait_done(&cap));
    TEST_ASSERT(cap.has_caps == 1);
    TEST_ASSERT(cap.roots_advertised == 0);
    TEST_ASSERT(cap.captured.kind  == CMCP_MSG_RESPONSE);
    TEST_ASSERT(cap.captured.error != NULL);
    TEST_ASSERT(cap.captured.error &&
                cap.captured.error->code == CMCP_RPC_METHOD_NOT_FOUND);

    cmcp_client_free(cli);
    cmcp_transport_close(p.client_t);
    pthread_join(th, NULL);
    cmcp_transport_close(p.server_t);
    capture_clear(&cap);
}

static void test_set_roots_advertises_cap_and_returns_list(void) {
    transport_pair_t p;
    TEST_ASSERT(make_pair(&p) == 0);

    capture_t cap; capture_init(&cap);
    miniserver_arg_t marg = { p.server_t, &cap, /*send=*/1, /*notif=*/0 };
    pthread_t th;
    pthread_create(&th, NULL, miniserver_thread, &marg);

    cmcp_client_t *cli = cmcp_client_new("host", "0.0.1");
    cmcp_root_t roots[] = {
        { "file:///home/u/proj", "main project" },
        { "file:///tmp/scratch", NULL          },
    };
    TEST_ASSERT(cmcp_client_set_roots(cli, roots, 2) == CMCP_OK);
    TEST_ASSERT(cmcp_client_handshake(cli, p.client_t) == CMCP_OK);

    TEST_ASSERT(wait_done(&cap));
    TEST_ASSERT(cap.has_caps == 1);
    TEST_ASSERT(cap.roots_advertised == 1);
    /* listChanged not opted in. */
    TEST_ASSERT(cap.roots_list_changed == 0);

    /* Response shape. */
    TEST_ASSERT(cap.captured.kind  == CMCP_MSG_RESPONSE);
    TEST_ASSERT(cap.captured.error == NULL);
    const cmcp_json_t *arr = cmcp_json_object_get(cap.captured.result, "roots");
    TEST_ASSERT(arr && arr->type == CMCP_JSON_ARRAY);
    TEST_ASSERT(cmcp_json_array_len(arr) == 2);

    const cmcp_json_t *r0 = cmcp_json_array_at(arr, 0);
    const cmcp_json_t *r1 = cmcp_json_array_at(arr, 1);
    const cmcp_json_t *u0 = cmcp_json_object_get(r0, "uri");
    const cmcp_json_t *n0 = cmcp_json_object_get(r0, "name");
    const cmcp_json_t *u1 = cmcp_json_object_get(r1, "uri");
    const cmcp_json_t *n1 = cmcp_json_object_get(r1, "name");
    TEST_ASSERT(u0 && strcmp(u0->str.s, "file:///home/u/proj") == 0);
    TEST_ASSERT(n0 && strcmp(n0->str.s, "main project")        == 0);
    TEST_ASSERT(u1 && strcmp(u1->str.s, "file:///tmp/scratch") == 0);
    /* Optional `name` was NULL — must be omitted, not echoed empty. */
    TEST_ASSERT(n1 == NULL);

    cmcp_client_free(cli);
    cmcp_transport_close(p.client_t);
    pthread_join(th, NULL);
    cmcp_transport_close(p.server_t);
    capture_clear(&cap);
}

static void test_set_roots_empty_array(void) {
    /* set_roots(NULL, 0) opts in to the cap with an empty list. */
    transport_pair_t p;
    TEST_ASSERT(make_pair(&p) == 0);

    capture_t cap; capture_init(&cap);
    miniserver_arg_t marg = { p.server_t, &cap, /*send=*/1, /*notif=*/0 };
    pthread_t th;
    pthread_create(&th, NULL, miniserver_thread, &marg);

    cmcp_client_t *cli = cmcp_client_new("host", "0.0.1");
    TEST_ASSERT(cmcp_client_set_roots(cli, NULL, 0) == CMCP_OK);
    TEST_ASSERT(cmcp_client_handshake(cli, p.client_t) == CMCP_OK);

    TEST_ASSERT(wait_done(&cap));
    TEST_ASSERT(cap.roots_advertised == 1);

    const cmcp_json_t *arr = cmcp_json_object_get(cap.captured.result, "roots");
    TEST_ASSERT(arr && arr->type == CMCP_JSON_ARRAY);
    TEST_ASSERT(cmcp_json_array_len(arr) == 0);

    cmcp_client_free(cli);
    cmcp_transport_close(p.client_t);
    pthread_join(th, NULL);
    cmcp_transport_close(p.server_t);
    capture_clear(&cap);
}

static void test_set_roots_replaces(void) {
    /* Calling set_roots twice replaces the prior set. */
    transport_pair_t p;
    TEST_ASSERT(make_pair(&p) == 0);

    capture_t cap; capture_init(&cap);
    miniserver_arg_t marg = { p.server_t, &cap, /*send=*/1, /*notif=*/0 };
    pthread_t th;
    pthread_create(&th, NULL, miniserver_thread, &marg);

    cmcp_client_t *cli = cmcp_client_new("host", "0.0.1");
    cmcp_root_t initial[] = { { "file:///old", NULL } };
    cmcp_root_t replaced[] = {
        { "file:///new1", "first"  },
        { "file:///new2", "second" },
    };
    TEST_ASSERT(cmcp_client_set_roots(cli, initial,  1) == CMCP_OK);
    TEST_ASSERT(cmcp_client_set_roots(cli, replaced, 2) == CMCP_OK);
    TEST_ASSERT(cmcp_client_handshake(cli, p.client_t) == CMCP_OK);

    TEST_ASSERT(wait_done(&cap));
    const cmcp_json_t *arr = cmcp_json_object_get(cap.captured.result, "roots");
    TEST_ASSERT(arr && cmcp_json_array_len(arr) == 2);
    const cmcp_json_t *r0 = cmcp_json_array_at(arr, 0);
    const cmcp_json_t *u0 = cmcp_json_object_get(r0, "uri");
    TEST_ASSERT(u0 && strcmp(u0->str.s, "file:///new1") == 0);

    cmcp_client_free(cli);
    cmcp_transport_close(p.client_t);
    pthread_join(th, NULL);
    cmcp_transport_close(p.server_t);
    capture_clear(&cap);
}

static void test_list_changed_advertised_and_emitted(void) {
    /* Cap advertise listChanged, then update roots and call notify;
     * server sees `notifications/roots/list_changed`. */
    transport_pair_t p;
    TEST_ASSERT(make_pair(&p) == 0);

    capture_t cap; capture_init(&cap);
    miniserver_arg_t marg = { p.server_t, &cap, /*send=*/0, /*notif=*/1 };
    pthread_t th;
    pthread_create(&th, NULL, miniserver_thread, &marg);

    cmcp_client_t *cli = cmcp_client_new("host", "0.0.1");
    cmcp_client_set_capabilities(cli, &(cmcp_client_capabilities_t){
        .roots_list_changed = 1,
    });
    cmcp_root_t r1[] = { { "file:///a", NULL } };
    TEST_ASSERT(cmcp_client_set_roots(cli, r1, 1) == CMCP_OK);
    TEST_ASSERT(cmcp_client_handshake(cli, p.client_t) == CMCP_OK);

    /* Wait for the mini-server to record the captured caps. */
    TEST_ASSERT(wait_done(&cap));
    TEST_ASSERT(cap.roots_advertised   == 1);
    TEST_ASSERT(cap.roots_list_changed == 1);

    /* Update roots and emit. */
    cmcp_root_t r2[] = {
        { "file:///a", NULL },
        { "file:///b", NULL },
    };
    TEST_ASSERT(cmcp_client_set_roots(cli, r2, 2) == CMCP_OK);
    TEST_ASSERT(cmcp_client_notify_roots_changed(cli) == CMCP_OK);

    TEST_ASSERT(wait_notif(&cap));
    pthread_mutex_lock(&cap.mu);
    TEST_ASSERT(cap.notif_method != NULL);
    TEST_ASSERT(cap.notif_method &&
                strcmp(cap.notif_method,
                       "notifications/roots/list_changed") == 0);
    pthread_mutex_unlock(&cap.mu);

    cmcp_client_free(cli);
    cmcp_transport_close(p.client_t);
    pthread_join(th, NULL);
    cmcp_transport_close(p.server_t);
    capture_clear(&cap);
}

static void test_notify_without_cap_returns_eprotocol(void) {
    /* No caps.roots_list_changed → notify_roots_changed must refuse. */
    cmcp_client_t *cli = cmcp_client_new("host", "0.0.1");
    cmcp_client_set_roots(cli, NULL, 0);
    int rc = cmcp_client_notify_roots_changed(cli);
    TEST_ASSERT(rc == CMCP_EPROTOCOL);
    cmcp_client_free(cli);
}

static void test_set_roots_rejects_null_uri(void) {
    /* A root entry with NULL uri is invalid. */
    cmcp_client_t *cli = cmcp_client_new("host", "0.0.1");
    cmcp_root_t bad[] = { { NULL, "no uri" } };
    int rc = cmcp_client_set_roots(cli, bad, 1);
    TEST_ASSERT(rc == CMCP_EINVAL);
    cmcp_client_free(cli);
}

/* ====================================================================== */

int main(void) {
    fprintf(stderr, "test_roots:\n");

    TEST_RUN(test_no_set_roots_no_cap_and_method_not_found);
    TEST_RUN(test_set_roots_advertises_cap_and_returns_list);
    TEST_RUN(test_set_roots_empty_array);
    TEST_RUN(test_set_roots_replaces);
    TEST_RUN(test_list_changed_advertised_and_emitted);
    TEST_RUN(test_notify_without_cap_returns_eprotocol);
    TEST_RUN(test_set_roots_rejects_null_uri);

    TEST_DONE();
}
