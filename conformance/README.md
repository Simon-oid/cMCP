# Conformance harness

Cross-checks cMCP against Anthropic's **MCP TypeScript reference
implementation**, in both wire roles. It answers one question: *does
cMCP speak the same protocol the reference SDK does?*

This is **not** part of the cMCP library, and not part of `make test`.
`make test` is hermetic and offline; this harness needs Node and
network access (it `npm install`s the reference SDK), so it lives
behind its own opt-in target.

## Run it

```bash
make conformance
```

That target:

1. builds the library + `examples/echo-server` + the C driver;
2. `npm install`s the pinned reference packages into
   `conformance/node_modules/` (first run only — network required);
3. runs both directions and fails on the first divergence.

## Prerequisites

- Node.js + npm (tested with Node 24 / npm 11).
- Network access for the initial `npm install`. Once
  `conformance/node_modules/` exists, re-runs are offline.

## What it checks

**Direction A — `client_vs_ts.c`: cMCP client vs TS reference server.**
Spawns `@modelcontextprotocol/server-everything` over stdio via
`cmcp_client_connect_stdio` and drives it with the cMCP client library:
handshake + protocol-version capture, `tools/list` + `tools/call`
(`echo` and a two-number sum tool), `resources/list` + `resources/read`,
`prompts/list` + `prompts/get`, and `progressToken` progress
notifications. Every assertion proves cMCP's **client** emits what the
reference **server** expects.

**Direction B — `client_vs_cmcp.mjs`: TS reference client vs cMCP
server.** Uses `@modelcontextprotocol/sdk`'s `Client` +
`StdioClientTransport` to spawn `examples/echo-server`, run the
handshake, and exercise `tools/list` / `tools/call` (including a
schema-violation rejection). The SDK validates every response against
its own schemas, so a shape divergence throws before the asserts run —
this proves cMCP's **server** emits what the reference **client**
expects.

## Notes

- **Pinned versions.** `package.json` pins the reference SDK and server.
  `node_modules/` and `package-lock.json` are gitignored; `package.json`
  and the two harness sources are tracked.
- **Tolerant of reference drift.** `server-everything`'s tool set
  changes between releases (e.g. `add` became `get-sum`). Direction A
  discovers tools by a small alias list and only hard-asserts the
  stable `echo` + sum tool; capability-specific checks self-skip when
  the reference server no longer offers them.
- **Scope.** TypeScript reference only. The Python SDK is a separate
  implementation of the same wire spec — redundant for pure conformance
  and deliberately out of scope.
