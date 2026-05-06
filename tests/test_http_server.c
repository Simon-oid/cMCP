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

    cmcp_transport_close(t);
    pthread_join(th, NULL);
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

    cmcp_transport_close(t);
    pthread_join(th, NULL);
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

    cmcp_transport_close(t);
    pthread_join(th, NULL);
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

    cmcp_transport_close(t);
    pthread_join(th, NULL);
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

    cmcp_transport_close(t);
    /* If the close failed to wake the reader, this hangs. The test
     * harness's parent process timeouts will catch that. */
    TEST_ASSERT(pthread_join(th, NULL) == 0);
    TEST_ASSERT(sa.rc == CMCP_OK);     /* clean shutdown */

    cmcp_server_free(s);
}

/* ====================================================================== */

int main(void) {
    fprintf(stderr, "test_http_server:\n");

    TEST_RUN(test_initialize_round_trip);
    TEST_RUN(test_session_id_required_after_init);
    TEST_RUN(test_malformed_request);
    TEST_RUN(test_get_sse_handshake);
    TEST_RUN(test_close_unblocks_reader);

    TEST_DONE();
}
