# cMCP architecture

How the library is partitioned, how a request flows through it, who
owns what memory, and which threads run where. Read alongside
`include/*.h` — the headers are the API contract; this doc is the
"why."

## Source partition

Three static link targets, share-and-stack:

```
libcmcp_core.a    json + rpc + schema + types + transport_stdio
                  + transport_http (server) + transport_http_client
libcmcp_server.a  + server.c + worker.c          (depends on core)
libcmcp_client.a  + client.c + session.c         (depends on core)
```

The server and client sides of an MCP conversation share most of the
machinery — JSON-RPC framing, message parsing, schema validation,
transport vtable, capability structs, log-level codec — so all of
that lives in core. What's actually asymmetric is small:

- **Server**: tool/resource/prompt registries, request dispatch, run
  loop, worker pool, server→client request infrastructure. Two files
  (`server.c` + the pool in `worker.c`).
- **Client**: reader-thread demuxer, pending-request table integration,
  per-call progress subscriptions, child-process management,
  multi-server session aggregator. Two files.

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
│   transport_stdio.c       (newline-delimited JSON over fd pair)   │
│   transport_http.c        (Streamable HTTP — server side)         │
│   transport_http_client.c (Streamable HTTP — client side, libcurl)│
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
(since 2025-06-18, still the case in 2025-11-25) *removes* batch
support at the protocol level, but the framing layer parses them so
higher layers can reject them with a clean `-32600`.

The in-flight ID table (`cmcp_rpc_pending_t`) is a single monotonic
positive-integer ID space per session. The client reserves an ID
before sending a request; the reader looks up the ID on the inbound
response and routes the parsed message to whoever was waiting.

### schema.c

JSON Schema validator — keyword list and deliberate departures in
[`schema-conformance.md`](schema-conformance.md). The server validates inbound
`tools/call.arguments` against the registered tool's `inputSchema`
*before* the handler runs; failures surface as `-32602` with structured
`{path, keyword, message}` error data. The same validator is also
used on the *outbound* side when a tool sets `outputSchema` and the
handler attaches `structuredContent` via
`cmcp_handler_set_structured` — a mismatch there surfaces as `-32603`
per spec ("server MUST provide structuredContent that matches"). No
`$ref`, no `oneOf`/`anyOf`, no `format`, no `pattern` — those would
multiply the validator's complexity for use cases MCP tools don't
actually need today.

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

Streamable HTTP per MCP 2025-11-25: a single `/mcp` endpoint with
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
read → dispatch → write, and there's only ever one logical session
per HTTP transport (concurrent multi-tenant deployments instantiate
multiple transports), so we use a single-slot mailbox rather than a
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
session id back. Exactly one session per transport; hosting multiple
concurrent sessions means multiple transports.

**SSE upgrade.** `GET /mcp` with `Accept: text/event-stream` validates
the session, sends `200 + Content-Type: text/event-stream + no
Content-Length`, and parks the connection on a holder thread that
polls for client disconnect every 250ms. Server-initiated frames
(notifications, plus the request half of `cmcp_server_send_request`
used by elicitation) are routed to every held-open holder by the
`write_fn` classifier — see *Server-initiated notifications →
Wire routing* below.

**Event IDs + Last-Event-Id resumption (MCP 2025-11-25 SEP-1699).**
Every SSE event carries an `id:` line whose value is a per-session
monotonic counter starting at 1. The transport keeps a ring buffer
of the last *N* events (default 256, env-tunable via
`CMCP_HTTP_SSE_REPLAY_BUFFER`, clamped to 65536); when a `GET /mcp`
request carries `Last-Event-Id: N`, every buffered event with
id > N is streamed before the holder is registered for live events.
Recording and replay both run under the holder-list mutex, so live
and replayed events cannot interleave out of order. An out-of-window
`Last-Event-Id` (e.g. older than the ring's tail or higher than
anything emitted) results in headers + no replay — spec-legal, the
client just sees live events from that point on.

**MCP-Protocol-Version header.** Per spec (since `2025-06-18`), every
post-handshake HTTP request MUST carry an `MCP-Protocol-Version:
<version>` header; the server validates inbound (415 on mismatch) and
the client emits outbound. The header is checked at the HTTP layer
before the JSON-RPC body even parses — saves a parse for the bad-peer
case.

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

**Polling + resumption (MCP 2025-11-25 SEP-1699).** The SSE thread
runs a reconnect loop, not a single long-poll: `curl_easy_perform`
returns when the server closes the stream (the spec now lets it do
that at will) and the thread re-establishes the GET, carrying
`Last-Event-Id: <highest-seen>` in its headers so the server can
replay any events that fanned out while the long-poll was being
re-established. Backoff is 50ms after a clean (200) close, doubling
from 100ms to a 5s cap on errors. The highest event id is tracked
under `sse_id_mu`, advanced on every event boundary that carried an
`id:` field (including empty id-only heartbeats).

**Shutdown.** The reconnect loop checks `shutting_down` on every
iteration; a wake makes the in-flight `curl_easy_perform` return
early via `CURLOPT_XFERINFOFUNCTION` returning non-zero, which forces
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

- `ping` — answered with `{}` per spec, before the readiness gate so
  it works pre-handshake too. The MUST-respond half of the spec is
  load-bearing; cMCP failing this on both sides was the first
  conformance violation closed in Tier 4.
- `tools/list`, `tools/call`
- `resources/list`, `resources/read`,
  `resources/subscribe`, `resources/unsubscribe`
- `prompts/list`, `prompts/get`
- `logging/setLevel` — cap-gated on `caps.logging`; stores the new
  floor under `log_mu` so concurrent `cmcp_server_log` calls from
  any worker observe a consistent threshold.

Other request methods get `-32601`. Operate-class methods sent before
initialize get `-32600`. Tool input schemas are validated between
dispatch and handler so handlers only see well-formed input. Prompt
arguments use a flat `[{name, description?, required?}]` descriptor
list (per spec — not full JSON Schema), so the server only enforces
*required-arg present*; richer validation is the prompt handler's
job.

Capabilities (`tools`, `resources`, `prompts`) are auto-advertised in
the `initialize` result whenever ≥1 of the corresponding kind is
registered. Sub-capabilities (`subscribe`, `listChanged`, `logging`)
stay opt-in via `cmcp_server_set_capabilities`.

`resources/subscribe` records the URI in a per-server set;
`resources/unsubscribe` removes it.
`cmcp_server_notify_resource_updated` checks against this set and
silently no-ops for URIs no peer subscribed to, so the wire stays
quiet for resources nobody cares about.

**Descriptors carry UI metadata.** Tools, resources and prompts each
gained an optional `title` field (echoed in their list descriptors)
that hosts can render as a human-readable label distinct from the
programmatic `name`. Tools additionally accept `output_schema`
(advertised as `outputSchema` in `tools/list`) — the schema a typed
`structuredContent` value will be validated against if the handler
sets one via `cmcp_handler_set_structured`. Both fields are deep-
copied at registration time; the `output_schema` string is parsed
eagerly so malformed JSON fails fast (`CMCP_EPARSE`).

All three registration kinds also accept an optional `icons` field
(MCP 2025-11-25 SEP-973) — a caller-owned JSON-text array of
`{src, mimeType?, sizes?}` objects, eagerly parsed at registration
time (malformed JSON or non-array shape → `CMCP_EPARSE`) and emitted
verbatim in the corresponding `*/list` descriptor.

**Server → client requests.** `cmcp_handler_elicit` (Phase 4.4 emit),
`cmcp_handler_elicit_url` (Phase 6.1.3 — URL-mode elicitation per
MCP 2025-11-25 SEP-1036), and any future server-initiated request go
through `cmcp_server_send_request`. URL-mode elicitation is gated by
the `elicitation.url` sub-cap; a peer that advertises only the
legacy flat `elicitation: {}` is treated as form-only. The server maintains its own outgoing
pending list (separate from the client-side one in `rpc.c` — they
have opposite ID spaces) and the run-loop thread routes inbound
`CMCP_MSG_RESPONSE` frames back to the parked worker by id. See
*Threading model → Server side → Server → client requests* for the
machinery.

### client.c + session.c

Async client with one reader thread per `cmcp_client_t`. The reader
demultiplexes responses by ID against per-call completion records
(condvars on a doubly-linked list) and routes server-initiated
frames by kind:

- **Responses** → `deliver_response`: look up the pending entry by
  id, move the parsed message into the completion record, broadcast.
- **`notifications/progress`** → `dispatch_progress_notification`:
  if the frame's `progressToken` matches a per-call subscription
  registered via `cmcp_client_call_async_progress`, fire that call's
  `cmcp_progress_fn`. Unmatched tokens fall through to the generic
  notification callback so per-call subscribers and global observers
  coexist.
- **Other notifications** → user-supplied `cmcp_notification_fn`
  (includes `notifications/message` from a logging server).
- **`ping`** → reply with an empty result `{}` per spec.
- **`sampling/createMessage`** → host handler if registered, default
  `-32601` decline if not.
- **`elicitation/create`** → host handler if registered, default
  `-32601` decline if not.
- **`roots/list`** → reply with the declarative roots list set via
  `cmcp_client_set_roots`, or `-32601` if the host never opted in.
- **Other server-initiated requests** → `-32601`.

**Cancel.** `cmcp_client_cancel(c, id, reason)` wins the race against
a late response: it removes the pending entry FIRST (atomic), then
signals the waiter (which returns `CMCP_ECANCELLED`), then emits
`notifications/cancelled` on the wire. A response arriving after the
take is silently dropped by `deliver_response` since the entry is
gone — no use-after-free.

`session.c` is the aggregator a multi-server host uses: add N clients
under host-supplied names, then per primitive:

- **tools** — `cmcp_session_tools_list` fans out async + fans in;
  `cmcp_session_tool_call` parses `<server>:<tool>` qualified names
  and routes. Per-client pagination is followed automatically: if a
  server returns `nextCursor`, the aggregator issues follow-up
  `tools/list` calls until the cursor is empty (same for
  `resources/list` and `prompts/list` — a server that paginates is
  no longer silently truncated to page one).
- **resources** — `cmcp_session_resources_list` aggregates;
  `cmcp_session_resource_read` takes an explicit `(server, uri)`
  pair. We do *not* fold the server into the URI: URIs already
  contain colons (scheme separator), so a single qualified string
  would be ambiguous.
- **prompts** — symmetric: `cmcp_session_prompts_list` aggregates,
  `cmcp_session_prompt_get` takes `(server, name, args)`.

## Threading model

### Server side

`cmcp_server_run` owns the transport and runs the read loop on one
thread: read a frame, parse it, dispatch. Dispatch then forks two ways.

- **Inline on the loop thread:** the handshake, `*/list` queries,
  `resources/subscribe`/`unsubscribe`, and every notification. These
  are cheap and either read-only or touch the lifecycle FSM — keeping
  them single-threaded means the FSM needs no lock.
- **On a worker pool:** the three handler-invoking methods
  (`tools/call`, `resources/read`, `prompts/get`), once the handshake
  is complete. A slow user handler therefore can't stall the loop, and
  several handlers run concurrently — replies come back in completion
  order, not request order, which JSON-RPC ids already tolerate.

The pool (`src/worker.c`, internal `worker.h`) is a fixed set of N
threads fed by a bounded blocking queue. `cmcp_pool_submit` blocks when
the queue is full, so backpressure propagates to the loop rather than
growing memory unbounded. `cmcp_pool_free` drains every queued job
before joining, so each job runs exactly once. N comes from
`CMCP_WORKERS` (default 4, clamped `[1,64]`). The HTTP transport
self-serializes one request at a time, so the pool needs no
HTTP-specific special-casing — it is transport-agnostic.

The transport's `write_fn` is internally mutex-guarded, so concurrent
workers (and `cmcp_server_notify` from any thread) never interleave
frames.

**Handler context.** Each pool request carries a `cmcp_handler_ctx_t`,
passed to the handler. It exposes `cmcp_handler_cancelled()` (a
cooperative cancel check) and `cmcp_handler_progress()` (emits
`notifications/progress` carrying the caller's `progressToken` from
`params._meta`). The ctx lives inside the work item and is registered
in the server's in-flight table at *enqueue* time.

**Cancellation + timeout.** An in-flight table maps request id → ctx,
guarded by `inflight_mu` (which also guards each ctx's `cancelled`
flag). `notifications/cancelled` is handled inline on the loop thread;
it matches `requestId` against the table and flips the flag. A handler
that polls `cmcp_handler_cancelled()` can then bail; either way, once
flagged, `process_work` drops the response (the MCP spec says a
cancelled request SHOULD NOT get one). A background watchdog thread
sweeps the table every 200ms and flags any request past its deadline —
`CMCP_HANDLER_TIMEOUT_MS` (default 30000, `0` disables). At shutdown
the loop flags every in-flight request before draining the pool, so
handlers stuck on a dead transport unwind instead of writing into it.

**The handler contract (cooperative cancellation — load-bearing).**
Both the `notifications/cancelled` path and the watchdog only *flag*;
neither force-kills. There is deliberately no `pthread_cancel` — async
cancellation of a thread that may hold the transport writer mutex or a
`malloc` arena lock is unsafe in C, and a bounded in-process pool is the
right footprint for the Pi-class target. The consequence is a contract
on handler authors, documented at `cmcp_server_add_tool` in
`include/cmcp_server.h`:

- A handler **MUST** poll `cmcp_handler_cancelled(hctx)` in any loop or
  before any long/blocking step and return early when it reads non-zero.
- A handler that blocks unboundedly burns its worker slot permanently.
  `CMCP_WORKERS` such handlers (default 4) deadlock the whole server —
  no further request, not even `initialize`, gets a worker. This is a
  contract violation by the handler, not a pool bug.

As coarse, opt-in insurance against a handler that leaks memory rather
than spins, `CMCP_HANDLER_RLIMIT_AS_MB` (unset/`0` → off) lowers the
process `RLIMIT_AS` soft limit once at `cmcp_server_run` entry, so a
runaway allocation hits `malloc`-returns-`NULL` instead of the OOM
killer. It is process-wide (caps library + host too) and best-effort —
never raises an existing limit, never exceeds the hard limit, silently
no-ops on any failure. It is NOT isolation: per-handler resource caps
and out-of-process sandboxing are a separate, deferred tier (the threat
model today is single-author tools, not untrusted third-party code).

**Server → client requests.** A worker can call
`cmcp_server_send_request` (used internally by `cmcp_handler_elicit`
to issue `elicitation/create`) from inside a handler. The function
allocates a fresh monotonic id from `outgoing_id_counter`, links a
new `outgoing_pending_t` onto `outgoing_head` under `outgoing_mu`,
writes the request through the same transport mutex everyone else
uses, then parks on the entry's own cv. The run-loop thread, seeing
an inbound `CMCP_MSG_RESPONSE`, walks the outgoing list and broadcasts
the matching entry. The parked worker wakes, moves the response out,
unlinks, frees, returns to the handler. Calling from the run-loop
thread itself would deadlock (the loop is the one that delivers the
response) — handlers run on workers, so in practice this is fine; the
header documents the constraint. The wait loop polls
`cmcp_handler_cancelled` on a 50ms tick so a cancelled handler can
unwind without stranding the worker on a peer that's no longer
answering. At shutdown the loop walks the outgoing list and marks
every entry cancelled, so parked workers return `CMCP_EIO` instead of
waiting forever on a dead transport.

### Client side

```
caller thread(s)                   │  reader thread
                                   │
cmcp_client_call_async[_progress]  │  for (;;) {
  - register pending ID            │      transport_read()
  - alloc completion record        │      cmcp_rpc_parse()
  - (optional) attach progress_fn  │      switch (kind):
  - link onto active list          │        RESPONSE → deliver_response()
  - send_message()                 │          → look up completion by ID
  - return id                      │          → broadcast cv
                                   │        NOTIFICATION →
cmcp_client_wait(id):              │          match progressToken vs
  - find completion on list        │          per-call subscriptions; else
  - cond_wait until done           │          fall through to notif_fn()
  - move response out, free        │        REQUEST → handle_ping /
                                   │          sampling / elicitation /
cmcp_client_cancel(id, reason):    │          roots / else -32601
  - pending_take (atomic, wins     │  }
    race with a late response)     │
  - signal waiter cancelled        │
  - emit notifications/cancelled   │
```

One reader, many waiters. Per-completion mutex + condvar gives every
waiter a localized wakeup — no thundering herd, no broadcasting on a
shared cv. Multiple async calls can be in flight; `wait` can be
called in any order. `cmcp_client_request` is just `call_async + wait`
in one call.

**Per-call progress subscriptions.** Each completion record optionally
carries `(has_progress_token, progress_token, progress_fn,
progress_ud)`. `cmcp_client_call_async_progress` generates a
monotonic token under `list_mu` (folded with the same lock that
guards the active list — saves a second mutex), writes it into
`params._meta.progressToken` so the server's
`cmcp_handler_progress` echoes it back, and parks the per-call
callback on the record. The reader walks the active list on every
`notifications/progress`, reads the matching `(fn, ud)` out under
`list_mu`, releases the lock, then fires `fn` — so the callback does
NOT run with `list_mu` held, and returns 1 so the caller skips the
generic handler fallthrough. When the call completes, the subscription
tears down with the record — no late callback after `wait` returns.

### Client thread-safety contract

The public surface of `cmcp_client.h` is documented in full there; the
machinery above is *why* it holds. In short:

- **Concurrent host threads** may share one client for `request`,
  `call_async`, `notify`, `cancel`, and the typed wrappers — id
  allocation (pending-table mutex), the active list (`list_mu`), and the
  transport writer (per-transport mutex) are each internally locked.
- **`wait` is single-owner per id.** The completion record is consumed
  and freed by the one waiter; a second waiter on the same id is a
  use-after-free. Every `call_async` id must eventually be waited on,
  even after `cancel` (otherwise the record lingers until
  `cmcp_client_free`).
- **Callbacks run on the reader thread**, serialized. They share that
  thread with response delivery, so a blocking or slow handler (sampling
  doing an LLM round-trip, elicitation prompting a user) stalls every
  response and notification until it returns. From inside a callback the
  *non-blocking* calls (`call_async`, `notify`, `cancel`) are safe — they
  don't wait on the reader and, as noted above, the progress dispatch no
  longer holds `list_mu` when it calls user code — but the *blocking*
  pair (`request`, `wait`) self-deadlocks, because the thread that would
  complete them is the one running the callback.
- **Setup vs. live:** handler/capability setters other than `set_roots`
  (which is `roots_mu`-guarded) are not synchronized against a running
  reader — set them before traffic. `cmcp_client_free` is single-thread,
  no-other-call-in-flight; it wakes+joins the reader, then reaps
  outstanding completions.

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

`cmcp_session_t` is the aggregator a multi-server host (butlerbot) uses
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

## Server-initiated notifications

The server can push frames at the client without going through the
request/response path. Two reasons it has to: list-changed signalling
(`tools/list_changed`, `resources/list_changed`, `prompts/list_changed`),
and resource-update events for clients that subscribed.

API:

- `cmcp_server_notify(s, method, params)` — generic. Caller passes the
  full method including the `notifications/` prefix.
- `cmcp_server_notify_{tools,resources,prompts}_changed(s)` — capability-
  gated convenience wrappers. Each refuses with `CMCP_EPROTOCOL` if the
  matching cap (`caps.tools_list_changed`, etc.) wasn't opted in via
  `cmcp_server_set_capabilities`. The intent is to fail loud when a
  server tries to emit a notification it never told the peer it might
  send — a peer that wasn't expecting the cap might not even be
  listening.
- `cmcp_server_notify_resource_updated(s, uri)` — same gating on
  `resources_subscribe`, plus a silent no-op for URIs that no peer
  subscribed to (no point cluttering the wire).

`cmcp_server_notify` is valid only between `cmcp_server_run()` entry
and exit. Outside that window the active transport is `NULL` and the
call returns `CMCP_EINVAL`. Inside the window it's safe from any
thread — the run loop and external callers serialise via a per-server
`notify_mu` for the pointer access, and via the transport's own
writer mutex for the wire write itself.

### Wire routing

For **stdio**, notifications and responses share the same wire and
are interleaved by the transport's writer mutex. The client reader
thread reads frames in order and dispatches by kind (notification →
user callback, response → pending-table waiter).

For **HTTP**, the routing is asymmetric. POST responses ride the slot
mailbox back through the request/response cycle, but server-initiated
notifications have no waiting POST to return through. Instead, the
HTTP transport's `write_fn` peeks the JSON-RPC body via
`classify_body()`:

```
write_fn(buf, len)
   │
   ▼
classify_body(buf)
   │
   ├── is_notif:   walk impl->sse_head, send `data: <body>\n\n` to each fd
   │               (if no SSE conn is held open, the notification is
   │               dropped — the spec allows this)
   │
   └── otherwise:  copy → resp slot, broadcast write_cv → POST handler
                   wakes, sends 200 + body, returns to acceptor pool
```

The client side already supports this end-to-end: the HTTP client's
SSE reader thread (Phase 2.2) parses `data: <json>\n\n` frames and
pushes them onto the same read queue that POST responses use, where
`client.c`'s reader thread routes them to the notification callback
just like a stdio transport would.

Cancellation (`notifications/cancelled`) is honored *cooperatively*:
handler-invoking methods run on the worker pool, and the in-flight
table lets a cancel — or the timeout watchdog — flag a request's
`cmcp_handler_ctx_t`. A handler that polls `cmcp_handler_cancelled()`
stops early; a handler that ignores the flag still runs to completion,
but its response is dropped either way. See *Threading model → Server
side* for the full machinery.

## Sampling (server → host LLM)

A server can request that the host's LLM produce a completion via
`sampling/createMessage` — useful for tools that want to feed raw
output back through the model before surfacing it. Together with
*elicitation* (below) and `roots/list`, these are the three
server-to-client request directions in the current spec surface.

API:

- `cmcp_client_set_sampling_handler(c, fn, ud)` registers a handler
  that receives the params and produces a result object.
- `cmcp_sampling_text_result(text, model, stop_reason)` builds the
  spec-shaped `{role, content, model, stopReason}` envelope.
- `caps.sampling = 1` opts the client into the wire signal — set it
  via `cmcp_client_set_capabilities` BEFORE handshake.

The handler runs on the reader thread, which is single-threaded. A
slow LLM call therefore stalls inbound frames until it returns.
Acceptable for now: real LLM calls take seconds, but in-flight
client→server requests still complete (the server keeps writing
responses; they queue at the transport and get processed once the
handler returns). Moving sampling onto a dedicated worker is
deferred until a real workload demands it. The same caveat applies
to the elicitation handler below.

### Authorisation

cMCP does **not** impose its own allow-list. A server with no handler
attached to its `cmcp_client_t` gets the default `-32601` reply, so
the trust gate is "did the host bother to register a handler for
this server?" In a multi-server `cmcp_session_t`, each `cmcp_client_t`
is its own decision — register a handler only on clients whose
servers you trust to spend tokens. Don't generalise; trust per-server.

The cap flag is a separate opt-in. Setting the handler does NOT
automatically advertise the cap, because the wire signal is what the
server uses to decide whether to issue the request at all — and a
handler attached *after* handshake will see requests the server
sent based on a stale cap-state. The library forces both calls so
the order is explicit:

```
cmcp_client_set_capabilities(cli, &(cmcp_client_capabilities_t){
    .sampling = 1,
});
cmcp_client_set_sampling_handler(cli, my_handler, my_ud);
cmcp_client_handshake(cli, transport);
```

If only the cap is set, sampling requests get `-32601` — server
sent something we said we'd handle, but we forgot to wire it.
Default-deny in both directions.

## Roots (host → server filesystem scoping)

Roots tell servers which paths or URIs the host considers in-scope.
A filesystem-shaped server reads this list before doing anything that
touches the outside world. cMCP carries the list; the server is the
one that enforces the boundary.

API:

- `cmcp_client_set_roots(c, roots, n)` — declarative. Library deep-
  copies. Calling with `(NULL, 0)` is "I support roots, the list is
  empty"; that's distinct from never calling, which means "I don't
  do roots at all."
- `cmcp_client_notify_roots_changed(c)` — emit
  `notifications/roots/list_changed`. Cap-gated:
  `caps.roots_list_changed = 1` required.

Symmetric to sampling, this is one of three server-initiated
*requests* the client knows how to answer (`sampling/createMessage`
and `elicitation/create` are the others). The reader thread catches
`roots/list` and replies with the stored list — no host callback
runs, the data is the data.

### Capability auto-advertise

`roots: {}` is added to the `initialize` capabilities object whenever
`cmcp_client_set_roots` was ever called, even with `n=0`. The cap
presence is the opt-in; the empty list is a valid state. If
`caps.roots_list_changed = 1` was also set, `listChanged: true`
appears under `roots`. This mirrors the server's auto-advertise rule
for tools/resources/prompts: caller behavior implies the cap.

Default-deny: if the host never called `set_roots`, a server-sent
`roots/list` request gets `-32601` and the cap isn't advertised
either, so a well-behaved server shouldn't be asking in the first
place.

### Concurrency

`cmcp_client_set_roots` is safe to call before or after handshake,
and from any thread. A `roots_mu` mutex on the client guards the
array pointer + contents; the reader thread takes a snapshot under
the lock when building the `roots/list` response. Replacing the
array atomically swaps the pointer and frees the old contents while
the lock is held.

## Elicitation (server → host structured prompt)

Mid-tool-call, a server can ask the user for additional structured
input — a confirmation, a missing argument, a credential — via
`elicitation/create`. The two halves live on opposite sides:

- **Host side** (`client.c`). `cmcp_client_set_elicitation_handler`
  registers a callback that the reader thread invokes on each
  inbound `elicitation/create`. The handler returns a result built
  via `cmcp_elicitation_result(action, content)` where `action` is
  `"accept"` (with a `content` object shaped per the request's
  `requestedSchema`), `"decline"`, or `"cancel"`. Default-decline if
  no handler is registered (`-32601`), with the same trust model as
  sampling: a handler is the host's per-server opt-in to letting that
  server interrupt the user.
- **Server side** (`server.c`). `cmcp_handler_elicit(hctx, message,
  requested_schema, &out_result)` is the convenience wrapper a tool
  handler calls from a worker thread. It is cap-gated on
  `s->peer_caps.elicitation` (the cap the client advertised at
  handshake) — if the peer didn't opt in, it short-circuits with
  `CMCP_EUNSUPPORTED` without ever touching the wire. Otherwise it
  builds the spec-shaped params (`message`, `requestedSchema` —
  defaulting to `{"type":"object"}` when the caller passes NULL)
  and delegates to `cmcp_server_send_request`, which handles the
  outgoing pending table and the response routing described under
  *Threading model → Server side → Server → client requests*.

The cap symmetry is what keeps this honest: a server that finds
`elicitation = 0` in `peer_caps` knows the request would just be
declined, so the library skips the round-trip entirely. The host
trust gate (`set_elicitation_handler`) and the wire signal
(`caps.elicitation = 1`) are deliberately separate, same model as
sampling — wire-signalled-but-no-handler ends in `-32601`, which is
default-deny in both directions.

## Host-side cancel + per-call progress

Phase 4.5 closed the symmetry gap on the host side. Handlers had
been able to observe cancellation (`cmcp_handler_cancelled`) and
emit progress (`cmcp_handler_progress`) since Tier 3, but the host
had no clean way to *initiate* either.

- **`cmcp_client_cancel(c, id, reason)`** does three things in
  order: removes the pending entry (atomic — wins the race against
  a response in flight), signals the waiter so `cmcp_client_wait`
  returns `CMCP_ECANCELLED`, then emits
  `notifications/cancelled {requestId, reason?}` on the wire. The
  ordering matters: if a response arrives between steps 1 and 3,
  `deliver_response` finds no pending entry and drops the frame
  silently — no use-after-free, no spurious double-completion. A
  slow handler that ignores `cmcp_handler_cancelled` still runs to
  completion server-side, but its response is dropped per spec.
- **`cmcp_client_call_async_progress(c, method, params, fn, ud,
  &id)`** attaches a per-call progress callback to a request. The
  library allocates a unique token under `list_mu` (folded counter),
  writes it into `params._meta.progressToken` (replacing any
  caller-supplied value at that path), stashes
  `(progress_fn, progress_ud, progress_token)` on the completion
  record, and sends the request. The reader's
  `dispatch_progress_notification` matches inbound
  `notifications/progress` against active subscriptions by token; on
  a match it fires the callback and skips the generic notification
  handler. Unmatched tokens (typed responses to other tools, or
  late frames after `wait` returned) fall through to the generic
  handler so observability isn't lost.

The subscription is tied to the completion record's lifetime — when
the caller returns from `wait`, the record is unlinked and freed
together with the subscription. No late callback fires after `wait`
returns, by construction.

## Structured tool output, resource_link, title

Phase 4.6 widened the `tools/call` response surface without
breaking the Tier 3 handler signature. Three additions:

- **`structuredContent`.** A tool that registered an `output_schema`
  can attach a typed result via
  `cmcp_handler_set_structured(hctx, value)` — additively, on the
  per-call handler context, not via the `out_*` return path. The
  dispatcher validates the value against the schema before send; a
  mismatch surfaces as `-32603` per spec. When the handler sets a
  structured value but doesn't fill `out_content`, the library
  synthesises a `[{type:"text", text:"<emit>"}]` fallback so
  legacy clients still see something rendered.
- **`resource_link` content items.**
  `cmcp_tool_resource_link_content(uri, name, description?,
  mime_type?)` builds a `{type:"resource_link", ...}` entry — a tool
  that wants to point at a resource instead of inlining its content
  (e.g. "the file you asked about is at `file:///…`"). Mix-and-match
  freely with `text` items in the same array.
- **`title`.** Tool, resource, and prompt descriptors gained an
  optional `title` field for UI display, distinct from the
  programmatic `name`. Echoed in `tools/list`, `resources/list`,
  `prompts/list` when set.

Why on the ctx for `structuredContent`? Because Phase 3.4's tool
handler signature is the public bar — adopters don't want to re-fan
it out per protocol revision. Putting the new knob on the ctx
preserves the bar; tools that don't care simply never call the new
function. The ctx also carries an `is_tool_call` flag so calling
`cmcp_handler_set_structured` from a resource or prompt handler is
a documented no-op (with auto-free), not a crash.

## Logging

Phase 4.7. A server can ship structured log events to the host via
`notifications/message {level, logger?, data}`, and the host can
dial the floor up or down with `logging/setLevel`. Both halves are
cap-gated on `caps.logging`.

- **Levels.** The eight RFC 5424 syslog levels — `debug`, `info`,
  `notice`, `warning`, `error`, `critical`, `alert`, `emergency` —
  are exposed as `cmcp_log_level_t` in the public types header.
  `cmcp_log_level_from_name` / `_to_name` round-trip between the
  enum and the wire strings.
- **Server emit.** `cmcp_server_log(s, level, logger, data)` checks
  the cap, compares `level` against the per-server floor (`log_mu`
  guards the read; the floor defaults to `debug` so nothing is
  silently dropped pre-`setLevel`), then builds and emits the
  notification through the same `cmcp_server_notify` path that
  carries `*/list_changed`. Filtering returns `CMCP_OK` — a
  too-verbose trace is not an error.
- **Client setLevel.** `cmcp_client_set_log_level(c, level)` is a
  synchronous request sender; it surfaces a peer-side `-32601` as
  `CMCP_EPROTOCOL`, so a host that hits an unprepared server
  notices.
- **Reception is free.** `notifications/message` reaches the host
  through the existing `cmcp_notification_fn` set via
  `cmcp_client_set_notification_handler` (Phase 1.9). A typed log
  callback was deemed sugar that the agent can do itself.

The cap signal is the route gate: the server only attaches the
`logging/setLevel` handler when `caps.logging = 1`, so an unprepared
peer answers `-32601` honestly instead of silently accepting a
setLevel it cannot honour.

## What's deliberately *not* in v0.4

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
- OAuth 2.1 on the HTTP transport. Deferred — no remote authed cMCP
  server in sight for the consumer (butlerbot).
- `completion/complete` (argument autocomplete for prompts and
  resources). Low value for an autonomous agent — butlerbot's LLM
  picks arguments, it does not drive a completion menu. Implement
  if an interactive host ever consumes cMCP.
- `resources/templates/list` (RFC 6570 URI-templated resources).
  Needs a URI-template engine cMCP does not have; deferred until a
  templated server is real.
- Connection pooling at the session layer. One client per server is
  the current shape; butlerbot's actual usage hasn't yet shown a need
  for more.
- Sampling / elicitation handlers running on a worker pool — they
  currently run on the client's reader thread, which means a slow
  interactive prompt or LLM call stalls inbound frames until it
  returns. Acceptable for current workloads; revisit if real
  throughput demands it.

The full phase plan is in [`TODO.md`](../TODO.md); the release log
is in [`CHANGELOG.md`](../CHANGELOG.md).
