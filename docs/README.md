# cMCP documentation

Design notes, operational guides, and policy for the cMCP library. Start with
[`architecture.md`](architecture.md); the rest is grouped by topic below.

## Architecture & design

- [`architecture.md`](architecture.md) — layering, threading model, memory-ownership rules.
- [`agentic-readiness.md`](agentic-readiness.md) — the Tier-5 quality plan (real-agent-in-loop readiness).

## JSON Schema validator

- [`schema-conformance.md`](schema-conformance.md) — the supported validator surface, benchmarked against Ajv.
- [`schema-audit.md`](schema-audit.md) — keyword-by-keyword gap analysis vs the TS SDK's Ajv reference.

## Security & deployment

- [`threat-model.md`](threat-model.md) — attack surface, trust boundaries, hardening knobs.
- [`deployment-tls.md`](deployment-tls.md) — running the HTTP transport behind TLS.

## Performance

- [`perf-baselines.md`](perf-baselines.md) — captured throughput/latency baselines.
- [`perf-regression-gate.md`](perf-regression-gate.md) — how `bench/` gates regressions (Tier 7.1).

## Quality gates (nightly / CI)

- [`fuzz-nightly.md`](fuzz-nightly.md) — nightly libFuzzer baselines + weekly corpus fold (Tier 7.2).
- [`soak-nightly.md`](soak-nightly.md) — long-running stability soak (Tier 7.3).
- [`coverage-policy.md`](coverage-policy.md) — coverage targets and the delta gate (Tier 7.4).

## Release & versioning

- [`SEMVER.md`](SEMVER.md) — versioning policy.
- [`spec-version-upgrade.md`](spec-version-upgrade.md) — checklist for bumping `CMCP_PROTOCOL_VERSION`.
- [`v0.6.0-acceptance.md`](v0.6.0-acceptance.md) — acceptance criteria for the v0.6.0 release.

## Dogfooding

- [`dogfood-cragmcp.md`](dogfood-cragmcp.md) — findings from driving `crag-mcp` through a real agent loop.
