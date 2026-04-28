/* pipe(2) — POSIX. */
#define _POSIX_C_SOURCE 200809L

#include "test.h"
#include "cmcp.h"
#include "cmcp_client.h"
#include "cmcp_json.h"
#include "cmcp_server.h"
#include "cmcp_transport.h"
#include "cmcp_types.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* ====================================================================== */
/* Pipe-pair scaffolding (mirrors test_lifecycle).                         */
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
/* Sample handlers                                                         */
/* ====================================================================== */

/* `echo` tool — returns its `text` argument back as text content. */
static int echo_handler(const cmcp_json_t *args, void *userdata,
                         cmcp_json_t **out_content, int *out_is_error) {
    (void)userdata;
    *out_is_error = 0;
    const cmcp_json_t *t = args ? cmcp_json_object_get(args, "text") : NULL;
    const char *s = (t && t->type == CMCP_JSON_STRING) ? t->str.s : "";
    *out_content = cmcp_tool_text_content(s);
    return CMCP_OK;
}

/* `add` tool — returns a + b as a single text content item. Counts how
 * many times it's been called via userdata. */
typedef struct { int calls; } add_ctx_t;

static int add_handler(const cmcp_json_t *args, void *userdata,
                        cmcp_json_t **out_content, int *out_is_error) {
    add_ctx_t *ctx = (add_ctx_t *)userdata;
    ctx->calls++;
    *out_is_error = 0;
    const cmcp_json_t *a = args ? cmcp_json_object_get(args, "a") : NULL;
    const cmcp_json_t *b = args ? cmcp_json_object_get(args, "b") : NULL;
    long long aa = (a && a->type == CMCP_JSON_INT) ? a->i : 0;
    long long bb = (b && b->type == CMCP_JSON_INT) ? b->i : 0;
    char buf[64];
    snprintf(buf, sizeof buf, "%lld", aa + bb);
    *out_content = cmcp_tool_text_content(buf);
    return CMCP_OK;
}

/* `tool_error` — runs successfully but reports a tool-level error
 * (e.g. simulating "file not found"). */
static int tool_error_handler(const cmcp_json_t *args, void *userdata,
                               cmcp_json_t **out_content, int *out_is_error) {
    (void)args; (void)userdata;
    *out_is_error = 1;
    *out_content = cmcp_tool_text_content("file not found: foo.txt");
    return CMCP_OK;
}

/* `internal_error` — handler returns non-zero. Library should map this
 * to a JSON-RPC -32603 internal error. */
static int internal_error_handler(const cmcp_json_t *args, void *userdata,
                                   cmcp_json_t **out_content,
                                   int *out_is_error) {
    (void)args; (void)userdata; (void)out_content; (void)out_is_error;
    return CMCP_EHANDLER;
}

/* ====================================================================== */
/* Helpers                                                                 */
/* ====================================================================== */

static cmcp_json_t *call_args_object(const char *name, cmcp_json_t *arguments) {
    cmcp_json_t *p = cmcp_json_new_object();
    cmcp_json_object_set(p, "name", cmcp_json_new_string(name));
    if (arguments) cmcp_json_object_set(p, "arguments", arguments);
    return p;
}

/* ====================================================================== */
/* Tests                                                                   */
/* ====================================================================== */

static void test_register_and_list(void) {
    cmcp_server_t *srv = cmcp_server_new("test-server", "0.1.0");

    int rc = cmcp_server_add_tool(srv, &(cmcp_tool_t){
        .name = "echo",
        .description = "Echo input text back.",
        .input_schema = "{\"type\":\"object\",\"properties\":{"
                         "\"text\":{\"type\":\"string\"}},"
                         "\"required\":[\"text\"]}",
        .handler = echo_handler,
    });
    TEST_ASSERT(rc == CMCP_OK);

    add_ctx_t ctx = {0};
    rc = cmcp_server_add_tool(srv, &(cmcp_tool_t){
        .name = "add",
        .description = "Add two integers.",
        .input_schema = NULL,    /* allowed; library defaults to object */
        .handler = add_handler,
        .userdata = &ctx,
    });
    TEST_ASSERT(rc == CMCP_OK);

    /* Duplicate name → CMCP_EPROTOCOL. */
    rc = cmcp_server_add_tool(srv, &(cmcp_tool_t){
        .name = "echo",
        .handler = echo_handler,
    });
    TEST_ASSERT(rc == CMCP_EPROTOCOL);

    /* Missing handler → CMCP_EINVAL. */
    rc = cmcp_server_add_tool(srv, &(cmcp_tool_t){ .name = "no_handler" });
    TEST_ASSERT(rc == CMCP_EINVAL);

    /* Bad schema JSON → CMCP_EPARSE. */
    rc = cmcp_server_add_tool(srv, &(cmcp_tool_t){
        .name = "bad",
        .handler = echo_handler,
        .input_schema = "{not-json",
    });
    TEST_ASSERT(rc == CMCP_EPARSE);

    /* Schema that's not an object → CMCP_EPARSE. */
    rc = cmcp_server_add_tool(srv, &(cmcp_tool_t){
        .name = "bad2",
        .handler = echo_handler,
        .input_schema = "[1,2,3]",
    });
    TEST_ASSERT(rc == CMCP_EPARSE);

    cmcp_server_free(srv);
}

/* End-to-end: handshake + tools/list + verify the descriptor shape. */
static void test_tools_list_over_wire(void) {
    transport_pair_t p;
    TEST_ASSERT(make_pair(&p) == 0);

    cmcp_server_t *srv = cmcp_server_new("test-server", "0.1.0");
    cmcp_server_add_tool(srv, &(cmcp_tool_t){
        .name = "echo",
        .description = "Echo input text back.",
        .input_schema = "{\"type\":\"object\","
                         "\"properties\":{\"text\":{\"type\":\"string\"}},"
                         "\"required\":[\"text\"]}",
        .handler = echo_handler,
    });

    server_arg_t sa = { srv, p.server_t, 0 };
    pthread_t th;
    TEST_ASSERT(pthread_create(&th, NULL, server_thread, &sa) == 0);

    cmcp_client_t *cli = cmcp_client_new("test-client", "0.0.1");
    TEST_ASSERT(cmcp_client_handshake(cli, p.client_t) == CMCP_OK);

    /* Server should now advertise the `tools` capability. */
    const cmcp_server_capabilities_t *caps = cmcp_client_server_caps(cli);
    TEST_ASSERT(caps != NULL);
    /* tools_list_changed wasn't set, but `tools` capability key is
     * present — the client doesn't model "supports tools" as its own
     * bool right now; we'll re-check this in a separate test by
     * sending `tools/list` directly and reading the JSON. */

    cmcp_rpc_message_t resp;
    TEST_ASSERT(cmcp_client_request(cli, "tools/list", NULL, &resp) == CMCP_OK);
    TEST_ASSERT(resp.kind == CMCP_MSG_RESPONSE);
    TEST_ASSERT(resp.error == NULL);
    TEST_ASSERT(resp.result != NULL && resp.result->type == CMCP_JSON_OBJECT);

    const cmcp_json_t *arr = cmcp_json_object_get(resp.result, "tools");
    TEST_ASSERT(arr != NULL && arr->type == CMCP_JSON_ARRAY);
    TEST_ASSERT(cmcp_json_array_len(arr) == 1);

    const cmcp_json_t *t0 = cmcp_json_array_at(arr, 0);
    TEST_ASSERT(t0 != NULL && t0->type == CMCP_JSON_OBJECT);
    const cmcp_json_t *nm = cmcp_json_object_get(t0, "name");
    const cmcp_json_t *ds = cmcp_json_object_get(t0, "description");
    const cmcp_json_t *sc = cmcp_json_object_get(t0, "inputSchema");
    TEST_ASSERT(nm && nm->type == CMCP_JSON_STRING);
    TEST_ASSERT(strcmp(nm->str.s, "echo") == 0);
    TEST_ASSERT(ds && ds->type == CMCP_JSON_STRING);
    TEST_ASSERT(strcmp(ds->str.s, "Echo input text back.") == 0);
    TEST_ASSERT(sc && sc->type == CMCP_JSON_OBJECT);
    const cmcp_json_t *typ = cmcp_json_object_get(sc, "type");
    TEST_ASSERT(typ && strcmp(typ->str.s, "object") == 0);

    cmcp_rpc_message_clear(&resp);

    cmcp_client_free(cli);
    cmcp_transport_close(p.client_t);
    pthread_join(th, NULL);
    cmcp_server_free(srv);
    cmcp_transport_close(p.server_t);
}

static void test_tools_call_success(void) {
    transport_pair_t p;
    TEST_ASSERT(make_pair(&p) == 0);

    cmcp_server_t *srv = cmcp_server_new("test-server", "0.1.0");
    add_ctx_t ctx = {0};
    cmcp_server_add_tool(srv, &(cmcp_tool_t){
        .name = "add",
        .handler = add_handler,
        .userdata = &ctx,
    });

    server_arg_t sa = { srv, p.server_t, 0 };
    pthread_t th;
    pthread_create(&th, NULL, server_thread, &sa);

    cmcp_client_t *cli = cmcp_client_new("test-client", "0.0.1");
    TEST_ASSERT(cmcp_client_handshake(cli, p.client_t) == CMCP_OK);

    cmcp_json_t *args = cmcp_json_new_object();
    cmcp_json_object_set(args, "a", cmcp_json_new_int(2));
    cmcp_json_object_set(args, "b", cmcp_json_new_int(40));

    cmcp_rpc_message_t resp;
    TEST_ASSERT(cmcp_client_request(cli, "tools/call",
                                     call_args_object("add", args),
                                     &resp) == CMCP_OK);
    TEST_ASSERT(resp.error == NULL);
    TEST_ASSERT(resp.result && resp.result->type == CMCP_JSON_OBJECT);

    const cmcp_json_t *content = cmcp_json_object_get(resp.result, "content");
    TEST_ASSERT(content && content->type == CMCP_JSON_ARRAY);
    TEST_ASSERT(cmcp_json_array_len(content) == 1);
    const cmcp_json_t *item = cmcp_json_array_at(content, 0);
    TEST_ASSERT(item && item->type == CMCP_JSON_OBJECT);
    const cmcp_json_t *typ = cmcp_json_object_get(item, "type");
    const cmcp_json_t *txt = cmcp_json_object_get(item, "text");
    TEST_ASSERT(typ && strcmp(typ->str.s, "text") == 0);
    TEST_ASSERT(txt && strcmp(txt->str.s, "42") == 0);

    const cmcp_json_t *iserr = cmcp_json_object_get(resp.result, "isError");
    TEST_ASSERT(iserr && iserr->type == CMCP_JSON_BOOL && iserr->b == 0);

    cmcp_rpc_message_clear(&resp);

    cmcp_client_free(cli);
    cmcp_transport_close(p.client_t);
    pthread_join(th, NULL);
    TEST_ASSERT(ctx.calls == 1);
    cmcp_server_free(srv);
    cmcp_transport_close(p.server_t);
}

static void test_tools_call_unknown(void) {
    transport_pair_t p;
    TEST_ASSERT(make_pair(&p) == 0);

    cmcp_server_t *srv = cmcp_server_new("test-server", "0.1.0");
    cmcp_server_add_tool(srv, &(cmcp_tool_t){
        .name = "echo", .handler = echo_handler,
    });

    server_arg_t sa = { srv, p.server_t, 0 };
    pthread_t th;
    pthread_create(&th, NULL, server_thread, &sa);

    cmcp_client_t *cli = cmcp_client_new("test-client", "0.0.1");
    TEST_ASSERT(cmcp_client_handshake(cli, p.client_t) == CMCP_OK);

    cmcp_rpc_message_t resp;
    TEST_ASSERT(cmcp_client_request(cli, "tools/call",
                                     call_args_object("nope", NULL),
                                     &resp) == CMCP_OK);
    TEST_ASSERT(resp.error != NULL);
    TEST_ASSERT(resp.error->code == CMCP_RPC_INVALID_PARAMS);
    TEST_ASSERT(resp.error->data && resp.error->data->type == CMCP_JSON_OBJECT);
    const cmcp_json_t *nm = cmcp_json_object_get(resp.error->data, "name");
    TEST_ASSERT(nm && strcmp(nm->str.s, "nope") == 0);
    cmcp_rpc_message_clear(&resp);

    cmcp_client_free(cli);
    cmcp_transport_close(p.client_t);
    pthread_join(th, NULL);
    cmcp_server_free(srv);
    cmcp_transport_close(p.server_t);
}

static void test_tools_call_missing_name(void) {
    transport_pair_t p;
    TEST_ASSERT(make_pair(&p) == 0);

    cmcp_server_t *srv = cmcp_server_new("test-server", "0.1.0");
    cmcp_server_add_tool(srv, &(cmcp_tool_t){
        .name = "echo", .handler = echo_handler,
    });

    server_arg_t sa = { srv, p.server_t, 0 };
    pthread_t th;
    pthread_create(&th, NULL, server_thread, &sa);

    cmcp_client_t *cli = cmcp_client_new("test-client", "0.0.1");
    TEST_ASSERT(cmcp_client_handshake(cli, p.client_t) == CMCP_OK);

    /* `tools/call` with no `name` field. */
    cmcp_json_t *empty = cmcp_json_new_object();

    cmcp_rpc_message_t resp;
    TEST_ASSERT(cmcp_client_request(cli, "tools/call", empty, &resp) == CMCP_OK);
    TEST_ASSERT(resp.error != NULL);
    TEST_ASSERT(resp.error->code == CMCP_RPC_INVALID_PARAMS);
    cmcp_rpc_message_clear(&resp);

    cmcp_client_free(cli);
    cmcp_transport_close(p.client_t);
    pthread_join(th, NULL);
    cmcp_server_free(srv);
    cmcp_transport_close(p.server_t);
}

/* Tool ran but reported a tool-level failure (file not found, etc.) —
 * server returns 200-ish RPC response with isError:true, NOT a JSON-RPC
 * error. */
static void test_tools_call_tool_level_error(void) {
    transport_pair_t p;
    TEST_ASSERT(make_pair(&p) == 0);

    cmcp_server_t *srv = cmcp_server_new("test-server", "0.1.0");
    cmcp_server_add_tool(srv, &(cmcp_tool_t){
        .name = "fail", .handler = tool_error_handler,
    });

    server_arg_t sa = { srv, p.server_t, 0 };
    pthread_t th;
    pthread_create(&th, NULL, server_thread, &sa);

    cmcp_client_t *cli = cmcp_client_new("test-client", "0.0.1");
    TEST_ASSERT(cmcp_client_handshake(cli, p.client_t) == CMCP_OK);

    cmcp_rpc_message_t resp;
    TEST_ASSERT(cmcp_client_request(cli, "tools/call",
                                     call_args_object("fail", NULL),
                                     &resp) == CMCP_OK);
    TEST_ASSERT(resp.error == NULL);    /* not a JSON-RPC error */
    TEST_ASSERT(resp.result != NULL);
    const cmcp_json_t *iserr = cmcp_json_object_get(resp.result, "isError");
    TEST_ASSERT(iserr && iserr->type == CMCP_JSON_BOOL && iserr->b == 1);
    const cmcp_json_t *content = cmcp_json_object_get(resp.result, "content");
    TEST_ASSERT(content && content->type == CMCP_JSON_ARRAY);
    TEST_ASSERT(cmcp_json_array_len(content) == 1);
    cmcp_rpc_message_clear(&resp);

    cmcp_client_free(cli);
    cmcp_transport_close(p.client_t);
    pthread_join(th, NULL);
    cmcp_server_free(srv);
    cmcp_transport_close(p.server_t);
}

/* Handler returns non-zero → JSON-RPC -32603 internal error. */
static void test_tools_call_handler_internal_error(void) {
    transport_pair_t p;
    TEST_ASSERT(make_pair(&p) == 0);

    cmcp_server_t *srv = cmcp_server_new("test-server", "0.1.0");
    cmcp_server_add_tool(srv, &(cmcp_tool_t){
        .name = "boom", .handler = internal_error_handler,
    });

    server_arg_t sa = { srv, p.server_t, 0 };
    pthread_t th;
    pthread_create(&th, NULL, server_thread, &sa);

    cmcp_client_t *cli = cmcp_client_new("test-client", "0.0.1");
    TEST_ASSERT(cmcp_client_handshake(cli, p.client_t) == CMCP_OK);

    cmcp_rpc_message_t resp;
    TEST_ASSERT(cmcp_client_request(cli, "tools/call",
                                     call_args_object("boom", NULL),
                                     &resp) == CMCP_OK);
    TEST_ASSERT(resp.error != NULL);
    TEST_ASSERT(resp.error->code == CMCP_RPC_INTERNAL_ERROR);
    cmcp_rpc_message_clear(&resp);

    cmcp_client_free(cli);
    cmcp_transport_close(p.client_t);
    pthread_join(th, NULL);
    cmcp_server_free(srv);
    cmcp_transport_close(p.server_t);
}

/* A server with no tools registered must still answer tools/list with
 * an empty array. The `tools` capability is omitted in that case (no
 * tools to advertise). */
static void test_no_tools_empty_list(void) {
    transport_pair_t p;
    TEST_ASSERT(make_pair(&p) == 0);

    cmcp_server_t *srv = cmcp_server_new("test-server", "0.1.0");

    server_arg_t sa = { srv, p.server_t, 0 };
    pthread_t th;
    pthread_create(&th, NULL, server_thread, &sa);

    cmcp_client_t *cli = cmcp_client_new("test-client", "0.0.1");
    TEST_ASSERT(cmcp_client_handshake(cli, p.client_t) == CMCP_OK);

    cmcp_rpc_message_t resp;
    TEST_ASSERT(cmcp_client_request(cli, "tools/list", NULL, &resp) == CMCP_OK);
    TEST_ASSERT(resp.error == NULL);
    const cmcp_json_t *arr = cmcp_json_object_get(resp.result, "tools");
    TEST_ASSERT(arr && arr->type == CMCP_JSON_ARRAY);
    TEST_ASSERT(cmcp_json_array_len(arr) == 0);
    cmcp_rpc_message_clear(&resp);

    cmcp_client_free(cli);
    cmcp_transport_close(p.client_t);
    pthread_join(th, NULL);
    cmcp_server_free(srv);
    cmcp_transport_close(p.server_t);
}

/* Force the registry past its initial capacity to exercise realloc(). */
static void test_many_tools_registered(void) {
    cmcp_server_t *srv = cmcp_server_new("big-server", "0.1.0");
    char namebuf[16];
    for (int i = 0; i < 25; i++) {
        snprintf(namebuf, sizeof namebuf, "tool_%d", i);
        cmcp_tool_t t = { .name = namebuf, .handler = echo_handler };
        TEST_ASSERT(cmcp_server_add_tool(srv, &t) == CMCP_OK);
    }
    /* Look one of them up via tools/list over an in-process pair. */
    transport_pair_t p;
    TEST_ASSERT(make_pair(&p) == 0);
    server_arg_t sa = { srv, p.server_t, 0 };
    pthread_t th;
    pthread_create(&th, NULL, server_thread, &sa);

    cmcp_client_t *cli = cmcp_client_new("c", "0");
    TEST_ASSERT(cmcp_client_handshake(cli, p.client_t) == CMCP_OK);
    cmcp_rpc_message_t resp;
    TEST_ASSERT(cmcp_client_request(cli, "tools/list", NULL, &resp) == CMCP_OK);
    const cmcp_json_t *arr = cmcp_json_object_get(resp.result, "tools");
    TEST_ASSERT(cmcp_json_array_len(arr) == 25);
    cmcp_rpc_message_clear(&resp);

    cmcp_client_free(cli);
    cmcp_transport_close(p.client_t);
    pthread_join(th, NULL);
    cmcp_server_free(srv);
    cmcp_transport_close(p.server_t);
}

/* ====================================================================== */

int main(void) {
    fprintf(stderr, "test_tools:\n");

    TEST_RUN(test_register_and_list);
    TEST_RUN(test_tools_list_over_wire);
    TEST_RUN(test_tools_call_success);
    TEST_RUN(test_tools_call_unknown);
    TEST_RUN(test_tools_call_missing_name);
    TEST_RUN(test_tools_call_tool_level_error);
    TEST_RUN(test_tools_call_handler_internal_error);
    TEST_RUN(test_no_tools_empty_list);
    TEST_RUN(test_many_tools_registered);

    TEST_DONE();
}
