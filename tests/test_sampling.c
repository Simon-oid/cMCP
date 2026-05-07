/* End-to-end tests for the Phase 2.5 sampling-handler surface.
 *
 * The library's `cmcp_server_t` doesn't initiate requests in v0.2, so
 * we hand-roll a mini-server (same pattern as test_server_notification
 * in test_client_server.c) that:
 *   1. Completes the initialize handshake.
 *   2. Sends a `sampling/createMessage` request.
 *   3. Captures the response or error code.
 *
 * The host-side wiring under test:
 *   - cmcp_client_set_sampling_handler stores fn + ud
 *   - reader thread, on REQUEST with method=sampling/createMessage:
 *       handler set    → invoke, build success response
 *       handler NULL   → reply -32601 (default decline)
 *       handler error  → reply -32603
 *   - cmcp_sampling_text_result helper builds the standard shape
 *   - caps.sampling = 1 propagates in the initialize request
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
/* Mini-server: handshake → sampling/createMessage → capture response       */
/* ====================================================================== */

typedef struct {
    pthread_mutex_t       mu;
    pthread_cond_t        cv;
    int                   done;        /* 1 once we've captured the response */
    int                   has_caps;    /* did the initialize carry sampling cap? */
    int                   client_sampling_cap;  /* the value */
    cmcp_rpc_message_t    captured;    /* moved-out copy of the response */
} sampling_capture_t;

static void capture_init(sampling_capture_t *c) {
    pthread_mutex_init(&c->mu, NULL);
    pthread_cond_init(&c->cv, NULL);
    c->done = 0;
    c->has_caps = 0;
    c->client_sampling_cap = 0;
    cmcp_rpc_message_init(&c->captured);
}

static void capture_clear(sampling_capture_t *c) {
    pthread_mutex_destroy(&c->mu);
    pthread_cond_destroy(&c->cv);
    cmcp_rpc_message_clear(&c->captured);
}

typedef struct {
    cmcp_transport_t   *t;
    sampling_capture_t *cap;
    /* Sampling request the mini-server should send (params object).
     * Mini-server takes ownership; freed after send. */
    cmcp_json_t        *sampling_params;
} miniserver_arg_t;

static void *miniserver_thread(void *arg) {
    miniserver_arg_t *a = (miniserver_arg_t *)arg;
    cmcp_transport_t *t = a->t;

    /* 1. Read initialize. Capture client capabilities along the way. */
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
            a->cap->client_sampling_cap =
                cmcp_json_object_get(cc, "sampling") ? 1 : 0;
        }
    }
    pthread_mutex_unlock(&a->cap->mu);

    /* 2. Reply minimal initialize success. */
    cmcp_json_t *result = cmcp_json_new_object();
    cmcp_json_object_set(result, "protocolVersion",
                          cmcp_json_new_string(CMCP_PROTOCOL_VERSION));
    cmcp_json_object_set(result, "capabilities", cmcp_json_new_object());
    cmcp_json_t *si = cmcp_json_new_object();
    cmcp_json_object_set(si, "name",    cmcp_json_new_string("samp-srv"));
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

    /* 3. Read notifications/initialized. */
    if (cmcp_transport_read(t, &frame, &flen) != CMCP_OK) return NULL;
    free(frame);

    /* 4. Send sampling/createMessage request. id=42 (server picks). */
    cmcp_rpc_message_t req;
    cmcp_rpc_message_init(&req);
    cmcp_rpc_make_request(&req, 42, "sampling/createMessage",
                           a->sampling_params);
    a->sampling_params = NULL;     /* ownership transferred */
    wire = cmcp_rpc_emit(&req);
    cmcp_transport_write(t, wire, strlen(wire));
    free(wire);
    cmcp_rpc_message_clear(&req);

    /* 5. Read the response (or error). Move into capture. */
    if (cmcp_transport_read(t, &frame, &flen) != CMCP_OK) return NULL;
    cmcp_rpc_message_t *rsps = NULL; size_t nr = 0;
    int rrc = cmcp_rpc_parse(frame, flen, &rsps, &nr);
    free(frame);

    pthread_mutex_lock(&a->cap->mu);
    if (rrc == CMCP_OK && nr == 1) {
        a->cap->captured = rsps[0];        /* move */
        cmcp_rpc_message_init(&rsps[0]);   /* prevent double-free */
    }
    a->cap->done = 1;
    pthread_cond_broadcast(&a->cap->cv);
    pthread_mutex_unlock(&a->cap->mu);
    cmcp_rpc_messages_free(rsps, nr);

    /* 6. Drain until client closes. */
    while (cmcp_transport_read(t, &frame, &flen) == CMCP_OK) free(frame);
    return NULL;
}

static int wait_for_capture(sampling_capture_t *c) {
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
/* Sample sampling-request params                                          */
/* ====================================================================== */

static cmcp_json_t *make_sampling_params(const char *user_text) {
    cmcp_json_t *p = cmcp_json_new_object();
    cmcp_json_t *messages = cmcp_json_new_array();
    cmcp_json_t *m = cmcp_json_new_object();
    cmcp_json_t *content = cmcp_json_new_object();
    cmcp_json_object_set(content, "type", cmcp_json_new_string("text"));
    cmcp_json_object_set(content, "text", cmcp_json_new_string(user_text));
    cmcp_json_object_set(m, "role",    cmcp_json_new_string("user"));
    cmcp_json_object_set(m, "content", content);
    cmcp_json_array_append(messages, m);
    cmcp_json_object_set(p, "messages",  messages);
    cmcp_json_object_set(p, "maxTokens", cmcp_json_new_int(64));
    return p;
}

/* ====================================================================== */
/* Test handlers                                                            */
/* ====================================================================== */

typedef struct {
    int            invoked;
    char          *captured_user_text;     /* echo from params */
    pthread_mutex_t mu;
} mock_sampler_ctx_t;

static int mock_sampler_ok(const cmcp_json_t *params, void *userdata,
                            cmcp_json_t **out_result) {
    mock_sampler_ctx_t *ctx = (mock_sampler_ctx_t *)userdata;
    pthread_mutex_lock(&ctx->mu);
    ctx->invoked = 1;
    /* Capture the user message text so the test can verify the
     * params were threaded through correctly. */
    if (params && params->type == CMCP_JSON_OBJECT) {
        const cmcp_json_t *msgs = cmcp_json_object_get(params, "messages");
        if (msgs && msgs->type == CMCP_JSON_ARRAY && msgs->arr.len > 0) {
            const cmcp_json_t *m0 = msgs->arr.items[0];
            if (m0 && m0->type == CMCP_JSON_OBJECT) {
                const cmcp_json_t *c = cmcp_json_object_get(m0, "content");
                if (c && c->type == CMCP_JSON_OBJECT) {
                    const cmcp_json_t *t = cmcp_json_object_get(c, "text");
                    if (t && t->type == CMCP_JSON_STRING) {
                        free(ctx->captured_user_text);
                        ctx->captured_user_text = strdup(t->str.s);
                    }
                }
            }
        }
    }
    pthread_mutex_unlock(&ctx->mu);
    *out_result = cmcp_sampling_text_result(
        "I'd say: 42.", "mock-model-1", "endTurn");
    return *out_result ? CMCP_OK : CMCP_ENOMEM;
}

static int mock_sampler_failing(const cmcp_json_t *params, void *userdata,
                                 cmcp_json_t **out_result) {
    (void)params; (void)userdata; (void)out_result;
    return CMCP_EHANDLER;       /* triggers -32603 INTERNAL_ERROR */
}

/* ====================================================================== */
/* Tests                                                                    */
/* ====================================================================== */

static void test_handler_invoked_and_response_shape(void) {
    transport_pair_t p;
    TEST_ASSERT(make_pair(&p) == 0);

    sampling_capture_t cap; capture_init(&cap);
    miniserver_arg_t marg = {
        p.server_t, &cap, make_sampling_params("What is 6 times 7?"),
    };

    pthread_t th;
    TEST_ASSERT(pthread_create(&th, NULL, miniserver_thread, &marg) == 0);

    mock_sampler_ctx_t mctx = {0};
    pthread_mutex_init(&mctx.mu, NULL);

    cmcp_client_t *cli = cmcp_client_new("host", "0.0.1");
    cmcp_client_set_capabilities(cli, &(cmcp_client_capabilities_t){
        .sampling = 1,
    });
    cmcp_client_set_sampling_handler(cli, mock_sampler_ok, &mctx);
    TEST_ASSERT(cmcp_client_handshake(cli, p.client_t) == CMCP_OK);

    /* Wait for the mini-server to receive a response. */
    TEST_ASSERT(wait_for_capture(&cap));

    /* The server's initialize handler captured the cap flag. */
    TEST_ASSERT(cap.has_caps == 1);
    TEST_ASSERT(cap.client_sampling_cap == 1);

    /* Handler ran and saw the user message. */
    pthread_mutex_lock(&mctx.mu);
    TEST_ASSERT(mctx.invoked == 1);
    TEST_ASSERT(mctx.captured_user_text != NULL);
    TEST_ASSERT(mctx.captured_user_text &&
                strcmp(mctx.captured_user_text, "What is 6 times 7?") == 0);
    pthread_mutex_unlock(&mctx.mu);

    /* Response is well-formed and matches the helper's shape. */
    TEST_ASSERT(cap.captured.kind == CMCP_MSG_RESPONSE);
    TEST_ASSERT(cap.captured.error == NULL);
    TEST_ASSERT(cap.captured.result &&
                cap.captured.result->type == CMCP_JSON_OBJECT);
    if (cap.captured.result) {
        const cmcp_json_t *role  = cmcp_json_object_get(cap.captured.result, "role");
        const cmcp_json_t *cont  = cmcp_json_object_get(cap.captured.result, "content");
        const cmcp_json_t *model = cmcp_json_object_get(cap.captured.result, "model");
        const cmcp_json_t *stop  = cmcp_json_object_get(cap.captured.result, "stopReason");
        TEST_ASSERT(role  && strcmp(role->str.s,  "assistant")    == 0);
        TEST_ASSERT(cont  && cont->type == CMCP_JSON_OBJECT);
        TEST_ASSERT(model && strcmp(model->str.s, "mock-model-1") == 0);
        TEST_ASSERT(stop  && strcmp(stop->str.s,  "endTurn")      == 0);
        if (cont) {
            const cmcp_json_t *typ = cmcp_json_object_get(cont, "type");
            const cmcp_json_t *txt = cmcp_json_object_get(cont, "text");
            TEST_ASSERT(typ && strcmp(typ->str.s, "text")          == 0);
            TEST_ASSERT(txt && strcmp(txt->str.s, "I'd say: 42.")  == 0);
        }
    }

    cmcp_client_free(cli);
    cmcp_transport_close(p.client_t);
    pthread_join(th, NULL);
    cmcp_transport_close(p.server_t);
    free(mctx.captured_user_text);
    pthread_mutex_destroy(&mctx.mu);
    capture_clear(&cap);
}

static void test_no_handler_replies_method_not_found(void) {
    /* Default decline: when the host hasn't registered a sampling
     * handler, the client must reply -32601. The cap flag also
     * should NOT be advertised — the host hasn't opted in. */
    transport_pair_t p;
    TEST_ASSERT(make_pair(&p) == 0);

    sampling_capture_t cap; capture_init(&cap);
    miniserver_arg_t marg = {
        p.server_t, &cap, make_sampling_params("Hi"),
    };
    pthread_t th;
    pthread_create(&th, NULL, miniserver_thread, &marg);

    cmcp_client_t *cli = cmcp_client_new("host", "0.0.1");
    /* No set_sampling_handler, no caps.sampling = 1. */
    TEST_ASSERT(cmcp_client_handshake(cli, p.client_t) == CMCP_OK);

    TEST_ASSERT(wait_for_capture(&cap));
    TEST_ASSERT(cap.client_sampling_cap == 0);
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
    transport_pair_t p;
    TEST_ASSERT(make_pair(&p) == 0);

    sampling_capture_t cap; capture_init(&cap);
    miniserver_arg_t marg = {
        p.server_t, &cap, make_sampling_params("Hi"),
    };
    pthread_t th;
    pthread_create(&th, NULL, miniserver_thread, &marg);

    cmcp_client_t *cli = cmcp_client_new("host", "0.0.1");
    cmcp_client_set_capabilities(cli, &(cmcp_client_capabilities_t){
        .sampling = 1,
    });
    cmcp_client_set_sampling_handler(cli, mock_sampler_failing, NULL);
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

static void test_helper_text_result_shape(void) {
    /* Pure unit test for cmcp_sampling_text_result — no transport. */
    cmcp_json_t *r = cmcp_sampling_text_result("hello", "m", "endTurn");
    TEST_ASSERT(r && r->type == CMCP_JSON_OBJECT);
    if (r) {
        const cmcp_json_t *role = cmcp_json_object_get(r, "role");
        const cmcp_json_t *cont = cmcp_json_object_get(r, "content");
        TEST_ASSERT(role && strcmp(role->str.s, "assistant") == 0);
        TEST_ASSERT(cont && cont->type == CMCP_JSON_OBJECT);
    }
    cmcp_json_free(r);

    /* model and stop_reason are optional — should be omitted, not
     * defaulted, when NULL is passed. */
    r = cmcp_sampling_text_result("ok", NULL, NULL);
    TEST_ASSERT(r != NULL);
    if (r) {
        TEST_ASSERT(cmcp_json_object_get(r, "model")      == NULL);
        TEST_ASSERT(cmcp_json_object_get(r, "stopReason") == NULL);
    }
    cmcp_json_free(r);

    /* NULL text is rejected. */
    TEST_ASSERT(cmcp_sampling_text_result(NULL, "m", "endTurn") == NULL);
}

/* ====================================================================== */

int main(void) {
    fprintf(stderr, "test_sampling:\n");

    TEST_RUN(test_handler_invoked_and_response_shape);
    TEST_RUN(test_no_handler_replies_method_not_found);
    TEST_RUN(test_handler_error_returns_internal);
    TEST_RUN(test_helper_text_result_shape);

    TEST_DONE();
}
