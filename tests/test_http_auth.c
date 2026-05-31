/* test_http_auth.c — B.1: HTTP request headers reach the handler.
 *
 * The MCP threat model terminates TLS in front of the server, but a host
 * that wants per-tool auth still needs the `Authorization` bytes to
 * reach policy. cmcp_handler_get_header() is that channel. This test:
 *
 *   - registers a tool whose handler reads several request headers,
 *   - drives a real HTTP handshake + tools/call carrying an
 *     `Authorization: Bearer <token>` (and a custom header),
 *   - asserts the handler saw both verbatim, that lookup is
 *     case-insensitive, and that an absent header reads as NULL,
 *   - captures the process's stderr across the whole exchange and
 *     asserts the secret token never leaked to it (CMCP_LOG_REDACT=1).
 *
 * No libcurl — raw HTTP/1.1 over socket(), like test_http_server.c. */

#define _POSIX_C_SOURCE 200809L

#include "test.h"
#include "cmcp.h"
#include "cmcp_json.h"
#include "cmcp_server.h"
#include "cmcp_transport.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <unistd.h>

#define BEARER "Bearer s3cr3t-DEADBEEF-tok3n"
#define XTEST  "hello-custom-value"

/* What the handler observed, asserted after the server is joined. */
static char g_seen_auth[256];
static char g_seen_xtest[256];
static int  g_absent_is_null = 0;
static int  g_handler_ran     = 0;

static int auth_probe(const cmcp_json_t *arguments, void *userdata,
                      cmcp_handler_ctx_t *hctx,
                      cmcp_json_t **out_content, int *out_is_error) {
    (void)arguments; (void)userdata; (void)out_is_error;
    /* Lowercase name on purpose: proves case-insensitive lookup. */
    const char *a = cmcp_handler_get_header(hctx, "authorization");
    const char *x = cmcp_handler_get_header(hctx, "X-Test-Header");
    const char *absent = cmcp_handler_get_header(hctx, "X-Absent-Header");

    if (a) { strncpy(g_seen_auth,  a, sizeof g_seen_auth  - 1); }
    if (x) { strncpy(g_seen_xtest, x, sizeof g_seen_xtest - 1); }
    g_absent_is_null = (absent == NULL);
    g_handler_ran    = 1;

    *out_content = cmcp_tool_text_content("ok");
    return CMCP_OK;
}

/* ---- minimal raw-HTTP harness (mirrors test_http_server.c) ----------- */

typedef struct { cmcp_server_t *s; cmcp_transport_t *t; } server_arg_t;

static void *server_thread_main(void *arg) {
    server_arg_t *a = (server_arg_t *)arg;
    cmcp_server_run(a->s, a->t);
    return NULL;
}

static unsigned short pick_port(void) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return 0;
    int yes = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof yes);
    struct sockaddr_in sa; memset(&sa, 0, sizeof sa);
    sa.sin_family = AF_INET;
    sa.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    sa.sin_port = 0;
    if (bind(fd, (struct sockaddr *)&sa, sizeof sa) != 0) { close(fd); return 0; }
    socklen_t slen = sizeof sa;
    if (getsockname(fd, (struct sockaddr *)&sa, &slen) != 0) { close(fd); return 0; }
    unsigned short p = ntohs(sa.sin_port);
    close(fd);
    return p;
}

static int open_client(unsigned short port) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    struct sockaddr_in sa; memset(&sa, 0, sizeof sa);
    sa.sin_family = AF_INET;
    sa.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    sa.sin_port = htons(port);
    for (int i = 0; i < 200; i++) {
        if (connect(fd, (struct sockaddr *)&sa, sizeof sa) == 0) return fd;
        struct timespec ts = { 0, 1000000 }; nanosleep(&ts, NULL);
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

static int recv_all(int fd, char **out, size_t *out_len) {
    size_t cap = 4096, len = 0;
    char *b = malloc(cap);
    if (!b) return -1;
    for (;;) {
        if (len + 1 >= cap) { cap *= 2; char *nb = realloc(b, cap);
                              if (!nb) { free(b); return -1; } b = nb; }
        ssize_t n = recv(fd, b + len, cap - 1 - len, 0);
        if (n < 0) { free(b); return -1; }
        if (n == 0) break;
        len += (size_t)n;
    }
    b[len] = '\0';
    *out = b; if (out_len) *out_len = len;
    return 0;
}

static int do_request(unsigned short port, const char *req,
                      char **out_resp, size_t *out_len) {
    int fd = open_client(port);
    if (fd < 0) return -1;
    if (send_all(fd, req, strlen(req)) != 0) { close(fd); return -1; }
    int rc = recv_all(fd, out_resp, out_len);
    close(fd);
    return rc;
}

static int extract_header(const char *resp, const char *name,
                          char *out, size_t out_cap) {
    const char *eoh = strstr(resp, "\r\n\r\n");
    if (!eoh) eoh = resp + strlen(resp);
    size_t nlen = strlen(name);
    const char *p = resp;
    while (p < eoh) {
        const char *eol = strstr(p, "\r\n");
        if (!eol || eol > eoh) break;
        if ((size_t)(eol - p) > nlen + 1 &&
            strncasecmp(p, name, nlen) == 0 && p[nlen] == ':') {
            const char *v = p + nlen + 1;
            while (v < eol && (*v == ' ' || *v == '\t')) v++;
            size_t vlen = (size_t)(eol - v);
            if (vlen >= out_cap) vlen = out_cap - 1;
            memcpy(out, v, vlen); out[vlen] = '\0';
            return 1;
        }
        p = eol + 2;
    }
    return 0;
}

static const char *INIT_BODY =
    "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"initialize\",\"params\":{"
        "\"protocolVersion\":\"" CMCP_PROTOCOL_VERSION "\","
        "\"capabilities\":{},"
        "\"clientInfo\":{\"name\":\"test\",\"version\":\"0\"}}}";

static char *build_post(const char *body, const char *session_id,
                        const char *extra_headers) {
    size_t body_len = body ? strlen(body) : 0;
    size_t cap = body_len + 1024;
    char *out = malloc(cap);
    if (!out) return NULL;
    int n = snprintf(out, cap,
        "POST /mcp HTTP/1.1\r\n"
        "Host: 127.0.0.1\r\n"
        "Content-Type: application/json\r\n"
        "%s%s%s"
        "%s"
        "Content-Length: %zu\r\n"
        "\r\n"
        "%s",
        session_id ? "Mcp-Session-Id: " : "",
        session_id ? session_id : "",
        session_id ? "\r\n" : "",
        extra_headers ? extra_headers : "",
        body_len, body ? body : "");
    if (n < 0 || (size_t)n >= cap) { free(out); return NULL; }
    return out;
}

/* ---- the test --------------------------------------------------------- */

static void test_auth_header_reaches_handler(void) {
    setenv("CMCP_LOG_REDACT", "1", 1);

    unsigned short port = pick_port();
    TEST_ASSERT(port != 0);

    cmcp_transport_t *t = cmcp_transport_http_listen("127.0.0.1", port);
    TEST_ASSERT(t != NULL);
    cmcp_server_t *s = cmcp_server_new("auth-srv", "0.1.0");
    cmcp_tool_t tool = { .name = "auth_probe", .handler = auth_probe };
    TEST_ASSERT(cmcp_server_add_tool(s, &tool) == CMCP_OK);

    /* Redirect stderr to a temp file for the whole server exchange, so we
     * can later prove the token never leaked to it. Restore BEFORE any
     * TEST_ASSERT so the test's own diagnostics stay visible. */
    char errpath[] = "/tmp/cmcp_auth_err_XXXXXX";
    int efd = mkstemp(errpath);
    fflush(stderr);
    int saved_err = dup(STDERR_FILENO);
    if (efd >= 0) dup2(efd, STDERR_FILENO);

    server_arg_t sa = { s, t };
    pthread_t th;
    int thrc = pthread_create(&th, NULL, server_thread_main, &sa);

    char sid[64] = {0};
    int got_sid = 0, got_call = 0, status_ok = 0;
    if (thrc == 0) {
        /* 1) initialize → mint a session. */
        char *req = build_post(INIT_BODY, NULL, NULL);
        char *resp = NULL;
        if (req && do_request(port, req, &resp, NULL) == 0 && resp) {
            got_sid = extract_header(resp, "Mcp-Session-Id", sid, sizeof sid);
        }
        free(req); free(resp);

        /* 2) notifications/initialized → reach SS_READY. */
        req = build_post("{\"jsonrpc\":\"2.0\",\"method\":\"notifications/initialized\"}",
                         got_sid ? sid : NULL, NULL);
        resp = NULL;
        if (req) do_request(port, req, &resp, NULL);
        free(req); free(resp);

        /* 3) tools/call carrying the auth + custom headers. */
        const char *extra =
            "Authorization: " BEARER "\r\n"
            "X-Test-Header: " XTEST "\r\n";
        req = build_post(
            "{\"jsonrpc\":\"2.0\",\"id\":2,\"method\":\"tools/call\","
            "\"params\":{\"name\":\"auth_probe\",\"arguments\":{}}}",
            got_sid ? sid : NULL, extra);
        resp = NULL;
        if (req && do_request(port, req, &resp, NULL) == 0 && resp) {
            got_call = 1;
            status_ok = (strncmp(resp, "HTTP/1.1 200", 12) == 0);
        }
        free(req); free(resp);

        cmcp_transport_wake(t);
        pthread_join(th, NULL);
    }

    /* Restore stderr, then slurp what the server wrote. */
    fflush(stderr);
    if (saved_err >= 0) { dup2(saved_err, STDERR_FILENO); close(saved_err); }
    char *errlog = NULL;
    if (efd >= 0) {
        lseek(efd, 0, SEEK_SET);
        size_t cap = 4096, len = 0;
        errlog = malloc(cap);
        if (errlog) {
            for (;;) {
                if (len + 4096 + 1 > cap) { cap *= 2; char *nb = realloc(errlog, cap);
                                            if (!nb) { free(errlog); errlog = NULL; break; }
                                            errlog = nb; }
                ssize_t r = read(efd, errlog + len, 4096);
                if (r <= 0) break;
                len += (size_t)r;
            }
            if (errlog) errlog[len] = '\0';
        }
        close(efd);
        unlink(errpath);
    }

    /* Assertions (stderr restored — these print normally). */
    TEST_ASSERT(thrc == 0);
    TEST_ASSERT(got_sid == 1);
    TEST_ASSERT(got_call == 1);
    TEST_ASSERT(status_ok == 1);
    TEST_ASSERT(g_handler_ran == 1);
    /* Verbatim Authorization, including the "Bearer " prefix. */
    TEST_ASSERT(strcmp(g_seen_auth, BEARER) == 0);
    /* Custom header, verbatim. */
    TEST_ASSERT(strcmp(g_seen_xtest, XTEST) == 0);
    /* Absent header → NULL. */
    TEST_ASSERT(g_absent_is_null == 1);
    /* The secret never leaked to stderr. */
    if (errlog) {
        TEST_ASSERT(strstr(errlog, "s3cr3t-DEADBEEF-tok3n") == NULL);
        free(errlog);
    } else {
        TEST_ASSERT(0 && "failed to capture stderr");
    }

    cmcp_transport_close(t);
    cmcp_server_free(s);
}

/* stdio transport has no headers: get_header must return NULL, never
 * crash. We can exercise that without a live server by confirming the
 * NULL-safety contract holds on a NULL ctx (the public NULL-safe path). */
static void test_get_header_null_safe(void) {
    TEST_ASSERT(cmcp_handler_get_header(NULL, "Authorization") == NULL);
}

int main(void) {
    fprintf(stderr, "test_http_auth:\n");
    TEST_RUN(test_get_header_null_safe);
    TEST_RUN(test_auth_header_reaches_handler);
    TEST_DONE();
}
