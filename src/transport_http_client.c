/* Streamable HTTP transport — client side.
 *
 * Connects to a Streamable HTTP MCP server. A single transport instance
 * holds:
 *
 *   - The base URL (full /mcp endpoint).
 *   - A latched `Mcp-Session-Id`, populated from the first `initialize`
 *     POST response. Subsequent POSTs propagate it via header.
 *   - A frame queue that read_fn pops. Two producers feed it:
 *       1) write_fn: synchronous POST → 200 + JSON-RPC response body
 *          (or 202 Accepted with no body).
 *       2) SSE reader thread: long-lived `GET /mcp` with
 *          `Accept: text/event-stream`, parsing each `data: <json>\n\n`
 *          block as one frame.
 *
 * write_fn is fully reentrant — each call uses a fresh curl easy
 * handle, so the application can have multiple POSTs in flight from
 * different threads. Their responses arrive on the queue and are
 * routed by client.c's reader thread via JSON-RPC id.
 *
 * Construction is cheap: no network I/O until the first write_fn.
 * The SSE thread is started immediately but parks on a condvar
 * until the session id has been latched.
 *
 * Shutdown: set `shutting_down`, broadcast the session and queue
 * condvars (to release any blocked threads), and the SSE curl
 * progress callback returns non-zero to abort the long-poll. Then
 * join the SSE thread and free state. */

#define _POSIX_C_SOURCE 200809L

#include "cmcp.h"
#include "cmcp_transport.h"

#include <curl/curl.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>

/* ====================================================================== */
/* Frame queue                                                              */
/* ====================================================================== */

typedef struct frame_node {
    char              *data;        /* NUL-terminated, owned */
    size_t             len;
    struct frame_node *next;
} frame_node_t;

typedef struct {
    char            *url;

    pthread_mutex_t  q_mu;
    pthread_cond_t   q_cv;
    frame_node_t    *q_head, *q_tail;

    pthread_mutex_t  session_mu;
    pthread_cond_t   session_cv;
    char             session_id[64];
    int              session_set;

    pthread_t        sse_thread;
    int              sse_started;

    /* Highest SSE event id observed (MCP 2025-11-25 SEP-1699). The
     * reader updates this as each event with an `id:` field is emitted;
     * the thread main snapshot it under sse_id_mu before each reconnect
     * to populate the `Last-Event-Id` header. 0 = no event seen yet. */
    pthread_mutex_t  sse_id_mu;
    uint64_t         last_event_id;

    /* Atomic flag — read from sse_progress_cb under libcurl's thread
     * without our mutex (line 341 below), so it needs real cross-
     * thread visibility, not just the volatile compiler hint. The
     * mutex-guarded read sites below use atomic_load too for
     * consistency; the mutex still provides the cv ordering. */
    atomic_int       shutting_down;
} http_client_impl_t;

/* Push one frame onto the queue. Copies data. Signals one waiter. */
static int queue_push(http_client_impl_t *impl,
                       const char *data, size_t len) {
    frame_node_t *n = (frame_node_t *)malloc(sizeof *n);
    if (!n) return CMCP_ENOMEM;
    n->data = (char *)malloc(len + 1);
    if (!n->data) { free(n); return CMCP_ENOMEM; }
    memcpy(n->data, data, len);
    n->data[len] = '\0';
    n->len  = len;
    n->next = NULL;

    pthread_mutex_lock(&impl->q_mu);
    if (impl->q_tail) impl->q_tail->next = n;
    else              impl->q_head       = n;
    impl->q_tail = n;
    pthread_cond_signal(&impl->q_cv);
    pthread_mutex_unlock(&impl->q_mu);
    return CMCP_OK;
}

static int queue_pop(http_client_impl_t *impl,
                      char **out_buf, size_t *out_len) {
    pthread_mutex_lock(&impl->q_mu);
    while (!impl->q_head &&
           !atomic_load_explicit(&impl->shutting_down,
                                  memory_order_relaxed)) {
        pthread_cond_wait(&impl->q_cv, &impl->q_mu);
    }
    if (!impl->q_head) {
        pthread_mutex_unlock(&impl->q_mu);
        return CMCP_EIO;
    }
    frame_node_t *n = impl->q_head;
    impl->q_head = n->next;
    if (!impl->q_head) impl->q_tail = NULL;
    pthread_mutex_unlock(&impl->q_mu);

    *out_buf = n->data;
    *out_len = n->len;
    free(n);                /* data ownership transferred */
    return CMCP_OK;
}

/* Latch the session id (idempotent). Returns 1 if this call was the
 * one that set it, 0 if already set or no-op. */
static int latch_session_id(http_client_impl_t *impl, const char *id) {
    if (!id || !*id) return 0;
    pthread_mutex_lock(&impl->session_mu);
    int was_set = impl->session_set;
    if (!was_set) {
        size_t n = strlen(id);
        if (n >= sizeof impl->session_id) n = sizeof impl->session_id - 1;
        memcpy(impl->session_id, id, n);
        impl->session_id[n] = '\0';
        impl->session_set = 1;
        pthread_cond_broadcast(&impl->session_cv);
    }
    pthread_mutex_unlock(&impl->session_mu);
    return !was_set;
}

/* ====================================================================== */
/* POST round-trip                                                          */
/* ====================================================================== */

typedef struct {
    char  *body;            /* response body buffer (grown by callback) */
    size_t len;
    size_t cap;
    char   session_id_seen[64];
} post_state_t;

static size_t post_write_cb(char *ptr, size_t size, size_t nmemb, void *ud) {
    post_state_t *ps = (post_state_t *)ud;
    size_t total = size * nmemb;
    if (ps->len + total + 1 > ps->cap) {
        size_t nc = ps->cap ? ps->cap * 2 : 1024;
        while (nc < ps->len + total + 1) nc *= 2;
        char *nb = (char *)realloc(ps->body, nc);
        if (!nb) return 0;
        ps->body = nb;
        ps->cap = nc;
    }
    memcpy(ps->body + ps->len, ptr, total);
    ps->len += total;
    ps->body[ps->len] = '\0';
    return total;
}

static size_t post_header_cb(char *buf, size_t size, size_t nmemb, void *ud) {
    post_state_t *ps = (post_state_t *)ud;
    size_t total = size * nmemb;
    static const char hname[] = "Mcp-Session-Id:";
    size_t hlen = sizeof hname - 1;
    if (total >= hlen && strncasecmp(buf, hname, hlen) == 0) {
        const char *v = buf + hlen;
        size_t vlen = total - hlen;
        while (vlen && (*v == ' ' || *v == '\t')) { v++; vlen--; }
        while (vlen && (v[vlen - 1] == '\r' || v[vlen - 1] == '\n' ||
                         v[vlen - 1] == ' '  || v[vlen - 1] == '\t'))
            vlen--;
        if (vlen > 0 && vlen < sizeof ps->session_id_seen) {
            memcpy(ps->session_id_seen, v, vlen);
            ps->session_id_seen[vlen] = '\0';
        }
    }
    return total;
}

static int do_post(http_client_impl_t *impl,
                    const char *body, size_t body_len) {
    CURL *c = curl_easy_init();
    if (!c) return CMCP_ENOMEM;

    struct curl_slist *headers = NULL;
    headers = curl_slist_append(headers, "Content-Type: application/json");
    headers = curl_slist_append(headers,
                "Accept: application/json, text/event-stream");

    char sid_header[96] = {0};
    pthread_mutex_lock(&impl->session_mu);
    int post_init = impl->session_set;
    if (post_init) {
        snprintf(sid_header, sizeof sid_header,
                  "Mcp-Session-Id: %s", impl->session_id);
    }
    pthread_mutex_unlock(&impl->session_mu);
    if (sid_header[0]) headers = curl_slist_append(headers, sid_header);
    /* Once the handshake has latched a session, every subsequent
     * request MUST carry the negotiated protocol version. */
    if (post_init)
        headers = curl_slist_append(headers,
                    "MCP-Protocol-Version: " CMCP_PROTOCOL_VERSION);

    post_state_t ps = {0};

    curl_easy_setopt(c, CURLOPT_URL,            impl->url);
    curl_easy_setopt(c, CURLOPT_POST,           1L);
    curl_easy_setopt(c, CURLOPT_POSTFIELDS,     body);
    curl_easy_setopt(c, CURLOPT_POSTFIELDSIZE,  (long)body_len);
    curl_easy_setopt(c, CURLOPT_HTTPHEADER,     headers);
    curl_easy_setopt(c, CURLOPT_WRITEFUNCTION,  post_write_cb);
    curl_easy_setopt(c, CURLOPT_WRITEDATA,      &ps);
    curl_easy_setopt(c, CURLOPT_HEADERFUNCTION, post_header_cb);
    curl_easy_setopt(c, CURLOPT_HEADERDATA,     &ps);
    curl_easy_setopt(c, CURLOPT_TIMEOUT,        30L);
    curl_easy_setopt(c, CURLOPT_NOSIGNAL,       1L);

    CURLcode rc = curl_easy_perform(c);
    long status = 0;
    curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &status);

    curl_easy_cleanup(c);
    curl_slist_free_all(headers);

    if (rc != CURLE_OK) {
        free(ps.body);
        return CMCP_EIO;
    }

    if (ps.session_id_seen[0]) {
        latch_session_id(impl, ps.session_id_seen);
    }

    /* Surface non-success HTTP status to the caller. Without this, a
     * 503/4xx response was silently dropped — write_fn returned OK,
     * the body never reached the queue, and the host's pending
     * cmcp_client_request hung forever waiting for a JSON-RPC frame.
     *
     *   503 → EAGAIN  (rate limited; server sends Retry-After)
     *   4xx → EIO     (request-level failure; not retriable as-is)
     *   5xx → EIO     (server-side failure other than rate limiting)
     *
     * Client.c's call_async tears the pending entry down on a write
     * error, so the host caller observes the right return code rather
     * than a hang. The body (if any) is discarded — it's an HTTP error
     * page, not a JSON-RPC frame. */
    int push_rc = CMCP_OK;
    if (status == 200 && ps.len > 0) {
        push_rc = queue_push(impl, ps.body, ps.len);
    } else if (status == 200 || status == 202) {
        /* 200 with empty body and 202 Accepted are both fine — the
         * latter is the spec-defined notification ack with no body. */
    } else if (status == 503) {
        push_rc = CMCP_EAGAIN;
    } else if (status >= 400) {
        push_rc = CMCP_EIO;
    }
    free(ps.body);
    return push_rc;
}

/* ====================================================================== */
/* SSE parser + reader thread                                              */
/* ====================================================================== */

typedef struct {
    char  *event_data;      /* concatenated `data:` field for the
                              current event */
    size_t event_data_len;
    size_t event_data_cap;

    char  *line_buf;        /* partial in-progress line */
    size_t line_buf_len;
    size_t line_buf_cap;

    /* `id:` field accumulated for the current event (SEP-1699). On
     * emit, parsed to uint64 and used to advance impl->last_event_id
     * so a reconnect can include the right Last-Event-Id header. */
    char     event_id_buf[32];
    int      event_id_present;

    http_client_impl_t *impl;
} sse_state_t;

static void sse_emit(sse_state_t *st) {
    if (st->event_data_len == 0) {
        /* Empty events (id-only heartbeats) still advance the id. */
        if (st->event_id_present) {
            char *end;
            unsigned long long parsed = strtoull(st->event_id_buf, &end, 10);
            if (end != st->event_id_buf && *end == '\0') {
                pthread_mutex_lock(&st->impl->sse_id_mu);
                if ((uint64_t)parsed > st->impl->last_event_id) {
                    st->impl->last_event_id = (uint64_t)parsed;
                }
                pthread_mutex_unlock(&st->impl->sse_id_mu);
            }
            st->event_id_present = 0;
        }
        return;
    }
    queue_push(st->impl, st->event_data, st->event_data_len);
    st->event_data_len = 0;
    if (st->event_id_present) {
        char *end;
        unsigned long long parsed = strtoull(st->event_id_buf, &end, 10);
        if (end != st->event_id_buf && *end == '\0') {
            pthread_mutex_lock(&st->impl->sse_id_mu);
            if ((uint64_t)parsed > st->impl->last_event_id) {
                st->impl->last_event_id = (uint64_t)parsed;
            }
            pthread_mutex_unlock(&st->impl->sse_id_mu);
        }
        st->event_id_present = 0;
    }
}

static int sse_data_append(sse_state_t *st, const char *v, size_t vlen) {
    size_t need = st->event_data_len + vlen + (st->event_data_len ? 1 : 0) + 1;
    if (need > st->event_data_cap) {
        size_t nc = st->event_data_cap ? st->event_data_cap * 2 : 256;
        while (nc < need) nc *= 2;
        char *nb = (char *)realloc(st->event_data, nc);
        if (!nb) return -1;
        st->event_data = nb;
        st->event_data_cap = nc;
    }
    if (st->event_data_len > 0) st->event_data[st->event_data_len++] = '\n';
    memcpy(st->event_data + st->event_data_len, v, vlen);
    st->event_data_len += vlen;
    st->event_data[st->event_data_len] = '\0';
    return 0;
}

static void sse_consume_line(sse_state_t *st, const char *line, size_t len) {
    /* Strip trailing \r (lines may end with \r\n or just \n). */
    while (len && line[len - 1] == '\r') len--;

    if (len == 0) {                  /* event terminator */
        sse_emit(st);
        return;
    }
    if (line[0] == ':') return;       /* comment */

    const char *colon = (const char *)memchr(line, ':', len);
    if (!colon) return;               /* malformed, ignore */

    size_t fname_len = (size_t)(colon - line);
    const char *fval = colon + 1;
    size_t fval_len = len - fname_len - 1;
    if (fval_len > 0 && fval[0] == ' ') { fval++; fval_len--; }

    if (fname_len == 4 && memcmp(line, "data", 4) == 0) {
        sse_data_append(st, fval, fval_len);
    } else if (fname_len == 2 && memcmp(line, "id", 2) == 0) {
        /* Capture the id; advancement happens on event emit so the
         * id is treated as ATOMIC with the event it labels. Truncate
         * to fit our small buffer — any well-formed uint64 fits. */
        size_t take = fval_len;
        if (take >= sizeof st->event_id_buf) take = sizeof st->event_id_buf - 1;
        memcpy(st->event_id_buf, fval, take);
        st->event_id_buf[take] = '\0';
        st->event_id_present = 1;
    }
    /* Other fields (event:, retry:) are ignored in v0.2. */
}

static int sse_line_push(sse_state_t *st, char ch) {
    if (st->line_buf_len + 2 > st->line_buf_cap) {
        size_t nc = st->line_buf_cap ? st->line_buf_cap * 2 : 256;
        char *nb = (char *)realloc(st->line_buf, nc);
        if (!nb) return -1;
        st->line_buf = nb;
        st->line_buf_cap = nc;
    }
    /* The grow branch above re-establishes the invariant
     * `line_buf != NULL ⇒ line_buf_cap > 0`; the analyser can't prove
     * that across the realloc boundary, so make it concrete here. */
    if (!st->line_buf) return -1;
    st->line_buf[st->line_buf_len++] = ch;
    st->line_buf[st->line_buf_len] = '\0';
    return 0;
}

/* libcurl's CURLOPT_WRITEFUNCTION signature requires `char *ptr` (non-const);
 * we don't mutate it, but the typedef is what it is. */
static size_t sse_write_cb(char *ptr, size_t size, size_t nmemb, void *ud) {  // NOLINT(readability-non-const-parameter)
    sse_state_t *st = (sse_state_t *)ud;
    size_t total = size * nmemb;

    for (size_t i = 0; i < total; i++) {
        char ch = ptr[i];
        if (ch == '\n') {
            sse_consume_line(st, st->line_buf, st->line_buf_len);
            st->line_buf_len = 0;
            if (st->line_buf) st->line_buf[0] = '\0';
        } else {
            if (sse_line_push(st, ch) != 0) return 0;
        }
    }
    return total;
}

static int sse_progress_cb(void *ud, curl_off_t dlt, curl_off_t dln,
                             curl_off_t ult, curl_off_t uln) {
    (void)dlt; (void)dln; (void)ult; (void)uln;
    http_client_impl_t *impl = (http_client_impl_t *)ud;
    return atomic_load_explicit(&impl->shutting_down,
                                 memory_order_relaxed) ? 1 : 0;
}

static void *sse_thread_main(void *arg) {
    http_client_impl_t *impl = (http_client_impl_t *)arg;

    /* Park until the first POST has latched a session id. */
    pthread_mutex_lock(&impl->session_mu);
    while (!impl->session_set &&
           !atomic_load_explicit(&impl->shutting_down,
                                  memory_order_relaxed)) {
        pthread_cond_wait(&impl->session_cv, &impl->session_mu);
    }
    int shut = atomic_load_explicit(&impl->shutting_down,
                                     memory_order_relaxed);
    char sid_header[96];
    snprintf(sid_header, sizeof sid_header,
              "Mcp-Session-Id: %s", impl->session_id);
    pthread_mutex_unlock(&impl->session_mu);
    if (shut) return NULL;

    sse_state_t st = {0};
    st.impl = impl;

    /* Reconnect loop (MCP 2025-11-25 SEP-1699). curl_easy_perform
     * returns either when the server closes the SSE stream or when
     * the progress callback aborts on shutdown. Servers are allowed
     * to disconnect at any time; clients must support resumption via
     * Last-Event-Id. We loop with exponential backoff (100ms → 5s
     * cap) until shutdown, sending Last-Event-Id on every attempt
     * after the first event has been seen. */
    long backoff_ms = 100;
    for (;;) {
        if (atomic_load_explicit(&impl->shutting_down,
                                  memory_order_relaxed)) break;

        pthread_mutex_lock(&impl->sse_id_mu);
        uint64_t resume = impl->last_event_id;
        pthread_mutex_unlock(&impl->sse_id_mu);

        char leid_header[64] = {0};
        if (resume > 0) {
            snprintf(leid_header, sizeof leid_header,
                      "Last-Event-Id: %llu", (unsigned long long)resume);
        }

        struct curl_slist *headers = NULL;
        headers = curl_slist_append(headers, "Accept: text/event-stream");
        headers = curl_slist_append(headers, "Cache-Control: no-cache");
        headers = curl_slist_append(headers, sid_header);
        headers = curl_slist_append(headers,
                    "MCP-Protocol-Version: " CMCP_PROTOCOL_VERSION);
        if (resume > 0) {
            headers = curl_slist_append(headers, leid_header);
        }

        CURL *c = curl_easy_init();
        if (!c) {
            curl_slist_free_all(headers);
            break;
        }
        curl_easy_setopt(c, CURLOPT_URL,              impl->url);
        curl_easy_setopt(c, CURLOPT_HTTPGET,          1L);
        curl_easy_setopt(c, CURLOPT_HTTPHEADER,       headers);
        curl_easy_setopt(c, CURLOPT_WRITEFUNCTION,    sse_write_cb);
        curl_easy_setopt(c, CURLOPT_WRITEDATA,        &st);
        curl_easy_setopt(c, CURLOPT_NOSIGNAL,         1L);
        curl_easy_setopt(c, CURLOPT_NOPROGRESS,       0L);
        curl_easy_setopt(c, CURLOPT_XFERINFOFUNCTION, sse_progress_cb);
        curl_easy_setopt(c, CURLOPT_XFERINFODATA,     impl);

        CURLcode perform_rc = curl_easy_perform(c);
        long status = 0;
        curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &status);
        curl_easy_cleanup(c);
        curl_slist_free_all(headers);

        if (atomic_load_explicit(&impl->shutting_down,
                                  memory_order_relaxed)) break;

        /* A successful long-poll that the server closed cleanly is the
         * polling shape the spec describes — reconnect immediately
         * (small backoff so we don't hot-loop if the server is in a
         * tight close cycle). On HTTP errors (4xx/5xx) or libcurl
         * connection failures back off more aggressively so we don't
         * spin against a dead endpoint. */
        long delay = (perform_rc == CURLE_OK && status == 200)
                       ? 50 : backoff_ms;
        struct timespec ts = { delay / 1000,
                                (long)(delay % 1000) * 1000000L };
        nanosleep(&ts, NULL);
        if (perform_rc != CURLE_OK || status >= 400) {
            backoff_ms = backoff_ms < 5000 ? backoff_ms * 2 : 5000;
        } else {
            backoff_ms = 100;
        }
    }

    free(st.event_data);
    free(st.line_buf);
    return NULL;
}

/* ====================================================================== */
/* Vtable                                                                   */
/* ====================================================================== */

static int http_client_read(cmcp_transport_t *t,
                              char **out_buf, size_t *out_len) {
    return queue_pop((http_client_impl_t *)t->impl, out_buf, out_len);
}

static int http_client_write(cmcp_transport_t *t,
                               const char *buf, size_t len) {
    return do_post((http_client_impl_t *)t->impl, buf, len);
}

/* Wake-only path (vtable wake_fn): signal everyone, don't free. */
static void http_client_wake(cmcp_transport_t *t) {
    if (!t) return;
    http_client_impl_t *impl = (http_client_impl_t *)t->impl;
    if (!impl) return;
    atomic_store_explicit(&impl->shutting_down, 1, memory_order_relaxed);
    pthread_mutex_lock(&impl->session_mu);
    pthread_cond_broadcast(&impl->session_cv);
    pthread_mutex_unlock(&impl->session_mu);
    pthread_mutex_lock(&impl->q_mu);
    pthread_cond_broadcast(&impl->q_cv);
    pthread_mutex_unlock(&impl->q_mu);
}

static void http_client_close(cmcp_transport_t *t) {
    if (!t) return;
    http_client_impl_t *impl = (http_client_impl_t *)t->impl;
    if (impl) {
        http_client_wake(t);  /* idempotent */

        if (impl->sse_started) {
            pthread_join(impl->sse_thread, NULL);
            impl->sse_started = 0;
        }

        /* Drain queue. */
        frame_node_t *n = impl->q_head;
        while (n) {
            frame_node_t *next = n->next;
            free(n->data);
            free(n);
            n = next;
        }

        free(impl->url);
        pthread_mutex_destroy(&impl->q_mu);
        pthread_cond_destroy(&impl->q_cv);
        pthread_mutex_destroy(&impl->session_mu);
        pthread_cond_destroy(&impl->session_cv);
        pthread_mutex_destroy(&impl->sse_id_mu);
        free(impl);
    }
    free(t);
}

/* ====================================================================== */
/* Constructor                                                              */
/* ====================================================================== */

static pthread_once_t curl_init_once = PTHREAD_ONCE_INIT;
static void curl_init_handler(void) {
    curl_global_init(CURL_GLOBAL_DEFAULT);
}

cmcp_transport_t *cmcp_transport_http_connect(const char *url) {
    if (!url || !*url) return NULL;
    pthread_once(&curl_init_once, curl_init_handler);

    http_client_impl_t *impl = (http_client_impl_t *)calloc(1, sizeof *impl);
    cmcp_transport_t   *t    = (cmcp_transport_t *)  calloc(1, sizeof *t);
    if (!impl || !t) goto fail;

    impl->url = strdup(url);
    if (!impl->url) goto fail;

    pthread_mutex_init(&impl->q_mu, NULL);
    pthread_cond_init (&impl->q_cv, NULL);
    pthread_mutex_init(&impl->session_mu, NULL);
    pthread_cond_init (&impl->session_cv, NULL);
    pthread_mutex_init(&impl->sse_id_mu, NULL);

    t->impl     = impl;
    t->read_fn  = http_client_read;
    t->write_fn = http_client_write;
    t->close_fn = http_client_close;
    t->wake_fn  = http_client_wake;

    if (pthread_create(&impl->sse_thread, NULL, sse_thread_main, impl) != 0) {
        pthread_mutex_destroy(&impl->q_mu);
        pthread_cond_destroy(&impl->q_cv);
        pthread_mutex_destroy(&impl->session_mu);
        pthread_cond_destroy(&impl->session_cv);
        pthread_mutex_destroy(&impl->sse_id_mu);
        goto fail;
    }
    impl->sse_started = 1;

    return t;

fail:
    if (impl) { free(impl->url); free(impl); }
    free(t);
    return NULL;
}
