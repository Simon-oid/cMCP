/* End-to-end tests for the Streamable HTTP transport (server side).
 *
 * Each test starts a real cmcp_server_t on top of the HTTP transport
 * (bound to 127.0.0.1 on an ephemeral port the kernel picks), drives
 * it with raw HTTP/1.1 over `socket()`, and asserts on the wire
 * response. No libcurl in the tests — keeping the wire honest. */

/* getaddrinfo, struct addrinfo, memmem (GNU extension). */
#define _POSIX_C_SOURCE 200809L
#define _GNU_SOURCE

#include "test.h"
#include "cmcp.h"
#include "cmcp_json.h"
#include "cmcp_server.h"
#include "cmcp_transport.h"
#include "cmcp_types.h"

#include <poll.h>
#include <signal.h>

#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/in.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

/* ====================================================================== */
/* Server harness                                                          */
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

/* Bind a listening socket on an ephemeral port to discover an unused
 * one, then close it and use that port for the actual transport. The
 * tiny race window is acceptable for a test. */
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

/* Open a TCP connection to 127.0.0.1:port. */
static int open_client(unsigned short port) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    struct sockaddr_in sa;
    memset(&sa, 0, sizeof sa);
    sa.sin_family      = AF_INET;
    sa.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    sa.sin_port        = htons(port);
    /* Retry briefly while the server's listen() races with our connect(). */
    for (int i = 0; i < 20; i++) {
        if (connect(fd, (struct sockaddr *)&sa, sizeof sa) == 0) return fd;
        struct timespec ts = { 0, 5 * 1000 * 1000 };  /* 5ms */
        nanosleep(&ts, NULL);
    }
    close(fd);
    return -1;
}

static int send_all(int fd, const char *buf, size_t len) {
    size_t sent = 0;
    while (sent < len) {
        ssize_t n = send(fd, buf + sent, len - sent, 0);
        if (n <= 0) return -1;
        sent += (size_t)n;
    }
    return 0;
}

/* Read until the peer closes (Connection: close), into a malloc'd buffer. */
static int recv_all(int fd, char **out_buf, size_t *out_len) {
    size_t cap = 4096, len = 0;
    char *buf = (char *)malloc(cap);
    if (!buf) return -1;
    for (;;) {
        if (len + 1 >= cap) {
            cap *= 2;
            char *nb = (char *)realloc(buf, cap);
            if (!nb) { free(buf); return -1; }
            buf = nb;
        }
        ssize_t n = recv(fd, buf + len, cap - 1 - len, 0);
        if (n < 0) { free(buf); return -1; }
        if (n == 0) break;
        len += (size_t)n;
    }
    buf[len] = '\0';
    *out_buf = buf;
    *out_len = len;
    return 0;
}

/* Find a header line in the response head; copies the trimmed value.
 * Returns 1 on hit, 0 on miss. Case-insensitive on name. */
static int extract_header(const char *resp, const char *name,
                            char *out, size_t out_cap) {
    const char *eoh = strstr(resp, "\r\n\r\n");
    if (!eoh) eoh = resp + strlen(resp);
    size_t name_len = strlen(name);
    const char *p = resp;
    while (p < eoh) {
        const char *eol = strstr(p, "\r\n");
        if (!eol || eol > eoh) break;
        if ((size_t)(eol - p) > name_len + 1 &&
            strncasecmp(p, name, name_len) == 0 && p[name_len] == ':') {
            const char *v = p + name_len + 1;
            while (v < eol && (*v == ' ' || *v == '\t')) v++;
            size_t vlen = (size_t)(eol - v);
            if (vlen >= out_cap) vlen = out_cap - 1;
            memcpy(out, v, vlen);
            out[vlen] = '\0';
            return 1;
        }
        p = eol + 2;
    }
    return 0;
}

static const char *find_body(const char *resp) {
    const char *eoh = strstr(resp, "\r\n\r\n");
    return eoh ? eoh + 4 : resp + strlen(resp);
}

/* Build "POST /mcp HTTP/1.1\r\nHost: x\r\n..." with the given body. */
static char *build_post(const char *body, const char *session_id) {
    size_t body_len = body ? strlen(body) : 0;
    size_t cap = body_len + 512;
    char *out = (char *)malloc(cap);
    if (!out) return NULL;
    int n = snprintf(out, cap,
        "POST /mcp HTTP/1.1\r\n"
        "Host: 127.0.0.1\r\n"
        "Content-Type: application/json\r\n"
        "%s%s%s"
        "Content-Length: %zu\r\n"
        "\r\n"
        "%s",
        session_id ? "Mcp-Session-Id: " : "",
        session_id ? session_id : "",
        session_id ? "\r\n" : "",
        body_len,
        body ? body : "");
    if (n < 0 || (size_t)n >= cap) { free(out); return NULL; }
    return out;
}

/* Like build_post, but also emits an MCP-Protocol-Version header when
 * `pv` is non-NULL. */
static char *build_post_pv(const char *body, const char *session_id,
                            const char *pv) {
    size_t body_len = body ? strlen(body) : 0;
    size_t cap = body_len + 512;
    char *out = (char *)malloc(cap);
    if (!out) return NULL;
    int n = snprintf(out, cap,
        "POST /mcp HTTP/1.1\r\n"
        "Host: 127.0.0.1\r\n"
        "Content-Type: application/json\r\n"
        "%s%s%s"
        "%s%s%s"
        "Content-Length: %zu\r\n"
        "\r\n"
        "%s",
        session_id ? "Mcp-Session-Id: " : "",
        session_id ? session_id : "",
        session_id ? "\r\n" : "",
        pv ? "MCP-Protocol-Version: " : "",
        pv ? pv : "",
        pv ? "\r\n" : "",
        body_len,
        body ? body : "");
    if (n < 0 || (size_t)n >= cap) { free(out); return NULL; }
    return out;
}

/* Build a GET /mcp request for SSE. */
static char *build_get_sse(const char *session_id) {
    size_t cap = 512;
    char *out = (char *)malloc(cap);
    if (!out) return NULL;
    int n = snprintf(out, cap,
        "GET /mcp HTTP/1.1\r\n"
        "Host: 127.0.0.1\r\n"
        "Accept: text/event-stream\r\n"
        "%s%s%s"
        "\r\n",
        session_id ? "Mcp-Session-Id: " : "",
        session_id ? session_id : "",
        session_id ? "\r\n" : "");
    if (n < 0 || (size_t)n >= cap) { free(out); return NULL; }
    return out;
}

/* Send `req`, return whole response. Caller frees. */
static int do_request(unsigned short port, const char *req,
                       char **out_resp, size_t *out_len) {
    int fd = open_client(port);
    if (fd < 0) return -1;
    if (send_all(fd, req, strlen(req)) != 0) { close(fd); return -1; }
    int rc = recv_all(fd, out_resp, out_len);
    close(fd);
    return rc;
}

/* Standard initialize body, accepts the cMCP protocol version. */
static const char *INIT_BODY =
    "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"initialize\",\"params\":{"
        "\"protocolVersion\":\"" CMCP_PROTOCOL_VERSION "\","
        "\"capabilities\":{},"
        "\"clientInfo\":{\"name\":\"test\",\"version\":\"0\"}"
    "}}";

/* ====================================================================== */
/* Tests                                                                   */
/* ====================================================================== */

static void test_initialize_round_trip(void) {
    unsigned short port = pick_port();
    TEST_ASSERT(port != 0);

    cmcp_transport_t *t = cmcp_transport_http_listen("127.0.0.1", port);
    TEST_ASSERT(t != NULL);

    cmcp_server_t *s = cmcp_server_new("http-srv", "0.1.0");
    server_arg_t sa = { s, t, 0 };
    pthread_t th;
    TEST_ASSERT(pthread_create(&th, NULL, server_thread_main, &sa) == 0);

    char *req = build_post(INIT_BODY, NULL);
    char *resp = NULL; size_t resp_len = 0;
    TEST_ASSERT(do_request(port, req, &resp, &resp_len) == 0);
    free(req);

    /* Status line. */
    TEST_ASSERT(strncmp(resp, "HTTP/1.1 200 OK\r\n", 17) == 0);

    /* Mcp-Session-Id header should have been minted. */
    char sid[64] = {0};
    TEST_ASSERT(extract_header(resp, "Mcp-Session-Id", sid, sizeof sid) == 1);
    TEST_ASSERT(strlen(sid) >= 32);   /* UUID-shaped */

    /* Body parses as a JSON-RPC success response. */
    const char *body = find_body(resp);
    cmcp_json_t *j = cmcp_json_parse(body, strlen(body));
    TEST_ASSERT(j != NULL);
    TEST_ASSERT(j && j->type == CMCP_JSON_OBJECT);
    if (j) {
        const cmcp_json_t *result = cmcp_json_object_get(j, "result");
        TEST_ASSERT(result != NULL);
        TEST_ASSERT(result && result->type == CMCP_JSON_OBJECT);
        const cmcp_json_t *pv =
            result ? cmcp_json_object_get(result, "protocolVersion") : NULL;
        TEST_ASSERT(pv && pv->type == CMCP_JSON_STRING);
        TEST_ASSERT(pv && strcmp(pv->str.s, CMCP_PROTOCOL_VERSION) == 0);
    }
    cmcp_json_free(j);
    free(resp);

    cmcp_transport_wake(t);
    pthread_join(th, NULL);
    cmcp_transport_close(t);
    cmcp_server_free(s);
}

static void test_session_id_required_after_init(void) {
    unsigned short port = pick_port();
    TEST_ASSERT(port != 0);

    cmcp_transport_t *t = cmcp_transport_http_listen("127.0.0.1", port);
    cmcp_server_t *s = cmcp_server_new("http-srv", "0.1.0");
    server_arg_t sa = { s, t, 0 };
    pthread_t th;
    pthread_create(&th, NULL, server_thread_main, &sa);

    /* Step 1: initialize, harvest session id. */
    char *req = build_post(INIT_BODY, NULL);
    char *resp = NULL; size_t rlen = 0;
    do_request(port, req, &resp, &rlen);
    free(req);
    char sid[64] = {0};
    TEST_ASSERT(extract_header(resp, "Mcp-Session-Id", sid, sizeof sid) == 1);
    free(resp);

    /* Step 1b: notifications/initialized — required to advance state. */
    const char *initialized_body =
        "{\"jsonrpc\":\"2.0\",\"method\":\"notifications/initialized\"}";
    req = build_post(initialized_body, sid);
    do_request(port, req, &resp, &rlen);
    free(req);
    TEST_ASSERT(strncmp(resp, "HTTP/1.1 202", 12) == 0);
    free(resp);

    /* Step 2: tools/list WITHOUT session id → 400. */
    const char *list_body =
        "{\"jsonrpc\":\"2.0\",\"id\":2,\"method\":\"tools/list\"}";
    req = build_post(list_body, NULL);
    do_request(port, req, &resp, &rlen);
    free(req);
    TEST_ASSERT(strncmp(resp, "HTTP/1.1 400", 12) == 0);
    free(resp);

    /* Step 3: tools/list with WRONG session id → 404. */
    req = build_post(list_body, "deadbeef-bad");
    do_request(port, req, &resp, &rlen);
    free(req);
    TEST_ASSERT(strncmp(resp, "HTTP/1.1 404", 12) == 0);
    free(resp);

    /* Step 4: tools/list WITH the right session id → 200. */
    req = build_post(list_body, sid);
    do_request(port, req, &resp, &rlen);
    free(req);
    TEST_ASSERT(strncmp(resp, "HTTP/1.1 200 OK\r\n", 17) == 0);
    /* Body is a JSON-RPC success with a tools array. */
    const char *body = find_body(resp);
    cmcp_json_t *j = cmcp_json_parse(body, strlen(body));
    TEST_ASSERT(j && j->type == CMCP_JSON_OBJECT);
    if (j) {
        const cmcp_json_t *result = cmcp_json_object_get(j, "result");
        TEST_ASSERT(result != NULL);
        const cmcp_json_t *tools =
            result ? cmcp_json_object_get(result, "tools") : NULL;
        TEST_ASSERT(tools && tools->type == CMCP_JSON_ARRAY);
    }
    cmcp_json_free(j);
    free(resp);

    cmcp_transport_wake(t);
    pthread_join(th, NULL);
    cmcp_transport_close(t);
    cmcp_server_free(s);
}

static void test_malformed_request(void) {
    unsigned short port = pick_port();
    TEST_ASSERT(port != 0);

    cmcp_transport_t *t = cmcp_transport_http_listen("127.0.0.1", port);
    cmcp_server_t *s = cmcp_server_new("http-srv", "0.1.0");
    server_arg_t sa = { s, t, 0 };
    pthread_t th;
    pthread_create(&th, NULL, server_thread_main, &sa);

    /* Garbage on the wire — no valid request line. */
    int fd = open_client(port);
    TEST_ASSERT(fd >= 0);
    const char *junk = "not a real request\r\n\r\n";
    send_all(fd, junk, strlen(junk));
    char *resp = NULL; size_t rlen = 0;
    recv_all(fd, &resp, &rlen);
    close(fd);
    TEST_ASSERT(strncmp(resp, "HTTP/1.1 400", 12) == 0);
    free(resp);

    /* Wrong method — PUT /mcp → 404. */
    int fd2 = open_client(port);
    TEST_ASSERT(fd2 >= 0);
    const char *put =
        "PUT /mcp HTTP/1.1\r\nHost: 127.0.0.1\r\nContent-Length: 0\r\n\r\n";
    send_all(fd2, put, strlen(put));
    recv_all(fd2, &resp, &rlen);
    close(fd2);
    TEST_ASSERT(strncmp(resp, "HTTP/1.1 404", 12) == 0);
    free(resp);

    /* Right method, wrong path → 404. */
    int fd3 = open_client(port);
    TEST_ASSERT(fd3 >= 0);
    const char *bad_path =
        "POST /elsewhere HTTP/1.1\r\nHost: 127.0.0.1\r\nContent-Length: 2\r\n\r\n{}";
    send_all(fd3, bad_path, strlen(bad_path));
    recv_all(fd3, &resp, &rlen);
    close(fd3);
    TEST_ASSERT(strncmp(resp, "HTTP/1.1 404", 12) == 0);
    free(resp);

    cmcp_transport_wake(t);
    pthread_join(th, NULL);
    cmcp_transport_close(t);
    cmcp_server_free(s);
}

static void test_get_sse_handshake(void) {
    unsigned short port = pick_port();
    TEST_ASSERT(port != 0);

    cmcp_transport_t *t = cmcp_transport_http_listen("127.0.0.1", port);
    cmcp_server_t *s = cmcp_server_new("http-srv", "0.1.0");
    server_arg_t sa = { s, t, 0 };
    pthread_t th;
    pthread_create(&th, NULL, server_thread_main, &sa);

    /* Initialize first to mint a session id. */
    char *req = build_post(INIT_BODY, NULL);
    char *resp = NULL; size_t rlen = 0;
    do_request(port, req, &resp, &rlen);
    free(req);
    char sid[64] = {0};
    extract_header(resp, "Mcp-Session-Id", sid, sizeof sid);
    free(resp);

    /* GET /mcp without session → 404. */
    char *getreq = build_get_sse(NULL);
    int fd = open_client(port);
    send_all(fd, getreq, strlen(getreq));
    free(getreq);
    char buf[256] = {0};
    ssize_t n = recv(fd, buf, sizeof buf - 1, 0);
    TEST_ASSERT(n > 0);
    TEST_ASSERT(strncmp(buf, "HTTP/1.1 404", 12) == 0);
    close(fd);

    /* GET /mcp with the right session → 200 + text/event-stream. */
    getreq = build_get_sse(sid);
    fd = open_client(port);
    send_all(fd, getreq, strlen(getreq));
    free(getreq);
    /* Read just enough to see the headers — the body never arrives
     * because the stream is held open. */
    size_t total = 0;
    while (total < 200) {
        ssize_t r = recv(fd, buf + total, sizeof buf - 1 - total, 0);
        if (r <= 0) break;
        total += (size_t)r;
        if (memmem(buf, total, "\r\n\r\n", 4)) break;
    }
    buf[total] = '\0';
    TEST_ASSERT(strncmp(buf, "HTTP/1.1 200 OK\r\n", 17) == 0);
    char ct[64] = {0};
    TEST_ASSERT(extract_header(buf, "Content-Type", ct, sizeof ct) == 1);
    TEST_ASSERT(strcasecmp(ct, "text/event-stream") == 0);
    char sid2[64] = {0};
    TEST_ASSERT(extract_header(buf, "Mcp-Session-Id", sid2, sizeof sid2) == 1);
    TEST_ASSERT(strcmp(sid2, sid) == 0);
    /* Close client side; the holder thread should notice and exit. */
    close(fd);

    cmcp_transport_wake(t);
    pthread_join(th, NULL);
    cmcp_transport_close(t);
    cmcp_server_free(s);
}

/* The MCP-Protocol-Version header (since 2025-06-18): a post-handshake POST
 * carrying a version cMCP does not speak is rejected with 400; the
 * matching version is accepted; an absent header falls back to the
 * spec default and is also accepted. */
static void test_protocol_version_header(void) {
    unsigned short port = pick_port();
    TEST_ASSERT(port != 0);

    cmcp_transport_t *t = cmcp_transport_http_listen("127.0.0.1", port);
    cmcp_server_t *s = cmcp_server_new("http-srv", "0.1.0");
    server_arg_t sa = { s, t, 0 };
    pthread_t th;
    pthread_create(&th, NULL, server_thread_main, &sa);

    /* Initialize → harvest session id, then notifications/initialized. */
    char *req = build_post(INIT_BODY, NULL);
    char *resp = NULL; size_t rlen = 0;
    do_request(port, req, &resp, &rlen);
    free(req);
    char sid[64] = {0};
    TEST_ASSERT(extract_header(resp, "Mcp-Session-Id", sid, sizeof sid) == 1);
    free(resp);

    const char *initialized_body =
        "{\"jsonrpc\":\"2.0\",\"method\":\"notifications/initialized\"}";
    req = build_post(initialized_body, sid);
    do_request(port, req, &resp, &rlen);
    free(req);
    free(resp);

    const char *list_body =
        "{\"jsonrpc\":\"2.0\",\"id\":2,\"method\":\"tools/list\"}";

    /* Matching MCP-Protocol-Version → 200. */
    req = build_post_pv(list_body, sid, CMCP_PROTOCOL_VERSION);
    do_request(port, req, &resp, &rlen);
    free(req);
    TEST_ASSERT(strncmp(resp, "HTTP/1.1 200 OK\r\n", 17) == 0);
    free(resp);

    /* Unsupported MCP-Protocol-Version → 400. */
    req = build_post_pv(list_body, sid, "1999-01-01");
    do_request(port, req, &resp, &rlen);
    free(req);
    TEST_ASSERT(strncmp(resp, "HTTP/1.1 400", 12) == 0);
    free(resp);

    /* Absent MCP-Protocol-Version → accepted (spec default fallback). */
    req = build_post(list_body, sid);
    do_request(port, req, &resp, &rlen);
    free(req);
    TEST_ASSERT(strncmp(resp, "HTTP/1.1 200 OK\r\n", 17) == 0);
    free(resp);

    cmcp_transport_wake(t);
    pthread_join(th, NULL);
    cmcp_transport_close(t);
    cmcp_server_free(s);
}

static void test_close_unblocks_reader(void) {
    /* If nothing ever connects, cmcp_server_run is parked in read_fn.
     * Closing the transport must wake it so the server thread can exit. */
    unsigned short port = pick_port();
    TEST_ASSERT(port != 0);

    cmcp_transport_t *t = cmcp_transport_http_listen("127.0.0.1", port);
    cmcp_server_t *s = cmcp_server_new("http-srv", "0.1.0");
    server_arg_t sa = { s, t, 0 };
    pthread_t th;
    TEST_ASSERT(pthread_create(&th, NULL, server_thread_main, &sa) == 0);

    /* Brief delay so the server reaches its first read. */
    struct timespec ts = { 0, 50 * 1000 * 1000 };
    nanosleep(&ts, NULL);

    cmcp_transport_wake(t);
    /* If wake failed to broadcast the slot cvs, this hangs. The test
     * harness's parent process timeouts will catch that. */
    TEST_ASSERT(pthread_join(th, NULL) == 0);
    TEST_ASSERT(sa.rc == CMCP_OK);     /* clean shutdown */

    cmcp_transport_close(t);
    cmcp_server_free(s);
}

/* Build a POST with an Origin header (and the standard headers). */
static char *build_post_origin(const char *body, const char *origin) {
    size_t body_len = body ? strlen(body) : 0;
    size_t cap = body_len + 512;
    char *out = (char *)malloc(cap);
    if (!out) return NULL;
    int n = snprintf(out, cap,
        "POST /mcp HTTP/1.1\r\n"
        "Host: 127.0.0.1\r\n"
        "Content-Type: application/json\r\n"
        "%s%s%s"
        "Content-Length: %zu\r\n"
        "\r\n"
        "%s",
        origin ? "Origin: " : "",
        origin ? origin : "",
        origin ? "\r\n" : "",
        body_len,
        body ? body : "");
    if (n < 0 || (size_t)n >= cap) { free(out); return NULL; }
    return out;
}

/* Origin allow-list (MCP 2025-11-25 Minor 3). CMCP_HTTP_ALLOWED_ORIGINS
 * is snapshot at listen time, so each sub-case spins up its own
 * transport with a different setenv before constructing the listener.
 * Three cases: matching origin → 200; mismatched origin → 403;
 * absent Origin header → 200 (non-browser clients don't emit it);
 * unset env var (no list) → 200 regardless of Origin (backward-compat). */
static void test_origin_allowlist(void) {
    /* (a) Allowed origin → 200 OK. */
    {
        setenv("CMCP_HTTP_ALLOWED_ORIGINS",
               "https://app.example.com, https://other.example.com", 1);
        unsigned short port = pick_port();
        TEST_ASSERT(port != 0);
        cmcp_transport_t *t = cmcp_transport_http_listen("127.0.0.1", port);
        cmcp_server_t *s = cmcp_server_new("http-srv", "0.1.0");
        server_arg_t sa = { s, t, 0 };
        pthread_t th;
        pthread_create(&th, NULL, server_thread_main, &sa);

        char *req = build_post_origin(INIT_BODY, "https://app.example.com");
        char *resp = NULL;
        size_t rn = 0;
        TEST_ASSERT(do_request(port, req, &resp, &rn) == 0);
        TEST_ASSERT(strstr(resp, "HTTP/1.1 200") != NULL);
        free(req); free(resp);

        cmcp_transport_wake(t);
        pthread_join(th, NULL);
        cmcp_transport_close(t);
        cmcp_server_free(s);
    }

    /* (b) Disallowed origin → 403 Forbidden. */
    {
        setenv("CMCP_HTTP_ALLOWED_ORIGINS", "https://app.example.com", 1);
        unsigned short port = pick_port();
        TEST_ASSERT(port != 0);
        cmcp_transport_t *t = cmcp_transport_http_listen("127.0.0.1", port);
        cmcp_server_t *s = cmcp_server_new("http-srv", "0.1.0");
        server_arg_t sa = { s, t, 0 };
        pthread_t th;
        pthread_create(&th, NULL, server_thread_main, &sa);

        char *req = build_post_origin(INIT_BODY, "https://evil.example.com");
        char *resp = NULL;
        size_t rn = 0;
        TEST_ASSERT(do_request(port, req, &resp, &rn) == 0);
        TEST_ASSERT(strstr(resp, "HTTP/1.1 403") != NULL);
        free(req); free(resp);

        cmcp_transport_wake(t);
        pthread_join(th, NULL);
        cmcp_transport_close(t);
        cmcp_server_free(s);
    }

    /* (c) Allow-list set, Origin header absent → 200 (curl/test path). */
    {
        setenv("CMCP_HTTP_ALLOWED_ORIGINS", "https://app.example.com", 1);
        unsigned short port = pick_port();
        TEST_ASSERT(port != 0);
        cmcp_transport_t *t = cmcp_transport_http_listen("127.0.0.1", port);
        cmcp_server_t *s = cmcp_server_new("http-srv", "0.1.0");
        server_arg_t sa = { s, t, 0 };
        pthread_t th;
        pthread_create(&th, NULL, server_thread_main, &sa);

        char *req = build_post(INIT_BODY, NULL);
        char *resp = NULL;
        size_t rn = 0;
        TEST_ASSERT(do_request(port, req, &resp, &rn) == 0);
        TEST_ASSERT(strstr(resp, "HTTP/1.1 200") != NULL);
        free(req); free(resp);

        cmcp_transport_wake(t);
        pthread_join(th, NULL);
        cmcp_transport_close(t);
        cmcp_server_free(s);
    }

    /* (d) Allow-list unset → arbitrary Origin accepted (backward-compat). */
    {
        unsetenv("CMCP_HTTP_ALLOWED_ORIGINS");
        unsigned short port = pick_port();
        TEST_ASSERT(port != 0);
        cmcp_transport_t *t = cmcp_transport_http_listen("127.0.0.1", port);
        cmcp_server_t *s = cmcp_server_new("http-srv", "0.1.0");
        server_arg_t sa = { s, t, 0 };
        pthread_t th;
        pthread_create(&th, NULL, server_thread_main, &sa);

        char *req = build_post_origin(INIT_BODY, "https://anything.test");
        char *resp = NULL;
        size_t rn = 0;
        TEST_ASSERT(do_request(port, req, &resp, &rn) == 0);
        TEST_ASSERT(strstr(resp, "HTTP/1.1 200") != NULL);
        free(req); free(resp);

        cmcp_transport_wake(t);
        pthread_join(th, NULL);
        cmcp_transport_close(t);
        cmcp_server_free(s);
    }
}

/* ============================================================
 * SSE event-id + Last-Event-Id resumption (MCP 2025-11-25 SEP-1699)
 * ============================================================ */

/* Tool that emits `notifications/tools/list_changed` whenever invoked.
 * The handler closes over the server pointer via userdata. Used to
 * force the server to record events in its SSE replay ring. */
static int trig_notif_handler(const cmcp_json_t *args, void *ud,
                                cmcp_handler_ctx_t *hctx,
                                cmcp_json_t **out_content, int *out_is_error) {
    (void)args; (void)hctx;
    cmcp_server_t *s = (cmcp_server_t *)ud;
    cmcp_server_notify_tools_changed(s);
    *out_content  = cmcp_tool_text_content("ok");
    *out_is_error = 0;
    return CMCP_OK;
}

/* Read up to `cap-1` bytes from `fd` within `timeout_ms`, then close.
 * Returns the length read (NUL-terminates the buffer). */
static size_t sse_read_window(int fd, char *buf, size_t cap, int timeout_ms) {
    size_t total = 0;
    int    remaining = timeout_ms;
    while (total + 1 < cap && remaining > 0) {
        struct pollfd p = { fd, POLLIN, 0 };
        struct timespec start;
        clock_gettime(CLOCK_MONOTONIC, &start);
        int pr = poll(&p, 1, remaining);
        if (pr <= 0) break;
        ssize_t n = recv(fd, buf + total, cap - 1 - total, 0);
        if (n <= 0) break;
        total += (size_t)n;
        struct timespec end;
        clock_gettime(CLOCK_MONOTONIC, &end);
        int elapsed = (int)((end.tv_sec  - start.tv_sec)  * 1000 +
                            (end.tv_nsec - start.tv_nsec) / 1000000L);
        remaining -= (elapsed > 0 ? elapsed : 1);
    }
    buf[total] = '\0';
    return total;
}

/* Build a GET /mcp request that resumes via Last-Event-Id when
 * include_leid is non-zero. Passing include_leid=1, leid=0 produces a
 * header `Last-Event-Id: 0`, distinct from "no header" which the
 * server treats as "no resume". */
static char *build_get_sse_with_leid(const char *session_id,
                                       int include_leid, uint64_t leid) {
    size_t cap = 512;
    char *out = (char *)malloc(cap);
    if (!out) return NULL;
    int n;
    if (include_leid) {
        n = snprintf(out, cap,
            "GET /mcp HTTP/1.1\r\n"
            "Host: 127.0.0.1\r\n"
            "Accept: text/event-stream\r\n"
            "Mcp-Session-Id: %s\r\n"
            "Last-Event-Id: %llu\r\n"
            "\r\n",
            session_id, (unsigned long long)leid);
    } else {
        n = snprintf(out, cap,
            "GET /mcp HTTP/1.1\r\n"
            "Host: 127.0.0.1\r\n"
            "Accept: text/event-stream\r\n"
            "Mcp-Session-Id: %s\r\n"
            "\r\n",
            session_id);
    }
    if (n < 0 || (size_t)n >= cap) { free(out); return NULL; }
    return out;
}

/* Drive a handshake POST and return the session id the server minted. */
static int handshake_post_get_sid(unsigned short port,
                                    char *out_sid, size_t cap) {
    char *req = build_post(INIT_BODY, NULL);
    if (!req) return -1;
    char *resp = NULL; size_t rn = 0;
    int rc = do_request(port, req, &resp, &rn);
    free(req);
    if (rc != 0) return -1;
    int ok = extract_header(resp, "Mcp-Session-Id", out_sid, cap);
    free(resp);
    return ok ? 0 : -1;
}

/* POST tools/call(trig) — fire one notification on the wire. */
static int trigger_one_notification(unsigned short port, const char *sid, int id) {
    char body[256];
    snprintf(body, sizeof body,
             "{\"jsonrpc\":\"2.0\",\"id\":%d,\"method\":\"tools/call\","
             "\"params\":{\"name\":\"trig\",\"arguments\":{}}}", id);
    char *req = build_post(body, sid);
    if (!req) return -1;
    char *resp = NULL; size_t rn = 0;
    int rc = do_request(port, req, &resp, &rn);
    free(req); free(resp);
    return rc;
}

/* Server emits an `id:` line on every SSE event (SEP-1699). A fresh
 * resume from `Last-Event-Id: 0` replays everything recorded; from a
 * mid-buffer id, replays only events after it. */
static void test_sse_event_ids_and_replay(void) {
    /* No Origin allow-list for this test. */
    unsetenv("CMCP_HTTP_ALLOWED_ORIGINS");
    unsigned short port = pick_port();
    TEST_ASSERT(port != 0);

    cmcp_transport_t *t = cmcp_transport_http_listen("127.0.0.1", port);
    TEST_ASSERT(t != NULL);

    cmcp_server_t *s = cmcp_server_new("http-srv", "0.1.0");
    cmcp_server_set_capabilities(s, &(cmcp_server_capabilities_t){
        .tools_list_changed = 1,
    });
    cmcp_server_add_tool(s, &(cmcp_tool_t){
        .name = "trig", .handler = trig_notif_handler, .userdata = s,
    });

    server_arg_t sa = { s, t, 0 };
    pthread_t th;
    TEST_ASSERT(pthread_create(&th, NULL, server_thread_main, &sa) == 0);

    char sid[64] = {0};
    TEST_ASSERT(handshake_post_get_sid(port, sid, sizeof sid) == 0);

    /* notifications/initialized so the server is READY. */
    {
        const char *body =
            "{\"jsonrpc\":\"2.0\",\"method\":\"notifications/initialized\"}";
        char *req = build_post(body, sid);
        char *resp = NULL; size_t rn = 0;
        TEST_ASSERT(do_request(port, req, &resp, &rn) == 0);
        free(req); free(resp);
    }

    /* Fire two notifications BEFORE any SSE holder opens. The events
     * land in the replay ring with ids 1 and 2; no live delivery
     * because no GET is open yet. */
    TEST_ASSERT(trigger_one_notification(port, sid, 2) == 0);
    TEST_ASSERT(trigger_one_notification(port, sid, 3) == 0);

    /* GET with Last-Event-Id: 0 → expect both events replayed. */
    {
        char *req = build_get_sse_with_leid(sid, 1, 0);
        TEST_ASSERT(req != NULL);
        int fd = open_client(port);
        TEST_ASSERT(fd >= 0);
        TEST_ASSERT(send_all(fd, req, strlen(req)) == 0);
        free(req);

        char buf[8192];
        size_t n = sse_read_window(fd, buf, sizeof buf, 500);
        close(fd);
        TEST_ASSERT(n > 0);
        /* Headers + at least the two id lines */
        TEST_ASSERT(strstr(buf, "HTTP/1.1 200")    != NULL);
        TEST_ASSERT(strstr(buf, "Content-Type: text/event-stream") != NULL);
        TEST_ASSERT(strstr(buf, "id: 1\n")          != NULL);
        TEST_ASSERT(strstr(buf, "id: 2\n")          != NULL);
        TEST_ASSERT(strstr(buf, "notifications/tools/list_changed") != NULL);
    }

    /* GET with Last-Event-Id: 1 → expect only event 2 replayed. */
    {
        char *req = build_get_sse_with_leid(sid, 1, 1);
        TEST_ASSERT(req != NULL);
        int fd = open_client(port);
        TEST_ASSERT(fd >= 0);
        TEST_ASSERT(send_all(fd, req, strlen(req)) == 0);
        free(req);

        char buf[8192];
        size_t n = sse_read_window(fd, buf, sizeof buf, 500);
        close(fd);
        TEST_ASSERT(n > 0);
        TEST_ASSERT(strstr(buf, "id: 1\n")  == NULL);  /* not replayed */
        TEST_ASSERT(strstr(buf, "id: 2\n")  != NULL);
    }

    /* GET with Last-Event-Id beyond the highest assigned → no replay
     * (server starts the stream fresh, no buffered events to deliver). */
    {
        char *req = build_get_sse_with_leid(sid, 1, 999);
        TEST_ASSERT(req != NULL);
        int fd = open_client(port);
        TEST_ASSERT(fd >= 0);
        TEST_ASSERT(send_all(fd, req, strlen(req)) == 0);
        free(req);

        char buf[8192];
        size_t n = sse_read_window(fd, buf, sizeof buf, 300);
        close(fd);
        TEST_ASSERT(n > 0);
        TEST_ASSERT(strstr(buf, "HTTP/1.1 200") != NULL);
        /* Stream opened but no historical events visible — only the
         * headers + an empty body window. */
        const char *body_start = strstr(buf, "\r\n\r\n");
        TEST_ASSERT(body_start && strstr(body_start, "id: ") == NULL);
    }

    cmcp_transport_wake(t);
    pthread_join(th, NULL);
    cmcp_transport_close(t);
    cmcp_server_free(s);
}

/* ====================================================================== */
/* Slowloris defense (Tier 6 axis 6.5.1b)                                  */
/* ====================================================================== */

/* A peer that opens a connection and sends only partial headers — never
 * finishing the request — used to be able to hold a worker indefinitely.
 * With CMCP_HTTP_IDLE_TIMEOUT_MS the server closes the connection (and
 * sends 408) after the configured idle window. We set 250ms here to
 * keep the test cheap. */
static void test_slowloris_idle_timeout(void) {
    setenv("CMCP_HTTP_IDLE_TIMEOUT_MS", "250", 1);

    unsigned short port = pick_port();
    TEST_ASSERT(port != 0);

    cmcp_server_t *s = cmcp_server_new("slow-loris-test", "0.1.0");
    cmcp_transport_t *t = cmcp_transport_http_listen("127.0.0.1", port);
    TEST_ASSERT(t != NULL);

    server_arg_t arg = { s, t, 0 };
    pthread_t th;
    pthread_create(&th, NULL, server_thread_main, &arg);

    int fd = open_client(port);
    TEST_ASSERT(fd >= 0);

    /* Send only the request line and a leading header — no terminator.
     * A polite peer would follow up; a slowloris stops here. */
    const char *partial =
        "POST /mcp HTTP/1.1\r\n"
        "Host: 127.0.0.1\r\n";
    TEST_ASSERT(send_all(fd, partial, strlen(partial)) == 0);

    /* Wait past the idle window and assert the server closes us out.
     * It should send 408 first; we tolerate either receiving the 408
     * or an outright RST — both are valid signals that the server is
     * defending itself, not hanging. */
    char buf[1024];
    /* Bounded read window: timeout is wall-clock at the test side, so
     * we wait up to 2s for the server to react to the 250ms idle. */
    struct pollfd p = { fd, POLLIN, 0 };
    int rv = poll(&p, 1, 2000);
    TEST_ASSERT(rv > 0);    /* server closed or wrote something */
    ssize_t n = recv(fd, buf, sizeof buf - 1, 0);
    close(fd);
    if (n > 0) {
        buf[n] = '\0';
        TEST_ASSERT(strstr(buf, "408") != NULL);
    } else {
        /* n == 0: server hung up (no bytes); n < 0: RST. Either way,
         * the connection did not survive the idle window — pass. */
        TEST_ASSERT(n <= 0);
    }

    cmcp_transport_wake(t);
    pthread_join(th, NULL);
    cmcp_transport_close(t);
    cmcp_server_free(s);
    unsetenv("CMCP_HTTP_IDLE_TIMEOUT_MS");
}

/* The whole-request deadline applies even if the peer dribbles bytes
 * inside the idle window. We set a very short deadline (500ms) and a
 * normal idle (1000ms), then have a peer send one byte every ~250ms
 * — never tripping the idle timer but blowing the deadline. */
static void test_slowloris_deadline(void) {
    setenv("CMCP_HTTP_IDLE_TIMEOUT_MS", "1000", 1);
    setenv("CMCP_HTTP_DEADLINE_MS", "500", 1);

    unsigned short port = pick_port();
    TEST_ASSERT(port != 0);

    cmcp_server_t *s = cmcp_server_new("slow-deadline-test", "0.1.0");
    cmcp_transport_t *t = cmcp_transport_http_listen("127.0.0.1", port);
    TEST_ASSERT(t != NULL);

    server_arg_t arg = { s, t, 0 };
    pthread_t th;
    pthread_create(&th, NULL, server_thread_main, &arg);

    int fd = open_client(port);
    TEST_ASSERT(fd >= 0);

    /* Send a long request line one byte at a time, ~50ms between bytes.
     * Each individual recv is well under idle_ms; cumulative time over
     * deadline_ms triggers the whole-request budget. */
    const char *partial = "POST /mcp HTTP/1.1\r\nHost: x\r\n";
    for (size_t i = 0; i < strlen(partial); i++) {
        if (send_all(fd, partial + i, 1) != 0) break;
        struct timespec ts = { 0, 50 * 1000 * 1000 };  /* 50ms */
        nanosleep(&ts, NULL);
    }

    /* By now ~1.5s elapsed at the test side; the server must have
     * tripped the 500ms deadline. */
    struct pollfd p = { fd, POLLIN, 0 };
    int rv = poll(&p, 1, 2000);
    TEST_ASSERT(rv > 0);
    char buf[1024];
    ssize_t n = recv(fd, buf, sizeof buf - 1, 0);
    close(fd);
    if (n > 0) {
        buf[n] = '\0';
        TEST_ASSERT(strstr(buf, "408") != NULL);
    } else {
        TEST_ASSERT(n <= 0);
    }

    cmcp_transport_wake(t);
    pthread_join(th, NULL);
    cmcp_transport_close(t);
    cmcp_server_free(s);
    unsetenv("CMCP_HTTP_IDLE_TIMEOUT_MS");
    unsetenv("CMCP_HTTP_DEADLINE_MS");
}

/* ====================================================================== */
/* Accept-rate limiter (Tier 6 axis 6.5.2)                                 */
/* ====================================================================== */

/* Open many TCP connections in quick succession and verify that the
 * accept-rate token bucket drains: once burst capacity is exhausted,
 * surplus connections must receive a 503 with Retry-After rather than
 * being silently accepted (which would let a flood saturate the
 * acceptor) or hung indefinitely.
 *
 * Setup: rate = 1 conn/sec, burst = 2. After 2 accepted POSTs we
 * expect the next several connections (sent within the same ~100ms
 * window) to be 503'd. */
static void test_accept_rate_limit_503(void) {
    setenv("CMCP_HTTP_ACCEPT_RATE",  "1",  1);
    setenv("CMCP_HTTP_ACCEPT_BURST", "2",  1);

    unsigned short port = pick_port();
    TEST_ASSERT(port != 0);

    cmcp_server_t *s = cmcp_server_new("rate-limit-test", "0.1.0");
    cmcp_transport_t *t = cmcp_transport_http_listen("127.0.0.1", port);
    TEST_ASSERT(t != NULL);

    server_arg_t arg = { s, t, 0 };
    pthread_t th;
    pthread_create(&th, NULL, server_thread_main, &arg);

    /* Hammer the listener with N connections back to back. Each opens
     * a fresh socket and sends a tiny invalid request line so the
     * server replies (whether 400 from the parser or 503 from the
     * acceptor — we just need a status to inspect). */
    enum { N = 12 };
    int seen_503 = 0;
    int seen_other = 0;
    for (int i = 0; i < N; i++) {
        int fd = open_client(port);
        if (fd < 0) continue;
        const char *req = "GET / HTTP/1.1\r\nHost: x\r\n\r\n";
        if (send_all(fd, req, strlen(req)) != 0) { close(fd); continue; }

        char buf[256];
        struct pollfd p = { fd, POLLIN, 0 };
        int rv = poll(&p, 1, 1000);
        if (rv > 0) {
            ssize_t n = recv(fd, buf, sizeof buf - 1, 0);
            if (n > 0) {
                buf[n] = '\0';
                if (strstr(buf, "503") && strstr(buf, "Retry-After"))
                    seen_503++;
                else
                    seen_other++;
            }
        }
        close(fd);
    }

    /* With burst=2 and rate=1/sec across ~N immediate connections, at
     * least some must have been 503'd. Exact count is timing-sensitive;
     * we only assert that the gate fired at least once. */
    TEST_ASSERT(seen_503 >= 1);
    /* Sanity: not *all* connections were 503'd — the burst budget
     * permitted at least one through. */
    TEST_ASSERT(seen_other >= 1);

    cmcp_transport_wake(t);
    pthread_join(th, NULL);
    cmcp_transport_close(t);
    cmcp_server_free(s);
    unsetenv("CMCP_HTTP_ACCEPT_RATE");
    unsetenv("CMCP_HTTP_ACCEPT_BURST");
}

/* Setting rate to 0 (or negative) disables the gate entirely: every
 * connection should be accepted regardless of arrival rate. Mirrors
 * the env-tunable "off switch" promised in CLAUDE.md / docs. */
static void test_accept_rate_disabled_passes_all(void) {
    setenv("CMCP_HTTP_ACCEPT_RATE", "0", 1);

    unsigned short port = pick_port();
    TEST_ASSERT(port != 0);

    cmcp_server_t *s = cmcp_server_new("rate-off-test", "0.1.0");
    cmcp_transport_t *t = cmcp_transport_http_listen("127.0.0.1", port);
    TEST_ASSERT(t != NULL);

    server_arg_t arg = { s, t, 0 };
    pthread_t th;
    pthread_create(&th, NULL, server_thread_main, &arg);

    int seen_503 = 0;
    for (int i = 0; i < 8; i++) {
        int fd = open_client(port);
        if (fd < 0) continue;
        const char *req = "GET / HTTP/1.1\r\nHost: x\r\n\r\n";
        send_all(fd, req, strlen(req));
        char buf[256];
        struct pollfd p = { fd, POLLIN, 0 };
        if (poll(&p, 1, 1000) > 0) {
            ssize_t n = recv(fd, buf, sizeof buf - 1, 0);
            if (n > 0) {
                buf[n] = '\0';
                if (strstr(buf, "503")) seen_503++;
            }
        }
        close(fd);
    }

    TEST_ASSERT(seen_503 == 0);

    cmcp_transport_wake(t);
    pthread_join(th, NULL);
    cmcp_transport_close(t);
    cmcp_server_free(s);
    unsetenv("CMCP_HTTP_ACCEPT_RATE");
}

/* ====================================================================== */

int main(void) {
    /* The slowloris tests intentionally write to a socket while the
     * server is closing it from its end (idle / deadline trip). Without
     * SIGPIPE ignored, the test process gets killed mid-send. The
     * library itself uses MSG_NOSIGNAL on its sends; the tests' raw
     * `send()` helpers don't, so we ignore at the process level. */
    signal(SIGPIPE, SIG_IGN);

    fprintf(stderr, "test_http_server:\n");

    TEST_RUN(test_initialize_round_trip);
    TEST_RUN(test_session_id_required_after_init);
    TEST_RUN(test_malformed_request);
    TEST_RUN(test_get_sse_handshake);
    TEST_RUN(test_protocol_version_header);
    TEST_RUN(test_origin_allowlist);
    TEST_RUN(test_sse_event_ids_and_replay);
    TEST_RUN(test_close_unblocks_reader);
    TEST_RUN(test_slowloris_idle_timeout);
    TEST_RUN(test_slowloris_deadline);
    TEST_RUN(test_accept_rate_limit_503);
    TEST_RUN(test_accept_rate_disabled_passes_all);

    TEST_DONE();
}
