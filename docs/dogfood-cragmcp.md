# Dogfooding crag-mcp — D1 findings

Generated 2026-05-30 as the council-D1 deliverable (see
`~/.claude/plans/eager-leaping-pike.md` Tier 7 + the council verdict
captured in chat). The premise: drive `tools/crag-mcp/` end-to-end
through the public client API the way **butlerbot will**, and write
down every moment the API felt wrong from the host's seat.

The harness lives at `tools/dogfood-crag-host/main.c`. It is built
opt-in (`make dogfood-crag-host`) and not part of `make all` — its
output is **this doc**, not a binary anybody links.

## Setup used

```sh
# corpus (cMCP's own docs/ — 9 markdown files, 126 chunks, 1024-dim)
CRAG_EMBED_BACKEND=ollama CRAG_EMBED_MODEL=mxbai-embed-large \
  ../cRAG/crag index /home/user/cMCP/docs \
    --db /tmp/cmcp-dogfood/cmcp-docs.db --ext md --workers 2

# build
make crag-mcp dogfood-crag-host

# drive a session with the wire captured
LOG=conformance/fixtures/crag-mcp/dogfood/session-2026-05-30.jsonl
CRAG_EMBED_BACKEND=ollama CRAG_EMBED_MODEL=mxbai-embed-large \
  ./tools/dogfood-crag-host/dogfood-crag-host \
    --tee-log "$LOG" \
    --db /tmp/cmcp-dogfood/cmcp-docs.db
```

The harness exercises (in order): handshake, tools/list, resources/read
of `crag://stats`, 3 sync `crag_search` calls, 3 async parallel
`crag_search` calls with reverse-order waits, two schema-bound error
paths, an unknown-tool path, teardown. 25 wire frames captured to the
JSONL above.

---

## Findings

Each finding is one of:

- **API ergonomics** — the API works, but felt wrong from the host
  seat. Candidate for v0.6.x convenience helpers.
- **API safety** — easy to write wrong host code with no help from the
  type system or docs. Candidate for an API tightening.
- **Documentation drift** — code is right, docs are stale.
- **Observability gap** — the host can't see something it would need
  to reason well.

### F1 — Single-client typed helpers are missing (API ergonomics)

A host talking to **one** server has no typed shortcut for the three
operations every host needs:

| Op | What exists | What I had to write |
|---|---|---|
| list tools | only `cmcp_session_tools_list` (multi-server) | `cmcp_client_request("tools/list", NULL, &resp)` then walk `result.tools[]` via `cmcp_json_object_get` |
| read a resource | only `cmcp_session_resource_read` | `cmcp_client_request("resources/read", {uri}, &resp)` then walk `result.contents[0].text` |
| call a tool | only `cmcp_session_tool_call` | `cmcp_client_request("tools/call", {name, arguments}, &resp)` then walk `result.content[0]` + check `result.isError` (and `response.error`, see F2) |

Even our own `cmcp-inspect` drops to raw JSON walking for this — so
butlerbot will too. The session-layer types are correct; mirror them
onto `cmcp_client_t`:

```c
int cmcp_client_tools_list(cmcp_client_t *c,
                            cmcp_client_tool_t **out_tools, size_t *out_n);
void cmcp_client_tools_free(cmcp_client_tool_t *tools, size_t n);

int cmcp_client_resource_read(cmcp_client_t *c, const char *uri,
                               char **out_text, size_t *out_n);

int cmcp_client_tool_call(cmcp_client_t *c, const char *name,
                           cmcp_json_t *args /* consumed */,
                           cmcp_json_t **out_result,
                           int *out_is_error);
```

Effort: ~½ day. They're light wrappers over the existing async core.

### F2 — Error-model bifurcation has no client-side flattener (API ergonomics, **high value**)

A `tools/call` can fail two ways the host must distinguish:

1. **JSON-RPC error** (`response.error.code/message/data`):
   - `-32601 Method not found`
   - `-32601 Unknown tool` ← server returns this for unknown *tool name*
   - `-32603 Internal error`
   - `-32602 Invalid params` ← in some paths
2. **Tool-level error** (`response.result.isError == true` plus
   `response.result.content[].text` carrying a human-prose reason):
   - Schema rejections (`minLength`, `maximum`, etc.)
   - Handler-reported errors

There is no client helper that flattens these into a single
`(success | tool_error | protocol_error)` channel. I wrote that
flattener three times in the harness as ad-hoc code per call site.

**Worse:** the two channels carry *different shapes* of structured
data. JSON-RPC `-32602` errors come with `error.data = {path, keyword,
message}` (structured, host-machine-readable). Tool-level errors come
with `result.content[0].text = "Invalid arguments for tool ... (path:
/query, keyword: minLength)"` — the same fields, but inline-stringified.
A host that wants to react programmatically to a "minLength violation
on /query" must either parse a sentence, or look up which error
channel the server chose.

This is a real product gap, not a style preference. Recommended fix:

```c
typedef enum {
    CMCP_TOOL_OK,                /* success — *out_result populated */
    CMCP_TOOL_ERR_TOOL_LEVEL,    /* isError:true — *out_text populated */
    CMCP_TOOL_ERR_PROTOCOL,      /* JSON-RPC error — *out_rpc_err populated */
} cmcp_tool_outcome_t;

cmcp_tool_outcome_t
cmcp_client_tool_call(cmcp_client_t *c, const char *name,
                       cmcp_json_t *args,
                       cmcp_json_t **out_result,         /* on OK */
                       char **out_text,                  /* on TOOL_LEVEL */
                       cmcp_rpc_error_t **out_rpc_err);  /* on PROTOCOL */
```

Plus optionally, on the *server* side, surfacing the structured
`{path, keyword, message}` block from the schema validator into
`result.structuredContent` (or `_meta.errorData`) so tool-level
errors have machine-readable data too — that's a server-side change,
not part of the client API ask.

### F3 — Schema-rejection error channel is inconsistent (server-side design question)

`crag-mcp`'s schema rejection returns **tool-level** `isError:true`
(per MCP 2025-11-25 convention for tools/call). But:

- The MCP spec's `-32602 Invalid params` JSON-RPC error code still
  exists and is what other validators (e.g. raw `cmcp_server`
  schema-rejection paths) historically returned.
- The 5.2 playbook documentation (`conformance/playbooks/crag-mcp.md`,
  task T5) explicitly says the empty-query rejection returns "-32602
  schema error" — that documentation is **stale**.
- An unknown *tool name* returns `-32601 Unknown tool` (JSON-RPC
  level), which is the right level for "you named a thing that does
  not exist."
- An unknown *argument schema* returns `isError:true` (tool level),
  which means "your call cannot proceed because the args are wrong."

Both are "the call you asked for cannot proceed." The line between
them is fine. The spec (2025-11-25) does sort this out — schema
rejections are explicitly tool-level — but the post-6.1.4 fixture
sweep should have also updated the playbook. **Doc drift item.**

### F4 — Public `cmcp_json` struct layout invites the wrong API (API safety)

While writing the harness I three times reached for `text->str.n`,
`text->str.s`, `is_err->boolean` — all of which are wrong field
names. The actual fields are `.str.len`, `.str.s`, `.b`. The compiler
caught me, but only because the field names happened to be different.
If they had collided, this would be a silent ABI-fragility hazard
across versions.

`include/cmcp_json.h` exposes the full struct so the parser can
construct values cheaply. But it also exposes typed accessors
(`cmcp_json_string()`, `cmcp_json_string_len()`, `cmcp_json_bool()`,
`cmcp_json_array_len()`, `cmcp_json_array_at()`) that I should have
used from the start.

The header doesn't *signal* "prefer accessors." Add either:

1. A `@warning` block in the struct's Doxygen block explicitly
   directing host code to the accessors and reserving the union
   for parser/library use, **or**
2. Move the union behind an opaque `cmcp_json_impl_t` and make the
   accessors mandatory. (More work; better long-term.)

Recommend (1) for v0.6.x, defer (2).

### F5 — JSON helper naming is inconsistent (API ergonomics, paper cut)

Constructors are verb-first:

```
cmcp_json_new_object()
cmcp_json_new_string()
cmcp_json_new_int()
```

Setters and accessors are subject-first:

```
cmcp_json_object_set()
cmcp_json_object_get()
cmcp_json_string()
```

So you write:

```c
cmcp_json_object_set(p, "name", cmcp_json_new_string("foo"));
/*                  ^ subject-first         ^ verb-first */
```

It is small, but every host author will pause once. Pick one rule for
v0.7.0 (the next MAJOR-eligible release; this is an ABI break).
**Recommend** subject-first throughout (matches the accessors, which
are the more-frequently-called API):

```
cmcp_json_object_new()
cmcp_json_string_new()
cmcp_json_int_new()
```

Defer until v0.7.0 — paper-cut-grade, not blocking butlerbot.

### F6 — Async parallelism is wire-level only; the host can't reason about server concurrency (observability gap)

Three sync calls cost 18.6 ms / 15.2 ms / 14.7 ms = ~48 ms total.
Three async calls fired in parallel completed in **28.2 ms total**.
The parallelism is real but small — and the win shrinks if I fire
more, because `crag-mcp` funnels every search through a single Ollama
embed call that itself serializes.

The wire layer is correctly parallel (multiple in-flight ids, reader
thread demuxes, any-order completion). The bottleneck is in the
*server-side handler*. A host author looking at the protocol has no
way to know whether firing 10 calls in parallel will give 10×
throughput, 2× throughput, or 1× throughput.

This is an MCP-spec gap as much as a cMCP gap — there is no
capability flag like `"server": { "maxConcurrentToolCalls": N }`. But
cMCP **could** introduce a vendor-prefixed extension that surfaces
"this server's `tools/call` is effectively serial / pooled-N /
unbounded" so a host budgets accordingly. Park as a discussion item;
not v0.6 work.

### F7 — Cold-start ambient cost (observability, not a finding)

The first sync `crag_search` call cost 612 ms on the *first* run of
this session (when Ollama hadn't loaded the embed model). Subsequent
calls were 14-16 ms. The wire-level handshake itself cost 4-5 ms.

**Not a finding** — this is Ollama, not cMCP. Captured because a host
author will reflexively blame the transport, and they shouldn't.

---

## Bug-class verdict

There is no real bug in cMCP. The schema validator works (the
post-6.7 conformance corpus proved it; the playbook captured the
correct tool-level rejection shape; the harness initially misread the
result because I looked in the JSON-RPC error channel instead of the
tool-level channel). The two surprises in step 6 of the first
harness run (`""` and `k=999` "ACCEPTED") were both **harness bugs**,
not server bugs — the harness was looking in the wrong error channel.

That harness mistake **is** the finding (F2): if even the dogfooder
who wrote the library mis-reads the error model on first try,
butlerbot will too.

## What this points to for v0.6.0

The council's D2 task (write the v0.6.0 acceptance criterion) should
take F1 + F2 as the primary load-bearing input. They are:

- Small (~1-2 days combined).
- Honest (driven by integration pain, not spec-reading).
- Forward-compatible (additive client API, no break).
- Generate test fixtures naturally (the dogfood harness re-runs as a
  regression gate once the helpers land).

F3 (schema-error-channel doc drift) is a doc-only fix and can land
alongside F1+F2. F4 (struct layout warning) is a one-line doc
comment. F5 (naming) defers to v0.7.0. F6 (server concurrency hint)
parks as a Tier 8 discussion.

**Recommended v0.6.0 scope** (subject to D2 confirmation):

1. Land F1's three single-client typed helpers.
2. Land F2's `cmcp_client_tool_call` flattener with the three-way
   outcome enum.
3. Land F3 + F4 doc tightening alongside.
4. The dogfood harness becomes the v0.6.x acceptance gate: it must
   rewrite shorter (no JSON walking, no two-channel error code) and
   the wire transcript must still match the captured fixture.
5. Tier 7's regression gates (perf, fuzz nightly, soak nightly,
   coverage delta, schema corpus growth) become the v0.7 axis.

That's a small, honest, evidence-driven v0.6. Cuts in ~1 week.

## Artifacts

- `tools/dogfood-crag-host/main.c` — the harness.
- `conformance/fixtures/crag-mcp/dogfood/session-2026-05-30.jsonl` —
  the captured wire transcript (25 frames; usable as a fresh replay
  fixture once F1/F2 land).
- This doc.
