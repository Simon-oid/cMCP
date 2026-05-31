#!/bin/sh
# bench/compare-baseline.sh — Tier 7.1 perf-regression gate.
#
# Runs `bench/run.sh` $N times (default 11), takes the per-metric
# median across runs, compares against `bench/baseline.json`, writes
# a markdown table to $OUT_MARKDOWN (default bench/delta.md), and
# exits 1 if any gated metric regressed past its per-metric
# tolerance band.
#
# Direction handling:
#   higher_is_better → fail if current < baseline * (1 - tolerance)
#   lower_is_better  → fail if current > baseline * (1 + tolerance)
#
# Median-of-N mitigates GitHub Actions shared-runner jitter (±15-30 %
# on small workloads). Per-metric tolerance bands further pad. See
# docs/perf-regression-gate.md for the calibration story.
#
# Env knobs:
#   BENCH_N           runs to median over   (default 11)
#   BASELINE_JSON     baseline path         (default bench/baseline.json)
#   OUT_MARKDOWN      delta-table path      (default bench/delta.md)
#   SKIP_BENCH        if 1, emit skip-marker + exit 0 (commit-message
#                     opt-out — [skip-bench] in the head subject)
#
# Dependencies: jq for JSON parsing, awk for arithmetic. Both
# preinstalled on ubuntu-latest.

set -eu

N=${BENCH_N:-11}
BASELINE=${BASELINE_JSON:-bench/baseline.json}
OUT=${OUT_MARKDOWN:-bench/delta.md}
RUNS_DIR=${BENCH_RUNS_DIR:-bench/runs}

mkdir -p "$(dirname "$OUT")" "$RUNS_DIR"

# --- skip path (commit message opt-out) ----------------------------
if [ "${SKIP_BENCH:-0}" = "1" ]; then
    {
        echo "### Perf regression (Tier 7.1)"
        echo
        echo "**Skipped** via \`[skip-bench]\` opt-out — see docs/perf-regression-gate.md."
    } > "$OUT"
    cat "$OUT"
    exit 0
fi

if [ ! -f "$BASELINE" ]; then
    echo "compare-baseline: $BASELINE missing" >&2
    exit 2
fi

command -v jq >/dev/null || { echo "compare-baseline: jq missing"   >&2; exit 2; }

# --- run the bench N times -----------------------------------------
echo "=== bench: $N runs ===" >&2
i=1
while [ "$i" -le "$N" ]; do
    OUTFILE="$RUNS_DIR/run-$i.csv"
    # bench/run.sh writes to bench/results.csv unconditionally.
    bench/run.sh >/dev/null 2>&1 || {
        echo "compare-baseline: run $i failed" >&2
        exit 2
    }
    cp bench/results.csv "$OUTFILE"
    echo "  run $i: ok" >&2
    i=$((i + 1))
done

# --- compute medians per (bench, metric) ---------------------------
# The CSV schema (bench/run.sh header) is:
#   bench,iterations,wall_ms,throughput_per_s,min_us,p50_us,p95_us,p99_us,p999_us,max_us,mean_us,extra
# columns:1   2          3       4                5      6      7      8      9      10     11       12
COL_wall_ms=3
COL_throughput_per_s=4
COL_p50_us=6
COL_p99_us=8

median() {
    # stdin: numbers, one per line. stdout: median.
    sort -n | awk '
        { a[NR] = $1 }
        END {
            if (NR == 0) { print "0"; exit }
            if (NR % 2) { print a[(NR+1)/2] }
            else        { printf "%.6f\n", (a[NR/2] + a[NR/2+1]) / 2 }
        }'
}

extract_col() {
    bench=$1; col=$2
    for f in "$RUNS_DIR"/run-*.csv; do
        awk -F, -v b="$bench" -v c="$col" '
            NR == 1 { next }
            $1 == b { print $c }
        ' "$f"
    done | median
}

# --- evaluate per-metric gate --------------------------------------
fail=0
table=""
# Iterate jq-flat over baseline metrics. Each row: bench metric direction baseline tolerance
mapping=$(jq -r '
    .metrics | to_entries[] | .key as $b |
    .value | to_entries[] | "\($b)\t\(.key)\t\(.value.direction)\t\(.value.baseline)\t\(.value.tolerance)"
' "$BASELINE")

# Make the column lookup table available to the loop subshell.
col_for() {
    case "$1" in
        wall_ms)          echo "$COL_wall_ms" ;;
        throughput_per_s) echo "$COL_throughput_per_s" ;;
        p50_us)           echo "$COL_p50_us" ;;
        p99_us)           echo "$COL_p99_us" ;;
        *) echo "" ;;
    esac
}

while IFS=$(printf '\t') read -r bench metric direction base tol; do
    [ -z "$bench" ] && continue
    col=$(col_for "$metric")
    if [ -z "$col" ]; then
        echo "compare-baseline: no column mapping for metric '$metric'" >&2
        exit 2
    fi
    cur=$(extract_col "$bench" "$col")
    if [ -z "$cur" ] || [ "$cur" = "0" ]; then
        echo "compare-baseline: no data for $bench.$metric" >&2
        exit 2
    fi

    # awk computes: delta_pct, verdict, formatted line
    eval_line=$(awk -v cur="$cur" -v base="$base" -v tol="$tol" -v dir="$direction" '
BEGIN {
    delta_pct = (cur - base) / base * 100.0
    if (dir == "higher_is_better") {
        threshold = -tol * 100.0
        fail = (delta_pct < threshold) ? 1 : 0
    } else if (dir == "lower_is_better") {
        threshold = tol * 100.0
        fail = (delta_pct > threshold) ? 1 : 0
    } else {
        threshold = 0
        fail = 0
    }
    printf "%.2f|%d", delta_pct, fail
}')
    delta_pct=$(echo "$eval_line" | awk -F'|' '{print $1}')
    metric_fail=$(echo "$eval_line" | awk -F'|' '{print $2}')

    if [ "$metric_fail" = "1" ]; then
        fail=1
        sign=":x:"
    else
        sign=":white_check_mark:"
    fi
    sign_pct=$(awk -v d="$delta_pct" 'BEGIN { printf (d >= 0 ? "+%.2f%%" : "%.2f%%"), d }')
    cur_fmt=$(awk -v c="$cur" 'BEGIN { printf "%.2f", c }')
    base_fmt=$(awk -v b="$base" 'BEGIN { printf "%.2f", b }')
    tol_fmt=$(awk -v t="$tol" 'BEGIN { printf "±%.0f%%", t*100 }')

    line=$(printf '| %s | %s | %s | %s | %s | %s | %s |' \
        "$bench" "$metric" "$base_fmt" "$cur_fmt" "$sign_pct" "$tol_fmt" "$sign")
    table=$(printf '%s\n%s' "$table" "$line")
done <<EOF
$mapping
EOF

verdict=$([ "$fail" = "1" ] \
    && echo "**FAIL** — at least one metric regressed past its tolerance band" \
    || echo "**PASS** — all metrics within tolerance")

{
    echo "### Perf regression (Tier 7.1)"
    echo
    echo "$verdict (median of $N runs on this CI host)."
    echo
    echo "| bench | metric | baseline | current | delta | tolerance | gate |"
    echo "|-------|--------|---------:|--------:|------:|----------:|:----:|"
    echo "$table" | sed '/^$/d'
    echo
    echo "Direction: throughput is higher-is-better; wall / p99 are lower-is-better."
    echo "Override path: add \`[skip-bench]\` to the commit message subject for a"
    echo "legitimate one-off. To bump the baseline intentionally, see"
    echo "[docs/perf-regression-gate.md](docs/perf-regression-gate.md)."
} > "$OUT"

cat "$OUT"

[ "$fail" = "1" ] && exit 1
exit 0
