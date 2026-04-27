# cMCP

A from-scratch implementation of the [Model Context Protocol](https://modelcontextprotocol.io/)
in pure C11. One library, two link targets (server + client), shared
JSON-RPC 2.0 core. stdio transport in v0.1; Streamable HTTP in v0.2.

No C++. No external JSON library. No third-party MCP SDK. The only
runtime dependency is libcurl. SQLite is pulled in only by the cRAG
reference server.

## Why

MCP is the standard interface that lets an LLM agent call tools (file
systems, databases, search, custom services) without each agent needing
a custom integration per tool. The reference SDKs are written in
TypeScript and Python. cMCP is the C answer — built so an embedded /
systems agent (in this case [openclawd](https://github.com/Simon-oid/openclawd))
can speak MCP without dragging a Node or Python runtime alongside it.

Built as a portfolio piece. The protocol layer is generic; the cRAG
server (`tools/crag-mcp/`) is one example consumer and is built
separately.

## Status

**Pre-v0.1 — scaffolding only.** Roadmap and architecture are locked;
implementation starts at Phase 1.1 (JSON layer). See [`TODO.md`](TODO.md)
for the full phasing.

## Build & test

```bash
make            # produces libcmcp_core.a, libcmcp_server.a, libcmcp_client.a
                # plus cmcp-inspect and the two example binaries
make test       # builds and runs all tests
make valgrind   # same, under valgrind (optional)
make crag-mcp   # builds the cRAG reference server (needs sibling ../cRAG/)
make clean
```

System dependency: `libcurl` headers (`pkg-config --cflags libcurl`
must work). Everything else is hand-rolled or system-standard.

## What ships

| Target | Purpose |
|---|---|
| `libcmcp_core.a`   | JSON, JSON-RPC 2.0, schema validator, transports, types |
| `libcmcp_server.a` | Tool/resource/prompt registry, dispatch, lifecycle |
| `libcmcp_client.a` | Connect, initialize, list/call/read/subscribe, multi-server multiplex |
| `tools/cmcp-inspect/` | CLI: spawn a server, dump tools/resources/prompts, call a tool |
| `tools/crag-mcp/`     | Reference server wrapping [cRAG](https://github.com/Simon-oid/cRAG) |
| `examples/echo-server.c`     | A one-tool server in <100 lines |
| `examples/minimal-client.c`  | Connect, list tools, call one |

## Protocol coverage

Tracking [MCP spec date `2025-06-18`](https://modelcontextprotocol.io/specification/) (pinned in `include/cmcp.h`).

| Feature | v0.1 | v0.2 | v0.3 |
|---|---|---|---|
| stdio transport               | ✓ | ✓ | ✓ |
| Streamable HTTP transport     |   | ✓ | ✓ |
| Tools (list, call)            | ✓ | ✓ | ✓ |
| Resources (list, read, subscribe) |   | ✓ | ✓ |
| Prompts (list, get)           |   | ✓ | ✓ |
| Sampling (host-side)          |   | ✓ | ✓ |
| Roots (host-side)             |   | ✓ | ✓ |
| Notifications (`*/list_changed`, `resources/updated`) |   | ✓ | ✓ |
| `$/cancel`, `$/progress`      |   | ✓ | ✓ |
| OAuth 2.1 (HTTP only)         |   |   | ✓ |
| Multi-server multiplexing (client) | ✓ | ✓ | ✓ |

## Layout

```
include/    public headers
src/        json, rpc, schema, transports, server, client
tools/      cmcp-inspect (CLI), crag-mcp (reference server)
examples/   echo-server, minimal-client
tests/      unit + integration; test_stdio_roundtrip forks a real child
docs/       architecture, design notes, benchmarks
```

Architecture and design constraints live in [`CLAUDE.md`](CLAUDE.md).

## License

MIT. See [`LICENSE`](LICENSE).
