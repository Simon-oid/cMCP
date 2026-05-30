/*
 * dogfood-crag-host — a butlerbot-shaped host harness that drives a
 * realistic session against tools/crag-mcp/.
 *
 * Purpose (council D1 verdict, 2026-05-30): produce evidence about
 * what the client API actually feels like from a host author's seat.
 * NOT a test — a use. Every "wait, the library should…" moment is a
 * finding for docs/dogfood-cragmcp.md.
 *
 * The harness intentionally exercises the patterns butlerbot will
 * use:
 *   1. handshake + capability inspection (read server identity)
 *   2. list tools, walk the inputSchema for one of them
 *   3. read crag://stats as ambient context (resources path)
 *   4. run 3 sync searches, instrumented latency per call
 *   5. run 3 async searches in parallel, wait in any order
 *   6. error path: empty query (schema bound), oversized k
 *   7. tear down cleanly
 *
 * Every step logs "expect" / "got" / "finding" lines to stderr.
 *
 * Build:   make dogfood-crag-host       (opt-in target)
 * Run:     CRAG_EMBED_BACKEND=ollama CRAG_EMBED_MODEL=mxbai-embed-large \
 *            ./tools/dogfood-crag-host/dogfood-crag-host \
 *              --db /tmp/cmcp-dogfood/cmcp-docs.db
 *
 * Wire-record under cmcp-tee:
 *          ./tools/dogfood-crag-host/dogfood-crag-host \
 *              --tee /tmp/cmcp-tee \
 *              --tee-log conformance/fixtures/crag-mcp/dogfood/session.jsonl \
 *              --db /tmp/cmcp-dogfood/cmcp-docs.db
 */

#define _POSIX_C_SOURCE 200809L

#include "cmcp.h"
#include "cmcp_client.h"
#include "cmcp_json.h"
#include "cmcp_types.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* ---------- step instrumentation ------------------------------------- */

static int g_step = 0;
static int g_findings = 0;

#define STEP(desc) do { \
    g_step++; \
    fprintf(stderr, "\n=== step %d — %s ===\n", g_step, desc); \
} while (0)

#define EXPECT(what) fprintf(stderr, "  expect: %s\n", what)
#define GOT(fmt, ...) fprintf(stderr, "  got:    " fmt "\n", ##__VA_ARGS__)

#define FINDING(label, body) do { \
    g_findings++; \
    fprintf(stderr, "  >>> FINDING #%d (%s): %s\n", g_findings, label, body); \
} while (0)

static double now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec * 1000.0 + (double)ts.tv_nsec / 1e6;
}

/* ---------- helpers ---------------------------------------------------- */

/* Build {"name": <name>, "arguments": <args_obj>}. args_obj is consumed. */
static cmcp_json_t *call_params(const char *name, cmcp_json_t *args_obj) {
    cmcp_json_t *p = cmcp_json_new_object();
    cmcp_json_object_set(p, "name", cmcp_json_new_string(name));
    cmcp_json_object_set(p, "arguments", args_obj);
    return p;
}

/* Print the first text content item from a tools/call result, or "<no
 * text content>" if there isn't one. Returns 1 if the result is a
 * tool-level error (isError:true), 0 otherwise. */
static int print_result_text(const cmcp_json_t *result, const char *label) {
    const cmcp_json_t *is_err  = cmcp_json_object_get(result, "isError");
    const cmcp_json_t *content = cmcp_json_object_get(result, "content");
    int err = (is_err && cmcp_json_bool(is_err));
    if (!content || content->type != CMCP_JSON_ARRAY || content->arr.len == 0) {
        fprintf(stderr, "  %s: <no content>\n", label);
        return err;
    }
    const cmcp_json_t *item = cmcp_json_array_at(content, 0);
    const cmcp_json_t *text = cmcp_json_object_get(item, "text");
    const char *s = cmcp_json_string(text);
    size_t n = cmcp_json_string_len(text);
    if (s) {
        char buf[120];
        size_t take = n < sizeof(buf) - 1 ? n : sizeof(buf) - 1;
        memcpy(buf, s, take);
        buf[take] = 0;
        for (size_t i = 0; i < take; i++)
            if (buf[i] == '\n') buf[i] = ' ';
        fprintf(stderr, "  %s: \"%s%s\" (%zu items)\n",
                label, buf, n > take ? "..." : "",
                cmcp_json_array_len(content));
    } else {
        fprintf(stderr, "  %s: <first item has no text>\n", label);
    }
    return err;
}

/* ---------- the session ----------------------------------------------- */

static int run_session(const char *server_path,
                        const char *db_path,
                        const char *tee_log,
                        const char *tee_path) {
    /* The harness deliberately uses only the public client surface. If
     * the API forces awkward shapes, those are findings. */

    cmcp_client_t *cli = cmcp_client_new("dogfood-crag-host", "0.1.0");
    if (!cli) {
        fprintf(stderr, "FATAL: cmcp_client_new failed\n");
        return 1;
    }

    /* --- step 1: spawn + handshake ----------------------------------- */
    STEP("spawn crag-mcp + handshake");
    EXPECT("CMCP_OK + server identity readable");

    /* Two spawn shapes: direct, or wrapped through cmcp-tee.
     *    direct: argv = [server_path, --db, db_path, NULL]
     *    tee:    argv = [tee_path, tee_log, server_path, --db, db_path, NULL] */
    const char *spawn_path = server_path;
    char *argv[8];
    int  ai = 0;
    if (tee_log) {
        spawn_path = tee_path;
        argv[ai++] = (char *)tee_path;
        argv[ai++] = (char *)tee_log;
        argv[ai++] = (char *)server_path;
    } else {
        argv[ai++] = (char *)server_path;
    }
    argv[ai++] = "--db";
    argv[ai++] = (char *)db_path;
    argv[ai] = NULL;
    double t0 = now_ms();
    int rc = cmcp_client_connect_stdio(cli, spawn_path, argv, NULL);
    double t1 = now_ms();
    if (rc != CMCP_OK) {
        fprintf(stderr, "FATAL: connect_stdio rc=%d\n", rc);
        cmcp_client_free(cli);
        return 1;
    }
    GOT("rc=CMCP_OK in %.1f ms — server=\"%s\" v=\"%s\" proto=\"%s\"",
        t1 - t0,
        cmcp_client_server_name(cli)     ? cmcp_client_server_name(cli)     : "(null)",
        cmcp_client_server_version(cli)  ? cmcp_client_server_version(cli)  : "(null)",
        cmcp_client_server_protocol(cli) ? cmcp_client_server_protocol(cli) : "(null)");

    /* --- step 2: tools/list ------------------------------------------ */
    STEP("tools/list and inspect inputSchema for crag_search");
    EXPECT("a typed helper that returns an array of {name, description, schema}");

    cmcp_rpc_message_t resp;
    memset(&resp, 0, sizeof(resp));
    rc = cmcp_client_request(cli, "tools/list", NULL, &resp);
    if (rc != CMCP_OK || resp.error) {
        fprintf(stderr, "FATAL: tools/list rc=%d error=%p\n", rc, (void *)resp.error);
        cmcp_rpc_message_clear(&resp);
        cmcp_client_free(cli);
        return 1;
    }
    const cmcp_json_t *tools_arr = cmcp_json_object_get(resp.result, "tools");
    size_t tools_n = cmcp_json_array_len(tools_arr);
    if (tools_n == 0) {
        fprintf(stderr, "FATAL: tools/list result has no \"tools\" array or it is empty\n");
        cmcp_rpc_message_clear(&resp);
        cmcp_client_free(cli);
        return 1;
    }
    GOT("walked %zu tools by hand via cmcp_json_object_get(\"tools\")", tools_n);
    FINDING("client-api-ergonomics",
            "single-client tools/list returns raw JSON; the host walks "
            "cmcp_json_object_get(\"tools\"), then per-tool "
            "cmcp_json_object_get(\"name\"/\"description\"/\"inputSchema\"). "
            "cmcp_session.h exposes a typed cmcp_session_tools_list returning "
            "cmcp_session_tool_t[] — but only via the session aggregator. A "
            "host talking to ONE server must either drop to raw JSON or wrap "
            "one server in a session. Mirror the session helpers onto "
            "cmcp_client_t (cmcp_client_tools_list, cmcp_client_resources_list, "
            "cmcp_client_prompts_list).");
    cmcp_rpc_message_clear(&resp);

    /* --- step 3: resources/read crag://stats ------------------------ */
    STEP("resources/read crag://stats (ambient-context pattern)");
    EXPECT("a one-call helper returning the resource's text directly");

    cmcp_json_t *rparams = cmcp_json_new_object();
    cmcp_json_object_set(rparams, "uri", cmcp_json_new_string("crag://stats"));
    memset(&resp, 0, sizeof(resp));
    rc = cmcp_client_request(cli, "resources/read", rparams, &resp);
    if (rc != CMCP_OK || resp.error) {
        fprintf(stderr, "FATAL: resources/read rc=%d\n", rc);
        cmcp_rpc_message_clear(&resp);
        cmcp_client_free(cli);
        return 1;
    }
    const cmcp_json_t *contents = cmcp_json_object_get(resp.result, "contents");
    if (cmcp_json_array_len(contents) > 0) {
        const cmcp_json_t *text =
            cmcp_json_object_get(cmcp_json_array_at(contents, 0), "text");
        const char *s = cmcp_json_string(text);
        size_t n = cmcp_json_string_len(text);
        if (s) {
            char buf[160];
            size_t take = n < sizeof(buf) - 1 ? n : sizeof(buf) - 1;
            memcpy(buf, s, take);
            buf[take] = 0;
            for (size_t i = 0; i < take; i++) if (buf[i] == '\n') buf[i] = ' ';
            GOT("stats text (preview): \"%s%s\"", buf, n > take ? "..." : "");
        }
    }
    FINDING("client-api-ergonomics",
            "same shape: resources/read returns {contents:[{type,text|blob,...}]} "
            "and the host hand-walks contents[0].text. Add "
            "cmcp_client_resource_read(uri, out_text_or_blob).");
    cmcp_rpc_message_clear(&resp);

    /* --- step 4: 3 sync searches ------------------------------------ */
    STEP("3 sync crag_search calls, varying query, default k");

    const char *queries[3] = {"schema validator", "HTTP transport", "fuzzing harness"};
    for (int i = 0; i < 3; i++) {
        cmcp_json_t *args = cmcp_json_new_object();
        cmcp_json_object_set(args, "query", cmcp_json_new_string(queries[i]));
        memset(&resp, 0, sizeof(resp));
        double q0 = now_ms();
        rc = cmcp_client_request(cli, "tools/call", call_params("crag_search", args), &resp);
        double q1 = now_ms();
        if (rc != CMCP_OK) {
            fprintf(stderr, "  search[%d]: rc=%d\n", i, rc);
            cmcp_rpc_message_clear(&resp);
            continue;
        }
        if (resp.error) {
            fprintf(stderr, "  search[%d]: RPC error %d %s\n", i,
                    resp.error->code,
                    resp.error->message ? resp.error->message : "");
            cmcp_rpc_message_clear(&resp);
            continue;
        }
        char label[64];
        snprintf(label, sizeof(label), "search[%d] %6.1fms \"%s\"",
                 i, q1 - q0, queries[i]);
        print_result_text(resp.result, label);
        cmcp_rpc_message_clear(&resp);
    }

    /* --- step 5: 3 async searches in parallel ----------------------- */
    STEP("3 async crag_search calls in parallel; wait in any order");
    EXPECT("3 IDs, each completable independently, total time ~= max(single)");

    long long ids[3];
    double s0 = now_ms();
    const char *async_q[3] = {"json layer", "client async API", "transport vtable"};
    for (int i = 0; i < 3; i++) {
        cmcp_json_t *args = cmcp_json_new_object();
        cmcp_json_object_set(args, "query", cmcp_json_new_string(async_q[i]));
        rc = cmcp_client_call_async(cli, "tools/call",
                                    call_params("crag_search", args), &ids[i]);
        if (rc != CMCP_OK) {
            fprintf(stderr, "  call_async[%d] rc=%d\n", i, rc);
            ids[i] = -1;
        }
    }

    /* Reverse-order wait. */
    for (int i = 2; i >= 0; i--) {
        if (ids[i] < 0) continue;
        memset(&resp, 0, sizeof(resp));
        rc = cmcp_client_wait(cli, ids[i], &resp);
        double sN = now_ms();
        if (rc != CMCP_OK) {
            fprintf(stderr, "  wait[%d] rc=%d\n", i, rc);
            cmcp_rpc_message_clear(&resp);
            continue;
        }
        char label[64];
        snprintf(label, sizeof(label), "async[%d] (id=%lld) @+%6.1fms \"%s\"",
                 i, ids[i], sN - s0, async_q[i]);
        print_result_text(resp.result, label);
        cmcp_rpc_message_clear(&resp);
    }

    /* --- step 6: schema-rejection error paths ----------------------- */
    STEP("error paths: empty query (minLength) and k=999 (maximum)");
    EXPECT("tool-level isError:true with informative content text "
           "(per MCP 2025-11-25 convention; NOT JSON-RPC -32602)");

    {
        cmcp_json_t *args = cmcp_json_new_object();
        cmcp_json_object_set(args, "query", cmcp_json_new_string(""));
        memset(&resp, 0, sizeof(resp));
        rc = cmcp_client_request(cli, "tools/call",
                                 call_params("crag_search", args), &resp);
        if (resp.error) {
            fprintf(stderr, "  empty query → JSON-RPC error code=%d msg=\"%s\"\n",
                    resp.error->code,
                    resp.error->message ? resp.error->message : "(null)");
        } else {
            int is_err = print_result_text(resp.result, "empty query");
            if (!is_err)
                FINDING("schema-error-shape",
                        "empty query was ACCEPTED (neither JSON-RPC error nor tool isError) — "
                        "schema bound missing or validator broken");
        }
        cmcp_rpc_message_clear(&resp);
    }
    {
        cmcp_json_t *args = cmcp_json_new_object();
        cmcp_json_object_set(args, "query", cmcp_json_new_string("anything"));
        cmcp_json_object_set(args, "k",     cmcp_json_new_int(999));
        memset(&resp, 0, sizeof(resp));
        rc = cmcp_client_request(cli, "tools/call",
                                 call_params("crag_search", args), &resp);
        if (resp.error) {
            fprintf(stderr, "  k=999 → JSON-RPC error code=%d msg=\"%s\"\n",
                    resp.error->code,
                    resp.error->message ? resp.error->message : "(null)");
        } else {
            int is_err = print_result_text(resp.result, "k=999");
            if (!is_err)
                FINDING("schema-k-bound",
                        "k=999 was ACCEPTED (neither JSON-RPC error nor tool isError) — "
                        "schema bound missing or validator broken");
        }
        cmcp_rpc_message_clear(&resp);
    }

    FINDING("error-model-bifurcation",
            "a tools/call can fail two ways the host must distinguish: (a) "
            "JSON-RPC error in response.error (transport/method/protocol "
            "errors), (b) tool-level error in result.isError=true + "
            "result.content[].text (handler-reported errors, including "
            "schema rejections per 2025-11-25). No client helper flattens "
            "both into a single success/error path. cmcp_client_tool_call("
            "name, args, &text, &is_error, &json_rpc_error) would.");

    /* --- step 7: unknown tool name --------------------------------- */
    STEP("error path: unknown tool name");
    EXPECT("tool-level error or -32602; SOME signal that tells the host \"that name does not exist\"");

    {
        cmcp_json_t *args = cmcp_json_new_object();
        memset(&resp, 0, sizeof(resp));
        rc = cmcp_client_request(cli, "tools/call",
                                 call_params("crag_doesnt_exist", args), &resp);
        if (resp.error) {
            GOT("unknown tool → code=%d msg=\"%s\"",
                resp.error->code,
                resp.error->message ? resp.error->message : "(null)");
        } else {
            print_result_text(resp.result, "unknown tool result");
        }
        cmcp_rpc_message_clear(&resp);
    }

    /* --- teardown --------------------------------------------------- */
    STEP("client free (asks child to exit, reaps)");
    cmcp_client_free(cli);

    fprintf(stderr, "\n=== summary ===\n  steps:    %d\n  findings: %d\n",
            g_step, g_findings);
    return 0;
}

/* ---------- main ------------------------------------------------------ */

static void usage(const char *argv0) {
    fprintf(stderr,
        "usage: %s --db DB_PATH [--server PATH] [--tee-log PATH] [--tee PATH]\n"
        "\n"
        "Drives a realistic host session against crag-mcp. Output is\n"
        "stderr-only (host-style logging); the wire goes through stdio\n"
        "as MCP/JSON-RPC.\n"
        "\n"
        "--tee-log PATH wraps the server through cmcp-tee, writing every\n"
        "  wire frame to PATH (JSONL). --tee PATH overrides the\n"
        "  cmcp-tee binary location (default ./tools/cmcp-tee/cmcp-tee).\n"
        "\n"
        "Env: CRAG_EMBED_BACKEND, CRAG_EMBED_MODEL must be set so the\n"
        "server can embed queries at search time.\n", argv0);
}

int main(int argc, char **argv) {
    const char *server_path = "./tools/crag-mcp/crag-mcp";
    const char *db_path     = NULL;
    const char *tee_log     = NULL;
    const char *tee_path    = "./tools/cmcp-tee/cmcp-tee";

    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--db") && i + 1 < argc) {
            db_path = argv[++i];
        } else if (!strcmp(argv[i], "--server") && i + 1 < argc) {
            server_path = argv[++i];
        } else if (!strcmp(argv[i], "--tee-log") && i + 1 < argc) {
            tee_log = argv[++i];
        } else if (!strcmp(argv[i], "--tee") && i + 1 < argc) {
            tee_path = argv[++i];
        } else if (!strcmp(argv[i], "-h") || !strcmp(argv[i], "--help")) {
            usage(argv[0]);
            return 0;
        } else {
            fprintf(stderr, "unknown arg: %s\n", argv[i]);
            usage(argv[0]);
            return 2;
        }
    }
    if (!db_path) {
        fprintf(stderr, "--db is required\n");
        usage(argv[0]);
        return 2;
    }
    return run_session(server_path, db_path, tee_log, tee_path);
}
