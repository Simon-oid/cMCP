/* install-smoke: minimal external consumer of an installed cMCP.
 *
 * Same source compiled twice from this directory:
 *   - via pkg-config + cc                (examples/install-smoke/Makefile)
 *   - via CMake find_package(cmcp)       (examples/install-smoke/CMakeLists.txt)
 *
 * Acts as a regression gate for the make install / .pc /
 * cmcpConfig.cmake plumbing. If either consumer fails to find the
 * headers, link the static lib, or run the program, the install
 * surface broke.
 *
 * The smoke does NOT exercise the wire — it just allocates and frees a
 * client and a server so that core + server + client linkage is real,
 * and prints the cMCP version so an operator running the script can
 * see which build under test produced the output. */

#include <stdio.h>
#include <cmcp.h>
#include <cmcp_server.h>
#include <cmcp_client.h>

int main(void) {
    printf("cMCP version: %s\n", CMCP_VERSION);
    printf("cMCP protocol: %s\n", CMCP_PROTOCOL_VERSION);
    printf("cmcp_errstr(CMCP_OK) = %s\n", cmcp_errstr(CMCP_OK));

    cmcp_server_t *s = cmcp_server_new("install-smoke-server", "0.0.1");
    if (!s) { fprintf(stderr, "cmcp_server_new failed\n"); return 1; }
    cmcp_server_free(s);

    cmcp_client_t *c = cmcp_client_new("install-smoke-client", "0.0.1");
    if (!c) { fprintf(stderr, "cmcp_client_new failed\n"); return 1; }
    cmcp_client_free(c);

    printf("install-smoke: ok\n");
    return 0;
}
