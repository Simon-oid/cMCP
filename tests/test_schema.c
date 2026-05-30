/* pipe(2) — POSIX. */
#define _POSIX_C_SOURCE 200809L

#include "test.h"
#include "cmcp.h"
#include "cmcp_client.h"
#include "cmcp_json.h"
#include "cmcp_schema.h"
#include "cmcp_server.h"
#include "cmcp_transport.h"
#include "cmcp_types.h"

#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* ====================================================================== */
/* Convenience: parse a JSON string into a tree (caller frees).            */
/* ====================================================================== */

static cmcp_json_t *J(const char *s) {
    cmcp_json_t *v = cmcp_json_parse(s, strlen(s));
    if (!v) {
        fprintf(stderr, "    JSON parse failed: %s\n", s);
        abort();
    }
    return v;
}

/* ====================================================================== */
/* type                                                                    */
/* ====================================================================== */

static void test_type_string(void) {
    cmcp_json_t *schema = J("{\"type\":\"string\"}");
    cmcp_json_t *ok     = J("\"hello\"");
    cmcp_json_t *bad    = J("42");

    TEST_ASSERT(cmcp_schema_validate(schema, ok, NULL) == CMCP_OK);

    cmcp_schema_error_t e;
    TEST_ASSERT(cmcp_schema_validate(schema, bad, &e) == CMCP_ESCHEMA);
    TEST_ASSERT(e.keyword && strcmp(e.keyword, "type") == 0);
    TEST_ASSERT(e.path && strcmp(e.path, "") == 0);
    TEST_ASSERT(e.message != NULL);
    cmcp_schema_error_clear(&e);

    cmcp_json_free(schema);
    cmcp_json_free(ok);
    cmcp_json_free(bad);
}

static void test_type_integer_vs_number(void) {
    cmcp_json_t *si = J("{\"type\":\"integer\"}");
    cmcp_json_t *sn = J("{\"type\":\"number\"}");
    cmcp_json_t *i  = J("7");
    cmcp_json_t *d  = J("7.5");

    TEST_ASSERT(cmcp_schema_validate(si, i, NULL) == CMCP_OK);
    TEST_ASSERT(cmcp_schema_validate(si, d, NULL) == CMCP_ESCHEMA);
    TEST_ASSERT(cmcp_schema_validate(sn, i, NULL) == CMCP_OK);
    TEST_ASSERT(cmcp_schema_validate(sn, d, NULL) == CMCP_OK);

    cmcp_json_free(si); cmcp_json_free(sn);
    cmcp_json_free(i);  cmcp_json_free(d);
}

static void test_type_boolean_array_object_null(void) {
    cmcp_json_t *sb = J("{\"type\":\"boolean\"}");
    cmcp_json_t *sa = J("{\"type\":\"array\"}");
    cmcp_json_t *so = J("{\"type\":\"object\"}");
    cmcp_json_t *sn = J("{\"type\":\"null\"}");

    cmcp_json_t *bt = J("true");
    cmcp_json_t *ar = J("[1,2,3]");
    cmcp_json_t *ob = J("{}");
    cmcp_json_t *nl = J("null");

    TEST_ASSERT(cmcp_schema_validate(sb, bt, NULL) == CMCP_OK);
    TEST_ASSERT(cmcp_schema_validate(sa, ar, NULL) == CMCP_OK);
    TEST_ASSERT(cmcp_schema_validate(so, ob, NULL) == CMCP_OK);
    TEST_ASSERT(cmcp_schema_validate(sn, nl, NULL) == CMCP_OK);

    /* Cross-type negatives. */
    TEST_ASSERT(cmcp_schema_validate(sb, ar, NULL) == CMCP_ESCHEMA);
    TEST_ASSERT(cmcp_schema_validate(sa, ob, NULL) == CMCP_ESCHEMA);
    TEST_ASSERT(cmcp_schema_validate(so, ar, NULL) == CMCP_ESCHEMA);
    TEST_ASSERT(cmcp_schema_validate(sn, bt, NULL) == CMCP_ESCHEMA);

    cmcp_json_free(sb); cmcp_json_free(sa); cmcp_json_free(so); cmcp_json_free(sn);
    cmcp_json_free(bt); cmcp_json_free(ar); cmcp_json_free(ob); cmcp_json_free(nl);
}

static void test_type_array_of_types(void) {
    cmcp_json_t *schema = J("{\"type\":[\"string\",\"null\"]}");
    cmcp_json_t *s = J("\"x\"");
    cmcp_json_t *n = J("null");
    cmcp_json_t *i = J("42");

    TEST_ASSERT(cmcp_schema_validate(schema, s, NULL) == CMCP_OK);
    TEST_ASSERT(cmcp_schema_validate(schema, n, NULL) == CMCP_OK);
    TEST_ASSERT(cmcp_schema_validate(schema, i, NULL) == CMCP_ESCHEMA);

    cmcp_json_free(schema);
    cmcp_json_free(s); cmcp_json_free(n); cmcp_json_free(i);
}

/* NULL value treated as JSON null. */
static void test_null_value(void) {
    cmcp_json_t *str = J("{\"type\":\"string\"}");
    cmcp_json_t *nul = J("{\"type\":\"null\"}");

    TEST_ASSERT(cmcp_schema_validate(str, NULL, NULL) == CMCP_ESCHEMA);
    TEST_ASSERT(cmcp_schema_validate(nul, NULL, NULL) == CMCP_OK);

    cmcp_json_free(str); cmcp_json_free(nul);
}

/* ====================================================================== */
/* properties + required                                                   */
/* ====================================================================== */

static void test_properties_recurse(void) {
    cmcp_json_t *schema = J(
        "{\"type\":\"object\","
         "\"properties\":{"
            "\"name\":{\"type\":\"string\"},"
            "\"age\":{\"type\":\"integer\"}"
         "}}");

    cmcp_json_t *ok  = J("{\"name\":\"alice\",\"age\":30}");
    cmcp_json_t *bad = J("{\"name\":\"alice\",\"age\":\"thirty\"}");

    TEST_ASSERT(cmcp_schema_validate(schema, ok, NULL) == CMCP_OK);

    cmcp_schema_error_t e;
    TEST_ASSERT(cmcp_schema_validate(schema, bad, &e) == CMCP_ESCHEMA);
    TEST_ASSERT(e.keyword && strcmp(e.keyword, "type") == 0);
    /* Path identifies the offending property. */
    TEST_ASSERT(e.path && strcmp(e.path, "/age") == 0);
    cmcp_schema_error_clear(&e);

    cmcp_json_free(schema); cmcp_json_free(ok); cmcp_json_free(bad);
}

static void test_required(void) {
    cmcp_json_t *schema = J(
        "{\"type\":\"object\","
         "\"properties\":{\"x\":{\"type\":\"integer\"}},"
         "\"required\":[\"x\"]}");

    cmcp_json_t *ok  = J("{\"x\":1}");
    cmcp_json_t *bad = J("{\"y\":1}");

    TEST_ASSERT(cmcp_schema_validate(schema, ok, NULL) == CMCP_OK);

    cmcp_schema_error_t e;
    TEST_ASSERT(cmcp_schema_validate(schema, bad, &e) == CMCP_ESCHEMA);
    TEST_ASSERT(e.keyword && strcmp(e.keyword, "required") == 0);
    TEST_ASSERT(e.path && strcmp(e.path, "/x") == 0);
    cmcp_schema_error_clear(&e);

    cmcp_json_free(schema); cmcp_json_free(ok); cmcp_json_free(bad);
}

/* `required` with NULL value (no arguments at all) → fail. */
static void test_required_with_null_value(void) {
    cmcp_json_t *schema = J(
        "{\"type\":\"object\","
         "\"properties\":{\"x\":{\"type\":\"integer\"}},"
         "\"required\":[\"x\"]}");

    /* NULL → JSON null → not an object → type fails first. */
    cmcp_schema_error_t e;
    TEST_ASSERT(cmcp_schema_validate(schema, NULL, &e) == CMCP_ESCHEMA);
    TEST_ASSERT(e.keyword && strcmp(e.keyword, "type") == 0);
    cmcp_schema_error_clear(&e);

    cmcp_json_free(schema);
}

/* ====================================================================== */
/* additionalProperties: false                                             */
/* ====================================================================== */

static void test_additional_properties_false(void) {
    cmcp_json_t *schema = J(
        "{\"type\":\"object\","
         "\"properties\":{\"x\":{\"type\":\"integer\"}},"
         "\"additionalProperties\":false}");

    cmcp_json_t *ok  = J("{\"x\":1}");
    cmcp_json_t *bad = J("{\"x\":1,\"y\":2}");

    TEST_ASSERT(cmcp_schema_validate(schema, ok, NULL) == CMCP_OK);

    cmcp_schema_error_t e;
    TEST_ASSERT(cmcp_schema_validate(schema, bad, &e) == CMCP_ESCHEMA);
    TEST_ASSERT(e.keyword && strcmp(e.keyword, "additionalProperties") == 0);
    TEST_ASSERT(e.path && strcmp(e.path, "/y") == 0);
    cmcp_schema_error_clear(&e);

    /* additionalProperties: true (or omitted) — extras allowed. */
    cmcp_json_t *open_schema = J(
        "{\"type\":\"object\","
         "\"properties\":{\"x\":{\"type\":\"integer\"}}}");
    TEST_ASSERT(cmcp_schema_validate(open_schema, bad, NULL) == CMCP_OK);

    cmcp_json_free(schema); cmcp_json_free(open_schema);
    cmcp_json_free(ok); cmcp_json_free(bad);
}

/* ====================================================================== */
/* enum                                                                    */
/* ====================================================================== */

static void test_enum(void) {
    cmcp_json_t *schema = J("{\"enum\":[\"red\",\"green\",\"blue\"]}");
    cmcp_json_t *ok  = J("\"green\"");
    cmcp_json_t *bad = J("\"purple\"");

    TEST_ASSERT(cmcp_schema_validate(schema, ok, NULL) == CMCP_OK);

    cmcp_schema_error_t e;
    TEST_ASSERT(cmcp_schema_validate(schema, bad, &e) == CMCP_ESCHEMA);
    TEST_ASSERT(e.keyword && strcmp(e.keyword, "enum") == 0);
    cmcp_schema_error_clear(&e);

    cmcp_json_free(schema); cmcp_json_free(ok); cmcp_json_free(bad);
}

static void test_enum_mixed_types(void) {
    cmcp_json_t *schema = J("{\"enum\":[1,\"two\",null,true]}");
    cmcp_json_t *t1 = J("1");
    cmcp_json_t *t2 = J("\"two\"");
    cmcp_json_t *t3 = J("null");
    cmcp_json_t *t4 = J("true");
    cmcp_json_t *bad = J("2");

    TEST_ASSERT(cmcp_schema_validate(schema, t1, NULL) == CMCP_OK);
    TEST_ASSERT(cmcp_schema_validate(schema, t2, NULL) == CMCP_OK);
    TEST_ASSERT(cmcp_schema_validate(schema, t3, NULL) == CMCP_OK);
    TEST_ASSERT(cmcp_schema_validate(schema, t4, NULL) == CMCP_OK);
    TEST_ASSERT(cmcp_schema_validate(schema, bad, NULL) == CMCP_ESCHEMA);

    cmcp_json_free(schema);
    cmcp_json_free(t1); cmcp_json_free(t2); cmcp_json_free(t3);
    cmcp_json_free(t4); cmcp_json_free(bad);
}

/* ====================================================================== */
/* String length                                                           */
/* ====================================================================== */

static void test_string_length(void) {
    cmcp_json_t *schema = J(
        "{\"type\":\"string\",\"minLength\":2,\"maxLength\":4}");

    cmcp_json_t *too_short = J("\"a\"");
    cmcp_json_t *just_min  = J("\"ab\"");
    cmcp_json_t *just_max  = J("\"abcd\"");
    cmcp_json_t *too_long  = J("\"abcde\"");

    cmcp_schema_error_t e;
    TEST_ASSERT(cmcp_schema_validate(schema, too_short, &e) == CMCP_ESCHEMA);
    TEST_ASSERT(e.keyword && strcmp(e.keyword, "minLength") == 0);
    cmcp_schema_error_clear(&e);

    TEST_ASSERT(cmcp_schema_validate(schema, just_min, NULL) == CMCP_OK);
    TEST_ASSERT(cmcp_schema_validate(schema, just_max, NULL) == CMCP_OK);

    TEST_ASSERT(cmcp_schema_validate(schema, too_long, &e) == CMCP_ESCHEMA);
    TEST_ASSERT(e.keyword && strcmp(e.keyword, "maxLength") == 0);
    cmcp_schema_error_clear(&e);

    cmcp_json_free(schema);
    cmcp_json_free(too_short); cmcp_json_free(just_min);
    cmcp_json_free(just_max);  cmcp_json_free(too_long);
}

/* UTF-8: minLength counts code points, not bytes. "café" = 4 cps but 5
 * bytes (é = U+00E9 = 0xC3 0xA9). Encoded literally to keep the test
 * source ASCII-only. */
static void test_string_length_unicode(void) {
    cmcp_json_t *schema = J("{\"type\":\"string\",\"minLength\":4}");
    cmcp_json_t *cafe   = J("\"caf\xc3\xa9\"");      /* 4 cps, 5 bytes */
    cmcp_json_t *cafe3  = J("\"caf\"");              /* 3 cps */

    TEST_ASSERT(cmcp_schema_validate(schema, cafe, NULL) == CMCP_OK);
    TEST_ASSERT(cmcp_schema_validate(schema, cafe3, NULL) == CMCP_ESCHEMA);

    cmcp_json_free(schema); cmcp_json_free(cafe); cmcp_json_free(cafe3);
}

/* ====================================================================== */
/* Number range                                                            */
/* ====================================================================== */

static void test_number_range(void) {
    cmcp_json_t *schema = J(
        "{\"type\":\"integer\",\"minimum\":1,\"maximum\":100}");

    cmcp_json_t *lo = J("0");
    cmcp_json_t *hi = J("101");
    cmcp_json_t *ok = J("50");
    cmcp_json_t *eq_min = J("1");
    cmcp_json_t *eq_max = J("100");

    cmcp_schema_error_t e;
    TEST_ASSERT(cmcp_schema_validate(schema, lo, &e) == CMCP_ESCHEMA);
    TEST_ASSERT(e.keyword && strcmp(e.keyword, "minimum") == 0);
    cmcp_schema_error_clear(&e);

    TEST_ASSERT(cmcp_schema_validate(schema, hi, &e) == CMCP_ESCHEMA);
    TEST_ASSERT(e.keyword && strcmp(e.keyword, "maximum") == 0);
    cmcp_schema_error_clear(&e);

    TEST_ASSERT(cmcp_schema_validate(schema, ok, NULL) == CMCP_OK);
    TEST_ASSERT(cmcp_schema_validate(schema, eq_min, NULL) == CMCP_OK);
    TEST_ASSERT(cmcp_schema_validate(schema, eq_max, NULL) == CMCP_OK);

    cmcp_json_free(schema);
    cmcp_json_free(lo); cmcp_json_free(hi); cmcp_json_free(ok);
    cmcp_json_free(eq_min); cmcp_json_free(eq_max);
}

static void test_number_range_double(void) {
    cmcp_json_t *schema = J(
        "{\"type\":\"number\",\"minimum\":0.5,\"maximum\":1.5}");

    cmcp_json_t *ok  = J("1.0");
    cmcp_json_t *lo  = J("0.49");
    cmcp_json_t *hi  = J("1.51");

    TEST_ASSERT(cmcp_schema_validate(schema, ok, NULL) == CMCP_OK);
    TEST_ASSERT(cmcp_schema_validate(schema, lo, NULL) == CMCP_ESCHEMA);
    TEST_ASSERT(cmcp_schema_validate(schema, hi, NULL) == CMCP_ESCHEMA);

    cmcp_json_free(schema); cmcp_json_free(ok);
    cmcp_json_free(lo); cmcp_json_free(hi);
}

/* ====================================================================== */
/* items (array element schema)                                            */
/* ====================================================================== */

static void test_items(void) {
    cmcp_json_t *schema = J(
        "{\"type\":\"array\",\"items\":{\"type\":\"integer\"}}");

    cmcp_json_t *ok  = J("[1,2,3]");
    cmcp_json_t *bad = J("[1,2,\"oops\"]");

    TEST_ASSERT(cmcp_schema_validate(schema, ok, NULL) == CMCP_OK);

    cmcp_schema_error_t e;
    TEST_ASSERT(cmcp_schema_validate(schema, bad, &e) == CMCP_ESCHEMA);
    TEST_ASSERT(e.keyword && strcmp(e.keyword, "type") == 0);
    /* Path points at the offending index. */
    TEST_ASSERT(e.path && strcmp(e.path, "/2") == 0);
    cmcp_schema_error_clear(&e);

    cmcp_json_free(schema); cmcp_json_free(ok); cmcp_json_free(bad);
}

/* ====================================================================== */
/* Path escaping                                                           */
/* ====================================================================== */

/* RFC 6901: `~` escapes to `~0`, `/` to `~1`. */
static void test_path_escape(void) {
    cmcp_json_t *schema = J(
        "{\"type\":\"object\","
         "\"properties\":{"
            "\"a/b\":{\"type\":\"integer\"},"
            "\"c~d\":{\"type\":\"integer\"}"
         "}}");

    cmcp_json_t *bad1 = J("{\"a/b\":\"x\"}");
    cmcp_json_t *bad2 = J("{\"c~d\":\"y\"}");

    cmcp_schema_error_t e;
    TEST_ASSERT(cmcp_schema_validate(schema, bad1, &e) == CMCP_ESCHEMA);
    TEST_ASSERT(e.path && strcmp(e.path, "/a~1b") == 0);
    cmcp_schema_error_clear(&e);

    TEST_ASSERT(cmcp_schema_validate(schema, bad2, &e) == CMCP_ESCHEMA);
    TEST_ASSERT(e.path && strcmp(e.path, "/c~0d") == 0);
    cmcp_schema_error_clear(&e);

    cmcp_json_free(schema); cmcp_json_free(bad1); cmcp_json_free(bad2);
}

/* ====================================================================== */
/* Deep nesting                                                            */
/* ====================================================================== */

static void test_nested_path(void) {
    cmcp_json_t *schema = J(
        "{\"type\":\"object\","
         "\"properties\":{"
            "\"users\":{"
                "\"type\":\"array\","
                "\"items\":{"
                    "\"type\":\"object\","
                    "\"properties\":{"
                        "\"name\":{\"type\":\"string\"},"
                        "\"age\":{\"type\":\"integer\"}"
                    "},"
                    "\"required\":[\"name\"]"
                "}"
            "}"
         "}}");

    cmcp_json_t *ok  = J(
        "{\"users\":[{\"name\":\"a\",\"age\":1},{\"name\":\"b\"}]}");
    cmcp_json_t *bad = J(
        "{\"users\":[{\"name\":\"a\"},{\"age\":2}]}");

    TEST_ASSERT(cmcp_schema_validate(schema, ok, NULL) == CMCP_OK);

    cmcp_schema_error_t e;
    TEST_ASSERT(cmcp_schema_validate(schema, bad, &e) == CMCP_ESCHEMA);
    TEST_ASSERT(e.keyword && strcmp(e.keyword, "required") == 0);
    TEST_ASSERT(e.path && strcmp(e.path, "/users/1/name") == 0);
    cmcp_schema_error_clear(&e);

    cmcp_json_free(schema); cmcp_json_free(ok); cmcp_json_free(bad);
}

/* ====================================================================== */
/* error_to_json                                                           */
/* ====================================================================== */

static void test_error_to_json(void) {
    cmcp_json_t *schema = J("{\"type\":\"string\"}");
    cmcp_json_t *bad    = J("42");

    cmcp_schema_error_t e;
    TEST_ASSERT(cmcp_schema_validate(schema, bad, &e) == CMCP_ESCHEMA);

    cmcp_json_t *data = cmcp_schema_error_to_json(&e);
    TEST_ASSERT(data && data->type == CMCP_JSON_OBJECT);
    const cmcp_json_t *p = cmcp_json_object_get(data, "path");
    const cmcp_json_t *k = cmcp_json_object_get(data, "keyword");
    const cmcp_json_t *m = cmcp_json_object_get(data, "message");
    TEST_ASSERT(p && p->type == CMCP_JSON_STRING);
    TEST_ASSERT(k && k->type == CMCP_JSON_STRING);
    TEST_ASSERT(strcmp(k->str.s, "type") == 0);
    TEST_ASSERT(m && m->type == CMCP_JSON_STRING);

    cmcp_json_free(data);
    cmcp_schema_error_clear(&e);
    cmcp_json_free(schema); cmcp_json_free(bad);
}

/* ====================================================================== */
/* Bad inputs                                                              */
/* ====================================================================== */

static void test_bad_inputs(void) {
    /* NULL schema → EINVAL. */
    TEST_ASSERT(cmcp_schema_validate(NULL, NULL, NULL) == CMCP_EINVAL);

    /* Non-object schema → EINVAL. */
    cmcp_json_t *not_object = J("[1,2,3]");
    TEST_ASSERT(cmcp_schema_validate(not_object, NULL, NULL) == CMCP_EINVAL);
    cmcp_json_free(not_object);

    /* Empty schema → accepts everything. */
    cmcp_json_t *empty = J("{}");
    cmcp_json_t *anything = J("[1,\"two\",{\"x\":3.5}]");
    TEST_ASSERT(cmcp_schema_validate(empty, anything, NULL) == CMCP_OK);
    cmcp_json_free(empty); cmcp_json_free(anything);
}

/* ====================================================================== */
/* Pipe-pair scaffolding (mirrors test_tools).                             */
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

static int echo_handler(const cmcp_json_t *args, void *userdata,
                         cmcp_handler_ctx_t *hctx,
                         cmcp_json_t **out_content, int *out_is_error) {
    (void)userdata; (void)hctx;
    *out_is_error = 0;
    const cmcp_json_t *t = args ? cmcp_json_object_get(args, "text") : NULL;
    const char *s = (t && t->type == CMCP_JSON_STRING) ? t->str.s : "";
    *out_content = cmcp_tool_text_content(s);
    return CMCP_OK;
}

static cmcp_json_t *call_args_object(const char *name, cmcp_json_t *arguments) {
    cmcp_json_t *p = cmcp_json_new_object();
    cmcp_json_object_set(p, "name", cmcp_json_new_string(name));
    if (arguments) cmcp_json_object_set(p, "arguments", arguments);
    return p;
}

/* ====================================================================== */
/* Integration: tools/call rejects bad args at the wire                    */
/* ====================================================================== */

/* Bad args (wrong type for `text`) → MCP 2025-11-25 returns
 * `{isError: true, content: [text]}` (tool-execution error, not
 * JSON-RPC error). The text content names the offending path and
 * keyword so the model can self-correct. */
static void test_tools_call_schema_violation(void) {
    transport_pair_t p;
    TEST_ASSERT(make_pair(&p) == 0);

    cmcp_server_t *srv = cmcp_server_new("test-server", "0.1.0");
    cmcp_server_add_tool(srv, &(cmcp_tool_t){
        .name = "echo",
        .input_schema = "{\"type\":\"object\","
                         "\"properties\":{\"text\":{\"type\":\"string\"}},"
                         "\"required\":[\"text\"]}",
        .handler = echo_handler,
    });

    server_arg_t sa = { srv, p.server_t, 0 };
    pthread_t th;
    pthread_create(&th, NULL, server_thread, &sa);

    cmcp_client_t *cli = cmcp_client_new("test-client", "0.0.1");
    TEST_ASSERT(cmcp_client_handshake(cli, p.client_t) == CMCP_OK);

    /* `text` is an integer, not a string → schema rejects. */
    cmcp_json_t *args = cmcp_json_new_object();
    cmcp_json_object_set(args, "text", cmcp_json_new_int(42));

    cmcp_rpc_message_t resp;
    TEST_ASSERT(cmcp_client_request(cli, "tools/call",
                                     call_args_object("echo", args),
                                     &resp) == CMCP_OK);
    TEST_ASSERT(resp.error == NULL);
    TEST_ASSERT(resp.result != NULL && resp.result->type == CMCP_JSON_OBJECT);
    const cmcp_json_t *is_err = cmcp_json_object_get(resp.result, "isError");
    TEST_ASSERT(is_err && is_err->type == CMCP_JSON_BOOL && is_err->b == 1);
    const cmcp_json_t *content = cmcp_json_object_get(resp.result, "content");
    TEST_ASSERT(content && content->type == CMCP_JSON_ARRAY && content->arr.len == 1);
    const cmcp_json_t *txt = cmcp_json_object_get(content->arr.items[0], "text");
    TEST_ASSERT(txt && txt->type == CMCP_JSON_STRING);
    /* The rendered message names the path + keyword so the model can fix it. */
    TEST_ASSERT(strstr(txt->str.s, "/text")   != NULL);
    TEST_ASSERT(strstr(txt->str.s, "type")    != NULL);
    cmcp_rpc_message_clear(&resp);

    cmcp_client_free(cli);
    cmcp_transport_close(p.client_t);
    pthread_join(th, NULL);
    cmcp_server_free(srv);
    cmcp_transport_close(p.server_t);
}

/* No `arguments` at all when schema has required → isError result
 * (same wire shape as a type-violation; the model is expected to
 * notice and supply the missing arg on retry). */
static void test_tools_call_missing_arguments(void) {
    transport_pair_t p;
    TEST_ASSERT(make_pair(&p) == 0);

    cmcp_server_t *srv = cmcp_server_new("test-server", "0.1.0");
    cmcp_server_add_tool(srv, &(cmcp_tool_t){
        .name = "echo",
        .input_schema = "{\"type\":\"object\","
                         "\"properties\":{\"text\":{\"type\":\"string\"}},"
                         "\"required\":[\"text\"]}",
        .handler = echo_handler,
    });

    server_arg_t sa = { srv, p.server_t, 0 };
    pthread_t th;
    pthread_create(&th, NULL, server_thread, &sa);

    cmcp_client_t *cli = cmcp_client_new("test-client", "0.0.1");
    TEST_ASSERT(cmcp_client_handshake(cli, p.client_t) == CMCP_OK);

    cmcp_rpc_message_t resp;
    TEST_ASSERT(cmcp_client_request(cli, "tools/call",
                                     call_args_object("echo", NULL),
                                     &resp) == CMCP_OK);
    TEST_ASSERT(resp.error == NULL);
    const cmcp_json_t *is_err = cmcp_json_object_get(resp.result, "isError");
    TEST_ASSERT(is_err && is_err->type == CMCP_JSON_BOOL && is_err->b == 1);
    cmcp_rpc_message_clear(&resp);

    cmcp_client_free(cli);
    cmcp_transport_close(p.client_t);
    pthread_join(th, NULL);
    cmcp_server_free(srv);
    cmcp_transport_close(p.server_t);
}

/* Tool with no schema accepts anything (schema validation skipped). */
static void test_tools_call_no_schema_accepts_anything(void) {
    transport_pair_t p;
    TEST_ASSERT(make_pair(&p) == 0);

    cmcp_server_t *srv = cmcp_server_new("test-server", "0.1.0");
    cmcp_server_add_tool(srv, &(cmcp_tool_t){
        .name = "echo",
        .input_schema = NULL,        /* opt-out of validation */
        .handler = echo_handler,
    });

    server_arg_t sa = { srv, p.server_t, 0 };
    pthread_t th;
    pthread_create(&th, NULL, server_thread, &sa);

    cmcp_client_t *cli = cmcp_client_new("test-client", "0.0.1");
    TEST_ASSERT(cmcp_client_handshake(cli, p.client_t) == CMCP_OK);

    /* Sends an integer where the handler expects a string — handler is
     * permissive, schema is absent, so we get a successful response. */
    cmcp_json_t *args = cmcp_json_new_object();
    cmcp_json_object_set(args, "text", cmcp_json_new_int(7));

    cmcp_rpc_message_t resp;
    TEST_ASSERT(cmcp_client_request(cli, "tools/call",
                                     call_args_object("echo", args),
                                     &resp) == CMCP_OK);
    TEST_ASSERT(resp.error == NULL);
    TEST_ASSERT(resp.result != NULL);
    cmcp_rpc_message_clear(&resp);

    cmcp_client_free(cli);
    cmcp_transport_close(p.client_t);
    pthread_join(th, NULL);
    cmcp_server_free(srv);
    cmcp_transport_close(p.server_t);
}

/* ====================================================================== */
/* 6.7 additions: const, pattern, multipleOf, exclusiveBounds,             */
/* min/maxItems, uniqueItems, min/maxProperties, propertyNames,            */
/* patternProperties, additionalProperties-subschema, prefixItems,         */
/* tuple-form items + additionalItems, allOf/anyOf/oneOf/not,              */
/* if/then/else, boolean schemas.                                          */
/* ====================================================================== */

static void test_const(void) {
    cmcp_json_t *s = J("{\"const\":42}");
    cmcp_json_t *ok = J("42");
    cmcp_json_t *bad = J("43");
    cmcp_schema_error_t e;
    TEST_ASSERT(cmcp_schema_validate(s, ok, NULL) == CMCP_OK);
    TEST_ASSERT(cmcp_schema_validate(s, bad, &e) == CMCP_ESCHEMA);
    TEST_ASSERT(e.keyword && strcmp(e.keyword, "const") == 0);
    cmcp_schema_error_clear(&e);
    cmcp_json_free(s); cmcp_json_free(ok); cmcp_json_free(bad);
}

static void test_const_object(void) {
    cmcp_json_t *s   = J("{\"const\":{\"a\":1,\"b\":\"x\"}}");
    cmcp_json_t *ok  = J("{\"a\":1,\"b\":\"x\"}");
    cmcp_json_t *bad = J("{\"a\":1,\"b\":\"y\"}");
    TEST_ASSERT(cmcp_schema_validate(s, ok, NULL) == CMCP_OK);
    TEST_ASSERT(cmcp_schema_validate(s, bad, NULL) == CMCP_ESCHEMA);
    cmcp_json_free(s); cmcp_json_free(ok); cmcp_json_free(bad);
}

static void test_pattern(void) {
    cmcp_json_t *s = J("{\"pattern\":\"^[A-Z][a-z]+$\"}");
    cmcp_json_t *ok = J("\"Hello\"");
    cmcp_json_t *bad = J("\"hello\"");
    cmcp_schema_error_t e;
    TEST_ASSERT(cmcp_schema_validate(s, ok, NULL) == CMCP_OK);
    TEST_ASSERT(cmcp_schema_validate(s, bad, &e) == CMCP_ESCHEMA);
    TEST_ASSERT(e.keyword && strcmp(e.keyword, "pattern") == 0);
    cmcp_schema_error_clear(&e);
    cmcp_json_free(s); cmcp_json_free(ok); cmcp_json_free(bad);
}

static void test_pattern_does_not_apply_to_non_strings(void) {
    cmcp_json_t *s   = J("{\"pattern\":\"^[A-Z]+$\"}");
    cmcp_json_t *num = J("42");
    /* Pattern is only meaningful for strings. */
    TEST_ASSERT(cmcp_schema_validate(s, num, NULL) == CMCP_OK);
    cmcp_json_free(s); cmcp_json_free(num);
}

static void test_pattern_invalid_regex_silently_accepts(void) {
    /* Malformed pattern is a schema author error; we accept the value
     * rather than reject every string. */
    cmcp_json_t *s = J("{\"pattern\":\"[unterminated\"}");
    cmcp_json_t *v = J("\"anything\"");
    TEST_ASSERT(cmcp_schema_validate(s, v, NULL) == CMCP_OK);
    cmcp_json_free(s); cmcp_json_free(v);
}

static void test_multipleof_integer(void) {
    cmcp_json_t *s = J("{\"multipleOf\":3}");
    cmcp_json_t *ok = J("9");
    cmcp_json_t *bad = J("10");
    TEST_ASSERT(cmcp_schema_validate(s, ok, NULL) == CMCP_OK);
    TEST_ASSERT(cmcp_schema_validate(s, bad, NULL) == CMCP_ESCHEMA);
    cmcp_json_free(s); cmcp_json_free(ok); cmcp_json_free(bad);
}

static void test_multipleof_fraction(void) {
    cmcp_json_t *s = J("{\"multipleOf\":0.5}");
    cmcp_json_t *ok = J("1.5");
    cmcp_json_t *bad = J("1.7");
    TEST_ASSERT(cmcp_schema_validate(s, ok, NULL) == CMCP_OK);
    TEST_ASSERT(cmcp_schema_validate(s, bad, NULL) == CMCP_ESCHEMA);
    cmcp_json_free(s); cmcp_json_free(ok); cmcp_json_free(bad);
}

static void test_exclusive_bounds(void) {
    cmcp_json_t *s = J("{\"exclusiveMinimum\":0,\"exclusiveMaximum\":10}");
    cmcp_json_t *ok = J("5");
    cmcp_json_t *zero = J("0");
    cmcp_json_t *ten  = J("10");
    TEST_ASSERT(cmcp_schema_validate(s, ok, NULL) == CMCP_OK);
    TEST_ASSERT(cmcp_schema_validate(s, zero, NULL) == CMCP_ESCHEMA);
    TEST_ASSERT(cmcp_schema_validate(s, ten,  NULL) == CMCP_ESCHEMA);
    cmcp_json_free(s); cmcp_json_free(ok); cmcp_json_free(zero); cmcp_json_free(ten);
}

static void test_min_max_items(void) {
    cmcp_json_t *s = J("{\"minItems\":2,\"maxItems\":4}");
    cmcp_json_t *too_short = J("[1]");
    cmcp_json_t *ok        = J("[1,2,3]");
    cmcp_json_t *too_long  = J("[1,2,3,4,5]");
    TEST_ASSERT(cmcp_schema_validate(s, too_short, NULL) == CMCP_ESCHEMA);
    TEST_ASSERT(cmcp_schema_validate(s, ok,        NULL) == CMCP_OK);
    TEST_ASSERT(cmcp_schema_validate(s, too_long,  NULL) == CMCP_ESCHEMA);
    cmcp_json_free(s); cmcp_json_free(too_short);
    cmcp_json_free(ok); cmcp_json_free(too_long);
}

static void test_unique_items(void) {
    cmcp_json_t *s = J("{\"uniqueItems\":true}");
    cmcp_json_t *ok = J("[1,2,3]");
    cmcp_json_t *bad = J("[1,2,2,3]");
    cmcp_json_t *deep_bad = J("[{\"x\":1},{\"x\":1}]");
    cmcp_schema_error_t e;
    TEST_ASSERT(cmcp_schema_validate(s, ok, NULL) == CMCP_OK);
    TEST_ASSERT(cmcp_schema_validate(s, bad, &e) == CMCP_ESCHEMA);
    TEST_ASSERT(e.keyword && strcmp(e.keyword, "uniqueItems") == 0);
    cmcp_schema_error_clear(&e);
    TEST_ASSERT(cmcp_schema_validate(s, deep_bad, NULL) == CMCP_ESCHEMA);
    cmcp_json_free(s); cmcp_json_free(ok);
    cmcp_json_free(bad); cmcp_json_free(deep_bad);
}

static void test_min_max_properties(void) {
    cmcp_json_t *s = J("{\"minProperties\":1,\"maxProperties\":2}");
    cmcp_json_t *empty = J("{}");
    cmcp_json_t *ok    = J("{\"a\":1,\"b\":2}");
    cmcp_json_t *big   = J("{\"a\":1,\"b\":2,\"c\":3}");
    TEST_ASSERT(cmcp_schema_validate(s, empty, NULL) == CMCP_ESCHEMA);
    TEST_ASSERT(cmcp_schema_validate(s, ok,    NULL) == CMCP_OK);
    TEST_ASSERT(cmcp_schema_validate(s, big,   NULL) == CMCP_ESCHEMA);
    cmcp_json_free(s); cmcp_json_free(empty);
    cmcp_json_free(ok); cmcp_json_free(big);
}

static void test_property_names(void) {
    cmcp_json_t *s = J(
        "{\"propertyNames\":{\"pattern\":\"^[a-z]+$\"}}");
    cmcp_json_t *ok = J("{\"foo\":1,\"bar\":2}");
    cmcp_json_t *bad = J("{\"foo\":1,\"Bar\":2}");
    cmcp_schema_error_t e;
    TEST_ASSERT(cmcp_schema_validate(s, ok, NULL) == CMCP_OK);
    TEST_ASSERT(cmcp_schema_validate(s, bad, &e) == CMCP_ESCHEMA);
    TEST_ASSERT(e.keyword && strcmp(e.keyword, "propertyNames") == 0);
    cmcp_schema_error_clear(&e);
    cmcp_json_free(s); cmcp_json_free(ok); cmcp_json_free(bad);
}

static void test_pattern_properties(void) {
    cmcp_json_t *s = J(
        "{\"patternProperties\":{\"^x_\":{\"type\":\"integer\"}}}");
    cmcp_json_t *ok      = J("{\"x_count\":3,\"name\":\"any\"}");
    cmcp_json_t *bad     = J("{\"x_count\":\"not-an-int\"}");
    TEST_ASSERT(cmcp_schema_validate(s, ok, NULL) == CMCP_OK);
    TEST_ASSERT(cmcp_schema_validate(s, bad, NULL) == CMCP_ESCHEMA);
    cmcp_json_free(s); cmcp_json_free(ok); cmcp_json_free(bad);
}

static void test_additional_properties_subschema(void) {
    /* properties pins `a` as integer; additionalProperties pins
     * everything else as string. */
    cmcp_json_t *s = J(
        "{\"properties\":{\"a\":{\"type\":\"integer\"}},"
        " \"additionalProperties\":{\"type\":\"string\"}}");
    cmcp_json_t *ok = J("{\"a\":7,\"b\":\"hello\"}");
    cmcp_json_t *bad = J("{\"a\":7,\"b\":42}");
    TEST_ASSERT(cmcp_schema_validate(s, ok, NULL) == CMCP_OK);
    TEST_ASSERT(cmcp_schema_validate(s, bad, NULL) == CMCP_ESCHEMA);
    cmcp_json_free(s); cmcp_json_free(ok); cmcp_json_free(bad);
}

static void test_prefix_items(void) {
    cmcp_json_t *s = J(
        "{\"prefixItems\":["
        "  {\"type\":\"string\"},"
        "  {\"type\":\"integer\"}"
        "]}");
    cmcp_json_t *ok  = J("[\"hi\",42]");
    cmcp_json_t *bad = J("[\"hi\",\"nope\"]");
    cmcp_json_t *extra = J("[\"hi\",42,\"trailing\"]"); /* allowed by default */
    TEST_ASSERT(cmcp_schema_validate(s, ok, NULL) == CMCP_OK);
    TEST_ASSERT(cmcp_schema_validate(s, bad, NULL) == CMCP_ESCHEMA);
    TEST_ASSERT(cmcp_schema_validate(s, extra, NULL) == CMCP_OK);
    cmcp_json_free(s); cmcp_json_free(ok); cmcp_json_free(bad); cmcp_json_free(extra);
}

static void test_prefix_items_with_items_for_rest(void) {
    /* 2020-12 shape: prefixItems for positions, items for the rest. */
    cmcp_json_t *s = J(
        "{\"prefixItems\":[{\"type\":\"string\"}],"
        " \"items\":{\"type\":\"integer\"}}");
    cmcp_json_t *ok  = J("[\"x\",1,2,3]");
    cmcp_json_t *bad = J("[\"x\",1,\"two\"]");
    TEST_ASSERT(cmcp_schema_validate(s, ok, NULL) == CMCP_OK);
    TEST_ASSERT(cmcp_schema_validate(s, bad, NULL) == CMCP_ESCHEMA);
    cmcp_json_free(s); cmcp_json_free(ok); cmcp_json_free(bad);
}

static void test_tuple_items_draft07(void) {
    /* Draft-07 shape: `items` is an array (tuple) + `additionalItems`
     * for the rest. */
    cmcp_json_t *s = J(
        "{\"items\":[{\"type\":\"string\"},{\"type\":\"integer\"}],"
        " \"additionalItems\":false}");
    cmcp_json_t *ok    = J("[\"x\",7]");
    cmcp_json_t *extra = J("[\"x\",7,42]");
    cmcp_schema_error_t e;
    TEST_ASSERT(cmcp_schema_validate(s, ok, NULL) == CMCP_OK);
    TEST_ASSERT(cmcp_schema_validate(s, extra, &e) == CMCP_ESCHEMA);
    TEST_ASSERT(e.keyword && strcmp(e.keyword, "additionalItems") == 0);
    cmcp_schema_error_clear(&e);
    cmcp_json_free(s); cmcp_json_free(ok); cmcp_json_free(extra);
}

static void test_all_of(void) {
    cmcp_json_t *s = J(
        "{\"allOf\":["
        "  {\"type\":\"integer\"},"
        "  {\"minimum\":10},"
        "  {\"maximum\":100}"
        "]}");
    cmcp_json_t *ok  = J("50");
    cmcp_json_t *low = J("5");
    cmcp_json_t *str = J("\"nope\"");
    TEST_ASSERT(cmcp_schema_validate(s, ok, NULL) == CMCP_OK);
    TEST_ASSERT(cmcp_schema_validate(s, low, NULL) == CMCP_ESCHEMA);
    TEST_ASSERT(cmcp_schema_validate(s, str, NULL) == CMCP_ESCHEMA);
    cmcp_json_free(s); cmcp_json_free(ok); cmcp_json_free(low); cmcp_json_free(str);
}

static void test_any_of(void) {
    cmcp_json_t *s = J(
        "{\"anyOf\":["
        "  {\"type\":\"string\"},"
        "  {\"type\":\"integer\",\"minimum\":0}"
        "]}");
    cmcp_json_t *str  = J("\"hi\"");
    cmcp_json_t *nat  = J("3");
    cmcp_json_t *neg  = J("-1");
    cmcp_json_t *bool_ = J("true");
    cmcp_schema_error_t e;
    TEST_ASSERT(cmcp_schema_validate(s, str, NULL) == CMCP_OK);
    TEST_ASSERT(cmcp_schema_validate(s, nat, NULL) == CMCP_OK);
    TEST_ASSERT(cmcp_schema_validate(s, neg, NULL) == CMCP_ESCHEMA);
    TEST_ASSERT(cmcp_schema_validate(s, bool_, &e) == CMCP_ESCHEMA);
    TEST_ASSERT(e.keyword && strcmp(e.keyword, "anyOf") == 0);
    cmcp_schema_error_clear(&e);
    cmcp_json_free(s); cmcp_json_free(str); cmcp_json_free(nat);
    cmcp_json_free(neg); cmcp_json_free(bool_);
}

static void test_one_of(void) {
    /* Disjoint branches: integer OR string. */
    cmcp_json_t *s = J(
        "{\"oneOf\":["
        "  {\"type\":\"integer\"},"
        "  {\"type\":\"string\"}"
        "]}");
    cmcp_json_t *i   = J("3");
    cmcp_json_t *str = J("\"x\"");
    cmcp_json_t *b   = J("true");
    TEST_ASSERT(cmcp_schema_validate(s, i, NULL) == CMCP_OK);
    TEST_ASSERT(cmcp_schema_validate(s, str, NULL) == CMCP_OK);
    TEST_ASSERT(cmcp_schema_validate(s, b, NULL) == CMCP_ESCHEMA);

    /* Overlapping branches: must fail because TWO match. */
    cmcp_json_t *over = J(
        "{\"oneOf\":["
        "  {\"type\":\"integer\"},"
        "  {\"type\":\"integer\",\"minimum\":0}"
        "]}");
    cmcp_json_t *positive = J("5");
    cmcp_schema_error_t e;
    TEST_ASSERT(cmcp_schema_validate(over, positive, &e) == CMCP_ESCHEMA);
    TEST_ASSERT(e.keyword && strcmp(e.keyword, "oneOf") == 0);
    cmcp_schema_error_clear(&e);

    cmcp_json_free(s); cmcp_json_free(i); cmcp_json_free(str); cmcp_json_free(b);
    cmcp_json_free(over); cmcp_json_free(positive);
}

static void test_not(void) {
    cmcp_json_t *s = J("{\"not\":{\"type\":\"string\"}}");
    cmcp_json_t *str = J("\"nope\"");
    cmcp_json_t *num = J("42");
    cmcp_schema_error_t e;
    TEST_ASSERT(cmcp_schema_validate(s, num, NULL) == CMCP_OK);
    TEST_ASSERT(cmcp_schema_validate(s, str, &e) == CMCP_ESCHEMA);
    TEST_ASSERT(e.keyword && strcmp(e.keyword, "not") == 0);
    cmcp_schema_error_clear(&e);
    cmcp_json_free(s); cmcp_json_free(str); cmcp_json_free(num);
}

static void test_if_then_else(void) {
    /* If object has key `kind == "user"`, require key `email`.
     * Else, require key `id`. */
    cmcp_json_t *s = J(
        "{\"if\":{\"properties\":{\"kind\":{\"const\":\"user\"}},"
                 "\"required\":[\"kind\"]},"
        " \"then\":{\"required\":[\"email\"]},"
        " \"else\":{\"required\":[\"id\"]}}");
    cmcp_json_t *user_ok   = J("{\"kind\":\"user\",\"email\":\"a@b\"}");
    cmcp_json_t *user_bad  = J("{\"kind\":\"user\"}");
    cmcp_json_t *other_ok  = J("{\"kind\":\"bot\",\"id\":1}");
    cmcp_json_t *other_bad = J("{\"kind\":\"bot\"}");
    TEST_ASSERT(cmcp_schema_validate(s, user_ok, NULL) == CMCP_OK);
    TEST_ASSERT(cmcp_schema_validate(s, user_bad, NULL) == CMCP_ESCHEMA);
    TEST_ASSERT(cmcp_schema_validate(s, other_ok, NULL) == CMCP_OK);
    TEST_ASSERT(cmcp_schema_validate(s, other_bad, NULL) == CMCP_ESCHEMA);
    cmcp_json_free(s); cmcp_json_free(user_ok); cmcp_json_free(user_bad);
    cmcp_json_free(other_ok); cmcp_json_free(other_bad);
}

static void test_boolean_schema_true_false(void) {
    /* Top-level boolean schemas. */
    cmcp_json_t *yes = J("true");
    cmcp_json_t *no  = J("false");
    cmcp_json_t *v   = J("42");
    TEST_ASSERT(cmcp_schema_validate(yes, v, NULL) == CMCP_OK);
    TEST_ASSERT(cmcp_schema_validate(no,  v, NULL) == CMCP_ESCHEMA);

    /* Boolean subschemas in additionalProperties: true accepts anything,
     * false rejects any unlisted property. */
    cmcp_json_t *open  = J(
        "{\"properties\":{\"a\":{\"type\":\"integer\"}},"
        " \"additionalProperties\":true}");
    cmcp_json_t *closed = J(
        "{\"properties\":{\"a\":{\"type\":\"integer\"}},"
        " \"additionalProperties\":false}");
    cmcp_json_t *val = J("{\"a\":1,\"extra\":\"anything\"}");
    TEST_ASSERT(cmcp_schema_validate(open, val, NULL) == CMCP_OK);
    TEST_ASSERT(cmcp_schema_validate(closed, val, NULL) == CMCP_ESCHEMA);

    cmcp_json_free(yes); cmcp_json_free(no); cmcp_json_free(v);
    cmcp_json_free(open); cmcp_json_free(closed); cmcp_json_free(val);
}

/* ====================================================================== */
/* $ref + $defs                                                            */
/* ====================================================================== */

static void test_ref_basic(void) {
    /* The classic example: definitions block + a $ref into it. */
    cmcp_json_t *schema = J(
        "{\"$defs\":{\"PositiveInt\":"
                    "{\"type\":\"integer\",\"minimum\":1}},"
         "\"$ref\":\"#/$defs/PositiveInt\"}");
    cmcp_json_t *ok  = J("5");
    cmcp_json_t *bad = J("0");
    TEST_ASSERT(cmcp_schema_validate(schema, ok,  NULL) == CMCP_OK);
    TEST_ASSERT(cmcp_schema_validate(schema, bad, NULL) == CMCP_ESCHEMA);
    cmcp_json_free(schema); cmcp_json_free(ok); cmcp_json_free(bad);
}

static void test_ref_definitions_draft07_alias(void) {
    /* Draft-07 schemas use `definitions` instead of `$defs`. We resolve
     * it the same way — the path is just keys, no semantic difference. */
    cmcp_json_t *schema = J(
        "{\"definitions\":{\"Color\":"
                    "{\"type\":\"string\",\"enum\":[\"red\",\"blue\"]}},"
         "\"$ref\":\"#/definitions/Color\"}");
    cmcp_json_t *ok  = J("\"red\"");
    cmcp_json_t *bad = J("\"green\"");
    TEST_ASSERT(cmcp_schema_validate(schema, ok,  NULL) == CMCP_OK);
    TEST_ASSERT(cmcp_schema_validate(schema, bad, NULL) == CMCP_ESCHEMA);
    cmcp_json_free(schema); cmcp_json_free(ok); cmcp_json_free(bad);
}

static void test_ref_root_self(void) {
    /* `#` resolves to root — a recursive shape that bottoms out on
     * `type: array` containing items matching the same root schema. */
    cmcp_json_t *schema = J(
        "{\"type\":\"array\",\"items\":{\"$ref\":\"#\"}}");
    cmcp_json_t *ok  = J("[[[],[]],[]]");
    cmcp_json_t *bad = J("[[\"oops\"]]");
    TEST_ASSERT(cmcp_schema_validate(schema, ok,  NULL) == CMCP_OK);
    TEST_ASSERT(cmcp_schema_validate(schema, bad, NULL) == CMCP_ESCHEMA);
    cmcp_json_free(schema); cmcp_json_free(ok); cmcp_json_free(bad);
}

static void test_ref_unresolvable(void) {
    /* A pointer into a missing key surfaces as a $ref error, not an
     * accept. */
    cmcp_json_t *schema = J(
        "{\"$ref\":\"#/$defs/Nope\"}");
    cmcp_json_t *v      = J("42");
    cmcp_schema_error_t e;
    TEST_ASSERT(cmcp_schema_validate(schema, v, &e) == CMCP_ESCHEMA);
    TEST_ASSERT(e.keyword && strcmp(e.keyword, "$ref") == 0);
    cmcp_schema_error_clear(&e);
    cmcp_json_free(schema); cmcp_json_free(v);
}

static void test_ref_cycle_bounded(void) {
    /* A direct ref cycle: validating against either side recurses
     * forever. The depth cap (CMCP_SCHEMA_MAX_DEPTH) must catch it
     * and surface a $ref error rather than blowing the stack. */
    cmcp_json_t *schema = J(
        "{\"$defs\":{\"X\":{\"$ref\":\"#/$defs/Y\"},"
                    "\"Y\":{\"$ref\":\"#/$defs/X\"}},"
         "\"$ref\":\"#/$defs/X\"}");
    cmcp_json_t *v = J("42");
    cmcp_schema_error_t e;
    TEST_ASSERT(cmcp_schema_validate(schema, v, &e) == CMCP_ESCHEMA);
    TEST_ASSERT(e.keyword && strcmp(e.keyword, "$ref") == 0);
    cmcp_schema_error_clear(&e);
    cmcp_json_free(schema); cmcp_json_free(v);
}

static void test_ref_sibling_keywords_honoured(void) {
    /* draft-2020-12 honours siblings of $ref. The target says
     * type:string; the sibling says minLength:3 — the value "hi" must
     * fail the sibling even though it satisfies the target. */
    cmcp_json_t *schema = J(
        "{\"$defs\":{\"S\":{\"type\":\"string\"}},"
         "\"$ref\":\"#/$defs/S\",\"minLength\":3}");
    cmcp_json_t *ok  = J("\"hello\"");
    cmcp_json_t *bad = J("\"hi\"");
    TEST_ASSERT(cmcp_schema_validate(schema, ok,  NULL) == CMCP_OK);
    TEST_ASSERT(cmcp_schema_validate(schema, bad, NULL) == CMCP_ESCHEMA);
    cmcp_json_free(schema); cmcp_json_free(ok); cmcp_json_free(bad);
}

/* ====================================================================== */
/* format (date-time, email, uri, uuid)                                    */
/* ====================================================================== */

static void test_format_date_time(void) {
    cmcp_json_t *schema = J(
        "{\"type\":\"string\",\"format\":\"date-time\"}");
    cmcp_json_t *ok1 = J("\"1985-04-12T23:20:50.52Z\"");
    cmcp_json_t *ok2 = J("\"2026-05-29T08:00:00+02:00\"");
    cmcp_json_t *bad1 = J("\"2026-05-29\"");        /* no time */
    cmcp_json_t *bad2 = J("\"2026/05/29T08:00:00Z\""); /* wrong separator */
    TEST_ASSERT(cmcp_schema_validate(schema, ok1,  NULL) == CMCP_OK);
    TEST_ASSERT(cmcp_schema_validate(schema, ok2,  NULL) == CMCP_OK);
    TEST_ASSERT(cmcp_schema_validate(schema, bad1, NULL) == CMCP_ESCHEMA);
    TEST_ASSERT(cmcp_schema_validate(schema, bad2, NULL) == CMCP_ESCHEMA);
    cmcp_json_free(schema);
    cmcp_json_free(ok1); cmcp_json_free(ok2);
    cmcp_json_free(bad1); cmcp_json_free(bad2);
}

static void test_format_email(void) {
    cmcp_json_t *schema = J(
        "{\"type\":\"string\",\"format\":\"email\"}");
    cmcp_json_t *ok  = J("\"user@example.com\"");
    cmcp_json_t *bad1 = J("\"no-at-sign\"");
    cmcp_json_t *bad2 = J("\"user@\"");
    cmcp_json_t *bad3 = J("\"user@no-dot\"");
    TEST_ASSERT(cmcp_schema_validate(schema, ok,   NULL) == CMCP_OK);
    TEST_ASSERT(cmcp_schema_validate(schema, bad1, NULL) == CMCP_ESCHEMA);
    TEST_ASSERT(cmcp_schema_validate(schema, bad2, NULL) == CMCP_ESCHEMA);
    TEST_ASSERT(cmcp_schema_validate(schema, bad3, NULL) == CMCP_ESCHEMA);
    cmcp_json_free(schema);
    cmcp_json_free(ok);
    cmcp_json_free(bad1); cmcp_json_free(bad2); cmcp_json_free(bad3);
}

static void test_format_uri(void) {
    cmcp_json_t *schema = J(
        "{\"type\":\"string\",\"format\":\"uri\"}");
    cmcp_json_t *ok1 = J("\"https://example.com/path?q=1\"");
    cmcp_json_t *ok2 = J("\"urn:isbn:0451450523\"");
    cmcp_json_t *bad1 = J("\"not a uri\"");
    cmcp_json_t *bad2 = J("\"://no-scheme\"");
    TEST_ASSERT(cmcp_schema_validate(schema, ok1,  NULL) == CMCP_OK);
    TEST_ASSERT(cmcp_schema_validate(schema, ok2,  NULL) == CMCP_OK);
    TEST_ASSERT(cmcp_schema_validate(schema, bad1, NULL) == CMCP_ESCHEMA);
    TEST_ASSERT(cmcp_schema_validate(schema, bad2, NULL) == CMCP_ESCHEMA);
    cmcp_json_free(schema);
    cmcp_json_free(ok1); cmcp_json_free(ok2);
    cmcp_json_free(bad1); cmcp_json_free(bad2);
}

static void test_format_uuid(void) {
    cmcp_json_t *schema = J(
        "{\"type\":\"string\",\"format\":\"uuid\"}");
    cmcp_json_t *ok1 = J("\"550e8400-e29b-41d4-a716-446655440000\"");
    cmcp_json_t *ok2 = J("\"550E8400-E29B-41D4-A716-446655440000\"");
    cmcp_json_t *bad1 = J("\"not-a-uuid\"");
    cmcp_json_t *bad2 = J("\"550e8400e29b41d4a716446655440000\""); /* no hyphens */
    TEST_ASSERT(cmcp_schema_validate(schema, ok1,  NULL) == CMCP_OK);
    TEST_ASSERT(cmcp_schema_validate(schema, ok2,  NULL) == CMCP_OK);
    TEST_ASSERT(cmcp_schema_validate(schema, bad1, NULL) == CMCP_ESCHEMA);
    TEST_ASSERT(cmcp_schema_validate(schema, bad2, NULL) == CMCP_ESCHEMA);
    cmcp_json_free(schema);
    cmcp_json_free(ok1); cmcp_json_free(ok2);
    cmcp_json_free(bad1); cmcp_json_free(bad2);
}

static void test_format_unknown_accepted(void) {
    /* An unknown format value is an annotation; the validator must NOT
     * reject it. Matches Ajv's default mode. */
    cmcp_json_t *schema = J(
        "{\"type\":\"string\",\"format\":\"hostname-with-extras\"}");
    cmcp_json_t *v = J("\"anything\"");
    TEST_ASSERT(cmcp_schema_validate(schema, v, NULL) == CMCP_OK);
    cmcp_json_free(schema); cmcp_json_free(v);
}

static void test_format_only_applies_to_strings(void) {
    /* Numbers / objects with `format` are a no-op — same posture as
     * `pattern`. */
    cmcp_json_t *schema = J("{\"format\":\"email\"}");
    cmcp_json_t *v      = J("42");
    TEST_ASSERT(cmcp_schema_validate(schema, v, NULL) == CMCP_OK);
    cmcp_json_free(schema); cmcp_json_free(v);
}

/* ====================================================================== */

int main(void) {
    fprintf(stderr, "test_schema:\n");

    /* Pure validator. */
    TEST_RUN(test_type_string);
    TEST_RUN(test_type_integer_vs_number);
    TEST_RUN(test_type_boolean_array_object_null);
    TEST_RUN(test_type_array_of_types);
    TEST_RUN(test_null_value);
    TEST_RUN(test_properties_recurse);
    TEST_RUN(test_required);
    TEST_RUN(test_required_with_null_value);
    TEST_RUN(test_additional_properties_false);
    TEST_RUN(test_enum);
    TEST_RUN(test_enum_mixed_types);
    TEST_RUN(test_string_length);
    TEST_RUN(test_string_length_unicode);
    TEST_RUN(test_number_range);
    TEST_RUN(test_number_range_double);
    TEST_RUN(test_items);
    TEST_RUN(test_path_escape);
    TEST_RUN(test_nested_path);
    TEST_RUN(test_error_to_json);
    TEST_RUN(test_bad_inputs);

    /* 6.7 additions. */
    TEST_RUN(test_const);
    TEST_RUN(test_const_object);
    TEST_RUN(test_pattern);
    TEST_RUN(test_pattern_does_not_apply_to_non_strings);
    TEST_RUN(test_pattern_invalid_regex_silently_accepts);
    TEST_RUN(test_multipleof_integer);
    TEST_RUN(test_multipleof_fraction);
    TEST_RUN(test_exclusive_bounds);
    TEST_RUN(test_min_max_items);
    TEST_RUN(test_unique_items);
    TEST_RUN(test_min_max_properties);
    TEST_RUN(test_property_names);
    TEST_RUN(test_pattern_properties);
    TEST_RUN(test_additional_properties_subschema);
    TEST_RUN(test_prefix_items);
    TEST_RUN(test_prefix_items_with_items_for_rest);
    TEST_RUN(test_tuple_items_draft07);
    TEST_RUN(test_all_of);
    TEST_RUN(test_any_of);
    TEST_RUN(test_one_of);
    TEST_RUN(test_not);
    TEST_RUN(test_if_then_else);
    TEST_RUN(test_boolean_schema_true_false);
    TEST_RUN(test_ref_basic);
    TEST_RUN(test_ref_definitions_draft07_alias);
    TEST_RUN(test_ref_root_self);
    TEST_RUN(test_ref_unresolvable);
    TEST_RUN(test_ref_cycle_bounded);
    TEST_RUN(test_ref_sibling_keywords_honoured);
    TEST_RUN(test_format_date_time);
    TEST_RUN(test_format_email);
    TEST_RUN(test_format_uri);
    TEST_RUN(test_format_uuid);
    TEST_RUN(test_format_unknown_accepted);
    TEST_RUN(test_format_only_applies_to_strings);

    /* Wire integration. */
    TEST_RUN(test_tools_call_schema_violation);
    TEST_RUN(test_tools_call_missing_arguments);
    TEST_RUN(test_tools_call_no_schema_accepts_anything);

    TEST_DONE();
}
