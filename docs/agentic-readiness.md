# cMCP agentic-readiness plan

How we get cMCP to a quality bar where an LLM agent (butlerbot,
Claude Code, any MCP host) can rely on it without a human in the
loop. This is not a feature roadmap — see [`TODO.md`](../TODO.md)
for that. This is the *quality* track: the work that turns "the
suite is green" into "an autonomous agent can drive this for hours
and survive whatever the wire throws at it."

## What "agent-autonomous" demands

An agent that picks up a tool through cMCP has no second chance to
catch your bug: it sees the result, makes a decision, takes action.
The bar is therefore stricter than "passes tests":

1. **No silent corruption.** A success return must mean the result is
   exactly what the server emitted. Partial / truncated / scrambled
   data masquerading as success is the worst failure mode — the
   agent acts on it.
2. **No silent loss.** A failure return must be returned, not
   discarded. The agent retries / falls back / asks the user only if
   it knows something went wrong.
3. **Informative failure.** Error codes must distinguish recoverable
   (transport blip, retry) from terminal (schema mismatch, give up).
4. **Long-running stability.** An agent can stay up for hours; cMCP
   cannot leak file descriptors, grow RSS, or drift latency.
5. **Robustness against hostile peers.** A misbehaving server must
   not crash the host process or corrupt the host's state.
6. **Spec parity across revisions.** If the agent's catalogue server
   speaks `2026-XX-XX`, cMCP must either speak it too or fail
   cleanly enough that the agent moves on.

## Current state (2026-05-24, post-Tier-5)

All seven axes of this plan are substantively closed. What's now in
place:

- **Conformance** against the MCP TypeScript reference SDK in both
  wire roles (`make conformance`).
- **Memory hygiene.** 22 binaries, ~2716 assertions, all green under
  `make valgrind`, `make test-asan` (`-fsanitize=address,undefined
  -fno-sanitize-recover=all`), and `make test-tsan`
  (`-fsanitize=thread`). Warning-clean under `-Wall -Wextra -Wpedantic`.
- **Spec compliance.** All optional capabilities of MCP `2025-06-18`
  shipped: tools, resources, prompts, sampling, roots, elicitation,
  structured output + resource_link + title, logging, pagination,
  cancel + progress.
- **Real reference servers.** `filesystem-mcp` (bundled, no external
  deps; `fs_write` hardened against symlink-leaf sandbox escape after
  a Tier-5 playbook P0), `crag-mcp` (separate build, real workload),
  `echo-server`, plus `tools/cmcp-tee/` for transparent wire capture.
- **Layered error model.** `cmcp_err_t` distinguishes transport,
  protocol, parse, schema, timeout, cancelled, unsupported, etc.
- **Sanitisers in CI.** `make test-asan` + `make test-tsan` run on
  every push (`fail-fast: false`).
- **Fuzzing.** Four libFuzzer harnesses (`json`, `rpc`, `schema`,
  `http`) with curated seed corpora; `make fuzz-smoke` runs each
  60s, ~32M execs/min total, zero findings. The HTTP request parser
  was extracted to `src/http_parser.c` for harness-level driving.
- **Hostile-peer suite.** `tests/test_hostile_peer.c`: 9 cases / 70
  assertions exercising malicious-side behaviour against both client
  and server. Wired into the CI matrix automatically.
- **Soak harness.** `tests/soak/soak_driver.c` + `run.sh` orchestrator
  with /proc-sampled RSS/FD/Threads + ring-buffered p50/p99 latency
  and awk drift criteria (RSS ≤15% growth, FDs non-growing, Threads
  flat, p99 ≤2× baseline). Opt-in via `make soak` / `make soak-churn`.
- **Real-agent-in-the-loop.** Three playbooks (`echo`, `filesystem`,
  `crag`) driven from Claude Code, ~10 tasks each. First pass
  surfaced one P0 (filesystem symlink-leaf escape — fixed, regression
  test added, fixture archived) and four description tightenings;
  zero protocol-level cMCP bugs.
- **Wire-fixture replay gate.** `conformance/replay/replay.py` +
  per-fixture registry. `make replay` is a new CI lane that fails on
  any frame mismatch against the recorded `dir:"out"` frames, with
  per-fixture masks for legitimately variable fields.
- **Spec-drift watch.** `scripts/check-spec-version.sh` + weekly
  `spec-drift.yml` workflow. Currently firing: upstream cut
  `2025-11-25` since the `2025-06-18` pin landed. Upgrade workflow
  documented in `docs/spec-version-upgrade.md`.

What's *still* on the runtime/budget side, not engineering:

- 24h fuzz baselines per harness (pure CI time).
- 6h soak nightlies (pure CI time).
- HTTP-specific soak variant — one driver against
  `cmcp_transport_http_connect`, same metrics + drift criteria.

## The five quality axes

Each axis below has a name, the gap from current state, concrete
actions, and an acceptance criterion that defines "done."

### Axis 1 — Wire-format correctness across spec revisions

**Current state.** `make conformance` cross-checks against the
pinned TS SDK at one moment in time. The protocol version is
hardcoded in `include/cmcp.h` (`CMCP_PROTOCOL_VERSION
"2025-06-18"`).

**Gap.** Conformance is one-shot — nothing alerts us when the MCP
spec releases a new revision or when our local interpretation
drifts from the reference SDK's. A server upgraded to a future
spec version won't immediately tell us why it stopped working.

**Actions.**

1. **CI conformance gate** *(1 day)*. Add `make conformance` to a
   GitHub Actions workflow (or local pre-commit hook) on every
   push to `main`. Fail the build on any check regression.
2. **Spec-version drift watch** *(half day)*. A `scripts/check-spec-
   version.sh` that fetches the latest version date from
   `modelcontextprotocol.io/specification/` and exits non-zero if it
   differs from `CMCP_PROTOCOL_VERSION`. Run weekly via cron or
   GitHub schedule; open an issue automatically on mismatch.
3. **Spec-regression fixtures** *(2 days)*. Capture wire transcripts
   from every conformance test as `conformance/fixtures/*.jsonl`.
   On TS SDK upgrade, diff old vs new transcripts — surfaces subtle
   spec drift the per-test asserts might miss.

**Acceptance criterion.** Every push to `main` runs the full
conformance battery; a spec-version mismatch alerts within 7 days
of upstream release.

### Axis 2 — Memory + concurrency safety beyond valgrind

**Current state.** valgrind clean across 21 binaries. No sanitizer
builds.

**Gap.** Valgrind catches a useful subset (definite leaks, invalid
reads/writes via dynamic instrumentation) but misses:

- *UB silenziosa* — signed overflow, alignment violations, OOB on
  stack arrays, shifts by ≥ width.
- *Race conditions* — cMCP has at least four thread classes (reader,
  worker pool, HTTP acceptor + holder, watchdog) writing to shared
  state. Valgrind cannot see ordering hazards.
- *Use-of-uninitialised-memory* on data paths valgrind didn't
  exercise.

**Actions.**

1. **`make test-asan`** *(2 hours)*. New target: rebuild everything
   with `-fsanitize=address,undefined -fno-omit-frame-pointer
   -fno-sanitize-recover=all` and run the existing 21 binaries. Fix
   findings; document any false-positive suppression in
   `tests/asan.supp`.
2. **`make test-tsan`** *(2 hours setup + however long the findings
   take)*. `-fsanitize=thread`. Expect findings around: writer
   mutex / notify_mu / inflight_mu interactions, the per-completion
   mutex/cv dance, the HTTP slot mailbox. Each finding either
   (a) reflects a real bug (fix), (b) is benign-by-design (annotate
   with `__tsan_acquire/release` or document why), or (c) is in
   third-party code (suppress).
3. **GCC `-fanalyzer` weekly run** *(half day setup)*. The static
   analyser catches some classes valgrind never will (e.g., NULL
   deref on error paths). Slow — don't gate CI on it, run weekly.
4. **Wire `test-asan` + `test-tsan` into the CI conformance gate
   from Axis 1** *(folded into that work)*.

**Acceptance criterion.** `make test-asan` and `make test-tsan`
both pass with zero unsuppressed findings. CI runs both on every
push.

### Axis 3 — Robustness under hostile / malformed input

**Current state.** Tests cover happy path, schema rejection,
transport EIO from peer crash. No mutation-based fuzzing.

**Gap.** The biggest attack surfaces are *parsers*:

- `src/json.c` — hand-rolled JSON parser. A malformed embedded
  string from a server could trigger UB the tests don't reach.
- `src/rpc.c` — JSON-RPC framing. Batch arrays, weird ID types,
  oversized strings.
- `src/schema.c` — schema validator. Deeply nested schemas, schema
  loops, malformed types arrays.
- `src/transport_http.c` — HTTP request parser. Header injection,
  oversized Content-Length, missing Content-Length, malformed
  chunked.

Also: behaviour against an *adversarial* peer (lies about caps,
sends notifications shaped like responses, replies with id we never
sent, schema-violates its own declared output_schema).

**Actions.**

1. **libFuzzer harnesses** *(3 days)*. Create `fuzz/` with one
   harness per parser:
   - `fuzz_json_parse.c` → `cmcp_json_parse(data, len)`
   - `fuzz_rpc_parse.c` → `cmcp_rpc_parse(data, len, ...)`
   - `fuzz_schema_validate.c` → both schema-parse and validate
   - `fuzz_http_parse_request.c` → the HTTP server's request parser
   Seed corpus from existing test fixtures; build with
   `-fsanitize=address,undefined,fuzzer`. Initial 24-hour run per
   target.
2. **Adversarial-peer test suite** *(2 days)*. New
   `tests/test_hostile_peer.c`: simulate a server that
   - sends `result` and `error` in the same response
   - lies about caps (advertises sampling, never sends one)
   - sends a response to an id that was never registered
   - sends notifications structured as responses
   - sends partial frames (close mid-stream)
   - replies with a 100MB body
   - schema-violates its own output_schema
   Each case: agent-equivalent (cmcp_client_*) returns a specific
   error code, no crash, no corruption of in-flight calls.
3. **Resource exhaustion limits** *(1 day)*. Document and enforce
   ceilings: max JSON depth (already implicit), max message size,
   max in-flight calls per client, max workers. Add tests that
   over-the-limit input returns `-32600` or `CMCP_EINVAL`, not OOM.

**Acceptance criterion.** All four fuzz harnesses run 24h with
zero crashes / hangs / leaks. Hostile-peer suite passes; every
case exits via a documented error path.

### Axis 4 — Long-running stability

**Current state.** None of the existing tests run for more than
a few seconds. We don't know what hours of continuous use does.

**Gap.** Likely fault classes that only show under load:

- File-descriptor leaks (HTTP holder threads, child processes).
- Slow memory growth (per-call completion record not freed on some
  branch).
- Queue saturation (worker pool, HTTP slot mailbox under burst).
- Latency drift (mutex contention scaling with in-flight count).
- Thread-creation leaks if a transport is opened/closed in a loop.

**Actions.**

1. **`tests/soak/run.sh`** *(2 days)*. Driver script that:
   - Spawns `examples/echo-server`
   - Launches N=16 client threads, each calling `add` and `echo`
     in a loop with random small arguments
   - Every 5 minutes samples `/proc/<pid>/status` (VmRSS, FDSize,
     Threads) for both client and server processes
   - Runs for configurable duration (default 6h)
   - Pass criteria: RSS growth ≤ 10% over the run, FDSize stable
     (±2), Threads stable, p99 call latency within 2x of the
     first-hour baseline
2. **HTTP-specific soak** *(1 day)*. Variant: spin up the HTTP
   server, beat it with both POST clients and SSE subscribers,
   verify holder threads clean up on disconnect.
3. **Connect / disconnect churn** *(half day)*. A separate scenario:
   open and close 10 000 stdio clients in sequence, verify no fd
   or thread leak.
4. **Run nightly** *(folded into CI work)*. The 6h soak is
   incompatible with per-push CI; run nightly on a self-hosted
   runner or local cron. Report deltas in a `soak-history.csv` so
   regressions are visible over time.

**Acceptance criterion.** A 6-hour `tests/soak/run.sh` and a
30-minute HTTP soak both pass the criteria above. Connect/disconnect
churn shows zero fd or thread growth.

### Axis 5 — Real agent in the loop

**Current state.** Nobody has watched an LLM actually drive cMCP.

**Gap.** This is the axis no offline test substitutes for. The
specific failure modes:

- Tool `description` fields too terse / too verbose / ambiguous → the
  model picks the wrong tool or formats arguments badly.
- `inputSchema` overly strict → model retries fail in confusing ways.
- Error messages not actionable → the model loops, tries the same
  thing, gives up.
- Resource and prompt naming → discoverability problems no human
  reviewing the code would notice.

**Actions.**

1. **Register the three reference servers with Claude Code**
   *(30 minutes)*. Add `filesystem-mcp` (over a sandbox dir),
   `crag-mcp` (over a real corpus), `echo-server` (smoke test) to
   `~/.claude/mcp_servers.json` (or the equivalent path). Document
   exact config in `docs/agent-validation.md`.
2. **Agent-task playbook** *(2 days to design, ongoing to run)*.
   Define ~10 representative tasks per server:
   - Filesystem: "find all TODO comments in this directory", "rename
     `foo.txt` to `bar.txt`", "show me the largest 5 files", etc.
   - cRAG: 10 search questions over a corpus where you know the
     ground truth answer
   - Echo: smoke only ("call echo with the text 'hello'")
   Run each task in a fresh Claude Code session weekly; record
   whether the agent completed it unassisted, what went wrong,
   what message would have helped.
3. **Bug pipeline** *(continuous)*. Every failed playbook run files
   an issue against the *tool description* or *error message*
   (not the agent). The fix lands in the next sprint; the playbook
   regresses on next run.
4. **butlerbot integration test** *(deferred until butlerbot exists)*.
   Once butlerbot is up, port the playbook to it — that becomes
   the canonical autonomous-agent regression suite.

**Acceptance criterion.** Weekly playbook run achieves ≥ 9/10 task
completion across all three servers, with the failure modes that
remain being explicitly classified as "model limitation, not tool
issue."

## Sequencing

Priority order if I were picking. Numbers are rough effort
estimates including the test-it-back-and-fix cycle.

| # | Axis | Effort | Why this order |
|---|------|--------|----------------|
| 1 | **Sanitizers** (Axis 2.1 + 2.2) | 2 days | Highest signal per hour. Permanent benefit on every test run thereafter. Catches bugs you didn't know existed. |
| 2 | **Agent-in-the-loop** (Axis 5.1 + 5.2) | 2.5 days + continuous | Free signal you can start today. Discovers failure classes no offline test ever will. The earlier you start, the more cycles of feedback you get. |
| 3 | **CI gate** (Axis 1.1) | 1 day | Consolidates 1 and 2 so regressions get caught automatically. Cheap once 1 + 2 exist; expensive if you defer it. |
| 4 | **Fuzzing** (Axis 3.1) | 3 days + 4× 24h runs | Most likely to find real bugs in the parsers — the smallest, best-defined attack surface. |
| 5 | **Hostile-peer suite** (Axis 3.2) | 2 days | Less likely to find bugs than fuzzing, but covers a different category (semantics, not syntax). |
| 6 | **Soak** (Axis 4.1 + 4.2 + 4.3) | 3.5 days + nightly runtime | Slowest to give feedback. Important but defer until the cheaper axes are clear. |
| 7 | **Spec drift watch** (Axis 1.2 + 1.3) | 2.5 days | Future-proofing. Only matters once MCP cuts another revision; not urgent today. |

Total: ~16 person-days of focused work, plus continuous overhead
for agent-in-the-loop runs and nightly soak.

## Explicitly out of scope

- **TLS in cMCP.** Deploy behind nginx/caddy. Hand-rolling TLS in
  C is its own project and adds nothing protocol-shaped.
- **Formal verification** (TLA+, Coq, etc.). Massive cost; benefit
  doesn't compound for an MCP-shaped workload. Sanitizers + fuzzing
  + soak cover the realistic failure modes.
- **Replacing libcurl.** Not a quality issue; libcurl is more
  audited than anything we'd write.
- **Multi-tenant HTTP sessions.** One session per transport is the
  shape; concurrent tenants run multiple transports. Not blocking
  agent use.
- **Performance benchmarks.** Latency / throughput are not on the
  agent-readiness critical path — the bottleneck is the LLM and
  the tool itself, not the protocol layer. Revisit if profiling
  ever points here.

## Tracking

Each axis above gets one entry in [`TODO.md`](../TODO.md) under a
new "Tier 5 — agentic readiness" section. As tasks land, the
acceptance criterion above is the bar for marking them done.
Cross-linked from the CHANGELOG once shipped.
