/* End-to-end tests for the Phase 4.5 host-side ergonomics:
 *
 *   - cmcp_client_cancel(c, id, reason) — emits notifications/cancelled
 *     for an in-flight call AND unblocks the waiter with CMCP_ECANCELLED.
 *
 *   - cmcp_client_call_async_progress(c, method, params, fn, ud, &id) —
 *     attaches a per-call progress callback that catches matching
 *     notifications/progress frames; other progress frames still reach
 *     the generic notification handler.
 *
 * Both tests run a REAL cmcp_server in a thread (over a pipe pair) and
 * a REAL cmcp_client on the main thread, exercising the run loop's
 * worker pool, cancellation watchdog, and progress emission.
 */

#define _POSIX_C_SOURCE 200809L

#include "test.h"
#include "cmcp.h"
#include "cmcp_client.h"
#include "cmcp_json.h"
#include "cmcp_server.h"
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

typedef struct {
    cmcp_server_t    *s;
    cmcp_transport_t *t;
} server_run_arg_t;

static void *server_run_thread(void *arg) {
    server_run_arg_t *a = (server_run_arg_t *)arg;
    cmcp_server_run(a->s, a->t);
    return NULL;
}

static void sleep_ms(long ms) {
    struct timespec ts = { ms / 1000, (ms % 1000) * 1000000L };
    nanosleep(&ts, NULL);
}

/* ====================================================================== */
/* Tool 1: slow — polls cancellation, bails when set                       */
/* ====================================================================== */

typedef struct {
    int               observed_cancel;   /* set if handler saw cancel */
    pthread_mutex_t   mu;
} slow_ctx_t;

static int slow_tool(const cmcp_json_t *arguments, void *userdata,
                     cmcp_handler_ctx_t *hctx,
                     cmcp_json_t **out_content, int *out_is_error) {
    (void)arguments;
    slow_ctx_t *ctx = (slow_ctx_t *)userdata;

    /* Poll cancellation up to ~3 seconds (cap defends against a buggy
     * cancel path leaving the handler running indefinitely). */
    for (int i = 0; i < 60; i++) {
        if (cmcp_handler_cancelled(hctx)) {
            pthread_mutex_lock(&ctx->mu);
            ctx->observed_cancel = 1;
            pthread_mutex_unlock(&ctx->mu);
            *out_is_error = 1;
            *out_content  = cmcp_tool_text_content("cancelled");
            return CMCP_OK;
        }
        sleep_ms(50);
    }
    *out_is_error = 0;
    *out_content  = cmcp_tool_text_content("completed");
    return CMCP_OK;
}

/* ====================================================================== */
/* Tool 2: progress — emits 3 notifications/progress, then returns         */
/* ====================================================================== */

static int progress_tool(const cmcp_json_t *arguments, void *userdata,
                          cmcp_handler_ctx_t *hctx,
                          cmcp_json_t **out_content, int *out_is_error) {
    (void)arguments; (void)userdata;
    for (int i = 1; i <= 3; i++) {
        char msg[32];
        snprintf(msg, sizeof msg, "step %d", i);
        cmcp_handler_progress(hctx, (double)i, 3.0, msg);
        sleep_ms(20);
    }
    *out_is_error = 0;
    *out_content  = cmcp_tool_text_content("done");
    return CMCP_OK;
}

/* ====================================================================== */
/* Test 1: cancel an in-flight call                                        */
/* ====================================================================== */

static void test_cancel_in_flight(void) {
    transport_pair_t p = {0};
    TEST_ASSERT(make_pair(&p) == 0);

    slow_ctx_t sctx = {0};
    pthread_mutex_init(&sctx.mu, NULL);

    cmcp_server_t *srv = cmcp_server_new("cancel-srv", "0.1.0");
    TEST_ASSERT(cmcp_server_add_tool(srv, &(cmcp_tool_t){
        .name = "slow",
        .handler = slow_tool,
        .userdata = &sctx,
    }) == CMCP_OK);

    server_run_arg_t sa = { srv, p.server_t };
    pthread_t srv_th;
    TEST_ASSERT(pthread_create(&srv_th, NULL, server_run_thread, &sa) == 0);

    cmcp_client_t *cli = cmcp_client_new("host", "0.0.1");
    TEST_ASSERT(cmcp_client_handshake(cli, p.client_t) == CMCP_OK);

    cmcp_json_t *params = cmcp_json_new_object();
    cmcp_json_object_set(params, "name", cmcp_json_new_string("slow"));
    long long id = 0;
    TEST_ASSERT(cmcp_client_call_async(cli, "tools/call", params, &id) == CMCP_OK);
    TEST_ASSERT(id > 0);

    /* Let the handler actually start before cancelling. */
    sleep_ms(100);

    TEST_ASSERT(cmcp_client_cancel(cli, id, "user pressed Ctrl-C") == CMCP_OK);

    cmcp_rpc_message_t resp;
    cmcp_rpc_message_init(&resp);
    int wrc = cmcp_client_wait(cli, id, &resp);
    TEST_ASSERT(wrc == CMCP_ECANCELLED);
    cmcp_rpc_message_clear(&resp);

    /* Give the handler a moment to actually poll the cancel flag (the
     * server's run loop reads the notification, calls inflight_cancel,
     * and the next poll iteration in slow_tool sees it). */
    for (int i = 0; i < 40; i++) {
        pthread_mutex_lock(&sctx.mu);
        int seen = sctx.observed_cancel;
        pthread_mutex_unlock(&sctx.mu);
        if (seen) break;
        sleep_ms(50);
    }
    pthread_mutex_lock(&sctx.mu);
    TEST_ASSERT(sctx.observed_cancel == 1);
    pthread_mutex_unlock(&sctx.mu);

    /* Cancelling an already-completed (here: cancelled + reaped) id
     * must return CMCP_EINVAL. */
    TEST_ASSERT(cmcp_client_cancel(cli, id, NULL) == CMCP_EINVAL);

    cmcp_client_free(cli);
    cmcp_transport_close(p.client_t);
    pthread_join(srv_th, NULL);
    cmcp_transport_close(p.server_t);
    cmcp_server_free(srv);
    pthread_mutex_destroy(&sctx.mu);
}

/* ====================================================================== */
/* Test 2: progress notifications routed to per-call callback              */
/* ====================================================================== */

typedef struct {
    pthread_mutex_t mu;
    int             count;
    double          last_progress;
    double          last_total;
    char           *last_message;     /* owned strdup */
    void           *seen_ud;
} progress_capture_t;

static void on_progress(double progress, double total,
                         const char *message, void *userdata) {
    progress_capture_t *c = (progress_capture_t *)userdata;
    pthread_mutex_lock(&c->mu);
    c->count++;
    c->last_progress = progress;
    c->last_total    = total;
    free(c->last_message);
    c->last_message  = message ? strdup(message) : NULL;
    c->seen_ud       = userdata;
    pthread_mutex_unlock(&c->mu);
}

/* Generic-handler counter — should NOT see the progress frames; they
 * must be consumed by the per-call callback. */
typedef struct {
    pthread_mutex_t mu;
    int             progress_count;
    int             other_count;
} notif_counter_t;

static void on_any_notification(const char *method,
                                 const cmcp_json_t *params,
                                 void *userdata) {
    (void)params;
    notif_counter_t *n = (notif_counter_t *)userdata;
    pthread_mutex_lock(&n->mu);
    if (method && strcmp(method, "notifications/progress") == 0)
        n->progress_count++;
    else
        n->other_count++;
    pthread_mutex_unlock(&n->mu);
}

static void test_progress_per_call_callback(void) {
    transport_pair_t p = {0};
    TEST_ASSERT(make_pair(&p) == 0);

    cmcp_server_t *srv = cmcp_server_new("prog-srv", "0.1.0");
    TEST_ASSERT(cmcp_server_add_tool(srv, &(cmcp_tool_t){
        .name = "go",
        .handler = progress_tool,
        .userdata = NULL,
    }) == CMCP_OK);

    server_run_arg_t sa = { srv, p.server_t };
    pthread_t srv_th;
    TEST_ASSERT(pthread_create(&srv_th, NULL, server_run_thread, &sa) == 0);

    cmcp_client_t *cli = cmcp_client_new("host", "0.0.1");
    notif_counter_t ncount = {0};
    pthread_mutex_init(&ncount.mu, NULL);
    cmcp_client_set_notification_handler(cli, on_any_notification, &ncount);
    TEST_ASSERT(cmcp_client_handshake(cli, p.client_t) == CMCP_OK);

    progress_capture_t pc = {0};
    pthread_mutex_init(&pc.mu, NULL);

    cmcp_json_t *params = cmcp_json_new_object();
    cmcp_json_object_set(params, "name", cmcp_json_new_string("go"));
    long long id = 0;
    TEST_ASSERT(cmcp_client_call_async_progress(
        cli, "tools/call", params, on_progress, &pc, &id) == CMCP_OK);

    cmcp_rpc_message_t resp;
    cmcp_rpc_message_init(&resp);
    TEST_ASSERT(cmcp_client_wait(cli, id, &resp) == CMCP_OK);
    TEST_ASSERT(resp.error == NULL);
    cmcp_rpc_message_clear(&resp);

    pthread_mutex_lock(&pc.mu);
    TEST_ASSERT(pc.count == 3);
    TEST_ASSERT(pc.last_progress == 3.0);
    TEST_ASSERT(pc.last_total    == 3.0);
    TEST_ASSERT(pc.last_message && strcmp(pc.last_message, "step 3") == 0);
    TEST_ASSERT(pc.seen_ud == &pc);
    pthread_mutex_unlock(&pc.mu);

    /* Generic handler must NOT have seen any progress (the per-call
     * callback consumed them all). */
    pthread_mutex_lock(&ncount.mu);
    TEST_ASSERT(ncount.progress_count == 0);
    pthread_mutex_unlock(&ncount.mu);

    cmcp_client_free(cli);
    cmcp_transport_close(p.client_t);
    pthread_join(srv_th, NULL);
    cmcp_transport_close(p.server_t);
    cmcp_server_free(srv);
    free(pc.last_message);
    pthread_mutex_destroy(&pc.mu);
    pthread_mutex_destroy(&ncount.mu);
}

/* ====================================================================== */
/* Test 3: unknown id → CMCP_EINVAL                                        */
/* ====================================================================== */

static void test_cancel_unknown_id(void) {
    transport_pair_t p = {0};
    TEST_ASSERT(make_pair(&p) == 0);

    cmcp_server_t *srv = cmcp_server_new("cancel-srv", "0.1.0");
    server_run_arg_t sa = { srv, p.server_t };
    pthread_t srv_th;
    pthread_create(&srv_th, NULL, server_run_thread, &sa);

    cmcp_client_t *cli = cmcp_client_new("host", "0.0.1");
    TEST_ASSERT(cmcp_client_handshake(cli, p.client_t) == CMCP_OK);

    TEST_ASSERT(cmcp_client_cancel(cli, 99999, NULL) == CMCP_EINVAL);

    cmcp_client_free(cli);
    cmcp_transport_close(p.client_t);
    pthread_join(srv_th, NULL);
    cmcp_transport_close(p.server_t);
    cmcp_server_free(srv);
}

/* ====================================================================== */
/* Test 4: progress for an unsubscribed token falls through to generic     */
/* ====================================================================== */

static void test_unmatched_progress_falls_through(void) {
    /* Caller doesn't use call_async_progress at all but the server
     * (well, a mini-server) emits a progress notification with a token
     * — the generic handler should see it. We synthesise the notification
     * from the server side via cmcp_server_notify since no in-tree
     * helper emits a free-floating progress. */
    transport_pair_t p = {0};
    TEST_ASSERT(make_pair(&p) == 0);

    cmcp_server_t *srv = cmcp_server_new("prog-srv", "0.1.0");
    /* No tool needed — we'll emit the notification directly. */
    server_run_arg_t sa = { srv, p.server_t };
    pthread_t srv_th;
    pthread_create(&srv_th, NULL, server_run_thread, &sa);

    cmcp_client_t *cli = cmcp_client_new("host", "0.0.1");
    notif_counter_t ncount = {0};
    pthread_mutex_init(&ncount.mu, NULL);
    cmcp_client_set_notification_handler(cli, on_any_notification, &ncount);
    TEST_ASSERT(cmcp_client_handshake(cli, p.client_t) == CMCP_OK);

    /* Build a progress notification with a token nobody is subscribed
     * to and inject it via cmcp_server_notify (which is the same
     * codepath the server uses for cmcp_handler_progress). */
    cmcp_json_t *params = cmcp_json_new_object();
    cmcp_json_object_set(params, "progressToken", cmcp_json_new_int(99));
    cmcp_json_object_set(params, "progress",      cmcp_json_new_double(0.5));
    TEST_ASSERT(cmcp_server_notify(srv, "notifications/progress", params) == CMCP_OK);

    /* Give the reader thread a beat to process the frame. */
    sleep_ms(80);

    pthread_mutex_lock(&ncount.mu);
    TEST_ASSERT(ncount.progress_count == 1);
    pthread_mutex_unlock(&ncount.mu);

    cmcp_client_free(cli);
    cmcp_transport_close(p.client_t);
    pthread_join(srv_th, NULL);
    cmcp_transport_close(p.server_t);
    cmcp_server_free(srv);
    pthread_mutex_destroy(&ncount.mu);
}

/* ====================================================================== */

int main(void) {
    fprintf(stderr, "test_host_cancel_progress:\n");
    TEST_RUN(test_cancel_in_flight);
    TEST_RUN(test_progress_per_call_callback);
    TEST_RUN(test_cancel_unknown_id);
    TEST_RUN(test_unmatched_progress_falls_through);
    TEST_DONE();
}
