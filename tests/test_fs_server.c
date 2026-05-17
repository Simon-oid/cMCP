/* test_fs_server — integration test for tools/filesystem-mcp.
 *
 * Spawns the real filesystem-mcp binary as a child (via
 * cmcp_client_connect_stdio), points it at a throwaway sandbox under
 * /tmp, and drives it through the client API. No mocking: this is the
 * server, the stdio transport, and the async client end-to-end.
 *
 * Two things are under test. The obvious one is the four tools'
 * happy paths. The load-bearing one is the sandbox — every escape
 * vector (`..`, symlink, absolute path) must be rejected, because a
 * filesystem server that leaks the root is the whole ballgame.
 *
 * Run from the repo root (the child path is relative): ./tests/test_fs_server
 */

#define _XOPEN_SOURCE 700

#include "cmcp.h"
#include "cmcp_client.h"
#include "cmcp_json.h"
#include "cmcp_types.h"
#include "test.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define FS_BIN "tools/filesystem-mcp/filesystem-mcp"

/* Shared sandbox + connection for the read/write tests. The readonly
 * test stands up its own child with a different environment. */
static char           g_root[256];
static cmcp_client_t *g_cli;

/* ====================================================================== */
/* Fixtures                                                                */
/* ====================================================================== */

static void write_file(const char *rel, const void *data, size_t n) {
    char path[512];
    snprintf(path, sizeof path, "%s/%s", g_root, rel);
    FILE *f = fopen(path, "wb");
    if (!f) { perror("fopen fixture"); exit(2); }
    if (n && fwrite(data, 1, n, f) != n) { perror("fwrite fixture"); exit(2); }
    fclose(f);
}

static void make_fixtures(void) {
    snprintf(g_root, sizeof g_root, "/tmp/fsmcp-test-XXXXXX");
    if (!mkdtemp(g_root)) { perror("mkdtemp"); exit(2); }

    static const char poem[] =
        "line one\nline two\nline three\nline four\nline five\n";
    write_file("poem.txt", poem, sizeof poem - 1);

    write_file("empty.txt", "", 0);

    static const unsigned char bin[] = { 'a','b','c','\0','d','e','f','\n' };
    write_file("binary.bin", bin, sizeof bin);

    char sub[512];
    snprintf(sub, sizeof sub, "%s/sub", g_root);
    if (mkdir(sub, 0755) != 0) { perror("mkdir sub"); exit(2); }
    write_file("sub/nested.txt", "nested\n", 7);

    /* A symlink that points clean out of the sandbox. */
    char link[512];
    snprintf(link, sizeof link, "%s/escape", g_root);
    if (symlink("/etc/passwd", link) != 0) { perror("symlink"); exit(2); }
}

static void remove_fixtures(void) {
    char cmd[512];
    snprintf(cmd, sizeof cmd, "rm -rf '%s'", g_root);
    if (system(cmd) != 0)
        fprintf(stderr, "    warning: failed to remove %s\n", g_root);
}

/* ====================================================================== */
/* Client helpers                                                          */
/* ====================================================================== */

static cmcp_client_t *connect_fs(char *const envp[]) {
    cmcp_client_t *c = cmcp_client_new("test_fs_server", "0.1.0");
    if (!c) return NULL;
    char *argv[] = { (char *)FS_BIN, "--root", g_root, NULL };
    if (cmcp_client_connect_stdio(c, FS_BIN, argv, envp) != CMCP_OK) {
        cmcp_client_free(c);
        return NULL;
    }
    return c;
}

/* Invoke a tool. `args` is consumed (NULL → empty object).
 *
 * Returns 0 on a successful dispatch: *out_text gets the first text
 * content item (malloc'd, caller frees; NULL if none) and *out_is_error
 * reflects the tool-level isError flag. A non-zero return is the
 * JSON-RPC error code (e.g. CMCP_RPC_INVALID_PARAMS) or a transport
 * error — neither sets the out-params. */
static int call_tool(cmcp_client_t *c, const char *name, cmcp_json_t *args,
                     char **out_text, int *out_is_error) {
    cmcp_json_t *params = cmcp_json_new_object();
    cmcp_json_object_set(params, "name", cmcp_json_new_string(name));
    cmcp_json_object_set(params, "arguments",
                         args ? args : cmcp_json_new_object());

    cmcp_rpc_message_t resp;
    int rc = cmcp_client_request(c, "tools/call", params, &resp);
    if (rc != CMCP_OK) return rc;
    if (resp.error) {
        int code = resp.error->code;
        cmcp_rpc_message_clear(&resp);
        return code;
    }

    *out_text     = NULL;
    *out_is_error = 0;
    const cmcp_json_t *ie = cmcp_json_object_get(resp.result, "isError");
    if (ie && ie->type == CMCP_JSON_BOOL && ie->b) *out_is_error = 1;

    const cmcp_json_t *content = cmcp_json_object_get(resp.result, "content");
    if (content && content->type == CMCP_JSON_ARRAY && content->arr.len > 0) {
        const cmcp_json_t *item = content->arr.items[0];
        const cmcp_json_t *txt =
            item ? cmcp_json_object_get(item, "text") : NULL;
        if (txt && txt->type == CMCP_JSON_STRING)
            *out_text = strdup(txt->str.s);
    }
    cmcp_rpc_message_clear(&resp);
    return 0;
}

/* Single string-valued argument object: {"<key>":"<val>"}. */
static cmcp_json_t *args1(const char *key, const char *val) {
    cmcp_json_t *o = cmcp_json_new_object();
    cmcp_json_object_set(o, key, cmcp_json_new_string(val));
    return o;
}

static int list_has_tool(const cmcp_json_t *tools, const char *name) {
    if (!tools || tools->type != CMCP_JSON_ARRAY) return 0;
    for (size_t i = 0; i < tools->arr.len; i++) {
        const cmcp_json_t *n =
            cmcp_json_object_get(tools->arr.items[i], "name");
        if (n && n->type == CMCP_JSON_STRING && strcmp(n->str.s, name) == 0)
            return 1;
    }
    return 0;
}

/* ====================================================================== */
/* Tests — handshake & discovery                                           */
/* ====================================================================== */

static void test_handshake(void) {
    const char *name = cmcp_client_server_name(g_cli);
    TEST_ASSERT(name != NULL && strcmp(name, "filesystem-mcp") == 0);
    TEST_ASSERT(cmcp_client_server_protocol(g_cli) != NULL);
}

static void test_tools_list(void) {
    cmcp_rpc_message_t resp;
    int rc = cmcp_client_request(g_cli, "tools/list", NULL, &resp);
    TEST_ASSERT(rc == CMCP_OK);
    if (rc != CMCP_OK) return;
    TEST_ASSERT(resp.error == NULL);

    const cmcp_json_t *tools = cmcp_json_object_get(resp.result, "tools");
    TEST_ASSERT(tools && tools->type == CMCP_JSON_ARRAY);
    TEST_ASSERT(tools && tools->arr.len == 4);
    TEST_ASSERT(list_has_tool(tools, "fs_list"));
    TEST_ASSERT(list_has_tool(tools, "fs_read"));
    TEST_ASSERT(list_has_tool(tools, "fs_stat"));
    TEST_ASSERT(list_has_tool(tools, "fs_write"));
    cmcp_rpc_message_clear(&resp);
}

/* ====================================================================== */
/* Tests — happy paths                                                     */
/* ====================================================================== */

static void test_fs_list(void) {
    char *text = NULL;
    int is_err = 1;
    int rc = call_tool(g_cli, "fs_list", NULL, &text, &is_err);
    TEST_ASSERT(rc == 0);
    TEST_ASSERT(is_err == 0);
    TEST_ASSERT(text && strstr(text, "poem.txt"));
    TEST_ASSERT(text && strstr(text, "sub"));
    free(text);
}

static void test_fs_read_whole(void) {
    char *text = NULL;
    int is_err = 1;
    int rc = call_tool(g_cli, "fs_read", args1("path", "poem.txt"),
                       &text, &is_err);
    TEST_ASSERT(rc == 0);
    TEST_ASSERT(is_err == 0);
    TEST_ASSERT(text && strstr(text, "line one"));
    TEST_ASSERT(text && strstr(text, "line five"));
    free(text);
}

static void test_fs_read_range(void) {
    cmcp_json_t *a = cmcp_json_new_object();
    cmcp_json_object_set(a, "path", cmcp_json_new_string("poem.txt"));
    cmcp_json_object_set(a, "offset", cmcp_json_new_int(2));
    cmcp_json_object_set(a, "limit", cmcp_json_new_int(2));

    char *text = NULL;
    int is_err = 1;
    int rc = call_tool(g_cli, "fs_read", a, &text, &is_err);
    TEST_ASSERT(rc == 0);
    TEST_ASSERT(is_err == 0);
    TEST_ASSERT(text && strstr(text, "line two"));
    TEST_ASSERT(text && strstr(text, "line three"));
    TEST_ASSERT(text && !strstr(text, "line one"));
    TEST_ASSERT(text && !strstr(text, "line four"));
    free(text);
}

static void test_fs_read_empty(void) {
    char *text = NULL;
    int is_err = 1;
    int rc = call_tool(g_cli, "fs_read", args1("path", "empty.txt"),
                       &text, &is_err);
    TEST_ASSERT(rc == 0);
    TEST_ASSERT(is_err == 0);
    TEST_ASSERT(text && strstr(text, "empty file"));
    free(text);
}

static void test_fs_read_range_past_eof(void) {
    cmcp_json_t *a = cmcp_json_new_object();
    cmcp_json_object_set(a, "path", cmcp_json_new_string("poem.txt"));
    cmcp_json_object_set(a, "offset", cmcp_json_new_int(99));

    char *text = NULL;
    int is_err = 1;
    int rc = call_tool(g_cli, "fs_read", a, &text, &is_err);
    TEST_ASSERT(rc == 0);
    TEST_ASSERT(is_err == 0);
    TEST_ASSERT(text && strstr(text, "no lines in the requested range"));
    free(text);
}

static void test_fs_stat(void) {
    char *text = NULL;
    int is_err = 1;
    int rc = call_tool(g_cli, "fs_stat", args1("path", "poem.txt"),
                       &text, &is_err);
    TEST_ASSERT(rc == 0);
    TEST_ASSERT(is_err == 0);
    TEST_ASSERT(text && strstr(text, "type:"));
    TEST_ASSERT(text && strstr(text, "file"));
    free(text);
}

static void test_fs_write_roundtrip(void) {
    static const char body[] = "written by test\n";
    char *text = NULL;
    int is_err = 1;

    cmcp_json_t *w = cmcp_json_new_object();
    cmcp_json_object_set(w, "path", cmcp_json_new_string("out.txt"));
    cmcp_json_object_set(w, "content", cmcp_json_new_string(body));
    int rc = call_tool(g_cli, "fs_write", w, &text, &is_err);
    TEST_ASSERT(rc == 0);
    TEST_ASSERT(is_err == 0);
    TEST_ASSERT(text && strstr(text, "wrote"));
    free(text);

    text = NULL;
    is_err = 1;
    rc = call_tool(g_cli, "fs_read", args1("path", "out.txt"),
                   &text, &is_err);
    TEST_ASSERT(rc == 0);
    TEST_ASSERT(is_err == 0);
    TEST_ASSERT(text && strcmp(text, body) == 0);
    free(text);
}

/* ====================================================================== */
/* Tests — the sandbox                                                     */
/* ====================================================================== */

static void test_traversal_rejected(void) {
    char *text = NULL;
    int is_err = 0;
    int rc = call_tool(g_cli, "fs_read",
                       args1("path", "../../../etc/passwd"),
                       &text, &is_err);
    TEST_ASSERT(rc == 0);
    TEST_ASSERT(is_err == 1);
    TEST_ASSERT(text && strstr(text, "escapes the server root"));
    free(text);
}

static void test_symlink_rejected(void) {
    char *text = NULL;
    int is_err = 0;
    int rc = call_tool(g_cli, "fs_read", args1("path", "escape"),
                       &text, &is_err);
    TEST_ASSERT(rc == 0);
    TEST_ASSERT(is_err == 1);
    TEST_ASSERT(text && strstr(text, "escapes the server root"));
    free(text);
}

static void test_absolute_rejected(void) {
    char *text = NULL;
    int is_err = 0;
    int rc = call_tool(g_cli, "fs_read", args1("path", "/etc/passwd"),
                       &text, &is_err);
    TEST_ASSERT(rc == 0);
    TEST_ASSERT(is_err == 1);
    TEST_ASSERT(text && strstr(text, "escapes the server root"));
    free(text);
}

static void test_write_traversal_rejected(void) {
    char *text = NULL;
    int is_err = 0;

    cmcp_json_t *w = cmcp_json_new_object();
    cmcp_json_object_set(w, "path",
                         cmcp_json_new_string("../fsmcp-PWNED.txt"));
    cmcp_json_object_set(w, "content", cmcp_json_new_string("pwned"));
    int rc = call_tool(g_cli, "fs_write", w, &text, &is_err);
    TEST_ASSERT(rc == 0);
    TEST_ASSERT(is_err == 1);
    TEST_ASSERT(text && strstr(text, "escapes the server root"));
    free(text);

    /* And make sure nothing actually landed outside the sandbox. */
    TEST_ASSERT(access("/tmp/fsmcp-PWNED.txt", F_OK) != 0);
}

/* ====================================================================== */
/* Tests — operation errors                                                */
/* ====================================================================== */

static void test_read_directory(void) {
    char *text = NULL;
    int is_err = 0;
    int rc = call_tool(g_cli, "fs_read", args1("path", "sub"),
                       &text, &is_err);
    TEST_ASSERT(rc == 0);
    TEST_ASSERT(is_err == 1);
    TEST_ASSERT(text && strstr(text, "directory"));
    free(text);
}

static void test_read_binary(void) {
    char *text = NULL;
    int is_err = 0;
    int rc = call_tool(g_cli, "fs_read", args1("path", "binary.bin"),
                       &text, &is_err);
    TEST_ASSERT(rc == 0);
    TEST_ASSERT(is_err == 1);
    TEST_ASSERT(text && strstr(text, "UTF-8"));
    free(text);
}

static void test_read_missing(void) {
    char *text = NULL;
    int is_err = 0;
    int rc = call_tool(g_cli, "fs_read", args1("path", "nope.txt"),
                       &text, &is_err);
    TEST_ASSERT(rc == 0);
    TEST_ASSERT(is_err == 1);
    TEST_ASSERT(text && strstr(text, "no such file"));
    free(text);
}

/* A missing required field is a protocol-level fault, not a tool-level
 * one: the server rejects it with -32602 before dispatch. */
static void test_schema_rejection(void) {
    char *text = NULL;
    int is_err = 0;
    int rc = call_tool(g_cli, "fs_read", NULL, &text, &is_err);
    TEST_ASSERT(rc == CMCP_RPC_INVALID_PARAMS);
}

/* ====================================================================== */
/* Tests — read-only mode                                                  */
/* ====================================================================== */

static void test_readonly_mode(void) {
    char *envp[] = { (char *)"FS_READONLY=1", NULL };
    cmcp_client_t *ro = connect_fs(envp);
    TEST_ASSERT(ro != NULL);
    if (!ro) return;

    cmcp_rpc_message_t resp;
    int rc = cmcp_client_request(ro, "tools/list", NULL, &resp);
    TEST_ASSERT(rc == CMCP_OK);
    if (rc == CMCP_OK) {
        const cmcp_json_t *tools = cmcp_json_object_get(resp.result, "tools");
        TEST_ASSERT(tools && tools->arr.len == 3);
        TEST_ASSERT(list_has_tool(tools, "fs_list"));
        TEST_ASSERT(list_has_tool(tools, "fs_read"));
        TEST_ASSERT(list_has_tool(tools, "fs_stat"));
        TEST_ASSERT(!list_has_tool(tools, "fs_write"));
        cmcp_rpc_message_clear(&resp);
    }

    /* fs_write is not registered — calling it is a JSON-RPC error. */
    char *text = NULL;
    int is_err = 0;
    cmcp_json_t *w = cmcp_json_new_object();
    cmcp_json_object_set(w, "path", cmcp_json_new_string("x.txt"));
    cmcp_json_object_set(w, "content", cmcp_json_new_string("y"));
    int crc = call_tool(ro, "fs_write", w, &text, &is_err);
    TEST_ASSERT(crc != 0);
    free(text);

    cmcp_client_free(ro);
}

/* ====================================================================== */
/* main                                                                    */
/* ====================================================================== */

int main(void) {
    make_fixtures();

    g_cli = connect_fs(NULL);
    if (!g_cli) {
        fprintf(stderr, "  could not spawn %s — build it first\n", FS_BIN);
        remove_fixtures();
        return 1;
    }

    TEST_RUN(test_handshake);
    TEST_RUN(test_tools_list);
    TEST_RUN(test_fs_list);
    TEST_RUN(test_fs_read_whole);
    TEST_RUN(test_fs_read_range);
    TEST_RUN(test_fs_read_empty);
    TEST_RUN(test_fs_read_range_past_eof);
    TEST_RUN(test_fs_stat);
    TEST_RUN(test_fs_write_roundtrip);
    TEST_RUN(test_traversal_rejected);
    TEST_RUN(test_symlink_rejected);
    TEST_RUN(test_absolute_rejected);
    TEST_RUN(test_write_traversal_rejected);
    TEST_RUN(test_read_directory);
    TEST_RUN(test_read_binary);
    TEST_RUN(test_read_missing);
    TEST_RUN(test_schema_rejection);

    cmcp_client_free(g_cli);

    TEST_RUN(test_readonly_mode);

    remove_fixtures();
    TEST_DONE();
}
