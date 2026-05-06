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
     * read_fn. Upper layers that run a reader thread should signal
     * the reader to exit (e.g. pthread_kill) and join before close. */
    void (*close_fn)(cmcp_transport_t *t);

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

#endif
