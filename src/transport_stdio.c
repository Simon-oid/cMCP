/* fdopen() — POSIX.1-2008. */
#define _POSIX_C_SOURCE 200809L

#include "cmcp.h"
#include "cmcp_transport.h"

#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

typedef struct {
    FILE  *in;
    FILE  *out;
    int    owns_streams;     /* fclose() in/out on close? */
    char  *line_buf;
    size_t line_cap;
    pthread_mutex_t write_mu;
} stdio_impl_t;

/* ---------------------------------------------------------------------- *
 * Frame-size cap (stdio analogue of the HTTP body cap).
 *
 * getline() grows its buffer without limit, so a peer streaming bytes
 * with no newline can drive unbounded allocation until the process OOMs.
 * That matters in both wire roles: cmcp_client_connect_stdio reads a
 * forked server's stdout through this same path, so a hostile/buggy MCP
 * server could OOM the *client*. We cap the per-frame size like the HTTP
 * transport caps the request body.
 *
 *   CMCP_STDIO_MAX_FRAME   default 16 MiB   (<=0 disables)
 *
 * Snapshotted once via pthread_once — zero per-read getenv() cost.
 * Over-budget frame → CMCP_EPROTOCOL (matching the HTTP overflow path).
 * ---------------------------------------------------------------------- */
#define CMCP_STDIO_MAX_FRAME_DEFAULT  ((size_t)16 * 1024 * 1024)

static size_t        g_stdio_max_frame = CMCP_STDIO_MAX_FRAME_DEFAULT;
static pthread_once_t g_stdio_caps_once = PTHREAD_ONCE_INIT;

static void stdio_caps_init(void) {
    const char *v = getenv("CMCP_STDIO_MAX_FRAME");
    if (v && *v) {
        char *end; long long x = strtoll(v, &end, 10);
        if (end != v && *end == '\0') {
            g_stdio_max_frame = (x <= 0) ? 0 : (size_t)x;
        }
    }
}

static size_t stdio_max_frame(void) {
    pthread_once(&g_stdio_caps_once, stdio_caps_init);
    return g_stdio_max_frame;
}

/* Bounded line read: like getline() into s->line_buf/s->line_cap, but
 * refuses to grow past `max_frame` data bytes (newline excluded). The
 * buffer persists across calls so steady-state reads don't re-allocate.
 * Returns the line length (incl. trailing newline if one was read),
 * -1 on EOF/error with nothing buffered, or -2 when the cap is hit. */
static ssize_t stdio_getline_bounded(stdio_impl_t *s, size_t max_frame) {
    size_t len = 0;
    for (;;) {
        if (len + 2 > s->line_cap) {
            size_t ncap = s->line_cap ? s->line_cap * 2 : 256;
            if (ncap < len + 2) ncap = len + 2;
            if (max_frame > 0 && ncap > max_frame + 2) ncap = max_frame + 2;
            char *nb = realloc(s->line_buf, ncap);
            if (!nb) return -1;
            s->line_buf = nb;
            s->line_cap = ncap;
        }
        int ch = getc(s->in);
        if (ch == EOF) {
            if (len == 0) return -1;     /* nothing read → EOF/error */
            break;                       /* final line, no trailing \n */
        }
        s->line_buf[len++] = (char)ch;
        if (ch == '\n') break;
        if (max_frame > 0 && len > max_frame) return -2;   /* over cap */
    }
    s->line_buf[len] = '\0';
    return (ssize_t)len;
}

/* ---------------------------------------------------------------------- */

static int stdio_read(cmcp_transport_t *t, char **out_buf, size_t *out_len) {
    stdio_impl_t *s = (stdio_impl_t *)t->impl;
    if (!out_buf || !out_len) return CMCP_EINVAL;

    size_t max_frame = stdio_max_frame();

    /* Skip blank lines: peers SHOULD NOT emit them, but tolerate them
     * since some intermediaries (terminals, line editors) might. */
    for (;;) {
        ssize_t n = stdio_getline_bounded(s, max_frame);
        if (n == -2) return CMCP_EPROTOCOL;  /* frame exceeds cap */
        if (n < 0)  return CMCP_EIO;         /* EOF or read error */

        /* Strip trailing newline if present. */
        if (n > 0 && s->line_buf[n - 1] == '\n') {
            s->line_buf[--n] = '\0';
            if (n > 0 && s->line_buf[n - 1] == '\r') {
                s->line_buf[--n] = '\0';
            }
        }
        if (n == 0) continue;            /* blank line, keep going */

        char *copy = (char *)malloc((size_t)n + 1);
        if (!copy) return CMCP_ENOMEM;
        memcpy(copy, s->line_buf, (size_t)n);
        copy[n] = '\0';
        *out_buf = copy;
        *out_len = (size_t)n;
        return CMCP_OK;
    }
}

static int stdio_write(cmcp_transport_t *t, const char *buf, size_t len) {
    stdio_impl_t *s = (stdio_impl_t *)t->impl;
    if (!buf && len > 0) return CMCP_EINVAL;

    /* Frames must not contain raw newlines — JSON encoders escape them
     * inside strings, so this should never trip in practice, but a
     * defensive check keeps a buggy upper layer from desyncing the
     * wire forever. memchr is undefined on a NULL pointer even when
     * len == 0; short-circuit so the static analyser can see we never
     * dereference. */
    if (len > 0 && memchr(buf, '\n', len) != NULL) return CMCP_EINVAL;

    pthread_mutex_lock(&s->write_mu);
    int rc = CMCP_OK;
    /* Three near-identical branches: each fails the same way (EIO);
     * the cascade just stops at the first one that does. Refactoring
     * to a single `||` chain would lose the ordering signal in a
     * future debugger session.
     * NOLINTBEGIN(bugprone-branch-clone) */
    if (len > 0 && fwrite(buf, 1, len, s->out) != len)        rc = CMCP_EIO;
    else if (fputc('\n', s->out) == EOF)                       rc = CMCP_EIO;
    else if (fflush(s->out) != 0)                              rc = CMCP_EIO;
    /* NOLINTEND(bugprone-branch-clone) */
    pthread_mutex_unlock(&s->write_mu);
    return rc;
}

static void stdio_close(cmcp_transport_t *t) {
    if (!t) return;
    stdio_impl_t *s = (stdio_impl_t *)t->impl;
    if (s) {
        if (s->owns_streams) {
            if (s->in)  fclose(s->in);
            if (s->out) fclose(s->out);
        }
        free(s->line_buf);
        pthread_mutex_destroy(&s->write_mu);
        free(s);
    }
    free(t);
}

/* ---------------------------------------------------------------------- */

static cmcp_transport_t *stdio_alloc(FILE *in, FILE *out, int owns) {
    cmcp_transport_t *t = (cmcp_transport_t *)calloc(1, sizeof *t);
    stdio_impl_t     *s = (stdio_impl_t *)calloc(1, sizeof *s);
    if (!t || !s) goto fail;
    if (pthread_mutex_init(&s->write_mu, NULL) != 0) goto fail;

    s->in  = in;
    s->out = out;
    s->owns_streams = owns;
    s->line_buf = NULL;
    s->line_cap = 0;

    t->impl     = s;
    t->read_fn  = stdio_read;
    t->write_fn = stdio_write;
    t->close_fn = stdio_close;
    return t;

fail:
    free(t);
    free(s);
    return NULL;
}

cmcp_transport_t *cmcp_transport_stdio_new(void) {
    return stdio_alloc(stdin, stdout, /*owns=*/0);
}

cmcp_transport_t *cmcp_transport_stdio_new_fds(int read_fd, int write_fd) {
    if (read_fd < 0 || write_fd < 0) return NULL;
    FILE *in  = fdopen(read_fd, "rb");
    FILE *out = fdopen(write_fd, "wb");
    if (!in || !out) {
        if (in)  fclose(in);
        if (out) fclose(out);
        return NULL;
    }
    cmcp_transport_t *t = stdio_alloc(in, out, /*owns=*/1);
    if (!t) {
        fclose(in);
        fclose(out);
        return NULL;
    }
    return t;
}
