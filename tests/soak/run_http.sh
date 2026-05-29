#!/bin/sh
# Long-running stability driver — HTTP variant (phase 6.6.4).
#
# Mirrors tests/soak/run.sh but uses soak_http_driver against the
# in-tree echo_http_server. Same CSV schema → same awk drift criteria
# (RSS ≤ +15% growth, FDs strictly non-growing, threads equal, p99
# ≤ 2× drift between baseline and end).
#
# Env overrides (same shape as run.sh):
#   SOAK_DURATION   total runtime in seconds            (default 120, nightly: 21600)
#   SOAK_WARMUP     skip-window before drift baseline   (default 30)
#   SOAK_INTERVAL   metric sample cadence in seconds    (default 5)
#   SOAK_CHURN      0/1, re-spawn server periodically   (default 0)
#   SOAK_OUT        CSV output path                     (default /tmp/cmcp_soak_http.csv)

set -eu

DURATION=${SOAK_DURATION:-120}
WARMUP=${SOAK_WARMUP:-30}
INTERVAL=${SOAK_INTERVAL:-5}
CHURN=${SOAK_CHURN:-0}
OUT=${SOAK_OUT:-/tmp/cmcp_soak_http.csv}

DRIVER=./tests/soak/soak_http_driver
SERVER=./tests/soak/echo_http_server

[ -x "$DRIVER" ] || { echo "soak-http: driver missing — run 'make soak-http' first" >&2; exit 1; }
[ -x "$SERVER" ] || { echo "soak-http: server missing — run 'make soak-http' first" >&2; exit 1; }

CHURN_FLAG=""
[ "$CHURN" = "1" ] && CHURN_FLAG="--churn"

echo "=== soak-http: duration=${DURATION}s warmup=${WARMUP}s interval=${INTERVAL}s churn=${CHURN} ==="
"$DRIVER" --duration="${DURATION}" \
          --sample-interval="${INTERVAL}" \
          --server="$SERVER" \
          ${CHURN_FLAG} \
    | tee "$OUT"

echo "=== drift check (baseline = first sample with elapsed >= warmup) ==="

set +e
awk -F, -v warmup="${WARMUP}" '
NR == 1 { next }  # header
{
    elapsed[NR-1]  = $1
    rss[NR-1]      = $2
    fd[NR-1]       = $3
    threads[NR-1]  = $4
    crss[NR-1]     = $5
    cfd[NR-1]      = $6
    cthreads[NR-1] = $7
    p99[NR-1]      = $10
    n = NR - 1
}
END {
    if (n < 4) {
        printf "drift: only %d samples — increase SOAK_DURATION\n", n
        exit 1
    }
    base = 0
    for (i = 1; i <= n; i++) {
        if (elapsed[i] >= warmup) { base = i; break }
    }
    if (base == 0) {
        printf "drift: warmup (%ds) exceeded duration — choose longer SOAK_DURATION\n", warmup
        exit 1
    }
    fail = 0
    if (rss[n] > rss[base] * 1.15) {
        printf "FAIL parent_rss: %d -> %d kB (>15%% growth)\n", rss[base], rss[n]
        fail = 1
    }
    if (crss[n] > crss[base] * 1.15) {
        printf "FAIL child_rss:  %d -> %d kB (>15%% growth)\n", crss[base], crss[n]
        fail = 1
    }
    if (fd[n] > fd[base]) {
        printf "FAIL parent_fd:  %d -> %d (FD leak)\n", fd[base], fd[n]
        fail = 1
    }
    if (cfd[n] > cfd[base]) {
        printf "FAIL child_fd:   %d -> %d (FD leak)\n", cfd[base], cfd[n]
        fail = 1
    }
    if (threads[n] != threads[base]) {
        printf "FAIL parent_threads: %d -> %d\n", threads[base], threads[n]
        fail = 1
    }
    if (cthreads[n] != cthreads[base]) {
        printf "FAIL child_threads:  %d -> %d\n", cthreads[base], cthreads[n]
        fail = 1
    }
    if (p99[base] > 0 && p99[n] > p99[base] * 2) {
        printf "FAIL p99_latency: %d -> %d us (>2x)\n", p99[base], p99[n]
        fail = 1
    }
    if (fail) exit 1
    printf "PASS soak-http (samples=%d, baseline elapsed=%ds)\n", n, elapsed[base]
}
' "$OUT"
rc=$?
exit $rc
