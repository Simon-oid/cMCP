#ifndef CMCP_CLIENT_H
#define CMCP_CLIENT_H

#include "cmcp_types.h"
#include "cmcp_transport.h"

typedef struct cmcp_client cmcp_client_t;

cmcp_client_t *cmcp_client_new(const char *name, const char *version);
void           cmcp_client_free(cmcp_client_t *c);

void cmcp_client_set_capabilities(cmcp_client_t *c,
                                   const cmcp_client_capabilities_t *caps);

/* Drive the initialize → notifications/initialized handshake on a
 * transport. The client borrows the transport for its lifetime; the
 * caller still closes it. Returns CMCP_OK on success; CMCP_EPROTOCOL
 * if the server returns an error or its protocolVersion doesn't
 * match CMCP_PROTOCOL_VERSION. */
int cmcp_client_handshake(cmcp_client_t *c, cmcp_transport_t *t);

/* Synchronous request: emit, then drain frames until the matching
 * response arrives. Server-initiated notifications received during
 * the wait are silently dropped (Phase 1.9 will route them properly).
 *
 * On success out_response is initialised by this call and owns its
 * fields — caller must cmcp_rpc_message_clear() it. */
int cmcp_client_request(cmcp_client_t *c, const char *method,
                        cmcp_json_t *params,
                        cmcp_rpc_message_t *out_response);

/* Fire-and-forget notification. */
int cmcp_client_notify(cmcp_client_t *c, const char *method,
                       cmcp_json_t *params);

/* Server identity + capabilities seen during the handshake. NULL
 * before handshake. */
const cmcp_server_capabilities_t *cmcp_client_server_caps(const cmcp_client_t *c);
const char *cmcp_client_server_name(const cmcp_client_t *c);
const char *cmcp_client_server_version(const cmcp_client_t *c);

#endif
