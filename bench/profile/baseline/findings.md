# cMCP profile findings — first pass

Captured against `bench/bench_server_inline` (in-process pipe pair,
`tools/call echo` 2000 × under callgrind). All numbers are
instruction counts (`Ir`) — wall-clock varies more across runs, but
under callgrind the instruction count is deterministic and lets us
talk about ratios with confidence. Wall-clock confirmation follows
the headline.

## Headline: where the cycles go

Before the 6.6.3 fix, `bench_server_inline` spent its 155.6 M
instructions roughly as follows:

| bucket | share | notes |
|---|---:|---|
| allocator (glibc malloc/free/calloc/realloc + internals) | ~38% | per-call JSON trees, ~30+ allocations per `tools/call` |
| `src/json.c` (parse + emit + tree manipulation) | ~36% | `emit_raw` (8.5%), `emit_quoted` (5%), `object_set_n` (4.5%), `parse_string` (4%), `object_get` (3%), `clone` (~2%), `free` (~2.5%) |
| libc string ops (`memcpy`, `strlen`, `memcmp`, `strcmp`) | ~9% | substring matches in JSON parser + object-key lookup |
| schema validator (`src/schema.c`) | ~0.7% | input schema is small (`echo` has one field) |
| RPC framing (`src/rpc.c`) | ~0.3% | thin layer |
| pthread mutex lock/unlock | ~1.5% | worker pool + transport writer |
| dl_lookup / dynamic linker | ~3% | one-shot at startup; amortised at scale, just visible here at N=2000 |

Heap footprint at steady state (massif): **~57 KB peak total**.
Working memory is *tiny*; the 38% in the allocator is pure
**churn**, not size. Per call we malloc + free ~30 small objects
(JSON tree nodes), all freed before the next call returns. The
arena pattern would eliminate most of this.

## What we fixed in this pass

**`emit_quoted` was calling `emit_raw` once per character.** For
strings with no escape characters (the common case — tool names,
field names, payloads), this meant N function calls + N×1-byte
`memcpy` calls instead of one `memcpy` of N bytes. Batched it: the
emitter now scans for runs of "normal" printable ASCII, then
flushes each run in one `emit_raw` call when it hits an escape
character or the end of the string.

| function | before | after | delta |
|---|---:|---:|---:|
| `emit_raw`     | 8.45% / 13.15 M Ir | 4.94% /  7.07 M Ir | **−46%** instructions |
| `emit_quoted`  | 4.95% /  7.70 M Ir | 3.25% /  4.65 M Ir | **−40%** instructions |
| **PROGRAM TOTALS** | 155.56 M Ir | 143.27 M Ir | **−7.9%** total instructions |

The full program counter dropped 7.9% from a one-line emit
refactor. Wall-clock on `bench_server_inline` confirms:

| | calls/s | p50 µs | p99 µs |
|---|---:|---:|---:|
| 6.6.1 baseline (before) | 48,813 | 20 | 24 |
| 6.6.3 (after)           | 50,487 | 19 | 27 |

Wall-clock improves ~3.4% — smaller than the instruction-count
improvement because the bench is partly memory-bound (cache misses,
not instruction throughput, dominate the remaining time). The
shape of the win is right though: we now do less work per call
without changing any observable behaviour.

## What we did NOT fix (deferred to future axes)

These are visible in the profile but each is a real refactor, not a
free fix. Documenting them here so the next axis can pick its
target from data.

### 1. Allocator churn — ~38% of CPU

The biggest single bucket. Every `tools/call` allocates a JSON
request tree (parsed from the wire) and a JSON response tree
(built by the handler + dispatch), each with ~10–15 small node
allocations. All are freed before the next call. Net working
memory is 57 KB peak; the cost is in `_int_malloc` / `_int_free`
overhead.

**Possible levers** (sized as separate axis-6.6.x work):

- Per-request arena: allocate a 4-KB slab at call entry, bump-allocate
  node + key + string into it, free the whole slab on call exit.
  Reduces ~30 mallocs to ~1.
- Free-list reuse for `cmcp_json_t` nodes (small struct, predictable
  size).
- Inline small strings into the `cmcp_json_t` node when they fit.

This is the next big lever. The right shape is a separate axis
(call it 6.6.x — arena pool), because it touches ownership semantics
and needs its own test coverage.

### 2. `cmcp_json_clone` in `rpc.c` — ~2% of CPU

`cmcp_rpc_to_json` clones `params` / `result` from the message
struct into the wire-tree, because `cmcp_json_object_set` takes
ownership and the original message must remain valid. In practice,
every call site of `send_message` immediately calls
`cmcp_rpc_message_clear` afterwards — the message *is* about to be
thrown away, the clone is dead weight.

**Fix shape:** add `cmcp_rpc_emit_take` that consumes the message
(transfers ownership of `params`/`result` into the wire-tree,
nulls them in the source), keep the existing `cmcp_rpc_emit` as a
clone-and-emit wrapper for callers that want const semantics. Then
flip the `send_message` callers to `_take` and drop the explicit
`_clear`.

Not free — it's an API addition and a callsite sweep. Punted.

### 3. `cmcp_json_object_get` — 3% of CPU (linear scan)

Object lookup is a linear scan over keys. For tool-call params (2
fields) and handler-result content (1–2 fields), the constant is
small and the hash-map win is marginal. Real wins would need
significantly larger objects, which we don't see in steady-state
MCP traffic. **Not a fix candidate.**

### 4. libc memcpy / strcmp / strlen — ~9% of CPU

These are the implementations behind our `emit_raw`,
`cmcp_json_object_set_n` (memcmp for duplicate-key checks), and
JSON string interning. AVX2 is already in use. The lever here is
*calling them less* (which the emit_quoted fix above does), not
making them faster.

## How to reproduce

```sh
make bench-build
bench/profile/cpu.sh        # callgrind, ~2 min; writes baseline/cpu-*
bench/profile/heap.sh       # massif, ~1 min; writes baseline/heap-*
```

The before/after callgrind text dumps committed in this directory
were both captured at `CMCP_BENCH_N=2000 CMCP_BENCH_WARMUP=200` on
the same Ryzen 5800X box `bench/results.csv` was captured on.
Re-running on a different machine produces different absolute
counts; the *ratios* port across boxes.
