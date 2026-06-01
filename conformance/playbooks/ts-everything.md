# Playbook: TS-SDK `server-everything` via `cmcp-inspect`

The other playbooks in this directory drive **in-tree** servers
(echo-server, filesystem-mcp, crag-mcp). This one points the cMCP client
at a **non-cMCP** server — the canonical TypeScript reference,
`@modelcontextprotocol/server-everything` — and walks it with the
`cmcp-inspect` reference binary. The goal (Phase C.1) is to enumerate every
place cMCP's client and a real third-party server diverge: argument shapes,
error models, capability surface, schema features.

## Relationship to `make conformance`

The cMCP **client library** is already cross-checked against this exact
server by `conformance/client_vs_ts.c` (run via `make conformance`):
handshake, `tools/list`+`call`, `resources/list`+`read`, `prompts/list`+
`get`, and progress notifications. As of `server-everything` 2026.1.26 /
`@modelcontextprotocol/sdk` 1.29 that gate is green — **32/32 assertions,
8/8 conformance checks**. So the library-level wire compatibility is a
tested fact, not a claim.

This playbook is the *complementary* axis: it exercises the **`cmcp-inspect`
CLI** (not the bespoke cross-check harness) so the binary an operator
actually runs is shown working against a foreign server, and it writes the
divergences down where a human reads them. By design `cmcp-inspect` drives
`tools/list` + `tools/call` only; the resources/prompts walk stays in
`client_vs_ts.c` (extending the CLI to list resources/prompts is deferred
feature-work, not in scope for a hardening pass).

## Setup

The reference SDK is vendored under `conformance/node_modules/` after the
first `make conformance` (it installs the pinned versions). Spawn it over
stdio directly — `cmcp-inspect` forks+execs the server and runs the
handshake:

```sh
EV=conformance/node_modules/@modelcontextprotocol/server-everything/dist/index.js

# List the tool surface (human-readable):
./tools/cmcp-inspect/cmcp-inspect -- node "$EV" stdio

# Call one tool (raw JSON-RPC result):
./tools/cmcp-inspect/cmcp-inspect --json -c get-sum -a '{"a":2,"b":3}' -- node "$EV" stdio
```

`npx -y @modelcontextprotocol/server-everything stdio` works as the server
command too, if you'd rather not depend on the vendored copy.

---

## Walk (observed 2026-06-01, server-everything 2.0.0, protocol 2025-11-25)

### W1. Handshake

```
Connected: mcp-servers/everything 2.0.0 (protocol 2025-11-25)
```

The server advertises **the same** protocol version cMCP pins
(`CMCP_PROTOCOL_VERSION` == `2025-11-25`), so the handshake is clean. (If a
future SDK bumps its version, recall cMCP's contract: a version mismatch is
*not* a handshake failure — the version is captured and the host decides;
see CLAUDE.md "Spec compliance".)

### W2. `tools/list` — 13 tools

`echo`, `get-annotated-message`, `get-env`, `get-resource-links`,
`get-resource-reference`, `get-structured-content`, `get-sum`,
`get-tiny-image`, `gzip-file-as-resource`, `toggle-simulated-logging`,
`toggle-subscriber-updates`, `trigger-long-running-operation`,
`simulate-research-query`. `cmcp-inspect` renders each name, title,
description, and required properties without error.

### W3. `tools/call` — representative happy paths

| Call | Result |
|---|---|
| `get-sum {a:2,b:3}` | `{"content":[{"type":"text","text":"The sum of 2 and 3 is 5."}]}` |
| `echo {message:"hi"}` | content text `"Echo: hi"` |
| `get-structured-content {location:"New York"}` | content + `structuredContent` against the tool's `outputSchema` |

---

## Divergences enumerated

Each is tagged **[cMCP bug]**, **[TS quirk]**, or **[spec ambiguity]** per
the C.1 acceptance criteria. The headline finding: **none are cMCP bugs.**
The cMCP client round-trips everything the reference server emits.

### D1. Tool-input validation error model — **[spec ambiguity]**

Calling `get-structured-content {location:"Paris"}` (not in the server's
`enum`) returns a **tool result with `isError:true`** whose text embeds
`MCP error -32602: Input validation error: ...`. Calling a missing tool
(`no-such-tool`) likewise comes back as an `isError` content item carrying
`MCP error -32602: Tool ... not found`.

**Correction (2026-06-01, verified by the P6 host-probe — this overturns
an earlier draft of this note):** cMCP's own server splits the two cases,
and NOT the way CLAUDE.md's "Schema" section claims:

- **Tool-input schema failure** (e.g. `add` with a required arg missing)
  comes back as a **tool-level `isError` result** — `src/server.c:1177`
  builds `{isError:true, content:[{text:"Invalid arguments for tool 'add':
  ... (path: /b, keyword: required)"}]}`. It is NOT a `-32602` JSON-RPC
  error. So on bad *input*, cMCP behaves the SAME as the TS reference, not
  the opposite — the host-probe observed exactly this.
- **Unknown tool / bad `protocolVersion`** DO surface as real `-32602`
  JSON-RPC errors (`src/server.c:651`, confirmed by the probe:
  `code=-32602 msg="Unknown tool"`).

CLAUDE.md still says schema failures "surface as `-32602` with
{path,keyword,message} data" — that is **stale**; the data is right but it
rides the `isError` content channel, not the error channel. (Tracked as a
P6 finding to reconcile doc vs. code.)

**Consequence for a host:** to catch a bad call universally you must check
*both* `resp.error` (JSON-RPC: unknown tool, transport) *and*
`result.isError` (tool result: bad args, handler failure) — checking one
is not enough, against cMCP *or* the TS reference. cMCP's client exposes
both cleanly via the 3-way `cmcp_tool_outcome_t` (the probe got
`CMCP_TOOL_ERR_PROTOCOL` for the unknown tool and
`CMCP_TOOL_ERR_TOOL_LEVEL` for the missing arg).

### D2. Schema features beyond cMCP's validator subset — **[TS quirk / by design]**

The server's `inputSchema`s use keywords cMCP's **server-side** validator
subset does not implement: `$schema` (draft-07 URI), `default`,
`format:"uri"`, and numeric `minimum`/`maximum` (the last two cMCP *does*
support; `enum` it supports too). There is also a tool-level
`execution:{taskSupport:...}` field and a top-level `outputSchema`.

This is a non-issue **as a client**: cMCP doesn't validate a remote
server's inputSchema — the server does — so unknown keywords pass through
transparently and `cmcp-inspect` lists the tools fine. It would only bite
if cMCP were *hosting* these tools: its schema subset (docs/schema-subset.md)
can't express `default`/`format`/`$schema`/`execution`. No action — the
subset is a documented, deliberate v0.x scope, not a bug.

### D3. Naming drift across SDK versions — **[TS quirk]**

`get-sum` (was `add`), `trigger-long-running-operation` (was
`longRunningOperation`). Already known and absorbed: `client_vs_ts.c`
resolves tools by an alias list rather than a fixed name. Recorded here so
the next version bump doesn't re-surprise anyone.

### D4. Task-based tools — **[spec ambiguity / not modelled]**

`simulate-research-query` advertises `execution.taskSupport:"required"` and
`trigger-long-running-operation` streams progress. cMCP models progress
(host-side `cmcp_progress_fn`) but does **not** model the newer
*task* lifecycle (`taskSupport`). Calling a `taskSupport:"required"` tool as
an ordinary `tools/call` is therefore untested ground — left uncalled in
this walk on purpose (it may block awaiting task protocol). Flag for if/when
cMCP grows task support; not a v0.x gap.

---

## Verdict

`cmcp-inspect` drives a foreign, spec-current reference server end-to-end
with zero client-side defects. Every divergence is either a documented
cMCP scope boundary (D2), a benign upstream rename (D3), an unmodelled
forward-looking feature (D4), or a both-legal placement choice the host
must be aware of (D1 — the one with a real consumer-facing consequence,
worth a line in any host-integration guide).
