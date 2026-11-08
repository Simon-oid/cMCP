/* test_client_modern — the dual-era client (MCP 2026-07-28, Phase 6).
 *
 * The cMCP client against the cMCP server over a pipe pair:
 *   - MODERN: server/discover instead of initialize, then requests that
 *     carry their own _meta;
 *   - multi round-trip requests driven by the client: an input_required
 *     tool call is answered with the host's elicitation handler and
 *     retried, with the server's requestState echoed;
 *   - logLevel sent per request instead of logging/setLevel;
 *   - AUTO: modern against a modern server, initialize against a
 *     legacy-only one (a small fake server that rejects server/discover).
 */

/* pipe(2) — POSIX. */
#define _POSIX_C_SOURCE 200809L

#include "test.h"
#include "cmcp.h"
#include "cmcp_client.h"
#include "cmcp_json.h"
#include "cmcp_server.h"
#include "cmcp_transport.h"
#include "wire_rig.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <pthread.h>
#include <sys/socket.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ====================================================================== */
/* cMCP server under test                                                  */
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

/* Asks for a name; round one also leaves a state the reply reports. */
static int ask(const cmcp_json_t *args, void *ud, cmcp_handler_ctx_t *hctx,
               cmcp_json_t **out, int *is_error) {
    (void)args; (void)ud;
    cmcp_json_t *ans = NULL;
    int rc = cmcp_handler_input(hctx, "name", "elicitation/create",
                                cmcp_json_parse_cstr("{\"message\":\"Name?\"}"),
                                &ans);
    if (rc == CMCP_EINPUT_REQUIRED) {
        cmcp_handler_set_request_state(hctx, "asked");
        return rc;
    }
    if (rc != CMCP_OK) return rc;
    const cmcp_json_t *n = cmcp_json_object_get(
        cmcp_json_object_get(ans, "content"), "name");
    const char *st = cmcp_handler_request_state(hctx);
    char text[128];
    snprintf(text, sizeof text, "hi %s (%s)",
             n && n->type == CMCP_JSON_STRING ? cmcp_json_string(n) : "?",
             st ? st : "legacy");
    cmcp_json_free(ans);
    *out = cmcp_tool_text_content(text);
    *is_error = 0;
    return CMCP_OK;
}

static int chatty(const cmcp_json_t *args, void *ud, cmcp_handler_ctx_t *hctx,
                  cmcp_json_t **out, int *is_error) {
    (void)args; (void)ud;
    cmcp_handler_log(hctx, CMCP_LOG_LEVEL_INFO, "chatty",
                     cmcp_json_parse_cstr("{\"m\":\"working\"}"));
    *out = cmcp_tool_text_content("done");
    *is_error = 0;
    return CMCP_OK;
}

static int where(const cmcp_json_t *args, void *ud, cmcp_handler_ctx_t *hctx,
                 cmcp_json_t **out, int *is_error) {
    (void)ud; (void)hctx;
    const cmcp_json_t *r = cmcp_json_object_get(args, "region");
    char text[96];
    snprintf(text, sizeof text, "region=%s",
             r && r->type == CMCP_JSON_STRING ? cmcp_json_string(r) : "?");
    *out = cmcp_tool_text_content(text);
    *is_error = 0;
    return CMCP_OK;
}

static int setup(cmcp_server_t *srv) {
    cmcp_server_capabilities_t caps = {.logging = 1};
    cmcp_server_set_capabilities(srv, &caps);
    const cmcp_tool_t tools[] = {
        {.name = "echo", .description = "d", .handler = echo,
         .input_schema = "{\"type\":\"object\"}"},
        {.name = "ask", .description = "d", .handler = ask,
         .input_schema = "{\"type\":\"object\"}"},
        {.name = "chatty", .description = "d", .handler = chatty,
         .input_schema = "{\"type\":\"object\"}"},
        {.name = "where", .description = "d", .handler = where,
         .input_schema = "{\"type\":\"object\",\"properties\":{\"region\":"
                         "{\"type\":\"string\",\"x-mcp-header\":\"Region\"}}}"},
    };
    for (size_t i = 0; i < sizeof tools / sizeof tools[0]; i++)
        if (cmcp_server_add_tool(srv, &tools[i]) != CMCP_OK) return -1;
    return 0;
}

/* ====================================================================== */
/* Client side                                                             */
/* ====================================================================== */

static atomic_int g_elicits;

static int on_elicit(const cmcp_json_t *params, void *ud, cmcp_json_t **out) {
    (void)params; (void)ud;
    atomic_fetch_add(&g_elicits, 1);
    *out = cmcp_elicitation_result("accept",
                                   cmcp_json_parse_cstr("{\"name\":\"Ada\"}"));
    return *out ? CMCP_OK : CMCP_ENOMEM;
}

static atomic_int g_log_lines;

static void on_notification(const char *method, const cmcp_json_t *params,
                            void *ud) {
    (void)params; (void)ud;
    if (strcmp(method, "notifications/message") == 0)
        atomic_fetch_add(&g_log_lines, 1);
}

static cmcp_client_t *new_client(cmcp_client_mode_t mode, int elicitation) {
    cmcp_client_t *c = cmcp_client_new("modern-client", "0.1");
    if (!c) return NULL;
    cmcp_client_capabilities_t caps = {.elicitation = elicitation};
    cmcp_client_set_capabilities(c, &caps);
    if (elicitation) cmcp_client_set_elicitation_handler(c, on_elicit, NULL);
    cmcp_client_set_notification_handler(c, on_notification, NULL);
    cmcp_client_set_mode(c, mode);
    return c;
}

/* Text of a tool call (owned), or NULL on any error. */
static char *call_text(cmcp_client_t *c, const char *tool, const char *args) {
    char *text = NULL;
    cmcp_rpc_error_t *err = NULL;
    int rc = cmcp_client_tool_call_text(c, tool, cmcp_json_parse_cstr(args),
                                        &text, &err);
    cmcp_rpc_error_free(err);
    if (rc != CMCP_OK) { free(text); return NULL; }
    return text;
}

/* ====================================================================== */
/* A legacy-only server: answers initialize, refuses server/discover       */
/* ====================================================================== */

typedef struct {
    cmcp_transport_t *t;
    pthread_t         th;
} fake_t;

static void reply(cmcp_transport_t *t, const cmcp_json_t *id, const char *body) {
    char *idt = cmcp_json_emit(id);
    char frame[1024];
    snprintf(frame, sizeof frame, "{\"jsonrpc\":\"2.0\",\"id\":%s,%s}",
             idt ? idt : "null", body);
    free(idt);
    cmcp_transport_write(t, frame, strlen(frame));
}

static void *fake_main(void *arg) {
    fake_t *f = (fake_t *)arg;
    for (;;) {
        char *in = NULL; size_t n = 0;
        if (cmcp_transport_read(f->t, &in, &n) != CMCP_OK) break;
        cmcp_json_t *m = cmcp_json_parse(in, n);
        free(in);
        const cmcp_json_t *id = get(m, "id");
        const cmcp_json_t *method = get(m, "method");
        const char *mth = method && method->type == CMCP_JSON_STRING
                              ? cmcp_json_string(method) : "";
        if (!id) { cmcp_json_free(m); continue; }        /* notification */
        if (strcmp(mth, "initialize") == 0) {
            reply(f->t, id, "\"result\":{\"protocolVersion\":\"2025-11-25\","
                            "\"capabilities\":{\"tools\":{}},"
                            "\"serverInfo\":{\"name\":\"old\",\"version\":\"1\"}}");
        } else if (strcmp(mth, "tools/list") == 0) {
            reply(f->t, id, "\"result\":{\"tools\":[]}");
        } else {
            reply(f->t, id, "\"error\":{\"code\":-32601,\"message\":\"Method not found\"}");
        }
        cmcp_json_free(m);
    }
    return NULL;
}

/* Start the fake on one end of a pipe pair; *client_t gets the other. */
static int fake_start(fake_t *f, cmcp_transport_t **client_t) {
    int c2s[2], s2c[2];
    if (pipe(c2s) != 0) return -1;
    if (pipe(s2c) != 0) { close(c2s[0]); close(c2s[1]); return -1; }
    *client_t = cmcp_transport_stdio_new_fds(s2c[0], c2s[1]);
    f->t = cmcp_transport_stdio_new_fds(c2s[0], s2c[1]);
    if (!*client_t || !f->t) return -1;
    return pthread_create(&f->th, NULL, fake_main, f) == 0 ? 0 : -1;
}

static void fake_stop(fake_t *f, cmcp_transport_t *client_t) {
    cmcp_transport_close(client_t);
    pthread_join(f->th, NULL);
    cmcp_transport_close(f->t);
}

/* ====================================================================== */
/* Tests                                                                   */
/* ====================================================================== */

/* The client borrows the rig's transport; rig_stop closes it, after the
 * client (and its reader thread) is gone. */
static void test_modern_connects_without_handshake(void) {
    rig_t r; TEST_ASSERT(rig_start(&r, "srv", "9.9", setup) == 0);
    cmcp_client_t *c = new_client(CMCP_CLIENT_MODERN, 0);
    TEST_ASSERT(cmcp_client_handshake(c, r.client_t) == CMCP_OK);
    TEST_ASSERT(cmcp_client_is_modern(c));
    TEST_ASSERT(strcmp(cmcp_client_server_protocol(c), "2026-07-28") == 0);
    TEST_ASSERT(cmcp_client_server_name(c) &&
                strcmp(cmcp_client_server_name(c), "srv") == 0);
    TEST_ASSERT(cmcp_client_server_caps(c)->logging == 1);
    /* Too late to change era once connected. */
    TEST_ASSERT(cmcp_client_set_mode(c, CMCP_CLIENT_LEGACY) == CMCP_EPROTOCOL);

    char *t = call_text(c, "echo", "{\"text\":\"over modern\"}");
    TEST_ASSERT(t && strcmp(t, "over modern") == 0);
    free(t);
    cmcp_client_free(c);
    rig_stop(&r);
}

static void test_mrtr_is_driven_by_the_client(void) {
    rig_t r; TEST_ASSERT(rig_start(&r, "srv", "9.9", setup) == 0);
    atomic_store(&g_elicits, 0);
    cmcp_client_t *c = new_client(CMCP_CLIENT_MODERN, 1);
    TEST_ASSERT(cmcp_client_handshake(c, r.client_t) == CMCP_OK);
    /* Round one: input_required; the client asks the host, retries
     * with inputResponses + requestState, and returns the final text. */
    char *t = call_text(c, "ask", "{}");
    TEST_ASSERT(t && strcmp(t, "hi Ada (asked)") == 0);
    TEST_ASSERT(atomic_load(&g_elicits) == 1);
    free(t);
    cmcp_client_free(c);
    rig_stop(&r);
}

/* The same handler on a legacy connection: a real elicitation/create. */
static void test_same_server_tool_on_legacy_client(void) {
    rig_t r; TEST_ASSERT(rig_start(&r, "srv", "9.9", setup) == 0);
    atomic_store(&g_elicits, 0);
    cmcp_client_t *c = new_client(CMCP_CLIENT_LEGACY, 1);
    TEST_ASSERT(cmcp_client_handshake(c, r.client_t) == CMCP_OK);
    TEST_ASSERT(!cmcp_client_is_modern(c));
    char *t = call_text(c, "ask", "{}");
    TEST_ASSERT(t && strcmp(t, "hi Ada (legacy)") == 0);
    TEST_ASSERT(atomic_load(&g_elicits) == 1);
    free(t);
    cmcp_client_free(c);
    rig_stop(&r);
}

/* Declared elicitation but registered no handler: the client cannot
 * answer, so the call fails instead of looping. */
static void test_mrtr_without_handler_fails(void) {
    rig_t r; TEST_ASSERT(rig_start(&r, "srv", "9.9", setup) == 0);
    cmcp_client_t *c = cmcp_client_new("modern-client", "0.1");
    cmcp_client_capabilities_t caps = {.elicitation = 1};
    cmcp_client_set_capabilities(c, &caps);
    cmcp_client_set_mode(c, CMCP_CLIENT_MODERN);
    TEST_ASSERT(cmcp_client_handshake(c, r.client_t) == CMCP_OK);
    cmcp_tool_result_t res = cmcp_client_tool_call(c, "ask",
                                                   cmcp_json_new_object());
    TEST_ASSERT(res.outcome == CMCP_TOOL_ERR_PROTOCOL);
    cmcp_tool_result_clear(&res);
    cmcp_client_free(c);
    rig_stop(&r);
}

/* logLevel rides in each request: set_log_level sends nothing, and
 * the next call's log line arrives. Without it, none does. */
static void test_log_level_per_request(void) {
    rig_t r; TEST_ASSERT(rig_start(&r, "srv", "9.9", setup) == 0);
    atomic_store(&g_log_lines, 0);
    cmcp_client_t *c = new_client(CMCP_CLIENT_MODERN, 0);
    TEST_ASSERT(cmcp_client_handshake(c, r.client_t) == CMCP_OK);
    char *t = call_text(c, "chatty", "{}");
    free(t);
    TEST_ASSERT(atomic_load(&g_log_lines) == 0);
    TEST_ASSERT(cmcp_client_set_log_level(c, CMCP_LOG_LEVEL_DEBUG) == CMCP_OK);
    t = call_text(c, "chatty", "{}");
    TEST_ASSERT(t && strcmp(t, "done") == 0);
    free(t);
    /* Notifications precede the response on the wire, and the reader
     * delivers in order, so the line has been counted by now. */
    TEST_ASSERT(atomic_load(&g_log_lines) == 1);
    TEST_ASSERT(cmcp_client_notify_roots_changed(c) == CMCP_EUNSUPPORTED);
    cmcp_client_free(c);
    rig_stop(&r);
}

static void test_auto_picks_modern_on_a_modern_server(void) {
    rig_t r; TEST_ASSERT(rig_start(&r, "srv", "9.9", setup) == 0);
    cmcp_client_t *c = new_client(CMCP_CLIENT_AUTO, 0);
    TEST_ASSERT(cmcp_client_handshake(c, r.client_t) == CMCP_OK);
    TEST_ASSERT(cmcp_client_is_modern(c));
    cmcp_client_free(c);
    rig_stop(&r);
}

static void test_auto_falls_back_on_a_legacy_server(void) {
    fake_t f; cmcp_transport_t *ct = NULL;
    TEST_ASSERT(fake_start(&f, &ct) == 0);
    cmcp_client_t *c = new_client(CMCP_CLIENT_AUTO, 0);
    TEST_ASSERT(cmcp_client_handshake(c, ct) == CMCP_OK);
    TEST_ASSERT(!cmcp_client_is_modern(c));
    TEST_ASSERT(strcmp(cmcp_client_server_protocol(c), "2025-11-25") == 0);
    TEST_ASSERT(cmcp_client_server_name(c) &&
                strcmp(cmcp_client_server_name(c), "old") == 0);
    cmcp_session_tool_t *tools = NULL; size_t n = 99;
    TEST_ASSERT(cmcp_client_tools_list(c, &tools, &n) == CMCP_OK && n == 0);
    cmcp_session_tools_free(tools, n);
    cmcp_client_free(c);
    fake_stop(&f, ct);
}

static void test_modern_refuses_a_legacy_server(void) {
    fake_t f; cmcp_transport_t *ct = NULL;
    TEST_ASSERT(fake_start(&f, &ct) == 0);
    cmcp_client_t *c = new_client(CMCP_CLIENT_MODERN, 0);
    TEST_ASSERT(cmcp_client_handshake(c, ct) != CMCP_OK);
    TEST_ASSERT(!cmcp_client_is_modern(c));
    cmcp_client_free(c);
    fake_stop(&f, ct);
}

/* ====================================================================== */
/* Over Streamable HTTP                                                    */
/* ====================================================================== */

typedef struct {
    cmcp_transport_t *t;
    cmcp_server_t    *s;
    pthread_t         th;
    char              url[64];
} http_rig_t;

static void *http_server_main(void *arg) {
    http_rig_t *r = (http_rig_t *)arg;
    cmcp_server_run(r->s, r->t);
    return NULL;
}

static unsigned short free_port(void) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in sa;
    memset(&sa, 0, sizeof sa);
    sa.sin_family = AF_INET;
    sa.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    socklen_t sl = sizeof sa;
    unsigned short port = 0;
    if (fd >= 0 && bind(fd, (struct sockaddr *)&sa, sizeof sa) == 0 &&
        getsockname(fd, (struct sockaddr *)&sa, &sl) == 0)
        port = ntohs(sa.sin_port);
    if (fd >= 0) close(fd);
    return port;
}

static int http_rig_start(http_rig_t *r) {
    memset(r, 0, sizeof *r);
    unsigned short port = free_port();
    r->t = port ? cmcp_transport_http_listen("127.0.0.1", port) : NULL;
    r->s = cmcp_server_new("http-srv", "2.0");
    if (!r->t || !r->s || setup(r->s) != 0) return -1;
    snprintf(r->url, sizeof r->url, "http://127.0.0.1:%u/mcp", port);
    return pthread_create(&r->th, NULL, http_server_main, r) == 0 ? 0 : -1;
}

static void http_rig_stop(http_rig_t *r) {
    cmcp_transport_wake(r->t);
    pthread_join(r->th, NULL);
    cmcp_transport_close(r->t);
    cmcp_server_free(r->s);
}

static void test_modern_over_http(void) {
    http_rig_t r; TEST_ASSERT(http_rig_start(&r) == 0);
    cmcp_transport_t *ct = cmcp_transport_http_connect(r.url);
    TEST_ASSERT(ct != NULL);
    atomic_store(&g_elicits, 0);
    atomic_store(&g_log_lines, 0);
    cmcp_client_t *c = new_client(CMCP_CLIENT_AUTO, 1);
    /* No session: discover is a plain POST with the Mcp-* headers. */
    TEST_ASSERT(cmcp_client_handshake(c, ct) == CMCP_OK);
    TEST_ASSERT(cmcp_client_is_modern(c));
    TEST_ASSERT(strcmp(cmcp_client_server_name(c), "http-srv") == 0);

    /* tools/list teaches the transport where's x-mcp-header; the server
     * rejects the call (-32020) unless Mcp-Param-Region matches. */
    cmcp_session_tool_t *tools = NULL; size_t n = 0;
    TEST_ASSERT(cmcp_client_tools_list(c, &tools, &n) == CMCP_OK && n == 4);
    cmcp_session_tools_free(tools, n);
    char *t = call_text(c, "where", "{\"region\":\"us-west1\"}");
    TEST_ASSERT(t && strcmp(t, "region=us-west1") == 0);
    free(t);

    /* MRTR over HTTP: two independent POSTs. */
    t = call_text(c, "ask", "{}");
    TEST_ASSERT(t && strcmp(t, "hi Ada (asked)") == 0);
    TEST_ASSERT(atomic_load(&g_elicits) == 1);
    free(t);

    /* The log line comes back on the POST's own SSE response. */
    TEST_ASSERT(cmcp_client_set_log_level(c, CMCP_LOG_LEVEL_DEBUG) == CMCP_OK);
    t = call_text(c, "chatty", "{}");
    TEST_ASSERT(t && strcmp(t, "done") == 0);
    free(t);
    TEST_ASSERT(atomic_load(&g_log_lines) == 1);

    cmcp_client_free(c);
    cmcp_transport_close(ct);
    http_rig_stop(&r);
}

int main(void) {
    TEST_RUN(test_modern_connects_without_handshake);
    TEST_RUN(test_mrtr_is_driven_by_the_client);
    TEST_RUN(test_same_server_tool_on_legacy_client);
    TEST_RUN(test_mrtr_without_handler_fails);
    TEST_RUN(test_log_level_per_request);
    TEST_RUN(test_auto_picks_modern_on_a_modern_server);
    TEST_RUN(test_auto_falls_back_on_a_legacy_server);
    TEST_RUN(test_modern_refuses_a_legacy_server);
    TEST_RUN(test_modern_over_http);
    TEST_DONE();
}
