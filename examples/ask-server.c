/* ask-server — one tool that asks the client for input, in both eras.
 *
 * `greet` needs the user's name. The same handler body serves:
 *
 *   - a legacy (2025-11-25) client: cmcp_handler_input sends a real
 *     `elicitation/create` request and blocks until the answer arrives;
 *   - a modern (2026-07-28) client: the first call ends with an
 *     InputRequiredResult listing the elicitation; the client asks the
 *     user and retries with `inputResponses`, and this time
 *     cmcp_handler_input hands the answer straight back.
 *
 * Usage: `ask-server` serves stdio; `ask-server --http PORT` serves
 * Streamable HTTP on 127.0.0.1:PORT/mcp.
 *
 * Build: `make examples/ask-server`. Used by `make conformance-py`. */
#include "cmcp.h"
#include "cmcp_json.h"
#include "cmcp_server.h"
#include "cmcp_transport.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int greet(const cmcp_json_t *arguments, void *userdata,
                 cmcp_handler_ctx_t *hctx, cmcp_json_t **out_content,
                 int *out_is_error) {
    (void)arguments; (void)userdata;
    cmcp_json_t *form = cmcp_json_parse_cstr(
        "{\"message\":\"What is your name?\","
        "\"requestedSchema\":{\"type\":\"object\","
        "\"properties\":{\"name\":{\"type\":\"string\"}},"
        "\"required\":[\"name\"]}}");

    cmcp_json_t *answer = NULL;
    int rc = cmcp_handler_input(hctx, "name", "elicitation/create", form,
                                &answer);
    if (rc == CMCP_EINPUT_REQUIRED) return rc;   /* modern: round one */
    if (rc != CMCP_OK) return rc;   /* e.g. no elicitation capability */

    const cmcp_json_t *action = cmcp_json_object_get(answer, "action");
    const cmcp_json_t *name = cmcp_json_object_get(
        cmcp_json_object_get(answer, "content"), "name");
    char text[256];
    if (action && action->type == CMCP_JSON_STRING &&
        name && name->type == CMCP_JSON_STRING) {
        snprintf(text, sizeof text, "Hello, %s!", cmcp_json_string(name));
    } else {
        snprintf(text, sizeof text, "Hello, stranger.");
    }
    cmcp_json_free(answer);
    *out_content = cmcp_tool_text_content(text);
    *out_is_error = 0;
    return CMCP_OK;
}

/* Serve on 127.0.0.1:port until the process is killed. */
static int run_http(cmcp_server_t *s, const char *port_arg) {
    long port = strtol(port_arg, NULL, 10);
    if (port <= 0 || port > 65535) return CMCP_EINVAL;
    cmcp_transport_t *t = cmcp_transport_http_listen("127.0.0.1",
                                                     (unsigned short)port);
    if (!t) return CMCP_EIO;
    int rc = cmcp_server_run(s, t);
    cmcp_transport_close(t);
    return rc;
}

int main(int argc, char **argv) {
    cmcp_server_t *s = cmcp_server_new("ask-server", CMCP_VERSION);
    if (!s) return 1;
    cmcp_tool_t tool = {
        .name = "greet",
        .description = "Greet the user by name, asking for it first",
        .input_schema = "{\"type\":\"object\"}",
        .handler = greet,
    };
    int rc = cmcp_server_add_tool(s, &tool);
    if (rc == CMCP_OK) {
        rc = (argc == 3 && strcmp(argv[1], "--http") == 0)
                 ? run_http(s, argv[2])
                 : cmcp_server_run_stdio(s);
    }
    cmcp_server_free(s);
    return rc == CMCP_OK ? 0 : 1;
}
