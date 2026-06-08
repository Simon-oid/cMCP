# cMCP — Testing Overview

A visual map of cMCP's test and quality posture, framed against the
three-layer MCP test pyramid (guide Ch.3.1), the load/resilience metrics
(Ch.4), and the MCP security controls (Ch.6).

All numbers below are from the hermetic `make test` run on 2026-06-08:
**3 885 assertions across 30 test binaries**, green under valgrind +
ASan/UBSan + TSan.

---

## 1. The test pyramid (guide Ch.3.1)

cMCP's suites mapped onto the guide's Unit → Integration → Evaluation
hierarchy, with a security/hardening axis that cuts across all layers.

```mermaid
graph TD
    L3["<b>L3 — Evaluation</b> (opt-in, real agent / cross-impl)<br/>conformance vs TS reference SDK (both directions) · host-probe ·<br/>dogfood-crag-host · replay wire-fixtures · agent playbooks"]
    L2["<b>L2 — Integration</b> · 21 suites · 2 170 assertions<br/>handshake · transports (stdio/HTTP) · tools/resources/prompts ·<br/>notifications · sampling · roots · elicitation · worker pool · cancel"]
    L1["<b>L1 — Unit</b> · 3 suites · 1 520 assertions<br/>json · rpc framing · schema validator"]
    SEC["<b>Security / Hardening</b> (cross-cutting) · 6 suites · 195 assertions<br/>hostile-peer · SSRF guard · frame caps · rlimit · knob interactions"]

    L3 --> L2 --> L1
    SEC -.spans all layers.-> L2

    classDef l3 fill:#dbeafe,stroke:#1e40af,color:#1e3a8a;
    classDef l2 fill:#dcfce7,stroke:#166534,color:#14532d;
    classDef l1 fill:#fef9c3,stroke:#854d0e,color:#713f12;
    classDef sec fill:#fee2e2,stroke:#991b1b,color:#7f1d1d;
    class L3 l3; class L2 l2; class L1 l1; class SEC sec;
```

### Assertion distribution (`make test`)

```mermaid
pie showData
    title Assertions per layer — 3 885 total
    "L2 Integration" : 2170
    "L1 Unit" : 1520
    "Security / Hardening" : 195
```

| Layer | Suites | Assertions | Relative |
|---|---:|---:|:--|
| **L1 — Unit** | 3 | 1 520 | `█████████████████████` |
| **L2 — Integration** | 21 | 2 170 | `██████████████████████████████` |
| **Security / Hardening** | 6 | 195 | `███` |
| **L3 — Evaluation** | opt-in | cross-impl* | — |

\* L3 is opt-in (`make conformance` / `make replay`, needs Node+network):
32 assertions cMCP-client-vs-TS-server, 8 checks TS-client-vs-cMCP-server,
plus the stateful `host-probe` and `dogfood-crag-host` consumer probes.

<details>
<summary>Per-suite breakdown (all 30 binaries)</summary>

**L1 — Unit**

| Suite | Assertions |
|---|---:|
| test_rpc | 800 |
| test_schema | 600 |
| test_json | 120 |

**L2 — Integration**

| Suite | Assertions |
|---|---:|
| test_stdio_roundtrip | 807 |
| test_client_helpers | 224 |
| test_resources_prompts | 133 |
| test_http_server | 110 |
| test_tools | 108 |
| test_client_server | 106 |
| test_lifecycle | 99 |
| test_elicitation | 80 |
| test_fs_server | 78 |
| test_logging | 53 |
| test_http_client | 51 |
| test_roots | 50 |
| test_notifications | 47 |
| test_structured_output | 41 |
| test_workers | 38 |
| test_crag_cutoff | 38 |
| test_sampling | 37 |
| test_host_cancel_progress | 30 |
| test_ping | 14 |
| test_pagination | 13 |
| test_http_auth | 13 |

**Security / Hardening**

| Suite | Assertions |
|---|---:|
| test_http_ssrf_guard | 76 |
| test_hostile_peer | 71 |
| test_hardening_cross_knob | 22 |
| test_stdio_frame_cap | 12 |
| test_tee_frame_cap | 10 |
| test_handler_rlimit | 4 |

</details>

---

## 2. Quality gates

Every gate below is wired into CI. ✅ = green as of 2026-06-08.

| Gate | Command | What it proves | Status |
|---|---|---|:--:|
| Unit + integration | `make test` | 3 885 assertions / 30 binaries | ✅ |
| Memory safety | `make valgrind` | leak-free, no invalid access | ✅ |
| ASan + UBSan | `make test-asan` | no heap/UB errors | ✅ |
| ThreadSanitizer | `make test-tsan` | no data races | ✅ |
| Fuzzing | `make fuzz-smoke` | 4 libFuzzer harnesses (json/rpc/schema/http) | ✅ |
| Wire regression | `make replay` | captured-transcript replay gate | ✅ |
| Soak (stdio+HTTP) | `make soak` / `soak-http` | no drift over long runs | ✅ |
| Cross-impl conformance | `make conformance` | matches TS reference SDK, both roles | ✅ |
| Spec-drift watch | `make check-spec-drift` | pinned protocol vs upstream | ✅ |

```mermaid
graph LR
    subgraph Correctness
        T[make test ✅]
        R[replay ✅]
        C[conformance ✅]
    end
    subgraph "Memory & Concurrency"
        V[valgrind ✅]
        A[ASan/UBSan ✅]
        TS[TSan ✅]
    end
    subgraph "Adversarial & Endurance"
        F[fuzz ✅]
        S[soak ✅]
        H[hostile-peer ✅]
    end
    classDef ok fill:#dcfce7,stroke:#166534,color:#14532d;
    class T,R,C,V,A,TS,F,S,H ok;
```

---

## 3. Performance (guide Ch.4)

Steady-state stdio `tools/call` throughput, single host ↔ server pair.
Per the guide's metric priority (Ch.4.2), reliability is primary and
latency is comfortably inside the sub-millisecond band that disappears
inside an LLM inference cycle.

| Implementation | calls/s | Relative throughput |
|---|---:|:--|
| **cMCP** | **50 487** | `████████████████████████████████████████` 1.0× |
| TypeScript SDK | ~8 700 | `███████` 0.17× |
| Python SDK | ~1 074 | `█` 0.02× |

- **Latency:** p50 = 19 µs · p99 = 27 µs
- **Speedup:** 5.8× the TS reference SDK, 47× the Python SDK
- **Resilience knobs (Ch.4.1 spike/idle traffic):** HTTP accept-rate
  token bucket, per-recv idle timeout + cumulative deadline (slowloris),
  stdio/HTTP frame caps (OOM).

---

## 4. Security controls (guide Ch.6)

How cMCP's hardening maps onto the guide's MCP threat catalogue. TLS is
deliberately terminator-only (reverse proxy); the deployment threat model
is documented in the architecture notes.

| Guide threat (Ch.6) | cMCP control | Env knob | Test |
|---|---|---|---|
| SSRF via discovery (6.1) | egress guard on resolved peer (rejects metadata/link-local/RFC1918/CGNAT/ULA); DNS-rebinding-safe | `CMCP_HTTP_ALLOW_PRIVATE` | test_http_ssrf_guard |
| DNS rebinding (6.1) | `Origin` allow-list on HTTP server | `CMCP_HTTP_ALLOWED_ORIGINS` | test_http_server |
| Insecure deserialization (6.1/6.3) | schema validation **before** dispatch; depth + element caps | `CMCP_JSON_MAX_DEPTH` / `_ELEMENTS` | test_schema, test_hostile_peer |
| Memory-bomb / OOM (4) | stdio frame cap, HTTP body cap, handler `RLIMIT_AS` | `CMCP_STDIO_MAX_FRAME`, … | test_stdio_frame_cap, test_handler_rlimit |
| Slowloris / spike (4) | accept-rate token bucket, idle + deadline timeouts | `CMCP_HTTP_ACCEPT_RATE`, `_IDLE_TIMEOUT_MS`, `_DEADLINE_MS` | test_hardening_cross_knob |
| Credential leakage (6) | log redactor over credential-shaped keys | `CMCP_LOG_REDACT` | test_http_auth, test_logging |
| Malformed / hostile peer | bounded parsers, fail-closed framing | — | test_hostile_peer |

```mermaid
graph TD
    NET["Untrusted network / peer"] --> ORIG["Origin allow-list"]
    NET --> SSRF["SSRF egress guard"]
    ORIG --> CAP["Frame / body caps · depth+element caps"]
    SSRF --> CAP
    CAP --> SCHEMA["Schema validation (pre-dispatch)"]
    SCHEMA --> RATE["Rate-limit · idle/deadline timeouts"]
    RATE --> HANDLER["Handler (RLIMIT_AS, worker pool)"]
    HANDLER --> REDACT["Log redaction"]
    classDef threat fill:#fee2e2,stroke:#991b1b,color:#7f1d1d;
    classDef ctrl fill:#dcfce7,stroke:#166534,color:#14532d;
    class NET threat;
    class ORIG,SSRF,CAP,SCHEMA,RATE,HANDLER,REDACT ctrl;
```

---

*Regenerate the assertion counts with `make test`; this document is a
point-in-time snapshot (2026-06-08).*
