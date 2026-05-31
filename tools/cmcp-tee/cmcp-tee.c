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
 * is NOT touched — let the host see it or redirect from the shell.
 *
 * Per-frame log cap (B.2): the wire is always teed faithfully, but each
 * LOG record is bounded by $CMCP_TEE_MAX_FRAME bytes (default 1 MiB; 0
 * disables). A frame over the cap is recorded clipped, with extra fields
 * `"truncated":true,"orig_len":<true-bytes>,"cap":<cap>`. This keeps a
 * hostile upstream from turning the wire log into a memory bomb (tee
 * links no cMCP libs, so it can't reuse the server's JSON caps). */

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

/* Per-frame byte cap for the JSONL *log record* (B.2). The tee stays
 * byte-for-byte transparent on the wire — this only bounds what we
 * write to the log, so a hostile peer's 10 MiB single-frame payload is
 * recorded as a truncated marker instead of (a) a 10 MiB JSONL line and
 * (b) a 10 MiB accumulation buffer in this process. Snapshotted once in
 * main from $CMCP_TEE_MAX_FRAME (default 1 MiB; 0 disables the cap). */
#define TEE_DEFAULT_MAX_FRAME (1024UL * 1024UL)
static size_t g_max_frame = TEE_DEFAULT_MAX_FRAME;

static double now_seconds(void) {
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

/* JSON-string-escape the first `len` bytes of `frame` and emit one log
 * line under lock so concurrent pump threads can't interleave writes.
 * When `orig_len > len` the frame was truncated to the cap: we add
 * `"truncated":true,"orig_len":<orig_len>,"cap":<len>` so a reader can
 * tell a faithful record from a clipped one. */
static void log_frame(const char *dir, const char *frame,
                      size_t len, size_t orig_len) {
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
    if (orig_len > len)
        fprintf(g_log, "\",\"truncated\":true,\"orig_len\":%zu,\"cap\":%zu}\n",
                orig_len, len);
    else
        fputs("\"}\n", g_log);
    fflush(g_log);
    pthread_mutex_unlock(&g_log_mu);
}

/* Pump bytes from in_fd to out_fd; whenever a '\n' arrives, the
 * just-completed line (without the newline) is sent to the log under
 * the given direction tag. */
static void pump(int in_fd, int out_fd, const char *dir) {
    char  *buf   = NULL;
    size_t alloc = 0;        /* bytes allocated in buf                 */
    size_t used  = 0;        /* bytes stored for the current line (<= cap) */
    size_t total = 0;        /* true current-line length, excl. newline */
    char   chunk[4096];
    for (;;) {
        ssize_t n = read(in_fd, chunk, sizeof chunk);
        if (n <= 0) break;
        /* Forward bytes to peer first so latency isn't affected by logging.
         * Forwarding is always faithful — the cap only bounds the log. */
        ssize_t w = 0;
        while (w < n) {
            ssize_t k = write(out_fd, chunk + w, (size_t)(n - w));
            if (k <= 0) goto done;
            w += k;
        }
        for (ssize_t i = 0; i < n; i++) {
            char c = chunk[i];
            if (c == '\n') {
                log_frame(dir, buf, used, total);
                used = 0; total = 0;
                continue;
            }
            total++;
            /* Store into the log buffer only while under the cap
             * (0 = unlimited). Past the cap we keep counting `total`
             * for the truncation marker but stop growing memory. */
            if (g_max_frame == 0 || used < g_max_frame) {
                if (used + 1 > alloc) {
                    size_t nalloc = alloc ? alloc * 2 : 256;
                    if (g_max_frame && nalloc > g_max_frame)
                        nalloc = g_max_frame;
                    char *nb = (char *)realloc(buf, nalloc);
                    if (!nb) goto done;
                    buf = nb; alloc = nalloc;
                }
                buf[used++] = c;
            }
        }
    }
done:
    /* Flush any trailing partial line so weird shutdowns are still visible. */
    if (total > 0) log_frame(dir, buf, used, total);
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

    /* Snapshot the per-frame log cap once, before any pump thread runs. */
    const char *mf = getenv("CMCP_TEE_MAX_FRAME");
    if (mf && *mf) {
        char *end;
        long v = strtol(mf, &end, 10);
        if (*end == '\0' && v >= 0) g_max_frame = (size_t)v;  /* 0 = unlimited */
    }

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
    if (g_log) fclose(g_log);
    return WIFEXITED(status) ? WEXITSTATUS(status) : 1;
}
