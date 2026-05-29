#!/usr/bin/env bash
# bench/profile/cpu.sh — CPU profile of bench_server_inline.
#
# Auto-detects the available tooling and produces:
#   - perf (preferred):   bench/profile/baseline/cpu-perf.folded
#                          + cpu-perf.svg if FlameGraph is on PATH
#   - callgrind (fallback): bench/profile/baseline/cpu-callgrind.out
#                          + cpu-callgrind.txt (callgrind_annotate top 30)
#
# Tries perf first; falls back to valgrind --tool=callgrind if not
# available. valgrind is already a project dep (`make valgrind`), so
# the fallback always works.
#
# Iteration count is dialled down for the profile run — we want the
# hot path to dominate, not 50k samples of overhead. Override with
# CMCP_BENCH_N / CMCP_BENCH_WARMUP if you need more signal.

set -u

BENCH_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO_ROOT="$(cd "${BENCH_DIR}/../.." && pwd)"
cd "${REPO_ROOT}"

BIN="${BIN:-bench/bench_server_inline}"
if [ ! -x "${BIN}" ]; then
    echo "${BIN} not built — run \`make bench-build\` first" >&2
    exit 1
fi

OUTDIR="${BENCH_DIR}/baseline"
mkdir -p "${OUTDIR}"

# Defaults tuned for profiling, not throughput measurement.
export CMCP_BENCH_N="${CMCP_BENCH_N:-5000}"
export CMCP_BENCH_WARMUP="${CMCP_BENCH_WARMUP:-500}"

if command -v perf >/dev/null 2>&1; then
    echo "Using perf (CMCP_BENCH_N=${CMCP_BENCH_N})"
    PERF_DATA="${OUTDIR}/cpu-perf.data"
    rm -f "${PERF_DATA}"
    perf record -F 997 -g --call-graph=dwarf -o "${PERF_DATA}" -- \
        "${BIN}" >/dev/null
    perf script -i "${PERF_DATA}" > "${OUTDIR}/cpu-perf.script"
    echo "  raw script → ${OUTDIR}/cpu-perf.script"

    if command -v stackcollapse-perf.pl >/dev/null 2>&1 && \
       command -v flamegraph.pl       >/dev/null 2>&1; then
        stackcollapse-perf.pl "${OUTDIR}/cpu-perf.script" \
            > "${OUTDIR}/cpu-perf.folded"
        flamegraph.pl --title "cMCP bench_server_inline" \
            "${OUTDIR}/cpu-perf.folded" > "${OUTDIR}/cpu-perf.svg"
        echo "  folded   → ${OUTDIR}/cpu-perf.folded"
        echo "  SVG      → ${OUTDIR}/cpu-perf.svg"
    else
        echo "  (stackcollapse-perf.pl + flamegraph.pl not on PATH —"
        echo "   skipping SVG; install FlameGraph repo to regenerate)"
    fi

    # Top-30 self-time list as a committable text artifact.
    perf report --stdio --no-children -i "${PERF_DATA}" 2>/dev/null \
        | sed -n '/^# Overhead/,/^$/p' | head -50 \
        > "${OUTDIR}/cpu-perf-top.txt"
    echo "  top-N    → ${OUTDIR}/cpu-perf-top.txt"
else
    if ! command -v valgrind >/dev/null 2>&1; then
        echo "neither perf nor valgrind found — install one" >&2
        exit 1
    fi
    # Callgrind is roughly 50× slower than native, so step down the
    # workload further. The hot path still dominates at this scale.
    export CMCP_BENCH_N="${CMCP_BENCH_N_CALLGRIND:-2000}"
    export CMCP_BENCH_WARMUP="${CMCP_BENCH_WARMUP_CALLGRIND:-200}"
    echo "Using valgrind --tool=callgrind (CMCP_BENCH_N=${CMCP_BENCH_N})"
    OUT="${OUTDIR}/cpu-callgrind.out"
    rm -f "${OUTDIR}"/callgrind.out.* "${OUT}"
    valgrind --tool=callgrind --collect-jumps=no --collect-systime=no \
             --callgrind-out-file="${OUT}" \
             "${BIN}" >/dev/null 2>&1
    echo "  raw      → ${OUT}"
    if command -v callgrind_annotate >/dev/null 2>&1; then
        callgrind_annotate --threshold=99 --auto=no "${OUT}" \
            > "${OUTDIR}/cpu-callgrind.txt"
        echo "  annotate → ${OUTDIR}/cpu-callgrind.txt"
    fi
fi
