/* Long-running stability driver — HTTP variant (phase 6.6.4).
 *
 * Same shape as soak_driver.c, but the parent ↔ child link is the
 * Streamable HTTP transport instead of stdio:
 *
 *   - The parent picks an ephemeral 127.0.0.1 port via bind(0) +
 *     getsockname() + close(), then fork+execs tests/soak/echo_http_server
 *     with that port baked into argv. The child binds it as its
 *     HTTP listener.
 *   - The parent connects via cmcp_transport_http_connect to
 *     http://127.0.0.1:<port>/mcp and runs the same tools/call echo
 *     workload + /proc sampling loop as the stdio variant.
 *
 * Why the ephemeral-port + argv handoff rather than letting the child
 * pick: the parent needs the child's PID for /proc sampling, but it
 * also needs the URL before it forks (cmcp_transport_http_connect runs
 * synchronously). The race window between close() and the child's
 * bind() is microseconds on loopback; we have not observed a collision
 * in practice.
 *
 * Shared helpers (proc sampling, latbuf, workload, CSV schema) live
 * in soak_common.h; identical to the stdio variant.
 *
 * Output CSV row is identical to soak_driver — tests/soak/run_http.sh
 * applies the same awk drift criteria. */

#include "soak_common.h"

#include "cmcp_transport.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

/* ====================================================================== */
/* Ephemeral-port picker                                                   */
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
/* Spawn server child on the chosen port                                   */
/* ====================================================================== */

static pid_t spawn_server(const char *path, unsigned short port) {
    pid_t pid = fork();
    if (pid < 0) return -1;
    if (pid == 0) {
        /* child */
        char arg[32];
        snprintf(arg, sizeof arg, "--port=%u", (unsigned)port);
        execlp(path, path, arg, (char *)NULL);
        _exit(127);
    }
    return pid;
}

/* ====================================================================== */
/* Session lifecycle                                                       */
/* ====================================================================== */

static int connect_session(const char *server,
                            unsigned short port,
                            pid_t *out_child,
                            cmcp_transport_t **out_t,
                            cmcp_client_t **out_c) {
    *out_child = spawn_server(server, port);
    if (*out_child < 0) return -1;

    /* Brief delay so the acceptor reaches poll() before connect. The
     * HTTP client transport doesn't retry connect failures; if the
     * child hasn't bound yet we'll see -ECONNREFUSED at handshake.
     * 50ms is enough on loopback for any reasonable startup. */
    struct timespec brief = { 0, 50 * 1000 * 1000 };
    nanosleep(&brief, NULL);

    char url[64];
    snprintf(url, sizeof url, "http://127.0.0.1:%u/mcp", (unsigned)port);
    *out_t = cmcp_transport_http_connect(url);
    if (!*out_t) return -1;

    *out_c = cmcp_client_new("soak-http-driver", "0.0.1");
    if (!*out_c) return -1;
    if (cmcp_client_handshake(*out_c, *out_t) != CMCP_OK) return -1;
    return 0;
}

static void teardown_session(pid_t child,
                              cmcp_transport_t *t,
                              cmcp_client_t *c) {
    if (c) cmcp_client_free(c);
    if (t) cmcp_transport_close(t);
    /* The child won't exit on its own — HTTP listeners run until the
     * accept loop sees a signal. SIGTERM is graceful enough; reap it. */
    if (child > 0) {
        kill(child, SIGTERM);
        waitpid(child, NULL, 0);
    }
}

/* ====================================================================== */
/* Main                                                                    */
/* ====================================================================== */

int main(int argc, char **argv) {
    int         duration       = 120;
    int         sample_seconds = 5;
    int         do_churn       = 0;
    int         churn_seconds  = 15;
    const char *server         = "./tests/soak/echo_http_server";

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

    signal(SIGPIPE, SIG_IGN);

    /* The 6.5.2 accept-rate gate (default 100 conn/sec, 200 burst) is
     * a peer-flood defense. The soak opens a fresh TCP connection per
     * `tools/call` (libcurl easy handle per POST), so it saturates
     * the burst within ~2 seconds and the server starts replying 503.
     * Since 6.6.x the client surfaces that as CMCP_EAGAIN promptly,
     * but the soak is testing leak/stability under sustained traffic,
     * not the gate itself (which has dedicated tests). Disable unless
     * the user explicitly set CMCP_HTTP_ACCEPT_RATE — and propagate
     * to the child via the inherited environment. */
    if (!getenv("CMCP_HTTP_ACCEPT_RATE")) {
        setenv("CMCP_HTTP_ACCEPT_RATE", "0", 1);
    }

    fputs(SOAK_CSV_HEADER, stdout);
    fflush(stdout);

    unsigned short port = pick_port();
    if (!port) {
        fprintf(stderr, "soak: pick_port failed\n");
        return 1;
    }

    cmcp_transport_t *t = NULL;
    cmcp_client_t    *c = NULL;
    pid_t             child = -1;
    if (connect_session(server, port, &child, &t, &c) != 0) {
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
            lat.n         = 0;
            total_calls   = 0;
            t_next_sample = now + sample_seconds;
        }

        if (do_churn && now >= t_next_churn) {
            teardown_session(child, t, c);
            t = NULL; c = NULL; child = -1;
            /* New port for each respawn — kernel TIME_WAIT on the old
             * port could otherwise stall the next bind(). */
            port = pick_port();
            if (!port ||
                connect_session(server, port, &child, &t, &c) != 0) {
                fprintf(stderr, "soak: churn respawn failed\n");
                break;
            }
            t_next_churn = now + churn_seconds;
        }
    }

    teardown_session(child, t, c);
    return 0;
}
