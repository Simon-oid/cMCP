# cMCP architecture

How the library is partitioned, how a request flows through it, who
owns what memory, and which threads run where. Read alongside
`include/*.h` — the headers are the API contract; this doc is the
"why."

## Source partition

Three static link targets, share-and-stack:

```
libcmcp_core.a    json + rpc + schema + types + transport_stdio
libcmcp_server.a  + server.c              (depends on core)
libcmcp_client.a  + client.c + session.c  (depends on core)
```

The server and client sides of an MCP conversation share most of the
machinery — JSON-RPC framing, message parsing, schema validation,
transport vtable, capability structs — so all of that lives in core.
What's actually asymmetric is small:

- **Server**: tool registry, request dispatch, run loop. One file.
- **Client**: reader-thread demuxer, pending-request table integration,
  child-process management, multi-server session aggregator. Two files.

Link order matters: consumers (server, client) before provider (core),
so the linker resolves core symbols pulled in by server.o / client.o
on the second pass. The Makefile does this for you.

## Six-layer pipeline

```
┌──────────────────────────────────────────────────────────────────┐
│                          tools (handlers)                         │  ← user code
├──────────────────────────────────────────────────────────────────┤
│  server.c — registry, dispatch     │   client.c — async client    │
│   server_run loop                  │    + reader thread + pending │
│                                    │   session.c — N-client mux   │
├──────────────────────────────────────────────────────────────────┤
│   schema.c — JSON Schema subset (validates tools/call.arguments)  │
├──────────────────────────────────────────────────────────────────┤
│   rpc.c — JSON-RPC 2.0 framing, in-flight ID table                │
├──────────────────────────────────────────────────────────────────┤
│   transport_stdio.c (newline-delimited JSON over fd pair)         │
│   transport_http.c   (Streamable HTTP — Phase 2.1, not yet)       │
├──────────────────────────────────────────────────────────────────┤
│   json.c — typed value tree, parser, emitter                      │
└──────────────────────────────────────────────────────────────────┘
```

Each layer talks only to the layer directly below it (and exposes
types from `cmcp_types.h` to the layer above). The transport layer
never inspects message content; the RPC layer never reads bytes; the
schema layer never speaks JSON-RPC. This is what lets the same RPC
core work for both halves and lets HTTP plug in on the same vtable.

### json.c

Hand-parsed JSON with a discriminated-union value tree (`cmcp_json_t`).
Lifted from cRAG's `util.c` as a starting point and extended with:

- Object/array constructors so emit code can build trees fluently.
- Escape-aware string emission (UTF-8 surrogate pairs, control bytes).
- Stable key ordering on emit — keys come out in insertion order.
  This is not a JSON requirement but it makes wire frames byte-stable
  for tests and for diffing logs.

### rpc.c

JSON-RPC 2.0 framing. Discriminated-union message type
(`cmcp_rpc_message_t`) covering request, response, notification.
`cmcp_rpc_parse` accepts batches as well as single messages — MCP
2025-06-18 *removes* batch support at the protocol level, but the
framing layer parses them so higher layers can reject them with a
clean `-32600`.

The in-flight ID table (`cmcp_rpc_pending_t`) is a single monotonic
positive-integer ID space per session. The client reserves an ID
before sending a request; the reader looks up the ID on the inbound
response and routes the parsed message to whoever was waiting.

### schema.c

JSON Schema subset — exactly the keywords listed in
[`schema-subset.md`](schema-subset.md). The server validates inbound
`tools/call.arguments` against the registered tool's `inputSchema`
*before* the handler runs; failures surface as `-32602` with structured
`{path, keyword, message}` error data. No `$ref`, no `oneOf`/`anyOf`,
no `format`, no `pattern` — those would multiply the validator's
complexity for use cases MCP tools don't actually need today.

### transport_stdio.c

Newline-delimited JSON over a `(read_fd, write_fd)` pair. Frames are 1:1
with messages. `getline()` for reads (so blank lines are tolerated and
the buffer grows with the message); manual `fwrite + '\n' + fflush` for
writes, guarded by a per-transport mutex so concurrent writers (e.g. the
server replying while a notification thread emits) never interleave.

The vtable (`cmcp_transport_t` in `cmcp_transport.h`) has three
operations: `read_fn`, `write_fn`, `close_fn`. The HTTP transport plugs
in on the same vtable.

### transport_http.c (server side)

Streamable HTTP per MCP 2025-06-18: a single `/mcp` endpoint with
`POST` (request → response) and `GET` with `Accept: text/event-stream`
(SSE upgrade). Hand-rolled HTTP/1.1 server on top of `socket()` +
`accept()`; a tiny request parser handles the request line, headers,
and Content-Length-driven bodies. Chunked transfer encoding, TLS, and
HTTP keep-alive are intentionally absent — `Connection: close` on
every response, and any prod deployment puts nginx/caddy in front for
TLS termination.

**Threading.** One acceptor thread per transport, created by
`cmcp_transport_http_listen`. Each connection is handled inline on
the acceptor for POSTs (one POST at a time, serialized into the
single-slot bridge below); SSE GETs detach to a per-connection holder
thread that holds the socket open until the client disconnects (or
shutdown). Close is graceful: shut down the listening socket so
`poll/accept` returns, join the acceptor, signal SSE holders to
unwind.

**Bridging onto the cmcp_transport_t vtable.** server.c is strictly
read → dispatch → write, and there's only ever one logical session per
HTTP transport in v0.2, so we use a single-slot mailbox rather than a
queue:

```
                                     ┌─ slot_mu ─────────────────┐
HTTP acceptor (POST handler):        │  req_body / req_present   │
  push body                          │  resp_body / resp_present │
  signal read_cv                     └─────┬──────────────────┬──┘
  if request: wait on resp_present         │                  │
  if notif:   wait on !req_present     read_cv             write_cv
  send HTTP response                       │                  │
                                           ▼                  ▼
                                    cmcp_transport_read   cmcp_transport_write
                                    (server.c run loop)   (server.c send_message)
```

`req_present` is cleared in `read_fn` when server.c consumes the
frame; `resp_present` is set in `write_fn` and cleared by the POST
handler when it steals the response. Notifications never produce a
response — server.c skips the write, the POST handler returns 202
Accepted as soon as it sees `req_present` go to 0.

Discriminating notifications from requests on the HTTP side requires
peeking the JSON-RPC body (via `cmcp_json_parse`): notifications get
202 with no body, requests get 200 with the response body. This is
the one place the transport layer looks at message contents — the
alternative was extending the vtable with a "no reply expected"
callback, which is more invasive for less clarity.

**Session.** The first `initialize` POST mints a v4-shaped UUID and
stamps it onto the response in an `Mcp-Session-Id` header. All
subsequent POSTs and GETs must carry a matching `Mcp-Session-Id` —
missing → 400, mismatched → 404. The session id is minted in
`write_fn` (not `read_fn`) so a malformed initialize never gets a
session id back. v0.2 supports exactly one session per transport;
hosting multiple concurrent sessions means multiple transports.

**SSE stub.** `GET /mcp` with `Accept: text/event-stream` validates
the session, sends `200 + Content-Type: text/event-stream + no
Content-Length`, and parks the connection on a holder thread that
polls for client disconnect every 250ms. There are no events to emit
yet — server-side notification emit is Phase 2.4.

### transport_http_client.c (client side)

The mirror image: `cmcp_transport_http_connect(url)` produces a
transport that the existing async client (`client.c`) drives like any
other. Internally it juggles two libcurl flows:

```
caller thread          SSE thread           libcurl easy
─────────────          ──────────           ────────────
write_fn(frame) ─────► do_post()
                       curl_easy_perform ──► POST /mcp
                       (synchronous)         ◄── 200 + body / 202
                       latch session id
                       queue_push(body)
                                                 ┌── on session latch:
                       (idle, parked on cv)  ────┤  open SSE
                                                 │  curl_easy_perform
                                                 │  WRITEFUNCTION
                                                 │   parses data: ...
                                                 │   queue_push(event)
                                                 └── (long-poll)
read_fn() ─────────────► queue_pop ◄────── (frames from either source)
```

**Why two flows.** Each `write_fn` call is a one-shot HTTP exchange
with its own libcurl easy handle, so multiple application threads can
be in `call_async` concurrently without serializing — their POSTs
race independently and their responses arrive on the queue, where
client.c's reader thread demultiplexes by JSON-RPC id. The SSE thread
is separate so server-pushed messages don't have to wait for an
application-side POST to drain.

**Session latch.** The first POST goes out without a session id
(it's the `initialize`). The response carries `Mcp-Session-Id`; a
header-callback parses it and `latch_session_id` stores it under a
mutex, broadcasting a condvar that wakes the SSE thread (which has
been parked since startup waiting for exactly this). Subsequent
POSTs add the header automatically.

**Shutdown.** The SSE thread is parked on a libcurl `curl_easy_perform`
that won't return until the server closes the connection — except
that we install a `CURLOPT_XFERINFOFUNCTION` progress callback that
returns non-zero when a `shutting_down` flag is set, which forces
curl to abort the transfer with `CURLE_ABORTED_BY_CALLBACK`. The
queue_pop loop in `read_fn` is plain pthread_cond_wait, which is
immune to signals, so we expose a `wake_fn` on the transport vtable.
client.c's free path calls `cmcp_transport_wake` before joining the
reader thread; the wake flips `shutting_down` and broadcasts the
queue cv, queue_pop returns CMCP_EIO, the reader exits.

The wake_fn vtable slot is optional — stdio leaves it NULL because
its `read_fn` blocks on a syscall (`getline`), and the SIGUSR2 +
non-restarting handler protocol that returns `EINTR` from the
syscall is enough for that case. Transports whose `read_fn` parks on
a userspace primitive (condvar, futex, channel) need wake_fn; those
that block on a syscall don't.

### server.c

Tool / resource / prompt registries → dispatch → run loop. The
handshake (`initialize`, `notifications/initialized`) plus the full
primitive surface is built in:

- `tools/list`, `tools/call`
- `resources/list`, `resources/read`,
  `resources/subscribe`, `resources/unsubscribe`
- `prompts/list`, `prompts/get`

Other request methods get `-32601`. Operate-class methods sent before
initialize get `-32600`. Tool input schemas are validated between
dispatch and handler so handlers only see well-formed input. Prompt
arguments use a flat `[{name, description?, required?}]` descriptor
list (per spec — not full JSON Schema), so the server only enforces
*required-arg present*; richer validation is the prompt handler's
job.

Capabilities (`tools`, `resources`, `prompts`) are auto-advertised in
the `initialize` result whenever ≥1 of the corresponding kind is
registered. Sub-capabilities (`subscribe`, `listChanged`) stay
opt-in via `cmcp_server_set_capabilities`.

`resources/subscribe` records the URI in a per-server set;
`resources/unsubscribe` removes it. Notification *emit* against that
set lands in Phase 2.4 — the storage is here so subscribe/unsubscribe
are real round-trips today.

### client.c + session.c

Async client with one reader thread per `cmcp_client_t`. The reader
demultiplexes responses by ID against per-call completion records
(condvars on a doubly-linked list), routes server-to-client
notifications to a user callback, and replies `-32601` to unsolicited
server requests (we don't support sampling/roots host-side yet).

`session.c` is the aggregator a multi-server host uses: add N clients
under host-supplied names, then per primitive:

- **tools** — `cmcp_session_tools_list` fans out async + fans in;
  `cmcp_session_tool_call` parses `<server>:<tool>` qualified names
  and routes.
- **resources** — `cmcp_session_resources_list` aggregates;
  `cmcp_session_resource_read` takes an explicit `(server, uri)`
  pair. We do *not* fold the server into the URI: URIs already
  contain colons (scheme separator), so a single qualified string
  would be ambiguous.
- **prompts** — symmetric: `cmcp_session_prompts_list` aggregates,
  `cmcp_session_prompt_get` takes `(server, name, args)`.

## Threading model

### Server side

Single-threaded run loop. `cmcp_server_run` owns the transport, reads
one frame at a time, dispatches it inline, writes one response back.
No worker pool yet — handlers must return promptly (or the run loop
stalls). This is fine for v0.1 because:

- Tools shipped today (echo, add, crag_search, crag_stats) all return
  in milliseconds.
- A worker pool would add per-handler-timeout machinery and ordering
  rules; cleaner to ship single-threaded and add concurrency when
  there's a real long-running tool to motivate the design.

### Client side

```
caller thread(s)                   │  reader thread
                                   │
cmcp_client_call_async             │  for (;;) {
  - register pending ID            │      transport_read()
  - alloc completion record        │      cmcp_rpc_parse()
  - link onto active list          │      switch (kind):
  - send_message()                 │        RESPONSE → deliver_response()
  - return id                      │          → look up completion by ID
                                   │          → broadcast cv
cmcp_client_wait(id):              │        NOTIFICATION → notif_fn()
  - find completion on list        │        REQUEST     → reply -32601
  - cond_wait until done           │  }
  - move response out, free        │
```

One reader, many waiters. Per-completion mutex + condvar gives every
waiter a localized wakeup — no thundering herd, no broadcasting on a
shared cv. Multiple async calls can be in flight; `wait` can be
called in any order. `cmcp_client_request` is just `call_async + wait`
in one call.

### Reader-thread shutdown

The non-obvious part. On Linux, closing or `dup2`-ing a file descriptor
does **not** interrupt a `read()` already blocked on that fd — the
kernel keeps a reference to the original until the syscall returns. So
the natural "close the fd to wake the reader" pattern doesn't work for
us.

What does work: send a signal to the reader thread with a non-restarting
handler (`SA_RESTART = 0`). The kernel returns `EINTR` from the read
syscall, the stdio transport surfaces this as `CMCP_EIO`, the reader
exits. We use `SIGUSR2` with a no-op handler (installed via
`pthread_once`).

There's a race: `cmcp_client_free` sets `shutting_down = 1` and signals
the reader, but the reader might be between the `shutting_down` check
and the read syscall when the signal arrives — the handler runs, the
syscall hasn't started yet, then the reader blocks forever waiting for
data that won't come. Fix: `pthread_tryjoin_np` retry loop, re-sending
the signal every 100µs until the join succeeds.

### Reader sees transport EIO

If the peer crashes or the child dies (e.g. `cmcp_client_connect_stdio`
exec failed), the reader gets EIO from `transport_read` and exits. It
must wake every waiter on its way out — otherwise `cmcp_client_wait`
calls block forever. The reader calls `cancel_all_waiters` before
returning; each pending completion is marked cancelled and broadcast,
and waiters return `CMCP_ECANCELLED`.

## Ownership rules

| Object | Who frees | When |
|--------|-----------|------|
| `cmcp_json_t *` passed into `*_set` / `*_request` / handlers | callee | After use; library's responsibility |
| `cmcp_json_t *` returned from `*_get` / `cmcp_json_object_get` | nobody — borrowed | Lives as long as the parent tree |
| `cmcp_rpc_message_t` | the field's struct holder | `cmcp_rpc_message_clear` before scope exit |
| Tool-handler `*out_content` | library | Library frees after emitting the response |
| Tool-handler `arguments` | caller (library) | Borrowed; handler must NOT free |
| Borrowed transport (`handshake`) | caller | Caller closes after `client_free` returns |
| Owned transport (`connect_stdio`) | client | `client_free` closes it |
| Spawned child process | client | `client_free` reaps via SIGTERM + waitpid |
| Clients added to a session | session | `session_free` walks and frees each |

The convention: any function whose docs say "consumed" or "takes
ownership" is a transfer point. Everything else is borrowed.

## Lifecycle data flow

A single `tools/call` from the client's perspective:

```
caller                client.c                            transport          server.c
──────                ────────                            ─────────          ────────
                  cmcp_client_request
                   ├─ call_async                                            (reader loop)
                   │    pending_register → id              write JSON ──→
                   │    completion alloc + link                              parse
                   │    send_message                                         dispatch:
                   │       rpc_emit(msg)                                       lookup tool
                   │                                                           validate args
                   │                                                           handler()
                   │                                                           build content
                   │                                       ←── write JSON     emit response
                   │                                                          ────────
                  reader thread (concurrently)
                   │  transport_read                       read JSON
                   │  rpc_parse                                              (server done)
                   │  deliver_response(msg, id)
                   │    completion->response = msg
                   │    cond_broadcast
                   │
                   └─ wait
                        cond_wait until done
                        move response out
                  (caller gets cmcp_rpc_message_t)
                  cmcp_rpc_message_clear
```

The server side is plain: read → parse → dispatch → handler → emit →
write. No threads, no pending table — the reply is always next on the
write side, in lock-step with the read side.

## Multi-server session

`cmcp_session_t` is the aggregator a multi-server host (openclawd) uses
to present a flat tool surface across N child servers:

```
host
 │
 ▼
cmcp_session_t  ────── add ──────  cmcp_client_t (alpha) → server A (child)
                                   cmcp_client_t (beta)  → server B (child)
                                   cmcp_client_t (gamma) → server C (child)
```

Tool names are namespaced as `<server>:<tool>`:

- `cmcp_session_tools_list` fans out async calls to every client, then
  fans in sequentially. A slow server can't stall fast ones because
  dispatch is async; collection is sequential because the result has
  to be ordered eventually anyway.
- `cmcp_session_tool_call(qualified, args)` parses the colon, finds
  the client by server name, builds `tools/call` params, and routes.
- The session takes ownership of clients on add; freeing the session
  frees every client, which closes their transports and reaps their
  spawned children.

Why namespace by server rather than just merge tools? Because two
servers are allowed to declare the same tool name. The host needs a
deterministic way to disambiguate, and a colon-separated qualified
name is what an LLM-facing menu can present without surprising the
model. Servers don't see the qualified form — the session strips the
prefix before dispatch.

## What's deliberately *not* in v0.2

- TLS. Deploy behind nginx/caddy. Hand-rolling TLS in C is a project
  unto itself and adds nothing protocol-shaped.
- HTTP keep-alive. Every response carries `Connection: close`. The
  cost of a TCP handshake per RPC is invisible compared to whatever
  the tool actually does.
- Chunked transfer encoding. `Content-Length` only. Bodies past 4 MiB
  are rejected.
- Concurrent HTTP sessions on a single transport. One session per
  `cmcp_transport_t`; concurrent multi-tenant deployments instantiate
  multiple transports.
- HTTP client transport. Phase 2.2.
- Worker pool / per-handler timeouts on the server. Tools today are
  fast; concurrency adds machinery without a motivating need.
- Resources, prompts, sampling, roots. The `*/list_changed` and
  `resources/updated` notifications. The full async cancel/progress
  machinery. All Phase 2.3+.
- Connection pooling at the session layer. One client per server is
  the v0.1 shape; openclawd's actual usage hasn't yet shown a need
  for more.

The full phase plan is in [`TODO.md`](../TODO.md).
