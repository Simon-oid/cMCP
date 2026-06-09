# bench/compare/

Comparison-bench: cMCP client vs cMCP/TypeScript/Python MCP servers.
Opt-in via `make bench-compare`; not part of the default test gate.

The cMCP **client** is held constant. Only the **server** changes per
row. This isolates server-side per-call cost from any client-side
quirks and answers the question the design plan posed for axis 6.6.2:
*"is cMCP in the right ballpark vs the TS/Python reference SDKs?"*

## Layout

| File | Purpose |
|---|---|
| `bench_compare.c` | C driver: spawns a server binary via `cmcp_client_connect_stdio`, runs warmup + measured `tools/call echo`, emits one CSV row. |
| `servers/echo.mjs` | Minimal TS-SDK stdio server, same tool surface as `examples/echo-server`. |
| `servers/echo.py` | Minimal Python-SDK (FastMCP) stdio server, same tool surface. |
| `package.json` | Pinned `@modelcontextprotocol/sdk` + `zod` for the TS server. |
| `requirements.txt` | Pinned `mcp` for the Python server. |
| `run.sh` | Orchestrator: probes for `node` / Python venv, runs each available impl, concatenates rows into `results.csv`, prints aligned summary. |
| `ci-report.sh` | Turns a finished run into a Markdown comparison report (throughput / latency / idle-RSS, with ratios vs cMCP). Run locally after `make bench-compare`, or let the `bench-compare` GitHub Action append it to the run summary. |

The [`bench-compare`](../../.github/workflows/bench-compare.yml) workflow
(weekly + manual `workflow_dispatch`) keeps this comparison from going
stale: it runs the bench on a CI runner and publishes `ci-report.sh`'s
table to the run summary + a CSV artifact. It deliberately does **not**
commit anything — runner numbers are noisy, so the snapshot in
[`docs/cmcp-engineering-report.pdf`](../../docs/cmcp-engineering-report.pdf)
(section 3) stays the canonical reference.

`results.csv` is produced by `make bench-compare` and is gitignored
(numbers vary per machine, per kernel, per build flags). The
locally-installed `node_modules/` and `.venv/` are gitignored too.

## Quick reference

```sh
# One-time toolchain setup (opt-in, kept local to this dir):
npm   install --prefix bench/compare         # TS SDK
python3 -m venv bench/compare/.venv          # Python venv
bench/compare/.venv/bin/pip install -r bench/compare/requirements.txt

make bench-compare-build       # compile the C driver
make bench-compare             # run all available impls + write results.csv
```

Each step is independent — if `node` isn't installed, `run.sh` skips
the TS row with a one-line explanation. Same for the Python venv. The
cMCP row always runs (it's part of `make`).

## Methodology

- **In-process client, subprocess server.** Each row spawns a fresh
  child process running the named server binary, runs the
  `initialize` handshake, then loops `tools/call echo {text:"ping"}`
  for the measured window. Subprocess startup + handshake cost is
  paid once and absorbed by the warmup; the measured window reports
  steady-state per-call cost only.
- **Apples-to-apples surface.** All three servers register the same
  two tools (`echo` taking `{text}`, `add` taking `{a,b}`) with the
  same input schemas. The bench only calls `echo` — `add` exists so
  the surface mirrors `examples/echo-server` end-to-end.
- **Same wire.** All three are driven over the same JSON-RPC over
  stdio transport. cMCP and the TS SDK both speak `2025-11-25`; the
  Python SDK's pinned version varies (the wire handshake negotiates).
- **CPU pin.** `taskset -c 0` if available; same as the cMCP-only
  bench.
- **Iteration count.** Default 10000 (the TS/Py SDKs are slow enough
  per call that 10k samples is enough for a stable p99). Override
  via `CMCP_BENCH_N`.

See [`docs/perf-baselines.md`](../../docs/perf-baselines.md) for the
observed numbers and what the ratios mean.

## Out of scope

- **HTTP-transport comparison.** Stdio-only comparison per the
  axis-6.6.2 design decision: the HTTP transport adds confounders
  (libcurl handshake, accept-rate gate, TCP setup) that we don't
  want fighting the SDK-vs-SDK signal.
- **`tools/list` / `prompts/get` / `resources/read` comparison.**
  One workload (`tools/call`) is enough for the order-of-magnitude
  story; multi-primitive comparison would be a separate axis.
- **Cold-start comparison.** Subprocess startup time is not what
  agent hosts care about — they keep one server connection open
  for a session.
