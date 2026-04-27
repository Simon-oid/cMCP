/* getline(), fdopen() — POSIX.1-2008. */
#define _POSIX_C_SOURCE 200809L

#include "cmcp.h"
#include "cmcp_transport.h"

#include <pthread.h>
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

/* ---------------------------------------------------------------------- */

static int stdio_read(cmcp_transport_t *t, char **out_buf, size_t *out_len) {
    stdio_impl_t *s = (stdio_impl_t *)t->impl;
    if (!out_buf || !out_len) return CMCP_EINVAL;

    /* Skip blank lines: peers SHOULD NOT emit them, but tolerate them
     * since some intermediaries (terminals, line editors) might. */
    for (;;) {
        ssize_t n = getline(&s->line_buf, &s->line_cap, s->in);
        if (n < 0) return CMCP_EIO;     /* EOF or read error */

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
     * wire forever. */
    if (memchr(buf, '\n', len) != NULL) return CMCP_EINVAL;

    pthread_mutex_lock(&s->write_mu);
    int rc = CMCP_OK;
    if (len > 0 && fwrite(buf, 1, len, s->out) != len)        rc = CMCP_EIO;
    else if (fputc('\n', s->out) == EOF)                       rc = CMCP_EIO;
    else if (fflush(s->out) != 0)                              rc = CMCP_EIO;
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
