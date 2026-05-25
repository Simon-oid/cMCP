# cMCP

[![CI](https://github.com/Simon-oid/cMCP/actions/workflows/ci.yml/badge.svg)](https://github.com/Simon-oid/cMCP/actions/workflows/ci.yml)

A from-scratch implementation of the [Model Context Protocol](https://modelcontextprotocol.io/)
in pure C11. Three static link targets — core, server, client — sharing
one JSON-RPC 2.0 pipeline. stdio and Streamable HTTP transports.
Tracking spec revision `2025-11-25`; all optional capabilities of the
prior `2025-06-18` revision shipped, with the 2025-11-25 wire-behavior
changes landing in Tier 6.1 (in progress).

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

**Tier 6 in progress (state-of-the-art library polish).** Builds on
Tier 5's agentic-readiness work (sanitisers, fuzzing, replay gate,
playbooks, etc.). Tier 6.1 is the spec bump to `2025-11-25` — pin
landed in `include/cmcp.h`; spec-compliance wire changes for
input-validation-as-tool-error, HTTP Origin allow-list, and the
optional `description` field on `Implementation` are wired here.
Optional 2025-11-25 capabilities (icons, EnumSchema, URL elicitation,
sampling `tools`/`toolChoice`, SSE polling) land in 6.1.2-6.1.4. See
[`TODO.md`](TODO.md) under "Tier 6" and the design plan in
`~/.claude/plans/eager-leaping-pike.md`.

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
make test       # build and run the test binaries — currently 2716 assertions across 22 binaries
make valgrind   # same, under valgrind (leak-free)
make test-asan  # full rebuild under -fsanitize=address,undefined; runs suite
make test-tsan  # full rebuild under -fsanitize=thread; runs suite
make replay     # wire-fixture regression gate (conformance/replay/)
make fuzz-smoke # 60s per libFuzzer harness against the seed corpus (clang-only)
make soak       # stdio soak driver (env knobs in tests/soak/run.sh)
make crag-mcp   # build the cRAG reference server (needs sibling ../cRAG/)
make conformance # cross-check vs the MCP TS reference impl (needs Node + network)
make check-spec-drift # compare CMCP_PROTOCOL_VERSION vs upstream spec dirs
make clean
```

`make test`, `make test-asan`, `make test-tsan`, and `make replay` are
hermetic and offline; CI runs all four on every push.
`make conformance` is the heavyweight opt-in — it `npm install`s
Anthropic's pinned TypeScript reference SDK and runs cMCP against it
in both directions; see [`conformance/README.md`](conformance/README.md).
`make soak`, `make fuzz-*`, and `make check-spec-drift` are also
opt-in (long-running, clang-only, or network-touching respectively).

System dependency: `libcurl` headers (`pkg-config --cflags libcurl`
must work) — used by the Streamable HTTP client transport. Everything
else is hand-rolled or system-standard.

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

Tracking [MCP spec date `2025-11-25`](https://modelcontextprotocol.io/specification/2025-11-25/) (pinned in `include/cmcp.h`). 2025-11-25 wire-behavior changes are landing across the Tier 6.1 sub-axes.

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
tools/        cmcp-inspect (CLI), filesystem-mcp + crag-mcp (reference servers),
              cmcp-tee (wire-capture proxy for replay-gate fixtures)
examples/     echo-server, minimal-client
tests/        one binary per test_*.c (22 total), all use tests/test.h
tests/soak/   long-running stability harness (Tier 5.6)
fuzz/         libFuzzer harnesses + seed corpora (Tier 5.4)
conformance/  cross-check vs MCP TS SDK; replay-based regression gate
              over captured wire transcripts; agent playbooks
scripts/      tooling (spec-version drift watch)
docs/         architecture, schema-subset, agentic-readiness plan,
              spec-version-upgrade checklist
```

Architecture, threading model, and ownership rules are in
[`docs/architecture.md`](docs/architecture.md). The supported
JSON Schema subset is in [`docs/schema-subset.md`](docs/schema-subset.md).
The Tier-5 quality plan is in
[`docs/agentic-readiness.md`](docs/agentic-readiness.md).

## License

MIT. See [`LICENSE`](LICENSE).
