/* wire_rig.h — raw-wire test rig shared by the modern-era tests.
 *
 * A cmcp server runs on its own thread behind a pipe pair; the test
 * writes exact JSON-RPC frames and reads the replies back, so it pins
 * the bytes on the wire rather than a client library's view of them.
 * Header-only (static functions): include it from exactly one .c per
 * test binary, after defining _POSIX_C_SOURCE. */
#ifndef CMCP_TEST_WIRE_RIG_H
#define CMCP_TEST_WIRE_RIG_H

#include "cmcp.h"
#include "cmcp_json.h"
#include "cmcp_server.h"
#include "cmcp_transport.h"

#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

typedef struct {
    cmcp_transport_t *client_t;
    cmcp_transport_t *server_t;
    cmcp_server_t    *srv;
    pthread_t         th;
    int               rc;
} rig_t;

/* Registers the test's primitives on a fresh server. 0 on success. */
typedef int (*rig_setup_fn)(cmcp_server_t *srv);

static inline void *rig_server_thread(void *arg) {
    rig_t *r = (rig_t *)arg;
    r->rc = cmcp_server_run(r->srv, r->server_t);
    return NULL;
}

static inline int rig_start(rig_t *r, const char *name, const char *version,
                     rig_setup_fn setup) {
    memset(r, 0, sizeof(*r));
    int c2s[2], s2c[2];
    if (pipe(c2s) != 0) return -1;
    if (pipe(s2c) != 0) { close(c2s[0]); close(c2s[1]); return -1; }
    r->client_t = cmcp_transport_stdio_new_fds(s2c[0], c2s[1]);
    r->server_t = cmcp_transport_stdio_new_fds(c2s[0], s2c[1]);
    r->srv = cmcp_server_new(name, version);
    if (!r->client_t || !r->server_t || !r->srv) return -1;
    if (setup && setup(r->srv) != 0) return -1;
    return pthread_create(&r->th, NULL, rig_server_thread, r) == 0 ? 0 : -1;
}

static inline void rig_stop(rig_t *r) {
    cmcp_transport_close(r->client_t);   /* EOF ends the server loop */
    pthread_join(r->th, NULL);
    cmcp_transport_close(r->server_t);
    cmcp_server_free(r->srv);
}

/* Frames go out WITHOUT a trailing newline: the stdio transport adds
 * it and rejects a frame that already contains one. */
static inline int rig_send(rig_t *r, const char *frame) {
    return cmcp_transport_write(r->client_t, frame, strlen(frame));
}

/* Read one frame and parse it. Owned value, or NULL on any failure. */
static inline cmcp_json_t *rig_recv(rig_t *r) {
    char *in = NULL; size_t n = 0;
    if (cmcp_transport_read(r->client_t, &in, &n) != CMCP_OK) return NULL;
    cmcp_json_t *v = cmcp_json_parse(in, n);
    free(in);
    return v;
}

/* Write one frame, read one frame back. */
static inline cmcp_json_t *exchange(rig_t *r, const char *frame) {
    if (rig_send(r, frame) != CMCP_OK) return NULL;
    return rig_recv(r);
}

/* Small JSON path helpers: NULL-safe, return NULL on any miss. */
static inline const cmcp_json_t *get(const cmcp_json_t *o, const char *key) {
    return (o && o->type == CMCP_JSON_OBJECT) ? cmcp_json_object_get(o, key)
                                              : NULL;
}

static inline int is_str(const cmcp_json_t *v, const char *want) {
    return v && v->type == CMCP_JSON_STRING &&
           strcmp(cmcp_json_string(v), want) == 0;
}

static inline long long int_of(const cmcp_json_t *v) {
    return (v && v->type == CMCP_JSON_INT) ? cmcp_json_int(v) : -999999;
}

#endif
