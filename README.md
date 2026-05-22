# cMCP

A from-scratch implementation of the [Model Context Protocol](https://modelcontextprotocol.io/)
in pure C11. Three static link targets — core, server, client — sharing
one JSON-RPC 2.0 pipeline. stdio transport in v0.1; Streamable HTTP in
v0.2.

No C++. No external JSON library. No third-party MCP SDK. Hand-rolled
JSON parser, hand-rolled JSON Schema subset validator, hand-rolled
JSON-RPC framing. The only runtime dependency is libcurl (for the
HTTP transport in v0.2; v0.1 uses it only because the build links it
unconditionally for forward compatibility). SQLite is pulled in only
by the cRAG reference server.

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

**v0.2 protocol surface — complete.** stdio + Streamable HTTP
transports, tools / resources / prompts on the server side, sampling
and roots on the host side, server-initiated notifications with
subscriber-aware filtering, multi-server multiplexing on the client.
A C host can spawn or connect to one or more MCP servers, run the
initialize handshake, list and call their tools, read and subscribe
to resources, get prompts, route notifications back, and present a
unified `<server>:<tool>` namespace across them. Async by design —
one reader thread per client, multiple in-flight calls, any-order
completion.

**v0.3 in progress.** The server now dispatches handler-invoking calls
(`tools/call`, `resources/read`, `prompts/get`) onto a worker pool, so
a slow handler can't stall the run loop and several run concurrently.
Handlers get cooperative `notifications/cancelled`, `progressToken`
progress, and a per-handler timeout watchdog. A second, non-cRAG
reference server (`tools/filesystem-mcp/`) also ships, and `make
conformance` cross-checks cMCP — in both wire roles — against the MCP
TypeScript reference implementation.

Tier 3 remaining (OAuth 2.1, connection pooling) is deferred — neither
blocks real consumers. See [`TODO.md`](TODO.md) for the full phasing.

## Build & test

```bash
make            # libs (core/server/client) + cmcp-inspect + examples
make test       # build and run the test binaries — currently 2422 assertions across 15 binaries
make valgrind   # same, under valgrind (optional)
make crag-mcp   # build the cRAG reference server (needs sibling ../cRAG/)
make conformance # cross-check vs the MCP TS reference impl (needs Node + network)
make clean
```

`make test` is hermetic and offline. `make conformance` is the opt-in
exception — it `npm install`s Anthropic's pinned TypeScript reference
SDK and runs cMCP against it in both directions; see
[`conformance/README.md`](conformance/README.md).

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
Connected: echo-server 0.1.0 (protocol 2025-06-18)

Tools (2):
  echo  -  Return the input text unchanged.
      - text: string (required)
  add  -  Add two integers.
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
| `libcmcp_core.a`     | JSON, JSON-RPC 2.0, schema validator, types, stdio + Streamable HTTP transports |
| `libcmcp_server.a`   | Tools / resources / prompts registries, worker-pool dispatch, handshake, lifecycle, server-initiated notifications, cooperative cancellation + progress |
| `libcmcp_client.a`   | Async client with reader thread, `connect_stdio`, sampling + roots host handlers, multi-server `cmcp_session_t` |
| `tools/cmcp-inspect/`     | CLI: spawn a server, dump tools, call one |
| `tools/crag-mcp/`         | Reference server wrapping [cRAG](https://github.com/Simon-oid/cRAG) — two tools (`crag_search`, `crag_stats`) plus the `crag://stats` resource |
| `examples/echo-server.c`     | A two-tool server in <80 lines |
| `examples/minimal-client.c`  | Spawn a server, list tools, call `echo` |

## Protocol coverage

Tracking [MCP spec date `2025-06-18`](https://modelcontextprotocol.io/specification/) (pinned in `include/cmcp.h`).

| Feature | v0.1 | v0.2 | v0.3 |
|---|---|---|---|
| stdio transport                          | ✓ | ✓ | ✓ |
| Streamable HTTP transport                |   | ✓ | ✓ |
| Tools (list, call)                       | ✓ | ✓ | ✓ |
| Multi-server multiplexing (client)       | ✓ | ✓ | ✓ |
| Server-to-client notifications (routing) | ✓ | ✓ | ✓ |
| Resources (list, read, subscribe)        |   | ✓ | ✓ |
| Prompts (list, get)                      |   | ✓ | ✓ |
| Sampling (host-side)                     |   | ✓ | ✓ |
| Roots (host-side)                        |   | ✓ | ✓ |
| `*/list_changed` emit (server side)      |   | ✓ | ✓ |
| `notifications/cancelled`, `progressToken` |   |   | ✓ |
| OAuth 2.1 (HTTP only)                    |   |   |   |

## Layout

```
include/    public headers (cmcp.h, cmcp_json.h, cmcp_types.h,
            cmcp_transport.h, cmcp_schema.h, cmcp_server.h,
            cmcp_client.h, cmcp_session.h)
src/        json, rpc, schema, types,
            transport_stdio, transport_http (server), transport_http_client
            → libcmcp_core.a
            server.c → libcmcp_server.a
            client.c, session.c → libcmcp_client.a
tools/      cmcp-inspect (CLI), crag-mcp (reference server)
examples/   echo-server, minimal-client
tests/      one binary per test_*.c (15 total), all use tests/test.h
docs/       architecture, schema-subset, design notes
```

Architecture, threading model, and ownership rules are in
[`docs/architecture.md`](docs/architecture.md). The supported
JSON Schema subset is in [`docs/schema-subset.md`](docs/schema-subset.md).

## License

MIT. See [`LICENSE`](LICENSE).
