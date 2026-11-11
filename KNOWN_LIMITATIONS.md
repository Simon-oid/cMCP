# Known limitations

Where cMCP stops, in one place. Most are deliberate scope decisions, with the
reasoning in [`CHANGELOG.md`](CHANGELOG.md) and the documents linked below.

## Scope (deliberate)

- **Linux only** (x86-64 and aarch64). The transports use `fork`, pipes and
  pthreads, and Linux is what CI builds and tests. macOS and Windows are not
  supported.
- **No OAuth 2.1** or any other application-layer authentication. The
  library trusts the request it receives: authentication is the job of the
  proxy in front of it ([`docs/threat-model.md`](docs/threat-model.md)).
- **No TLS in the library, by design.** Run the HTTP transport behind nginx,
  caddy or Envoy, which also do HTTP/2 and rate limiting
  ([`docs/deployment-tls.md`](docs/deployment-tls.md)). This is a permanent
  decision, not a deferral.
- **No HTTP/2 or HTTP/3**, for the same reason.
- **Not implemented:** `completion/complete` (argument autocomplete) and
  `resources/templates/list` (needs an RFC 6570 URI-template engine). No
  consumer needs them yet.

## JSON Schema validation

Draft 2020-12 semantics with draft-07 compatibility, with these edges
([`docs/schema-conformance.md`](docs/schema-conformance.md)):

- **Refused at registration:** `unevaluatedProperties`, `unevaluatedItems`,
  `$dynamicRef`, `$recursiveRef`, a `$ref` to another document or URI (cMCP
  never fetches during validation), and any other `$schema` dialect. A tool
  using them is rejected with a reason, rather than registered and silently
  under-validated.
- **`format` is partly an annotation.** `date-time`, `email`, `uri` and
  `uuid` get a lexical check. Any other format (`ipv4`, `hostname`,
  `regex`…) is accepted without being enforced, as Ajv does by default.
- **`1.0` is not an integer.** The parser keeps the lexical form, so a
  schema that says `integer` rejects `1.0`. Use `"number"` to accept both.
- **`pattern` is POSIX ERE**, not ECMAScript regex. Patterns that rely on
  ECMAScript-only features behave differently.

## Security: what is only partly defended

[`docs/threat-model.md`](docs/threat-model.md) lists every threat with its
status. The ones marked partial:

- **A peer holding an SSE stream open.** The replay buffer is bounded per
  session, and the stream closes with the session.
- **Regex cost from an untrusted `inputSchema`.** POSIX ERE has no
  catastrophic-backtracking path like ECMAScript's, and compiled patterns are
  cached, but pathological POSIX patterns are still possible.
- **A server flooding a client with notifications.** They are handled one
  at a time by the reader thread, and nothing buffers without bound.

Out of the library's reach altogether:

- **Resource use inside a handler** (disk, network egress, child processes):
  the tool author decides what is safe.
- **Cryptography:** the only use is reading the session id from
  `/dev/urandom`. Nothing is signed or encrypted.

## Performance numbers

- **One machine, one workload.** The headline figures (~43,000
  `tools/call`/s over stdio, p50 22 µs; 3.9× lower p50 latency than the
  official TypeScript SDK, ~43× lower than the Python SDK) are medians of 5
  runs on a Ryzen 7 5800X with a trivial tool, measured 2026-11-08. They
  measure protocol overhead, not a real tool's cost. Methodology and the
  command to reproduce: [`docs/perf-baselines.md`](docs/perf-baselines.md).

## Reference servers

- **`crag-mcp` retrieves; it does not judge relevance.** Its cosine cutoff
  drops far-off-topic chunks only. A same-domain question whose fact is
  absent still returns plausible chunks, and the agent must check them, as
  the tool's description says.
- **`crag-mcp` returns text, not structured content.** Clients parse
  `[cos … bm25 … fusion …] path` lines. cMCP supports structured tool output,
  but this server does not use it yet.
- **`filesystem-mcp` is bounded to one root,** with symlink leaves refused
  on write ([`docs/case-study-symlink-escape.md`](docs/case-study-symlink-escape.md)).
  It has no delete, rename or permission tools.
