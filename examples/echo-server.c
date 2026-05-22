/* echo-server — minimal MCP server over stdio.
 *
 * Registers two tools — `echo` (returns its `text` argument back) and
 * `add` (returns the sum of two integers) — and runs the lifecycle
 * loop on stdin/stdout. Used by `cmcp-inspect` smoke tests and as a
 * "hello world" example for anyone reading the repo.
 *
 * Build:  make examples/echo-server
 * Run:    ./examples/echo-server   (drives stdio; pipe JSON-RPC frames in) */

#include "cmcp.h"
#include "cmcp_json.h"
#include "cmcp_server.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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

static int add_handler(const cmcp_json_t *args, void *userdata,
                        cmcp_handler_ctx_t *hctx,
                        cmcp_json_t **out_content, int *out_is_error) {
    (void)userdata; (void)hctx;
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

int main(void) {
    cmcp_server_t *s = cmcp_server_new("echo-server", "0.1.0");
    if (!s) {
        fprintf(stderr, "echo-server: out of memory\n");
        return 1;
    }

    int rc = cmcp_server_add_tool(s, &(cmcp_tool_t){
        .name        = "echo",
        .description = "Return the input text unchanged.",
        .input_schema = "{\"type\":\"object\","
                         "\"properties\":{\"text\":{\"type\":\"string\"}},"
                         "\"required\":[\"text\"]}",
        .handler     = echo_handler,
    });
    if (rc != CMCP_OK) { fprintf(stderr, "echo-server: add echo failed\n"); return 1; }

    rc = cmcp_server_add_tool(s, &(cmcp_tool_t){
        .name        = "add",
        .description = "Add two integers.",
        .input_schema = "{\"type\":\"object\","
                         "\"properties\":{"
                            "\"a\":{\"type\":\"integer\"},"
                            "\"b\":{\"type\":\"integer\"}"
                         "},"
                         "\"required\":[\"a\",\"b\"]}",
        .handler     = add_handler,
    });
    if (rc != CMCP_OK) { fprintf(stderr, "echo-server: add add failed\n"); return 1; }

    int run_rc = cmcp_server_run_stdio(s);
    cmcp_server_free(s);
    return run_rc == CMCP_OK ? 0 : 1;
}
