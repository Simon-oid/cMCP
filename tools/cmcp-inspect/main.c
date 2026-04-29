/* cmcp-inspect — terminal client for poking at any MCP server.
 *
 * Spawns the named server as a child process, runs the MCP handshake
 * over the child's stdin/stdout, lists all tools, and optionally calls
 * one with caller-supplied JSON arguments.
 *
 * Usage:
 *
 *   cmcp-inspect [options] -- <server-cmd> [server-args...]
 *
 * Options:
 *   -c, --call NAME     After listing, call this tool.
 *   -a, --args JSON     JSON object passed as the tool's arguments
 *                       (default {}). Must parse as a JSON object.
 *   -j, --json          Emit raw JSON for tools/list and tools/call
 *                       results instead of human-formatted text.
 *   -h, --help          Show this help and exit.
 *
 * The `--` separator is required: everything after it is the server
 * command line, passed to execvp() unchanged.
 *
 * Examples:
 *
 *   cmcp-inspect -- ./examples/echo-server
 *   cmcp-inspect -c echo -a '{"text":"hi"}' -- ./examples/echo-server
 *   cmcp-inspect --json -- ./tools/crag-mcp/crag-mcp /path/to/db
 *
 * Exit codes:
 *   0   handshake + (optional) tool call all succeeded
 *   1   handshake failed, transport error, or server returned an error
 *   2   bad CLI usage (parse error, missing --, etc.)
 *  127  failed to exec the server child
 */

#define _POSIX_C_SOURCE 200809L

#include "cmcp.h"
#include "cmcp_client.h"
#include "cmcp_json.h"
#include "cmcp_transport.h"
#include "cmcp_types.h"

#include <errno.h>
#include <getopt.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

/* ====================================================================== */
/* Output helpers                                                          */
/* ====================================================================== */

static void print_help(void) {
    fputs(
"Usage: cmcp-inspect [options] -- <server-cmd> [server-args...]\n"
"\n"
"  Spawn an MCP server as a child process, run the handshake over its\n"
"  stdin/stdout, list its tools, and optionally call one.\n"
"\n"
"Options:\n"
"  -c, --call NAME    After listing, call this tool.\n"
"  -a, --args JSON    JSON object for --call (default '{}').\n"
"  -j, --json         Emit raw JSON for results.\n"
"  -h, --help         Show this help and exit.\n"
"\n"
"Examples:\n"
"  cmcp-inspect -- ./examples/echo-server\n"
"  cmcp-inspect -c echo -a '{\"text\":\"hi\"}' -- ./examples/echo-server\n",
          stderr);
}

/* Print one tool descriptor in a compact human-readable form. */
static void print_tool(const cmcp_json_t *t) {
    const cmcp_json_t *name = cmcp_json_object_get(t, "name");
    const cmcp_json_t *desc = cmcp_json_object_get(t, "description");
    const cmcp_json_t *sch  = cmcp_json_object_get(t, "inputSchema");
    if (!name || name->type != CMCP_JSON_STRING) return;

    printf("  %s", name->str.s);
    if (desc && desc->type == CMCP_JSON_STRING)
        printf("  -  %s", desc->str.s);
    putchar('\n');

    if (!sch || sch->type != CMCP_JSON_OBJECT) return;
    const cmcp_json_t *props = cmcp_json_object_get(sch, "properties");
    const cmcp_json_t *req   = cmcp_json_object_get(sch, "required");
    if (!props || props->type != CMCP_JSON_OBJECT) return;

    for (size_t i = 0; i < props->obj.len; i++) {
        const char *pname = props->obj.keys[i];
        const cmcp_json_t *p = props->obj.values[i];
        const cmcp_json_t *type = (p && p->type == CMCP_JSON_OBJECT)
            ? cmcp_json_object_get(p, "type") : NULL;
        const char *tstr = (type && type->type == CMCP_JSON_STRING)
            ? type->str.s : "any";

        int is_required = 0;
        if (req && req->type == CMCP_JSON_ARRAY) {
            for (size_t k = 0; k < req->arr.len; k++) {
                const cmcp_json_t *r = req->arr.items[k];
                if (r && r->type == CMCP_JSON_STRING &&
                    strcmp(r->str.s, pname) == 0) {
                    is_required = 1; break;
                }
            }
        }
        printf("      - %s: %s%s\n", pname, tstr,
                is_required ? " (required)" : "");
    }
}

/* Print a tool-call result. Pulls out the first text-content item if
 * there is one; otherwise dumps the JSON content array. */
static void print_call_result(const cmcp_json_t *result) {
    const cmcp_json_t *is_err = cmcp_json_object_get(result, "isError");
    int err = (is_err && is_err->type == CMCP_JSON_BOOL && is_err->b);

    const cmcp_json_t *content = cmcp_json_object_get(result, "content");
    if (!content || content->type != CMCP_JSON_ARRAY) {
        printf("(no content)\n");
        return;
    }

    if (err) printf("[tool-level error]\n");

    for (size_t i = 0; i < content->arr.len; i++) {
        const cmcp_json_t *item = content->arr.items[i];
        if (!item || item->type != CMCP_JSON_OBJECT) continue;
        const cmcp_json_t *type = cmcp_json_object_get(item, "type");
        const cmcp_json_t *text = cmcp_json_object_get(item, "text");
        if (type && type->type == CMCP_JSON_STRING &&
            strcmp(type->str.s, "text") == 0 &&
            text && text->type == CMCP_JSON_STRING) {
            fwrite(text->str.s, 1, text->str.len, stdout);
            putchar('\n');
        } else {
            char *raw = cmcp_json_emit(item);
            if (raw) { printf("%s\n", raw); free(raw); }
        }
    }
}

static void print_rpc_error(const cmcp_rpc_error_t *e, const char *what) {
    fprintf(stderr, "%s failed: %d %s",
            what, e->code, e->message ? e->message : "");
    if (e->data) {
        char *raw = cmcp_json_emit(e->data);
        if (raw) { fprintf(stderr, " %s", raw); free(raw); }
    }
    fputc('\n', stderr);
}

/* ====================================================================== */
/* Spawn helper                                                            */
/* ====================================================================== */

/* Fork and exec the server child. On success, returns the child pid
 * and populates *read_fd / *write_fd with the parent's end of the
 * pipes. On failure, returns -1 and leaves the FDs unset. */
static pid_t spawn_server(char **server_argv, int *read_fd, int *write_fd) {
    int p2c[2], c2p[2];
    if (pipe(p2c) != 0) return -1;
    if (pipe(c2p) != 0) {
        close(p2c[0]); close(p2c[1]);
        return -1;
    }

    pid_t pid = fork();
    if (pid < 0) {
        close(p2c[0]); close(p2c[1]);
        close(c2p[0]); close(c2p[1]);
        return -1;
    }

    if (pid == 0) {
        /* child: stdin <- parent, stdout -> parent. */
        if (dup2(p2c[0], STDIN_FILENO)  < 0 ||
            dup2(c2p[1], STDOUT_FILENO) < 0) {
            perror("cmcp-inspect: dup2");
            _exit(127);
        }
        close(p2c[0]); close(p2c[1]);
        close(c2p[0]); close(c2p[1]);
        execvp(server_argv[0], server_argv);
        fprintf(stderr, "cmcp-inspect: exec %s: %s\n",
                server_argv[0], strerror(errno));
        _exit(127);
    }

    /* parent: keep our ends, close the child's. */
    close(p2c[0]);
    close(c2p[1]);
    *write_fd = p2c[1];
    *read_fd  = c2p[0];
    return pid;
}

/* ====================================================================== */
/* Command actions                                                         */
/* ====================================================================== */

/* tools/list: returns 0 on success, 1 on RPC error. */
static int do_list(cmcp_client_t *cli, int json_mode) {
    cmcp_rpc_message_t resp;
    int rc = cmcp_client_request(cli, "tools/list", NULL, &resp);
    if (rc != CMCP_OK) {
        fprintf(stderr, "tools/list: transport error (%d)\n", rc);
        return 1;
    }
    if (resp.error) {
        print_rpc_error(resp.error, "tools/list");
        cmcp_rpc_message_clear(&resp);
        return 1;
    }

    if (json_mode) {
        char *raw = cmcp_json_emit(resp.result);
        if (raw) { printf("%s\n", raw); free(raw); }
    } else {
        const cmcp_json_t *arr = cmcp_json_object_get(resp.result, "tools");
        size_t n = (arr && arr->type == CMCP_JSON_ARRAY) ? arr->arr.len : 0;
        printf("Tools (%zu):\n", n);
        for (size_t i = 0; i < n; i++) {
            print_tool(arr->arr.items[i]);
        }
    }
    cmcp_rpc_message_clear(&resp);
    return 0;
}

/* tools/call: returns 0 on RPC success (including tool-level isError),
 * 1 on transport or JSON-RPC error. */
static int do_call(cmcp_client_t *cli, const char *name,
                    const char *args_json, int json_mode) {
    cmcp_json_t *args = NULL;
    if (args_json) {
        args = cmcp_json_parse(args_json, strlen(args_json));
        if (!args || args->type != CMCP_JSON_OBJECT) {
            fprintf(stderr, "--args must be a JSON object: %s\n", args_json);
            cmcp_json_free(args);
            return 2;
        }
    } else {
        args = cmcp_json_new_object();
    }

    cmcp_json_t *params = cmcp_json_new_object();
    cmcp_json_object_set(params, "name", cmcp_json_new_string(name));
    cmcp_json_object_set(params, "arguments", args);

    cmcp_rpc_message_t resp;
    int rc = cmcp_client_request(cli, "tools/call", params, &resp);
    if (rc != CMCP_OK) {
        fprintf(stderr, "tools/call: transport error (%d)\n", rc);
        return 1;
    }
    if (resp.error) {
        print_rpc_error(resp.error, "tools/call");
        cmcp_rpc_message_clear(&resp);
        return 1;
    }

    if (json_mode) {
        char *raw = cmcp_json_emit(resp.result);
        if (raw) { printf("%s\n", raw); free(raw); }
    } else {
        print_call_result(resp.result);
    }
    cmcp_rpc_message_clear(&resp);
    return 0;
}

/* ====================================================================== */
/* main                                                                    */
/* ====================================================================== */

int main(int argc, char **argv) {
    const char *call_name = NULL;
    const char *call_args = NULL;
    int         json_mode = 0;

    /* Hand-rolled option parsing so we can stop cleanly at `--`.
     * getopt_long would also work but its handling of `--` is fine
     * either way. */
    static const struct option longopts[] = {
        { "call", required_argument, NULL, 'c' },
        { "args", required_argument, NULL, 'a' },
        { "json", no_argument,       NULL, 'j' },
        { "help", no_argument,       NULL, 'h' },
        { NULL,   0,                 NULL,  0  },
    };

    int opt;
    while ((opt = getopt_long(argc, argv, "c:a:jh", longopts, NULL)) != -1) {
        switch (opt) {
        case 'c': call_name = optarg; break;
        case 'a': call_args = optarg; break;
        case 'j': json_mode = 1;      break;
        case 'h': print_help(); return 0;
        default:  print_help(); return 2;
        }
    }

    if (call_args && !call_name) {
        fprintf(stderr, "cmcp-inspect: --args requires --call\n");
        return 2;
    }
    if (optind >= argc) {
        fprintf(stderr, "cmcp-inspect: missing server command\n\n");
        print_help();
        return 2;
    }

    char **server_argv = &argv[optind];

    int rfd = -1, wfd = -1;
    pid_t pid = spawn_server(server_argv, &rfd, &wfd);
    if (pid < 0) {
        fprintf(stderr, "cmcp-inspect: spawn failed: %s\n", strerror(errno));
        return 1;
    }

    cmcp_transport_t *t = cmcp_transport_stdio_new_fds(rfd, wfd);
    if (!t) {
        fprintf(stderr, "cmcp-inspect: transport allocation failed\n");
        close(rfd); close(wfd);
        kill(pid, SIGTERM);
        waitpid(pid, NULL, 0);
        return 1;
    }

    cmcp_client_t *cli = cmcp_client_new("cmcp-inspect", CMCP_VERSION);
    if (!cli) {
        fprintf(stderr, "cmcp-inspect: client allocation failed\n");
        cmcp_transport_close(t);
        kill(pid, SIGTERM);
        waitpid(pid, NULL, 0);
        return 1;
    }

    int exit_code = 0;
    int rc = cmcp_client_handshake(cli, t);
    if (rc != CMCP_OK) {
        fprintf(stderr, "cmcp-inspect: handshake failed (%d)\n", rc);
        exit_code = 1;
        goto cleanup;
    }

    if (!json_mode) {
        const char *sn = cmcp_client_server_name(cli);
        const char *sv = cmcp_client_server_version(cli);
        printf("Connected: %s %s (protocol %s)\n\n",
                sn ? sn : "(unnamed)", sv ? sv : "?",
                CMCP_PROTOCOL_VERSION);
    }

    if (do_list(cli, json_mode) != 0) {
        exit_code = 1;
        goto cleanup;
    }

    if (call_name) {
        if (!json_mode) printf("\nCalling %s:\n", call_name);
        int crc = do_call(cli, call_name, call_args, json_mode);
        if (crc != 0) exit_code = crc;
    }

cleanup:
    cmcp_client_free(cli);
    cmcp_transport_close(t);

    /* If the child is still alive (stdin closed should EOF it), wait
     * briefly; SIGTERM as a fallback so we never leak a daemon. */
    int status = 0;
    if (waitpid(pid, &status, WNOHANG) == 0) {
        kill(pid, SIGTERM);
        waitpid(pid, &status, 0);
    }
    return exit_code;
}
