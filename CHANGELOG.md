# Changelog

All notable changes to cMCP are recorded here. Phase numbers match
[`TODO.md`](TODO.md) and the commit log. One MCP spec revision is
pinned per release in `include/cmcp.h` (`CMCP_PROTOCOL_VERSION`).

## [0.4.0] — 2026-05-24

Closes Tier 4: all optional capabilities of MCP `2025-06-18`, plus
the three pre-existing spec violations.

### Added

- **4.1 `ping` — both sides answer.** `ping` requests are answered
  with `{}` per spec. Previously returned `-32601` on both sides — a
  peer pinging cMCP for liveness saw a dead endpoint.
- **4.2 Client-side list pagination.** The session aggregator
  follows `nextCursor` on `tools/list`, `resources/list`, and
  `prompts/list` until exhaustion. Servers that paginate were
  previously silently truncated to page one.
- **4.3 HTTP `MCP-Protocol-Version` header.** Sent on every
  post-handshake request and validated on receipt. `2025-06-18` made
  this a MUST.
- **4.4 Elicitation, both halves.** Client:
  `cmcp_client_set_elicitation_handler` + `cmcp_elicitation_result`
  helper. Server: `cmcp_handler_elicit` (cap-gated, returns
  `CMCP_EUNSUPPORTED` if the peer didn't advertise). Enables
  mid-tool-call structured prompts for additional input.
- **4.5 Host-side cancel / progress ergonomics.** `cmcp_client_cancel
  (c, id, reason)` tears down an in-flight call AND emits
  `notifications/cancelled` on the wire. New
  `cmcp_client_call_async_progress` attaches a per-call
  `cmcp_progress_fn` that receives matching `notifications/progress`
  frames for the call's lifetime.
- **4.6 Structured tool output.** `cmcp_handler_set_structured(hctx,
  value)` attaches a typed result to a `tools/call` response.
  Validated against an optional `output_schema` field
  (mismatch → `-32603` per spec). New
  `cmcp_tool_resource_link_content` helper for the `resource_link`
  content-item type. Optional `title` field on tools, resources, and
  prompts — echoed in the respective list descriptors.
- **4.7 Logging.** Server cap-gated `logging/setLevel` route +
  `cmcp_server_log(s, level, logger, data)` that filters against the
  per-server floor (default: `debug`, i.e. no filter). Client sugar:
  `cmcp_client_set_log_level`. The eight RFC 5424 levels are exposed
  as `cmcp_log_level_t` with `from_name`/`to_name` codec helpers.

### Fixed

- Two pre-existing test leaks in `test_http_client` and
  `test_notifications` (heap-allocated `server_arg_t` fixtures from
  phase 2.2 that nobody freed). Surfaced by `make valgrind` after
  4.7 lit up the rest of the path.

### Tests

- 21 binaries, 2642 assertions. Leak-free under `make valgrind`,
  warning-clean under `-Wall -Wextra -Wpedantic`. `make conformance`
  green in both wire directions against the MCP TypeScript reference
  SDK.

## [0.3.0] — 2026-05-22

Tier 3: handler isolation, conformance harness, second reference
server.

### Added

- **3.2 Conformance harness** (`make conformance`). Cross-checks
  cMCP in both wire roles against the MCP TypeScript reference SDK
  (`server-everything` on one side, an `npm`-installed Node client
  on the other). Opt-in (`npm install` + network); deliberately
  separate from the hermetic `make test`.
- **3.3 `tools/filesystem-mcp/`** — second (non-cRAG) reference
  server, ships in `make`. Bounded-root path validation, no symlink
  follow-through, optional read-only mode.
- **3.4 + 3.5 Worker-pool handler dispatch.** Handler-invoking calls
  (`tools/call`, `resources/read`, `prompts/get`) run on a worker
  pool — a slow handler no longer stalls the run loop, and multiple
  in-flight calls execute concurrently. Configurable via
  `CMCP_WORKERS` (default 4, clamped `[1,64]`). Cooperative
  cancellation: handlers poll `cmcp_handler_cancelled(hctx)` and
  surface a `notifications/cancelled` from the peer or a watchdog
  timeout (`CMCP_HANDLER_TIMEOUT_MS`, default 30 s). Progress
  emission: `cmcp_handler_progress(hctx, progress, total, message)`.

### Deferred

- **3.1 OAuth 2.1** — no remote authed server in sight for the
  consumer (butlerbot). Tracked under "Deferred past Tier 4".

## [0.2.0] — 2026-05-07

Tier 2: Streamable HTTP transport, full primitive surface, bidirectional
notifications.

### Added

- **2.1 Streamable HTTP transport, server side**
  (`src/transport_http.c`). Hand-rolled on `socket()` / `accept()` /
  a tiny request parser. SSE for held-open server-to-client streams.
- **2.2 Streamable HTTP transport, client side**
  (`src/transport_http_client.c`). libcurl with a background SSE
  reader thread.
- **2.3 Resources and Prompts.** `resources/list`, `resources/read`,
  `resources/subscribe`/`unsubscribe`; `prompts/list`, `prompts/get`.
  Capabilities auto-advertised when ≥1 is registered.
- **2.4 Server-initiated notifications.** Generic `cmcp_server_notify`
  plus capability-gated wrappers for `*/list_changed` and
  subscriber-filtered `resources/updated`. HTTP delivery routes onto
  held-open SSE connections.
- **2.5 Sampling (host-side).** `cmcp_client_set_sampling_handler`;
  default-decline (`-32601`) if no handler is registered. Authorization
  is the host's job — cMCP carries the bits.
- **2.6 Roots (host-side).** `cmcp_client_set_roots` +
  `cmcp_client_notify_roots_changed`. The `roots` capability is
  auto-advertised once `set_roots` is called.

### Fixed

- **Spec-compliant version negotiation.** Server now responds with
  success on a version mismatch (advertising its own version) and
  lets the host decide whether to disconnect, per spec. Client
  mirrors the same behaviour: captures the server's version and
  proceeds, exposing it via `cmcp_client_server_protocol`.

## [0.1.0] — 2026-05-06

First wire. The library scaffolding, stdio transport, tools, and
the cRAG reference server.

### Added

- **1.1 JSON layer** (`src/json.c`, `include/cmcp_json.h`). Typed
  value tree, hand-rolled parser, emitter with stable key ordering,
  UTF-8 surrogate-pair handling. Lifted as a starting point from
  cRAG's `util.c`, then extended.
- **1.2 JSON-RPC 2.0 framing** (`src/rpc.c`,
  `include/cmcp_types.h`). Request/response/notification + batch,
  in-flight pending table, `cmcp_rpc_dispatch` with a route table.
- **1.3 stdio transport** (`src/transport_stdio.c`,
  `include/cmcp_transport.h`). Newline-framed JSON over `stdin`/
  `stdout`. Vtable type (`cmcp_transport_t`) for later HTTP transports.
- **1.4 Lifecycle.** Server FSM
  (`UNINIT`/`HANDSHAKE`/`READY`/`CLOSED`), capability negotiation
  both sides, protocol-version pinning.
- **1.5 Tool registry + dispatch.** `cmcp_server_add_tool`,
  `tools/list`, `tools/call`. Tool-level errors vs JSON-RPC errors
  cleanly separated.
- **1.6 JSON Schema validator subset** (`src/schema.c`). `type`,
  `properties`, `required`, `enum`, `minLength`/`maxLength`
  (Unicode), `minimum`/`maximum`, `items`, `additionalProperties:
  false`. Server validates before dispatch; failures surface as
  `-32602` with structured `{path, keyword, message}` data. See
  `docs/schema-subset.md`.
- **1.7 `cmcp-inspect` CLI.** Spawn-a-server, dump tools / resources
  / prompts, optionally call a tool.
- **1.8 `crag-mcp` reference server** — first MCP server built on
  cMCP, wrapping cRAG. Two tools (`crag_search`, `crag_stats`) plus
  the `crag://stats` resource. Built behind a separate
  `make crag-mcp` target.
- **1.9 Async client + multi-server session.** Reader-thread demux,
  pending completion table, `cmcp_client_call_async` +
  `cmcp_client_wait` (multiple in-flight, any-order completion),
  `cmcp_client_connect_stdio` for spawn-fork-exec-handshake in one
  call. `cmcp_session_t` aggregates N already-handshaken clients
  under a `<server>:<tool>` qualified namespace.
- **1.10 README + architecture doc.**
