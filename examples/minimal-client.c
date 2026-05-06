/* minimal-client — "hello world" for the cMCP client API.
 *
 * Spawns an MCP server as a child process, runs the handshake,
 * lists its tools, and (if the server has an `echo` tool) calls
 * it once with {"text":"hello from cMCP"}.
 *
 * The whole point of this example is to show that connecting to
 * an MCP server is a few lines of C: client_new, connect_stdio,
 * client_request, done. The library handles fork/exec, the stdio
 * pipe pair, the JSON-RPC framing, the initialize handshake, and
 * cleans up the child on free.
 *
 * Build:  make examples/minimal-client
 * Run:    ./examples/minimal-client -- ./examples/echo-server
 *         ./examples/minimal-client -- ./tools/crag-mcp/crag-mcp /path/to/db
 */

#include "cmcp.h"
#include "cmcp_client.h"
#include "cmcp_json.h"
#include "cmcp_types.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int has_tool(const cmcp_json_t *result, const char *name) {
    if (!result || result->type != CMCP_JSON_OBJECT) return 0;
    const cmcp_json_t *arr = cmcp_json_object_get(result, "tools");
    if (!arr || arr->type != CMCP_JSON_ARRAY) return 0;
    for (size_t i = 0; i < arr->arr.len; i++) {
        const cmcp_json_t *t = arr->arr.items[i];
        if (!t || t->type != CMCP_JSON_OBJECT) continue;
        const cmcp_json_t *n = cmcp_json_object_get(t, "name");
        if (n && n->type == CMCP_JSON_STRING && strcmp(n->str.s, name) == 0)
            return 1;
    }
    return 0;
}

int main(int argc, char **argv) {
    /* Argv shape: minimal-client [--] <server-cmd> [server-args...] */
    int first = 1;
    if (first < argc && strcmp(argv[first], "--") == 0) first++;
    if (first >= argc) {
        fprintf(stderr, "usage: %s [--] <server-cmd> [server-args...]\n",
                argv[0]);
        return 2;
    }
    char **server_argv = &argv[first];

    cmcp_client_t *cli = cmcp_client_new("minimal-client", CMCP_VERSION);
    if (!cli) { fprintf(stderr, "out of memory\n"); return 1; }

    int rc = cmcp_client_connect_stdio(cli, server_argv[0], server_argv, NULL);
    if (rc != CMCP_OK) {
        fprintf(stderr, "connect/handshake failed (%d)\n", rc);
        cmcp_client_free(cli);
        return 1;
    }

    const char *sn = cmcp_client_server_name(cli);
    const char *sv = cmcp_client_server_version(cli);
    printf("Connected to %s %s (protocol %s)\n",
            sn ? sn : "(unnamed)", sv ? sv : "?", CMCP_PROTOCOL_VERSION);

    /* tools/list */
    cmcp_rpc_message_t list_resp;
    cmcp_rpc_message_init(&list_resp);
    rc = cmcp_client_request(cli, "tools/list", NULL, &list_resp);
    if (rc != CMCP_OK || list_resp.error) {
        fprintf(stderr, "tools/list failed\n");
        cmcp_rpc_message_clear(&list_resp);
        cmcp_client_free(cli);
        return 1;
    }
    const cmcp_json_t *tools = cmcp_json_object_get(list_resp.result, "tools");
    size_t n = (tools && tools->type == CMCP_JSON_ARRAY) ? tools->arr.len : 0;
    printf("Server offers %zu tool%s.\n", n, n == 1 ? "" : "s");

    /* If the server has `echo`, call it as a demo. */
    if (has_tool(list_resp.result, "echo")) {
        cmcp_json_t *params = cmcp_json_new_object();
        cmcp_json_object_set(params, "name", cmcp_json_new_string("echo"));
        cmcp_json_t *args = cmcp_json_new_object();
        cmcp_json_object_set(args, "text",
                              cmcp_json_new_string("hello from cMCP"));
        cmcp_json_object_set(params, "arguments", args);

        cmcp_rpc_message_t call_resp;
        cmcp_rpc_message_init(&call_resp);
        rc = cmcp_client_request(cli, "tools/call", params, &call_resp);
        if (rc == CMCP_OK && !call_resp.error) {
            const cmcp_json_t *content =
                cmcp_json_object_get(call_resp.result, "content");
            if (content && content->type == CMCP_JSON_ARRAY &&
                content->arr.len > 0) {
                const cmcp_json_t *item = content->arr.items[0];
                const cmcp_json_t *text =
                    item ? cmcp_json_object_get(item, "text") : NULL;
                if (text && text->type == CMCP_JSON_STRING)
                    printf("echo: %s\n", text->str.s);
            }
        }
        cmcp_rpc_message_clear(&call_resp);
    }
    cmcp_rpc_message_clear(&list_resp);

    cmcp_client_free(cli);    /* closes transport, reaps child */
    return 0;
}
