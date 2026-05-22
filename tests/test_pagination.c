/* Phase 4.2 — client-side list pagination.
 *
 * A server may split tools / resources / prompts across pages, ending
 * each non-final page with an opaque `nextCursor`. A client that does
 * not follow the cursor silently sees only page one. This test stands
 * up a mini-server that paginates all three list methods and asserts
 * that cmcp_session_*_list collects every page.
 *
 *   tools/list      3 pages → 5 tools
 *   resources/list  2 pages → 3 resources
 *   prompts/list    2 pages → 2 prompts
 */

#define _POSIX_C_SOURCE 200809L

#include "test.h"
#include "cmcp.h"
#include "cmcp_client.h"
#include "cmcp_session.h"
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
/* Paginating mini-server                                                  */
/* ====================================================================== */

static cmcp_json_t *tool_item(const char *name) {
    cmcp_json_t *o = cmcp_json_new_object();
    cmcp_json_object_set(o, "name", cmcp_json_new_string(name));
    cmcp_json_object_set(o, "description", cmcp_json_new_string("a tool"));
    cmcp_json_t *sch = cmcp_json_new_object();
    cmcp_json_object_set(sch, "type", cmcp_json_new_string("object"));
    cmcp_json_object_set(o, "inputSchema", sch);
    return o;
}

static cmcp_json_t *resource_item(const char *name) {
    cmcp_json_t *o = cmcp_json_new_object();
    char uri[64];
    snprintf(uri, sizeof uri, "res://%s", name);
    cmcp_json_object_set(o, "uri",  cmcp_json_new_string(uri));
    cmcp_json_object_set(o, "name", cmcp_json_new_string(name));
    return o;
}

static cmcp_json_t *prompt_item(const char *name) {
    cmcp_json_t *o = cmcp_json_new_object();
    cmcp_json_object_set(o, "name", cmcp_json_new_string(name));
    return o;
}

/* {<arr_key>: [items...], "nextCursor": <next>?}. Items are consumed. */
static cmcp_json_t *make_page(const char *arr_key, cmcp_json_t **items,
                               size_t n_items, const char *next_cursor) {
    cmcp_json_t *result = cmcp_json_new_object();
    cmcp_json_t *arr = cmcp_json_new_array();
    for (size_t i = 0; i < n_items; i++)
        cmcp_json_array_append(arr, items[i]);
    cmcp_json_object_set(result, arr_key, arr);
    if (next_cursor)
        cmcp_json_object_set(result, "nextCursor",
                              cmcp_json_new_string(next_cursor));
    return result;
}

/* The page a given (method, cursor) pair should return. cursor==NULL is
 * the first page. */
static cmcp_json_t *list_page(const char *method, const char *cursor) {
    if (strcmp(method, "tools/list") == 0) {
        if (!cursor) {
            cmcp_json_t *it[] = { tool_item("t1"), tool_item("t2") };
            return make_page("tools", it, 2, "tc1");
        }
        if (strcmp(cursor, "tc1") == 0) {
            cmcp_json_t *it[] = { tool_item("t3"), tool_item("t4") };
            return make_page("tools", it, 2, "tc2");
        }
        if (strcmp(cursor, "tc2") == 0) {
            cmcp_json_t *it[] = { tool_item("t5") };
            return make_page("tools", it, 1, NULL);
        }
    } else if (strcmp(method, "resources/list") == 0) {
        if (!cursor) {
            cmcp_json_t *it[] = { resource_item("r1"), resource_item("r2") };
            return make_page("resources", it, 2, "rc1");
        }
        if (strcmp(cursor, "rc1") == 0) {
            cmcp_json_t *it[] = { resource_item("r3") };
            return make_page("resources", it, 1, NULL);
        }
    } else if (strcmp(method, "prompts/list") == 0) {
        if (!cursor) {
            cmcp_json_t *it[] = { prompt_item("p1") };
            return make_page("prompts", it, 1, "pc1");
        }
        if (strcmp(cursor, "pc1") == 0) {
            cmcp_json_t *it[] = { prompt_item("p2") };
            return make_page("prompts", it, 1, NULL);
        }
    }
    return cmcp_json_new_object();   /* unknown method/cursor → empty */
}

/* Handshake, then serve paginated list requests until the client
 * closes the transport. */
static void *paged_server(void *arg) {
    cmcp_transport_t *t = (cmcp_transport_t *)arg;
    char *frame = NULL; size_t flen = 0;

    while (cmcp_transport_read(t, &frame, &flen) == CMCP_OK) {
        cmcp_rpc_message_t *msgs = NULL; size_t n = 0;
        int prc = cmcp_rpc_parse(frame, flen, &msgs, &n);
        free(frame); frame = NULL;
        if (prc != CMCP_OK || n != 1) {
            cmcp_rpc_messages_free(msgs, n);
            continue;
        }
        cmcp_rpc_message_t *m = &msgs[0];
        if (m->kind != CMCP_MSG_REQUEST) {     /* skip notifications */
            cmcp_rpc_messages_free(msgs, n);
            continue;
        }

        cmcp_json_t *result;
        if (strcmp(m->method, "initialize") == 0) {
            result = cmcp_json_new_object();
            cmcp_json_object_set(result, "protocolVersion",
                                  cmcp_json_new_string(CMCP_PROTOCOL_VERSION));
            cmcp_json_object_set(result, "capabilities",
                                  cmcp_json_new_object());
            cmcp_json_t *si = cmcp_json_new_object();
            cmcp_json_object_set(si, "name",
                                  cmcp_json_new_string("paged-srv"));
            cmcp_json_object_set(si, "version",
                                  cmcp_json_new_string("0.1.0"));
            cmcp_json_object_set(result, "serverInfo", si);
        } else {
            const cmcp_json_t *cj = m->params
                ? cmcp_json_object_get(m->params, "cursor") : NULL;
            const char *cursor = (cj && cj->type == CMCP_JSON_STRING)
                                    ? cj->str.s : NULL;
            result = list_page(m->method, cursor);
        }

        cmcp_rpc_message_t resp;
        cmcp_rpc_message_init(&resp);
        cmcp_rpc_make_response(&resp, &m->id, result);
        char *wire = cmcp_rpc_emit(&resp);
        cmcp_transport_write(t, wire, strlen(wire));
        free(wire);
        cmcp_rpc_message_clear(&resp);
        cmcp_rpc_messages_free(msgs, n);
    }
    return NULL;
}

/* ====================================================================== */
/* Test                                                                    */
/* ====================================================================== */

static int has_tool(const cmcp_session_tool_t *tools, size_t n,
                      const char *name) {
    for (size_t i = 0; i < n; i++)
        if (strcmp(tools[i].name, name) == 0) return 1;
    return 0;
}

static void test_session_follows_pagination(void) {
    transport_pair_t p = {0};
    TEST_ASSERT(make_pair(&p) == 0);

    pthread_t th;
    TEST_ASSERT(pthread_create(&th, NULL, paged_server, p.server_t) == 0);

    cmcp_client_t *cli = cmcp_client_new("host", "0.0.1");
    TEST_ASSERT(cmcp_client_handshake(cli, p.client_t) == CMCP_OK);

    cmcp_session_t *sess = cmcp_session_new();
    TEST_ASSERT(sess != NULL);
    TEST_ASSERT(cmcp_session_add(sess, "paged", cli) == CMCP_OK);

    /* tools/list — 3 pages, 5 tools total. */
    cmcp_session_tool_t *tools = NULL; size_t nt = 0;
    TEST_ASSERT(cmcp_session_tools_list(sess, &tools, &nt) == CMCP_OK);
    TEST_ASSERT(nt == 5);
    /* A first-page AND a last-page entry both present — proves the loop
     * neither stopped at page one nor skipped straight to the end. */
    TEST_ASSERT(has_tool(tools, nt, "t1"));
    TEST_ASSERT(has_tool(tools, nt, "t5"));
    cmcp_session_tools_free(tools, nt);

    /* resources/list — 2 pages, 3 resources total. */
    cmcp_session_resource_t *res = NULL; size_t nr = 0;
    TEST_ASSERT(cmcp_session_resources_list(sess, &res, &nr) == CMCP_OK);
    TEST_ASSERT(nr == 3);
    cmcp_session_resources_free(res, nr);

    /* prompts/list — 2 pages, 2 prompts total. */
    cmcp_session_prompt_t *pr = NULL; size_t np = 0;
    TEST_ASSERT(cmcp_session_prompts_list(sess, &pr, &np) == CMCP_OK);
    TEST_ASSERT(np == 2);
    cmcp_session_prompts_free(pr, np);

    cmcp_session_free(sess);          /* frees the added client */
    cmcp_transport_close(p.client_t);
    pthread_join(th, NULL);
    cmcp_transport_close(p.server_t);
}

int main(void) {
    fprintf(stderr, "test_pagination:\n");
    TEST_RUN(test_session_follows_pagination);
    TEST_DONE();
}
