# cMCP threat model

This document is the **threat-model contract** for cMCP. It enumerates
the trust boundaries inside the library, the threats at each boundary,
the mitigations cMCP has (and does not have) today, and what is
out-of-scope.

cMCP is a *protocol library*. The threat model assumes:

- A host process (e.g. butlerbot) links cMCP and uses it to drive one
  or more MCP servers, or accepts MCP requests from one or more
  clients.
- The host is trusted (cMCP runs inside the host's address space —
  no privilege boundary to defend across).
- An MCP **peer** (server seen by a client, or client seen by a
  server) may be hostile or buggy. The peer's input is untrusted.
- The deployment may put cMCP behind a reverse proxy (nginx / caddy);
  TLS termination always happens at the proxy, never inside cMCP
  (see `docs/deployment-tls.md`).

STRIDE categories used below: **S**poofing, **T**ampering,
**R**epudiation, **I**nformation disclosure, **D**enial of service,
**E**levation of privilege.

## Trust boundaries

```
                      hostile network
                              │
                     ┌────────┴────────┐
                     │  reverse proxy   │  (TLS, allow-list)
                     │  (out of scope)  │
                     └────────┬────────┘
                              │
        ╔═════════════════════╪═════════════════════╗
        ║       peer ↔ transport (server side)      ║   B1
        ╠════════════════════════════════════════════╣
        ║              transport ↔ rpc               ║   B2
        ╠════════════════════════════════════════════╣
        ║          rpc ↔ handler (worker pool)        ║  B3
        ╠════════════════════════════════════════════╣
        ║              handler ↔ host                ║   B4
        ╚════════════════════════════════════════════╝
                              │
                       remote MCP server
                              │
        ╔════════════════════╪═════════════════════╗
        ║       peer ↔ transport (client side)      ║   B5
        ╚════════════════════════════════════════════╝
```

The five boundaries are addressed individually below. Mitigations
flagged 🟢 are present; 🟡 partial; ◻️ deferred to a follow-up
commit; ⛔ explicit non-goal.

---

## B1: peer ↔ transport (server side)

**Assets:** the host's CPU, memory, file descriptors, log buffer, and
session integrity.

| # | Threat | Cat. | Mitigation |
|---|--------|------|------------|
| 1.1 | Malformed JSON-RPC body crashes the parser | S/T | 🟢 Hand-rolled JSON parser fuzzed via libFuzzer (`fuzz/fuzz_json`, `fuzz/fuzz_rpc`); ~32M execs/min, zero findings on the seed corpus. |
| 1.2 | Oversized request body exhausts memory | D | 🟢 `HTTP_MAX_BODY_BYTES` (4 MiB) caps the body read; oversized requests get 413. |
| 1.3 | Oversized header block exhausts memory | D | 🟢 `HTTP_MAX_HEADERS_BYTES` (16 KiB) caps the header read. |
| 1.4 | Slow-write peer (slowloris) holds connections open | D | 🟢 6.5.1: per-connection idle read timeout (`CMCP_HTTP_IDLE_TIMEOUT_MS`, default 30s); per-connection total deadline (`CMCP_HTTP_DEADLINE_MS`, default 120s). |
| 1.5 | Connection flood saturates accept queue / fds | D | 🟢 6.5.2: token-bucket accept-rate gate in the acceptor (`CMCP_HTTP_ACCEPT_RATE`, default 100/s; `CMCP_HTTP_ACCEPT_BURST`, default 200). Rate <= 0 disables. Over-budget → 503 + `Retry-After`; budget refills monotonically. The transport's single-acceptor model already serialises POST handling, so the rate gate is the primary structural defense at this boundary. |
| 1.6 | DNS-rebinding attack on a localhost-bound server | S | 🟢 `Origin` allow-list (`CMCP_HTTP_ALLOWED_ORIGINS`, MCP 2025-11-25 Minor 3); mismatched `Origin` → 403. Defense-in-depth: `cmcp_transport_http_listen` defaults to a loopback bind (127.0.0.1) when `host` is NULL/empty, so the unconfigured server is unreachable off-box; an explicit non-loopback bind with no allow-list set emits a one-shot stderr warning. |
| 1.7 | Cross-version handshake confusion | S/T | 🟢 The server echoes its own `protocolVersion` and lets the client decide; only missing/malformed → `-32602` (matches the spec's "client decides whether to disconnect" rule). |
| 1.8 | Hostile peer holds an SSE GET stream open | D | 🟡 SSE replay buffer is per-session bounded (`CMCP_HTTP_SSE_REPLAY_BUFFER`, default 256, capped 65536). Idle-timeout on the read side from 1.4 covers the request setup; the long-lived SSE write side closes when the session closes. |
| 1.9 | Capability advertisement spoofing | S | ⛔ The peer's advertised capabilities are *informational input*, not authority. Server-side decisions are made against the registry, not the peer's claim. |
| 1.10 | Untrusted `inputSchema` triggers regex DoS | D | 🟡 6.7: regex uses POSIX ERE (`REG_EXTENDED | REG_NOSUB`). POSIX ERE has predictable complexity for ASCII patterns; no backtracking-disaster path comparable to ECMAScript regex with nested quantifiers. Compiled patterns are cached (see 3.6) so a hot tool does not recompile per call. Pathological POSIX patterns are still possible — bounded by 1.4's idle timeout. |
| 1.11 | Header injection via CRLF in returned values | T | 🟢 `http_write_response` writes only fixed status, content-type, and length; no caller-controlled headers. SSE event framing only emits `data:`/`id:` and escapes nothing dangerous. |
| 1.12 | `Mcp-Session-Id` guess / forge | S | 🟢 Session ids are 128-bit-equivalent (UUIDv4 minted from `/dev/urandom`) and compared by exact string match. |
| 1.13 | Malformed POST body deadlocks the request/response slot | D | 🟢 The transport's request↔response bridge takes the no-reply 202 path only for genuine notifications and JSON-RPC responses (`classify_body` `is_notif`/`is_response`). Every other body — a request, a batch, or any malformed/unparseable JSON — is answered by `server.c` with one error frame, which the POST handler waits for and returns; a previous `!is_request` heuristic mis-routed malformed bodies to 202, stranding the unstolen error frame in `resp_present` and permanently deadlocking every subsequent request on the transport. Regression: `tests/test_http_server.c::test_malformed_body_does_not_deadlock`. |

### Out of scope for B1

- **mTLS / client certs.** Terminator does TLS; if the deployment
  needs client-cert auth, the terminator validates and forwards
  `X-Client-Cert-*` headers (see `docs/deployment-tls.md`).
- **WebSocket-style heartbeats.** Out of scope; the SSE keepalive
  mechanism in MCP 2025-11-25 covers the equivalent need.
- **HTTP/2, HTTP/3.** Out of scope (Tier 5 ruling stands; modern
  HTTP is the terminator's job).

---

## B2: transport ↔ rpc

The boundary between bytes-on-the-wire and structured JSON-RPC
messages. The transport produces complete framed bodies; rpc parses
and dispatches.

**Assets:** the dispatch loop's invariants — id-table integrity, no
half-built responses, no use-after-free across the handoff.

| # | Threat | Cat. | Mitigation |
|---|--------|------|------------|
| 2.1 | Malformed JSON-RPC frame crashes dispatch | T | 🟢 `cmcp_rpc_parse` validates the JSON-RPC 2.0 envelope; bad frames produce `-32700`/`-32600` rather than aborting. |
| 2.2 | Request id collisions between peer and host | T | 🟢 Server only ever responds with the peer-provided id; host-initiated requests use a monotonic counter in a separate id space. |
| 2.3 | JSON tree pathologically deep (stack exhaustion) | D | 🟢 6.5.3: `CMCP_JSON_MAX_DEPTH` (default 64) bounds recursive-descent depth in `parse_object` / `parse_array`. Trip → parser returns NULL → `-32700`. Snapshotted once via `pthread_once`. |
| 2.4 | JSON tree pathologically wide (memory bomb) | D | 🟢 6.5.3: `CMCP_JSON_MAX_ELEMENTS` (default 65536) caps both array element count and object key count per container. `HTTP_MAX_BODY_BYTES` (4 MiB) remains the outer envelope. |
| 2.5 | In-flight request table exhaustion | D | 🟢 6.5.3: `CMCP_RPC_MAX_INFLIGHT` (default 1024) snapshotted at `cmcp_rpc_pending_new`; surplus `register` returns `-1` → `CMCP_EAGAIN` at the host caller. Run-time override via `cmcp_rpc_pending_set_max_inflight(t, cap)`; `0` disables. |
| 2.6 | Frame straddling read buffer corrupts state | T | 🟢 Both stdio and HTTP transports are framed — stdio is newline-delimited with bounded line length; HTTP is `Content-Length`-bounded. No partial frames in the dispatch loop. |

---

## B3: rpc ↔ handler (worker pool)

Handlers run on a worker-pool thread; the rpc loop owns dispatch.

**Assets:** handler isolation (a misbehaving handler must not corrupt
the dispatch loop), timeout discipline (handlers don't block forever),
cancellation propagation.

| # | Threat | Cat. | Mitigation |
|---|--------|------|------------|
| 3.1 | Handler runs forever | D | 🟢 Per-handler timeout watchdog (`CMCP_HANDLER_TIMEOUT_MS`, default 30s). The handler ctx exposes `cmcp_handler_cancelled()` for cooperative early-exit. |
| 3.2 | Handler crashes / aborts | T | 🟢 The library never `exit()`s; a crashing handler crashes the host. This is by design — the host owns process lifecycle. Handler authors should treat input as untrusted. |
| 3.3 | Worker pool starvation | D | 🟢 `CMCP_WORKERS` (default 4, clamped 1..64) bounds the worker count. Each tools/call gets one worker; pool exhaustion → request waits in the queue, never spawns new threads. |
| 3.4 | Handler leaks memory cumulatively | D | ⛔ cMCP doesn't track handler heap usage. The host is responsible for monitoring its own RSS. |
| 3.5 | Handler is fed unvalidated `arguments` | T/E | 🟢 Schema validator (6.7) runs *before* dispatch. Tools that opt out via `input_schema = NULL` accept whatever the peer sent — that's the explicit contract. |
| 3.6 | Schema validator pulls in regex compile per call | D | 🟢 6.6: `pattern` / `patternProperties` regexes are compiled once and cached by pattern text in a process-global, mutex-guarded table (`src/schema.c`). Patterns come from author-registered `inputSchema`s, so the distinct set is small and bounded; repeat validations hit the cache instead of recompiling. The cache never evicts (held allocations stay reachable, so no leak), falling back to compile-use-free per call only if it ever fills. Still bounded by the request-rate caps in 1.5 and POSIX-ERE's predictable complexity. |

---

## B4: handler ↔ host

The handler returns into cMCP, which builds the response, which the
host eventually sends on the wire.

**Assets:** response integrity, log integrity, no host-secret leakage.

| # | Threat | Cat. | Mitigation |
|---|--------|------|------------|
| 4.1 | Handler returns a malformed result tree | T | 🟢 The library checks response shape against the JSON-RPC schema before serialising; a malformed handler return produces an internal-error response instead of corrupting the wire. |
| 4.2 | Sensitive values land in MCP wire logs (`notifications/message`) | I | 🟢 6.5.4: `cmcp_server_log` scrubs `data` via `cmcp_json_redact` before emit. Matches normalized keys (lowercase, alphanumeric-only) against `password`/`passwd`/`token`/`secret`/`apikey`/`authorization`/`bearer`/`credential`; matching values become `"[REDACTED]"` regardless of original type. Toggled by `CMCP_LOG_REDACT` (default `1`; `0` to disable for explicit debugging — never set in production). |
| 4.3 | Stack traces in error messages leak file paths | I | 🟡 Library error messages are constant strings; host-facing errors only carry `{path, keyword, message}` from the schema validator (paths are JSON Pointers into the *value*, not file paths). |
| 4.4 | Handler output contains binary noise reaching stdout | T | 🟢 Logging goes to *stderr* only; stdout is owned by the stdio transport and never touched by `cmcp_log`. |

---

## B5: peer ↔ transport (client side)

The client connects to a remote MCP server. The remote server is the
*peer* and may be hostile or buggy.

**Assets:** the host's CPU/memory, the host's filesystem (roots
exposed to the server via `roots/list`), the integrity of in-flight
calls.

| # | Threat | Cat. | Mitigation |
|---|--------|------|------------|
| 5.1 | Server returns oversized response | D | 🟢 Same `HTTP_MAX_BODY_BYTES` cap on the libcurl-side; libcurl's `CURLOPT_MAXFILESIZE` aligns with the server cap. |
| 5.2 | Server claims `roots/list` URI outside host's intent | E | 🟢 The host explicitly sets `roots` via `cmcp_client_set_roots`; the server only sees what the host advertised. |
| 5.3 | Server's `sampling/createMessage` triggers cost / model abuse | E | 🟢 Default decline (returns `-32601` if no handler is set). Opting in requires `caps.sampling = 1` + a registered handler — the host explicitly authorizes. |
| 5.4 | Server cancels client's calls maliciously | D | 🟡 `notifications/cancelled` from the server can only target requests *the server made* to the host (e.g. sampling) — by id-space separation. A server cancelling its own past-replied tool calls is a no-op. |
| 5.5 | Server flood (many notifications) starves the reader | D | 🟡 The reader thread is the single producer for notifications. Per-message handling is bounded by the registered handler's cost; nothing buffers unboundedly on the client side. |
| 5.6 | Server-Sent Event resumption forgery | T | 🟢 `Last-Event-Id` is per-session (the session id binds to the originating stream). Client only honours events arriving on its own active GET. |

---

## TLS posture

**cMCP does not link a TLS library.** Reasoning:

- A protocol library is the wrong layer to terminate TLS at. Long-
  running TLS support means cert rotation, OCSP stapling, session
  resumption — all of which are deployment concerns, not library
  concerns.
- The state-of-the-art deployment shape for a JSON-RPC server is
  behind nginx / caddy / Envoy, which already do TLS, HTTP/2, and
  rate-limiting better than any single-purpose library could.
- Linking OpenSSL/BoringSSL/wolfSSL doubles cMCP's exposed dependency
  surface and forces a single TLS implementation choice on
  downstream consumers.

The closed answer (per the "state-of-the-art" feedback memory):
terminator-only. `docs/deployment-tls.md` covers the nginx/caddy
recipes including `X-Forwarded-Proto` trust and the
`X-Forwarded-For`-behind-`--trust-proxy-headers` flow.

This is *not* a "TLS later" position — it's the architectural
decision for cMCP's lifetime. If a downstream really needs cMCP to
speak TLS directly, they wrap it (stunnel, socat) or vendor a TLS
fork. We will not merge an in-tree TLS implementation.

## What this document does NOT cover

- **Application-layer authentication.** OAuth2.1 / API keys / mTLS
  user identification is the terminator's job. cMCP receives the
  authenticated request and trusts it.
- **Resource exhaustion in the handler.** Disk space, network
  egress, child-process spawning — the handler decides what's safe.
- **Cryptographic correctness.** cMCP doesn't sign / encrypt / hash
  for security purposes. The one CSPRNG read (session id minting,
  `/dev/urandom`) is the entire crypto-surface.
- **Side channels.** Constant-time string compare for session id
  is *not* implemented; an attacker who can measure response
  timing precisely could in principle leak some bits of a session
  id, but doing so requires already-near-position privileges that
  defeat the point. Not in scope without a real attack scenario.

## Where to find things

- `tests/test_hostile_peer.c` — 9 cases / 70 assertions covering
  the negative shape: malformed parses, undersize/oversize messages,
  capability spoof attempts. 6.5.1 adds slow-write + connection-
  flood cases.
- `fuzz/fuzz_*.c` — JSON / JSON-RPC / schema / HTTP-parser
  harnesses.
- `conformance/replay/` — captured-wire regression gate.
- `docs/deployment-tls.md` — terminator recipes + the closed-TLS
  rationale.
- `docs/agentic-readiness.md` — Tier 5 design plan that landed
  the hostile-peer suite and sanitiser matrix.
