/* Phase 4.1 — `ping` over the wire, both directions.
 *
 * The spec makes answering a `ping` request mandatory for whichever
 * party receives it (an empty result, never -32601). This test proves
 * both halves:
 *   A. a cMCP server answers a client `ping`.
 *   B. a cMCP client answers a server `ping`.
 *
 * Direction A uses a real cmcp_server. Direction B hand-rolls a
 * mini-server (same pattern as test_sampling.c) since cmcp_server_t
 * does not initiate server->client requests.
 */

#define _POSIX_C_SOURCE 200809L

#include "test.h"
#include "cmcp.h"
#include "cmcp_client.h"
#include "cmcp_server.h"
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
/* Direction A — a real cMCP server answers a client `ping`                */
/* ====================================================================== */

typedef struct {
    cmcp_server_t    *s;
    cmcp_transport_t *t;
    int               rc;
} server_arg_t;

static void *server_thread(void *arg) {
    server_arg_t *a = (server_arg_t *)arg;
    a->rc = cmcp_server_run(a->s, a->t);
    return NULL;
}

static void test_server_answers_ping(void) {
    transport_pair_t p;
    TEST_ASSERT(make_pair(&p) == 0);

    cmcp_server_t *srv = cmcp_server_new("ping-server", "0.1.0");
    server_arg_t sa = { srv, p.server_t, 0 };
    pthread_t th;
    TEST_ASSERT(pthread_create(&th, NULL, server_thread, &sa) == 0);

    cmcp_client_t *cli = cmcp_client_new("ping-client", "0.0.1");
    TEST_ASSERT(cmcp_client_handshake(cli, p.client_t) == CMCP_OK);

    /* A bare `ping` with no params must come back as a result, not an
     * error. The result is an (empty) object per spec. */
    cmcp_rpc_message_t resp;
    TEST_ASSERT(cmcp_client_request(cli, "ping", NULL, &resp) == CMCP_OK);
    TEST_ASSERT(resp.kind  == CMCP_MSG_RESPONSE);
    TEST_ASSERT(resp.error == NULL);
    TEST_ASSERT(resp.result != NULL && resp.result->type == CMCP_JSON_OBJECT);
    cmcp_rpc_message_clear(&resp);

    cmcp_client_free(cli);
    cmcp_transport_close(p.client_t);
    pthread_join(th, NULL);
    cmcp_server_free(srv);
    cmcp_transport_close(p.server_t);
}

/* ====================================================================== */
/* Direction B — a real cMCP client answers a server `ping`                */
/* ====================================================================== */

typedef struct {
    pthread_mutex_t    mu;
    pthread_cond_t     cv;
    int                done;
    cmcp_rpc_message_t captured;   /* moved-out copy of the client's reply */
} ping_capture_t;

static void capture_init(ping_capture_t *c) {
    pthread_mutex_init(&c->mu, NULL);
    pthread_cond_init(&c->cv, NULL);
    c->done = 0;
    cmcp_rpc_message_init(&c->captured);
}

static void capture_clear(ping_capture_t *c) {
    pthread_mutex_destroy(&c->mu);
    pthread_cond_destroy(&c->cv);
    cmcp_rpc_message_clear(&c->captured);
}

typedef struct {
    cmcp_transport_t *t;
    ping_capture_t   *cap;
} miniserver_arg_t;

/* Mini-server: handshake, then send a `ping` request and capture the
 * client's reply. */
static void *miniserver_thread(void *arg) {
    miniserver_arg_t *a = (miniserver_arg_t *)arg;
    cmcp_transport_t *t = a->t;
    char *frame = NULL; size_t flen = 0;

    /* 1. Read initialize. */
    if (cmcp_transport_read(t, &frame, &flen) != CMCP_OK) return NULL;
    cmcp_rpc_message_t *msgs = NULL; size_t nmsgs = 0;
    int prc = cmcp_rpc_parse(frame, flen, &msgs, &nmsgs);
    free(frame);
    if (prc != CMCP_OK || nmsgs != 1) {
        cmcp_rpc_messages_free(msgs, nmsgs);
        return NULL;
    }

    /* 2. Reply minimal initialize success. */
    cmcp_json_t *result = cmcp_json_new_object();
    cmcp_json_object_set(result, "protocolVersion",
                          cmcp_json_new_string(CMCP_PROTOCOL_VERSION));
    cmcp_json_object_set(result, "capabilities", cmcp_json_new_object());
    cmcp_json_t *si = cmcp_json_new_object();
    cmcp_json_object_set(si, "name",    cmcp_json_new_string("ping-srv"));
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

    /* 4. Send a `ping` request — no params, id picked by the server. */
    cmcp_rpc_message_t req;
    cmcp_rpc_message_init(&req);
    cmcp_rpc_make_request(&req, 99, "ping", NULL);
    wire = cmcp_rpc_emit(&req);
    cmcp_transport_write(t, wire, strlen(wire));
    free(wire);
    cmcp_rpc_message_clear(&req);

    /* 5. Capture the client's reply. */
    if (cmcp_transport_read(t, &frame, &flen) != CMCP_OK) return NULL;
    cmcp_rpc_message_t *rsps = NULL; size_t nr = 0;
    int rrc = cmcp_rpc_parse(frame, flen, &rsps, &nr);
    free(frame);

    pthread_mutex_lock(&a->cap->mu);
    if (rrc == CMCP_OK && nr == 1) {
        a->cap->captured = rsps[0];        /* move */
        cmcp_rpc_message_init(&rsps[0]);   /* prevent double-free */
    }
    a->cap->done = 1;
    pthread_cond_broadcast(&a->cap->cv);
    pthread_mutex_unlock(&a->cap->mu);
    cmcp_rpc_messages_free(rsps, nr);

    /* 6. Drain until the client closes. */
    while (cmcp_transport_read(t, &frame, &flen) == CMCP_OK) free(frame);
    return NULL;
}

static int wait_for_capture(ping_capture_t *c) {
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

static void test_client_answers_ping(void) {
    transport_pair_t p;
    TEST_ASSERT(make_pair(&p) == 0);

    ping_capture_t cap; capture_init(&cap);
    miniserver_arg_t marg = { p.server_t, &cap };
    pthread_t th;
    TEST_ASSERT(pthread_create(&th, NULL, miniserver_thread, &marg) == 0);

    cmcp_client_t *cli = cmcp_client_new("ping-host", "0.0.1");
    TEST_ASSERT(cmcp_client_handshake(cli, p.client_t) == CMCP_OK);

    TEST_ASSERT(wait_for_capture(&cap));
    TEST_ASSERT(cap.captured.kind  == CMCP_MSG_RESPONSE);
    TEST_ASSERT(cap.captured.error == NULL);
    TEST_ASSERT(cap.captured.result != NULL &&
                cap.captured.result->type == CMCP_JSON_OBJECT);

    cmcp_client_free(cli);
    cmcp_transport_close(p.client_t);
    pthread_join(th, NULL);
    cmcp_transport_close(p.server_t);
    capture_clear(&cap);
}

int main(void) {
    fprintf(stderr, "test_ping:\n");
    TEST_RUN(test_server_answers_ping);
    TEST_RUN(test_client_answers_ping);
    TEST_DONE();
}
