/* bench_compare — drive the cMCP client against an arbitrary MCP server
 * binary and emit one CSV row of steady-state tools/call latency.
 *
 * Holds the *client* constant (always our cmcp_client_t) so the per-call
 * cost measured varies only with the server implementation. Use:
 *
 *   bench_compare <impl-label> <server-cmd> [server-arg ...]
 *
 *     bench_compare cmcp ./examples/echo-server
 *     bench_compare ts   node bench/compare/servers/echo.mjs
 *     bench_compare py   python3 bench/compare/servers/echo.py
 *
 * The driver:
 *   1) forks+execs the server via cmcp_client_connect_stdio (handshake
 *      included),
 *   2) issues CMCP_BENCH_WARMUP warmup `tools/call echo {text:"ping"}`,
 *   3) issues CMCP_BENCH_N measured calls, recording per-call latency,
 *   4) emits one CSV row to stdout with `extra=impl=<label> server=<cmd>`,
 *   5) frees the client (which closes the transport, which SIGKILLs the
 *      child if it doesn't exit on EOF).
 *
 * Defaults: 1000 warmup, 10000 measured. The TS/Py SDKs are slower per
 * call than cMCP — 10k is enough to reach a stable p99 without making
 * `make bench-compare` glacial. Override via env. */

#include "../bench_util.h"

#include "cmcp.h"
#include "cmcp_client.h"
#include "cmcp_json.h"
#include "cmcp_types.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static size_t env_size(const char *k, size_t fallback) {
    const char *v = getenv(k);
    if (!v || !*v) return fallback;
    char *end; long n = strtol(v, &end, 10);
    if (end == v || *end || n <= 0) return fallback;
    return (size_t)n;
}

static int do_one_call(cmcp_client_t *c, long *out_us) {
    cmcp_json_t *params = cmcp_json_new_object();
    cmcp_json_object_set(params, "name", cmcp_json_new_string("echo"));
    cmcp_json_t *args = cmcp_json_new_object();
    cmcp_json_object_set(args, "text", cmcp_json_new_string("ping"));
    cmcp_json_object_set(params, "arguments", args);

    long long t0 = bench_now_ns();
    cmcp_rpc_message_t resp;
    cmcp_rpc_message_init(&resp);
    int rc = cmcp_client_request(c, "tools/call", params, &resp);
    long long t1 = bench_now_ns();
    /* The result content isn't asserted — we only want roundtrip cost.
     * A non-OK rc abandons the bench with a diagnostic on stderr. */
    cmcp_rpc_message_clear(&resp);
    *out_us = (long)((t1 - t0) / 1000);
    return rc;
}

int main(int argc, char **argv) {
    if (argc < 3) {
        fprintf(stderr,
                "usage: %s <impl-label> <server-cmd> [server-arg ...]\n",
                argv[0]);
        return 2;
    }
    const char *label = argv[1];
    const char *server_path = argv[2];

    size_t warmup = env_size("CMCP_BENCH_WARMUP", 1000);
    size_t n      = env_size("CMCP_BENCH_N",      10000);

    /* Build the argv for the child: argv[0] = path, then argv[3..] from
     * our argv, then NULL. */
    int child_argc = argc - 2;
    char **child_argv = (char **)calloc((size_t)child_argc + 1,
                                          sizeof(char *));
    if (!child_argv) { fprintf(stderr, "alloc failed\n"); return 1; }
    for (int i = 0; i < child_argc; i++) child_argv[i] = argv[2 + i];
    child_argv[child_argc] = NULL;

    cmcp_client_t *cli = cmcp_client_new("bench-compare", "0.0.1");
    if (!cli) {
        fprintf(stderr, "cmcp_client_new failed\n");
        free(child_argv); return 1;
    }

    int rc = cmcp_client_connect_stdio(cli, server_path, child_argv, NULL);
    if (rc != CMCP_OK) {
        fprintf(stderr, "connect_stdio(%s) failed: %d\n", server_path, rc);
        cmcp_client_free(cli);
        free(child_argv);
        return 3;
    }

    bench_lat_t lat = {0};
    if (bench_lat_init(&lat, n) != 0) {
        fprintf(stderr, "lat alloc failed\n");
        cmcp_client_free(cli);
        free(child_argv);
        return 4;
    }

    for (size_t i = 0; i < warmup; i++) {
        long us;
        if (do_one_call(cli, &us) != CMCP_OK) {
            fprintf(stderr, "warmup %zu failed (impl=%s)\n", i, label);
            bench_lat_free(&lat);
            cmcp_client_free(cli);
            free(child_argv);
            return 5;
        }
    }

    long long wall0 = bench_now_ns();
    for (size_t i = 0; i < n; i++) {
        long us;
        if (do_one_call(cli, &us) != CMCP_OK) {
            fprintf(stderr, "call %zu failed (impl=%s)\n", i, label);
            bench_lat_free(&lat);
            cmcp_client_free(cli);
            free(child_argv);
            return 6;
        }
        bench_lat_record(&lat, us);
    }
    double wall_s = (double)(bench_now_ns() - wall0) / 1e9;

    bench_summary_t s;
    bench_lat_summarize(&lat, &s);

    char extra[256];
    /* Truncation is fine: the label + server path are diagnostic, not
     * load-bearing. snprintf clamps to the buffer. */
    snprintf(extra, sizeof extra, "impl=%s server=%s", label, server_path);

    fputs(BENCH_CSV_HEADER, stdout);
    bench_emit_row(stdout, "compare_stdio_echo", n, wall_s, &s, extra);

    bench_lat_free(&lat);
    cmcp_client_free(cli);
    free(child_argv);
    return 0;
}
