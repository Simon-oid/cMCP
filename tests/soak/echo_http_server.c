/* HTTP echo server for the soak harness.
 *
 * Mirrors examples/echo-server's tool surface — `echo` returns its
 * `text` argument unchanged, `add` returns `a + b` — but binds an
 * HTTP listener on 127.0.0.1:<argv> instead of running on stdio.
 *
 * Kept under tests/soak/ rather than examples/ so the example stays
 * stdio-only (it's the "hello world" reference). This binary is
 * spawned as a child by tests/soak/soak_http_driver.c.
 *
 * Usage:  echo_http_server --port=N    (default 18080) */

#define _POSIX_C_SOURCE 200809L

#include "cmcp.h"
#include "cmcp_json.h"
#include "cmcp_server.h"
#include "cmcp_transport.h"

#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int echo_handler(const cmcp_json_t *args, void *userdata,
                         cmcp_handler_ctx_t *hctx,
                         cmcp_json_t **out_content, int *out_is_error) {
    (void)userdata; (void)hctx;
    *out_is_error = 0;
    const cmcp_json_t *t = args ? cmcp_json_object_get(args, "text") : NULL;
    const char *s = (t && t->type == CMCP_JSON_STRING) ? t->str.s : "";
    *out_content = cmcp_tool_text_content(s);
    return *out_content ? CMCP_OK : CMCP_ENOMEM;
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
    return *out_content ? CMCP_OK : CMCP_ENOMEM;
}

int main(int argc, char **argv) {
    unsigned short port = 18080;
    for (int i = 1; i < argc; i++) {
        if (!strncmp(argv[i], "--port=", 7)) {
            int p = atoi(argv[i] + 7);
            if (p > 0 && p <= 65535) port = (unsigned short)p;
        }
    }

    /* Soak driver kills us with SIGTERM; SIGPIPE on a half-closed
     * client write should not take us down. */
    signal(SIGPIPE, SIG_IGN);

    cmcp_transport_t *t = cmcp_transport_http_listen("127.0.0.1", port);
    if (!t) {
        fprintf(stderr, "echo_http_server: listen on 127.0.0.1:%u failed\n",
                port);
        return 2;
    }

    cmcp_server_t *s = cmcp_server_new("soak-http-echo", "0.1.0");
    if (!s) { cmcp_transport_close(t); return 1; }

    int rc = cmcp_server_add_tool(s, &(cmcp_tool_t){
        .name        = "echo",
        .description = "Return the `text` argument unchanged.",
        .input_schema = "{\"type\":\"object\","
                         "\"properties\":{\"text\":{\"type\":\"string\"}},"
                         "\"required\":[\"text\"]}",
        .handler     = echo_handler,
    });
    if (rc != CMCP_OK) {
        cmcp_server_free(s); cmcp_transport_close(t); return 1;
    }

    rc = cmcp_server_add_tool(s, &(cmcp_tool_t){
        .name        = "add",
        .description = "Sum two integers as a decimal string.",
        .input_schema = "{\"type\":\"object\","
                         "\"properties\":{"
                            "\"a\":{\"type\":\"integer\"},"
                            "\"b\":{\"type\":\"integer\"}"
                         "},"
                         "\"required\":[\"a\",\"b\"]}",
        .handler     = add_handler,
    });
    if (rc != CMCP_OK) {
        cmcp_server_free(s); cmcp_transport_close(t); return 1;
    }

    /* Print the bound port to stderr so the soak driver can sanity-
     * check what it's connecting to. Not a coordination channel —
     * the driver passes the port in via argv. */
    fprintf(stderr, "echo_http_server: listening on 127.0.0.1:%u\n", port);

    int run_rc = cmcp_server_run(s, t);
    cmcp_server_free(s);
    cmcp_transport_close(t);
    return run_rc == CMCP_OK ? 0 : 1;
}
