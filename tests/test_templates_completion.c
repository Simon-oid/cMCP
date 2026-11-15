/* resources/templates/list, template-routed resources/read, and
 * completion/complete, end to end over a pipe pair. */

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

typedef struct {
    cmcp_transport_t *client_t, *server_t;
} pair_t;
typedef struct {
    cmcp_server_t *s;
    cmcp_transport_t *t;
} srv_arg_t;

static int make_pair(pair_t *out) {
    int c2s[2], s2c[2];
    if (pipe(c2s) != 0)
        return -1;
    if (pipe(s2c) != 0) {
        close(c2s[0]);
        close(c2s[1]);
        return -1;
    }
    out->client_t = cmcp_transport_stdio_new_fds(s2c[0], c2s[1]);
    out->server_t = cmcp_transport_stdio_new_fds(c2s[0], s2c[1]);
    return out->client_t && out->server_t ? 0 : -1;
}

static void *serve(void *arg) {
    srv_arg_t *a = arg;
    cmcp_server_run(a->s, a->t);
    return NULL;
}

/* Echoes the URI it was asked for, so a test can see which handler ran. */
static int echo_read(const char *uri, void *userdata, cmcp_handler_ctx_t *hctx,
                     cmcp_json_t **out, int *is_error) {
    (void)hctx;
    char text[256];
    snprintf(text, sizeof text, "%s:%s", (const char *)userdata, uri);
    *out = cmcp_resource_text_contents(uri, "text/plain", text);
    *is_error = 0;
    return CMCP_OK;
}

static int prompt_h(const cmcp_json_t *args, void *u, cmcp_handler_ctx_t *h,
                    cmcp_json_t **out) {
    (void)args;
    (void)u;
    (void)h;
    *out = cmcp_prompt_text_messages("user", "hi");
    return CMCP_OK;
}

static const char *LANGS[] = {"python", "perl", "php", "pascal", "c"};

/* Completes from a fixed list by prefix; for "many", 150 values. */
static int complete(const char *ref_type, const char *ref_id, const char *arg,
                    const char *value, const cmcp_json_t *context,
                    void *userdata, cmcp_json_t **out_values,
                    int *out_has_more) {
    (void)context;
    (void)userdata;
    (void)ref_id;
    (void)arg;
    cmcp_json_t *v = cmcp_json_new_array();
    if (strcmp(value, "many") == 0) {
        for (int i = 0; i < 150; i++) {
            char b[16];
            snprintf(b, sizeof b, "v%d", i);
            cmcp_json_array_append(v, cmcp_json_new_string(b));
        }
    } else {
        for (size_t i = 0; i < sizeof LANGS / sizeof LANGS[0]; i++)
            if (strncmp(LANGS[i], value, strlen(value)) == 0)
                cmcp_json_array_append(v, cmcp_json_new_string(LANGS[i]));
    }
    if (strcmp(ref_type, "ref/resource") == 0)
        *out_has_more = 1;
    *out_values = v;
    return CMCP_OK;
}

typedef struct {
    pair_t p;
    cmcp_server_t *s;
    cmcp_client_t *c;
    pthread_t th;
    srv_arg_t a;
} rig_t;

static cmcp_server_t *new_server(int with_completion) {
    cmcp_server_t *s = cmcp_server_new("tc-srv", "0.1.0");
    TEST_ASSERT(cmcp_server_add_resource(
                    s, &(cmcp_resource_t){.uri = "db://users/rows/1",
                                          .name = "first",
                                          .read = echo_read,
                                          .userdata = "exact"}) == CMCP_OK);
    TEST_ASSERT(cmcp_server_add_resource_template(
                    s, &(cmcp_resource_template_t){
                           .uri_template = "db://{table}/rows/{id}",
                           .name = "row",
                           .title = "A row",
                           .mime_type = "text/plain",
                           .read = echo_read,
                           .userdata = "row"}) == CMCP_OK);
    TEST_ASSERT(
        cmcp_server_add_resource_template(
            s, &(cmcp_resource_template_t){.uri_template = "docs://{+path}",
                                           .name = "doc",
                                           .read = echo_read,
                                           .userdata = "doc"}) == CMCP_OK);
    TEST_ASSERT(
        cmcp_server_add_prompt(
            s, &(cmcp_prompt_t){.name = "code_review",
                                .arguments = "[{\"name\":\"language\"}]",
                                .handler = prompt_h}) == CMCP_OK);
    if (with_completion)
        TEST_ASSERT(cmcp_server_set_completion(s, complete, NULL) == CMCP_OK);
    return s;
}

static void rig_up(rig_t *r, cmcp_server_t *s) {
    TEST_ASSERT(make_pair(&r->p) == 0);
    r->s = s;
    r->a = (srv_arg_t){s, r->p.server_t};
    pthread_create(&r->th, NULL, serve, &r->a);
    r->c = cmcp_client_new("tc-cli", "0.0.1");
    TEST_ASSERT(cmcp_client_handshake(r->c, r->p.client_t) == CMCP_OK);
}

static void rig_down(rig_t *r) {
    cmcp_client_free(r->c);
    cmcp_transport_close(r->p.client_t);
    pthread_join(r->th, NULL);
    cmcp_server_free(r->s);
    cmcp_transport_close(r->p.server_t);
}

static const char *str_at(const cmcp_json_t *o, const char *key) {
    return cmcp_json_string(cmcp_json_object_get(o, key));
}

static void test_registration_validates(void) {
    cmcp_server_t *s = cmcp_server_new("x", "1");
    cmcp_resource_template_t t = {
        .uri_template = "db://{a}{b}", .name = "n", .read = echo_read};
    TEST_ASSERT(cmcp_server_add_resource_template(s, &t) ==
                CMCP_EINVAL); /* ambiguous */
    t.uri_template = "db://{a}";
    t.read = NULL;
    TEST_ASSERT(cmcp_server_add_resource_template(s, &t) ==
                CMCP_EINVAL); /* no handler */
    t.read = echo_read;
    TEST_ASSERT(cmcp_server_add_resource_template(s, &t) == CMCP_OK);
    TEST_ASSERT(cmcp_server_add_resource_template(s, &t) ==
                CMCP_EPROTOCOL); /* duplicate */
    TEST_ASSERT(cmcp_server_set_completion(s, NULL, NULL) == CMCP_EINVAL);
    cmcp_server_free(s);
}

static void test_templates_list_and_capabilities(void) {
    rig_t r;
    rig_up(&r, new_server(1));
    const cmcp_server_capabilities_t *caps = cmcp_client_server_caps(r.c);
    TEST_ASSERT(caps && caps->completions == 1);
    cmcp_rpc_message_t resp;
    TEST_ASSERT(cmcp_client_request(r.c, "resources/templates/list", NULL,
                                    &resp) == CMCP_OK);
    TEST_ASSERT(resp.error == NULL);
    const cmcp_json_t *arr =
        cmcp_json_object_get(resp.result, "resourceTemplates");
    TEST_ASSERT(arr && cmcp_json_array_len(arr) == 2);
    const cmcp_json_t *t0 = cmcp_json_array_at(arr, 0);
    TEST_ASSERT(str_at(t0, "uriTemplate") &&
                strcmp(str_at(t0, "uriTemplate"), "db://{table}/rows/{id}") ==
                    0);
    TEST_ASSERT(str_at(t0, "title") &&
                strcmp(str_at(t0, "title"), "A row") == 0);
    TEST_ASSERT(str_at(t0, "mimeType") &&
                strcmp(str_at(t0, "mimeType"), "text/plain") == 0);
    TEST_ASSERT(cmcp_json_object_get(t0, "uri") == NULL);
    cmcp_rpc_message_clear(&resp);
    rig_down(&r);
}

static const char *read_text(cmcp_client_t *c, const char *uri, int *code) {
    static char buf[256];
    cmcp_json_t *params = cmcp_json_new_object();
    cmcp_json_object_set(params, "uri", cmcp_json_new_string(uri));
    cmcp_rpc_message_t resp;
    *code = 0;
    buf[0] = '\0';
    if (cmcp_client_request(c, "resources/read", params, &resp) != CMCP_OK)
        return buf;
    if (resp.error) {
        *code = resp.error->code;
    } else {
        const cmcp_json_t *contents =
            cmcp_json_object_get(resp.result, "contents");
        const char *t = str_at(cmcp_json_array_at(contents, 0), "text");
        snprintf(buf, sizeof buf, "%s", t ? t : "");
    }
    cmcp_rpc_message_clear(&resp);
    return buf;
}

static void test_read_routes_through_templates(void) {
    rig_t r;
    rig_up(&r, new_server(0));
    int code;
    TEST_ASSERT(strcmp(read_text(r.c, "db://users/rows/1", &code),
                       "exact:db://users/rows/1") == 0);
    TEST_ASSERT(strcmp(read_text(r.c, "db://users/rows/42", &code),
                       "row:db://users/rows/42") == 0);
    TEST_ASSERT(strcmp(read_text(r.c, "docs://a/b/c.md", &code),
                       "doc:docs://a/b/c.md") == 0);
    read_text(r.c, "db://a/b/rows/42", &code); /* {table} is one segment */
    TEST_ASSERT(code == CMCP_RPC_INVALID_PARAMS);
    rig_down(&r);
}

/* params JSON text -> response; caller clears. */
static cmcp_rpc_message_t complete_call(cmcp_client_t *c,
                                        const char *params_json) {
    cmcp_rpc_message_t resp;
    memset(&resp, 0, sizeof resp);
    cmcp_json_t *params = cmcp_json_parse(params_json, strlen(params_json));
    TEST_ASSERT(cmcp_client_request(c, "completion/complete", params, &resp) ==
                CMCP_OK);
    return resp;
}

static void test_completion_for_prompt_and_template(void) {
    rig_t r;
    rig_up(&r, new_server(1));
    cmcp_rpc_message_t resp = complete_call(
        r.c, "{\"ref\":{\"type\":\"ref/prompt\",\"name\":\"code_review\"},"
             "\"argument\":{\"name\":\"language\",\"value\":\"p\"}}");
    TEST_ASSERT(resp.error == NULL);
    const cmcp_json_t *comp = cmcp_json_object_get(resp.result, "completion");
    const cmcp_json_t *vals = cmcp_json_object_get(comp, "values");
    TEST_ASSERT(cmcp_json_array_len(vals) == 4);
    TEST_ASSERT(
        strcmp(cmcp_json_string(cmcp_json_array_at(vals, 0)), "python") == 0);
    TEST_ASSERT(cmcp_json_int(cmcp_json_object_get(comp, "total")) == 4);
    TEST_ASSERT(cmcp_json_bool(cmcp_json_object_get(comp, "hasMore")) == 0);
    cmcp_rpc_message_clear(&resp);

    resp = complete_call(r.c,
                         "{\"ref\":{\"type\":\"ref/resource\",\"uri\":\"db://"
                         "{table}/rows/{id}\"},"
                         "\"argument\":{\"name\":\"table\",\"value\":\"c\"}}");
    TEST_ASSERT(resp.error == NULL);
    comp = cmcp_json_object_get(resp.result, "completion");
    TEST_ASSERT(cmcp_json_bool(cmcp_json_object_get(comp, "hasMore")) == 1);
    cmcp_rpc_message_clear(&resp);
    rig_down(&r);
}

static void test_completion_caps_values_at_100(void) {
    rig_t r;
    rig_up(&r, new_server(1));
    cmcp_rpc_message_t resp = complete_call(
        r.c, "{\"ref\":{\"type\":\"ref/prompt\",\"name\":\"code_review\"},"
             "\"argument\":{\"name\":\"language\",\"value\":\"many\"}}");
    const cmcp_json_t *comp = cmcp_json_object_get(resp.result, "completion");
    TEST_ASSERT(cmcp_json_array_len(cmcp_json_object_get(comp, "values")) ==
                100);
    TEST_ASSERT(cmcp_json_int(cmcp_json_object_get(comp, "total")) == 150);
    TEST_ASSERT(cmcp_json_bool(cmcp_json_object_get(comp, "hasMore")) == 1);
    cmcp_rpc_message_clear(&resp);
    rig_down(&r);
}

static void test_completion_rejects_bad_requests(void) {
    rig_t r;
    rig_up(&r, new_server(1));
    const char *bad[] = {
        "{\"ref\":{\"type\":\"ref/"
        "prompt\",\"name\":\"nope\"},\"argument\":{\"name\":\"a\",\"value\":"
        "\"\"}}",
        "{\"ref\":{\"type\":\"ref/resource\",\"uri\":\"db://"
        "nope\"},\"argument\":{\"name\":\"a\",\"value\":\"\"}}",
        "{\"ref\":{\"type\":\"ref/"
        "tool\",\"name\":\"x\"},\"argument\":{\"name\":\"a\",\"value\":\"\"}}",
        "{\"ref\":{\"type\":\"ref/prompt\",\"name\":\"code_review\"}}",
        "{\"ref\":{\"type\":\"ref/"
        "prompt\",\"name\":\"code_review\"},\"argument\":{\"name\":\"a\"}}",
    };
    for (size_t i = 0; i < sizeof bad / sizeof bad[0]; i++) {
        cmcp_rpc_message_t resp = complete_call(r.c, bad[i]);
        TEST_ASSERT(resp.error && resp.error->code == CMCP_RPC_INVALID_PARAMS);
        cmcp_rpc_message_clear(&resp);
    }
    rig_down(&r);
}

static void test_completion_without_a_handler_is_method_not_found(void) {
    rig_t r;
    rig_up(&r, new_server(0));
    TEST_ASSERT(cmcp_client_server_caps(r.c)->completions == 0);
    cmcp_rpc_message_t resp = complete_call(
        r.c, "{\"ref\":{\"type\":\"ref/prompt\",\"name\":\"code_review\"},"
             "\"argument\":{\"name\":\"language\",\"value\":\"p\"}}");
    TEST_ASSERT(resp.error && resp.error->code == CMCP_RPC_METHOD_NOT_FOUND);
    cmcp_rpc_message_clear(&resp);
    rig_down(&r);
}

int main(void) {
    fprintf(stderr, "test_templates_completion:\n");
    TEST_RUN(test_registration_validates);
    TEST_RUN(test_templates_list_and_capabilities);
    TEST_RUN(test_read_routes_through_templates);
    TEST_RUN(test_completion_for_prompt_and_template);
    TEST_RUN(test_completion_caps_values_at_100);
    TEST_RUN(test_completion_rejects_bad_requests);
    TEST_RUN(test_completion_without_a_handler_is_method_not_found);
    TEST_DONE();
}
