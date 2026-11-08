/* test_mrtr — multi round-trip requests on the 2026-07-28 path.
 *
 * MCP 2026-07-28 replaced server→client requests with MRTR: a handler
 * that needs client input ends the call with an InputRequiredResult and
 * runs again when the client retries with `inputResponses`. These tests
 * drive both rounds over the raw wire, plus the per-request rules that
 * come with it: capabilities and logLevel read from each request's
 * `_meta`, and a `requestState` that is sealed and verified. */

/* pipe(2), strdup — POSIX. */
#define _POSIX_C_SOURCE 200809L

#include "test.h"
#include "cmcp.h"
#include "cmcp_json.h"
#include "cmcp_server.h"
#include "cmcp_types.h"
#include "wire_rig.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MODERN "2026-07-28"

/* Modern _meta with the given clientCapabilities JSON and extra keys. */
#define META(caps, extra)                                            \
    "\"_meta\":{"                                                    \
    "\"io.modelcontextprotocol/protocolVersion\":\"" MODERN "\","    \
    "\"io.modelcontextprotocol/clientCapabilities\":" caps           \
    extra "}"

#define CAPS_ELICIT "{\"elicitation\":{}}"
#define CAPS_NONE   "{}"

/* The most recently started server, for handlers that need it. */
static cmcp_server_t *g_srv;

/* ====================================================================== */
/* Tools under test                                                        */
/* ====================================================================== */

static cmcp_json_t *name_form(void) {
    return cmcp_json_parse_cstr(
        "{\"message\":\"Your name?\",\"requestedSchema\":{\"type\":\"object\","
        "\"properties\":{\"name\":{\"type\":\"string\"}},"
        "\"required\":[\"name\"]}}");
}

/* Asks for a name. Round one also leaves a state behind, which round
 * two reports back so the test can see it survived the seal. */
static int ask_name(const cmcp_json_t *args, void *ud, cmcp_handler_ctx_t *hctx,
                    cmcp_json_t **out, int *is_error) {
    (void)args; (void)ud;
    cmcp_json_t *ans = NULL;
    int rc = cmcp_handler_input(hctx, "name", "elicitation/create",
                                name_form(), &ans);
    if (rc == CMCP_EINPUT_REQUIRED) {
        cmcp_handler_set_request_state(hctx, "round=1");
        return rc;
    }
    if (rc != CMCP_OK) return rc;

    const cmcp_json_t *nm = cmcp_json_object_get(
        cmcp_json_object_get(ans, "content"), "name");
    const char *st = cmcp_handler_request_state(hctx);
    char text[128];
    snprintf(text, sizeof text, "hello %s state=%s",
             nm && nm->type == CMCP_JSON_STRING ? cmcp_json_string(nm) : "?",
             st ? st : "none");
    cmcp_json_free(ans);
    *out = cmcp_tool_text_content(text);
    *is_error = 0;
    return CMCP_OK;
}

/* "Request state only" InputRequiredResult (spec: load shedding): come
 * back later, with no input requests at all. */
static int shed(const cmcp_json_t *args, void *ud, cmcp_handler_ctx_t *hctx,
                cmcp_json_t **out, int *is_error) {
    (void)args; (void)ud;
    if (!cmcp_handler_request_state(hctx)) {
        cmcp_handler_set_request_state(hctx, "queued");
        return CMCP_EINPUT_REQUIRED;
    }
    *out = cmcp_tool_text_content("done");
    *is_error = 0;
    return CMCP_OK;
}

/* Reports what it sees: the per-request sampling capability, and
 * whether the blocking legacy calls are refused. */
static int probe(const cmcp_json_t *args, void *ud, cmcp_handler_ctx_t *hctx,
                 cmcp_json_t **out, int *is_error) {
    (void)args; (void)ud;
    const cmcp_client_capabilities_t *c = cmcp_handler_client_caps(hctx);
    cmcp_json_t *res = NULL;
    int erc = cmcp_handler_elicit(hctx, "blocked?", NULL, &res);
    cmcp_json_free(res);
    cmcp_rpc_message_t m;
    int src = cmcp_server_send_request(g_srv, hctx, "roots/list", NULL, &m);
    if (src == CMCP_OK) cmcp_rpc_message_clear(&m);
    char text[128];
    snprintf(text, sizeof text, "modern=%d sampling=%d elicit=%d send=%d",
             cmcp_handler_is_modern(hctx), c ? c->sampling : -1,
             erc == CMCP_EUNSUPPORTED, src == CMCP_EUNSUPPORTED);
    *out = cmcp_tool_text_content(text);
    *is_error = 0;
    return CMCP_OK;
}

/* Logs one info line through the per-request channel. */
static int chatty(const cmcp_json_t *args, void *ud, cmcp_handler_ctx_t *hctx,
                  cmcp_json_t **out, int *is_error) {
    (void)args; (void)ud;
    cmcp_handler_log(hctx, CMCP_LOG_LEVEL_INFO, "chatty",
                     cmcp_json_parse_cstr("{\"msg\":\"working\"}"));
    *out = cmcp_tool_text_content("ok");
    *is_error = 0;
    return CMCP_OK;
}

#define NO_ARGS "{\"type\":\"object\"}"

static int add_tools(cmcp_server_t *srv) {
    g_srv = srv;
    cmcp_server_capabilities_t caps = {.logging = 1};
    cmcp_server_set_capabilities(srv, &caps);
    const cmcp_tool_t tools[] = {
        {.name = "ask_name", .description = "d", .input_schema = NO_ARGS,
         .handler = ask_name},
        {.name = "shed", .description = "d", .input_schema = NO_ARGS,
         .handler = shed},
        {.name = "probe", .description = "d", .input_schema = NO_ARGS,
         .handler = probe},
        {.name = "chatty", .description = "d", .input_schema = NO_ARGS,
         .handler = chatty},
    };
    for (size_t i = 0; i < sizeof tools / sizeof tools[0]; i++)
        if (cmcp_server_add_tool(srv, &tools[i]) != CMCP_OK) return -1;
    return 0;
}

/* ====================================================================== */
/* Helpers                                                                 */
/* ====================================================================== */

static const char *first_text(const cmcp_json_t *result) {
    const cmcp_json_t *c = cmcp_json_array_at(get(result, "content"), 0);
    const cmcp_json_t *t = get(c, "text");
    return (t && t->type == CMCP_JSON_STRING) ? cmcp_json_string(t) : "";
}

static long long error_code(const cmcp_json_t *v) {
    return int_of(get(get(v, "error"), "code"));
}

static char *dup_str(const cmcp_json_t *v) {
    return (v && v->type == CMCP_JSON_STRING) ? strdup(cmcp_json_string(v))
                                              : NULL;
}

/* Round one of ask_name. Returns the sealed requestState (owned). */
static char *ask_name_round_one(rig_t *r) {
    cmcp_json_t *v = exchange(r,
        "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"tools/call\",\"params\":{"
        "\"name\":\"ask_name\",\"arguments\":{}," META(CAPS_ELICIT, "") "}}");
    char *tok = dup_str(get(get(v, "result"), "requestState"));
    cmcp_json_free(v);
    return tok;
}

/* Round two of ask_name answering with `who`, sent to rig `r`. */
static cmcp_json_t *ask_name_round_two(rig_t *r, const char *tok,
                                       const char *who) {
    char frame[2048];
    snprintf(frame, sizeof frame,
        "{\"jsonrpc\":\"2.0\",\"id\":2,\"method\":\"tools/call\",\"params\":{"
        "\"name\":\"ask_name\",\"arguments\":{},"
        "\"inputResponses\":{\"name\":{\"action\":\"accept\","
        "\"content\":{\"name\":\"%s\"}}},"
        "\"requestState\":\"%s\"," META(CAPS_ELICIT, "") "}}",
        who, tok ? tok : "");
    return exchange(r, frame);
}

/* ====================================================================== */
/* Tests                                                                   */
/* ====================================================================== */

/* Round one: no answer yet, so the call ends with input_required. */
static void test_first_round_asks_for_input(void) {
    rig_t r; TEST_ASSERT(rig_start(&r, "mrtr", "1", add_tools) == 0);
    cmcp_json_t *v = exchange(&r,
        "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"tools/call\",\"params\":{"
        "\"name\":\"ask_name\",\"arguments\":{}," META(CAPS_ELICIT, "") "}}");
    const cmcp_json_t *res = get(v, "result");
    TEST_ASSERT(is_str(get(res, "resultType"), "input_required"));
    const cmcp_json_t *req = get(get(res, "inputRequests"), "name");
    TEST_ASSERT(is_str(get(req, "method"), "elicitation/create"));
    TEST_ASSERT(is_str(get(get(req, "params"), "message"), "Your name?"));
    const cmcp_json_t *st = get(res, "requestState");
    TEST_ASSERT(st && st->type == CMCP_JSON_STRING &&
                strncmp(cmcp_json_string(st), "v1.", 3) == 0);
    /* Interim results are not cacheable and carry no hints. */
    TEST_ASSERT(get(res, "ttlMs") == NULL);
    TEST_ASSERT(get(res, "content") == NULL);
    cmcp_json_free(v);
    rig_stop(&r);
}

/* Round two: the retry carries the answer and the state; the handler
 * runs again, finds both, and completes. */
static void test_retry_completes_with_answer_and_state(void) {
    rig_t r; TEST_ASSERT(rig_start(&r, "mrtr", "1", add_tools) == 0);
    char *tok = ask_name_round_one(&r);
    TEST_ASSERT(tok != NULL);
    cmcp_json_t *v = ask_name_round_two(&r, tok, "Ada");
    const cmcp_json_t *res = get(v, "result");
    TEST_ASSERT(is_str(get(res, "resultType"), "complete"));
    TEST_ASSERT(strcmp(first_text(res), "hello Ada state=round=1") == 0);
    cmcp_json_free(v);
    free(tok);
    rig_stop(&r);
}

/* A modified requestState is attacker input: rejected before the
 * handler ever sees it. */
static void test_tampered_state_is_rejected(void) {
    rig_t r; TEST_ASSERT(rig_start(&r, "mrtr", "1", add_tools) == 0);
    char *tok = ask_name_round_one(&r);
    TEST_ASSERT(tok != NULL);
    if (tok) tok[4] = (char)(tok[4] == 'A' ? 'B' : 'A');
    cmcp_json_t *v = ask_name_round_two(&r, tok, "Mallory");
    TEST_ASSERT(error_code(v) == CMCP_RPC_INVALID_PARAMS);
    cmcp_json_free(v);
    free(tok);
    rig_stop(&r);
}

/* A genuine state minted for one tool cannot be replayed on another. */
static void test_state_is_bound_to_its_request(void) {
    rig_t r; TEST_ASSERT(rig_start(&r, "mrtr", "1", add_tools) == 0);
    char *tok = ask_name_round_one(&r);
    TEST_ASSERT(tok != NULL);

    char frame[2048];
    snprintf(frame, sizeof frame,
        "{\"jsonrpc\":\"2.0\",\"id\":4,\"method\":\"tools/call\",\"params\":{"
        "\"name\":\"shed\",\"arguments\":{},\"requestState\":\"%s\","
        META(CAPS_NONE, "") "}}", tok ? tok : "");
    cmcp_json_t *v = exchange(&r, frame);
    TEST_ASSERT(error_code(v) == CMCP_RPC_INVALID_PARAMS);
    cmcp_json_free(v);
    free(tok);
    rig_stop(&r);
}

/* A state the server did not issue at all is rejected the same way. */
static void test_unsealed_state_is_rejected(void) {
    rig_t r; TEST_ASSERT(rig_start(&r, "mrtr", "1", add_tools) == 0);
    cmcp_json_t *v = exchange(&r,
        "{\"jsonrpc\":\"2.0\",\"id\":5,\"method\":\"tools/call\",\"params\":{"
        "\"name\":\"shed\",\"arguments\":{},\"requestState\":\"queued\","
        META(CAPS_NONE, "") "}}");
    TEST_ASSERT(error_code(v) == CMCP_RPC_INVALID_PARAMS);
    cmcp_json_free(v);
    rig_stop(&r);
}

/* The client did not declare elicitation: the server must not send the
 * request, and the failed call maps to -32021 naming the capability. */
static void test_missing_capability_is_32021(void) {
    rig_t r; TEST_ASSERT(rig_start(&r, "mrtr", "1", add_tools) == 0);
    cmcp_json_t *v = exchange(&r,
        "{\"jsonrpc\":\"2.0\",\"id\":6,\"method\":\"tools/call\",\"params\":{"
        "\"name\":\"ask_name\",\"arguments\":{}," META(CAPS_NONE, "") "}}");
    TEST_ASSERT(error_code(v) == CMCP_RPC_MISSING_CLIENT_CAPABILITY);
    const cmcp_json_t *need =
        get(get(get(v, "error"), "data"), "requiredCapabilities");
    TEST_ASSERT(get(need, "elicitation") != NULL);
    cmcp_json_free(v);
    rig_stop(&r);
}

/* Malformed inputResponses: the container, then one entry. */
static void test_malformed_input_responses(void) {
    rig_t r; TEST_ASSERT(rig_start(&r, "mrtr", "1", add_tools) == 0);
    cmcp_json_t *v = exchange(&r,
        "{\"jsonrpc\":\"2.0\",\"id\":7,\"method\":\"tools/call\",\"params\":{"
        "\"name\":\"ask_name\",\"arguments\":{},\"inputResponses\":[],"
        META(CAPS_ELICIT, "") "}}");
    TEST_ASSERT(error_code(v) == CMCP_RPC_INVALID_PARAMS);
    cmcp_json_free(v);

    v = exchange(&r,
        "{\"jsonrpc\":\"2.0\",\"id\":8,\"method\":\"tools/call\",\"params\":{"
        "\"name\":\"ask_name\",\"arguments\":{},"
        "\"inputResponses\":{\"name\":\"Ada\"}," META(CAPS_ELICIT, "") "}}");
    TEST_ASSERT(error_code(v) == CMCP_RPC_INVALID_PARAMS);
    cmcp_json_free(v);
    rig_stop(&r);
}

/* State only, no input requests: the client may retry immediately. */
static void test_state_only_round_trip(void) {
    rig_t r; TEST_ASSERT(rig_start(&r, "mrtr", "1", add_tools) == 0);
    cmcp_json_t *v = exchange(&r,
        "{\"jsonrpc\":\"2.0\",\"id\":9,\"method\":\"tools/call\",\"params\":{"
        "\"name\":\"shed\",\"arguments\":{}," META(CAPS_NONE, "") "}}");
    const cmcp_json_t *res = get(v, "result");
    TEST_ASSERT(is_str(get(res, "resultType"), "input_required"));
    TEST_ASSERT(get(res, "inputRequests") == NULL);
    char *tok = dup_str(get(res, "requestState"));
    TEST_ASSERT(tok != NULL);
    cmcp_json_free(v);

    char frame[2048];
    snprintf(frame, sizeof frame,
        "{\"jsonrpc\":\"2.0\",\"id\":10,\"method\":\"tools/call\",\"params\":{"
        "\"name\":\"shed\",\"arguments\":{},\"requestState\":\"%s\","
        META(CAPS_NONE, "") "}}", tok ? tok : "");
    v = exchange(&r, frame);
    TEST_ASSERT(is_str(get(get(v, "result"), "resultType"), "complete"));
    TEST_ASSERT(strcmp(first_text(get(v, "result")), "done") == 0);
    cmcp_json_free(v);
    free(tok);
    rig_stop(&r);
}

/* Capabilities come from each request alone (the spec forbids inferring
 * them from earlier requests), and the blocking server→client calls are
 * refused on the modern path. */
static void test_capabilities_are_per_request(void) {
    rig_t r; TEST_ASSERT(rig_start(&r, "mrtr", "1", add_tools) == 0);
    cmcp_json_t *v = exchange(&r,
        "{\"jsonrpc\":\"2.0\",\"id\":11,\"method\":\"tools/call\",\"params\":{"
        "\"name\":\"probe\",\"arguments\":{},"
        META("{\"sampling\":{},\"elicitation\":{}}", "") "}}");
    TEST_ASSERT(strcmp(first_text(get(v, "result")),
                       "modern=1 sampling=1 elicit=1 send=1") == 0);
    cmcp_json_free(v);

    v = exchange(&r,
        "{\"jsonrpc\":\"2.0\",\"id\":12,\"method\":\"tools/call\",\"params\":{"
        "\"name\":\"probe\",\"arguments\":{}," META(CAPS_NONE, "") "}}");
    TEST_ASSERT(strcmp(first_text(get(v, "result")),
                       "modern=1 sampling=0 elicit=1 send=1") == 0);
    cmcp_json_free(v);
    rig_stop(&r);
}

/* Logging: only with a logLevel in _meta, and only at or above it. */
static void test_log_level_is_per_request(void) {
    rig_t r; TEST_ASSERT(rig_start(&r, "mrtr", "1", add_tools) == 0);

    /* No logLevel: the very next frame is the response. */
    cmcp_json_t *v = exchange(&r,
        "{\"jsonrpc\":\"2.0\",\"id\":13,\"method\":\"tools/call\",\"params\":{"
        "\"name\":\"chatty\",\"arguments\":{}," META(CAPS_NONE, "") "}}");
    TEST_ASSERT(int_of(get(v, "id")) == 13);
    cmcp_json_free(v);

    /* Above the handler's level: still silent. */
    v = exchange(&r,
        "{\"jsonrpc\":\"2.0\",\"id\":14,\"method\":\"tools/call\",\"params\":{"
        "\"name\":\"chatty\",\"arguments\":{},"
        META(CAPS_NONE, ",\"io.modelcontextprotocol/logLevel\":\"error\"") "}}");
    TEST_ASSERT(int_of(get(v, "id")) == 14);
    cmcp_json_free(v);

    /* Opted in at debug: the log line comes first, then the response. */
    v = exchange(&r,
        "{\"jsonrpc\":\"2.0\",\"id\":15,\"method\":\"tools/call\",\"params\":{"
        "\"name\":\"chatty\",\"arguments\":{},"
        META(CAPS_NONE, ",\"io.modelcontextprotocol/logLevel\":\"debug\"") "}}");
    TEST_ASSERT(is_str(get(v, "method"), "notifications/message"));
    TEST_ASSERT(is_str(get(get(v, "params"), "level"), "info"));
    cmcp_json_free(v);
    v = rig_recv(&r);
    TEST_ASSERT(int_of(get(v, "id")) == 15);
    cmcp_json_free(v);

    /* A logLevel that is not a level name is a protocol error. */
    v = exchange(&r,
        "{\"jsonrpc\":\"2.0\",\"id\":16,\"method\":\"tools/call\",\"params\":{"
        "\"name\":\"chatty\",\"arguments\":{},"
        META(CAPS_NONE, ",\"io.modelcontextprotocol/logLevel\":\"loud\"") "}}");
    TEST_ASSERT(error_code(v) == CMCP_RPC_INVALID_PARAMS);
    cmcp_json_free(v);
    rig_stop(&r);
}

/* The same ask_name handler on the legacy path: cmcp_handler_input
 * sends a real elicitation/create and blocks for the answer. */
static void test_same_handler_on_legacy_path(void) {
    rig_t r; TEST_ASSERT(rig_start(&r, "mrtr", "1", add_tools) == 0);
    cmcp_json_t *v = exchange(&r,
        "{\"jsonrpc\":\"2.0\",\"id\":20,\"method\":\"initialize\","
        "\"params\":{\"protocolVersion\":\"" CMCP_PROTOCOL_VERSION "\","
        "\"capabilities\":{\"elicitation\":{}},"
        "\"clientInfo\":{\"name\":\"legacy\",\"version\":\"0\"}}}");
    cmcp_json_free(v);
    TEST_ASSERT(rig_send(&r,
        "{\"jsonrpc\":\"2.0\",\"method\":\"notifications/initialized\"}") == CMCP_OK);

    /* The server's own request arrives before our call's response. */
    v = exchange(&r,
        "{\"jsonrpc\":\"2.0\",\"id\":21,\"method\":\"tools/call\","
        "\"params\":{\"name\":\"ask_name\",\"arguments\":{}}}");
    TEST_ASSERT(is_str(get(v, "method"), "elicitation/create"));
    long long sid = int_of(get(v, "id"));
    cmcp_json_free(v);

    char frame[256];
    snprintf(frame, sizeof frame,
        "{\"jsonrpc\":\"2.0\",\"id\":%lld,\"result\":{\"action\":\"accept\","
        "\"content\":{\"name\":\"Grace\"}}}", sid);
    v = exchange(&r, frame);
    const cmcp_json_t *res = get(v, "result");
    TEST_ASSERT(int_of(get(v, "id")) == 21);
    TEST_ASSERT(strcmp(first_text(res), "hello Grace state=none") == 0);
    TEST_ASSERT(get(res, "resultType") == NULL);
    cmcp_json_free(v);
    rig_stop(&r);
}

/* Server-wide logging stays silent until a legacy session opted in. */
static void test_server_log_needs_a_legacy_session(void) {
    rig_t r; TEST_ASSERT(rig_start(&r, "mrtr", "1", add_tools) == 0);
    /* Give the run loop a request first, so the transport is live. */
    cmcp_json_t *v = exchange(&r,
        "{\"jsonrpc\":\"2.0\",\"id\":30,\"method\":\"tools/list\","
        "\"params\":{" META(CAPS_NONE, "") "}}");
    cmcp_json_free(v);
    TEST_ASSERT(cmcp_server_log(r.srv, CMCP_LOG_LEVEL_ERROR, "bg",
                                cmcp_json_parse_cstr("{}")) == CMCP_OK);
    /* Nothing was written: the next frame is the reply to this request. */
    v = exchange(&r,
        "{\"jsonrpc\":\"2.0\",\"id\":31,\"method\":\"tools/list\","
        "\"params\":{" META(CAPS_NONE, "") "}}");
    TEST_ASSERT(int_of(get(v, "id")) == 31);
    cmcp_json_free(v);
    rig_stop(&r);
}

/* A fixed key lets a second server instance accept the first one's
 * state (the multi-instance case); only 32-byte keys are accepted. The
 * key is configuration, set before the server runs. */
static const unsigned char SHARED_KEY[32] = "shared-key-for-all-instances-01";

static int add_tools_shared_key(cmcp_server_t *srv) {
    if (cmcp_server_set_state_key(srv, SHARED_KEY, 31) != CMCP_EINVAL) return -1;
    if (cmcp_server_set_state_key(srv, SHARED_KEY, 32) != CMCP_OK) return -1;
    return add_tools(srv);
}

static void test_shared_state_key_across_instances(void) {
    rig_t a; TEST_ASSERT(rig_start(&a, "mrtr", "1", add_tools_shared_key) == 0);
    rig_t b; TEST_ASSERT(rig_start(&b, "mrtr", "1", add_tools_shared_key) == 0);

    char *tok = ask_name_round_one(&a);
    TEST_ASSERT(tok != NULL);
    cmcp_json_t *v = ask_name_round_two(&b, tok, "Lin");
    TEST_ASSERT(strcmp(first_text(get(v, "result")),
                       "hello Lin state=round=1") == 0);
    cmcp_json_free(v);
    free(tok);
    rig_stop(&a);
    rig_stop(&b);
}

/* Without the shared key, another instance refuses the state. */
static void test_random_keys_differ_across_instances(void) {
    rig_t a; TEST_ASSERT(rig_start(&a, "mrtr", "1", add_tools) == 0);
    rig_t b; TEST_ASSERT(rig_start(&b, "mrtr", "1", add_tools) == 0);
    char *tok = ask_name_round_one(&a);
    TEST_ASSERT(tok != NULL);
    cmcp_json_t *v = ask_name_round_two(&b, tok, "Lin");
    TEST_ASSERT(error_code(v) == CMCP_RPC_INVALID_PARAMS);
    cmcp_json_free(v);
    free(tok);
    rig_stop(&a);
    rig_stop(&b);
}

int main(void) {
    TEST_RUN(test_first_round_asks_for_input);
    TEST_RUN(test_retry_completes_with_answer_and_state);
    TEST_RUN(test_tampered_state_is_rejected);
    TEST_RUN(test_state_is_bound_to_its_request);
    TEST_RUN(test_unsealed_state_is_rejected);
    TEST_RUN(test_missing_capability_is_32021);
    TEST_RUN(test_malformed_input_responses);
    TEST_RUN(test_state_only_round_trip);
    TEST_RUN(test_capabilities_are_per_request);
    TEST_RUN(test_log_level_is_per_request);
    TEST_RUN(test_same_handler_on_legacy_path);
    TEST_RUN(test_server_log_needs_a_legacy_session);
    TEST_RUN(test_shared_state_key_across_instances);
    TEST_RUN(test_random_keys_differ_across_instances);
    TEST_DONE();
}
