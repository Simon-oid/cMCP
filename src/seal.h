/* seal.h — internal integrity seal for MCP `requestState`.
 *
 * Not a public header: it lives in src/ and is linked into the core.
 *
 * MCP 2026-07-28 (basic/patterns/mrtr) lets a server hand the client an
 * opaque `requestState` string that comes back on the retry. The client
 * is untrusted, so the spec requires the server to protect the state's
 * integrity and recommends binding it to an expiry and to the request
 * it belongs to. A sealed token is
 *
 *     v1.<base64url(payload)>.<base64url(HMAC-SHA256(key, "v1." + payload64))>
 *
 * where payload is the JSON object {"m": method, "n": name, "x": expiry,
 * "s": state}. Opening a token checks the MAC in constant time, then the
 * method, the primitive name and the expiry. The state is signed, not
 * encrypted: the client can read it, so it must not hold secrets. */
#ifndef CMCP_SEAL_H
#define CMCP_SEAL_H

#include <stddef.h>

#define CMCP_SEAL_KEY_LEN   32u      /* recommended key size (bytes)   */
#define CMCP_SEAL_MAX_TOKEN 65536u   /* longest token we try to open   */

/* Seal `state` for a request to `method` on primitive `name` (NULL is
 * treated as ""), valid until unix time `expiry`. On CMCP_OK *out_token
 * is a malloc'd NUL-terminated string the caller frees. */
int cmcp_seal_make(const unsigned char *key, size_t key_len,
                   const char *method, const char *name,
                   const char *state, long long expiry,
                   char **out_token);

/* Verify `token` and recover its state. Returns CMCP_OK with *out_state
 * malloc'd, CMCP_EPROTOCOL when the token is malformed, forged, bound to
 * another method or name, or expired at unix time `now`, CMCP_ENOMEM or
 * CMCP_EINVAL otherwise. *out_state is NULL on any failure. */
int cmcp_seal_open(const unsigned char *key, size_t key_len,
                   const char *token, const char *method, const char *name,
                   long long now, char **out_state);

/* base64url without padding (RFC 4648 §5). Exposed for the tests.
 * encode: returns a malloc'd string or NULL. decode: returns a malloc'd
 * buffer of *out_len bytes (NUL-terminated for convenience) or NULL on
 * bad input. */
char          *cmcp_b64url_encode(const unsigned char *in, size_t len);
unsigned char *cmcp_b64url_decode(const char *in, size_t len, size_t *out_len);

#endif
