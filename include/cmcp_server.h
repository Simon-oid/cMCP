#ifndef CMCP_SERVER_H
#define CMCP_SERVER_H

#include "cmcp_types.h"
#include "cmcp_transport.h"

typedef struct cmcp_server cmcp_server_t;

/* Allocate a server. name/version are copied. */
cmcp_server_t *cmcp_server_new(const char *name, const char *version);
void           cmcp_server_free(cmcp_server_t *s);

/* Declare what this server can do. Caller's struct is copied. If never
 * called, defaults are all-zero (no optional capabilities). */
void cmcp_server_set_capabilities(cmcp_server_t *s,
                                   const cmcp_server_capabilities_t *caps);

/* Drive the server on a transport until the transport closes.
 *
 * The server reads frames in a loop, parses them as JSON-RPC, and
 * dispatches them. The handshake (`initialize` request, `initialized`
 * notification) is built in. Other request methods get -32601 until
 * Phase 1.5 wires a tool registry.
 *
 * Does NOT take ownership of the transport — caller closes it after
 * cmcp_server_run() returns. Returns CMCP_OK on clean shutdown
 * (transport closed), or a negative cmcp_err_t on misuse. */
int cmcp_server_run(cmcp_server_t *s, cmcp_transport_t *t);

/* Negotiated client capabilities and identity, valid after the
 * handshake completes. Pointers returned are owned by the server. */
const cmcp_client_capabilities_t *cmcp_server_client_caps(const cmcp_server_t *s);
const char *cmcp_server_client_name(const cmcp_server_t *s);
const char *cmcp_server_client_version(const cmcp_server_t *s);

#endif
