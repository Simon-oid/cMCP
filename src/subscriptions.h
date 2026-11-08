/* subscriptions.h — internal registry of MCP 2026-07-28 subscriptions.
 *
 * Not a public header: it lives in src/ and is linked into the server.
 *
 * `subscriptions/listen` (spec: basic/patterns/subscriptions) opens a
 * long-lived stream of change notifications. The request carries a
 * filter; the server keeps the subset it can honour, acknowledges it
 * with `notifications/subscriptions/acknowledged`, and from then on tags
 * every notification it sends for that subscription with
 * `_meta["io.modelcontextprotocol/subscriptionId"]` = the request's
 * JSON-RPC id. The request gets no response while the stream is open.
 * It ends when the client cancels it (`notifications/cancelled` on
 * stdio, closing the stream on HTTP).
 *
 * Thread-safe: notifications can be emitted from any thread. */
#ifndef CMCP_SUBSCRIPTIONS_H
#define CMCP_SUBSCRIPTIONS_H

#include "cmcp_json.h"

#include <stddef.h>

#define CMCP_META_SUBSCRIPTION_ID "io.modelcontextprotocol/subscriptionId"

/* Notification kinds a subscription can ask for. Unsigned macros, not
 * an enum: enumerators are signed ints, and these are bit flags. */
#define CMCP_SUB_TOOLS     0x1u   /* notifications/tools/list_changed     */
#define CMCP_SUB_PROMPTS   0x2u   /* notifications/prompts/list_changed   */
#define CMCP_SUB_RESOURCES 0x4u   /* notifications/resources/list_changed */
#define CMCP_SUB_UPDATES   0x8u   /* notifications/resources/updated      */

typedef struct cmcp_subs cmcp_subs_t;

cmcp_subs_t *cmcp_subs_new(void);
void         cmcp_subs_free(cmcp_subs_t *subs);

/* Register the subscription opened by request `id` with `params`
 * (params.notifications is the filter), keeping only the kinds in
 * `supported` (CMCP_SUB_* bits). A second listen with the same id
 * replaces the first. On CMCP_OK, *out_ack is the owned params object of
 * the acknowledgment notification. CMCP_EPROTOCOL for a malformed filter
 * (the caller answers -32602), CMCP_ENOMEM otherwise. */
int cmcp_subs_listen(cmcp_subs_t *subs, const cmcp_json_t *id,
                     const cmcp_json_t *params, unsigned supported,
                     cmcp_json_t **out_ack);

/* End the subscription opened by request `id`. Returns 1 if one ended. */
int cmcp_subs_cancel(cmcp_subs_t *subs, const cmcp_json_t *id);

/* The ids of the subscriptions that asked for `kind` (and, for
 * CMCP_SUB_UPDATES, for `uri`), as an owned JSON array of cloned ids, or
 * NULL when there are none (or on allocation failure). A snapshot: the
 * caller emits without holding the registry lock. */
cmcp_json_t *cmcp_subs_match(cmcp_subs_t *subs, unsigned kind,
                             const char *uri);

/* Notification params tagged for subscription `id`:
 * {"_meta": {subscriptionId: id}, ...extra}. Consumes `extra` (an object
 * or NULL). NULL on allocation failure. */
cmcp_json_t *cmcp_subs_tag(const cmcp_json_t *id, cmcp_json_t *extra);

#endif
