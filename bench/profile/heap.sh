#!/usr/bin/env bash
# bench/profile/heap.sh — heap-allocation profile of bench_server_inline.
#
# Auto-detects the available tooling and produces:
#   - heaptrack (preferred): bench/profile/baseline/heap-heaptrack.txt
#   - massif    (fallback):  bench/profile/baseline/heap-massif.txt
#
# valgrind --tool=massif is always available (project dep). heaptrack
# gives a richer allocation flame graph; install it via the system
# package manager if you want SVGs.

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

export CMCP_BENCH_N="${CMCP_BENCH_N:-5000}"
export CMCP_BENCH_WARMUP="${CMCP_BENCH_WARMUP:-500}"

if command -v heaptrack >/dev/null 2>&1; then
    echo "Using heaptrack (CMCP_BENCH_N=${CMCP_BENCH_N})"
    DAT="${OUTDIR}/heap-heaptrack.zst"
    rm -f "${DAT}"
    heaptrack -o "${DAT%.zst}" "${BIN}" >/dev/null 2>&1
    if command -v heaptrack_print >/dev/null 2>&1; then
        # heaptrack writes its raw file with a suffix; find it.
        ACTUAL="$(ls -t "${OUTDIR}"/heap-heaptrack.*.zst 2>/dev/null | head -1)"
        [ -z "${ACTUAL}" ] && ACTUAL="${DAT}"
        heaptrack_print "${ACTUAL}" > "${OUTDIR}/heap-heaptrack.txt"
        echo "  text     → ${OUTDIR}/heap-heaptrack.txt"
        echo "  raw      → ${ACTUAL}"
    else
        echo "  raw      → ${DAT}"
        echo "  (install heaptrack_print to get a committable text"
        echo "   summary; use heaptrack_gui to explore interactively)"
    fi
else
    if ! command -v valgrind >/dev/null 2>&1; then
        echo "neither heaptrack nor valgrind found — install one" >&2
        exit 1
    fi
    # Massif is slower than heaptrack; trim further.
    export CMCP_BENCH_N="${CMCP_BENCH_N_MASSIF:-2000}"
    export CMCP_BENCH_WARMUP="${CMCP_BENCH_WARMUP_MASSIF:-200}"
    echo "Using valgrind --tool=massif (CMCP_BENCH_N=${CMCP_BENCH_N})"
    OUT="${OUTDIR}/heap-massif.out"
    rm -f "${OUT}"
    valgrind --tool=massif --pages-as-heap=no --massif-out-file="${OUT}" \
             "${BIN}" >/dev/null 2>&1
    echo "  raw      → ${OUT}"
    if command -v ms_print >/dev/null 2>&1; then
        ms_print "${OUT}" > "${OUTDIR}/heap-massif.txt"
        echo "  ms_print → ${OUTDIR}/heap-massif.txt"
    fi
fi
