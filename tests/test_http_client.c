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
#include <poll.h>
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
    free(a);
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
                      cmcp_handler_ctx_t *hctx,
                      cmcp_json_t **out_content, int *out_is_error) {
    (void)userdata; (void)out_is_error; (void)hctx;
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
    /* For the HTTP server transport, wake_fn is the signal that breaks
     * the server thread out of its slot_mu cond_wait — closing the
     * client transport doesn't propagate to the server like stdio does.
     * Join BEFORE close so close_fn doesn't destroy the mutex while
     * the server thread is still unwinding through cond_wait. */
    if (f->server_t) cmcp_transport_wake(f->server_t);
    pthread_join(f->server_th, NULL);
    if (f->server_t) cmcp_transport_close(f->server_t);
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
/* 503 surfacing — a mock server that always replies 503 to POSTs and    */
/* sinks the GET, used to prove cmcp_client_handshake doesn't hang on an */
/* HTTP error status. Regression for the 6.6.1-discovered defect.        */
/* ====================================================================== */

typedef struct {
    unsigned short  port;
    int             listen_fd;
    pthread_mutex_t mu;
    int             stop;
} mock503_t;

static int mock503_send_all(int fd, const char *buf, size_t len) {
    size_t sent = 0;
    while (sent < len) {
        ssize_t n = send(fd, buf + sent, len - sent, 0);
        if (n <= 0) return -1;
        sent += (size_t)n;
    }
    return 0;
}

static void mock503_handle(int fd) {
    /* Peek at the first byte to distinguish GET (SSE long-poll) from
     * POST. The SSE GET goes off on its own thread inside the client
     * transport; we answer it with a sink response so the SSE loop
     * doesn't hot-spin. POSTs all get 503 with Retry-After. */
    char first[8] = {0};
    ssize_t n = recv(fd, first, sizeof first - 1, MSG_PEEK);
    if (n <= 0) return;

    /* Drain the request headers (up to a CRLF CRLF) so the peer doesn't
     * see a half-closed connection before we reply. We don't need the
     * body — 503 ignores it. */
    char buf[2048];
    size_t got = 0;
    while (got < sizeof buf - 1) {
        ssize_t r = recv(fd, buf + got, sizeof buf - 1 - got, 0);
        if (r <= 0) break;
        got += (size_t)r;
        buf[got] = '\0';
        if (strstr(buf, "\r\n\r\n")) break;
    }

    if (strncmp(first, "GET", 3) == 0) {
        const char *sse =
            "HTTP/1.1 200 OK\r\n"
            "Content-Type: text/event-stream\r\n"
            "Connection: close\r\n\r\n";
        mock503_send_all(fd, sse, strlen(sse));
        return;
    }
    const char *resp =
        "HTTP/1.1 503 Service Unavailable\r\n"
        "Retry-After: 1\r\n"
        "Content-Length: 0\r\n"
        "Connection: close\r\n\r\n";
    mock503_send_all(fd, resp, strlen(resp));
}

static void *mock503_main(void *arg) {
    mock503_t *m = (mock503_t *)arg;
    for (;;) {
        pthread_mutex_lock(&m->mu);
        int stop = m->stop;
        pthread_mutex_unlock(&m->mu);
        if (stop) return NULL;

        struct pollfd p = { m->listen_fd, POLLIN, 0 };
        if (poll(&p, 1, 100) <= 0) continue;
        int cfd = accept(m->listen_fd, NULL, NULL);
        if (cfd < 0) continue;
        mock503_handle(cfd);
        close(cfd);
    }
}

static int mock503_listen(unsigned short port) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    int yes = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof yes);
    struct sockaddr_in sa;
    memset(&sa, 0, sizeof sa);
    sa.sin_family      = AF_INET;
    sa.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    sa.sin_port        = htons(port);
    if (bind(fd, (struct sockaddr *)&sa, sizeof sa) != 0) {
        close(fd); return -1;
    }
    if (listen(fd, 16) != 0) { close(fd); return -1; }
    return fd;
}

/* Before the fix, do_post silently dropped non-200 responses and
 * returned CMCP_OK — the body never reached the queue, and the host's
 * pending request hung waiting for a JSON-RPC frame the server never
 * sent. This test proves the write path now returns CMCP_EAGAIN on
 * 503 so cmcp_client_handshake fails fast instead of hanging. */
static void test_post_503_surfaces_eagain(void) {
    mock503_t m;
    memset(&m, 0, sizeof m);
    pthread_mutex_init(&m.mu, NULL);
    m.port = pick_port();
    TEST_ASSERT(m.port != 0);
    m.listen_fd = mock503_listen(m.port);
    TEST_ASSERT(m.listen_fd >= 0);

    pthread_t th;
    TEST_ASSERT(pthread_create(&th, NULL, mock503_main, &m) == 0);

    char *url = make_url(m.port);
    cmcp_transport_t *ct = cmcp_transport_http_connect(url);
    free(url);
    TEST_ASSERT(ct != NULL);

    cmcp_client_t *cli = cmcp_client_new("503-cli", "0.0.1");
    int rc = cmcp_client_handshake(cli, ct);
    /* The initialize POST gets 503. Pre-fix this hung forever; post-fix
     * the write surfaces CMCP_EAGAIN, which the handshake propagates. */
    TEST_ASSERT(rc != CMCP_OK);
    TEST_ASSERT(rc == CMCP_EAGAIN);

    cmcp_client_free(cli);
    cmcp_transport_close(ct);
    pthread_mutex_lock(&m.mu);
    m.stop = 1;
    pthread_mutex_unlock(&m.mu);
    pthread_join(th, NULL);
    close(m.listen_fd);
    pthread_mutex_destroy(&m.mu);
}

/* ====================================================================== */
/* Protocol-version header — a raw-socket sniffer that records whether    */
/* the cMCP client emits MCP-Protocol-Version on the wire.                */
/* ====================================================================== */

typedef struct {
    unsigned short  port;
    int             listen_fd;
    pthread_mutex_t mu;
    int             stop;          /* teardown signal for the accept loop */
    int             init_had_pv;   /* header on the initialize POST? (expect 0) */
    int             req_seen;      /* a post-handshake request was processed */
    int             req_had_pv;    /* header on that request? (expect 1) */
} sniffer_t;

static int send_all(int fd, const char *buf, size_t len) {
    size_t sent = 0;
    while (sent < len) {
        ssize_t n = send(fd, buf + sent, len - sent, 0);
        if (n <= 0) return -1;
        sent += (size_t)n;
    }
    return 0;
}

/* Read one HTTP request: head (through \r\n\r\n) plus a Content-Length
 * body. Both buffers are malloc'd and NUL-terminated. */
static int read_http_req(int fd, char **out_head, char **out_body,
                          size_t *out_body_len) {
    size_t cap = 4096, len = 0;
    char *buf = (char *)malloc(cap);
    if (!buf) return -1;
    char *hdr_end = NULL;
    while (!hdr_end) {
        if (len + 1 >= cap) {
            cap *= 2;
            char *nb = (char *)realloc(buf, cap);
            if (!nb) { free(buf); return -1; }
            buf = nb;
        }
        ssize_t n = recv(fd, buf + len, cap - 1 - len, 0);
        if (n <= 0) { free(buf); return -1; }
        len += (size_t)n;
        buf[len] = '\0';
        hdr_end = strstr(buf, "\r\n\r\n");
    }
    size_t head_len = (size_t)(hdr_end - buf) + 4;
    size_t body_len = 0;
    const char *cl = strstr(buf, "\r\nContent-Length:");
    if (cl) body_len = (size_t)strtoul(cl + 17, NULL, 10);
    while (len - head_len < body_len) {
        if (len + 1 >= cap) {
            cap *= 2;
            char *nb = (char *)realloc(buf, cap);
            if (!nb) { free(buf); return -1; }
            buf = nb;
        }
        ssize_t n = recv(fd, buf + len, cap - 1 - len, 0);
        if (n <= 0) { free(buf); return -1; }
        len += (size_t)n;
    }
    char *head = (char *)malloc(head_len + 1);
    char *body = (char *)malloc(body_len + 1);
    if (!head || !body) { free(head); free(body); free(buf); return -1; }
    memcpy(head, buf, head_len);     head[head_len] = '\0';
    memcpy(body, buf + head_len, body_len); body[body_len] = '\0';
    free(buf);
    *out_head = head; *out_body = body; *out_body_len = body_len;
    return 0;
}

/* Send an HTTP 200 wrapping a JSON-RPC response for `id`. */
static void send_rpc_response(int fd, const cmcp_id_t *id,
                               cmcp_json_t *result, const char *extra) {
    cmcp_rpc_message_t resp;
    cmcp_rpc_message_init(&resp);
    cmcp_rpc_make_response(&resp, id, result);
    char *json = cmcp_rpc_emit(&resp);
    cmcp_rpc_message_clear(&resp);
    if (!json) return;
    char hdr[256];
    int hn = snprintf(hdr, sizeof hdr,
        "HTTP/1.1 200 OK\r\n"
        "Content-Type: application/json\r\n"
        "%s"
        "Content-Length: %zu\r\n"
        "Connection: close\r\n\r\n",
        extra ? extra : "", strlen(json));
    if (hn > 0 && (size_t)hn < sizeof hdr) {
        send_all(fd, hdr, (size_t)hn);
        send_all(fd, json, strlen(json));
    }
    free(json);
}

static void handle_sniff_conn(sniffer_t *sn, int fd) {
    char *head = NULL, *body = NULL; size_t blen = 0;
    if (read_http_req(fd, &head, &body, &blen) != 0) return;

    if (strncmp(head, "GET ", 4) == 0) {
        /* The client's SSE GET — answer the upgrade and close. */
        const char *sse = "HTTP/1.1 200 OK\r\n"
                          "Content-Type: text/event-stream\r\n"
                          "Connection: close\r\n\r\n";
        send_all(fd, sse, strlen(sse));
        free(head); free(body);
        return;
    }

    int has_pv =
        strstr(head, "MCP-Protocol-Version: " CMCP_PROTOCOL_VERSION) != NULL;

    cmcp_rpc_message_t *msgs = NULL; size_t n = 0;
    if (cmcp_rpc_parse(body, blen, &msgs, &n) != CMCP_OK || n != 1) {
        cmcp_rpc_messages_free(msgs, n);
        const char *e = "HTTP/1.1 400 Bad Request\r\n"
                        "Content-Length: 0\r\nConnection: close\r\n\r\n";
        send_all(fd, e, strlen(e));
        free(head); free(body);
        return;
    }
    cmcp_rpc_message_t *m = &msgs[0];

    if (m->kind == CMCP_MSG_NOTIFICATION) {
        const char *acc = "HTTP/1.1 202 Accepted\r\n"
                          "Content-Length: 0\r\nConnection: close\r\n\r\n";
        send_all(fd, acc, strlen(acc));
    } else if (m->method && strcmp(m->method, "initialize") == 0) {
        pthread_mutex_lock(&sn->mu);
        sn->init_had_pv = has_pv;
        pthread_mutex_unlock(&sn->mu);
        cmcp_json_t *result = cmcp_json_new_object();
        cmcp_json_object_set(result, "protocolVersion",
                              cmcp_json_new_string(CMCP_PROTOCOL_VERSION));
        cmcp_json_object_set(result, "capabilities", cmcp_json_new_object());
        cmcp_json_t *si = cmcp_json_new_object();
        cmcp_json_object_set(si, "name", cmcp_json_new_string("sniff-srv"));
        cmcp_json_object_set(si, "version", cmcp_json_new_string("0.1.0"));
        cmcp_json_object_set(result, "serverInfo", si);
        send_rpc_response(fd, &m->id, result, "Mcp-Session-Id: sniff-1\r\n");
    } else {
        /* A post-handshake request — the one we want to inspect. */
        pthread_mutex_lock(&sn->mu);
        sn->req_had_pv = has_pv;
        sn->req_seen   = 1;
        pthread_mutex_unlock(&sn->mu);
        send_rpc_response(fd, &m->id, cmcp_json_new_object(), NULL);
    }
    cmcp_rpc_messages_free(msgs, n);
    free(head); free(body);
}

/* poll-with-timeout accept loop: close() does not reliably unblock a
 * thread parked in accept(), so the loop polls and checks a stop flag
 * (the same pattern the real HTTP server's acceptor uses). */
static void *sniffer_main(void *arg) {
    sniffer_t *sn = (sniffer_t *)arg;
    for (;;) {
        pthread_mutex_lock(&sn->mu);
        int stop = sn->stop;
        pthread_mutex_unlock(&sn->mu);
        if (stop) return NULL;

        struct pollfd p = { sn->listen_fd, POLLIN, 0 };
        if (poll(&p, 1, 100) <= 0) continue;
        int cfd = accept(sn->listen_fd, NULL, NULL);
        if (cfd < 0) continue;
        handle_sniff_conn(sn, cfd);
        close(cfd);
    }
}

static int sniffer_listen(unsigned short port) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    int yes = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof yes);
    struct sockaddr_in sa;
    memset(&sa, 0, sizeof sa);
    sa.sin_family      = AF_INET;
    sa.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    sa.sin_port        = htons(port);
    if (bind(fd, (struct sockaddr *)&sa, sizeof sa) != 0) {
        close(fd); return -1;
    }
    if (listen(fd, 16) != 0) { close(fd); return -1; }
    return fd;
}

static void test_client_sends_protocol_version(void) {
    sniffer_t sn;
    memset(&sn, 0, sizeof sn);
    pthread_mutex_init(&sn.mu, NULL);
    sn.port = pick_port();
    TEST_ASSERT(sn.port != 0);
    sn.listen_fd = sniffer_listen(sn.port);
    TEST_ASSERT(sn.listen_fd >= 0);

    pthread_t th;
    TEST_ASSERT(pthread_create(&th, NULL, sniffer_main, &sn) == 0);

    char *url = make_url(sn.port);
    cmcp_transport_t *ct = cmcp_transport_http_connect(url);
    free(url);
    TEST_ASSERT(ct != NULL);

    cmcp_client_t *cli = cmcp_client_new("pv-cli", "0.0.1");
    TEST_ASSERT(cmcp_client_handshake(cli, ct) == CMCP_OK);

    /* One post-handshake request — its POST must carry the header. */
    cmcp_rpc_message_t resp;
    cmcp_rpc_message_init(&resp);
    TEST_ASSERT(cmcp_client_request(cli, "ping", NULL, &resp) == CMCP_OK);
    cmcp_rpc_message_clear(&resp);

    pthread_mutex_lock(&sn.mu);
    int init_pv  = sn.init_had_pv;
    int req_seen = sn.req_seen;
    int req_pv   = sn.req_had_pv;
    pthread_mutex_unlock(&sn.mu);

    TEST_ASSERT(init_pv  == 0);   /* not sent before the version is known */
    TEST_ASSERT(req_seen == 1);
    TEST_ASSERT(req_pv   == 1);   /* sent on every post-handshake request */

    cmcp_client_free(cli);
    cmcp_transport_close(ct);
    pthread_mutex_lock(&sn.mu);
    sn.stop = 1;                  /* the poll loop exits within ~100ms */
    pthread_mutex_unlock(&sn.mu);
    pthread_join(th, NULL);
    close(sn.listen_fd);
    pthread_mutex_destroy(&sn.mu);
}

/* ====================================================================== */
/* P7 second-probe leg: the typed tool-call API (by-value result + opaque  */
/* handle, v0.9.0) over the HTTP client transport. The original host-probe */
/* was stdio-only; the libcurl client + background SSE reader thread was    */
/* never exercised by the new API. This is the falsification pass — does    */
/* the struct/handle surface compose with the HTTP demux path?              */
/* ====================================================================== */

/* content[0].text out of a by-value tool result (OK branch). */
static char *result_text_http(const cmcp_tool_result_t *res) {
    if (res->outcome != CMCP_TOOL_OK || !res->result) return NULL;
    const cmcp_json_t *arr = cmcp_json_object_get(res->result, "content");
    if (!arr || arr->type != CMCP_JSON_ARRAY || arr->arr.len == 0) return NULL;
    const cmcp_json_t *item = arr->arr.items[0];
    if (!item || item->type != CMCP_JSON_OBJECT) return NULL;
    const cmcp_json_t *t = cmcp_json_object_get(item, "text");
    if (!t || t->type != CMCP_JSON_STRING) return NULL;
    return strdup(t->str.s);
}

static cmcp_json_t *echo_args(const char *msg) {
    cmcp_json_t *a = cmcp_json_new_object();
    cmcp_json_object_set(a, "message", cmcp_json_new_string(msg));
    return a;
}

static void test_typed_tool_api_over_http(void) {
    fixture_t f;
    TEST_ASSERT(fixture_up(&f) == 0);

    /* (1) Sync by-value call over HTTP. */
    cmcp_tool_result_t r = cmcp_client_tool_call(f.cli, "echo",
                                                 echo_args("sync-http"));
    TEST_ASSERT(r.outcome == CMCP_TOOL_OK);
    char *t = result_text_http(&r);
    TEST_ASSERT(t && strcmp(t, "sync-http") == 0);
    free(t);
    cmcp_tool_result_clear(&r);

    /* (2) Async fan of two, reaped in reverse — exercises the handle +
     * the SSE/libcurl reader-thread demux under the new API. */
    cmcp_tool_handle_t h1 = cmcp_client_tool_call_async(f.cli, "echo",
                                                        echo_args("first"));
    cmcp_tool_handle_t h2 = cmcp_client_tool_call_async(f.cli, "echo",
                                                        echo_args("second"));
    TEST_ASSERT(cmcp_tool_handle_valid(h1) && cmcp_tool_handle_valid(h2));
    TEST_ASSERT(h1.client == f.cli && h2.client == f.cli);

    cmcp_tool_result_t r2 = cmcp_client_tool_wait(h2);
    cmcp_tool_result_t r1 = cmcp_client_tool_wait(h1);
    char *t2 = result_text_http(&r2);
    char *t1 = result_text_http(&r1);
    TEST_ASSERT(t2 && strcmp(t2, "second") == 0);
    TEST_ASSERT(t1 && strcmp(t1, "first")  == 0);
    free(t1); free(t2);
    cmcp_tool_result_clear(&r1);
    cmcp_tool_result_clear(&r2);

    /* (3) Protocol error path over HTTP: unknown tool → -32602 on the
     * error channel (not a tool-level isError). */
    cmcp_tool_result_t re = cmcp_client_tool_call(f.cli, "ghost", NULL);
    TEST_ASSERT(re.outcome == CMCP_TOOL_ERR_PROTOCOL);
    TEST_ASSERT(re.error != NULL);
    TEST_ASSERT(re.error->code == CMCP_RPC_INVALID_PARAMS);
    cmcp_tool_result_clear(&re);

    fixture_down(&f);
}

/* ====================================================================== */

int main(void) {
    fprintf(stderr, "test_http_client:\n");

    TEST_RUN(test_handshake_and_call);
    TEST_RUN(test_async_multi_inflight);
    TEST_RUN(test_session_id_propagates);
    TEST_RUN(test_close_unblocks_reader);
    TEST_RUN(test_post_503_surfaces_eagain);
    TEST_RUN(test_client_sends_protocol_version);
    TEST_RUN(test_typed_tool_api_over_http);

    TEST_DONE();
}
