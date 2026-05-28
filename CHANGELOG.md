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

`$ref` / `$defs` / `definitions` (bounded ref resolver), `format`
(`date-time`, `email`, `uri`, `uuid`), `dependentRequired` /
`dependentSchemas` / `dependencies`, `contains` / `minContains` /
`maxContains`, `unevaluatedProperties` / `unevaluatedItems`. Rationale
per keyword in `docs/schema-conformance.md`. The 6.7.3 Ajv cross-check
harness (corpus of ≥500 (schema, value) pairs vs Ajv) lands in a
follow-up commit alongside `make schema-conformance`.

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

#### Deferred to 6.5.4

- Log redactor + `CMCP_LOG_REDACT` (6.5.4).
- `docs/deployment-tls.md` (terminator recipes; 6.5.4).

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
