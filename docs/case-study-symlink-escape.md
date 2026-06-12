# Case study: an LLM agent found a sandbox escape in filesystem-mcp

A write-up of the one P0 found during cMCP's development, because *how*
it was found matters more than the bug itself: a human-written test
suite (2,800+ assertions at the time) and four fuzzers had all missed
it, and a playbook-driven LLM agent surfaced it within one session.
Fixed in commit `ac3b086` (Tier 5.2, 2026-05-24).

## Context: real-agent-in-the-loop testing

Tier 5 of the quality plan ([`agentic-readiness.md`](agentic-readiness.md))
added an axis no amount of unit testing covers: register the reference
servers with a real LLM host (Claude Code) and have the agent drive
them through scripted task sets — the playbooks under
[`conformance/playbooks/`](../conformance/playbooks/). Every session
runs through `cmcp-tee`, a transparent stdio proxy that tees each wire
frame in both directions to a JSONL log, so anything interesting the
agent does is captured as a replayable transcript rather than lost in
scrollback.

The [`filesystem-mcp` playbook](../conformance/playbooks/filesystem-mcp.md)
includes a sandbox-escape task (T7): set up symlinks pointing outside
the sandbox root and ask the agent to read and write through them. The
server's whole contract is that no operation touches anything outside
its `--root`.

## The find

```
Setup:   ln -sf /tmp/totally-pwned sandbox/symwrite    # target does not exist
Request: fs_write({path: "symwrite", content: "x"})
Result:  "wrote 1 bytes to symwrite" — and /tmp/totally-pwned now exists.
```

A symlink *leaf* whose target is outside the sandbox **and does not
exist yet** let `fs_write` create and write an arbitrary out-of-sandbox
path. Classic severity-P0 for a tool whose value proposition is the
sandbox.

## Root cause

`resolve_path(must_exist=0)` canonicalises with `realpath()`. For a
dangling symlink, `realpath()` fails with `ENOENT` — so the code took
its fallback branch for not-yet-existing files: canonicalise the
*parent* directory, then re-attach the literal basename. Both pieces
are inside the sandbox, so the containment check passes. The check
never sees that the basename is itself a symlink pointing out.

Then `fopen("wb")` happily *follows* the symlink at write time. Two
layers each did something locally reasonable; the hole is in the seam
between them.

## The fix

In `tools/filesystem-mcp/main.c::fs_write_handler`:

- `lstat()` the resolved leaf and refuse if `S_ISLNK`, with a clear
  message naming the path as a symlink.
- Replace `fopen("wb")` with
  `open(O_WRONLY | O_CREAT | O_TRUNC | O_NOFOLLOW)` + `fdopen()`.
  `O_NOFOLLOW` is the load-bearing half: it is enforced atomically by
  the kernel at open time, so a symlink racing into place between the
  `lstat` and the `open` (TOCTOU) still loses.
- Map `ELOOP` back to a "path is a symlink — refusing to follow" error
  instead of a generic failure.

Reads through *in-sandbox* symlinks remain transparently followed —
that is the correct non-bug behaviour, and the playbook pins it too so
an overzealous future fix can't regress usability in the name of
security.

## Locking it in

A fix without a regression net is a fix waiting to be undone. Three
artifacts pin this one:

1. **Regression test** —
   `tests/test_fs_server.c::test_write_symlink_leaf_escape_rejected`
   reproduces the exact scenario and asserts the out-of-sandbox target
   was never created.
2. **Wire fixture** — the pre-fix transcript captured by `cmcp-tee`
   lives at
   `conformance/fixtures/filesystem-mcp/fs_write_symlink_leaf_escape.jsonl`
   and runs in the replay gate (`make replay`) on every push.
3. **Playbook matrix** — T7 grew into a 9-variant escape matrix
   covering read- and write-side paths: absolute paths, `..`
   traversal, symlink directories, symlink leaves (existing and
   dangling targets), and the in-sandbox-symlink non-bug case.

## Why an agent found what the tests didn't

The unit tests checked the containment *function*; the fuzzers checked
the *parsers*. Neither composes operations the way a motivated user
does: create a dangling symlink with one tool call, then write through
it with another, across two requests. An LLM agent given the goal
"try to escape the sandbox" explores exactly that compositional space
— it behaves like a hostile-but-plausible user, not like a test
fixture. And because every session is teed to JSONL, the interesting
behaviour was already a regression fixture the moment it happened:
capture, don't discard.

That loop — playbook → agent → tee → fixture → replay gate — is the
reusable part. The symlink bug is just the first thing it caught.
