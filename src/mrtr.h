/* mrtr.h — internal Multi Round-Trip Request state for one modern call.
 *
 * Not a public header: it lives in src/ and is linked into the server.
 *
 * MCP 2026-07-28 removes server→client requests. A handler that needs
 * input from the client (elicitation, sampling, roots) instead ends its
 * call with an `InputRequiredResult`; the client gathers the answers and
 * retries the same request with `inputResponses` (and the server's
 * `requestState`, if one was sent). Each round is a fresh, independent
 * request, so the handler simply runs again and finds its answers.
 *
 * One cmcp_mrtr_t lives inside each handler context. server.c fills it
 * from the request (`cmcp_mrtr_begin`), the handler reads and appends to
 * it through the public cmcp_handler_* calls, and server.c turns it into
 * the wire result (`cmcp_mrtr_finish`). */
#ifndef CMCP_MRTR_H
#define CMCP_MRTR_H

#include "cmcp_json.h"
#include "cmcp_types.h"

#include <stddef.h>

/* How long a sealed requestState stays valid. Long enough for a person
 * to answer an elicitation form, short enough to bound replay. */
#define CMCP_MRTR_STATE_TTL_S 600

typedef struct {
    int modern;                         /* 0 on the legacy path         */
    cmcp_client_capabilities_t caps;    /* this request's capabilities  */
    int log_level;                      /* requested floor, -1 = none   */
    const char *method;                 /* borrowed from the request    */
    const char *name;                   /* tool/prompt name or URI      */
    const cmcp_json_t *input_responses; /* borrowed, may be NULL        */
    char *state_in;                     /* verified state, owned        */
    cmcp_json_t *input_requests;        /* owned, built by the handler  */
    char *state_out;                    /* owned, sealed in finish      */
    cmcp_json_t *missing_caps;          /* owned requiredCapabilities   */
    int bad_response;                   /* a malformed inputResponses entry */
} cmcp_mrtr_t;

/* Read this request's capabilities, logLevel, inputResponses and
 * requestState into *m (zeroing it first). The state is verified with
 * the server's key; an invalid one is a protocol error. Returns CMCP_OK,
 * or fills *resp with -32602/-32603 and returns CMCP_EPROTOCOL. */
int cmcp_mrtr_begin(cmcp_mrtr_t *m, const cmcp_rpc_message_t *req,
                    const unsigned char *key, size_t key_len,
                    cmcp_rpc_message_t *resp);

/* Which client capability, if any, `method` (with `params`) needs and
 * `caps` lacks. Returns NULL when it is available, otherwise an owned
 * object in the `requiredCapabilities` shape (e.g. {"elicitation":{}}).
 * Sets *known to 0 for a method that is not an input request at all. */
cmcp_json_t *cmcp_mrtr_missing_cap(const cmcp_client_capabilities_t *caps,
                                   const char *method,
                                   const cmcp_json_t *params, int *known);

/* Modern half of cmcp_handler_input (see cmcp_server.h). Consumes
 * `params`. */
int cmcp_mrtr_input(cmcp_mrtr_t *m, const char *key, const char *method,
                    cmcp_json_t *params, cmcp_json_t **out_result);

/* Record the state to send back with an input_required result. NULL
 * clears it. CMCP_EUNSUPPORTED on the legacy path. */
int cmcp_mrtr_set_state(cmcp_mrtr_t *m, const char *state);

/* Rewrite *resp for a call that asked for input: an InputRequiredResult
 * when the handler queued input requests or state, a -32021 error when
 * it failed for want of a capability, -32602 for a malformed
 * inputResponses entry. Leaves *resp alone otherwise. */
void cmcp_mrtr_finish(cmcp_mrtr_t *m, const cmcp_rpc_message_t *req,
                      const unsigned char *key, size_t key_len,
                      cmcp_rpc_message_t *resp);

/* Free what *m owns. Safe on a zeroed struct. */
void cmcp_mrtr_clear(cmcp_mrtr_t *m);

#endif
