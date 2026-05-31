/* test_tee_frame_cap.c — B.2: cmcp-tee per-frame log cap.
 *
 * cmcp-tee links no cMCP libs, so it cannot reuse the server's JSON
 * depth/element caps. A hostile upstream can therefore ship an enormous
 * single frame; without a bound the tee would both allocate it and write
 * a giant JSONL line. The fix is a byte-level cap on the *log record*
 * (env CMCP_TEE_MAX_FRAME, default 1 MiB, 0 disables) — the wire stays
 * faithful, but an over-cap frame is recorded as a truncated marker.
 *
 * This test execs the real tee binary wrapping /bin/cat, sends a line
 * well over a deliberately tiny cap, and asserts the log carries a
 * truncated record naming the original length and the cap. */

#define _POSIX_C_SOURCE 200809L

#include "test.h"

#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#define TEE_BIN "tools/cmcp-tee/cmcp-tee"
#define CAP     64
#define LINELEN 200

/* Slurp a file into a NUL-terminated heap buffer. Returns NULL on error. */
static char *slurp(const char *path) {
    int fd = open(path, O_RDONLY);
    if (fd < 0) return NULL;
    size_t cap = 4096, len = 0;
    char *b = malloc(cap);
    if (!b) { close(fd); return NULL; }
    for (;;) {
        if (len + 4096 + 1 > cap) {
            cap *= 2;
            char *nb = realloc(b, cap);
            if (!nb) { free(b); close(fd); return NULL; }
            b = nb;
        }
        ssize_t n = read(fd, b + len, 4096);
        if (n < 0) { free(b); close(fd); return NULL; }
        if (n == 0) break;
        len += (size_t)n;
    }
    b[len] = '\0';
    close(fd);
    return b;
}

static void test_oversize_frame_is_truncated_in_log(void) {
    char logpath[] = "/tmp/cmcp_tee_cap_XXXXXX";
    int lfd = mkstemp(logpath);
    TEST_ASSERT(lfd >= 0);
    close(lfd);                                   /* tee opens it itself */

    int in_pipe[2];
    TEST_ASSERT(pipe(in_pipe) == 0);

    setenv("CMCP_TEE_MAX_FRAME", "64", 1);        /* == CAP */

    pid_t pid = fork();
    TEST_ASSERT(pid >= 0);
    if (pid == 0) {
        dup2(in_pipe[0], STDIN_FILENO);
        int devnull = open("/dev/null", O_WRONLY);
        if (devnull >= 0) dup2(devnull, STDOUT_FILENO);
        close(in_pipe[0]); close(in_pipe[1]);
        execl(TEE_BIN, TEE_BIN, logpath, "/bin/cat", (char *)NULL);
        _exit(127);
    }
    close(in_pipe[0]);

    /* One line of LINELEN 'A's + newline, comfortably over CAP. */
    char line[LINELEN + 1];
    memset(line, 'A', LINELEN);
    line[LINELEN] = '\n';
    ssize_t w = write(in_pipe[1], line, sizeof line);
    TEST_ASSERT(w == (ssize_t)sizeof line);
    close(in_pipe[1]);                            /* EOF → tee + cat exit */

    int status = 0;
    waitpid(pid, &status, 0);
    TEST_ASSERT(WIFEXITED(status));

    char *log = slurp(logpath);
    TEST_ASSERT(log != NULL);
    if (log) {
        /* A truncated record naming the true length and the cap. */
        TEST_ASSERT(strstr(log, "\"truncated\":true") != NULL);
        TEST_ASSERT(strstr(log, "\"orig_len\":200")   != NULL);
        TEST_ASSERT(strstr(log, "\"cap\":64")         != NULL);
        /* The faithful 200-'A' run must NOT appear — the log was clipped
         * to the cap. (cat would have echoed all 200 on the wire, but the
         * wire isn't the log.) */
        char big[LINELEN + 1];
        memset(big, 'A', LINELEN);
        big[LINELEN] = '\0';
        TEST_ASSERT(strstr(log, big) == NULL);
        free(log);
    }
    unlink(logpath);
}

int main(void) {
    fprintf(stderr, "test_tee_frame_cap:\n");
    TEST_RUN(test_oversize_frame_is_truncated_in_log);
    TEST_DONE();
}
