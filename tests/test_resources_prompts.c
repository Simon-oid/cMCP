/* End-to-end tests for the Phase 2.3 resources + prompts surface.
 *
 * Mirrors test_tools.c's pipe-pair scaffolding:
 *   - server runs on a pthread over a pipe pair
 *   - client drives via cmcp_client_request
 *   - assertions are made against the parsed result trees
 *
 * Plus session-side aggregation tests (two servers, fan-out/fan-in)
 * mirroring test_client_server.c.
 */

#define _POSIX_C_SOURCE 200809L

#include "test.h"
#include "cmcp.h"
#include "cmcp_client.h"
#include "cmcp_json.h"
#include "cmcp_server.h"
#include "cmcp_session.h"
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

/* ====================================================================== */
/* Sample resource + prompt handlers                                       */
/* ====================================================================== */

typedef struct { int reads; } stats_ctx_t;

static int stats_read(const char *uri, void *userdata,
                       cmcp_handler_ctx_t *hctx,
                       cmcp_json_t **out_contents, int *out_is_error) {
    (void)hctx;
    stats_ctx_t *ctx = (stats_ctx_t *)userdata;
    if (ctx) ctx->reads++;
    *out_is_error = 0;
    *out_contents = cmcp_resource_text_contents(uri, "text/plain",
                                                 "chunks=12 files=3");
    return *out_contents ? CMCP_OK : CMCP_ENOMEM;
}

static int unhealthy_read(const char *uri, void *userdata,
                           cmcp_handler_ctx_t *hctx,
                           cmcp_json_t **out_contents, int *out_is_error) {
    (void)userdata; (void)hctx;
    *out_is_error = 1;
    *out_contents = cmcp_resource_text_contents(uri, "text/plain",
                                                 "transient backend error");
    return CMCP_OK;
}

static int code_review_handler(const cmcp_json_t *args, void *userdata,
                                cmcp_handler_ctx_t *hctx,
                                cmcp_json_t **out_messages) {
    (void)userdata; (void)hctx;
    const cmcp_json_t *lang = args ? cmcp_json_object_get(args, "language") : NULL;
    const char *l = (lang && lang->type == CMCP_JSON_STRING) ? lang->str.s : "?";
    char buf[256];
    snprintf(buf, sizeof buf, "Review the following %s code.", l);
    *out_messages = cmcp_prompt_text_messages("user", buf);
    return *out_messages ? CMCP_OK : CMCP_ENOMEM;
}

static int empty_prompt_handler(const cmcp_json_t *args, void *userdata,
                                 cmcp_handler_ctx_t *hctx,
                                 cmcp_json_t **out_messages) {
    (void)args; (void)userdata; (void)hctx;
    *out_messages = NULL;
    return CMCP_OK;
}

/* ====================================================================== */
/* Registration                                                            */
/* ====================================================================== */

static void test_register_resource(void) {
    cmcp_server_t *srv = cmcp_server_new("rp-srv", "0.1.0");

    int rc = cmcp_server_add_resource(srv, &(cmcp_resource_t){
        .uri         = "test://stats",
        .name        = "Stats",
        .description = "DB stats",
        .mime_type   = "text/plain",
        .read        = stats_read,
    });
    TEST_ASSERT(rc == CMCP_OK);

    /* Duplicate URI → CMCP_EPROTOCOL. */
    rc = cmcp_server_add_resource(srv, &(cmcp_resource_t){
        .uri = "test://stats", .name = "x", .read = stats_read,
    });
    TEST_ASSERT(rc == CMCP_EPROTOCOL);

    /* Missing read → CMCP_EINVAL. */
    rc = cmcp_server_add_resource(srv, &(cmcp_resource_t){
        .uri = "test://other", .name = "x",
    });
    TEST_ASSERT(rc == CMCP_EINVAL);

    /* Missing uri → CMCP_EINVAL. */
    rc = cmcp_server_add_resource(srv, &(cmcp_resource_t){
        .name = "x", .read = stats_read,
    });
    TEST_ASSERT(rc == CMCP_EINVAL);

    /* Missing name → CMCP_EINVAL. */
    rc = cmcp_server_add_resource(srv, &(cmcp_resource_t){
        .uri = "test://other", .read = stats_read,
    });
    TEST_ASSERT(rc == CMCP_EINVAL);

    cmcp_server_free(srv);
}

static void test_register_prompt(void) {
    cmcp_server_t *srv = cmcp_server_new("rp-srv", "0.1.0");

    int rc = cmcp_server_add_prompt(srv, &(cmcp_prompt_t){
        .name        = "code_review",
        .description = "Review code",
        .arguments   = "[{\"name\":\"language\",\"required\":true}]",
        .handler     = code_review_handler,
    });
    TEST_ASSERT(rc == CMCP_OK);

    /* Duplicate name → CMCP_EPROTOCOL. */
    rc = cmcp_server_add_prompt(srv, &(cmcp_prompt_t){
        .name = "code_review", .handler = code_review_handler,
    });
    TEST_ASSERT(rc == CMCP_EPROTOCOL);

    /* Missing handler → CMCP_EINVAL. */
    rc = cmcp_server_add_prompt(srv, &(cmcp_prompt_t){ .name = "x" });
    TEST_ASSERT(rc == CMCP_EINVAL);

    /* Bad arguments JSON → CMCP_EPARSE. */
    rc = cmcp_server_add_prompt(srv, &(cmcp_prompt_t){
        .name = "p2", .handler = code_review_handler,
        .arguments = "{not-an-array",
    });
    TEST_ASSERT(rc == CMCP_EPARSE);

    /* arguments must be a JSON array, not object → CMCP_EPARSE. */
    rc = cmcp_server_add_prompt(srv, &(cmcp_prompt_t){
        .name = "p3", .handler = code_review_handler,
        .arguments = "{\"name\":\"x\"}",
    });
    TEST_ASSERT(rc == CMCP_EPARSE);

    cmcp_server_free(srv);
}

/* ====================================================================== */
/* resources/list                                                          */
/* ====================================================================== */

static void test_resources_list_over_wire(void) {
    transport_pair_t p;
    TEST_ASSERT(make_pair(&p) == 0);

    cmcp_server_t *srv = cmcp_server_new("rp-srv", "0.1.0");
    stats_ctx_t ctx = {0};
    cmcp_server_add_resource(srv, &(cmcp_resource_t){
        .uri         = "test://stats",
        .name        = "Stats",
        .description = "DB diagnostics",
        .mime_type   = "text/plain",
        .read        = stats_read, .userdata = &ctx,
    });

    server_arg_t sa = { srv, p.server_t, 0 };
    pthread_t th;
    pthread_create(&th, NULL, server_thread, &sa);

    cmcp_client_t *cli = cmcp_client_new("rp-cli", "0.0.1");
    TEST_ASSERT(cmcp_client_handshake(cli, p.client_t) == CMCP_OK);

    cmcp_rpc_message_t resp;
    TEST_ASSERT(cmcp_client_request(cli, "resources/list", NULL, &resp)
                == CMCP_OK);
    TEST_ASSERT(resp.error == NULL);
    TEST_ASSERT(resp.result && resp.result->type == CMCP_JSON_OBJECT);

    const cmcp_json_t *arr = cmcp_json_object_get(resp.result, "resources");
    TEST_ASSERT(arr && arr->type == CMCP_JSON_ARRAY);
    TEST_ASSERT(cmcp_json_array_len(arr) == 1);

    const cmcp_json_t *r0 = cmcp_json_array_at(arr, 0);
    const cmcp_json_t *uri  = cmcp_json_object_get(r0, "uri");
    const cmcp_json_t *name = cmcp_json_object_get(r0, "name");
    const cmcp_json_t *desc = cmcp_json_object_get(r0, "description");
    const cmcp_json_t *mime = cmcp_json_object_get(r0, "mimeType");
    TEST_ASSERT(uri  && strcmp(uri->str.s,  "test://stats")    == 0);
    TEST_ASSERT(name && strcmp(name->str.s, "Stats")           == 0);
    TEST_ASSERT(desc && strcmp(desc->str.s, "DB diagnostics")  == 0);
    TEST_ASSERT(mime && strcmp(mime->str.s, "text/plain")      == 0);

    cmcp_rpc_message_clear(&resp);
    cmcp_client_free(cli);
    cmcp_transport_close(p.client_t);
    pthread_join(th, NULL);
    cmcp_server_free(srv);
    cmcp_transport_close(p.server_t);
}

/* ====================================================================== */
/* resources/read                                                          */
/* ====================================================================== */

static void test_resources_read_success(void) {
    transport_pair_t p;
    TEST_ASSERT(make_pair(&p) == 0);

    cmcp_server_t *srv = cmcp_server_new("rp-srv", "0.1.0");
    stats_ctx_t ctx = {0};
    cmcp_server_add_resource(srv, &(cmcp_resource_t){
        .uri = "test://stats", .name = "Stats", .read = stats_read,
        .mime_type = "text/plain", .userdata = &ctx,
    });

    server_arg_t sa = { srv, p.server_t, 0 };
    pthread_t th;
    pthread_create(&th, NULL, server_thread, &sa);

    cmcp_client_t *cli = cmcp_client_new("rp-cli", "0.0.1");
    TEST_ASSERT(cmcp_client_handshake(cli, p.client_t) == CMCP_OK);

    cmcp_json_t *params = cmcp_json_new_object();
    cmcp_json_object_set(params, "uri", cmcp_json_new_string("test://stats"));

    cmcp_rpc_message_t resp;
    TEST_ASSERT(cmcp_client_request(cli, "resources/read", params, &resp)
                == CMCP_OK);
    TEST_ASSERT(resp.error == NULL);
    TEST_ASSERT(ctx.reads == 1);

    const cmcp_json_t *contents = cmcp_json_object_get(resp.result, "contents");
    TEST_ASSERT(contents && contents->type == CMCP_JSON_ARRAY);
    TEST_ASSERT(cmcp_json_array_len(contents) == 1);
    const cmcp_json_t *item = cmcp_json_array_at(contents, 0);
    const cmcp_json_t *uri  = cmcp_json_object_get(item, "uri");
    const cmcp_json_t *text = cmcp_json_object_get(item, "text");
    const cmcp_json_t *mime = cmcp_json_object_get(item, "mimeType");
    TEST_ASSERT(uri  && strcmp(uri->str.s,  "test://stats")     == 0);
    TEST_ASSERT(text && strcmp(text->str.s, "chunks=12 files=3") == 0);
    TEST_ASSERT(mime && strcmp(mime->str.s, "text/plain")        == 0);

    cmcp_rpc_message_clear(&resp);
    cmcp_client_free(cli);
    cmcp_transport_close(p.client_t);
    pthread_join(th, NULL);
    cmcp_server_free(srv);
    cmcp_transport_close(p.server_t);
}

static void test_resources_read_unknown(void) {
    transport_pair_t p;
    TEST_ASSERT(make_pair(&p) == 0);

    cmcp_server_t *srv = cmcp_server_new("rp-srv", "0.1.0");
    stats_ctx_t ctx = {0};
    cmcp_server_add_resource(srv, &(cmcp_resource_t){
        .uri = "test://stats", .name = "Stats",
        .read = stats_read, .userdata = &ctx,
    });

    server_arg_t sa = { srv, p.server_t, 0 };
    pthread_t th;
    pthread_create(&th, NULL, server_thread, &sa);

    cmcp_client_t *cli = cmcp_client_new("rp-cli", "0.0.1");
    TEST_ASSERT(cmcp_client_handshake(cli, p.client_t) == CMCP_OK);

    cmcp_json_t *params = cmcp_json_new_object();
    cmcp_json_object_set(params, "uri", cmcp_json_new_string("test://nope"));
    cmcp_rpc_message_t resp;
    TEST_ASSERT(cmcp_client_request(cli, "resources/read", params, &resp)
                == CMCP_OK);
    TEST_ASSERT(resp.error != NULL);
    TEST_ASSERT(resp.error && resp.error->code == CMCP_RPC_INVALID_PARAMS);
    /* Structured `data.uri` echoes the bad URI for client surfacing. */
    TEST_ASSERT(resp.error && resp.error->data &&
                resp.error->data->type == CMCP_JSON_OBJECT);
    const cmcp_json_t *bad = cmcp_json_object_get(resp.error->data, "uri");
    TEST_ASSERT(bad && strcmp(bad->str.s, "test://nope") == 0);

    cmcp_rpc_message_clear(&resp);
    cmcp_client_free(cli);
    cmcp_transport_close(p.client_t);
    pthread_join(th, NULL);
    cmcp_server_free(srv);
    cmcp_transport_close(p.server_t);
}

/* Resource handler can flag a resource-level error analogous to
 * tools/call's isError. The wire still carries `contents`. */
static void test_resources_read_handler_error(void) {
    transport_pair_t p;
    TEST_ASSERT(make_pair(&p) == 0);

    cmcp_server_t *srv = cmcp_server_new("rp-srv", "0.1.0");
    cmcp_server_add_resource(srv, &(cmcp_resource_t){
        .uri = "test://broken", .name = "Broken", .read = unhealthy_read,
    });

    server_arg_t sa = { srv, p.server_t, 0 };
    pthread_t th;
    pthread_create(&th, NULL, server_thread, &sa);

    cmcp_client_t *cli = cmcp_client_new("rp-cli", "0.0.1");
    TEST_ASSERT(cmcp_client_handshake(cli, p.client_t) == CMCP_OK);

    cmcp_json_t *params = cmcp_json_new_object();
    cmcp_json_object_set(params, "uri", cmcp_json_new_string("test://broken"));
    cmcp_rpc_message_t resp;
    TEST_ASSERT(cmcp_client_request(cli, "resources/read", params, &resp)
                == CMCP_OK);
    TEST_ASSERT(resp.error == NULL);

    const cmcp_json_t *iserr = cmcp_json_object_get(resp.result, "isError");
    TEST_ASSERT(iserr && iserr->type == CMCP_JSON_BOOL && iserr->b == 1);
    const cmcp_json_t *contents = cmcp_json_object_get(resp.result, "contents");
    TEST_ASSERT(contents && contents->type == CMCP_JSON_ARRAY);

    cmcp_rpc_message_clear(&resp);
    cmcp_client_free(cli);
    cmcp_transport_close(p.client_t);
    pthread_join(th, NULL);
    cmcp_server_free(srv);
    cmcp_transport_close(p.server_t);
}

/* ====================================================================== */
/* resources/subscribe + unsubscribe                                       */
/* ====================================================================== */

static void test_resources_subscribe_unsubscribe(void) {
    transport_pair_t p;
    TEST_ASSERT(make_pair(&p) == 0);

    cmcp_server_t *srv = cmcp_server_new("rp-srv", "0.1.0");
    cmcp_server_add_resource(srv, &(cmcp_resource_t){
        .uri = "test://stats", .name = "Stats", .read = stats_read,
    });

    server_arg_t sa = { srv, p.server_t, 0 };
    pthread_t th;
    pthread_create(&th, NULL, server_thread, &sa);

    cmcp_client_t *cli = cmcp_client_new("rp-cli", "0.0.1");
    TEST_ASSERT(cmcp_client_handshake(cli, p.client_t) == CMCP_OK);

    /* subscribe → empty result. */
    cmcp_json_t *params = cmcp_json_new_object();
    cmcp_json_object_set(params, "uri", cmcp_json_new_string("test://stats"));
    cmcp_rpc_message_t resp;
    TEST_ASSERT(cmcp_client_request(cli, "resources/subscribe", params, &resp)
                == CMCP_OK);
    TEST_ASSERT(resp.error == NULL);
    TEST_ASSERT(resp.result && resp.result->type == CMCP_JSON_OBJECT);
    cmcp_rpc_message_clear(&resp);

    /* Subscribe again is idempotent — also returns success. */
    params = cmcp_json_new_object();
    cmcp_json_object_set(params, "uri", cmcp_json_new_string("test://stats"));
    TEST_ASSERT(cmcp_client_request(cli, "resources/subscribe", params, &resp)
                == CMCP_OK);
    TEST_ASSERT(resp.error == NULL);
    cmcp_rpc_message_clear(&resp);

    /* unsubscribe. */
    params = cmcp_json_new_object();
    cmcp_json_object_set(params, "uri", cmcp_json_new_string("test://stats"));
    TEST_ASSERT(cmcp_client_request(cli, "resources/unsubscribe", params, &resp)
                == CMCP_OK);
    TEST_ASSERT(resp.error == NULL);
    cmcp_rpc_message_clear(&resp);

    /* Unsubscribe an unknown URI is also a no-op success. */
    params = cmcp_json_new_object();
    cmcp_json_object_set(params, "uri", cmcp_json_new_string("test://ghost"));
    TEST_ASSERT(cmcp_client_request(cli, "resources/unsubscribe", params, &resp)
                == CMCP_OK);
    TEST_ASSERT(resp.error == NULL);
    cmcp_rpc_message_clear(&resp);

    /* Missing uri → -32602. */
    TEST_ASSERT(cmcp_client_request(cli, "resources/subscribe",
                                     cmcp_json_new_object(), &resp)
                == CMCP_OK);
    TEST_ASSERT(resp.error != NULL);
    TEST_ASSERT(resp.error && resp.error->code == CMCP_RPC_INVALID_PARAMS);
    cmcp_rpc_message_clear(&resp);

    cmcp_client_free(cli);
    cmcp_transport_close(p.client_t);
    pthread_join(th, NULL);
    cmcp_server_free(srv);
    cmcp_transport_close(p.server_t);
}

/* ====================================================================== */
/* prompts/list + prompts/get                                              */
/* ====================================================================== */

static void test_prompts_list_over_wire(void) {
    transport_pair_t p;
    TEST_ASSERT(make_pair(&p) == 0);

    cmcp_server_t *srv = cmcp_server_new("rp-srv", "0.1.0");
    cmcp_server_add_prompt(srv, &(cmcp_prompt_t){
        .name        = "code_review",
        .description = "Generate a code review prompt.",
        .arguments   = "[{\"name\":\"language\",\"description\":\"language tag\","
                        "\"required\":true}]",
        .handler     = code_review_handler,
    });

    server_arg_t sa = { srv, p.server_t, 0 };
    pthread_t th;
    pthread_create(&th, NULL, server_thread, &sa);

    cmcp_client_t *cli = cmcp_client_new("rp-cli", "0.0.1");
    TEST_ASSERT(cmcp_client_handshake(cli, p.client_t) == CMCP_OK);

    cmcp_rpc_message_t resp;
    TEST_ASSERT(cmcp_client_request(cli, "prompts/list", NULL, &resp)
                == CMCP_OK);
    TEST_ASSERT(resp.error == NULL);
    const cmcp_json_t *arr = cmcp_json_object_get(resp.result, "prompts");
    TEST_ASSERT(arr && arr->type == CMCP_JSON_ARRAY);
    TEST_ASSERT(cmcp_json_array_len(arr) == 1);

    const cmcp_json_t *p0   = cmcp_json_array_at(arr, 0);
    const cmcp_json_t *name = cmcp_json_object_get(p0, "name");
    const cmcp_json_t *desc = cmcp_json_object_get(p0, "description");
    const cmcp_json_t *args = cmcp_json_object_get(p0, "arguments");
    TEST_ASSERT(name && strcmp(name->str.s, "code_review")              == 0);
    TEST_ASSERT(desc && strcmp(desc->str.s, "Generate a code review prompt.") == 0);
    TEST_ASSERT(args && args->type == CMCP_JSON_ARRAY);
    TEST_ASSERT(cmcp_json_array_len(args) == 1);
    const cmcp_json_t *a0 = cmcp_json_array_at(args, 0);
    const cmcp_json_t *an = cmcp_json_object_get(a0, "name");
    const cmcp_json_t *ar = cmcp_json_object_get(a0, "required");
    TEST_ASSERT(an && strcmp(an->str.s, "language") == 0);
    TEST_ASSERT(ar && ar->type == CMCP_JSON_BOOL && ar->b == 1);

    cmcp_rpc_message_clear(&resp);
    cmcp_client_free(cli);
    cmcp_transport_close(p.client_t);
    pthread_join(th, NULL);
    cmcp_server_free(srv);
    cmcp_transport_close(p.server_t);
}

static void test_prompts_get_success(void) {
    transport_pair_t p;
    TEST_ASSERT(make_pair(&p) == 0);

    cmcp_server_t *srv = cmcp_server_new("rp-srv", "0.1.0");
    cmcp_server_add_prompt(srv, &(cmcp_prompt_t){
        .name        = "code_review",
        .description = "Generate a code review prompt.",
        .arguments   = "[{\"name\":\"language\",\"required\":true}]",
        .handler     = code_review_handler,
    });

    server_arg_t sa = { srv, p.server_t, 0 };
    pthread_t th;
    pthread_create(&th, NULL, server_thread, &sa);

    cmcp_client_t *cli = cmcp_client_new("rp-cli", "0.0.1");
    TEST_ASSERT(cmcp_client_handshake(cli, p.client_t) == CMCP_OK);

    cmcp_json_t *params = cmcp_json_new_object();
    cmcp_json_object_set(params, "name", cmcp_json_new_string("code_review"));
    cmcp_json_t *args = cmcp_json_new_object();
    cmcp_json_object_set(args, "language", cmcp_json_new_string("python"));
    cmcp_json_object_set(params, "arguments", args);

    cmcp_rpc_message_t resp;
    TEST_ASSERT(cmcp_client_request(cli, "prompts/get", params, &resp)
                == CMCP_OK);
    TEST_ASSERT(resp.error == NULL);

    const cmcp_json_t *desc = cmcp_json_object_get(resp.result, "description");
    TEST_ASSERT(desc && strcmp(desc->str.s, "Generate a code review prompt.") == 0);

    const cmcp_json_t *msgs = cmcp_json_object_get(resp.result, "messages");
    TEST_ASSERT(msgs && msgs->type == CMCP_JSON_ARRAY);
    TEST_ASSERT(cmcp_json_array_len(msgs) == 1);
    const cmcp_json_t *m0 = cmcp_json_array_at(msgs, 0);
    const cmcp_json_t *role = cmcp_json_object_get(m0, "role");
    const cmcp_json_t *content = cmcp_json_object_get(m0, "content");
    TEST_ASSERT(role && strcmp(role->str.s, "user") == 0);
    TEST_ASSERT(content && content->type == CMCP_JSON_OBJECT);
    const cmcp_json_t *typ = cmcp_json_object_get(content, "type");
    const cmcp_json_t *txt = cmcp_json_object_get(content, "text");
    TEST_ASSERT(typ && strcmp(typ->str.s, "text") == 0);
    TEST_ASSERT(txt && strcmp(txt->str.s, "Review the following python code.") == 0);

    cmcp_rpc_message_clear(&resp);
    cmcp_client_free(cli);
    cmcp_transport_close(p.client_t);
    pthread_join(th, NULL);
    cmcp_server_free(srv);
    cmcp_transport_close(p.server_t);
}

static void test_prompts_get_missing_required(void) {
    transport_pair_t p;
    TEST_ASSERT(make_pair(&p) == 0);

    cmcp_server_t *srv = cmcp_server_new("rp-srv", "0.1.0");
    cmcp_server_add_prompt(srv, &(cmcp_prompt_t){
        .name      = "code_review",
        .arguments = "[{\"name\":\"language\",\"required\":true}]",
        .handler   = code_review_handler,
    });

    server_arg_t sa = { srv, p.server_t, 0 };
    pthread_t th;
    pthread_create(&th, NULL, server_thread, &sa);

    cmcp_client_t *cli = cmcp_client_new("rp-cli", "0.0.1");
    TEST_ASSERT(cmcp_client_handshake(cli, p.client_t) == CMCP_OK);

    /* No arguments at all. */
    cmcp_json_t *params = cmcp_json_new_object();
    cmcp_json_object_set(params, "name", cmcp_json_new_string("code_review"));
    cmcp_rpc_message_t resp;
    TEST_ASSERT(cmcp_client_request(cli, "prompts/get", params, &resp)
                == CMCP_OK);
    TEST_ASSERT(resp.error != NULL);
    TEST_ASSERT(resp.error && resp.error->code == CMCP_RPC_INVALID_PARAMS);
    /* `data.name` echoes the missing arg name. */
    TEST_ASSERT(resp.error && resp.error->data);
    const cmcp_json_t *miss = cmcp_json_object_get(resp.error->data, "name");
    TEST_ASSERT(miss && strcmp(miss->str.s, "language") == 0);
    cmcp_rpc_message_clear(&resp);

    cmcp_client_free(cli);
    cmcp_transport_close(p.client_t);
    pthread_join(th, NULL);
    cmcp_server_free(srv);
    cmcp_transport_close(p.server_t);
}

static void test_prompts_get_unknown(void) {
    transport_pair_t p;
    TEST_ASSERT(make_pair(&p) == 0);

    cmcp_server_t *srv = cmcp_server_new("rp-srv", "0.1.0");
    cmcp_server_add_prompt(srv, &(cmcp_prompt_t){
        .name = "noop", .handler = empty_prompt_handler,
    });

    server_arg_t sa = { srv, p.server_t, 0 };
    pthread_t th;
    pthread_create(&th, NULL, server_thread, &sa);

    cmcp_client_t *cli = cmcp_client_new("rp-cli", "0.0.1");
    TEST_ASSERT(cmcp_client_handshake(cli, p.client_t) == CMCP_OK);

    cmcp_json_t *params = cmcp_json_new_object();
    cmcp_json_object_set(params, "name", cmcp_json_new_string("ghost"));
    cmcp_rpc_message_t resp;
    TEST_ASSERT(cmcp_client_request(cli, "prompts/get", params, &resp)
                == CMCP_OK);
    TEST_ASSERT(resp.error != NULL);
    TEST_ASSERT(resp.error && resp.error->code == CMCP_RPC_INVALID_PARAMS);
    cmcp_rpc_message_clear(&resp);

    cmcp_client_free(cli);
    cmcp_transport_close(p.client_t);
    pthread_join(th, NULL);
    cmcp_server_free(srv);
    cmcp_transport_close(p.server_t);
}

/* ====================================================================== */
/* Capability auto-advertise                                               */
/* ====================================================================== */

static void test_capabilities_auto_advertise(void) {
    /* When a server has resources or prompts registered, the
     * initialize result must advertise those capabilities so a
     * client can decide whether to even bother listing. */
    transport_pair_t p;
    TEST_ASSERT(make_pair(&p) == 0);

    cmcp_server_t *srv = cmcp_server_new("rp-srv", "0.1.0");
    cmcp_server_add_resource(srv, &(cmcp_resource_t){
        .uri = "test://x", .name = "X", .read = stats_read,
    });
    cmcp_server_add_prompt(srv, &(cmcp_prompt_t){
        .name = "p", .handler = empty_prompt_handler,
    });

    server_arg_t sa = { srv, p.server_t, 0 };
    pthread_t th;
    pthread_create(&th, NULL, server_thread, &sa);

    cmcp_client_t *cli = cmcp_client_new("rp-cli", "0.0.1");
    TEST_ASSERT(cmcp_client_handshake(cli, p.client_t) == CMCP_OK);

    /* The server-side capability struct is filled by the user's
     * cmcp_server_set_capabilities() call only — the *wire* signal of
     * "I have resources" is the presence of the `resources` key in
     * the initialize result, which the client doesn't currently
     * surface as a struct field. So instead, verify the wire shape by
     * driving resources/list and prompts/list and seeing them succeed
     * without -32601. */
    cmcp_rpc_message_t r1, r2;
    TEST_ASSERT(cmcp_client_request(cli, "resources/list", NULL, &r1) == CMCP_OK);
    TEST_ASSERT(r1.error == NULL);
    cmcp_rpc_message_clear(&r1);

    TEST_ASSERT(cmcp_client_request(cli, "prompts/list", NULL, &r2) == CMCP_OK);
    TEST_ASSERT(r2.error == NULL);
    cmcp_rpc_message_clear(&r2);

    cmcp_client_free(cli);
    cmcp_transport_close(p.client_t);
    pthread_join(th, NULL);
    cmcp_server_free(srv);
    cmcp_transport_close(p.server_t);
}

/* ====================================================================== */
/* Session aggregation                                                     */
/* ====================================================================== */

static int register_named_resource(cmcp_server_t *s, const char *uri,
                                    const char *name) {
    return cmcp_server_add_resource(s, &(cmcp_resource_t){
        .uri = uri, .name = name, .read = stats_read,
        .mime_type = "text/plain",
    });
}

static int register_named_prompt(cmcp_server_t *s, const char *name) {
    return cmcp_server_add_prompt(s, &(cmcp_prompt_t){
        .name = name, .handler = empty_prompt_handler,
    });
}

static void test_session_resources_aggregation(void) {
    transport_pair_t pa, pb;
    TEST_ASSERT(make_pair(&pa) == 0);
    TEST_ASSERT(make_pair(&pb) == 0);

    cmcp_server_t *srv_a = cmcp_server_new("srv-a", "0.1.0");
    cmcp_server_t *srv_b = cmcp_server_new("srv-b", "0.1.0");
    TEST_ASSERT(register_named_resource(srv_a, "a://stats", "AStats") == CMCP_OK);
    TEST_ASSERT(register_named_resource(srv_b, "b://stats", "BStats") == CMCP_OK);

    server_arg_t sa = { srv_a, pa.server_t, 0 };
    server_arg_t sb = { srv_b, pb.server_t, 0 };
    pthread_t th_a, th_b;
    pthread_create(&th_a, NULL, server_thread, &sa);
    pthread_create(&th_b, NULL, server_thread, &sb);

    cmcp_client_t *cli_a = cmcp_client_new("ses", "0.0.1");
    cmcp_client_t *cli_b = cmcp_client_new("ses", "0.0.1");
    TEST_ASSERT(cmcp_client_handshake(cli_a, pa.client_t) == CMCP_OK);
    TEST_ASSERT(cmcp_client_handshake(cli_b, pb.client_t) == CMCP_OK);

    cmcp_session_t *ses = cmcp_session_new();
    TEST_ASSERT(cmcp_session_add(ses, "alpha", cli_a) == CMCP_OK);
    TEST_ASSERT(cmcp_session_add(ses, "beta",  cli_b) == CMCP_OK);

    /* Aggregated resources/list. */
    cmcp_session_resource_t *res = NULL;
    size_t n = 0;
    TEST_ASSERT(cmcp_session_resources_list(ses, &res, &n) == CMCP_OK);
    TEST_ASSERT(n == 2);

    int saw_a = 0, saw_b = 0;
    for (size_t i = 0; i < n; i++) {
        if (strcmp(res[i].server, "alpha") == 0 &&
            strcmp(res[i].uri, "a://stats") == 0) {
            TEST_ASSERT(strcmp(res[i].name, "AStats") == 0);
            TEST_ASSERT(res[i].mime_type &&
                        strcmp(res[i].mime_type, "text/plain") == 0);
            saw_a = 1;
        } else if (strcmp(res[i].server, "beta") == 0 &&
                   strcmp(res[i].uri, "b://stats") == 0) {
            TEST_ASSERT(strcmp(res[i].name, "BStats") == 0);
            saw_b = 1;
        }
    }
    TEST_ASSERT(saw_a && saw_b);
    cmcp_session_resources_free(res, n);

    /* Routed read on alpha. */
    cmcp_rpc_message_t resp;
    TEST_ASSERT(cmcp_session_resource_read(ses, "alpha", "a://stats", &resp)
                == CMCP_OK);
    TEST_ASSERT(resp.error == NULL);
    const cmcp_json_t *contents = cmcp_json_object_get(resp.result, "contents");
    TEST_ASSERT(contents && contents->type == CMCP_JSON_ARRAY);
    TEST_ASSERT(cmcp_json_array_len(contents) == 1);
    cmcp_rpc_message_clear(&resp);

    /* Routed read on missing server → ENOTFOUND. */
    TEST_ASSERT(cmcp_session_resource_read(ses, "ghost", "a://stats", &resp)
                == CMCP_ENOTFOUND);

    cmcp_session_free(ses);
    cmcp_transport_close(pa.client_t);
    cmcp_transport_close(pb.client_t);
    pthread_join(th_a, NULL);
    pthread_join(th_b, NULL);
    cmcp_server_free(srv_a);
    cmcp_server_free(srv_b);
    cmcp_transport_close(pa.server_t);
    cmcp_transport_close(pb.server_t);
}

static void test_session_prompts_aggregation(void) {
    transport_pair_t pa, pb;
    TEST_ASSERT(make_pair(&pa) == 0);
    TEST_ASSERT(make_pair(&pb) == 0);

    cmcp_server_t *srv_a = cmcp_server_new("srv-a", "0.1.0");
    cmcp_server_t *srv_b = cmcp_server_new("srv-b", "0.1.0");
    TEST_ASSERT(register_named_prompt(srv_a, "p_a") == CMCP_OK);
    TEST_ASSERT(register_named_prompt(srv_b, "p_b") == CMCP_OK);

    server_arg_t sa = { srv_a, pa.server_t, 0 };
    server_arg_t sb = { srv_b, pb.server_t, 0 };
    pthread_t th_a, th_b;
    pthread_create(&th_a, NULL, server_thread, &sa);
    pthread_create(&th_b, NULL, server_thread, &sb);

    cmcp_client_t *cli_a = cmcp_client_new("ses", "0.0.1");
    cmcp_client_t *cli_b = cmcp_client_new("ses", "0.0.1");
    TEST_ASSERT(cmcp_client_handshake(cli_a, pa.client_t) == CMCP_OK);
    TEST_ASSERT(cmcp_client_handshake(cli_b, pb.client_t) == CMCP_OK);

    cmcp_session_t *ses = cmcp_session_new();
    TEST_ASSERT(cmcp_session_add(ses, "alpha", cli_a) == CMCP_OK);
    TEST_ASSERT(cmcp_session_add(ses, "beta",  cli_b) == CMCP_OK);

    cmcp_session_prompt_t *pr = NULL;
    size_t n = 0;
    TEST_ASSERT(cmcp_session_prompts_list(ses, &pr, &n) == CMCP_OK);
    TEST_ASSERT(n == 2);

    int saw_a = 0, saw_b = 0;
    for (size_t i = 0; i < n; i++) {
        if (strcmp(pr[i].server, "alpha") == 0 &&
            strcmp(pr[i].name, "p_a") == 0) saw_a = 1;
        else if (strcmp(pr[i].server, "beta") == 0 &&
                 strcmp(pr[i].name, "p_b") == 0) saw_b = 1;
    }
    TEST_ASSERT(saw_a && saw_b);
    cmcp_session_prompts_free(pr, n);

    /* Routed get on alpha. */
    cmcp_rpc_message_t resp;
    TEST_ASSERT(cmcp_session_prompt_get(ses, "alpha", "p_a", NULL, &resp)
                == CMCP_OK);
    TEST_ASSERT(resp.error == NULL);
    const cmcp_json_t *msgs = cmcp_json_object_get(resp.result, "messages");
    TEST_ASSERT(msgs && msgs->type == CMCP_JSON_ARRAY);
    cmcp_rpc_message_clear(&resp);

    /* Routed get on missing server → ENOTFOUND. */
    TEST_ASSERT(cmcp_session_prompt_get(ses, "ghost", "p_a", NULL, &resp)
                == CMCP_ENOTFOUND);

    cmcp_session_free(ses);
    cmcp_transport_close(pa.client_t);
    cmcp_transport_close(pb.client_t);
    pthread_join(th_a, NULL);
    pthread_join(th_b, NULL);
    cmcp_server_free(srv_a);
    cmcp_server_free(srv_b);
    cmcp_transport_close(pa.server_t);
    cmcp_transport_close(pb.server_t);
}

/* ====================================================================== */

int main(void) {
    fprintf(stderr, "test_resources_prompts:\n");

    TEST_RUN(test_register_resource);
    TEST_RUN(test_register_prompt);
    TEST_RUN(test_resources_list_over_wire);
    TEST_RUN(test_resources_read_success);
    TEST_RUN(test_resources_read_unknown);
    TEST_RUN(test_resources_read_handler_error);
    TEST_RUN(test_resources_subscribe_unsubscribe);
    TEST_RUN(test_prompts_list_over_wire);
    TEST_RUN(test_prompts_get_success);
    TEST_RUN(test_prompts_get_missing_required);
    TEST_RUN(test_prompts_get_unknown);
    TEST_RUN(test_capabilities_auto_advertise);
    TEST_RUN(test_session_resources_aggregation);
    TEST_RUN(test_session_prompts_aggregation);

    TEST_DONE();
}
