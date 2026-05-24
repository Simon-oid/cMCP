/* Hostile-peer test suite (phase 5.5).
 *
 * Each case wires up one real cMCP side (client OR server) against a
 * raw pipe-fd "hostile peer" that the test drives by hand. The hostile
 * side writes whatever bytes a misbehaving implementation might send:
 * unmatched response ids, duplicate ids, malformed JSON, unknown
 * methods, schema-violating tool calls, pre-handshake methods, etc.
 *
 * Pass criteria for every case:
 *   - the real side does not crash, abort, or leak
 *   - the real side surfaces the behaviour the spec mandates (-32601
 *     for unknown methods, -32602 for schema-violating params, etc.)
 *   - subsequent legitimate traffic on the same session still works
 *
 * Pairs naturally with the sanitizer matrix: when run under
 * ASan/UBSan/TSan, any heap corruption / use-after-free / data race
 * the hostile traffic could trigger will fail the test instead of
 * silently passing. */

#define _POSIX_C_SOURCE 200809L

#include "test.h"
#include "cmcp.h"
#include "cmcp_client.h"
#include "cmcp_json.h"
#include "cmcp_server.h"
#include "cmcp_transport.h"
#include "cmcp_types.h"

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <unistd.h>

/* ====================================================================== */
/* Pipe-pair scaffolding                                                   */
/* ====================================================================== */
/* The "real" side gets a stdio transport over its end of two pipes; the
 * "hostile" side keeps the raw file descriptors so it can write arbitrary
 * bytes (including framing violations) and read what the real side emits.
 * Layout mirrors test_client_server's make_pair but exposes the raw fds. */

typedef struct {
    cmcp_transport_t *real_t;  /* the legitimate cMCP transport */
    int               h_read;  /* hostile reads what real side wrote */
    int               h_write; /* hostile writes what real side will read */
} pair_t;

/* Build a transport for the *client* role on one end and expose the raw
 * fds for a hostile *server* on the other end. */
static int make_client_pair(pair_t *out) {
    int c2s[2], s2c[2];
    if (pipe(c2s) != 0) return -1;
    if (pipe(s2c) != 0) { close(c2s[0]); close(c2s[1]); return -1; }
    /* Real client: reads s2c, writes c2s. */
    out->real_t  = cmcp_transport_stdio_new_fds(s2c[0], c2s[1]);
    out->h_read  = c2s[0];
    out->h_write = s2c[1];
    if (!out->real_t) {
        close(c2s[0]); close(c2s[1]); close(s2c[0]); close(s2c[1]);
        return -1;
    }
    return 0;
}

/* Mirror image: real *server* on one end, hostile *client* on the other. */
static int make_server_pair(pair_t *out) {
    int c2s[2], s2c[2];
    if (pipe(c2s) != 0) return -1;
    if (pipe(s2c) != 0) { close(c2s[0]); close(c2s[1]); return -1; }
    /* Real server: reads c2s, writes s2c. */
    out->real_t  = cmcp_transport_stdio_new_fds(c2s[0], s2c[1]);
    out->h_read  = s2c[0];
    out->h_write = c2s[1];
    if (!out->real_t) {
        close(c2s[0]); close(c2s[1]); close(s2c[0]); close(s2c[1]);
        return -1;
    }
    return 0;
}

/* Write one newline-framed JSON frame; the trailing '\n' is what the
 * stdio transport splits on. */
static int hostile_send(int fd, const char *line) {
    size_t n = strlen(line);
    if (write(fd, line, n) != (ssize_t)n) return -1;
    if (write(fd, "\n", 1) != 1) return -1;
    return 0;
}

/* Read one newline-framed frame with a wallclock cap. Returns a malloc'd
 * NUL-terminated string (caller frees) or NULL on EOF / timeout / OOM.
 * Used to inspect what the real side sent back. */
static char *hostile_recv(int fd, int timeout_ms) {
    char  *buf  = NULL;
    size_t cap  = 0;
    size_t used = 0;
    for (;;) {
        fd_set r;
        FD_ZERO(&r); FD_SET(fd, &r);
        struct timeval tv = { timeout_ms / 1000, (timeout_ms % 1000) * 1000 };
        int sel = select(fd + 1, &r, NULL, NULL, &tv);
        if (sel <= 0) { free(buf); return NULL; }

        char c;
        ssize_t rc = read(fd, &c, 1);
        if (rc <= 0) { free(buf); return NULL; }
        if (c == '\n') {
            if (!buf) buf = (char *)malloc(1);
            if (buf) buf[used] = '\0';
            return buf;
        }
        if (used + 1 >= cap) {
            cap = cap ? cap * 2 : 128;
            char *nb = (char *)realloc(buf, cap);
            if (!nb) { free(buf); return NULL; }
            buf = nb;
        }
        buf[used++] = c;
    }
}

/* ====================================================================== */
/* Helpers: drive the cMCP handshake from the hostile side                 */
/* ====================================================================== */

/* Read the real client's `initialize` request and emit a canned successful
 * response, then read and discard `notifications/initialized`. The id of
 * the initialize request is reflected back so the client's pending table
 * matches. Returns 0 on success. */
static int hostile_server_complete_handshake(int rfd, int wfd) {
    char *init_req = hostile_recv(rfd, 2000);
    if (!init_req) return -1;
    /* Extract `"id":N` — the client always uses an integer id starting at 1
     * but we don't want to hard-code; do a tiny ad-hoc scan. */
    long long id = 1;
    const char *p = strstr(init_req, "\"id\":");
    if (p) id = strtoll(p + 5, NULL, 10);
    free(init_req);

    char resp[512];
    snprintf(resp, sizeof resp,
        "{\"jsonrpc\":\"2.0\",\"id\":%lld,\"result\":{"
        "\"protocolVersion\":\"%s\","
        "\"capabilities\":{},"
        "\"serverInfo\":{\"name\":\"hostile\",\"version\":\"0.0.1\"}}}",
        id, CMCP_PROTOCOL_VERSION);
    if (hostile_send(wfd, resp) != 0) return -1;

    char *initd = hostile_recv(rfd, 2000);
    if (!initd) return -1;
    free(initd);
    return 0;
}

/* Drive the handshake from a hostile *client* against a real server. */
static int hostile_client_complete_handshake(int rfd, int wfd) {
    if (hostile_send(wfd,
        "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"initialize\","
        "\"params\":{\"protocolVersion\":\"" CMCP_PROTOCOL_VERSION "\","
        "\"capabilities\":{},"
        "\"clientInfo\":{\"name\":\"hostile-client\",\"version\":\"0.0.1\"}}}") != 0)
        return -1;
    char *resp = hostile_recv(rfd, 2000);
    if (!resp) return -1;
    free(resp);
    /* Notifications-initialized is fire-and-forget. */
    if (hostile_send(wfd,
        "{\"jsonrpc\":\"2.0\",\"method\":\"notifications/initialized\"}") != 0)
        return -1;
    return 0;
}

/* ====================================================================== */
/* Echo tool — used by the schema-violation test                           */
/* ====================================================================== */

static int echo_tool(const cmcp_json_t *arguments, void *userdata,
                      cmcp_handler_ctx_t *hctx,
                      cmcp_json_t **out_content, int *out_is_error) {
    (void)userdata; (void)hctx; (void)out_is_error;
    const cmcp_json_t *m = arguments
        ? cmcp_json_object_get(arguments, "x") : NULL;
    const char *msg = (m && m->type == CMCP_JSON_STRING) ? m->str.s : "";
    *out_content = cmcp_tool_text_content(msg);
    return *out_content ? CMCP_OK : CMCP_ENOMEM;
}

static const char echo_schema[] =
    "{\"type\":\"object\","
     "\"properties\":{\"x\":{\"type\":\"string\"}},"
     "\"required\":[\"x\"],"
     "\"additionalProperties\":false}";

/* ====================================================================== */
/* Hostile-server thread plumbing                                           */
/* ====================================================================== */
/* Each hostile-server scenario runs in its own thread so the real client
 * (which calls blocking handshake/request fns on the test thread) has a
 * peer to talk to. */

typedef int (*hostile_server_fn)(int rfd, int wfd, void *ud);

typedef struct {
    int                rfd;
    int                wfd;
    hostile_server_fn  fn;
    void              *ud;
    int                rc;
} hostile_arg_t;

static void *hostile_server_thread(void *arg) {
    hostile_arg_t *a = (hostile_arg_t *)arg;
    a->rc = a->fn(a->rfd, a->wfd, a->ud);
    return NULL;
}

/* ====================================================================== */
/* Server-thread plumbing (for hostile-client tests)                       */
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

/* ====================================================================== */
/* (A) Hostile server: unsolicited response with an unmatched id           */
/* ====================================================================== */
/* Server pushes a response carrying an id that was never registered in
 * the client's pending table, then answers the real call. The client
 * must silently drop the stray and still deliver the legitimate one. */

static int scenario_unmatched_id(int rfd, int wfd, void *ud) {
    (void)ud;
    if (hostile_server_complete_handshake(rfd, wfd) != 0) return -1;
    /* Stray response — no matching pending entry. */
    if (hostile_send(wfd,
        "{\"jsonrpc\":\"2.0\",\"id\":9999,\"result\":{\"stray\":true}}") != 0)
        return -1;
    /* Now read the real client's request and reflect its id back. */
    char *req = hostile_recv(rfd, 2000);
    if (!req) return -1;
    long long id = 0;
    const char *p = strstr(req, "\"id\":");
    if (p) id = strtoll(p + 5, NULL, 10);
    free(req);
    char resp[256];
    snprintf(resp, sizeof resp,
        "{\"jsonrpc\":\"2.0\",\"id\":%lld,\"result\":{\"ok\":true}}", id);
    return hostile_send(wfd, resp);
}

static void test_hostile_server_unmatched_response_id(void) {
    pair_t p;
    TEST_ASSERT(make_client_pair(&p) == 0);

    pthread_t th;
    hostile_arg_t arg = { p.h_read, p.h_write, scenario_unmatched_id, NULL, 0 };
    pthread_create(&th, NULL, hostile_server_thread, &arg);

    cmcp_client_t *c = cmcp_client_new("real-client", "0.0.1");
    TEST_ASSERT(c != NULL);
    TEST_ASSERT(cmcp_client_handshake(c, p.real_t) == CMCP_OK);

    cmcp_rpc_message_t resp;
    cmcp_rpc_message_init(&resp);
    int rc = cmcp_client_request(c, "tools/list", NULL, &resp);
    TEST_ASSERT(rc == CMCP_OK);
    TEST_ASSERT(resp.kind == CMCP_MSG_RESPONSE);
    TEST_ASSERT(resp.result != NULL);
    cmcp_rpc_message_clear(&resp);

    cmcp_client_free(c);
    pthread_join(th, NULL);
    TEST_ASSERT(arg.rc == 0);
    cmcp_transport_close(p.real_t);
    close(p.h_read); close(p.h_write);
}

/* ====================================================================== */
/* (B) Hostile server: duplicate response for the same id                  */
/* ====================================================================== */
/* The first response wins; the second must be silently dropped without
 * disturbing the next legitimate call (which uses a fresh id). */

static int scenario_duplicate_response(int rfd, int wfd, void *ud) {
    (void)ud;
    if (hostile_server_complete_handshake(rfd, wfd) != 0) return -1;
    /* First real call. */
    char *req1 = hostile_recv(rfd, 2000);
    if (!req1) return -1;
    long long id1 = 0;
    const char *p = strstr(req1, "\"id\":");
    if (p) id1 = strtoll(p + 5, NULL, 10);
    free(req1);
    char resp[256];
    snprintf(resp, sizeof resp,
        "{\"jsonrpc\":\"2.0\",\"id\":%lld,\"result\":{\"first\":true}}", id1);
    if (hostile_send(wfd, resp) != 0) return -1;
    /* Duplicate, same id, different payload — must be dropped. */
    snprintf(resp, sizeof resp,
        "{\"jsonrpc\":\"2.0\",\"id\":%lld,\"result\":{\"dup\":true}}", id1);
    if (hostile_send(wfd, resp) != 0) return -1;
    /* Second real call. */
    char *req2 = hostile_recv(rfd, 2000);
    if (!req2) return -1;
    long long id2 = 0;
    p = strstr(req2, "\"id\":");
    if (p) id2 = strtoll(p + 5, NULL, 10);
    free(req2);
    snprintf(resp, sizeof resp,
        "{\"jsonrpc\":\"2.0\",\"id\":%lld,\"result\":{\"second\":true}}", id2);
    return hostile_send(wfd, resp);
}

static void test_hostile_server_duplicate_response(void) {
    pair_t p;
    TEST_ASSERT(make_client_pair(&p) == 0);

    pthread_t th;
    hostile_arg_t arg = { p.h_read, p.h_write,
                           scenario_duplicate_response, NULL, 0 };
    pthread_create(&th, NULL, hostile_server_thread, &arg);

    cmcp_client_t *c = cmcp_client_new("real-client", "0.0.1");
    TEST_ASSERT(cmcp_client_handshake(c, p.real_t) == CMCP_OK);

    cmcp_rpc_message_t r1, r2;
    cmcp_rpc_message_init(&r1);
    cmcp_rpc_message_init(&r2);
    TEST_ASSERT(cmcp_client_request(c, "tools/list", NULL, &r1) == CMCP_OK);
    TEST_ASSERT(r1.result != NULL);
    /* The first-or-dup race is between the reader thread and the wire;
     * what we care about is that ONE of the two delivered cleanly. */
    const cmcp_json_t *first =
        r1.result ? cmcp_json_object_get(r1.result, "first") : NULL;
    const cmcp_json_t *dup =
        r1.result ? cmcp_json_object_get(r1.result, "dup")   : NULL;
    TEST_ASSERT(first != NULL || dup != NULL);

    TEST_ASSERT(cmcp_client_request(c, "tools/list", NULL, &r2) == CMCP_OK);
    TEST_ASSERT(r2.result != NULL);
    const cmcp_json_t *second =
        r2.result ? cmcp_json_object_get(r2.result, "second") : NULL;
    TEST_ASSERT(second != NULL);
    cmcp_rpc_message_clear(&r1);
    cmcp_rpc_message_clear(&r2);

    cmcp_client_free(c);
    pthread_join(th, NULL);
    TEST_ASSERT(arg.rc == 0);
    cmcp_transport_close(p.real_t);
    close(p.h_read); close(p.h_write);
}

/* ====================================================================== */
/* (C) Hostile server: malformed JSON in the middle of a session           */
/* ====================================================================== */
/* Reader-thread must swallow the parse error and stay alive; the next
 * legitimate response is still delivered to its waiter. */

static int scenario_malformed_then_real(int rfd, int wfd, void *ud) {
    (void)ud;
    if (hostile_server_complete_handshake(rfd, wfd) != 0) return -1;
    /* Junk frame before answering the real request. */
    if (hostile_send(wfd, "{not json at all,,,") != 0) return -1;
    char *req = hostile_recv(rfd, 2000);
    if (!req) return -1;
    long long id = 0;
    const char *p = strstr(req, "\"id\":");
    if (p) id = strtoll(p + 5, NULL, 10);
    free(req);
    /* Also send a junk frame AFTER the request arrives but BEFORE the
     * legitimate response — exercises mid-stream interleaving. */
    if (hostile_send(wfd, "][[][") != 0) return -1;
    char resp[256];
    snprintf(resp, sizeof resp,
        "{\"jsonrpc\":\"2.0\",\"id\":%lld,\"result\":{\"survived\":true}}", id);
    return hostile_send(wfd, resp);
}

static void test_hostile_server_malformed_json(void) {
    pair_t p;
    TEST_ASSERT(make_client_pair(&p) == 0);

    pthread_t th;
    hostile_arg_t arg = { p.h_read, p.h_write,
                           scenario_malformed_then_real, NULL, 0 };
    pthread_create(&th, NULL, hostile_server_thread, &arg);

    cmcp_client_t *c = cmcp_client_new("real-client", "0.0.1");
    TEST_ASSERT(cmcp_client_handshake(c, p.real_t) == CMCP_OK);

    cmcp_rpc_message_t resp;
    cmcp_rpc_message_init(&resp);
    TEST_ASSERT(cmcp_client_request(c, "tools/list", NULL, &resp) == CMCP_OK);
    TEST_ASSERT(resp.result != NULL);
    const cmcp_json_t *survived =
        resp.result ? cmcp_json_object_get(resp.result, "survived") : NULL;
    TEST_ASSERT(survived != NULL);
    cmcp_rpc_message_clear(&resp);

    cmcp_client_free(c);
    pthread_join(th, NULL);
    TEST_ASSERT(arg.rc == 0);
    cmcp_transport_close(p.real_t);
    close(p.h_read); close(p.h_write);
}

/* ====================================================================== */
/* (D) Hostile server: server-initiated request with an unknown method     */
/* ====================================================================== */
/* The client must reply -32601 (method not found) and keep going. */

static int scenario_unknown_server_request(int rfd, int wfd, void *ud) {
    (void)ud;
    if (hostile_server_complete_handshake(rfd, wfd) != 0) return -1;
    if (hostile_send(wfd,
        "{\"jsonrpc\":\"2.0\",\"id\":42,\"method\":\"evil/method\","
        "\"params\":{}}") != 0)
        return -1;
    char *reply = hostile_recv(rfd, 2000);
    if (!reply) return -1;
    int ok = (strstr(reply, "\"id\":42") != NULL) &&
             (strstr(reply, "-32601")   != NULL);
    free(reply);
    return ok ? 0 : -1;
}

static void test_hostile_server_unknown_request_replied(void) {
    pair_t p;
    TEST_ASSERT(make_client_pair(&p) == 0);

    pthread_t th;
    hostile_arg_t arg = { p.h_read, p.h_write,
                           scenario_unknown_server_request, NULL, 0 };
    pthread_create(&th, NULL, hostile_server_thread, &arg);

    cmcp_client_t *c = cmcp_client_new("real-client", "0.0.1");
    TEST_ASSERT(cmcp_client_handshake(c, p.real_t) == CMCP_OK);

    /* Give the hostile thread time to round-trip the request/response. */
    pthread_join(th, NULL);
    TEST_ASSERT(arg.rc == 0);

    cmcp_client_free(c);
    cmcp_transport_close(p.real_t);
    close(p.h_read); close(p.h_write);
}

/* ====================================================================== */
/* (E) Hostile client: methods before handshake are rejected               */
/* ====================================================================== */

static void test_hostile_client_pre_handshake_method(void) {
    pair_t p;
    TEST_ASSERT(make_server_pair(&p) == 0);

    cmcp_server_t *s = cmcp_server_new("real-server", "0.0.1");
    TEST_ASSERT(s != NULL);

    pthread_t th;
    server_arg_t sa = { s, p.real_t, 0 };
    pthread_create(&th, NULL, server_thread, &sa);

    /* No initialize — straight to tools/list. */
    TEST_ASSERT(hostile_send(p.h_write,
        "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"tools/list\"}") == 0);
    char *resp = hostile_recv(p.h_read, 2000);
    TEST_ASSERT(resp != NULL);
    TEST_ASSERT(resp && strstr(resp, "\"id\":1") != NULL);
    TEST_ASSERT(resp && strstr(resp, "-32600")  != NULL);
    free(resp);

    /* Tear down: closing the hostile-write fd gives the server EOF. */
    close(p.h_write);
    pthread_join(th, NULL);

    cmcp_transport_close(p.real_t);
    cmcp_server_free(s);
    close(p.h_read);
}

/* ====================================================================== */
/* (F) Hostile client: unknown method post-handshake → -32601              */
/* ====================================================================== */

static void test_hostile_client_unknown_method(void) {
    pair_t p;
    TEST_ASSERT(make_server_pair(&p) == 0);

    cmcp_server_t *s = cmcp_server_new("real-server", "0.0.1");
    TEST_ASSERT(s != NULL);

    pthread_t th;
    server_arg_t sa = { s, p.real_t, 0 };
    pthread_create(&th, NULL, server_thread, &sa);

    TEST_ASSERT(hostile_client_complete_handshake(p.h_read, p.h_write) == 0);

    TEST_ASSERT(hostile_send(p.h_write,
        "{\"jsonrpc\":\"2.0\",\"id\":7,\"method\":\"weird/method\"}") == 0);
    char *resp = hostile_recv(p.h_read, 2000);
    TEST_ASSERT(resp != NULL);
    TEST_ASSERT(resp && strstr(resp, "\"id\":7") != NULL);
    TEST_ASSERT(resp && strstr(resp, "-32601")  != NULL);
    free(resp);

    close(p.h_write);
    pthread_join(th, NULL);

    cmcp_transport_close(p.real_t);
    cmcp_server_free(s);
    close(p.h_read);
}

/* ====================================================================== */
/* (G) Hostile client: tools/call against an unregistered tool             */
/* ====================================================================== */
/* The server has one tool ("echo"); the hostile call names a different
 * one. The spec uses INVALID_PARAMS for unknown tool names. */

static void test_hostile_client_tools_call_unknown_tool(void) {
    pair_t p;
    TEST_ASSERT(make_server_pair(&p) == 0);

    cmcp_server_t *s = cmcp_server_new("real-server", "0.0.1");
    TEST_ASSERT(s != NULL);
    cmcp_tool_t tool = {0};
    tool.name         = "echo";
    tool.description  = "echoes x";
    tool.input_schema = echo_schema;
    tool.handler      = echo_tool;
    TEST_ASSERT(cmcp_server_add_tool(s, &tool) == CMCP_OK);

    pthread_t th;
    server_arg_t sa = { s, p.real_t, 0 };
    pthread_create(&th, NULL, server_thread, &sa);

    TEST_ASSERT(hostile_client_complete_handshake(p.h_read, p.h_write) == 0);

    TEST_ASSERT(hostile_send(p.h_write,
        "{\"jsonrpc\":\"2.0\",\"id\":11,\"method\":\"tools/call\","
        "\"params\":{\"name\":\"no_such_tool\",\"arguments\":{}}}") == 0);
    char *resp = hostile_recv(p.h_read, 2000);
    TEST_ASSERT(resp != NULL);
    TEST_ASSERT(resp && strstr(resp, "\"id\":11") != NULL);
    /* Error field present, any negative code is fine — must not be a result. */
    TEST_ASSERT(resp && strstr(resp, "\"error\"") != NULL);
    TEST_ASSERT(resp && strstr(resp, "\"result\"") == NULL);
    free(resp);

    close(p.h_write);
    pthread_join(th, NULL);

    cmcp_transport_close(p.real_t);
    cmcp_server_free(s);
    close(p.h_read);
}

/* ====================================================================== */
/* (H) Hostile client: tools/call whose arguments violate the input schema */
/* ====================================================================== */
/* `echo` requires {x:string}; we send {}. Schema validator must reject
 * with -32602 and structured data (path/keyword/message). */

static void test_hostile_client_tools_call_schema_violation(void) {
    pair_t p;
    TEST_ASSERT(make_server_pair(&p) == 0);

    cmcp_server_t *s = cmcp_server_new("real-server", "0.0.1");
    TEST_ASSERT(s != NULL);
    cmcp_tool_t tool = {0};
    tool.name         = "echo";
    tool.description  = "echoes x";
    tool.input_schema = echo_schema;
    tool.handler      = echo_tool;
    TEST_ASSERT(cmcp_server_add_tool(s, &tool) == CMCP_OK);

    pthread_t th;
    server_arg_t sa = { s, p.real_t, 0 };
    pthread_create(&th, NULL, server_thread, &sa);

    TEST_ASSERT(hostile_client_complete_handshake(p.h_read, p.h_write) == 0);

    TEST_ASSERT(hostile_send(p.h_write,
        "{\"jsonrpc\":\"2.0\",\"id\":13,\"method\":\"tools/call\","
        "\"params\":{\"name\":\"echo\",\"arguments\":{}}}") == 0);
    char *resp = hostile_recv(p.h_read, 2000);
    TEST_ASSERT(resp != NULL);
    TEST_ASSERT(resp && strstr(resp, "\"id\":13") != NULL);
    TEST_ASSERT(resp && strstr(resp, "-32602")   != NULL);
    /* The validator should report a `required`-keyword failure. */
    TEST_ASSERT(resp && strstr(resp, "required")  != NULL);
    free(resp);

    /* Sanity: after a schema rejection the server keeps serving. */
    TEST_ASSERT(hostile_send(p.h_write,
        "{\"jsonrpc\":\"2.0\",\"id\":14,\"method\":\"tools/call\","
        "\"params\":{\"name\":\"echo\",\"arguments\":{\"x\":\"hi\"}}}") == 0);
    char *resp2 = hostile_recv(p.h_read, 2000);
    TEST_ASSERT(resp2 != NULL);
    TEST_ASSERT(resp2 && strstr(resp2, "\"id\":14") != NULL);
    TEST_ASSERT(resp2 && strstr(resp2, "\"result\"") != NULL);
    free(resp2);

    close(p.h_write);
    pthread_join(th, NULL);

    cmcp_transport_close(p.real_t);
    cmcp_server_free(s);
    close(p.h_read);
}

/* ====================================================================== */
/* (I) Hostile client: malformed JSON frame → -32700, server keeps going   */
/* ====================================================================== */

static void test_hostile_client_malformed_json(void) {
    pair_t p;
    TEST_ASSERT(make_server_pair(&p) == 0);

    cmcp_server_t *s = cmcp_server_new("real-server", "0.0.1");
    TEST_ASSERT(s != NULL);

    pthread_t th;
    server_arg_t sa = { s, p.real_t, 0 };
    pthread_create(&th, NULL, server_thread, &sa);

    /* Junk before handshake — server should reply -32700 with null id. */
    TEST_ASSERT(hostile_send(p.h_write, "{not json,,,") == 0);
    char *resp = hostile_recv(p.h_read, 2000);
    TEST_ASSERT(resp != NULL);
    TEST_ASSERT(resp && strstr(resp, "-32700") != NULL);
    free(resp);

    /* Server should still process subsequent frames. */
    TEST_ASSERT(hostile_client_complete_handshake(p.h_read, p.h_write) == 0);

    TEST_ASSERT(hostile_send(p.h_write,
        "{\"jsonrpc\":\"2.0\",\"id\":21,\"method\":\"tools/list\"}") == 0);
    char *resp2 = hostile_recv(p.h_read, 2000);
    TEST_ASSERT(resp2 != NULL);
    TEST_ASSERT(resp2 && strstr(resp2, "\"id\":21")  != NULL);
    TEST_ASSERT(resp2 && strstr(resp2, "\"result\"") != NULL);
    free(resp2);

    close(p.h_write);
    pthread_join(th, NULL);

    cmcp_transport_close(p.real_t);
    cmcp_server_free(s);
    close(p.h_read);
}

/* ====================================================================== */

int main(void) {
    fprintf(stderr, "test_hostile_peer\n");
    /* Real-client vs hostile-server */
    TEST_RUN(test_hostile_server_unmatched_response_id);
    TEST_RUN(test_hostile_server_duplicate_response);
    TEST_RUN(test_hostile_server_malformed_json);
    TEST_RUN(test_hostile_server_unknown_request_replied);
    /* Real-server vs hostile-client */
    TEST_RUN(test_hostile_client_pre_handshake_method);
    TEST_RUN(test_hostile_client_unknown_method);
    TEST_RUN(test_hostile_client_tools_call_unknown_tool);
    TEST_RUN(test_hostile_client_tools_call_schema_violation);
    TEST_RUN(test_hostile_client_malformed_json);
    TEST_DONE();
}
