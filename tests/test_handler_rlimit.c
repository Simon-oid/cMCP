/* _POSIX_C_SOURCE: setenv/unsetenv, fork, pipe, getrlimit. */
#define _POSIX_C_SOURCE 200809L

/* test_handler_rlimit.c — F.4 opt-in RLIMIT_AS insurance.
 *
 * The knob is `CMCP_HANDLER_RLIMIT_AS_MB`: when a positive integer, the
 * server tightens the process RLIMIT_AS soft limit once, at
 * cmcp_server_run() entry, so a memory-leaking handler hits
 * malloc-returns-NULL instead of the OOM killer. It must:
 *   - be INERT by default (unset / invalid / zero / negative → no-op),
 *   - only ever TIGHTEN (never raise an existing soft limit), and
 *   - never exceed the hard limit.
 *
 * Each case runs in a forked child: the limit is process-wide and the
 * init is pthread_once-guarded, so isolation per case is mandatory. The
 * child does the full assertion and reports via exit status.
 *
 * Tightening to a finite cap is unsafe under ASan/TSan — both reserve
 * huge virtual-address ranges at startup that a finite RLIMIT_AS would
 * then starve — so the cases that actually lower the limit are skipped
 * under a sanitizer. The inert-by-default cases run everywhere; they are
 * the load-bearing contract (a library must not surprise its host). */

#include "cmcp_server.h"
#include "cmcp_transport.h"
#include "test.h"

#include <fcntl.h>
#include <sys/resource.h>
#include <sys/wait.h>
#include <unistd.h>

#if defined(__SANITIZE_ADDRESS__) || defined(__SANITIZE_THREAD__)
#  define UNDER_SANITIZER 1
#elif defined(__has_feature)
#  if __has_feature(address_sanitizer) || __has_feature(thread_sanitizer)
#    define UNDER_SANITIZER 1
#  endif
#endif
#ifndef UNDER_SANITIZER
#  define UNDER_SANITIZER 0
#endif

#define ONE_MIB (1024UL * 1024UL)

/* Valgrind maps large VA ranges of its own; a finite RLIMIT_AS would
 * starve them and crash the guest. Detect it at runtime (the LD_PRELOAD
 * it injects names vgpreload) so `make valgrind` skips the tightening
 * cases just like the sanitizers do. */
static int unsafe_to_tighten(void) {
    if (UNDER_SANITIZER) return 1;
    const char *pre = getenv("LD_PRELOAD");
    if (pre && (strstr(pre, "vgpreload") || strstr(pre, "valgrind")))
        return 1;
    return 0;
}

/* Drive a server to immediate EOF so cmcp_server_run() returns at once,
 * having run maybe_apply_rlimit_as() on the way in. */
static void run_server_eof(void) {
    int p[2];
    if (pipe(p) != 0) _exit(2);
    close(p[1]);                                  /* read end now at EOF */
    int devnull = open("/dev/null", O_WRONLY);
    if (devnull < 0) _exit(2);

    cmcp_transport_t *t = cmcp_transport_stdio_new_fds(p[0], devnull);
    cmcp_server_t    *s = cmcp_server_new("rlimit-test", "0");
    if (!t || !s) _exit(2);
    cmcp_server_run(s, t);
    cmcp_transport_close(t);
    cmcp_server_free(s);
}

/* Inert cases: set (or clear) the env, run a server, assert RLIMIT_AS is
 * byte-for-byte unchanged. Returns 0 on pass, 1 on fail. */
static int body_expect_noop(const char *envval) {
    struct rlimit before, after;
    if (getrlimit(RLIMIT_AS, &before) != 0) return 1;
    if (envval) setenv("CMCP_HANDLER_RLIMIT_AS_MB", envval, 1);
    else        unsetenv("CMCP_HANDLER_RLIMIT_AS_MB");
    run_server_eof();
    if (getrlimit(RLIMIT_AS, &after) != 0) return 1;
    return (after.rlim_cur == before.rlim_cur &&
            after.rlim_max == before.rlim_max) ? 0 : 1;
}

/* Tightening: a 1 GiB cap must leave rlim_cur finite and <= 1 GiB. If the
 * baseline was unlimited it must now be finite (proves we applied a
 * ceiling). The hard limit must be untouched. Returns 0/1. */
static int body_expect_tighten(void) {
    struct rlimit before, after;
    if (getrlimit(RLIMIT_AS, &before) != 0) return 1;
    setenv("CMCP_HANDLER_RLIMIT_AS_MB", "1024", 1);
    run_server_eof();
    if (getrlimit(RLIMIT_AS, &after) != 0) return 1;

    rlim_t cap = 1024UL * ONE_MIB;
    if (before.rlim_cur == RLIM_INFINITY) {
        if (after.rlim_cur == RLIM_INFINITY) return 1;
        if (after.rlim_cur > cap)            return 1;
    } else {
        if (after.rlim_cur > before.rlim_cur) return 1;   /* never raised */
        if (after.rlim_cur > cap)             return 1;
    }
    return (after.rlim_max == before.rlim_max) ? 0 : 1;   /* hard untouched */
}

/* Never-raise: ask for a cap ABOVE the current finite soft limit;
 * rlim_cur must stay put. Vacuous (pass) when baseline is unlimited. */
static int body_never_raise(void) {
    struct rlimit before, after;
    if (getrlimit(RLIMIT_AS, &before) != 0) return 1;
    if (before.rlim_cur == RLIM_INFINITY) return 0;       /* vacuous */

    unsigned long mb = (unsigned long)(before.rlim_cur / ONE_MIB) + 4096UL;
    char buf[32];
    snprintf(buf, sizeof buf, "%lu", mb);
    setenv("CMCP_HANDLER_RLIMIT_AS_MB", buf, 1);
    run_server_eof();
    if (getrlimit(RLIMIT_AS, &after) != 0) return 1;
    return (after.rlim_cur == before.rlim_cur) ? 0 : 1;
}

/* Run `body` in a forked child; the TEST_ASSERT passes iff it exits 0. */
#define RUN_CHILD(label, call) do {                                           \
    fprintf(stderr, "  - %s\n", (label));                                     \
    pid_t pid = fork();                                                       \
    if (pid == 0) _exit(call);                                                \
    int st = 0; waitpid(pid, &st, 0);                                         \
    TEST_ASSERT(WIFEXITED(st) && WEXITSTATUS(st) == 0);                       \
} while (0)

int main(void) {
    fprintf(stderr, "test_handler_rlimit:\n");

    /* Inert by default — every lane (these never tighten). */
    RUN_CHILD("test_unset_is_noop",    body_expect_noop(NULL));
    RUN_CHILD("test_invalid_is_noop",  body_expect_noop("not-a-number"));
    RUN_CHILD("test_zero_is_noop",     body_expect_noop("0"));
    RUN_CHILD("test_negative_is_noop", body_expect_noop("-512"));

    if (unsafe_to_tighten()) {
        fprintf(stderr, "  - test_tightens (skipped: sanitizer/valgrind)\n");
        fprintf(stderr, "  - test_never_raises (skipped: sanitizer/valgrind)\n");
    } else {
        RUN_CHILD("test_tightens",     body_expect_tighten());
        RUN_CHILD("test_never_raises", body_never_raise());
    }

    TEST_DONE();
}
