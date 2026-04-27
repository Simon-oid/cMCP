#include "test.h"
#include "cmcp.h"
#include "cmcp_json.h"
#include "cmcp_types.h"

#include <stdio.h>
#include <string.h>

/* ====================================================================== */
/* cmcp_id_t                                                               */
/* ====================================================================== */

static void test_id_kinds(void) {
    cmcp_id_t a, b;

    cmcp_id_init_none(&a);
    TEST_ASSERT(a.kind == CMCP_ID_NONE);

    cmcp_id_init_null(&a);
    TEST_ASSERT(a.kind == CMCP_ID_NULL);

    cmcp_id_init_int(&a, 42);
    TEST_ASSERT(a.kind == CMCP_ID_INT && a.i == 42);

    cmcp_id_clear(&a);
    TEST_ASSERT(a.kind == CMCP_ID_NONE);

    TEST_ASSERT(cmcp_id_init_string(&a, "abc", 3) == CMCP_OK);
    TEST_ASSERT(a.kind == CMCP_ID_STRING && a.s_len == 3);
    TEST_ASSERT(memcmp(a.s, "abc", 3) == 0);

    cmcp_id_init_none(&b);
    TEST_ASSERT(cmcp_id_copy(&b, &a) == CMCP_OK);
    TEST_ASSERT(b.kind == CMCP_ID_STRING && b.s_len == 3);
    TEST_ASSERT(b.s != a.s);                    /* deep copy */
    TEST_ASSERT(cmcp_id_equal(&a, &b));

    cmcp_id_clear(&a);
    cmcp_id_clear(&b);
}

static void test_id_equality(void) {
    cmcp_id_t a, b;
    cmcp_id_init_int(&a, 5);
    cmcp_id_init_int(&b, 5);
    TEST_ASSERT(cmcp_id_equal(&a, &b));
    cmcp_id_init_int(&b, 6);
    TEST_ASSERT(!cmcp_id_equal(&a, &b));

    cmcp_id_init_string(&a, "hi", 2);
    cmcp_id_init_string(&b, "hi", 2);
    TEST_ASSERT(cmcp_id_equal(&a, &b));
    cmcp_id_clear(&b);
    cmcp_id_init_string(&b, "hello", 5);
    TEST_ASSERT(!cmcp_id_equal(&a, &b));

    /* different kinds never equal */
    cmcp_id_clear(&b);
    cmcp_id_init_int(&b, 0);
    TEST_ASSERT(!cmcp_id_equal(&a, &b));

    cmcp_id_clear(&a);
    cmcp_id_clear(&b);
}

/* ====================================================================== */
/* Encode                                                                  */
/* ====================================================================== */

static void test_emit_request_no_params(void) {
    cmcp_rpc_message_t m;
    TEST_ASSERT(cmcp_rpc_make_request(&m, 7, "ping", NULL) == CMCP_OK);
    char *s = cmcp_rpc_emit(&m);
    TEST_ASSERT(s != NULL);
    /* stable order: id, jsonrpc, method (alphabetical) */
    TEST_ASSERT(strcmp(s, "{\"id\":7,\"jsonrpc\":\"2.0\",\"method\":\"ping\"}") == 0);
    free(s);
    cmcp_rpc_message_clear(&m);
}

static void test_emit_request_with_params(void) {
    cmcp_rpc_message_t m;
    cmcp_json_t *params = cmcp_json_new_object();
    cmcp_json_object_set(params, "name", cmcp_json_new_string("crag"));
    TEST_ASSERT(cmcp_rpc_make_request(&m, 1, "tools/call", params) == CMCP_OK);
    char *s = cmcp_rpc_emit(&m);
    TEST_ASSERT(s != NULL);
    TEST_ASSERT(strstr(s, "\"jsonrpc\":\"2.0\"") != NULL);
    TEST_ASSERT(strstr(s, "\"method\":\"tools/call\"") != NULL);
    TEST_ASSERT(strstr(s, "\"params\":{\"name\":\"crag\"}") != NULL);
    TEST_ASSERT(strstr(s, "\"id\":1") != NULL);
    free(s);
    cmcp_rpc_message_clear(&m);
}

static void test_emit_notification(void) {
    cmcp_rpc_message_t m;
    TEST_ASSERT(cmcp_rpc_make_notification(&m, "notifications/initialized",
                                            NULL) == CMCP_OK);
    char *s = cmcp_rpc_emit(&m);
    TEST_ASSERT(s != NULL);
    TEST_ASSERT(strstr(s, "\"id\"") == NULL);   /* no id */
    TEST_ASSERT(strstr(s, "\"method\":\"notifications/initialized\"") != NULL);
    free(s);
    cmcp_rpc_message_clear(&m);
}

static void test_emit_response_success(void) {
    cmcp_id_t id; cmcp_id_init_int(&id, 7);
    cmcp_rpc_message_t m;
    cmcp_json_t *res = cmcp_json_new_object();
    cmcp_json_object_set(res, "ok", cmcp_json_new_bool(1));
    TEST_ASSERT(cmcp_rpc_make_response(&m, &id, res) == CMCP_OK);
    char *s = cmcp_rpc_emit(&m);
    TEST_ASSERT(s != NULL);
    TEST_ASSERT(strcmp(s, "{\"id\":7,\"jsonrpc\":\"2.0\",\"result\":{\"ok\":true}}") == 0);
    free(s);
    cmcp_rpc_message_clear(&m);
    cmcp_id_clear(&id);
}

static void test_emit_response_null_result(void) {
    cmcp_id_t id; cmcp_id_init_int(&id, 1);
    cmcp_rpc_message_t m;
    TEST_ASSERT(cmcp_rpc_make_response(&m, &id, NULL) == CMCP_OK);
    char *s = cmcp_rpc_emit(&m);
    TEST_ASSERT(s != NULL);
    TEST_ASSERT(strcmp(s, "{\"id\":1,\"jsonrpc\":\"2.0\",\"result\":null}") == 0);
    free(s);
    cmcp_rpc_message_clear(&m);
    cmcp_id_clear(&id);
}

static void test_emit_error(void) {
    cmcp_id_t id; cmcp_id_init_int(&id, 9);
    cmcp_rpc_message_t m;
    TEST_ASSERT(cmcp_rpc_make_error(&m, &id, CMCP_RPC_METHOD_NOT_FOUND,
                                     "Method not found", NULL) == CMCP_OK);
    char *s = cmcp_rpc_emit(&m);
    TEST_ASSERT(s != NULL);
    TEST_ASSERT(strstr(s, "\"code\":-32601") != NULL);
    TEST_ASSERT(strstr(s, "\"message\":\"Method not found\"") != NULL);
    TEST_ASSERT(strstr(s, "\"id\":9") != NULL);
    free(s);
    cmcp_rpc_message_clear(&m);
    cmcp_id_clear(&id);
}

static void test_emit_error_with_data(void) {
    cmcp_id_t id; cmcp_id_init_string(&id, "req-1", 5);
    cmcp_rpc_message_t m;
    cmcp_json_t *data = cmcp_json_new_object();
    cmcp_json_object_set(data, "field", cmcp_json_new_string("name"));
    TEST_ASSERT(cmcp_rpc_make_error(&m, &id, CMCP_RPC_INVALID_PARAMS,
                                     "Bad input", data) == CMCP_OK);
    char *s = cmcp_rpc_emit(&m);
    TEST_ASSERT(s != NULL);
    TEST_ASSERT(strstr(s, "\"id\":\"req-1\"") != NULL);
    TEST_ASSERT(strstr(s, "\"data\":{\"field\":\"name\"}") != NULL);
    TEST_ASSERT(strstr(s, "\"code\":-32602") != NULL);
    free(s);
    cmcp_rpc_message_clear(&m);
    cmcp_id_clear(&id);
}

/* ====================================================================== */
/* Decode                                                                  */
/* ====================================================================== */

static void test_parse_request(void) {
    const char *src =
        "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"ping\","
        "\"params\":{\"x\":42}}";
    cmcp_rpc_message_t *msgs = NULL;
    size_t n = 0;
    TEST_ASSERT(cmcp_rpc_parse(src, strlen(src), &msgs, &n) == CMCP_OK);
    TEST_ASSERT(n == 1);
    TEST_ASSERT(msgs[0].kind == CMCP_MSG_REQUEST);
    TEST_ASSERT(msgs[0].id.kind == CMCP_ID_INT && msgs[0].id.i == 1);
    TEST_ASSERT(strcmp(msgs[0].method, "ping") == 0);
    TEST_ASSERT(msgs[0].params != NULL);
    const cmcp_json_t *x = cmcp_json_object_get(msgs[0].params, "x");
    TEST_ASSERT(x && x->type == CMCP_JSON_INT && x->i == 42);
    cmcp_rpc_messages_free(msgs, n);
}

static void test_parse_request_string_id(void) {
    const char *src = "{\"jsonrpc\":\"2.0\",\"id\":\"abc\",\"method\":\"ping\"}";
    cmcp_rpc_message_t *msgs = NULL; size_t n = 0;
    TEST_ASSERT(cmcp_rpc_parse(src, strlen(src), &msgs, &n) == CMCP_OK);
    TEST_ASSERT(n == 1);
    TEST_ASSERT(msgs[0].id.kind == CMCP_ID_STRING);
    TEST_ASSERT(msgs[0].id.s_len == 3 && memcmp(msgs[0].id.s, "abc", 3) == 0);
    cmcp_rpc_messages_free(msgs, n);
}

static void test_parse_notification(void) {
    const char *src = "{\"jsonrpc\":\"2.0\",\"method\":\"hello\"}";
    cmcp_rpc_message_t *msgs = NULL; size_t n = 0;
    TEST_ASSERT(cmcp_rpc_parse(src, strlen(src), &msgs, &n) == CMCP_OK);
    TEST_ASSERT(n == 1);
    TEST_ASSERT(msgs[0].kind == CMCP_MSG_NOTIFICATION);
    TEST_ASSERT(msgs[0].id.kind == CMCP_ID_NONE);
    TEST_ASSERT(strcmp(msgs[0].method, "hello") == 0);
    cmcp_rpc_messages_free(msgs, n);
}

static void test_parse_response_success(void) {
    const char *src = "{\"jsonrpc\":\"2.0\",\"id\":3,\"result\":{\"ok\":true}}";
    cmcp_rpc_message_t *msgs = NULL; size_t n = 0;
    TEST_ASSERT(cmcp_rpc_parse(src, strlen(src), &msgs, &n) == CMCP_OK);
    TEST_ASSERT(msgs[0].kind == CMCP_MSG_RESPONSE);
    TEST_ASSERT(msgs[0].id.kind == CMCP_ID_INT && msgs[0].id.i == 3);
    TEST_ASSERT(msgs[0].result != NULL && msgs[0].error == NULL);
    cmcp_rpc_messages_free(msgs, n);
}

static void test_parse_response_error(void) {
    const char *src =
        "{\"jsonrpc\":\"2.0\",\"id\":4,"
        "\"error\":{\"code\":-32601,\"message\":\"nope\"}}";
    cmcp_rpc_message_t *msgs = NULL; size_t n = 0;
    TEST_ASSERT(cmcp_rpc_parse(src, strlen(src), &msgs, &n) == CMCP_OK);
    TEST_ASSERT(msgs[0].kind == CMCP_MSG_RESPONSE);
    TEST_ASSERT(msgs[0].error != NULL && msgs[0].result == NULL);
    TEST_ASSERT(msgs[0].error->code == CMCP_RPC_METHOD_NOT_FOUND);
    TEST_ASSERT(strcmp(msgs[0].error->message, "nope") == 0);
    cmcp_rpc_messages_free(msgs, n);
}

static void test_parse_response_null_id(void) {
    const char *src =
        "{\"jsonrpc\":\"2.0\",\"id\":null,"
        "\"error\":{\"code\":-32700,\"message\":\"parse\"}}";
    cmcp_rpc_message_t *msgs = NULL; size_t n = 0;
    TEST_ASSERT(cmcp_rpc_parse(src, strlen(src), &msgs, &n) == CMCP_OK);
    TEST_ASSERT(msgs[0].id.kind == CMCP_ID_NULL);
    cmcp_rpc_messages_free(msgs, n);
}

/* ====================================================================== */
/* Round-trip                                                              */
/* ====================================================================== */

static void test_roundtrip_request(void) {
    cmcp_rpc_message_t m;
    cmcp_json_t *params = cmcp_json_new_object();
    cmcp_json_object_set(params, "k", cmcp_json_new_int(99));
    cmcp_rpc_make_request(&m, 11, "foo", params);
    char *s = cmcp_rpc_emit(&m);
    TEST_ASSERT(s != NULL);

    cmcp_rpc_message_t *msgs = NULL; size_t n = 0;
    TEST_ASSERT(cmcp_rpc_parse(s, strlen(s), &msgs, &n) == CMCP_OK);
    TEST_ASSERT(n == 1);
    TEST_ASSERT(msgs[0].kind == CMCP_MSG_REQUEST);
    TEST_ASSERT(msgs[0].id.i == 11);
    TEST_ASSERT(strcmp(msgs[0].method, "foo") == 0);
    const cmcp_json_t *k = cmcp_json_object_get(msgs[0].params, "k");
    TEST_ASSERT(k && k->type == CMCP_JSON_INT && k->i == 99);

    free(s);
    cmcp_rpc_messages_free(msgs, n);
    cmcp_rpc_message_clear(&m);
}

static void test_roundtrip_response_error(void) {
    cmcp_id_t id; cmcp_id_init_int(&id, 42);
    cmcp_rpc_message_t m;
    cmcp_json_t *data = cmcp_json_new_array();
    cmcp_json_array_append(data, cmcp_json_new_string("detail"));
    cmcp_rpc_make_error(&m, &id, CMCP_RPC_INVALID_PARAMS, "bad", data);
    char *s = cmcp_rpc_emit(&m);

    cmcp_rpc_message_t *msgs = NULL; size_t n = 0;
    TEST_ASSERT(cmcp_rpc_parse(s, strlen(s), &msgs, &n) == CMCP_OK);
    TEST_ASSERT(msgs[0].kind == CMCP_MSG_RESPONSE);
    TEST_ASSERT(msgs[0].error->code == CMCP_RPC_INVALID_PARAMS);
    TEST_ASSERT(strcmp(msgs[0].error->message, "bad") == 0);
    TEST_ASSERT(msgs[0].error->data != NULL);
    TEST_ASSERT(msgs[0].error->data->type == CMCP_JSON_ARRAY);

    free(s);
    cmcp_rpc_messages_free(msgs, n);
    cmcp_rpc_message_clear(&m);
    cmcp_id_clear(&id);
}

/* MCP initialize request — real-world wire shape */
static void test_roundtrip_mcp_initialize(void) {
    const char *src =
        "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"initialize\","
        "\"params\":{"
            "\"protocolVersion\":\"2025-06-18\","
            "\"capabilities\":{\"tools\":{}},"
            "\"clientInfo\":{\"name\":\"openclawd\",\"version\":\"0.0.1\"}"
        "}}";
    cmcp_rpc_message_t *msgs = NULL; size_t n = 0;
    TEST_ASSERT(cmcp_rpc_parse(src, strlen(src), &msgs, &n) == CMCP_OK);
    TEST_ASSERT(n == 1);
    TEST_ASSERT(msgs[0].kind == CMCP_MSG_REQUEST);
    TEST_ASSERT(strcmp(msgs[0].method, "initialize") == 0);
    const cmcp_json_t *pv = cmcp_json_object_get(msgs[0].params, "protocolVersion");
    TEST_ASSERT(pv && pv->type == CMCP_JSON_STRING);
    TEST_ASSERT(strcmp(pv->str.s, "2025-06-18") == 0);

    /* Re-emit and re-parse — values should still match. */
    char *s = cmcp_rpc_emit(&msgs[0]);
    cmcp_rpc_message_t *msgs2 = NULL; size_t n2 = 0;
    TEST_ASSERT(cmcp_rpc_parse(s, strlen(s), &msgs2, &n2) == CMCP_OK);
    const cmcp_json_t *ci = cmcp_json_object_get(msgs2[0].params, "clientInfo");
    TEST_ASSERT(ci && ci->type == CMCP_JSON_OBJECT);
    const cmcp_json_t *name = cmcp_json_object_get(ci, "name");
    TEST_ASSERT(name && strcmp(name->str.s, "openclawd") == 0);
    free(s);
    cmcp_rpc_messages_free(msgs, n);
    cmcp_rpc_messages_free(msgs2, n2);
}

/* ====================================================================== */
/* Batch                                                                   */
/* ====================================================================== */

static void test_parse_batch(void) {
    const char *src =
        "[{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"a\"},"
        " {\"jsonrpc\":\"2.0\",\"method\":\"b\"}]";
    cmcp_rpc_message_t *msgs = NULL; size_t n = 0;
    TEST_ASSERT(cmcp_rpc_parse(src, strlen(src), &msgs, &n) == CMCP_OK);
    TEST_ASSERT(n == 2);
    TEST_ASSERT(msgs[0].kind == CMCP_MSG_REQUEST);
    TEST_ASSERT(msgs[1].kind == CMCP_MSG_NOTIFICATION);
    cmcp_rpc_messages_free(msgs, n);
}

static void test_emit_batch(void) {
    cmcp_rpc_message_t batch[2];
    cmcp_rpc_make_request(&batch[0], 1, "a", NULL);
    cmcp_rpc_make_notification(&batch[1], "b", NULL);
    char *s = cmcp_rpc_emit_batch(batch, 2);
    TEST_ASSERT(s != NULL);
    TEST_ASSERT(s[0] == '[');
    TEST_ASSERT(strstr(s, "\"method\":\"a\"") != NULL);
    TEST_ASSERT(strstr(s, "\"method\":\"b\"") != NULL);
    free(s);
    cmcp_rpc_message_clear(&batch[0]);
    cmcp_rpc_message_clear(&batch[1]);
}

static void test_empty_batch_rejected(void) {
    cmcp_rpc_message_t *msgs = NULL; size_t n = 0;
    TEST_ASSERT(cmcp_rpc_parse("[]", 2, &msgs, &n) == CMCP_EPROTOCOL);
}

/* ====================================================================== */
/* Malformed input                                                         */
/* ====================================================================== */

static void test_reject_bad_jsonrpc_version(void) {
    const char *src = "{\"jsonrpc\":\"1.0\",\"id\":1,\"method\":\"x\"}";
    cmcp_rpc_message_t *msgs = NULL; size_t n = 0;
    TEST_ASSERT(cmcp_rpc_parse(src, strlen(src), &msgs, &n) == CMCP_EPROTOCOL);
}

static void test_reject_missing_jsonrpc(void) {
    const char *src = "{\"id\":1,\"method\":\"x\"}";
    cmcp_rpc_message_t *msgs = NULL; size_t n = 0;
    TEST_ASSERT(cmcp_rpc_parse(src, strlen(src), &msgs, &n) == CMCP_EPROTOCOL);
}

static void test_reject_method_with_result(void) {
    const char *src =
        "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"x\",\"result\":1}";
    cmcp_rpc_message_t *msgs = NULL; size_t n = 0;
    TEST_ASSERT(cmcp_rpc_parse(src, strlen(src), &msgs, &n) == CMCP_EPROTOCOL);
}

static void test_reject_response_both_result_and_error(void) {
    const char *src =
        "{\"jsonrpc\":\"2.0\",\"id\":1,\"result\":1,"
        "\"error\":{\"code\":-1,\"message\":\"x\"}}";
    cmcp_rpc_message_t *msgs = NULL; size_t n = 0;
    TEST_ASSERT(cmcp_rpc_parse(src, strlen(src), &msgs, &n) == CMCP_EPROTOCOL);
}

static void test_reject_response_neither_result_nor_error(void) {
    const char *src = "{\"jsonrpc\":\"2.0\",\"id\":1}";
    cmcp_rpc_message_t *msgs = NULL; size_t n = 0;
    TEST_ASSERT(cmcp_rpc_parse(src, strlen(src), &msgs, &n) == CMCP_EPROTOCOL);
}

static void test_reject_garbage(void) {
    cmcp_rpc_message_t *msgs = NULL; size_t n = 0;
    TEST_ASSERT(cmcp_rpc_parse("not json", 8, &msgs, &n) == CMCP_EPARSE);
}

static void test_reject_wrong_id_type(void) {
    const char *src = "{\"jsonrpc\":\"2.0\",\"id\":1.5,\"method\":\"x\"}";
    cmcp_rpc_message_t *msgs = NULL; size_t n = 0;
    TEST_ASSERT(cmcp_rpc_parse(src, strlen(src), &msgs, &n) == CMCP_EPROTOCOL);
}

static void test_reject_bad_error_shape(void) {
    /* error missing required code */
    const char *src =
        "{\"jsonrpc\":\"2.0\",\"id\":1,\"error\":{\"message\":\"x\"}}";
    cmcp_rpc_message_t *msgs = NULL; size_t n = 0;
    TEST_ASSERT(cmcp_rpc_parse(src, strlen(src), &msgs, &n) == CMCP_EPROTOCOL);
}

/* ====================================================================== */
/* Pending table                                                           */
/* ====================================================================== */

static void test_pending_register_take(void) {
    cmcp_rpc_pending_t *t = cmcp_rpc_pending_new();
    TEST_ASSERT(t != NULL);

    int sentinel_a = 1, sentinel_b = 2;
    long long id1 = cmcp_rpc_pending_register(t, &sentinel_a);
    long long id2 = cmcp_rpc_pending_register(t, &sentinel_b);
    TEST_ASSERT(id1 > 0 && id2 > 0 && id1 != id2);
    TEST_ASSERT(cmcp_rpc_pending_count(t) == 2);

    void *got = NULL;
    TEST_ASSERT(cmcp_rpc_pending_take(t, id1, &got) == 1);
    TEST_ASSERT(got == &sentinel_a);
    TEST_ASSERT(cmcp_rpc_pending_count(t) == 1);

    /* take again is a miss */
    TEST_ASSERT(cmcp_rpc_pending_take(t, id1, &got) == 0);

    TEST_ASSERT(cmcp_rpc_pending_take(t, id2, &got) == 1);
    TEST_ASSERT(got == &sentinel_b);
    TEST_ASSERT(cmcp_rpc_pending_count(t) == 0);

    cmcp_rpc_pending_free(t);
}

static void test_pending_unknown_id(void) {
    cmcp_rpc_pending_t *t = cmcp_rpc_pending_new();
    void *got = NULL;
    TEST_ASSERT(cmcp_rpc_pending_take(t, 999, &got) == 0);
    cmcp_rpc_pending_free(t);
}

static void test_pending_resize(void) {
    /* Force resize: register more than initial capacity (16). */
    cmcp_rpc_pending_t *t = cmcp_rpc_pending_new();
    long long ids[200];
    int sentinels[200];
    for (int i = 0; i < 200; i++) {
        sentinels[i] = i;
        ids[i] = cmcp_rpc_pending_register(t, &sentinels[i]);
        TEST_ASSERT(ids[i] > 0);
    }
    TEST_ASSERT(cmcp_rpc_pending_count(t) == 200);
    /* Take all in reverse order */
    for (int i = 199; i >= 0; i--) {
        void *got = NULL;
        TEST_ASSERT(cmcp_rpc_pending_take(t, ids[i], &got) == 1);
        TEST_ASSERT(got == &sentinels[i]);
    }
    TEST_ASSERT(cmcp_rpc_pending_count(t) == 0);
    cmcp_rpc_pending_free(t);
}

/* ====================================================================== */
/* Dispatch                                                                */
/* ====================================================================== */

typedef struct {
    int  call_count;
    long long last_id;
} echo_ctx_t;

static int echo_handler(const cmcp_rpc_message_t *in,
                        cmcp_rpc_message_t *out, void *userdata) {
    echo_ctx_t *ctx = (echo_ctx_t *)userdata;
    ctx->call_count++;
    if (in->id.kind == CMCP_ID_INT) ctx->last_id = in->id.i;
    if (out) {
        out->result = cmcp_json_new_string("ok");
    }
    return CMCP_OK;
}

static int failing_handler(const cmcp_rpc_message_t *in,
                           cmcp_rpc_message_t *out, void *userdata) {
    (void)in; (void)out; (void)userdata;
    return CMCP_EHANDLER;
}

static void test_dispatch_request(void) {
    echo_ctx_t ctx = {0, 0};
    cmcp_rpc_route_t routes[] = {
        { "ping", echo_handler, &ctx },
    };

    cmcp_rpc_message_t in;
    cmcp_rpc_make_request(&in, 5, "ping", NULL);
    cmcp_rpc_message_t out;
    TEST_ASSERT(cmcp_rpc_dispatch(&in, routes, 1, &out) == CMCP_OK);
    TEST_ASSERT(ctx.call_count == 1 && ctx.last_id == 5);
    TEST_ASSERT(out.kind == CMCP_MSG_RESPONSE);
    TEST_ASSERT(out.id.kind == CMCP_ID_INT && out.id.i == 5);
    TEST_ASSERT(out.result != NULL && out.error == NULL);

    cmcp_rpc_message_clear(&in);
    cmcp_rpc_message_clear(&out);
}

static void test_dispatch_method_not_found(void) {
    cmcp_rpc_route_t routes[] = { {0} };
    cmcp_rpc_message_t in, out;
    cmcp_rpc_make_request(&in, 1, "nope", NULL);
    TEST_ASSERT(cmcp_rpc_dispatch(&in, routes, 0, &out) == CMCP_OK);
    TEST_ASSERT(out.error != NULL);
    TEST_ASSERT(out.error->code == CMCP_RPC_METHOD_NOT_FOUND);
    cmcp_rpc_message_clear(&in);
    cmcp_rpc_message_clear(&out);
}

static void test_dispatch_handler_error(void) {
    cmcp_rpc_route_t routes[] = {
        { "boom", failing_handler, NULL },
    };
    cmcp_rpc_message_t in, out;
    cmcp_rpc_make_request(&in, 2, "boom", NULL);
    TEST_ASSERT(cmcp_rpc_dispatch(&in, routes, 1, &out) == CMCP_OK);
    TEST_ASSERT(out.error != NULL);
    TEST_ASSERT(out.error->code == CMCP_RPC_INTERNAL_ERROR);
    cmcp_rpc_message_clear(&in);
    cmcp_rpc_message_clear(&out);
}

static void test_dispatch_notification(void) {
    echo_ctx_t ctx = {0, 0};
    cmcp_rpc_route_t routes[] = {
        { "evt", echo_handler, &ctx },
    };
    cmcp_rpc_message_t in;
    cmcp_rpc_make_notification(&in, "evt", NULL);
    /* No out for notifications. */
    TEST_ASSERT(cmcp_rpc_dispatch(&in, routes, 1, NULL) == CMCP_OK);
    TEST_ASSERT(ctx.call_count == 1);
    cmcp_rpc_message_clear(&in);
}

static void test_dispatch_unknown_notification_silent(void) {
    cmcp_rpc_route_t routes[] = { {0} };
    cmcp_rpc_message_t in;
    cmcp_rpc_make_notification(&in, "evt", NULL);
    /* Unknown notifications must NOT produce an error reply. */
    TEST_ASSERT(cmcp_rpc_dispatch(&in, routes, 0, NULL) == CMCP_OK);
    cmcp_rpc_message_clear(&in);
}

static void test_dispatch_response_rejected(void) {
    cmcp_id_t id; cmcp_id_init_int(&id, 1);
    cmcp_rpc_message_t in, out;
    cmcp_rpc_make_response(&in, &id, NULL);
    TEST_ASSERT(cmcp_rpc_dispatch(&in, NULL, 0, &out) == CMCP_EINVAL);
    cmcp_rpc_message_clear(&in);
    cmcp_id_clear(&id);
}

/* ====================================================================== */

int main(void) {
    fprintf(stderr, "test_rpc:\n");

    TEST_RUN(test_id_kinds);
    TEST_RUN(test_id_equality);

    TEST_RUN(test_emit_request_no_params);
    TEST_RUN(test_emit_request_with_params);
    TEST_RUN(test_emit_notification);
    TEST_RUN(test_emit_response_success);
    TEST_RUN(test_emit_response_null_result);
    TEST_RUN(test_emit_error);
    TEST_RUN(test_emit_error_with_data);

    TEST_RUN(test_parse_request);
    TEST_RUN(test_parse_request_string_id);
    TEST_RUN(test_parse_notification);
    TEST_RUN(test_parse_response_success);
    TEST_RUN(test_parse_response_error);
    TEST_RUN(test_parse_response_null_id);

    TEST_RUN(test_roundtrip_request);
    TEST_RUN(test_roundtrip_response_error);
    TEST_RUN(test_roundtrip_mcp_initialize);

    TEST_RUN(test_parse_batch);
    TEST_RUN(test_emit_batch);
    TEST_RUN(test_empty_batch_rejected);

    TEST_RUN(test_reject_bad_jsonrpc_version);
    TEST_RUN(test_reject_missing_jsonrpc);
    TEST_RUN(test_reject_method_with_result);
    TEST_RUN(test_reject_response_both_result_and_error);
    TEST_RUN(test_reject_response_neither_result_nor_error);
    TEST_RUN(test_reject_garbage);
    TEST_RUN(test_reject_wrong_id_type);
    TEST_RUN(test_reject_bad_error_shape);

    TEST_RUN(test_pending_register_take);
    TEST_RUN(test_pending_unknown_id);
    TEST_RUN(test_pending_resize);

    TEST_RUN(test_dispatch_request);
    TEST_RUN(test_dispatch_method_not_found);
    TEST_RUN(test_dispatch_handler_error);
    TEST_RUN(test_dispatch_notification);
    TEST_RUN(test_dispatch_unknown_notification_silent);
    TEST_RUN(test_dispatch_response_rejected);

    TEST_DONE();
}
