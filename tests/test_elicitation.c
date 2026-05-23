/* End-to-end tests for the Phase 4.4 elicitation-handler surface
 * (receive half — the client/host side).
 *
 * Same hand-rolled mini-server pattern as test_sampling.c, since
 * `cmcp_server_t` does not initiate requests yet. The mini-server:
 *   1. Completes the initialize handshake (capturing the client cap).
 *   2. Sends an `elicitation/create` request.
 *   3. Captures the response or error code.
 *
 * Under test:
 *   - cmcp_client_set_elicitation_handler stores fn + ud.
 *   - reader thread routes elicitation/create:
 *       handler set    → invoke, build success response.
 *       handler NULL   → reply -32601 (default decline).
 *       handler error  → reply -32603.
 *   - cmcp_elicitation_result helper validates action and folds content
 *     in for "accept" only.
 *   - caps.elicitation = 1 propagates in the initialize request.
 */

#define _POSIX_C_SOURCE 200809L

#include "test.h"
#include "cmcp.h"
#include "cmcp_client.h"
#include "cmcp_json.h"
#include "cmcp_transport.h"
#include "cmcp_types.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* ====================================================================== */
/* Pipe-pair scaffolding                                                   */
/* ====================================================================== */

typedef struct {
    cmcp_transport_t *client_t;
    cmcp_transport_t *server_t;
} transport_pair_t;

static int make_pair(transport_pair_t *out) {
    int c2s[2], s2c[2];
    if (pipe(c2s) != 0) return -1;
    if (pipe(s2c) != 0) { close(c2s[0]); close(c2s[1]); return -1; }
    out->client_t = cmcp_transport_stdio_new_fds(s2c[0], c2s[1]);
    out->server_t = cmcp_transport_stdio_new_fds(c2s[0], s2c[1]);
    if (!out->client_t || !out->server_t) {
        cmcp_transport_close(out->client_t);
        cmcp_transport_close(out->server_t);
        return -1;
    }
    return 0;
}

/* ====================================================================== */
/* Mini-server: handshake → elicitation/create → capture response          */
/* ====================================================================== */

typedef struct {
    pthread_mutex_t       mu;
    pthread_cond_t        cv;
    int                   done;
    int                   has_caps;
    int                   client_elicit_cap;
    cmcp_rpc_message_t    captured;
} elicit_capture_t;

static void capture_init(elicit_capture_t *c) {
    pthread_mutex_init(&c->mu, NULL);
    pthread_cond_init(&c->cv, NULL);
    c->done = 0;
    c->has_caps = 0;
    c->client_elicit_cap = 0;
    cmcp_rpc_message_init(&c->captured);
}

static void capture_clear(elicit_capture_t *c) {
    pthread_mutex_destroy(&c->mu);
    pthread_cond_destroy(&c->cv);
    cmcp_rpc_message_clear(&c->captured);
}

typedef struct {
    cmcp_transport_t *t;
    elicit_capture_t *cap;
    cmcp_json_t      *elicit_params;     /* moved out on send */
} miniserver_arg_t;

static void *miniserver_thread(void *arg) {
    miniserver_arg_t *a = (miniserver_arg_t *)arg;
    cmcp_transport_t *t = a->t;

    char *frame = NULL; size_t flen = 0;
    if (cmcp_transport_read(t, &frame, &flen) != CMCP_OK) return NULL;
    cmcp_rpc_message_t *msgs = NULL; size_t nmsgs = 0;
    int prc = cmcp_rpc_parse(frame, flen, &msgs, &nmsgs);
    free(frame);
    if (prc != CMCP_OK || nmsgs != 1) {
        cmcp_rpc_messages_free(msgs, nmsgs);
        return NULL;
    }

    pthread_mutex_lock(&a->cap->mu);
    if (msgs[0].params && msgs[0].params->type == CMCP_JSON_OBJECT) {
        const cmcp_json_t *cc = cmcp_json_object_get(msgs[0].params, "capabilities");
        if (cc && cc->type == CMCP_JSON_OBJECT) {
            a->cap->has_caps = 1;
            a->cap->client_elicit_cap =
                cmcp_json_object_get(cc, "elicitation") ? 1 : 0;
        }
    }
    pthread_mutex_unlock(&a->cap->mu);

    cmcp_json_t *result = cmcp_json_new_object();
    cmcp_json_object_set(result, "protocolVersion",
                          cmcp_json_new_string(CMCP_PROTOCOL_VERSION));
    cmcp_json_object_set(result, "capabilities", cmcp_json_new_object());
    cmcp_json_t *si = cmcp_json_new_object();
    cmcp_json_object_set(si, "name",    cmcp_json_new_string("elicit-srv"));
    cmcp_json_object_set(si, "version", cmcp_json_new_string("0.1.0"));
    cmcp_json_object_set(result, "serverInfo", si);

    cmcp_rpc_message_t resp;
    cmcp_rpc_message_init(&resp);
    cmcp_rpc_make_response(&resp, &msgs[0].id, result);
    char *wire = cmcp_rpc_emit(&resp);
    cmcp_transport_write(t, wire, strlen(wire));
    free(wire);
    cmcp_rpc_message_clear(&resp);
    cmcp_rpc_messages_free(msgs, nmsgs);

    if (cmcp_transport_read(t, &frame, &flen) != CMCP_OK) return NULL;
    free(frame);

    cmcp_rpc_message_t req;
    cmcp_rpc_message_init(&req);
    cmcp_rpc_make_request(&req, 42, "elicitation/create", a->elicit_params);
    a->elicit_params = NULL;
    wire = cmcp_rpc_emit(&req);
    cmcp_transport_write(t, wire, strlen(wire));
    free(wire);
    cmcp_rpc_message_clear(&req);

    if (cmcp_transport_read(t, &frame, &flen) != CMCP_OK) return NULL;
    cmcp_rpc_message_t *rsps = NULL; size_t nr = 0;
    int rrc = cmcp_rpc_parse(frame, flen, &rsps, &nr);
    free(frame);

    pthread_mutex_lock(&a->cap->mu);
    if (rrc == CMCP_OK && nr == 1) {
        a->cap->captured = rsps[0];
        cmcp_rpc_message_init(&rsps[0]);
    }
    a->cap->done = 1;
    pthread_cond_broadcast(&a->cap->cv);
    pthread_mutex_unlock(&a->cap->mu);
    cmcp_rpc_messages_free(rsps, nr);

    while (cmcp_transport_read(t, &frame, &flen) == CMCP_OK) free(frame);
    return NULL;
}

static int wait_for_capture(elicit_capture_t *c) {
    pthread_mutex_lock(&c->mu);
    while (!c->done) {
        struct timespec ts;
        clock_gettime(CLOCK_REALTIME, &ts);
        ts.tv_sec += 2;
        if (pthread_cond_timedwait(&c->cv, &c->mu, &ts) != 0) break;
    }
    int hit = c->done;
    pthread_mutex_unlock(&c->mu);
    return hit;
}

/* ====================================================================== */
/* Sample elicitation params                                                */
/* ====================================================================== */

/* A minimal but realistic params: a single boolean confirm. Matches the
 * spec's restricted-schema rules (flat object, primitives only). */
static cmcp_json_t *make_elicit_params(const char *message) {
    cmcp_json_t *p = cmcp_json_new_object();
    cmcp_json_object_set(p, "message", cmcp_json_new_string(message));
    cmcp_json_t *schema = cmcp_json_new_object();
    cmcp_json_object_set(schema, "type", cmcp_json_new_string("object"));
    cmcp_json_t *props = cmcp_json_new_object();
    cmcp_json_t *confirm = cmcp_json_new_object();
    cmcp_json_object_set(confirm, "type", cmcp_json_new_string("boolean"));
    cmcp_json_object_set(props, "confirm", confirm);
    cmcp_json_object_set(schema, "properties", props);
    cmcp_json_object_set(p, "requestedSchema", schema);
    return p;
}

/* ====================================================================== */
/* Test handlers                                                            */
/* ====================================================================== */

typedef struct {
    int             invoked;
    char           *captured_message;
    pthread_mutex_t mu;
} mock_elicit_ctx_t;

static int mock_elicit_accept(const cmcp_json_t *params, void *userdata,
                               cmcp_json_t **out_result) {
    mock_elicit_ctx_t *ctx = (mock_elicit_ctx_t *)userdata;
    pthread_mutex_lock(&ctx->mu);
    ctx->invoked = 1;
    if (params && params->type == CMCP_JSON_OBJECT) {
        const cmcp_json_t *m = cmcp_json_object_get(params, "message");
        if (m && m->type == CMCP_JSON_STRING) {
            free(ctx->captured_message);
            ctx->captured_message = strdup(m->str.s);
        }
    }
    pthread_mutex_unlock(&ctx->mu);

    cmcp_json_t *content = cmcp_json_new_object();
    cmcp_json_object_set(content, "confirm", cmcp_json_new_bool(1));
    *out_result = cmcp_elicitation_result("accept", content);
    return *out_result ? CMCP_OK : CMCP_ENOMEM;
}

static int mock_elicit_failing(const cmcp_json_t *params, void *userdata,
                                cmcp_json_t **out_result) {
    (void)params; (void)userdata; (void)out_result;
    return CMCP_EHANDLER;
}

/* ====================================================================== */
/* Tests                                                                    */
/* ====================================================================== */

static void test_handler_invoked_and_accept_shape(void) {
    transport_pair_t p = {0};
    TEST_ASSERT(make_pair(&p) == 0);

    elicit_capture_t cap; capture_init(&cap);
    miniserver_arg_t marg = {
        p.server_t, &cap,
        make_elicit_params("Delete /tmp/scratch?"),
    };

    pthread_t th;
    TEST_ASSERT(pthread_create(&th, NULL, miniserver_thread, &marg) == 0);

    mock_elicit_ctx_t mctx = {0};
    pthread_mutex_init(&mctx.mu, NULL);

    cmcp_client_t *cli = cmcp_client_new("host", "0.0.1");
    cmcp_client_set_capabilities(cli, &(cmcp_client_capabilities_t){
        .elicitation = 1,
    });
    cmcp_client_set_elicitation_handler(cli, mock_elicit_accept, &mctx);
    TEST_ASSERT(cmcp_client_handshake(cli, p.client_t) == CMCP_OK);

    TEST_ASSERT(wait_for_capture(&cap));

    TEST_ASSERT(cap.has_caps == 1);
    TEST_ASSERT(cap.client_elicit_cap == 1);

    pthread_mutex_lock(&mctx.mu);
    TEST_ASSERT(mctx.invoked == 1);
    TEST_ASSERT(mctx.captured_message != NULL);
    TEST_ASSERT(mctx.captured_message &&
                strcmp(mctx.captured_message, "Delete /tmp/scratch?") == 0);
    pthread_mutex_unlock(&mctx.mu);

    TEST_ASSERT(cap.captured.kind == CMCP_MSG_RESPONSE);
    TEST_ASSERT(cap.captured.error == NULL);
    TEST_ASSERT(cap.captured.result &&
                cap.captured.result->type == CMCP_JSON_OBJECT);
    if (cap.captured.result) {
        const cmcp_json_t *act = cmcp_json_object_get(cap.captured.result, "action");
        const cmcp_json_t *con = cmcp_json_object_get(cap.captured.result, "content");
        TEST_ASSERT(act && act->type == CMCP_JSON_STRING &&
                    strcmp(act->str.s, "accept") == 0);
        TEST_ASSERT(con && con->type == CMCP_JSON_OBJECT);
        if (con) {
            const cmcp_json_t *cv = cmcp_json_object_get(con, "confirm");
            TEST_ASSERT(cv && cv->type == CMCP_JSON_BOOL && cv->b == 1);
        }
    }

    cmcp_client_free(cli);
    cmcp_transport_close(p.client_t);
    pthread_join(th, NULL);
    cmcp_transport_close(p.server_t);
    free(mctx.captured_message);
    pthread_mutex_destroy(&mctx.mu);
    capture_clear(&cap);
}

static void test_no_handler_replies_method_not_found(void) {
    transport_pair_t p = {0};
    TEST_ASSERT(make_pair(&p) == 0);

    elicit_capture_t cap; capture_init(&cap);
    miniserver_arg_t marg = {
        p.server_t, &cap, make_elicit_params("Hi?"),
    };
    pthread_t th;
    pthread_create(&th, NULL, miniserver_thread, &marg);

    cmcp_client_t *cli = cmcp_client_new("host", "0.0.1");
    /* No handler, no cap. */
    TEST_ASSERT(cmcp_client_handshake(cli, p.client_t) == CMCP_OK);

    TEST_ASSERT(wait_for_capture(&cap));
    TEST_ASSERT(cap.client_elicit_cap == 0);
    TEST_ASSERT(cap.captured.kind  == CMCP_MSG_RESPONSE);
    TEST_ASSERT(cap.captured.error != NULL);
    TEST_ASSERT(cap.captured.error &&
                cap.captured.error->code == CMCP_RPC_METHOD_NOT_FOUND);

    cmcp_client_free(cli);
    cmcp_transport_close(p.client_t);
    pthread_join(th, NULL);
    cmcp_transport_close(p.server_t);
    capture_clear(&cap);
}

static void test_handler_error_returns_internal(void) {
    transport_pair_t p = {0};
    TEST_ASSERT(make_pair(&p) == 0);

    elicit_capture_t cap; capture_init(&cap);
    miniserver_arg_t marg = {
        p.server_t, &cap, make_elicit_params("Hi?"),
    };
    pthread_t th;
    pthread_create(&th, NULL, miniserver_thread, &marg);

    cmcp_client_t *cli = cmcp_client_new("host", "0.0.1");
    cmcp_client_set_capabilities(cli, &(cmcp_client_capabilities_t){
        .elicitation = 1,
    });
    cmcp_client_set_elicitation_handler(cli, mock_elicit_failing, NULL);
    TEST_ASSERT(cmcp_client_handshake(cli, p.client_t) == CMCP_OK);

    TEST_ASSERT(wait_for_capture(&cap));
    TEST_ASSERT(cap.captured.error != NULL);
    TEST_ASSERT(cap.captured.error &&
                cap.captured.error->code == CMCP_RPC_INTERNAL_ERROR);

    cmcp_client_free(cli);
    cmcp_transport_close(p.client_t);
    pthread_join(th, NULL);
    cmcp_transport_close(p.server_t);
    capture_clear(&cap);
}

static void test_helper_action_validation(void) {
    /* Pure unit test for cmcp_elicitation_result — no transport. */

    /* accept: needs an object content; result carries action + content. */
    cmcp_json_t *content = cmcp_json_new_object();
    cmcp_json_object_set(content, "x", cmcp_json_new_int(1));
    cmcp_json_t *r = cmcp_elicitation_result("accept", content);
    TEST_ASSERT(r != NULL);
    if (r) {
        const cmcp_json_t *a = cmcp_json_object_get(r, "action");
        const cmcp_json_t *c = cmcp_json_object_get(r, "content");
        TEST_ASSERT(a && strcmp(a->str.s, "accept") == 0);
        TEST_ASSERT(c && c->type == CMCP_JSON_OBJECT);
    }
    cmcp_json_free(r);

    /* decline: content is dropped even if passed; only action present. */
    content = cmcp_json_new_object();
    cmcp_json_object_set(content, "x", cmcp_json_new_int(1));
    r = cmcp_elicitation_result("decline", content);
    TEST_ASSERT(r != NULL);
    if (r) {
        TEST_ASSERT(cmcp_json_object_get(r, "content") == NULL);
        const cmcp_json_t *a = cmcp_json_object_get(r, "action");
        TEST_ASSERT(a && strcmp(a->str.s, "decline") == 0);
    }
    cmcp_json_free(r);

    /* cancel: same — no content. */
    r = cmcp_elicitation_result("cancel", NULL);
    TEST_ASSERT(r != NULL);
    if (r) {
        TEST_ASSERT(cmcp_json_object_get(r, "content") == NULL);
        const cmcp_json_t *a = cmcp_json_object_get(r, "action");
        TEST_ASSERT(a && strcmp(a->str.s, "cancel") == 0);
    }
    cmcp_json_free(r);

    /* accept without content: rejected. */
    TEST_ASSERT(cmcp_elicitation_result("accept", NULL) == NULL);

    /* accept with non-object content: rejected (and content freed). */
    cmcp_json_t *bad = cmcp_json_new_string("not an object");
    TEST_ASSERT(cmcp_elicitation_result("accept", bad) == NULL);

    /* Unknown action: rejected (and content freed). */
    cmcp_json_t *unused = cmcp_json_new_object();
    TEST_ASSERT(cmcp_elicitation_result("maybe", unused) == NULL);

    /* NULL action: rejected. */
    TEST_ASSERT(cmcp_elicitation_result(NULL, NULL) == NULL);
}

/* ====================================================================== */

int main(void) {
    fprintf(stderr, "test_elicitation:\n");

    TEST_RUN(test_handler_invoked_and_accept_shape);
    TEST_RUN(test_no_handler_replies_method_not_found);
    TEST_RUN(test_handler_error_returns_internal);
    TEST_RUN(test_helper_action_validation);

    TEST_DONE();
}
