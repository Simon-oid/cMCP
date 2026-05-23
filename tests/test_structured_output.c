/* End-to-end tests for Phase 4.6 spec breadth:
 *
 *   - structuredContent: a tool may declare an output_schema and attach
 *     a typed result via cmcp_handler_set_structured; the server wraps
 *     it as `structuredContent` in tools/call results and validates
 *     against the schema. Mismatch -> -32603.
 *   - resource_link content items: a tool can point at a resource via
 *     cmcp_tool_resource_link_content instead of inlining text.
 *   - title field: optional human-display name on tools, resources,
 *     prompts - echoed in the various list descriptors when set.
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
/* Scaffolding (mirrors other Phase 4 tests)                                */
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
} server_run_arg_t;

static void *server_run_thread(void *arg) {
    server_run_arg_t *a = (server_run_arg_t *)arg;
    cmcp_server_run(a->s, a->t);
    return NULL;
}

/* ====================================================================== */
/* Tools                                                                   */
/* ====================================================================== */

/* A tool with an output_schema; the handler builds a structuredContent
 * object that satisfies it and attaches via cmcp_handler_set_structured.
 * No content array — the server will synthesise a text rendering. */
static int weather_tool(const cmcp_json_t *args, void *userdata,
                        cmcp_handler_ctx_t *hctx,
                        cmcp_json_t **out_content, int *out_is_error) {
    (void)args; (void)userdata;
    cmcp_json_t *obj = cmcp_json_new_object();
    cmcp_json_object_set(obj, "city", cmcp_json_new_string("Rome"));
    cmcp_json_object_set(obj, "tempC", cmcp_json_new_double(22.5));
    cmcp_handler_set_structured(hctx, obj);
    *out_content  = NULL;     /* let library synthesise from structured */
    *out_is_error = 0;
    return CMCP_OK;
}

/* A tool whose output_schema requires `city` as string, but the handler
 * sets a malformed structured value missing the field. The server must
 * catch that and reply -32603. */
static int bad_weather_tool(const cmcp_json_t *args, void *userdata,
                             cmcp_handler_ctx_t *hctx,
                             cmcp_json_t **out_content, int *out_is_error) {
    (void)args; (void)userdata;
    cmcp_json_t *obj = cmcp_json_new_object();
    /* Missing required `city` field. */
    cmcp_json_object_set(obj, "tempC", cmcp_json_new_double(22.5));
    cmcp_handler_set_structured(hctx, obj);
    *out_content  = NULL;
    *out_is_error = 0;
    return CMCP_OK;
}

/* A tool that emits a content array containing a resource_link item
 * alongside a text item, exercising the spec-compliant content-types
 * mix. */
static int link_tool(const cmcp_json_t *args, void *userdata,
                     cmcp_handler_ctx_t *hctx,
                     cmcp_json_t **out_content, int *out_is_error) {
    (void)args; (void)userdata; (void)hctx;
    cmcp_json_t *arr = cmcp_tool_text_content("see resource:");
    cmcp_json_t *link = cmcp_tool_resource_link_content(
        "file:///report.txt", "report",
        "Quarterly report", "text/plain");
    /* link comes back as a 1-element array — append its single item to
     * arr instead of appending the wrapping array. */
    if (link && link->type == CMCP_JSON_ARRAY && link->arr.len == 1) {
        cmcp_json_t *item = link->arr.items[0];
        link->arr.items[0] = NULL;       /* defuse free of item itself */
        link->arr.len = 0;
        cmcp_json_array_append(arr, item);
    }
    cmcp_json_free(link);
    *out_content  = arr;
    *out_is_error = 0;
    return CMCP_OK;
}

/* Prompt handler returns one user message. Used to verify title echo
 * on the prompts/list descriptor. */
static int boring_prompt(const cmcp_json_t *args, void *userdata,
                          cmcp_handler_ctx_t *hctx, cmcp_json_t **out) {
    (void)args; (void)userdata; (void)hctx;
    *out = cmcp_prompt_text_messages("user", "Hi");
    return CMCP_OK;
}

static int boring_resource(const char *uri, void *userdata,
                            cmcp_handler_ctx_t *hctx,
                            cmcp_json_t **out, int *isErr) {
    (void)userdata; (void)hctx;
    *isErr = 0;
    *out   = cmcp_resource_text_contents(uri, "text/plain", "ok");
    return CMCP_OK;
}

/* ====================================================================== */
/* Tests                                                                   */
/* ====================================================================== */

static const char *WEATHER_SCHEMA =
    "{\"type\":\"object\","
     "\"properties\":{"
       "\"city\":{\"type\":\"string\"},"
       "\"tempC\":{\"type\":\"number\"}},"
     "\"required\":[\"city\",\"tempC\"]}";

static void test_structured_roundtrip(void) {
    transport_pair_t p = {0};
    TEST_ASSERT(make_pair(&p) == 0);

    cmcp_server_t *srv = cmcp_server_new("4.6-srv", "0.1.0");
    TEST_ASSERT(cmcp_server_add_tool(srv, &(cmcp_tool_t){
        .name          = "weather",
        .title         = "Weather lookup",
        .description   = "Returns the current weather for a city.",
        .output_schema = WEATHER_SCHEMA,
        .handler       = weather_tool,
    }) == CMCP_OK);

    server_run_arg_t sa = { srv, p.server_t };
    pthread_t th;
    TEST_ASSERT(pthread_create(&th, NULL, server_run_thread, &sa) == 0);

    cmcp_client_t *cli = cmcp_client_new("host", "0.0.1");
    TEST_ASSERT(cmcp_client_handshake(cli, p.client_t) == CMCP_OK);

    /* tools/list should advertise title + outputSchema. */
    cmcp_rpc_message_t list_resp;
    cmcp_rpc_message_init(&list_resp);
    TEST_ASSERT(cmcp_client_request(cli, "tools/list", NULL, &list_resp) == CMCP_OK);
    if (list_resp.result) {
        const cmcp_json_t *tools = cmcp_json_object_get(list_resp.result, "tools");
        TEST_ASSERT(tools && tools->type == CMCP_JSON_ARRAY &&
                    tools->arr.len == 1);
        if (tools && tools->arr.len == 1) {
            const cmcp_json_t *t0 = tools->arr.items[0];
            const cmcp_json_t *title = cmcp_json_object_get(t0, "title");
            const cmcp_json_t *osch  = cmcp_json_object_get(t0, "outputSchema");
            TEST_ASSERT(title && strcmp(title->str.s, "Weather lookup") == 0);
            TEST_ASSERT(osch && osch->type == CMCP_JSON_OBJECT);
        }
    }
    cmcp_rpc_message_clear(&list_resp);

    /* tools/call weather → expect structuredContent + a synthesised
     * text content fallback. */
    cmcp_json_t *params = cmcp_json_new_object();
    cmcp_json_object_set(params, "name", cmcp_json_new_string("weather"));
    cmcp_rpc_message_t resp;
    cmcp_rpc_message_init(&resp);
    TEST_ASSERT(cmcp_client_request(cli, "tools/call", params, &resp) == CMCP_OK);
    TEST_ASSERT(resp.error == NULL);
    if (resp.result) {
        const cmcp_json_t *sc = cmcp_json_object_get(resp.result, "structuredContent");
        TEST_ASSERT(sc && sc->type == CMCP_JSON_OBJECT);
        if (sc) {
            const cmcp_json_t *city  = cmcp_json_object_get(sc, "city");
            const cmcp_json_t *temp  = cmcp_json_object_get(sc, "tempC");
            TEST_ASSERT(city && strcmp(city->str.s, "Rome") == 0);
            TEST_ASSERT(temp && temp->type == CMCP_JSON_DOUBLE && temp->d == 22.5);
        }
        const cmcp_json_t *content = cmcp_json_object_get(resp.result, "content");
        TEST_ASSERT(content && content->type == CMCP_JSON_ARRAY &&
                    content->arr.len == 1);
        const cmcp_json_t *err = cmcp_json_object_get(resp.result, "isError");
        TEST_ASSERT(err && err->type == CMCP_JSON_BOOL && err->b == 0);
    }
    cmcp_rpc_message_clear(&resp);

    cmcp_client_free(cli);
    cmcp_transport_close(p.client_t);
    pthread_join(th, NULL);
    cmcp_transport_close(p.server_t);
    cmcp_server_free(srv);
}

static void test_structured_schema_violation(void) {
    transport_pair_t p = {0};
    TEST_ASSERT(make_pair(&p) == 0);

    cmcp_server_t *srv = cmcp_server_new("4.6-srv", "0.1.0");
    TEST_ASSERT(cmcp_server_add_tool(srv, &(cmcp_tool_t){
        .name          = "bad_weather",
        .output_schema = WEATHER_SCHEMA,
        .handler       = bad_weather_tool,
    }) == CMCP_OK);

    server_run_arg_t sa = { srv, p.server_t };
    pthread_t th;
    pthread_create(&th, NULL, server_run_thread, &sa);

    cmcp_client_t *cli = cmcp_client_new("host", "0.0.1");
    TEST_ASSERT(cmcp_client_handshake(cli, p.client_t) == CMCP_OK);

    cmcp_json_t *params = cmcp_json_new_object();
    cmcp_json_object_set(params, "name", cmcp_json_new_string("bad_weather"));
    cmcp_rpc_message_t resp;
    cmcp_rpc_message_init(&resp);
    TEST_ASSERT(cmcp_client_request(cli, "tools/call", params, &resp) == CMCP_OK);

    /* Spec: server MUST validate; mismatch is a -32603 internal error. */
    TEST_ASSERT(resp.error != NULL);
    TEST_ASSERT(resp.error && resp.error->code == CMCP_RPC_INTERNAL_ERROR);
    cmcp_rpc_message_clear(&resp);

    cmcp_client_free(cli);
    cmcp_transport_close(p.client_t);
    pthread_join(th, NULL);
    cmcp_transport_close(p.server_t);
    cmcp_server_free(srv);
}

static void test_resource_link_content(void) {
    transport_pair_t p = {0};
    TEST_ASSERT(make_pair(&p) == 0);

    cmcp_server_t *srv = cmcp_server_new("4.6-srv", "0.1.0");
    TEST_ASSERT(cmcp_server_add_tool(srv, &(cmcp_tool_t){
        .name    = "link",
        .handler = link_tool,
    }) == CMCP_OK);

    server_run_arg_t sa = { srv, p.server_t };
    pthread_t th;
    pthread_create(&th, NULL, server_run_thread, &sa);

    cmcp_client_t *cli = cmcp_client_new("host", "0.0.1");
    TEST_ASSERT(cmcp_client_handshake(cli, p.client_t) == CMCP_OK);

    cmcp_json_t *params = cmcp_json_new_object();
    cmcp_json_object_set(params, "name", cmcp_json_new_string("link"));
    cmcp_rpc_message_t resp;
    cmcp_rpc_message_init(&resp);
    TEST_ASSERT(cmcp_client_request(cli, "tools/call", params, &resp) == CMCP_OK);
    TEST_ASSERT(resp.error == NULL);
    if (resp.result) {
        const cmcp_json_t *content = cmcp_json_object_get(resp.result, "content");
        TEST_ASSERT(content && content->type == CMCP_JSON_ARRAY &&
                    content->arr.len == 2);
        if (content && content->arr.len == 2) {
            const cmcp_json_t *txt = content->arr.items[0];
            const cmcp_json_t *lnk = content->arr.items[1];
            TEST_ASSERT(cmcp_json_object_get(txt, "type") &&
                        strcmp(cmcp_json_object_get(txt, "type")->str.s, "text") == 0);
            const cmcp_json_t *type = cmcp_json_object_get(lnk, "type");
            const cmcp_json_t *uri  = cmcp_json_object_get(lnk, "uri");
            const cmcp_json_t *name = cmcp_json_object_get(lnk, "name");
            const cmcp_json_t *desc = cmcp_json_object_get(lnk, "description");
            const cmcp_json_t *mime = cmcp_json_object_get(lnk, "mimeType");
            TEST_ASSERT(type && strcmp(type->str.s, "resource_link") == 0);
            TEST_ASSERT(uri  && strcmp(uri->str.s,  "file:///report.txt") == 0);
            TEST_ASSERT(name && strcmp(name->str.s, "report") == 0);
            TEST_ASSERT(desc && strcmp(desc->str.s, "Quarterly report") == 0);
            TEST_ASSERT(mime && strcmp(mime->str.s, "text/plain") == 0);
        }
    }
    cmcp_rpc_message_clear(&resp);

    cmcp_client_free(cli);
    cmcp_transport_close(p.client_t);
    pthread_join(th, NULL);
    cmcp_transport_close(p.server_t);
    cmcp_server_free(srv);
}

static void test_title_on_resource_and_prompt_lists(void) {
    transport_pair_t p = {0};
    TEST_ASSERT(make_pair(&p) == 0);

    cmcp_server_t *srv = cmcp_server_new("4.6-srv", "0.1.0");
    TEST_ASSERT(cmcp_server_add_resource(srv, &(cmcp_resource_t){
        .uri   = "boring://x",
        .name  = "x",
        .title = "Display X",
        .read  = boring_resource,
    }) == CMCP_OK);
    TEST_ASSERT(cmcp_server_add_prompt(srv, &(cmcp_prompt_t){
        .name    = "greet",
        .title   = "Friendly Greeting",
        .handler = boring_prompt,
    }) == CMCP_OK);

    server_run_arg_t sa = { srv, p.server_t };
    pthread_t th;
    pthread_create(&th, NULL, server_run_thread, &sa);

    cmcp_client_t *cli = cmcp_client_new("host", "0.0.1");
    TEST_ASSERT(cmcp_client_handshake(cli, p.client_t) == CMCP_OK);

    cmcp_rpc_message_t resp;

    cmcp_rpc_message_init(&resp);
    TEST_ASSERT(cmcp_client_request(cli, "resources/list", NULL, &resp) == CMCP_OK);
    if (resp.result) {
        const cmcp_json_t *list = cmcp_json_object_get(resp.result, "resources");
        if (list && list->arr.len == 1) {
            const cmcp_json_t *t = cmcp_json_object_get(list->arr.items[0], "title");
            TEST_ASSERT(t && strcmp(t->str.s, "Display X") == 0);
        } else {
            TEST_ASSERT(0 && "expected one resource");
        }
    }
    cmcp_rpc_message_clear(&resp);

    cmcp_rpc_message_init(&resp);
    TEST_ASSERT(cmcp_client_request(cli, "prompts/list", NULL, &resp) == CMCP_OK);
    if (resp.result) {
        const cmcp_json_t *list = cmcp_json_object_get(resp.result, "prompts");
        if (list && list->arr.len == 1) {
            const cmcp_json_t *t = cmcp_json_object_get(list->arr.items[0], "title");
            TEST_ASSERT(t && strcmp(t->str.s, "Friendly Greeting") == 0);
        } else {
            TEST_ASSERT(0 && "expected one prompt");
        }
    }
    cmcp_rpc_message_clear(&resp);

    cmcp_client_free(cli);
    cmcp_transport_close(p.client_t);
    pthread_join(th, NULL);
    cmcp_transport_close(p.server_t);
    cmcp_server_free(srv);
}

/* ====================================================================== */

int main(void) {
    fprintf(stderr, "test_structured_output:\n");
    TEST_RUN(test_structured_roundtrip);
    TEST_RUN(test_structured_schema_violation);
    TEST_RUN(test_resource_link_content);
    TEST_RUN(test_title_on_resource_and_prompt_lists);
    TEST_DONE();
}
