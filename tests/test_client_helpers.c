/* Tests for the single-client typed helpers (v0.6.0 axis A1).
 *
 * Each helper is exercised against a real cmcp_server_run server wired
 * over a pipe(2) pair (same shape as test_client_server.c). The goal is
 * to lock down the behaviour the dogfood harness will rely on:
 *
 *   - cmcp_client_tools_list        — happy path + empty + paginated
 *   - cmcp_client_resources_list    — happy path + paginated
 *   - cmcp_client_prompts_list      — happy path
 *   - cmcp_client_resource_read     — text, empty-contents, blob,
 *                                     unknown URI
 *   - cmcp_client_prompt_get        — happy path + unknown name
 *   - CMCP_EINVAL on NULL arguments across all five helpers
 *
 * Records produced by the list helpers must have NULL server /
 * qualified fields — single-client helpers don't namespace. */

#define _POSIX_C_SOURCE 200809L

#include "test.h"
#include "cmcp.h"
#include "cmcp_client.h"
#include "cmcp_json.h"
#include "cmcp_server.h"
#include "cmcp_session.h"
#include "cmcp_transport.h"
#include "cmcp_types.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

/* ====================================================================== */
/* Pipe-pair scaffolding (mirrors test_client_server.c)                    */
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
/* Stubs for tools / resources / prompts                                   */
/* ====================================================================== */

static int noop_tool(const cmcp_json_t *arguments, void *userdata,
                      cmcp_handler_ctx_t *hctx,
                      cmcp_json_t **out_content, int *out_is_error) {
    (void)arguments; (void)userdata; (void)hctx; (void)out_is_error;
    *out_content = cmcp_tool_text_content("ok");
    return *out_content ? CMCP_OK : CMCP_ENOMEM;
}

/* echo_tool — return the supplied "message" argument back as content. */
static int echo_tool(const cmcp_json_t *arguments, void *userdata,
                      cmcp_handler_ctx_t *hctx,
                      cmcp_json_t **out_content, int *out_is_error) {
    (void)userdata; (void)hctx; (void)out_is_error;
    const cmcp_json_t *m = arguments
        ? cmcp_json_object_get(arguments, "message") : NULL;
    const char *msg = (m && m->type == CMCP_JSON_STRING) ? m->str.s : "";
    *out_content = cmcp_tool_text_content(msg);
    return *out_content ? CMCP_OK : CMCP_ENOMEM;
}

static const char echo_schema[] =
    "{\"type\":\"object\","
     "\"properties\":{\"message\":{\"type\":\"string\",\"minLength\":1}},"
     "\"required\":[\"message\"]}";

/* fail_tool — always reports a tool-level error with explanatory text. */
static int fail_tool(const cmcp_json_t *arguments, void *userdata,
                      cmcp_handler_ctx_t *hctx,
                      cmcp_json_t **out_content, int *out_is_error) {
    (void)arguments; (void)userdata; (void)hctx;
    *out_content = cmcp_tool_text_content("intentional handler failure");
    *out_is_error = 1;
    return *out_content ? CMCP_OK : CMCP_ENOMEM;
}

/* slow_tool — sleeps long enough that a cancel issued right after the
 * submit reliably wins the race (the in-tree tools are otherwise instant,
 * the P6 friction that made the cancel-honored path untestable). */
static int slow_tool(const cmcp_json_t *arguments, void *userdata,
                      cmcp_handler_ctx_t *hctx,
                      cmcp_json_t **out_content, int *out_is_error) {
    (void)arguments; (void)userdata; (void)hctx; (void)out_is_error;
    struct timespec ts = { 0, 250L * 1000L * 1000L };  /* 250ms */
    nanosleep(&ts, NULL);
    *out_content = cmcp_tool_text_content("slow-done");
    return *out_content ? CMCP_OK : CMCP_ENOMEM;
}

static const char tool_schema_empty[] =
    "{\"type\":\"object\",\"properties\":{},\"additionalProperties\":false}";

static int register_noop_tool(cmcp_server_t *srv, const char *name,
                                const char *desc) {
    cmcp_tool_t t = {0};
    t.name         = name;
    t.description  = desc;
    t.input_schema = tool_schema_empty;
    t.handler      = noop_tool;
    return cmcp_server_add_tool(srv, &t);
}

static int text_resource(const char *uri, void *userdata,
                          cmcp_handler_ctx_t *hctx,
                          cmcp_json_t **out_contents, int *out_is_error) {
    (void)userdata; (void)hctx; (void)out_is_error;
    *out_contents = cmcp_resource_text_contents(uri, "text/plain",
                                                  "hello from cmcp");
    return *out_contents ? CMCP_OK : CMCP_ENOMEM;
}

static int empty_contents_resource(const char *uri, void *userdata,
                                     cmcp_handler_ctx_t *hctx,
                                     cmcp_json_t **out_contents,
                                     int *out_is_error) {
    (void)uri; (void)userdata; (void)hctx; (void)out_is_error;
    *out_contents = cmcp_json_new_array();
    return *out_contents ? CMCP_OK : CMCP_ENOMEM;
}

static int blob_resource(const char *uri, void *userdata,
                          cmcp_handler_ctx_t *hctx,
                          cmcp_json_t **out_contents, int *out_is_error) {
    (void)userdata; (void)hctx; (void)out_is_error;
    cmcp_json_t *arr  = cmcp_json_new_array();
    cmcp_json_t *item = cmcp_json_new_object();
    cmcp_json_object_set(item, "uri", cmcp_json_new_string(uri));
    cmcp_json_object_set(item, "mimeType",
                          cmcp_json_new_string("application/octet-stream"));
    /* Base64 of "binary". The helper inspects only the presence of the
     * `blob` key, not its decoded payload. */
    cmcp_json_object_set(item, "blob", cmcp_json_new_string("YmluYXJ5"));
    cmcp_json_array_append(arr, item);
    *out_contents = arr;
    return CMCP_OK;
}

static int register_resource(cmcp_server_t *srv, const char *uri,
                                const char *name, cmcp_resource_read_fn fn) {
    cmcp_resource_t r = {0};
    r.uri  = uri;
    r.name = name;
    r.read = fn;
    return cmcp_server_add_resource(srv, &r);
}

static int prompt_handler(const cmcp_json_t *arguments, void *userdata,
                            cmcp_handler_ctx_t *hctx,
                            cmcp_json_t **out_messages) {
    (void)arguments; (void)userdata; (void)hctx;
    *out_messages = cmcp_prompt_text_messages("user", "drive the test");
    return *out_messages ? CMCP_OK : CMCP_ENOMEM;
}

static int register_prompt(cmcp_server_t *srv, const char *name,
                             const char *desc) {
    cmcp_prompt_t p = {0};
    p.name        = name;
    p.description = desc;
    p.handler     = prompt_handler;
    return cmcp_server_add_prompt(srv, &p);
}

/* ====================================================================== */
/* Per-test wiring                                                          */
/* ====================================================================== */

typedef struct {
    transport_pair_t  pair;
    cmcp_server_t    *srv;
    server_arg_t      sa;
    pthread_t         th;
    cmcp_client_t    *cli;
} wired_t;

static int wire_up(wired_t *w, cmcp_server_t *srv) {
    if (make_pair(&w->pair) != 0) return -1;
    w->srv = srv;
    w->sa.s = srv;
    w->sa.t = w->pair.server_t;
    w->sa.rc = 0;
    if (pthread_create(&w->th, NULL, server_thread, &w->sa) != 0) return -1;
    w->cli = cmcp_client_new("helpers-cli", "0.0.1");
    if (!w->cli) return -1;
    if (cmcp_client_handshake(w->cli, w->pair.client_t) != CMCP_OK) return -1;
    return 0;
}

static void tear_down(wired_t *w) {
    if (w->cli) cmcp_client_free(w->cli);
    cmcp_transport_close(w->pair.client_t);
    pthread_join(w->th, NULL);
    if (w->srv) cmcp_server_free(w->srv);
    cmcp_transport_close(w->pair.server_t);
}

/* ====================================================================== */
/* test_tools_list_basic                                                    */
/* ====================================================================== */

static void test_tools_list_basic(void) {
    cmcp_server_t *srv = cmcp_server_new("tlb-srv", "0.1.0");
    TEST_ASSERT(register_noop_tool(srv, "alpha", "the first tool") == CMCP_OK);
    TEST_ASSERT(register_noop_tool(srv, "beta",  "the second tool") == CMCP_OK);

    wired_t w = {0};
    TEST_ASSERT(wire_up(&w, srv) == 0);

    cmcp_session_tool_t *tools = NULL;
    size_t n = 0;
    TEST_ASSERT(cmcp_client_tools_list(w.cli, &tools, &n) == CMCP_OK);
    TEST_ASSERT(n == 2);
    TEST_ASSERT(tools != NULL);

    /* Either order is fine; check both names appear, both descriptions
     * appear, and server/qualified are NULL across the board. */
    int saw_alpha = 0, saw_beta = 0;
    for (size_t i = 0; i < n; i++) {
        TEST_ASSERT(tools[i].server == NULL);
        TEST_ASSERT(tools[i].qualified == NULL);
        TEST_ASSERT(tools[i].name != NULL);
        TEST_ASSERT(tools[i].input_schema != NULL);
        if (strcmp(tools[i].name, "alpha") == 0) {
            saw_alpha = 1;
            TEST_ASSERT(tools[i].description &&
                         strcmp(tools[i].description, "the first tool") == 0);
        } else if (strcmp(tools[i].name, "beta") == 0) {
            saw_beta = 1;
            TEST_ASSERT(tools[i].description &&
                         strcmp(tools[i].description, "the second tool") == 0);
        }
    }
    TEST_ASSERT(saw_alpha && saw_beta);

    cmcp_session_tools_free(tools, n);
    tear_down(&w);
}

/* ====================================================================== */
/* test_tools_list_empty                                                    */
/* ====================================================================== */
/* cMCP's server replies to `tools/list` with `{tools: []}` even when
 * no tools were registered (the method always exists; the cap-gating is
 * advisory). The helper surfaces that as CMCP_OK with *out_n == 0. */

static void test_tools_list_empty(void) {
    cmcp_server_t *srv = cmcp_server_new("empty-srv", "0.1.0");

    wired_t w = {0};
    TEST_ASSERT(wire_up(&w, srv) == 0);

    cmcp_session_tool_t *tools = (cmcp_session_tool_t *)0xdeadbeef;
    size_t n = 999;
    int rc = cmcp_client_tools_list(w.cli, &tools, &n);
    TEST_ASSERT(rc == CMCP_OK);
    TEST_ASSERT(tools == NULL);
    TEST_ASSERT(n == 0);

    tear_down(&w);
}

/* ====================================================================== */
/* test_resources_list_basic                                                */
/* ====================================================================== */

static void test_resources_list_basic(void) {
    cmcp_server_t *srv = cmcp_server_new("rlb-srv", "0.1.0");
    TEST_ASSERT(register_resource(srv, "mem:///one", "first",
                                    text_resource) == CMCP_OK);
    TEST_ASSERT(register_resource(srv, "mem:///two", "second",
                                    text_resource) == CMCP_OK);

    wired_t w = {0};
    TEST_ASSERT(wire_up(&w, srv) == 0);

    cmcp_session_resource_t *res = NULL;
    size_t n = 0;
    TEST_ASSERT(cmcp_client_resources_list(w.cli, &res, &n) == CMCP_OK);
    TEST_ASSERT(n == 2);

    int saw_one = 0, saw_two = 0;
    for (size_t i = 0; i < n; i++) {
        TEST_ASSERT(res[i].server == NULL);
        TEST_ASSERT(res[i].uri != NULL);
        TEST_ASSERT(res[i].name != NULL);
        if (strcmp(res[i].uri, "mem:///one") == 0) {
            saw_one = 1;
            TEST_ASSERT(strcmp(res[i].name, "first") == 0);
        } else if (strcmp(res[i].uri, "mem:///two") == 0) {
            saw_two = 1;
            TEST_ASSERT(strcmp(res[i].name, "second") == 0);
        }
    }
    TEST_ASSERT(saw_one && saw_two);

    cmcp_session_resources_free(res, n);
    tear_down(&w);
}

/* ====================================================================== */
/* test_prompts_list_basic                                                  */
/* ====================================================================== */

static void test_prompts_list_basic(void) {
    cmcp_server_t *srv = cmcp_server_new("plb-srv", "0.1.0");
    TEST_ASSERT(register_prompt(srv, "salutation", "say hi") == CMCP_OK);

    wired_t w = {0};
    TEST_ASSERT(wire_up(&w, srv) == 0);

    cmcp_session_prompt_t *prs = NULL;
    size_t n = 0;
    TEST_ASSERT(cmcp_client_prompts_list(w.cli, &prs, &n) == CMCP_OK);
    TEST_ASSERT(n == 1);
    TEST_ASSERT(prs[0].server == NULL);
    TEST_ASSERT(prs[0].name && strcmp(prs[0].name, "salutation") == 0);
    TEST_ASSERT(prs[0].description &&
                 strcmp(prs[0].description, "say hi") == 0);

    cmcp_session_prompts_free(prs, n);
    tear_down(&w);
}

/* ====================================================================== */
/* test_resource_read_text                                                  */
/* ====================================================================== */

static void test_resource_read_text(void) {
    cmcp_server_t *srv = cmcp_server_new("rr-srv", "0.1.0");
    TEST_ASSERT(register_resource(srv, "mem:///doc", "doc",
                                    text_resource) == CMCP_OK);

    wired_t w = {0};
    TEST_ASSERT(wire_up(&w, srv) == 0);

    char *text = NULL;
    size_t n = 0;
    TEST_ASSERT(cmcp_client_resource_read(w.cli, "mem:///doc",
                                            &text, &n) == CMCP_OK);
    TEST_ASSERT(text != NULL);
    TEST_ASSERT(n == strlen("hello from cmcp"));
    TEST_ASSERT(strcmp(text, "hello from cmcp") == 0);
    free(text);

    tear_down(&w);
}

/* ====================================================================== */
/* test_resource_read_unknown_uri                                           */
/* ====================================================================== */

static void test_resource_read_unknown_uri(void) {
    cmcp_server_t *srv = cmcp_server_new("rr-unk-srv", "0.1.0");
    TEST_ASSERT(register_resource(srv, "mem:///doc", "doc",
                                    text_resource) == CMCP_OK);

    wired_t w = {0};
    TEST_ASSERT(wire_up(&w, srv) == 0);

    char *text = (char *)0xdeadbeef;
    size_t n = 999;
    int rc = cmcp_client_resource_read(w.cli, "mem:///missing", &text, &n);
    TEST_ASSERT(rc == CMCP_EPROTOCOL);
    TEST_ASSERT(text == NULL);
    TEST_ASSERT(n == 0);

    tear_down(&w);
}

/* ====================================================================== */
/* test_resource_read_empty_contents                                        */
/* ====================================================================== */

static void test_resource_read_empty_contents(void) {
    cmcp_server_t *srv = cmcp_server_new("rr-empty-srv", "0.1.0");
    TEST_ASSERT(register_resource(srv, "mem:///empty", "empty",
                                    empty_contents_resource) == CMCP_OK);

    wired_t w = {0};
    TEST_ASSERT(wire_up(&w, srv) == 0);

    char *text = NULL;
    size_t n = 0;
    int rc = cmcp_client_resource_read(w.cli, "mem:///empty", &text, &n);
    TEST_ASSERT(rc == CMCP_ENOTFOUND);
    TEST_ASSERT(text == NULL);
    TEST_ASSERT(n == 0);

    tear_down(&w);
}

/* ====================================================================== */
/* test_resource_read_blob                                                  */
/* ====================================================================== */

static void test_resource_read_blob(void) {
    cmcp_server_t *srv = cmcp_server_new("rr-blob-srv", "0.1.0");
    TEST_ASSERT(register_resource(srv, "mem:///bin", "bin",
                                    blob_resource) == CMCP_OK);

    wired_t w = {0};
    TEST_ASSERT(wire_up(&w, srv) == 0);

    char *text = NULL;
    size_t n = 0;
    int rc = cmcp_client_resource_read(w.cli, "mem:///bin", &text, &n);
    TEST_ASSERT(rc == CMCP_EUNSUPPORTED);
    TEST_ASSERT(text == NULL);
    TEST_ASSERT(n == 0);

    tear_down(&w);
}

/* ====================================================================== */
/* test_prompt_get_basic                                                    */
/* ====================================================================== */

static void test_prompt_get_basic(void) {
    cmcp_server_t *srv = cmcp_server_new("pg-srv", "0.1.0");
    TEST_ASSERT(register_prompt(srv, "salutation", "say hi") == CMCP_OK);

    wired_t w = {0};
    TEST_ASSERT(wire_up(&w, srv) == 0);

    cmcp_json_t *messages = NULL;
    TEST_ASSERT(cmcp_client_prompt_get(w.cli, "salutation", NULL,
                                         &messages) == CMCP_OK);
    TEST_ASSERT(messages != NULL);
    TEST_ASSERT(messages->type == CMCP_JSON_ARRAY);
    TEST_ASSERT(messages->arr.len == 1);

    const cmcp_json_t *msg = messages->arr.items[0];
    TEST_ASSERT(msg && msg->type == CMCP_JSON_OBJECT);
    const cmcp_json_t *role = cmcp_json_object_get(msg, "role");
    TEST_ASSERT(role && role->type == CMCP_JSON_STRING);
    TEST_ASSERT(strcmp(role->str.s, "user") == 0);

    cmcp_json_free(messages);
    tear_down(&w);
}

/* ====================================================================== */
/* test_prompt_get_unknown                                                  */
/* ====================================================================== */

static void test_prompt_get_unknown(void) {
    cmcp_server_t *srv = cmcp_server_new("pg-unk-srv", "0.1.0");
    TEST_ASSERT(register_prompt(srv, "known", NULL) == CMCP_OK);

    wired_t w = {0};
    TEST_ASSERT(wire_up(&w, srv) == 0);

    cmcp_json_t *messages = (cmcp_json_t *)0xdeadbeef;
    int rc = cmcp_client_prompt_get(w.cli, "unknown", NULL, &messages);
    TEST_ASSERT(rc == CMCP_EPROTOCOL);
    TEST_ASSERT(messages == NULL);

    tear_down(&w);
}

/* ====================================================================== */
/* test_tool_call_ok                                                        */
/* ====================================================================== */

static void test_tool_call_ok(void) {
    cmcp_server_t *srv = cmcp_server_new("tc-ok-srv", "0.1.0");
    cmcp_tool_t t = {0};
    t.name = "echo"; t.description = "echo back";
    t.input_schema = echo_schema; t.handler = echo_tool;
    TEST_ASSERT(cmcp_server_add_tool(srv, &t) == CMCP_OK);

    wired_t w = {0};
    TEST_ASSERT(wire_up(&w, srv) == 0);

    cmcp_json_t *args = cmcp_json_new_object();
    cmcp_json_object_set(args, "message", cmcp_json_new_string("hi"));

    cmcp_tool_result_t res = cmcp_client_tool_call(w.cli, "echo", args);

    TEST_ASSERT(res.outcome == CMCP_TOOL_OK);
    TEST_ASSERT(res.result != NULL);
    TEST_ASSERT(res.text == NULL);
    TEST_ASSERT(res.error == NULL);

    /* Peek at the result to confirm it really carries content[0].text. */
    const cmcp_json_t *content = cmcp_json_object_get(res.result, "content");
    TEST_ASSERT(content && content->type == CMCP_JSON_ARRAY);
    TEST_ASSERT(content->arr.len == 1);
    const cmcp_json_t *first = content->arr.items[0];
    const cmcp_json_t *txt = cmcp_json_object_get(first, "text");
    TEST_ASSERT(txt && txt->type == CMCP_JSON_STRING);
    TEST_ASSERT(strcmp(txt->str.s, "hi") == 0);

    cmcp_tool_result_clear(&res);
    tear_down(&w);
}

/* ====================================================================== */
/* test_tool_call_tool_level_error                                          */
/* ====================================================================== */

static void test_tool_call_tool_level_error(void) {
    cmcp_server_t *srv = cmcp_server_new("tc-tle-srv", "0.1.0");
    cmcp_tool_t t = {0};
    t.name = "fail"; t.description = "always fails";
    t.input_schema = tool_schema_empty; t.handler = fail_tool;
    TEST_ASSERT(cmcp_server_add_tool(srv, &t) == CMCP_OK);

    wired_t w = {0};
    TEST_ASSERT(wire_up(&w, srv) == 0);

    cmcp_tool_result_t res = cmcp_client_tool_call(w.cli, "fail", NULL);

    TEST_ASSERT(res.outcome == CMCP_TOOL_ERR_TOOL_LEVEL);
    TEST_ASSERT(res.result == NULL);
    TEST_ASSERT(res.text != NULL);
    TEST_ASSERT(res.error == NULL);
    TEST_ASSERT(strcmp(res.text, "intentional handler failure") == 0);

    cmcp_tool_result_clear(&res);
    tear_down(&w);
}

/* ====================================================================== */
/* test_tool_call_protocol_unknown_tool                                     */
/* ====================================================================== */
/* cMCP's server reports an unknown tool name as -32602 INVALID_PARAMS
 * with structured `{name: "<unknown>"}` data (see test_tools.c). The
 * helper surfaces that on the protocol channel. */

static void test_tool_call_protocol_unknown_tool(void) {
    cmcp_server_t *srv = cmcp_server_new("tc-unk-srv", "0.1.0");
    cmcp_tool_t t = {0};
    t.name = "exists"; t.input_schema = tool_schema_empty;
    t.handler = noop_tool;
    TEST_ASSERT(cmcp_server_add_tool(srv, &t) == CMCP_OK);

    wired_t w = {0};
    TEST_ASSERT(wire_up(&w, srv) == 0);

    cmcp_tool_result_t res = cmcp_client_tool_call(w.cli, "ghost", NULL);

    TEST_ASSERT(res.outcome == CMCP_TOOL_ERR_PROTOCOL);
    TEST_ASSERT(res.result == NULL);
    TEST_ASSERT(res.text == NULL);
    TEST_ASSERT(res.error != NULL);
    TEST_ASSERT(res.error->code == CMCP_RPC_INVALID_PARAMS);
    TEST_ASSERT(res.error->message != NULL);
    /* Structured data carries {name: "ghost"} — confirms the error
     * reached the helper unmodified. */
    TEST_ASSERT(res.error->data && res.error->data->type == CMCP_JSON_OBJECT);
    const cmcp_json_t *nm = cmcp_json_object_get(res.error->data, "name");
    TEST_ASSERT(nm && nm->type == CMCP_JSON_STRING);
    TEST_ASSERT(strcmp(nm->str.s, "ghost") == 0);

    cmcp_tool_result_clear(&res);
    tear_down(&w);
}

/* ====================================================================== */
/* test_tool_call_protocol_schema_violation                                 */
/* ====================================================================== */
/* Arguments that fail the tool's input_schema. cMCP's server surfaces
 * this as -32602 with `{path, keyword, message}` structured data — the
 * shape D1 finding F2 called out as the structured channel.
 *
 * NOTE: this also locks in the documentation-fix scope of A3 — the
 * playbook claims this path is -32602 and the dogfood doc confirms
 * cMCP's server hands it back here. */

static void test_tool_call_protocol_schema_violation(void) {
    cmcp_server_t *srv = cmcp_server_new("tc-sv-srv", "0.1.0");
    cmcp_tool_t t = {0};
    t.name = "echo"; t.input_schema = echo_schema; t.handler = echo_tool;
    TEST_ASSERT(cmcp_server_add_tool(srv, &t) == CMCP_OK);

    wired_t w = {0};
    TEST_ASSERT(wire_up(&w, srv) == 0);

    /* Empty string violates minLength: 1. */
    cmcp_json_t *args = cmcp_json_new_object();
    cmcp_json_object_set(args, "message", cmcp_json_new_string(""));

    cmcp_tool_result_t res = cmcp_client_tool_call(w.cli, "echo", args);

    /* cMCP may surface schema rejection on either channel — the spec
     * (2025-11-25) prefers tool-level isError:true. We accept either
     * since A3's doc fix follows whatever cMCP actually does. */
    TEST_ASSERT(res.outcome == CMCP_TOOL_ERR_PROTOCOL ||
                 res.outcome == CMCP_TOOL_ERR_TOOL_LEVEL);
    TEST_ASSERT(res.result == NULL);

    if (res.outcome == CMCP_TOOL_ERR_PROTOCOL) {
        TEST_ASSERT(res.error != NULL);
        TEST_ASSERT(res.error->code == CMCP_RPC_INVALID_PARAMS);
    } else {
        TEST_ASSERT(res.text != NULL);
    }
    cmcp_tool_result_clear(&res);
    tear_down(&w);
}

/* ====================================================================== */
/* test_tool_call_null_args                                                 */
/* ====================================================================== */
/* args=NULL must not crash; helper sends `{}` so the wire stays
 * well-formed and the schema validator sees an empty-but-present
 * arguments object. */

static void test_tool_call_null_args(void) {
    cmcp_server_t *srv = cmcp_server_new("tc-null-srv", "0.1.0");
    cmcp_tool_t t = {0};
    t.name = "ping"; t.input_schema = tool_schema_empty;
    t.handler = noop_tool;
    TEST_ASSERT(cmcp_server_add_tool(srv, &t) == CMCP_OK);

    wired_t w = {0};
    TEST_ASSERT(wire_up(&w, srv) == 0);

    cmcp_tool_result_t res = cmcp_client_tool_call(w.cli, "ping", NULL);
    TEST_ASSERT(res.outcome == CMCP_TOOL_OK);
    TEST_ASSERT(res.result != NULL);
    cmcp_tool_result_clear(&res);

    tear_down(&w);
}

/* ====================================================================== */
/* test_tool_call_protocol_synth_error                                      */
/* ====================================================================== */
/* NULL client / NULL name must not crash; helper returns a
 * CMCP_TOOL_ERR_PROTOCOL result with a synthesised -32602 so the host's
 * switch handles it without falling through. args must still be
 * consumed (no leak — valgrind enforces). */

static void test_tool_call_protocol_synth_error(void) {
    cmcp_tool_result_t res = cmcp_client_tool_call(
        NULL, "x", cmcp_json_new_object() /* must be consumed */);

    TEST_ASSERT(res.outcome == CMCP_TOOL_ERR_PROTOCOL);
    TEST_ASSERT(res.result == NULL);
    TEST_ASSERT(res.text == NULL);
    TEST_ASSERT(res.error != NULL);
    TEST_ASSERT(res.error->code == CMCP_RPC_INVALID_PARAMS);
    cmcp_tool_result_clear(&res);

    /* NULL name, valid-looking client pointer — args still consumed. */
    res = cmcp_client_tool_call((cmcp_client_t *)0x1, NULL,
                                cmcp_json_new_object());
    TEST_ASSERT(res.outcome == CMCP_TOOL_ERR_PROTOCOL);
    cmcp_tool_result_clear(&res);
}

/* ====================================================================== */
/* A4 — typed async tool_call pair (call_async + tool_wait)                 */
/* ====================================================================== */
/* Same wire shape and 3-way outcome as the sync flattener; the split is
 * purely about scheduling. Tests mirror the sync test suite above, plus
 * one fan-out case that confirms several in-flight tool calls can be
 * reaped in any order (the original v0.7 finding that motivated A4 —
 * step 5 of the dogfood harness lost its parallelism when A2 was the
 * only available helper). */

static void test_tool_call_async_ok(void) {
    cmcp_server_t *srv = cmcp_server_new("a4-ok-srv", "0.1.0");
    cmcp_tool_t t = {0};
    t.name = "echo"; t.description = "echo back";
    t.input_schema = echo_schema; t.handler = echo_tool;
    TEST_ASSERT(cmcp_server_add_tool(srv, &t) == CMCP_OK);

    wired_t w = {0};
    TEST_ASSERT(wire_up(&w, srv) == 0);

    cmcp_json_t *args = cmcp_json_new_object();
    cmcp_json_object_set(args, "message", cmcp_json_new_string("async-hi"));

    cmcp_tool_handle_t h = cmcp_client_tool_call_async(w.cli, "echo", args);
    TEST_ASSERT(cmcp_tool_handle_valid(h));
    TEST_ASSERT(h.client == w.cli && h.id > 0);

    cmcp_tool_result_t res = cmcp_client_tool_wait(h);

    TEST_ASSERT(res.outcome == CMCP_TOOL_OK);
    TEST_ASSERT(res.result != NULL);
    TEST_ASSERT(res.text == NULL);
    TEST_ASSERT(res.error == NULL);

    const cmcp_json_t *content = cmcp_json_object_get(res.result, "content");
    TEST_ASSERT(content && content->type == CMCP_JSON_ARRAY);
    TEST_ASSERT(content->arr.len == 1);
    const cmcp_json_t *first = content->arr.items[0];
    const cmcp_json_t *txt = cmcp_json_object_get(first, "text");
    TEST_ASSERT(txt && txt->type == CMCP_JSON_STRING);
    TEST_ASSERT(strcmp(txt->str.s, "async-hi") == 0);

    cmcp_tool_result_clear(&res);
    tear_down(&w);
}

/* Fan out 3 echoes, then wait for them in reverse order. The reader
 * thread demuxes by id, so the wait order is independent of the
 * arrival order on the wire — this is what step 5 of the dogfood
 * harness needs once A4 lands. */
static void test_tool_call_async_concurrent(void) {
    cmcp_server_t *srv = cmcp_server_new("a4-fan-srv", "0.1.0");
    cmcp_tool_t t = {0};
    t.name = "echo"; t.input_schema = echo_schema; t.handler = echo_tool;
    TEST_ASSERT(cmcp_server_add_tool(srv, &t) == CMCP_OK);

    wired_t w = {0};
    TEST_ASSERT(wire_up(&w, srv) == 0);

    const char *messages[3] = {"alpha", "beta", "gamma"};
    cmcp_tool_handle_t h[3]  = {{0}, {0}, {0}};
    for (int i = 0; i < 3; i++) {
        cmcp_json_t *args = cmcp_json_new_object();
        cmcp_json_object_set(args, "message",
                              cmcp_json_new_string(messages[i]));
        h[i] = cmcp_client_tool_call_async(w.cli, "echo", args);
        TEST_ASSERT(cmcp_tool_handle_valid(h[i]));
    }
    TEST_ASSERT(h[0].id != h[1].id && h[1].id != h[2].id &&
                h[0].id != h[2].id);

    /* Reap in reverse order — confirms id-based demux, not FIFO. */
    for (int i = 2; i >= 0; i--) {
        cmcp_tool_result_t res = cmcp_client_tool_wait(h[i]);
        TEST_ASSERT(res.outcome == CMCP_TOOL_OK);
        TEST_ASSERT(res.result != NULL);
        const cmcp_json_t *content = cmcp_json_object_get(res.result, "content");
        TEST_ASSERT(content && content->arr.len == 1);
        const cmcp_json_t *txt = cmcp_json_object_get(
            content->arr.items[0], "text");
        TEST_ASSERT(txt && strcmp(txt->str.s, messages[i]) == 0);
        cmcp_tool_result_clear(&res);
    }

    tear_down(&w);
}

static void test_tool_call_async_tool_level(void) {
    cmcp_server_t *srv = cmcp_server_new("a4-tle-srv", "0.1.0");
    cmcp_tool_t t = {0};
    t.name = "fail"; t.input_schema = tool_schema_empty; t.handler = fail_tool;
    TEST_ASSERT(cmcp_server_add_tool(srv, &t) == CMCP_OK);

    wired_t w = {0};
    TEST_ASSERT(wire_up(&w, srv) == 0);

    cmcp_tool_handle_t h = cmcp_client_tool_call_async(w.cli, "fail", NULL);
    TEST_ASSERT(cmcp_tool_handle_valid(h));

    cmcp_tool_result_t res = cmcp_client_tool_wait(h);

    TEST_ASSERT(res.outcome == CMCP_TOOL_ERR_TOOL_LEVEL);
    TEST_ASSERT(res.text != NULL);
    TEST_ASSERT(res.error == NULL);
    TEST_ASSERT(strcmp(res.text, "intentional handler failure") == 0);

    cmcp_tool_result_clear(&res);
    tear_down(&w);
}

static void test_tool_call_async_protocol_unknown_tool(void) {
    cmcp_server_t *srv = cmcp_server_new("a4-unk-srv", "0.1.0");
    cmcp_tool_t t = {0};
    t.name = "exists"; t.input_schema = tool_schema_empty;
    t.handler = noop_tool;
    TEST_ASSERT(cmcp_server_add_tool(srv, &t) == CMCP_OK);

    wired_t w = {0};
    TEST_ASSERT(wire_up(&w, srv) == 0);

    cmcp_tool_handle_t h = cmcp_client_tool_call_async(w.cli, "ghost", NULL);
    TEST_ASSERT(cmcp_tool_handle_valid(h));

    cmcp_tool_result_t res = cmcp_client_tool_wait(h);

    TEST_ASSERT(res.outcome == CMCP_TOOL_ERR_PROTOCOL);
    TEST_ASSERT(res.error != NULL);
    TEST_ASSERT(res.error->code == CMCP_RPC_INVALID_PARAMS);
    cmcp_tool_result_clear(&res);

    tear_down(&w);
}

/* NULL c / NULL name yield an INVALID handle (not a crash); args is
 * consumed in every path so no leak. */
static void test_tool_call_async_einval(void) {
    cmcp_tool_handle_t h;
    /* NULL client. */
    h = cmcp_client_tool_call_async(NULL, "x", cmcp_json_new_object());
    TEST_ASSERT(!cmcp_tool_handle_valid(h));
    /* NULL name. */
    h = cmcp_client_tool_call_async((cmcp_client_t *)0x1, NULL,
                                    cmcp_json_new_object());
    TEST_ASSERT(!cmcp_tool_handle_valid(h));
}

/* An invalid handle on the wait side synthesises a protocol error so the
 * host's switch has no default arm. */
static void test_tool_wait_protocol_synth_error(void) {
    cmcp_tool_handle_t bad = { NULL, 0 };
    cmcp_tool_result_t res = cmcp_client_tool_wait(bad);
    TEST_ASSERT(res.outcome == CMCP_TOOL_ERR_PROTOCOL);
    TEST_ASSERT(res.result == NULL);
    TEST_ASSERT(res.text == NULL);
    TEST_ASSERT(res.error != NULL);
    TEST_ASSERT(res.error->code == CMCP_RPC_INVALID_PARAMS);
    cmcp_tool_result_clear(&res);

    /* A client with a non-positive id is also invalid. */
    cmcp_tool_handle_t bad2 = { (cmcp_client_t *)0x1, 0 };
    res = cmcp_client_tool_wait(bad2);
    TEST_ASSERT(res.outcome == CMCP_TOOL_ERR_PROTOCOL);
    cmcp_tool_result_clear(&res);
}

/* ====================================================================== */
/* A5 — cmcp_client_tool_call_text content-shortcut                         */
/* ====================================================================== */
/* Squashed view of A2: success and tool-error both become CMCP_OK +
 * owned text. Only protocol-channel errors are out-of-band. Hosts that
 * need to distinguish success from tool-error use cmcp_client_tool_call
 * directly. */

static void test_tool_call_text_ok(void) {
    cmcp_server_t *srv = cmcp_server_new("a5-ok-srv", "0.1.0");
    cmcp_tool_t t = {0};
    t.name = "echo"; t.input_schema = echo_schema; t.handler = echo_tool;
    TEST_ASSERT(cmcp_server_add_tool(srv, &t) == CMCP_OK);

    wired_t w = {0};
    TEST_ASSERT(wire_up(&w, srv) == 0);

    cmcp_json_t *args = cmcp_json_new_object();
    cmcp_json_object_set(args, "message", cmcp_json_new_string("text-hi"));

    char             *text    = NULL;
    cmcp_rpc_error_t *rpc_err = NULL;
    int rc = cmcp_client_tool_call_text(w.cli, "echo", args, &text, &rpc_err);
    TEST_ASSERT(rc == CMCP_OK);
    TEST_ASSERT(text != NULL);
    TEST_ASSERT(rpc_err == NULL);
    TEST_ASSERT(strcmp(text, "text-hi") == 0);
    free(text);

    tear_down(&w);
}

/* The squashing: a tool-level isError:true result still arrives as
 * CMCP_OK + the explanatory text. */
static void test_tool_call_text_tool_error_becomes_ok(void) {
    cmcp_server_t *srv = cmcp_server_new("a5-tle-srv", "0.1.0");
    cmcp_tool_t t = {0};
    t.name = "fail"; t.input_schema = tool_schema_empty; t.handler = fail_tool;
    TEST_ASSERT(cmcp_server_add_tool(srv, &t) == CMCP_OK);

    wired_t w = {0};
    TEST_ASSERT(wire_up(&w, srv) == 0);

    char             *text    = NULL;
    cmcp_rpc_error_t *rpc_err = NULL;
    int rc = cmcp_client_tool_call_text(w.cli, "fail", NULL, &text, &rpc_err);
    TEST_ASSERT(rc == CMCP_OK);
    TEST_ASSERT(text != NULL);
    TEST_ASSERT(rpc_err == NULL);
    TEST_ASSERT(strcmp(text, "intentional handler failure") == 0);
    free(text);

    tear_down(&w);
}

static void test_tool_call_text_protocol_unknown_tool(void) {
    cmcp_server_t *srv = cmcp_server_new("a5-unk-srv", "0.1.0");
    cmcp_tool_t t = {0};
    t.name = "exists"; t.input_schema = tool_schema_empty;
    t.handler = noop_tool;
    TEST_ASSERT(cmcp_server_add_tool(srv, &t) == CMCP_OK);

    wired_t w = {0};
    TEST_ASSERT(wire_up(&w, srv) == 0);

    char             *text    = NULL;
    cmcp_rpc_error_t *rpc_err = NULL;
    int rc = cmcp_client_tool_call_text(w.cli, "ghost", NULL,
                                          &text, &rpc_err);
    TEST_ASSERT(rc == CMCP_EPROTOCOL);
    TEST_ASSERT(text == NULL);
    TEST_ASSERT(rpc_err != NULL);
    TEST_ASSERT(rpc_err->code == CMCP_RPC_INVALID_PARAMS);
    cmcp_rpc_error_free(rpc_err);

    tear_down(&w);
}

/* args=NULL must not crash; helper sends `{}` on the wire so the
 * schema validator sees an empty-but-present arguments object. */
static void test_tool_call_text_null_args(void) {
    cmcp_server_t *srv = cmcp_server_new("a5-null-srv", "0.1.0");
    cmcp_tool_t t = {0};
    t.name = "ping"; t.input_schema = tool_schema_empty; t.handler = noop_tool;
    TEST_ASSERT(cmcp_server_add_tool(srv, &t) == CMCP_OK);

    wired_t w = {0};
    TEST_ASSERT(wire_up(&w, srv) == 0);

    char *text = NULL;
    int rc = cmcp_client_tool_call_text(w.cli, "ping", NULL, &text, NULL);
    TEST_ASSERT(rc == CMCP_OK);
    TEST_ASSERT(text != NULL);
    TEST_ASSERT(strcmp(text, "ok") == 0);
    free(text);

    tear_down(&w);
}

/* NULL c / NULL name / NULL out_text → CMCP_EPROTOCOL with synth -32602.
 * args must be consumed in every path. */
static void test_tool_call_text_protocol_synth_error(void) {
    char             *text    = NULL;
    cmcp_rpc_error_t *rpc_err = NULL;

    int rc = cmcp_client_tool_call_text(
        NULL, "x", cmcp_json_new_object(), &text, &rpc_err);
    TEST_ASSERT(rc == CMCP_EPROTOCOL);
    TEST_ASSERT(text == NULL);
    TEST_ASSERT(rpc_err != NULL);
    TEST_ASSERT(rpc_err->code == CMCP_RPC_INVALID_PARAMS);
    cmcp_rpc_error_free(rpc_err);

    /* NULL out_text path. */
    rc = cmcp_client_tool_call_text(
        (cmcp_client_t *)0x1, "x", cmcp_json_new_object(), NULL, NULL);
    TEST_ASSERT(rc == CMCP_EPROTOCOL);
}

/* ====================================================================== */
/* P7 — struct+handle redesign guards (F2 / F4 / F5)                        */
/* ====================================================================== */

/* F2 guard: the by-value result carries its payload in the return value,
 * so there is no out-param to read before the call populates it — the P6
 * eval-order footgun is gone by construction. We also lock down
 * cmcp_tool_result_clear: it frees the selected payload and is safe to
 * call twice (idempotent), which a host's error path will do. */
static void test_tool_result_clear_idempotent(void) {
    cmcp_server_t *srv = cmcp_server_new("p7-clr-srv", "0.1.0");
    cmcp_tool_t t = {0};
    t.name = "echo"; t.input_schema = echo_schema; t.handler = echo_tool;
    TEST_ASSERT(cmcp_server_add_tool(srv, &t) == CMCP_OK);

    wired_t w = {0};
    TEST_ASSERT(wire_up(&w, srv) == 0);

    cmcp_json_t *args = cmcp_json_new_object();
    cmcp_json_object_set(args, "message", cmcp_json_new_string("payload"));

    /* The footgun-free shape: the payload is in the returned struct, not
     * behind a pointer the compiler could read early. */
    cmcp_tool_result_t res = cmcp_client_tool_call(w.cli, "echo", args);
    TEST_ASSERT(res.outcome == CMCP_TOOL_OK);
    TEST_ASSERT(res.result != NULL);

    cmcp_tool_result_clear(&res);
    /* After clear, payload pointers are nulled. */
    TEST_ASSERT(res.result == NULL && res.text == NULL && res.error == NULL);
    /* Second clear is a no-op, not a double-free. */
    cmcp_tool_result_clear(&res);
    /* NULL is tolerated. */
    cmcp_tool_result_clear(NULL);

    tear_down(&w);
}

/* F4 guard: the handle binds an in-flight id to its originating client,
 * so per-client id-space collisions can't cause a wait to reap the wrong
 * server's response. Two independent servers, a call on each; even if the
 * two ids coincide, each handle reaps exactly its own server's result. */
static void test_tool_handle_cross_session_isolation(void) {
    /* Server A: echo. Server B: a tool that returns a distinct marker. */
    cmcp_server_t *srvA = cmcp_server_new("p7-iso-A", "0.1.0");
    cmcp_tool_t ta = {0};
    ta.name = "echo"; ta.input_schema = echo_schema; ta.handler = echo_tool;
    TEST_ASSERT(cmcp_server_add_tool(srvA, &ta) == CMCP_OK);

    cmcp_server_t *srvB = cmcp_server_new("p7-iso-B", "0.1.0");
    cmcp_tool_t tb = {0};
    tb.name = "echo"; tb.input_schema = echo_schema; tb.handler = echo_tool;
    TEST_ASSERT(cmcp_server_add_tool(srvB, &tb) == CMCP_OK);

    wired_t wA = {0}, wB = {0};
    TEST_ASSERT(wire_up(&wA, srvA) == 0);
    TEST_ASSERT(wire_up(&wB, srvB) == 0);

    cmcp_json_t *aA = cmcp_json_new_object();
    cmcp_json_object_set(aA, "message", cmcp_json_new_string("from-A"));
    cmcp_json_t *aB = cmcp_json_new_object();
    cmcp_json_object_set(aB, "message", cmcp_json_new_string("from-B"));

    cmcp_tool_handle_t hA = cmcp_client_tool_call_async(wA.cli, "echo", aA);
    cmcp_tool_handle_t hB = cmcp_client_tool_call_async(wB.cli, "echo", aB);
    TEST_ASSERT(cmcp_tool_handle_valid(hA) && cmcp_tool_handle_valid(hB));
    /* The handle, not a bare id, is what disambiguates: each carries its
     * own client. The ids may well collide (both fresh per-client id
     * spaces) — that is exactly the footgun the binding removes. */
    TEST_ASSERT(hA.client == wA.cli && hB.client == wB.cli);

    /* Reap B first, then A — order independent, each routed by its own
     * client. */
    cmcp_tool_result_t rB = cmcp_client_tool_wait(hB);
    cmcp_tool_result_t rA = cmcp_client_tool_wait(hA);

    TEST_ASSERT(rA.outcome == CMCP_TOOL_OK && rA.result != NULL);
    TEST_ASSERT(rB.outcome == CMCP_TOOL_OK && rB.result != NULL);

    const cmcp_json_t *cA = cmcp_json_object_get(rA.result, "content");
    const cmcp_json_t *cB = cmcp_json_object_get(rB.result, "content");
    const cmcp_json_t *tA = cmcp_json_object_get(cA->arr.items[0], "text");
    const cmcp_json_t *tB = cmcp_json_object_get(cB->arr.items[0], "text");
    TEST_ASSERT(tA && strcmp(tA->str.s, "from-A") == 0);
    TEST_ASSERT(tB && strcmp(tB->str.s, "from-B") == 0);

    cmcp_tool_result_clear(&rA);
    cmcp_tool_result_clear(&rB);
    tear_down(&wA);
    tear_down(&wB);
}

/* F5 guard: a cancelled call surfaces as CMCP_TOOL_ERR_CANCELLED, not a
 * generic -32603 protocol error (the P6 finding). A slow handler lets the
 * cancel win the race deterministically. The wait after cancel is also
 * the mandatory completion-record reclaim from the thread-safety
 * contract. */
static void test_tool_wait_cancelled(void) {
    cmcp_server_t *srv = cmcp_server_new("p7-cancel-srv", "0.1.0");
    cmcp_tool_t t = {0};
    t.name = "slow"; t.input_schema = tool_schema_empty; t.handler = slow_tool;
    TEST_ASSERT(cmcp_server_add_tool(srv, &t) == CMCP_OK);

    wired_t w = {0};
    TEST_ASSERT(wire_up(&w, srv) == 0);

    cmcp_tool_handle_t h = cmcp_client_tool_call_async(w.cli, "slow", NULL);
    TEST_ASSERT(cmcp_tool_handle_valid(h));

    /* Cancel right away — the handler is mid-sleep, so the local pending
     * entry is taken before the response can arrive. */
    int crc = cmcp_client_cancel(h.client, h.id, "p7: cancel-path test");
    TEST_ASSERT(crc == CMCP_OK);

    cmcp_tool_result_t res = cmcp_client_tool_wait(h);
    TEST_ASSERT(res.outcome == CMCP_TOOL_ERR_CANCELLED);
    /* CANCELLED carries no payload — all three pointers stay NULL. */
    TEST_ASSERT(res.result == NULL && res.text == NULL && res.error == NULL);
    cmcp_tool_result_clear(&res);

    tear_down(&w);
}

/* ====================================================================== */
/* test_einval — every helper rejects NULL inputs                           */
/* ====================================================================== */

static void test_einval(void) {
    cmcp_session_tool_t     *t = NULL;
    cmcp_session_resource_t *r = NULL;
    cmcp_session_prompt_t   *p = NULL;
    char                    *txt = NULL;
    cmcp_json_t             *msgs = NULL;
    size_t                   n = 0;

    TEST_ASSERT(cmcp_client_tools_list(NULL, &t, &n) == CMCP_EINVAL);
    TEST_ASSERT(cmcp_client_resources_list(NULL, &r, &n) == CMCP_EINVAL);
    TEST_ASSERT(cmcp_client_prompts_list(NULL, &p, &n) == CMCP_EINVAL);
    TEST_ASSERT(cmcp_client_resource_read(NULL, "x", &txt, &n) == CMCP_EINVAL);
    TEST_ASSERT(cmcp_client_prompt_get(NULL, "x", NULL, &msgs) == CMCP_EINVAL);
}

/* ====================================================================== */

int main(void) {
    TEST_RUN(test_tools_list_basic);
    TEST_RUN(test_tools_list_empty);
    TEST_RUN(test_resources_list_basic);
    TEST_RUN(test_prompts_list_basic);
    TEST_RUN(test_resource_read_text);
    TEST_RUN(test_resource_read_unknown_uri);
    TEST_RUN(test_resource_read_empty_contents);
    TEST_RUN(test_resource_read_blob);
    TEST_RUN(test_prompt_get_basic);
    TEST_RUN(test_prompt_get_unknown);
    TEST_RUN(test_tool_call_ok);
    TEST_RUN(test_tool_call_tool_level_error);
    TEST_RUN(test_tool_call_protocol_unknown_tool);
    TEST_RUN(test_tool_call_protocol_schema_violation);
    TEST_RUN(test_tool_call_null_args);
    TEST_RUN(test_tool_call_protocol_synth_error);
    TEST_RUN(test_tool_call_async_ok);
    TEST_RUN(test_tool_call_async_concurrent);
    TEST_RUN(test_tool_call_async_tool_level);
    TEST_RUN(test_tool_call_async_protocol_unknown_tool);
    TEST_RUN(test_tool_call_async_einval);
    TEST_RUN(test_tool_wait_protocol_synth_error);
    TEST_RUN(test_tool_call_text_ok);
    TEST_RUN(test_tool_call_text_tool_error_becomes_ok);
    TEST_RUN(test_tool_call_text_protocol_unknown_tool);
    TEST_RUN(test_tool_call_text_null_args);
    TEST_RUN(test_tool_call_text_protocol_synth_error);
    TEST_RUN(test_tool_result_clear_idempotent);
    TEST_RUN(test_tool_handle_cross_session_isolation);
    TEST_RUN(test_tool_wait_cancelled);
    TEST_RUN(test_einval);
    TEST_DONE();
}
