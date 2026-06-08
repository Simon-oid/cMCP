# Nightly soak — operational guide (Tier 7.3)

cMCP's soak harness exercises a steady `tools/call` workload against a
live echo server for `$SOAK_DURATION` seconds, sampling `/proc` metrics
for both the parent (driver) and child (server) every
`$SOAK_INTERVAL` seconds, and applying a drift gate at the end:

| Metric        | Criterion (baseline → final)                          |
|---------------|-------------------------------------------------------|
| Parent RSS    | ≤ +15 % growth                                        |
| Child RSS     | ≤ +15 % growth                                        |
| Parent FDs    | strictly non-growing                                  |
| Child FDs     | strictly non-growing                                  |
| Parent threads| equal (pools are bounded)                             |
| Child threads | equal                                                 |
| p99 latency   | ≤ 2 × baseline                                        |

The Tier-5 closure already ships `make soak` / `make soak-churn` /
`make soak-http` / `make soak-http-churn`, each capable of running for
6 hours via `SOAK_DURATION=21600`. What Tier 7.3 adds is the
**orchestrator** that runs both transports back-to-back, persists
per-day CSVs, and surfaces a single pass/fail at the day granularity
so a cron entry can drive the whole thing.

## The orchestrator

```
tests/soak/nightly.sh
```

Sequential, not parallel — concurrent runs share the host's CPU/mem
budget and pollute each other's RSS / p99 drift signal, which is
exactly what the gate measures. 12 hours of overnight budget is
plenty.

Env knobs (all optional):

| Variable             | Default              | Purpose                                     |
|----------------------|----------------------|---------------------------------------------|
| `SOAK_NIGHTLY_DIR`   | `~/.cmcp-soak`       | output root                                 |
| `SOAK_DURATION`      | `21600` (6 h)        | per-leg runtime                             |
| `SOAK_WARMUP`        | `600` (10 min)       | skip-window before drift baseline           |
| `SOAK_INTERVAL`      | `30` s               | sample cadence                              |
| `SOAK_NIGHTLY_REBUILD` | `1`                | `0` to skip `make clean && make && make soak-http` (smoke tests) |

Exit status: `0` on both legs PASS, `1` on either leg FAIL or harness
error. A file marker (`PASSED` or `FAILED`) is also written into the
dated output directory so a monitoring script can poll without parsing
the log.

## Output layout

```
$SOAK_NIGHTLY_DIR/
  2026-05-31/
    log.txt        full combined stdout+stderr (build + both legs)
    stdio.csv      tests/soak/run.sh CSV (parent+child /proc samples)
    http.csv       tests/soak/run_http.sh CSV
    PASSED         (or FAILED, mutually exclusive)
  2026-06-01/
    ...
```

A week of nightlies fits in ~20 MB. Retain by deleting old dated
directories.

## Cron entry

Recommended `crontab -e` line for a local Linux box (02:00 daily):

```
0 2 * * * cd /home/user/cMCP && tests/soak/nightly.sh >/dev/null 2>&1
```

The script tees everything into `log.txt` already; the redirect just
silences cron's `MAILTO` plumbing. To get notified on failure, drop in
a one-liner check after the run:

```
5 8 * * * test -f /home/user/.cmcp-soak/$(date +\%F)/PASSED || \
          notify-send -u critical 'cMCP soak FAILED'
```

(Substitute `notify-send` with whatever your monitoring story is —
mail, push, dashboard.)

## Smoke test

To verify the wiring without spending 12 hours, run a 1-minute version
against the existing build (no rebuild):

```sh
SOAK_DURATION=60 \
SOAK_WARMUP=10 \
SOAK_INTERVAL=5 \
SOAK_NIGHTLY_REBUILD=0 \
tests/soak/nightly.sh
```

Total ~2 min (60 s stdio + 60 s HTTP + a few seconds of `awk` drift
checking). The dated output directory + PASSED marker should appear;
the log should show `PASS soak` and `PASS soak-http`.

## Triage when a nightly fails

1. Look at the marker file: `cat $SOAK_NIGHTLY_DIR/<date>/FAILED` (it's
   empty, but its presence vs `PASSED` is the day-level signal).
2. Open `log.txt`; the failing leg (`stdio` or `http`) prints the
   `FAIL` line that tripped the awk gate, naming the metric.
3. Open the corresponding CSV (`stdio.csv` or `http.csv`); look at the
   first sample after `elapsed >= warmup` (the baseline) and the final
   sample for the metric in question. `awk -F, 'NR==1||NR==2||END' …`
   prints both rows quickly.
4. If it's a slow leak: the relevant metric grows monotonically across
   the CSV. The slope tells you whether it's a per-call leak (steady)
   or a one-shot allocation that never frees (step). Cross-reference
   with the `tools/cmcp-tee` capture if the leg was teed.
5. If it's a p99 spike: the metric is bimodal — the baseline is fine,
   the final is high, but intermediate samples may also be high. Plot
   the column with `awk -F, 'NR>1 {print $1","$10}' stdio.csv | …` and
   look for the regime change.

If the failure repeats across multiple consecutive nights, it is a
regression; bisect against the commits between the last green and the
first red night (`git log --since=...`).

## What this is NOT

- **A CPU benchmark.** Soak measures *drift*, not steady-state
  throughput. `make bench` is the perf instrument.
- **A correctness gate.** Soak doesn't cross-check responses against
  expectations; `make replay` / `make test` / `make conformance` do
  that. Soak only checks for the resource-leak / unbounded-thread /
  latency-tail patterns that surface at hour scale.
- **A CI lane.** The orchestrator is designed for a local cron on a
  persistent Linux box. GitHub Actions shared runners have ±15 %
  jitter on small workloads and a 6-hour-per-job limit — both
  tolerable individually, but stacking two 6-hour legs on shared
  hardware would noise the gate up to the point where it would page
  on green. See `TODO.md` Tier 7 open question 1 for the rationale.

## Related

- [`tests/soak/run.sh`](../tests/soak/run.sh) — per-leg stdio harness
  (drift-criteria awk lives here)
- [`tests/soak/run_http.sh`](../tests/soak/run_http.sh) — same for HTTP
- [`docs/perf-baselines.md`](perf-baselines.md) — companion: steady-state
  perf numbers (Tier 6.6.1)
- [`CLAUDE.md`](../CLAUDE.md) — env-var reference incl. `CMCP_WORKERS`
