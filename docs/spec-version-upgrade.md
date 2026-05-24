# Upgrading `CMCP_PROTOCOL_VERSION`

This document is the checklist to run when the spec-drift watch
(`scripts/check-spec-version.sh`, exercised weekly in CI) reports that
a newer dated revision exists upstream. It does **not** trigger
automatically — cMCP pins one protocol version per release on purpose,
and a bump touches wire-relevant code paths that need a human reading
the changelog.

## When this fires

CI's `spec-drift` job runs `./scripts/check-spec-version.sh` and exits
non-zero when:

- `CMCP_PROTOCOL_VERSION` in `include/cmcp.h` doesn't match the
  newest dated directory under
  [`modelcontextprotocol/modelcontextprotocol@main:schema/`](https://github.com/modelcontextprotocol/modelcontextprotocol/tree/main/schema)
  (ignoring `draft/`).

That's the *only* signal. A failing job here is not blocking: the wire
remains spec-compliant against whatever revision is currently pinned,
and `cmcp_server_run` / `cmcp_client_connect_stdio` already accept
cross-version handshakes (per the spec compliance note in `CLAUDE.md`).

## Decide whether to upgrade

Read both the new spec and the diff:

- `https://modelcontextprotocol.io/specification/<NEW>` — narrative
  changelog, breaking-change call-outs.
- `https://github.com/modelcontextprotocol/modelcontextprotocol/tree/main/schema/<NEW>`
  — the JSON schema files; diff them against
  `schema/<CURRENT>/` to see the wire surface that moved.
- The TS SDK's `LATEST_PROTOCOL_VERSION` constant in
  `@modelcontextprotocol/sdk` — usually bumps within a release of the
  spec landing, useful as a tracking signal.

Three outcomes are reasonable:

1. **Defer.** Cross-version handshakes work; pin stays. Re-evaluate
   next time the drift watch fires.
2. **Upgrade.** Follow the checklist below.
3. **Partial upgrade.** Cherry-pick specific schema additions
   (new methods, capability flags) without rolling the pin. Rare.
   Doing this means the server returns the old pin on handshake
   while advertising newer surface; document why.

## Upgrade checklist

When the decision is to bump:

- [ ] **Bump the pin** in `include/cmcp.h`:
      `#define CMCP_PROTOCOL_VERSION "<NEW>"`.
- [ ] **Search for hardcoded version strings** that aren't going
      through the macro:
      `grep -rn '<CURRENT>' --include='*.[ch]' --include='*.md'`.
      Updates expected in: `README.md`, `docs/*.md`, any test that
      asserts the literal in a response payload.
- [ ] **`make clean && make test`** — handshake tests, lifecycle
      tests, and `test_stdio_roundtrip` all assert the pinned version
      somewhere. Failures here are the schema/lifecycle changes you
      need to implement.
- [ ] **Re-read the lifecycle / capability sections of the new
      spec.** Negotiate new capabilities at `initialize`. New
      MCP-level methods that the spec mandates need handler stubs
      (or explicit `-32601` if intentionally unsupported in this
      release).
- [ ] **Re-record every fixture under `conformance/fixtures/`.**
      Any frame that includes `protocolVersion` will need to match
      the new pin. Procedure per fixture: re-capture the same
      interaction via `tools/cmcp-tee` against the rebuilt server,
      then `make replay` to confirm the new fixtures pass.
- [ ] **`make replay`** — the wire-fixture gate is what catches a
      forgotten fixture. After re-capture, every present fixture must
      PASS.
- [ ] **`make test-asan && make test-tsan`** — sanitiser sweep on
      the changed handshake/dispatch paths.
- [ ] **`make fuzz-smoke`** — 60s per harness against the existing
      corpus. A schema change can crash an old corpus entry; expect
      to extend the seed corpora for any new keyword the spec
      introduces.
- [ ] **Conformance harness** (`make conformance`, opt-in) — run
      cMCP against the TS reference SDK in both wire roles. The TS
      SDK is the most reliable upstream witness for "did I implement
      this right?".
- [ ] **README + roadmap** — update the user-facing spec-version line
      in `README.md` and tick the entry in `TODO.md` (or open a
      new tracking entry if the upgrade unblocks something).

The pin bump should be its own commit (`phase X.Y: bump
CMCP_PROTOCOL_VERSION to <NEW>` plus the directly-required wire
changes); follow-up commits per substantive schema change.

## What this watch deliberately does *not* do

- **Auto-update the pin.** Wire-format decisions need human review.
- **Block development.** The job runs on a schedule, not on every
  push, and lives in its own CI lane.
- **File issues.** First version is exit-1 + clear log. If we end up
  ignoring the failure regularly, `gh issue create` from the job is
  a one-line follow-up.
