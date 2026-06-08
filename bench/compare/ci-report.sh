#!/usr/bin/env bash
# bench/compare/ci-report.sh — turn a finished `make bench-compare` run
# into a Markdown comparison report (cMCP vs the reference SDKs), with a
# fresh idle-RSS measurement folded in.
#
# Output is Markdown on stdout. The bench-compare CI workflow appends it
# to $GITHUB_STEP_SUMMARY; locally you can eyeball it after a run:
#
#   make bench-compare && bash bench/compare/ci-report.sh
#
# Numbers are indicative — CI runners are shared/virtualised, so absolute
# throughput varies run to run. The canonical published numbers come from
# a controlled box (see docs/testing-overview.md); this report exists so
# the comparison never goes stale and a relative-standing change is visible.

set -u

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
CSV="${1:-$ROOT/bench/compare/results.csv}"
[ -f "$CSV" ] || { echo "no results.csv at $CSV — run 'make bench-compare' first" >&2; exit 1; }

# --- idle RSS (kB) per impl, best-effort (skips an impl whose toolchain
#     is absent). Hold stdin open with `sleep` so the stdio server blocks
#     on read instead of seeing EOF and exiting. ----------------------------
measure_rss() {  # $@ = server command
    sleep 4 | "$@" >/dev/null 2>&1 &
    local pid=$!
    sleep 1.3
    local rss=""
    [ -r "/proc/$pid/status" ] && rss=$(awk '/^VmRSS:/{print $2}' "/proc/$pid/status" 2>/dev/null)
    kill "$pid" 2>/dev/null; wait 2>/dev/null
    echo "${rss:-}"
}

RSS_CMCP=""; RSS_TS=""; RSS_PY=""
[ -x "$ROOT/examples/echo-server" ] && \
    RSS_CMCP=$(measure_rss "$ROOT/examples/echo-server")
if command -v node >/dev/null 2>&1 && [ -f "$ROOT/bench/compare/servers/echo.mjs" ]; then
    RSS_TS=$(measure_rss node "$ROOT/bench/compare/servers/echo.mjs")
fi
[ -x "$ROOT/bench/compare/.venv/bin/python" ] && \
    RSS_PY=$(measure_rss "$ROOT/bench/compare/.venv/bin/python" "$ROOT/bench/compare/servers/echo.py")

# --- parse results.csv + emit the report -----------------------------------
# CSV columns: bench,iterations,wall_ms,throughput_per_s,min,p50,p95,p99,
#              p999,max,mean,extra   (extra holds "impl=<x> server=<...>")
awk -F, \
    -v rss_cmcp="$RSS_CMCP" -v rss_ts="$RSS_TS" -v rss_py="$RSS_PY" \
    -v date="$(date -u +%Y-%m-%d)" '
NR == 1 { next }
{
    impl = ""
    if (match($0, /impl=[^ ,]+/)) impl = substr($0, RSTART + 5, RLENGTH - 5)
    if (impl == "") next
    tput[impl] = $4 + 0; p50[impl] = $6 + 0; p99[impl] = $8 + 0; mean[impl] = $11 + 0
    seen[impl] = 1
}
END {
    n = split("cmcp ts py", order, " ")
    label["cmcp"] = "cMCP (C11)"
    label["ts"]   = "TypeScript SDK (Node)"
    label["py"]   = "Python SDK"
    rss["cmcp"] = rss_cmcp + 0; rss["ts"] = rss_ts + 0; rss["py"] = rss_py + 0
    rss_have["cmcp"] = (rss_cmcp != ""); rss_have["ts"] = (rss_ts != ""); rss_have["py"] = (rss_py != "")
    base = (seen["cmcp"] ? tput["cmcp"] : 0)
    brss = (rss_have["cmcp"] ? rss["cmcp"] : 0)

    print "## cMCP vs reference SDKs — `make bench-compare` (" date ", CI runner)"
    print ""
    print "_Indicative: CI runners are shared/virtualised. Canonical numbers live in docs/testing-overview.md._"
    print ""

    print "### Throughput — calls/s (higher is better)"
    print ""
    print "| Server | calls/s | vs cMCP |"
    print "|---|---:|---:|"
    for (i = 1; i <= n; i++) { k = order[i]; if (!seen[k]) continue
        r = (base > 0 ? sprintf("%.1f×", base / tput[k]) : "—")
        printf "| %s | %d | %s |\n", label[k], tput[k], r
    }
    print ""

    print "### Per-call latency — µs (lower is better)"
    print ""
    print "| Server | p50 | p99 | mean |"
    print "|---|---:|---:|---:|"
    for (i = 1; i <= n; i++) { k = order[i]; if (!seen[k]) continue
        printf "| %s | %d | %d | %d |\n", label[k], p50[k], p99[k], mean[k]
    }
    print ""

    if (rss_have["cmcp"] || rss_have["ts"] || rss_have["py"]) {
        print "### Idle memory — RSS (lower is better)"
        print ""
        print "| Server | idle RSS | vs cMCP |"
        print "|---|---:|---:|"
        for (i = 1; i <= n; i++) { k = order[i]; if (!rss_have[k]) continue
            r = (brss > 0 ? sprintf("%.0f×", rss[k] / brss) : "—")
            printf "| %s | %.1f MB | %s |\n", label[k], rss[k] / 1024.0, r
        }
        print ""
    }
}' "$CSV"
