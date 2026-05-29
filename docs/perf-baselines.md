# Performance baselines

This document captures cMCP's first-pass performance numbers. The
baselines are *informational*, not a per-PR gate. They exist so:

1. Consumers (butlerbot, cRAG-as-MCP-server, third-party hosts) can
   reason about expected per-call cost when sizing their workloads.
2. Future tier work has a reference point — if a refactor halves
   throughput on the inline bench, that needs explaining.
3. Comparison against the TS / Python reference SDKs (Phase 6.6.2)
   has somewhere to land.

> Companion documents: [`CHANGELOG.md`](../CHANGELOG.md) phase 6.6.1
> (this commit), [`agentic-readiness.md`](agentic-readiness.md) for the
> overall quality-bar context.

## What we measure

Each bench binary in `bench/` emits one CSV row to stdout with the
schema:

```
bench,iterations,wall_ms,throughput_per_s,min_us,p50_us,p95_us,p99_us,p999_us,max_us,mean_us,extra
```

`make bench` runs all three and concatenates into
`bench/results.csv`. Re-running overwrites that file (history is
intentionally not preserved in-repo — these numbers vary per machine,
per kernel, and per build flag).

### `bench_server_inline` (stdio)

In-process pipe pair, in-process server, sync `tools/call` against an
inline-fast `echo` tool. **No subprocess, no socket** — measures the
JSON-RPC + dispatch + worker-pool roundtrip plus stdio framing.

- Iterations: 50000 measured (after 1000 warmup).
- Tool: returns a single text-content item from the input `message`.
- Workload representative of: agent issuing back-to-back calls
  against a stdio-spawned local server.

### `bench_server_pool` (stdio, async)

Same in-process pipe pair, but the tool sleeps 50ms (env-tunable),
and the bench fires `CMCP_BENCH_N` async calls with
`cmcp_client_call_async`, then waits for all. Surfaces the
worker-pool concurrency: with `CMCP_WORKERS=W` and `N >> W`, expect
wall ≈ `ceil(N/W) * sleep_ms`. Serial baseline is `N * sleep_ms`.

- Defaults: 64 calls, 50ms sleep each, 4 workers.
- The per-call p50 is dominated by the sleep itself plus queue wait;
  the aggregate throughput is the more interesting metric here.

### `bench_server_inline` (HTTP)

Mirrors the stdio inline bench, but the transport is Streamable HTTP
on a loopback ephemeral port. The client is libcurl through
`cmcp_transport_http_connect`; the server is the hand-rolled
acceptor + parser + `cmcp_server_run` on a worker thread.

- Iterations: 5000 measured (after 1000 warmup). HTTP is slower than
  stdio; we don't need 50k to reach steady state.
- Workload representative of: agent connecting over a terminator to
  a remote MCP server.

## How to run

```sh
make bench-build                                # compile the three bench binaries
make bench                                       # run all + write bench/results.csv

# Override iteration count + warmup for a quick sanity check:
CMCP_BENCH_N=5000 CMCP_BENCH_WARMUP=100 make bench

# Pool concurrency: 4 workers vs 1 worker on the same workload
CMCP_BENCH_N=32 CMCP_BENCH_SLEEP_MS=50 CMCP_WORKERS=4 make bench
CMCP_BENCH_N=32 CMCP_BENCH_SLEEP_MS=50 CMCP_WORKERS=1 make bench
```

`bench/run.sh` pins the bench process to CPU 0 with `taskset -c 0`
if available — meaningfully reduces p99 noise on a multi-socket /
hybrid-core machine. Falls back gracefully if `taskset` is not in
PATH.

## Methodology notes

- **In-process measurement.** Each bench keeps client and server in
  the same process. Subprocess fork/exec is paid once at handshake
  on real deployments; steady-state per-call latency is what the
  baseline reports.
- **Warmup is real, not a fixed delay.** 1000 (stdio) / 1000 (HTTP)
  warmup calls are issued and discarded before the measurement
  window opens. Worker pool threads are already running, libcurl
  connection is established, kernel TCP state has settled, page
  cache is warm.
- **Quantiles via qsort.** The bench keeps every per-call sample in
  a fixed array, then `qsort`s once at the end and reads off
  p50/p95/p99/p999. Tractable up to ~200k samples; we run 50k.
- **Build flags.** `make bench` uses the standard `-O2 -g` profile
  the library ships with. Sanitisers (`-fsanitize=...`) and
  coverage (`--coverage`) both slow the library substantially —
  never bench an asan/cov build.
- **Statistical posture.** Single-run numbers. We do not report
  confidence intervals; sources of variance on a typical Linux box
  (scheduler, NUMA, cache effects from other processes) tend to
  swamp them anyway. The baseline is "is cMCP in the right
  order of magnitude," not a stats paper.

## Hardware / environment context

Numbers in this document were captured on:

- CPU: AMD Ryzen 7 5800X (8c/16t, Zen 3, 3.8 GHz base)
- RAM: 32 GB
- Kernel: Linux 6.18.x (Arch)
- libc: glibc 2.x
- compiler: gcc / clang shipped with the system, `-O2 -g`
- transport-loopback: 127.0.0.1 on the same NUMA node
- background load: idle desktop, no other user processes hammering
  CPU during the run

Re-running on a different machine will produce different absolute
numbers; the *ratios* between rows (stdio-vs-HTTP, pool-concurrency
factor) are the more portable signal.

## Observed numbers

> The numbers below are the first capture from
> `make bench` on the Ryzen 5800X box described above. They drift
> with each re-run on different hardware; treat them as
> order-of-magnitude reference, not absolute truth.

| bench                  | iterations | throughput (calls/s) | p50 (µs) | p99 (µs) | p999 (µs) |
|------------------------|-----------:|---------------------:|---------:|---------:|----------:|
| server_inline_stdio    | 50,000     | 48,813               | 20       | 24       | 29        |
| server_pool_stdio      | 64         | 80                   | 451,893  | 802,600  | 802,600   |
| server_inline_http     | 5,000      | 5,378                | 185      | 238      | 324       |

What these numbers say:

- **`server_inline_stdio` at p50 = 20 µs.** Steady-state cost of one
  full `tools/call` over a stdio pipe pair: client serializes JSON,
  writes a line, server reads + parses + dispatches + schema-validates
  + runs the handler + emits a response, client reads + parses. All
  in 20 µs at p50, p999 < 30 µs. The pipe + JSON path is fast enough
  that schema validation and handler dispatch are not bottlenecks
  at this scale.

- **`server_pool_stdio` at 80 calls/s.** The pool multiplexes exactly
  as advertised: `4 workers / 50 ms sleep = 80 calls/s` matches the
  measured throughput to the digit. p50 = 451 ms is the *per-call*
  wait time including queue wait — the 32nd call sits behind 7 other
  rounds (~350 ms) before its 50 ms tool fires. If the pool weren't
  concurrent, the same workload would take 64 × 50 ms = 3.2 s instead
  of the observed 802 ms.

- **`server_inline_http` at 5,378 calls/s, p50 = 185 µs.** ~9× slower
  than stdio. The dominant cost is *one TCP connection per call*:
  `cmcp_transport_http_connect` currently creates a fresh libcurl
  easy handle per POST, so every call pays for `connect()` +
  `accept()` + libcurl setup. Production hosts that want HTTP latency
  closer to stdio should request connection pooling on the transport
  (not yet implemented); the 9× headroom is what's available to
  recover.

The pool-bench throughput should always track `workers / sleep_s`
closely. If a future change drops it below that, the pool isn't
multiplexing — bug, not perf-tuning territory.

## Known issue surfaced by this bench (filed for follow-up)

While bringing up `bench_http` we noticed two intertwined behaviors:

1. **Bench saturates the 6.5.2 accept-rate gate.** Default
   `CMCP_HTTP_ACCEPT_RATE=100/s` with burst 200 is a peer-flood
   defense and fires inside ~2 seconds for any high-rate
   single-process bench. `bench_http` works around this by calling
   `setenv("CMCP_HTTP_ACCEPT_RATE", "0", 1)` at startup *unless* the
   user has explicitly set the var.
2. **Client hangs on 503 instead of erroring out.** When the server
   sent `503 Service Unavailable` to a POST, the libcurl client path
   didn't surface this to `cmcp_client_request` — the call hung
   waiting for a JSON-RPC response that the server never sent. Fixed
   in 6.6.x follow-up: `do_post` in `transport_http_client.c` now
   maps `503 → CMCP_EAGAIN` and other non-success status → `CMCP_EIO`,
   so the host's pending request resolves with a return code instead
   of hanging. Regression test:
   `test_post_503_surfaces_eagain` in `tests/test_http_client.c`.

## Comparison vs the TS / Python reference SDKs (Phase 6.6.2)

The comparison harness in `bench/compare/` drives the **cMCP client**
against three different MCP **servers** — cMCP, the official
TypeScript SDK (`@modelcontextprotocol/sdk`), and the official Python
SDK (`mcp`, FastMCP shape). The client is held constant; only the
server changes. Each row is one steady-state `tools/call echo` after
1000 warmup calls; iteration count default 10000.

Numbers from the same Ryzen 5800X box:

| impl | throughput (calls/s) | p50 µs | p99 µs | p999 µs | max µs |
|---|---:|---:|---:|---:|---:|
| cmcp (C, this repo) | 48,669 |  20 |   21 |    26 |     80 |
| ts   (Node 24)      |  8,361 |  89 |  245 |  6,286 | 12,499 |
| py   (CPython 3.14) |  1,043 | 954 | 1,054 | 1,188 |  1,393 |

What the ratios say:

1. **cMCP is ~5.8× faster than the TS SDK at p50** (20 µs vs 89 µs),
   and ~47× faster than the Python SDK (20 µs vs 954 µs). At p99 the
   gap widens to ~12× over TS — V8 GC pauses dominate the TS tail
   (`max = 12.5 ms`), while cMCP's tail is essentially flat
   (`p999 = 26 µs, max = 80 µs`).
2. **cMCP's latency distribution is unusually tight.** `p99 / p50 ≈
   1.05`. There is essentially no tail. This is what you get when
   there's no runtime, no GC, no JIT — just `read()` + parse +
   dispatch + `write()`. The TS SDK and Python SDK both pay
   interpreter overhead per call.
3. **The Python SDK is slower than the TS SDK at p50** but has a
   **tighter tail** (p999 = 1.2 ms vs 6.3 ms; max = 1.4 ms vs
   12.5 ms). CPython's GC pauses are smaller and more frequent than
   V8's, which is the classic interpreter-vs-JIT tradeoff.
4. **cMCP is in the right order of magnitude.** "Are we faster than
   the reference SDKs?" — yes, by 5–50×. That's the right answer for
   a from-scratch C implementation. The interesting question for
   Tier 6.6.3 (profiling) is whether the existing 20 µs has further
   headroom.

The comparison is informational, not a regression gate. Re-running on
a different machine will produce different absolute numbers; the
*ratios* between rows are the more portable signal. To reproduce:

```sh
# One-time toolchain setup:
npm install --prefix bench/compare
python3 -m venv bench/compare/.venv && \
  bench/compare/.venv/bin/pip install -r bench/compare/requirements.txt

make bench-compare    # writes bench/compare/results.csv + prints summary
```

`run.sh` skips the TS or Python row gracefully (one-line reason) if
the toolchain isn't installed; the cMCP row always runs.

## Profile baseline (Phase 6.6.3)

`bench/profile/cpu.sh` + `bench/profile/heap.sh` capture call-graph
and allocation profiles of `bench_server_inline`. The scripts prefer
`perf record` + FlameGraph and `heaptrack`; fall back to
`valgrind --tool=callgrind` and `--tool=massif` (always available —
valgrind is already a project dep).

[`bench/profile/baseline/findings.md`](../bench/profile/baseline/findings.md)
is the durable artifact. The callgrind text dumps in
`bench/profile/baseline/` are the supporting evidence, before and
after the one fix landed in 6.6.3.

Key findings:

- **~38% of CPU is in the allocator.** Working memory is tiny
  (~57 KB peak); the cost is per-call churn — ~30 small mallocs +
  frees per `tools/call`. Largest single lever for future axes; the
  fix shape is a per-request arena (separate axis, not free).
- **~36% in `src/json.c`** (parse + emit + tree manipulation). The
  6.6.3 commit batches `emit_quoted`'s per-character writes into
  runs — dropped total instructions 7.9% under callgrind and lifted
  throughput from 48,813 → 50,487 calls/s on `bench_server_inline`.
- **2% in `cmcp_json_clone` (via `rpc.c`)** — dead clone, every
  caller throws the message away immediately after. Fix needs an
  ownership-transferring `cmcp_rpc_emit_take`; deferred as a
  follow-up.

Updated headline number after the 6.6.3 fix:

| bench                  | throughput | p50 µs | p99 µs |
|------------------------|-----------:|-------:|-------:|
| `server_inline_stdio` (6.6.1) | 48,813 calls/s | 20 | 24 |
| `server_inline_stdio` (6.6.3) | 50,487 calls/s | 19 | 27 |

## HTTP soak (Phase 6.6.4)

`make soak-http` (and `make soak-http-churn`) drive the cMCP client
through the Streamable HTTP transport against a child process
running `tests/soak/echo_http_server`. Same drift criteria as the
stdio soak (`make soak`): RSS ≤ +15% growth, FDs strictly
non-growing, threads equal, p99 latency ≤ 2× drift between the
post-warmup baseline and the end sample.

Closes the Tier-5 deferral: stdio soak landed in 5.6, HTTP soak was
punted on runtime-budget grounds. The two now share `soak_common.h`
(proc sampling, latbuf, workload, CSV schema), so a future drift in
the metric definitions touches one file.

Observed on a 30s smoke run + a 45s churn run (post-warmup, RSS in kB):

| run | parent_rss | parent_fd | parent_threads | child_rss | child_fd | child_threads | p99 µs |
|---|---:|---:|---:|---:|---:|---:|---:|
| `soak-http` smoke      | 9164 → 9164 | 6 → 6 | 3 → 3 | 5888 → 5888 | 5 → 5 | 8 → 8 | 170–190 |
| `soak-http-churn`      | 9320 → 9404 | 6 → 6 | 3 → 3 | 5996 → 5936 | 5 → 5 | 8 → 8 | 162–182 |

(+0.9% parent RSS on churn is well under the 15% threshold; FD /
thread counts are flat across the three child respawns.)

p99 latency tracks the bench (`server_inline_http`: p99 = 238 µs at
the steady-state burn-in of `bench_http`; soak p99 is similar). No
new defects surfaced.

The driver `setenv`s `CMCP_HTTP_ACCEPT_RATE=0` at startup unless the
user explicitly set it — same workaround the `bench_http` micro-bench
applies. Reason: every `tools/call` opens a fresh libcurl easy
handle, so the test saturates the default 200-burst gate in ~2s.
The gate has its own dedicated tests (`test_accept_rate_limit_503`);
the soak is testing leak/stability under sustained traffic, not
the gate itself.

## What this baseline does NOT cover


- **`bench_session_fanout`** — N-server session aggregator latency
  with concurrent fan-in. Phase 6.6.x; punted from 6.6.1.
- **Regression gate.** Tier 7 posture if ever; for now, baselines
  are reference numbers, not CI tripwires.

## What to do with the numbers

If a future change moves throughput by more than ~2× or p99 by more
than ~3×, that change deserves a paragraph in its CHANGELOG entry —
positive *or* negative. Order-of-magnitude movement in either
direction is interesting enough to surface.

Sub-2× variance is noise on a typical desktop; don't optimize for
it without a profile to back the change.
