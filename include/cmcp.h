/**
 * @file cmcp.h
 * @brief Umbrella header: package + protocol version macros, library-
 *        wide error codes, and `cmcp_errstr`.
 *
 * Every cMCP source file should include this header. The error codes
 * declared here are returned by every public function whose return
 * type is `int` (or `cmcp_err_t`) — negative values are errors, zero
 * is success. See `docs/SEMVER.md` for the policy that governs
 * additions and removals in this enum.
 */
#ifndef CMCP_H
#define CMCP_H

/** Package version of this cMCP build (e.g. "0.8.0"). Independent of
 *  `CMCP_PROTOCOL_VERSION` — see docs/SEMVER.md. */
#define CMCP_VERSION       "0.11.0"

/** Legacy (handshake-based) MCP revision this build speaks on the
 *  `initialize` path. The handshake captures whatever the peer
 *  advertises and the host decides whether to continue. */
#define CMCP_PROTOCOL_VERSION "2025-11-25"

/** Modern (stateless, per-request `_meta`) MCP revision this build
 *  serves. cMCP is dual-era: a request carrying
 *  `io.modelcontextprotocol/protocolVersion` in its `_meta` is served
 *  under this revision with no handshake; `initialize` selects
 *  CMCP_PROTOCOL_VERSION. */
#define CMCP_PROTOCOL_VERSION_MODERN "2026-07-28"

/**
 * @brief Library-wide error codes. Negative on failure, zero on success.
 *
 * Every public cMCP function returning `int` returns one of these
 * (negative) on failure. Additions go at the end of the enum (so
 * existing numeric encodings stay stable across MINOR bumps); see
 * `docs/SEMVER.md` for the policy.
 */
typedef enum {
    CMCP_OK             =  0,  /**< Success. */
    CMCP_EINVAL         = -1,  /**< Invalid argument from caller. */
    CMCP_ENOMEM         = -2,  /**< Allocation failure. */
    CMCP_EIO            = -3,  /**< I/O error on transport. */
    CMCP_EPARSE         = -4,  /**< Malformed input (JSON or HTTP). */
    CMCP_ETRANSPORT     = -5,  /**< Transport-layer failure. */
    CMCP_EPROTOCOL      = -6,  /**< Peer violated the MCP spec. */
    CMCP_ETIMEOUT       = -7,  /**< Operation timed out. */
    CMCP_ECANCELLED     = -8,  /**< Operation cancelled (peer or host). */
    CMCP_EUNSUPPORTED   = -9,  /**< Requested feature not implemented. */
    CMCP_ESCHEMA        = -10, /**< Tool input failed schema validation. */
    CMCP_ENOTFOUND      = -11, /**< Named primitive (tool/resource/prompt) not registered. */
    CMCP_EHANDLER       = -12, /**< Handler returned a non-success result. */
    CMCP_EAGAIN         = -13, /**< Capacity/rate cap reached; retry later. */
    CMCP_EINPUT_REQUIRED = -14, /**< Handler needs client input first (MCP
                                 *   2026-07-28 multi round-trip); see
                                 *   cmcp_handler_input. */
} cmcp_err_t;

/**
 * @brief Return a human-readable label for a `cmcp_err_t` value.
 *
 * Unknown values map to "unknown". Returned pointer is to a static
 * string — never NULL, never needs freeing.
 *
 * @param err A value of `cmcp_err_t` (or an `int` carrying one).
 * @return  Borrowed static string.
 */
const char *cmcp_errstr(int err);

#endif
