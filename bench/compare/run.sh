#!/usr/bin/env bash
# bench/compare/run.sh — drive bench_compare against every SDK we can
# find on the host. cMCP is always run; TS / Python skip gracefully if
# their toolchain isn't installed (no automatic global installs).
#
# Per-impl row schema is identical to bench/results.csv (same
# BENCH_CSV_HEADER from bench_util.h), so the two CSVs can be
# concatenated for downstream tooling.
#
# Env knobs (forwarded to bench_compare):
#   CMCP_BENCH_N         iterations per impl       (default 10000)
#   CMCP_BENCH_WARMUP    warmup calls per impl     (default 1000)

set -u

BENCH_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO_ROOT="$(cd "${BENCH_DIR}/../.." && pwd)"
cd "${REPO_ROOT}"

if [ ! -x examples/echo-server ]; then
    echo "examples/echo-server not built — run \`make\` first" >&2
    exit 1
fi
if [ ! -x bench/compare/bench_compare ]; then
    echo "bench/compare/bench_compare not built — run \`make bench-compare-build\` first" >&2
    exit 1
fi

# Use taskset -c 0 to pin the bench process to CPU 0 — reduces p99 noise.
PIN=()
if command -v taskset >/dev/null 2>&1; then
    PIN=(taskset -c 0)
fi

CSV="${BENCH_DIR}/results.csv"
# bench_compare prints the header on every run; we only keep one.
HEADER_WRITTEN=0
: > "${CSV}"

write_row() {
    # arg 1: impl label    arg 2..: server command
    local label="$1"; shift
    local out
    if ! out="$("${PIN[@]}" ./bench/compare/bench_compare "${label}" "$@" 2>&1)"; then
        echo "  ! ${label} bench failed:"
        echo "${out}" | sed 's/^/    /'
        return 1
    fi
    if [ "${HEADER_WRITTEN}" -eq 0 ]; then
        echo "${out}" | head -1 >> "${CSV}"
        HEADER_WRITTEN=1
    fi
    echo "${out}" | tail -1 >> "${CSV}"
    echo "  ✓ ${label}"
}

echo "Running bench_compare …"

# ---- cMCP ----------------------------------------------------------------
write_row cmcp ./examples/echo-server

# ---- TypeScript SDK ------------------------------------------------------
if ! command -v node >/dev/null 2>&1; then
    echo "  - ts impl skipped: \`node\` not on PATH"
elif [ ! -d "${BENCH_DIR}/node_modules/@modelcontextprotocol" ]; then
    echo "  - ts impl skipped: run \`npm install --prefix bench/compare\` first"
else
    write_row ts node bench/compare/servers/echo.mjs
fi

# ---- Python SDK ----------------------------------------------------------
PY=""
if [ -x "${BENCH_DIR}/.venv/bin/python" ]; then
    PY="${BENCH_DIR}/.venv/bin/python"
fi
if [ -z "${PY}" ]; then
    echo "  - py impl skipped: no venv at bench/compare/.venv (create with"
    echo "    \`python3 -m venv bench/compare/.venv && bench/compare/.venv/bin/pip"
    echo "    install -r bench/compare/requirements.txt\`)"
else
    write_row py "${PY}" bench/compare/servers/echo.py
fi

# ---- Summary -------------------------------------------------------------
echo
echo "=== bench/compare/results.csv ==="
if command -v column >/dev/null 2>&1; then
    column -s, -t < "${CSV}"
else
    cat "${CSV}"
fi
