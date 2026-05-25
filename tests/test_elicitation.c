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
#include "cmcp_server.h"
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
/* Emit half — real cmcp_server tool calls cmcp_handler_elicit              */
/* ====================================================================== */

/* A demo tool whose handler asks the host to confirm via elicitation,
 * then echoes the host's answer back in the tool's text response. The
 * tool exists only for this test (and as a working example of the
 * emit half — adopters can mirror this shape). */

typedef struct {
    int   confirm_seen;
    char *captured_msg;
    pthread_mutex_t mu;
} confirm_capture_t;

static int confirm_tool(const cmcp_json_t *arguments, void *userdata,
                        cmcp_handler_ctx_t *hctx,
                        cmcp_json_t **out_content, int *out_is_error) {
    (void)arguments;
    confirm_capture_t *cap = (confirm_capture_t *)userdata;

    cmcp_json_t *schema = cmcp_json_new_object();
    cmcp_json_object_set(schema, "type", cmcp_json_new_string("object"));
    cmcp_json_t *props = cmcp_json_new_object();
    cmcp_json_t *confirm = cmcp_json_new_object();
    cmcp_json_object_set(confirm, "type", cmcp_json_new_string("boolean"));
    cmcp_json_object_set(props, "confirm", confirm);
    cmcp_json_object_set(schema, "properties", props);

    cmcp_json_t *result = NULL;
    int rc = cmcp_handler_elicit(hctx, "Confirm the action?", schema, &result);
    if (rc != CMCP_OK) {
        *out_is_error = 1;
        *out_content  = cmcp_tool_text_content("elicit failed");
        return CMCP_OK;
    }

    const cmcp_json_t *act     = cmcp_json_object_get(result, "action");
    const cmcp_json_t *content = cmcp_json_object_get(result, "content");
    const char *action_s = (act && act->type == CMCP_JSON_STRING) ? act->str.s : "?";
    int confirmed = 0;
    if (content && content->type == CMCP_JSON_OBJECT) {
        const cmcp_json_t *cv = cmcp_json_object_get(content, "confirm");
        if (cv && cv->type == CMCP_JSON_BOOL) confirmed = cv->b;
    }
    pthread_mutex_lock(&cap->mu);
    cap->confirm_seen = confirmed;
    free(cap->captured_msg);
    cap->captured_msg = strdup(action_s);
    pthread_mutex_unlock(&cap->mu);

    /* Format BEFORE freeing the result tree — action_s borrows into it. */
    char buf[64];
    snprintf(buf, sizeof buf, "action=%s confirm=%d", action_s, confirmed);
    *out_content  = cmcp_tool_text_content(buf);
    *out_is_error = 0;
    cmcp_json_free(result);
    return CMCP_OK;
}

/* Host-side elicitation handler that always replies accept with the
 * `confirm` field set true — what a real interactive UI would do after
 * the user clicked OK. */
static int host_accept_confirm(const cmcp_json_t *params, void *userdata,
                                cmcp_json_t **out_result) {
    (void)params; (void)userdata;
    cmcp_json_t *content = cmcp_json_new_object();
    cmcp_json_object_set(content, "confirm", cmcp_json_new_bool(1));
    *out_result = cmcp_elicitation_result("accept", content);
    return *out_result ? CMCP_OK : CMCP_ENOMEM;
}

/* Host-side handler that always declines (no content). */
static int host_decline(const cmcp_json_t *params, void *userdata,
                         cmcp_json_t **out_result) {
    (void)params; (void)userdata;
    *out_result = cmcp_elicitation_result("decline", NULL);
    return *out_result ? CMCP_OK : CMCP_ENOMEM;
}

typedef struct {
    cmcp_server_t    *s;
    cmcp_transport_t *t;
} server_run_arg_t;

static void *server_run_thread(void *arg) {
    server_run_arg_t *a = (server_run_arg_t *)arg;
    cmcp_server_run(a->s, a->t);
    return NULL;
}

static void test_emit_roundtrip_accept(void) {
    transport_pair_t p = {0};
    TEST_ASSERT(make_pair(&p) == 0);

    confirm_capture_t cap = { .confirm_seen = -1, .captured_msg = NULL };
    pthread_mutex_init(&cap.mu, NULL);

    cmcp_server_t *srv = cmcp_server_new("emit-srv", "0.1.0");
    TEST_ASSERT(cmcp_server_add_tool(srv, &(cmcp_tool_t){
        .name = "confirm",
        .description = "Elicit a confirmation from the user.",
        .handler = confirm_tool,
        .userdata = &cap,
    }) == CMCP_OK);

    server_run_arg_t sa = { srv, p.server_t };
    pthread_t srv_th;
    TEST_ASSERT(pthread_create(&srv_th, NULL, server_run_thread, &sa) == 0);

    cmcp_client_t *cli = cmcp_client_new("host", "0.0.1");
    cmcp_client_set_capabilities(cli, &(cmcp_client_capabilities_t){
        .elicitation = 1,
    });
    cmcp_client_set_elicitation_handler(cli, host_accept_confirm, NULL);
    TEST_ASSERT(cmcp_client_handshake(cli, p.client_t) == CMCP_OK);

    /* tools/call confirm — triggers the server-side elicit round-trip. */
    cmcp_json_t *params = cmcp_json_new_object();
    cmcp_json_object_set(params, "name", cmcp_json_new_string("confirm"));
    cmcp_json_object_set(params, "arguments", cmcp_json_new_object());
    cmcp_rpc_message_t resp;
    cmcp_rpc_message_init(&resp);
    TEST_ASSERT(cmcp_client_request(cli, "tools/call", params, &resp) == CMCP_OK);
    TEST_ASSERT(resp.error == NULL);
    TEST_ASSERT(resp.result && resp.result->type == CMCP_JSON_OBJECT);

    /* The tool packaged the host's reply into a text content item. */
    if (resp.result) {
        const cmcp_json_t *content = cmcp_json_object_get(resp.result, "content");
        TEST_ASSERT(content && content->type == CMCP_JSON_ARRAY &&
                    content->arr.len == 1);
        if (content && content->arr.len > 0) {
            const cmcp_json_t *item = content->arr.items[0];
            const cmcp_json_t *txt  = cmcp_json_object_get(item, "text");
            TEST_ASSERT(txt && txt->type == CMCP_JSON_STRING);
            TEST_ASSERT(txt && strcmp(txt->str.s, "action=accept confirm=1") == 0);
        }
    }
    cmcp_rpc_message_clear(&resp);

    pthread_mutex_lock(&cap.mu);
    TEST_ASSERT(cap.confirm_seen == 1);
    TEST_ASSERT(cap.captured_msg && strcmp(cap.captured_msg, "accept") == 0);
    pthread_mutex_unlock(&cap.mu);

    cmcp_client_free(cli);
    cmcp_transport_close(p.client_t);
    pthread_join(srv_th, NULL);
    cmcp_transport_close(p.server_t);
    cmcp_server_free(srv);
    free(cap.captured_msg);
    pthread_mutex_destroy(&cap.mu);
}

static void test_emit_decline(void) {
    transport_pair_t p = {0};
    TEST_ASSERT(make_pair(&p) == 0);

    confirm_capture_t cap = { .confirm_seen = -1, .captured_msg = NULL };
    pthread_mutex_init(&cap.mu, NULL);

    cmcp_server_t *srv = cmcp_server_new("emit-srv", "0.1.0");
    cmcp_server_add_tool(srv, &(cmcp_tool_t){
        .name = "confirm",
        .handler = confirm_tool,
        .userdata = &cap,
    });

    server_run_arg_t sa = { srv, p.server_t };
    pthread_t srv_th;
    pthread_create(&srv_th, NULL, server_run_thread, &sa);

    cmcp_client_t *cli = cmcp_client_new("host", "0.0.1");
    cmcp_client_set_capabilities(cli, &(cmcp_client_capabilities_t){
        .elicitation = 1,
    });
    cmcp_client_set_elicitation_handler(cli, host_decline, NULL);
    TEST_ASSERT(cmcp_client_handshake(cli, p.client_t) == CMCP_OK);

    cmcp_json_t *params = cmcp_json_new_object();
    cmcp_json_object_set(params, "name", cmcp_json_new_string("confirm"));
    cmcp_rpc_message_t resp;
    cmcp_rpc_message_init(&resp);
    TEST_ASSERT(cmcp_client_request(cli, "tools/call", params, &resp) == CMCP_OK);
    TEST_ASSERT(resp.error == NULL);

    /* No content on decline → confirm=0 in the tool's echoed string. */
    if (resp.result) {
        const cmcp_json_t *content = cmcp_json_object_get(resp.result, "content");
        if (content && content->arr.len > 0) {
            const cmcp_json_t *txt = cmcp_json_object_get(
                content->arr.items[0], "text");
            TEST_ASSERT(txt && strcmp(txt->str.s, "action=decline confirm=0") == 0);
        }
    }
    cmcp_rpc_message_clear(&resp);

    pthread_mutex_lock(&cap.mu);
    TEST_ASSERT(cap.confirm_seen == 0);
    TEST_ASSERT(cap.captured_msg && strcmp(cap.captured_msg, "decline") == 0);
    pthread_mutex_unlock(&cap.mu);

    cmcp_client_free(cli);
    cmcp_transport_close(p.client_t);
    pthread_join(srv_th, NULL);
    cmcp_transport_close(p.server_t);
    cmcp_server_free(srv);
    free(cap.captured_msg);
    pthread_mutex_destroy(&cap.mu);
}

static void test_emit_without_peer_cap(void) {
    /* Server tool calls elicit but the host never advertised the cap.
     * cmcp_handler_elicit must short-circuit with CMCP_EUNSUPPORTED;
     * the tool surfaces that as isError. No round-trip happens — that's
     * the point of the cap gate. */
    transport_pair_t p = {0};
    TEST_ASSERT(make_pair(&p) == 0);

    confirm_capture_t cap = { .confirm_seen = -1, .captured_msg = NULL };
    pthread_mutex_init(&cap.mu, NULL);

    cmcp_server_t *srv = cmcp_server_new("emit-srv", "0.1.0");
    cmcp_server_add_tool(srv, &(cmcp_tool_t){
        .name = "confirm",
        .handler = confirm_tool,
        .userdata = &cap,
    });

    server_run_arg_t sa = { srv, p.server_t };
    pthread_t srv_th;
    pthread_create(&srv_th, NULL, server_run_thread, &sa);

    cmcp_client_t *cli = cmcp_client_new("host", "0.0.1");
    /* Deliberately no caps.elicitation and no handler. */
    TEST_ASSERT(cmcp_client_handshake(cli, p.client_t) == CMCP_OK);

    cmcp_json_t *params = cmcp_json_new_object();
    cmcp_json_object_set(params, "name", cmcp_json_new_string("confirm"));
    cmcp_rpc_message_t resp;
    cmcp_rpc_message_init(&resp);
    TEST_ASSERT(cmcp_client_request(cli, "tools/call", params, &resp) == CMCP_OK);
    TEST_ASSERT(resp.error == NULL);     /* tool-level isError, not RPC error */
    if (resp.result) {
        const cmcp_json_t *err = cmcp_json_object_get(resp.result, "isError");
        TEST_ASSERT(err && err->type == CMCP_JSON_BOOL && err->b == 1);
    }
    cmcp_rpc_message_clear(&resp);

    cmcp_client_free(cli);
    cmcp_transport_close(p.client_t);
    pthread_join(srv_th, NULL);
    cmcp_transport_close(p.server_t);
    cmcp_server_free(srv);
    free(cap.captured_msg);
    pthread_mutex_destroy(&cap.mu);
}

/* ====================================================================== */
/* URL-mode elicitation (MCP 2025-11-25 SEP-1036)                          */
/* ====================================================================== */

/* A tool whose handler asks the host to send the user to a URL. Mirrors
 * confirm_tool's shape but uses cmcp_handler_elicit_url. */
static int url_tool(const cmcp_json_t *arguments, void *userdata,
                     cmcp_handler_ctx_t *hctx,
                     cmcp_json_t **out_content, int *out_is_error) {
    (void)arguments; (void)userdata;
    cmcp_json_t *result = NULL;
    int rc = cmcp_handler_elicit_url(hctx, "Please authorize",
                                      "https://example.com/consent",
                                      &result);
    if (rc != CMCP_OK) {
        *out_is_error = 1;
        *out_content  = cmcp_tool_text_content(
            rc == CMCP_EUNSUPPORTED ? "url-cap missing" : "url-elicit failed");
        return CMCP_OK;
    }
    const cmcp_json_t *act = cmcp_json_object_get(result, "action");
    const char *action_s = (act && act->type == CMCP_JSON_STRING)
                            ? act->str.s : "?";
    char buf[64];
    snprintf(buf, sizeof buf, "url-action=%s", action_s);
    *out_content  = cmcp_tool_text_content(buf);
    *out_is_error = 0;
    cmcp_json_free(result);
    return CMCP_OK;
}

/* Host-side URL elicitation handler. Captures whether the params carried
 * the URL-mode shape (mode=="url" + url field present). */
typedef struct {
    int             saw_mode_url;
    int             saw_url;
    pthread_mutex_t mu;
} url_capture_t;

static int host_url_accept(const cmcp_json_t *params, void *userdata,
                            cmcp_json_t **out_result) {
    url_capture_t *cap = (url_capture_t *)userdata;
    pthread_mutex_lock(&cap->mu);
    if (params && params->type == CMCP_JSON_OBJECT) {
        const cmcp_json_t *m = cmcp_json_object_get(params, "mode");
        const cmcp_json_t *u = cmcp_json_object_get(params, "url");
        cap->saw_mode_url = (m && m->type == CMCP_JSON_STRING &&
                             strcmp(m->str.s, "url") == 0);
        cap->saw_url      = (u && u->type == CMCP_JSON_STRING &&
                             strcmp(u->str.s, "https://example.com/consent") == 0);
    }
    pthread_mutex_unlock(&cap->mu);
    /* No content payload on a URL-mode accept — the redirect *is* the
     * payload; the spec leaves "what the user did at the URL" to the
     * server's followup. */
    *out_result = cmcp_elicitation_result("accept", NULL);
    if (!*out_result) {
        /* Fallback: helper rejects "accept" without content — wrap an
         * empty object so the helper accepts the tuple. */
        *out_result = cmcp_elicitation_result("accept", cmcp_json_new_object());
    }
    return *out_result ? CMCP_OK : CMCP_ENOMEM;
}

static void test_emit_url_roundtrip(void) {
    transport_pair_t p = {0};
    TEST_ASSERT(make_pair(&p) == 0);

    url_capture_t cap = { .saw_mode_url = 0, .saw_url = 0 };
    pthread_mutex_init(&cap.mu, NULL);

    cmcp_server_t *srv = cmcp_server_new("url-srv", "0.1.0");
    cmcp_server_add_tool(srv, &(cmcp_tool_t){
        .name = "authorize",
        .description = "URL-mode elicitation demo.",
        .handler = url_tool,
    });

    server_run_arg_t sa = { srv, p.server_t };
    pthread_t srv_th;
    pthread_create(&srv_th, NULL, server_run_thread, &sa);

    cmcp_client_t *cli = cmcp_client_new("host", "0.0.1");
    cmcp_client_set_capabilities(cli, &(cmcp_client_capabilities_t){
        .elicitation     = 1,
        .elicitation_url = 1,
    });
    cmcp_client_set_elicitation_handler(cli, host_url_accept, &cap);
    TEST_ASSERT(cmcp_client_handshake(cli, p.client_t) == CMCP_OK);

    cmcp_json_t *params = cmcp_json_new_object();
    cmcp_json_object_set(params, "name", cmcp_json_new_string("authorize"));
    cmcp_rpc_message_t resp;
    cmcp_rpc_message_init(&resp);
    TEST_ASSERT(cmcp_client_request(cli, "tools/call", params, &resp) == CMCP_OK);
    TEST_ASSERT(resp.error == NULL);
    if (resp.result) {
        const cmcp_json_t *content = cmcp_json_object_get(resp.result, "content");
        TEST_ASSERT(content && content->arr.len > 0);
        if (content && content->arr.len > 0) {
            const cmcp_json_t *txt = cmcp_json_object_get(
                content->arr.items[0], "text");
            TEST_ASSERT(txt && strcmp(txt->str.s, "url-action=accept") == 0);
        }
    }
    cmcp_rpc_message_clear(&resp);

    pthread_mutex_lock(&cap.mu);
    TEST_ASSERT(cap.saw_mode_url == 1);
    TEST_ASSERT(cap.saw_url      == 1);
    pthread_mutex_unlock(&cap.mu);

    cmcp_client_free(cli);
    cmcp_transport_close(p.client_t);
    pthread_join(srv_th, NULL);
    cmcp_transport_close(p.server_t);
    cmcp_server_free(srv);
    pthread_mutex_destroy(&cap.mu);
}

static void test_emit_url_without_url_subcap(void) {
    /* Host advertises elicitation (form-only — no url sub-cap). Server's
     * URL helper short-circuits with CMCP_EUNSUPPORTED; tool reports
     * "url-cap missing" via isError. The host's elicitation handler is
     * never invoked. */
    transport_pair_t p = {0};
    TEST_ASSERT(make_pair(&p) == 0);

    url_capture_t cap = { .saw_mode_url = 0, .saw_url = 0 };
    pthread_mutex_init(&cap.mu, NULL);

    cmcp_server_t *srv = cmcp_server_new("url-srv", "0.1.0");
    cmcp_server_add_tool(srv, &(cmcp_tool_t){
        .name = "authorize",
        .handler = url_tool,
    });

    server_run_arg_t sa = { srv, p.server_t };
    pthread_t srv_th;
    pthread_create(&srv_th, NULL, server_run_thread, &sa);

    cmcp_client_t *cli = cmcp_client_new("host", "0.0.1");
    /* form-only opt-in — no elicitation_url */
    cmcp_client_set_capabilities(cli, &(cmcp_client_capabilities_t){
        .elicitation      = 1,
        .elicitation_form = 1,
    });
    cmcp_client_set_elicitation_handler(cli, host_url_accept, &cap);
    TEST_ASSERT(cmcp_client_handshake(cli, p.client_t) == CMCP_OK);

    cmcp_json_t *params = cmcp_json_new_object();
    cmcp_json_object_set(params, "name", cmcp_json_new_string("authorize"));
    cmcp_rpc_message_t resp;
    cmcp_rpc_message_init(&resp);
    TEST_ASSERT(cmcp_client_request(cli, "tools/call", params, &resp) == CMCP_OK);
    TEST_ASSERT(resp.error == NULL);
    if (resp.result) {
        const cmcp_json_t *err = cmcp_json_object_get(resp.result, "isError");
        TEST_ASSERT(err && err->type == CMCP_JSON_BOOL && err->b == 1);
        const cmcp_json_t *content = cmcp_json_object_get(resp.result, "content");
        if (content && content->arr.len > 0) {
            const cmcp_json_t *txt = cmcp_json_object_get(
                content->arr.items[0], "text");
            TEST_ASSERT(txt && strcmp(txt->str.s, "url-cap missing") == 0);
        }
    }
    cmcp_rpc_message_clear(&resp);

    pthread_mutex_lock(&cap.mu);
    TEST_ASSERT(cap.saw_mode_url == 0);
    TEST_ASSERT(cap.saw_url      == 0);
    pthread_mutex_unlock(&cap.mu);

    cmcp_client_free(cli);
    cmcp_transport_close(p.client_t);
    pthread_join(srv_th, NULL);
    cmcp_transport_close(p.server_t);
    cmcp_server_free(srv);
    pthread_mutex_destroy(&cap.mu);
}

/* ====================================================================== */

int main(void) {
    fprintf(stderr, "test_elicitation:\n");

    TEST_RUN(test_handler_invoked_and_accept_shape);
    TEST_RUN(test_no_handler_replies_method_not_found);
    TEST_RUN(test_handler_error_returns_internal);
    TEST_RUN(test_helper_action_validation);
    TEST_RUN(test_emit_roundtrip_accept);
    TEST_RUN(test_emit_decline);
    TEST_RUN(test_emit_without_peer_cap);
    TEST_RUN(test_emit_url_roundtrip);
    TEST_RUN(test_emit_url_without_url_subcap);

    TEST_DONE();
}
