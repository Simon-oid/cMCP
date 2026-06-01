/* End-to-end tests for the async client + multi-server session.
 *
 * Exercises:
 *   - cmcp_client_request (sync) against a real cmcp_server_run server
 *   - cmcp_client_call_async + cmcp_client_wait (async)
 *   - server-to-client notification routing via the user callback
 *   - cmcp_session_t aggregation across two real servers, with
 *     "<server>:<tool>" name namespacing and routed tools/call
 *
 * Each test wires its servers up through pipe(2) pairs (mirroring
 * test_lifecycle / test_stdio_roundtrip) and runs the server in a
 * pthread so the client side stays in the test thread. */

/* pipe(2) — POSIX. */
#define _POSIX_C_SOURCE 200809L

#include "test.h"
#include "cmcp.h"
#include "cmcp_client.h"
#include "cmcp_json.h"
#include "cmcp_server.h"
#include "cmcp_session.h"
#include "cmcp_transport.h"
#include "cmcp_types.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
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
    int               rc;
} server_arg_t;

static void *server_thread(void *arg) {
    server_arg_t *a = (server_arg_t *)arg;
    a->rc = cmcp_server_run(a->s, a->t);
    return NULL;
}

/* ====================================================================== */
/* Echo tool — used by the request/async/session tests.                    */
/* ====================================================================== */

/* Returns content [{"type":"text","text": <prefix><message>}].
 * userdata is a const char * prefix. */
static int echo_tool(const cmcp_json_t *arguments, void *userdata,
                      cmcp_handler_ctx_t *hctx,
                      cmcp_json_t **out_content, int *out_is_error) {
    (void)out_is_error; (void)hctx;
    const char *prefix = userdata ? (const char *)userdata : "";
    const cmcp_json_t *m = arguments
        ? cmcp_json_object_get(arguments, "message") : NULL;
    const char *msg = (m && m->type == CMCP_JSON_STRING) ? m->str.s : "";

    size_t pn = strlen(prefix), mn = strlen(msg);
    char *buf = (char *)malloc(pn + mn + 1);
    if (!buf) return CMCP_ENOMEM;
    memcpy(buf, prefix, pn);
    memcpy(buf + pn, msg, mn);
    buf[pn + mn] = '\0';

    *out_content = cmcp_tool_text_content(buf);
    free(buf);
    return *out_content ? CMCP_OK : CMCP_ENOMEM;
}

static const char echo_schema[] =
    "{\"type\":\"object\","
     "\"properties\":{\"message\":{\"type\":\"string\"}},"
     "\"required\":[\"message\"]}";

static int register_echo(cmcp_server_t *srv, const char *name,
                          const char *prefix) {
    cmcp_tool_t t = {0};
    t.name         = name;
    t.description  = "echo a message with a server-specific prefix";
    t.input_schema = echo_schema;
    t.handler      = echo_tool;
    t.userdata     = (void *)prefix;
    return cmcp_server_add_tool(srv, &t);
}

/* Helper: pull the .text out of a tools/call response's first content
 * item. Returns a freshly malloc'd string, or NULL if shape mismatch. */
static char *extract_text(const cmcp_rpc_message_t *resp) {
    if (!resp || !resp->result || resp->result->type != CMCP_JSON_OBJECT)
        return NULL;
    const cmcp_json_t *arr = cmcp_json_object_get(resp->result, "content");
    if (!arr || arr->type != CMCP_JSON_ARRAY || arr->arr.len == 0) return NULL;
    const cmcp_json_t *item = arr->arr.items[0];
    if (!item || item->type != CMCP_JSON_OBJECT) return NULL;
    const cmcp_json_t *text = cmcp_json_object_get(item, "text");
    if (!text || text->type != CMCP_JSON_STRING) return NULL;
    return strdup(text->str.s);
}

/* Build {"name":<tool>,"arguments":{"message":<msg>}} for tools/call. */
static cmcp_json_t *make_tool_call_params(const char *tool, const char *msg) {
    cmcp_json_t *p = cmcp_json_new_object();
    cmcp_json_object_set(p, "name", cmcp_json_new_string(tool));
    cmcp_json_t *args = cmcp_json_new_object();
    cmcp_json_object_set(args, "message", cmcp_json_new_string(msg));
    cmcp_json_object_set(p, "arguments", args);
    return p;
}

/* ====================================================================== */
/* test_request_sync — cmcp_client_request round-trip                      */
/* ====================================================================== */

static void test_request_sync(void) {
    transport_pair_t p;
    TEST_ASSERT(make_pair(&p) == 0);

    cmcp_server_t *srv = cmcp_server_new("sync-srv", "0.1.0");
    TEST_ASSERT(register_echo(srv, "echo", "S:") == CMCP_OK);
    server_arg_t sa = { srv, p.server_t, 0 };
    pthread_t th;
    TEST_ASSERT(pthread_create(&th, NULL, server_thread, &sa) == 0);

    cmcp_client_t *cli = cmcp_client_new("sync-cli", "0.0.1");
    TEST_ASSERT(cmcp_client_handshake(cli, p.client_t) == CMCP_OK);

    cmcp_rpc_message_t resp;
    cmcp_rpc_message_init(&resp);
    cmcp_json_t *params = make_tool_call_params("echo", "hello");
    TEST_ASSERT(cmcp_client_request(cli, "tools/call", params, &resp)
                == CMCP_OK);
    TEST_ASSERT(resp.kind == CMCP_MSG_RESPONSE);
    TEST_ASSERT(resp.error == NULL);

    char *text = extract_text(&resp);
    TEST_ASSERT(text != NULL);
    TEST_ASSERT(text && strcmp(text, "S:hello") == 0);
    free(text);
    cmcp_rpc_message_clear(&resp);

    cmcp_client_free(cli);
    cmcp_transport_close(p.client_t);
    pthread_join(th, NULL);
    TEST_ASSERT(sa.rc == CMCP_OK);
    cmcp_server_free(srv);
    cmcp_transport_close(p.server_t);
}

/* ====================================================================== */
/* test_async_call — call_async + wait, multiple in-flight                 */
/* ====================================================================== */

static void test_async_call(void) {
    transport_pair_t p;
    TEST_ASSERT(make_pair(&p) == 0);

    cmcp_server_t *srv = cmcp_server_new("async-srv", "0.1.0");
    TEST_ASSERT(register_echo(srv, "echo", "A:") == CMCP_OK);
    server_arg_t sa = { srv, p.server_t, 0 };
    pthread_t th;
    TEST_ASSERT(pthread_create(&th, NULL, server_thread, &sa) == 0);

    cmcp_client_t *cli = cmcp_client_new("async-cli", "0.0.1");
    TEST_ASSERT(cmcp_client_handshake(cli, p.client_t) == CMCP_OK);

    /* Fire three async calls back-to-back without waiting. */
    long long id1 = 0, id2 = 0, id3 = 0;
    TEST_ASSERT(cmcp_client_call_async(cli, "tools/call",
                make_tool_call_params("echo", "one"), &id1) == CMCP_OK);
    TEST_ASSERT(cmcp_client_call_async(cli, "tools/call",
                make_tool_call_params("echo", "two"), &id2) == CMCP_OK);
    TEST_ASSERT(cmcp_client_call_async(cli, "tools/call",
                make_tool_call_params("echo", "three"), &id3) == CMCP_OK);

    TEST_ASSERT(id1 != 0 && id2 != 0 && id3 != 0);
    TEST_ASSERT(id1 != id2 && id2 != id3 && id1 != id3);

    /* Wait in non-arrival order to confirm the demux is real. */
    cmcp_rpc_message_t r1, r2, r3;
    cmcp_rpc_message_init(&r1);
    cmcp_rpc_message_init(&r2);
    cmcp_rpc_message_init(&r3);
    TEST_ASSERT(cmcp_client_wait(cli, id3, &r3) == CMCP_OK);
    TEST_ASSERT(cmcp_client_wait(cli, id1, &r1) == CMCP_OK);
    TEST_ASSERT(cmcp_client_wait(cli, id2, &r2) == CMCP_OK);

    char *t1 = extract_text(&r1);
    char *t2 = extract_text(&r2);
    char *t3 = extract_text(&r3);
    TEST_ASSERT(t1 && strcmp(t1, "A:one") == 0);
    TEST_ASSERT(t2 && strcmp(t2, "A:two") == 0);
    TEST_ASSERT(t3 && strcmp(t3, "A:three") == 0);
    free(t1); free(t2); free(t3);
    cmcp_rpc_message_clear(&r1);
    cmcp_rpc_message_clear(&r2);
    cmcp_rpc_message_clear(&r3);

    cmcp_client_free(cli);
    cmcp_transport_close(p.client_t);
    pthread_join(th, NULL);
    cmcp_server_free(srv);
    cmcp_transport_close(p.server_t);
}

/* ====================================================================== */
/* test_async_wait_unknown_id — wait on an id that was never registered    */
/* ====================================================================== */

static void test_async_wait_unknown_id(void) {
    transport_pair_t p;
    TEST_ASSERT(make_pair(&p) == 0);

    cmcp_server_t *srv = cmcp_server_new("srv", "0.1.0");
    TEST_ASSERT(register_echo(srv, "echo", "X:") == CMCP_OK);
    server_arg_t sa = { srv, p.server_t, 0 };
    pthread_t th;
    TEST_ASSERT(pthread_create(&th, NULL, server_thread, &sa) == 0);

    cmcp_client_t *cli = cmcp_client_new("cli", "0.0.1");
    TEST_ASSERT(cmcp_client_handshake(cli, p.client_t) == CMCP_OK);

    cmcp_rpc_message_t resp;
    cmcp_rpc_message_init(&resp);
    /* 999999 was never returned by call_async; wait must reject it. */
    int rc = cmcp_client_wait(cli, 999999, &resp);
    TEST_ASSERT(rc == CMCP_ENOTFOUND);

    cmcp_client_free(cli);
    cmcp_transport_close(p.client_t);
    pthread_join(th, NULL);
    cmcp_server_free(srv);
    cmcp_transport_close(p.server_t);
}

/* ====================================================================== */
/* test_server_notification — server-to-client notification routing        */
/* ====================================================================== */
/* The library doesn't yet expose a server-side notify API, so for this
 * test we hand-roll a mini-server that completes the initialize
 * handshake and then emits one notification on its own. */

static void *notif_server_thread(void *arg) {
    cmcp_transport_t *t = (cmcp_transport_t *)arg;

    /* 1. Read initialize. */
    char *frame = NULL; size_t flen = 0;
    if (cmcp_transport_read(t, &frame, &flen) != CMCP_OK) return NULL;
    cmcp_rpc_message_t *msgs = NULL; size_t nmsgs = 0;
    int prc = cmcp_rpc_parse(frame, flen, &msgs, &nmsgs);
    free(frame);
    if (prc != CMCP_OK || nmsgs != 1) {
        cmcp_rpc_messages_free(msgs, nmsgs);
        return NULL;
    }

    /* 2. Reply with a minimal initialize success. */
    cmcp_json_t *result = cmcp_json_new_object();
    cmcp_json_object_set(result, "protocolVersion",
                          cmcp_json_new_string(CMCP_PROTOCOL_VERSION));
    cmcp_json_object_set(result, "capabilities", cmcp_json_new_object());
    cmcp_json_t *si = cmcp_json_new_object();
    cmcp_json_object_set(si, "name",    cmcp_json_new_string("notif-srv"));
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

    /* 4. Emit a server→client notification. */
    cmcp_json_t *params = cmcp_json_new_object();
    cmcp_json_object_set(params, "level", cmcp_json_new_string("info"));
    cmcp_json_object_set(params, "data",  cmcp_json_new_string("hello-from-srv"));
    cmcp_rpc_message_t notif;
    cmcp_rpc_message_init(&notif);
    cmcp_rpc_make_notification(&notif, "notifications/message", params);
    wire = cmcp_rpc_emit(&notif);
    cmcp_transport_write(t, wire, strlen(wire));
    free(wire);
    cmcp_rpc_message_clear(&notif);

    /* 5. Drain whatever follows until the client closes. */
    while (cmcp_transport_read(t, &frame, &flen) == CMCP_OK) free(frame);
    return NULL;
}

typedef struct {
    pthread_mutex_t mu;
    pthread_cond_t  cv;
    int             received;
    char           *method;
    char           *data_field;
} notif_capture_t;

static void notif_handler(const char *method, const cmcp_json_t *params,
                           void *userdata) {
    notif_capture_t *c = (notif_capture_t *)userdata;
    pthread_mutex_lock(&c->mu);
    c->method = strdup(method ? method : "");
    if (params && params->type == CMCP_JSON_OBJECT) {
        const cmcp_json_t *d = cmcp_json_object_get(params, "data");
        if (d && d->type == CMCP_JSON_STRING) c->data_field = strdup(d->str.s);
    }
    c->received = 1;
    pthread_cond_broadcast(&c->cv);
    pthread_mutex_unlock(&c->mu);
}

static void test_server_notification(void) {
    transport_pair_t p;
    TEST_ASSERT(make_pair(&p) == 0);

    pthread_t th;
    TEST_ASSERT(pthread_create(&th, NULL, notif_server_thread, p.server_t) == 0);

    notif_capture_t cap = {0};
    pthread_mutex_init(&cap.mu, NULL);
    pthread_cond_init(&cap.cv, NULL);

    cmcp_client_t *cli = cmcp_client_new("notif-cli", "0.0.1");
    cmcp_client_set_notification_handler(cli, notif_handler, &cap);
    TEST_ASSERT(cmcp_client_handshake(cli, p.client_t) == CMCP_OK);

    /* Wait for the notification (cond_wait, no spinning). Bound it
     * with a timed wait so a regression doesn't hang the suite. */
    pthread_mutex_lock(&cap.mu);
    while (!cap.received) {
        struct timespec ts;
        clock_gettime(CLOCK_REALTIME, &ts);
        ts.tv_sec += 2;
        if (pthread_cond_timedwait(&cap.cv, &cap.mu, &ts) != 0) break;
    }
    int got = cap.received;
    pthread_mutex_unlock(&cap.mu);

    TEST_ASSERT(got == 1);
    TEST_ASSERT(cap.method != NULL);
    TEST_ASSERT(cap.method && strcmp(cap.method, "notifications/message") == 0);
    TEST_ASSERT(cap.data_field != NULL);
    TEST_ASSERT(cap.data_field && strcmp(cap.data_field, "hello-from-srv") == 0);

    cmcp_client_free(cli);
    cmcp_transport_close(p.client_t);
    pthread_join(th, NULL);
    cmcp_transport_close(p.server_t);

    free(cap.method);
    free(cap.data_field);
    pthread_mutex_destroy(&cap.mu);
    pthread_cond_destroy(&cap.cv);
}

/* ====================================================================== */
/* test_version_negotiation / _malformed — client-side protocol handling   */
/* ====================================================================== */
/* Per the MCP spec a version mismatch is non-fatal: the client captures
 * the server's advertised version and proceeds, leaving the host to
 * decide. Only a missing/malformed protocolVersion aborts the
 * handshake. We hand-roll a mini-server emitting a controllable
 * initialize response to exercise both paths. */

typedef struct {
    cmcp_transport_t *t;
    const char       *pv;   /* protocolVersion to advertise; NULL = omit */
} version_srv_arg_t;

static void *version_srv_thread(void *arg) {
    version_srv_arg_t *a = (version_srv_arg_t *)arg;
    cmcp_transport_t  *t = a->t;

    /* 1. Read initialize. */
    char *frame = NULL; size_t flen = 0;
    if (cmcp_transport_read(t, &frame, &flen) != CMCP_OK) return NULL;
    cmcp_rpc_message_t *msgs = NULL; size_t nmsgs = 0;
    int prc = cmcp_rpc_parse(frame, flen, &msgs, &nmsgs);
    free(frame);
    if (prc != CMCP_OK || nmsgs != 1) {
        cmcp_rpc_messages_free(msgs, nmsgs);
        return NULL;
    }

    /* 2. Reply. protocolVersion is included only when pv != NULL, so one
     *    fixture covers both the mismatch and the missing-field cases. */
    cmcp_json_t *result = cmcp_json_new_object();
    if (a->pv)
        cmcp_json_object_set(result, "protocolVersion",
                              cmcp_json_new_string(a->pv));
    cmcp_json_object_set(result, "capabilities", cmcp_json_new_object());

    cmcp_rpc_message_t resp;
    cmcp_rpc_message_init(&resp);
    cmcp_rpc_make_response(&resp, &msgs[0].id, result);
    char *wire = cmcp_rpc_emit(&resp);
    cmcp_transport_write(t, wire, strlen(wire));
    free(wire);
    cmcp_rpc_message_clear(&resp);
    cmcp_rpc_messages_free(msgs, nmsgs);

    /* 3. Drain whatever follows (notifications/initialized on success,
     *    nothing on the rejected path) until the client closes. */
    while (cmcp_transport_read(t, &frame, &flen) == CMCP_OK) free(frame);
    return NULL;
}

static void test_version_negotiation(void) {
    transport_pair_t p;
    TEST_ASSERT(make_pair(&p) == 0);

    version_srv_arg_t sa = { p.server_t, "2099-12-31" };
    pthread_t th;
    TEST_ASSERT(pthread_create(&th, NULL, version_srv_thread, &sa) == 0);

    cmcp_client_t *cli = cmcp_client_new("ver-cli", "0.0.1");
    /* A differing protocol version is NOT a handshake failure. */
    TEST_ASSERT(cmcp_client_handshake(cli, p.client_t) == CMCP_OK);
    /* The server's version is captured and exposed for the host. */
    const char *sp = cmcp_client_server_protocol(cli);
    TEST_ASSERT(sp != NULL);
    TEST_ASSERT(sp && strcmp(sp, "2099-12-31") == 0);

    cmcp_client_free(cli);
    cmcp_transport_close(p.client_t);
    pthread_join(th, NULL);
    cmcp_transport_close(p.server_t);
}

static void test_version_malformed(void) {
    transport_pair_t p;
    TEST_ASSERT(make_pair(&p) == 0);

    /* pv = NULL → server omits protocolVersion entirely. */
    version_srv_arg_t sa = { p.server_t, NULL };
    pthread_t th;
    TEST_ASSERT(pthread_create(&th, NULL, version_srv_thread, &sa) == 0);

    cmcp_client_t *cli = cmcp_client_new("ver-cli", "0.0.1");
    /* A missing protocolVersion IS fatal. */
    TEST_ASSERT(cmcp_client_handshake(cli, p.client_t) == CMCP_EPROTOCOL);
    TEST_ASSERT(cmcp_client_server_protocol(cli) == NULL);

    cmcp_client_free(cli);
    cmcp_transport_close(p.client_t);
    pthread_join(th, NULL);
    cmcp_transport_close(p.server_t);
}

/* ====================================================================== */
/* test_session_two_servers — aggregation + qualified routing              */
/* ====================================================================== */

static void test_session_two_servers(void) {
    transport_pair_t pa, pb;
    TEST_ASSERT(make_pair(&pa) == 0);
    TEST_ASSERT(make_pair(&pb) == 0);

    cmcp_server_t *srv_a = cmcp_server_new("srv-a", "0.1.0");
    cmcp_server_t *srv_b = cmcp_server_new("srv-b", "0.1.0");
    TEST_ASSERT(register_echo(srv_a, "echo_a", "A:") == CMCP_OK);
    TEST_ASSERT(register_echo(srv_b, "echo_b", "B:") == CMCP_OK);

    server_arg_t sa = { srv_a, pa.server_t, 0 };
    server_arg_t sb = { srv_b, pb.server_t, 0 };
    pthread_t th_a, th_b;
    TEST_ASSERT(pthread_create(&th_a, NULL, server_thread, &sa) == 0);
    TEST_ASSERT(pthread_create(&th_b, NULL, server_thread, &sb) == 0);

    cmcp_client_t *cli_a = cmcp_client_new("ses-cli", "0.0.1");
    cmcp_client_t *cli_b = cmcp_client_new("ses-cli", "0.0.1");
    TEST_ASSERT(cmcp_client_handshake(cli_a, pa.client_t) == CMCP_OK);
    TEST_ASSERT(cmcp_client_handshake(cli_b, pb.client_t) == CMCP_OK);

    cmcp_session_t *ses = cmcp_session_new();
    TEST_ASSERT(ses != NULL);
    TEST_ASSERT(cmcp_session_add(ses, "alpha", cli_a) == CMCP_OK);
    TEST_ASSERT(cmcp_session_add(ses, "beta",  cli_b) == CMCP_OK);

    /* Server-name validation. */
    cmcp_client_t *throwaway = cmcp_client_new("x", "x");
    TEST_ASSERT(cmcp_session_add(ses, "alpha", throwaway) == CMCP_EPROTOCOL);
    TEST_ASSERT(cmcp_session_add(ses, "bad:name", throwaway) == CMCP_EINVAL);
    TEST_ASSERT(cmcp_session_add(ses, "", throwaway) == CMCP_EINVAL);
    cmcp_client_free(throwaway);  /* still owned by us — session rejected */

    TEST_ASSERT(cmcp_session_count(ses) == 2);
    TEST_ASSERT(cmcp_session_get(ses, "alpha") == cli_a);
    TEST_ASSERT(cmcp_session_get(ses, "beta")  == cli_b);
    TEST_ASSERT(cmcp_session_get(ses, "ghost") == NULL);

    /* Aggregated tools/list — both tools, properly qualified. */
    cmcp_session_tool_t *tools = NULL;
    size_t n = 0;
    TEST_ASSERT(cmcp_session_tools_list(ses, &tools, &n) == CMCP_OK);
    TEST_ASSERT(n == 2);

    int saw_a = 0, saw_b = 0;
    for (size_t i = 0; i < n; i++) {
        if (strcmp(tools[i].qualified, "alpha:echo_a") == 0) {
            TEST_ASSERT(strcmp(tools[i].server, "alpha")  == 0);
            TEST_ASSERT(strcmp(tools[i].name,   "echo_a") == 0);
            TEST_ASSERT(tools[i].input_schema != NULL);
            saw_a = 1;
        } else if (strcmp(tools[i].qualified, "beta:echo_b") == 0) {
            TEST_ASSERT(strcmp(tools[i].server, "beta")   == 0);
            TEST_ASSERT(strcmp(tools[i].name,   "echo_b") == 0);
            TEST_ASSERT(tools[i].input_schema != NULL);
            saw_b = 1;
        }
    }
    TEST_ASSERT(saw_a == 1);
    TEST_ASSERT(saw_b == 1);
    cmcp_session_tools_free(tools, n);

    /* Routed tool_call into server A. */
    cmcp_rpc_message_t ra;
    cmcp_rpc_message_init(&ra);
    cmcp_json_t *args_a = cmcp_json_new_object();
    cmcp_json_object_set(args_a, "message", cmcp_json_new_string("hi"));
    TEST_ASSERT(cmcp_session_tool_call(ses, "alpha:echo_a", args_a, &ra)
                == CMCP_OK);
    TEST_ASSERT(ra.error == NULL);
    char *txt_a = extract_text(&ra);
    TEST_ASSERT(txt_a && strcmp(txt_a, "A:hi") == 0);
    free(txt_a);
    cmcp_rpc_message_clear(&ra);

    /* Routed tool_call into server B. */
    cmcp_rpc_message_t rb;
    cmcp_rpc_message_init(&rb);
    cmcp_json_t *args_b = cmcp_json_new_object();
    cmcp_json_object_set(args_b, "message", cmcp_json_new_string("hi"));
    TEST_ASSERT(cmcp_session_tool_call(ses, "beta:echo_b", args_b, &rb)
                == CMCP_OK);
    TEST_ASSERT(rb.error == NULL);
    char *txt_b = extract_text(&rb);
    TEST_ASSERT(txt_b && strcmp(txt_b, "B:hi") == 0);
    free(txt_b);
    cmcp_rpc_message_clear(&rb);

    /* Unknown server prefix. */
    cmcp_rpc_message_t rg;
    cmcp_rpc_message_init(&rg);
    cmcp_json_t *args_g = cmcp_json_new_object();
    cmcp_json_object_set(args_g, "message", cmcp_json_new_string("hi"));
    TEST_ASSERT(cmcp_session_tool_call(ses, "ghost:echo", args_g, &rg)
                == CMCP_ENOTFOUND);
    /* args consumed even on failure */

    /* Malformed qualified name. */
    cmcp_json_t *args_m = cmcp_json_new_object();
    cmcp_rpc_message_t rm;
    cmcp_rpc_message_init(&rm);
    TEST_ASSERT(cmcp_session_tool_call(ses, "noColon", args_m, &rm)
                == CMCP_EINVAL);

    /* cmcp_session_free closes both clients, which closes their
     * client-side transports, which gets the server threads to exit. */
    cmcp_session_free(ses);
    cmcp_transport_close(pa.client_t);
    cmcp_transport_close(pb.client_t);
    pthread_join(th_a, NULL);
    pthread_join(th_b, NULL);
    cmcp_server_free(srv_a);
    cmcp_server_free(srv_b);
    cmcp_transport_close(pa.server_t);
    cmcp_transport_close(pb.server_t);
}

/* ====================================================================== */
/* test_session_tool_call_async_fanout — F3 session-layer parallel calls    */
/* ====================================================================== */
/* The whole point of a multi-server session is fanning tool calls out
 * across servers in parallel. cmcp_session_tool_call_async keeps the
 * <server>:<tool> routing in the session and hands back the client-level
 * cmcp_tool_handle_t, so the host reaps with cmcp_session_tool_wait in
 * any order. This exercises both cross-server fan-out AND two concurrent
 * calls into the SAME server (whose ids would collide with the other
 * server's — the binding is what keeps each reap correct). */

/* Pull content[0].text out of a by-value tool result. */
static char *result_text(const cmcp_tool_result_t *res) {
    if (res->outcome != CMCP_TOOL_OK || !res->result) return NULL;
    const cmcp_json_t *arr = cmcp_json_object_get(res->result, "content");
    if (!arr || arr->type != CMCP_JSON_ARRAY || arr->arr.len == 0) return NULL;
    const cmcp_json_t *item = arr->arr.items[0];
    if (!item || item->type != CMCP_JSON_OBJECT) return NULL;
    const cmcp_json_t *t = cmcp_json_object_get(item, "text");
    if (!t || t->type != CMCP_JSON_STRING) return NULL;
    return strdup(t->str.s);
}

static void test_session_tool_call_async_fanout(void) {
    transport_pair_t pa, pb;
    TEST_ASSERT(make_pair(&pa) == 0);
    TEST_ASSERT(make_pair(&pb) == 0);

    cmcp_server_t *srv_a = cmcp_server_new("srv-a", "0.1.0");
    cmcp_server_t *srv_b = cmcp_server_new("srv-b", "0.1.0");
    TEST_ASSERT(register_echo(srv_a, "echo_a", "A:") == CMCP_OK);
    TEST_ASSERT(register_echo(srv_b, "echo_b", "B:") == CMCP_OK);

    server_arg_t sa = { srv_a, pa.server_t, 0 };
    server_arg_t sb = { srv_b, pb.server_t, 0 };
    pthread_t th_a, th_b;
    TEST_ASSERT(pthread_create(&th_a, NULL, server_thread, &sa) == 0);
    TEST_ASSERT(pthread_create(&th_b, NULL, server_thread, &sb) == 0);

    cmcp_client_t *cli_a = cmcp_client_new("ses-cli", "0.0.1");
    cmcp_client_t *cli_b = cmcp_client_new("ses-cli", "0.0.1");
    TEST_ASSERT(cmcp_client_handshake(cli_a, pa.client_t) == CMCP_OK);
    TEST_ASSERT(cmcp_client_handshake(cli_b, pb.client_t) == CMCP_OK);

    cmcp_session_t *ses = cmcp_session_new();
    TEST_ASSERT(ses != NULL);
    TEST_ASSERT(cmcp_session_add(ses, "alpha", cli_a) == CMCP_OK);
    TEST_ASSERT(cmcp_session_add(ses, "beta",  cli_b) == CMCP_OK);

    /* Fan out three calls without blocking: two into alpha, one into
     * beta. The two alpha calls + the beta call cover both same-server
     * concurrency and cross-server fan-out in one burst. */
    cmcp_json_t *a1 = cmcp_json_new_object();
    cmcp_json_object_set(a1, "message", cmcp_json_new_string("one"));
    cmcp_json_t *a2 = cmcp_json_new_object();
    cmcp_json_object_set(a2, "message", cmcp_json_new_string("two"));
    cmcp_json_t *b1 = cmcp_json_new_object();
    cmcp_json_object_set(b1, "message", cmcp_json_new_string("three"));

    cmcp_tool_handle_t h1 = cmcp_session_tool_call_async(ses, "alpha:echo_a", a1);
    cmcp_tool_handle_t h2 = cmcp_session_tool_call_async(ses, "alpha:echo_a", a2);
    cmcp_tool_handle_t h3 = cmcp_session_tool_call_async(ses, "beta:echo_b",  b1);

    TEST_ASSERT(cmcp_tool_handle_valid(h1));
    TEST_ASSERT(cmcp_tool_handle_valid(h2));
    TEST_ASSERT(cmcp_tool_handle_valid(h3));
    /* The session routed each to the right backing client. */
    TEST_ASSERT(h1.client == cli_a && h2.client == cli_a);
    TEST_ASSERT(h3.client == cli_b);

    /* Reap in a NON-submission order (h3, h1, h2) — the handle carries
     * its own client, so order and cross-server collisions don't matter. */
    cmcp_tool_result_t r3 = cmcp_session_tool_wait(h3);
    cmcp_tool_result_t r1 = cmcp_session_tool_wait(h1);
    cmcp_tool_result_t r2 = cmcp_session_tool_wait(h2);

    char *t3 = result_text(&r3);
    char *t1 = result_text(&r1);
    char *t2 = result_text(&r2);
    TEST_ASSERT(t3 && strcmp(t3, "B:three") == 0);   /* routed to beta  */
    TEST_ASSERT(t1 && strcmp(t1, "A:one")   == 0);   /* routed to alpha */
    TEST_ASSERT(t2 && strcmp(t2, "A:two")   == 0);   /* routed to alpha */
    free(t1); free(t2); free(t3);
    cmcp_tool_result_clear(&r1);
    cmcp_tool_result_clear(&r2);
    cmcp_tool_result_clear(&r3);

    /* Unknown server prefix → invalid handle, args still consumed. */
    cmcp_json_t *ag = cmcp_json_new_object();
    cmcp_json_object_set(ag, "message", cmcp_json_new_string("x"));
    cmcp_tool_handle_t hg = cmcp_session_tool_call_async(ses, "ghost:echo", ag);
    TEST_ASSERT(!cmcp_tool_handle_valid(hg));

    /* Malformed qualified name (no colon) → invalid handle, args consumed. */
    cmcp_json_t *am = cmcp_json_new_object();
    cmcp_tool_handle_t hm = cmcp_session_tool_call_async(ses, "noColon", am);
    TEST_ASSERT(!cmcp_tool_handle_valid(hm));

    /* NULL session / NULL qualified → invalid handle, args consumed. */
    cmcp_tool_handle_t hn = cmcp_session_tool_call_async(NULL, "alpha:echo_a",
                                                         cmcp_json_new_object());
    TEST_ASSERT(!cmcp_tool_handle_valid(hn));
    cmcp_tool_handle_t hq = cmcp_session_tool_call_async(ses, NULL,
                                                         cmcp_json_new_object());
    TEST_ASSERT(!cmcp_tool_handle_valid(hq));

    cmcp_session_free(ses);
    cmcp_transport_close(pa.client_t);
    cmcp_transport_close(pb.client_t);
    pthread_join(th_a, NULL);
    pthread_join(th_b, NULL);
    cmcp_server_free(srv_a);
    cmcp_server_free(srv_b);
    cmcp_transport_close(pa.server_t);
    cmcp_transport_close(pb.server_t);
}

/* ====================================================================== */

int main(void) {
    fprintf(stderr, "test_client_server:\n");

    TEST_RUN(test_request_sync);
    TEST_RUN(test_async_call);
    TEST_RUN(test_async_wait_unknown_id);
    TEST_RUN(test_server_notification);
    TEST_RUN(test_version_negotiation);
    TEST_RUN(test_version_malformed);
    TEST_RUN(test_session_two_servers);
    TEST_RUN(test_session_tool_call_async_fanout);

    TEST_DONE();
}
