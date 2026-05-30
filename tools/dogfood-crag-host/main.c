/*
 * dogfood-crag-host — a butlerbot-shaped host harness that drives a
 * realistic session against tools/crag-mcp/.
 *
 * Purpose (council D1 verdict, 2026-05-30): produce evidence about
 * what the client API actually feels like from a host author's seat.
 * NOT a test — a use. Every "wait, the library should…" moment is a
 * finding for docs/dogfood-cragmcp.md.
 *
 * v0.6.0 rewrite (O1, 2026-05-30): the harness now uses ONLY the
 * typed helpers from A1 + A2. Zero direct cmcp_json_object_get,
 * zero direct cmcp_rpc_message_t. Step 6 (error paths) is a single
 * switch (cmcp_client_tool_call(...)) per call site. The original
 * F1 (client-api-ergonomics) and F2 (error-model-bifurcation)
 * findings are retired here — they reappear as ✓-closed markers so
 * the wire log of a dogfood run still records what the new surface
 * looks like in action.
 *
 * Pattern coverage (still the butlerbot shape):
 *   1. handshake + capability inspection (read server identity)
 *   2. cmcp_client_tools_list -> walk typed records
 *   3. cmcp_client_resource_read -> ambient context (crag://stats)
 *   4. 3 sync cmcp_client_tool_call -> 3-way outcome switch
 *   5. (was parallel async) sequential fan — A1/A2 are sync-only;
 *      see new finding for the async-typed gap
 *   6. error paths: empty query, k=999 -> one switch per site
 *   7. unknown-tool: one switch -> CMCP_TOOL_ERR_PROTOCOL
 *   8. tear down cleanly
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
#include "cmcp_session.h"   /* typed record shapes (cmcp_session_tool_t etc.) */
#include "cmcp_types.h"     /* cmcp_rpc_error_t + cmcp_rpc_error_free */

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

#define CLOSED(label, body) \
    fprintf(stderr, "  >>> CLOSED (%s): %s\n", label, body)

static double now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec * 1000.0 + (double)ts.tv_nsec / 1e6;
}

/* ---------- one-call-per-site tool_call helper printer -----------------
 *
 * Wraps cmcp_client_tool_call's 3-way switch into a "what should the host
 * log?" decision. The harness uses this for every step 4 / 6 / 7 call
 * site so the host's error handling is a single switch per call rather
 * than two branches (response.error vs result.isError) the original
 * harness had to write by hand. */
static void log_tool_outcome(double dt_ms,
                              const char *site,
                              cmcp_tool_outcome_t outcome,
                              cmcp_json_t *result_json,
                              char *text,
                              cmcp_rpc_error_t *rpc_err) {
    switch (outcome) {
    case CMCP_TOOL_OK: {
        /* The OK channel still hands the host a raw cmcp_json_t — the
         * host wants the first content[].text for display, but A1/A2
         * don't have a flattener for the success path. emit_stable
         * shows the whole shape; the v0.7 finding below pencils a
         * cmcp_client_tool_call_text shortcut. */
        char *s = cmcp_json_emit_stable(result_json);
        size_t n = s ? strlen(s) : 0;
        char buf[160];
        size_t take = n < sizeof(buf) - 1 ? n : sizeof(buf) - 1;
        if (s) memcpy(buf, s, take);
        buf[take] = 0;
        for (size_t i = 0; i < take; i++) if (buf[i] == '\n') buf[i] = ' ';
        fprintf(stderr, "  %s %.1fms: OK %s%s\n",
                site, dt_ms, buf, n > take ? "..." : "");
        free(s);
        cmcp_json_free(result_json);
        break;
    }
    case CMCP_TOOL_ERR_TOOL_LEVEL: {
        char buf[160];
        size_t n = text ? strlen(text) : 0;
        size_t take = n < sizeof(buf) - 1 ? n : sizeof(buf) - 1;
        if (text) memcpy(buf, text, take);
        buf[take] = 0;
        for (size_t i = 0; i < take; i++) if (buf[i] == '\n') buf[i] = ' ';
        fprintf(stderr, "  %s %.1fms: TOOL_ERR \"%s%s\"\n",
                site, dt_ms, buf, n > take ? "..." : "");
        free(text);
        break;
    }
    case CMCP_TOOL_ERR_PROTOCOL: {
        fprintf(stderr, "  %s %.1fms: PROTOCOL code=%d msg=\"%s\"\n",
                site, dt_ms,
                rpc_err ? rpc_err->code : 0,
                (rpc_err && rpc_err->message) ? rpc_err->message : "(null)");
        cmcp_rpc_error_free(rpc_err);
        break;
    }
    }
}

/* ---------- the session ----------------------------------------------- */

static int run_session(const char *server_path,
                        const char *db_path,
                        const char *tee_log,
                        const char *tee_path) {
    /* The harness deliberately uses only the A1/A2 public client
     * surface. Anywhere the surface forces an awkward shape, that is
     * a finding. */

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

    /* --- step 2: tools/list via cmcp_client_tools_list --------------- */
    STEP("cmcp_client_tools_list (A1) and inspect typed records");
    EXPECT("CMCP_OK + array of cmcp_session_tool_t with name/description/input_schema");

    cmcp_session_tool_t *tools = NULL;
    size_t tools_n = 0;
    rc = cmcp_client_tools_list(cli, &tools, &tools_n);
    if (rc != CMCP_OK) {
        fprintf(stderr, "FATAL: cmcp_client_tools_list rc=%d\n", rc);
        cmcp_client_free(cli);
        return 1;
    }
    GOT("%zu tools, typed records — no raw JSON walking", tools_n);
    for (size_t i = 0; i < tools_n; i++) {
        fprintf(stderr, "    [%zu] name=\"%s\" desc=%s schema=%s server=%s qualified=%s\n",
                i,
                tools[i].name ? tools[i].name : "(null)",
                tools[i].description ? "<present>" : "<null>",
                tools[i].input_schema ? "<present>" : "<null>",
                tools[i].server ? tools[i].server : "<null (single-client)>",
                tools[i].qualified ? tools[i].qualified : "<null (single-client)>");
    }
    CLOSED("F1 client-api-ergonomics-tools",
           "A1 cmcp_client_tools_list closes the host-side hand-walk gap "
           "for tools/list. .server/.qualified are NULL on single-client "
           "records (documented).");
    cmcp_session_tools_free(tools, tools_n);

    /* --- step 3: cmcp_client_resource_read -------------------------- */
    STEP("cmcp_client_resource_read(\"crag://stats\") (A1, ambient-context pattern)");
    EXPECT("CMCP_OK + owned text payload; no resp.result walking");

    char *stats_text = NULL;
    size_t stats_n = 0;
    rc = cmcp_client_resource_read(cli, "crag://stats", &stats_text, &stats_n);
    if (rc != CMCP_OK) {
        fprintf(stderr, "  resource_read rc=%d (CMCP_ENOTFOUND/EUNSUPPORTED/EPROTOCOL paths covered by the helper)\n", rc);
    } else if (stats_text) {
        char buf[160];
        size_t take = stats_n < sizeof(buf) - 1 ? stats_n : sizeof(buf) - 1;
        memcpy(buf, stats_text, take);
        buf[take] = 0;
        for (size_t i = 0; i < take; i++) if (buf[i] == '\n') buf[i] = ' ';
        GOT("stats text %zu bytes (preview): \"%s%s\"",
            stats_n, buf, stats_n > take ? "..." : "");
    }
    CLOSED("F1 client-api-ergonomics-resources",
           "A1 cmcp_client_resource_read closes the host-side hand-walk "
           "gap for resources/read. Blob bodies surface as CMCP_EUNSUPPORTED, "
           "so the host opts in to text-only without walking type tags.");
    free(stats_text);

    /* --- step 4: 3 sync crag_search via cmcp_client_tool_call ------- */
    STEP("3 sync cmcp_client_tool_call (A2) — one 3-way switch per site");

    const char *queries[3] = {"schema validator", "HTTP transport", "fuzzing harness"};
    for (int i = 0; i < 3; i++) {
        cmcp_json_t *args = cmcp_json_new_object();
        cmcp_json_object_set(args, "query", cmcp_json_new_string(queries[i]));

        cmcp_json_t      *result_json = NULL;
        char             *err_text    = NULL;
        cmcp_rpc_error_t *rpc_err     = NULL;
        char site[64];
        snprintf(site, sizeof(site), "search[%d] \"%s\"", i, queries[i]);

        double q0 = now_ms();
        cmcp_tool_outcome_t outcome = cmcp_client_tool_call(
            cli, "crag_search", args, &result_json, &err_text, &rpc_err);
        double q1 = now_ms();
        log_tool_outcome(q1 - q0, site, outcome, result_json, err_text, rpc_err);
    }

    /* --- step 5: was async-parallel; A1/A2 are sync-only ------------ */
    STEP("sequential fan of 3 cmcp_client_tool_call (was async-parallel)");
    EXPECT("3 sync calls; total time is sum, not max(single)");

    const char *async_q[3] = {"json layer", "client async API", "transport vtable"};
    double s0 = now_ms();
    for (int i = 0; i < 3; i++) {
        cmcp_json_t *args = cmcp_json_new_object();
        cmcp_json_object_set(args, "query", cmcp_json_new_string(async_q[i]));

        cmcp_json_t      *result_json = NULL;
        char             *err_text    = NULL;
        cmcp_rpc_error_t *rpc_err     = NULL;
        char site[64];
        snprintf(site, sizeof(site), "fan[%d] @+%.1fms \"%s\"",
                 i, now_ms() - s0, async_q[i]);

        double q0 = now_ms();
        cmcp_tool_outcome_t outcome = cmcp_client_tool_call(
            cli, "crag_search", args, &result_json, &err_text, &rpc_err);
        double q1 = now_ms();
        log_tool_outcome(q1 - q0, site, outcome, result_json, err_text, rpc_err);
    }
    FINDING("v0.7-async-typed-tool-call",
            "A1/A2 are sync-only. The async surface (cmcp_client_call_async + "
            "cmcp_client_wait) still hands raw cmcp_rpc_message_t to the host, "
            "so the rewrite cannot use it without re-importing the two-channel "
            "error model A2 eliminated. v0.7 candidate: cmcp_client_tool_call_async("
            "name, args, &id) returning a long long, paired with cmcp_client_tool_wait("
            "id, &result, &text, &rpc_err) that yields the same 3-way outcome as A2. "
            "Parallel fan-out then stays in the flattened model.");

    /* --- step 6: schema-rejection error paths ----------------------- */
    STEP("error paths: empty query (minLength), k=999 (maximum) — one switch per site");
    EXPECT("CMCP_TOOL_ERR_TOOL_LEVEL with informative text "
           "(MCP 2025-11-25 routes input-schema rejection to the result channel)");

    {
        cmcp_json_t *args = cmcp_json_new_object();
        cmcp_json_object_set(args, "query", cmcp_json_new_string(""));
        cmcp_json_t      *result_json = NULL;
        char             *err_text    = NULL;
        cmcp_rpc_error_t *rpc_err     = NULL;
        double q0 = now_ms();
        cmcp_tool_outcome_t outcome = cmcp_client_tool_call(
            cli, "crag_search", args, &result_json, &err_text, &rpc_err);
        double q1 = now_ms();
        log_tool_outcome(q1 - q0, "empty-query", outcome,
                         result_json, err_text, rpc_err);
        if (outcome == CMCP_TOOL_OK)
            FINDING("schema-error-shape",
                    "empty query was ACCEPTED — schema bound missing or validator broken");
    }
    {
        cmcp_json_t *args = cmcp_json_new_object();
        cmcp_json_object_set(args, "query", cmcp_json_new_string("anything"));
        cmcp_json_object_set(args, "k",     cmcp_json_new_int(999));
        cmcp_json_t      *result_json = NULL;
        char             *err_text    = NULL;
        cmcp_rpc_error_t *rpc_err     = NULL;
        double q0 = now_ms();
        cmcp_tool_outcome_t outcome = cmcp_client_tool_call(
            cli, "crag_search", args, &result_json, &err_text, &rpc_err);
        double q1 = now_ms();
        log_tool_outcome(q1 - q0, "k=999", outcome,
                         result_json, err_text, rpc_err);
        if (outcome == CMCP_TOOL_OK)
            FINDING("schema-k-bound",
                    "k=999 was ACCEPTED — schema bound missing or validator broken");
    }
    CLOSED("F2 error-model-bifurcation",
           "A2 cmcp_client_tool_call collapses the response.error / result.isError "
           "two-channel model into a single 3-way switch. Step 6 has zero "
           "two-branch error reads.");

    /* --- step 7: unknown tool name ---------------------------------- */
    STEP("error path: unknown tool name — one switch");
    EXPECT("CMCP_TOOL_ERR_PROTOCOL with -32602 + {name} structured data");

    {
        cmcp_json_t *args = cmcp_json_new_object();
        cmcp_json_t      *result_json = NULL;
        char             *err_text    = NULL;
        cmcp_rpc_error_t *rpc_err     = NULL;
        double q0 = now_ms();
        cmcp_tool_outcome_t outcome = cmcp_client_tool_call(
            cli, "crag_doesnt_exist", args, &result_json, &err_text, &rpc_err);
        double q1 = now_ms();
        log_tool_outcome(q1 - q0, "unknown-tool", outcome,
                         result_json, err_text, rpc_err);
    }

    /* Bonus finding (the OK-path content shortcut). cmcp_client_tool_call's
     * OK channel returns raw cmcp_json_t — the host still has to walk the
     * result to extract content[].text for display, even though A2 already
     * extracts it on the TOOL_LEVEL path. */
    FINDING("v0.7-tool-call-text-shortcut",
            "cmcp_client_tool_call OK path returns raw cmcp_json_t — the host "
            "wants content[0].text but the helper only extracts it for the "
            "TOOL_LEVEL path. v0.7 candidate: cmcp_client_tool_call_text(name, "
            "args, &text) flattens both content[].text outcomes (success or "
            "tool error) into a single string, keeping CMCP_TOOL_ERR_PROTOCOL "
            "as the only out-of-band channel.");

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
