/* Streamable HTTP transport — server side.
 *
 * Wire shape (MCP 2025-06-18):
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

#define HTTP_MAX_HEADERS_BYTES   (16 * 1024)    /* 16 KiB request line + headers */
#define HTTP_MAX_BODY_BYTES      (4 * 1024 * 1024)   /* 4 MiB body */
#define HTTP_LISTEN_BACKLOG      16
#define HTTP_ACCEPT_POLL_MS      250            /* shutdown polling cadence */

/* Wake signal for SSE holder threads so close() can join them. */
#ifndef CMCP_HTTP_WAKE_SIGNAL
#define CMCP_HTTP_WAKE_SIGNAL  SIGUSR2
#endif

/* ====================================================================== */
/* Tiny string utilities                                                    */
/* ====================================================================== */

static int starts_with_ci(const char *s, const char *prefix) {
    while (*prefix) {
        if (!*s) return 0;
        char a = *s++, b = *prefix++;
        if (a >= 'A' && a <= 'Z') a = (char)(a + 32);
        if (b >= 'A' && b <= 'Z') b = (char)(b + 32);
        if (a != b) return 0;
    }
    return 1;
}

/* Trim ASCII whitespace in place (returns possibly-shifted start). */
static char *trim_inplace(char *s) {
    while (*s == ' ' || *s == '\t') s++;
    char *end = s + strlen(s);
    while (end > s && (end[-1] == ' ' || end[-1] == '\t' ||
                        end[-1] == '\r' || end[-1] == '\n'))
        *--end = '\0';
    return s;
}

/* ====================================================================== */
/* HTTP request                                                             */
/* ====================================================================== */

#define MAX_HEADERS  64

typedef struct {
    char *name;     /* canonical lowercase, owned */
    char *value;    /* trimmed, owned */
} http_header_t;

typedef struct {
    char         *method;       /* "POST", "GET", ... */
    char         *target;       /* "/mcp", "/mcp?foo=bar", ... */
    http_header_t headers[MAX_HEADERS];
    size_t        n_headers;
    char         *body;         /* may be NULL if Content-Length 0 */
    size_t        body_len;
} http_request_t;

static void http_request_clear(http_request_t *r) {
    if (!r) return;
    free(r->method);
    free(r->target);
    for (size_t i = 0; i < r->n_headers; i++) {
        free(r->headers[i].name);
        free(r->headers[i].value);
    }
    free(r->body);
    memset(r, 0, sizeof *r);
}

static const char *http_header_get(const http_request_t *r, const char *name) {
    for (size_t i = 0; i < r->n_headers; i++) {
        if (strcasecmp(r->headers[i].name, name) == 0)
            return r->headers[i].value;
    }
    return NULL;
}

/* ====================================================================== */
/* Read until \r\n\r\n or limit reached. Caller frees *out_buf.            */
/* Returns CMCP_OK on success, CMCP_EIO on socket error / EOF before       */
/* end of headers, CMCP_EPROTOCOL on header section overflow.              */
/* ====================================================================== */

static int read_headers_block(int fd, char **out_buf, size_t *out_len,
                               size_t *out_body_offset) {
    size_t cap = 4096, len = 0;
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
        ssize_t n = recv(fd, buf + len, cap - 1 - len, 0);
        if (n <= 0) { free(buf); return CMCP_EIO; }
        len += (size_t)n;
        buf[len] = '\0';

        /* Look for terminator anywhere in the freshly extended buffer. */
        char *eoh = strstr(buf, "\r\n\r\n");
        if (eoh) {
            *out_buf = buf;
            *out_len = len;
            *out_body_offset = (size_t)(eoh - buf) + 4;
            return CMCP_OK;
        }
        /* Also tolerate bare-LF terminators (\n\n) — some test clients
         * use them. */
        char *eoh2 = strstr(buf, "\n\n");
        if (eoh2) {
            *out_buf = buf;
            *out_len = len;
            *out_body_offset = (size_t)(eoh2 - buf) + 2;
            return CMCP_OK;
        }
    }
}

/* Parse the request line + headers section. The block already
 * contains the entire header section (terminated). Body is read
 * separately based on Content-Length. */
static int parse_request_head(char *block, size_t body_offset,
                               http_request_t *out) {
    memset(out, 0, sizeof *out);
    /* Replace the empty line with a NUL so we work in the head only. */
    block[body_offset >= 2 ? body_offset - 2 : 0] = '\0';

    char *line = block;
    char *next = strchr(line, '\n');
    if (!next) return CMCP_EPROTOCOL;
    *next = '\0';
    if (next > line && next[-1] == '\r') next[-1] = '\0';

    /* Request line: METHOD SP TARGET SP HTTP/1.x */
    char *sp1 = strchr(line, ' ');
    if (!sp1) return CMCP_EPROTOCOL;
    *sp1 = '\0';
    char *target = sp1 + 1;
    char *sp2 = strchr(target, ' ');
    if (!sp2) return CMCP_EPROTOCOL;
    *sp2 = '\0';
    /* sp2+1 is the version; we don't validate beyond the prefix. */
    if (!starts_with_ci(sp2 + 1, "HTTP/1.")) return CMCP_EPROTOCOL;

    out->method = strdup(line);
    out->target = strdup(target);
    if (!out->method || !out->target) return CMCP_ENOMEM;

    /* Headers: Name: Value lines until empty. */
    line = next + 1;
    while (line && *line) {
        char *eol = strchr(line, '\n');
        if (eol) {
            *eol = '\0';
            if (eol > line && eol[-1] == '\r') eol[-1] = '\0';
        }
        if (*line == '\0') break;
        char *colon = strchr(line, ':');
        if (!colon) return CMCP_EPROTOCOL;
        *colon = '\0';
        char *name  = trim_inplace(line);
        char *value = trim_inplace(colon + 1);
        if (out->n_headers >= MAX_HEADERS) return CMCP_EPROTOCOL;

        out->headers[out->n_headers].name  = strdup(name);
        out->headers[out->n_headers].value = strdup(value);
        if (!out->headers[out->n_headers].name ||
            !out->headers[out->n_headers].value) return CMCP_ENOMEM;
        out->n_headers++;

        line = eol ? eol + 1 : NULL;
    }
    return CMCP_OK;
}

/* Read exactly `len` bytes into a fresh buffer (NUL-terminated). */
static int read_exact(int fd, size_t len, char **out, char *prefix,
                       size_t prefix_len) {
    char *buf = (char *)malloc(len + 1);
    if (!buf) return CMCP_ENOMEM;
    size_t have = 0;
    if (prefix_len) {
        size_t take = prefix_len > len ? len : prefix_len;
        memcpy(buf, prefix, take);
        have = take;
    }
    while (have < len) {
        ssize_t n = recv(fd, buf + have, len - have, 0);
        if (n <= 0) { free(buf); return CMCP_EIO; }
        have += (size_t)n;
    }
    buf[len] = '\0';
    *out = buf;
    return CMCP_OK;
}

/* Read one full HTTP request from fd. */
static int http_read_request(int fd, http_request_t *out) {
    char *block = NULL; size_t block_len = 0, body_off = 0;
    int rc = read_headers_block(fd, &block, &block_len, &body_off);
    if (rc != CMCP_OK) return rc;

    rc = parse_request_head(block, body_off, out);
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
        if (v < 0 || end == cl || *end != '\0' ||
            (size_t)v > HTTP_MAX_BODY_BYTES) {
            free(block); http_request_clear(out); return CMCP_EPROTOCOL;
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
        rc = read_exact(fd, body_len, &body, prefix, prefix_len);
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
        ssize_t n = read(fd, raw, sizeof raw);
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
    volatile int       shutting_down;

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

    /* SSE bookkeeping. */
    pthread_mutex_t    sse_mu;
    sse_conn_t        *sse_head;
} http_impl_t;

/* Forward decls. */
static void *acceptor_main(void *arg);
static void  handle_one_connection(http_impl_t *impl, int fd);
static void  release_pending_failure(http_impl_t *impl);

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
    /* Hold the connection open until either:
     *   - the client disconnects (recv returns 0/error)
     *   - the transport shuts down (close fd or signal)
     *
     * Phase 2.4 will replace the loop body with "drain notification
     * queue; SSE-emit each frame." For 2.1 we just block on poll. */
    for (;;) {
        if (c->owner->shutting_down) break;
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
    }
    cmcp_json_free(j);
    return k;
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
            !impl->shutting_down) {
        pthread_cond_wait(&impl->write_cv, &impl->slot_mu);
    }
    if (impl->shutting_down) {
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
    pthread_cond_signal(&impl->read_cv);

    /* Notifications and JSON-RPC responses produce no upper-layer
     * reply. Per spec, those POSTs get 202 Accepted with no body.
     * Wait until server.c consumes the request (so we don't race the
     * close+free path) and then return 202. */
    if (kind.is_notif || !kind.is_request) {
        while (impl->req_present && !impl->shutting_down) {
            pthread_cond_wait(&impl->write_cv, &impl->slot_mu);
        }
        pthread_mutex_unlock(&impl->slot_mu);
        http_write_response(fd, 202, "Accepted", "text/plain",
                              NULL, "", 0);
        return;
    }

    /* Wait for the response. */
    while (!impl->resp_present && !impl->shutting_down) {
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

    sse_conn_t *c = (sse_conn_t *)calloc(1, sizeof *c);
    if (!c) { close(fd); return; }
    c->fd    = fd;
    c->owner = impl;

    pthread_mutex_lock(&impl->sse_mu);
    c->next = impl->sse_head;
    impl->sse_head = c;
    pthread_mutex_unlock(&impl->sse_mu);

    if (pthread_create(&c->thread, NULL, sse_holder_main, c) != 0) {
        pthread_mutex_lock(&impl->sse_mu);
        impl->sse_head = c->next;
        pthread_mutex_unlock(&impl->sse_mu);
        close(c->fd);
        free(c);
    } else {
        pthread_detach(c->thread);
    }
}

static void handle_one_connection(http_impl_t *impl, int fd) {
    http_request_t req = {0};
    int rc = http_read_request(fd, &req);
    if (rc != CMCP_OK) {
        if (rc == CMCP_EUNSUPPORTED) {
            reply_error(fd, 501, "Not Implemented",
                         "chunked transfer encoding not supported\n");
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

    if (is_post_mcp) {
        handle_post(impl, fd, &req);
        close(fd);
    } else if (is_get_mcp) {
        const char *acc = http_header_get(&req, "Accept");
        if (acc && strstr(acc, "text/event-stream")) {
            handle_get_sse(impl, fd, &req);
            /* fd ownership moves to the holder thread; do NOT close. */
        } else {
            reply_error(fd, 406, "Not Acceptable",
                         "GET /mcp requires Accept: text/event-stream\n");
            close(fd);
        }
    } else {
        reply_error(fd, 404, "Not Found", "no such endpoint\n");
        close(fd);
    }
    http_request_clear(&req);
}

/* ====================================================================== */
/* Acceptor                                                                 */
/* ====================================================================== */

static void *acceptor_main(void *arg) {
    http_impl_t *impl = (http_impl_t *)arg;
    while (!impl->shutting_down) {
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
    while (!impl->req_present && !impl->shutting_down) {
        pthread_cond_wait(&impl->read_cv, &impl->slot_mu);
    }
    if (impl->shutting_down && !impl->req_present) {
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

/* Frame `body` as an SSE event and send to one fd. Each event is a
 * single `data: <body>\n\n`. Newlines inside `body` are emitted as
 * separate `data:` continuation lines, per the EventSource spec. */
static int sse_emit_event(int fd, const char *body, size_t len) {
    /* Use a small write buffer to avoid one send() per byte. */
    size_t cap = len + 16;
    char *out = (char *)malloc(cap);
    if (!out) return CMCP_ENOMEM;
    size_t n = 0;
    /* Open the line. */
    memcpy(out + n, "data: ", 6); n += 6;
    /* Walk body, splitting on '\n' into continuation `data:` lines. */
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
            memcpy(out + n, "data: ", 6); n += 6;
        } else {
            out[n++] = c;
        }
    }
    /* Close the event with a blank line. */
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
        sse_conn_t *c = impl->sse_head;
        while (c) {
            sse_emit_event(c->fd, buf, len);
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

static void http_close_fn(cmcp_transport_t *t) {
    if (!t) return;
    http_impl_t *impl = (http_impl_t *)t->impl;
    if (impl) {
        impl->shutting_down = 1;

        /* Unblock the acceptor thread; closing the listen fd makes
         * accept/poll return immediately. */
        if (impl->listen_fd >= 0) {
            shutdown(impl->listen_fd, SHUT_RDWR);
            close(impl->listen_fd);
            impl->listen_fd = -1;
        }
        if (impl->acceptor_started) {
            pthread_join(impl->acceptor, NULL);
            impl->acceptor_started = 0;
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

    struct addrinfo *res = NULL;
    int gai = getaddrinfo(host && *host ? host : NULL, port_buf, &hints, &res);
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

    t->impl     = impl;
    t->read_fn  = http_read_fn;
    t->write_fn = http_write_fn;
    t->close_fn = http_close_fn;

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
