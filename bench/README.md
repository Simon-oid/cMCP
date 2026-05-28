# bench/

Performance baselines for cMCP. Opt-in via `make bench`; not part of
the default test gate.

## Layout

| File | Purpose |
|---|---|
| `bench_util.h` | Header-only helpers: monotonic clock, fixed-size latency histogram, CSV row emitter. |
| `bench_server_inline.c` | Stdio inline-tool throughput + latency. 50000 iterations. |
| `bench_server_pool.c` | Stdio async fan-out against a sleep tool; surfaces worker-pool concurrency. |
| `bench_http.c` | HTTP-transport variant of the inline bench. 5000 iterations. |
| `run.sh` | Orchestrator: runs each binary, collates CSV rows into `results.csv`, prints a column-aligned summary. |

`results.csv` is produced by `make bench` and is gitignored (numbers
vary per machine, per kernel, per build flags).

## Quick reference

```sh
make bench-build         # compile the three bench binaries
make bench               # run all + write bench/results.csv + print summary

CMCP_BENCH_N=5000     ./bench/bench_server_inline    # smaller measurement window
CMCP_WORKERS=1        ./bench/bench_server_pool      # serialized pool baseline
CMCP_BENCH_WARMUP=100 ./bench/bench_http             # quick HTTP sanity check
```

See [`docs/perf-baselines.md`](../docs/perf-baselines.md) for the
full methodology, observed numbers, and what the ratios mean.

## Out of scope for 6.6.1

- Comparison against the TS / Python reference SDKs (Phase 6.6.2).
- `perf record` + `heaptrack` flamegraphs (Phase 6.6.3).
- HTTP soak driver (Phase 6.6.4 — variant of `tests/soak/`).
- `bench_session_fanout` (Phase 6.6.x).
- Regression gate (Tier 7 posture if ever).
