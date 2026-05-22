/* Conformance — Direction A: cMCP client vs the MCP TypeScript
 * reference server.
 *
 * Spawns `@modelcontextprotocol/server-everything` (the canonical
 * reference server) over a stdio pipe via cmcp_client_connect_stdio,
 * then drives it with the cMCP client library: handshake, tools/list +
 * call, resources/list + read, prompts/list + get, and progress
 * notifications. Every assertion proves cMCP's *client* speaks the MCP
 * wire format the reference implementation expects.
 *
 * Built + run by `make conformance`, which first `npm install`s the
 * pinned reference SDK into conformance/node_modules. Not part of
 * `make test` — see conformance/README.md.
 *
 * Exit code: 0 = all conformance checks passed, 1 = a divergence. */

#define _POSIX_C_SOURCE 200809L

#include "../tests/test.h"
#include "cmcp.h"
#include "cmcp_client.h"
#include "cmcp_json.h"
#include "cmcp_types.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* Default location of the reference server, relative to the repo root
 * (where `make conformance` runs this binary). Override with
 * $CMCP_CONF_EVERYTHING. */
#define EVERYTHING_JS \
    "conformance/node_modules/@modelcontextprotocol/server-everything/dist/index.js"

/* ====================================================================== */
/* Server-to-client notification tap                                       */
/* ====================================================================== */

/* Written only by the client's single reader thread; read by the main
 * thread after cmcp_client_wait returns (a happens-before edge). */
static int g_progress_count = 0;

static void on_notification(const char *method, const cmcp_json_t *params,
                            void *userdata) {
    (void)params; (void)userdata;
    if (method && strcmp(method, "notifications/progress") == 0)
        g_progress_count++;
}

/* ====================================================================== */
/* Small response helpers                                                  */
/* ====================================================================== */

/* The named array inside a result object, or NULL. */
static const cmcp_json_t *result_array(const cmcp_rpc_message_t *r,
                                       const char *key) {
    if (!r || r->error || !r->result || r->result->type != CMCP_JSON_OBJECT)
        return NULL;
    const cmcp_json_t *a = cmcp_json_object_get(r->result, key);
    return (a && a->type == CMCP_JSON_ARRAY) ? a : NULL;
}

/* Concatenated text of every {"type":"text"} item in a content array. */
static char *content_text(const cmcp_json_t *content) {
    if (!content || content->type != CMCP_JSON_ARRAY) return NULL;
    size_t cap = 1, len = 0;
    char *buf = (char *)malloc(cap);
    if (!buf) return NULL;
    buf[0] = '\0';
    for (size_t i = 0; i < content->arr.len; i++) {
        const cmcp_json_t *it = content->arr.items[i];
        if (!it || it->type != CMCP_JSON_OBJECT) continue;
        const cmcp_json_t *t = cmcp_json_object_get(it, "text");
        if (!t || t->type != CMCP_JSON_STRING) continue;
        size_t add = strlen(t->str.s);
        char *nb = (char *)realloc(buf, len + add + 1);
        if (!nb) { free(buf); return NULL; }
        buf = nb;
        memcpy(buf + len, t->str.s, add + 1);
        len += add;
    }
    return buf;
}

/* 1 if the tools/list (or any descriptor) array holds an object whose
 * "name" equals `name`. */
static int array_has_named(const cmcp_json_t *arr, const char *name) {
    if (!arr) return 0;
    for (size_t i = 0; i < arr->arr.len; i++) {
        const cmcp_json_t *o = arr->arr.items[i];
        if (!o || o->type != CMCP_JSON_OBJECT) continue;
        const cmcp_json_t *n = cmcp_json_object_get(o, "name");
        if (n && n->type == CMCP_JSON_STRING && strcmp(n->str.s, name) == 0)
            return 1;
    }
    return 0;
}

/* Build {"name":..,"arguments":..} for tools/call; `args` is consumed. */
static cmcp_json_t *call_params(const char *tool, cmcp_json_t *args) {
    cmcp_json_t *p = cmcp_json_new_object();
    cmcp_json_object_set(p, "name", cmcp_json_new_string(tool));
    cmcp_json_object_set(p, "arguments", args ? args : cmcp_json_new_object());
    return p;
}

/* First alias from `aliases` (NULL-terminated) present in the
 * descriptor array, copied into `out`; out[0]='\0' if none match. */
static void find_tool(const cmcp_json_t *arr, const char *const *aliases,
                      char *out, size_t outsz) {
    out[0] = '\0';
    for (const char *const *a = aliases; *a; a++) {
        if (array_has_named(arr, *a)) {
            snprintf(out, outsz, "%s", *a);
            return;
        }
    }
}

/* ====================================================================== */
/* Discovered tools — set by test_tools_list, gating later tests so the   */
/* harness tolerates the reference server's surface drifting between      */
/* versions (e.g. `add` was renamed `get-sum` in 2026.x).                  */
/* ====================================================================== */

static int  g_has_echo = 0;
static char g_sum_tool[64]  = {0};   /* "get-sum" / "add" */
static char g_long_tool[64] = {0};   /* long-running-operation, any spelling */

/* ====================================================================== */
/* Tests                                                                   */
/* ====================================================================== */

static void test_handshake(cmcp_client_t *cli) {
    const char *name  = cmcp_client_server_name(cli);
    const char *proto = cmcp_client_server_protocol(cli);
    TEST_ASSERT(name != NULL && *name != '\0');
    TEST_ASSERT(cmcp_client_server_version(cli) != NULL);
    /* The reference server advertises a protocol version string. */
    TEST_ASSERT(proto != NULL && *proto != '\0');
}

static void test_tools_list(cmcp_client_t *cli) {
    cmcp_rpc_message_t resp;
    TEST_ASSERT(cmcp_client_request(cli, "tools/list", NULL, &resp) == CMCP_OK);
    const cmcp_json_t *tools = result_array(&resp, "tools");
    TEST_ASSERT(tools != NULL);
    TEST_ASSERT(tools && tools->arr.len > 0);
    /* `echo` and a two-number sum tool are the long-stable reference
     * tools, though the sum tool's name has changed across versions. */
    static const char *const sum_aliases[]  = { "get-sum", "add", NULL };
    static const char *const long_aliases[] = {
        "trigger-long-running-operation", "longRunningOperation", NULL };
    g_has_echo = array_has_named(tools, "echo");
    find_tool(tools, sum_aliases,  g_sum_tool,  sizeof g_sum_tool);
    find_tool(tools, long_aliases, g_long_tool, sizeof g_long_tool);
    TEST_ASSERT(g_has_echo == 1);
    TEST_ASSERT(g_sum_tool[0] != '\0');
    cmcp_rpc_message_clear(&resp);
}

static void test_tool_echo(cmcp_client_t *cli) {
    if (!g_has_echo) { TEST_ASSERT(0 && "echo tool missing"); return; }
    cmcp_json_t *args = cmcp_json_new_object();
    cmcp_json_object_set(args, "message",
                         cmcp_json_new_string("cmcp-conformance"));
    cmcp_rpc_message_t resp;
    TEST_ASSERT(cmcp_client_request(cli, "tools/call",
                                    call_params("echo", args), &resp)
                == CMCP_OK);
    TEST_ASSERT(resp.error == NULL);
    const cmcp_json_t *content = resp.result
        ? cmcp_json_object_get(resp.result, "content") : NULL;
    char *txt = content_text(content);
    /* Reference `echo` echoes the message back inside its text. */
    TEST_ASSERT(txt != NULL && strstr(txt, "cmcp-conformance") != NULL);
    free(txt);
    cmcp_rpc_message_clear(&resp);
}

static void test_tool_add(cmcp_client_t *cli) {
    if (!g_sum_tool[0]) { TEST_ASSERT(0 && "sum tool missing"); return; }
    cmcp_json_t *args = cmcp_json_new_object();
    cmcp_json_object_set(args, "a", cmcp_json_new_int(2));
    cmcp_json_object_set(args, "b", cmcp_json_new_int(40));
    cmcp_rpc_message_t resp;
    TEST_ASSERT(cmcp_client_request(cli, "tools/call",
                                    call_params(g_sum_tool, args), &resp)
                == CMCP_OK);
    TEST_ASSERT(resp.error == NULL);
    const cmcp_json_t *content = resp.result
        ? cmcp_json_object_get(resp.result, "content") : NULL;
    char *txt = content_text(content);
    /* 2 + 40 = 42 — present somewhere in the rendered result text. */
    TEST_ASSERT(txt != NULL && strstr(txt, "42") != NULL);
    free(txt);
    cmcp_rpc_message_clear(&resp);
}

static void test_resources(cmcp_client_t *cli) {
    cmcp_rpc_message_t list;
    TEST_ASSERT(cmcp_client_request(cli, "resources/list", NULL, &list)
                == CMCP_OK);
    const cmcp_json_t *res = result_array(&list, "resources");
    TEST_ASSERT(res != NULL);
    TEST_ASSERT(res && res->arr.len > 0);

    /* Read the first advertised resource by its uri. */
    char *uri = NULL;
    if (res && res->arr.len > 0) {
        const cmcp_json_t *u = cmcp_json_object_get(res->arr.items[0], "uri");
        if (u && u->type == CMCP_JSON_STRING) uri = strdup(u->str.s);
    }
    cmcp_rpc_message_clear(&list);
    TEST_ASSERT(uri != NULL);
    if (!uri) return;

    cmcp_json_t *p = cmcp_json_new_object();
    cmcp_json_object_set(p, "uri", cmcp_json_new_string(uri));
    free(uri);
    cmcp_rpc_message_t rd;
    TEST_ASSERT(cmcp_client_request(cli, "resources/read", p, &rd) == CMCP_OK);
    TEST_ASSERT(rd.error == NULL);
    const cmcp_json_t *contents = result_array(&rd, "contents");
    TEST_ASSERT(contents != NULL);
    TEST_ASSERT(contents && contents->arr.len > 0);
    cmcp_rpc_message_clear(&rd);
}

/* 1 if a prompt descriptor takes no *required* argument — safe to
 * prompts/get with empty arguments. */
static int prompt_is_argless(const cmcp_json_t *prompt) {
    const cmcp_json_t *args = cmcp_json_object_get(prompt, "arguments");
    if (!args || args->type != CMCP_JSON_ARRAY) return 1;
    for (size_t i = 0; i < args->arr.len; i++) {
        const cmcp_json_t *a = args->arr.items[i];
        const cmcp_json_t *req = a
            ? cmcp_json_object_get(a, "required") : NULL;
        if (req && req->type == CMCP_JSON_BOOL && req->b) return 0;
    }
    return 1;
}

static void test_prompts(cmcp_client_t *cli) {
    cmcp_rpc_message_t list;
    TEST_ASSERT(cmcp_client_request(cli, "prompts/list", NULL, &list)
                == CMCP_OK);
    const cmcp_json_t *prompts = result_array(&list, "prompts");
    TEST_ASSERT(prompts != NULL);
    TEST_ASSERT(prompts && prompts->arr.len > 0);

    /* Pick the first prompt that needs no required argument. */
    char *name = NULL;
    if (prompts) {
        for (size_t i = 0; i < prompts->arr.len && !name; i++) {
            const cmcp_json_t *pr = prompts->arr.items[i];
            if (!pr || !prompt_is_argless(pr)) continue;
            const cmcp_json_t *n = cmcp_json_object_get(pr, "name");
            if (n && n->type == CMCP_JSON_STRING) name = strdup(n->str.s);
        }
    }
    cmcp_rpc_message_clear(&list);
    TEST_ASSERT(name != NULL);
    if (!name) return;

    cmcp_json_t *p = cmcp_json_new_object();
    cmcp_json_object_set(p, "name", cmcp_json_new_string(name));
    free(name);
    cmcp_rpc_message_t get;
    TEST_ASSERT(cmcp_client_request(cli, "prompts/get", p, &get) == CMCP_OK);
    TEST_ASSERT(get.error == NULL);
    const cmcp_json_t *msgs = result_array(&get, "messages");
    TEST_ASSERT(msgs != NULL);
    cmcp_rpc_message_clear(&get);
}

static void test_progress_notifications(cmcp_client_t *cli) {
    if (!g_long_tool[0]) {
        /* Reference server dropped the long-running tool — skip rather
         * than fail; progress routing is covered by test_workers.c. */
        fprintf(stderr, "    (skipped: no long-running operation tool)\n");
        return;
    }
    g_progress_count = 0;

    cmcp_json_t *args = cmcp_json_new_object();
    cmcp_json_object_set(args, "duration", cmcp_json_new_int(1));
    cmcp_json_object_set(args, "steps", cmcp_json_new_int(5));
    cmcp_json_t *p = call_params(g_long_tool, args);
    /* Ask for progress: the token lives at params._meta.progressToken. */
    cmcp_json_t *meta = cmcp_json_new_object();
    cmcp_json_object_set(meta, "progressToken",
                         cmcp_json_new_string("conf-progress"));
    cmcp_json_object_set(p, "_meta", meta);

    cmcp_rpc_message_t resp;
    TEST_ASSERT(cmcp_client_request(cli, "tools/call", p, &resp) == CMCP_OK);
    TEST_ASSERT(resp.error == NULL);
    cmcp_rpc_message_clear(&resp);

    /* Progress notifications arrive on the reader thread before the
     * final response, so by now they have all been counted. */
    TEST_ASSERT(g_progress_count > 0);
}

/* ====================================================================== */

int main(void) {
    const char *js = getenv("CMCP_CONF_EVERYTHING");
    if (!js || !*js) js = EVERYTHING_JS;

    if (access(js, R_OK) != 0) {
        fprintf(stderr,
            "client_vs_ts: reference server not found at %s\n"
            "  Run `make conformance` — it npm-installs the pinned\n"
            "  reference SDK into conformance/node_modules first.\n", js);
        return 1;
    }

    fprintf(stderr, "client_vs_ts (cMCP client vs TS server-everything):\n");

    cmcp_client_t *cli = cmcp_client_new("cmcp-conformance", "0.0.1");
    if (!cli) { fprintf(stderr, "  client alloc failed\n"); return 1; }
    cmcp_client_set_notification_handler(cli, on_notification, NULL);

    char *argv[] = { "node", (char *)js, "stdio", NULL };
    if (cmcp_client_connect_stdio(cli, "node", argv, NULL) != CMCP_OK) {
        fprintf(stderr, "  failed to spawn / handshake the reference "
                        "server (node %s stdio)\n", js);
        cmcp_client_free(cli);
        return 1;
    }

    TEST_RUN_ARG(test_handshake, cli);
    TEST_RUN_ARG(test_tools_list, cli);
    TEST_RUN_ARG(test_tool_echo, cli);
    TEST_RUN_ARG(test_tool_add, cli);
    TEST_RUN_ARG(test_resources, cli);
    TEST_RUN_ARG(test_prompts, cli);
    TEST_RUN_ARG(test_progress_notifications, cli);

    cmcp_client_free(cli);   /* closes the transport, reaps the child */

    TEST_DONE();
}
