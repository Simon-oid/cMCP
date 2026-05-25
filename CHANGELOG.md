# Changelog

All notable changes to cMCP are recorded here. Phase numbers match
[`TODO.md`](TODO.md) and the commit log. One MCP spec revision is
pinned per release in `include/cmcp.h` (`CMCP_PROTOCOL_VERSION`).

## Unreleased — Tier 6 (state-of-the-art library polish)

Seven axes mapped from five quality lenses (QUALITY / PROFESSIONALISM /
CONFORMITY / SECURITY / PERFORMANCE). Foundation-ordered execution.
Full design in [`TODO.md`](TODO.md) under "Tier 6".

### 6.1.1 protocol spec bump — pin + spec-compliance wire changes

`CMCP_PROTOCOL_VERSION` advances `2025-06-18` → `2025-11-25`. Four
spec-compliance items also land in this commit; optional capabilities
introduced by the new revision (icons, EnumSchema, URL-mode
elicitation, sampling tools/toolChoice, SSE resumption) split into
follow-up sub-commits 6.1.2 / 6.1.3 / 6.1.4 to keep blast radius
contained.

- **Pin bump.** `include/cmcp.h` advances to `2025-11-25`.
  `scripts/check-spec-version.sh` exits 0 again. The version
  appears in handshake payloads and HTTP `MCP-Protocol-Version`
  headers on both sides automatically; the in-tree literal sweep
  updates README / docs / comments / test asserts.
- **Tools input-validation errors → tool-execution errors (Minor 5).**
  Per spec clarification, an `inputSchema` violation on `tools/call`
  now returns a success response with `{isError: true, content:
  [...]}` rather than `-32602 INVALID_PARAMS`. Rationale: lets the
  model self-correct on the next turn instead of failing the whole
  RPC. Pre-handshake, non-object params, missing `name`, and
  unknown-tool errors stay JSON-RPC errors (still protocol-shaped).
  Structured `{path, keyword, message}` from the validator is
  rendered into the error text content.
- **HTTP Origin enforcement (Minor 3).** `transport_http.c` gains
  an Origin allow-list controlled by `CMCP_HTTP_ALLOWED_ORIGINS`
  (comma-separated). Default: no list configured → no Origin check
  (backward-compat for tests / curl). When set, a request whose
  `Origin` header doesn't appear in the list gets HTTP 403
  Forbidden before reaching the JSON-RPC layer. Defense against
  DNS rebinding from a browser.
- **Optional `description` on Implementation (Minor 2).** New
  `cmcp_server_set_description` / `cmcp_client_set_description`
  setters; emitted in `serverInfo`/`clientInfo` when set and parsed
  on the other side. Getters `cmcp_client_server_description` /
  `cmcp_server_client_description`. Backward-compat (omitted on
  the wire when unset).
- **stdio stderr-for-all-logging (Minor 1).** Doc-only — cMCP
  already does this.

### Tests

- `test_schema` integration assertions over the wire: schema
  failures now match the `isError` shape, not `-32602`. Unit-level
  schema tests are unaffected (validator return values unchanged).
- `test_hostile_peer` schema-violating-args case: same shape flip.
- New `test_http_origin` covers allow-list match / mismatch /
  unset-allowlist paths.
- New `test_description_field` covers both round-trip directions
  of `cmcp_*_set_description`.
- Test literals advance `2025-06-18` → `2025-11-25` in
  `test_json.c`, `test_rpc.c`, fixtures.

### Fixtures

- All six wire fixtures under `conformance/fixtures/` re-captured
  via `cmcp-tee`. Initialize-response `protocolVersion` flips to
  `2025-11-25`. Two echo-server fixtures
  (`add_schema_missing_required`, `add_schema_type_mismatch`)
  also flip from the `-32602` shape to the `isError` shape;
  `make replay` covers both.

## Tier 5 (agentic readiness, 2026-05-24)

No protocol-surface changes — `CMCP_PROTOCOL_VERSION` stayed at
`2025-06-18`. Tier 5 is the quality bar for letting an LLM agent
drive cMCP without a human in the loop. Closed in roughly axis order:

### Added — quality infrastructure

- **5.1 Sanitisers.** `make test-asan` (`-fsanitize=address,undefined`,
  `-fno-sanitize-recover=all`) and `make test-tsan`
  (`-fsanitize=thread`) each do a full clean rebuild and run the
  whole suite. Both wired into CI on every push (`fail-fast: false`
  so an ASan finding doesn't hide a TSan finding on the same commit).
- **5.2 Real agent in the loop.** New `tools/cmcp-tee/` transparent
  stdio MCP proxy (pure pthreads, no cMCP libs linked) for capturing
  wire transcripts byte-faithfully. `conformance/playbooks/` defines
  ~10 tasks per reference server with expected behaviour + a "watch
  for" line naming the fixable failure mode. First pass discovered
  and fixed a **P0 sandbox-escape in `filesystem-mcp` `fs_write`**:
  a pre-existing symlink leaf whose target lay outside the root *and*
  did not exist slipped past `resolve_path`'s fallback branch, and
  `fopen` followed it. Fix: `lstat` guard + `O_NOFOLLOW`-flagged
  `open` + `fdopen` (TOCTOU-proof). Regression test
  `test_write_symlink_leaf_escape_rejected`.
- **5.3 Wire-fixture replay gate.** New `conformance/replay/`:
  `replay.py` driver streams every recorded `dir:"in"` frame at the
  configured server and asserts every `dir:"out"` frame matches under
  JSON-equality, with per-fixture masks for legitimately variable
  fields. Six fixtures wired in. New `make replay` target + CI lane.
- **5.4 Fuzzing the parsers.** `fuzz/` with four libFuzzer harnesses
  (`json`, `rpc`, `schema`, `http`) + curated seed corpora. Schema
  harness uses a 2-byte length prefix to split fuzz input into
  schema + value. The HTTP request parser was extracted from
  `transport_http.c` into `src/http_parser.c` / `include/cmcp_http_parser.h`
  so the harness can drive it without sockets — same refactor
  pattern cRAG used for `embed_pool`. `make fuzz-build`
  (clang-only); `make fuzz-smoke` runs each harness 60s against its
  seed corpus.
- **5.5 Hostile-peer test suite.** `tests/test_hostile_peer.c` — 9
  cases / 70 assertions. Each case wires up one real cMCP side
  against a raw pipe-fd peer the test drives by hand; pass criteria
  everywhere: real side doesn't crash/leak/race, surfaces the spec-
  mandated error, keeps serving subsequent legitimate traffic.
- **5.6 Soak / long-running stability harness.** `tests/soak/soak_driver.c`
  C workload + `run.sh` orchestrator. Spawns `echo-server`, hammers
  `tools/call`, samples `VmRSS`/open-FD/`Threads` from `/proc` for
  both parent and child, emits CSV, ring-buffered p50/p99 per
  window. `awk` drift criteria (RSS ≤15% growth, FDs strictly
  non-growing, Threads exactly equal, p99 ≤2× baseline). Opt-in via
  `make soak` / `make soak-churn`.
- **5.7 Spec-version drift watch.** `scripts/check-spec-version.sh`
  + weekly `.github/workflows/spec-drift.yml`. Reports a non-zero
  exit when `CMCP_PROTOCOL_VERSION` differs from the newest dated
  revision under `modelcontextprotocol/modelcontextprotocol@main:schema/`.
  At Tier-5 ship: firing (pin `2025-06-18`, upstream had cut
  `2025-11-25`). Resolved in Tier 6.1. Upgrade workflow checklist
  in `docs/spec-version-upgrade.md`.

### Added — tool descriptions tightened by playbook pass

- `examples/echo-server.c`: both `echo` and `add` descriptions now
  name the *output contract* (text content block with byte-identical
  body for `echo`; decimal-string sum for `add`) — without that, a
  model has to guess the return shape.
- `tools/crag-mcp/main.c`: `crag_search` description now spells out
  plain-English queries, the `[cos … bm25 … fusion …] <path>`
  per-chunk header, and what `(no chunk cleared the relevance
  threshold)` actually means; the `crag://stats` resource description
  now points hosts at it as the preferred ambient-context path over
  the `crag_stats` tool.

### Fixed

- `tools/filesystem-mcp/`: P0 symlink-leaf sandbox escape in
  `fs_write_handler` (see 5.2 above).

### Tests

- 22 binaries, ~2716 assertions. Leak-free under `make valgrind`,
  warning-clean under `-Wall -Wextra -Wpedantic`, green under
  `make test-asan` and `make test-tsan`. Fuzz smoke ~32M execs/min
  across the four harnesses with zero findings on the seed corpora.

### Deferred (runtime budget, not engineering)

- 24h fuzz baselines per harness.
- 6h soak nightlies.
- 5.6 HTTP-specific soak variant.

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
