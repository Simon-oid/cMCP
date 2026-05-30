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
    TEST_RUN(test_einval);
    TEST_DONE();
}
