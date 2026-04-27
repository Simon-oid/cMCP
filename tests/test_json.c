#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "cmcp_json.h"
#include "test.h"

/* === parse: primitive types ============================================ */

static void test_parse_null(void) {
    cmcp_json_t *v = cmcp_json_parse_cstr("null");
    TEST_ASSERT(v != NULL);
    TEST_ASSERT(cmcp_json_is_null(v));
    cmcp_json_free(v);
}

static void test_parse_bools(void) {
    cmcp_json_t *t = cmcp_json_parse_cstr("true");
    cmcp_json_t *f = cmcp_json_parse_cstr("false");
    TEST_ASSERT(t && t->type == CMCP_JSON_BOOL && cmcp_json_bool(t) == 1);
    TEST_ASSERT(f && f->type == CMCP_JSON_BOOL && cmcp_json_bool(f) == 0);
    cmcp_json_free(t);
    cmcp_json_free(f);
}

static void test_parse_int(void) {
    cmcp_json_t *v = cmcp_json_parse_cstr("42");
    TEST_ASSERT(v && v->type == CMCP_JSON_INT && cmcp_json_int(v) == 42);
    cmcp_json_free(v);

    v = cmcp_json_parse_cstr("-17");
    TEST_ASSERT(v && cmcp_json_int(v) == -17);
    cmcp_json_free(v);

    v = cmcp_json_parse_cstr("0");
    TEST_ASSERT(v && cmcp_json_int(v) == 0);
    cmcp_json_free(v);
}

static void test_parse_double(void) {
    cmcp_json_t *v = cmcp_json_parse_cstr("3.14");
    TEST_ASSERT(v && v->type == CMCP_JSON_DOUBLE);
    TEST_ASSERT(cmcp_json_double(v) > 3.13 && cmcp_json_double(v) < 3.15);
    cmcp_json_free(v);

    v = cmcp_json_parse_cstr("-1.5e10");
    TEST_ASSERT(v && v->type == CMCP_JSON_DOUBLE);
    TEST_ASSERT(cmcp_json_double(v) == -1.5e10);
    cmcp_json_free(v);

    v = cmcp_json_parse_cstr("0.5");
    TEST_ASSERT(v && cmcp_json_double(v) == 0.5);
    cmcp_json_free(v);
}

static void test_parse_int_vs_double_distinction(void) {
    /* Critical for JSON-RPC IDs: 7 must round-trip as 7, not 7.0. */
    cmcp_json_t *v = cmcp_json_parse_cstr("7");
    TEST_ASSERT(v && v->type == CMCP_JSON_INT);
    cmcp_json_free(v);

    v = cmcp_json_parse_cstr("7.0");
    TEST_ASSERT(v && v->type == CMCP_JSON_DOUBLE);
    cmcp_json_free(v);
}

static void test_parse_string(void) {
    cmcp_json_t *v = cmcp_json_parse_cstr("\"hello\"");
    TEST_ASSERT(v && v->type == CMCP_JSON_STRING);
    TEST_ASSERT(cmcp_json_string_len(v) == 5);
    TEST_ASSERT(strcmp(cmcp_json_string(v), "hello") == 0);
    cmcp_json_free(v);
}

static void test_parse_string_escapes(void) {
    cmcp_json_t *v = cmcp_json_parse_cstr("\"a\\\"b\\nc\\td\"");
    TEST_ASSERT(v != NULL);
    TEST_ASSERT(strcmp(cmcp_json_string(v), "a\"b\nc\td") == 0);
    cmcp_json_free(v);
}

static void test_parse_string_unicode_bmp(void) {
    /* é -> é (U+00E9) -> 0xC3 0xA9 */
    cmcp_json_t *v = cmcp_json_parse_cstr("\"caf\\u00e9\"");
    TEST_ASSERT(v != NULL);
    TEST_ASSERT(strcmp(cmcp_json_string(v), "caf\xC3\xA9") == 0);
    cmcp_json_free(v);
}

static void test_parse_string_unicode_surrogate_pair(void) {
    /* 😀 -> 😀 (U+1F600) -> 0xF0 0x9F 0x98 0x80 */
    cmcp_json_t *v = cmcp_json_parse_cstr("\"\\uD83D\\uDE00\"");
    TEST_ASSERT(v != NULL);
    TEST_ASSERT(cmcp_json_string_len(v) == 4);
    const char *s = cmcp_json_string(v);
    TEST_ASSERT((unsigned char)s[0] == 0xF0);
    TEST_ASSERT((unsigned char)s[1] == 0x9F);
    TEST_ASSERT((unsigned char)s[2] == 0x98);
    TEST_ASSERT((unsigned char)s[3] == 0x80);
    cmcp_json_free(v);
}

static void test_parse_string_lone_surrogate_rejected(void) {
    cmcp_json_t *v = cmcp_json_parse_cstr("\"\\uD83D\"");
    TEST_ASSERT(v == NULL);
    v = cmcp_json_parse_cstr("\"\\uDC00\"");
    TEST_ASSERT(v == NULL);
}

static void test_parse_string_unescaped_control_rejected(void) {
    /* Literal newline inside a string is illegal in JSON. */
    cmcp_json_t *v = cmcp_json_parse_cstr("\"a\nb\"");
    TEST_ASSERT(v == NULL);
}

/* === parse: arrays / objects =========================================== */

static void test_parse_empty_array(void) {
    cmcp_json_t *v = cmcp_json_parse_cstr("[]");
    TEST_ASSERT(v && v->type == CMCP_JSON_ARRAY);
    TEST_ASSERT(cmcp_json_array_len(v) == 0);
    cmcp_json_free(v);
}

static void test_parse_array(void) {
    cmcp_json_t *v = cmcp_json_parse_cstr("[1, 2, 3]");
    TEST_ASSERT(v && cmcp_json_array_len(v) == 3);
    TEST_ASSERT(cmcp_json_int(cmcp_json_array_at(v, 0)) == 1);
    TEST_ASSERT(cmcp_json_int(cmcp_json_array_at(v, 2)) == 3);
    cmcp_json_free(v);
}

static void test_parse_empty_object(void) {
    cmcp_json_t *v = cmcp_json_parse_cstr("{}");
    TEST_ASSERT(v && v->type == CMCP_JSON_OBJECT);
    TEST_ASSERT(cmcp_json_object_len(v) == 0);
    cmcp_json_free(v);
}

static void test_parse_object(void) {
    cmcp_json_t *v = cmcp_json_parse_cstr("{\"a\":1,\"b\":\"two\",\"c\":true}");
    TEST_ASSERT(v && cmcp_json_object_len(v) == 3);
    TEST_ASSERT(cmcp_json_int(cmcp_json_object_get(v, "a")) == 1);
    TEST_ASSERT(strcmp(cmcp_json_string(cmcp_json_object_get(v, "b")), "two") == 0);
    TEST_ASSERT(cmcp_json_bool(cmcp_json_object_get(v, "c")) == 1);
    TEST_ASSERT(cmcp_json_object_get(v, "missing") == NULL);
    cmcp_json_free(v);
}

static void test_parse_nested(void) {
    const char *src =
        "{\"a\":[1,{\"b\":[true,null,\"x\"]}],\"c\":{\"d\":3.5}}";
    cmcp_json_t *v = cmcp_json_parse_cstr(src);
    TEST_ASSERT(v != NULL);
    const cmcp_json_t *a = cmcp_json_object_get(v, "a");
    TEST_ASSERT(cmcp_json_array_len(a) == 2);
    const cmcp_json_t *inner = cmcp_json_array_at(a, 1);
    const cmcp_json_t *b = cmcp_json_object_get(inner, "b");
    TEST_ASSERT(cmcp_json_array_len(b) == 3);
    TEST_ASSERT(cmcp_json_bool(cmcp_json_array_at(b, 0)) == 1);
    TEST_ASSERT(cmcp_json_is_null(cmcp_json_array_at(b, 1)));
    TEST_ASSERT(strcmp(cmcp_json_string(cmcp_json_array_at(b, 2)), "x") == 0);
    cmcp_json_free(v);
}

static void test_parse_whitespace(void) {
    cmcp_json_t *v = cmcp_json_parse_cstr(
        "  \n\t{ \"k\" :   42 ,\r \"a\" : [ 1 , 2 ] }  ");
    TEST_ASSERT(v != NULL);
    TEST_ASSERT(cmcp_json_int(cmcp_json_object_get(v, "k")) == 42);
    cmcp_json_free(v);
}

/* === parse: error cases ================================================ */

static void test_parse_garbage(void) {
    TEST_ASSERT(cmcp_json_parse_cstr("")          == NULL);
    TEST_ASSERT(cmcp_json_parse_cstr("xyz")       == NULL);
    TEST_ASSERT(cmcp_json_parse_cstr("{")         == NULL);
    TEST_ASSERT(cmcp_json_parse_cstr("[1,]")      == NULL);
    TEST_ASSERT(cmcp_json_parse_cstr("{\"k\":}")  == NULL);
    TEST_ASSERT(cmcp_json_parse_cstr("{k:1}")     == NULL);
    TEST_ASSERT(cmcp_json_parse_cstr("nul")       == NULL);
    TEST_ASSERT(cmcp_json_parse_cstr("01")        == NULL);  /* leading zero */
    TEST_ASSERT(cmcp_json_parse_cstr("1 trailing") == NULL); /* trailing garbage */
}

/* === emit: round-trip ================================================== */

static void test_emit_primitives(void) {
    cmcp_json_t *v;
    char *s;

    v = cmcp_json_new_null();   s = cmcp_json_emit(v); TEST_ASSERT(strcmp(s, "null")  == 0); free(s); cmcp_json_free(v);
    v = cmcp_json_new_bool(1);  s = cmcp_json_emit(v); TEST_ASSERT(strcmp(s, "true")  == 0); free(s); cmcp_json_free(v);
    v = cmcp_json_new_bool(0);  s = cmcp_json_emit(v); TEST_ASSERT(strcmp(s, "false") == 0); free(s); cmcp_json_free(v);
    v = cmcp_json_new_int(42);  s = cmcp_json_emit(v); TEST_ASSERT(strcmp(s, "42")    == 0); free(s); cmcp_json_free(v);
    v = cmcp_json_new_int(-7);  s = cmcp_json_emit(v); TEST_ASSERT(strcmp(s, "-7")    == 0); free(s); cmcp_json_free(v);
    v = cmcp_json_new_string("hi");
    s = cmcp_json_emit(v);
    TEST_ASSERT(strcmp(s, "\"hi\"") == 0); free(s); cmcp_json_free(v);
}

static void test_emit_string_escapes(void) {
    cmcp_json_t *v = cmcp_json_new_string("a\"b\nc\\d");
    char *s = cmcp_json_emit(v);
    TEST_ASSERT(strcmp(s, "\"a\\\"b\\nc\\\\d\"") == 0);
    free(s);
    cmcp_json_free(v);
}

static void test_emit_string_control_char(void) {
    char raw[4] = { 'a', 0x01, 'b', 0 };
    cmcp_json_t *v = cmcp_json_new_string(raw);
    char *s = cmcp_json_emit(v);
    TEST_ASSERT(strcmp(s, "\"a\\u0001b\"") == 0);
    free(s);
    cmcp_json_free(v);
}

static void test_emit_array(void) {
    cmcp_json_t *a = cmcp_json_new_array();
    cmcp_json_array_append(a, cmcp_json_new_int(1));
    cmcp_json_array_append(a, cmcp_json_new_int(2));
    cmcp_json_array_append(a, cmcp_json_new_string("x"));
    char *s = cmcp_json_emit(a);
    TEST_ASSERT(strcmp(s, "[1,2,\"x\"]") == 0);
    free(s);
    cmcp_json_free(a);
}

static void test_emit_object_insertion_order(void) {
    cmcp_json_t *o = cmcp_json_new_object();
    cmcp_json_object_set(o, "b", cmcp_json_new_int(1));
    cmcp_json_object_set(o, "a", cmcp_json_new_int(2));
    char *s = cmcp_json_emit(o);
    TEST_ASSERT(strcmp(s, "{\"b\":1,\"a\":2}") == 0);
    free(s);
    cmcp_json_free(o);
}

static void test_emit_object_stable_order(void) {
    cmcp_json_t *o = cmcp_json_new_object();
    cmcp_json_object_set(o, "b", cmcp_json_new_int(1));
    cmcp_json_object_set(o, "a", cmcp_json_new_int(2));
    cmcp_json_object_set(o, "c", cmcp_json_new_int(3));
    char *s = cmcp_json_emit_stable(o);
    TEST_ASSERT(strcmp(s, "{\"a\":2,\"b\":1,\"c\":3}") == 0);
    free(s);
    cmcp_json_free(o);
}

static void test_roundtrip_complex(void) {
    const char *src = "{\"a\":[1,2,3],\"b\":{\"c\":\"x\",\"d\":true}}";
    cmcp_json_t *v = cmcp_json_parse_cstr(src);
    TEST_ASSERT(v != NULL);
    char *out = cmcp_json_emit(v);
    /* Insertion-order emit should match exactly since parse preserves it. */
    TEST_ASSERT(strcmp(out, src) == 0);
    free(out);
    cmcp_json_free(v);
}

/* === clone / equal ===================================================== */

static void test_clone_and_equal(void) {
    const char *src = "{\"a\":[1,2.5,\"x\",null,true],\"b\":{\"c\":42}}";
    cmcp_json_t *v = cmcp_json_parse_cstr(src);
    cmcp_json_t *c = cmcp_json_clone(v);
    TEST_ASSERT(cmcp_json_equal(v, c) == 1);
    cmcp_json_free(v);
    cmcp_json_free(c);
}

static void test_equal_order_independent_for_objects(void) {
    cmcp_json_t *a = cmcp_json_parse_cstr("{\"x\":1,\"y\":2}");
    cmcp_json_t *b = cmcp_json_parse_cstr("{\"y\":2,\"x\":1}");
    TEST_ASSERT(cmcp_json_equal(a, b) == 1);
    cmcp_json_free(a);
    cmcp_json_free(b);
}

static void test_equal_distinguishes_int_from_double(void) {
    cmcp_json_t *a = cmcp_json_parse_cstr("7");
    cmcp_json_t *b = cmcp_json_parse_cstr("7.0");
    TEST_ASSERT(cmcp_json_equal(a, b) == 0);  /* different types */
    cmcp_json_free(a);
    cmcp_json_free(b);
}

/* === builder mutation ================================================== */

static void test_object_set_replaces_existing_key(void) {
    cmcp_json_t *o = cmcp_json_new_object();
    cmcp_json_object_set(o, "k", cmcp_json_new_int(1));
    cmcp_json_object_set(o, "k", cmcp_json_new_int(2));
    TEST_ASSERT(cmcp_json_object_len(o) == 1);
    TEST_ASSERT(cmcp_json_int(cmcp_json_object_get(o, "k")) == 2);
    cmcp_json_free(o);
}

/* === MCP fixture: an initialize request ================================ */

static void test_mcp_initialize_request_roundtrip(void) {
    /* Shape from the MCP spec: a JSON-RPC 2.0 initialize request. */
    const char *src =
        "{"
            "\"jsonrpc\":\"2.0\","
            "\"id\":1,"
            "\"method\":\"initialize\","
            "\"params\":{"
                "\"protocolVersion\":\"2025-06-18\","
                "\"capabilities\":{\"roots\":{\"listChanged\":true}},"
                "\"clientInfo\":{\"name\":\"openclawd\",\"version\":\"0.0.1\"}"
            "}"
        "}";

    cmcp_json_t *v = cmcp_json_parse_cstr(src);
    TEST_ASSERT(v != NULL);

    /* Field accessors */
    TEST_ASSERT(strcmp(cmcp_json_string(cmcp_json_object_get(v, "jsonrpc")),
                       "2.0") == 0);
    TEST_ASSERT(cmcp_json_int(cmcp_json_object_get(v, "id")) == 1);
    TEST_ASSERT(strcmp(cmcp_json_string(cmcp_json_object_get(v, "method")),
                       "initialize") == 0);

    const cmcp_json_t *params = cmcp_json_object_get(v, "params");
    TEST_ASSERT(params != NULL);
    TEST_ASSERT(strcmp(cmcp_json_string(
        cmcp_json_object_get(params, "protocolVersion")),
        "2025-06-18") == 0);

    const cmcp_json_t *client_info = cmcp_json_object_get(params, "clientInfo");
    TEST_ASSERT(strcmp(cmcp_json_string(
        cmcp_json_object_get(client_info, "name")), "openclawd") == 0);

    /* Round-trip: parse → stable-emit → parse → equal */
    char *out = cmcp_json_emit_stable(v);
    cmcp_json_t *v2 = cmcp_json_parse_cstr(out);
    TEST_ASSERT(v2 != NULL);
    TEST_ASSERT(cmcp_json_equal(v, v2) == 1);
    free(out);
    cmcp_json_free(v);
    cmcp_json_free(v2);
}

/* === escape helper (cRAG carry-over) =================================== */

static void test_escape_helper(void) {
    char out[64];
    int n = cmcp_json_escape("a\"b\nc", out, sizeof out);
    TEST_ASSERT(n > 0);
    TEST_ASSERT(strcmp(out, "a\\\"b\\nc") == 0);

    char *d = cmcp_json_escape_dup("hi");
    TEST_ASSERT(d && strcmp(d, "hi") == 0);
    free(d);
}

int main(void) {
    fprintf(stderr, "test_json:\n");
    TEST_RUN(test_parse_null);
    TEST_RUN(test_parse_bools);
    TEST_RUN(test_parse_int);
    TEST_RUN(test_parse_double);
    TEST_RUN(test_parse_int_vs_double_distinction);
    TEST_RUN(test_parse_string);
    TEST_RUN(test_parse_string_escapes);
    TEST_RUN(test_parse_string_unicode_bmp);
    TEST_RUN(test_parse_string_unicode_surrogate_pair);
    TEST_RUN(test_parse_string_lone_surrogate_rejected);
    TEST_RUN(test_parse_string_unescaped_control_rejected);
    TEST_RUN(test_parse_empty_array);
    TEST_RUN(test_parse_array);
    TEST_RUN(test_parse_empty_object);
    TEST_RUN(test_parse_object);
    TEST_RUN(test_parse_nested);
    TEST_RUN(test_parse_whitespace);
    TEST_RUN(test_parse_garbage);
    TEST_RUN(test_emit_primitives);
    TEST_RUN(test_emit_string_escapes);
    TEST_RUN(test_emit_string_control_char);
    TEST_RUN(test_emit_array);
    TEST_RUN(test_emit_object_insertion_order);
    TEST_RUN(test_emit_object_stable_order);
    TEST_RUN(test_roundtrip_complex);
    TEST_RUN(test_clone_and_equal);
    TEST_RUN(test_equal_order_independent_for_objects);
    TEST_RUN(test_equal_distinguishes_int_from_double);
    TEST_RUN(test_object_set_replaces_existing_key);
    TEST_RUN(test_mcp_initialize_request_roundtrip);
    TEST_RUN(test_escape_helper);
    TEST_DONE();
}
