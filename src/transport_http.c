/* Streamable HTTP transport — server side.
 *
 * Wire shape (MCP 2025-11-25):
 *   POST /mcp    Content-Type: application/json
 *                Body: one JSON-RPC frame (request, response, or
 *                       notification)
 *                Reply: 200 + JSON-RPC response body OR 202 Accepted
 *                       (for notifications/responses-only-no-body)
 *
 *   GET /mcp     Accept: text/event-stream
 *                Reply: 200 + SSE stream that the server uses to push
 *                       server-to-client messages until the client
 *                       disconnects.
 *
 * Session: every interaction after the first `initialize` POST must
 * carry an `Mcp-Session-Id` header matching the one returned in that
 * POST's response. v0.2 supports exactly one logical session per
 * transport; concurrent sessions need separate transports.
 *
 * Threading:
 *   - One acceptor thread (created in _listen, joined in close).
 *   - Each accepted connection is handled inline on the acceptor:
 *     POST handlers serialize on a single "pending request" slot that
 *     bridges to the cmcp_transport_t read/write vtable; SSE handlers
 *     detach to per-connection holder threads.
 *
 * Bridging onto cmcp_transport_t:
 *   The server.c run loop is read → dispatch → write in lockstep, so
 *   we only ever have one outstanding request at a time. The acceptor
 *   pushes a POST body into a single slot, signals the read condvar,
 *   then waits on the write condvar for the response, then writes the
 *   HTTP response and closes the conn. Notifications and async writes
 *   from the server side route to the active SSE stream (Phase 2.4
 *   adds the actual emit API; Phase 2.1 just keeps the SSE conn
 *   alive). */

/* getaddrinfo, strcasecmp, struct sigaction, accept4 if available. */
#define _POSIX_C_SOURCE 200809L
#define _DEFAULT_SOURCE

#include "cmcp.h"
#include "cmcp_http_parser.h"
#include "cmcp_json.h"
#include "cmcp_transport.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

/* ====================================================================== */
/* Constants & limits                                                       */
/* ====================================================================== */

#define HTTP_MAX_HEADERS_BYTES   ((size_t)16 * 1024)         /* 16 KiB request line + headers */
#define HTTP_MAX_BODY_BYTES      ((size_t)4 * 1024 * 1024)   /* 4 MiB body */

/* File-local sentinel for "Content-Length exceeds HTTP_MAX_BODY_BYTES".
 * Distinct from every cmcp_err_t value (those are 0..-13) so the caller
 * can map it to `413 Payload Too Large` rather than folding it into the
 * generic `400 Bad Request` path — matches threat-model.md row 1.2. */
#define HTTP_ETOOLARGE  (-200)
#define HTTP_LISTEN_BACKLOG      16
#define HTTP_ACCEPT_POLL_MS      250            /* shutdown polling cadence */
#define HTTP_SSE_REPLAY_DEFAULT  256            /* events kept for resumption */
#define HTTP_SSE_REPLAY_MAX      65536          /* env-set ceiling */

/* Slowloris / per-connection-budget defaults (Tier 6 axis 6.5.1). The
 * idle timeout caps any single recv() wait — a slow-write peer that
 * dribbles bytes can't hold a worker indefinitely. The deadline caps
 * the full request-reception window: header block + body read combined.
 * Both are env-tunable; 0 (or negative) disables. */
#define HTTP_IDLE_TIMEOUT_DEFAULT_MS  30000     /* per-recv wait */
#define HTTP_DEADLINE_DEFAULT_MS      120000    /* whole request-receive wall clock */

/* Accept-rate token bucket (Tier 6 axis 6.5.2). Bounds the rate at
 * which fresh connections enter the listen-accept cycle so a connection
 * flood can't exhaust the listen backlog or saturate the acceptor.
 * Defaults: 100 connections per second sustained, 200 burst capacity
 * (~ a 2-second tolerable spike). Both env-tunable; rate <= 0 disables. */
#define HTTP_ACCEPT_RATE_DEFAULT      100.0     /* tokens per second */
#define HTTP_ACCEPT_BURST_DEFAULT     200.0     /* max bucket capacity */

/* Wake signal for SSE holder threads so close() can join them. */
#ifndef CMCP_HTTP_WAKE_SIGNAL
#define CMCP_HTTP_WAKE_SIGNAL  SIGUSR2
#endif

/* ====================================================================== */
/* Tiny string utilities                                                    */
/* ====================================================================== */

/* ====================================================================== */
/* HTTP request                                                             */
/* ====================================================================== */

/* Parser types + entry point live in cmcp_http_parser.h. Aliases keep
 * the rest of this file readable without churn. */
typedef cmcp_http_request_t http_request_t;
#define http_request_clear cmcp_http_request_clear
#define http_header_get    cmcp_http_header_get

/* ====================================================================== */
/* Per-connection read budget (Tier 6 axis 6.5.1)                          */
/* ====================================================================== */

/* Threaded through every recv() inside one request-receive cycle. The
 * idle limit caps any one poll() wait; the deadline caps the cumulative
 * wall-clock budget for the whole request. */
typedef struct {
    int idle_ms;        /* per-recv timeout; <= 0 means no idle cap */
    int deadline_ms;    /* remaining whole-request budget; < 0 means none */
} conn_budget_t;

/* Wait until fd is readable (subject to budget), then recv. Same return
 * shape as recv() but with a dedicated timeout sentinel instead of an
 * errno contract: >0 bytes, 0 peer-closed, BUDGET_TIMEOUT (idle/deadline
 * expiry → caller maps to `408 Request Timeout`), BUDGET_ERR (any other
 * hard error). Returning the timeout via the value, not errno, means
 * callers never read errno across this wrapper — which was fragile (a
 * future -1 path that forgot to set errno would read a stale value) and
 * which the static analyzer (rightly) could not prove safe. Updates the
 * deadline in-place. */
#define BUDGET_ERR     ((ssize_t)-1)
#define BUDGET_TIMEOUT ((ssize_t)-2)
static ssize_t budgeted_recv(int fd, void *buf, size_t n,
                              conn_budget_t *b) {
    int wait_ms = b->idle_ms > 0 ? b->idle_ms : -1;
    if (b->deadline_ms >= 0) {
        if (wait_ms < 0 || b->deadline_ms < wait_ms) wait_ms = b->deadline_ms;
    }

    struct timespec t0;
    clock_gettime(CLOCK_MONOTONIC, &t0);

    struct pollfd p = { fd, POLLIN, 0 };
    int rv = poll(&p, 1, wait_ms);

    struct timespec t1;
    clock_gettime(CLOCK_MONOTONIC, &t1);
    long elapsed_ms = (long)(t1.tv_sec  - t0.tv_sec)  * 1000 +
                      (long)(t1.tv_nsec - t0.tv_nsec) / 1000000;
    if (b->deadline_ms >= 0) {
        b->deadline_ms -= (int)elapsed_ms;
        if (b->deadline_ms < 0) b->deadline_ms = 0;
    }

    if (rv < 0) {
        /* EINTR loops at the caller's discretion; everything else is a
         * hard error. */
        return BUDGET_ERR;
    }
    if (rv == 0) {
        return BUDGET_TIMEOUT;
    }
    if (p.revents & (POLLERR | POLLHUP | POLLNVAL)) {
        /* Peer error / hangup before any bytes — treat as clean close
         * so the caller distinguishes from "transport corrupted." */
        return 0;
    }
    ssize_t r = recv(fd, buf, n, 0);
    return r < 0 ? BUDGET_ERR : r;
}

/* ====================================================================== */
/* Read until \r\n\r\n or limit reached. Caller frees *out_buf.            */
/* Returns CMCP_OK on success, CMCP_EIO on socket error / EOF before       */
/* end of headers, CMCP_EPROTOCOL on header section overflow,              */
/* CMCP_ETIMEOUT when the idle / deadline budget expires.                  */
/* ====================================================================== */

static int read_headers_block(int fd, conn_budget_t *budget,
                               char **out_buf, size_t *out_len,
                               size_t *out_body_offset) {
    size_t cap = 4096, len = 0, scanned = 0;
    char *buf = (char *)malloc(cap);
    if (!buf) return CMCP_ENOMEM;

    for (;;) {
        if (len + 1 >= cap) {
            if (cap >= HTTP_MAX_HEADERS_BYTES) { free(buf); return CMCP_EPROTOCOL; }
            size_t new_cap = cap * 2;
            if (new_cap > HTTP_MAX_HEADERS_BYTES) new_cap = HTTP_MAX_HEADERS_BYTES;
            char *nb = (char *)realloc(buf, new_cap);
            if (!nb) { free(buf); return CMCP_ENOMEM; }
            buf = nb;
            cap = new_cap;
        }
        ssize_t n = budgeted_recv(fd, buf + len, cap - 1 - len, budget);
        if (n < 0) {
            int rc = (n == BUDGET_TIMEOUT) ? CMCP_ETIMEOUT : CMCP_EIO;
            free(buf); return rc;
        }
        if (n == 0) { free(buf); return CMCP_EIO; }
        len += (size_t)n;
        buf[len] = '\0';

        /* Scan only the freshly arrived bytes, overlapping the previous
         * tail by 3 so a terminator split across two recv() chunks isn't
         * missed (3 = max(len("\r\n\r\n"), len("\n\n")) - 1). The header
         * block is capped at HTTP_MAX_HEADERS_BYTES, so this is about not
         * rescanning settled bytes, not a scaling win. */
        size_t from = (scanned > 3) ? scanned - 3 : 0;

        /* Look for terminator. */
        char *eoh = strstr(buf + from, "\r\n\r\n");
        if (eoh) {
            *out_buf = buf;
            *out_len = len;
            *out_body_offset = (size_t)(eoh - buf) + 4;
            return CMCP_OK;
        }
        /* Also tolerate bare-LF terminators (\n\n) — some test clients
         * use them. */
        char *eoh2 = strstr(buf + from, "\n\n");
        if (eoh2) {
            *out_buf = buf;
            *out_len = len;
            *out_body_offset = (size_t)(eoh2 - buf) + 2;
            return CMCP_OK;
        }
        scanned = len;
    }
}

/* Read exactly `len` bytes into a fresh buffer (NUL-terminated). */
static int read_exact(int fd, conn_budget_t *budget, size_t len,
                       char **out, char *prefix, size_t prefix_len) {
    char *buf = (char *)malloc(len + 1);
    if (!buf) return CMCP_ENOMEM;
    size_t have = 0;
    if (prefix_len) {
        size_t take = prefix_len > len ? len : prefix_len;
        memcpy(buf, prefix, take);
        have = take;
    }
    while (have < len) {
        ssize_t n = budgeted_recv(fd, buf + have, len - have, budget);
        if (n < 0) {
            int rc = (n == BUDGET_TIMEOUT) ? CMCP_ETIMEOUT : CMCP_EIO;
            free(buf); return rc;
        }
        if (n == 0) { free(buf); return CMCP_EIO; }
        have += (size_t)n;
    }
    buf[len] = '\0';
    *out = buf;
    return CMCP_OK;
}

/* Read one full HTTP request from fd. */
static int http_read_request(int fd, conn_budget_t *budget,
                              http_request_t *out) {
    char *block = NULL; size_t block_len = 0, body_off = 0;
    int rc = read_headers_block(fd, budget, &block, &block_len, &body_off);
    if (rc != CMCP_OK) return rc;

    rc = cmcp_http_parse_head(block, body_off, out);
    if (rc != CMCP_OK) { free(block); http_request_clear(out); return rc; }

    /* Reject Transfer-Encoding: chunked — Content-Length only in v0.2. */
    const char *te = http_header_get(out, "Transfer-Encoding");
    if (te && strcasecmp(te, "identity") != 0) {
        free(block); http_request_clear(out); return CMCP_EUNSUPPORTED;
    }

    const char *cl = http_header_get(out, "Content-Length");
    size_t body_len = 0;
    if (cl) {
        char *end = NULL;
        long long v = strtoll(cl, &end, 10);
        if (v < 0 || end == cl || *end != '\0') {
            free(block); http_request_clear(out); return CMCP_EPROTOCOL;
        }
        if ((size_t)v > HTTP_MAX_BODY_BYTES) {
            free(block); http_request_clear(out); return HTTP_ETOOLARGE;
        }
        body_len = (size_t)v;
    }

    /* Anything past body_off is the start of the body. */
    char *prefix = block + body_off;
    size_t prefix_len = block_len - body_off;

    if (body_len == 0) {
        out->body = NULL;
        out->body_len = 0;
    } else {
        char *body = NULL;
        rc = read_exact(fd, budget, body_len, &body, prefix, prefix_len);
        if (rc != CMCP_OK) {
            free(block); http_request_clear(out); return rc;
        }
        out->body = body;
        out->body_len = body_len;
    }
    free(block);
    return CMCP_OK;
}

/* ====================================================================== */
/* HTTP response writer                                                     */
/* ====================================================================== */

static int send_all(int fd, const char *buf, size_t len) {
    size_t sent = 0;
    while (sent < len) {
        ssize_t n = send(fd, buf + sent, len - sent, MSG_NOSIGNAL);
        if (n <= 0) return CMCP_EIO;
        sent += (size_t)n;
    }
    return CMCP_OK;
}

/* Send a plain HTTP/1.1 response with optional body. body may be NULL
 * with body_len 0. Adds Content-Length, Connection: close, and any
 * extra headers passed in extra_headers (one trailing \r\n required
 * per header line, or NULL). */
static int http_write_response(int fd, int status, const char *reason,
                                 const char *content_type,
                                 const char *extra_headers,
                                 const char *body, size_t body_len) {
    char head[1024];
    int n = snprintf(head, sizeof head,
        "HTTP/1.1 %d %s\r\n"
        "Content-Type: %s\r\n"
        "Content-Length: %zu\r\n"
        "Connection: close\r\n"
        "%s"
        "\r\n",
        status, reason ? reason : "",
        content_type ? content_type : "application/octet-stream",
        body_len,
        extra_headers ? extra_headers : "");
    if (n < 0 || (size_t)n >= sizeof head) return CMCP_EIO;
    if (send_all(fd, head, (size_t)n) != CMCP_OK) return CMCP_EIO;
    if (body_len > 0 && send_all(fd, body, body_len) != CMCP_OK)
        return CMCP_EIO;
    return CMCP_OK;
}

/* Send the SSE response head (no Content-Length; stream stays open). */
static int http_write_sse_head(int fd, const char *session_id) {
    char head[512];
    int n = snprintf(head, sizeof head,
        "HTTP/1.1 200 OK\r\n"
        "Content-Type: text/event-stream\r\n"
        "Cache-Control: no-cache\r\n"
        "Connection: close\r\n"
        "%s%s%s"
        "\r\n",
        session_id ? "Mcp-Session-Id: " : "",
        session_id ? session_id : "",
        session_id ? "\r\n" : "");
    if (n < 0 || (size_t)n >= sizeof head) return CMCP_EIO;
    return send_all(fd, head, (size_t)n);
}

/* ====================================================================== */
/* Session id minting                                                       */
/* ====================================================================== */

/* 16 random hex pairs joined with hyphens, UUID-shaped without claiming
 * to be a real RFC 4122 UUID. Reads from /dev/urandom; falls back to a
 * time-based scramble. */
static void mint_session_id(char out[37]) {
    unsigned char raw[16];
    int got = 0;
    int fd = open("/dev/urandom", O_RDONLY | O_CLOEXEC);
    if (fd >= 0) {
        /* /dev/urandom never blocks once the kernel has seeded its CSPRNG
         * (effectively always, on any system that has finished boot). The
         * surrounding lock protects the session table; the few microseconds
         * a 16-byte read costs aren't worth refactoring the mint path to
         * happen outside the lock. */
        ssize_t n = read(fd, raw, sizeof raw);  // NOLINT(clang-analyzer-unix.BlockInCriticalSection)
        if (n == (ssize_t)sizeof raw) got = 1;
        close(fd);
    }
    if (!got) {
        struct timespec ts;
        clock_gettime(CLOCK_REALTIME, &ts);
        unsigned long long mix =
            ((unsigned long long)ts.tv_sec * 1000000000ull
              + (unsigned long long)ts.tv_nsec)
            ^ (unsigned long long)(uintptr_t)&out;
        for (size_t i = 0; i < sizeof raw; i++) {
            mix = mix * 6364136223846793005ull + 1442695040888963407ull;
            raw[i] = (unsigned char)(mix >> 56);
        }
    }
    /* RFC 4122-ish version 4 marker, just so the id "looks right." */
    raw[6] = (unsigned char)((raw[6] & 0x0F) | 0x40);
    raw[8] = (unsigned char)((raw[8] & 0x3F) | 0x80);
    snprintf(out, 37,
        "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
        raw[0], raw[1], raw[2], raw[3],
        raw[4], raw[5], raw[6], raw[7],
        raw[8], raw[9], raw[10], raw[11],
        raw[12], raw[13], raw[14], raw[15]);
}

/* Is the address currently bound to `fd` a loopback address? Used only
 * to decide whether to warn about a LAN-exposed bind. getsockname after a
 * successful bind+listen can't fail for our purposes; on the off chance it
 * does, we treat the result as loopback (no warning) — failing safe toward
 * silence rather than a spurious scare. Covers both IPv4 (127.0.0.0/8) and
 * IPv6 (::1), so checking the *actual* bound address sidesteps the
 * NULL→string-substitution IPv6 footgun entirely. */
static int bound_addr_is_loopback(int fd) {
    struct sockaddr_storage ss;
    socklen_t slen = sizeof ss;
    if (getsockname(fd, (struct sockaddr *)&ss, &slen) != 0) return 1;
    if (ss.ss_family == AF_INET) {
        const struct sockaddr_in *s4 = (const struct sockaddr_in *)&ss;
        return (ntohl(s4->sin_addr.s_addr) >> 24) == 127;
    }
    if (ss.ss_family == AF_INET6) {
        const struct sockaddr_in6 *s6 = (const struct sockaddr_in6 *)&ss;
        return IN6_IS_ADDR_LOOPBACK(&s6->sin6_addr);
    }
    return 0;
}

/* ====================================================================== */
/* Transport impl                                                           */
/* ====================================================================== */

typedef struct sse_conn {
    int                       fd;
    pthread_t                 thread;
    struct sse_conn          *next;
    struct http_impl         *owner;
} sse_conn_t;

typedef struct http_impl {
    int                listen_fd;

    pthread_t          acceptor;
    int                acceptor_started;
    /* Atomic flag — read from the per-SSE-connection holder threads
     * without our mutex (line 487 below), so it needs real cross-
     * thread visibility, not just the volatile compiler hint. The
     * mutex-guarded read sites use atomic_load too for consistency. */
    atomic_int         shutting_down;

    /* Single-session state. session_id is empty until the first
     * `initialize` POST mints it. After that, all requests must
     * carry a matching id. */
    pthread_mutex_t    session_mu;
    char               session_id[37];

    /* Pending request slot. The acceptor pushes here before signalling
     * read_cv; read_fn pops. Then the acceptor blocks on response_cv
     * until write_fn deposits a response. */
    pthread_mutex_t    slot_mu;
    pthread_cond_t     read_cv;
    pthread_cond_t     write_cv;
    char              *req_body;
    size_t             req_len;
    int                req_present;     /* non-zero iff req_body set */
    int                req_is_init;     /* this request is `initialize` */

    char              *resp_body;
    size_t             resp_len;
    int                resp_present;
    int                resp_was_minted; /* mint session id on this response */

    /* Headers of the request currently in the slot (B.1). Deep-copied
     * from the parsed request under slot_mu when the body is deposited,
     * so a handler can reach e.g. `Authorization` via
     * cmcp_handler_get_header. Replaced on the next deposit; because the
     * transport handles one request at a time, a value handed to a
     * handler stays valid for that handler's whole call. */
    cmcp_http_header_t cur_headers[CMCP_HTTP_MAX_HEADERS];
    size_t             cur_n_headers;

    /* SSE bookkeeping. sse_mu guards both the holder list AND the
     * event ring + counter (MCP 2025-11-25 SEP-1699): a single lock so
     * an emit can atomically assign an id, record the event in the
     * ring, and fan out to every holder. Replay during a new GET also
     * runs under this lock, so a holder registered for replay starts
     * receiving live events at exactly the right point. */
    pthread_mutex_t    sse_mu;
    sse_conn_t        *sse_head;

    /* Event ring for Last-Event-Id resumption. Per-session monotonic
     * seq counter; the ring records the last `event_ring_capacity`
     * events. Body is owned by the ring entry (freed on eviction).
     * Sized at listen time from CMCP_HTTP_SSE_REPLAY_BUFFER (default
     * HTTP_SSE_REPLAY_DEFAULT, clamped to HTTP_SSE_REPLAY_MAX). */
    uint64_t           event_next_id;
    void              *event_ring;          /* sse_buf_entry_t[] */
    size_t             event_ring_capacity;
    size_t             event_ring_head;     /* next write slot */
    size_t             event_ring_count;    /* live entries */

    /* Allow-list for the Origin header (MCP 2025-11-25 Minor 3:
     * DNS-rebinding defense). NULL or empty → no check; otherwise a
     * comma-separated list, snapshot from CMCP_HTTP_ALLOWED_ORIGINS at
     * listen time so per-request handling is allocation-free. */
    char              *allowed_origins;

    /* Per-connection read budget (Tier 6 axis 6.5.1). Snapshotted from
     * CMCP_HTTP_IDLE_TIMEOUT_MS and CMCP_HTTP_DEADLINE_MS at listen
     * time. Both are advisory caps; 0 (or negative) disables. */
    int                idle_timeout_ms;
    int                deadline_ms;

    /* Accept-rate token bucket (Tier 6 axis 6.5.2). All four fields
     * are touched only by the acceptor thread, so no synchronisation is
     * needed. accept_rate <= 0 disables the bucket. */
    double             accept_rate;        /* tokens/sec; <= 0 = disabled */
    double             accept_burst;       /* bucket capacity */
    double             accept_tokens;      /* current fractional balance */
    struct timespec    accept_last_refill; /* CLOCK_MONOTONIC */
} http_impl_t;

typedef struct {
    uint64_t  id;
    char     *body;     /* owned; raw JSON-RPC notification body */
    size_t    len;
} sse_buf_entry_t;

/* Forward decls. */
static void *acceptor_main(void *arg);
static void  handle_one_connection(http_impl_t *impl, int fd);
static void  release_pending_failure(http_impl_t *impl);
static int   sse_emit_event_to_fd(int fd, uint64_t id,
                                   const char *body, size_t len);
static uint64_t sse_record_event(http_impl_t *impl,
                                  const char *body, size_t len);
static int   sse_replay_after(http_impl_t *impl, uint64_t last_event_id, int fd);

/* ====================================================================== */
/* SSE holder thread                                                        */
/* ====================================================================== */

/* SIGUSR2 handler — needs to install only once per process. */
static pthread_once_t   wake_init_once = PTHREAD_ONCE_INIT;
static void wake_handler(int s) { (void)s; }
static void wake_init(void) {
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = wake_handler;
    sa.sa_flags = 0;        /* no SA_RESTART — we want EINTR */
    sigemptyset(&sa.sa_mask);
    sigaction(CMCP_HTTP_WAKE_SIGNAL, &sa, NULL);
}

static void *sse_holder_main(void *arg) {
    sse_conn_t *c = (sse_conn_t *)arg;
    /* Self-detach: the acceptor used to detach us via pthread_detach
     * after pthread_create returned, but that races with a short-lived
     * holder that exits + frees `c` before the acceptor's detach call
     * runs (TSan-correctly flagged the UAF on c->thread). Self-detach
     * eliminates the race — we own the lifetime decision. */
    pthread_detach(pthread_self());
    /* Hold the connection open until either:
     *   - the client disconnects (recv returns 0/error)
     *   - the transport shuts down (close fd or signal) */
    for (;;) {
        if (atomic_load_explicit(&c->owner->shutting_down,
                                  memory_order_relaxed)) break;
        struct pollfd p = { c->fd, POLLIN, 0 };
        int rv = poll(&p, 1, HTTP_ACCEPT_POLL_MS);
        if (rv < 0) {
            if (errno == EINTR) continue;
            break;
        }
        if (rv > 0) {
            /* Either client closed (POLLHUP/POLLIN with 0 bytes) or
             * sent something we don't care about. Bail. */
            char tmp[64];
            ssize_t n = recv(c->fd, tmp, sizeof tmp, MSG_DONTWAIT);
            if (n <= 0) break;
            /* Otherwise keep looping — server doesn't expect inbound on
             * an SSE stream. */
        }
    }
    close(c->fd);
    /* Unlink ourselves and free. */
    pthread_mutex_lock(&c->owner->sse_mu);
    sse_conn_t **pp = &c->owner->sse_head;
    while (*pp && *pp != c) pp = &(*pp)->next;
    if (*pp) *pp = c->next;
    pthread_mutex_unlock(&c->owner->sse_mu);
    free(c);
    return NULL;
}

/* ====================================================================== */
/* Connection handling                                                      */
/* ====================================================================== */

/* Send a tiny error reply (text/plain) and close. */
static void reply_error(int fd, int status, const char *reason,
                          const char *body) {
    http_write_response(fd, status, reason, "text/plain", NULL,
                         body, body ? strlen(body) : 0);
}

/* Inspect a JSON-RPC body to determine routing:
 *   - is_request    : has a top-level "id" AND has "method"
 *   - is_notif      : has "method" but no "id"
 *   - method_is_init: top-level "method" equals "initialize"
 *
 * Notifications (and bare responses, which don't apply to MCP server
 * inputs) get HTTP 202 Accepted with no body — server.c won't write
 * anything back, and we mustn't sit on the slot waiting for a response.
 *
 * Parses with cmcp_json. The body is already in memory and small;
 * doing a real parse here is more honest than a regex peek and lets
 * the transport stay correct even with whitespace/escapes/etc. */
typedef struct {
    int is_request;
    int is_notif;
    int is_response;     /* JSON-RPC response: id + (result|error), no method */
    int method_is_init;
} body_kind_t;

static body_kind_t classify_body(const char *body, size_t len) {
    body_kind_t k = {0};
    cmcp_json_t *j = cmcp_json_parse(body, len);
    if (!j || j->type != CMCP_JSON_OBJECT) { cmcp_json_free(j); return k; }
    const cmcp_json_t *method = cmcp_json_object_get(j, "method");
    const cmcp_json_t *id     = cmcp_json_object_get(j, "id");
    if (method && method->type == CMCP_JSON_STRING) {
        if (id) k.is_request = 1; else k.is_notif = 1;
        if (strcmp(method->str.s, "initialize") == 0) k.method_is_init = 1;
    } else if (id && (cmcp_json_object_get(j, "result") ||
                       cmcp_json_object_get(j, "error"))) {
        /* A well-formed JSON-RPC response (answer to a server→client
         * request). server.c consumes it and writes nothing back. */
        k.is_response = 1;
    }
    cmcp_json_free(j);
    return k;
}

/* B.1 — current-request header snapshot. -------------------------------- */

/* Free the snapshot. Caller holds slot_mu (or is in single-threaded
 * teardown). */
static void http_clear_cur_headers(http_impl_t *impl) {
    for (size_t i = 0; i < impl->cur_n_headers; i++) {
        free(impl->cur_headers[i].name);
        free(impl->cur_headers[i].value);
        impl->cur_headers[i].name  = NULL;
        impl->cur_headers[i].value = NULL;
    }
    impl->cur_n_headers = 0;
}

/* Deep-copy `req`'s headers into the slot so a handler can read them
 * after `req` (a stack local in the acceptor) is gone. Caller holds
 * slot_mu. A strdup failure drops that one header (it simply reads as
 * absent) rather than failing the request — auth is handler policy, not
 * a transport invariant. */
static void http_set_cur_headers(http_impl_t *impl, const http_request_t *req) {
    http_clear_cur_headers(impl);
    size_t n = req->n_headers;
    if (n > CMCP_HTTP_MAX_HEADERS) n = CMCP_HTTP_MAX_HEADERS;
    for (size_t i = 0; i < n; i++) {
        char *nm = req->headers[i].name  ? strdup(req->headers[i].name)  : NULL;
        char *vl = req->headers[i].value ? strdup(req->headers[i].value) : NULL;
        if ((req->headers[i].name && !nm) || (req->headers[i].value && !vl)) {
            free(nm); free(vl);
            continue;
        }
        impl->cur_headers[impl->cur_n_headers].name  = nm;
        impl->cur_headers[impl->cur_n_headers].value = vl;
        impl->cur_n_headers++;
    }
}

/* vtable request_header_fn: case-insensitive lookup in the current
 * request's snapshot. The borrowed pointer stays valid for the handler's
 * call because the transport handles one request at a time — the next
 * deposit (which would replace the snapshot) cannot run until this
 * request's response has been written. */
static const char *http_request_header_fn(cmcp_transport_t *t,
                                          const char *name) {
    http_impl_t *impl = (http_impl_t *)t->impl;
    const char *val = NULL;
    pthread_mutex_lock(&impl->slot_mu);
    for (size_t i = 0; i < impl->cur_n_headers; i++) {
        if (impl->cur_headers[i].name &&
            strcasecmp(impl->cur_headers[i].name, name) == 0) {
            val = impl->cur_headers[i].value;
            break;
        }
    }
    pthread_mutex_unlock(&impl->slot_mu);
    return val;
}

static void handle_post(http_impl_t *impl, int fd, http_request_t *req) {
    if (req->body_len == 0) {
        reply_error(fd, 400, "Bad Request", "empty body\n");
        return;
    }

    body_kind_t kind = classify_body(req->body, req->body_len);

    /* Session check. The first `initialize` request is allowed without
     * a session id (it mints one); everything else requires a match. */
    const char *sid = http_header_get(req, "Mcp-Session-Id");
    pthread_mutex_lock(&impl->session_mu);
    int have_session = impl->session_id[0] != '\0';
    if (!kind.method_is_init && !have_session) {
        pthread_mutex_unlock(&impl->session_mu);
        reply_error(fd, 400, "Bad Request",
                     "missing Mcp-Session-Id (no session yet)\n");
        return;
    }
    if (have_session && sid && strcmp(sid, impl->session_id) != 0) {
        pthread_mutex_unlock(&impl->session_mu);
        reply_error(fd, 404, "Not Found",
                     "unknown Mcp-Session-Id\n");
        return;
    }
    if (!kind.method_is_init && have_session && !sid) {
        pthread_mutex_unlock(&impl->session_mu);
        reply_error(fd, 400, "Bad Request",
                     "missing Mcp-Session-Id\n");
        return;
    }
    pthread_mutex_unlock(&impl->session_mu);

    /* Push the body into the request slot. The slot is busy from the
     * moment we deposit a body until either:
     *   - server.c writes a response (req_present and resp_present
     *     both become 0 after the handler steals the response), or
     *   - server.c consumes a notification (req_present clears in
     *     read_fn, resp_present stays 0).
     * Wait for both to be clear before pushing a new request. */
    pthread_mutex_lock(&impl->slot_mu);
    while ((impl->req_present || impl->resp_present) &&
            !atomic_load_explicit(&impl->shutting_down,
                                   memory_order_relaxed)) {
        pthread_cond_wait(&impl->write_cv, &impl->slot_mu);
    }
    if (atomic_load_explicit(&impl->shutting_down, memory_order_relaxed)) {
        pthread_mutex_unlock(&impl->slot_mu);
        reply_error(fd, 503, "Service Unavailable", "shutting down\n");
        return;
    }
    impl->req_body    = (char *)malloc(req->body_len + 1);
    if (!impl->req_body) {
        pthread_mutex_unlock(&impl->slot_mu);
        reply_error(fd, 500, "Internal Server Error", "oom\n");
        return;
    }
    memcpy(impl->req_body, req->body, req->body_len);
    impl->req_body[req->body_len] = '\0';
    impl->req_len     = req->body_len;
    impl->req_is_init = kind.method_is_init;
    impl->req_present = 1;
    http_set_cur_headers(impl, req);     /* B.1: expose headers to handler */
    pthread_cond_signal(&impl->read_cv);

    /* Only genuine notifications and JSON-RPC responses produce no
     * upper-layer reply; per spec those POSTs get 202 Accepted with no
     * body. Everything else — a request, a batch, or any malformed body
     * — is answered by server.c with exactly one frame (the result, or a
     * -32600/-32700 error), so we MUST wait for the response slot. The
     * earlier `!is_request` heuristic mis-routed every malformed body to
     * the 202 path: server.c then deposited an error frame that no POST
     * handler ever stole, leaving resp_present pinned and permanently
     * deadlocking every subsequent request on this transport.
     * Wait until server.c consumes the request (so we don't race the
     * close+free path) and then return 202. */
    if (kind.is_notif || kind.is_response) {
        while (impl->req_present &&
               !atomic_load_explicit(&impl->shutting_down,
                                      memory_order_relaxed)) {
            pthread_cond_wait(&impl->write_cv, &impl->slot_mu);
        }
        pthread_mutex_unlock(&impl->slot_mu);
        http_write_response(fd, 202, "Accepted", "text/plain",
                              NULL, "", 0);
        return;
    }

    /* Wait for the response. */
    while (!impl->resp_present &&
           !atomic_load_explicit(&impl->shutting_down,
                                  memory_order_relaxed)) {
        pthread_cond_wait(&impl->write_cv, &impl->slot_mu);
    }
    if (!impl->resp_present) {
        pthread_mutex_unlock(&impl->slot_mu);
        reply_error(fd, 503, "Service Unavailable", "shutting down\n");
        return;
    }

    /* Steal response state. */
    char *body = impl->resp_body;
    size_t blen = impl->resp_len;
    int    minted = impl->resp_was_minted;
    impl->resp_body = NULL;
    impl->resp_len  = 0;
    impl->resp_present = 0;
    impl->resp_was_minted = 0;
    pthread_cond_signal(&impl->write_cv);
    pthread_mutex_unlock(&impl->slot_mu);

    /* Send HTTP 200 with the JSON-RPC response. If we just minted a
     * session id on the initialize response, include it in headers. */
    char extra[128] = {0};
    if (minted) {
        snprintf(extra, sizeof extra, "Mcp-Session-Id: %s\r\n",
                  impl->session_id);
    }
    http_write_response(fd, 200, "OK", "application/json",
                          minted ? extra : NULL, body, blen);
    free(body);
}

static void handle_get_sse(http_impl_t *impl, int fd, http_request_t *req) {
    /* SSE requires an existing session. */
    pthread_mutex_lock(&impl->session_mu);
    int have_session = impl->session_id[0] != '\0';
    char sid_copy[37] = {0};
    if (have_session) memcpy(sid_copy, impl->session_id, sizeof sid_copy);
    pthread_mutex_unlock(&impl->session_mu);

    const char *sid = http_header_get(req, "Mcp-Session-Id");
    if (!have_session || !sid || strcmp(sid, sid_copy) != 0) {
        reply_error(fd, 404, "Not Found", "no matching session\n");
        return;
    }

    if (http_write_sse_head(fd, sid_copy) != CMCP_OK) {
        close(fd);
        return;
    }

    /* Last-Event-Id resumption (MCP 2025-11-25 SEP-1699). If the header
     * is present and parses to an integer, replay every buffered event
     * with id > that value, then register the new holder for live
     * events — both under sse_mu so live and replayed events are not
     * interleaved out of order. Unknown / unparseable / older-than-
     * buffer ids result in no replay, which is spec-legal: the client
     * just starts seeing live events. */
    uint64_t last_event_id = 0;
    int      have_resume   = 0;
    const char *leid = http_header_get(req, "Last-Event-Id");
    if (leid && *leid) {
        char *end;
        unsigned long long parsed = strtoull(leid, &end, 10);
        if (end != leid && *end == '\0') {
            last_event_id = (uint64_t)parsed;
            have_resume = 1;
        }
    }

    sse_conn_t *c = (sse_conn_t *)calloc(1, sizeof *c);
    if (!c) { close(fd); return; }
    c->fd    = fd;
    c->owner = impl;

    pthread_mutex_lock(&impl->sse_mu);
    if (have_resume) {
        int rc = sse_replay_after(impl, last_event_id, fd);
        if (rc != CMCP_OK) {
            pthread_mutex_unlock(&impl->sse_mu);
            free(c);
            close(fd);
            return;
        }
    }
    c->next = impl->sse_head;
    impl->sse_head = c;
    pthread_mutex_unlock(&impl->sse_mu);

    if (pthread_create(&c->thread, NULL, sse_holder_main, c) != 0) {
        pthread_mutex_lock(&impl->sse_mu);
        impl->sse_head = c->next;
        pthread_mutex_unlock(&impl->sse_mu);
        close(c->fd);
        free(c);
    }
    /* On success: holder thread detaches itself in sse_holder_main —
     * detaching here would race with a short-lived holder that has
     * already exited and freed c. */
}

/* Validate the MCP-Protocol-Version header (transports section).
 * Absent → accept: per spec the server falls back to the default
 * revision. Present but not the version cMCP speaks → 400.
 * Returns 1 to proceed, 0 if a 400 was already sent. */
static int check_protocol_version(int fd, const http_request_t *req) {
    const char *pv = http_header_get(req, "MCP-Protocol-Version");
    if (pv && strcmp(pv, CMCP_PROTOCOL_VERSION) != 0) {
        reply_error(fd, 400, "Bad Request",
                     "unsupported MCP-Protocol-Version\n");
        return 0;
    }
    return 1;
}

/* Origin allow-list check (MCP 2025-11-25 Minor 3 — DNS rebinding
 * defense). Caller passes `impl->allowed_origins` (NULL/"" → no check).
 * Header absent → accept (non-browser clients don't emit Origin).
 * Header present + matches one entry of the comma-separated list →
 * accept. Otherwise 403. Returns 1 to proceed, 0 if 403 was sent. */
static int check_origin(http_impl_t *impl, int fd, const http_request_t *req) {
    if (!impl->allowed_origins || !*impl->allowed_origins) return 1;
    const char *origin = http_header_get(req, "Origin");
    if (!origin) return 1;
    size_t olen = strlen(origin);
    const char *p = impl->allowed_origins;
    while (*p) {
        const char *comma = strchr(p, ',');
        const char *start = p;
        size_t len = comma ? (size_t)(comma - p) : strlen(p);
        while (len > 0 && *start == ' ') { start++; len--; }
        while (len > 0 && start[len - 1] == ' ') len--;
        if (len == olen && memcmp(start, origin, olen) == 0) return 1;
        if (!comma) break;
        p = comma + 1;
    }
    reply_error(fd, 403, "Forbidden", "Origin not in allow-list\n");
    return 0;
}

static void handle_one_connection(http_impl_t *impl, int fd) {
    http_request_t req = {0};
    conn_budget_t budget = {
        .idle_ms     = impl->idle_timeout_ms,
        .deadline_ms = impl->deadline_ms,
    };
    int rc = http_read_request(fd, &budget, &req);
    if (rc != CMCP_OK) {
        if (rc == CMCP_EUNSUPPORTED) {
            reply_error(fd, 501, "Not Implemented",
                         "chunked transfer encoding not supported\n");
        } else if (rc == HTTP_ETOOLARGE) {
            /* Content-Length past HTTP_MAX_BODY_BYTES (4 MiB). 413 is the
             * spec code for an over-cap body (threat-model.md row 1.2). */
            reply_error(fd, 413, "Payload Too Large",
                         "request body exceeds maximum size\n");
        } else if (rc == CMCP_ETIMEOUT) {
            /* Slowloris / deadline: peer dribbled bytes (or stalled
             * entirely) past the configured budget. 408 is the spec
             * code for client-side timeout; libcurl + most browsers
             * treat it as a clean abort. */
            reply_error(fd, 408, "Request Timeout",
                         "idle or whole-request deadline exceeded\n");
        } else {
            reply_error(fd, 400, "Bad Request", "malformed request\n");
        }
        http_request_clear(&req);
        close(fd);
        return;
    }

    /* Route. Path may carry a query string; ignore beyond ?. */
    char *path = req.target;
    char *q = strchr(path, '?');
    if (q) *q = '\0';

    int is_post_mcp = (strcmp(req.method, "POST") == 0 &&
                        strcmp(path, "/mcp") == 0);
    int is_get_mcp  = (strcmp(req.method, "GET") == 0 &&
                        strcmp(path, "/mcp") == 0);

    /* The two POST/GET dispatch ladders below have multiple branches that
     * all end with close(fd); the response bodies they emit (origin reject,
     * protocol-version reject, success) are written from inside the
     * check_origin / check_protocol_version / handle_post helpers, so the
     * close-on-exit pattern is genuine uniformity, not accidental
     * duplication. */
    if (is_post_mcp) {
        /* NOLINTBEGIN(bugprone-branch-clone) */
        if (!check_origin(impl, fd, &req)) {
            close(fd);
        } else if (check_protocol_version(fd, &req)) {
            handle_post(impl, fd, &req);
            close(fd);
        } else {
            close(fd);
        }
        /* NOLINTEND(bugprone-branch-clone) */
    } else if (is_get_mcp) {
        const char *acc = http_header_get(&req, "Accept");
        /* NOLINTBEGIN(bugprone-branch-clone) */
        if (!check_origin(impl, fd, &req)) {
            close(fd);
        } else if (!check_protocol_version(fd, &req)) {
            close(fd);
        } else if (acc && strstr(acc, "text/event-stream")) {
            handle_get_sse(impl, fd, &req);
            /* fd ownership moves to the holder thread; do NOT close. */
        } else {
            reply_error(fd, 406, "Not Acceptable",
                         "GET /mcp requires Accept: text/event-stream\n");
            close(fd);
        }
        /* NOLINTEND(bugprone-branch-clone) */
    } else {
        reply_error(fd, 404, "Not Found", "no such endpoint\n");
        close(fd);
    }
    http_request_clear(&req);
}

/* ====================================================================== */
/* Acceptor                                                                 */
/* ====================================================================== */

/* Try to consume one token from the accept-rate bucket. Returns 1 on
 * success (caller proceeds with handle_one_connection), 0 if the
 * bucket is empty (caller sends 503). Called only from the acceptor
 * thread, so no synchronisation. accept_rate <= 0 → always 1. */
static int accept_bucket_consume(http_impl_t *impl) {
    if (impl->accept_rate <= 0.0) return 1;

    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    double elapsed_s = (double)(now.tv_sec - impl->accept_last_refill.tv_sec)
                      + (double)(now.tv_nsec - impl->accept_last_refill.tv_nsec) / 1e9;
    impl->accept_last_refill = now;

    if (elapsed_s > 0.0) {
        impl->accept_tokens += elapsed_s * impl->accept_rate;
        if (impl->accept_tokens > impl->accept_burst)
            impl->accept_tokens = impl->accept_burst;
    }

    if (impl->accept_tokens >= 1.0) {
        impl->accept_tokens -= 1.0;
        return 1;
    }
    return 0;
}

/* Send a 503 with Retry-After in seconds and close. Used when the
 * accept-rate bucket is empty — the peer should back off and retry.
 * Retry-After is computed from the configured rate (1/rate, clamped
 * to [1, 60] so it's both sensible and recognisable). */
static void reply_rate_limited(http_impl_t *impl, int fd) {
    int retry_s = 1;
    if (impl->accept_rate > 0.0) {
        double secs = 1.0 / impl->accept_rate;
        if (secs > 60.0) secs = 60.0;
        if (secs < 1.0)  secs = 1.0;
        retry_s = (int)secs;
    }
    char extra[64];
    snprintf(extra, sizeof extra, "Retry-After: %d\r\n", retry_s);
    const char *msg = "accept-rate limit exceeded\n";
    http_write_response(fd, 503, "Service Unavailable",
                         "text/plain", extra, msg, strlen(msg));
    close(fd);
}

static void *acceptor_main(void *arg) {
    http_impl_t *impl = (http_impl_t *)arg;
    /* Seed the bucket: full burst capacity at start. */
    impl->accept_tokens = impl->accept_burst;
    clock_gettime(CLOCK_MONOTONIC, &impl->accept_last_refill);

    while (!atomic_load_explicit(&impl->shutting_down,
                                  memory_order_relaxed)) {
        struct pollfd p = { impl->listen_fd, POLLIN, 0 };
        int rv = poll(&p, 1, HTTP_ACCEPT_POLL_MS);
        if (rv < 0) {
            if (errno == EINTR) continue;
            break;
        }
        if (rv == 0) continue;
        struct sockaddr_storage ss;
        socklen_t slen = sizeof ss;
        int cfd = accept(impl->listen_fd, (struct sockaddr *)&ss, &slen);
        if (cfd < 0) {
            if (errno == EINTR || errno == EAGAIN) continue;
            break;
        }

        /* Accept-rate gate (6.5.2). Peer's connection is already
         * established at the kernel layer; we send a 503 and close
         * so they see a definitive answer rather than a hang. */
        if (!accept_bucket_consume(impl)) {
            reply_rate_limited(impl, cfd);
            continue;
        }

        handle_one_connection(impl, cfd);
    }
    /* Wake any pending POST waiters so they can bail. */
    release_pending_failure(impl);
    return NULL;
}

static void release_pending_failure(http_impl_t *impl) {
    pthread_mutex_lock(&impl->slot_mu);
    pthread_cond_broadcast(&impl->read_cv);
    pthread_cond_broadcast(&impl->write_cv);
    pthread_mutex_unlock(&impl->slot_mu);
}

/* ====================================================================== */
/* Vtable                                                                   */
/* ====================================================================== */

static int http_read_fn(cmcp_transport_t *t, char **out_buf, size_t *out_len) {
    http_impl_t *impl = (http_impl_t *)t->impl;
    pthread_mutex_lock(&impl->slot_mu);
    while (!impl->req_present &&
           !atomic_load_explicit(&impl->shutting_down,
                                  memory_order_relaxed)) {
        pthread_cond_wait(&impl->read_cv, &impl->slot_mu);
    }
    if (atomic_load_explicit(&impl->shutting_down,
                              memory_order_relaxed) &&
        !impl->req_present) {
        pthread_mutex_unlock(&impl->slot_mu);
        return CMCP_EIO;
    }
    *out_buf = impl->req_body;
    *out_len = impl->req_len;
    impl->req_body = NULL;
    impl->req_len  = 0;
    impl->req_present = 0;
    /* Wake the POST handler — for notifications it's waiting for
     * exactly this consumed-event signal. */
    pthread_cond_broadcast(&impl->write_cv);
    pthread_mutex_unlock(&impl->slot_mu);
    return CMCP_OK;
}

/* Frame `body` as an SSE event with the given id and send to one fd.
 * Each event is `id: <id>\ndata: <body>\n\n`. Newlines inside `body`
 * are emitted as separate `data:` continuation lines, per the
 * EventSource spec. Per SEP-1699 (MCP 2025-11-25) every event carries
 * an id so a reconnecting client can pick up via Last-Event-Id. */
static int sse_emit_event_to_fd(int fd, uint64_t id,
                                  const char *body, size_t len) {
    /* Sized for the id header (decimal uint64 fits in 24 chars) + the
     * worst-case body framing. The 1.5x slop covers the case where
     * every byte of body is a newline that doubles into 7 bytes
     * (`\ndata: `). */
    size_t cap = 64 + len * 8 + 16;
    char *out = (char *)malloc(cap);
    if (!out) return CMCP_ENOMEM;
    size_t n = 0;
    int idlen = snprintf(out, cap, "id: %llu\n", (unsigned long long)id);
    if (idlen < 0 || (size_t)idlen >= cap) { free(out); return CMCP_EIO; }
    n = (size_t)idlen;
    /* The buffer being assembled is an SSE wire frame, not a C string —
     * length is tracked in `n` and the eventual send() goes through
     * write(2) with that length. Both memcpys deliberately copy 6 bytes
     * of "data: " without a trailing NUL; the analyser's
     * not-null-terminated-result rule is for C-string contexts. */
    memcpy(out + n, "data: ", 6); n += 6;  // NOLINT(bugprone-not-null-terminated-result)
    for (size_t i = 0; i < len; i++) {
        char c = body[i];
        if (n + 8 >= cap) {
            cap *= 2;
            char *nb = (char *)realloc(out, cap);
            if (!nb) { free(out); return CMCP_ENOMEM; }
            out = nb;
        }
        if (c == '\n') {
            out[n++] = '\n';
            memcpy(out + n, "data: ", 6); n += 6;  // NOLINT(bugprone-not-null-terminated-result)
        } else {
            out[n++] = c;
        }
    }
    if (n + 2 > cap) {
        cap += 2;
        char *nb = (char *)realloc(out, cap);
        if (!nb) { free(out); return CMCP_ENOMEM; }
        out = nb;
    }
    out[n++] = '\n';
    out[n++] = '\n';
    int rc = send_all(fd, out, n);
    free(out);
    return rc;
}

/* Record `body` in the replay ring (evicts the oldest entry if full).
 * Caller must hold sse_mu. Returns the assigned event id; 0 means
 * allocation failure and the event isn't recorded (but should still
 * be emitted to live holders — the spec doesn't require persistence). */
static uint64_t sse_record_event(http_impl_t *impl,
                                   const char *body, size_t len) {
    uint64_t id = impl->event_next_id++;
    if (impl->event_ring_capacity == 0) return id;
    sse_buf_entry_t *ring = (sse_buf_entry_t *)impl->event_ring;
    char *copy = (char *)malloc(len);
    if (!copy) return id;   /* live emit still happens; just no replay */
    memcpy(copy, body, len);
    sse_buf_entry_t *slot = &ring[impl->event_ring_head];
    free(slot->body);
    slot->id   = id;
    slot->body = copy;
    slot->len  = len;
    impl->event_ring_head = (impl->event_ring_head + 1) % impl->event_ring_capacity;
    if (impl->event_ring_count < impl->event_ring_capacity) {
        impl->event_ring_count++;
    }
    return id;
}

/* Stream every buffered event with id > last_event_id, in order, to
 * the given fd. Caller must hold sse_mu. Returns 0 on success, an
 * error code if any send failed (caller drops the holder). */
static int sse_replay_after(http_impl_t *impl, uint64_t last_event_id, int fd) {
    if (impl->event_ring_capacity == 0 || impl->event_ring_count == 0) return CMCP_OK;
    sse_buf_entry_t *ring = (sse_buf_entry_t *)impl->event_ring;
    size_t cap = impl->event_ring_capacity;
    /* Oldest entry index = head - count (mod capacity). */
    size_t idx = (impl->event_ring_head + cap - impl->event_ring_count) % cap;
    for (size_t i = 0; i < impl->event_ring_count; i++) {
        sse_buf_entry_t *e = &ring[idx];
        if (e->id > last_event_id) {
            int rc = sse_emit_event_to_fd(fd, e->id, e->body, e->len);
            if (rc != CMCP_OK) return rc;
        }
        idx = (idx + 1) % cap;
    }
    return CMCP_OK;
}

static int http_write_fn(cmcp_transport_t *t, const char *buf, size_t len) {
    http_impl_t *impl = (http_impl_t *)t->impl;

    /* Server-initiated notifications (no `id` field) ride the SSE
     * channel rather than the request/response slot — they're not
     * answers to a POST, so there's no waiting POST handler to hand
     * them to. We send synchronously to every currently-held SSE
     * connection. If none are open, the notification is dropped (the
     * spec allows this; the client should open the SSE stream if it
     * wants to receive). */
    body_kind_t kind = classify_body(buf, len);
    if (kind.is_notif) {
        pthread_mutex_lock(&impl->sse_mu);
        uint64_t id = sse_record_event(impl, buf, len);
        sse_conn_t *c = impl->sse_head;
        while (c) {
            sse_emit_event_to_fd(c->fd, id, buf, len);
            c = c->next;
        }
        pthread_mutex_unlock(&impl->sse_mu);
        return CMCP_OK;
    }

    /* Response path: into the slot mailbox. */
    char *copy = (char *)malloc(len + 1);
    if (!copy) return CMCP_ENOMEM;
    memcpy(copy, buf, len);
    copy[len] = '\0';

    pthread_mutex_lock(&impl->slot_mu);
    /* Drop any prior unanswered response (shouldn't happen). */
    free(impl->resp_body);
    impl->resp_body = copy;
    impl->resp_len  = len;
    impl->resp_present = 1;

    /* If this turn started as an `initialize` request and we don't
     * yet have a session id, mint one and stamp it onto the response
     * headers. The check is done here (not on read) so that clients
     * sending a *malformed* initialize never get a session id. */
    pthread_mutex_lock(&impl->session_mu);
    int needs_mint = impl->req_is_init && impl->session_id[0] == '\0';
    if (needs_mint) {
        mint_session_id(impl->session_id);
        impl->resp_was_minted = 1;
    }
    pthread_mutex_unlock(&impl->session_mu);

    /* Mark this request consumed so the next POST can be queued. */
    impl->req_present = 0;
    impl->req_is_init = 0;

    pthread_cond_broadcast(&impl->write_cv);
    pthread_mutex_unlock(&impl->slot_mu);
    return CMCP_OK;
}

/* Wake without freeing: signals shutting_down + broadcasts every cv a
 * server thread (or POST handler) might be parked on. After this returns
 * the host MUST pthread_join its server thread before calling close_fn —
 * otherwise close_fn destroys the slot mutex while the server is still
 * unwinding through cond_wait. The vtable convention is the same as the
 * HTTP client transport: wake_fn is the signal, close_fn is the
 * destructor; closing without a prior wake works (acceptor's release
 * fires the cvs as it dies) but only if the host has already joined. */
static void http_wake_fn(cmcp_transport_t *t) {
    if (!t) return;
    http_impl_t *impl = (http_impl_t *)t->impl;
    if (!impl) return;
    atomic_store_explicit(&impl->shutting_down, 1, memory_order_relaxed);
    pthread_mutex_lock(&impl->slot_mu);
    pthread_cond_broadcast(&impl->read_cv);
    pthread_cond_broadcast(&impl->write_cv);
    pthread_mutex_unlock(&impl->slot_mu);
}

static void http_close_fn(cmcp_transport_t *t) {
    if (!t) return;
    http_impl_t *impl = (http_impl_t *)t->impl;
    if (impl) {
        atomic_store_explicit(&impl->shutting_down, 1,
                               memory_order_relaxed);

        /* Unblock the acceptor's poll() via SHUT_RDWR on the listen
         * socket, then JOIN before clearing listen_fd — otherwise the
         * acceptor and the close race on the same int (TSan-correctly
         * flagged the write-vs-read on listen_fd). The fd itself stays
         * open until after the join so the acceptor sees a stable
         * value in its last loop iteration. */
        if (impl->listen_fd >= 0) {
            shutdown(impl->listen_fd, SHUT_RDWR);
        }
        if (impl->acceptor_started) {
            pthread_join(impl->acceptor, NULL);
            impl->acceptor_started = 0;
        }
        if (impl->listen_fd >= 0) {
            close(impl->listen_fd);
            impl->listen_fd = -1;
        }
        /* Tell SSE holders to exit; close their sockets so poll wakes. */
        pthread_mutex_lock(&impl->sse_mu);
        sse_conn_t *c = impl->sse_head;
        while (c) {
            shutdown(c->fd, SHUT_RDWR);
            c = c->next;
        }
        pthread_mutex_unlock(&impl->sse_mu);
        /* SSE holders are detached; they'll free themselves once they
         * notice shutting_down. We do NOT join them — they'd race with
         * the impl free. Instead, spin briefly. */
        for (int i = 0; i < 50; i++) {
            pthread_mutex_lock(&impl->sse_mu);
            int empty = impl->sse_head == NULL;
            pthread_mutex_unlock(&impl->sse_mu);
            if (empty) break;
            struct timespec ts = { 0, 1000000 };  /* 1ms */
            nanosleep(&ts, NULL);
        }
        free(impl->req_body);
        free(impl->resp_body);
        free(impl->allowed_origins);
        http_clear_cur_headers(impl);        /* B.1: free header snapshot */
        if (impl->event_ring) {
            sse_buf_entry_t *ring = (sse_buf_entry_t *)impl->event_ring;
            for (size_t i = 0; i < impl->event_ring_capacity; i++) {
                free(ring[i].body);
            }
            free(impl->event_ring);
        }
        pthread_mutex_destroy(&impl->slot_mu);
        pthread_mutex_destroy(&impl->session_mu);
        pthread_mutex_destroy(&impl->sse_mu);
        pthread_cond_destroy(&impl->read_cv);
        pthread_cond_destroy(&impl->write_cv);
        free(impl);
    }
    free(t);
}

/* ====================================================================== */
/* Constructor                                                              */
/* ====================================================================== */

cmcp_transport_t *cmcp_transport_http_listen(const char *host,
                                              unsigned short port) {
    pthread_once(&wake_init_once, wake_init);

    struct addrinfo hints;
    memset(&hints, 0, sizeof hints);
    hints.ai_family   = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_flags    = AI_PASSIVE;

    char port_buf[8];
    snprintf(port_buf, sizeof port_buf, "%u", port);

    /* Bind-address default (MCP server-side localhost obligation). A
     * NULL/empty host means "no preference" — which for a no-TLS MCP
     * server must mean loopback, NOT the wildcard.
     *
     * We resolve "127.0.0.1" rather than "localhost": this transport owns
     * a single listen fd (the loop below binds the first address that
     * works and stops), so it can bind exactly one address. "localhost"
     * resolves to both 127.0.0.1 and ::1, and which one we'd bind would
     * depend on the host resolver's ordering — a ::1-only bind would then
     * silently refuse an IPv4 client. 127.0.0.1 is the universally-present
     * loopback and is deterministic. A host that needs IPv6 loopback
     * passes "::1" explicitly; one that wants every interface passes
     * "0.0.0.0" / "::". (Verdict: council, 2026-06-01.) */
    const char *node = (host && *host) ? host : "127.0.0.1";

    struct addrinfo *res = NULL;
    int gai = getaddrinfo(node, port_buf, &hints, &res);
    if (gai != 0 || !res) return NULL;

    int fd = -1;
    for (struct addrinfo *ai = res; ai; ai = ai->ai_next) {
        fd = socket(ai->ai_family, ai->ai_socktype | SOCK_CLOEXEC,
                     ai->ai_protocol);
        if (fd < 0) continue;
        int yes = 1;
        setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof yes);
        if (bind(fd, ai->ai_addr, ai->ai_addrlen) == 0 &&
            listen(fd, HTTP_LISTEN_BACKLOG) == 0) break;
        close(fd);
        fd = -1;
    }
    freeaddrinfo(res);
    if (fd < 0) return NULL;

    http_impl_t *impl = (http_impl_t *)calloc(1, sizeof *impl);
    cmcp_transport_t *t = (cmcp_transport_t *)calloc(1, sizeof *t);
    if (!impl || !t) goto fail;

    impl->listen_fd = fd;
    pthread_mutex_init(&impl->slot_mu, NULL);
    pthread_mutex_init(&impl->session_mu, NULL);
    pthread_mutex_init(&impl->sse_mu, NULL);
    pthread_cond_init(&impl->read_cv, NULL);
    pthread_cond_init(&impl->write_cv, NULL);

    /* Snapshot the Origin allow-list once. NULL or "" → no check. */
    const char *envv = getenv("CMCP_HTTP_ALLOWED_ORIGINS");
    if (envv && *envv) {
        size_t n = strlen(envv);
        impl->allowed_origins = (char *)malloc(n + 1);
        if (impl->allowed_origins) memcpy(impl->allowed_origins, envv, n + 1);
    }

    /* A caller who explicitly binds beyond loopback (e.g. "0.0.0.0") with
     * no Origin allow-list set has opened a DNS-rebinding surface. We warn
     * rather than refuse: binding a non-loopback interface behind the
     * TLS-terminating reverse proxy the threat model assumes is a
     * legitimate, supported deployment — refusing it would be the
     * transport enforcing host policy. The warning surfaces the
     * misconfiguration without dictating one. (Verdict: council, 2026-06-01.) */
    if (!impl->allowed_origins && !bound_addr_is_loopback(fd)) {
        fprintf(stderr,
            "cmcp_transport_http_listen: bound a non-loopback address with no "
            "CMCP_HTTP_ALLOWED_ORIGINS set — the server is reachable beyond "
            "localhost with no DNS-rebinding (Origin) defense. Set "
            "CMCP_HTTP_ALLOWED_ORIGINS, or bind 127.0.0.1, or front it with a "
            "reverse proxy.\n");
    }

    /* Snapshot the per-connection read-budget knobs (Tier 6 axis 6.5.1).
     * Idle: per-recv timeout — slowloris defense. Deadline: cumulative
     * wall-clock budget for the whole request-receive cycle (headers +
     * body). Both default to the constants; 0 or negative disables. */
    impl->idle_timeout_ms = HTTP_IDLE_TIMEOUT_DEFAULT_MS;
    const char *it = getenv("CMCP_HTTP_IDLE_TIMEOUT_MS");
    if (it && *it) {
        char *end; long v = strtol(it, &end, 10);
        if (end != it && *end == '\0') impl->idle_timeout_ms = (int)v;
    }
    impl->deadline_ms = HTTP_DEADLINE_DEFAULT_MS;
    const char *dl = getenv("CMCP_HTTP_DEADLINE_MS");
    if (dl && *dl) {
        char *end; long v = strtol(dl, &end, 10);
        if (end != dl && *end == '\0') impl->deadline_ms = (int)v;
    }

    /* Snapshot the accept-rate token bucket (Tier 6 axis 6.5.2).
     * Rate <= 0 disables; otherwise rate is sustained tokens/sec and
     * burst is the bucket capacity. Both are parsed as doubles so that
     * sub-1 rates (e.g. 0.5 conn/sec for low-traffic deployments) and
     * fractional bursts are expressible. */
    impl->accept_rate = HTTP_ACCEPT_RATE_DEFAULT;
    const char *ar = getenv("CMCP_HTTP_ACCEPT_RATE");
    if (ar && *ar) {
        char *end; double v = strtod(ar, &end);
        if (end != ar && *end == '\0') impl->accept_rate = v;
    }
    impl->accept_burst = HTTP_ACCEPT_BURST_DEFAULT;
    const char *ab = getenv("CMCP_HTTP_ACCEPT_BURST");
    if (ab && *ab) {
        char *end; double v = strtod(ab, &end);
        if (end != ab && *end == '\0' && v > 0.0) impl->accept_burst = v;
    }
    /* accept_tokens / accept_last_refill are seeded by acceptor_main()
     * at thread start; leaving them zero here is intentional. */

    /* Snapshot the SSE replay-buffer size from CMCP_HTTP_SSE_REPLAY_BUFFER
     * (default HTTP_SSE_REPLAY_DEFAULT; clamped to HTTP_SSE_REPLAY_MAX).
     * 0 disables the buffer (events still get IDs but no resumption). */
    size_t cap = HTTP_SSE_REPLAY_DEFAULT;
    const char *re = getenv("CMCP_HTTP_SSE_REPLAY_BUFFER");
    if (re && *re) {
        char *end;
        unsigned long parsed = strtoul(re, &end, 10);
        if (end != re && *end == '\0') {
            if (parsed > HTTP_SSE_REPLAY_MAX) parsed = HTTP_SSE_REPLAY_MAX;
            cap = (size_t)parsed;
        }
    }
    impl->event_ring_capacity = cap;
    impl->event_next_id       = 1;
    if (cap > 0) {
        /* `cap` is the env-tunable replay-ring size, clamped to
         * HTTP_SSE_REPLAY_MAX (65536) at parse time above. Worst case is
         * 65536 * sizeof(sse_buf_entry_t) (a few MB) — bounded, not
         * unbounded attacker input. */
        impl->event_ring = calloc(cap, sizeof(sse_buf_entry_t));  // NOLINT(clang-analyzer-optin.taint.TaintedAlloc)
        /* Tolerate calloc failure: ring stays NULL, events are still
         * assigned ids and emitted, just not buffered for replay. */
        if (!impl->event_ring) impl->event_ring_capacity = 0;
    }

    t->impl     = impl;
    t->read_fn  = http_read_fn;
    t->write_fn = http_write_fn;
    t->close_fn = http_close_fn;
    t->wake_fn  = http_wake_fn;
    t->request_header_fn = http_request_header_fn;

    if (pthread_create(&impl->acceptor, NULL, acceptor_main, impl) != 0) {
        pthread_mutex_destroy(&impl->slot_mu);
        pthread_mutex_destroy(&impl->session_mu);
        pthread_mutex_destroy(&impl->sse_mu);
        pthread_cond_destroy(&impl->read_cv);
        pthread_cond_destroy(&impl->write_cv);
        goto fail;
    }
    impl->acceptor_started = 1;
    return t;

fail:
    if (fd >= 0) close(fd);
    free(impl);
    free(t);
    return NULL;
}
