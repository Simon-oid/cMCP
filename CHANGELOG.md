# Changelog

All notable changes to cMCP are recorded here. Phase numbers match
[`TODO.md`](TODO.md) and the commit log. One MCP spec revision is
pinned per release in `include/cmcp.h` (`CMCP_PROTOCOL_VERSION`).

## Unreleased — v0.7 host-API extensions

Continuing the v0.6.0 thread: close the two follow-up findings the O1
dogfood rewrite surfaced (`v0.7-async-typed-tool-call` and
`v0.7-tool-call-text-shortcut`) and restore the dogfood harness's
step-5 parallel fan-out that the sync-only A1/A2 surface lost.
Additive only; no protocol bump, no struct layout change, no
removals — SemVer-minor.

### Added

- **A4** — typed async `tools/call` pair on `cmcp_client_t`:
  - `cmcp_client_tool_call_async(c, name, args, &id)` — dispatch
    half. Builds `{name, arguments}`, sends via the async core,
    stores the in-flight id in `*out_id`. `args` is consumed in
    every code path (including caller-misuse: NULL c / NULL name /
    NULL out_id). Returns `CMCP_OK` or one of `CMCP_EINVAL` /
    `CMCP_ENOMEM` / `CMCP_EAGAIN` (in-flight cap) / transport
    error from the writer.
  - `cmcp_client_tool_wait(c, id, &result, &text, &rpc_err)` —
    reap half. Blocks until the response arrives, then maps it
    onto the same `cmcp_tool_outcome_t` 3-way enum A2 introduced.
    Transport-side failures (unknown id, mid-flight cancel,
    malformed response) surface as `CMCP_TOOL_ERR_PROTOCOL` with
    a synthesized `cmcp_rpc_error_t` so the caller's switch has
    no default arm.
  - Wire shape, ownership, and error semantics are identical to
    `cmcp_client_tool_call` — the split is purely about
    scheduling. The host fans out N concurrent tool calls and
    reaps them in any order without dropping back to the raw
    `cmcp_client_call_async` + `cmcp_client_wait` surface (which
    would re-expose the two-channel error model A2 eliminated).
- **A5** — `cmcp_client_tool_call_text`, content-shortcut helper.
  Many host call sites want one thing from a `tools/call`: the
  LLM-facing text. Both the success branch
  (`result.content[0].text`) and the tool-error branch
  (`isError:true` + `result.content[0].text`) produce that text;
  A2 only extracts the latter automatically. A5 flattens both:
  on `CMCP_OK`, `*out_text` is the first `content[].text` from
  the result as an owned C string (empty-string fallback so the
  caller has no NULL-vs-empty ambiguity); on `CMCP_EPROTOCOL`,
  `*out_rpc_err` carries the wire/synth error and `*out_text`
  stays NULL. Success-vs-tool-error is *deliberately* squashed —
  hosts that need the distinction use `cmcp_client_tool_call`.

### Changed

- `tools/dogfood-crag-host/main.c` step 5 restored to the async
  fan-out shape lost in the v0.6.0 O1 rewrite. Now uses A4 — 3x
  `cmcp_client_tool_call_async` followed by 3x
  `cmcp_client_tool_wait` in reverse order, exercising the
  reader-thread id-based demux end-to-end. The
  `v0.7-async-typed-tool-call` finding is retired as a ✓-closed
  marker in the wire log.
- `tools/dogfood-crag-host/main.c` grows a new step 8 — one
  `cmcp_client_tool_call_text` call against `crag_search` —
  proving A5 end-to-end through the dogfood harness. The
  `v0.7-tool-call-text-shortcut` finding is retired as a ✓-closed
  marker; the harness summary now reports `findings: 0`, the
  v0.6.0 dogfood thread is fully closed.

### Internal

- `src/client.c` refactored: `cmcp_client_tool_call` (A2) now
  shares `build_tool_call_params` + `process_tool_response`
  helpers with the new A4 wait half and A5 flattener, so the
  3-way branching policy stays single-sourced across all three
  call sites.
- `conformance/fixtures/crag-mcp/dogfood/session-2026-05-30.jsonl`
  re-captured against the A4 step-5 + A5 step-8; now 27 wire
  frames covering 14 RPC calls. Step 5's three `tools/call`
  requests arrive as a back-to-back burst (responses follow in
  id order because `CMCP_WORKERS=1` is pinned in the replay
  registry env block); step 8 adds one more `tools/call` for the
  A5 demo.
- `tests/test_client_helpers.c` grows from 17 to 25 test cases /
  124 to 204 assertions: A4 OK / concurrent fan-out / tool-level
  / protocol-unknown-tool / EINVAL / synth-error, plus A5 OK /
  tool-error-becomes-OK / protocol-unknown-tool / NULL args /
  synth-error.

### Quality matrix (all green)

- `make test` (23 binaries; `test_client_helpers` 204/204
  assertions including the new A4 + A5 cases)
- `valgrind ./tests/test_client_helpers` (leak-free)
- `make test-asan` / `make test-tsan`
- `make replay` (6 pass / 1 skip, including the re-captured
  dogfood fixture)

## v0.6.0 — first host-driven cut (2026-05-30)

The first release sized by **a real host's pain**, not the spec. After
Tier 6 closed (v0.5.0, 2026-05-29) the council met to decide what
v0.6.0 should target. The verdict from D1 — running
`tools/dogfood-crag-host/` as a butlerbot-shaped host against the
in-tree `tools/crag-mcp/` — surfaced four ergonomics findings
(F1–F4) in the public client surface. v0.6.0 ships the additive
closure of those findings, the rewritten dogfood harness that
proves they hold, and a permanent replay-gated wire fixture
capturing the new shape. No protocol bump, no struct layout
change, no removals — SemVer-minor. Full origin in
[`docs/dogfood-cragmcp.md`](docs/dogfood-cragmcp.md); release
contract in [`docs/v0.6.0-acceptance.md`](docs/v0.6.0-acceptance.md).

### Headline

- **A1 (closes F1)** — single-client typed helpers on
  `cmcp_client_t`:
  - `cmcp_client_tools_list`     → `cmcp_session_tool_t[]`
  - `cmcp_client_resources_list` → `cmcp_session_resource_t[]`
  - `cmcp_client_prompts_list`   → `cmcp_session_prompt_t[]`
  - `cmcp_client_resource_read`  → owned text payload
                                   (blob bodies surface as
                                   `CMCP_EUNSUPPORTED`)
  - `cmcp_client_prompt_get`     → owned `cmcp_json_t` messages
                                   array
  Pagination via `nextCursor` is consumed automatically. Records
  reuse the `cmcp_session_*_t` typedefs already in
  `include/cmcp_session.h`; on single-client records the
  `.server` and `.qualified` fields are NULL by contract. Host
  code that talks to one server no longer has to hand-walk
  `cmcp_json_object_get("tools")` or wrap a single client in a
  session aggregator just to get a typed shape.
- **A2 (closes F2)** — `cmcp_client_tool_call` flattener +
  `cmcp_tool_outcome_t` 3-way enum:
  - `CMCP_TOOL_OK`              → `*out_result`  (owned `cmcp_json_t`)
  - `CMCP_TOOL_ERR_TOOL_LEVEL`  → `*out_text`    (owned C string)
  - `CMCP_TOOL_ERR_PROTOCOL`    → `*out_rpc_err` (owned
                                                  `cmcp_rpc_error_t`)
  Collapses the two-channel error model (JSON-RPC `response.error`
  vs `result.isError + content[].text`) that the dogfood harness
  mis-read on its first run into a single switch with three
  outcomes. Caller may pass NULL for any out_*; helper silently
  frees that branch. `args` is consumed in every code path,
  NULL is upgraded to `{}` so the wire stays well-formed. To
  make `CMCP_TOOL_ERR_PROTOCOL` ownership real,
  `cmcp_rpc_error_free` is now public.
- **A3 (closes F3 + F4)** — doc tightenings:
  - `include/cmcp_json.h`'s `struct cmcp_json` gains a `@warning`
    block directing host code to the five typed accessor
    families (`cmcp_json_string`, `_int`, `_bool`,
    `_array_at`, `_object_get` + sizes). States explicitly that
    the struct layout is **not** SemVer-stable while the
    accessors are.
  - `conformance/playbooks/crag-mcp.md` T5 and
    `conformance/playbooks/echo-server.md` T9 updated to the MCP
    `2025-11-25` Minor-5 convention: input-schema rejection on
    `tools/call` surfaces as `result.isError:true +
    content[].text` (the result channel, so the model can
    self-correct), **not** as JSON-RPC `-32602`. The pre-2025-11-25
    `-32602` shape becomes the "Watch for" regression signal.
- **O1** — `tools/dogfood-crag-host/` rewritten to use ONLY the
  A1/A2 helpers. Zero direct `cmcp_json_object_get` calls; zero
  direct `cmcp_rpc_message_t` declarations; every `tools/call`
  site is a single switch on `cmcp_tool_outcome_t`. Two new
  v0.7-candidate findings surfaced by the rewrite itself:
  (a) `cmcp_client_tool_call_async` + a typed
  `cmcp_client_tool_wait` to keep parallel fan-out in the
  flattened model (A1/A2 are sync-only; the rewrite degraded
  step 5 from async-parallel to sequential fan to avoid
  re-importing the two-channel error model); (b)
  `cmcp_client_tool_call_text` to flatten `content[].text` on
  the OK path too.
- **O3** — new replay-gated fixture
  `conformance/fixtures/crag-mcp/dogfood/session-2026-05-30.jsonl`
  captured under `tools/cmcp-tee/` from the rewritten harness
  against an empty DB. Registered in
  `conformance/replay/fixtures.json` with regex masking of the
  `db:` line in `crag://stats` (only env-variable field) and
  `CMCP_WORKERS=1` pinned so burst-pumped input matches the
  captured sequential interleaving. Prereqs: `tools/crag-mcp/crag-mcp`
  on disk + `ollama` on PATH (skips cleanly on hosts without
  ollama, e.g. CI).

### Quality matrix (all green)

| Gate | Command | State |
|---|---|---|
| Unit tests | `make test` | 23 binaries / 2922+ assertions pass |
| Valgrind | `make valgrind` | leak-clean |
| ASan/UBSan | `make test-asan` | clean |
| TSan | `make test-tsan` | clean |
| Replay | `make replay` | 6 passed, 1 skipped (CRAG_TEST_DB), 0 failed |
| Schema conformance | `make schema-conformance` | 83/83 cMCP vs Ajv agreement |
| Fuzz smoke | `make fuzz-smoke` | no crashes (60s/harness × 4) |
| Conformance | `make conformance` | green vs TS reference SDK |

### Out of scope (deliberate, for v0.7)

- F5 (JSON helper naming consistency `cmcp_json_new_string` vs
  `cmcp_json_object_set`) — paper cut, rename is an ABI break.
- F6 (server-concurrency capability flag) — spec-level
  conversation, not a vendor-prefixed extension.
- Tier 7 axes (perf-regression CI gate, nightly fuzz, nightly
  soak, coverage delta, schema-conformance corpus 83 → 500+).
  All five remain filed in `TODO.md` as the v0.7 tier.
- The two v0.7-candidate findings surfaced by O1
  (`cmcp_client_tool_call_async` + typed wait;
  `cmcp_client_tool_call_text` content shortcut) — recorded in
  the dogfood log for future tiering.

## v0.5.0 — Tier 6 (state-of-the-art library polish, 2026-05-29)

Seven axes mapped from five quality lenses (QUALITY / PROFESSIONALISM /
CONFORMITY / SECURITY / PERFORMANCE). Foundation-ordered execution.
Full design in [`TODO.md`](TODO.md) under "Tier 6".

### Headline

- **6.1** — protocol pinned at MCP `2025-11-25` (was `2025-06-18`).
  Spec-compliance wire changes + optional 2025-11-25 capabilities
  (icons, EnumSchema, URL elicitation, sampling tools/toolChoice,
  SSE polling + Last-Event-Id resumption) all landed.
- **6.2** — code-quality measurement: `make coverage` (lcov+gcovr),
  `make analyze` (clang-tidy + scan-build + cppcheck + CodeQL CI lane).
- **6.3** — public API doc (`make docs` → Doxygen under `docs/api/`),
  SemVer policy (`docs/SEMVER.md`), retro-tag script.
- **6.4** — `make install` / `pkg-config` / CMake `find_package(cmcp)`
  / `make uninstall` / `make dist`. External-consumer smoke test
  (`make install-smoke`) builds against the installed library via both
  discovery paths.
- **6.5** — formal threat model (`docs/threat-model.md`), HTTP
  slowloris + accept-rate + protocol-layer caps + log redactor.
  Terminator-only TLS posture (`docs/deployment-tls.md`).
- **6.6** — perf baselines (`bench/`), TS/Py SDK comparison
  (`bench/compare/`), profile baseline + JSON emitter batched-write
  fix (`bench/profile/`), HTTP soak (`make soak-http`).
  Steady-state stdio: 50k calls/s p50=19µs p99=27µs (5.8× TS-SDK,
  47× Python-SDK).
- **6.7** — schema validator near-parity with Ajv: `oneOf`/`anyOf`/
  `allOf`/`not`, `pattern`, `multipleOf`, `min/maxItems`,
  `min/maxProperties`, tuple `items`, `const`, `if/then/else`, plus
  6.7-closure additions `$ref` / `$defs` / draft-07 `definitions`
  (bounded ref resolver, draft-2020-12 sibling semantics) and `format`
  (`date-time`, `email`, `uri`, `uuid` with unknown-format = annotation).
  A new `make schema-conformance` lane proves agreement with Ajv
  (draft-2020-12 + ajv-formats) over an 83-entry corpus.

Plus a handful of follow-ups discovered while building the above:
HTTP-client 503 hang fix (6.6.x), `cmcp-inspect` ergonomics, etc.

`docs/SEMVER.md` is the first post-policy release record: every
section that follows ships with its `git tag` at release time.
Retro-tags for `v0.1.0` … `v0.4.1` are documented (one-time
maintainer action; see SEMVER.md `## Retro-tagging`).

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

### 6.1.2 SSE polling + Last-Event-Id resumption (SEP-1699)

The HTTP server now stamps every SSE event with an `id:` line and
keeps a per-session ring of the last *N* events; clients that
reconnect after a server-initiated disconnect resume via
`Last-Event-Id`. SEP-1699 also reframes server-initiated disconnects
as legal and expects clients to poll, which the cMCP client transport
now does.

**Server.** Every SSE event emitted via `cmcp_server_notify*` carries
a per-session monotonic id starting at 1. Events are recorded in a
fixed-capacity ring (default 256, env-tunable via
`CMCP_HTTP_SSE_REPLAY_BUFFER`, clamped to 65536). A GET `/mcp`
carrying `Last-Event-Id: N` replays every buffered event with id > N
into the new stream before any live events; an absent header or an
unknown / out-of-window id results in no replay (spec-legal — the
client just starts seeing live events). Recording, replay, and
live-emit all run under the shared `sse_mu` so replayed and live
events cannot interleave out of order.

**Client.** The SSE reader thread now loops on `curl_easy_perform`
return: a clean server-side close triggers an immediate reconnect
(50ms breath); HTTP errors and libcurl connection failures back off
exponentially to a 5s cap. Every reconnect after the first event has
been observed sends `Last-Event-Id: <highest>` so the server can
replay anything that emit-fanned-out while the long-poll was being
re-established. Highest event id is tracked under a new `sse_id_mu`,
advanced on every event boundary that carried an `id:` field
(including empty id-only heartbeats).

**Tests.** `test_sse_event_ids_and_replay` (3 sub-cases / 16
assertions in `test_http_server`): replay from id=0 yields all
buffered events; replay from id=1 yields only event 2; replay from
an out-of-window id (999) yields headers only, no events. Existing
HTTP-SSE notification test still passes — the new id-bearing wire
shape is backward-compatible with consumers that ignore `id:`.

### 6.1.3 optional 2025-11-25 capabilities — icons, URL elicitation, cap sub-flags

Three orthogonal extension surfaces from the new spec revision, all
opt-in and all backward-compatible.

**Icons on tools / resources / prompts (SEP-973).** Optional `icons`
field on `cmcp_tool_t`, `cmcp_resource_t`, `cmcp_prompt_t` —
caller-owned JSON-text array of `{src, mimeType?, sizes?}` objects per
spec. Parsed eagerly at registration (`cmcp_server_add_*`) and
emitted verbatim in the corresponding `*/list` descriptor. Bad JSON
or non-array shape → CMCP_EPARSE at registration; absent → field
omitted on the wire (no `"icons": null`).

**URL-mode elicitation (SEP-1036).** New
`cmcp_handler_elicit_url(hctx, message, url, &out)` sends an
`elicitation/create` with `{message, mode: "url", url}` shape
instead of `requestedSchema`. The existing `cmcp_handler_elicit`
(form mode) is unchanged. URL helper is cap-gated by the new
`elicitation.url` sub-flag — a peer that advertises only the legacy
flat `elicitation: {}` is treated as form-only for safety, since a
URL redirect is a meaningfully different trust ask than a schema
form.

**Sub-capability flags (SEP-1036 + SEP-1577).**
`cmcp_client_capabilities_t` grows three boolean fields:
`sampling_tools`, `elicitation_form`, `elicitation_url`. When set,
the client emits the corresponding sub-objects
(`sampling: {tools: {}}`, `elicitation: {form: {}, url: {}}`). When
unset, the legacy flat `{}` shape is preserved — existing hosts that
just toggle `.elicitation = 1` keep advertising what they used to.
The server parser reads the sub-flags into `cmcp_server_client_caps(s)`
so `cmcp_handler_elicit_url` can cap-gate against them.

Three follow-ups deliberately deferred (no consumer in sight; see
TODO 6.1.3 for the per-item reasoning): EnumSchema single/multi-select
(Major 5) and elicitation primitive default-values (Minor 9) fold into
the 6.7 schema audit; sampling `tools`/`toolChoice` request fields
(Major 7) need a server-side tool-descriptor declaration hook out of
scope for this sub-axis (the cap flag is wired so a future axis can
light it up without re-handshaking).

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
- `test_subcap_wire_roundtrip` (test_lifecycle): both-on +
  flat-only sub-cap round-trips.
- `test_emit_url_roundtrip` + `test_emit_url_without_url_subcap`
  (test_elicitation): server tool calls
  `cmcp_handler_elicit_url`; host sees `mode: "url"` + `url`
  fields and responds; gating returns CMCP_EUNSUPPORTED when the
  peer didn't opt into the `elicitation.url` sub-cap.
- `test_icons_emitted_on_tools_list` +
  `test_icons_register_rejects_bad_shape` (test_tools):
  registered `icons` JSON survives round-trip through
  `tools/list`; malformed JSON / non-array shape rejected.

### Fixtures

- All six wire fixtures under `conformance/fixtures/` re-captured
  via `cmcp-tee`. Initialize-response `protocolVersion` flips to
  `2025-11-25`. Two echo-server fixtures
  (`add_schema_missing_required`, `add_schema_type_mismatch`)
  also flip from the `-32602` shape to the `isError` shape;
  `make replay` covers both.

### 6.1.4 fixture audit + full quality matrix re-green

Closes the 6.1 axis. No new surface — verification only.

- Fixture audit. All six tracked wire fixtures already pin
  `protocolVersion: "2025-11-25"` (re-captured during 6.1.1) and
  use the `{isError: true, content: [...]}` validation-failure
  shape. The new surface from 6.1.2/6.1.3 isn't exercisable from
  the existing echo-server / filesystem-mcp / crag-mcp traffic
  shapes (SSE resumption is HTTP-only; icons / URL elicitation /
  sub-caps need consumers that don't exist yet), so no
  additional fixtures land in this sub-axis — that surface is
  covered by unit tests under `tests/`.
- Full quality matrix re-greened post-6.1.3, on `main`
  (`d297431`):
  - `make test`: 2826 assertions across 22 binaries.
  - `make test-asan`: clean (ASan + UBSan).
  - `make test-tsan`: clean.
  - `make valgrind`: clean.
  - `make replay`: 5 PASS + 1 SKIP
    (`crag-mcp/stats_resource_read` legitimately skipped: needs
    `$CRAG_TEST_DB` pointing at a real indexed cRAG DB; the
    other two crag-mcp fixtures pass against a fresh empty DB).
  - `make conformance`: 32/32 (cMCP client vs TS
    server-everything, latest reference SDK) + 8/8 (TS client vs
    cMCP echo-server, including the explicit "2025-11-25 shape"
    isError assertion the SDK schema-validates against).
  - `make fuzz-smoke`: 60s per harness × 4 harnesses (json, rpc,
    schema, http), no crashes / leaks. ~1.6M / 2.0M / 2.4M / 26M
    runs respectively on the 5800X.

### 6.2 code-quality measurement (coverage + static analysis)

Closes Tier 5's measurement gap: the sanitiser bar (5.1) told us
nothing about which library code the suite actually exercised, and
the bug classes the sanitisers miss (NULL-deref on error paths,
unchecked returns, dead branches, taint-flow) had no automated
signal at all. 6.2 introduces two new local quality gates and two
new CI lanes, plus a one-time triage pass that closes every
finding either with a fix or a documented suppression.

#### Coverage (`make coverage`)

Rebuilds the library + test suite with `gcc --coverage -O0` (gcov
instrumentation), runs the full hermetic suite to populate `.gcda`
files, then aggregates into three views under `coverage/`:

- `coverage.info` — lcov tracefile.
- `html/index.html` — browsable line + branch report (via
  `genhtml --branch-coverage`).
- `summary.txt` — gcovr text summary for CI logs.

Only `src/` is measured. `tests/`, `fuzz/`, `conformance/`,
`examples/`, and `tools/` are excluders, not subjects. Baseline at
this commit on `main`:

- Lines:     86.8 % (3974 of 4578)
- Functions: 98.8 % (318 of 322)
- Branches:  63.2 % (2303 of 3642)

The branch number is the lowest of the three because gcov counts
every error path as a branch — most uncovered branches are
`malloc returned NULL`, `recv() < 0`, `clock_gettime() failed`
and similar shapes the hermetic suite doesn't simulate.

#### Static-analysis matrix (`make analyze`)

Three independent checkers run in sequence; the target fails on
the first unsuppressed finding from any of them:

1. **clang-tidy** — per-translation-unit checks. Curated `.clang-tidy`
   config: `bugprone-*`, `cert-*`, `clang-analyzer-*`, `portability-*`,
   plus selective `misc/performance/readability` picks. C++-specific
   groups (cppcoreguidelines, modernize, fuchsia, google-*) explicitly
   off. Inline NOLINT comments at the call sites carry rationale.
2. **scan-build** — clang static analyzer driving the full build
   (interprocedural, path-sensitive). `unix.BlockInCriticalSection`
   is disabled globally — see the inline rationale in
   `transport_http.c` (one read of `/dev/urandom` inside the session-
   table mutex; the read never blocks on Linux once the CSPRNG is
   seeded).
3. **cppcheck** — independent open-source analyzer.
   `--enable=warning,performance,portability`. The `style` category
   is intentionally off: it's dominated by const-correctness
   suggestions which are useful as a future const-pass but not a
   bug class. Project suppressions live in `.cppcheck-suppressions`.

A fourth checker, **CodeQL**, runs in CI only (free for OSS repos)
on every push, every PR, and weekly as a flake fallback. A fifth,
**gcc -fanalyzer**, runs weekly via a separate cron workflow; it's
slow + FP-prone, so it surfaces findings as job-log warnings rather
than blocking the build.

#### One-time triage (closing all initial findings on `main`)

The first clang-tidy run surfaced 28 findings across 9 categories;
all closed in this commit:

| Finding | Resolution |
|---|---|
| 10× `bugprone-reserved-identifier` on POSIX `_POSIX_C_SOURCE` / `_GNU_SOURCE` / `_XOPEN_SOURCE` / `_DEFAULT_SOURCE` | **Suppress in config.** The leading-underscore + uppercase shape *is* the POSIX-mandated form for feature-test macros; the check has no AllowedIdentifiers option upstream. |
| 7× `bugprone-macro-parentheses` on negative-integer JSON-RPC error-code macros (`cmcp_types.h`) | **Fix.** Wrap each `-32xxx` in parentheses. |
| 6× `bugprone-implicit-widening-of-multiplication-result` (timespec arithmetic in `server.c`; size-constant macros in `transport_http.c`) | **Fix.** Cast literal to `long` / `size_t` so the multiplication happens at the destination width. |
| 4× `bugprone-branch-clone` (cascading `rc = CMCP_EIO` in stdio write; close-on-failure ladders in `transport_http.c`; UTF-8 lead-byte fallback in `schema.c`) | **Suppress in-source.** Each cascade is a deliberate signal in a future debugger session, not duplication. |
| 2× `bugprone-not-null-terminated-result` on SSE-frame `memcpy("data: ", ..., 6)` | **Suppress in-source.** The buffer is a length-prefixed wire frame, not a C string. |
| 2× `clang-analyzer-core.NonNullParamChecker` (server dispatch `strcmp(msg->method, ...)`; stdio write `memchr(buf, ...)`) | **Fix.** Add defensive `if (!msg->method)` guard in the request-dispatch path (the JSON-RPC parser already rejects method-less requests; this is belt-and-braces). Short-circuit `len == 0` before `memchr` in stdio write so the analyzer-tracked precondition becomes concrete. |
| 1× `clang-analyzer-unix.BlockInCriticalSection` on `read("/dev/urandom", ...)` under the session-table mutex | **Suppress in-source + scan-build flag.** `/dev/urandom` is non-blocking once the CSPRNG is seeded; refactoring to move the open outside the mutex would add lock ordering for no real win. |
| 1× `clang-analyzer-optin.taint.TaintedAlloc` on the SSE replay-ring `calloc` | **Suppress in-source.** Size comes from `CMCP_HTTP_SSE_REPLAY_BUFFER`, clamped to 65536 (HTTP_SSE_REPLAY_MAX) at parse time — bounded, not unbounded attacker input. |
| 1× `clang-analyzer-core.NullDereference` on the SSE line-buffer push (`transport_http_client.c`) | **Fix.** Add explicit `if (!st->line_buf) return -1;` after the realloc branch — the invariant was already correct, the analyzer just couldn't see it. |
| 1× `readability-non-const-parameter` on the libcurl `CURLOPT_WRITEFUNCTION` callback | **Suppress in-source.** libcurl's callback typedef requires the non-const signature. |

scan-build surfaced one additional finding (cmcp-tee `fclose(NULL)`
on an unreachable error path) — fixed with a defensive
`if (g_log)` guard. cppcheck (warning/performance/portability)
surfaced zero substantive findings; the 26 `style`-category
const-correctness hints are deferred to a future const-correctness
sweep.

#### CI lanes added

- `ci.yml` gains `coverage` (uploads `coverage/html/` as artifact)
  and `analyze` (clang-tidy + scan-build + cppcheck) jobs.
- `codeql.yml` runs CodeQL `cpp` on every push + PR + weekly
  fallback cron.
- `gcc-fanalyzer.yml` runs GCC's static analyzer weekly; warnings
  only, full log archived as an artifact.

### 6.4 packaging & install

First time cMCP can be consumed off-tree without copy-pasting headers
or hard-coding `-I/-L` paths. Standard GNU install layout; three
discovery surfaces installed alongside the libs (pkg-config, CMake
find_package, the install-smoke regression gate).

#### `make install` / `make uninstall`

- `PREFIX` (default `/usr/local`) + `DESTDIR` (staging) supported,
  matching the GNU conventions.
- Public headers land flat under `$PREFIX/include/` (every header
  already carries a `cmcp_` prefix — a subdir would add `-I` gymnastics
  without buying namespace separation).
- Static libs (`libcmcp_core.a` / `libcmcp_server.a` /
  `libcmcp_client.a`) under `$PREFIX/lib/`.
- Reference binaries (`cmcp-inspect`, `filesystem-mcp`, `cmcp-tee`)
  under `$PREFIX/bin/`. `crag-mcp` is excluded — it's only built on
  explicit `make crag-mcp` against an external cRAG tree.
- `uninstall` removes the exact file set `install` created, plus best-
  effort `rmdir` of empty `$CMAKEDIR` / `$PKGCONFDIR` (silent no-op
  if anything else lives there — we don't own `/usr/local/lib`).

#### pkg-config (`packaging/pkgconfig/*.pc.in`)

Three files installed under `$PREFIX/lib/pkgconfig/`:

- `cmcp-core.pc` — declares `Libs.private: -lcurl -lpthread` so
  static-link consumers pick up the transitive symbols.
- `cmcp-server.pc` — `Requires: cmcp-core`; pulls in core's libs +
  cflags transitively.
- `cmcp-client.pc` — same `Requires: cmcp-core` shape.

`pkg-config --libs cmcp-server cmcp-client cmcp-core` emits the link
flags in the right order for ld in one shot.

#### CMake (`packaging/cmake/cmcp{Config,ConfigVersion}.cmake.in`)

`find_package(cmcp REQUIRED COMPONENTS core server client)` produces
imported targets `cmcp::core`, `cmcp::server`, `cmcp::client` with
`INTERFACE_LINK_LIBRARIES` wired so a consumer's
`target_link_libraries(... cmcp::server)` resolves the full chain
(server → core → CURL::libcurl + Threads::Threads).

Version compat is SameMinorVersion — cMCP is pre-1.0, so by SemVer any
MINOR bump may break ABI; PATCH bumps are compatible. The version file
emits `PACKAGE_VERSION_EXACT` and `PACKAGE_VERSION_COMPATIBLE`
accordingly.

#### Shared libraries (opt-in: `ENABLE_SHARED=1`)

Static remains the default per the project's posture. With
`ENABLE_SHARED=1`, `make all` also builds `libcmcp_<x>.so.<VERSION>`
with `-Wl,-soname,libcmcp_<x>.so.<MAJOR>`; `make install` then drops
the standard real-file / SONAME-symlink / dev-link triple under
`$PREFIX/lib/`. PIC objects build to `src/*.pic.o` so the static and
shared link targets coexist in one build tree without re-compilation.

#### `make dist`

Produces a reproducible source tarball `cmcp-<VERSION>.tar.gz` via
`git archive`. Honours `.gitignore` naturally (only tracked files are
in the tarball). VERSION extracted from `CMCP_VERSION` in
`include/cmcp.h` (single source of truth — no separate version files
to keep in sync).

#### `make install-smoke` (regression gate)

`examples/install-smoke/` ships a tiny external consumer (`smoke.c`)
that uses the public headers + core/server/client lifecycle entry
points. The gate:

1. Builds + installs cMCP into a throwaway `$(mktemp -d)`.
2. Builds the consumer against the install via pkg-config + `cc`.
3. Builds the same consumer against the install via
   `find_package(cmcp)` + CMake.
4. Runs both and asserts they exit 0.

CI wires it into `ci.yml` as the `install-smoke` job (next to
`coverage` and `analyze`). If the install / `.pc` / `cmcpConfig.cmake`
plumbing breaks for any reason, this fails before downstream notices.

#### Bug caught during 6.4 itself

The first cut had `install-pkgconfig` / `install-cmake` stage rendered
`.pc` / `.cmake` files under `build/` with `PREFIX` baked at first-
seen time. A second `make install PREFIX=...` with a different
prefix silently reused the cached files — so the install-smoke
consumer saw `prefix=/usr/local` even though it installed to
`/tmp/...`. Fixed by inlining the sed substitution into the install
rules so each invocation re-renders against the active `PREFIX`. The
install-smoke gate caught it on its first run — vindicating the
"install-smoke as gate, not just demo" framing.

### 6.3 API reference, SemVer policy, release tagging

First written contract for cMCP's public surface. Documents which
headers callers can rely on across releases, which symbols can change
in any patch, and how the package version `CMCP_VERSION` relates to
the wire-protocol version `CMCP_PROTOCOL_VERSION` (independent
timelines). Companion deliverables: a generated API reference via
Doxygen and the retro-tag plan for `v0.1.0`…`v0.4.1`.

#### `docs/SEMVER.md`

The policy. TL;DR: MAJOR for any public-surface break, MINOR for
strict additions, PATCH for invisible bug fixes. Public surface =
exactly the 9 headers under `include/`. CMake compatibility encoded
as `SameMinorVersion` while pre-1.0; flips to `SameMajorVersion` at
`1.0.0`. Retro-tag commands at the bottom of the file.

#### `make docs` + `Doxyfile`

Doxygen build of the public headers + tracked markdown
(`README.md`, `docs/*.md`, `CHANGELOG.md`, `conformance/README.md`)
into a browsable HTML reference under `docs/api/html/`. Tuned for C:
typedef-of-struct hidden, `EXTRACT_ALL = YES` so declarations show up
even before per-function `/** */` doc comments land,
`JAVADOC_AUTOBRIEF = YES` so a one-line lead-in becomes the brief.
Warning log at `docs/api/doxygen.log`. The remaining 8 warnings are
expected — cross-refs to `TODO.md` / `CLAUDE.md`, which are
gitignored on purpose.

#### Header pass

- Every public header (`include/cmcp.h`, `cmcp_json.h`, `cmcp_types.h`,
  `cmcp_transport.h`, `cmcp_http_parser.h`, `cmcp_schema.h`,
  `cmcp_server.h`, `cmcp_client.h`, `cmcp_session.h`) opens with a
  `@file` block summarising what's in it.
- `cmcp.h` and `cmcp_json.h` (which had no prior doc comments) got
  `/** */` briefs on every public declaration: error codes,
  constructors / accessors / parse-emit / lifecycle groups.
- The headers that already carried long prose comments (server,
  client, transport, session, types) kept them as-is — Doxygen
  surfaces the declarations via `EXTRACT_ALL`; converting the prose
  blocks to `/** */` is incremental work that can land over time
  without churn-for-churn's-sake commits.

#### `.github/workflows/docs.yml`

- Build job: installs `doxygen`, runs `make docs`, tails the last 80
  lines of `doxygen.log`, uploads `docs/api/html/` as `docs-html`
  artifact on every push + PR.
- Deploy job: on `push` to `main`, also uploads the Pages artifact
  and calls `actions/deploy-pages@v4`. `concurrency: pages` so a
  later push always wins over an in-flight older one.
- Pages is opt-in at the repo level (Settings → Pages → Source:
  GitHub Actions). Until enabled, the deploy step exits cleanly with
  a "Pages not configured" notice.

#### Retro-tagging

Annotated tags `v0.1.0` through `v0.4.1` ship with the SEMVER doc as
maintainer-run commands rather than auto-applied. Tag operations are
hard to undo once pushed; the user runs them on `main`, then
`git push --tags` once. Going forward (post-policy), every release
section in CHANGELOG ships with its tag at release time.

### 6.7 schema validator: near-parity with Ajv

`src/schema.c` expanded from a 9-keyword subset to near-parity with
Ajv (the JSON Schema implementation the TypeScript MCP SDK uses).
This closes the silent-divergence risk between cMCP-hosted and TS-
SDK-hosted tools — schemas written for either now validate identically
on both sides for the keywords listed below.

#### Audit (`docs/schema-audit.md`)

Enumerated every JSON Schema draft 2020-12 / Ajv keyword and mapped
each to one of: ✅ pre-existing, 🟢 added in 6.7, 🟡 partial / departure,
⛔ deliberately not implemented, ◻️ deferred post-6.7. Used to scope
6.7.2 and to size the 6.7.3 cross-check corpus.

#### Implemented in 6.7.2

- **Combinators:** `allOf`, `anyOf`, `oneOf`, `not`.
  - `anyOf` short-circuits on first match; `oneOf` stops counting
    after the second match.
  - Combinator failure surfaces the combinator keyword itself
    (e.g. `"keyword": "anyOf"`), not the inner branch failure —
    when no branch matched, no single branch is "the" reason.
- **Conditional:** `if` / `then` / `else`. `if` runs in
  schema-only mode (its failure is silent — only steers the branch).
- **Annotation:** `const` (deep-equality; equivalent to single-
  element `enum` but matches Ajv's surface).
- **Numeric:** `multipleOf` (integer fast-path via `%`; `fmod` with
  1e-9 relative epsilon for fractional divisors), `exclusiveMinimum`,
  `exclusiveMaximum`.
- **Strings:** `pattern` via POSIX ERE (`<regex.h>`). Flavour
  differences from ECMAScript regex documented in
  `docs/schema-conformance.md` — ASCII patterns are identical;
  `\d` / `\w` / lookahead are POSIX departures.
- **Arrays:** `minItems`, `maxItems`, `uniqueItems` (deep-equality
  pairwise), `prefixItems` (draft-2020-12 tuple form), `items`-as-
  array (draft-07 tuple form), `additionalItems` (draft-07).
- **Objects:** `minProperties`, `maxProperties`, `propertyNames`
  (subschema applied to each key as a string), `patternProperties`
  (POSIX ERE → subschema), `additionalProperties` as subschema
  (the existing `false` short-form remains).
- **Boolean schemas:** `true` (always accept) and `false` (always
  reject) at every position — top-level and as subschemas inside
  `additionalProperties` / `items` / etc.

`-lm` added to `LDLIBS` (and `Libs.private:` in `cmcp-core.pc.in`)
because the validator's `multipleOf` uses `fmod`. CMake imported
target `cmcp::core` gains `m` in `INTERFACE_LINK_LIBRARIES`. The
install-smoke `Makefile` now passes `--static` to `pkg-config` so
the consumer's static link line picks up `Libs.private` transitives.

#### Tests

`tests/test_schema.c` grows 23 new test functions (66 new assertions)
covering positive + negative cases for each new keyword. Total
schema-test count: 104 → 170 assertions. Full suite: 2826 → 2892
assertions. Valgrind-clean on `tests/test_schema` — `regcomp`/
`regfree` pairs balanced; combinator speculation never leaks the
err scratch.

#### `docs/schema-conformance.md`

Supersedes `docs/schema-subset.md` (removed). The keyword table is
the new public surface contract for the validator. Cross-references
from `README.md`, `docs/architecture.md`, and `include/cmcp_schema.h`
updated.

#### Deferred to post-6.7

`dependentRequired` / `dependentSchemas` / `dependencies`, `contains` /
`minContains` / `maxContains`, `unevaluatedProperties` /
`unevaluatedItems`. Rationale per keyword in
`docs/schema-conformance.md`. Deferred to demand — no in-tree
consumer needs them today.

### 6.7.5 close 6.7 — `$ref`, `format`, and the Ajv cross-check lane

Closes the two keyword families that 6.7.2 deferred and lands the
6.7.3 cross-check harness that the axis acceptance criterion called
for.

#### `$ref` / `$defs` / draft-07 `definitions`

`src/schema.c` grows a JSON-Pointer (RFC 6901) resolver that walks
references against the root schema passed to `cmcp_schema_validate`.
Supports `#` (the root itself) and `#/segment/segment` paths with
`~0` / `~1` un-escaping. Both the 2020-12 (`$defs`) and draft-07
(`definitions`) addresses work.

Threaded through every recursive call via a new `validate_ctx_t`
(root + depth + max depth) so resolver context never escapes the
caller's frame. Sibling keywords are honoured per draft-2020-12:
`{"$ref":"#/$defs/S", "minLength":3}` enforces *both* the referenced
schema *and* the inline `minLength`. (Draft-07 made `$ref` exclusive;
2020-12 reversed it. We track 2020-12.)

Recursion is bounded by `CMCP_SCHEMA_MAX_DEPTH = 64`. A cyclic
schema (`{"$ref":"#"}` against a self-recursive value) trips the cap
and surfaces as `"keyword":"$ref"` rather than overflowing the stack.
External refs (network or filesystem fetches) are explicitly *not*
supported; documented as such.

#### `format` — `date-time` / `email` / `uri` / `uuid`

Lexical (fast-mode) validators in the same posture Ajv's
`ajv-formats` plugin uses by default: enforced for strings, no-op
on non-strings, **unknown formats accepted silently**. This matches
the JSON-Schema annotation posture and keeps schemas portable
across implementations that enforce a different format subset.

- `date-time`: RFC 3339 lexical shape with `Z` or `±hh:mm` zone.
- `email`: single `@`, non-empty local, domain with at least one `.`
  and only `[A-Za-z0-9._+-]`.
- `uri`: `scheme:` followed by a non-empty, non-whitespace remainder.
- `uuid`: case-insensitive RFC 4122 textual form.

#### Tests

`tests/test_schema.c` grows 12 new test functions (30 new assertions)
covering `$ref` against `$defs` / `definitions` / root, unresolvable
refs, cycle-bounded refs, sibling-honoured refs, and 4 + 2 cases per
format keyword (positive, negative, non-string no-op, unknown-format
accept). Schema-test count: 170 → 200 assertions. Full suite:
2892 → 2922 assertions across 22 binaries. ASan / valgrind clean.

#### `make schema-conformance` — cMCP vs Ajv (draft-2020-12)

New Makefile target. Builds `conformance/schema_ajv_runner` (a tiny
libcmcp_core-only C driver that walks a JSON corpus and emits per-
entry `<name>\t<ok|fail>`), then runs `conformance/schema_ajv_crosscheck.mjs`
which loads the same corpus, runs Ajv (`ajv/dist/2020.js` with
`ajv-formats`), and asserts agreement on every entry. Disagreements
exit non-zero.

Corpus (`conformance/corpus_schema.json`): 83 (schema, value,
expected) triples covering every supported keyword family — types,
enum/const, numeric bounds, string bounds, pattern, format,
object shape (`properties`/`required`/`additionalProperties`/
`patternProperties`/`min/maxProperties`/`propertyNames`), array
shape (`items`/`prefixItems`/`uniqueItems`/`min/maxItems`),
combinators (`allOf`/`anyOf`/`oneOf`/`not`), conditional
(`if`/`then`/`else`), boolean schemas, and references
(`$ref`/`$defs`/`definitions`/recursive/sibling).

83/83 agree on `main`. The harness becomes the gate against future
divergence — adding a new keyword without a corpus entry is fine; a
keyword that misbehaves vs Ajv surfaces as a CI failure here.

#### Docs

`docs/schema-conformance.md` gains a "References" subsection and a
"Format" subsection that document the new behaviour; the "Not yet
implemented" table loses the `$ref` / `format` rows. The
"Cross-check methodology" section is rewritten to describe the
working `make schema-conformance` target (was: "lands in a
follow-up").

`conformance/package.json` adds `ajv` and `ajv-formats` to
`dependencies`. The harness installs them on first run via
`npm install --prefix conformance`.

#### .gitignore

Adds `conformance/schema_ajv_runner` (the compiled C driver). The
`.c`, `.mjs`, and `.json` sources are tracked.

### 6.5.1 threat model + HTTP slowloris defense

First written threat model for cMCP, plus the HTTP transport's first
DoS-defense layer.

#### `docs/threat-model.md`

STRIDE pass over the 5 trust boundaries (peer↔transport server-side,
peer↔transport client-side, transport↔rpc, rpc↔handler, handler↔host).
Per boundary: assets, numbered threats, mitigations (existing 🟢,
partial 🟡, deferred ◻️, out-of-scope ⛔). Cross-references the
hostile-peer suite and the fuzz harnesses for "what we test."
Includes the explicit closed-question record on TLS: terminator only,
not "TLS later" — `docs/deployment-tls.md` covers the proxy recipes
(slated for 6.5.4).

#### Per-connection read budget (`src/transport_http.c`)

Two new env knobs, both snapshotted at `cmcp_transport_http_listen`
time (per-request handling stays allocation-free):

- `CMCP_HTTP_IDLE_TIMEOUT_MS` (default `30000`). Caps any single
  `recv()` wait. A slow-write peer that dribbles bytes one per
  minute can no longer hold a worker indefinitely.
- `CMCP_HTTP_DEADLINE_MS` (default `120000`). Caps the cumulative
  wall-clock budget for one request-receive cycle (headers + body).
  Defends against the dribble-just-fast-enough-to-evade-idle case:
  the peer sends one byte every 25ms, never trips the idle timer,
  but blows the whole-request deadline.

Both default to non-zero values; `0` or negative disables. When
either trips, the server returns `408 Request Timeout` and closes
the connection.

Implementation: a `conn_budget_t` struct threaded through
`read_headers_block` and `read_exact`. A new `budgeted_recv()` helper
wraps `poll()` + `recv()` and decrements the deadline by the wall-
clock elapsed (`clock_gettime(CLOCK_MONOTONIC)`). `errno = ETIMEDOUT`
on timeout so the caller maps to `CMCP_ETIMEOUT` → 408.

#### Tests

Two new cases in `tests/test_http_server.c`:

- `test_slowloris_idle_timeout`: send a partial request line, wait
  past the configured idle window (250ms in the test), assert
  408 or clean hangup.
- `test_slowloris_deadline`: send one byte every 50ms for a request
  that takes ~1.5s — under the 1000ms idle but over the 500ms
  deadline. Assert the server closes us out.

Both install `SIG_IGN` for `SIGPIPE` (the library uses `MSG_NOSIGNAL`
on its sends; the tests' raw `send()` helpers don't, so writing into
a closed socket would otherwise kill the test process). Total
suite: 2892 → 2903 assertions across 22 binaries. ASan / valgrind
clean.

#### Threat-table cross-refs

The threat-model entries `B1.4` (slow-write peer) and `B1.5`
(connection flood) reference the new env knobs. Mitigations for
`B1.4` are 🟢 (this commit). `B1.5` is 🟢 *with a 6.5.2 caveat* —
the concurrent-connection cap + accept-rate limiter land in the
next commit; the threat-model text is forward-stated and will be
accurate once 6.5.2 ships.

#### Deferred to 6.5.4

- Log redactor + `CMCP_LOG_REDACT` (6.5.4).
- `docs/deployment-tls.md` (terminator recipes; 6.5.4).

### 6.5.2 HTTP accept-rate token bucket

Second HTTP-DoS defense layer: bound the rate at which fresh
connections enter the acceptor so a connection flood can't saturate
the listen path or exhaust file descriptors.

#### Design — why a token bucket, why no `MAX_CONNECTIONS`

The transport already has a structural cap on *concurrent* work: the
acceptor is a single thread that calls `handle_one_connection()`
inline. POST handling is serialised. The threat at this boundary is
*arrival rate*, not concurrency depth — a flood of brief connections
that each get a 503 and close still ties up the acceptor for the
parse/reply cycle.

A token bucket fits this exactly: a sustained allowance (`rate`,
tokens/sec) plus a burst budget (`burst`, capacity) that lets normal
agent workloads spike without false-positive rate-limits. Defaults
are deliberately permissive — 100 conn/sec sustained, 200 burst
capacity — which absorbs ordinary noise while killing trivial floods.
Operators tighten via env.

#### Per-acceptor token bucket (`src/transport_http.c`)

Two new env knobs, both snapshotted at `cmcp_transport_http_listen`
time and read only from the acceptor thread (no synchronisation):

- `CMCP_HTTP_ACCEPT_RATE` (default `100`, double). Sustained
  tokens/sec. `<= 0` disables the gate entirely.
- `CMCP_HTTP_ACCEPT_BURST` (default `200`, double). Bucket capacity.
  Seeded full at `acceptor_main()` start so the first burst of
  connections after listen passes through unthrottled.

Bucket math (`accept_bucket_consume`): on each `accept()` return,
refill by `elapsed_seconds * rate` (clamped to `burst`), then try to
consume one token. If the bucket is below 1.0, the caller sends 503
and closes; otherwise the connection proceeds to
`handle_one_connection`.

Over-budget response (`reply_rate_limited`): `503 Service Unavailable`
with `Retry-After: <s>` where `<s> = clamp(1/rate, 1, 60)`. The peer
gets a definitive answer rather than a hang; the integer-second
clamp keeps the header value sensible.

#### Tests

Two new cases in `tests/test_http_server.c`:

- `test_accept_rate_limit_503`: configure `rate=1`, `burst=2`, open
  12 connections back-to-back, assert at least one gets `503` with
  `Retry-After` *and* at least one passes through (the burst budget
  permitted some).
- `test_accept_rate_disabled_passes_all`: configure `rate=0`,
  hammer with 8 connections, assert zero 503s — confirms the
  off-switch promised in CLAUDE.md / docs.

Total suite: 2903 → 2910 assertions across 22 binaries. ASan clean.

#### Threat-table cross-ref

The threat-model entry `B1.5` (connection flood) is now plain 🟢
(no caveat). Text rewritten to reflect the single-acceptor structural
property and the new env knobs.

### 6.5.3 protocol-layer parser + in-flight caps

Three bounds on peer-controlled growth at the rpc layer. None of these
were exploitable to a *crash* today (the worker queue is already
back-pressured, JSON allocations are tracked, the pending table is
unbounded but a peer pipelining a million requests would just be
back-pressured by the transport), but unbounded growth invites the
next ingenious peer.

#### `CMCP_JSON_MAX_DEPTH` (`src/json.c`)

The parser is recursive descent. A peer that sends `{{{...{1}...}}}`
with 100k braces could drive the call stack into exhaustion territory.
Now a depth counter is threaded through `parser_t` and checked at
every `parse_object` / `parse_array` entry; default cap is **64**
(matches typical JSON-Schema/JSON-RPC depth posture). Trip → parser
returns NULL → caller surfaces `CMCP_EPARSE` → wire response `-32700`.

#### `CMCP_JSON_MAX_ELEMENTS` (`src/json.c`)

A single cap covering both array element count and object key count
per container. Defense against `[1,1,1,...]` memory bombs. Default
**65536** — comfortably above any legitimate MCP payload, kills the
class. Tripped count → reject the whole parse (containers are
discarded as the stack unwinds).

Both JSON caps are snapshotted once via `pthread_once` at first parse,
so per-parse cost is zero on the hot path; the env is read once per
process. `<= 0` for either disables that cap.

#### `CMCP_RPC_MAX_INFLIGHT` (`src/rpc.c`)

`cmcp_rpc_pending_t` (the host-side request-ID table) grew unbounded.
A buggy or hostile peer that never replies could drive the hash table
to arbitrary size on the host side.

New per-table field `max_inflight`, default **1024**, snapshotted at
`cmcp_rpc_pending_new()` from `CMCP_RPC_MAX_INFLIGHT`. Surplus
`cmcp_rpc_pending_register` returns `-1` (distinct from the
allocation-failure `0`). Both `client.c` call sites translate:
`id == 0 → CMCP_ENOMEM`, `id < 0 → CMCP_EAGAIN`.

Run-time configurability:
`cmcp_rpc_pending_set_max_inflight(t, cap)` /
`cmcp_rpc_pending_max_inflight(t)` (additive to the public
`cmcp_rpc_pending_*` API). `0` disables.

#### New public error code

`CMCP_EAGAIN = -13` added to `cmcp_err_t`. Additive — no existing
numeric encoding changes. Per `docs/SEMVER.md` this is a MINOR-class
addition. The `cmcp_errstr` table grows one entry: `"capacity
exceeded, retry"`.

#### Tests

- `tests/test_json.c`:
  - `test_parse_depth_within_limit_accepted` — 32-deep array passes.
  - `test_parse_depth_exceeds_limit_rejected` — 128-deep array rejected.
  - `test_parse_elements_within_limit_accepted` — 1000-element array
    passes and yields `cmcp_json_array_len(v) == 1000`.
  - `test_parse_elements_exceeds_limit_rejected` — 70000-element array
    rejected.
- `tests/test_rpc.c`:
  - `test_pending_max_inflight_caps` — set cap=3, register 3 succeed,
    next two return `-1`; take one, next register succeeds; set cap=0,
    50 more registers succeed.

Total suite: 2910 → 2980 assertions across 22 binaries. ASan/UBSan
clean.

#### Threat-table cross-ref

`B2.3` (deep JSON), `B2.4` (wide JSON), `B2.5` (in-flight table) all
flip from ◻️/🟡 to 🟢 with concrete env knobs.

### 6.5.4 log redactor + deployment-TLS doc (closes axis 6.5)

Two small deliverables that finish axis 6.5: a credential-shaped
value scrubber for the MCP wire log, and the deployment-TLS document
the threat model has been forward-referencing since 6.5.1.

#### `cmcp_json_redact` (`src/json.c`, `include/cmcp_json.h`)

New public utility: in-place recursive walk that replaces values
under credential-shaped keys with the literal string `"[REDACTED]"`.
Match logic:

- Key is **normalized**: lowercase, alphanumeric only. So `api_key`,
  `API-Key`, `apiKey`, `myApiKey`, `customer_secret`, `Authorization`,
  `BearerToken` all match; a bare `key` does not (the normalized
  form is "key", and the pattern list is `apikey`, not `key`).
- Patterns matched as **substring** on the normalized key:
  `password`, `passwd`, `token`, `secret`, `apikey`, `authorization`,
  `bearer`, `credential`.
- Match → replace the value with `cmcp_json_new_string("[REDACTED]")`
  regardless of original type (so a numeric `secret: 12345` also
  gets scrubbed).
- No match → recurse into the value (objects/arrays walked deeply).

Scalar root or `NULL` input is a safe no-op. Allocation failure
during replacement leaves the original value in place — best-effort,
never aborts.

Heuristic value-shape detection (e.g. "looks like a JWT") is
deliberately **not** implemented. Either it false-positives on
legitimate payloads or it misses real tokens; key-based matching is
the documented contract.

#### Wired into `cmcp_server_log` (`src/server.c`)

Snapshot `g_log_redact` once via `pthread_once` at first log call;
defaults to on. `CMCP_LOG_REDACT=0` disables (any nonzero, or unset,
keeps the default on). With the toggle on, `data` is run through
`cmcp_json_redact` before being placed under `params.data` in the
outgoing `notifications/message`. The scrub happens at the source,
not the sink — the host on the receiving end may persist or forward
the payload (file logs, ops pipelines), and the secret should never
have left the process.

#### `docs/deployment-tls.md`

The closed-question record for "should cMCP terminate TLS?" — no,
and here's how to deploy with a terminator. Four sections:

1. **Why no built-in TLS** — TLS's own surface is a moving target;
   real deployments use a terminator regardless; static-by-default
   packaging conflicts with linking OpenSSL; defense-in-depth scope
   creep. Decision closed.
2. **nginx / caddy / HAProxy recipes** — minimal configs that handle
   the Streamable HTTP shape (POST + long-lived SSE GET). Each
   recipe includes SSE-specific tuning (no response buffering) and
   `Origin` header preservation so cMCP's `CMCP_HTTP_ALLOWED_ORIGINS`
   check sees the original peer claim.
3. **mTLS via terminator** — `ssl_verify_client` recipe with
   `X-Client-Cert-*` forwarding. cMCP itself does not parse these
   headers; the host's handler logic does, treating them as
   advisory input over the terminator's authoritative verdict.
4. **Pre-production checklist** — bound to `127.0.0.1`,
   `CMCP_HTTP_ALLOWED_ORIGINS` set, SSE buffering off, log redaction
   left at default, etc.

The document is referenced from `docs/threat-model.md` B1 ("Out of
scope: mTLS") and the closing "TLS posture rationale" question;
those references resolve cleanly now.

#### Tests

`tests/test_json.c`: six new direct tests on `cmcp_json_redact`:
- `test_redact_basic_object` — single sensitive key + benign key.
- `test_redact_key_variants` — `api_key`, `apiKey`, `API-Key`,
  `Authorization`, `BearerToken` all hit; bare `key` and `name`
  don't.
- `test_redact_non_string_values` — numeric `secret` replaced;
  numeric `count` untouched.
- `test_redact_nested` — object inside array inside object.
- `test_redact_no_match_left_alone` — payload with no sensitive
  keys round-trips identically (compared via `cmcp_json_emit_stable`).
- `test_redact_safe_on_null_and_scalar` — no-op on NULL and on a
  string root.

`tests/test_logging.c`: one integration test
(`test_log_data_credentials_redacted`) that drives a real
client/server pair: the tool's handler logs
`{"action": "deploy", "api_key": "sk-LIVE-12345", "user": "alice"}`;
the captured wire `notifications/message` shows `api_key` →
`"[REDACTED]"` and `action` / `user` untouched.

The off-switch (`CMCP_LOG_REDACT=0`) is not auto-tested: the
`pthread_once` snapshot is per-process, so the test harness can't
flip it between cases without spawning subprocesses. The off-path
is a strict subset of the on-path (no transformation), so testing
the on-path covers the harder direction.

Total suite: 2980 → 3005 assertions across 22 binaries. ASan/UBSan
clean.

#### Threat-table cross-ref

`B4.2` (sensitive values in wire logs) flips from ◻️ to 🟢, citing
the new env knob and the scrub utility.

Axis 6.5 (security hardening) is now closed; the only B-row caveats
remaining are spec-out-of-scope (mTLS handled by terminator, etc.).

### 6.6.1 bench tree (cMCP-only baselines)

First-pass performance baselines. Three in-process micro-benches +
a CSV-emitting orchestrator + a methodology document. Cross-SDK
comparison, profiling baselines, and the HTTP soak harness split
into follow-up sub-commits 6.6.2 / 6.6.3 / 6.6.4 to keep this one
self-contained.

#### `bench/` tree

```
bench/
├── bench_util.h               header-only: monotonic clock, lat hist, CSV row
├── bench_server_inline.c      stdio inline-tool throughput + latency
├── bench_server_pool.c        stdio async fan-out → worker-pool concurrency
├── bench_http.c               HTTP-transport variant of the inline bench
├── run.sh                     orchestrator → bench/results.csv + stdout summary
└── README.md
```

Each bench binary spawns an in-process server (pipe pair for stdio,
loopback ephemeral port for HTTP), runs handshake + warmup + a fixed
measurement window, then emits exactly one CSV row to stdout with
schema:

```
bench,iterations,wall_ms,throughput_per_s,
min_us,p50_us,p95_us,p99_us,p999_us,max_us,mean_us,extra
```

`run.sh` runs all three (pinned to CPU 0 via `taskset` when
available), concatenates rows into `bench/results.csv`, and prints
a column-aligned summary. The `extra` column carries free-form
key=value pairs (e.g. `workers=4 sleep_ms=50`) so the row stays
self-describing without growing more columns.

#### Why in-process

Subprocess fork/exec is paid once at session start in real
deployments. Steady-state per-call latency is what consumers care
about. In-process measurement gives a tighter, more reproducible
number; the stdio transport's per-call cost (newline-delimited JSON
over a pipe pair) is identical whether the server is in a thread
or a child process.

The pool bench specifically needs in-process because we want to
control `CMCP_WORKERS` from the bench launcher; doing that across
fork+exec is brittle.

#### Defaults + env knobs

| Bench | Default N | Warmup | Other knobs |
|---|---|---|---|
| `bench_server_inline` | 50000 | 1000 | — |
| `bench_server_pool`   | 64    | n/a  | `CMCP_BENCH_SLEEP_MS` (50ms), `CMCP_WORKERS` (4) |
| `bench_http`          | 5000  | 1000 | — |

HTTP iteration count is deliberately smaller — the transport is
slower than stdio and we don't need 50k samples to reach a stable
p99.

#### Methodology

`docs/perf-baselines.md` covers:

1. What each bench measures and why those iteration counts.
2. Warmup is real (1000 discarded calls) so the worker pool, libcurl
   keep-alive, and kernel state have all settled.
3. Quantiles via `qsort` over the full sample buffer — tractable up
   to ~200k samples; we run 50k.
4. Build flags must be `-O2 -g` (the default); never bench an
   ASan or coverage build.
5. Single-run numbers, no confidence intervals. Sources of variance
   on a typical Linux box (scheduler, NUMA, cache effects) tend to
   swamp formal stats anyway. The baseline is "is cMCP in the right
   order of magnitude," not a stats paper.

#### Build system + gitignore

- `make bench-build` compiles the three binaries.
- `make bench` builds + runs + writes `bench/results.csv`.
- `make clean` removes the binaries and the CSV.
- `.gitignore` adds the three compiled binaries and `results.csv`.
- The Makefile bench rule is fenced behind its own pattern so it
  doesn't collide with the `tests/%` pattern.

#### Out of scope for 6.6.1

- **Comparison vs TS/Py reference SDKs** (Phase 6.6.2). Needs an
  external workload script and apples-to-apples calibration.
- **`perf record` + `heaptrack` flamegraphs** (Phase 6.6.3). Needs
  decisions on what to commit (SVGs are large; raw data isn't
  portable).
- **HTTP soak harness** (Phase 6.6.4). Variant of the existing
  `tests/soak/` driver going through `cmcp_transport_http_connect`
  instead of stdio.
- **`bench_session_fanout`** (Phase 6.6.x). The N-server session
  aggregator throughput bench. Punted because the session API has
  enough nuance (per-server async + paginated list fan-in) that
  it's better as its own commit.
- **Regression gate.** Tier 7 posture if ever; for now, the
  baselines are reference numbers, not CI tripwires.

#### What the first numbers tell us (on a Ryzen 5800X)

| bench                  | throughput     | p50 µs | p99 µs |
|------------------------|---------------:|-------:|-------:|
| `server_inline_stdio`  | 48,813 calls/s |     20 |     24 |
| `server_pool_stdio`    | 80 calls/s     |451,893 |802,600 |
| `server_inline_http`   | 5,378 calls/s  |    185 |    238 |

The two ratios that matter:

1. **stdio vs HTTP throughput ≈ 9×.** The cost of one fresh TCP +
   libcurl handshake per call. `cmcp_transport_http_connect`
   currently creates a new easy handle per POST; the headroom is
   what's available to recover with connection pooling.
2. **Pool concurrency factor = 4.** Throughput tracks
   `workers/sleep_s` to the digit: `4/0.05 = 80 calls/s` measured.
   The pool multiplexes as advertised.

#### Discovered issues (filed for follow-up)

`bench_http` saturates the 6.5.2 accept-rate gate within ~2 seconds
of starting — it's a peer-flood defense, not a self-cap, so the
bench sets `CMCP_HTTP_ACCEPT_RATE=0` at startup unless the user has
explicitly overridden the var. While debugging this, surfaced a
real client-side defect: when the server replies `503` to a POST,
the libcurl client path doesn't surface this to
`cmcp_client_request` — the call hangs waiting for a JSON-RPC
response. Tracked for a follow-up; not blocking 6.6.1.

No test-suite assertion delta in this commit.

### 6.6.4 HTTP soak harness (closes Tier-5 deferral)

Stdio soak landed in 5.6; HTTP soak was deferred on runtime-budget
grounds. This commit closes that deferral.

#### New + refactored under `tests/soak/`

```
tests/soak/
├── soak_common.h          shared helpers (proc sampling, latbuf, workload, CSV schema)
├── soak_driver.c          stdio variant, now uses soak_common.h
├── soak_http_driver.c     HTTP variant
├── echo_http_server.c     HTTP child binary spawned by the HTTP driver
├── run.sh                 stdio orchestrator + drift check (unchanged)
└── run_http.sh            HTTP orchestrator + drift check
```

`soak_common.h` extracts the bits that should be identical between
the two drivers — `/proc/<pid>/{status,fd}` sampling, the 4096-entry
latency ring buffer, the `tools/call echo` workload payload, and the
CSV header. The drivers still own their own session lifecycle (pipe
vs ephemeral TCP) and main loop. A future change to the drift-metric
shape now touches one file.

#### HTTP driver shape

`soak_http_driver` mirrors `soak_driver` but switches transports:

1. Parent picks an ephemeral 127.0.0.1 port via `bind(0)` +
   `getsockname` + `close`.
2. Parent `fork`+`exec`s `echo_http_server --port=<picked>`. Child
   binds it and runs `cmcp_server_run` against
   `cmcp_transport_http_listen`.
3. Parent waits 50ms for the acceptor to reach `poll`, then connects
   via `cmcp_transport_http_connect("http://127.0.0.1:<port>/mcp")`
   and runs the same workload + sampling loop.

The ephemeral-port race window (between `close()` and the child's
`bind()`) is microseconds on loopback; not observed in practice.

#### `CMCP_HTTP_ACCEPT_RATE=0` workaround (consistent with bench)

Every `tools/call` opens a fresh libcurl easy handle (no connection
pooling on the client transport yet), so the soak saturates the
6.5.2 accept-rate gate (default 100 conn/sec, 200 burst) within
~2s. The 6.6.x fix makes the client surface 503s as `CMCP_EAGAIN`
promptly instead of hanging, but the soak is testing leak/stability
under sustained traffic, not the gate (which has its own dedicated
test `test_accept_rate_limit_503`). The driver `setenv`s
`CMCP_HTTP_ACCEPT_RATE=0` at startup unless the user explicitly set
the var; the child inherits this through the environment.

#### Drift criteria — same shape as 5.6 stdio soak

RSS ≤ +15% growth, FDs strictly non-growing, threads equal, p99
latency ≤ 2× drift between the post-warmup baseline and the end
sample. `run_http.sh`'s awk is byte-identical to `run.sh`'s except
for the label.

Observed on this machine (Ryzen 5800X):

| run | duration | parent_rss | parent_fd | parent_threads | child_rss | child_fd | child_threads | p99 µs |
|---|---:|---:|---:|---:|---:|---:|---:|---:|
| `soak-http` (smoke)       | 30s | 9164 → 9164 | 6 → 6 | 3 → 3 | 5888 → 5888 | 5 → 5 | 8 → 8 | 170–190 |
| `soak-http-churn` (smoke) | 45s | 9320 → 9404 | 6 → 6 | 3 → 3 | 5996 → 5936 | 5 → 5 | 8 → 8 | 162–182 |

(+0.9% parent RSS on churn is well under the threshold; the three
respawns each drop and replace the child without any FD or thread
leakage on the parent side.)

#### Build system + gitignore

- `make soak-http` runs `tests/soak/run_http.sh`.
- `make soak-http-churn` runs the same with `SOAK_CHURN=1` (periodic
  child respawn — exercises the connect/teardown path).
- `make clean` removes both new binaries.
- `.gitignore` adds `tests/soak/soak_http_driver` and
  `tests/soak/echo_http_server`.

`docs/perf-baselines.md` gains an "HTTP soak" section with the
observed numbers + the deliberate `CMCP_HTTP_ACCEPT_RATE` workaround
documented. No test-suite assertion delta.

### 6.6.3 profiling baseline + JSON emitter batched write

`bench/profile/` tree. Two scripts (`cpu.sh`, `heap.sh`) capture
call-graph and allocation profiles of `bench_server_inline`. Both
auto-detect their tooling: prefer `perf record` + FlameGraph and
`heaptrack`; fall back to `valgrind --tool=callgrind` /
`--tool=massif` (always available since valgrind is already a
project dep). Findings + before/after callgrind text dumps committed
under `bench/profile/baseline/`.

#### What we measured

Under callgrind, on `bench_server_inline` × 2000 calls (in-process
pipe pair, echo tool):

| bucket | share of CPU |
|---|---:|
| glibc allocator (malloc/free/calloc + internals) | ~38% |
| `src/json.c` (parse + emit + tree manipulation) | ~36% |
| libc string ops (`memcpy`, `strlen`, `memcmp`, `strcmp`) | ~9% |
| pthread mutex lock/unlock | ~1.5% |
| schema validator + RPC framing combined | ~1% |

Heap peak ~57 KB. Working memory is tiny; the 38% in the allocator
is per-call **churn** (~30 mallocs + frees per `tools/call`), not
size. The biggest single lever for future axes is a per-request
arena to convert N allocations into 1.

#### The one fix landed in this commit (free win)

`emit_quoted` was calling `emit_raw` once per source character.
For typical strings (no escapes — tool names, field names, ASCII
payloads), that meant N function calls + N×1-byte `memcpy` calls
instead of one `memcpy` of N bytes. Batched into runs: the emitter
now scans for stretches of "normal" printable ASCII, flushes each
stretch in one `emit_raw` call when it hits an escape character or
the end of the string. One file changed (`src/json.c`); semantics
unchanged; full test suite (2700+ assertions) still green.

Callgrind delta:

| function | before | after |
|---|---:|---:|
| `emit_raw`     | 8.45% (13.15 M Ir) | 4.94% ( 7.07 M Ir) |
| `emit_quoted`  | 4.95% ( 7.70 M Ir) | 3.25% ( 4.65 M Ir) |
| **TOTAL**      | **155.56 M Ir**    | **143.27 M Ir (−7.9%)** |

Wall-clock confirmation on `bench_server_inline`:

| | calls/s | p50 µs | p99 µs |
|---|---:|---:|---:|
| 6.6.1 baseline (before) | 48,813 | 20 | 24 |
| 6.6.3 (after)           | 50,487 | 19 | 27 |

Wall-clock improvement is smaller than the instruction-count
improvement (the bench is partly memory-bound — cache misses
dominate remaining time, not instruction throughput), but the
direction is right.

#### What's deferred to future axes

`bench/profile/baseline/findings.md` triages each one with its
proposed fix shape. Headline:

- **Allocator arena** — biggest single lever (~38%). Needs a
  per-request bump-allocator + ownership-aware free path. Sized
  for its own axis.
- **`cmcp_rpc_emit_take`** — every caller of `send_message` clears
  the message immediately after, so the clones inside
  `cmcp_rpc_to_json` (~2% of CPU) are dead weight. Needs an
  ownership-transferring emit variant + callsite sweep.
- **HTTP-transport profile** — stdio only in 6.6.3; lands with
  6.6.4 alongside HTTP soak.

#### Build system + gitignore

- `make bench-profile-cpu` / `make bench-profile-heap` run the
  respective script. `make bench-profile` runs both.
- `.gitignore` lists `*.svg`, `heap-heaptrack.*.zst`,
  `cpu-perf.*` artifacts — machine- and run-specific. The
  committed `cpu-callgrind-{before,}.{out,txt}` and
  `heap-massif.{out,txt}` are tracked so the baseline is reviewable
  in the repo without re-running valgrind.

`docs/perf-baselines.md` gains a profile-baseline section linking
to the findings + the updated headline number. No test-suite
assertion delta.

### 6.6.2 comparison vs TS / Python reference SDKs

New `bench/compare/` tree. Drives the cMCP client against three
different MCP servers — cMCP itself, the official TypeScript SDK
(`@modelcontextprotocol/sdk`), and the official Python SDK (`mcp`,
FastMCP shape). The client is held constant; only the server changes
per row. Answers axis-6.6.2's question: *"is cMCP in the right
ballpark vs the reference SDKs?"* — yes, 5–50× faster at p50.

#### `bench/compare/` tree

```
bench/compare/
├── bench_compare.c          C driver: spawn server, warmup, measured tools/call, CSV row
├── servers/echo.mjs         TS-SDK stdio server, same shape as examples/echo-server
├── servers/echo.py          Python-SDK (FastMCP) stdio server, same shape
├── package.json             pinned @modelcontextprotocol/sdk + zod
├── requirements.txt         pinned mcp
├── run.sh                   orchestrator; skips TS/Py if toolchain absent
└── README.md
```

The C driver takes `<impl-label> <server-cmd> [args]`, uses
`cmcp_client_connect_stdio` to spawn + handshake, runs 1000 warmup
and 10000 measured `tools/call echo` (env-tunable), then emits one
CSV row matching `bench/results.csv`'s schema so the two CSVs can
be concatenated downstream.

#### Toolchain isolation

External SDKs are **not** auto-installed by the cMCP build:

- **TS:** `npm install --prefix bench/compare` populates a local
  `node_modules/` (gitignored).
- **Python:** `python3 -m venv bench/compare/.venv` + `pip install
  -r requirements.txt` creates a local venv (gitignored). cMCP's own
  build/install never touches system Python.

`run.sh` probes each in turn and prints a one-line skip notice if
the dependency tree isn't present. The cMCP row always runs because
the bench depends only on `examples/echo-server` (built by `make`).

#### Methodology

- **Apples-to-apples surface.** All three servers register the same
  two tools (`echo` taking `{text}`, `add` taking `{a,b}`) with the
  same input schemas. The bench only calls `echo` for the
  measurement window.
- **Stdio only.** HTTP transport is excluded by design — too many
  confounders (libcurl handshake, accept-rate gate, TCP setup) that
  would dilute the SDK-vs-SDK signal.
- **CPU pin.** `taskset -c 0` if available.
- **Subprocess startup absorbed by warmup.** 1000 warmup calls
  before the measurement window opens means the worker pool / JIT
  / Python import cost have all paid out.

#### Numbers (Ryzen 5800X, `-O2 -g`, Node 24 + CPython 3.14)

| impl | throughput | p50 µs | p99 µs | p999 µs | max µs |
|---|---:|---:|---:|---:|---:|
| `cmcp` (C, this repo) | 48,669 calls/s |  20 |   21 |   26 |     80 |
| `ts`   (Node)         |  8,361 calls/s |  89 |  245 | 6,286 | 12,499 |
| `py`   (CPython)      |  1,043 calls/s | 954 | 1,054 | 1,188 |  1,393 |

The interesting ratios:

1. cMCP is ~5.8× faster than TS at p50 and ~47× faster than Python.
2. cMCP's `p99/p50 ≈ 1.05` — no tail. No runtime, no GC, no JIT;
   just `read()` + parse + dispatch + `write()`.
3. V8 GC pauses dominate the TS tail (`max = 12.5 ms`); the Python
   tail is much tighter (CPython's GC pauses are smaller and more
   frequent than V8's).

#### Build system + gitignore

- `make bench-compare-build` compiles the C driver.
- `make bench-compare` builds + runs + writes `bench/compare/results.csv`.
- `make clean` removes the binary and the CSV.
- `.gitignore` adds the binary, the CSV, `node_modules/`, the venv,
  and `__pycache__` trees. Sources (`*.c`, `*.mjs`, `*.py`,
  `package.json`, `requirements.txt`, `run.sh`, `README.md`) are
  tracked.

#### Out of scope for 6.6.2

- **HTTP-transport comparison.** Stdio only per the design
  decision.
- **`tools/list` / `prompts/get` / `resources/read` comparison.**
  One workload is enough for the order-of-magnitude story.
- **Cold-start comparison.** Agent hosts keep one connection per
  session; cold start isn't a steady-state metric.

`docs/perf-baselines.md` gains a "Comparison vs the TS / Python
reference SDKs" section with the methodology and ratios. No
test-suite assertion delta.

### 6.6.x HTTP client surfaces non-success status (closes the 6.6.1 follow-up)

`do_post` in `src/transport_http_client.c` used to silently discard
any response whose status wasn't `200` — it returned `CMCP_OK` and
pushed nothing onto the frame queue, leaving the host's pending
`cmcp_client_request` waiting forever for a JSON-RPC body the server
never sent. Surfaced by `bench_http` against the 6.5.2 accept-rate
gate (returns `503 Retry-After` once the burst budget is spent).

Fix: map non-success status to a return code from `write_fn`:

| HTTP status            | `do_post` returns |
|------------------------|-------------------|
| `200` (with body)      | `CMCP_OK`, body queued |
| `200` (empty), `202`   | `CMCP_OK`, nothing queued (notification ack shape) |
| `503`                  | `CMCP_EAGAIN` (rate-limited; server sent `Retry-After`) |
| other `4xx`/`5xx`      | `CMCP_EIO` |

`cmcp_client_call_async` already unwinds the pending entry when
`send_message` (the wrapper around `cmcp_transport_write`) fails, so
the host caller observes the right error code instead of hanging.
The HTTP error body is discarded — it's an HTTP-layer error page,
not a JSON-RPC frame.

Regression: `tests/test_http_client.c::test_post_503_surfaces_eagain`
stands up a mock HTTP server that always replies `503 Retry-After: 1`
to POSTs (and sinks the SSE GET so the client's reader thread doesn't
hot-spin), then asserts `cmcp_client_handshake` returns `CMCP_EAGAIN`.
Pre-fix, the test would have hung on the initialize POST.

`docs/perf-baselines.md` updated: the "known issue" section flips
from "filed for follow-up" to "fixed; regression test linked."
`bench/bench_http.c` keeps the `CMCP_HTTP_ACCEPT_RATE=0` override,
since the bench legitimately wants to run above the production
default; the comment now notes the 503 path no longer hangs.

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
