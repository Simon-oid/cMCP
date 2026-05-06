/* HTTP client transport tests — exercises the Phase 2.2 client against
 * an in-process Phase 2.1 server.
 *
 * Each test:
 *   1) Brings up a real cmcp_server_t on cmcp_transport_http_listen
 *      (ephemeral 127.0.0.1 port).
 *   2) Connects a real cmcp_client_t through cmcp_transport_http_connect.
 *   3) Drives the resulting client: handshake, list tools, call tools.
 *
 * This is the same end-to-end shape as test_client_server.c, but on a
 * full HTTP wire (including the client.c → curl POST → HTTP/1.1 →
 * server.c run loop → HTTP response → curl → client.c reader path). */

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
/* Server harness (mirrors test_http_server)                               */
/* ====================================================================== */

typedef struct {
    cmcp_server_t    *s;
    cmcp_transport_t *t;
    int               rc;
} server_arg_t;

static void *server_thread_main(void *arg) {
    server_arg_t *a = (server_arg_t *)arg;
    a->rc = cmcp_server_run(a->s, a->t);
    return NULL;
}

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
    unsigned short p = ntohs(sa.sin_port);
    close(fd);
    return p;
}

static char *make_url(unsigned short port) {
    char *u = (char *)malloc(64);
    snprintf(u, 64, "http://127.0.0.1:%u/mcp", port);
    return u;
}

/* ====================================================================== */
/* Test tools                                                              */
/* ====================================================================== */

static int echo_tool(const cmcp_json_t *args, void *userdata,
                      cmcp_json_t **out_content, int *out_is_error) {
    (void)userdata; (void)out_is_error;
    const cmcp_json_t *m = args ? cmcp_json_object_get(args, "message") : NULL;
    const char *msg = (m && m->type == CMCP_JSON_STRING) ? m->str.s : "";
    *out_content = cmcp_tool_text_content(msg);
    return *out_content ? CMCP_OK : CMCP_ENOMEM;
}

static int register_echo(cmcp_server_t *srv) {
    cmcp_tool_t t = {0};
    t.name         = "echo";
    t.description  = "echo a message";
    t.input_schema = "{\"type\":\"object\",\"properties\":"
                      "{\"message\":{\"type\":\"string\"}},"
                      "\"required\":[\"message\"]}";
    t.handler      = echo_tool;
    return cmcp_server_add_tool(srv, &t);
}

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

static cmcp_json_t *make_tool_call_params(const char *tool, const char *msg) {
    cmcp_json_t *p = cmcp_json_new_object();
    cmcp_json_object_set(p, "name", cmcp_json_new_string(tool));
    cmcp_json_t *args = cmcp_json_new_object();
    cmcp_json_object_set(args, "message", cmcp_json_new_string(msg));
    cmcp_json_object_set(p, "arguments", args);
    return p;
}

/* Bring up server + connect client. Caller cleans up via cleanup(). */
typedef struct {
    unsigned short    port;
    cmcp_transport_t *server_t;
    cmcp_server_t    *srv;
    pthread_t         server_th;
    cmcp_transport_t *client_t;
    cmcp_client_t    *cli;
} fixture_t;

static int fixture_up(fixture_t *f) {
    memset(f, 0, sizeof *f);
    f->port = pick_port();
    if (!f->port) return -1;

    f->server_t = cmcp_transport_http_listen("127.0.0.1", f->port);
    if (!f->server_t) return -1;

    f->srv = cmcp_server_new("http-srv", "0.1.0");
    register_echo(f->srv);
    server_arg_t *sa = (server_arg_t *)calloc(1, sizeof *sa);
    sa->s = f->srv; sa->t = f->server_t;
    if (pthread_create(&f->server_th, NULL, server_thread_main, sa) != 0) {
        free(sa);
        return -1;
    }
    /* Brief delay to let the acceptor reach poll(). */
    struct timespec ts = { 0, 20 * 1000 * 1000 };  /* 20ms */
    nanosleep(&ts, NULL);

    char *url = make_url(f->port);
    f->client_t = cmcp_transport_http_connect(url);
    free(url);
    if (!f->client_t) return -1;

    f->cli = cmcp_client_new("http-cli", "0.0.1");
    if (cmcp_client_handshake(f->cli, f->client_t) != CMCP_OK) return -1;
    return 0;
}

static void fixture_down(fixture_t *f) {
    if (f->cli) cmcp_client_free(f->cli);
    if (f->client_t) cmcp_transport_close(f->client_t);
    if (f->server_t) cmcp_transport_close(f->server_t);
    pthread_join(f->server_th, NULL);
    if (f->srv) cmcp_server_free(f->srv);
}

/* ====================================================================== */
/* Tests                                                                   */
/* ====================================================================== */

static void test_handshake_and_call(void) {
    fixture_t f;
    TEST_ASSERT(fixture_up(&f) == 0);

    /* Server identity captured during handshake. */
    TEST_ASSERT(cmcp_client_server_name(f.cli) != NULL);
    TEST_ASSERT(strcmp(cmcp_client_server_name(f.cli), "http-srv") == 0);

    /* tools/list returns our registered tool. */
    cmcp_rpc_message_t resp;
    cmcp_rpc_message_init(&resp);
    TEST_ASSERT(cmcp_client_request(f.cli, "tools/list", NULL, &resp)
                == CMCP_OK);
    TEST_ASSERT(resp.error == NULL);
    const cmcp_json_t *tools = cmcp_json_object_get(resp.result, "tools");
    TEST_ASSERT(tools && tools->type == CMCP_JSON_ARRAY);
    TEST_ASSERT(tools && tools->arr.len == 1);
    cmcp_rpc_message_clear(&resp);

    /* tools/call round-trip. */
    cmcp_rpc_message_init(&resp);
    cmcp_json_t *params = make_tool_call_params("echo", "hello-http");
    TEST_ASSERT(cmcp_client_request(f.cli, "tools/call", params, &resp)
                == CMCP_OK);
    TEST_ASSERT(resp.error == NULL);
    char *txt = extract_text(&resp);
    TEST_ASSERT(txt && strcmp(txt, "hello-http") == 0);
    free(txt);
    cmcp_rpc_message_clear(&resp);

    fixture_down(&f);
}

static void test_async_multi_inflight(void) {
    fixture_t f;
    TEST_ASSERT(fixture_up(&f) == 0);

    /* Three async POSTs back-to-back. Each one is a separate HTTP
     * exchange, each from the application thread sequentially (the
     * client.c demuxer doesn't itself parallelize POSTs — the test
     * here verifies that several call_async + wait pairs route
     * correctly via JSON-RPC id). */
    long long id1, id2, id3;
    TEST_ASSERT(cmcp_client_call_async(f.cli, "tools/call",
                make_tool_call_params("echo", "one"),   &id1) == CMCP_OK);
    TEST_ASSERT(cmcp_client_call_async(f.cli, "tools/call",
                make_tool_call_params("echo", "two"),   &id2) == CMCP_OK);
    TEST_ASSERT(cmcp_client_call_async(f.cli, "tools/call",
                make_tool_call_params("echo", "three"), &id3) == CMCP_OK);

    cmcp_rpc_message_t r1, r2, r3;
    cmcp_rpc_message_init(&r1);
    cmcp_rpc_message_init(&r2);
    cmcp_rpc_message_init(&r3);
    TEST_ASSERT(cmcp_client_wait(f.cli, id3, &r3) == CMCP_OK);
    TEST_ASSERT(cmcp_client_wait(f.cli, id1, &r1) == CMCP_OK);
    TEST_ASSERT(cmcp_client_wait(f.cli, id2, &r2) == CMCP_OK);

    char *t1 = extract_text(&r1);
    char *t2 = extract_text(&r2);
    char *t3 = extract_text(&r3);
    TEST_ASSERT(t1 && strcmp(t1, "one")   == 0);
    TEST_ASSERT(t2 && strcmp(t2, "two")   == 0);
    TEST_ASSERT(t3 && strcmp(t3, "three") == 0);
    free(t1); free(t2); free(t3);
    cmcp_rpc_message_clear(&r1);
    cmcp_rpc_message_clear(&r2);
    cmcp_rpc_message_clear(&r3);

    fixture_down(&f);
}

static void test_session_id_propagates(void) {
    /* The HTTP client transport must latch the session id from the
     * initialize response and propagate it on subsequent POSTs. We
     * can't observe the wire here directly; we verify indirectly by
     * issuing a non-init request *after* handshake — if the session
     * id weren't propagated the server would 400 us. */
    fixture_t f;
    TEST_ASSERT(fixture_up(&f) == 0);

    cmcp_rpc_message_t resp;
    cmcp_rpc_message_init(&resp);
    TEST_ASSERT(cmcp_client_request(f.cli, "tools/list", NULL, &resp)
                == CMCP_OK);
    /* If session-id wasn't carried we'd see a transport error or a
     * synthesized error response. We see a real success result. */
    TEST_ASSERT(resp.error == NULL);
    TEST_ASSERT(resp.result != NULL);
    cmcp_rpc_message_clear(&resp);

    fixture_down(&f);
}

static void test_close_unblocks_reader(void) {
    /* Bring up + handshake; the client.c reader thread is now parked
     * inside read_fn → queue_pop. Closing the client must unblock it
     * cleanly, including the SSE thread. */
    fixture_t f;
    TEST_ASSERT(fixture_up(&f) == 0);

    /* Tear down. If the close path failed to wake the reader thread
     * or the SSE thread, this hangs — `make test` timeouts will
     * surface it. */
    fixture_down(&f);
    TEST_ASSERT(1);
}

/* ====================================================================== */

int main(void) {
    fprintf(stderr, "test_http_client:\n");

    TEST_RUN(test_handshake_and_call);
    TEST_RUN(test_async_multi_inflight);
    TEST_RUN(test_session_id_propagates);
    TEST_RUN(test_close_unblocks_reader);

    TEST_DONE();
}
