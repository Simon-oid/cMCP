/* Long-running stability driver — stdio variant (phase 5.6).
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
 * stdio transport over the parent's ends.
 *
 * Shared bits (proc sampling, latbuf, workload, CSV schema) live in
 * tests/soak/soak_common.h; the HTTP variant uses the same helpers. */

#include "soak_common.h"

#include "cmcp_transport.h"

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

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
/* Session lifecycle                                                       */
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

/* ====================================================================== */
/* Main                                                                    */
/* ====================================================================== */

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

    fputs(SOAK_CSV_HEADER, stdout);
    fflush(stdout);

    cmcp_transport_t *t = NULL;
    cmcp_client_t    *c = NULL;
    pid_t             child = -1;
    if (connect_session(server, &child, &t, &c) != 0) {
        fprintf(stderr, "soak: initial session failed\n");
        teardown_session(child, t, c);
        return 1;
    }

    double t_start       = soak_now_s();
    double t_next_sample = t_start + sample_seconds;
    double t_next_churn  = t_start + churn_seconds;
    long   total_calls   = 0;
    int    err_count     = 0;
    soak_latbuf_t lat = {0};

    for (;;) {
        double now = soak_now_s();
        if (now - t_start >= duration) break;

        long us = 0;
        int rc = soak_do_one_call(c, &us);
        if (rc == CMCP_OK) {
            if (lat.n < SOAK_LAT_CAP) lat.buf[lat.n++] = us;
            total_calls++;
        } else {
            err_count++;
            if (err_count > 100) {
                fprintf(stderr, "soak: too many call failures, aborting\n");
                break;
            }
        }

        if (now >= t_next_sample) {
            soak_proc_t mp, mc;
            soak_read_proc(getpid(), &mp);
            soak_read_proc(child, &mc);
            long p50, p99;
            soak_latbuf_quantiles(&lat, &p50, &p99);
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
