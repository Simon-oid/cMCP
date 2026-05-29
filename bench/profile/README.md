# bench/profile/

CPU + heap profile baseline for cMCP. Opt-in scripts; not part of
`make bench` or any other default gate. Methodology + observed
hotspots live in [`baseline/findings.md`](baseline/findings.md);
that file is the durable artifact, the raw `.out` and per-tool text
dumps under `baseline/` are the supporting evidence.

## Layout

| File | Purpose |
|---|---|
| `cpu.sh` | CPU profile of `bench_server_inline`. Prefers `perf record` + FlameGraph; falls back to `valgrind --tool=callgrind` + `callgrind_annotate`. |
| `heap.sh` | Allocation profile of `bench_server_inline`. Prefers `heaptrack`; falls back to `valgrind --tool=massif` + `ms_print`. |
| `baseline/findings.md` | Triage summary — hot paths, what got fixed in this pass, what's deferred. |
| `baseline/cpu-callgrind-before.txt` | `callgrind_annotate` summary before the 6.6.3 `emit_quoted` batching fix. |
| `baseline/cpu-callgrind.txt` | `callgrind_annotate` summary after the fix. |
| `baseline/heap-massif.txt` | `ms_print` heap snapshot timeline. |

Only the human-reviewable `.txt` summaries are tracked. The raw
`.out` files (callgrind, massif) are large, machine-specific, and
referenced absolute paths — regenerate them locally with the
scripts when you want to drive KCachegrind / interactive tooling.

## Quick reference

```sh
make bench-build                  # bench_server_inline must exist first
bench/profile/cpu.sh              # writes baseline/cpu-{tool}.{out,txt}
bench/profile/heap.sh             # writes baseline/heap-{tool}.{out,txt}

# Override iteration counts (default = 5000 for perf/heaptrack,
# stepped down to 2000 under valgrind because it's 50× slower):
CMCP_BENCH_N=10000 bench/profile/cpu.sh
```

Both scripts auto-detect the available toolchain:

- `cpu.sh` prefers `perf` (kernel profiler, sub-1% overhead, can drive
  FlameGraph SVGs). Falls back to `valgrind --tool=callgrind`
  (instrumentation, ~50× slower, but always available since cMCP
  already depends on valgrind for `make valgrind`).
- `heap.sh` prefers `heaptrack` (allocation tracker, can drive
  flamegraphs of malloc-by-stack). Falls back to `valgrind
  --tool=massif` (heap-timeline snapshots).

If you have `perf` + the [FlameGraph](https://github.com/brendangregg/FlameGraph)
scripts on `PATH`, `cpu.sh` will additionally emit
`baseline/cpu-perf.svg`. That file is gitignored — flamegraphs are
large and machine-specific; regenerate locally.

## Why measure this at all?

The 6.6.1 baseline showed cMCP at 48k stdio calls/s with p50 = 20 µs.
At that scale, every 1% of CPU shows up at ~0.2 µs — small enough to
be drowned in scheduler / cache noise on a real box, but big enough
that *systemic* overhead (allocator churn, redundant copies, hot
linear scans) shows up clearly under callgrind's deterministic
instruction count.

The 6.6.3 plan called for "triage obvious wins, fix what's free,
defer big refactors." This commit applies the obvious free win
(JSON emitter batching) and triages the rest for future axes —
specifically the allocator-pressure story, which is the next big
lever and needs an arena, not a 1-line edit.

## Out of scope for 6.6.3

- **Arena allocator for per-call JSON trees.** The profile data
  motivates this (allocator is ~38% of CPU); the implementation is
  a refactor sized for its own axis.
- **Lifecycle refactor to remove rpc_to_json clones.** Same shape —
  needs an API change (ownership-transferring emit variant), not
  free.
- **HTTP-transport profile.** Stdio-only profile in 6.6.3; HTTP
  baseline lands with 6.6.4 (HTTP soak).
- **Per-PR regression gate on cycle counts.** Baselines are
  reference numbers; a CI tripwire is a Tier 7 posture if ever.
