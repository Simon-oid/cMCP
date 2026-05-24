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
#include <stdatomic.h>
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
    int                        cancelled;  /* set on shutdown or cancel */
    cmcp_rpc_message_t         response;   /* valid iff done && !cancelled */
    /* Optional per-call progress subscription. has_progress_token=1
     * means progress_token holds the int value the library handed the
     * server in _meta.progressToken; the reader matches against it. */
    int                        has_progress_token;
    long long                  progress_token;
    cmcp_progress_fn           progress_fn;
    void                      *progress_ud;
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
    char                       *server_protocol;   /* advertised protocol version */
    cmcp_server_capabilities_t  server_caps;

    /* Reader thread + active completions. shutting_down is an atomic
     * hint — the reader polls it on its loop edges to bail early on
     * teardown. Real wake-up uses transport_wake + SIGUSR2; the flag
     * just keeps the reader from racing back into a read after we've
     * already decided to die. memory_order_relaxed is enough because
     * no other memory is being published through this flag. */
    pthread_t                   reader;
    int                         reader_started;
    atomic_int                  shutting_down;

    pthread_mutex_t             list_mu;
    pending_completion_t       *active_head;       /* doubly-linked list */

    /* Monotonic counter for per-call progress tokens (allocated by
     * cmcp_client_call_async_progress). Guarded by list_mu — we always
     * take list_mu when touching the active list anyway, so folding
     * the token counter under it avoids a second mutex. */
    long long                   next_progress_token;

    /* Notification routing. */
    cmcp_notification_fn        notif_fn;
    void                       *notif_ud;

    /* Sampling (server-initiated `sampling/createMessage`). NULL fn
     * means "decline" — the reader emits -32601 in that case. */
    cmcp_sampling_handler_fn    sampling_fn;
    void                       *sampling_ud;

    /* Elicitation (server-initiated `elicitation/create`). Same
     * default-decline model as sampling: NULL fn → -32601. */
    cmcp_elicitation_handler_fn elicitation_fn;
    void                       *elicitation_ud;

    /* Roots: declarative list the reader replies with on
     * `roots/list`. roots_mu guards both the array pointer and its
     * contents; the reader takes a snapshot under the lock then
     * builds the response. roots_set flips on the first set_roots
     * call (even with n=0) — that's what triggers cap advertisement. */
    pthread_mutex_t             roots_mu;
    int                         roots_mu_init;
    cmcp_root_t                *roots;          /* deep-copied; uri/name owned */
    size_t                      n_roots;
    int                         roots_set;

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

/* Build the client's capabilities sub-object. The `roots` key is
 * present whenever the host has called cmcp_client_set_roots (even
 * with n=0) — that's the opt-in signal. `listChanged` is the
 * separate flag in the cap struct. */
static cmcp_json_t *client_caps_to_json(const cmcp_client_t *cli) {
    const cmcp_client_capabilities_t *c = &cli->caps;
    cmcp_json_t *root = cmcp_json_new_object();
    if (!root) return NULL;
    if (c->sampling) {
        cmcp_json_object_set(root, "sampling", cmcp_json_new_object());
    }
    if (c->elicitation) {
        cmcp_json_object_set(root, "elicitation", cmcp_json_new_object());
    }
    if (cli->roots_set || c->roots_list_changed) {
        cmcp_json_t *roots = cmcp_json_new_object();
        if (c->roots_list_changed)
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

/* Reply to a server-initiated `ping` with an empty result. The spec
 * makes answering a ping mandatory; it carries no params. */
static void handle_ping(cmcp_client_t *c, const cmcp_rpc_message_t *req) {
    cmcp_rpc_message_t resp;
    if (cmcp_rpc_make_response(&resp, &req->id,
                                cmcp_json_new_object()) != CMCP_OK) return;
    send_message(c, &resp);
    cmcp_rpc_message_clear(&resp);
}

/* Reply to `roots/list` with the host's current root set. The list
 * is purely declarative (set via cmcp_client_set_roots); no host
 * callback runs. If the host never called set_roots, fall through to
 * the -32601 path — the cap wasn't advertised either, so the server
 * shouldn't be asking. */
static void handle_roots_list(cmcp_client_t *c,
                                const cmcp_rpc_message_t *req) {
    pthread_mutex_lock(&c->roots_mu);
    if (!c->roots_set) {
        pthread_mutex_unlock(&c->roots_mu);
        reply_method_not_found(c, &req->id);
        return;
    }
    cmcp_json_t *arr = cmcp_json_new_array();
    for (size_t i = 0; i < c->n_roots; i++) {
        cmcp_json_t *o = cmcp_json_new_object();
        cmcp_json_object_set(o, "uri", cmcp_json_new_string(c->roots[i].uri));
        if (c->roots[i].name)
            cmcp_json_object_set(o, "name",
                                  cmcp_json_new_string(c->roots[i].name));
        cmcp_json_array_append(arr, o);
    }
    pthread_mutex_unlock(&c->roots_mu);

    cmcp_json_t *result = cmcp_json_new_object();
    cmcp_json_object_set(result, "roots", arr);

    cmcp_rpc_message_t resp;
    if (cmcp_rpc_make_response(&resp, &req->id, result) != CMCP_OK) return;
    send_message(c, &resp);
    cmcp_rpc_message_clear(&resp);
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

/* Dispatch `elicitation/create` to the host's handler. Same shape as
 * sampling: no handler → -32601; handler error → -32603. The handler
 * runs on the reader thread, so a slow / interactive prompt stalls
 * inbound frames. v0.4 ships the receive surface; an emit half that
 * makes interactive elicitation testable under Claude Code lands in
 * its own segment. */
static void handle_elicitation_request(cmcp_client_t *c,
                                        const cmcp_rpc_message_t *req) {
    if (!c->elicitation_fn) {
        reply_method_not_found(c, &req->id);
        return;
    }
    cmcp_json_t *result = NULL;
    int rc = c->elicitation_fn(req->params, c->elicitation_ud, &result);

    cmcp_rpc_message_t resp;
    if (rc != CMCP_OK) {
        cmcp_json_free(result);
        if (cmcp_rpc_make_error(&resp, &req->id, CMCP_RPC_INTERNAL_ERROR,
                                 "elicitation handler failed", NULL) != CMCP_OK) {
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

/* Route a notifications/progress frame to a per-call progress callback
 * if one is registered against the frame's progressToken. Returns 1
 * on a match (so the caller skips the generic-handler fallthrough);
 * 0 if the token is missing, malformed, or unmatched — let the caller
 * deliver it through the generic notification handler instead. The
 * callback runs while we hold list_mu briefly; it must not block or
 * call back into the client. */
static int dispatch_progress_notification(cmcp_client_t *c,
                                           const cmcp_json_t *params) {
    if (!params || params->type != CMCP_JSON_OBJECT) return 0;
    const cmcp_json_t *tok = cmcp_json_object_get(params, "progressToken");
    if (!tok || tok->type != CMCP_JSON_INT) return 0;
    long long token = tok->i;

    const cmcp_json_t *p_progress = cmcp_json_object_get(params, "progress");
    const cmcp_json_t *p_total    = cmcp_json_object_get(params, "total");
    const cmcp_json_t *p_message  = cmcp_json_object_get(params, "message");

    double progress = 0;
    if (p_progress && p_progress->type == CMCP_JSON_DOUBLE)
        progress = p_progress->d;
    else if (p_progress && p_progress->type == CMCP_JSON_INT)
        progress = (double)p_progress->i;

    double total = -1;
    if (p_total && p_total->type == CMCP_JSON_DOUBLE) total = p_total->d;
    else if (p_total && p_total->type == CMCP_JSON_INT)
        total = (double)p_total->i;

    const char *message = (p_message && p_message->type == CMCP_JSON_STRING)
        ? p_message->str.s : NULL;

    pthread_mutex_lock(&c->list_mu);
    pending_completion_t *p = c->active_head;
    while (p) {
        if (p->has_progress_token && p->progress_token == token) break;
        p = p->next;
    }
    cmcp_progress_fn fn = p ? p->progress_fn : NULL;
    void           *ud = p ? p->progress_ud : NULL;
    pthread_mutex_unlock(&c->list_mu);

    if (!fn) return 0;
    fn(progress, total, message, ud);
    return 1;
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
        if (atomic_load_explicit(&c->shutting_down, memory_order_relaxed))
            return NULL;
        char *frame = NULL; size_t flen = 0;
        int rc = cmcp_transport_read(c->transport, &frame, &flen);
        if (rc != CMCP_OK) {
            free(frame);
            /* Transport is dead. If we're not already shutting down
             * (peer crashed, EOF, etc.) wake every pending waiter
             * before exiting so they don't block forever. */
            if (!atomic_load_explicit(&c->shutting_down,
                                       memory_order_relaxed))
                cancel_all_waiters(c);
            return NULL;     /* EIO / EOF / signal-interrupted shutdown */
        }
        if (atomic_load_explicit(&c->shutting_down, memory_order_relaxed)) {
            free(frame);
            return NULL;
        }
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
                if (m->method &&
                    strcmp(m->method, "notifications/progress") == 0 &&
                    dispatch_progress_notification(c, m->params)) {
                    /* Routed to a per-call callback; do not also deliver
                     * to the generic handler. */
                } else if (c->notif_fn && m->method) {
                    c->notif_fn(m->method, m->params, c->notif_ud);
                }
                cmcp_rpc_message_clear(m);
                break;
            case CMCP_MSG_REQUEST:
                if (m->method && strcmp(m->method, "ping") == 0) {
                    handle_ping(c, m);
                } else if (m->method &&
                    strcmp(m->method, "sampling/createMessage") == 0) {
                    handle_sampling_request(c, m);
                } else if (m->method &&
                           strcmp(m->method, "elicitation/create") == 0) {
                    handle_elicitation_request(c, m);
                } else if (m->method &&
                           strcmp(m->method, "roots/list") == 0) {
                    handle_roots_list(c, m);
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
    if (pthread_mutex_init(&c->roots_mu, NULL) != 0) {
        cmcp_client_free(c);
        return NULL;
    }
    c->roots_mu_init = 1;
    if (!c->name || !c->version || !c->pending) {
        cmcp_client_free(c);
        return NULL;
    }
    return c;
}

void cmcp_client_free(cmcp_client_t *c) {
    if (!c) return;

    atomic_store_explicit(&c->shutting_down, 1, memory_order_relaxed);

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
    free(c->server_protocol);
    cmcp_rpc_pending_free(c->pending);
    pthread_mutex_destroy(&c->list_mu);
    for (size_t i = 0; i < c->n_roots; i++) {
        free((char *)c->roots[i].uri);
        free((char *)c->roots[i].name);
    }
    free(c->roots);
    if (c->roots_mu_init) pthread_mutex_destroy(&c->roots_mu);
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

void cmcp_client_set_elicitation_handler(cmcp_client_t *c,
                                          cmcp_elicitation_handler_fn fn,
                                          void *userdata) {
    if (!c) return;
    c->elicitation_fn = fn;
    c->elicitation_ud = userdata;
}

int cmcp_client_set_roots(cmcp_client_t *c,
                           const cmcp_root_t *roots, size_t n) {
    if (!c) return CMCP_EINVAL;
    if (n > 0 && !roots) return CMCP_EINVAL;

    /* Build the new array first so we don't lose the old one on
     * partial allocation failure. */
    cmcp_root_t *fresh = NULL;
    if (n > 0) {
        fresh = (cmcp_root_t *)calloc(n, sizeof *fresh);
        if (!fresh) return CMCP_ENOMEM;
        for (size_t i = 0; i < n; i++) {
            if (!roots[i].uri) {
                /* Roll back partial allocs. */
                for (size_t j = 0; j < i; j++) {
                    free((char *)fresh[j].uri);
                    free((char *)fresh[j].name);
                }
                free(fresh);
                return CMCP_EINVAL;
            }
            fresh[i].uri  = xstrdup(roots[i].uri);
            fresh[i].name = roots[i].name ? xstrdup(roots[i].name) : NULL;
            if (!fresh[i].uri || (roots[i].name && !fresh[i].name)) {
                for (size_t j = 0; j <= i; j++) {
                    free((char *)fresh[j].uri);
                    free((char *)fresh[j].name);
                }
                free(fresh);
                return CMCP_ENOMEM;
            }
        }
    }

    pthread_mutex_lock(&c->roots_mu);
    /* Free old array. */
    for (size_t i = 0; i < c->n_roots; i++) {
        free((char *)c->roots[i].uri);
        free((char *)c->roots[i].name);
    }
    free(c->roots);
    c->roots     = fresh;
    c->n_roots   = n;
    c->roots_set = 1;
    pthread_mutex_unlock(&c->roots_mu);
    return CMCP_OK;
}

int cmcp_client_notify_roots_changed(cmcp_client_t *c) {
    if (!c) return CMCP_EINVAL;
    if (!c->caps.roots_list_changed) return CMCP_EPROTOCOL;
    return cmcp_client_notify(c, "notifications/roots/list_changed", NULL);
}

int cmcp_client_set_log_level(cmcp_client_t *c, cmcp_log_level_t level) {
    if (!c) return CMCP_EINVAL;
    const char *lname = cmcp_log_level_to_name(level);
    if (!lname) return CMCP_EINVAL;

    cmcp_json_t *params = cmcp_json_new_object();
    if (!params) return CMCP_ENOMEM;
    cmcp_json_object_set(params, "level", cmcp_json_new_string(lname));

    cmcp_rpc_message_t resp;
    cmcp_rpc_message_init(&resp);
    int rc = cmcp_client_request(c, "logging/setLevel", params, &resp);
    if (rc != CMCP_OK) return rc;
    if (resp.error) {
        cmcp_rpc_message_clear(&resp);
        return CMCP_EPROTOCOL;
    }
    cmcp_rpc_message_clear(&resp);
    return CMCP_OK;
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

cmcp_json_t *cmcp_elicitation_result(const char *action,
                                       cmcp_json_t *content) {
    if (!action) { cmcp_json_free(content); return NULL; }
    int is_accept  = strcmp(action, "accept")  == 0;
    int is_decline = strcmp(action, "decline") == 0;
    int is_cancel  = strcmp(action, "cancel")  == 0;
    if (!is_accept && !is_decline && !is_cancel) {
        cmcp_json_free(content);
        return NULL;
    }
    if (is_accept) {
        if (!content || content->type != CMCP_JSON_OBJECT) {
            cmcp_json_free(content);
            return NULL;
        }
    } else if (content) {
        /* Spec: content present only on accept. Drop it. */
        cmcp_json_free(content);
        content = NULL;
    }
    cmcp_json_t *r = cmcp_json_new_object();
    if (!r) { cmcp_json_free(content); return NULL; }
    cmcp_json_object_set(r, "action", cmcp_json_new_string(action));
    if (content) cmcp_json_object_set(r, "content", content);
    return r;
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
const char *cmcp_client_server_protocol(const cmcp_client_t *c) {
    return c ? c->server_protocol : NULL;
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

int cmcp_client_cancel(cmcp_client_t *c, long long id, const char *reason) {
    if (!c) return CMCP_EINVAL;

    /* Take the entry out of the pending table FIRST. This wins the race
     * against a response that's about to arrive: if we take it, the
     * reader's deliver_response simply drops the (now-orphaned) frame;
     * if the response wins, our pending_take here returns 0 and we
     * report CMCP_EINVAL — caller's wait already saw the response. */
    void *ud = NULL;
    if (!cmcp_rpc_pending_take(c->pending, id, &ud) || !ud) {
        return CMCP_EINVAL;
    }
    pending_completion_t *p = (pending_completion_t *)ud;

    /* Signal the waiter. After we unlock p->mu the waiter may unlink
     * and free p — do not touch p again. */
    pthread_mutex_lock(&p->mu);
    if (!p->done) {
        p->cancelled = 1;
        p->done = 1;
        pthread_cond_broadcast(&p->cv);
    }
    pthread_mutex_unlock(&p->mu);

    /* Wire notification. Build params even if reason is NULL — the
     * spec requires requestId. */
    cmcp_json_t *params = cmcp_json_new_object();
    if (!params) return CMCP_ENOMEM;
    cmcp_json_object_set(params, "requestId", cmcp_json_new_int(id));
    if (reason)
        cmcp_json_object_set(params, "reason", cmcp_json_new_string(reason));
    return cmcp_client_notify(c, "notifications/cancelled", params);
}

int cmcp_client_call_async_progress(cmcp_client_t *c, const char *method,
                                     cmcp_json_t *params,
                                     cmcp_progress_fn fn, void *userdata,
                                     long long *out_id) {
    if (!c || !c->transport || !method || !out_id || !fn) {
        cmcp_json_free(params);
        return CMCP_EINVAL;
    }
    /* NULL params → upgrade to empty object so we have somewhere to
     * attach _meta. The library owns the upgraded object. */
    if (!params) {
        params = cmcp_json_new_object();
        if (!params) return CMCP_ENOMEM;
    }
    if (params->type != CMCP_JSON_OBJECT) {
        cmcp_json_free(params);
        return CMCP_EINVAL;
    }

    /* Allocate the token under list_mu (folded counter — same lock that
     * guards the list the reader walks to dispatch progress). */
    pthread_mutex_lock(&c->list_mu);
    long long token = ++c->next_progress_token;
    pthread_mutex_unlock(&c->list_mu);

    /* Inject params._meta.progressToken = token, preserving any other
     * _meta keys the caller put there. cmcp_json_object_get returns
     * const but the underlying tree is mutable — same trick used in
     * tests when we need to update a known-mutable node in place. */
    cmcp_json_t *meta = (cmcp_json_t *)cmcp_json_object_get(params, "_meta");
    if (!meta) {
        meta = cmcp_json_new_object();
        if (!meta) { cmcp_json_free(params); return CMCP_ENOMEM; }
        cmcp_json_object_set(params, "_meta", meta);
    }
    cmcp_json_object_set(meta, "progressToken", cmcp_json_new_int(token));

    pending_completion_t *p = pending_completion_new(0);
    if (!p) { cmcp_json_free(params); return CMCP_ENOMEM; }
    p->has_progress_token = 1;
    p->progress_token     = token;
    p->progress_fn        = fn;
    p->progress_ud        = userdata;

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
    cmcp_rpc_message_clear(&req);
    if (rc != CMCP_OK) {
        list_unlink(c, p);
        cmcp_rpc_pending_take(c->pending, id, NULL);
        pending_completion_free(p);
        return rc;
    }
    *out_id = id;
    return CMCP_OK;
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
                          client_caps_to_json(c));
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
    /* Protocol version negotiation. Per the MCP spec a version mismatch
     * is not a wire error: the client decides whether to continue. We
     * capture whatever the server advertised (exposed via
     * cmcp_client_server_protocol) and proceed — a host that wants
     * stricter behaviour inspects it and disconnects itself. Only a
     * missing or malformed protocolVersion field is fatal here, which
     * mirrors the server side. */
    const cmcp_json_t *pv = cmcp_json_object_get(resp.result, "protocolVersion");
    if (!pv || pv->type != CMCP_JSON_STRING) {
        cmcp_rpc_message_clear(&resp);
        return CMCP_EPROTOCOL;
    }
    free(c->server_protocol);
    c->server_protocol = xstrdup(pv->str.s);
    if (strcmp(pv->str.s, CMCP_PROTOCOL_VERSION) != 0) {
        fprintf(stderr,
                "cmcp_client: server speaks protocol %s, we pin %s; "
                "proceeding (host may disconnect)\n",
                pv->str.s, CMCP_PROTOCOL_VERSION);
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
