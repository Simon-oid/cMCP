/* For kill(2), waitpid(2), execvp(2), sigaction(2). */
#define _POSIX_C_SOURCE 200809L
/* For pthread_tryjoin_np (Linux extension; cMCP runs on Linux). */
#define _GNU_SOURCE

/* cmcp client — async core with synchronous convenience wrapper.
 *
 * Threading model
 * ---------------
 * After cmcp_client_handshake() (or _connect_stdio()) returns, the
 * client owns one background reader thread that:
 *   - reads frames from the transport,
 *   - matches responses against the pending table by ID and signals
 *     the waiter's completion record,
 *   - dispatches notifications to the user-supplied callback,
 *   - replies -32601 to any server-initiated request (sampling /
 *     elicitation / roots are Phase 2.5+).
 *
 * Multiple caller threads may call cmcp_client_request concurrently;
 * the pending table and transport writer are both mutex-guarded.
 *
 * Shutdown
 * --------
 * cmcp_client_free closes the transport (causing the reader's
 * blocking read to return EIO), joins the reader thread, then walks
 * the active-completion list and signals every waiter with
 * CMCP_ECANCELLED. If the client owns a spawned child it is
 * SIGTERM'd if still alive, then waitpid()'d.
 */

#include "cmcp.h"
#include "cmcp_client.h"
#include "cmcp_json.h"
#include "cmcp_types.h"
#include "cmcp_transport.h"

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

/* ====================================================================== */
/* Reader-thread wakeup signal                                             */
/* ====================================================================== */
/* Closing a pipe FD or dup2'ing it does not interrupt a blocked read(2)
 * already in progress on Linux — the kernel keeps the original FD
 * reference. The reliable portable wake is pthread_kill(reader, sig)
 * with a non-restarting signal handler installed. The handler does
 * nothing; the syscall returns -1 with errno=EINTR, getline() returns
 * -1, the stdio transport returns CMCP_EIO, and the reader exits.
 *
 * SIGUSR2 is unused by the rest of the library; the host application
 * must not rely on its default behavior in any thread that runs cmcp
 * client code. */
#define CMCP_WAKE_SIGNAL  SIGUSR2

static void wake_signal_handler(int sig) { (void)sig; }

static pthread_once_t wake_init_once = PTHREAD_ONCE_INIT;
static void wake_init(void) {
    struct sigaction sa = {0};
    sa.sa_handler = wake_signal_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;                  /* explicitly NOT SA_RESTART */
    sigaction(CMCP_WAKE_SIGNAL, &sa, NULL);
}

/* ====================================================================== */
/* Per-request completion record                                           */
/* ====================================================================== */

typedef struct pending_completion {
    long long                  id;
    pthread_mutex_t            mu;
    pthread_cond_t             cv;
    int                        done;       /* 0 → 1 once response is in */
    int                        cancelled;  /* set on shutdown */
    cmcp_rpc_message_t         response;   /* valid iff done && !cancelled */
    struct pending_completion *next, *prev;
} pending_completion_t;

static pending_completion_t *pending_completion_new(long long id) {
    pending_completion_t *p = (pending_completion_t *)calloc(1, sizeof *p);
    if (!p) return NULL;
    p->id = id;
    pthread_mutex_init(&p->mu, NULL);
    pthread_cond_init(&p->cv, NULL);
    return p;
}

static void pending_completion_free(pending_completion_t *p) {
    if (!p) return;
    if (p->done && !p->cancelled) cmcp_rpc_message_clear(&p->response);
    pthread_mutex_destroy(&p->mu);
    pthread_cond_destroy(&p->cv);
    free(p);
}

/* ====================================================================== */
/* Client struct                                                           */
/* ====================================================================== */

struct cmcp_client {
    char                       *name;
    char                       *version;
    cmcp_client_capabilities_t  caps;

    cmcp_transport_t           *transport;        /* may be borrowed or owned */
    int                         own_transport;
    cmcp_rpc_pending_t         *pending;
    int                         initialized;

    /* Server identity captured during handshake. */
    char                       *server_name;
    char                       *server_version;
    cmcp_server_capabilities_t  server_caps;

    /* Reader thread + active completions. */
    pthread_t                   reader;
    int                         reader_started;
    int                         shutting_down;

    pthread_mutex_t             list_mu;
    pending_completion_t       *active_head;       /* doubly-linked list */

    /* Notification routing. */
    cmcp_notification_fn        notif_fn;
    void                       *notif_ud;

    /* Sampling (server-initiated `sampling/createMessage`). NULL fn
     * means "decline" — the reader emits -32601 in that case. */
    cmcp_sampling_handler_fn    sampling_fn;
    void                       *sampling_ud;

    /* Spawned child (cmcp_client_connect_stdio). */
    pid_t                       child_pid;
};

/* ====================================================================== */
/* Helpers                                                                 */
/* ====================================================================== */

static char *xstrdup(const char *s) {
    if (!s) return NULL;
    size_t n = strlen(s);
    char *p = (char *)malloc(n + 1);
    if (!p) return NULL;
    memcpy(p, s, n + 1);
    return p;
}

static void list_link(cmcp_client_t *c, pending_completion_t *p) {
    pthread_mutex_lock(&c->list_mu);
    p->prev = NULL;
    p->next = c->active_head;
    if (c->active_head) c->active_head->prev = p;
    c->active_head = p;
    pthread_mutex_unlock(&c->list_mu);
}

static void list_unlink(cmcp_client_t *c, pending_completion_t *p) {
    pthread_mutex_lock(&c->list_mu);
    if (p->prev) p->prev->next = p->next;
    else if (c->active_head == p) c->active_head = p->next;
    if (p->next) p->next->prev = p->prev;
    p->prev = p->next = NULL;
    pthread_mutex_unlock(&c->list_mu);
}

static int send_message(cmcp_client_t *c, const cmcp_rpc_message_t *m) {
    char *wire = cmcp_rpc_emit(m);
    if (!wire) return CMCP_ENOMEM;
    int rc = cmcp_transport_write(c->transport, wire, strlen(wire));
    free(wire);
    return rc;
}

/* ====================================================================== */
/* Capability codecs (unchanged from Phase 1.4)                            */
/* ====================================================================== */

static cmcp_json_t *client_caps_to_json(const cmcp_client_capabilities_t *c) {
    cmcp_json_t *root = cmcp_json_new_object();
    if (!root) return NULL;
    if (c->sampling) {
        cmcp_json_object_set(root, "sampling", cmcp_json_new_object());
    }
    if (c->roots_list_changed) {
        cmcp_json_t *roots = cmcp_json_new_object();
        cmcp_json_object_set(roots, "listChanged", cmcp_json_new_bool(1));
        cmcp_json_object_set(root, "roots", roots);
    }
    return root;
}

static void server_caps_from_json(const cmcp_json_t *o,
                                   cmcp_server_capabilities_t *out) {
    memset(out, 0, sizeof *out);
    if (!o || o->type != CMCP_JSON_OBJECT) return;
    const cmcp_json_t *tools = cmcp_json_object_get(o, "tools");
    if (tools && tools->type == CMCP_JSON_OBJECT) {
        const cmcp_json_t *lc = cmcp_json_object_get(tools, "listChanged");
        if (lc && lc->type == CMCP_JSON_BOOL && lc->b) {
            out->tools_list_changed = 1;
        }
    }
    const cmcp_json_t *res = cmcp_json_object_get(o, "resources");
    if (res && res->type == CMCP_JSON_OBJECT) {
        const cmcp_json_t *sub = cmcp_json_object_get(res, "subscribe");
        const cmcp_json_t *lc  = cmcp_json_object_get(res, "listChanged");
        if (sub && sub->type == CMCP_JSON_BOOL && sub->b) out->resources_subscribe = 1;
        if (lc  && lc->type  == CMCP_JSON_BOOL && lc->b)  out->resources_list_changed = 1;
    }
    const cmcp_json_t *prompts = cmcp_json_object_get(o, "prompts");
    if (prompts && prompts->type == CMCP_JSON_OBJECT) {
        const cmcp_json_t *lc = cmcp_json_object_get(prompts, "listChanged");
        if (lc && lc->type == CMCP_JSON_BOOL && lc->b) {
            out->prompts_list_changed = 1;
        }
    }
    if (cmcp_json_object_get(o, "logging")) out->logging = 1;
}

/* ====================================================================== */
/* Reader thread                                                           */
/* ====================================================================== */

static void deliver_response(cmcp_client_t *c, cmcp_rpc_message_t *resp) {
    if (resp->id.kind != CMCP_ID_INT) {
        cmcp_rpc_message_clear(resp);
        return;
    }
    void *ud = NULL;
    if (!cmcp_rpc_pending_take(c->pending, resp->id.i, &ud) || !ud) {
        /* Stale / unknown response. */
        cmcp_rpc_message_clear(resp);
        return;
    }
    pending_completion_t *p = (pending_completion_t *)ud;
    pthread_mutex_lock(&p->mu);
    p->response = *resp;                      /* move */
    cmcp_rpc_message_init(resp);              /* prevent caller free */
    p->done = 1;
    pthread_cond_broadcast(&p->cv);
    pthread_mutex_unlock(&p->mu);
}

static void reply_method_not_found(cmcp_client_t *c, const cmcp_id_t *id) {
    cmcp_rpc_message_t err;
    if (cmcp_rpc_make_error(&err, id, CMCP_RPC_METHOD_NOT_FOUND,
                             "client does not handle server-initiated requests",
                             NULL) != CMCP_OK) return;
    send_message(c, &err);
    cmcp_rpc_message_clear(&err);
}

/* Dispatch `sampling/createMessage` to the host's handler. If no
 * handler is registered, fall through to the generic -32601 path —
 * the host effectively declines server-initiated sampling. Handler
 * runs on the reader thread; a slow LLM call therefore stalls
 * inbound frames until it returns. Document, don't fix in v0.2. */
static void handle_sampling_request(cmcp_client_t *c,
                                     const cmcp_rpc_message_t *req) {
    if (!c->sampling_fn) {
        reply_method_not_found(c, &req->id);
        return;
    }
    cmcp_json_t *result = NULL;
    int rc = c->sampling_fn(req->params, c->sampling_ud, &result);

    cmcp_rpc_message_t resp;
    if (rc != CMCP_OK) {
        cmcp_json_free(result);
        if (cmcp_rpc_make_error(&resp, &req->id, CMCP_RPC_INTERNAL_ERROR,
                                 "sampling handler failed", NULL) != CMCP_OK) {
            return;
        }
    } else {
        if (!result) result = cmcp_json_new_object();
        if (cmcp_rpc_make_response(&resp, &req->id, result) != CMCP_OK) {
            return;
        }
    }
    send_message(c, &resp);
    cmcp_rpc_message_clear(&resp);
}

/* Wake every still-pending waiter with cancelled=1. Idempotent: safe
 * to call multiple times — the per-completion `done` check stops a
 * second broadcast. Used by the reader on transport failure (so a
 * dead server doesn't strand callers) and by cmcp_client_free for
 * normal teardown. */
static void cancel_all_waiters(cmcp_client_t *c) {
    pthread_mutex_lock(&c->list_mu);
    pending_completion_t *p = c->active_head;
    while (p) {
        pthread_mutex_lock(&p->mu);
        if (!p->done) {
            p->cancelled = 1;
            p->done = 1;
            pthread_cond_broadcast(&p->cv);
        }
        pthread_mutex_unlock(&p->mu);
        p = p->next;
    }
    pthread_mutex_unlock(&c->list_mu);
}

static void *reader_main(void *arg) {
    cmcp_client_t *c = (cmcp_client_t *)arg;
    for (;;) {
        if (c->shutting_down) return NULL;
        char *frame = NULL; size_t flen = 0;
        int rc = cmcp_transport_read(c->transport, &frame, &flen);
        if (rc != CMCP_OK) {
            free(frame);
            /* Transport is dead. If we're not already shutting down
             * (peer crashed, EOF, etc.) wake every pending waiter
             * before exiting so they don't block forever. */
            if (!c->shutting_down) cancel_all_waiters(c);
            return NULL;     /* EIO / EOF / signal-interrupted shutdown */
        }
        if (c->shutting_down) { free(frame); return NULL; }
        cmcp_rpc_message_t *msgs = NULL; size_t n = 0;
        int prc = cmcp_rpc_parse(frame, flen, &msgs, &n);
        free(frame);
        if (prc != CMCP_OK || n == 0) {
            cmcp_rpc_messages_free(msgs, n);
            continue;
        }
        for (size_t i = 0; i < n; i++) {
            cmcp_rpc_message_t *m = &msgs[i];
            switch (m->kind) {
            case CMCP_MSG_RESPONSE:
                deliver_response(c, m);
                break;
            case CMCP_MSG_NOTIFICATION:
                if (c->notif_fn && m->method) {
                    c->notif_fn(m->method, m->params, c->notif_ud);
                }
                cmcp_rpc_message_clear(m);
                break;
            case CMCP_MSG_REQUEST:
                if (m->method &&
                    strcmp(m->method, "sampling/createMessage") == 0) {
                    handle_sampling_request(c, m);
                } else {
                    reply_method_not_found(c, &m->id);
                }
                cmcp_rpc_message_clear(m);
                break;
            default:
                cmcp_rpc_message_clear(m);
                break;
            }
        }
        free(msgs);
    }
}

/* ====================================================================== */
/* Construction / destruction                                              */
/* ====================================================================== */

cmcp_client_t *cmcp_client_new(const char *name, const char *version) {
    if (!name || !version) return NULL;
    pthread_once(&wake_init_once, wake_init);

    cmcp_client_t *c = (cmcp_client_t *)calloc(1, sizeof *c);
    if (!c) return NULL;
    c->name    = xstrdup(name);
    c->version = xstrdup(version);
    c->pending = cmcp_rpc_pending_new();
    pthread_mutex_init(&c->list_mu, NULL);
    if (!c->name || !c->version || !c->pending) {
        cmcp_client_free(c);
        return NULL;
    }
    return c;
}

void cmcp_client_free(cmcp_client_t *c) {
    if (!c) return;

    c->shutting_down = 1;

    /* Wake the reader and join. Two complementary mechanisms:
     *
     *   1) Transport's wake_fn (HTTP client, etc.) — flips an internal
     *      flag and broadcasts the userspace condvar that read_fn is
     *      parked on, causing read_fn to return CMCP_EIO.
     *
     *   2) pthread_kill(SIGUSR2) with a non-restarting handler — for
     *      transports whose read_fn blocks on a syscall (stdio's
     *      getline), the signal forces EINTR, the transport surfaces
     *      it as CMCP_EIO, the reader exits.
     *
     * We do both: wake_fn handles userspace-blocked readers, the
     * signal handles syscall-blocked ones, and the tryjoin retry loop
     * covers the race where the reader was between its shutdown check
     * and the actual blocking call. */
    if (c->reader_started) {
        if (c->transport) cmcp_transport_wake(c->transport);
        for (;;) {
            int jr = pthread_tryjoin_np(c->reader, NULL);
            if (jr == 0) break;
            if (c->transport) cmcp_transport_wake(c->transport);
            pthread_kill(c->reader, CMCP_WAKE_SIGNAL);
            struct timespec ts = { 0, 100000 };  /* 100us */
            nanosleep(&ts, NULL);
        }
        c->reader_started = 0;
    }

    /* Cancel any waiters still on the active list. */
    cancel_all_waiters(c);

    /* Close our owned transport, if any. (Borrowed transports stay
     * with the caller — they wake the reader via _wake but the
     * caller still owns close.) */
    if (c->own_transport && c->transport) {
        cmcp_transport_close(c->transport);
        c->transport = NULL;
    }

    /* Reap spawned child. */
    if (c->child_pid > 0) {
        int status;
        pid_t r = waitpid(c->child_pid, &status, WNOHANG);
        if (r == 0) {
            kill(c->child_pid, SIGTERM);
            waitpid(c->child_pid, &status, 0);
        }
    }

    free(c->name);
    free(c->version);
    free(c->server_name);
    free(c->server_version);
    cmcp_rpc_pending_free(c->pending);
    pthread_mutex_destroy(&c->list_mu);
    free(c);
}

void cmcp_client_set_capabilities(cmcp_client_t *c,
                                   const cmcp_client_capabilities_t *caps) {
    if (!c || !caps) return;
    c->caps = *caps;
}

void cmcp_client_set_notification_handler(cmcp_client_t *c,
                                           cmcp_notification_fn fn,
                                           void *userdata) {
    if (!c) return;
    c->notif_fn = fn;
    c->notif_ud = userdata;
}

void cmcp_client_set_sampling_handler(cmcp_client_t *c,
                                       cmcp_sampling_handler_fn fn,
                                       void *userdata) {
    if (!c) return;
    c->sampling_fn = fn;
    c->sampling_ud = userdata;
}

cmcp_json_t *cmcp_sampling_text_result(const char *text,
                                        const char *model,
                                        const char *stop_reason) {
    if (!text) return NULL;
    cmcp_json_t *result  = cmcp_json_new_object();
    cmcp_json_t *content = cmcp_json_new_object();
    if (!result || !content) {
        cmcp_json_free(result); cmcp_json_free(content);
        return NULL;
    }
    cmcp_json_object_set(content, "type", cmcp_json_new_string("text"));
    cmcp_json_object_set(content, "text", cmcp_json_new_string(text));
    cmcp_json_object_set(result, "role",    cmcp_json_new_string("assistant"));
    cmcp_json_object_set(result, "content", content);
    if (model)
        cmcp_json_object_set(result, "model", cmcp_json_new_string(model));
    if (stop_reason)
        cmcp_json_object_set(result, "stopReason",
                              cmcp_json_new_string(stop_reason));
    return result;
}

const cmcp_server_capabilities_t *cmcp_client_server_caps(const cmcp_client_t *c) {
    return c ? &c->server_caps : NULL;
}
const char *cmcp_client_server_name(const cmcp_client_t *c) {
    return c ? c->server_name : NULL;
}
const char *cmcp_client_server_version(const cmcp_client_t *c) {
    return c ? c->server_version : NULL;
}

/* ====================================================================== */
/* Async core                                                              */
/* ====================================================================== */

int cmcp_client_call_async(cmcp_client_t *c, const char *method,
                            cmcp_json_t *params, long long *out_id) {
    if (!c || !c->transport || !method || !out_id) {
        cmcp_json_free(params);
        return CMCP_EINVAL;
    }

    pending_completion_t *p = pending_completion_new(0);
    if (!p) { cmcp_json_free(params); return CMCP_ENOMEM; }

    long long id = cmcp_rpc_pending_register(c->pending, p);
    if (id == 0) {
        pending_completion_free(p);
        cmcp_json_free(params);
        return CMCP_ENOMEM;
    }
    p->id = id;
    list_link(c, p);

    cmcp_rpc_message_t req;
    int rc = cmcp_rpc_make_request(&req, id, method, params);
    if (rc != CMCP_OK) {
        list_unlink(c, p);
        cmcp_rpc_pending_take(c->pending, id, NULL);
        pending_completion_free(p);
        cmcp_json_free(params);
        return rc;
    }

    rc = send_message(c, &req);
    cmcp_rpc_message_clear(&req);     /* clears stored params too */
    if (rc != CMCP_OK) {
        list_unlink(c, p);
        cmcp_rpc_pending_take(c->pending, id, NULL);
        pending_completion_free(p);
        return rc;
    }
    *out_id = id;
    return CMCP_OK;
}

int cmcp_client_wait(cmcp_client_t *c, long long id,
                      cmcp_rpc_message_t *out_response) {
    if (!c || !out_response) return CMCP_EINVAL;

    /* Find the completion record on our active list. */
    pthread_mutex_lock(&c->list_mu);
    pending_completion_t *p = c->active_head;
    while (p && p->id != id) p = p->next;
    pthread_mutex_unlock(&c->list_mu);
    if (!p) return CMCP_ENOTFOUND;

    pthread_mutex_lock(&p->mu);
    while (!p->done) pthread_cond_wait(&p->cv, &p->mu);
    int cancelled = p->cancelled;
    if (!cancelled) {
        *out_response = p->response;
        cmcp_rpc_message_init(&p->response);   /* moved out */
    }
    pthread_mutex_unlock(&p->mu);

    list_unlink(c, p);
    /* If the call was cancelled mid-flight the pending entry may
     * still be there — drop it. */
    cmcp_rpc_pending_take(c->pending, id, NULL);
    pending_completion_free(p);

    return cancelled ? CMCP_ECANCELLED : CMCP_OK;
}

int cmcp_client_request(cmcp_client_t *c, const char *method,
                         cmcp_json_t *params,
                         cmcp_rpc_message_t *out_response) {
    long long id = 0;
    int rc = cmcp_client_call_async(c, method, params, &id);
    if (rc != CMCP_OK) return rc;
    return cmcp_client_wait(c, id, out_response);
}

int cmcp_client_notify(cmcp_client_t *c, const char *method,
                        cmcp_json_t *params) {
    if (!c || !c->transport || !method) {
        cmcp_json_free(params);
        return CMCP_EINVAL;
    }
    cmcp_rpc_message_t n;
    int rc = cmcp_rpc_make_notification(&n, method, params);
    if (rc != CMCP_OK) { cmcp_json_free(params); return rc; }
    rc = send_message(c, &n);
    cmcp_rpc_message_clear(&n);
    return rc;
}

/* ====================================================================== */
/* Handshake                                                               */
/* ====================================================================== */

static int do_initialize(cmcp_client_t *c) {
    cmcp_json_t *params = cmcp_json_new_object();
    if (!params) return CMCP_ENOMEM;
    cmcp_json_object_set(params, "protocolVersion",
                          cmcp_json_new_string(CMCP_PROTOCOL_VERSION));
    cmcp_json_object_set(params, "capabilities",
                          client_caps_to_json(&c->caps));
    cmcp_json_t *ci = cmcp_json_new_object();
    cmcp_json_object_set(ci, "name",    cmcp_json_new_string(c->name));
    cmcp_json_object_set(ci, "version", cmcp_json_new_string(c->version));
    cmcp_json_object_set(params, "clientInfo", ci);

    cmcp_rpc_message_t resp;
    cmcp_rpc_message_init(&resp);
    int rc = cmcp_client_request(c, "initialize", params, &resp);
    if (rc != CMCP_OK) return rc;

    if (resp.error) {
        cmcp_rpc_message_clear(&resp);
        return CMCP_EPROTOCOL;
    }
    if (!resp.result || resp.result->type != CMCP_JSON_OBJECT) {
        cmcp_rpc_message_clear(&resp);
        return CMCP_EPROTOCOL;
    }
    const cmcp_json_t *pv = cmcp_json_object_get(resp.result, "protocolVersion");
    if (!pv || pv->type != CMCP_JSON_STRING ||
        strcmp(pv->str.s, CMCP_PROTOCOL_VERSION) != 0) {
        cmcp_rpc_message_clear(&resp);
        return CMCP_EPROTOCOL;
    }
    const cmcp_json_t *si = cmcp_json_object_get(resp.result, "serverInfo");
    if (si && si->type == CMCP_JSON_OBJECT) {
        const cmcp_json_t *n = cmcp_json_object_get(si, "name");
        const cmcp_json_t *v = cmcp_json_object_get(si, "version");
        if (n && n->type == CMCP_JSON_STRING) {
            free(c->server_name);
            c->server_name = xstrdup(n->str.s);
        }
        if (v && v->type == CMCP_JSON_STRING) {
            free(c->server_version);
            c->server_version = xstrdup(v->str.s);
        }
    }
    server_caps_from_json(cmcp_json_object_get(resp.result, "capabilities"),
                           &c->server_caps);
    cmcp_rpc_message_clear(&resp);

    rc = cmcp_client_notify(c, "notifications/initialized", NULL);
    if (rc != CMCP_OK) return rc;

    c->initialized = 1;
    return CMCP_OK;
}

int cmcp_client_handshake(cmcp_client_t *c, cmcp_transport_t *t) {
    if (!c || !t) return CMCP_EINVAL;
    if (c->initialized) return CMCP_EPROTOCOL;
    c->transport     = t;
    c->own_transport = 0;

    /* Reader must be running before we send the initialize request,
     * since its response comes back through the pending table. */
    if (pthread_create(&c->reader, NULL, reader_main, c) != 0) {
        c->transport = NULL;
        return CMCP_EIO;
    }
    c->reader_started = 1;

    return do_initialize(c);
}

/* ====================================================================== */
/* connect_stdio: spawn child + handshake                                  */
/* ====================================================================== */

static int set_cloexec(int fd) {
    int fl = fcntl(fd, F_GETFD, 0);
    if (fl < 0) return -1;
    return fcntl(fd, F_SETFD, fl | FD_CLOEXEC);
}

int cmcp_client_connect_stdio(cmcp_client_t *c,
                               const char *path,
                               char *const argv[],
                               char *const envp[]) {
    if (!c || !path || !argv) return CMCP_EINVAL;
    if (c->initialized) return CMCP_EPROTOCOL;

    /* Pipes:
     *   parent_to_child[0] → child stdin   parent_to_child[1] → parent writes
     *   child_to_parent[0] → parent reads  child_to_parent[1] → child stdout */
    int p2c[2], c2p[2];
    if (pipe(p2c) != 0) return CMCP_EIO;
    if (pipe(c2p) != 0) {
        close(p2c[0]); close(p2c[1]);
        return CMCP_EIO;
    }

    pid_t pid = fork();
    if (pid < 0) {
        close(p2c[0]); close(p2c[1]);
        close(c2p[0]); close(c2p[1]);
        return CMCP_EIO;
    }
    if (pid == 0) {
        /* child */
        dup2(p2c[0], STDIN_FILENO);
        dup2(c2p[1], STDOUT_FILENO);
        close(p2c[0]); close(p2c[1]);
        close(c2p[0]); close(c2p[1]);
        if (envp) execve(path, argv, envp);
        else      execvp(path, argv);
        fprintf(stderr, "cmcp_client_connect_stdio: exec(%s) failed: %s\n",
                 path, strerror(errno));
        _exit(127);
    }

    /* parent */
    close(p2c[0]);
    close(c2p[1]);
    set_cloexec(p2c[1]);
    set_cloexec(c2p[0]);

    cmcp_transport_t *t = cmcp_transport_stdio_new_fds(c2p[0], p2c[1]);
    if (!t) {
        close(p2c[1]); close(c2p[0]);
        kill(pid, SIGTERM);
        waitpid(pid, NULL, 0);
        return CMCP_ENOMEM;
    }

    c->child_pid     = pid;
    c->transport     = t;
    c->own_transport = 1;

    if (pthread_create(&c->reader, NULL, reader_main, c) != 0) {
        cmcp_transport_close(t);
        c->transport = NULL;
        c->own_transport = 0;
        kill(pid, SIGTERM);
        waitpid(pid, NULL, 0);
        c->child_pid = 0;
        return CMCP_EIO;
    }
    c->reader_started = 1;

    int rc = do_initialize(c);
    if (rc != CMCP_OK) {
        /* Tear down: reader thread + child get cleaned up by free. */
        return rc;
    }
    return CMCP_OK;
}
