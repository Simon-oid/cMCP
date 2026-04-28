/* pipe(2) — POSIX. */
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
/* Pipe-pair scaffolding (mirrors test_stdio_roundtrip).                   */
/* ====================================================================== */

typedef struct {
    cmcp_transport_t *client_t;   /* held by the test thread */
    cmcp_transport_t *server_t;   /* handed to the server thread */
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

typedef struct {
    cmcp_server_t    *s;
    cmcp_transport_t *t;
    int               rc;
} server_arg_t;

static void *server_thread(void *arg) {
    server_arg_t *a = (server_arg_t *)arg;
    a->rc = cmcp_server_run(a->s, a->t);
    return NULL;
}

/* Send a request as raw wire bytes and read the single response back.
 * On success, *out_resp owns its fields and must be cleared by caller. */
static int rpc_round_trip(cmcp_transport_t *t, cmcp_rpc_message_t *req,
                          cmcp_rpc_message_t *out_resp) {
    char *wire = cmcp_rpc_emit(req);
    if (!wire) return CMCP_ENOMEM;
    int rc = cmcp_transport_write(t, wire, strlen(wire));
    free(wire);
    if (rc != CMCP_OK) return rc;

    char *frame = NULL; size_t flen = 0;
    rc = cmcp_transport_read(t, &frame, &flen);
    if (rc != CMCP_OK) return rc;

    cmcp_rpc_message_t *msgs = NULL; size_t n = 0;
    int prc = cmcp_rpc_parse(frame, flen, &msgs, &n);
    free(frame);
    if (prc != CMCP_OK || n != 1) {
        cmcp_rpc_messages_free(msgs, n);
        return CMCP_EPARSE;
    }
    *out_resp = msgs[0];
    cmcp_rpc_message_init(&msgs[0]);   /* prevent double-free */
    cmcp_rpc_messages_free(msgs, n);
    return CMCP_OK;
}

/* ====================================================================== */
/* Tests                                                                   */
/* ====================================================================== */

static void test_happy_path(void) {
    transport_pair_t p;
    TEST_ASSERT(make_pair(&p) == 0);

    cmcp_server_t *srv = cmcp_server_new("test-server", "0.1.0");
    cmcp_server_capabilities_t scaps = {0};
    scaps.tools_list_changed   = 1;
    scaps.resources_subscribe  = 1;
    scaps.prompts_list_changed = 1;
    cmcp_server_set_capabilities(srv, &scaps);

    server_arg_t sa = { srv, p.server_t, 0 };
    pthread_t th;
    TEST_ASSERT(pthread_create(&th, NULL, server_thread, &sa) == 0);

    cmcp_client_t *cli = cmcp_client_new("test-client", "0.0.1");
    cmcp_client_capabilities_t ccaps = {0};
    ccaps.sampling           = 1;
    ccaps.roots_list_changed = 1;
    cmcp_client_set_capabilities(cli, &ccaps);

    TEST_ASSERT(cmcp_client_handshake(cli, p.client_t) == CMCP_OK);

    /* Client sees server identity + caps. */
    TEST_ASSERT(cmcp_client_server_name(cli) != NULL);
    TEST_ASSERT(strcmp(cmcp_client_server_name(cli), "test-server") == 0);
    TEST_ASSERT(strcmp(cmcp_client_server_version(cli), "0.1.0") == 0);
    const cmcp_server_capabilities_t *scap_seen = cmcp_client_server_caps(cli);
    TEST_ASSERT(scap_seen != NULL);
    TEST_ASSERT(scap_seen->tools_list_changed == 1);
    TEST_ASSERT(scap_seen->resources_subscribe == 1);
    TEST_ASSERT(scap_seen->resources_list_changed == 0);
    TEST_ASSERT(scap_seen->prompts_list_changed == 1);

    /* Closing the client end shuts the server loop down cleanly. */
    cmcp_client_free(cli);
    cmcp_transport_close(p.client_t);
    pthread_join(th, NULL);
    TEST_ASSERT(sa.rc == CMCP_OK);

    /* Server captured client identity + caps during handshake. */
    TEST_ASSERT(cmcp_server_client_name(srv) != NULL);
    TEST_ASSERT(strcmp(cmcp_server_client_name(srv), "test-client") == 0);
    TEST_ASSERT(strcmp(cmcp_server_client_version(srv), "0.0.1") == 0);
    const cmcp_client_capabilities_t *ccap_seen = cmcp_server_client_caps(srv);
    TEST_ASSERT(ccap_seen != NULL);
    TEST_ASSERT(ccap_seen->sampling == 1);
    TEST_ASSERT(ccap_seen->roots_list_changed == 1);

    cmcp_server_free(srv);
    cmcp_transport_close(p.server_t);
}

static void test_version_mismatch(void) {
    transport_pair_t p;
    TEST_ASSERT(make_pair(&p) == 0);

    cmcp_server_t *srv = cmcp_server_new("test-server", "0.1.0");
    server_arg_t sa = { srv, p.server_t, 0 };
    pthread_t th;
    TEST_ASSERT(pthread_create(&th, NULL, server_thread, &sa) == 0);

    /* Build initialize with a bogus protocol version. */
    cmcp_json_t *params = cmcp_json_new_object();
    cmcp_json_object_set(params, "protocolVersion",
                         cmcp_json_new_string("1999-01-01"));
    cmcp_json_object_set(params, "capabilities", cmcp_json_new_object());
    cmcp_json_t *ci = cmcp_json_new_object();
    cmcp_json_object_set(ci, "name",    cmcp_json_new_string("c"));
    cmcp_json_object_set(ci, "version", cmcp_json_new_string("0"));
    cmcp_json_object_set(params, "clientInfo", ci);

    cmcp_rpc_message_t req;
    TEST_ASSERT(cmcp_rpc_make_request(&req, 1, "initialize", params)
                == CMCP_OK);

    cmcp_rpc_message_t resp;
    TEST_ASSERT(rpc_round_trip(p.client_t, &req, &resp) == CMCP_OK);
    cmcp_rpc_message_clear(&req);

    TEST_ASSERT(resp.kind == CMCP_MSG_RESPONSE);
    TEST_ASSERT(resp.id.kind == CMCP_ID_INT && resp.id.i == 1);
    TEST_ASSERT(resp.error != NULL);
    TEST_ASSERT(resp.error->code == CMCP_RPC_INVALID_PARAMS);
    TEST_ASSERT(resp.error->data != NULL);
    TEST_ASSERT(resp.error->data->type == CMCP_JSON_OBJECT);
    const cmcp_json_t *sup = cmcp_json_object_get(resp.error->data, "supported");
    const cmcp_json_t *got = cmcp_json_object_get(resp.error->data, "requested");
    TEST_ASSERT(sup && sup->type == CMCP_JSON_STRING);
    TEST_ASSERT(strcmp(sup->str.s, CMCP_PROTOCOL_VERSION) == 0);
    TEST_ASSERT(got && got->type == CMCP_JSON_STRING);
    TEST_ASSERT(strcmp(got->str.s, "1999-01-01") == 0);
    cmcp_rpc_message_clear(&resp);

    cmcp_transport_close(p.client_t);
    pthread_join(th, NULL);
    cmcp_server_free(srv);
    cmcp_transport_close(p.server_t);
}

static void test_double_initialize(void) {
    transport_pair_t p;
    TEST_ASSERT(make_pair(&p) == 0);

    cmcp_server_t *srv = cmcp_server_new("test-server", "0.1.0");
    server_arg_t sa = { srv, p.server_t, 0 };
    pthread_t th;
    TEST_ASSERT(pthread_create(&th, NULL, server_thread, &sa) == 0);

    cmcp_client_t *cli = cmcp_client_new("test-client", "0.0.1");
    TEST_ASSERT(cmcp_client_handshake(cli, p.client_t) == CMCP_OK);

    /* Send a second `initialize` directly over the wire (the client
     * API doesn't expose this; we have to bypass it). */
    cmcp_json_t *params = cmcp_json_new_object();
    cmcp_json_object_set(params, "protocolVersion",
                         cmcp_json_new_string(CMCP_PROTOCOL_VERSION));
    cmcp_json_object_set(params, "capabilities", cmcp_json_new_object());

    cmcp_rpc_message_t req;
    TEST_ASSERT(cmcp_rpc_make_request(&req, 99, "initialize", params)
                == CMCP_OK);

    cmcp_rpc_message_t resp;
    TEST_ASSERT(rpc_round_trip(p.client_t, &req, &resp) == CMCP_OK);
    cmcp_rpc_message_clear(&req);

    TEST_ASSERT(resp.kind == CMCP_MSG_RESPONSE);
    TEST_ASSERT(resp.id.kind == CMCP_ID_INT && resp.id.i == 99);
    TEST_ASSERT(resp.error != NULL);
    TEST_ASSERT(resp.error->code == CMCP_RPC_INVALID_REQUEST);
    cmcp_rpc_message_clear(&resp);

    cmcp_client_free(cli);
    cmcp_transport_close(p.client_t);
    pthread_join(th, NULL);
    cmcp_server_free(srv);
    cmcp_transport_close(p.server_t);
}

static void test_operate_before_initialized(void) {
    transport_pair_t p;
    TEST_ASSERT(make_pair(&p) == 0);

    cmcp_server_t *srv = cmcp_server_new("test-server", "0.1.0");
    server_arg_t sa = { srv, p.server_t, 0 };
    pthread_t th;
    TEST_ASSERT(pthread_create(&th, NULL, server_thread, &sa) == 0);

    /* Pre-handshake operate-class request must be rejected as -32600.
     * The state-check fires before the method lookup, so even a
     * known method like `tools/list` is rejected here. */
    cmcp_rpc_message_t req;
    TEST_ASSERT(cmcp_rpc_make_request(&req, 7, "tools/list", NULL)
                == CMCP_OK);

    cmcp_rpc_message_t resp;
    TEST_ASSERT(rpc_round_trip(p.client_t, &req, &resp) == CMCP_OK);
    cmcp_rpc_message_clear(&req);

    TEST_ASSERT(resp.kind == CMCP_MSG_RESPONSE);
    TEST_ASSERT(resp.id.kind == CMCP_ID_INT && resp.id.i == 7);
    TEST_ASSERT(resp.error != NULL);
    TEST_ASSERT(resp.error->code == CMCP_RPC_INVALID_REQUEST);
    cmcp_rpc_message_clear(&resp);

    cmcp_transport_close(p.client_t);
    pthread_join(th, NULL);
    cmcp_server_free(srv);
    cmcp_transport_close(p.server_t);
}

static void test_unknown_method_after_init(void) {
    transport_pair_t p;
    TEST_ASSERT(make_pair(&p) == 0);

    cmcp_server_t *srv = cmcp_server_new("test-server", "0.1.0");
    server_arg_t sa = { srv, p.server_t, 0 };
    pthread_t th;
    TEST_ASSERT(pthread_create(&th, NULL, server_thread, &sa) == 0);

    cmcp_client_t *cli = cmcp_client_new("test-client", "0.0.1");
    TEST_ASSERT(cmcp_client_handshake(cli, p.client_t) == CMCP_OK);

    /* `frobs/list` is intentionally not a method we implement. */
    cmcp_rpc_message_t resp;
    TEST_ASSERT(cmcp_client_request(cli, "frobs/list", NULL, &resp) == CMCP_OK);
    TEST_ASSERT(resp.kind == CMCP_MSG_RESPONSE);
    TEST_ASSERT(resp.error != NULL);
    TEST_ASSERT(resp.error->code == CMCP_RPC_METHOD_NOT_FOUND);
    cmcp_rpc_message_clear(&resp);

    cmcp_client_free(cli);
    cmcp_transport_close(p.client_t);
    pthread_join(th, NULL);
    cmcp_server_free(srv);
    cmcp_transport_close(p.server_t);
}

static void test_notification_dropped_silently(void) {
    /* Notifications never get a reply. We send `notifications/foo` and
     * follow with a request; the request's reply is what we read,
     * proving no errant frame appeared between them. */
    transport_pair_t p;
    TEST_ASSERT(make_pair(&p) == 0);

    cmcp_server_t *srv = cmcp_server_new("test-server", "0.1.0");
    server_arg_t sa = { srv, p.server_t, 0 };
    pthread_t th;
    TEST_ASSERT(pthread_create(&th, NULL, server_thread, &sa) == 0);

    cmcp_client_t *cli = cmcp_client_new("test-client", "0.0.1");
    TEST_ASSERT(cmcp_client_handshake(cli, p.client_t) == CMCP_OK);

    /* Stray notification — server should ignore. */
    TEST_ASSERT(cmcp_client_notify(cli, "notifications/bogus", NULL)
                == CMCP_OK);

    /* Follow-up unknown-method request: response code must be -32601
     * with a matching id — proves we didn't read a stale frame from
     * the notification. */
    cmcp_rpc_message_t resp;
    TEST_ASSERT(cmcp_client_request(cli, "frobs/list", NULL, &resp)
                == CMCP_OK);
    TEST_ASSERT(resp.error != NULL);
    TEST_ASSERT(resp.error->code == CMCP_RPC_METHOD_NOT_FOUND);
    cmcp_rpc_message_clear(&resp);

    cmcp_client_free(cli);
    cmcp_transport_close(p.client_t);
    pthread_join(th, NULL);
    cmcp_server_free(srv);
    cmcp_transport_close(p.server_t);
}

/* ====================================================================== */

int main(void) {
    fprintf(stderr, "test_lifecycle:\n");

    TEST_RUN(test_happy_path);
    TEST_RUN(test_version_mismatch);
    TEST_RUN(test_double_initialize);
    TEST_RUN(test_operate_before_initialized);
    TEST_RUN(test_unknown_method_after_init);
    TEST_RUN(test_notification_dropped_silently);

    TEST_DONE();
}
