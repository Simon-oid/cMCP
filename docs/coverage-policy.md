# Coverage policy (Tier 7.4)

cMCP's CI publishes line, function, and branch coverage for the library
code under `src/` on every push and pull request. Tier 7.4 turns that
publication into a **delta gate**: a PR that meaningfully shrinks
coverage relative to `main` does not pass review without an explicit
opt-out.

## What the gate does

On every PR:

1. CI runs `make coverage`, which produces `coverage/summary.txt` via
   `gcovr --print-summary`.
2. CI restores the most recent `main`-side `coverage/summary.txt` from
   GitHub Actions cache.
3. `scripts/coverage-delta.sh` parses both, writes a markdown delta
   table to `coverage/delta.md`, posts it as a sticky PR comment, and
   exits **1** if either of the following dropped by more than
   **1.0 percentage point**:
   - `lines` coverage
   - `functions` coverage
4. Branch coverage is reported but **not gated** — rare error paths
   make it too noisy at the per-PR scale.

On every push to `main`: CI runs the same coverage step and writes
the resulting `summary.txt` into a cache entry keyed by `main`. This
is what subsequent PRs compare against. GitHub Actions evicts caches
after 7 days of inactivity; if the cache disappears (e.g. low main
push frequency), the gate becomes informational for one cycle until
the next `main` push refills it.

## Why 1.0 pp

Below that the noise floor of test ordering + new-file additions makes
the gate trip on no-op PRs. Above it, multi-pp degradations would slip
through unnoticed. 1.0 pp lets small natural drift through, blocks any
material shrinkage.

## Why lines + functions but not branches

- **Lines** is the most legible signal — "did this PR delete tests for
  some code path" maps to lines almost directly.
- **Functions** catches the case where a whole entry point becomes
  un-exercised — `lines` only goes down a little, but a public function
  has zero hits. This is the consumer-facing regression we care most
  about.
- **Branches** is dominated by rare error paths the suite can't trigger
  (rare allocation failures, peer-disconnect timing windows). A PR
  that adds a new defensive `if (x == NULL) return CMCP_EINVAL;` would
  drop branch coverage every time, which is correct C-engineering
  hygiene, not a regression.

## Override path

For legitimate cleanups that drop coverage (deleting a tested function
along with its test, removing dead-but-tested code), opt out by adding
`[skip-cov]` anywhere in the **commit-message subject** of the PR's
HEAD commit. The CI step reads `git log -1 --pretty=%s` and sets
`SKIP_COV=1` on a match; the delta script then emits a "Skipped" table
and exits 0.

The opt-out is intended to be rare. Two consecutive `[skip-cov]` PRs
that aren't paired with test cleanups should prompt review of whether
the gate is calibrated correctly (or whether something has rotted).

## Updating the baseline intentionally

You don't. The baseline is whatever the latest green `main` push
produced — there is no `baseline.json` to bump. This is deliberate:
the gate is anchored to actual reality, not to a stored aspiration
that can age out of alignment.

If a refactor needs to drop coverage and the team agrees that's fine,
the PR carries `[skip-cov]` and lands. The next push to `main`
republishes the (lower) baseline, and subsequent PRs are compared
against that.

## Locally reproducing the gate

```sh
make coverage
# Capture baseline once (e.g. from `git stash` of the main copy).
cp coverage/summary.txt /tmp/cov-baseline.txt
# Make your change.
make coverage
scripts/coverage-delta.sh /tmp/cov-baseline.txt coverage/summary.txt
echo "exit=$?"
```

Exit code is the gate verdict; `coverage/delta.md` is what CI would
post as a comment.

## What is NOT covered by this gate

- **Absolute coverage threshold** — Tier 7 explicitly chose
  delta-only. An absolute (e.g. 90 %) bar is a Tier 8 posture if it
  ever happens, per TODO Tier 7 "Out of scope."
- **Per-file coverage** — the gate operates on the whole `src/`
  rollup. A PR that improves `src/json.c` by 3 pp while regressing
  `src/schema.c` by 0.5 pp passes as long as the rollup nets above
  the threshold. Per-file gating is too noisy and too easy to game.
- **Tests/fuzz/examples/conformance coverage** — those directories
  are exercisers, not subjects. `make coverage` already excludes them
  via the lcov + gcovr filter chain.
- **Coverage on documentation, configuration, generated code** —
  obviously not measured.

## Related

- [`Makefile`](../Makefile) — the `coverage` target produces the inputs
- [`scripts/coverage-delta.sh`](../scripts/coverage-delta.sh) — the
  comparator
- [`docs/SEMVER.md`](SEMVER.md) — coverage isn't part of the SemVer
  contract, but a quiet coverage drop often precedes a quiet API
  surface drift
- [`TODO.md`](../TODO.md) §7.4 — design notes + rationale
