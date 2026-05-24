/* Long-running stability driver (phase 5.6).
 *
 * Spawns a child MCP server, runs the handshake, then hammers it with
 * a steady tools/call workload while sampling /proc metrics — for both
 * the parent (client + reader thread + pending table) and the child
 * (server, worker pool, registries) — every --sample-interval seconds.
 * Optional --churn re-spawns the server periodically to also stress
 * connect/disconnect tearup paths.
 *
 * Output is a single CSV stream to stdout: one header line plus one
 * row per sample interval. tests/soak/run.sh applies the pass/fail
 * drift criteria (awk).
 *
 * Why fork+exec by hand instead of cmcp_client_connect_stdio: we need
 * the child pid to sample /proc/<pid>/{status,fd}; connect_stdio
 * holds the pid privately. The setup below mirrors what connect_stdio
 * does internally — pipe pair, dup2 onto child's stdin/stdout, and a
 * stdio transport over the parent's ends. */

#define _POSIX_C_SOURCE 200809L

#include "cmcp.h"
#include "cmcp_client.h"
#include "cmcp_json.h"
#include "cmcp_transport.h"
#include "cmcp_types.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

/* ====================================================================== */
/* Time + /proc helpers                                                    */
/* ====================================================================== */

static double now_seconds(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

typedef struct {
    long rss_kb;
    int  threads;
    int  fd_count;
} proc_metrics_t;

/* Read VmRSS + Threads from /proc/<pid>/status; count open FDs from
 * /proc/<pid>/fd. The FD count is more honest than FDSize (which only
 * grows) for leak detection. */
static void read_proc_metrics(pid_t pid, proc_metrics_t *out) {
    out->rss_kb = -1; out->threads = -1; out->fd_count = -1;

    char path[64];
    snprintf(path, sizeof path, "/proc/%d/status", (int)pid);
    FILE *f = fopen(path, "r");
    if (f) {
        char line[256];
        while (fgets(line, sizeof line, f)) {
            if (strncmp(line, "VmRSS:", 6) == 0)
                sscanf(line + 6, "%ld", &out->rss_kb);
            else if (strncmp(line, "Threads:", 8) == 0)
                sscanf(line + 8, "%d", &out->threads);
        }
        fclose(f);
    }

    snprintf(path, sizeof path, "/proc/%d/fd", (int)pid);
    DIR *d = opendir(path);
    int n = 0;
    if (d) {
        struct dirent *e;
        while ((e = readdir(d))) {
            if (e->d_name[0] != '.') n++;
        }
        closedir(d);
    }
    out->fd_count = n;
}

/* ====================================================================== */
/* Spawn a server child and return a stdio transport on its end           */
/* ====================================================================== */

static pid_t spawn_child(const char *path, cmcp_transport_t **out_t) {
    int p2c[2], c2p[2];
    if (pipe(p2c) != 0) return -1;
    if (pipe(c2p) != 0) { close(p2c[0]); close(p2c[1]); return -1; }
    pid_t pid = fork();
    if (pid < 0) {
        close(p2c[0]); close(p2c[1]); close(c2p[0]); close(c2p[1]);
        return -1;
    }
    if (pid == 0) {
        /* child */
        dup2(p2c[0], STDIN_FILENO);
        dup2(c2p[1], STDOUT_FILENO);
        close(p2c[0]); close(p2c[1]); close(c2p[0]); close(c2p[1]);
        execlp(path, path, (char *)NULL);
        _exit(127);
    }
    /* parent */
    close(p2c[0]); close(c2p[1]);
    *out_t = cmcp_transport_stdio_new_fds(c2p[0], p2c[1]);
    if (!*out_t) {
        close(p2c[1]); close(c2p[0]);
        kill(pid, SIGTERM); waitpid(pid, NULL, 0);
        return -1;
    }
    return pid;
}

/* ====================================================================== */
/* Latency ring buffer + quantiles                                         */
/* ====================================================================== */
/* Reset each sample interval; reports p50/p99 over the just-completed
 * window. Capped at 4096 entries — at sub-ms call latency and 5s sample
 * interval we typically fit well within. */

#define LAT_CAP 4096

typedef struct {
    long   buf[LAT_CAP];
    size_t n;
} latbuf_t;

static int lat_cmp(const void *a, const void *b) {
    long da = *(const long *)a, db = *(const long *)b;
    return (da > db) - (da < db);
}

static void latbuf_quantiles(const latbuf_t *l, long *p50, long *p99) {
    if (l->n == 0) { *p50 = 0; *p99 = 0; return; }
    long *tmp = (long *)malloc(l->n * sizeof(long));
    if (!tmp) { *p50 = 0; *p99 = 0; return; }
    memcpy(tmp, l->buf, l->n * sizeof(long));
    qsort(tmp, l->n, sizeof(long), lat_cmp);
    *p50 = tmp[l->n / 2];
    *p99 = tmp[(l->n * 99) / 100];
    free(tmp);
}

/* ====================================================================== */
/* Workload                                                                */
/* ====================================================================== */

static int do_one_call(cmcp_client_t *c, long *out_us) {
    cmcp_json_t *params = cmcp_json_new_object();
    cmcp_json_object_set(params, "name", cmcp_json_new_string("echo"));
    cmcp_json_t *args = cmcp_json_new_object();
    cmcp_json_object_set(args, "text",
                          cmcp_json_new_string("soak workload payload"));
    cmcp_json_object_set(params, "arguments", args);

    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    cmcp_rpc_message_t resp;
    cmcp_rpc_message_init(&resp);
    int rc = cmcp_client_request(c, "tools/call", params, &resp);
    clock_gettime(CLOCK_MONOTONIC, &t1);
    cmcp_rpc_message_clear(&resp);

    *out_us = (long)(t1.tv_sec - t0.tv_sec) * 1000000L
            + (long)(t1.tv_nsec - t0.tv_nsec) / 1000L;
    return rc;
}

/* ====================================================================== */
/* Main                                                                    */
/* ====================================================================== */

static int connect_session(const char *server,
                            pid_t *out_child,
                            cmcp_transport_t **out_t,
                            cmcp_client_t **out_c) {
    *out_child = spawn_child(server, out_t);
    if (*out_child < 0) return -1;
    *out_c = cmcp_client_new("soak-driver", "0.0.1");
    if (!*out_c) return -1;
    if (cmcp_client_handshake(*out_c, *out_t) != CMCP_OK) return -1;
    return 0;
}

static void teardown_session(pid_t child,
                              cmcp_transport_t *t,
                              cmcp_client_t *c) {
    if (c) cmcp_client_free(c);
    if (t) cmcp_transport_close(t);
    if (child > 0) waitpid(child, NULL, 0);
}

int main(int argc, char **argv) {
    int         duration       = 120;
    int         sample_seconds = 5;
    int         do_churn       = 0;
    int         churn_seconds  = 15;
    const char *server         = "./examples/echo-server";

    for (int i = 1; i < argc; i++) {
        if (!strncmp(argv[i], "--duration=", 11))
            duration = atoi(argv[i] + 11);
        else if (!strncmp(argv[i], "--sample-interval=", 18))
            sample_seconds = atoi(argv[i] + 18);
        else if (!strncmp(argv[i], "--server=", 9))
            server = argv[i] + 9;
        else if (!strcmp(argv[i], "--churn"))
            do_churn = 1;
        else if (!strncmp(argv[i], "--churn-interval=", 17))
            churn_seconds = atoi(argv[i] + 17);
    }

    /* SIGPIPE would otherwise kill us if the child dies mid-write. */
    signal(SIGPIPE, SIG_IGN);

    printf("elapsed_s,parent_rss_kb,parent_fd,parent_threads,"
           "child_rss_kb,child_fd,child_threads,calls,p50_us,p99_us\n");
    fflush(stdout);

    cmcp_transport_t *t = NULL;
    cmcp_client_t    *c = NULL;
    pid_t             child = -1;
    if (connect_session(server, &child, &t, &c) != 0) {
        fprintf(stderr, "soak: initial session failed\n");
        teardown_session(child, t, c);
        return 1;
    }

    double t_start       = now_seconds();
    double t_next_sample = t_start + sample_seconds;
    double t_next_churn  = t_start + churn_seconds;
    long   total_calls   = 0;
    int    err_count     = 0;
    latbuf_t lat = {0};

    for (;;) {
        double now = now_seconds();
        if (now - t_start >= duration) break;

        long us = 0;
        int rc = do_one_call(c, &us);
        if (rc == CMCP_OK) {
            if (lat.n < LAT_CAP) lat.buf[lat.n++] = us;
            total_calls++;
        } else {
            err_count++;
            if (err_count > 100) {
                fprintf(stderr, "soak: too many call failures, aborting\n");
                break;
            }
        }

        if (now >= t_next_sample) {
            proc_metrics_t mp, mc;
            read_proc_metrics(getpid(), &mp);
            read_proc_metrics(child, &mc);
            long p50, p99;
            latbuf_quantiles(&lat, &p50, &p99);
            printf("%.0f,%ld,%d,%d,%ld,%d,%d,%ld,%ld,%ld\n",
                   now - t_start,
                   mp.rss_kb, mp.fd_count, mp.threads,
                   mc.rss_kb, mc.fd_count, mc.threads,
                   total_calls, p50, p99);
            fflush(stdout);
            lat.n        = 0;
            total_calls  = 0;
            t_next_sample = now + sample_seconds;
        }

        if (do_churn && now >= t_next_churn) {
            teardown_session(child, t, c);
            t = NULL; c = NULL; child = -1;
            if (connect_session(server, &child, &t, &c) != 0) {
                fprintf(stderr, "soak: churn respawn failed\n");
                break;
            }
            t_next_churn = now + churn_seconds;
        }
    }

    teardown_session(child, t, c);
    return 0;
}
