/* bench_server_pool — surface worker-pool concurrency.
 *
 * Register a tool whose handler sleeps for a fixed interval, fire N
 * concurrent async tools/call requests, wait for all, and measure
 * wall-clock. With W workers in the pool and N >> W, total wall time
 * should be ~ ceil(N / W) * sleep_ms; serial execution would be
 * N * sleep_ms. The ratio is the concurrency surfaced by the pool.
 *
 * Defaults: 64 calls, 50ms sleep per call. Override via env
 * CMCP_BENCH_N / CMCP_BENCH_SLEEP_MS / CMCP_WORKERS.
 *
 * Reports throughput = N / wall_s alongside the configured worker
 * count; the consumer can derive concurrency = throughput * sleep_s. */

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
/* Sleep tool                                                              */
/* ====================================================================== */

typedef struct { int sleep_us; } sleep_cfg_t;

static int sleep_tool(const cmcp_json_t *arguments, void *userdata,
                       cmcp_handler_ctx_t *hctx,
                       cmcp_json_t **out_content, int *out_is_error) {
    (void)arguments; (void)hctx;
    sleep_cfg_t *cfg = (sleep_cfg_t *)userdata;
    struct timespec ts = {
        .tv_sec  = cfg->sleep_us / 1000000,
        .tv_nsec = (long)(cfg->sleep_us % 1000000) * 1000L,
    };
    nanosleep(&ts, NULL);
    *out_is_error = 0;
    *out_content = cmcp_tool_text_content("ok");
    return *out_content ? CMCP_OK : CMCP_ENOMEM;
}

/* ====================================================================== */
/* Pipe pair + server thread (mirrors bench_server_inline)                 */
/* ====================================================================== */

typedef struct { cmcp_transport_t *c, *s; } pair_t;

static int make_pair(pair_t *p) {
    int c2s[2], s2c[2];
    if (pipe(c2s) != 0) return -1;
    if (pipe(s2c) != 0) { close(c2s[0]); close(c2s[1]); return -1; }
    p->c = cmcp_transport_stdio_new_fds(s2c[0], c2s[1]);
    p->s = cmcp_transport_stdio_new_fds(c2s[0], s2c[1]);
    if (!p->c || !p->s) {
        cmcp_transport_close(p->c); cmcp_transport_close(p->s); return -1;
    }
    return 0;
}

typedef struct { cmcp_server_t *s; cmcp_transport_t *t; } srv_arg_t;
static void *server_main(void *arg) {
    srv_arg_t *a = (srv_arg_t *)arg;
    cmcp_server_run(a->s, a->t);
    return NULL;
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
    size_t n        = env_size("CMCP_BENCH_N",        64);
    size_t sleep_ms = env_size("CMCP_BENCH_SLEEP_MS", 50);
    size_t workers  = env_size("CMCP_WORKERS",        4);

    pair_t p;
    if (make_pair(&p) != 0) { fprintf(stderr, "pair failed\n"); return 1; }

    cmcp_server_t *srv = cmcp_server_new("bench-pool", "0.1.0");
    sleep_cfg_t cfg = { (int)(sleep_ms * 1000) };
    cmcp_tool_t tool = {
        .name = "sleep", .description = "fixed sleep",
        .handler = sleep_tool, .userdata = &cfg,
    };
    cmcp_server_add_tool(srv, &tool);

    srv_arg_t sa = { srv, p.s };
    pthread_t th;
    pthread_create(&th, NULL, server_main, &sa);

    cmcp_client_t *cli = cmcp_client_new("bench-pool-client", "0.0.1");
    if (cmcp_client_handshake(cli, p.c) != CMCP_OK) {
        fprintf(stderr, "handshake failed\n"); return 2;
    }

    /* Fire N async calls, then wait for all. */
    long long *ids = (long long *)calloc(n, sizeof(long long));
    bench_lat_t lat = {0};
    if (bench_lat_init(&lat, n) != 0) { fprintf(stderr, "lat alloc\n"); return 3; }

    long long wall0 = bench_now_ns();
    long long *t0 = (long long *)calloc(n, sizeof(long long));
    for (size_t i = 0; i < n; i++) {
        cmcp_json_t *params = cmcp_json_new_object();
        cmcp_json_object_set(params, "name", cmcp_json_new_string("sleep"));
        cmcp_json_object_set(params, "arguments", cmcp_json_new_object());
        t0[i] = bench_now_ns();
        int rc = cmcp_client_call_async(cli, "tools/call", params, &ids[i]);
        if (rc != CMCP_OK) {
            fprintf(stderr, "async %zu rc=%d\n", i, rc); return 4;
        }
    }
    for (size_t i = 0; i < n; i++) {
        cmcp_rpc_message_t resp;
        cmcp_rpc_message_init(&resp);
        int rc = cmcp_client_wait(cli, ids[i], &resp);
        long long t1 = bench_now_ns();
        cmcp_rpc_message_clear(&resp);
        if (rc != CMCP_OK) {
            fprintf(stderr, "wait %zu rc=%d\n", i, rc); return 5;
        }
        bench_lat_record(&lat, (long)((t1 - t0[i]) / 1000));
    }
    double wall_s = (double)(bench_now_ns() - wall0) / 1e9;

    bench_summary_t sumry;
    bench_lat_summarize(&lat, &sumry);

    char extra[96];
    snprintf(extra, sizeof extra,
             "transport=stdio tool=sleep workers=%zu sleep_ms=%zu",
             workers, sleep_ms);

    fputs(BENCH_CSV_HEADER, stdout);
    bench_emit_row(stdout, "server_pool_stdio", n, wall_s, &sumry, extra);

    free(ids); free(t0); bench_lat_free(&lat);
    cmcp_client_free(cli);
    cmcp_transport_close(p.c);
    pthread_join(th, NULL);
    cmcp_server_free(srv);
    cmcp_transport_close(p.s);
    return 0;
}
