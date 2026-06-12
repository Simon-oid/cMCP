# Security policy

## Supported versions

cMCP is pre-1.0. Security fixes land on `main` and ship in the next
release; older 0.x tags are not back-patched. Track the latest tagged
release.

## Reporting a vulnerability

Please report vulnerabilities privately via
[GitHub's private vulnerability reporting](https://github.com/Simon-oid/cMCP/security/advisories/new)
(Security tab → "Report a vulnerability"). Do not open a public issue
for an exploitable bug.

You should get an acknowledgement within a few days. Reports that
include a wire transcript (capture one with `tools/cmcp-tee/`) or a
minimal reproducer are the fastest to act on.

## Threat model & hardening posture

The attack surface, trust boundaries, and the rationale for every
hardening knob are documented in
[`docs/threat-model.md`](docs/threat-model.md) (STRIDE-style pass over
five trust boundaries). Highlights:

- Parser-level caps: JSON depth (`CMCP_JSON_MAX_DEPTH`), container
  size (`CMCP_JSON_MAX_ELEMENTS`), per-frame byte limits on stdio
  (`CMCP_STDIO_MAX_FRAME`) and HTTP bodies, in-flight request caps
  (`CMCP_RPC_MAX_INFLIGHT`).
- HTTP server: loopback bind by default, `Origin` allow-list
  (DNS-rebinding defence), slowloris idle/deadline budgets,
  accept-rate token bucket.
- HTTP client: SSRF egress guard against the *resolved* peer address
  (cloud-metadata, RFC1918, CGNAT, ULA ranges) — defeats DNS
  rebinding; opt out with `CMCP_HTTP_ALLOW_PRIVATE=1` for intranet
  servers you own.
- Credential-shaped fields are redacted from structured log output by
  default (`CMCP_LOG_REDACT`).
- TLS is terminator-only by design — run the HTTP transport behind a
  TLS-terminating reverse proxy; see
  [`docs/deployment-tls.md`](docs/deployment-tls.md).

The parser surface (JSON, JSON-RPC, schema, HTTP) is fuzzed nightly
with libFuzzer; the suite runs under ASan/UBSan, TSan, and valgrind on
every push. One real P0 has been found and fixed to date — the
write-up is at
[`docs/case-study-symlink-escape.md`](docs/case-study-symlink-escape.md).
