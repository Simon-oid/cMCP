/* bench_http — steady-state tools/call latency over the HTTP transport.
 *
 * Mirrors bench_server_inline, but the transport is Streamable HTTP
 * (loopback). The HTTP path goes through libcurl on the client and
 * the hand-rolled acceptor + parser on the server, so per-call cost
 * is dominated by request/response framing, not by JSON or dispatch.
 *
 * In-process: server thread + client thread, no subprocess. The
 * acceptor binds 127.0.0.1 on an ephemeral port.
 *
 * Defaults: 1000 warmup + 5000 measured (HTTP is slower than stdio,
 * we don't need 50000 to reach steady state). Override via env. */

#include "bench_util.h"

#include "cmcp.h"
#include "cmcp_client.h"
#include "cmcp_json.h"
#include "cmcp_server.h"
#include "cmcp_transport.h"
#include "cmcp_types.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <pthread.h>
#include <sys/socket.h>
#include <unistd.h>

/* ====================================================================== */
/* Inline echo tool                                                        */
/* ====================================================================== */

static int echo_tool(const cmcp_json_t *args, void *userdata,
                      cmcp_handler_ctx_t *hctx,
                      cmcp_json_t **out_content, int *out_is_error) {
    (void)userdata; (void)hctx;
    *out_is_error = 0;
    const cmcp_json_t *m = args ? cmcp_json_object_get(args, "message") : NULL;
    const char *msg = (m && m->type == CMCP_JSON_STRING) ? m->str.s : "";
    *out_content = cmcp_tool_text_content(msg);
    return *out_content ? CMCP_OK : CMCP_ENOMEM;
}

static const char echo_schema[] =
    "{\"type\":\"object\","
     "\"properties\":{\"message\":{\"type\":\"string\"}},"
     "\"required\":[\"message\"]}";

/* ====================================================================== */
/* Ephemeral port picker                                                   */
/* ====================================================================== */

static unsigned short pick_port(void) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return 0;
    int yes = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof yes);
    struct sockaddr_in sa;
    memset(&sa, 0, sizeof sa);
    sa.sin_family      = AF_INET;
    sa.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    sa.sin_port        = 0;
    if (bind(fd, (struct sockaddr *)&sa, sizeof sa) != 0) {
        close(fd); return 0;
    }
    socklen_t slen = sizeof sa;
    if (getsockname(fd, (struct sockaddr *)&sa, &slen) != 0) {
        close(fd); return 0;
    }
    unsigned short p = ntohs(sa.sin_port);
    close(fd);
    return p;
}

/* ====================================================================== */
/* Server thread                                                           */
/* ====================================================================== */

typedef struct { cmcp_server_t *s; cmcp_transport_t *t; } srv_arg_t;
static void *server_main(void *arg) {
    srv_arg_t *a = (srv_arg_t *)arg;
    cmcp_server_run(a->s, a->t);
    return NULL;
}

/* ====================================================================== */
/* One synchronous call                                                    */
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
    size_t n      = env_size("CMCP_BENCH_N",      5000);

    /* The 6.5.2 accept-rate gate (default 100 conn/sec, 200 burst) is
     * a peer-flood defense — meaningful for an exposed server, not
     * for a single-process bench that opens a fresh TCP connection
     * per call. Without this override, the bench saturates the burst
     * budget in <2 seconds and then either rate-limits us to 100/sec
     * (50s for 5k calls) or hangs on a 503 reply that the client
     * doesn't surface promptly. Override unless the user explicitly
     * set CMCP_HTTP_ACCEPT_RATE in the environment. */
    if (!getenv("CMCP_HTTP_ACCEPT_RATE")) {
        setenv("CMCP_HTTP_ACCEPT_RATE", "0", 1);
    }

    unsigned short port = pick_port();
    if (!port) { fprintf(stderr, "pick_port failed\n"); return 1; }

    cmcp_transport_t *st = cmcp_transport_http_listen("127.0.0.1", port);
    if (!st) { fprintf(stderr, "listen failed\n"); return 2; }

    cmcp_server_t *srv = cmcp_server_new("bench-http", "0.1.0");
    cmcp_tool_t tool = {
        .name = "echo", .description = "echo",
        .input_schema = echo_schema, .handler = echo_tool,
    };
    cmcp_server_add_tool(srv, &tool);

    srv_arg_t sa = { srv, st };
    pthread_t th;
    pthread_create(&th, NULL, server_main, &sa);

    /* Brief delay to let the acceptor reach poll() before the client
     * tries to connect. */
    struct timespec brief = { 0, 20 * 1000 * 1000 }; nanosleep(&brief, NULL);

    char url[64];
    snprintf(url, sizeof url, "http://127.0.0.1:%u/mcp", port);
    cmcp_transport_t *ct = cmcp_transport_http_connect(url);
    if (!ct) { fprintf(stderr, "connect failed\n"); return 3; }

    cmcp_client_t *cli = cmcp_client_new("bench-http-client", "0.0.1");
    if (cmcp_client_handshake(cli, ct) != CMCP_OK) {
        fprintf(stderr, "handshake failed\n"); return 4;
    }

    bench_lat_t lat = {0};
    if (bench_lat_init(&lat, n) != 0) {
        fprintf(stderr, "lat alloc failed\n"); return 5;
    }

    for (size_t i = 0; i < warmup; i++) {
        long us;
        if (do_one_call(cli, &us) != CMCP_OK) {
            fprintf(stderr, "warmup %zu failed\n", i); return 6;
        }
    }

    long long wall0 = bench_now_ns();
    for (size_t i = 0; i < n; i++) {
        long us;
        if (do_one_call(cli, &us) != CMCP_OK) {
            fprintf(stderr, "call %zu failed\n", i); return 7;
        }
        bench_lat_record(&lat, us);
    }
    double wall_s = (double)(bench_now_ns() - wall0) / 1e9;

    bench_summary_t s;
    bench_lat_summarize(&lat, &s);

    fputs(BENCH_CSV_HEADER, stdout);
    bench_emit_row(stdout, "server_inline_http", n, wall_s, &s,
                    "transport=http tool=echo");

    bench_lat_free(&lat);
    cmcp_client_free(cli);
    cmcp_transport_close(ct);
    /* Wake the server so the acceptor poll() returns and run() exits. */
    cmcp_transport_wake(st);
    pthread_join(th, NULL);
    cmcp_transport_close(st);
    cmcp_server_free(srv);
    return 0;
}
