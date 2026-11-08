/* Conformance — Direction D: the cMCP *client* vs an MCP Python SDK server.
 *
 * The Python SDK (`mcp` 2.x) is dual-era, so its server is the reference
 * witness for cMCP's dual-era client. conformance/py/server_for_cmcp.py
 * exposes echo, add, greet (asks for a name: a server-initiated
 * elicitation on the legacy path, an InputRequiredResult on the modern
 * one) and where (an x-mcp-header argument the SDK checks over HTTP).
 *
 * Every (transport, mode) pair runs the same checks. EXPECTED records the
 * outcome each pair must have against the current build; any difference,
 * better or worse, fails the run, so the table changes only on purpose.
 *
 * Env: CMCP_CONF_PY (python, default conformance/py/.venv/bin/python),
 *      CMCP_CONF_PY_SERVER (default conformance/py/server_for_cmcp.py).
 * Exit 0 when every pair matched its expectation, 1 otherwise. */

#define _POSIX_C_SOURCE 200809L

#include "cmcp.h"
#include "cmcp_client.h"
#include "cmcp_json.h"
#include "cmcp_transport.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <signal.h>
#include <spawn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

extern char **environ;

typedef struct {
    const char        *transport;   /* "stdio" | "http" */
    const char        *label;
    cmcp_client_mode_t mode;
    const char        *protocol;    /* what the client must end up on */
    int                expect_pass;
} pair_t;

static const pair_t EXPECTED[] = {
    {"stdio", "legacy", CMCP_CLIENT_LEGACY, "2025-11-25", 1},
    {"stdio", "auto",   CMCP_CLIENT_AUTO,   "2026-07-28", 1},
    {"stdio", "modern", CMCP_CLIENT_MODERN, "2026-07-28", 1},
    {"http",  "auto",   CMCP_CLIENT_AUTO,   "2026-07-28", 1},
    {"http",  "modern", CMCP_CLIENT_MODERN, "2026-07-28", 1},
};

static const char *py(void) {
    const char *p = getenv("CMCP_CONF_PY");
    return p ? p : "conformance/py/.venv/bin/python";
}

static const char *py_server(void) {
    const char *p = getenv("CMCP_CONF_PY_SERVER");
    return p ? p : "conformance/py/server_for_cmcp.py";
}

/* ---------------------------------------------------------------- */
/* Checks                                                            */
/* ---------------------------------------------------------------- */

static int g_elicits;

static int on_elicit(const cmcp_json_t *params, void *ud, cmcp_json_t **out) {
    (void)params; (void)ud;
    g_elicits++;
    *out = cmcp_elicitation_result("accept",
                                   cmcp_json_parse_cstr("{\"name\":\"Ada\"}"));
    return *out ? CMCP_OK : CMCP_ENOMEM;
}

static int g_failures;

static void check(int ok, const char *what, const char *got) {
    fprintf(stderr, "    %s  %s%s%s\n", ok ? "ok  " : "FAIL", what,
            got ? " -> " : "", got ? got : "");
    if (!ok) g_failures++;
}

/* Call `tool` and compare its text with `want`. */
static void check_call(cmcp_client_t *c, const char *tool, const char *args,
                       const char *want) {
    char *text = NULL;
    cmcp_rpc_error_t *err = NULL;
    int rc = cmcp_client_tool_call_text(c, tool, cmcp_json_parse_cstr(args),
                                        &text, &err);
    char label[96];
    snprintf(label, sizeof label, "%s %s", tool, args);
    check(rc == CMCP_OK && text && strcmp(text, want) == 0, label,
          text ? text : (err && err->message ? err->message : cmcp_errstr(rc)));
    free(text);
    cmcp_rpc_error_free(err);
}

static void run_checks(cmcp_client_t *c, const pair_t *p) {
    const char *proto = cmcp_client_server_protocol(c);
    check(proto && strcmp(proto, p->protocol) == 0, "protocol", proto);

    cmcp_session_tool_t *tools = NULL;
    size_t n = 0;
    int rc = cmcp_client_tools_list(c, &tools, &n);
    char names[256] = "";
    for (size_t i = 0; rc == CMCP_OK && i < n; i++) {
        strncat(names, tools[i].name, sizeof names - strlen(names) - 2);
        strncat(names, " ", sizeof names - strlen(names) - 1);
    }
    check(rc == CMCP_OK && n == 4, "tools/list", names);
    cmcp_session_tools_free(tools, n);

    check_call(c, "echo", "{\"text\":\"cmcp-client\"}", "cmcp-client");
    check_call(c, "add", "{\"a\":2,\"b\":40}", "42");
    g_elicits = 0;
    check_call(c, "greet", "{}", "Hello, Ada!");
    check(g_elicits == 1, "elicitation answered once", NULL);
    /* Over HTTP the SDK rejects the call unless Mcp-Param-Region
     * matches; over stdio the annotation is ignored. */
    check_call(c, "where", "{\"region\":\"us-west1\"}", "region=us-west1");
}

static cmcp_client_t *new_client(cmcp_client_mode_t mode) {
    cmcp_client_t *c = cmcp_client_new("cmcp-conformance", CMCP_VERSION);
    if (!c) return NULL;
    cmcp_client_capabilities_t caps = {.elicitation = 1};
    cmcp_client_set_capabilities(c, &caps);
    cmcp_client_set_elicitation_handler(c, on_elicit, NULL);
    cmcp_client_set_mode(c, mode);
    return c;
}

/* ---------------------------------------------------------------- */
/* Transports                                                        */
/* ---------------------------------------------------------------- */

static void run_stdio(const pair_t *p) {
    cmcp_client_t *c = new_client(p->mode);
    char *const argv[] = {(char *)py(), "-I", (char *)py_server(), NULL};
    int rc = c ? cmcp_client_connect_stdio(c, py(), argv, NULL) : CMCP_ENOMEM;
    check(rc == CMCP_OK, "connect", cmcp_errstr(rc));
    if (rc == CMCP_OK) run_checks(c, p);
    cmcp_client_free(c);
}

static unsigned short free_port(void) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in sa;
    memset(&sa, 0, sizeof sa);
    sa.sin_family = AF_INET;
    sa.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    socklen_t sl = sizeof sa;
    unsigned short port = 0;
    if (fd >= 0 && bind(fd, (struct sockaddr *)&sa, sizeof sa) == 0 &&
        getsockname(fd, (struct sockaddr *)&sa, &sl) == 0)
        port = ntohs(sa.sin_port);
    if (fd >= 0) close(fd);
    return port;
}

/* Wait (up to ~15 s) until something accepts on 127.0.0.1:port. */
static int wait_listening(unsigned short port) {
    for (int i = 0; i < 300; i++) {
        int fd = socket(AF_INET, SOCK_STREAM, 0);
        struct sockaddr_in sa;
        memset(&sa, 0, sizeof sa);
        sa.sin_family = AF_INET;
        sa.sin_port = htons(port);
        sa.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        int ok = fd >= 0 && connect(fd, (struct sockaddr *)&sa, sizeof sa) == 0;
        if (fd >= 0) close(fd);
        if (ok) return 0;
        struct timespec ts = {0, 50 * 1000 * 1000};
        nanosleep(&ts, NULL);
    }
    return -1;
}

static void run_http(const pair_t *p) {
    unsigned short port = free_port();
    char port_s[8], url[64];
    snprintf(port_s, sizeof port_s, "%u", port);
    snprintf(url, sizeof url, "http://127.0.0.1:%u/mcp", port);
    char *const argv[] = {(char *)py(), "-I", (char *)py_server(), "--http",
                          port_s, NULL};
    pid_t pid = 0;
    int spawned = posix_spawn(&pid, py(), NULL, NULL, argv, environ) == 0;
    int rc = spawned && wait_listening(port) == 0 ? CMCP_OK : CMCP_EIO;
    check(rc == CMCP_OK, "server listening", url);

    cmcp_transport_t *t = rc == CMCP_OK ? cmcp_transport_http_connect(url) : NULL;
    cmcp_client_t *c = t ? new_client(p->mode) : NULL;
    if (c) {
        rc = cmcp_client_handshake(c, t);
        check(rc == CMCP_OK, "connect", cmcp_errstr(rc));
        if (rc == CMCP_OK) run_checks(c, p);
    }
    cmcp_client_free(c);
    cmcp_transport_close(t);
    if (spawned) {
        kill(pid, SIGTERM);
        waitpid(pid, NULL, 0);
    }
}

int main(void) {
    fprintf(stderr, "client_vs_py (cMCP client vs Python SDK server):\n");
    int mismatches = 0;
    for (size_t i = 0; i < sizeof EXPECTED / sizeof EXPECTED[0]; i++) {
        const pair_t *p = &EXPECTED[i];
        fprintf(stderr, "  %s / %s\n", p->transport, p->label);
        g_failures = 0;
        if (strcmp(p->transport, "stdio") == 0) run_stdio(p);
        else run_http(p);
        int passed = g_failures == 0;
        int matched = passed == p->expect_pass;
        fprintf(stderr, "  %-5s %-7s %s (expected %s) -> %s\n", p->transport,
                p->label, passed ? "pass" : "fail",
                p->expect_pass ? "pass" : "fail",
                matched ? "as expected" : "UNEXPECTED");
        if (!matched) mismatches++;
    }
    return mismatches ? 1 : 0;
}
