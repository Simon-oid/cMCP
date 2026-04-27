# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## What this project is

cMCP is a from-scratch implementation of the [Model Context Protocol](https://modelcontextprotocol.io/) (MCP) in pure C11. It is a **library**, not an application — it ships three static link targets (`libcmcp_core.a`, `libcmcp_server.a`, `libcmcp_client.a`) plus reference binaries.

cMCP is the protocol layer used by **openclawd** (the user's Claude-Code-style C agent) to call tools. The first such tool is **cRAG**, wrapped by the in-tree reference server `tools/crag-mcp/`.

cMCP is intentionally *not* cRAG-shaped: cRAG is one example consumer, not a privileged dependency. Resist any temptation to add cRAG-specific code paths inside `src/`.

## Build & Test

```bash
make                  # build libs + cmcp-inspect + examples
make test             # build and run all tests
make valgrind         # run all tests under valgrind (requires valgrind installed)
make crag-mcp         # build the cRAG reference server (requires sibling ../cRAG)
make crag-mcp CRAG_DIR=/path/to/cRAG   # override cRAG location
make clean            # remove all objects, libs, and binaries
```

Run a single test binary directly after `make test`:
```bash
./tests/test_json
./tests/test_rpc
./tests/test_schema
./tests/test_lifecycle
./tests/test_stdio_roundtrip
./tests/test_client_server
```

System dependency: `libcurl` headers (`pkg-config --cflags libcurl` must work). Everything else is hand-rolled or system-standard. No third-party MCP SDK; no third-party JSON library.

## Architecture

cMCP is one source tree with three link targets, share-and-stack pattern:

```
libcmcp_core.a    json + rpc + schema + types + transports
libcmcp_server.a  + server.c + notif.c     (depends on core)
libcmcp_client.a  + client.c               (depends on core)
```

Both the server and client sides of an MCP conversation share ~60% of the code (JSON-RPC framing, message schemas, transports, capability negotiation), so they live in the core. The asymmetric pieces (registries on the server, multiplexing on the client) live in their respective libs.

The pipeline has six layers, each in its own module:

1. **JSON** (`src/json.c`) — Hand-parsed/emitted JSON. Lifted as a starting point from cRAG's `src/util.c`. Extended with object construction, escape-aware string emission, and stable key ordering for testability.

2. **JSON-RPC 2.0** (`src/rpc.c`) — Request/response/notification framing. In-flight ID table for request/response matching. Single monotonic ID space per session. Standard error codes (`-32700/-32600/-32601/-32602/-32603`) plus MCP-specific range.

3. **Schema** (`src/schema.c`) — JSON Schema *subset* validator for tool inputs: `type`, `properties`, `required`, `enum`, primitive types, `minLength`/`maxLength`. No `$ref` or `oneOf` in v0.1. Validates on both sides — server before dispatch, client before send.

4. **Transport** (`src/transport_*.c`) — Plugin architecture via `cmcp_transport_t` vtable (`read`, `write`, `close`). Two backends: `stdio` (newline-delimited JSON over stdin/stdout) and `http` (Streamable HTTP, Phase 3). The transport layer never touches message semantics — it just moves frames.

5. **Server / Client** (`src/server.c`, `src/client.c`) — The asymmetric halves.
   - Server: tool/resource/prompt registry, dispatch, lifecycle, notifications.
   - Client: connect, initialize handshake, list/call/read/subscribe, multi-server multiplexing.

6. **Reference binaries** (`tools/`):
   - `cmcp-inspect` — CLI client. Spawns a server, runs the handshake, lists everything, optionally calls a tool.
   - `crag-mcp` — Reference server wrapping cRAG. Three tools: `crag_query`, `crag_stats`, `crag_index` (the last gated by env var).

**Lifecycle data flow:** transport `read()` → JSON parse → JSON-RPC dispatch → (server) registry lookup + schema validate + handler call + response build, OR (client) ID match against pending request table + completion callback.

## Key Design Constraints

- **Pure C11**, no C++ or external JSON libraries. JSON is hand-parsed in `src/json.c`.
- **All HTTP via libcurl** (client side); server-side HTTP is hand-rolled on top of `socket()` + a tiny request parser.
- **Logging goes to stderr only.** Never write to stdout — the stdio transport owns it, and any stray write corrupts the wire. Configurable level via `CMCP_LOG_LEVEL`.
- **Concurrency:** one reader thread per transport, handlers run on a small thread pool, single writer mutex per transport. No partial frames.
- **Memory ownership:** caller owns arguments to handlers; library owns response builders, freed after send. Manual `malloc`/`free` throughout.
- **Spec compliance over shortcuts.** Match the MCP spec's wire format exactly. Pin one protocol version per release (currently `CMCP_PROTOCOL_VERSION` in `include/cmcp.h`); `initialize` rejects mismatches with a clear error.
- **Negative return values are error codes** (`cmcp_err_t` in `include/cmcp.h`). Library never `exit()`s — handlers must survive single bad calls without dying.

## Environment Variables

| Variable | Purpose |
|---|---|
| `CMCP_LOG_LEVEL` | `error`, `warn`, `info`, `debug`, `trace` (default: `warn`) |
| `CMCP_LOG_JSON` | If `1`, log lines are JSON objects on stderr (helps cross-wire debugging) |
| `CMCP_PROTOCOL_STRICT` | If `1`, refuse any spec-deviating message instead of best-effort parse |
| `CMCP_HANDLER_TIMEOUT_MS` | Default per-handler timeout (default: 30000) |

## Test Framework

Tests use a minimal macro framework in `tests/test.h` (same shape as cRAG's: `TEST_ASSERT`, `TEST_RUN`, `TEST_DONE`). Tests create temporary fixtures under `/tmp/` and clean up after themselves. No mocking — `test_stdio_roundtrip` actually `fork()`s a child server and runs a real handshake; `test_client_server` runs both halves in-process over a `socketpair()`.

## What lives where

- `include/cmcp.h` — public umbrella header (version, error codes, top-level types)
- `include/cmcp_types.h` — JSON-RPC message types, capability structs
- `include/cmcp_transport.h` — transport vtable
- `src/` — implementation
- `tools/crag-mcp/` — reference server. Links cRAG from `$CRAG_DIR` (default `../cRAG`). **Not** built by `make all` — explicit `make crag-mcp`.
- `tools/cmcp-inspect/` — CLI client for poking at any MCP server
- `examples/` — minimal echo-server and minimal-client (~100 lines each, the "hello world")
- `tests/` — unit + integration tests
- `docs/` — architecture and benchmark notes
