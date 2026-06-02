/**
 * @file cmcp_transport.h
 * @brief Transport vtable + stdio/HTTP constructors.
 *
 * Plugin layer: a `cmcp_transport_t` is a `(read, write, close,
 * optional wake)` vtable plus a void* context. cMCP ships three
 * backends:
 *
 *   - **stdio** (`cmcp_transport_stdio_new` / `_new_fds`): newline-
 *     delimited JSON over stdin/stdout (or any pre-opened fd pair).
 *   - **HTTP server** (`cmcp_transport_http_listen`): Streamable HTTP
 *     hand-rolled on top of `socket()` / `accept()`.
 *   - **HTTP client** (`cmcp_transport_http_connect`): Streamable
 *     HTTP via libcurl with a background SSE reader thread.
 *
 * Servers and clients borrow a transport; the *caller* still closes
 * it. The transport layer is intentionally message-agnostic, with
 * one principled exception in the HTTP backend (which has to peek
 * at the JSON-RPC body to distinguish requests-needing-a-response
 * from notifications-getting-202-Accepted).
 */
#ifndef CMCP_TRANSPORT_H
#define CMCP_TRANSPORT_H

#include <stddef.h>

/* ====================================================================== */
/* Transport vtable                                                        */
/* ====================================================================== */
/* A cmcp_transport_t moves opaque byte frames between two MCP peers. It
 * never inspects message contents — that's the RPC layer's job. Frames
 * are 1:1 with JSON-RPC messages: one read() returns one whole message,
 * one write() emits one whole message.
 *
 * Concrete transports (stdio, http) plug in by populating the vtable
 * fields and storing impl-private state in `impl`. */

typedef struct cmcp_transport cmcp_transport_t;

struct cmcp_transport {
    /* Block until one full frame is available.
     *
     * On success: returns CMCP_OK; *out_buf is a freshly malloc'd
     * NUL-terminated buffer (caller frees with free()), *out_len is its
     * length in bytes (excluding the NUL).
     *
     * On clean EOF or any read failure: returns CMCP_EIO. The caller
     * should treat this as "the conversation is over." */
    int  (*read_fn)(cmcp_transport_t *t, char **out_buf, size_t *out_len);

    /* Atomically emit one full frame. Adds whatever framing the
     * transport requires (e.g. trailing newline for stdio). Never
     * writes partially; safe to call concurrently from multiple
     * threads (each transport guards writes with its own mutex).
     *
     * Returns CMCP_OK on success, CMCP_EIO on write failure,
     * CMCP_EINVAL if buf contains bytes that would break framing
     * (e.g. embedded '\n' for stdio). */
    int  (*write_fn)(cmcp_transport_t *t, const char *buf, size_t len);

    /* Tear down the transport: close any owned file descriptors,
     * release the impl state, free `t` itself. Safe to call with
     * NULL. After close, t is invalid.
     *
     * close_fn must NOT be called while another thread is inside
     * read_fn. Upper layers that run a reader thread should call
     * wake_fn (or pthread_kill, for transports that block on a
     * syscall returning EINTR) to wake the reader, join, then close. */
    void (*close_fn)(cmcp_transport_t *t);

    /* Optional. Wake any thread blocked inside read_fn so it returns
     * CMCP_EIO. Idempotent and safe to call multiple times. Does NOT
     * free anything — close_fn still has to be called separately.
     *
     * Required for transports whose read_fn blocks on a userspace
     * primitive (condvar, futex, etc.) that doesn't surface signals.
     * Stdio leaves this NULL — its read_fn blocks on a read syscall
     * which the upper layer interrupts via pthread_kill. */
    void (*wake_fn)(cmcp_transport_t *t);

    /* Optional. Look up a header from the request currently being
     * handled, case-insensitively, returning a borrowed pointer (valid
     * only until the next frame is read) or NULL if absent. Lets a
     * handler reach transport-level metadata it otherwise couldn't —
     * e.g. an HTTP `Authorization` value so a host can implement
     * per-tool auth. Only meaningful for request/response transports
     * that carry headers; stdio leaves this NULL (always "no header").
     *
     * Safe because the HTTP transport handles exactly one request at a
     * time (single acceptor, single-slot handoff), so "the current
     * request" is unambiguous for the duration of a handler call. */
    const char *(*request_header_fn)(cmcp_transport_t *t, const char *name);

    void *impl;
};

/* Convenience wrappers — exactly equivalent to dispatching through the
 * vtable. Use these from upper layers so call sites read naturally. */

static inline int cmcp_transport_read(cmcp_transport_t *t,
                                       char **out_buf, size_t *out_len) {
    return t->read_fn(t, out_buf, out_len);
}

static inline int cmcp_transport_write(cmcp_transport_t *t,
                                        const char *buf, size_t len) {
    return t->write_fn(t, buf, len);
}

static inline void cmcp_transport_close(cmcp_transport_t *t) {
    if (t) t->close_fn(t);
}

static inline void cmcp_transport_wake(cmcp_transport_t *t) {
    if (t && t->wake_fn) t->wake_fn(t);
}

static inline const char *cmcp_transport_request_header(cmcp_transport_t *t,
                                                         const char *name) {
    if (t && t->request_header_fn) return t->request_header_fn(t, name);
    return NULL;
}

/* ====================================================================== */
/* stdio transport (newline-delimited JSON)                                */
/* ====================================================================== */
/* Frames are JSON messages followed by a single '\n'. The transport
 * refuses writes containing raw newlines so framing can never desync.
 *
 * IMPORTANT: when this transport owns the process's stdin/stdout, NO
 * other code in the program may write to stdout — every byte must be
 * a wire frame from this transport. Use stderr for logging. */

/* Open a transport over the calling process's stdin / stdout. Does NOT
 * close stdin/stdout on close(). Most common case: a server invoked
 * by a host that pipes to it. */
cmcp_transport_t *cmcp_transport_stdio_new(void);

/* Open a transport over the given read/write file descriptors. Takes
 * ownership: close() will close both FDs. Used for spawning children
 * (read = child's stdout, write = child's stdin) and for tests. */
cmcp_transport_t *cmcp_transport_stdio_new_fds(int read_fd, int write_fd);

/* ====================================================================== */
/* Streamable HTTP transport — server side                                 */
/* ====================================================================== */
/* Streamable HTTP per MCP spec 2025-11-25: a single `/mcp` endpoint that
 * accepts POST (request → response) and GET with `Accept:
 * text/event-stream` (SSE upgrade for server-to-client streams). Session
 * is identified by the `Mcp-Session-Id` header — minted on the first
 * `initialize` POST and required on every subsequent request.
 *
 * Threading: the transport owns one acceptor thread that loops
 * accept(). Each connection is handled inline on the acceptor (POSTs
 * are 1-shot and serialized; SSE connections detach to a background
 * holder thread). read_fn blocks until a POST body arrives; write_fn
 * pairs with the most recent unanswered request and unblocks the
 * acceptor's POST handler so it can send the HTTP response.
 *
 * v0.2 limits: one logical session per transport, no TLS (deploy
 * behind nginx/caddy), no HTTP keep-alive (one request per
 * connection), Content-Length only (no chunked transfer encoding).
 *
 * `host` selects the bind address. A NULL or empty `host` binds the
 * loopback interface (127.0.0.1) — a safe default that is unreachable
 * off-box. Pass an explicit address (e.g. "0.0.0.0" or "::") to listen
 * on a public interface; if you do so without setting
 * CMCP_HTTP_ALLOWED_ORIGINS, the transport emits a one-shot stderr
 * warning, since a non-loopback bind with no Origin allow-list has no
 * DNS-rebinding defense. The transport owns a single listen fd and binds
 * the first address `getaddrinfo` returns for `host`, so prefer a literal
 * address over a name that resolves to several families.
 *
 * Returns NULL on bind/listen failure (e.g. port in use, permission
 * denied). The transport begins accepting immediately. */
cmcp_transport_t *cmcp_transport_http_listen(const char *host,
                                              unsigned short port);

/* ====================================================================== */
/* Streamable HTTP transport — client side                                 */
/* ====================================================================== */
/* Connect to a Streamable HTTP MCP server. `url` is the full /mcp
 * endpoint, e.g. "http://127.0.0.1:8080/mcp" or
 * "https://example.com/api/mcp" (TLS handled by libcurl).
 *
 * Construction is cheap — no network I/O happens until the first
 * write_fn fires, which POSTs the frame and synchronously waits for
 * the HTTP response. The transport runs a background SSE reader
 * thread: once a session id has been latched (from the first
 * `initialize` response), it opens a long-lived `GET /mcp` with
 * `Accept: text/event-stream` and feeds each `data: <json>\n\n`
 * block back through `read_fn` as a frame.
 *
 * Frames returned by read_fn come from two sources merged by a
 * thread-safe queue: synchronous POST responses (HTTP 200 + body)
 * and SSE-streamed server-pushed messages. Notification POSTs
 * (HTTP 202 Accepted) carry no body and don't enqueue.
 *
 * v0.2 limits: no auto-reconnect on SSE drop; TLS verification uses
 * the system CA bundle (no custom CA pinning); no exposed knobs for
 * proxies (set CURL env vars if you need them). */
cmcp_transport_t *cmcp_transport_http_connect(const char *url);

#endif
