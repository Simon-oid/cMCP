/* test_http_modern — MCP 2026-07-28 requests over Streamable HTTP.
 *
 * Real sockets against cmcp_transport_http_listen. The modern revision
 * has no sessions, mirrors body fields into headers that the server must
 * check (MCP-Protocol-Version, Mcp-Method, Mcp-Name → -32020 on any
 * mismatch), maps protocol errors to HTTP statuses (400 / 404), and
 * carries a request's own notifications on an SSE response scoped to
 * that request. The legacy session path must keep working beside it. */

/* nanosleep, strncasecmp — POSIX. */
#define _POSIX_C_SOURCE 200809L

#include "test.h"
#include "cmcp.h"
#include "cmcp_json.h"
#include "cmcp_server.h"
#include "cmcp_transport.h"
#include "cmcp_types.h"
#include "../src/http_modern.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#define MODERN "2026-07-28"

#define META "\"_meta\":{"                                              \
    "\"io.modelcontextprotocol/protocolVersion\":\"" MODERN "\","       \
    "\"io.modelcontextprotocol/clientCapabilities\":{}}"

#define META_PROGRESS "\"_meta\":{"                                     \
    "\"io.modelcontextprotocol/protocolVersion\":\"" MODERN "\","       \
    "\"io.modelcontextprotocol/clientCapabilities\":{},"                \
    "\"progressToken\":\"p1\"}"

#define ECHO_CALL "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"tools/call\"," \
    "\"params\":{\"name\":\"echo\",\"arguments\":{\"text\":\"hi\"}," META "}}"

#define STD_HEADERS "MCP-Protocol-Version: " MODERN "\r\n"              \
    "Mcp-Method: tools/call\r\nMcp-Name: echo\r\n"

/* ====================================================================== */
/* Server under test                                                       */
/* ====================================================================== */

static int echo(const cmcp_json_t *args, void *ud, cmcp_handler_ctx_t *hctx,
                cmcp_json_t **out, int *is_error) {
    (void)ud; (void)hctx;
    const cmcp_json_t *t = cmcp_json_object_get(args, "text");
    *out = cmcp_tool_text_content(t && t->type == CMCP_JSON_STRING
                                      ? cmcp_json_string(t) : "");
    *is_error = 0;
    return CMCP_OK;
}

static int ask(const cmcp_json_t *args, void *ud, cmcp_handler_ctx_t *hctx,
               cmcp_json_t **out, int *is_error) {
    (void)args; (void)ud;
    cmcp_json_t *ans = NULL;
    int rc = cmcp_handler_input(hctx, "q", "elicitation/create",
                                cmcp_json_parse_cstr("{\"message\":\"?\"}"),
                                &ans);
    if (rc != CMCP_OK) return rc;
    cmcp_json_free(ans);
    *out = cmcp_tool_text_content("answered");
    *is_error = 0;
    return CMCP_OK;
}

/* Two progress updates, then the result. */
static int slow(const cmcp_json_t *args, void *ud, cmcp_handler_ctx_t *hctx,
                cmcp_json_t **out, int *is_error) {
    (void)args; (void)ud;
    cmcp_handler_progress(hctx, 1, 2, "half");
    cmcp_handler_progress(hctx, 2, 2, "done");
    *out = cmcp_tool_text_content("slow done");
    *is_error = 0;
    return CMCP_OK;
}

typedef struct {
    cmcp_transport_t *t;
    cmcp_server_t    *s;
    pthread_t         th;
    unsigned short    port;
} hrig_t;

static void *server_main(void *arg) {
    hrig_t *r = (hrig_t *)arg;
    cmcp_server_run(r->s, r->t);
    return NULL;
}

static unsigned short pick_port(void) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return 0;
    struct sockaddr_in sa;
    memset(&sa, 0, sizeof sa);
    sa.sin_family = AF_INET;
    sa.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    socklen_t sl = sizeof sa;
    if (bind(fd, (struct sockaddr *)&sa, sizeof sa) != 0 ||
        getsockname(fd, (struct sockaddr *)&sa, &sl) != 0) {
        close(fd);
        return 0;
    }
    close(fd);
    return ntohs(sa.sin_port);
}

static int hrig_start(hrig_t *r) {
    memset(r, 0, sizeof *r);
    r->port = pick_port();
    r->t = cmcp_transport_http_listen("127.0.0.1", r->port);
    r->s = cmcp_server_new("http-modern", "1.0");
    if (!r->port || !r->t || !r->s) return -1;
    const cmcp_tool_t tools[] = {
        {.name = "echo", .description = "d", .handler = echo,
         .input_schema = "{\"type\":\"object\",\"properties\":"
                         "{\"text\":{\"type\":\"string\"}}}"},
        {.name = "ask", .description = "d", .handler = ask,
         .input_schema = "{\"type\":\"object\"}"},
        {.name = "slow", .description = "d", .handler = slow,
         .input_schema = "{\"type\":\"object\"}"},
    };
    for (size_t i = 0; i < sizeof tools / sizeof tools[0]; i++)
        if (cmcp_server_add_tool(r->s, &tools[i]) != CMCP_OK) return -1;
    return pthread_create(&r->th, NULL, server_main, r) == 0 ? 0 : -1;
}

static void hrig_stop(hrig_t *r) {
    cmcp_transport_wake(r->t);
    pthread_join(r->th, NULL);
    cmcp_transport_close(r->t);
    cmcp_server_free(r->s);
}

/* ====================================================================== */
/* HTTP client helpers                                                     */
/* ====================================================================== */

static int open_client(unsigned short port) {
    for (int i = 0; i < 100; i++) {
        int fd = socket(AF_INET, SOCK_STREAM, 0);
        if (fd < 0) return -1;
        struct sockaddr_in sa;
        memset(&sa, 0, sizeof sa);
        sa.sin_family = AF_INET;
        sa.sin_port = htons(port);
        sa.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        if (connect(fd, (struct sockaddr *)&sa, sizeof sa) == 0) return fd;
        close(fd);
        struct timespec ts = {0, 10 * 1000 * 1000};
        nanosleep(&ts, NULL);
    }
    return -1;
}

/* Read until the server closes (every reply is Connection: close). */
static char *recv_all(int fd) {
    size_t cap = 4096, len = 0;
    char *buf = (char *)malloc(cap);
    while (buf) {
        if (len + 1 >= cap) {
            char *nb = (char *)realloc(buf, cap * 2);
            if (!nb) { free(buf); return NULL; }
            buf = nb;
            cap *= 2;
        }
        ssize_t got = recv(fd, buf + len, cap - 1 - len, 0);
        if (got <= 0) break;
        len += (size_t)got;
    }
    if (buf) buf[len] = '\0';
    return buf;
}

/* POST `body` with extra header lines; returns the whole response. */
static char *post(hrig_t *r, const char *headers, const char *accept,
                  const char *body) {
    char req[8192];
    int n = snprintf(req, sizeof req,
        "POST /mcp HTTP/1.1\r\nHost: localhost\r\n"
        "Content-Type: application/json\r\nAccept: %s\r\n%s"
        "Content-Length: %zu\r\n\r\n%s",
        accept, headers, strlen(body), body);
    if (n < 0 || (size_t)n >= sizeof req) return NULL;
    int fd = open_client(r->port);
    if (fd < 0) return NULL;
    size_t off = 0;
    while (off < (size_t)n) {
        ssize_t w = send(fd, req + off, (size_t)n - off, 0);
        if (w <= 0) { close(fd); return NULL; }
        off += (size_t)w;
    }
    char *resp = recv_all(fd);
    close(fd);
    return resp;
}

#define ACCEPT_BOTH "application/json, text/event-stream"
#define ACCEPT_JSON "application/json"

static int status_of(const char *resp) {
    int st = 0;
    return (resp && sscanf(resp, "HTTP/1.1 %d", &st) == 1) ? st : 0;
}

static int has_header(const char *resp, const char *name) {
    const char *eoh = resp ? strstr(resp, "\r\n\r\n") : NULL;
    size_t nl = strlen(name);
    for (const char *p = resp; p && eoh && p < eoh; ) {
        const char *line = strstr(p, "\r\n");
        if (!line) break;
        p = line + 2;
        if (strncasecmp(p, name, nl) == 0 && p[nl] == ':') return 1;
    }
    return 0;
}

static cmcp_json_t *json_body(const char *resp) {
    const char *b = resp ? strstr(resp, "\r\n\r\n") : NULL;
    return b ? cmcp_json_parse_cstr(b + 4) : NULL;
}

static const cmcp_json_t *get(const cmcp_json_t *o, const char *k) {
    return (o && o->type == CMCP_JSON_OBJECT) ? cmcp_json_object_get(o, k) : NULL;
}

static long long int_or(const cmcp_json_t *v) {
    return (v && v->type == CMCP_JSON_INT) ? cmcp_json_int(v) : -1;
}

static int is_str(const cmcp_json_t *v, const char *want) {
    return v && v->type == CMCP_JSON_STRING &&
           strcmp(cmcp_json_string(v), want) == 0;
}

static long long error_code_of(const char *resp) {
    cmcp_json_t *j = json_body(resp);
    long long v = int_or(get(get(j, "error"), "code"));
    cmcp_json_free(j);
    return v;
}

/* POST and expect `status` with JSON-RPC error `code`. */
static void expect_error(hrig_t *r, const char *headers, const char *body,
                         int status, long long code) {
    char *resp = post(r, headers, ACCEPT_BOTH, body);
    TEST_ASSERT(status_of(resp) == status);
    TEST_ASSERT(error_code_of(resp) == code);
    free(resp);
}

/* ====================================================================== */
/* Tests                                                                   */
/* ====================================================================== */

static void test_modern_call_needs_no_session(void) {
    hrig_t r; TEST_ASSERT(hrig_start(&r) == 0);
    /* A stale Mcp-Session-Id from an older client is ignored. */
    char *resp = post(&r, STD_HEADERS "Mcp-Session-Id: not-a-session\r\n",
                      ACCEPT_BOTH, ECHO_CALL);
    TEST_ASSERT(status_of(resp) == 200);
    TEST_ASSERT(!has_header(resp, "Mcp-Session-Id"));
    cmcp_json_t *j = json_body(resp);
    TEST_ASSERT(is_str(get(get(j, "result"), "resultType"), "complete"));
    cmcp_json_free(j);
    free(resp);
    hrig_stop(&r);
}

static void test_missing_or_mismatched_headers_are_32020(void) {
    hrig_t r; TEST_ASSERT(hrig_start(&r) == 0);
    /* No MCP-Protocol-Version. */
    expect_error(&r, "Mcp-Method: tools/call\r\nMcp-Name: echo\r\n",
                 ECHO_CALL, 400, CMCP_RPC_HEADER_MISMATCH);
    /* Version header disagrees with the body. */
    expect_error(&r, "MCP-Protocol-Version: 2025-11-25\r\n"
                     "Mcp-Method: tools/call\r\nMcp-Name: echo\r\n",
                 ECHO_CALL, 400, CMCP_RPC_HEADER_MISMATCH);
    /* No Mcp-Method, then the wrong one. */
    expect_error(&r, "MCP-Protocol-Version: " MODERN "\r\nMcp-Name: echo\r\n",
                 ECHO_CALL, 400, CMCP_RPC_HEADER_MISMATCH);
    expect_error(&r, "MCP-Protocol-Version: " MODERN "\r\n"
                     "Mcp-Method: tools/list\r\nMcp-Name: echo\r\n",
                 ECHO_CALL, 400, CMCP_RPC_HEADER_MISMATCH);
    /* No Mcp-Name, then the wrong one: a router could have sent this
     * to the shard for `rm` while the body runs `echo`. */
    expect_error(&r, "MCP-Protocol-Version: " MODERN "\r\nMcp-Method: tools/call\r\n",
                 ECHO_CALL, 400, CMCP_RPC_HEADER_MISMATCH);
    expect_error(&r, "MCP-Protocol-Version: " MODERN "\r\n"
                     "Mcp-Method: tools/call\r\nMcp-Name: rm\r\n",
                 ECHO_CALL, 400, CMCP_RPC_HEADER_MISMATCH);
    hrig_stop(&r);
}

static void test_base64_sentinel_name_is_decoded(void) {
    hrig_t r; TEST_ASSERT(hrig_start(&r) == 0);
    /* "echo" = ZWNobw== */
    char *resp = post(&r, "MCP-Protocol-Version: " MODERN "\r\n"
                          "Mcp-Method: tools/call\r\n"
                          "Mcp-Name: =?base64?ZWNobw==?=\r\n",
                      ACCEPT_BOTH, ECHO_CALL);
    TEST_ASSERT(status_of(resp) == 200);
    free(resp);
    /* A sentinel that is not base64 is a malformed header. */
    expect_error(&r, "MCP-Protocol-Version: " MODERN "\r\n"
                     "Mcp-Method: tools/call\r\nMcp-Name: =?base64?***?=\r\n",
                 ECHO_CALL, 400, CMCP_RPC_HEADER_MISMATCH);
    hrig_stop(&r);
}

static void test_unsupported_version_is_400_32022(void) {
    hrig_t r; TEST_ASSERT(hrig_start(&r) == 0);
    const char *body =
        "{\"jsonrpc\":\"2.0\",\"id\":3,\"method\":\"tools/list\",\"params\":{"
        "\"_meta\":{\"io.modelcontextprotocol/protocolVersion\":\"2099-01-01\","
        "\"io.modelcontextprotocol/clientCapabilities\":{}}}}";
    char *resp = post(&r, "MCP-Protocol-Version: 2099-01-01\r\n"
                          "Mcp-Method: tools/list\r\n", ACCEPT_BOTH, body);
    TEST_ASSERT(status_of(resp) == 400);
    TEST_ASSERT(error_code_of(resp) == CMCP_RPC_UNSUPPORTED_PROTOCOL_VERSION);
    cmcp_json_t *j = json_body(resp);
    const cmcp_json_t *sup = get(get(get(j, "error"), "data"), "supported");
    TEST_ASSERT(sup && sup->type == CMCP_JSON_ARRAY && cmcp_json_array_len(sup) == 2);
    TEST_ASSERT(int_or(get(j, "id")) == 3);
    cmcp_json_free(j);
    free(resp);
    hrig_stop(&r);
}

static void test_unknown_method_is_404(void) {
    hrig_t r; TEST_ASSERT(hrig_start(&r) == 0);
    expect_error(&r, "MCP-Protocol-Version: " MODERN "\r\nMcp-Method: ping\r\n",
                 "{\"jsonrpc\":\"2.0\",\"id\":4,\"method\":\"ping\",\"params\":{"
                 META "}}", 404, CMCP_RPC_METHOD_NOT_FOUND);
    hrig_stop(&r);
}

static void test_missing_capability_is_400_32021(void) {
    hrig_t r; TEST_ASSERT(hrig_start(&r) == 0);
    expect_error(&r, "MCP-Protocol-Version: " MODERN "\r\n"
                     "Mcp-Method: tools/call\r\nMcp-Name: ask\r\n",
                 "{\"jsonrpc\":\"2.0\",\"id\":5,\"method\":\"tools/call\","
                 "\"params\":{\"name\":\"ask\",\"arguments\":{}," META "}}",
                 400, CMCP_RPC_MISSING_CLIENT_CAPABILITY);
    hrig_stop(&r);
}

static void test_input_required_is_200(void) {
    hrig_t r; TEST_ASSERT(hrig_start(&r) == 0);
    char *resp = post(&r, "MCP-Protocol-Version: " MODERN "\r\n"
                          "Mcp-Method: tools/call\r\nMcp-Name: ask\r\n",
        ACCEPT_BOTH,
        "{\"jsonrpc\":\"2.0\",\"id\":6,\"method\":\"tools/call\","
        "\"params\":{\"name\":\"ask\",\"arguments\":{},\"_meta\":{"
        "\"io.modelcontextprotocol/protocolVersion\":\"" MODERN "\","
        "\"io.modelcontextprotocol/clientCapabilities\":{\"elicitation\":{}}}}}");
    TEST_ASSERT(status_of(resp) == 200);
    TEST_ASSERT(resp && strstr(resp, "\"input_required\"") != NULL);
    TEST_ASSERT(resp && strstr(resp, "\"elicitation/create\"") != NULL);
    free(resp);
    hrig_stop(&r);
}

/* Progress rides an SSE stream scoped to this request, ending with the
 * response; events carry no ids (2026-07-28 streams are not resumable). */
static void test_progress_streams_on_the_request(void) {
    hrig_t r; TEST_ASSERT(hrig_start(&r) == 0);
    const char *body =
        "{\"jsonrpc\":\"2.0\",\"id\":7,\"method\":\"tools/call\","
        "\"params\":{\"name\":\"slow\",\"arguments\":{}," META_PROGRESS "}}";
    char *resp = post(&r, "MCP-Protocol-Version: " MODERN "\r\n"
                          "Mcp-Method: tools/call\r\nMcp-Name: slow\r\n",
                      ACCEPT_BOTH, body);
    TEST_ASSERT(status_of(resp) == 200);
    TEST_ASSERT(resp && strstr(resp, "Content-Type: text/event-stream") != NULL);
    TEST_ASSERT(resp && strstr(resp, "X-Accel-Buffering: no") != NULL);
    const char *p1 = resp ? strstr(resp, "\"half\"") : NULL;
    const char *p2 = resp ? strstr(resp, "\"done\"") : NULL;
    const char *fin = resp ? strstr(resp, "\"slow done\"") : NULL;
    TEST_ASSERT(p1 && p2 && fin && p1 < p2 && p2 < fin);
    TEST_ASSERT(resp && strstr(resp, "\nid:") == NULL);
    free(resp);

    /* A client that only accepts JSON gets the plain response. */
    resp = post(&r, "MCP-Protocol-Version: " MODERN "\r\n"
                    "Mcp-Method: tools/call\r\nMcp-Name: slow\r\n",
                ACCEPT_JSON, body);
    TEST_ASSERT(status_of(resp) == 200);
    TEST_ASSERT(resp && strstr(resp, "Content-Type: application/json") != NULL);
    TEST_ASSERT(resp && strstr(resp, "\"slow done\"") != NULL);
    TEST_ASSERT(resp && strstr(resp, "notifications/progress") == NULL);
    free(resp);
    hrig_stop(&r);
}

/* The legacy session path is untouched, and both eras share a server. */
static void test_legacy_session_beside_modern(void) {
    hrig_t r; TEST_ASSERT(hrig_start(&r) == 0);
    char *resp = post(&r, "", ACCEPT_BOTH,
        "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"initialize\",\"params\":{"
        "\"protocolVersion\":\"" CMCP_PROTOCOL_VERSION "\",\"capabilities\":{},"
        "\"clientInfo\":{\"name\":\"legacy\",\"version\":\"0\"}}}");
    TEST_ASSERT(status_of(resp) == 200);
    TEST_ASSERT(has_header(resp, "Mcp-Session-Id"));
    free(resp);

    /* A legacy request without the session is still refused... */
    resp = post(&r, "", ACCEPT_BOTH,
                "{\"jsonrpc\":\"2.0\",\"id\":2,\"method\":\"tools/list\"}");
    TEST_ASSERT(status_of(resp) == 400);
    free(resp);
    /* ...while a modern one needs none. */
    resp = post(&r, STD_HEADERS, ACCEPT_BOTH, ECHO_CALL);
    TEST_ASSERT(status_of(resp) == 200);
    free(resp);
    hrig_stop(&r);
}

static void test_header_decode_unit(void) {
    char *v = cmcp_http_header_decode("plain");
    TEST_ASSERT(v && strcmp(v, "plain") == 0);
    free(v);
    /* Spec examples: "Hello, 世界" and " padded ". */
    v = cmcp_http_header_decode("=?base64?SGVsbG8sIOS4lueVjA==?=");
    TEST_ASSERT(v && strcmp(v, "Hello, \xe4\xb8\x96\xe7\x95\x8c") == 0);
    free(v);
    v = cmcp_http_header_decode("=?base64?IHBhZGRlZCA=?=");
    TEST_ASSERT(v && strcmp(v, " padded ") == 0);
    free(v);
    TEST_ASSERT(cmcp_http_header_decode("=?base64?abc?=") == NULL);    /* len */
    TEST_ASSERT(cmcp_http_header_decode("=?base64?AA==?=") == NULL);   /* NUL */
    TEST_ASSERT(cmcp_http_header_decode("=?base64?a=bc?=") == NULL);   /* pad */
}

int main(void) {
    TEST_RUN(test_header_decode_unit);
    TEST_RUN(test_modern_call_needs_no_session);
    TEST_RUN(test_missing_or_mismatched_headers_are_32020);
    TEST_RUN(test_base64_sentinel_name_is_decoded);
    TEST_RUN(test_unsupported_version_is_400_32022);
    TEST_RUN(test_unknown_method_is_404);
    TEST_RUN(test_missing_capability_is_400_32021);
    TEST_RUN(test_input_required_is_200);
    TEST_RUN(test_progress_streams_on_the_request);
    TEST_RUN(test_legacy_session_beside_modern);
    TEST_DONE();
}
