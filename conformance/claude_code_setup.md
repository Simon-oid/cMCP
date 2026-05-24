# Driving cMCP servers from Claude Code

This is the operational guide for phase 5.2 — "real agent in the loop."
Claude Code is the host; the in-tree cMCP servers are the test
subjects. Every Claude Code session that calls one of these servers
exercises the server side of the library (cmcp_server + core + the
stdio transport) under realistic-but-unpredictable load that no
offline test can synthesise.

The matching client-side coverage (cmcp_client) lives in
`conformance/client_vs_ts.c`, which runs the cMCP client against the
TS reference SDK server. The two together cover both wire roles.

## 1. Build the servers

```sh
make                          # echo-server, filesystem-mcp, cmcp-tee
make crag-mcp                 # also crag-mcp, if cRAG is at $CRAG_DIR
```

You should now have:

- `examples/echo-server`           — 2 tools (`echo`, `add`); the smoke test
- `tools/filesystem-mcp/filesystem-mcp` — 4 tools (`fs_list`, `fs_read`, `fs_stat`, `fs_write`); the "second consumer" that keeps the API honest
- `tools/crag-mcp/crag-mcp`        — 2 tools (`crag_search`, `crag_stats`) + 1 resource (`crag://stats`); the realistic retrieval workload
- `tools/cmcp-tee/cmcp-tee`        — transparent wire-capture proxy

## 2. Register a server with Claude Code

User scope (available in every Claude Code session you start as
this user):

```sh
claude mcp add --scope user cmcp-echo \
    /absolute/path/to/cMCP/examples/echo-server
```

Project scope (available only when Claude Code is started inside this
repo — written to `.mcp.json`, which is gitignored here for exactly
this reason):

```sh
claude mcp add --scope project cmcp-echo \
    /absolute/path/to/cMCP/examples/echo-server
```

Start a new Claude Code session. The server appears in `/mcp` with its
tools listed. Ask Claude Code to use one to confirm wiring:

> Use the cmcp-echo "echo" tool to repeat the phrase "hello".

If `/mcp` shows the server but the call hangs or errors, check the
server's stderr — by default it goes to wherever Claude Code routes
stderr (usually `~/.claude/projects/.../mcp-logs/`).

## 3. Capture wire transcripts with cmcp-tee

For any session you want to keep as a regression fixture, swap the
real server out for the tee wrapper. Same `claude mcp add` shape,
but the binary is `cmcp-tee` and its first argument is the log path,
followed by the real server path and any args:

```sh
claude mcp add --scope user cmcp-echo-tee \
    /absolute/path/to/cMCP/tools/cmcp-tee/cmcp-tee \
    /tmp/cmcp-echo-wire.jsonl \
    /absolute/path/to/cMCP/examples/echo-server
```

Now run any session. `/tmp/cmcp-echo-wire.jsonl` will fill up with
one line per wire frame. The wrapper:

- Adds **zero** to the message contents — frames are forwarded
  unchanged byte-for-byte.
- Preserves frame boundaries (newline-delimited per the stdio
  transport spec). The trailing newline is stripped before the
  frame is embedded in the log.
- Opens the log in append mode, so multiple sessions accumulate
  (separated by their `t` timestamps).
- Does not touch the server's stderr — let Claude Code see it or
  redirect from your shell.

### Log format

One JSON object per line:

```jsonl
{"t":1764019200.123456,"dir":"in", "frame":"{...full client request...}"}
{"t":1764019200.123890,"dir":"out","frame":"{...full server response...}"}
```

- `t`     — unix time (float seconds, microsecond precision)
- `dir`   — `"in"` (host → server) or `"out"` (server → host)
- `frame` — JSON-string-escaped raw frame body, without the trailing `\n`

This is the format the replay-based conformance gate (phase 5.3
finalisation) will consume.

## 3a. Running mode: in-band vs shell

Not every playbook task is best executed *through* a Claude Code
session. Tasks whose payload is small and where the question is "does
the agent understand and use the tool right" belong in-band. Tasks
whose payload is large or repetitive, or where the question is purely
"does the server handle N bytes / this edge case correctly", should
be run directly via shell pipe — Claude Code becomes a costly
middleman otherwise.

**Cost note:** every byte of a tool argument and every byte of the
response goes through the model's context window four times (your
prompt → tool call → server response → assistant context). Repetitive
content tokenises poorly — 1024 identical ASCII characters can cost
~700-1000 tokens per pass, ~3-4K per full round trip. A 8000-byte
echo test is easily 30K+ tokens.

**Rule of thumb:**

| Test shape                                              | Mode    |
|---------------------------------------------------------|---------|
| Small string / typical-size args, agent-behaviour focus | in-band |
| Schema-violating call, error-surface focus              | in-band |
| Tool-choice / discovery / multi-step reasoning          | in-band |
| Payload >~256 chars OR highly repetitive                | shell   |
| Pure server-side correctness (no agent reasoning value) | shell   |

Tasks that should run via shell are marked accordingly inside the
per-server playbooks (look for "**Mode: shell**" next to the task
title). The standard shell incantation for an echo-server boundary
test is:

```sh
# Generate the payload offline, then drive the server directly
python3 -c 'print("a"*8000, end="")' > /tmp/big.txt

( printf '{"jsonrpc":"2.0","id":1,"method":"initialize","params":{"protocolVersion":"2025-06-18","capabilities":{},"clientInfo":{"name":"t","version":"0"}}}\n'
  printf '{"jsonrpc":"2.0","method":"notifications/initialized"}\n'
  printf '{"jsonrpc":"2.0","id":2,"method":"tools/call","params":{"name":"echo","arguments":{"text":"%s"}}}\n' "$(cat /tmp/big.txt)"
  sleep 0.2
) | ./examples/echo-server | tail -1 | tee /tmp/echo.out | wc -c
```

Zero tokens consumed, same server, faithful test.

## 4. Run the playbooks

Each in-tree server has a playbook of ~10 tasks designed to exercise
its surface broadly enough to flush out tool-description issues, error
messages that don't teach the model anything, and protocol edge cases.

- [`playbooks/echo-server.md`](playbooks/echo-server.md)
- [`playbooks/filesystem-mcp.md`](playbooks/filesystem-mcp.md)
- [`playbooks/crag-mcp.md`](playbooks/crag-mcp.md)

Run each task in a fresh Claude Code session (or at least after
`/clear`) so the agent's prior context doesn't contaminate the
result. For any task that misbehaves, the wire transcript from
cmcp-tee is the artefact to attach when filing the issue — drop it
under `conformance/fixtures/<server>/` with a name that explains
what went wrong (e.g. `fs_read_offset_off_by_one.jsonl`).

## 5. Triage rule

When a task fails, attribute the failure to one specific layer:

| Symptom                                       | File against                    |
|-----------------------------------------------|---------------------------------|
| Wire format violates MCP spec                 | cMCP server bug                 |
| Host called a method we don't speak           | cMCP missing capability         |
| Model gives up before trying the tool         | Tool **description** is unclear |
| Model retries the same failing call ≥3 times  | Tool **error message** is unclear|
| Model uses tool but mis-parses the result     | Tool **output shape** is unclear|
| Host crashes / disconnects mid-session        | cMCP transport bug              |

Each row maps to the layer that should be edited to fix the issue.
"Tool description unclear" is a *fixable* outcome — edit the
`.description` field on the tool registration. Same for error
messages: a `cmcp_schema_error_t` should tell the model what to
change, not just that it was wrong.
