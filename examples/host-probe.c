/* host-probe — a stateful multi-server MCP host that drives the client
 * API the way a real agent host (butlerbot) would, deliberately BEYOND
 * what cmcp-inspect / minimal-client already cover (connect → handshake →
 * list → one sync call). This is the P6 validation probe from the
 * 2026-06-01 LLM Council verdict.
 *
 * The council's framing: the deliverable is NOT this program — it is the
 * friction list against the client API that writing and running a real
 * stateful consumer surfaces. Findings are tagged `FRICTION:` in comments
 * here and collected in TODO.md. The point is falsification: every prior
 * hardening claim was self-judged; this is the first real host to hold
 * the client API in anger.
 *
 * What it exercises (the untested surface, per peer review):
 *   1. Two child servers (echo-server + filesystem-mcp) aggregated in one
 *      cmcp_session — multi-server multiplexing.
 *   2. tools/list fanned across both (parallel fan-out + merge).
 *   3. THREE tools/call in flight at once, waited in non-submission order
 *      — async multi-in-flight + any-order completion. cmcp-inspect only
 *      ever has one call outstanding.
 *   4. Cancel of an in-flight call + the mandatory post-cancel wait that
 *      reclaims the completion record (the thread-safety contract's rule).
 *   5. The two-channel error model (JSON-RPC error vs tool-level isError)
 *      via the 3-way cmcp_tool_outcome_t.
 *
 * Build:  make examples/host-probe
 * Run:    ./examples/host-probe            (from the repo root)
 */

#include "cmcp.h"
#include "cmcp_client.h"
#include "cmcp_session.h"
#include "cmcp_json.h"
#include "cmcp_types.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Tiny helper: spawn a server as a child and hand it to the session.
 * Returns CMCP_OK or a negative error. On failure the client is freed. */
static int add_server(cmcp_session_t *sess, const char *name,
                      const char *path, char *const argv[]) {
    cmcp_client_t *c = cmcp_client_new("host-probe", "0.1.0");
    if (!c) return CMCP_ENOMEM;
    int rc = cmcp_client_connect_stdio(c, path, argv, NULL);
    if (rc != CMCP_OK) {
        fprintf(stderr, "  ! connect/handshake to %s (%s) failed: %d\n",
                name, path, rc);
        cmcp_client_free(c);
        return rc;
    }
    rc = cmcp_session_add(sess, name, c);
    if (rc != CMCP_OK) {
        fprintf(stderr, "  ! session_add(%s) failed: %d\n", name, rc);
        cmcp_client_free(c);              /* still ours on add failure */
        return rc;
    }
    fprintf(stderr, "  + %-4s -> %s (server: %s %s)\n", name, path,
            cmcp_client_server_name(c) ? cmcp_client_server_name(c) : "?",
            cmcp_client_server_version(c) ? cmcp_client_server_version(c) : "?");
    return CMCP_OK;
}

/* Print one tool-call outcome compactly, then free the result's owned
 * payload. The result is passed by value (P7 redesign) — there is no
 * out-param to read in the wrong order anymore. */
static void report_outcome(const char *label, cmcp_tool_result_t res) {
    switch (res.outcome) {
    case CMCP_TOOL_OK: {
        char *raw = res.result ? cmcp_json_emit(res.result) : NULL;
        fprintf(stderr, "  [%s] OK            %s\n", label, raw ? raw : "(null)");
        free(raw);
        break;
    }
    case CMCP_TOOL_ERR_TOOL_LEVEL:
        fprintf(stderr, "  [%s] TOOL-ERROR    %s\n", label,
                res.text ? res.text : "");
        break;
    case CMCP_TOOL_ERR_PROTOCOL:
        fprintf(stderr, "  [%s] RPC-ERROR     code=%d msg=%s\n", label,
                res.error ? res.error->code : 0,
                (res.error && res.error->message) ? res.error->message : "");
        break;
    case CMCP_TOOL_ERR_CANCELLED:
        fprintf(stderr, "  [%s] CANCELLED\n", label);
        break;
    }
    cmcp_tool_result_clear(&res);
}

int main(void) {
    fprintf(stderr, "host-probe: stateful multi-server client-API probe\n");

    cmcp_session_t *sess = cmcp_session_new();
    if (!sess) { fprintf(stderr, "session_new failed\n"); return 1; }

    /* ---- 1. Two child servers under one session -------------------- */
    fprintf(stderr, "\n[1] spawn + handshake two servers\n");
    char *echo_argv[] = { (char *)"./examples/echo-server", NULL };
    char *fs_argv[]   = { (char *)"./tools/filesystem-mcp/filesystem-mcp",
                          (char *)"--root", (char *)"/tmp", NULL };
    if (add_server(sess, "echo", echo_argv[0], echo_argv) != CMCP_OK ||
        add_server(sess, "fs",   fs_argv[0],   fs_argv)   != CMCP_OK) {
        cmcp_session_free(sess);
        return 1;
    }
    fprintf(stderr, "  session holds %zu servers\n", cmcp_session_count(sess));

    /* ---- 2. tools/list fanned across both -------------------------- */
    fprintf(stderr, "\n[2] tools/list (parallel fan-out + merge)\n");
    cmcp_session_tool_t *tools = NULL; size_t ntools = 0;
    if (cmcp_session_tools_list(sess, &tools, &ntools) == CMCP_OK) {
        for (size_t i = 0; i < ntools; i++)
            fprintf(stderr, "  %-16s (%s)\n", tools[i].qualified,
                    tools[i].description ? tools[i].description : "");
    }
    cmcp_session_tools_free(tools, ntools);

    /* ---- 3. THREE calls in flight at once, waited in reverse ------- *
     * FRICTION 1 (still open): the session has no async tool_call — only
     * the SYNC cmcp_session_tool_call exists. To get concurrency a host
     * must drop through cmcp_session_get() to the raw clients, re-doing
     * the routing the session already knows. Tracked as F3 (session-layer
     * async on the new handle). */
    fprintf(stderr, "\n[3] three tools/call in flight, waited reverse order\n");
    cmcp_client_t *ce = cmcp_session_get(sess, "echo");
    cmcp_client_t *cf = cmcp_session_get(sess, "fs");

    cmcp_json_t *a_echo = cmcp_json_new_object();
    cmcp_json_object_set(a_echo, "text", cmcp_json_new_string("alpha"));
    cmcp_json_t *a_add = cmcp_json_new_object();
    cmcp_json_object_set(a_add, "a", cmcp_json_new_int(2));
    cmcp_json_object_set(a_add, "b", cmcp_json_new_int(40));
    cmcp_json_t *a_ls = cmcp_json_new_object();   /* fs_list root */

    /* P7 RESOLVED (F4): the async call returns a handle that binds the
     * in-flight id to its client, so a wait can't be mis-routed to the
     * wrong client (per-client id spaces would otherwise collide). */
    cmcp_tool_handle_t h_echo = cmcp_client_tool_call_async(ce, "echo",    a_echo);
    cmcp_tool_handle_t h_add  = cmcp_client_tool_call_async(ce, "add",     a_add);
    cmcp_tool_handle_t h_ls   = cmcp_client_tool_call_async(cf, "fs_list", a_ls);
    fprintf(stderr, "  submitted ids: echo=%lld add=%lld fs_list=%lld\n",
            h_echo.id, h_add.id, h_ls.id);

    /* Wait in the OPPOSITE order — any-order completion. P7 RESOLVED
     * (F2): the result comes back BY VALUE, so the eval-order footgun
     * that silently dropped payloads on the first draft of this probe
     * (reading out-params before the call filled them) is gone by
     * construction — there is no pointer to read early. */
    report_outcome("fs_list", cmcp_client_tool_wait(h_ls));
    report_outcome("add",     cmcp_client_tool_wait(h_add));
    report_outcome("echo",    cmcp_client_tool_wait(h_echo));

    /* ---- 4. Cancel an in-flight call + mandatory reclaim wait ------- *
     * FRICTION 3: every in-tree tool is instant, so the cancel almost
     * always LOSES the race to the response — we cannot deterministically
     * exercise the cancel-honored path without a slow server. We still
     * drive the API and report which side won. The contract requires a
     * wait after cancel to reclaim the completion record regardless. */
    fprintf(stderr, "\n[4] cancel an in-flight call (race vs instant tool)\n");
    cmcp_json_t *a_c = cmcp_json_new_object();
    cmcp_json_object_set(a_c, "text", cmcp_json_new_string("cancel-me"));
    cmcp_tool_handle_t h_c = cmcp_client_tool_call_async(ce, "echo", a_c);
    int crc = cmcp_client_cancel(h_c.client, h_c.id, "probe: testing cancel path");
    fprintf(stderr, "  cancel rc=%d (%s)\n", crc,
            crc == CMCP_OK ? "cancel won the race"
                           : "response beat the cancel (CMCP_EINVAL)");
    /* P7 RESOLVED (F5): a cancelled call now surfaces as
     * CMCP_TOOL_ERR_CANCELLED, not a generic -32603 protocol error. */
    report_outcome("cancel-wait", cmcp_client_tool_wait(h_c));

    /* ---- 5. Two-channel error model -------------------------------- */
    fprintf(stderr, "\n[5] error paths (3-way outcome)\n");
    /* (a) unknown tool */
    cmcp_tool_handle_t h_bad =
        cmcp_client_tool_call_async(ce, "no_such_tool", cmcp_json_new_object());
    report_outcome("unknown-tool", cmcp_client_tool_wait(h_bad));
    /* (b) schema rejection: add requires both a and b; omit b */
    cmcp_json_t *a_bad = cmcp_json_new_object();
    cmcp_json_object_set(a_bad, "a", cmcp_json_new_int(1));
    cmcp_tool_handle_t h_sch = cmcp_client_tool_call_async(ce, "add", a_bad);
    report_outcome("missing-arg", cmcp_client_tool_wait(h_sch));

    fprintf(stderr, "\nhost-probe: done. Freeing session (reaps both children).\n");
    cmcp_session_free(sess);
    return 0;
}
