/* test_modern — the 2026-07-28 ("modern", stateless) server path.
 *
 * A dual-era server picks the era per request: a request whose
 * `params._meta` carries `io.modelcontextprotocol/protocolVersion` is
 * served statelessly, with no `initialize` handshake; `initialize`
 * selects the legacy 2025-11-25 semantics. These tests drive the
 * server with raw wire JSON over a pipe pair so they pin the exact
 * frames, not a client library's view of them. */

/* pipe(2) — POSIX. */
#define _POSIX_C_SOURCE 200809L

#include "test.h"
#include "cmcp.h"
#include "cmcp_json.h"
#include "cmcp_server.h"
#include "cmcp_transport.h"
#include "cmcp_types.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define MODERN "2026-07-28"

/* Standard modern _meta block, spliced into requests below. */
#define META_OK                                                      \
    "\"_meta\":{"                                                    \
    "\"io.modelcontextprotocol/protocolVersion\":\"" MODERN "\","    \
    "\"io.modelcontextprotocol/clientCapabilities\":{},"             \
    "\"io.modelcontextprotocol/clientInfo\":"                        \
    "{\"name\":\"test-modern\",\"version\":\"0.0.1\"}}"

/* ====================================================================== */
/* Scaffolding                                                             */
/* ====================================================================== */

typedef struct {
    cmcp_transport_t *client_t;
    cmcp_transport_t *server_t;
    cmcp_server_t    *srv;
    pthread_t         th;
    int               rc;
} rig_t;

static int echo_handler(const cmcp_json_t *arguments, void *userdata,
                        cmcp_handler_ctx_t *hctx, cmcp_json_t **out_content,
                        int *out_is_error) {
    (void)userdata; (void)hctx;
    const cmcp_json_t *text = cmcp_json_object_get(arguments, "text");
    /* cmcp_tool_text_content already returns the content *array*. */
    *out_content = cmcp_tool_text_content(
        text && text->type == CMCP_JSON_STRING ? cmcp_json_string(text) : "");
    *out_is_error = 0;
    return CMCP_OK;
}

static void *server_thread(void *arg) {
    rig_t *r = (rig_t *)arg;
    r->rc = cmcp_server_run(r->srv, r->server_t);
    return NULL;
}

static int rig_start(rig_t *r) {
    memset(r, 0, sizeof(*r));
    int c2s[2], s2c[2];
    if (pipe(c2s) != 0) return -1;
    if (pipe(s2c) != 0) { close(c2s[0]); close(c2s[1]); return -1; }
    r->client_t = cmcp_transport_stdio_new_fds(s2c[0], c2s[1]);
    r->server_t = cmcp_transport_stdio_new_fds(c2s[0], s2c[1]);
    r->srv = cmcp_server_new("modern-server", "1.2.3");
    if (!r->client_t || !r->server_t || !r->srv) return -1;

    cmcp_tool_t echo = {
        .name = "echo",
        .description = "Echo the text back",
        .input_schema = "{\"type\":\"object\",\"properties\":"
                        "{\"text\":{\"type\":\"string\"}},"
                        "\"required\":[\"text\"]}",
        .handler = echo_handler,
    };
    if (cmcp_server_add_tool(r->srv, &echo) != CMCP_OK) return -1;
    return pthread_create(&r->th, NULL, server_thread, r) == 0 ? 0 : -1;
}

static void rig_stop(rig_t *r) {
    cmcp_transport_close(r->client_t);   /* EOF ends the server loop */
    pthread_join(r->th, NULL);
    cmcp_transport_close(r->server_t);
    cmcp_server_free(r->srv);
}

/* Write one raw frame, read one frame back, parse it as JSON.
 * Returns an owned value, or NULL on any failure. */
static cmcp_json_t *exchange(rig_t *r, const char *frame) {
    if (cmcp_transport_write(r->client_t, frame, strlen(frame)) != CMCP_OK)
        return NULL;
    char *in = NULL; size_t n = 0;
    if (cmcp_transport_read(r->client_t, &in, &n) != CMCP_OK) return NULL;
    cmcp_json_t *v = cmcp_json_parse(in, n);
    free(in);
    return v;
}

/* Small JSON path helpers: NULL-safe, return NULL on any miss. */
static const cmcp_json_t *get(const cmcp_json_t *o, const char *key) {
    return (o && o->type == CMCP_JSON_OBJECT) ? cmcp_json_object_get(o, key)
                                              : NULL;
}

static int is_str(const cmcp_json_t *v, const char *want) {
    return v && v->type == CMCP_JSON_STRING && strcmp(cmcp_json_string(v), want) == 0;
}

static long long int_of(const cmcp_json_t *v) {
    return (v && v->type == CMCP_JSON_INT) ? cmcp_json_int(v) : -999999;
}

static int array_has_str(const cmcp_json_t *arr, const char *want) {
    if (!arr || arr->type != CMCP_JSON_ARRAY) return 0;
    for (size_t i = 0; i < cmcp_json_array_len(arr); i++) {
        if (is_str(cmcp_json_array_at(arr, i), want)) return 1;
    }
    return 0;
}

/* Every modern "complete" result carries resultType and serverInfo. */
static void assert_modern_complete(const cmcp_json_t *result) {
    TEST_ASSERT(is_str(get(result, "resultType"), "complete"));
    const cmcp_json_t *si =
        get(get(result, "_meta"), "io.modelcontextprotocol/serverInfo");
    TEST_ASSERT(is_str(get(si, "name"), "modern-server"));
    TEST_ASSERT(is_str(get(si, "version"), "1.2.3"));
}

/* Cacheable results (lists, resources/read, discover) carry hints. */
static void assert_cache_hints(const cmcp_json_t *result) {
    TEST_ASSERT(int_of(get(result, "ttlMs")) >= 0);
    const cmcp_json_t *scope = get(result, "cacheScope");
    TEST_ASSERT(is_str(scope, "public") || is_str(scope, "private"));
}

/* ====================================================================== */
/* Tests                                                                   */
/* ====================================================================== */

static void test_discover_reports_both_eras(void) {
    rig_t r; TEST_ASSERT(rig_start(&r) == 0);

    cmcp_json_t *v = exchange(&r,
        "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"server/discover\","
        "\"params\":{" META_OK "}}");
    const cmcp_json_t *res = get(v, "result");
    TEST_ASSERT(res != NULL);
    assert_modern_complete(res);
    assert_cache_hints(res);
    const cmcp_json_t *sv = get(res, "supportedVersions");
    TEST_ASSERT(array_has_str(sv, MODERN));
    TEST_ASSERT(array_has_str(sv, CMCP_PROTOCOL_VERSION));
    const cmcp_json_t *caps = get(res, "capabilities");
    TEST_ASSERT(caps && caps->type == CMCP_JSON_OBJECT);
    TEST_ASSERT(get(caps, "tools") != NULL);

    cmcp_json_free(v);
    rig_stop(&r);
}

static void test_missing_protocol_version_is_invalid_params(void) {
    rig_t r; TEST_ASSERT(rig_start(&r) == 0);

    cmcp_json_t *v = exchange(&r,
        "{\"jsonrpc\":\"2.0\",\"id\":2,\"method\":\"server/discover\","
        "\"params\":{\"_meta\":{"
        "\"io.modelcontextprotocol/clientCapabilities\":{}}}}");
    TEST_ASSERT(int_of(get(get(v, "error"), "code")) == CMCP_RPC_INVALID_PARAMS);

    cmcp_json_free(v);
    rig_stop(&r);
}

static void test_missing_client_capabilities_is_invalid_params(void) {
    rig_t r; TEST_ASSERT(rig_start(&r) == 0);

    cmcp_json_t *v = exchange(&r,
        "{\"jsonrpc\":\"2.0\",\"id\":3,\"method\":\"tools/list\","
        "\"params\":{\"_meta\":{"
        "\"io.modelcontextprotocol/protocolVersion\":\"" MODERN "\"}}}");
    TEST_ASSERT(int_of(get(get(v, "error"), "code")) == CMCP_RPC_INVALID_PARAMS);

    cmcp_json_free(v);
    rig_stop(&r);
}

static void test_unsupported_version_lists_supported(void) {
    rig_t r; TEST_ASSERT(rig_start(&r) == 0);

    cmcp_json_t *v = exchange(&r,
        "{\"jsonrpc\":\"2.0\",\"id\":4,\"method\":\"tools/list\","
        "\"params\":{\"_meta\":{"
        "\"io.modelcontextprotocol/protocolVersion\":\"1900-01-01\","
        "\"io.modelcontextprotocol/clientCapabilities\":{}}}}");
    const cmcp_json_t *err = get(v, "error");
    TEST_ASSERT(int_of(get(err, "code")) ==
                CMCP_RPC_UNSUPPORTED_PROTOCOL_VERSION);
    const cmcp_json_t *data = get(err, "data");
    TEST_ASSERT(array_has_str(get(data, "supported"), MODERN));
    TEST_ASSERT(is_str(get(data, "requested"), "1900-01-01"));

    cmcp_json_free(v);
    rig_stop(&r);
}

static void test_tools_list_without_handshake(void) {
    rig_t r; TEST_ASSERT(rig_start(&r) == 0);

    cmcp_json_t *v = exchange(&r,
        "{\"jsonrpc\":\"2.0\",\"id\":5,\"method\":\"tools/list\","
        "\"params\":{" META_OK "}}");
    const cmcp_json_t *res = get(v, "result");
    TEST_ASSERT(res != NULL);
    assert_modern_complete(res);
    assert_cache_hints(res);
    const cmcp_json_t *tools = get(res, "tools");
    TEST_ASSERT(tools && tools->type == CMCP_JSON_ARRAY &&
                cmcp_json_array_len(tools) == 1);
    TEST_ASSERT(is_str(get(cmcp_json_array_at(tools, 0), "name"), "echo"));

    cmcp_json_free(v);
    rig_stop(&r);
}

static void test_tools_call_without_handshake(void) {
    rig_t r; TEST_ASSERT(rig_start(&r) == 0);

    cmcp_json_t *v = exchange(&r,
        "{\"jsonrpc\":\"2.0\",\"id\":6,\"method\":\"tools/call\","
        "\"params\":{\"name\":\"echo\",\"arguments\":{\"text\":\"hi\"},"
        META_OK "}}");
    const cmcp_json_t *res = get(v, "result");
    TEST_ASSERT(res != NULL);
    assert_modern_complete(res);
    /* tools/call is not cacheable: no hints. */
    TEST_ASSERT(get(res, "ttlMs") == NULL);
    const cmcp_json_t *item = cmcp_json_array_at(get(res, "content"), 0);
    TEST_ASSERT(is_str(get(item, "text"), "hi"));

    cmcp_json_free(v);
    rig_stop(&r);
}

static void test_removed_methods_are_unknown_on_modern_path(void) {
    rig_t r; TEST_ASSERT(rig_start(&r) == 0);

    static const char *removed[] = {
        "ping", "logging/setLevel", "resources/subscribe",
        "resources/unsubscribe",
    };
    for (size_t i = 0; i < sizeof(removed) / sizeof(removed[0]); i++) {
        char frame[512];
        snprintf(frame, sizeof frame,
                 "{\"jsonrpc\":\"2.0\",\"id\":%zu,\"method\":\"%s\","
                 "\"params\":{" META_OK "}}", 10 + i, removed[i]);
        cmcp_json_t *v = exchange(&r, frame);
        TEST_ASSERT(int_of(get(get(v, "error"), "code")) ==
                    CMCP_RPC_METHOD_NOT_FOUND);
        cmcp_json_free(v);
    }
    rig_stop(&r);
}

/* Legacy semantics are untouched: initialize, then a request without
 * modern _meta gets a 2025-11-25 result with no resultType. */
static void test_legacy_path_unchanged(void) {
    rig_t r; TEST_ASSERT(rig_start(&r) == 0);

    cmcp_json_t *init = exchange(&r,
        "{\"jsonrpc\":\"2.0\",\"id\":20,\"method\":\"initialize\","
        "\"params\":{\"protocolVersion\":\"" CMCP_PROTOCOL_VERSION "\","
        "\"capabilities\":{},\"clientInfo\":{\"name\":\"legacy\","
        "\"version\":\"0\"}}}");
    TEST_ASSERT(is_str(get(get(init, "result"), "protocolVersion"),
                       CMCP_PROTOCOL_VERSION));
    cmcp_json_free(init);

    const char *initialized =
        "{\"jsonrpc\":\"2.0\",\"method\":\"notifications/initialized\"}";
    TEST_ASSERT(cmcp_transport_write(r.client_t, initialized,
                                     strlen(initialized)) == CMCP_OK);

    cmcp_json_t *v = exchange(&r,
        "{\"jsonrpc\":\"2.0\",\"id\":21,\"method\":\"tools/list\"}");
    const cmcp_json_t *res = get(v, "result");
    TEST_ASSERT(res != NULL);
    TEST_ASSERT(get(res, "resultType") == NULL);
    TEST_ASSERT(get(res, "ttlMs") == NULL);
    cmcp_json_free(v);
    rig_stop(&r);
}

/* A legacy request that arrives before initialize is still refused:
 * the modern path only opens for requests that carry modern _meta. */
static void test_legacy_request_before_initialize_still_refused(void) {
    rig_t r; TEST_ASSERT(rig_start(&r) == 0);

    cmcp_json_t *v = exchange(&r,
        "{\"jsonrpc\":\"2.0\",\"id\":30,\"method\":\"tools/list\"}");
    TEST_ASSERT(int_of(get(get(v, "error"), "code")) ==
                CMCP_RPC_INVALID_REQUEST);
    cmcp_json_free(v);
    rig_stop(&r);
}

/* Both eras on one process: a legacy session is live and a modern
 * request is still served statelessly next to it. */
static void test_both_eras_on_one_process(void) {
    rig_t r; TEST_ASSERT(rig_start(&r) == 0);

    cmcp_json_t *init = exchange(&r,
        "{\"jsonrpc\":\"2.0\",\"id\":40,\"method\":\"initialize\","
        "\"params\":{\"protocolVersion\":\"" CMCP_PROTOCOL_VERSION "\","
        "\"capabilities\":{},\"clientInfo\":{\"name\":\"legacy\","
        "\"version\":\"0\"}}}");
    TEST_ASSERT(get(init, "result") != NULL);
    cmcp_json_free(init);

    cmcp_json_t *v = exchange(&r,
        "{\"jsonrpc\":\"2.0\",\"id\":41,\"method\":\"tools/list\","
        "\"params\":{" META_OK "}}");
    assert_modern_complete(get(v, "result"));
    cmcp_json_free(v);
    rig_stop(&r);
}

int main(void) {
    TEST_RUN(test_discover_reports_both_eras);
    TEST_RUN(test_missing_protocol_version_is_invalid_params);
    TEST_RUN(test_missing_client_capabilities_is_invalid_params);
    TEST_RUN(test_unsupported_version_lists_supported);
    TEST_RUN(test_tools_list_without_handshake);
    TEST_RUN(test_tools_call_without_handshake);
    TEST_RUN(test_removed_methods_are_unknown_on_modern_path);
    TEST_RUN(test_legacy_path_unchanged);
    TEST_RUN(test_legacy_request_before_initialize_still_refused);
    TEST_RUN(test_both_eras_on_one_process);
    TEST_DONE();
}
