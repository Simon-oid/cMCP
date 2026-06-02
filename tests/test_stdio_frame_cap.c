/* pipe(), fdopen() via the transport — POSIX. */
#define _POSIX_C_SOURCE 200809L

#include "test.h"
#include "cmcp.h"
#include "cmcp_transport.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* ====================================================================== *
 * Stdio frame-size cap (DoS hardening).
 *
 * getline() grows without bound, so a peer streaming a frame with no
 * newline can drive unbounded allocation until OOM. The stdio transport
 * caps the per-frame size via CMCP_STDIO_MAX_FRAME (default 16 MiB), the
 * analogue of the HTTP body cap. Over-budget → CMCP_EPROTOCOL.
 *
 * The cap is snapshotted ONCE per process via pthread_once on the first
 * read, so this lives in its own binary and sets the env before any read
 * happens — same isolation pattern the redact/HTTP knob tests use.
 * ====================================================================== */

/* Oversize frame is refused with CMCP_EPROTOCOL, not OOM. */
static void test_oversize_frame_rejected(void) {
    int rp[2], sp[2];
    TEST_ASSERT(pipe(rp) == 0);
    TEST_ASSERT(pipe(sp) == 0);

    cmcp_transport_t *t = cmcp_transport_stdio_new_fds(rp[0], sp[1]);
    TEST_ASSERT(t != NULL);

    /* 200 non-newline bytes + newline: past the 64-byte cap set in main.
     * Well under the pipe buffer (~64 KiB) so the write won't block. */
    char big[201];
    memset(big, 'x', 200);
    big[200] = '\n';
    TEST_ASSERT(write(rp[1], big, sizeof big) == (ssize_t)sizeof big);

    char *got = NULL; size_t got_len = 0;
    TEST_ASSERT(cmcp_transport_read(t, &got, &got_len) == CMCP_EPROTOCOL);

    cmcp_transport_close(t);
    close(rp[1]);
    close(sp[0]);
}

/* A frame exactly at the cap still round-trips cleanly. */
static void test_frame_at_cap_ok(void) {
    int rp[2], sp[2];
    TEST_ASSERT(pipe(rp) == 0);
    TEST_ASSERT(pipe(sp) == 0);

    cmcp_transport_t *t = cmcp_transport_stdio_new_fds(rp[0], sp[1]);
    TEST_ASSERT(t != NULL);

    char ok[65];
    memset(ok, 'y', 64);
    ok[64] = '\n';
    TEST_ASSERT(write(rp[1], ok, sizeof ok) == (ssize_t)sizeof ok);

    char *got = NULL; size_t got_len = 0;
    TEST_ASSERT(cmcp_transport_read(t, &got, &got_len) == CMCP_OK);
    TEST_ASSERT(got_len == 64);
    TEST_ASSERT(memcmp(got, ok, 64) == 0);
    free(got);

    cmcp_transport_close(t);
    close(rp[1]);
    close(sp[0]);
}

int main(void) {
    /* Latch a tiny cap before the first stdio read in this process. */
    setenv("CMCP_STDIO_MAX_FRAME", "64", /*overwrite=*/1);

    fprintf(stderr, "test_stdio_frame_cap:\n");

    TEST_RUN(test_oversize_frame_rejected);
    TEST_RUN(test_frame_at_cap_ok);

    TEST_DONE();
}
