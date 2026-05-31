# Perf regression gate (Tier 7.1)

cMCP's CI lane runs `make bench` (three workloads — stdio inline, stdio
worker-pool, HTTP inline) eleven times and takes the per-metric median,
then diffs against `bench/baseline.json`. A PR that regresses a gated
metric past its tolerance band fails this check and gets a comment
showing the delta table.

## Gated metrics

| Workload              | Metric             | Direction         | Default tolerance |
|-----------------------|--------------------|-------------------|------------------:|
| `server_inline_stdio` | `throughput_per_s` | higher_is_better  | ±25 %             |
| `server_inline_stdio` | `p99_us`           | lower_is_better   | ±40 %             |
| `server_pool_stdio`   | `wall_ms`          | lower_is_better   | ±20 %             |
| `server_inline_http`  | `throughput_per_s` | higher_is_better  | ±30 %             |
| `server_inline_http`  | `p99_us`           | lower_is_better   | ±40 %             |

Per-metric tolerance bands are wider for HTTP (a real socket + a real
syscall round-trip per call) than for stdio (in-process pipes), and
wider for tail latency than for throughput (the tail is what jitter
hits hardest). Defaults are tuned for the noise floor of GitHub
Actions ubuntu-latest shared runners — see _Risk: CI noise_ below.

## How the gate works

For each metric:

```
delta_pct = (current - baseline) / baseline * 100

higher_is_better:  fail if delta_pct < -tolerance_pct
lower_is_better:   fail if delta_pct >  tolerance_pct
```

`current` is the median of N=11 runs, computed by
`bench/compare-baseline.sh`. `baseline` lives in `bench/baseline.json`.

The gate is binary per metric. If any gated metric fails, the job
exits non-zero and the PR check goes red.

## Risk: CI noise

GitHub Actions shared runners have documented ±15-30 % latency
jitter on small workloads, which is **wider than the regressions we
want to catch**. Mitigations stacked:

1. **Median of N=11** runs per metric — the central order statistic
   shrugs off occasional outliers.
2. **Generous per-metric bands** — tight enough to catch a 30 %
   regression on the inline-stdio fast path, loose enough that a
   green PR doesn't go red on noise.
3. **HTTP gets wider bands** — accept() / TCP scheduling adds
   variance even median-of-11 can't fully hide.
4. **`[skip-bench]` opt-out** — the escape valve when the noise wins
   despite the above, or when an intentional perf hit lands.

If the gate becomes flaky (frequent false positives on no-op PRs),
the answer is to widen the offending metric's tolerance, not to drop
the gate. Calibration is part of operating the gate.

A self-hosted bare-metal runner would let us tighten the bands to
±10 % and catch much smaller regressions. That's the Tier 8 follow-
up if traffic justifies the lab cost.

## Baseline maintenance

`bench/baseline.json` is **committed** — it is not auto-updated by CI.
This is deliberate (Tier 7 open question 2 in `TODO.md`): explicit
beats inferred. A PR that intentionally changes performance comes in
two commits:

1. The code change. Gate trips → CI fails. PR description explains the
   trade-off.
2. The baseline bump. `bench/baseline.json` updated with new numbers.
   PR description references commit 1 and explains *why* the new
   numbers are the right floor (e.g. "+0.5 µs/call to validate
   schemas server-side — buys -32602 spec compliance, see <issue>").

This makes baseline changes reviewable: every drop in expectations
shows up in the diff with a paper trail. By contrast, an auto-updating
baseline would silently absorb slow leaks — exactly what Tier 7 is
guarding against.

### When to update the baseline

- The PR intentionally trades perf for some other property (security,
  correctness, capability) and the loss is within the new tolerance
  band.
- Hardware substrate of the CI runner materially changed (GitHub
  swapped the underlying VM family, runner image kernel upgrade with
  meaningful overhead change). Catch via a cluster of no-op PRs all
  trending red on the same metric.
- A genuine optimisation landed and you want the new floor recorded
  so subsequent regressions are caught at the better number, not the
  old one.

### When NOT to update the baseline

- "The gate is annoying" — calibrate the band, don't move the floor.
- A single noisy run tripped — re-run; the median should settle.
- One metric is slightly under tolerance on this PR but the cause is
  unknown — don't paper over; investigate first.

## Override path

For a one-off where you've confirmed the perf delta is intentional and
you don't want to bump the baseline in the same PR, add `[skip-bench]`
to the **HEAD commit subject**. CI reads `git log -1 --pretty=%s`,
sets `SKIP_BENCH=1`, and the compare script emits a "Skipped" table
and exits 0.

Don't lean on this — the gate exists for a reason, and skipped PRs
don't refresh the baseline.

## Locally reproducing the gate

```sh
make bench-build
BENCH_N=11 ./bench/compare-baseline.sh
echo "exit=$?"
```

Exit code is the gate verdict; `bench/delta.md` is what CI would
post. Note that local results vary by ±5x from CI numbers (your laptop
is faster than GitHub Actions shared runners), so a *delta* against
the committed baseline is meaningful only at CI parity — locally,
use the script to check that the **shape** of the output is right
(no NaNs, no missing metrics) and the **direction** matches what your
change should produce.

## Adding a new gated metric

1. Add the metric column to `bench/run.sh` CSV header + the producing
   bench binary.
2. Map the column index in `bench/compare-baseline.sh`'s `col_for()`.
3. Add the entry under `metrics.<workload>` in `bench/baseline.json`
   with `direction` and `tolerance`.
4. Wait one CI cycle on main to confirm the baseline number is
   actually representative of the runner; tune `tolerance` if the
   per-run noise turns out wider than expected.

## What is NOT covered by this gate

- **Memory / heap** — `bench` measures latency + throughput, not
  RSS. Heap leaks are caught by valgrind + sanitisers (`make
  test-asan`, `make valgrind`).
- **Long-running drift** — the `tests/soak/` family covers that
  (Tier 7.3); soak measures *change over hours*, this gate measures
  *steady-state cost per call*.
- **Per-machine absolute targets** — this gate is comparative. The
  reference numbers in `docs/perf-baselines.md` are the absolute
  targets, but they live in narrative prose, not in CI.

## Related

- [`bench/run.sh`](../bench/run.sh) — produces `bench/results.csv`
- [`bench/compare-baseline.sh`](../bench/compare-baseline.sh) — the
  median-of-N + diff comparator
- [`bench/baseline.json`](../bench/baseline.json) — the floor
- [`docs/perf-baselines.md`](perf-baselines.md) — absolute baseline
  numbers + run methodology
- [`TODO.md`](../TODO.md) §7.1 — design notes + rationale
