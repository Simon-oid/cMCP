# cMCP

[![CI](https://github.com/Simon-oid/cMCP/actions/workflows/ci.yml/badge.svg)](https://github.com/Simon-oid/cMCP/actions/workflows/ci.yml)
[![CodeQL](https://github.com/Simon-oid/cMCP/actions/workflows/codeql.yml/badge.svg)](https://github.com/Simon-oid/cMCP/actions/workflows/codeql.yml)
[![MCP spec](https://img.shields.io/badge/MCP%20spec-2025--11--25-blue)](https://modelcontextprotocol.io/specification/2025-11-25/)
[![License: MIT](https://img.shields.io/badge/license-MIT-green)](LICENSE)

![cMCP demo: spawn a server with cmcp-inspect, call tools, watch a symlink sandbox-escape get refused, then the raw JSON-RPC wire captured with cmcp-tee](docs/demo.gif)

A from-scratch implementation of the
[Model Context Protocol](https://modelcontextprotocol.io/) in pure C11
— client **and** server, stdio **and** Streamable HTTP.

No C++. No external JSON library. No third-party MCP SDK. Hand-rolled
JSON parser, hand-rolled JSON Schema validator (near-parity with Ajv),
hand-rolled JSON-RPC 2.0 framing. The only runtime dependency is
libcurl (for the Streamable HTTP client transport).

## Highlights

- **50,487 `tools/call`/s** steady-state over stdio (p50 19 µs, p99
  27 µs on a Ryzen 5800X) — **5.8× the official TypeScript SDK, 47×
  the Python SDK** on the same workload. Methodology and numbers in
  [`docs/perf-baselines.md`](docs/perf-baselines.md).
- **2,826 assertions across 27 test binaries**, all green under
  valgrind, ASan/UBSan, and TSan on every push.
- **Conformance-tested in both wire roles** against the official MCP
  TypeScript reference SDK, plus a replay gate over captured wire
  transcripts (`make replay`).
- **500/500 agreement with Ajv** across the JSON Schema validator
  conformance corpus
  ([`docs/schema-conformance.md`](docs/schema-conformance.md)).
- **Fuzzed nightly** — four libFuzzer harnesses (JSON, JSON-RPC,
  schema, HTTP parser), 6 hours each per night, zero outstanding
  findings.
- **Threat-modelled and hardened** — STRIDE pass over five trust
  boundaries; SSRF egress guard, slowloris budgets, accept-rate token
  bucket, parser depth/size caps, credential log redaction
  ([`docs/threat-model.md`](docs/threat-model.md)).
- **CI-gated against slow drift** — perf-regression gate vs committed
  baselines, coverage-delta gate, nightly soak runs, weekly
  spec-version drift watch.
- **Tested by a real LLM agent** — playbook-driven sessions where
  Claude Code drives the reference servers through real tasks. The
  first pass found a genuine sandbox-escape P0:
  [`docs/case-study-symlink-escape.md`](docs/case-study-symlink-escape.md).

## Why

MCP is the standard interface that lets an LLM agent call tools (file
systems, databases, search, custom services) without each agent needing
a custom integration per tool. The reference SDKs are written in
TypeScript and Python. cMCP is the C answer — built so an embedded or
systems-level agent written in C can speak MCP without dragging a Node
or Python runtime alongside it.

The protocol layer is generic; the cRAG server (`tools/crag-mcp/`) is
one example consumer and is built separately, behind an explicit
`make crag-mcp` target.

## Architecture

One source tree, three static link targets in a share-and-stack
pattern — both sides of an MCP conversation share ~60% of the code
(framing, schemas, transports, capability negotiation), so that lives
in the core:

```
            ┌──────────────────────────────────────────────┐
            │               libcmcp_core.a                 │
            │   JSON · JSON-RPC 2.0 · schema validator ·   │
            │   types · stdio + Streamable HTTP transports │
            └───────────────┬──────────────┬───────────────┘
                            │              │
            ┌───────────────┴───┐      ┌───┴────────────────┐
            │  libcmcp_server.a │      │  libcmcp_client.a  │
            │  tool/resource/   │      │  async reader-     │
            │  prompt registry, │      │  thread demux,     │
            │  worker-pool      │      │  multi-server      │
            │  dispatch         │      │  session           │
            └───────────────────┘      └────────────────────┘
```

Concurrency model: one reader thread per transport, handlers on a
small worker pool with cooperative cancellation + timeout watchdog,
single writer mutex per transport — no partial frames. Layering,
threading, and memory-ownership rules are in
[`docs/architecture.md`](docs/architecture.md).

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

## Use it with Claude Code

The reference servers are ordinary MCP stdio servers, so any MCP host
can drive them. With [Claude Code](https://claude.com/claude-code):

```bash
make
mkdir -p /tmp/fs-sandbox
claude mcp add cmcp-fs -- \
    "$PWD"/tools/filesystem-mcp/filesystem-mcp --root /tmp/fs-sandbox
claude mcp add cmcp-echo -- "$PWD"/examples/echo-server
```

From the next session, Claude lists and calls the C servers' tools
like any other MCP server. To capture the wire traffic of a session
(the format the replay gate consumes), wrap the server in `cmcp-tee`:

```bash
claude mcp add cmcp-fs-tee -- \
    "$PWD"/tools/cmcp-tee/cmcp-tee /tmp/fs-wire.jsonl \
    "$PWD"/tools/filesystem-mcp/filesystem-mcp --root /tmp/fs-sandbox
```

The playbooks under [`conformance/playbooks/`](conformance/playbooks/)
are scripted task sets for exactly this loop — they are how the
[symlink sandbox-escape P0](docs/case-study-symlink-escape.md) was
found.

## What ships

| Target | Purpose |
|---|---|
| `libcmcp_core.a`     | JSON, JSON-RPC 2.0, schema validator, types + log levels, stdio + Streamable HTTP transports |
| `libcmcp_server.a`   | Tools / resources / prompts registries, worker-pool dispatch, handshake, lifecycle, server-initiated notifications, cooperative cancellation + progress, server→client requests (elicitation), structured tool output, structured logging |
| `libcmcp_client.a`   | Async client with reader thread, `connect_stdio`, typed tool-call API, sampling + roots + elicitation host handlers, host-side cancel + per-call progress, multi-server `cmcp_session_t` with pagination |
| `tools/cmcp-inspect/`     | CLI: spawn a server, dump tools / resources / prompts, call one |
| `tools/filesystem-mcp/`   | Reference server: bounded-root filesystem reads/writes. No external deps; ships in `make`. `fs_write` hardened with `O_NOFOLLOW` + `lstat` after an agent-driven playbook pass surfaced a symlink-leaf sandbox-escape P0. |
| `tools/crag-mcp/`         | Reference server wrapping [cRAG](https://github.com/Simon-oid/cRAG) — two tools (`crag_search`, `crag_stats`) plus the `crag://stats` resource. Built behind a separate `make crag-mcp` target. |
| `tools/cmcp-tee/`         | Transparent stdio MCP proxy. Tees every wire frame in both directions to a JSONL log; the capture format the replay gate consumes. Links no cMCP libs. |
| `examples/echo-server.c`     | A two-tool server in <80 lines |
| `examples/minimal-client.c`  | Spawn a server, list tools, call `echo` |

## Protocol coverage

Tracking [MCP spec date `2025-11-25`](https://modelcontextprotocol.io/specification/2025-11-25/)
(pinned in `include/cmcp.h`). The protocol surface has been complete
since v0.4 — every release after that was hardening and quality gates.
The table records the version each feature first shipped in:

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

The `2025-11-25` revision's wire-behaviour changes and optional
capabilities (icons, EnumSchema, URL elicitation, sampling
tools/toolChoice, SSE polling + `Last-Event-Id` resumption) all landed
in the v0.5 protocol bump.

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
`Doxyfile` via `make docs` and published to
[GitHub Pages](https://simon-oid.github.io/cMCP/) on every push to
`main`. The public surface and the versioning policy that governs it
are documented in [`docs/SEMVER.md`](docs/SEMVER.md) — including which
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

## Status & roadmap

Current release: **v0.10.0** (2026-06-12), pinned to MCP spec revision
`2025-11-25`. The protocol surface has been complete since v0.4;
everything after was hardening, quality gates, and host-API ergonomics
driven by real consumers. The full per-release history — including
the rationale behind each design decision — is in
[`CHANGELOG.md`](CHANGELOG.md).

### Known limitations (deliberate)

- **Linux-only.** The transports use `fork`/pipes/pthreads; Linux
  (x86-64 and aarch64) is what CI builds and tests. macOS and Windows
  are not supported.
- **OAuth 2.1 deferred** — no remote authenticated-server consumer in
  sight yet. TLS is terminator-only by design (run the HTTP transport
  behind nginx/caddy); rationale in
  [`docs/deployment-tls.md`](docs/deployment-tls.md).
- **`completion/complete` deferred** — argument autocomplete; agentic
  hosts don't drive completion menus.
- **`resources/templates/list` deferred** — needs an RFC 6570
  URI-template engine; no templated-server consumer yet.

Each deferral is a scope decision, not a gap discovered late — the
rationale for each is recorded in [`CHANGELOG.md`](CHANGELOG.md).

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

## License

MIT. See [`LICENSE`](LICENSE).
