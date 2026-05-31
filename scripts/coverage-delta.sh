#!/bin/sh
# coverage-delta.sh — Tier 7.4 coverage delta gate.
#
# Compares two gcovr `--print-summary` outputs (BASELINE vs CURRENT)
# and:
#   1. Writes a markdown delta table to $OUT_MARKDOWN (default
#      coverage/delta.md) — used by CI to comment on the PR.
#   2. Exits 1 if either `lines` or `functions` dropped more than
#      $THRESHOLD_PP percentage points (default 1.0). Branch
#      coverage is reported in the table but not gated (too noisy
#      with rare error paths).
#
# Inputs are the literal file paths to the two `summary.txt` files
# (each one gcovr's `lines:` / `functions:` / `branches:` block).
#
# Usage:
#   scripts/coverage-delta.sh <baseline.txt> <current.txt>
#
# Env:
#   THRESHOLD_PP   max allowed drop in percentage points (default 1.0)
#   OUT_MARKDOWN   markdown delta output path           (default coverage/delta.md)
#   SKIP_COV       if "1", emit a skip-marker table + exit 0 (commit-message
#                  opt-out path — see docs/coverage-policy.md)

set -eu

BASELINE=${1:-}
CURRENT=${2:-}
THRESHOLD=${THRESHOLD_PP:-1.0}
OUT=${OUT_MARKDOWN:-coverage/delta.md}

if [ -z "$BASELINE" ] || [ -z "$CURRENT" ]; then
    echo "usage: $0 <baseline.txt> <current.txt>" >&2
    exit 2
fi

if [ ! -f "$CURRENT" ]; then
    echo "coverage-delta: current summary missing: $CURRENT" >&2
    exit 2
fi

mkdir -p "$(dirname "$OUT")"

# gcovr summary lines look like:
#   lines: 87.0% (1234 out of 1418)
#   functions: 98.5% (132 out of 134)
#   branches: 65.4% (456 out of 697)
# Extract just the % number (with one decimal place); missing metric → "-"
extract() {
    file=$1; metric=$2
    awk -v m="$metric" '
        $0 ~ ("^" m ":") {
            for (i=1; i<=NF; i++) if ($i ~ /%/) { sub(/%/, "", $i); print $i; exit }
        }
    ' "$file" 2>/dev/null
}

cur_lines=$(extract "$CURRENT" lines)
cur_funcs=$(extract "$CURRENT" functions)
cur_branch=$(extract "$CURRENT" branches)
: "${cur_lines:=}"; : "${cur_funcs:=}"; : "${cur_branch:=}"

# Skip path: commit-message [skip-cov] or env override — emit the
# table but force PASS. Documented in docs/coverage-policy.md.
if [ "${SKIP_COV:-0}" = "1" ]; then
    {
        echo "### Coverage delta (Tier 7.4)"
        echo
        echo "**Skipped** via \`[skip-cov]\` opt-out — see docs/coverage-policy.md."
        echo
        echo "| metric | current |"
        echo "|--------|--------:|"
        printf '| lines     | %s%% |\n' "${cur_lines:-?}"
        printf '| functions | %s%% |\n' "${cur_funcs:-?}"
        printf '| branches  | %s%% |\n' "${cur_branch:-?}"
    } > "$OUT"
    cat "$OUT"
    exit 0
fi

# First main run (no baseline yet) — establish the table without
# gating. Subsequent runs will have a baseline restored from cache.
if [ ! -f "$BASELINE" ]; then
    {
        echo "### Coverage delta (Tier 7.4)"
        echo
        echo "_No baseline cached yet — this is either the first run on a fresh repo,"
        echo "or the cache from \`main\` has expired (7 days unused → eviction)._"
        echo
        echo "| metric | current |"
        echo "|--------|--------:|"
        printf '| lines     | %s%% |\n' "${cur_lines:-?}"
        printf '| functions | %s%% |\n' "${cur_funcs:-?}"
        printf '| branches  | %s%% |\n' "${cur_branch:-?}"
    } > "$OUT"
    cat "$OUT"
    exit 0
fi

base_lines=$(extract "$BASELINE" lines)
base_funcs=$(extract "$BASELINE" functions)
base_branch=$(extract "$BASELINE" branches)
: "${base_lines:=}"; : "${base_funcs:=}"; : "${base_branch:=}"

# awk does float arithmetic and the gate decision in one pass.
gate=$(awk -v bl="$base_lines" -v cl="$cur_lines" \
           -v bf="$base_funcs" -v cf="$cur_funcs" \
           -v bb="$base_branch" -v cb="$cur_branch" \
           -v thr="$THRESHOLD" '
BEGIN {
    dl = (cl == "" || bl == "") ? "" : (cl - bl)
    df = (cf == "" || bf == "") ? "" : (cf - bf)
    db = (cb == "" || bb == "") ? "" : (cb - bb)
    fail = 0
    if (dl != "" && dl < -thr) fail = 1
    if (df != "" && df < -thr) fail = 1
    printf "%s|%s|%s|%d\n", dl, df, db, fail
}')

dl=$(echo "$gate" | awk -F'|' '{print $1}')
df=$(echo "$gate" | awk -F'|' '{print $2}')
db=$(echo "$gate" | awk -F'|' '{print $3}')
fail=$(echo "$gate" | awk -F'|' '{print $4}')

fmt_delta() {
    d=$1
    [ -z "$d" ] && { echo "—"; return; }
    awk -v d="$d" 'BEGIN { printf (d >= 0 ? "+%.2fpp" : "%.2fpp"), d }'
}

verdict=$([ "$fail" = "1" ] && echo "**FAIL** (lines or functions dropped more than ${THRESHOLD}pp)" || echo "**PASS**")

{
    echo "### Coverage delta (Tier 7.4)"
    echo
    echo "$verdict — threshold ±${THRESHOLD}pp on lines + functions; branches informational."
    echo
    echo "| metric    | baseline | current | delta |"
    echo "|-----------|---------:|--------:|------:|"
    printf '| lines     | %s%% | %s%% | %s |\n' "${base_lines:-?}" "${cur_lines:-?}"  "$(fmt_delta "$dl")"
    printf '| functions | %s%% | %s%% | %s |\n' "${base_funcs:-?}" "${cur_funcs:-?}"  "$(fmt_delta "$df")"
    printf '| branches  | %s%% | %s%% | %s |\n' "${base_branch:-?}" "${cur_branch:-?}" "$(fmt_delta "$db")"
    echo
    echo "Override path: add \`[skip-cov]\` to the commit message subject for a"
    echo "legitimate one-off (e.g. deletion of dead-code lines that happen to be"
    echo "tested). See [docs/coverage-policy.md](docs/coverage-policy.md)."
} > "$OUT"

cat "$OUT"

[ "$fail" = "1" ] && exit 1
exit 0
