/* cmcp-tee — transparent stdio MCP proxy with JSONL wire capture.
 *
 * Wraps any cMCP server (echo-server, filesystem-mcp, crag-mcp, …) so a
 * host (Claude Code, cmcp-inspect, anything) can drive it normally
 * while we keep a faithful byte-for-byte transcript of every frame in
 * both directions. Feeds the conformance/fixtures/ corpus and the
 * replay-based regression gate that 5.3 will need.
 *
 *   cmcp-tee <log.jsonl> <server-path> [server-args...]
 *
 * Log format: one JSON object per line.
 *
 *   {"t":<unix-seconds-float>,"dir":"in"|"out","frame":"<raw JSON frame>"}
 *
 *   dir="in"  → host → server (client request / notification)
 *   dir="out" → server → host (response / notification)
 *
 * Frames preserve newline-delimited boundaries; the trailing newline is
 * stripped before logging. The file is opened in append mode so multiple
 * sessions accumulate, separated by `t`. stderr from the wrapped server
 * is NOT touched — let the host see it or redirect from the shell. */

#define _POSIX_C_SOURCE 200809L

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

static FILE            *g_log    = NULL;
static pthread_mutex_t  g_log_mu = PTHREAD_MUTEX_INITIALIZER;

static double now_seconds(void) {
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

/* JSON-string-escape `frame` and emit one log line under lock so
 * concurrent pump threads can't interleave their writes. */
static void log_frame(const char *dir, const char *frame, size_t len) {
    pthread_mutex_lock(&g_log_mu);
    fprintf(g_log, "{\"t\":%.6f,\"dir\":\"%s\",\"frame\":\"",
            now_seconds(), dir);
    for (size_t i = 0; i < len; i++) {
        unsigned char c = (unsigned char)frame[i];
        switch (c) {
        case '"':  fputs("\\\"", g_log); break;
        case '\\': fputs("\\\\", g_log); break;
        case '\n': fputs("\\n",  g_log); break;
        case '\r': fputs("\\r",  g_log); break;
        case '\t': fputs("\\t",  g_log); break;
        default:
            if (c < 0x20) fprintf(g_log, "\\u%04x", c);
            else          fputc((int)c, g_log);
        }
    }
    fputs("\"}\n", g_log);
    fflush(g_log);
    pthread_mutex_unlock(&g_log_mu);
}

/* Pump bytes from in_fd to out_fd; whenever a '\n' arrives, the
 * just-completed line (without the newline) is sent to the log under
 * the given direction tag. */
static void pump(int in_fd, int out_fd, const char *dir) {
    char  *buf  = NULL;
    size_t cap  = 0;
    size_t used = 0;
    char   chunk[4096];
    for (;;) {
        ssize_t n = read(in_fd, chunk, sizeof chunk);
        if (n <= 0) break;
        /* Forward bytes to peer first so latency isn't affected by logging. */
        ssize_t w = 0;
        while (w < n) {
            ssize_t k = write(out_fd, chunk + w, (size_t)(n - w));
            if (k <= 0) goto done;
            w += k;
        }
        for (ssize_t i = 0; i < n; i++) {
            if (used + 1 > cap) {
                size_t ncap = cap ? cap * 2 : 256;
                char *nb = (char *)realloc(buf, ncap);
                if (!nb) goto done;
                buf = nb; cap = ncap;
            }
            buf[used++] = chunk[i];
            if (chunk[i] == '\n') {
                log_frame(dir, buf, used - 1);
                used = 0;
            }
        }
    }
done:
    /* Flush any trailing partial line so weird shutdowns are still visible. */
    if (used > 0) log_frame(dir, buf, used);
    free(buf);
}

typedef struct {
    int         in_fd;
    int         out_fd;
    const char *dir;
} pump_arg_t;

static void *pump_thread(void *arg) {
    pump_arg_t *a = (pump_arg_t *)arg;
    pump(a->in_fd, a->out_fd, a->dir);
    return NULL;
}

int main(int argc, char **argv) {
    if (argc < 3) {
        fprintf(stderr,
            "usage: cmcp-tee <log.jsonl> <server-path> [server-args...]\n");
        return 2;
    }
    const char *log_path = argv[1];
    const char *server   = argv[2];

    g_log = fopen(log_path, "a");
    if (!g_log) {
        fprintf(stderr, "cmcp-tee: cannot open log '%s': %s\n",
                log_path, strerror(errno));
        return 1;
    }
    signal(SIGPIPE, SIG_IGN);

    int p2c[2], c2p[2];
    if (pipe(p2c) != 0 || pipe(c2p) != 0) {
        fprintf(stderr, "cmcp-tee: pipe(): %s\n", strerror(errno));
        return 1;
    }
    pid_t pid = fork();
    if (pid < 0) {
        fprintf(stderr, "cmcp-tee: fork(): %s\n", strerror(errno));
        return 1;
    }
    if (pid == 0) {
        /* child = wrapped server */
        dup2(p2c[0], STDIN_FILENO);
        dup2(c2p[1], STDOUT_FILENO);
        close(p2c[0]); close(p2c[1]); close(c2p[0]); close(c2p[1]);
        /* Pass argv[2..] as the server's own argv (argv[2] becomes argv[0]). */
        execvp(server, argv + 2);
        fprintf(stderr, "cmcp-tee: execvp '%s': %s\n",
                server, strerror(errno));
        _exit(127);
    }
    /* parent = proxy */
    close(p2c[0]); close(c2p[1]);

    pthread_t  th_out;
    pump_arg_t a_out = { c2p[0], STDOUT_FILENO, "out" };
    pthread_create(&th_out, NULL, pump_thread, &a_out);

    /* Main thread pumps host stdin → server stdin. When the host closes
     * its end (EOF), close the server's stdin so the server exits and
     * its stdout EOFs in turn — that joins the out-pump cleanly. */
    pump(STDIN_FILENO, p2c[1], "in");

    close(p2c[1]);
    pthread_join(th_out, NULL);
    close(c2p[0]);

    int status = 0;
    waitpid(pid, &status, 0);
    fclose(g_log);
    return WIFEXITED(status) ? WEXITSTATUS(status) : 1;
}
