/* test_schema_check — JSON Schema 2020-12 conformance work (Phase 5).
 *
 *   - dialects: 2020-12 by default, draft-07 by declaration, anything
 *     else reported as unsupported instead of validated wrongly;
 *   - the keywords added in this phase: contains / minContains /
 *     maxContains, dependentRequired / dependentSchemas / dependencies;
 *   - cmcp_schema_check: external or dangling $ref, unsupported
 *     keywords, malformed keyword values, size and depth bounds;
 *   - x-mcp-header: the registration lint and the per-call comparison. */

/* setenv(3) — POSIX. */
#define _POSIX_C_SOURCE 200809L

#include "test.h"
#include "cmcp.h"
#include "cmcp_json.h"
#include "cmcp_schema.h"
#include "cmcp_server.h"
#include "../src/xheader.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Validate `value` against `schema` (both JSON text); returns the rc. */
static int vrc(const char *schema, const char *value) {
    cmcp_json_t *s = cmcp_json_parse_cstr(schema);
    cmcp_json_t *v = cmcp_json_parse_cstr(value);
    int rc = s ? cmcp_schema_validate(s, v, NULL) : -999;
    cmcp_json_free(s);
    cmcp_json_free(v);
    return rc;
}

/* cmcp_schema_check on JSON text; copies the error path into `path`. */
static int crc(const char *schema, char *path, size_t cap) {
    cmcp_json_t *s = cmcp_json_parse_cstr(schema);
    if (!s) return -999;                       /* the test JSON is broken */
    cmcp_schema_error_t e;
    int rc = cmcp_schema_check(s, &e);
    if (path) snprintf(path, cap, "%s", e.path ? e.path : "");
    cmcp_schema_error_clear(&e);
    cmcp_json_free(s);
    return rc;
}

static int lint_rc(const char *schema) {
    cmcp_json_t *s = cmcp_json_parse_cstr(schema);
    if (!s) return -999;
    cmcp_schema_error_t e;
    int rc = cmcp_xheader_lint(s, &e);
    cmcp_schema_error_clear(&e);
    cmcp_json_free(s);
    return rc;
}

/* ---------------------------------------------------------------- */
/* Dialects                                                          */
/* ---------------------------------------------------------------- */

static void test_dialects(void) {
    TEST_ASSERT(vrc("{\"type\":\"string\"}", "\"x\"") == CMCP_OK);
    TEST_ASSERT(vrc("{\"$schema\":\"https://json-schema.org/draft/2020-12/schema\","
                    "\"type\":\"string\"}", "\"x\"") == CMCP_OK);
    TEST_ASSERT(vrc("{\"$schema\":\"http://json-schema.org/draft-07/schema#\","
                    "\"type\":\"string\"}", "1") == CMCP_ESCHEMA);
    /* A dialect cMCP does not implement is reported, not guessed at. */
    TEST_ASSERT(vrc("{\"$schema\":\"http://json-schema.org/draft-04/schema#\","
                    "\"type\":\"string\"}", "\"x\"") == CMCP_EUNSUPPORTED);
    TEST_ASSERT(crc("{\"$schema\":\"http://json-schema.org/draft-04/schema#\"}",
                    NULL, 0) == CMCP_EUNSUPPORTED);
}

/* draft-07 ignores keywords next to $ref; 2020-12 applies them. */
static void test_ref_siblings_follow_the_dialect(void) {
    const char *body = "\"$defs\":{\"s\":{\"type\":\"string\"}},"
                       "\"$ref\":\"#/$defs/s\",\"maxLength\":2";
    char s2020[256], s07[256];
    snprintf(s2020, sizeof s2020, "{%s}", body);
    snprintf(s07, sizeof s07,
             "{\"$schema\":\"http://json-schema.org/draft-07/schema#\",%s}", body);
    TEST_ASSERT(vrc(s2020, "\"long\"") == CMCP_ESCHEMA);
    TEST_ASSERT(vrc(s07, "\"long\"") == CMCP_OK);
}

/* ---------------------------------------------------------------- */
/* New keywords                                                      */
/* ---------------------------------------------------------------- */

static void test_contains(void) {
    const char *s = "{\"contains\":{\"type\":\"integer\"}}";
    TEST_ASSERT(vrc(s, "[\"a\",1]") == CMCP_OK);
    TEST_ASSERT(vrc(s, "[\"a\",\"b\"]") == CMCP_ESCHEMA);
    TEST_ASSERT(vrc(s, "[]") == CMCP_ESCHEMA);
    TEST_ASSERT(vrc(s, "\"not an array\"") == CMCP_OK);

    const char *range = "{\"contains\":{\"type\":\"integer\"},"
                        "\"minContains\":2,\"maxContains\":3}";
    TEST_ASSERT(vrc(range, "[1]") == CMCP_ESCHEMA);
    TEST_ASSERT(vrc(range, "[1,2,\"x\"]") == CMCP_OK);
    TEST_ASSERT(vrc(range, "[1,2,3,4]") == CMCP_ESCHEMA);
    /* minContains 0: an array with no match is fine. */
    TEST_ASSERT(vrc("{\"contains\":{\"type\":\"integer\"},\"minContains\":0}",
                    "[]") == CMCP_OK);
}

static void test_dependent_keywords(void) {
    const char *req = "{\"dependentRequired\":{\"card\":[\"cvv\"]}}";
    TEST_ASSERT(vrc(req, "{\"card\":1,\"cvv\":2}") == CMCP_OK);
    TEST_ASSERT(vrc(req, "{\"card\":1}") == CMCP_ESCHEMA);
    TEST_ASSERT(vrc(req, "{\"cvv\":2}") == CMCP_OK);

    const char *sch = "{\"dependentSchemas\":{\"card\":"
                      "{\"required\":[\"zip\"]}}}";
    TEST_ASSERT(vrc(sch, "{\"card\":1,\"zip\":\"x\"}") == CMCP_OK);
    TEST_ASSERT(vrc(sch, "{\"card\":1}") == CMCP_ESCHEMA);

    /* draft-07 `dependencies`, both forms. */
    const char *d07 = "{\"dependencies\":{\"a\":[\"b\"],"
                      "\"c\":{\"required\":[\"d\"]}}}";
    TEST_ASSERT(vrc(d07, "{\"a\":1,\"b\":1}") == CMCP_OK);
    TEST_ASSERT(vrc(d07, "{\"a\":1}") == CMCP_ESCHEMA);
    TEST_ASSERT(vrc(d07, "{\"c\":1}") == CMCP_ESCHEMA);
}

/* ---------------------------------------------------------------- */
/* cmcp_schema_check                                                 */
/* ---------------------------------------------------------------- */

static void test_check_accepts_real_schemas(void) {
    TEST_ASSERT(crc("{\"type\":\"object\",\"properties\":{"
                    "\"name\":{\"type\":\"string\",\"minLength\":1},"
                    "\"tags\":{\"type\":\"array\",\"items\":{\"$ref\":\"#/$defs/tag\"}}},"
                    "\"required\":[\"name\"],"
                    "\"$defs\":{\"tag\":{\"enum\":[\"a\",\"b\"]}},"
                    "\"anyOf\":[{\"required\":[\"name\"]},true]}", NULL, 0) == CMCP_OK);
    TEST_ASSERT(crc("true", NULL, 0) == CMCP_OK);
}

static void test_check_rejects_refs_it_cannot_follow(void) {
    char path[128];
    /* Never fetched, so rejected rather than treated as permissive. */
    TEST_ASSERT(crc("{\"properties\":{\"a\":{\"$ref\":\"https://evil.example/s.json\"}}}",
                    path, sizeof path) == CMCP_EUNSUPPORTED);
    TEST_ASSERT(strcmp(path, "/properties/a") == 0);
    TEST_ASSERT(crc("{\"$ref\":\"other.json#/x\"}", NULL, 0) == CMCP_EUNSUPPORTED);
    TEST_ASSERT(crc("{\"$ref\":\"#/$defs/missing\"}", NULL, 0) == CMCP_EINVAL);
}

static void test_check_rejects_unsupported_keywords(void) {
    TEST_ASSERT(crc("{\"type\":\"object\",\"unevaluatedProperties\":false}",
                    NULL, 0) == CMCP_EUNSUPPORTED);
    TEST_ASSERT(crc("{\"allOf\":[{\"unevaluatedItems\":false}]}", NULL, 0)
                == CMCP_EUNSUPPORTED);
    TEST_ASSERT(crc("{\"$dynamicRef\":\"#meta\"}", NULL, 0) == CMCP_EUNSUPPORTED);
}

static void test_check_rejects_malformed_keywords(void) {
    char path[128];
    TEST_ASSERT(crc("{\"type\":\"text\"}", NULL, 0) == CMCP_EINVAL);
    TEST_ASSERT(crc("{\"type\":[]}", NULL, 0) == CMCP_EINVAL);
    TEST_ASSERT(crc("{\"required\":[1]}", NULL, 0) == CMCP_EINVAL);
    TEST_ASSERT(crc("{\"allOf\":[]}", NULL, 0) == CMCP_EINVAL);
    TEST_ASSERT(crc("{\"minLength\":-1}", NULL, 0) == CMCP_EINVAL);
    TEST_ASSERT(crc("{\"properties\":[]}", NULL, 0) == CMCP_EINVAL);
    TEST_ASSERT(crc("{\"properties\":{\"a\":5}}", path, sizeof path) == CMCP_EINVAL);
    TEST_ASSERT(strcmp(path, "/properties/a") == 0);
}

/* Bounds against a schema built to exhaust the validator. main() raises
 * the JSON parser's own depth cap so the walker's bound is what trips. */
static void test_check_bounds(void) {
    char deep[2048] = "";
    for (int i = 0; i < 70; i++) strcat(deep, "{\"not\":");
    strcat(deep, "true");
    for (int i = 0; i < 70; i++) strcat(deep, "}");
    TEST_ASSERT(crc(deep, NULL, 0) == CMCP_EINVAL);

    /* 10001 subschemas in one allOf. */
    size_t cap = 10001 * 5 + 32;
    char *wide = (char *)malloc(cap);
    TEST_ASSERT(wide != NULL);
    if (!wide) return;
    size_t n = (size_t)snprintf(wide, cap, "{\"allOf\":[");
    for (int i = 0; i < 10001; i++)
        n += (size_t)snprintf(wide + n, cap - n, i ? ",true" : "true");
    snprintf(wide + n, cap - n, "]}");
    TEST_ASSERT(crc(wide, NULL, 0) == CMCP_EINVAL);
    free(wide);
}

/* ---------------------------------------------------------------- */
/* x-mcp-header                                                      */
/* ---------------------------------------------------------------- */

static void test_xheader_lint(void) {
    TEST_ASSERT(lint_rc("{\"type\":\"object\",\"properties\":{"
                        "\"region\":{\"type\":\"string\",\"x-mcp-header\":\"Region\"},"
                        "\"cfg\":{\"type\":\"object\",\"properties\":{"
                        "\"n\":{\"type\":\"integer\",\"x-mcp-header\":\"N\"}}}}}")
                == CMCP_OK);
    /* Not an HTTP token. */
    TEST_ASSERT(lint_rc("{\"properties\":{\"a\":{\"type\":\"string\","
                        "\"x-mcp-header\":\"Re gion\"}}}") == CMCP_EINVAL);
    TEST_ASSERT(lint_rc("{\"properties\":{\"a\":{\"type\":\"string\","
                        "\"x-mcp-header\":\"\"}}}") == CMCP_EINVAL);
    /* Unique case-insensitively. */
    TEST_ASSERT(lint_rc("{\"properties\":{"
                        "\"a\":{\"type\":\"string\",\"x-mcp-header\":\"Region\"},"
                        "\"b\":{\"type\":\"string\",\"x-mcp-header\":\"region\"}}}")
                == CMCP_EINVAL);
    /* Primitive types only: number is excluded. */
    TEST_ASSERT(lint_rc("{\"properties\":{\"a\":{\"type\":\"number\","
                        "\"x-mcp-header\":\"A\"}}}") == CMCP_EINVAL);
    /* Not statically reachable: items, allOf, the root itself. */
    TEST_ASSERT(lint_rc("{\"properties\":{\"a\":{\"type\":\"array\",\"items\":"
                        "{\"type\":\"string\",\"x-mcp-header\":\"A\"}}}}") == CMCP_EINVAL);
    TEST_ASSERT(lint_rc("{\"allOf\":[{\"properties\":{\"a\":{\"type\":\"string\","
                        "\"x-mcp-header\":\"A\"}}}]}") == CMCP_EINVAL);
    TEST_ASSERT(lint_rc("{\"type\":\"string\",\"x-mcp-header\":\"A\"}") == CMCP_EINVAL);
}

/* A tiny header table standing in for an HTTP request. */
typedef struct { const char *name, *value; } hdr_t;

static const char *table_get(void *ctx, const char *name) {
    for (const hdr_t *h = (const hdr_t *)ctx; h->name; h++)
        if (strcmp(h->name, name) == 0) return h->value;
    return NULL;
}

#define XSCHEMA "{\"type\":\"object\",\"properties\":{"                       \
    "\"region\":{\"type\":\"string\",\"x-mcp-header\":\"Region\"},"            \
    "\"shard\":{\"type\":\"integer\",\"x-mcp-header\":\"Shard\"},"             \
    "\"dry\":{\"type\":\"boolean\",\"x-mcp-header\":\"Dry\"},"                 \
    "\"q\":{\"type\":\"string\"}}}"

static int xcheck(const char *args, const hdr_t *headers) {
    cmcp_json_t *s = cmcp_json_parse_cstr(XSCHEMA);
    cmcp_json_t *a = cmcp_json_parse_cstr(args);
    char *msg = NULL;
    int rc = cmcp_xheader_check(s, a, table_get, (void *)headers, &msg);
    free(msg);
    cmcp_json_free(s);
    cmcp_json_free(a);
    return rc;
}

static void test_xheader_check(void) {
    const hdr_t ok[] = {{"Mcp-Param-Region", "us-west1"}, {"Mcp-Param-Shard", "42.0"},
                        {"Mcp-Param-Dry", "true"}, {NULL, NULL}};
    TEST_ASSERT(xcheck("{\"region\":\"us-west1\",\"shard\":42,\"dry\":true,"
                       "\"q\":\"x\"}", ok) == 0);
    /* Argument absent or null: no header expected. */
    const hdr_t none[] = {{NULL, NULL}};
    TEST_ASSERT(xcheck("{\"q\":\"x\",\"region\":null}", none) == 0);
    /* Present in the body, missing from the headers. */
    TEST_ASSERT(xcheck("{\"region\":\"us-west1\"}", none) == 1);
    /* Different values. */
    const hdr_t wrong[] = {{"Mcp-Param-Region", "eu-north1"}, {NULL, NULL}};
    TEST_ASSERT(xcheck("{\"region\":\"us-west1\"}", wrong) == 1);
    const hdr_t wrong_n[] = {{"Mcp-Param-Shard", "41"}, {NULL, NULL}};
    TEST_ASSERT(xcheck("{\"shard\":42}", wrong_n) == 1);
    const hdr_t wrong_b[] = {{"Mcp-Param-Dry", "TRUE"}, {NULL, NULL}};
    TEST_ASSERT(xcheck("{\"dry\":true}", wrong_b) == 1);
    /* The base64 sentinel is decoded before comparing. */
    const hdr_t b64[] = {{"Mcp-Param-Region", "=?base64?SGVsbG8sIOS4lueVjA==?="},
                         {NULL, NULL}};
    TEST_ASSERT(xcheck("{\"region\":\"Hello, \xe4\xb8\x96\xe7\x95\x8c\"}", b64) == 0);
}

/* add_tool refuses schemas the checks reject. */
static int nop(const cmcp_json_t *a, void *u, cmcp_handler_ctx_t *h,
               cmcp_json_t **o, int *e) {
    (void)a; (void)u; (void)h;
    *o = cmcp_tool_text_content("");
    *e = 0;
    return CMCP_OK;
}

static void test_add_tool_runs_the_checks(void) {
    cmcp_server_t *s = cmcp_server_new("t", "1");
    cmcp_tool_t t = {.name = "a", .description = "d", .handler = nop,
                     .input_schema = "{\"$ref\":\"https://x.example/s\"}"};
    TEST_ASSERT(cmcp_server_add_tool(s, &t) == CMCP_EUNSUPPORTED);
    t.input_schema = "{\"properties\":{\"r\":{\"type\":\"number\","
                     "\"x-mcp-header\":\"R\"}}}";
    TEST_ASSERT(cmcp_server_add_tool(s, &t) == CMCP_EINVAL);
    t.input_schema = "{\"type\":\"object\"}";
    t.output_schema = "{\"type\":\"object\",\"unevaluatedProperties\":false}";
    TEST_ASSERT(cmcp_server_add_tool(s, &t) == CMCP_EUNSUPPORTED);
    t.output_schema = NULL;
    TEST_ASSERT(cmcp_server_add_tool(s, &t) == CMCP_OK);
    cmcp_server_free(s);
}

int main(void) {
    /* Before the first parse: the cap is snapshotted once. */
    setenv("CMCP_JSON_MAX_DEPTH", "256", 1);
    TEST_RUN(test_dialects);
    TEST_RUN(test_ref_siblings_follow_the_dialect);
    TEST_RUN(test_contains);
    TEST_RUN(test_dependent_keywords);
    TEST_RUN(test_check_accepts_real_schemas);
    TEST_RUN(test_check_rejects_refs_it_cannot_follow);
    TEST_RUN(test_check_rejects_unsupported_keywords);
    TEST_RUN(test_check_rejects_malformed_keywords);
    TEST_RUN(test_check_bounds);
    TEST_RUN(test_xheader_lint);
    TEST_RUN(test_xheader_check);
    TEST_RUN(test_add_tool_runs_the_checks);
    TEST_DONE();
}
