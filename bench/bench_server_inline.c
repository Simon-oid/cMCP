/* bench_server_inline — steady-state tools/call latency over stdio.
 *
 * Spawns an in-process echo server on a pipe pair, runs the handshake,
 * then issues N synchronous `tools/call` requests against an
 * inline-fast tool (no sleep, no I/O) and reports p50/p95/p99/p999
 * latency plus per-second throughput.
 *
 * In-process measurement deliberately: subprocess setup is paid once
 * at session start, not per call. Steady-state per-call latency is
 * what consumers care about.
 *
 * Defaults: 1000 warmup calls + 50000 measured calls. Override via
 * env CMCP_BENCH_WARMUP / CMCP_BENCH_N. */

#include "bench_util.h"

#include "cmcp.h"
#include "cmcp_client.h"
#include "cmcp_json.h"
#include "cmcp_server.h"
#include "cmcp_transport.h"
#include "cmcp_types.h"

#include <pthread.h>
#include <unistd.h>

/* ====================================================================== */
/* Inline echo tool                                                        */
/* ====================================================================== */

static int echo_tool(const cmcp_json_t *arguments, void *userdata,
                      cmcp_handler_ctx_t *hctx,
                      cmcp_json_t **out_content, int *out_is_error) {
    (void)userdata; (void)hctx;
    *out_is_error = 0;
    const cmcp_json_t *m = arguments
        ? cmcp_json_object_get(arguments, "message") : NULL;
    const char *msg = (m && m->type == CMCP_JSON_STRING) ? m->str.s : "";
    *out_content = cmcp_tool_text_content(msg);
    return *out_content ? CMCP_OK : CMCP_ENOMEM;
}

static const char echo_schema[] =
    "{\"type\":\"object\","
     "\"properties\":{\"message\":{\"type\":\"string\"}},"
     "\"required\":[\"message\"]}";

/* ====================================================================== */
/* Pipe pair + server thread                                               */
/* ====================================================================== */

typedef struct {
    cmcp_transport_t *client_t;
    cmcp_transport_t *server_t;
} pair_t;

static int make_pair(pair_t *p) {
    int c2s[2], s2c[2];
    if (pipe(c2s) != 0) return -1;
    if (pipe(s2c) != 0) { close(c2s[0]); close(c2s[1]); return -1; }
    p->client_t = cmcp_transport_stdio_new_fds(s2c[0], c2s[1]);
    p->server_t = cmcp_transport_stdio_new_fds(c2s[0], s2c[1]);
    if (!p->client_t || !p->server_t) {
        cmcp_transport_close(p->client_t);
        cmcp_transport_close(p->server_t);
        return -1;
    }
    return 0;
}

typedef struct {
    cmcp_server_t    *s;
    cmcp_transport_t *t;
} srv_arg_t;

static void *server_main(void *arg) {
    srv_arg_t *a = (srv_arg_t *)arg;
    cmcp_server_run(a->s, a->t);
    return NULL;
}

/* ====================================================================== */
/* One inline tools/call iteration                                         */
/* ====================================================================== */

static int do_one_call(cmcp_client_t *c, long *out_us) {
    cmcp_json_t *params = cmcp_json_new_object();
    cmcp_json_object_set(params, "name", cmcp_json_new_string("echo"));
    cmcp_json_t *args = cmcp_json_new_object();
    cmcp_json_object_set(args, "message", cmcp_json_new_string("ping"));
    cmcp_json_object_set(params, "arguments", args);

    long long t0 = bench_now_ns();
    cmcp_rpc_message_t resp;
    cmcp_rpc_message_init(&resp);
    int rc = cmcp_client_request(c, "tools/call", params, &resp);
    long long t1 = bench_now_ns();
    cmcp_rpc_message_clear(&resp);

    *out_us = (long)((t1 - t0) / 1000);
    return rc;
}

/* ====================================================================== */
/* Main                                                                    */
/* ====================================================================== */

static size_t env_size(const char *k, size_t fallback) {
    const char *v = getenv(k);
    if (!v || !*v) return fallback;
    char *end; long n = strtol(v, &end, 10);
    if (end == v || *end || n <= 0) return fallback;
    return (size_t)n;
}

int main(void) {
    size_t warmup = env_size("CMCP_BENCH_WARMUP", 1000);
    size_t n      = env_size("CMCP_BENCH_N",      50000);

    pair_t p;
    if (make_pair(&p) != 0) { fprintf(stderr, "pair failed\n"); return 1; }

    cmcp_server_t *srv = cmcp_server_new("bench-inline", "0.1.0");
    cmcp_tool_t tool = {
        .name = "echo", .description = "echo",
        .input_schema = echo_schema, .handler = echo_tool,
    };
    cmcp_server_add_tool(srv, &tool);

    srv_arg_t sa = { srv, p.server_t };
    pthread_t th;
    pthread_create(&th, NULL, server_main, &sa);

    cmcp_client_t *cli = cmcp_client_new("bench-inline-client", "0.0.1");
    if (cmcp_client_handshake(cli, p.client_t) != CMCP_OK) {
        fprintf(stderr, "handshake failed\n"); return 2;
    }

    /* Warmup. */
    for (size_t i = 0; i < warmup; i++) {
        long us;
        if (do_one_call(cli, &us) != CMCP_OK) {
            fprintf(stderr, "warmup call %zu failed\n", i); return 3;
        }
    }

    bench_lat_t lat = {0};
    if (bench_lat_init(&lat, n) != 0) { fprintf(stderr, "lat alloc\n"); return 4; }

    long long wall0 = bench_now_ns();
    for (size_t i = 0; i < n; i++) {
        long us;
        if (do_one_call(cli, &us) != CMCP_OK) {
            fprintf(stderr, "call %zu failed\n", i); return 5;
        }
        bench_lat_record(&lat, us);
    }
    double wall_s = (double)(bench_now_ns() - wall0) / 1e9;

    bench_summary_t s;
    bench_lat_summarize(&lat, &s);

    /* Always emit a header so each binary's stdout is self-describing.
     * run.sh deduplicates if needed. */
    fputs(BENCH_CSV_HEADER, stdout);
    bench_emit_row(stdout, "server_inline_stdio", n, wall_s, &s,
                    "transport=stdio tool=echo");

    bench_lat_free(&lat);
    cmcp_client_free(cli);
    cmcp_transport_close(p.client_t);
    pthread_join(th, NULL);
    cmcp_server_free(srv);
    cmcp_transport_close(p.server_t);
    return 0;
}
