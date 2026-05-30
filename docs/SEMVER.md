# cMCP versioning policy

cMCP follows [Semantic Versioning 2.0.0](https://semver.org/), with the
clarifications and conventions below. This document is the contract:
what a downstream consumer can rely on from one `cmcp X.Y.Z` to the
next.

## TL;DR

| Bump  | When                                                              |
|-------|-------------------------------------------------------------------|
| MAJOR | Any change to the **public surface** that breaks the ABI or API. |
| MINOR | New functions, new fields in tagged-extensible structs, new env  |
|       | variables. No removal, no signature change, no struct-layout     |
|       | change.                                                          |
| PATCH | Bug fixes, performance improvements, doc/test changes. No        |
|       | observable change to the public surface.                         |

`CMCP_PROTOCOL_VERSION` (the MCP wire protocol revision cMCP speaks) is
**independent** of the cMCP package version (`CMCP_VERSION`) — see
[Protocol version vs package version](#protocol-version-vs-package-version)
below.

---

## The public surface

The **public surface** is exactly the set of identifiers declared in:

```
include/cmcp.h
include/cmcp_json.h
include/cmcp_types.h
include/cmcp_transport.h
include/cmcp_http_parser.h
include/cmcp_schema.h
include/cmcp_server.h
include/cmcp_client.h
include/cmcp_session.h
```

Plus the documented set of environment variables (see
[README.md](../README.md#environment-variables) and the table in
[CLAUDE.md](../CLAUDE.md) for the in-tree mirror).

Everything in `src/*.h`, `src/*.c`, `tools/`, `tests/`, `fuzz/`,
`conformance/`, `examples/`, and `bench/` is **internal**. It may
change in any release, including patch releases. Downstream consumers
must not include it directly or link against it as a stable surface.
The reference binaries `cmcp-inspect`, `filesystem-mcp`, and `cmcp-tee`
are installable but their CLI flags are not covered by this policy —
their job is to be useful, not to provide a stable scriptable surface.

### What a MAJOR bump means

A MAJOR bump (e.g. `1.x.y` → `2.0.0`, or eventually `0.x.y` → `1.0.0`)
is required if **any** of the following changes occur in the public
surface:

- A function is **removed** or **renamed**.
- A function's **signature** changes (parameter types, parameter
  count, return type, calling convention).
- A function's **documented behavior** changes in a way that a
  conforming caller could observe (return semantics, side-effect
  ordering, error-code mapping, output format).
- A **struct's layout** changes in a way that breaks the ABI — adding
  fields in the middle, reordering fields, changing field types. Only
  structs that callers allocate or copy by value are layout-sensitive.
  Opaque structs (declared as `typedef struct foo foo_t;` with no
  members exposed) can grow freely.
- An **enum** value's numeric encoding changes, or values are
  re-numbered.
- A macro that callers may have stringified or branched on changes
  meaning.
- An **environment variable's** documented semantics change (e.g.
  default changes from "off" to "on", or an old name is removed).

### What a MINOR bump covers

A MINOR bump (e.g. `1.4.0` → `1.5.0`) covers additions only:

- New public functions.
- New error codes (added at the end of `cmcp_err_t`, never inserted in
  the middle — that would change the numeric encoding of existing
  values and bump MAJOR).
- New fields appended at the end of tagged-extensible structs (the
  capability structs `cmcp_server_capabilities_t` and
  `cmcp_client_capabilities_t` are designed for this; growing them
  remains MINOR).
- New environment variables, with sensible defaults that preserve
  prior behavior.
- New build targets / packaging metadata.

### What a PATCH bump covers

Bug fixes, performance work, internal refactoring, documentation,
tests. Nothing observable from the public surface changes. PATCH
bumps are always ABI-compatible and source-compatible with the same
MAJOR.MINOR.

---

## Pre-1.0 caveat

cMCP is in `0.x.y`. Per SemVer's own
[clause 4](https://semver.org/#spec-item-4): *"Anything MAY change at
any time. The public API SHOULD NOT be considered stable."* In
practice we still **follow the policy above** during 0.x — additions
go in MINOR, breaks force MINOR (since MAJOR is pinned at 0). Treat
every `0.x` → `0.x+1` minor bump as potentially ABI-incompatible; treat
`0.x.y` → `0.x.y+1` patch as ABI-stable.

When cMCP cuts `1.0.0`, the policy above becomes binding — no breaking
change without a MAJOR bump.

### CMake compatibility encoding

The installed `cmcpConfigVersion.cmake` exposes `SameMinorVersion`
compatibility while we are 0.x — `find_package(cmcp 0.5)` matches any
installed `0.5.z` but rejects `0.6.z` or `0.4.z`. Once we hit `1.0.0`,
this becomes `SameMajorVersion`.

### pkg-config compatibility encoding

pkg-config has no built-in compatibility relation. The installed
`cmcp-core.pc` exposes `Version: $VERSION` and downstream `Requires:`
should pin a range (e.g. `Requires: cmcp-core >= 0.5.0, cmcp-core <
0.6.0`) — the equivalent of the CMake SameMinorVersion rule, expressed
by hand at the consumer site.

---

## Protocol version vs package version {#protocol-version-vs-package-version}

cMCP carries two version numbers, deliberately separate:

| Macro                    | What it identifies                              |
|--------------------------|-------------------------------------------------|
| `CMCP_VERSION`           | The cMCP package release (this SemVer policy). |
| `CMCP_PROTOCOL_VERSION`  | The MCP wire protocol revision (e.g. `2025-11-25`).  |

A package release can bump `CMCP_PROTOCOL_VERSION` without changing the
public C surface — that goes in a MINOR (new behavior added) or even
PATCH if the protocol bump is a tightening of an existing behavior. A
package release can also change the public C surface without touching
`CMCP_PROTOCOL_VERSION` — that goes by the rules above.

The two version numbers move on independent timelines. The
[spec-version-upgrade workflow](spec-version-upgrade.md) documents how
`CMCP_PROTOCOL_VERSION` is bumped; this file is silent on it.

---

## Release tagging

Every release lands as an annotated git tag named `v$VERSION` (e.g.
`v0.4.1`). The tag message is the corresponding CHANGELOG section so
`git show v0.4.1` reproduces what shipped without leaving the repo.

To retro-tag the pre-policy releases, the maintainer runs the
commands at the bottom of this file once.

---

## What is NOT covered

These are not part of the SemVer contract — they may change in any
release:

- **Internal headers** under `src/*.h`. Tagged-internal even when
  they look API-shaped.
- **Wire format of log messages.** Stderr is a debug surface, not a
  parseable contract. JSON-format logs (`CMCP_LOG_JSON=1`) keep their
  key set stable within a MAJOR but add new keys freely.
- **Reference binary CLI flags.** `cmcp-inspect --help` is honest;
  `cmcp-inspect` itself is not a script-stable surface.
- **Conformance fixtures, soak driver, fuzz corpus.** Internal test
  infrastructure.
- **The build system itself.** New `make` targets are additive; the
  set of `CFLAGS`/`LDFLAGS` we accept may grow.

---

## Retro-tagging (one-time, maintainer)

Pre-policy releases `0.1.0` through `0.4.1` shipped without git tags.
To attach annotated tags pointing at the closing commit for each tier,
the maintainer runs once:

```bash
# Tier 1 — protocol & stdio (closes at phase 1.9)
git tag -a v0.1.0 -m "Tier 1: protocol layer + stdio transport" 8889388
# Tier 2 — HTTP transport (closes at phase 2.6, "closes tier 2")
git tag -a v0.2.0 -m "Tier 2: HTTP transport (Streamable HTTP, client+server)" 425fd4d
# Tier 3 — server registries + reference servers (closes at phase 3.2+3.4+3.5)
git tag -a v0.3.0 -m "Tier 3: server-side registries + filesystem-mcp + crag-mcp" 722cf07
# Tier 4 — agentic primitives (0.4.0 cut commit: README+CHANGELOG roll-up)
git tag -a v0.4.0 -m "Tier 4: structured output, elicitation, sampling, progress, cancel" 4087110
# Tier 5 — agentic readiness (post-Tier-5 docs roll-up, before Tier 6 work began)
git tag -a v0.4.1 -m "Tier 5: agentic readiness (fuzz, replay, soak, hostile-peer, sanitizer matrix)" a53fd8b
git push origin v0.1.0 v0.2.0 v0.3.0 v0.4.0 v0.4.1
```

Each SHA above was verified against the matching `CHANGELOG.md` release
date: the commit's author-date matches the date stamped on the
release section, and no later commit changes anything user-visible
between the tag point and the next release. If the history is ever
rewritten such that one of these SHAs no longer exists, regenerate
the list from `git log --oneline --reverse` rather than re-running a
grep-based search.

`v0.5.0` is the first **post-policy** release: it gets its tag at
release time (not retro), pointing at the "cut v0.5.0" commit:

```bash
git tag -a v0.5.0 -m "Tier 6: state-of-the-art library polish" HEAD
git push --tags
```

`v0.6.0` is the first **host-driven cut** — sized by the dogfood
findings F1–F4 against a real consumer (`tools/crag-mcp/` driven by
`tools/dogfood-crag-host/`) rather than by a spec axis. The tag is
applied at release time against the "cut v0.6.0" commit (the
paperwork roll-up that lands `CHANGELOG`, `README`, `cmcp.h`
`CMCP_VERSION` bump, and this file):

```bash
git tag -a v0.6.0 -m "v0.6.0: first host-driven cut (A1/A2/A3 + dogfood replay)" HEAD
git push --tags
```

Going forward, every CHANGELOG release section ships with its tag at
release time — no retro work needed.
