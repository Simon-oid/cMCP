#!/bin/sh
# Nightly soak orchestrator — Tier 7.3.
#
# Runs the 6h stdio soak followed by the 6h HTTP soak (12h total)
# against whatever HEAD is checked out, persists CSVs + a combined
# log into a dated directory under $SOAK_NIGHTLY_DIR, and exits
# non-zero if either leg's drift gate fires. Designed for local
# cron on a persistent Linux box — the existing
# `make soak` / `make soak-http` targets carry the in-process
# drift criteria (RSS <= +15% growth, FDs strictly non-growing,
# threads equal, p99 <= 2x drift); this script is purely the
# orchestrator that schedules them, captures their artefacts, and
# surfaces pass/fail at the day granularity.
#
# Why sequential and not parallel: the two harnesses share the
# host's CPU/memory budget. Running them concurrently pollutes the
# RSS / p99 drift metrics each one collects on its own server, so
# the gate would noise up. 12h overnight is plenty of budget.
#
# Cron entry (example, 02:00 daily):
#
#   0 2 * * * cd /path/to/cMCP && tests/soak/nightly.sh
#
# Env knobs:
#   SOAK_NIGHTLY_DIR    output root        (default ~/.cmcp-soak)
#   SOAK_DURATION       per-leg duration   (default 21600 = 6h)
#   SOAK_WARMUP         drift-baseline skip (default 600 = 10min)
#   SOAK_INTERVAL       sample cadence     (default 30s)
#   SOAK_NIGHTLY_REBUILD 1 to make clean+make (default 1; 0 to use
#                        existing build, e.g. for smoke tests)
#
# Exit status: 0 on both legs PASS, 1 on either leg FAIL or harness
# error. A file marker (PASSED or FAILED) is also written into the
# dated output directory so monitoring scripts can poll without
# parsing the log.

set -eu

REPO=$(pwd)
DATE=$(date +%F)
OUTROOT=${SOAK_NIGHTLY_DIR:-$HOME/.cmcp-soak}
OUT=$OUTROOT/$DATE
mkdir -p "$OUT"
LOG=$OUT/log.txt

DURATION=${SOAK_DURATION:-21600}
WARMUP=${SOAK_WARMUP:-600}
INTERVAL=${SOAK_INTERVAL:-30}
REBUILD=${SOAK_NIGHTLY_REBUILD:-1}

export SOAK_DURATION=$DURATION
export SOAK_WARMUP=$WARMUP
export SOAK_INTERVAL=$INTERVAL

log() { echo "$*" | tee -a "$LOG"; }

log "=== nightly soak: $DATE ==="
log "  repo:     $REPO"
log "  head:     $(git -C "$REPO" rev-parse --short HEAD 2>/dev/null || echo not-a-repo)"
log "  output:   $OUT"
log "  duration: ${DURATION}s per leg (warmup=${WARMUP}s, interval=${INTERVAL}s)"
log "  rebuild:  $REBUILD"
log ""

if [ "$REBUILD" = "1" ]; then
    log "--- build ---"
    if ! { make clean && make && make soak-build soak-http-build; } >>"$LOG" 2>&1; then
        log "BUILD FAILED — see $LOG"
        touch "$OUT/FAILED"
        exit 1
    fi
    log "  build ok"
    log ""
fi

ec_stdio=0
ec_http=0

log "--- leg 1: stdio soak ---"
if SOAK_OUT="$OUT/stdio.csv" ./tests/soak/run.sh >>"$LOG" 2>&1; then
    log "  stdio: PASS"
else
    ec_stdio=$?
    log "  stdio: FAIL (exit=$ec_stdio)"
fi
log ""

log "--- leg 2: http soak ---"
if SOAK_OUT="$OUT/http.csv" ./tests/soak/run_http.sh >>"$LOG" 2>&1; then
    log "  http:  PASS"
else
    ec_http=$?
    log "  http:  FAIL (exit=$ec_http)"
fi
log ""

log "=== summary ==="
log "  stdio: $([ "$ec_stdio" -eq 0 ] && echo PASS || echo FAIL)"
log "  http:  $([ "$ec_http"  -eq 0 ] && echo PASS || echo FAIL)"

if [ "$ec_stdio" -ne 0 ] || [ "$ec_http" -ne 0 ]; then
    touch "$OUT/FAILED"
    exit 1
fi

touch "$OUT/PASSED"
exit 0
