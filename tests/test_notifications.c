/* End-to-end tests for the Phase 2.4 server-initiated notification
 * surface.
 *
 * Server emits via cmcp_server_notify (and the capability-gated
 * wrappers); client picks them up via the existing
 * cmcp_client_set_notification_handler from Phase 1.9.
 *
 * Two transport profiles are exercised:
 *
 *   - stdio: handler emits inline, response follows on the same wire.
 *   - HTTP:  notification is routed onto the held-open SSE channel by
 *            transport_http's write_fn classifier.
 */

#define _POSIX_C_SOURCE 200809L

#include "test.h"
#include "cmcp.h"
#include "cmcp_client.h"
#include "cmcp_json.h"
#include "cmcp_server.h"
#include "cmcp_transport.h"
#include "cmcp_types.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

/* ====================================================================== */
/* Pipe-pair scaffolding (mirrors test_resources_prompts).                 */
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
    int               rc;
} server_arg_t;

static void *server_thread(void *arg) {
    server_arg_t *a = (server_arg_t *)arg;
    a->rc = cmcp_server_run(a->s, a->t);
    return NULL;
}

/* ====================================================================== */
/* Notification capture                                                    */
/* ====================================================================== */

typedef struct notif_record {
    char                *method;
    cmcp_json_t         *params;     /* deep-cloned */
    struct notif_record *next;
} notif_record_t;

typedef struct {
    pthread_mutex_t   mu;
    pthread_cond_t    cv;
    notif_record_t   *head;
    notif_record_t   *tail;
    size_t            count;
} notif_capture_t;

static void capture_init(notif_capture_t *c) {
    pthread_mutex_init(&c->mu, NULL);
    pthread_cond_init(&c->cv, NULL);
    c->head = c->tail = NULL;
    c->count = 0;
}

static void capture_clear(notif_capture_t *c) {
    pthread_mutex_lock(&c->mu);
    notif_record_t *r = c->head;
    while (r) {
        notif_record_t *n = r->next;
        free(r->method);
        cmcp_json_free(r->params);
        free(r);
        r = n;
    }
    c->head = c->tail = NULL;
    c->count = 0;
    pthread_mutex_unlock(&c->mu);
    pthread_mutex_destroy(&c->mu);
    pthread_cond_destroy(&c->cv);
}

static void on_notification(const char *method,
                             const cmcp_json_t *params,
                             void *userdata) {
    notif_capture_t *cap = (notif_capture_t *)userdata;
    notif_record_t *r = (notif_record_t *)calloc(1, sizeof *r);
    if (!r) return;
    r->method = strdup(method);
    r->params = params ? cmcp_json_clone(params) : NULL;

    pthread_mutex_lock(&cap->mu);
    if (cap->tail) cap->tail->next = r;
    else           cap->head = r;
    cap->tail = r;
    cap->count++;
    pthread_cond_broadcast(&cap->cv);
    pthread_mutex_unlock(&cap->mu);
}

/* Wait until the capture has at least n notifications, or 2s elapses. */
static int capture_wait_count(notif_capture_t *cap, size_t n) {
    pthread_mutex_lock(&cap->mu);
    while (cap->count < n) {
        struct timespec ts;
        clock_gettime(CLOCK_REALTIME, &ts);
        ts.tv_sec += 2;
        if (pthread_cond_timedwait(&cap->cv, &cap->mu, &ts) != 0) break;
    }
    int hit = (cap->count >= n);
    pthread_mutex_unlock(&cap->mu);
    return hit;
}

/* ====================================================================== */
/* Tools that emit notifications when invoked                              */
/* ====================================================================== */

/* Each "trigger" tool emits a specific notification kind. The handler
 * runs on the server's run-loop thread, so the notification is
 * written on the wire BEFORE the tool's response. The client reader
 * therefore observes notification → response. */

typedef struct {
    cmcp_server_t *server;
} trigger_ctx_t;

static int trig_resources_changed(const cmcp_json_t *args, void *userdata,
                                   cmcp_json_t **out_content, int *out_is_error) {
    (void)args;
    trigger_ctx_t *c = (trigger_ctx_t *)userdata;
    *out_is_error = 0;
    int rc = cmcp_server_notify_resources_changed(c->server);
    *out_content = cmcp_tool_text_content(rc == CMCP_OK ? "emitted" : "skipped");
    return CMCP_OK;
}

static int trig_tools_changed(const cmcp_json_t *args, void *userdata,
                               cmcp_json_t **out_content, int *out_is_error) {
    (void)args;
    trigger_ctx_t *c = (trigger_ctx_t *)userdata;
    *out_is_error = 0;
    cmcp_server_notify_tools_changed(c->server);
    *out_content = cmcp_tool_text_content("ok");
    return CMCP_OK;
}

static int trig_prompts_changed(const cmcp_json_t *args, void *userdata,
                                 cmcp_json_t **out_content, int *out_is_error) {
    (void)args;
    trigger_ctx_t *c = (trigger_ctx_t *)userdata;
    *out_is_error = 0;
    cmcp_server_notify_prompts_changed(c->server);
    *out_content = cmcp_tool_text_content("ok");
    return CMCP_OK;
}

static int trig_resource_updated(const cmcp_json_t *args, void *userdata,
                                  cmcp_json_t **out_content, int *out_is_error) {
    trigger_ctx_t *c = (trigger_ctx_t *)userdata;
    *out_is_error = 0;
    const cmcp_json_t *uri = args ? cmcp_json_object_get(args, "uri") : NULL;
    int rc = CMCP_OK;
    if (uri && uri->type == CMCP_JSON_STRING) {
        rc = cmcp_server_notify_resource_updated(c->server, uri->str.s);
    }
    *out_content = cmcp_tool_text_content(rc == CMCP_OK ? "emitted-or-skipped"
                                                          : "rejected");
    return CMCP_OK;
}

/* Generic notifier — tool emits an arbitrary notification with a
 * caller-supplied method and params. Used for the progress-ordering
 * test below. */
static int trig_progress(const cmcp_json_t *args, void *userdata,
                          cmcp_json_t **out_content, int *out_is_error) {
    (void)args;
    trigger_ctx_t *c = (trigger_ctx_t *)userdata;
    *out_is_error = 0;
    cmcp_json_t *params = cmcp_json_new_object();
    cmcp_json_object_set(params, "progressToken", cmcp_json_new_string("p1"));
    cmcp_json_object_set(params, "progress", cmcp_json_new_double(0.5));
    cmcp_json_object_set(params, "total",    cmcp_json_new_double(1.0));
    cmcp_server_notify(c->server, "notifications/progress", params);
    *out_content = cmcp_tool_text_content("done");
    return CMCP_OK;
}

/* ====================================================================== */
/* Helpers                                                                 */
/* ====================================================================== */

static cmcp_json_t *call_args(const char *name, cmcp_json_t *arguments) {
    cmcp_json_t *p = cmcp_json_new_object();
    cmcp_json_object_set(p, "name", cmcp_json_new_string(name));
    if (arguments) cmcp_json_object_set(p, "arguments", arguments);
    return p;
}

/* Spec: a resource handler that's never actually read but exists so
 * subscribe() targets a registered URI. */
static int sample_read(const char *uri, void *userdata,
                        cmcp_json_t **out_contents, int *out_is_error) {
    (void)userdata; (void)uri;
    *out_is_error = 0;
    *out_contents = cmcp_resource_text_contents(uri, "text/plain", "x");
    return *out_contents ? CMCP_OK : CMCP_ENOMEM;
}

/* ====================================================================== */
/* test 1: emit_before_or_after_run_rejected                                */
/* ====================================================================== */

static void test_emit_outside_run_rejected(void) {
    cmcp_server_t *srv = cmcp_server_new("test", "0.1.0");
    /* No run → no transport stashed. */
    int rc = cmcp_server_notify(srv, "notifications/tools/list_changed", NULL);
    TEST_ASSERT(rc == CMCP_EINVAL);

    rc = cmcp_server_notify_tools_changed(srv);
    TEST_ASSERT(rc == CMCP_EPROTOCOL);   /* cap not advertised */

    cmcp_server_set_capabilities(srv, &(cmcp_server_capabilities_t){
        .tools_list_changed = 1,
    });
    rc = cmcp_server_notify_tools_changed(srv);
    /* Cap is set, but transport is still NULL → EINVAL from the
     * generic notify after the cap gate passes. */
    TEST_ASSERT(rc == CMCP_EINVAL);

    cmcp_server_free(srv);
}

/* ====================================================================== */
/* test 2: capability gating                                                */
/* ====================================================================== */

static void test_capability_gating(void) {
    /* Stand up a real server but do NOT advertise any change-cap. The
     * convenience wrappers must refuse with CMCP_EPROTOCOL. */
    transport_pair_t p;
    TEST_ASSERT(make_pair(&p) == 0);

    cmcp_server_t *srv = cmcp_server_new("test", "0.1.0");
    trigger_ctx_t tctx = { srv };

    /* The "trigger" tool calls the wrapper; if the wrapper refuses
     * we just take that as success — we'll observe that no
     * notification arrives at the client. */
    cmcp_server_add_tool(srv, &(cmcp_tool_t){
        .name = "trig", .handler = trig_tools_changed, .userdata = &tctx,
    });

    server_arg_t sa = { srv, p.server_t, 0 };
    pthread_t th;
    pthread_create(&th, NULL, server_thread, &sa);

    notif_capture_t cap; capture_init(&cap);
    cmcp_client_t *cli = cmcp_client_new("c", "0.0.1");
    cmcp_client_set_notification_handler(cli, on_notification, &cap);
    TEST_ASSERT(cmcp_client_handshake(cli, p.client_t) == CMCP_OK);

    cmcp_rpc_message_t resp;
    TEST_ASSERT(cmcp_client_request(cli, "tools/call",
                                     call_args("trig", NULL), &resp) == CMCP_OK);
    cmcp_rpc_message_clear(&resp);

    /* Give any in-flight notification a brief grace period to surface. */
    struct timespec ts = { 0, 50 * 1000 * 1000 };  /* 50ms */
    nanosleep(&ts, NULL);

    pthread_mutex_lock(&cap.mu);
    int seen = (int)cap.count;
    pthread_mutex_unlock(&cap.mu);
    TEST_ASSERT(seen == 0);            /* wrapper refused; no wire emit */

    cmcp_client_free(cli);
    cmcp_transport_close(p.client_t);
    pthread_join(th, NULL);
    cmcp_server_free(srv);
    cmcp_transport_close(p.server_t);
    capture_clear(&cap);
}

/* ====================================================================== */
/* test 3: tools/list_changed round-trips                                   */
/* ====================================================================== */

static void test_tools_list_changed_emit(void) {
    transport_pair_t p;
    TEST_ASSERT(make_pair(&p) == 0);

    cmcp_server_t *srv = cmcp_server_new("test", "0.1.0");
    cmcp_server_set_capabilities(srv, &(cmcp_server_capabilities_t){
        .tools_list_changed = 1,
    });
    trigger_ctx_t tctx = { srv };
    cmcp_server_add_tool(srv, &(cmcp_tool_t){
        .name = "trig", .handler = trig_tools_changed, .userdata = &tctx,
    });

    server_arg_t sa = { srv, p.server_t, 0 };
    pthread_t th;
    pthread_create(&th, NULL, server_thread, &sa);

    notif_capture_t cap; capture_init(&cap);
    cmcp_client_t *cli = cmcp_client_new("c", "0.0.1");
    cmcp_client_set_notification_handler(cli, on_notification, &cap);
    TEST_ASSERT(cmcp_client_handshake(cli, p.client_t) == CMCP_OK);

    cmcp_rpc_message_t resp;
    TEST_ASSERT(cmcp_client_request(cli, "tools/call",
                                     call_args("trig", NULL), &resp) == CMCP_OK);
    cmcp_rpc_message_clear(&resp);

    TEST_ASSERT(capture_wait_count(&cap, 1));
    pthread_mutex_lock(&cap.mu);
    TEST_ASSERT(cap.head != NULL);
    TEST_ASSERT(cap.head &&
                strcmp(cap.head->method, "notifications/tools/list_changed") == 0);
    pthread_mutex_unlock(&cap.mu);

    cmcp_client_free(cli);
    cmcp_transport_close(p.client_t);
    pthread_join(th, NULL);
    cmcp_server_free(srv);
    cmcp_transport_close(p.server_t);
    capture_clear(&cap);
}

/* ====================================================================== */
/* test 4: resources/list_changed and prompts/list_changed                  */
/* ====================================================================== */

static void test_resources_and_prompts_changed(void) {
    transport_pair_t p;
    TEST_ASSERT(make_pair(&p) == 0);

    cmcp_server_t *srv = cmcp_server_new("test", "0.1.0");
    cmcp_server_set_capabilities(srv, &(cmcp_server_capabilities_t){
        .resources_list_changed = 1,
        .prompts_list_changed   = 1,
    });
    trigger_ctx_t tctx = { srv };
    cmcp_server_add_tool(srv, &(cmcp_tool_t){
        .name = "trig_res", .handler = trig_resources_changed, .userdata = &tctx,
    });
    cmcp_server_add_tool(srv, &(cmcp_tool_t){
        .name = "trig_pr",  .handler = trig_prompts_changed,   .userdata = &tctx,
    });

    server_arg_t sa = { srv, p.server_t, 0 };
    pthread_t th;
    pthread_create(&th, NULL, server_thread, &sa);

    notif_capture_t cap; capture_init(&cap);
    cmcp_client_t *cli = cmcp_client_new("c", "0.0.1");
    cmcp_client_set_notification_handler(cli, on_notification, &cap);
    TEST_ASSERT(cmcp_client_handshake(cli, p.client_t) == CMCP_OK);

    cmcp_rpc_message_t r1, r2;
    TEST_ASSERT(cmcp_client_request(cli, "tools/call",
                                     call_args("trig_res", NULL), &r1) == CMCP_OK);
    TEST_ASSERT(cmcp_client_request(cli, "tools/call",
                                     call_args("trig_pr",  NULL), &r2) == CMCP_OK);
    cmcp_rpc_message_clear(&r1);
    cmcp_rpc_message_clear(&r2);

    TEST_ASSERT(capture_wait_count(&cap, 2));
    pthread_mutex_lock(&cap.mu);
    notif_record_t *n0 = cap.head;
    notif_record_t *n1 = n0 ? n0->next : NULL;
    TEST_ASSERT(n0 && strcmp(n0->method, "notifications/resources/list_changed") == 0);
    TEST_ASSERT(n1 && strcmp(n1->method, "notifications/prompts/list_changed")   == 0);
    pthread_mutex_unlock(&cap.mu);

    cmcp_client_free(cli);
    cmcp_transport_close(p.client_t);
    pthread_join(th, NULL);
    cmcp_server_free(srv);
    cmcp_transport_close(p.server_t);
    capture_clear(&cap);
}

/* ====================================================================== */
/* test 5: resource_updated subscriber filter                               */
/* ====================================================================== */

static void test_resource_updated_subscriber_filter(void) {
    transport_pair_t p;
    TEST_ASSERT(make_pair(&p) == 0);

    cmcp_server_t *srv = cmcp_server_new("test", "0.1.0");
    cmcp_server_set_capabilities(srv, &(cmcp_server_capabilities_t){
        .resources_subscribe = 1,
    });
    cmcp_server_add_resource(srv, &(cmcp_resource_t){
        .uri = "test://x", .name = "X", .read = sample_read,
    });
    cmcp_server_add_resource(srv, &(cmcp_resource_t){
        .uri = "test://y", .name = "Y", .read = sample_read,
    });
    trigger_ctx_t tctx = { srv };
    cmcp_server_add_tool(srv, &(cmcp_tool_t){
        .name = "trig", .handler = trig_resource_updated, .userdata = &tctx,
        .input_schema =
            "{\"type\":\"object\",\"properties\":{\"uri\":{\"type\":\"string\"}},"
            "\"required\":[\"uri\"]}",
    });

    server_arg_t sa = { srv, p.server_t, 0 };
    pthread_t th;
    pthread_create(&th, NULL, server_thread, &sa);

    notif_capture_t cap; capture_init(&cap);
    cmcp_client_t *cli = cmcp_client_new("c", "0.0.1");
    cmcp_client_set_notification_handler(cli, on_notification, &cap);
    TEST_ASSERT(cmcp_client_handshake(cli, p.client_t) == CMCP_OK);

    /* Subscribe to test://x only. */
    cmcp_json_t *sp = cmcp_json_new_object();
    cmcp_json_object_set(sp, "uri", cmcp_json_new_string("test://x"));
    cmcp_rpc_message_t r;
    TEST_ASSERT(cmcp_client_request(cli, "resources/subscribe", sp, &r) == CMCP_OK);
    TEST_ASSERT(r.error == NULL);
    cmcp_rpc_message_clear(&r);

    /* Trigger emit for x → subscribed, expect notification. */
    cmcp_json_t *args1 = cmcp_json_new_object();
    cmcp_json_object_set(args1, "uri", cmcp_json_new_string("test://x"));
    TEST_ASSERT(cmcp_client_request(cli, "tools/call",
                                     call_args("trig", args1), &r) == CMCP_OK);
    cmcp_rpc_message_clear(&r);

    /* Trigger emit for y → not subscribed, expect NO notification. */
    cmcp_json_t *args2 = cmcp_json_new_object();
    cmcp_json_object_set(args2, "uri", cmcp_json_new_string("test://y"));
    TEST_ASSERT(cmcp_client_request(cli, "tools/call",
                                     call_args("trig", args2), &r) == CMCP_OK);
    cmcp_rpc_message_clear(&r);

    /* Wait for the one we expect, then assert nothing extra arrived
     * within a brief grace window. */
    TEST_ASSERT(capture_wait_count(&cap, 1));
    struct timespec ts = { 0, 50 * 1000 * 1000 };  /* 50ms */
    nanosleep(&ts, NULL);

    pthread_mutex_lock(&cap.mu);
    TEST_ASSERT(cap.count == 1);
    notif_record_t *n0 = cap.head;
    TEST_ASSERT(n0 && strcmp(n0->method, "notifications/resources/updated") == 0);
    if (n0 && n0->params) {
        const cmcp_json_t *uri = cmcp_json_object_get(n0->params, "uri");
        TEST_ASSERT(uri && strcmp(uri->str.s, "test://x") == 0);
    }
    pthread_mutex_unlock(&cap.mu);

    cmcp_client_free(cli);
    cmcp_transport_close(p.client_t);
    pthread_join(th, NULL);
    cmcp_server_free(srv);
    cmcp_transport_close(p.server_t);
    capture_clear(&cap);
}

/* ====================================================================== */
/* test 6: progress before response (ordering)                              */
/* ====================================================================== */

static void test_progress_then_response(void) {
    transport_pair_t p;
    TEST_ASSERT(make_pair(&p) == 0);

    cmcp_server_t *srv = cmcp_server_new("test", "0.1.0");
    trigger_ctx_t tctx = { srv };
    cmcp_server_add_tool(srv, &(cmcp_tool_t){
        .name = "go", .handler = trig_progress, .userdata = &tctx,
    });

    server_arg_t sa = { srv, p.server_t, 0 };
    pthread_t th;
    pthread_create(&th, NULL, server_thread, &sa);

    notif_capture_t cap; capture_init(&cap);
    cmcp_client_t *cli = cmcp_client_new("c", "0.0.1");
    cmcp_client_set_notification_handler(cli, on_notification, &cap);
    TEST_ASSERT(cmcp_client_handshake(cli, p.client_t) == CMCP_OK);

    /* The handler emits progress, then returns the tool result.
     * On the wire: notification first, response second. The client
     * reader processes them in order, so by the time
     * cmcp_client_request returns its response, the notification
     * callback has already fired (reader is single-threaded). */
    cmcp_rpc_message_t resp;
    TEST_ASSERT(cmcp_client_request(cli, "tools/call",
                                     call_args("go", NULL), &resp) == CMCP_OK);
    TEST_ASSERT(resp.error == NULL);
    cmcp_rpc_message_clear(&resp);

    /* Notification must be visible with no extra waiting. */
    pthread_mutex_lock(&cap.mu);
    TEST_ASSERT(cap.count >= 1);
    notif_record_t *n0 = cap.head;
    TEST_ASSERT(n0 && strcmp(n0->method, "notifications/progress") == 0);
    if (n0 && n0->params) {
        const cmcp_json_t *tok = cmcp_json_object_get(n0->params, "progressToken");
        const cmcp_json_t *pr  = cmcp_json_object_get(n0->params, "progress");
        TEST_ASSERT(tok && tok->type == CMCP_JSON_STRING &&
                    strcmp(tok->str.s, "p1") == 0);
        TEST_ASSERT(pr && pr->type == CMCP_JSON_DOUBLE && pr->d == 0.5);
    }
    pthread_mutex_unlock(&cap.mu);

    cmcp_client_free(cli);
    cmcp_transport_close(p.client_t);
    pthread_join(th, NULL);
    cmcp_server_free(srv);
    cmcp_transport_close(p.server_t);
    capture_clear(&cap);
}

/* ====================================================================== */
/* test 7: HTTP transport — notification rides the SSE channel              */
/* ====================================================================== */

static unsigned short pick_port(void) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return 0;
    int yes = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof yes);
    struct sockaddr_in sa;
    memset(&sa, 0, sizeof sa);
    sa.sin_family      = AF_INET;
    sa.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    sa.sin_port        = 0;
    if (bind(fd, (struct sockaddr *)&sa, sizeof sa) != 0) {
        close(fd); return 0;
    }
    socklen_t slen = sizeof sa;
    if (getsockname(fd, (struct sockaddr *)&sa, &slen) != 0) {
        close(fd); return 0;
    }
    unsigned short port = ntohs(sa.sin_port);
    close(fd);
    return port;
}

static void test_http_sse_notification(void) {
    /* Same test as #3 (tools/list_changed) but the wire is the full
     * Phase 2.1 HTTP server + Phase 2.2 HTTP client. The notification
     * is routed by transport_http's write_fn classifier onto the
     * held-open SSE GET; the HTTP client SSE thread reads it and
     * pushes a frame onto its read queue, which client.c's reader
     * routes to the notification callback. */
    unsigned short port = pick_port();
    TEST_ASSERT(port != 0);

    cmcp_transport_t *server_t = cmcp_transport_http_listen("127.0.0.1", port);
    TEST_ASSERT(server_t != NULL);

    cmcp_server_t *srv = cmcp_server_new("http-srv", "0.1.0");
    cmcp_server_set_capabilities(srv, &(cmcp_server_capabilities_t){
        .tools_list_changed = 1,
    });
    trigger_ctx_t tctx = { srv };
    cmcp_server_add_tool(srv, &(cmcp_tool_t){
        .name = "trig", .handler = trig_tools_changed, .userdata = &tctx,
    });

    server_arg_t *sa = (server_arg_t *)calloc(1, sizeof *sa);
    sa->s = srv; sa->t = server_t;
    pthread_t th;
    TEST_ASSERT(pthread_create(&th, NULL, server_thread, sa) == 0);

    /* Brief wait for acceptor to reach poll(). */
    struct timespec ts = { 0, 20 * 1000 * 1000 };
    nanosleep(&ts, NULL);

    char url[64];
    snprintf(url, sizeof url, "http://127.0.0.1:%u/mcp", port);
    cmcp_transport_t *client_t = cmcp_transport_http_connect(url);
    TEST_ASSERT(client_t != NULL);

    notif_capture_t cap; capture_init(&cap);
    cmcp_client_t *cli = cmcp_client_new("http-cli", "0.0.1");
    cmcp_client_set_notification_handler(cli, on_notification, &cap);
    TEST_ASSERT(cmcp_client_handshake(cli, client_t) == CMCP_OK);

    /* Give the HTTP client's SSE thread time to latch the session id
     * and open the long-poll GET. Otherwise the server might emit
     * the notification before any holder is registered, and the
     * notification gets dropped (per the documented "no holder, no
     * delivery" rule). 200ms is a generous slop budget. */
    struct timespec wait_sse = { 0, 200 * 1000 * 1000 };
    nanosleep(&wait_sse, NULL);

    cmcp_rpc_message_t resp;
    TEST_ASSERT(cmcp_client_request(cli, "tools/call",
                                     call_args("trig", NULL), &resp) == CMCP_OK);
    cmcp_rpc_message_clear(&resp);

    /* SSE delivery is asynchronous — wait. */
    TEST_ASSERT(capture_wait_count(&cap, 1));
    pthread_mutex_lock(&cap.mu);
    TEST_ASSERT(cap.head != NULL);
    TEST_ASSERT(cap.head &&
                strcmp(cap.head->method,
                       "notifications/tools/list_changed") == 0);
    pthread_mutex_unlock(&cap.mu);

    cmcp_client_free(cli);
    cmcp_transport_close(client_t);
    cmcp_transport_close(server_t);
    pthread_join(th, NULL);
    cmcp_server_free(srv);
    capture_clear(&cap);
}

/* ====================================================================== */

int main(void) {
    fprintf(stderr, "test_notifications:\n");

    TEST_RUN(test_emit_outside_run_rejected);
    TEST_RUN(test_capability_gating);
    TEST_RUN(test_tools_list_changed_emit);
    TEST_RUN(test_resources_and_prompts_changed);
    TEST_RUN(test_resource_updated_subscriber_filter);
    TEST_RUN(test_progress_then_response);
    TEST_RUN(test_http_sse_notification);

    TEST_DONE();
}
