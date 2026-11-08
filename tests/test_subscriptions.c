/* test_subscriptions — `subscriptions/listen` on stdio (MCP 2026-07-28).
 *
 * A listen request opens a stream of change notifications: the server
 * acknowledges the subset of the filter it can honour, tags every
 * notification with the subscription id, sends nothing a client did not
 * ask for, and answers nothing while the stream is open. On stdio the
 * client ends it with notifications/cancelled.
 *
 * "Nothing was sent" is checked with a sentinel: a modern tools/list
 * whose reply must be the very next frame. */

/* pipe(2) — POSIX. */
#define _POSIX_C_SOURCE 200809L

#include "test.h"
#include "cmcp.h"
#include "cmcp_json.h"
#include "cmcp_server.h"
#include "cmcp_types.h"
#include "wire_rig.h"

#include <stdio.h>
#include <string.h>

#define MODERN "2026-07-28"
#define META "\"_meta\":{"                                              \
    "\"io.modelcontextprotocol/protocolVersion\":\"" MODERN "\","       \
    "\"io.modelcontextprotocol/clientCapabilities\":{}}"

#define LISTEN(id, filter)                                              \
    "{\"jsonrpc\":\"2.0\",\"id\":" #id ",\"method\":\"subscriptions/listen\"," \
    "\"params\":{" META ",\"notifications\":" filter "}}"

#define SUB_ID "io.modelcontextprotocol/subscriptionId"

/* Tools list-changed and resource updates; no prompts list-changed. */
static int setup(cmcp_server_t *srv) {
    cmcp_server_capabilities_t caps = {
        .tools_list_changed = 1,
        .resources_subscribe = 1,
    };
    cmcp_server_set_capabilities(srv, &caps);
    return 0;
}

static long long sub_id_of(const cmcp_json_t *frame) {
    return int_of(get(get(get(frame, "params"), "_meta"), SUB_ID));
}

/* Send a modern tools/list and check its reply is the next frame. */
static int next_is_sentinel(rig_t *r, int id) {
    char frame[256];
    snprintf(frame, sizeof frame,
             "{\"jsonrpc\":\"2.0\",\"id\":%d,\"method\":\"tools/list\","
             "\"params\":{" META "}}", id);
    cmcp_json_t *v = exchange(r, frame);
    int ok = int_of(get(v, "id")) == id && get(v, "result") != NULL;
    cmcp_json_free(v);
    return ok;
}

static void test_listen_acknowledges_supported_subset(void) {
    rig_t r; TEST_ASSERT(rig_start(&r, "subs", "1", setup) == 0);
    cmcp_json_t *ack = exchange(&r, LISTEN(7,
        "{\"toolsListChanged\":true,\"promptsListChanged\":true,"
        "\"resourceSubscriptions\":[\"a://x\"]}"));
    TEST_ASSERT(is_str(get(ack, "method"), "notifications/subscriptions/acknowledged"));
    TEST_ASSERT(sub_id_of(ack) == 7);
    const cmcp_json_t *n = get(get(ack, "params"), "notifications");
    TEST_ASSERT(get(n, "toolsListChanged") != NULL);
    /* The server cannot emit prompts list-changed: dropped from the ack. */
    TEST_ASSERT(get(n, "promptsListChanged") == NULL);
    const cmcp_json_t *rs = get(n, "resourceSubscriptions");
    TEST_ASSERT(rs && cmcp_json_array_len(rs) == 1 &&
                is_str(cmcp_json_array_at(rs, 0), "a://x"));
    cmcp_json_free(ack);
    /* No response to the listen request while it is open. */
    TEST_ASSERT(next_is_sentinel(&r, 100));
    rig_stop(&r);
}

static void test_notifications_carry_subscription_id(void) {
    rig_t r; TEST_ASSERT(rig_start(&r, "subs", "1", setup) == 0);
    cmcp_json_free(exchange(&r, LISTEN(7,
        "{\"toolsListChanged\":true,\"resourceSubscriptions\":[\"a://x\"]}")));

    TEST_ASSERT(cmcp_server_notify_tools_changed(r.srv) == CMCP_OK);
    cmcp_json_t *v = rig_recv(&r);
    TEST_ASSERT(is_str(get(v, "method"), "notifications/tools/list_changed"));
    TEST_ASSERT(sub_id_of(v) == 7);
    cmcp_json_free(v);

    TEST_ASSERT(cmcp_server_notify_resource_updated(r.srv, "a://x") == CMCP_OK);
    v = rig_recv(&r);
    TEST_ASSERT(is_str(get(v, "method"), "notifications/resources/updated"));
    TEST_ASSERT(is_str(get(get(v, "params"), "uri"), "a://x"));
    TEST_ASSERT(sub_id_of(v) == 7);
    cmcp_json_free(v);

    /* Not subscribed, and not supported: nothing goes out. */
    TEST_ASSERT(cmcp_server_notify_resource_updated(r.srv, "a://y") == CMCP_OK);
    TEST_ASSERT(cmcp_server_notify_prompts_changed(r.srv) == CMCP_EPROTOCOL);
    TEST_ASSERT(next_is_sentinel(&r, 101));
    rig_stop(&r);
}

static void test_only_matching_subscriptions_receive(void) {
    rig_t r; TEST_ASSERT(rig_start(&r, "subs", "1", setup) == 0);
    cmcp_json_free(exchange(&r, LISTEN(7, "{\"toolsListChanged\":true}")));
    cmcp_json_free(exchange(&r, LISTEN(8, "{\"resourceSubscriptions\":[\"a://x\"]}")));

    TEST_ASSERT(cmcp_server_notify_tools_changed(r.srv) == CMCP_OK);
    cmcp_json_t *v = rig_recv(&r);
    TEST_ASSERT(sub_id_of(v) == 7);
    cmcp_json_free(v);
    /* Subscription 8 did not ask for tools: the next frame is ours. */
    TEST_ASSERT(next_is_sentinel(&r, 102));
    rig_stop(&r);
}

static void test_cancel_ends_subscription(void) {
    rig_t r; TEST_ASSERT(rig_start(&r, "subs", "1", setup) == 0);
    cmcp_json_free(exchange(&r, LISTEN(7, "{\"toolsListChanged\":true}")));
    TEST_ASSERT(rig_send(&r,
        "{\"jsonrpc\":\"2.0\",\"method\":\"notifications/cancelled\","
        "\"params\":{\"requestId\":7}}") == CMCP_OK);
    /* The loop handles frames in order: once this answers, the cancel
     * has been applied. */
    TEST_ASSERT(next_is_sentinel(&r, 103));
    TEST_ASSERT(cmcp_server_notify_tools_changed(r.srv) == CMCP_OK);
    TEST_ASSERT(next_is_sentinel(&r, 104));
    rig_stop(&r);
}

static void test_bad_filter_is_invalid_params(void) {
    rig_t r; TEST_ASSERT(rig_start(&r, "subs", "1", setup) == 0);
    cmcp_json_t *v = exchange(&r, LISTEN(9, "{\"toolsListChanged\":\"yes\"}"));
    TEST_ASSERT(int_of(get(get(v, "error"), "code")) == CMCP_RPC_INVALID_PARAMS);
    TEST_ASSERT(int_of(get(v, "id")) == 9);
    cmcp_json_free(v);
    v = exchange(&r, LISTEN(10, "{\"resourceSubscriptions\":[1]}"));
    TEST_ASSERT(int_of(get(get(v, "error"), "code")) == CMCP_RPC_INVALID_PARAMS);
    cmcp_json_free(v);
    rig_stop(&r);
}

/* With no listener and no legacy session, nobody asked: nothing sent. */
static void test_no_unsolicited_notifications(void) {
    rig_t r; TEST_ASSERT(rig_start(&r, "subs", "1", setup) == 0);
    TEST_ASSERT(next_is_sentinel(&r, 105));   /* the run loop is live */
    TEST_ASSERT(cmcp_server_notify_tools_changed(r.srv) == CMCP_OK);
    TEST_ASSERT(next_is_sentinel(&r, 106));
    rig_stop(&r);
}

/* A legacy session still gets list-changed, untagged. */
static void test_legacy_session_still_notified(void) {
    rig_t r; TEST_ASSERT(rig_start(&r, "subs", "1", setup) == 0);
    cmcp_json_free(exchange(&r,
        "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"initialize\",\"params\":{"
        "\"protocolVersion\":\"" CMCP_PROTOCOL_VERSION "\",\"capabilities\":{},"
        "\"clientInfo\":{\"name\":\"legacy\",\"version\":\"0\"}}}"));
    TEST_ASSERT(rig_send(&r,
        "{\"jsonrpc\":\"2.0\",\"method\":\"notifications/initialized\"}") == CMCP_OK);
    /* Sync: once this legacy request answers, the session is ready. */
    cmcp_json_free(exchange(&r,
        "{\"jsonrpc\":\"2.0\",\"id\":2,\"method\":\"tools/list\"}"));

    TEST_ASSERT(cmcp_server_notify_tools_changed(r.srv) == CMCP_OK);
    cmcp_json_t *v = rig_recv(&r);
    TEST_ASSERT(is_str(get(v, "method"), "notifications/tools/list_changed"));
    TEST_ASSERT(get(get(v, "params"), "_meta") == NULL);
    cmcp_json_free(v);
    rig_stop(&r);
}

int main(void) {
    TEST_RUN(test_listen_acknowledges_supported_subset);
    TEST_RUN(test_notifications_carry_subscription_id);
    TEST_RUN(test_only_matching_subscriptions_receive);
    TEST_RUN(test_cancel_ends_subscription);
    TEST_RUN(test_bad_filter_is_invalid_params);
    TEST_RUN(test_no_unsolicited_notifications);
    TEST_RUN(test_legacy_session_still_notified);
    TEST_DONE();
}
