/* End-to-end tests for Phase 4.7 structured logging.
 *
 * Wire: `logging/setLevel` (client → server) sets the floor, and
 * `notifications/message` (server → client) carries `{level, logger?,
 * data}`. Cap-gated on the server side: caps.logging must be opted in
 * for either half to function.
 *
 * Four cases:
 *   1. setLevel round-trips and stores the new floor;
 *   2. `cmcp_server_log` filters silently below the floor;
 *   3. capability-not-set: setLevel → CMCP_EPROTOCOL, server_log →
 *      CMCP_EPROTOCOL;
 *   4. wire payload shape (level, logger, data) is correct.
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
/* Pipe-pair scaffolding                                                    */
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
/* Notification capture                                                     */
/* ====================================================================== */

typedef struct notif_record {
    char                *method;
    cmcp_json_t         *params;
    struct notif_record *next;
} notif_record_t;

typedef struct {
    pthread_mutex_t mu;
    pthread_cond_t  cv;
    notif_record_t *head;
    notif_record_t *tail;
    size_t          count;
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
/* Tool that asks the server to log at a caller-specified level             */
/* ====================================================================== */

typedef struct {
    cmcp_server_t *server;
} log_ctx_t;

/* Tool args: {"level": "info"|..., "logger"?: string, "text"?: string}.
 * The tool calls cmcp_server_log with those values and returns its
 * return code as the text content — so a "filtered" log is observable
 * (CMCP_OK + no wire notification) and an error is observable too. */
static int trig_log(const cmcp_json_t *args, void *userdata,
                     cmcp_handler_ctx_t *hctx,
                     cmcp_json_t **out_content, int *out_is_error) {
    (void)hctx;
    log_ctx_t *c = (log_ctx_t *)userdata;
    *out_is_error = 0;
    const cmcp_json_t *lv  = args ? cmcp_json_object_get(args, "level")  : NULL;
    const cmcp_json_t *lg  = args ? cmcp_json_object_get(args, "logger") : NULL;
    const cmcp_json_t *tx  = args ? cmcp_json_object_get(args, "text")   : NULL;

    cmcp_log_level_t lvl = CMCP_LOG_LEVEL_INFO;
    if (lv && lv->type == CMCP_JSON_STRING)
        cmcp_log_level_from_name(lv->str.s, &lvl);
    const char *logger = (lg && lg->type == CMCP_JSON_STRING) ? lg->str.s : NULL;

    cmcp_json_t *data = cmcp_json_new_object();
    cmcp_json_object_set(data, "message", cmcp_json_new_string(
        (tx && tx->type == CMCP_JSON_STRING) ? tx->str.s : ""));
    int rc = cmcp_server_log(c->server, lvl, logger, data);

    char buf[64];
    snprintf(buf, sizeof buf, "rc=%d", rc);
    *out_content = cmcp_tool_text_content(buf);
    return CMCP_OK;
}

static cmcp_json_t *call_args(const char *name, cmcp_json_t *arguments) {
    cmcp_json_t *p = cmcp_json_new_object();
    cmcp_json_object_set(p, "name", cmcp_json_new_string(name));
    if (arguments) cmcp_json_object_set(p, "arguments", arguments);
    return p;
}

/* Pull the "text" string from a tools/call success result so a test
 * can read back the rc the trig_log tool returned to us. */
static const char *tool_text(const cmcp_rpc_message_t *resp) {
    if (!resp->result || resp->result->type != CMCP_JSON_OBJECT) return NULL;
    const cmcp_json_t *content = cmcp_json_object_get(resp->result, "content");
    if (!content || content->type != CMCP_JSON_ARRAY ||
        content->arr.len == 0) return NULL;
    const cmcp_json_t *first = content->arr.items[0];
    if (!first || first->type != CMCP_JSON_OBJECT) return NULL;
    const cmcp_json_t *txt = cmcp_json_object_get(first, "text");
    if (!txt || txt->type != CMCP_JSON_STRING) return NULL;
    return txt->str.s;
}

/* ====================================================================== */
/* test 1: setLevel round-trip and floor enforcement                        */
/* ====================================================================== */

static void test_set_level_filters(void) {
    transport_pair_t p;
    TEST_ASSERT(make_pair(&p) == 0);

    cmcp_server_t *srv = cmcp_server_new("test", "0.1.0");
    cmcp_server_set_capabilities(srv, &(cmcp_server_capabilities_t){
        .logging = 1,
    });
    log_ctx_t lctx = { srv };
    cmcp_server_add_tool(srv, &(cmcp_tool_t){
        .name = "trig_log", .handler = trig_log, .userdata = &lctx,
    });

    server_arg_t sa = { srv, p.server_t, 0 };
    pthread_t th;
    pthread_create(&th, NULL, server_thread, &sa);

    notif_capture_t cap; capture_init(&cap);
    cmcp_client_t *cli = cmcp_client_new("c", "0.0.1");
    cmcp_client_set_notification_handler(cli, on_notification, &cap);
    TEST_ASSERT(cmcp_client_handshake(cli, p.client_t) == CMCP_OK);

    /* Server must have advertised the logging cap. */
    const cmcp_server_capabilities_t *sc = cmcp_client_server_caps(cli);
    TEST_ASSERT(sc && sc->logging == 1);

    /* Default floor (DEBUG) → an info-level log fires. */
    cmcp_json_t *args = cmcp_json_new_object();
    cmcp_json_object_set(args, "level", cmcp_json_new_string("info"));
    cmcp_json_object_set(args, "text",  cmcp_json_new_string("first"));
    cmcp_rpc_message_t resp;
    TEST_ASSERT(cmcp_client_request(cli, "tools/call",
                                     call_args("trig_log", args),
                                     &resp) == CMCP_OK);
    TEST_ASSERT(tool_text(&resp) && strcmp(tool_text(&resp), "rc=0") == 0);
    cmcp_rpc_message_clear(&resp);
    TEST_ASSERT(capture_wait_count(&cap, 1));

    /* Raise floor to warning; an info log is now silently dropped. */
    TEST_ASSERT(cmcp_client_set_log_level(cli, CMCP_LOG_LEVEL_WARNING)
                == CMCP_OK);

    args = cmcp_json_new_object();
    cmcp_json_object_set(args, "level", cmcp_json_new_string("info"));
    cmcp_json_object_set(args, "text",  cmcp_json_new_string("dropped"));
    TEST_ASSERT(cmcp_client_request(cli, "tools/call",
                                     call_args("trig_log", args),
                                     &resp) == CMCP_OK);
    TEST_ASSERT(tool_text(&resp) && strcmp(tool_text(&resp), "rc=0") == 0);
    cmcp_rpc_message_clear(&resp);

    /* Give any stray notification time to surface — must NOT appear. */
    struct timespec ts = { 0, 50 * 1000 * 1000 };
    nanosleep(&ts, NULL);
    pthread_mutex_lock(&cap.mu);
    size_t after_drop = cap.count;
    pthread_mutex_unlock(&cap.mu);
    TEST_ASSERT(after_drop == 1);

    /* A warning log clears the floor and fires. */
    args = cmcp_json_new_object();
    cmcp_json_object_set(args, "level", cmcp_json_new_string("warning"));
    cmcp_json_object_set(args, "text",  cmcp_json_new_string("kept"));
    TEST_ASSERT(cmcp_client_request(cli, "tools/call",
                                     call_args("trig_log", args),
                                     &resp) == CMCP_OK);
    cmcp_rpc_message_clear(&resp);
    TEST_ASSERT(capture_wait_count(&cap, 2));

    cmcp_client_free(cli);
    cmcp_transport_close(p.client_t);
    pthread_join(th, NULL);
    cmcp_server_free(srv);
    cmcp_transport_close(p.server_t);
    capture_clear(&cap);
}

/* ====================================================================== */
/* test 2: wire payload shape                                              */
/* ====================================================================== */

static void test_payload_shape(void) {
    transport_pair_t p;
    TEST_ASSERT(make_pair(&p) == 0);

    cmcp_server_t *srv = cmcp_server_new("test", "0.1.0");
    cmcp_server_set_capabilities(srv, &(cmcp_server_capabilities_t){
        .logging = 1,
    });
    log_ctx_t lctx = { srv };
    cmcp_server_add_tool(srv, &(cmcp_tool_t){
        .name = "trig_log", .handler = trig_log, .userdata = &lctx,
    });

    server_arg_t sa = { srv, p.server_t, 0 };
    pthread_t th;
    pthread_create(&th, NULL, server_thread, &sa);

    notif_capture_t cap; capture_init(&cap);
    cmcp_client_t *cli = cmcp_client_new("c", "0.0.1");
    cmcp_client_set_notification_handler(cli, on_notification, &cap);
    TEST_ASSERT(cmcp_client_handshake(cli, p.client_t) == CMCP_OK);

    cmcp_json_t *args = cmcp_json_new_object();
    cmcp_json_object_set(args, "level",  cmcp_json_new_string("error"));
    cmcp_json_object_set(args, "logger", cmcp_json_new_string("ingest"));
    cmcp_json_object_set(args, "text",   cmcp_json_new_string("boom"));
    cmcp_rpc_message_t resp;
    TEST_ASSERT(cmcp_client_request(cli, "tools/call",
                                     call_args("trig_log", args),
                                     &resp) == CMCP_OK);
    cmcp_rpc_message_clear(&resp);
    TEST_ASSERT(capture_wait_count(&cap, 1));

    pthread_mutex_lock(&cap.mu);
    notif_record_t *r = cap.head;
    TEST_ASSERT(r && strcmp(r->method, "notifications/message") == 0);
    TEST_ASSERT(r->params && r->params->type == CMCP_JSON_OBJECT);
    const cmcp_json_t *lv = cmcp_json_object_get(r->params, "level");
    const cmcp_json_t *lg = cmcp_json_object_get(r->params, "logger");
    const cmcp_json_t *dt = cmcp_json_object_get(r->params, "data");
    TEST_ASSERT(lv && lv->type == CMCP_JSON_STRING &&
                strcmp(lv->str.s, "error") == 0);
    TEST_ASSERT(lg && lg->type == CMCP_JSON_STRING &&
                strcmp(lg->str.s, "ingest") == 0);
    TEST_ASSERT(dt && dt->type == CMCP_JSON_OBJECT);
    const cmcp_json_t *m = cmcp_json_object_get(dt, "message");
    TEST_ASSERT(m && m->type == CMCP_JSON_STRING &&
                strcmp(m->str.s, "boom") == 0);
    pthread_mutex_unlock(&cap.mu);

    cmcp_client_free(cli);
    cmcp_transport_close(p.client_t);
    pthread_join(th, NULL);
    cmcp_server_free(srv);
    cmcp_transport_close(p.server_t);
    capture_clear(&cap);
}

/* ====================================================================== */
/* test 3: server didn't advertise caps.logging                            */
/* ====================================================================== */

static void test_cap_not_advertised(void) {
    transport_pair_t p;
    TEST_ASSERT(make_pair(&p) == 0);

    cmcp_server_t *srv = cmcp_server_new("test", "0.1.0");
    /* No caps.logging — the server doesn't opt in. */
    log_ctx_t lctx = { srv };
    cmcp_server_add_tool(srv, &(cmcp_tool_t){
        .name = "trig_log", .handler = trig_log, .userdata = &lctx,
    });

    server_arg_t sa = { srv, p.server_t, 0 };
    pthread_t th;
    pthread_create(&th, NULL, server_thread, &sa);

    notif_capture_t cap; capture_init(&cap);
    cmcp_client_t *cli = cmcp_client_new("c", "0.0.1");
    cmcp_client_set_notification_handler(cli, on_notification, &cap);
    TEST_ASSERT(cmcp_client_handshake(cli, p.client_t) == CMCP_OK);

    /* Server caps must show no logging. */
    const cmcp_server_capabilities_t *sc = cmcp_client_server_caps(cli);
    TEST_ASSERT(sc && sc->logging == 0);

    /* setLevel from the client surfaces as CMCP_EPROTOCOL (server
     * answered -32601 since the route is gated on caps.logging). */
    TEST_ASSERT(cmcp_client_set_log_level(cli, CMCP_LOG_LEVEL_DEBUG)
                == CMCP_EPROTOCOL);

    /* server_log refuses too — caller said they wouldn't log. */
    cmcp_json_t *args = cmcp_json_new_object();
    cmcp_json_object_set(args, "level", cmcp_json_new_string("info"));
    cmcp_json_object_set(args, "text",  cmcp_json_new_string("nope"));
    cmcp_rpc_message_t resp;
    TEST_ASSERT(cmcp_client_request(cli, "tools/call",
                                     call_args("trig_log", args),
                                     &resp) == CMCP_OK);
    TEST_ASSERT(tool_text(&resp) &&
                strcmp(tool_text(&resp), "rc=-6") == 0);   /* CMCP_EPROTOCOL */
    cmcp_rpc_message_clear(&resp);

    struct timespec ts = { 0, 50 * 1000 * 1000 };
    nanosleep(&ts, NULL);
    pthread_mutex_lock(&cap.mu);
    size_t seen = cap.count;
    pthread_mutex_unlock(&cap.mu);
    TEST_ASSERT(seen == 0);

    cmcp_client_free(cli);
    cmcp_transport_close(p.client_t);
    pthread_join(th, NULL);
    cmcp_server_free(srv);
    cmcp_transport_close(p.server_t);
    capture_clear(&cap);
}

/* ====================================================================== */
/* Tier 6.5.4: secret-shaped fields in `data` are scrubbed on the wire     */
/* ====================================================================== */

/* Same shape as trig_log but the data payload contains both a benign
 * field and a credential-shaped field. We assert the wire-side
 * notification redacts only the credential. */
static int trig_log_with_secret(const cmcp_json_t *args, void *userdata,
                                  cmcp_handler_ctx_t *hctx,
                                  cmcp_json_t **out_content, int *out_is_error) {
    (void)args; (void)hctx;
    log_ctx_t *c = (log_ctx_t *)userdata;
    *out_is_error = 0;

    cmcp_json_t *data = cmcp_json_new_object();
    cmcp_json_object_set(data, "action",  cmcp_json_new_string("deploy"));
    cmcp_json_object_set(data, "api_key", cmcp_json_new_string("sk-LIVE-12345"));
    cmcp_json_object_set(data, "user",    cmcp_json_new_string("alice"));
    int rc = cmcp_server_log(c->server, CMCP_LOG_LEVEL_INFO, "audit", data);

    char buf[64];
    snprintf(buf, sizeof buf, "rc=%d", rc);
    *out_content = cmcp_tool_text_content(buf);
    return CMCP_OK;
}

static void test_log_data_credentials_redacted(void) {
    transport_pair_t p;
    TEST_ASSERT(make_pair(&p) == 0);

    cmcp_server_t *srv = cmcp_server_new("test", "0.1.0");
    cmcp_server_set_capabilities(srv, &(cmcp_server_capabilities_t){
        .logging = 1,
    });
    log_ctx_t lctx = { srv };
    cmcp_server_add_tool(srv, &(cmcp_tool_t){
        .name = "trig", .handler = trig_log_with_secret, .userdata = &lctx,
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
                                     call_args("trig", NULL),
                                     &resp) == CMCP_OK);
    cmcp_rpc_message_clear(&resp);
    TEST_ASSERT(capture_wait_count(&cap, 1));

    pthread_mutex_lock(&cap.mu);
    notif_record_t *r = cap.head;
    TEST_ASSERT(r && strcmp(r->method, "notifications/message") == 0);
    const cmcp_json_t *dt = cmcp_json_object_get(r->params, "data");
    TEST_ASSERT(dt && dt->type == CMCP_JSON_OBJECT);
    const cmcp_json_t *act = cmcp_json_object_get(dt, "action");
    const cmcp_json_t *key = cmcp_json_object_get(dt, "api_key");
    const cmcp_json_t *usr = cmcp_json_object_get(dt, "user");
    TEST_ASSERT(act && strcmp(act->str.s, "deploy")     == 0);
    TEST_ASSERT(key && strcmp(key->str.s, "[REDACTED]") == 0);
    TEST_ASSERT(usr && strcmp(usr->str.s, "alice")      == 0);
    pthread_mutex_unlock(&cap.mu);

    cmcp_client_free(cli);
    cmcp_transport_close(p.client_t);
    pthread_join(th, NULL);
    cmcp_server_free(srv);
    cmcp_transport_close(p.server_t);
    capture_clear(&cap);
}

/* ====================================================================== */
/* test 4: level name codec                                                */
/* ====================================================================== */

static void test_level_name_codec(void) {
    /* Spot-check the eight RFC 5424 names round-trip. */
    cmcp_log_level_t lvl;
    TEST_ASSERT(cmcp_log_level_from_name("debug",     &lvl) == 0 &&
                lvl == CMCP_LOG_LEVEL_DEBUG);
    TEST_ASSERT(cmcp_log_level_from_name("info",      &lvl) == 0 &&
                lvl == CMCP_LOG_LEVEL_INFO);
    TEST_ASSERT(cmcp_log_level_from_name("notice",    &lvl) == 0 &&
                lvl == CMCP_LOG_LEVEL_NOTICE);
    TEST_ASSERT(cmcp_log_level_from_name("warning",   &lvl) == 0 &&
                lvl == CMCP_LOG_LEVEL_WARNING);
    TEST_ASSERT(cmcp_log_level_from_name("error",     &lvl) == 0 &&
                lvl == CMCP_LOG_LEVEL_ERROR);
    TEST_ASSERT(cmcp_log_level_from_name("critical",  &lvl) == 0 &&
                lvl == CMCP_LOG_LEVEL_CRITICAL);
    TEST_ASSERT(cmcp_log_level_from_name("alert",     &lvl) == 0 &&
                lvl == CMCP_LOG_LEVEL_ALERT);
    TEST_ASSERT(cmcp_log_level_from_name("emergency", &lvl) == 0 &&
                lvl == CMCP_LOG_LEVEL_EMERGENCY);

    /* Unknown / NULL inputs rejected. */
    TEST_ASSERT(cmcp_log_level_from_name("DEBUG",  &lvl) == -1);
    TEST_ASSERT(cmcp_log_level_from_name("",       &lvl) == -1);
    TEST_ASSERT(cmcp_log_level_from_name(NULL,     &lvl) == -1);

    /* Reverse direction. */
    TEST_ASSERT(strcmp(cmcp_log_level_to_name(CMCP_LOG_LEVEL_DEBUG),
                       "debug") == 0);
    TEST_ASSERT(strcmp(cmcp_log_level_to_name(CMCP_LOG_LEVEL_EMERGENCY),
                       "emergency") == 0);
    TEST_ASSERT(cmcp_log_level_to_name((cmcp_log_level_t)-1) == NULL);
    TEST_ASSERT(cmcp_log_level_to_name((cmcp_log_level_t)99) == NULL);
}

int main(void) {
    fprintf(stderr, "test_logging\n");
    TEST_RUN(test_level_name_codec);
    TEST_RUN(test_set_level_filters);
    TEST_RUN(test_payload_shape);
    TEST_RUN(test_cap_not_advertised);
    TEST_RUN(test_log_data_credentials_redacted);
    TEST_DONE();
}
