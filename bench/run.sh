#!/bin/sh
# bench/run.sh — drive the bench binaries, concatenate their CSV rows,
# print a human-readable summary.
#
# Env overrides (forwarded to the bench binaries):
#   CMCP_BENCH_N         iterations per bench
#   CMCP_BENCH_WARMUP    warmup iterations
#   CMCP_BENCH_SLEEP_MS  sleep_tool duration (pool bench only)
#   CMCP_WORKERS         worker-pool size (pool bench only)
#
# Output: bench/results.csv (machine-readable) and stdout (summary).
# Exit non-zero only if a bench fails to run; baseline numbers are
# informational, not gates.
set -eu

cd "$(dirname "$0")"
OUT=results.csv

# Detect CPU pinning capability for steadier numbers, but don't require it.
PIN=
if command -v taskset >/dev/null 2>&1; then
    PIN="taskset -c 0"
fi

BENCHES="
bench_server_inline
bench_server_pool
bench_http
"

# Header (the binaries each emit their own; we collapse to one here).
HEADER='bench,iterations,wall_ms,throughput_per_s,min_us,p50_us,p95_us,p99_us,p999_us,max_us,mean_us,extra'
printf '%s\n' "$HEADER" > "$OUT"

for b in $BENCHES; do
    if [ ! -x "./$b" ]; then
        echo "warning: ./$b missing, run 'make bench-build' first" >&2
        continue
    fi
    echo "=== ./$b ===" >&2
    # Run the bench; capture stdout; drop its emitted header (line 1).
    OUTPUT=$($PIN ./"$b" 2>&1) || { echo "$b failed" >&2; echo "$OUTPUT" >&2; exit 2; }
    echo "$OUTPUT" | tail -n +2 >> "$OUT"
done

echo >&2
echo "results written to $(pwd)/$OUT" >&2
echo >&2

# Pretty-print column-aligned summary to stdout.
if command -v column >/dev/null 2>&1; then
    column -s, -t < "$OUT"
else
    cat "$OUT"
fi
