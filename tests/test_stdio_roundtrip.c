/* fork(), pipe(), dup2() — POSIX. */
#define _POSIX_C_SOURCE 200809L

#include "test.h"
#include "cmcp.h"
#include "cmcp_transport.h"
#include "cmcp_json.h"
#include "cmcp_types.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

/* ====================================================================== */
/* In-process pipe pair: build two transports back-to-back.                */
/* ====================================================================== */
/* Layout:
 *   a->b pipe:  A writes here, B reads from here.
 *   b->a pipe:  B writes here, A reads from here.
 * Each transport owns exactly one endpoint of each pipe. */

typedef struct {
    cmcp_transport_t *a;
    cmcp_transport_t *b;
} transport_pair_t;

static int make_pipe_pair(transport_pair_t *out) {
    int a_to_b[2], b_to_a[2];
    if (pipe(a_to_b) != 0) return -1;
    if (pipe(b_to_a) != 0) { close(a_to_b[0]); close(a_to_b[1]); return -1; }

    out->a = cmcp_transport_stdio_new_fds(b_to_a[0], a_to_b[1]);
    out->b = cmcp_transport_stdio_new_fds(a_to_b[0], b_to_a[1]);
    if (!out->a || !out->b) {
        cmcp_transport_close(out->a);
        cmcp_transport_close(out->b);
        return -1;
    }
    return 0;
}

static void close_pair(transport_pair_t *p) {
    cmcp_transport_close(p->a);
    cmcp_transport_close(p->b);
}

/* ====================================================================== */
/* Single-frame round trip                                                 */
/* ====================================================================== */

static void test_single_frame(void) {
    transport_pair_t p;
    TEST_ASSERT(make_pipe_pair(&p) == 0);

    const char *msg = "{\"hello\":\"world\"}";
    TEST_ASSERT(cmcp_transport_write(p.a, msg, strlen(msg)) == CMCP_OK);

    char *got = NULL; size_t got_len = 0;
    TEST_ASSERT(cmcp_transport_read(p.b, &got, &got_len) == CMCP_OK);
    TEST_ASSERT(got_len == strlen(msg));
    TEST_ASSERT(strcmp(got, msg) == 0);
    free(got);

    close_pair(&p);
}

static void test_many_frames(void) {
    transport_pair_t p;
    TEST_ASSERT(make_pipe_pair(&p) == 0);

    /* Write N frames, then read them all back in order. Pipe buffer
     * (typically 64KiB) easily fits this. */
    enum { N = 50 };
    for (int i = 0; i < N; i++) {
        char buf[64];
        int n = snprintf(buf, sizeof buf, "{\"i\":%d}", i);
        TEST_ASSERT(cmcp_transport_write(p.a, buf, (size_t)n) == CMCP_OK);
    }
    for (int i = 0; i < N; i++) {
        char *got = NULL; size_t got_len = 0;
        TEST_ASSERT(cmcp_transport_read(p.b, &got, &got_len) == CMCP_OK);
        char want[64];
        snprintf(want, sizeof want, "{\"i\":%d}", i);
        TEST_ASSERT(strcmp(got, want) == 0);
        free(got);
    }

    close_pair(&p);
}

static void test_blank_lines_skipped(void) {
    int reader_pipe[2], sink_pipe[2];
    TEST_ASSERT(pipe(reader_pipe) == 0);
    TEST_ASSERT(pipe(sink_pipe) == 0);

    const char *raw = "\n\n{\"a\":1}\n\n{\"b\":2}\n";
    TEST_ASSERT(write(reader_pipe[1], raw, strlen(raw))
                == (ssize_t)strlen(raw));
    close(reader_pipe[1]);

    cmcp_transport_t *t = cmcp_transport_stdio_new_fds(reader_pipe[0],
                                                        sink_pipe[1]);
    TEST_ASSERT(t != NULL);

    char *got = NULL; size_t got_len = 0;
    TEST_ASSERT(cmcp_transport_read(t, &got, &got_len) == CMCP_OK);
    TEST_ASSERT(strcmp(got, "{\"a\":1}") == 0);
    free(got);
    TEST_ASSERT(cmcp_transport_read(t, &got, &got_len) == CMCP_OK);
    TEST_ASSERT(strcmp(got, "{\"b\":2}") == 0);
    free(got);
    /* No more data — reader hits EOF. */
    TEST_ASSERT(cmcp_transport_read(t, &got, &got_len) == CMCP_EIO);

    cmcp_transport_close(t);
    close(sink_pipe[0]);
}

/* ====================================================================== */
/* Refuse writes with embedded newlines                                    */
/* ====================================================================== */

static void test_reject_embedded_newline(void) {
    transport_pair_t p;
    TEST_ASSERT(make_pipe_pair(&p) == 0);

    const char *bad = "line1\nline2";
    TEST_ASSERT(cmcp_transport_write(p.a, bad, strlen(bad)) == CMCP_EINVAL);

    /* Subsequent legitimate frames still work. */
    const char *ok = "{\"ok\":1}";
    TEST_ASSERT(cmcp_transport_write(p.a, ok, strlen(ok)) == CMCP_OK);
    char *got = NULL; size_t got_len = 0;
    TEST_ASSERT(cmcp_transport_read(p.b, &got, &got_len) == CMCP_OK);
    TEST_ASSERT(strcmp(got, ok) == 0);
    free(got);

    close_pair(&p);
}

/* ====================================================================== */
/* EOF handling                                                            */
/* ====================================================================== */

static void test_read_after_writer_closes(void) {
    int rp[2], sp[2];
    TEST_ASSERT(pipe(rp) == 0);
    TEST_ASSERT(pipe(sp) == 0);

    /* Write one frame from the OS side, then close the writer. */
    const char *msg = "{\"x\":1}\n";
    TEST_ASSERT(write(rp[1], msg, strlen(msg)) == (ssize_t)strlen(msg));
    close(rp[1]);

    cmcp_transport_t *t = cmcp_transport_stdio_new_fds(rp[0], sp[1]);
    char *got = NULL; size_t got_len = 0;
    TEST_ASSERT(cmcp_transport_read(t, &got, &got_len) == CMCP_OK);
    TEST_ASSERT(strcmp(got, "{\"x\":1}") == 0);
    free(got);

    /* Next read sees EOF. */
    TEST_ASSERT(cmcp_transport_read(t, &got, &got_len) == CMCP_EIO);

    cmcp_transport_close(t);
    close(sp[0]);
}

/* ====================================================================== */
/* Concurrent writers                                                      */
/* ====================================================================== */
/* Spawn N writer threads each emitting M distinct frames concurrently.
 * The single reader thread must receive exactly N*M whole frames whose
 * contents match one of the expected strings (no interleaving). */

#define CONC_WRITERS  4
#define CONC_PER      25
#define CONC_TOTAL    (CONC_WRITERS * CONC_PER)

typedef struct {
    cmcp_transport_t *t;
    int               wid;
} writer_arg_t;

static void *writer_thread(void *arg) {
    writer_arg_t *wa = (writer_arg_t *)arg;
    for (int i = 0; i < CONC_PER; i++) {
        char buf[64];
        int n = snprintf(buf, sizeof buf, "{\"w\":%d,\"i\":%d}", wa->wid, i);
        if (cmcp_transport_write(wa->t, buf, (size_t)n) != CMCP_OK) {
            return (void *)(intptr_t)1;
        }
    }
    return NULL;
}

static void test_concurrent_writers(void) {
    transport_pair_t p;
    TEST_ASSERT(make_pipe_pair(&p) == 0);

    pthread_t        th[CONC_WRITERS];
    writer_arg_t     args[CONC_WRITERS];
    for (int i = 0; i < CONC_WRITERS; i++) {
        args[i].t = p.a;
        args[i].wid = i;
        TEST_ASSERT(pthread_create(&th[i], NULL, writer_thread, &args[i]) == 0);
    }

    /* Track how many frames we've seen from each writer. */
    int seen[CONC_WRITERS] = {0};
    for (int i = 0; i < CONC_TOTAL; i++) {
        char *got = NULL; size_t got_len = 0;
        TEST_ASSERT(cmcp_transport_read(p.b, &got, &got_len) == CMCP_OK);

        /* Parse {"w":X,"i":Y} and verify it's a frame we expect. */
        cmcp_json_t *j = cmcp_json_parse(got, got_len);
        TEST_ASSERT(j != NULL && j->type == CMCP_JSON_OBJECT);
        const cmcp_json_t *jw = cmcp_json_object_get(j, "w");
        const cmcp_json_t *ji = cmcp_json_object_get(j, "i");
        TEST_ASSERT(jw && jw->type == CMCP_JSON_INT);
        TEST_ASSERT(ji && ji->type == CMCP_JSON_INT);
        int w = (int)jw->i, ii = (int)ji->i;
        TEST_ASSERT(w >= 0 && w < CONC_WRITERS);
        TEST_ASSERT(ii >= 0 && ii < CONC_PER);
        seen[w]++;
        cmcp_json_free(j);
        free(got);
    }

    for (int i = 0; i < CONC_WRITERS; i++) {
        void *rv = NULL;
        pthread_join(th[i], &rv);
        TEST_ASSERT(rv == NULL);
        TEST_ASSERT(seen[i] == CONC_PER);
    }

    close_pair(&p);
}

/* ====================================================================== */
/* Cross-process handshake (fork an echo child)                            */
/* ====================================================================== */
/* Child runs an echo server: read frame, write frame back, until EOF.
 * Parent sends a real JSON-RPC `initialize` request, reads the echoed
 * frame, parses it, asserts every field round-tripped. */

static int run_echo_child(void) {
    /* Child uses inherited stdin/stdout — those have already been
     * dup2()'d to the pipes by the parent before exec. */
    cmcp_transport_t *t = cmcp_transport_stdio_new();
    if (!t) return 1;
    for (;;) {
        char *buf = NULL; size_t len = 0;
        int rc = cmcp_transport_read(t, &buf, &len);
        if (rc != CMCP_OK) break;
        cmcp_transport_write(t, buf, len);
        free(buf);
    }
    cmcp_transport_close(t);
    return 0;
}

static void test_fork_echo_handshake(void) {
    int p_to_c[2], c_to_p[2];
    TEST_ASSERT(pipe(p_to_c) == 0);
    TEST_ASSERT(pipe(c_to_p) == 0);

    pid_t pid = fork();
    TEST_ASSERT(pid >= 0);

    if (pid == 0) {
        /* Child: wire pipes to stdin/stdout, drop other ends. */
        dup2(p_to_c[0], STDIN_FILENO);
        dup2(c_to_p[1], STDOUT_FILENO);
        close(p_to_c[0]); close(p_to_c[1]);
        close(c_to_p[0]); close(c_to_p[1]);
        _exit(run_echo_child());
    }

    /* Parent: keep the ends we need; close the child's ends. */
    close(p_to_c[0]);
    close(c_to_p[1]);
    cmcp_transport_t *t = cmcp_transport_stdio_new_fds(c_to_p[0], p_to_c[1]);
    TEST_ASSERT(t != NULL);

    /* Build a real MCP `initialize` request via the RPC layer. */
    cmcp_json_t *params = cmcp_json_new_object();
    cmcp_json_object_set(params, "protocolVersion",
                         cmcp_json_new_string(CMCP_PROTOCOL_VERSION));
    cmcp_json_object_set(params, "capabilities", cmcp_json_new_object());
    cmcp_json_t *ci = cmcp_json_new_object();
    cmcp_json_object_set(ci, "name", cmcp_json_new_string("openclawd"));
    cmcp_json_object_set(ci, "version", cmcp_json_new_string("0.0.1"));
    cmcp_json_object_set(params, "clientInfo", ci);

    cmcp_rpc_message_t req;
    cmcp_rpc_make_request(&req, 1, "initialize", params);
    char *wire = cmcp_rpc_emit(&req);
    TEST_ASSERT(wire != NULL);

    /* Send it, read the echoed frame back, parse, compare. */
    TEST_ASSERT(cmcp_transport_write(t, wire, strlen(wire)) == CMCP_OK);

    char *got = NULL; size_t got_len = 0;
    TEST_ASSERT(cmcp_transport_read(t, &got, &got_len) == CMCP_OK);

    cmcp_rpc_message_t *parsed = NULL; size_t n = 0;
    TEST_ASSERT(cmcp_rpc_parse(got, got_len, &parsed, &n) == CMCP_OK);
    TEST_ASSERT(n == 1);
    TEST_ASSERT(parsed[0].kind == CMCP_MSG_REQUEST);
    TEST_ASSERT(parsed[0].id.kind == CMCP_ID_INT && parsed[0].id.i == 1);
    TEST_ASSERT(strcmp(parsed[0].method, "initialize") == 0);
    const cmcp_json_t *pv = cmcp_json_object_get(parsed[0].params,
                                                  "protocolVersion");
    TEST_ASSERT(pv && strcmp(pv->str.s, CMCP_PROTOCOL_VERSION) == 0);
    const cmcp_json_t *cci = cmcp_json_object_get(parsed[0].params,
                                                   "clientInfo");
    TEST_ASSERT(cci != NULL);
    const cmcp_json_t *nm = cmcp_json_object_get(cci, "name");
    TEST_ASSERT(nm && strcmp(nm->str.s, "openclawd") == 0);

    free(got);
    free(wire);
    cmcp_rpc_messages_free(parsed, n);
    cmcp_rpc_message_clear(&req);

    /* Closing the parent's transport hangs up the child's stdin → it
     * sees EOF and exits cleanly. */
    cmcp_transport_close(t);

    int status = 0;
    TEST_ASSERT(waitpid(pid, &status, 0) == pid);
    TEST_ASSERT(WIFEXITED(status));
    TEST_ASSERT(WEXITSTATUS(status) == 0);
}

/* ====================================================================== */

int main(void) {
    fprintf(stderr, "test_stdio_roundtrip:\n");

    TEST_RUN(test_single_frame);
    TEST_RUN(test_many_frames);
    TEST_RUN(test_blank_lines_skipped);
    TEST_RUN(test_reject_embedded_newline);
    TEST_RUN(test_read_after_writer_closes);
    TEST_RUN(test_concurrent_writers);
    TEST_RUN(test_fork_echo_handshake);

    TEST_DONE();
}
