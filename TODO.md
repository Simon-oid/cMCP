# cMCP Roadmap

cMCP is at scaffolding stage — directory layout, public headers, build
system, and docs exist; no implementation yet.

The roadmap follows the same three-tier shape as cRAG: Tier 1 closes
v0.1.0 (the cut openclawd actually consumes); Tier 2 is HTTP transport
and the polish that makes the library look professional; Tier 3 is
deferred large-scope work.

Current target: **Phase 1.5 (Tool registry + dispatch)** — Phases 1.1
through 1.4 are landed. The server can negotiate capabilities and
reject unknown methods with `-32601`; the next step is wiring a real
tool registry behind that.

## Tier 1 — blocking v0.1.0 ("openclawd can call cRAG via MCP")

Both the server and client halves are needed for the openclawd use
case, so both go in Tier 1. v0.1.0 = stdio transport only; HTTP and
remote auth are Tier 2.

### 1.1 JSON layer (`src/json.c`) — DONE

- Shipped: `cmcp_json_t` typed value tree (discriminated union with
  null/bool/int/double/string/array/object). Recursive-descent parser,
  recursive emitter, builder API, deep clone, deep equal, JSON-escape
  helper carried over from cRAG's `util.c`.
- INT and DOUBLE are deliberately separate types so JSON-RPC integer
  IDs round-trip as `7`, not `7.0`.
- Stable-order emit (`cmcp_json_emit_stable`) sorts object keys
  alphabetically via insertion sort over an index array — fine for
  MCP message sizes (< 32 keys).
- Strings: full UTF-8 escape decoding incl. surrogate pairs; lone
  surrogates and unescaped control chars rejected at parse time.
- Object equality is order-independent; insertion order is preserved
  for the default emitter.
- 89 assertions across 31 tests in `tests/test_json.c`, including a
  real MCP `initialize` request round-trip.

### 1.2 JSON-RPC 2.0 framing (`src/rpc.c`) — DONE

- Shipped: `cmcp_rpc_message_t` discriminated union (REQUEST /
  RESPONSE / NOTIFICATION) with `cmcp_id_t` (NONE / NULL / INT /
  STRING). Encoder builds on `cmcp_json_emit_stable` so wire output
  is byte-stable for tests. Decoder validates `jsonrpc=="2.0"`,
  classifies by presence of method/result/error/id, rejects
  ambiguous shapes (method + result, both result + error, etc.).
- Standard error code constants live in `include/cmcp_types.h`
  (`CMCP_RPC_PARSE_ERROR` … `CMCP_RPC_INTERNAL_ERROR`,
  plus the `-32099..-32000` server-error range).
- Pending table (`cmcp_rpc_pending_t`): open-addressed linear-probe
  hash, power-of-two capacity (init 16), monotonic positive IDs,
  resize at 75% load, tombstones for deletions, internal pthread
  mutex. Stores opaque `void*` userdata.
- `cmcp_rpc_dispatch()` walks a `cmcp_rpc_route_t[]` (linear scan
  is fine for the per-server method counts MCP uses), synthesises
  `-32601` for unknown methods on requests, silently drops unknown
  notifications, refuses to dispatch responses (those go to the
  pending table).
- Batches: parser returns N messages and `cmcp_rpc_emit_batch` writes
  them as a JSON array. MCP 2025-06-18 dropped batch support, but the
  framing layer carries it so higher layers can decide to reject.
- 739 assertions across 38 tests in `tests/test_rpc.c`; covers a real
  MCP `initialize` round-trip, the pending table past its initial
  capacity (200 entries, forced resize), and every malformed-shape
  branch.

### 1.3 stdio transport (`src/transport_stdio.c`) — DONE

- Shipped: `cmcp_transport_t` vtable in `include/cmcp_transport.h`
  (`read_fn` / `write_fn` / `close_fn` + `impl`), with thin static-inline
  wrappers (`cmcp_transport_read/write/close`) so call sites read
  naturally. The HTTP transport (Phase 2.1) plugs into the same shape.
- Two stdio constructors: `cmcp_transport_stdio_new()` over the
  process's own stdin/stdout (does NOT close them), and
  `_new_fds(read_fd, write_fd)` taking ownership of the FDs (used for
  spawning a child or testing).
- Newline-delimited framing. Read = blocking `getline()`; the impl owns
  a growable line buffer reused across reads, returns a fresh malloc'd
  copy per frame so caller ownership is unambiguous. Skips blank lines
  defensively.
- Write is mutex-guarded `fwrite + fputc('\n') + fflush` and refuses
  buffers containing raw newlines — a bad upper layer can't desync the
  wire forever.
- EOF and read errors collapse to `CMCP_EIO`; callers treat both as
  "conversation over."
- Tests: `test_stdio_roundtrip` — single frame, 50 frames in order,
  blank-line tolerance, embedded-newline rejection, EOF after writer
  closes, 4 concurrent writer threads × 25 frames each (single reader
  asserts whole frames), and a real `fork()` echo child where the
  parent sends a real `initialize` request via `cmcp_rpc_emit`, reads
  the echoed frame, parses and asserts every field round-tripped.
  807 assertions, 7 tests.

### 1.4 Lifecycle (`src/server.c`, `src/client.c` minimal) — DONE

- Shipped: `cmcp_server_t` and `cmcp_client_t` with explicit state
  machine on the server side
  (`SS_UNINIT → SS_HANDSHAKE → SS_READY → SS_CLOSED`). Server's
  `cmcp_server_run()` is a single read-loop: parse one frame, dispatch,
  reply iff request. Notifications are silently dropped, responses are
  ignored (servers don't issue requests in v0.1), batches are rejected
  with `-32600`, parse failures emit `-32700`.
- Capability structs (`cmcp_server_capabilities_t`,
  `cmcp_client_capabilities_t`) are now in `include/cmcp_types.h` and
  encoded/decoded on both sides — server emits its own caps in the
  `initialize` result, client emits its caps in the request, each side
  parses the other's into a struct after the handshake.
- Pinned protocol version: server returns `-32602` with structured
  data `{ "supported": ..., "requested": ... }` on mismatch; client
  validates the response's `protocolVersion` and refuses with
  `CMCP_EPROTOCOL` if it differs.
- Pre-handshake non-`initialize` requests rejected as `-32600`;
  re-`initialize` after the handshake also rejected as `-32600`;
  unknown methods after handshake are `-32601` (proper tool registry
  arrives in 1.5).
- `cmcp_client_request()` is synchronous — drains intermediate frames
  until the matching response ID arrives, drops stray notifications
  along the way. Allocates IDs from the Phase 1.2 pending table.
- Tests: `test_lifecycle` — happy path (server in pthread, client on
  main, both sides see the other's identity + caps), version mismatch
  (asserts the `supported`/`requested` data), double-`initialize`,
  operate-before-`initialized`, unknown method after handshake, stray
  notification dropped silently. 63 assertions across 6 tests.
- Build: link order changed to `server, client, core` so the
  single-pass linker can resolve core symbols pulled in by server.o /
  client.o after their archives are scanned.

### 1.5 Tool registry + dispatch (`src/server.c`)

- Public API:
  ```c
  cmcp_server_t *s = cmcp_server_new("crag-mcp", "0.1.0");
  cmcp_server_add_tool(s, &(cmcp_tool_t){
      .name = "crag_query",
      .description = "Hybrid retrieval over the cRAG index.",
      .input_schema = "{...JSON Schema...}",
      .handler = crag_query_handler,
      .userdata = ctx,
  });
  cmcp_server_run_stdio(s);
  ```
- `tools/list` walks the registry in registration order.
- `tools/call` validates input against the schema (1.6), dispatches,
  wraps the return value as `content[]` per spec.
- Tests: `test_tools` — register/list/call, unknown tool, schema
  rejection, handler error propagation.

### 1.6 JSON Schema validator subset (`src/schema.c`)

- Supported: `type` (string/number/integer/boolean/array/object/null),
  `properties`, `required`, `enum`, `minLength`/`maxLength`,
  `minimum`/`maximum`, `items`, `additionalProperties: false`.
- Not supported in v0.1: `$ref`, `oneOf`/`anyOf`/`allOf`, `format`.
  Document the supported subset clearly in `docs/schema-subset.md`.
- Validate on both sides — server before dispatch, client before send.
- Tests: `test_schema` — every supported keyword with positive +
  negative cases.

### 1.7 `cmcp-inspect` CLI (`tools/cmcp-inspect/`)

- Spawns a server as a child process (`exec` with argv), runs the
  handshake, lists all tools/resources/prompts, optionally calls a
  tool with JSON args from `--args` or stdin.
- Equivalent of Anthropic's TypeScript `mcp inspector`, but a terminal
  binary that links `libcmcp_client.a`.

### 1.8 `crag-mcp` reference server (`tools/crag-mcp/`)

- Three tools: `crag_query`, `crag_stats`, `crag_index`.
- `crag_index` is gated by `CRAG_MCP_ALLOW_INDEX=1` — indexing from a
  tool call is dangerous-by-default.
- Links cRAG statically from `$CRAG_DIR` (default `../cRAG`). The
  Makefile target is opt-in (`make crag-mcp`).
- **Definition of v0.1.0 done:** Claude Desktop config points at
  `crag-mcp`, you ask "what's in my notes about X" in Claude Desktop,
  the tool fires and a real answer comes back. Same flow from
  openclawd via `libcmcp_client.a`.

### 1.9 Client side + multi-server multiplexing (`src/client.c`)

- `cmcp_client_connect_stdio(path, argv, env)` forks child, runs
  `initialize`, returns handle. Async by design — every call returns
  a request ID; completion via callback or `cmcp_client_wait(id)`.
- `cmcp_session_t` aggregates many clients. `cmcp_session_tools_list()`
  returns the union, namespaced as `<server>:<tool>`.
  `cmcp_session_tool_call("crag-mcp:crag_query", args)` routes
  correctly.
- Tests: `test_client_server` — both halves in-process over
  `socketpair()`, real handshake, real call, multiplex with two
  in-process servers and assert routing.

### 1.10 README + architecture doc

- README with what cMCP is, build/test, five-minute "spawn cRAG-MCP
  from openclawd" example, protocol coverage matrix, layout, status,
  MIT.
- `docs/architecture.md` — module boundaries, data flow diagrams,
  threading model, ownership rules.

## Tier 2 — Streamable HTTP + protocol breadth

The cuts that take cMCP from "openclawd works" to "professional MCP
library someone else could pick up."

### 2.1 Streamable HTTP transport — server (`src/transport_http.c`)

- Single `/mcp` endpoint. POST = request/response. GET = SSE upgrade
  for server-to-client streams. Session via `Mcp-Session-Id` header.
- Hand-rolled HTTP/1.1 server on top of `socket()` + `accept()` + a
  tiny request parser. No TLS — document that prod use should put it
  behind nginx/caddy. (Decision recorded in `docs/architecture.md`.)
- Tests: `test_http_server` — happy path POST/response, SSE stream
  with multiple events, session resume, malformed request rejection.

### 2.2 Streamable HTTP transport — client

- libcurl multi handle for non-blocking I/O. SSE parser
  (`data: <line>\n\n` framing). Session header propagation.
- Tests: `test_http_client` — against the in-process server from 2.1.

### 2.3 Resources + prompts

- Server: `cmcp_server_add_resource()`, `cmcp_server_add_prompt()`.
  `resources/list`, `resources/read`, `resources/subscribe`,
  `prompts/list`, `prompts/get`.
- Client: mirror functions on the session aggregator.
- cRAG can expose its DB stats as a resource (`crag://stats`) — gives
  a non-tool primitive to exercise the pathway end-to-end.

### 2.4 Notifications

- Handle `tools/list_changed`, `resources/list_changed`,
  `resources/updated`, `$/progress`, `$/cancel`. Server emit;
  client subscribe + callback delivery.

### 2.5 Sampling (host-side)

- When a server requests `sampling/createMessage`, route to openclawd's
  configured model client. **Behind a per-server allow flag** —
  untrusted servers must not be able to make the host spend tokens
  silently.

### 2.6 Roots (host-side)

- Host tells server which filesystem paths are in scope. Used by tools
  that operate on files (a future filesystem server, etc.).

## Tier 3 — deferred

Large scope. None of these block openclawd; pick up only when there's
a concrete reason.

### 3.1 OAuth 2.1 (HTTP transport only)

- Authorization server discovery, dynamic client registration, PKCE,
  resource indicators. 1–2 weeks of fiddly RFC reading. Defer until
  there is a remote cMCP server worth publishing.

### 3.2 Conformance test harness

- Run cMCP's server against Anthropic's published reference clients;
  run cMCP's client against Anthropic's published reference servers.
  Assert behavior matches.

### 3.3 Second non-cRAG reference server

- Filesystem or shell-exec server (the latter with strict allowlists).
  Proves the framework isn't accidentally cRAG-shaped.

### 3.4 Connection pooling / per-server worker pools

- Currently one reader thread per transport, one writer mutex per
  transport. At scale (many concurrent in-flight tool calls per
  server) this may need revisiting. Wait for a real workload before
  optimising.

## Done (for reference)

- Repo skeleton, `CLAUDE.md`, `README.md`, this `TODO.md`, `Makefile`,
  `LICENSE`, `.gitignore`, public umbrella header.
- Phase 1.1: JSON layer (`src/json.c`, `include/cmcp_json.h`). Typed
  value tree, parser, emitter (insertion-order + alphabetical-stable),
  builder, clone, equal, UTF-8 surrogate pairs. 89/89 assertions pass.
  ~600 lines of impl, ~440 lines of tests.
- Phase 1.2: JSON-RPC 2.0 framing (`src/rpc.c`,
  `include/cmcp_types.h`). Message struct, encode/decode (incl. batch
  + real MCP `initialize`), in-flight pending table (open-addressed
  hash, mutex, resize), `cmcp_rpc_dispatch` with route table.
  739/739 assertions across 38 tests. ~660 lines impl + 180 line
  header, ~620 lines of tests.
- Phase 1.3: stdio transport (`src/transport_stdio.c`,
  `include/cmcp_transport.h`). Vtable type for transports + stdio
  impl: newline framing, mutex-guarded writes, growable read buffer,
  EOF→EIO. Two ctors: process stdin/stdout vs. own-the-FDs.
  `test_stdio_roundtrip` covers concurrent writers and a real
  fork()ed echo child running a JSON-RPC handshake.
  807/807 assertions across 7 tests.
- Phase 1.4: Lifecycle (`src/server.c`, `src/client.c`,
  `include/cmcp_server.h`, `include/cmcp_client.h`). Server state
  machine (UNINIT/HANDSHAKE/READY/CLOSED), capability negotiation
  on both sides, protocol-version pinning with structured
  `-32602` data, sync client request/response with intermediate
  frame draining. `test_lifecycle` covers happy path, version
  mismatch, double-init, operate-before-init, unknown method,
  stray notification. 63/63 assertions across 6 tests. Total now
  1698 assertions over four test binaries.
- Wildcard-based Makefile so `CORE_SRC`/`SERVER_SRC`/`CLIENT_SRC`
  auto-grow as phases land — `make` only compiles files that exist.
