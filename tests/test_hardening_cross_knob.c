/* F.3 — cross-knob hostile-peer pass for the HTTP-server hardening knobs.
 *
 * Each hardening knob was already tested in isolation (test_http_server.c:
 * origin allow-list, SSE replay ring, idle/deadline slowloris, accept-rate
 * token bucket; test_http_auth.c + test_logging.c: the credential
 * redactor; test_json.c: the JSON depth/element caps). What nobody had
 * verified is that *combinations* don't open a seam — e.g. that the SSE
 * replay ring can't resurface a secret the redactor scrubbed, or that the
 * accept-rate gate plus the per-request deadline don't wedge the
 * single-acceptor server. This file is that cross-knob pass.
 *
 * Knob-snapshot timing matters for how a case is structured:
 *   - HTTP knobs (origin, idle, deadline, accept-rate, replay-ring) are
 *     snapshotted per `cmcp_transport_http_listen`, so a fresh listener
 *     re-reads the env — combinable freely across cases in one process.
 *   - CMCP_LOG_REDACT is snapshotted once per process via pthread_once.
 *     The redact-OFF control therefore runs in a forked child so its
 *     snapshot can't bleed into (or be pre-empted by) the default-ON case.
 *
 * No libcurl here — raw HTTP/1.1 over socket(), keeping the wire honest. */

#define _POSIX_C_SOURCE 200809L

#include "test.h"
#include "cmcp.h"
#include "cmcp_json.h"
#include "cmcp_server.h"
#include "cmcp_transport.h"
#include "cmcp_types.h"

#include <poll.h>
#include <signal.h>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

/* The secret that must never survive the redactor onto the SSE wire. */
#define SECRET "hunter2-DEADBEEF-sekret"

/* ====================================================================== */
/* Server harness (mirrors test_http_server.c; one harness per test file) */
/* ====================================================================== */

typedef struct {
    cmcp_server_t    *s;
    cmcp_transport_t *t;
    int               rc;
} server_arg_t;

static void *server_thread_main(void *arg) {
    server_arg_t *a = (server_arg_t *)arg;
    a->rc = cmcp_server_run(a->s, a->t);
    return NULL;
}

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
    if (bind(fd, (struct sockaddr *)&sa, sizeof sa) != 0) { close(fd); return 0; }
    socklen_t slen = sizeof sa;
    if (getsockname(fd, (struct sockaddr *)&sa, &slen) != 0) { close(fd); return 0; }
    unsigned short p = ntohs(sa.sin_port);
    close(fd);
    return p;
}

static int open_client(unsigned short port) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    struct sockaddr_in sa;
    memset(&sa, 0, sizeof sa);
    sa.sin_family      = AF_INET;
    sa.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    sa.sin_port        = htons(port);
    for (int i = 0; i < 20; i++) {
        if (connect(fd, (struct sockaddr *)&sa, sizeof sa) == 0) return fd;
        struct timespec ts = { 0, 5 * 1000 * 1000 };
        nanosleep(&ts, NULL);
    }
    close(fd);
    return -1;
}

static int send_all(int fd, const char *buf, size_t len) {
    size_t sent = 0;
    while (sent < len) {
        ssize_t n = send(fd, buf + sent, len - sent, 0);
        if (n <= 0) return -1;
        sent += (size_t)n;
    }
    return 0;
}

static int recv_all(int fd, char **out_buf, size_t *out_len) {
    size_t cap = 4096, len = 0;
    char *buf = (char *)malloc(cap);
    if (!buf) return -1;
    for (;;) {
        if (len + 1 >= cap) {
            cap *= 2;
            char *nb = (char *)realloc(buf, cap);
            if (!nb) { free(buf); return -1; }
            buf = nb;
        }
        ssize_t n = recv(fd, buf + len, cap - 1 - len, 0);
        if (n < 0) { free(buf); return -1; }
        if (n == 0) break;
        len += (size_t)n;
    }
    buf[len] = '\0';
    *out_buf = buf;
    *out_len = len;
    return 0;
}

static int extract_header(const char *resp, const char *name,
                          char *out, size_t out_cap) {
    const char *eoh = strstr(resp, "\r\n\r\n");
    if (!eoh) eoh = resp + strlen(resp);
    size_t name_len = strlen(name);
    const char *p = resp;
    while (p < eoh) {
        const char *eol = strstr(p, "\r\n");
        if (!eol || eol > eoh) break;
        if ((size_t)(eol - p) > name_len + 1 &&
            strncasecmp(p, name, name_len) == 0 && p[name_len] == ':') {
            const char *v = p + name_len + 1;
            while (v < eol && (*v == ' ' || *v == '\t')) v++;
            size_t vlen = (size_t)(eol - v);
            if (vlen >= out_cap) vlen = out_cap - 1;
            memcpy(out, v, vlen);
            out[vlen] = '\0';
            return 1;
        }
        p = eol + 2;
    }
    return 0;
}

static char *build_post(const char *body, const char *session_id) {
    size_t body_len = body ? strlen(body) : 0;
    size_t cap = body_len + 512;
    char *out = (char *)malloc(cap);
    if (!out) return NULL;
    int n = snprintf(out, cap,
        "POST /mcp HTTP/1.1\r\n"
        "Host: 127.0.0.1\r\n"
        "Content-Type: application/json\r\n"
        "%s%s%s"
        "Content-Length: %zu\r\n"
        "\r\n"
        "%s",
        session_id ? "Mcp-Session-Id: " : "",
        session_id ? session_id : "",
        session_id ? "\r\n" : "",
        body_len,
        body ? body : "");
    if (n < 0 || (size_t)n >= cap) { free(out); return NULL; }
    return out;
}

static int do_request(unsigned short port, const char *req,
                      char **out_resp, size_t *out_len) {
    int fd = open_client(port);
    if (fd < 0) return -1;
    if (send_all(fd, req, strlen(req)) != 0) { close(fd); return -1; }
    int rc = recv_all(fd, out_resp, out_len);
    close(fd);
    return rc;
}

static const char *INIT_BODY =
    "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"initialize\",\"params\":{"
        "\"protocolVersion\":\"" CMCP_PROTOCOL_VERSION "\","
        "\"capabilities\":{},"
        "\"clientInfo\":{\"name\":\"test\",\"version\":\"0\"}"
    "}}";

/* Read up to cap-1 bytes within timeout_ms, then return. */
static size_t read_window(int fd, char *buf, size_t cap, int timeout_ms) {
    size_t total = 0;
    int    remaining = timeout_ms;
    while (total + 1 < cap && remaining > 0) {
        struct pollfd p = { fd, POLLIN, 0 };
        struct timespec start;
        clock_gettime(CLOCK_MONOTONIC, &start);
        int pr = poll(&p, 1, remaining);
        if (pr <= 0) break;
        ssize_t n = recv(fd, buf + total, cap - 1 - total, 0);
        if (n <= 0) break;
        total += (size_t)n;
        struct timespec end;
        clock_gettime(CLOCK_MONOTONIC, &end);
        int elapsed = (int)((end.tv_sec  - start.tv_sec)  * 1000 +
                            (end.tv_nsec - start.tv_nsec) / 1000000L);
        remaining -= (elapsed > 0 ? elapsed : 1);
    }
    buf[total] = '\0';
    return total;
}

static int handshake_get_sid(unsigned short port, char *out_sid, size_t cap) {
    char *req = build_post(INIT_BODY, NULL);
    if (!req) return -1;
    char *resp = NULL; size_t rn = 0;
    int rc = do_request(port, req, &resp, &rn);
    free(req);
    if (rc != 0) return -1;
    int ok = extract_header(resp, "Mcp-Session-Id", out_sid, cap);
    free(resp);
    return ok ? 0 : -1;
}

/* ====================================================================== */
/* Case 1+2: redactor x SSE replay ring                                    */
/* ====================================================================== */
/* A tool that logs a credential-shaped payload through cmcp_server_log.
 * The redactor (if on) scrubs it inside server.c BEFORE the frame reaches
 * the transport — so whatever lands in the SSE replay ring is already
 * post-redaction. This handler is the source of that one event. */
static int leak_log_handler(const cmcp_json_t *args, void *ud,
                            cmcp_handler_ctx_t *hctx,
                            cmcp_json_t **out_content, int *out_is_error) {
    (void)args; (void)hctx;
    cmcp_server_t *s = (cmcp_server_t *)ud;
    cmcp_json_t *data = cmcp_json_new_object();
    cmcp_json_object_set(data, "password", cmcp_json_new_string(SECRET));
    cmcp_json_object_set(data, "note",     cmcp_json_new_string("benign"));
    cmcp_server_log(s, CMCP_LOG_LEVEL_WARNING, "audit", data);  /* consumes data */
    *out_content  = cmcp_tool_text_content("logged");
    *out_is_error = 0;
    return CMCP_OK;
}

/* Run the full flow: spin a server, handshake, fire the logging tool (one
 * notifications/message recorded into the replay ring at id 1), then GET
 * the SSE stream with Last-Event-Id: 0 to force a replay. The replayed
 * bytes are copied into out_window. Returns 0 on a clean run. */
static int run_redact_flow(char *out_window, size_t cap) {
    out_window[0] = '\0';
    unsigned short port = pick_port();
    if (port == 0) return -1;

    cmcp_transport_t *t = cmcp_transport_http_listen("127.0.0.1", port);
    if (!t) return -1;
    cmcp_server_t *s = cmcp_server_new("redact-replay", "0.1.0");
    cmcp_server_set_capabilities(s, &(cmcp_server_capabilities_t){ .logging = 1 });
    cmcp_server_add_tool(s, &(cmcp_tool_t){
        .name = "leak", .handler = leak_log_handler, .userdata = s,
    });

    server_arg_t sa = { s, t, 0 };
    pthread_t th;
    if (pthread_create(&th, NULL, server_thread_main, &sa) != 0) {
        cmcp_transport_close(t); cmcp_server_free(s); return -1;
    }

    int ret = -1;
    char sid[64] = {0};
    if (handshake_get_sid(port, sid, sizeof sid) != 0) goto done;

    /* notifications/initialized → server READY. */
    {
        const char *body =
            "{\"jsonrpc\":\"2.0\",\"method\":\"notifications/initialized\"}";
        char *req = build_post(body, sid);
        char *resp = NULL; size_t rn = 0;
        if (do_request(port, req, &resp, &rn) != 0) { free(req); goto done; }
        free(req); free(resp);
    }

    /* Fire the logging tool BEFORE opening any SSE holder. The event lands
     * in the replay ring (no live holder yet); it is replayed on resume. */
    {
        const char *body =
            "{\"jsonrpc\":\"2.0\",\"id\":2,\"method\":\"tools/call\","
            "\"params\":{\"name\":\"leak\",\"arguments\":{}}}";
        char *req = build_post(body, sid);
        char *resp = NULL; size_t rn = 0;
        if (do_request(port, req, &resp, &rn) != 0) { free(req); goto done; }
        free(req); free(resp);
    }

    /* GET /mcp with Last-Event-Id: 0 → replay everything with id > 0. */
    {
        char get[512];
        snprintf(get, sizeof get,
            "GET /mcp HTTP/1.1\r\n"
            "Host: 127.0.0.1\r\n"
            "Accept: text/event-stream\r\n"
            "Mcp-Session-Id: %s\r\n"
            "Last-Event-Id: 0\r\n"
            "\r\n", sid);
        int fd = open_client(port);
        if (fd < 0) goto done;
        if (send_all(fd, get, strlen(get)) != 0) { close(fd); goto done; }
        read_window(fd, out_window, cap, 600);
        close(fd);
    }
    ret = 0;

done:
    cmcp_transport_wake(t);
    pthread_join(th, NULL);
    cmcp_transport_close(t);
    cmcp_server_free(s);
    return ret;
}

/* Run the redact flow in THIS process under the requested knob setting and
 * check the replay window against the expectation. Both ON and OFF go
 * through a forked child (see the two tests below) precisely because
 * CMCP_LOG_REDACT is snapshotted once-per-process: if the parent ever
 * logged first, every child would inherit that completed snapshot and the
 * child's setenv would be a no-op. The parent main never logs, so each
 * child gets a clean, independent snapshot. Returns a child exit code:
 * 0 = expectation met; non-zero codes distinguish failure modes. */
static int redact_child(int redact_on) {
    if (redact_on) unsetenv("CMCP_LOG_REDACT");   /* default == on */
    else           setenv("CMCP_LOG_REDACT", "0", 1);

    char window[16384];
    if (run_redact_flow(window, sizeof window) != 0) return 2;  /* flow error */

    int has_secret   = strstr(window, SECRET)                  != NULL;
    int has_redacted = strstr(window, "[REDACTED]")            != NULL;
    int has_notif    = strstr(window, "notifications/message") != NULL;
    int has_200      = strstr(window, "HTTP/1.1 200")          != NULL;
    if (!has_200 || !has_notif) return 3;   /* event never reached the ring */

    if (redact_on)
        return (!has_secret &&  has_redacted) ? 0 : 4;  /* must be scrubbed */
    else
        return ( has_secret && !has_redacted) ? 0 : 5;  /* control: leaks */
}

/* Case 1: redactor ON (default). The replay ring must serve the scrubbed
 * form — the secret gone, "[REDACTED]" present. This is the headline seam:
 * a late SSE resumer cannot pull a credential the log pipeline already
 * redacted, because redaction happens upstream of the transport that owns
 * the ring. Forked so its CMCP_LOG_REDACT snapshot is isolated. */
static void test_redact_on_x_replay_scrubs(void) {
    pid_t pid = fork();
    TEST_ASSERT(pid >= 0);
    if (pid == 0) _exit(redact_child(1));
    int status = 0;
    TEST_ASSERT(waitpid(pid, &status, 0) == pid);
    TEST_ASSERT(WIFEXITED(status));
    TEST_ASSERT(WEXITSTATUS(status) == 0);   /* 0 = scrubbed, no secret */
}

/* Case 2: redactor OFF — the control. With the knob off the secret DOES
 * reach the replay ring, proving (a) the gating in Case 1 is the redactor
 * and not some unrelated SSE-side filter, and (b) the env knob is
 * load-bearing. Forked so its OFF snapshot can't bleed into Case 1 (and
 * isn't pre-empted by it). */
static void test_redact_off_x_replay_leaks_control(void) {
    pid_t pid = fork();
    TEST_ASSERT(pid >= 0);
    if (pid == 0) _exit(redact_child(0));
    int status = 0;
    TEST_ASSERT(waitpid(pid, &status, 0) == pid);
    TEST_ASSERT(WIFEXITED(status));
    TEST_ASSERT(WEXITSTATUS(status) == 0);   /* 0 = secret leaked as expected */
}

/* ====================================================================== */
/* Case 3: accept-rate token bucket x per-request deadline                 */
/* ====================================================================== */
/* Both gates active at once. A storm of connections — some dribbling
 * partial headers (slowloris), some fast junk — must NOT wedge the
 * single-acceptor server: the rate gate sheds surplus connections (503)
 * and the deadline reaps the dribblers (408), but neither leaves the
 * acceptor stuck. The load-bearing assertion is liveness: once the token
 * bucket refills, a well-formed handshake still completes with 200. */
static void test_accept_rate_x_deadline_liveness(void) {
    setenv("CMCP_HTTP_ACCEPT_RATE",     "2",   1);
    setenv("CMCP_HTTP_ACCEPT_BURST",    "2",   1);
    setenv("CMCP_HTTP_IDLE_TIMEOUT_MS", "300", 1);
    setenv("CMCP_HTTP_DEADLINE_MS",     "400", 1);

    unsigned short port = pick_port();
    TEST_ASSERT(port != 0);

    cmcp_transport_t *t = cmcp_transport_http_listen("127.0.0.1", port);
    TEST_ASSERT(t != NULL);
    cmcp_server_t *s = cmcp_server_new("rate-x-deadline", "0.1.0");
    server_arg_t sa = { s, t, 0 };
    pthread_t th;
    TEST_ASSERT(pthread_create(&th, NULL, server_thread_main, &sa) == 0);

    /* Storm: 12 connections in quick succession. Even-indexed peers send
     * a complete junk request line (the parser/acceptor answers fast);
     * odd-indexed peers dribble a partial header and stall (deadline bait).
     * We only need to confirm the gates fire and nothing hangs. */
    int responded = 0;
    for (int i = 0; i < 12; i++) {
        int fd = open_client(port);
        if (fd < 0) continue;
        if (i % 2 == 0) {
            const char *junk = "GET / HTTP/1.1\r\nHost: x\r\n\r\n";
            send_all(fd, junk, strlen(junk));
        } else {
            const char *partial = "POST /mcp HTTP/1.1\r\nHost: x\r\n";
            send_all(fd, partial, strlen(partial));  /* never terminated */
        }
        char buf[256];
        struct pollfd p = { fd, POLLIN, 0 };
        if (poll(&p, 1, 700) > 0) {
            ssize_t n = recv(fd, buf, sizeof buf - 1, 0);
            if (n > 0) responded++;   /* got SOME status (503/408/400) */
        }
        close(fd);
    }
    /* At least some connections drew a response — the gates are live and
     * the acceptor kept servicing connections through the storm. */
    TEST_ASSERT(responded >= 1);

    /* Liveness: wait for the token bucket to refill (rate 2/sec), then a
     * clean handshake must succeed. Retry across a bounded window so the
     * assertion isn't hostage to exact refill timing. */
    int got_200 = 0;
    for (int attempt = 0; attempt < 8 && !got_200; attempt++) {
        struct timespec ts = { 0, 400 * 1000 * 1000 };  /* 400ms */
        nanosleep(&ts, NULL);
        char *req = build_post(INIT_BODY, NULL);
        char *resp = NULL; size_t rn = 0;
        if (do_request(port, req, &resp, &rn) == 0 && resp) {
            if (strncmp(resp, "HTTP/1.1 200", 12) == 0) got_200 = 1;
        }
        free(req); free(resp);
    }
    TEST_ASSERT(got_200 == 1);

    cmcp_transport_wake(t);
    pthread_join(th, NULL);
    cmcp_transport_close(t);
    cmcp_server_free(s);
    unsetenv("CMCP_HTTP_ACCEPT_RATE");
    unsetenv("CMCP_HTTP_ACCEPT_BURST");
    unsetenv("CMCP_HTTP_IDLE_TIMEOUT_MS");
    unsetenv("CMCP_HTTP_DEADLINE_MS");
}

/* ====================================================================== */
/* Case 4: JSON depth cap x active idle timeout                            */
/* ====================================================================== */
/* A JSON depth-bomb (nesting well past the default CMCP_JSON_MAX_DEPTH of
 * 64) arrives on a connection that is also under an active idle timeout.
 * The parser cap must trip — bounding recursion — so the body is rejected
 * rather than dispatched, and the server must stay alive for the next
 * client. Verifies the parser cap composes with the connection-level
 * timeouts: the bomb is handled inside the request cycle, not wedged. */
static void test_json_depth_bomb_x_idle_timeout(void) {
    setenv("CMCP_HTTP_IDLE_TIMEOUT_MS", "500", 1);

    unsigned short port = pick_port();
    TEST_ASSERT(port != 0);

    cmcp_transport_t *t = cmcp_transport_http_listen("127.0.0.1", port);
    TEST_ASSERT(t != NULL);
    cmcp_server_t *s = cmcp_server_new("depth-x-idle", "0.1.0");
    server_arg_t sa = { s, t, 0 };
    pthread_t th;
    TEST_ASSERT(pthread_create(&th, NULL, server_thread_main, &sa) == 0);

    /* Build a 200-deep nested-array body: "[[[ ... ]]]". Past the default
     * depth cap of 64, the parser returns NULL → the server cannot treat
     * this as a successful call. */
    enum { DEPTH = 200 };
    char body[2 * DEPTH + 1];
    for (int i = 0; i < DEPTH; i++)        body[i]         = '[';
    for (int i = 0; i < DEPTH; i++)        body[DEPTH + i] = ']';
    body[2 * DEPTH] = '\0';

    char *req = build_post(body, NULL);
    TEST_ASSERT(req != NULL);
    char *resp = NULL; size_t rn = 0;
    int rc = do_request(port, req, &resp, &rn);
    free(req);
    /* The server must respond (not hang/crash) ... */
    TEST_ASSERT(rc == 0);
    TEST_ASSERT(resp != NULL && rn > 0);
    /* ... and it must NOT have processed the bomb as a successful call.
     * Whether the rejection surfaces as HTTP 400 or HTTP 200 + a JSON-RPC
     * error, there must be no `"result"` object on the wire. */
    if (resp) TEST_ASSERT(strstr(resp, "\"result\"") == NULL);
    free(resp);

    /* Liveness: a well-formed handshake on a fresh connection still works. */
    char sid[64] = {0};
    TEST_ASSERT(handshake_get_sid(port, sid, sizeof sid) == 0);
    TEST_ASSERT(sid[0] != '\0');

    cmcp_transport_wake(t);
    pthread_join(th, NULL);
    cmcp_transport_close(t);
    cmcp_server_free(s);
    unsetenv("CMCP_HTTP_IDLE_TIMEOUT_MS");
}

/* ====================================================================== */

int main(void) {
    /* The storm/slowloris cases write to sockets the server is closing
     * from its end; ignore SIGPIPE so a raced send() can't kill us. */
    signal(SIGPIPE, SIG_IGN);

    fprintf(stderr, "test_hardening_cross_knob:\n");
    TEST_RUN(test_redact_on_x_replay_scrubs);
    TEST_RUN(test_redact_off_x_replay_leaks_control);
    TEST_RUN(test_accept_rate_x_deadline_liveness);
    TEST_RUN(test_json_depth_bomb_x_idle_timeout);
    TEST_DONE();
}
