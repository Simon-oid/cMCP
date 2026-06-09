# cMCP

[![CI](https://github.com/Simon-oid/cMCP/actions/workflows/ci.yml/badge.svg)](https://github.com/Simon-oid/cMCP/actions/workflows/ci.yml)

A from-scratch implementation of the [Model Context Protocol](https://modelcontextprotocol.io/)
in pure C11. Three static link targets — core, server, client — sharing
one JSON-RPC 2.0 pipeline. stdio and Streamable HTTP transports.
Tracking spec revision `2025-11-25`; all optional capabilities of the
prior `2025-06-18` revision shipped.

No C++. No external JSON library. No third-party MCP SDK. Hand-rolled
JSON parser, hand-rolled JSON Schema subset validator, hand-rolled
JSON-RPC framing. The only runtime dependency is libcurl (for the
Streamable HTTP client transport). SQLite is pulled in only by the
cRAG reference server.

## Why

MCP is the standard interface that lets an LLM agent call tools (file
systems, databases, search, custom services) without each agent needing
a custom integration per tool. The reference SDKs are written in
TypeScript and Python. cMCP is the C answer — built so an embedded /
systems agent (in this case [butlerbot](https://github.com/Simon-oid/butlerbot))
can speak MCP without dragging a Node or Python runtime alongside it.

The protocol layer is generic; the cRAG server (`tools/crag-mcp/`) is
one example consumer and is built separately, behind an explicit
`make crag-mcp` target.

## Status

**v0.9 — typed tool-call API redesign (P7, 2026-06-01).** A single
pre-1.0 **breaking** wave on the client-side typed tool-call surface,
collapsing five findings from the P6 host-probe — the first real
stateful consumer of the client API — into one corrected call/handle
shape. `cmcp_client_tool_call` now returns a `cmcp_tool_result_t` by
value (no more eval-order-hazardous out-params);
`cmcp_client_tool_call_async` hands back an opaque
`cmcp_tool_handle_t` that binds id→client so a `tool_wait` can't be
mis-routed across servers; cancellation gets its own
`CMCP_TOOL_ERR_CANCELLED` outcome; and the session aggregator gains an
async routed pair. All in-tree callers migrated in the same cut. Wire
format and protocol version (`2025-11-25`) are unchanged — this is
purely the C surface that exposes them.

A follow-on hardening pass (in flight, see [`CHANGELOG.md`](CHANGELOG.md))
adds a loopback-default HTTP bind, `413 Payload Too Large` on oversize
bodies, double-promotion for out-of-range integer literals, and a
compiled-regex cache for `pattern` validation. No public API or
wire-format change.

The v0.8 axes — the Tier 7 regression gates — are the foundation v0.9
built on. They are summarised here:

**v0.8 — Tier 7 closed (regression gates over the Tier 6 baselines, 2026-05-31).**
The four Tier 7 axes deferred at v0.7.0 land in this cut: each one
turns a Tier-6 baseline into a CI gate (or, for the long-haul axes,
a nightly cron lane + tracking-issue plumbing). The present-tense
bugs Tier 5/6 closed are now defended against the slow-drift kind —
silent perf regressions, coverage hollowing, deep-corpus crash
discoveries, multi-hour resource leaks. Additive only, SemVer-minor;
no protocol bump, no struct layout change.

- **7.1 perf-regression CI gate.** `bench/baseline.json` carries the
  committed reference numbers per workload + metric (direction +
  tolerance band per metric). `bench/compare-baseline.sh` runs
  `bench/run.sh` median-of-11 in CI, diffs vs the baseline, sticky-
  comments the delta table on the PR, exits non-zero past the band.
  Five gated metrics across stdio/HTTP. Honours `[skip-bench]` for
  intentional perf trade-offs (two-commit workflow documented in
  `docs/perf-regression-gate.md`).
- **7.2 nightly fuzz baselines.** `.github/workflows/fuzz-nightly.yml`
  runs each of the four libFuzzer harnesses (json, rpc, schema, http)
  for 6h at 02:00 UTC daily. Post-run corpus uploads as 90-day
  artefact; any crash/leak/timeout opens a tracking issue (with
  reuse-or-comment dedup on repeat fires) and fails the job.
  `tools/fuzz-corpus-roll.sh` is the weekly fold helper driving
  libFuzzer `-merge=1` so the seed corpus stays small + curated.
- **7.3 nightly soak runs.** `tests/soak/nightly.sh` orchestrates
  6h stdio + 6h HTTP sequentially (not parallel: concurrent runs
  pollute each other's RSS/p99 drift signal, which is the gate's
  measurement). Dated output dir under `~/.cmcp-soak/`, PASSED or
  FAILED file marker. Local-cron lane, not GitHub Actions —
  shared-runner jitter would noise the gate to the point of paging
  on green. Cron recipe + triage steps in `docs/soak-nightly.md`.
- **7.4 coverage delta gate.** `scripts/coverage-delta.sh` diffs
  `coverage/summary.txt` against the prior main-side snapshot
  restored from actions cache, fails the PR if lines or functions
  dropped more than 1.0pp. Branches reported but not gated (too
  noisy). Sticky PR comment via `gh api` (no third-party action).
  Honours `[skip-cov]` for legitimate cleanups.
  Policy + rationale in `docs/coverage-policy.md`.

Sticky PR comments use a first-party `gh api` sentinel-marker pattern
so CI gains no third-party action dependencies. The 7.5 axis
(schema-conformance corpus 83 → 500 against Ajv) shipped in v0.7.0
and stays at 500/500 agreement under the gate.

The v0.7.0 axes — A4 + A5 host-API extensions and the schema corpus
growth — are the foundation v0.8 layered the gates on. They are
summarised here:

**v0.7 — host-API extensions + schema-corpus growth (2026-05-31).**
**A4** is `cmcp_client_tool_call_async` + `cmcp_client_tool_wait`
(parallel fan-out stays in the flattened 3-way outcome model);
**A5** is `cmcp_client_tool_call_text` (flattens `content[].text`
on the OK path too, squashing success vs tool-error). The dogfood
harness exercises both — step 5 in A4 async fan-out, step 8 in A5
— and the replay fixture covers the wire shape; `findings: 0`.
Schema-conformance Ajv cross-check corpus grew from 83 → 500
(schema, value) pairs across 14 keyword families;
`make schema-conformance` is 500/500.

The v0.6.0 axes — A1/A2/A3 + dogfood rewrite + replay gate — are
the foundation v0.7 built on. They are summarised here:

- **A1 — single-client typed helpers** on `cmcp_client_t`:
  `tools_list` / `resources_list` / `prompts_list` /
  `resource_read` / `prompt_get`. Returns the same
  `cmcp_session_*_t` typedefs the multi-server aggregator uses,
  with `.server`/`.qualified` NULL on single-client records.
  Host code talking to one server no longer hand-walks
  `cmcp_json_object_get("tools")` or wraps a single client in a
  session.
- **A2 — `cmcp_client_tool_call` + 3-way outcome enum.** Collapses
  the JSON-RPC `response.error` vs `result.isError +
  content[].text` two-channel error model into a single switch
  with three outcomes (`CMCP_TOOL_OK` / `CMCP_TOOL_ERR_TOOL_LEVEL`
  / `CMCP_TOOL_ERR_PROTOCOL`). Caller owns the populated branch;
  `cmcp_rpc_error_free` is now public.
- **A3 — doc tightenings.** `struct cmcp_json` gains a `@warning`
  block directing host code to the typed accessors (the struct
  layout is **not** SemVer-stable while the accessors are). The
  crag-mcp + echo-server playbooks updated to the MCP 2025-11-25
  Minor-5 convention: `tools/call` input-schema rejection surfaces
  on the result channel as `isError:true + content[].text`, not
  as JSON-RPC `-32602`.
- **Dogfood harness + replay gate.** `tools/dogfood-crag-host/`
  uses only the typed helpers; the wire capture lives at
  `conformance/fixtures/crag-mcp/dogfood/session-2026-05-30.jsonl`
  and is registered in `make replay`.

The Tier 6 quality bar that v0.6.0 is built on:

**v0.5 — Tier 6 done (state-of-the-art library polish, 2026-05-29).**
Built on Tier 5's agentic-readiness foundation (sanitisers, fuzzing,
replay gate, playbooks). Seven axes, mapped from five quality lenses
(QUALITY / PROFESSIONALISM / CONFORMITY / SECURITY / PERFORMANCE):

- **Protocol pinned at MCP `2025-11-25`.** All wire-behaviour changes
  + optional capabilities of the new revision (icons, EnumSchema, URL
  elicitation, sampling tools/toolChoice, SSE polling + Last-Event-Id
  resumption) shipped.
- **Code-quality measurement.** `make coverage` (lcov + gcovr HTML),
  `make analyze` (clang-tidy + scan-build + cppcheck), plus a CodeQL
  CI lane. Coverage HTML + analyzer findings published as CI artefacts
  every push.
- **Public API surface & SemVer.** Doxygen-built API reference under
  `docs/api/` (`make docs` + auto-published to GitHub Pages on `main`).
  [`docs/SEMVER.md`](docs/SEMVER.md) defines what's public, what's
  internal, and the bump policy. `v0.5.0` is the first post-policy
  release. The pre-policy `v0.1.0`…`v0.4.1` series was retro-tagged
  on release-cut day against the closing commit for each tier (the
  SHAs are pinned in `docs/SEMVER.md`).
- **Packaging.** `make install` (GNU layout, `PREFIX` + `DESTDIR`),
  pkg-config (`cmcp-{core,server,client}.pc`), CMake
  `find_package(cmcp)`, `make uninstall`, `make dist`. An external
  consumer (`examples/install-smoke/`) builds against the installed
  library through both discovery paths in CI.
- **Threat model & hardening.** [`docs/threat-model.md`](docs/threat-model.md)
  (STRIDE-style pass over five trust boundaries). HTTP transport gained
  slowloris idle/deadline budgets, accept-rate token bucket,
  protocol-layer caps on JSON depth / object size / in-flight
  requests, and a stderr log redactor for credential-shaped fields.
  TLS posture: terminator-only — rationale in
  [`docs/deployment-tls.md`](docs/deployment-tls.md).
- **Schema validator near-parity with Ajv.** `oneOf`/`anyOf`/`allOf`/
  `not`, `pattern` (POSIX regex), common `format`s, `multipleOf`,
  `min/maxItems`, `uniqueItems`, `min/maxProperties`, tuple `items`,
  `const`, `if/then/else`. Audit + outcome documented in
  [`docs/schema-conformance.md`](docs/schema-conformance.md).
- **Performance baselines.** `bench/` — three in-process micro-benches
  (stdio inline / worker pool / HTTP). `bench/compare/` — same workload
  against the TS and Python reference SDKs. `bench/profile/` —
  callgrind / massif baselines + a JSON emitter batched-write fix that
  trimmed total instructions 7.9%. `make soak-http` — long-running
  stability through the HTTP transport.

Steady-state stdio throughput on a Ryzen 5800X (p50 = 19 µs, p99 =
27 µs): **50,487 `tools/call`/s** — 5.8× the TS reference SDK,
47× the Python SDK at the same workload. Numbers + methodology in
[`docs/perf-baselines.md`](docs/perf-baselines.md).

**v0.4 + agentic-readiness hardening (Tier 5 done, 2026-05-24).** The
protocol surface stayed at v0.4 (full `2025-06-18` conformance); on
top of it, the quality bar for letting an LLM agent drive cMCP without
a human in the loop. What landed:

- **Sanitisers in CI** — every push runs the suite under ASan/UBSan
  and TSan in parallel (`make test-asan` / `make test-tsan`).
- **Fuzzing** — four libFuzzer harnesses against the parser surface
  (`json`, `rpc`, `schema`, `http`); ~32M execs/min total, zero
  findings on the seed corpora (`make fuzz-smoke`).
- **Hostile-peer test suite** — 9 cases / 70 assertions exercising
  malicious-side behaviour against both client and server (unmatched
  IDs, duplicate responses, malformed mid-session JSON, schema-
  violating tool calls, etc.).
- **Soak harness** — stdio driver with per-window p50/p99 latency
  and /proc-sampled RSS/FD/thread drift criteria (`make soak`,
  `make soak-churn`).
- **Real-agent-in-the-loop playbooks** — three reference servers
  (echo, filesystem, crag) driven from Claude Code with ~10 tasks
  each, plus `tools/cmcp-tee/` for transparent wire-frame capture.
  First pass discovered and fixed a **P0 sandbox escape in
  `filesystem-mcp`** (pre-existing symlink leaf with non-existent
  out-of-sandbox target → `fopen` followed it; fix: `O_NOFOLLOW` +
  `lstat` guard, regression test in place).
- **Wire-fixture replay gate** — `make replay` replays captured
  transcripts under `conformance/fixtures/` and asserts every
  recorded response frame matches, with per-fixture masks for
  legitimately variable fields. New CI lane.
- **Spec-version drift watch** — weekly job
  (`scripts/check-spec-version.sh`) compares `CMCP_PROTOCOL_VERSION`
  against the newest dated revision under
  `modelcontextprotocol/modelcontextprotocol@main:schema/`.
  Fired through 2026-05-24 against upstream `2025-11-25`; the
  bump landed in Tier 6.1 and the watch is back to green.
  See [`docs/spec-version-upgrade.md`](docs/spec-version-upgrade.md)
  for the upgrade workflow.

**v0.4 — complete protocol surface for MCP `2025-06-18`** (the
revision that preceded the Tier 6.1 bump). All optional capabilities
live and the three pre-existing spec violations closed (`ping`,
client-side list pagination, HTTP
`MCP-Protocol-Version` header). New in v0.4: elicitation (both
halves), structured tool output + `resource_link` content + UI `title`
fields, structured logging (`logging/setLevel` +
`notifications/message`), and host-side ergonomics for cancel
(`cmcp_client_cancel`) and per-call progress callbacks
(`cmcp_progress_fn`). Cross-checked in both wire roles against the
MCP TypeScript reference implementation (`make conformance`,
leak-free under valgrind, warning-clean).

**v0.3.** Handler-invoking calls (`tools/call`, `resources/read`,
`prompts/get`) dispatch onto a worker pool — a slow handler can't
stall the run loop, several run concurrently. Handlers get
cooperative `notifications/cancelled`, `progressToken` progress, and
a per-handler timeout watchdog. A second, non-cRAG reference server
(`tools/filesystem-mcp/`) ships in the default build. Opt-in
conformance harness (`make conformance`) cross-checks cMCP in both
wire directions against the MCP TypeScript reference SDK.

**v0.2.** stdio + Streamable HTTP transports. Resources, prompts,
sampling, roots. Server-initiated notifications with subscriber-aware
filtering. Multi-server multiplexing on the client side (a
`<server>:<tool>` qualified namespace across N already-handshaken
clients). Async by design — one reader thread per client, multiple
in-flight calls, any-order completion.

**v0.1.** First wire — stdio, tools, hand-rolled JSON-RPC 2.0 +
JSON Schema subset, lifecycle handshake. `cmcp-inspect` CLI and
`crag-mcp` reference server.

Deferred past Tier 4: OAuth 2.1 (no remote authed server in sight),
`completion/complete` (argument autocomplete — agentic hosts don't
drive completion menus), `resources/templates/list` (needs an
RFC 6570 URI-template engine; no templated server consumer yet).
See [`CHANGELOG.md`](CHANGELOG.md) for the release log and
[`TODO.md`](TODO.md) for the full phasing.

## Build & test

```bash
make            # libs (core/server/client) + cmcp-inspect + filesystem-mcp + cmcp-tee + examples
make test       # build and run the test binaries — currently 2826 assertions across 27 binaries
make valgrind   # same, under valgrind (leak-free)
make test-asan  # full rebuild under -fsanitize=address,undefined; runs suite
make test-tsan  # full rebuild under -fsanitize=thread; runs suite
make replay     # wire-fixture regression gate (conformance/replay/)
make coverage   # rebuild with --coverage; lcov + genhtml + gcovr report under coverage/
make analyze    # clang-tidy + scan-build + cppcheck static-analysis matrix
make fuzz-smoke # 60s per libFuzzer harness against the seed corpus (clang-only)
make soak       # stdio soak driver (env knobs in tests/soak/run.sh)
make soak-http  # HTTP soak driver — same drift criteria, HTTP transport
make bench      # in-process micro-benches → bench/results.csv (stdio + HTTP)
make bench-compare # cMCP vs TS/Py reference SDKs → bench/compare/results.csv
make bench-profile # CPU + heap profile of bench_server_inline → bench/profile/baseline/
make crag-mcp   # build the cRAG reference server (needs sibling ../cRAG/)
make conformance # cross-check vs the MCP TS reference impl (needs Node + network)
make check-spec-drift # compare CMCP_PROTOCOL_VERSION vs upstream spec dirs
make install   # install libs, headers, binaries, pkg-config + CMake files under $PREFIX (default /usr/local)
make uninstall # remove the install tree symmetrically
make dist      # produce cmcp-<version>.tar.gz from HEAD via `git archive`
make install-smoke # build, install to a temp prefix, then build a tiny external consumer via pkg-config AND CMake
make docs      # build Doxygen API reference under docs/api/html/
make clean
```

`make test`, `make test-asan`, `make test-tsan`, `make replay`,
`make coverage`, and `make analyze` are hermetic and offline; CI
runs all six on every push (the coverage HTML lands as a job
artifact, and a CodeQL lane runs alongside as a fourth static-
analysis checker). `make conformance` is the heavyweight opt-in —
it `npm install`s Anthropic's pinned TypeScript reference SDK and
runs cMCP against it in both directions; see
[`conformance/README.md`](conformance/README.md). `make soak`,
`make fuzz-*`, and `make check-spec-drift` are also opt-in
(long-running, clang-only, or network-touching respectively).

System dependency: `libcurl` headers (`pkg-config --cflags libcurl`
must work) — used by the Streamable HTTP client transport. Everything
else is hand-rolled or system-standard. `make coverage` additionally
needs `lcov` + `genhtml` + `gcovr`; `make analyze` needs `clang-tidy`
+ `scan-build` + `cppcheck`. Each target prints which tool is missing
and exits cleanly so the rest of the suite stays runnable on a
minimal box.

### API reference

A Doxygen-generated reference for the public headers is built from
`Doxyfile` via `make docs` and lives under `docs/api/html/`. CI builds
it on every push (artifact `docs-html` on every run; auto-published
to GitHub Pages on `main`).

The public surface and the versioning policy that governs it are
documented in [`docs/SEMVER.md`](docs/SEMVER.md) — including which
headers count as "public" (everything under `include/`), which
identifiers do not (`src/*.h`, reference-binary CLI flags), and how
`CMCP_VERSION` relates to `CMCP_PROTOCOL_VERSION` (they move on
independent timelines).

### Installing as a system library

cMCP follows the standard GNU install layout. By default everything
lands under `/usr/local` (override with `PREFIX=...`; stage with
`DESTDIR=...` for packaging):

```bash
make
sudo make install                                # /usr/local
make install PREFIX=$HOME/.local                 # user-local prefix
make install DESTDIR=/tmp/stage PREFIX=/usr      # staged for packaging
```

Static libraries (`libcmcp_core.a`, `libcmcp_server.a`,
`libcmcp_client.a`) are the default. Shared libraries are opt-in via
`ENABLE_SHARED=1`; they ship with the standard
`libcmcp_<x>.so.<MAJOR>` SONAME (`.so` dev-link + `.so.<MAJOR>` SONAME
symlink + real `.so.<VERSION>` file).

Three discovery surfaces are installed alongside the libs so downstream
consumers don't have to reinvent the flags:

- **pkg-config** — three `.pc` files (`cmcp-core`, `cmcp-server`,
  `cmcp-client`) with the dependency chain encoded via `Requires:`,
  so `pkg-config --libs cmcp-server` already pulls in core + libcurl +
  pthread in the right link order.
- **CMake** — `find_package(cmcp REQUIRED COMPONENTS core server client)`
  exposes `cmcp::core` / `cmcp::server` / `cmcp::client` imported
  targets with `INTERFACE_LINK_LIBRARIES` wired so a consumer's
  `target_link_libraries(... cmcp::server)` resolves the full chain.
- **`make install-smoke`** — a regression gate that installs into a
  throwaway temp prefix and builds + runs the tiny external consumer
  under `examples/install-smoke/` against the installed library
  through both pkg-config and CMake. Catches packaging breakage
  before it reaches downstream.

## Five-minute tour

Spawn the toy echo server and inspect it:

```bash
make
./tools/cmcp-inspect/cmcp-inspect -- ./examples/echo-server
```

Output:

```
Connected: echo-server 0.1.0 (protocol 2025-11-25)

Tools (2):
  echo  -  Return the `text` argument unchanged. Result is a single text content block whose body is byte-identical to the input (no quoting, no formatting, no trimming).
      - text: string (required)
  add  -  Add two signed integers and return the sum. Result is a single text content block holding the sum formatted as a base-10 decimal string (e.g. `42`, `-7`).
      - a: integer (required)
      - b: integer (required)
```

Call a tool:

```bash
./tools/cmcp-inspect/cmcp-inspect \
    -c add -a '{"a":2,"b":40}' -- ./examples/echo-server
# → 42
```

Or write your own minimal C client:

```c
cmcp_client_t *c = cmcp_client_new("hello", "0.1.0");
cmcp_client_connect_stdio(c, "./examples/echo-server",
                           (char *[]){"echo-server", NULL}, NULL);

cmcp_json_t *params = cmcp_json_new_object();
cmcp_json_object_set(params, "name", cmcp_json_new_string("echo"));
cmcp_json_t *args = cmcp_json_new_object();
cmcp_json_object_set(args, "text", cmcp_json_new_string("hi"));
cmcp_json_object_set(params, "arguments", args);

cmcp_rpc_message_t resp;
cmcp_rpc_message_init(&resp);
cmcp_client_request(c, "tools/call", params, &resp);
/* resp.result.content[0].text == "hi" */
cmcp_rpc_message_clear(&resp);
cmcp_client_free(c);   /* closes transport, reaps child */
```

`examples/minimal-client.c` is a fuller version that also lists tools
first; `tools/cmcp-inspect/main.c` is the production-shaped CLI.

For a real RAG server, drop in `crag-mcp` — same wire protocol, real
hybrid retrieval:

```bash
make crag-mcp                           # needs ../cRAG built
./tools/cmcp-inspect/cmcp-inspect \
    -c crag_search -a '{"query":"…","k":5}' \
    -- ./tools/crag-mcp/crag-mcp /path/to/index.db
```

## What ships

| Target | Purpose |
|---|---|
| `libcmcp_core.a`     | JSON, JSON-RPC 2.0, schema validator, types + log levels, stdio + Streamable HTTP transports |
| `libcmcp_server.a`   | Tools / resources / prompts registries, worker-pool dispatch, handshake, lifecycle, server-initiated notifications, cooperative cancellation + progress, server→client requests (elicitation), structured tool output, structured logging |
| `libcmcp_client.a`   | Async client with reader thread, `connect_stdio`, sampling + roots + elicitation host handlers, host-side cancel + per-call progress, multi-server `cmcp_session_t` with pagination |
| `tools/cmcp-inspect/`     | CLI: spawn a server, dump tools / resources / prompts, call one |
| `tools/filesystem-mcp/`   | Reference server: bounded-root filesystem reads/writes. No external deps; ships in `make`. `fs_write` hardened with `O_NOFOLLOW` + `lstat` after a Tier-5 playbook pass surfaced a symlink-leaf sandbox-escape P0. |
| `tools/crag-mcp/`         | Reference server wrapping [cRAG](https://github.com/Simon-oid/cRAG) — two tools (`crag_search`, `crag_stats`) plus the `crag://stats` resource. Built behind a separate `make crag-mcp` target. |
| `tools/cmcp-tee/`         | Transparent stdio MCP proxy. Tees every wire frame in both directions to a JSONL log; the capture format the replay gate consumes. Links no cMCP libs. |
| `examples/echo-server.c`     | A two-tool server in <80 lines |
| `examples/minimal-client.c`  | Spawn a server, list tools, call `echo` |

## Protocol coverage

Tracking [MCP spec date `2025-11-25`](https://modelcontextprotocol.io/specification/2025-11-25/) (pinned in `include/cmcp.h`); the `2025-11-25` wire-behavior changes and optional capabilities all landed in the Tier 6.1 bump. The table below records the version each feature first shipped in (protocol surface was complete by v0.4; later releases were hardening and quality gates).

| Feature                                                    | v0.1 | v0.2 | v0.3 | v0.4 |
|---|---|---|---|---|
| stdio transport                                            | ✓ | ✓ | ✓ | ✓ |
| Streamable HTTP transport                                  |   | ✓ | ✓ | ✓ |
| HTTP `MCP-Protocol-Version` header                         |   |   |   | ✓ |
| `ping` — both sides answer                                 |   |   |   | ✓ |
| Tools (list, call)                                         | ✓ | ✓ | ✓ | ✓ |
| Tool `outputSchema` + `structuredContent`                  |   |   |   | ✓ |
| `resource_link` content item                               |   |   |   | ✓ |
| Tool / resource / prompt `title` field                     |   |   |   | ✓ |
| Resources (list, read, subscribe)                          |   | ✓ | ✓ | ✓ |
| Prompts (list, get)                                        |   | ✓ | ✓ | ✓ |
| Sampling (host-side)                                       |   | ✓ | ✓ | ✓ |
| Roots (host-side)                                          |   | ✓ | ✓ | ✓ |
| Elicitation (receive + emit)                               |   |   |   | ✓ |
| Logging (`setLevel` + `notifications/message`)             |   |   |   | ✓ |
| Multi-server multiplexing (client)                         | ✓ | ✓ | ✓ | ✓ |
| Server-to-client notifications (routing)                   | ✓ | ✓ | ✓ | ✓ |
| `*/list_changed` emit (server side)                        |   | ✓ | ✓ | ✓ |
| Client-side list pagination (follows `nextCursor`)         |   |   |   | ✓ |
| Worker-pool handler dispatch                               |   |   | ✓ | ✓ |
| `notifications/cancelled` + `progressToken` (handler side) |   |   | ✓ | ✓ |
| Host-side `cmcp_client_cancel` + per-call `progress_fn`    |   |   |   | ✓ |
| OAuth 2.1 (HTTP only)                                      |   |   |   |   |

## Layout

```
include/      public headers (cmcp.h, cmcp_json.h, cmcp_types.h,
              cmcp_transport.h, cmcp_http_parser.h, cmcp_schema.h,
              cmcp_server.h, cmcp_client.h, cmcp_session.h)
src/          json, rpc, schema, types, http_parser,
              transport_stdio, transport_http (server), transport_http_client
              → libcmcp_core.a
              server.c, worker.c → libcmcp_server.a
              client.c, session.c → libcmcp_client.a
build/        generated objects + archives (gitignored); `make clean` wipes it
tools/        cmcp-inspect (CLI), filesystem-mcp + crag-mcp (reference servers),
              cmcp-tee (wire-capture proxy for replay-gate fixtures),
              dogfood-crag-host (real-agent-in-loop host probe)
examples/     echo-server, minimal-client, host-probe;
              install-smoke/ (consumer build smoke-test, Make + CMake)
tests/        one binary per test_*.c (27 total), all use tests/test.h
tests/soak/   long-running stability harness (Tier 5.6)
fuzz/         libFuzzer harnesses + seed corpora (Tier 5.4)
bench/        microbenchmarks + perf-regression baselines (Tier 7.1);
              bench/compare/ cross-language harness, bench/profile/ callgrind+massif
conformance/  cross-check vs MCP TS SDK; replay-based regression gate
              over captured wire transcripts; agent playbooks
packaging/    pkg-config (.pc.in) + CMake package-config templates
scripts/      tooling (spec-version drift watch, coverage delta)
docs/         see docs/README.md — architecture, schema, threat model,
              perf, nightly-gate guides, versioning policy
```

Architecture, threading model, and ownership rules are in
[`docs/architecture.md`](docs/architecture.md). The supported
JSON Schema validator surface (near-parity with Ajv) is documented in
[`docs/schema-conformance.md`](docs/schema-conformance.md).
The Tier-5 quality plan is in
[`docs/agentic-readiness.md`](docs/agentic-readiness.md).

## License

MIT. See [`LICENSE`](LICENSE).
