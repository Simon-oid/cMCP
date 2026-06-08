/* test_http_ssrf_guard.c — G.2: SSRF egress guard on the HTTP client.
 *
 * cmcp_transport_http_connect refuses to dial cloud-metadata, link-local,
 * and private addresses by default, so a hostile MCP server or a poisoned
 * discovery document can't coax the client into Server-Side Request
 * Forgery (http://169.254.169.254/ for cloud IAM creds, intranet panels,
 * etc.). The check vets the *resolved* peer in libcurl's
 * CURLOPT_OPENSOCKETFUNCTION, so it also defeats DNS rebinding.
 *
 * Two halves:
 *   1) Unit-drive the classifier (cmcp_http_addr_blocked) over a table of
 *      v4/v6 addresses including range boundaries and IPv4-mapped v6 — no
 *      network needed.
 *   2) Integration: a real loopback handshake still succeeds, proving the
 *      guard does NOT break the dominant legitimate case (a local server
 *      you spawned), which is also what the whole HTTP test/soak infra
 *      relies on.
 */

#define _POSIX_C_SOURCE 200809L

#include "test.h"
#include "cmcp.h"
#include "cmcp_json.h"
#include "cmcp_client.h"
#include "cmcp_server.h"
#include "cmcp_transport.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

/* Internal symbol from src/transport_http_client.c — not in any public
 * header, exposed only so this test can drive it without a network. */
extern int cmcp_http_addr_blocked(const struct sockaddr *sa);

static int blocked_v4(const char *ip) {
    struct sockaddr_in sa;
    memset(&sa, 0, sizeof sa);
    sa.sin_family = AF_INET;
    TEST_ASSERT(inet_pton(AF_INET, ip, &sa.sin_addr) == 1);
    return cmcp_http_addr_blocked((struct sockaddr *)&sa);
}

static int blocked_v6(const char *ip) {
    struct sockaddr_in6 sa;
    memset(&sa, 0, sizeof sa);
    sa.sin6_family = AF_INET6;
    TEST_ASSERT(inet_pton(AF_INET6, ip, &sa.sin6_addr) == 1);
    return cmcp_http_addr_blocked((struct sockaddr *)&sa);
}

/* ---- unit: the blocked ranges ---------------------------------------- */

static void test_classifier_blocks_internal_v4(void) {
    /* Cloud metadata + link-local (169.254/16). */
    TEST_ASSERT(blocked_v4("169.254.169.254"));   /* the headline target */
    TEST_ASSERT(blocked_v4("169.254.0.1"));
    TEST_ASSERT(blocked_v4("169.254.255.255"));
    /* RFC1918. */
    TEST_ASSERT(blocked_v4("10.0.0.1"));
    TEST_ASSERT(blocked_v4("10.255.255.255"));
    TEST_ASSERT(blocked_v4("172.16.0.1"));
    TEST_ASSERT(blocked_v4("172.31.255.255"));
    TEST_ASSERT(blocked_v4("192.168.0.1"));
    TEST_ASSERT(blocked_v4("192.168.255.255"));
    /* CGNAT (100.64/10). */
    TEST_ASSERT(blocked_v4("100.64.0.1"));
    TEST_ASSERT(blocked_v4("100.127.255.255"));
}

static void test_classifier_allows_public_and_loopback_v4(void) {
    /* Loopback is intentionally allowed — local server you own. */
    TEST_ASSERT(!blocked_v4("127.0.0.1"));
    TEST_ASSERT(!blocked_v4("127.255.255.255"));
    /* Public. */
    TEST_ASSERT(!blocked_v4("8.8.8.8"));
    TEST_ASSERT(!blocked_v4("1.1.1.1"));
    TEST_ASSERT(!blocked_v4("11.0.0.1"));
    /* Just outside each blocked range (boundary checks). */
    TEST_ASSERT(!blocked_v4("172.15.255.255")); /* below 172.16/12 */
    TEST_ASSERT(!blocked_v4("172.32.0.0"));     /* above 172.16/12 */
    TEST_ASSERT(!blocked_v4("192.167.255.255"));/* below 192.168/16 */
    TEST_ASSERT(!blocked_v4("192.169.0.0"));    /* above 192.168/16 */
    TEST_ASSERT(!blocked_v4("169.253.255.255"));/* below 169.254/16 */
    TEST_ASSERT(!blocked_v4("169.255.0.0"));    /* above 169.254/16 */
    TEST_ASSERT(!blocked_v4("100.63.255.255")); /* below 100.64/10 */
    TEST_ASSERT(!blocked_v4("100.128.0.0"));    /* above 100.64/10 */
}

static void test_classifier_v6(void) {
    /* link-local fe80::/10 (covers fe80–febf). */
    TEST_ASSERT(blocked_v6("fe80::1"));
    TEST_ASSERT(blocked_v6("febf::1"));
    /* ULA fc00::/7 (covers fc00–fdff). */
    TEST_ASSERT(blocked_v6("fc00::1"));
    TEST_ASSERT(blocked_v6("fdff::1"));
    /* IPv4-mapped v6 is unwrapped and re-checked as v4. */
    TEST_ASSERT(blocked_v6("::ffff:169.254.169.254"));
    TEST_ASSERT(blocked_v6("::ffff:10.0.0.1"));
    /* Allowed: loopback, public, mapped-public, deprecated site-local. */
    TEST_ASSERT(!blocked_v6("::1"));
    TEST_ASSERT(!blocked_v6("2001:4860:4860::8888"));
    TEST_ASSERT(!blocked_v6("::ffff:8.8.8.8"));
    TEST_ASSERT(!blocked_v6("fec0::1"));   /* fec0 is not in fe80/10 or fc00/7 */
}

/* ---- integration: loopback still works ------------------------------- */

typedef struct { cmcp_server_t *s; cmcp_transport_t *t; } server_arg_t;

static void *server_thread_main(void *arg) {
    server_arg_t *a = (server_arg_t *)arg;
    cmcp_server_run(a->s, a->t);
    return NULL;
}

static unsigned short pick_port(void) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return 0;
    int yes = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof yes);
    struct sockaddr_in sa;
    memset(&sa, 0, sizeof sa);
    sa.sin_family = AF_INET;
    sa.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    sa.sin_port = 0;
    if (bind(fd, (struct sockaddr *)&sa, sizeof sa) != 0) { close(fd); return 0; }
    socklen_t slen = sizeof sa;
    if (getsockname(fd, (struct sockaddr *)&sa, &slen) != 0) { close(fd); return 0; }
    unsigned short p = ntohs(sa.sin_port);
    close(fd);
    return p;
}

static int echo_handler(const cmcp_json_t *arguments, void *userdata,
                        cmcp_handler_ctx_t *hctx,
                        cmcp_json_t **out_content, int *out_is_error) {
    (void)arguments; (void)userdata; (void)hctx; (void)out_is_error;
    *out_content = cmcp_tool_text_content("pong");
    return CMCP_OK;
}

/* With the guard at its secure default, a handshake + tools/call against a
 * loopback server must succeed. This is the regression that proves the
 * SSRF guard doesn't strangle the common legitimate case. */
static void test_loopback_handshake_succeeds(void) {
    unsigned short port = pick_port();
    TEST_ASSERT(port != 0);

    cmcp_transport_t *st = cmcp_transport_http_listen("127.0.0.1", port);
    TEST_ASSERT(st != NULL);
    cmcp_server_t *s = cmcp_server_new("ssrf-loopback-srv", "0.1.0");
    cmcp_tool_t tool = { .name = "ping", .handler = echo_handler };
    TEST_ASSERT(cmcp_server_add_tool(s, &tool) == CMCP_OK);

    server_arg_t sa = { s, st };
    pthread_t th;
    TEST_ASSERT(pthread_create(&th, NULL, server_thread_main, &sa) == 0);

    char url[64];
    snprintf(url, sizeof url, "http://127.0.0.1:%u/mcp", (unsigned)port);

    cmcp_transport_t *ct = cmcp_transport_http_connect(url);
    TEST_ASSERT(ct != NULL);
    cmcp_client_t *c = cmcp_client_new("ssrf-test-client", "0.0.1");
    TEST_ASSERT(c != NULL);
    TEST_ASSERT(cmcp_client_handshake(c, ct) == CMCP_OK);

    /* One real tool call through the guarded socket path. */
    cmcp_json_t *params = cmcp_json_new_object();
    cmcp_json_object_set(params, "name", cmcp_json_new_string("ping"));
    cmcp_json_object_set(params, "arguments", cmcp_json_new_object());
    cmcp_rpc_message_t resp;
    cmcp_rpc_message_init(&resp);
    TEST_ASSERT(cmcp_client_request(c, "tools/call", params, &resp) == CMCP_OK);
    cmcp_rpc_message_clear(&resp);

    cmcp_client_free(c);
    cmcp_transport_close(ct);

    cmcp_transport_wake(st);
    pthread_join(th, NULL);
    cmcp_transport_close(st);
    cmcp_server_free(s);
}

int main(void) {
    fprintf(stderr, "test_http_ssrf_guard:\n");
    TEST_RUN(test_classifier_blocks_internal_v4);
    TEST_RUN(test_classifier_allows_public_and_loopback_v4);
    TEST_RUN(test_classifier_v6);
    TEST_RUN(test_loopback_handshake_succeeds);
    TEST_DONE();
}
