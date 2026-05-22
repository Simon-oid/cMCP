/* Worker-pool tests — Phase 3.4 + 3.5.
 *
 * Exercises the server-side concurrency machinery end to end:
 *   - several handler-invoking requests run *concurrently* on the pool
 *     and complete out of submission order;
 *   - a cooperatively-cancelled request observes the cancel flag and
 *     gets NO response, per the MCP spec;
 *   - notifications/progress emitted from inside a handler reach the
 *     wire with the caller's progressToken;
 *   - closing the transport mid-handler unwinds cleanly — the run loop
 *     cancels every in-flight request, the pool drains, and
 *     cmcp_server_run returns without hanging.
 *
 * Unlike test_client_server, these tests drive the server over a raw
 * pipe and speak JSON-RPC by hand: the cancellation test needs to
 * assert the *absence* of a response frame, which the client library
 * (no wait-with-timeout) cannot observe. */

#define _POSIX_C_SOURCE 200809L

#include "test.h"
#include "cmcp.h"
#include "cmcp_json.h"
#include "cmcp_server.h"
#include "cmcp_transport.h"
#include "cmcp_types.h"

#include <errno.h>
#include <poll.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

/* ====================================================================== */
/* Raw-wire harness: a real cmcp_server_run on a pipe pair                  */
/* ====================================================================== */

typedef struct {
    int               to_server;    /* test → server (write end)  */
    int               from_server;  /* server → test (read end)   */
    cmcp_server_t    *srv;
    cmcp_transport_t *srv_t;
    pthread_t         th;
    int               running;
} harness_t;

static int harness_init(harness_t *h) {
    memset(h, 0, sizeof *h);
    h->to_server = h->from_server = -1;
    int c2s[2], s2c[2];
    if (pipe(c2s) != 0) return -1;
    if (pipe(s2c) != 0) { close(c2s[0]); close(c2s[1]); return -1; }
    h->to_server   = c2s[1];
    h->from_server = s2c[0];
    h->srv_t = cmcp_transport_stdio_new_fds(c2s[0], s2c[1]);
    h->srv   = cmcp_server_new("workers-test", "0.0.1");
    return (h->srv_t && h->srv) ? 0 : -1;
}

static void *serve_main(void *arg) {
    harness_t *h = (harness_t *)arg;
    cmcp_server_run(h->srv, h->srv_t);
    return NULL;
}

/* Write one newline-delimited JSON-RPC frame. */
static int send_frame(int fd, const char *json) {
    size_t n = strlen(json);
    if (write(fd, json, n) != (ssize_t)n) return -1;
    return write(fd, "\n", 1) == 1 ? 0 : -1;
}

/* Read one newline-delimited frame. Returns a malloc'd string with the
 * trailing newline stripped, or NULL on timeout / EOF. */
static char *recv_frame(int fd, int timeout_ms) {
    size_t cap = 256, len = 0;
    char *buf = (char *)malloc(cap);
    if (!buf) return NULL;
    for (;;) {
        struct pollfd pfd = { fd, POLLIN, 0 };
        if (poll(&pfd, 1, timeout_ms) <= 0) { free(buf); return NULL; }
        char c;
        if (read(fd, &c, 1) <= 0) { free(buf); return NULL; }
        if (c == '\n') {
            if (len == 0) continue;          /* skip blank lines */
            buf[len] = '\0';
            return buf;
        }
        if (len + 1 >= cap) {
            char *nb = (char *)realloc(buf, cap *= 2);
            if (!nb) { free(buf); return NULL; }
            buf = nb;
        }
        buf[len++] = c;
    }
}

/* initialize → consume result → notifications/initialized. */
static int do_handshake(harness_t *h) {
    char init[256];
    snprintf(init, sizeof init,
        "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"initialize\","
        "\"params\":{\"protocolVersion\":\"%s\",\"capabilities\":{},"
        "\"clientInfo\":{\"name\":\"test\",\"version\":\"0\"}}}",
        CMCP_PROTOCOL_VERSION);
    if (send_frame(h->to_server, init) != 0) return -1;
    char *resp = recv_frame(h->from_server, 2000);
    if (!resp) return -1;
    free(resp);
    return send_frame(h->to_server,
        "{\"jsonrpc\":\"2.0\",\"method\":\"notifications/initialized\"}");
}

static int harness_serve(harness_t *h) {
    if (pthread_create(&h->th, NULL, serve_main, h) != 0) return -1;
    h->running = 1;
    return do_handshake(h);
}

static void harness_stop(harness_t *h) {
    /* Closing the write end gives the server EOF → run loop exits. */
    if (h->to_server >= 0) { close(h->to_server); h->to_server = -1; }
    if (h->running) pthread_join(h->th, NULL);
    if (h->from_server >= 0) { close(h->from_server); h->from_server = -1; }
    cmcp_transport_close(h->srv_t);
    cmcp_server_free(h->srv);
}

/* Build + send a tools/call. `meta` is appended verbatim after the
 * arguments value (e.g. ",\"_meta\":{...}") or may be NULL. */
static void send_call(int fd, long long id, const char *tool,
                      const char *args_json, const char *meta) {
    char buf[512];
    snprintf(buf, sizeof buf,
        "{\"jsonrpc\":\"2.0\",\"id\":%lld,\"method\":\"tools/call\","
        "\"params\":{\"name\":\"%s\",\"arguments\":%s%s}}",
        id, tool, args_json, meta ? meta : "");
    send_frame(fd, buf);
}

/* The "id" of a parsed frame, or -1 if absent / not an int. */
static long long frame_id(const char *frame) {
    cmcp_json_t *j = cmcp_json_parse(frame, strlen(frame));
    if (!j) return -1;
    const cmcp_json_t *v = cmcp_json_object_get(j, "id");
    long long id = (v && v->type == CMCP_JSON_INT) ? v->i : -1;
    cmcp_json_free(j);
    return id;
}

/* 1 if `frame` is a success response for `id` (has a result object). */
static int frame_is_result(const char *frame, long long id) {
    cmcp_json_t *j = cmcp_json_parse(frame, strlen(frame));
    if (!j) return 0;
    const cmcp_json_t *v = cmcp_json_object_get(j, "id");
    const cmcp_json_t *r = cmcp_json_object_get(j, "result");
    int ok = v && v->type == CMCP_JSON_INT && v->i == id && r != NULL;
    cmcp_json_free(j);
    return ok;
}

/* ====================================================================== */
/* Test tools                                                              */
/* ====================================================================== */

/* Barrier tool: every concurrent invocation parks until N of them have
 * arrived. It can only ever release if the pool runs N handlers at
 * once — a serial dispatcher would deadlock here (and time out). */
static pthread_mutex_t g_bar_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t  g_bar_cv = PTHREAD_COND_INITIALIZER;
static int g_bar_count;
static int g_bar_target;
static int g_bar_timed_out;

static int barrier_tool(const cmcp_json_t *args, void *ud,
                         cmcp_handler_ctx_t *hctx,
                         cmcp_json_t **out, int *is_err) {
    (void)args; (void)ud; (void)hctx; (void)is_err;
    pthread_mutex_lock(&g_bar_mu);
    g_bar_count++;
    pthread_cond_broadcast(&g_bar_cv);
    struct timespec dl;
    clock_gettime(CLOCK_REALTIME, &dl);
    dl.tv_sec += 3;                       /* generous: a healthy pool is instant */
    while (g_bar_count < g_bar_target) {
        if (pthread_cond_timedwait(&g_bar_cv, &g_bar_mu, &dl) == ETIMEDOUT) {
            g_bar_timed_out = 1;
            break;
        }
    }
    pthread_mutex_unlock(&g_bar_mu);
    *out = cmcp_tool_text_content("done");
    return *out ? CMCP_OK : CMCP_ENOMEM;
}

/* Cooperative tool: spins checking the cancel flag, ~3s ceiling so a
 * never-delivered cancel fails the test instead of hanging. Records
 * observed cancellation into the int that `userdata` points at. */
static int coop_tool(const cmcp_json_t *args, void *ud,
                     cmcp_handler_ctx_t *hctx,
                     cmcp_json_t **out, int *is_err) {
    (void)args; (void)is_err;
    int *observed = (int *)ud;
    for (int i = 0; i < 600; i++) {
        if (cmcp_handler_cancelled(hctx)) {
            if (observed) *observed = 1;
            break;
        }
        struct timespec ts = { 0, 5 * 1000 * 1000 };   /* 5ms */
        nanosleep(&ts, NULL);
    }
    *out = cmcp_tool_text_content("done");
    return *out ? CMCP_OK : CMCP_ENOMEM;
}

/* Fast tool — a liveness probe. */
static int ping_tool(const cmcp_json_t *args, void *ud,
                     cmcp_handler_ctx_t *hctx,
                     cmcp_json_t **out, int *is_err) {
    (void)args; (void)ud; (void)hctx; (void)is_err;
    *out = cmcp_tool_text_content("pong");
    return *out ? CMCP_OK : CMCP_ENOMEM;
}

/* Emits three progress updates, then returns. */
static int progress_tool(const cmcp_json_t *args, void *ud,
                          cmcp_handler_ctx_t *hctx,
                          cmcp_json_t **out, int *is_err) {
    (void)args; (void)ud; (void)is_err;
    for (int i = 1; i <= 3; i++)
        cmcp_handler_progress(hctx, (double)i, 3.0, "step");
    *out = cmcp_tool_text_content("done");
    return *out ? CMCP_OK : CMCP_ENOMEM;
}

static int add_tool(cmcp_server_t *s, const char *name,
                    cmcp_tool_handler_fn fn, void *userdata) {
    cmcp_tool_t t = {0};
    t.name         = name;
    t.handler      = fn;
    t.userdata     = userdata;
    t.input_schema = "{\"type\":\"object\"}";
    return cmcp_server_add_tool(s, &t);
}

/* ====================================================================== */
/* test_concurrent_out_of_order                                            */
/* ====================================================================== */

static void test_concurrent_out_of_order(void) {
    g_bar_count = 0; g_bar_target = 6; g_bar_timed_out = 0;

    harness_t h;
    TEST_ASSERT(harness_init(&h) == 0);
    TEST_ASSERT(add_tool(h.srv, "barrier", barrier_tool, NULL) == CMCP_OK);
    TEST_ASSERT(harness_serve(&h) == 0);

    /* Six calls in flight at once. */
    for (int i = 0; i < 6; i++)
        send_call(h.to_server, 100 + i, "barrier", "{}", NULL);

    int seen[6] = {0};
    int got = 0;
    for (int i = 0; i < 6; i++) {
        char *f = recv_frame(h.from_server, 5000);
        TEST_ASSERT(f != NULL);
        if (!f) break;
        long long id = frame_id(f);
        if (id >= 100 && id < 106) { seen[id - 100] = 1; got++; }
        free(f);
    }
    TEST_ASSERT(got == 6);
    for (int i = 0; i < 6; i++) TEST_ASSERT(seen[i] == 1);
    /* The barrier released → six handlers ran simultaneously. */
    TEST_ASSERT(g_bar_timed_out == 0);

    harness_stop(&h);
}

/* ====================================================================== */
/* test_cooperative_cancellation                                           */
/* ====================================================================== */

static void test_cooperative_cancellation(void) {
    int observed = 0;

    harness_t h;
    TEST_ASSERT(harness_init(&h) == 0);
    TEST_ASSERT(add_tool(h.srv, "coop", coop_tool, &observed) == CMCP_OK);
    TEST_ASSERT(add_tool(h.srv, "ping", ping_tool, NULL) == CMCP_OK);
    TEST_ASSERT(harness_serve(&h) == 0);

    send_call(h.to_server, 42, "coop", "{}", NULL);
    struct timespec ts = { 0, 40 * 1000 * 1000 };   /* let the worker enter */
    nanosleep(&ts, NULL);
    send_frame(h.to_server,
        "{\"jsonrpc\":\"2.0\",\"method\":\"notifications/cancelled\","
        "\"params\":{\"requestId\":42}}");

    /* Per spec, a cancelled request SHOULD NOT receive a response. */
    char *f = recv_frame(h.from_server, 1500);
    TEST_ASSERT(f == NULL);
    free(f);
    TEST_ASSERT(observed == 1);

    /* The server is still healthy: a fresh call still completes. */
    send_call(h.to_server, 43, "ping", "{}", NULL);
    char *r = recv_frame(h.from_server, 2000);
    TEST_ASSERT(r != NULL);
    if (r) {
        TEST_ASSERT(frame_is_result(r, 43));
        free(r);
    }

    harness_stop(&h);
}

/* ====================================================================== */
/* test_progress_notifications                                             */
/* ====================================================================== */

static void test_progress_notifications(void) {
    harness_t h;
    TEST_ASSERT(harness_init(&h) == 0);
    TEST_ASSERT(add_tool(h.srv, "prog", progress_tool, NULL) == CMCP_OK);
    TEST_ASSERT(harness_serve(&h) == 0);

    send_call(h.to_server, 70, "prog", "{}",
              ",\"_meta\":{\"progressToken\":\"ptok\"}");

    int progress = 0, got_resp = 0;
    for (int i = 0; i < 10 && !got_resp; i++) {
        char *f = recv_frame(h.from_server, 3000);
        TEST_ASSERT(f != NULL);
        if (!f) break;
        cmcp_json_t *j = cmcp_json_parse(f, strlen(f));
        if (j) {
            const cmcp_json_t *method = cmcp_json_object_get(j, "method");
            const cmcp_json_t *id     = cmcp_json_object_get(j, "id");
            if (method && method->type == CMCP_JSON_STRING &&
                strcmp(method->str.s, "notifications/progress") == 0) {
                const cmcp_json_t *p = cmcp_json_object_get(j, "params");
                const cmcp_json_t *tok = p
                    ? cmcp_json_object_get(p, "progressToken") : NULL;
                if (tok && tok->type == CMCP_JSON_STRING &&
                    strcmp(tok->str.s, "ptok") == 0)
                    progress++;
            } else if (id && id->type == CMCP_JSON_INT && id->i == 70) {
                got_resp = 1;
            }
            cmcp_json_free(j);
        }
        free(f);
    }
    TEST_ASSERT(got_resp == 1);
    TEST_ASSERT(progress == 3);

    harness_stop(&h);
}

/* ====================================================================== */
/* test_transport_close_mid_handler                                        */
/* ====================================================================== */

static void test_transport_close_mid_handler(void) {
    int observed = 0;

    harness_t h;
    TEST_ASSERT(harness_init(&h) == 0);
    TEST_ASSERT(add_tool(h.srv, "coop", coop_tool, &observed) == CMCP_OK);
    TEST_ASSERT(harness_serve(&h) == 0);

    send_call(h.to_server, 80, "coop", "{}", NULL);
    struct timespec ts = { 0, 60 * 1000 * 1000 };   /* let the worker enter */
    nanosleep(&ts, NULL);

    /* Slam the transport shut while the handler is still running.
     * harness_stop joins the server thread: if the run loop failed to
     * cancel in-flight work and drain the pool, this hangs and the
     * `make test` timeout surfaces it. */
    harness_stop(&h);

    /* Shutdown flagged the in-flight handler as cancelled. */
    TEST_ASSERT(observed == 1);
}

/* ====================================================================== */

int main(void) {
    /* 8 workers > the 6-way barrier in test_concurrent_out_of_order. */
    setenv("CMCP_WORKERS", "8", 1);

    fprintf(stderr, "test_workers:\n");

    TEST_RUN(test_concurrent_out_of_order);
    TEST_RUN(test_cooperative_cancellation);
    TEST_RUN(test_progress_notifications);
    TEST_RUN(test_transport_close_mid_handler);

    TEST_DONE();
}
