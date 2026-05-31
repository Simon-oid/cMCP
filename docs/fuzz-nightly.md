# Nightly fuzz baselines (Tier 7.2)

cMCP ships four libFuzzer harnesses (JSON, JSON-RPC, schema-validator,
HTTP-parser). CI runs each one for 60 seconds on every push — the
[`fuzz-smoke`](../Makefile) target. That catches the obvious crashes
but not the deep-corpus discoveries that come from hour-scale
coverage growth.

Tier 7.2 adds a **nightly cron** that runs each harness for 6 hours.
The post-run corpus uploads as a 90-day artifact; any crash / leak /
timeout artefact fails the job and opens a tracking issue with the
minimised input attached.

## The workflow

```
.github/workflows/fuzz-nightly.yml
```

| Property         | Value                                          |
|------------------|------------------------------------------------|
| Trigger          | cron `0 2 * * *` (02:00 UTC daily) + `workflow_dispatch` |
| Per-harness budget | 6h libFuzzer (`-max_total_time=21600`)       |
| Parallelism      | 4 jobs in matrix (`json`, `rpc`, `schema`, `http`) |
| Job timeout      | 380 min (6h budget + ~20 min build/upload)     |
| Concurrency      | one nightly at a time (queue drops fresh fires)|
| Permissions      | `contents: read`, `issues: write`              |

Why 6h and not 24h: GitHub Actions free-tier ceiling on per-job
wall-clock. Stacking 4 harnesses × 6h matrix-parallel gets the
equivalent of one 24h coverage budget per night, distributed across
the four parsers. A self-hosted bare-metal runner could lift the
per-harness ceiling — Tier 8 follow-up if traffic warrants the lab
cost.

## What lands in the artifact

For each successful harness:

- `fuzz-<h>-corpus` — the **post-run** corpus directory plus
  `run-<h>.log` (libFuzzer's final-stats output: total executions,
  coverage edges, peak RSS, exec/sec). 90-day retention.
- `fuzz-<h>-artefacts` — present only if the run produced any
  crash / leak / timeout artefact (`if-no-files-found: ignore`).
  90-day retention.

The corpus artefact is the input to the weekly corpus-roll step
(below). The artefact artefact is the input to the bug-fix workflow.

## What happens on a crash

The harness binary writes any reproducible crash to its
`-artifact_prefix` directory. After the run completes (or libFuzzer
exits early on the first crash), the workflow:

1. Uploads the `artefacts/` dir as an artifact.
2. Inspects the dir — if empty, the harness exits 0 ("clean").
3. If non-empty, opens a GitHub issue:
   - Title: `fuzz nightly <h>: <first-artefact-filename>`
   - Body: artefact count + minimal reproduction command + a back-link
     to the workflow run.
   - Labels: `fuzz, bug` if the labels exist, falling back to no
     labels if they don't (fresh repos).
4. On a repeat fire (same title already open), the workflow comments
   on the existing issue rather than spamming a duplicate.

The job exits non-zero whenever artefacts exist — that's the gate
signal feeding the repo's "is this branch healthy" status.

## Weekly corpus-roll workflow

The nightly run grows the corpus directory in-place during the run.
What gets uploaded is the **post-run** corpus, which can be 10-100x
the size of the committed seed corpus. We don't want to commit that
entire firehose — most entries are coverage duplicates of existing
seeds.

Once a week, a maintainer:

1. Downloads the four `fuzz-<h>-corpus` artifacts from a recent green
   nightly run.
2. Unpacks each into `./incoming/<h>/`.
3. Runs `tools/fuzz-corpus-roll.sh` — drives libFuzzer's `-merge=1`
   mode, which picks the minimal subset of `{seed ∪ incoming}` whose
   coverage equals the union. Stages the result as
   `fuzz/corpus_<h>.new`.
4. Reviews `git diff --no-index fuzz/corpus_<h> fuzz/corpus_<h>.new`,
   promotes useful additions (new shapes, new edge cases), discards
   noise.
5. Commits the survivors (`fuzz: fold nightly corpus week of <date>`).

This keeps the seed corpus small (fast `fuzz-smoke` per-PR runs) and
curated (each entry represents a meaningful shape) while still
ratcheting coverage forward against the nightly findings.

```sh
# Quick run, all four harnesses, dry-run preview:
make fuzz-build
DRY_RUN=1 tools/fuzz-corpus-roll.sh

# Real roll (writes corpus_*.new):
tools/fuzz-corpus-roll.sh
```

## What is NOT covered

- **Coverage-percentage trend over weeks.** The artifact log shows
  per-run coverage edges, but there's no plotted history. If we want
  this, the data is in the artifact log lines (`cov:` field); a
  small script + a static page would do. Tier 8 candidate.
- **In-band fuzzing of the HTTP transport's socket layer.** The
  fuzz harness drives the parser entry point (`cmcp_http_parse_head`)
  directly so the harness doesn't have to manage sockets — the
  parser is the actual attack surface anyway, since the socket layer
  is libcurl on the client side and a tiny `accept()`/`recv()` loop
  on the server side.
- **Cross-harness corpus sharing.** Each harness owns its own corpus
  + artefact dir. Mixing them would mostly produce noise (a valid
  HTTP request isn't valid JSON).
- **Auto-bisect on first crash.** The opened issue includes the
  workflow's `run_id`; correlating with `main`'s commit history is a
  manual step. A genuinely valuable nightly-bisect runner is a Tier 8
  follow-up.

## Triage when a nightly fires

1. The issue body links the workflow run. Download the
   `fuzz-<h>-artefacts` artifact.
2. Reproduce locally:
   ```sh
   make fuzz/fuzz_<h>
   ./fuzz/fuzz_<h> <attached-artefact-file>
   ```
3. The harness's ASan/UBSan output (it's compiled with
   `-fsanitize=address,undefined,fuzzer`) points to the offending
   line. Most crashes here are NULL deref / OOB read / use-after-free
   in a parser path; fix lives in `src/`.
4. After landing the fix, add the (now-minimised) artefact to
   `fuzz/corpus_<h>/seed_<descriptive-name>` so the smoke run
   regression-guards against it.
5. Close the GitHub issue with a link to the fixing PR.

## Related

- [`Makefile`](../Makefile) — the `fuzz-build` / `fuzz-smoke` targets
- [`fuzz/`](../fuzz) — the four harnesses + seed corpora
- [`tools/fuzz-corpus-roll.sh`](../tools/fuzz-corpus-roll.sh) — the
  weekly fold helper
- [`docs/agentic-readiness.md`](agentic-readiness.md) — Tier 5
  context that introduced the harnesses
- [`TODO.md`](../TODO.md) §7.2 — design notes + rationale
