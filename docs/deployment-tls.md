# TLS deployment guide

cMCP's HTTP transport **does not link a TLS library** and never will.
This document explains why, and gives concrete recipes for the
deployment shape we expect: a reverse-proxy terminator (nginx, caddy,
HAProxy, Envoy) speaking TLS to clients and plain HTTP/1.1 to cMCP on
the loopback interface.

> Companion documents: [`threat-model.md`](threat-model.md) (boundary
> B1, peer ↔ transport server-side; the row on TLS is closed by this
> doc), [`CHANGELOG.md`](../CHANGELOG.md) phase 6.5.1.

## Why no built-in TLS

A JSON-RPC protocol library is the wrong layer to terminate TLS at:

1. **TLS is its own surface.** Cipher selection, OCSP stapling,
   certificate rotation, SNI, ALPN, session-resumption tuning,
   post-quantum readiness, mTLS revocation — every one of these is
   a moving target. Operators already run terminators that solve
   them; we would be shipping an inferior, hand-rolled fork of work
   they've already done.
2. **The deployed posture wins anyway.** Real deployments put cMCP
   behind nginx/caddy/HAProxy/Envoy regardless of whether the library
   does TLS, because operators want a single place to do
   rate-limiting, header rewriting, HTTP/2 / HTTP/3 fan-in, and
   centralized logging. A "TLS-capable" cMCP would still be deployed
   behind a terminator in production.
3. **No good library choice for static-by-default.** Linking OpenSSL
   makes the static library considerably heavier and forces every
   downstream consumer (butlerbot, cRAG-as-MCP-server, any other host)
   into the OpenSSL ABI. The static-first packaging posture in 6.4
   would have to choose between bloating every binary or shipping
   shared-only — neither is acceptable for a protocol library.
4. **Defense-in-depth scope creep.** Once TLS is in the library, the
   threat model expands to certificate verification, hostname checks,
   CRL handling, key storage. That is *not* the threat surface cMCP
   exists to defend.

This decision is **closed**. See
[`threat-model.md`](threat-model.md) §B1 "Out of scope" and the
"TLS posture rationale" closing question.

## The deployment shape

```
                       TLS                     loopback HTTP/1.1
   client/peer ─────────────────► terminator ───────────────────► cmcp
                                  (nginx,                          (HTTP
                                   caddy,                           transport,
                                   HAProxy,                         bound to
                                   Envoy)                           127.0.0.1)
```

cMCP listens on `127.0.0.1:<port>` (loopback only, never on a public
interface in this posture). The terminator owns:

- Public-facing certificate and key material.
- TLS handshake and cipher negotiation.
- Optional client-certificate validation (mTLS).
- Optional rate-limiting at the edge (orthogonal to cMCP's
  6.5.2 token bucket, which still defends the loopback path).

cMCP is responsible for:

- HTTP framing correctness on the loopback side.
- Slowloris / accept-rate / per-connection budgets (6.5.1, 6.5.2)
  — even though the terminator should never produce a malformed
  request, the budgets defend against a misconfigured or compromised
  terminator on the same host.
- Origin allow-listing (`CMCP_HTTP_ALLOWED_ORIGINS`) for the
  DNS-rebinding defense.

## Trusting `X-Forwarded-*` headers

When a terminator forwards a request, it rewrites `Host` and adds
`X-Forwarded-Proto`, `X-Forwarded-For`, and (optionally) client-cert
headers. cMCP does **not** trust these by default in 0.5.0 — the
transport reads `Host` and `Origin` directly from the wire request.

If a downstream host wants to make decisions based on the *outer*
TLS connection (e.g. "only accept requests that arrived over HTTPS"),
that decision belongs in the terminator's config or in the host's
handler logic, **not** in cMCP. The terminator should reject
non-HTTPS traffic before it reaches the loopback path; cMCP itself
must remain transport-agnostic above the byte stream.

Recipe: the terminator listens on `:443` only, returns `301`/`308`
for any `:80` request, and forwards exclusively over HTTPS-terminated
loopback. cMCP never sees a non-HTTPS request because no such request
reaches the loopback port.

## nginx (most common)

Minimal `server { ... }` block. Assumes cMCP is listening on
`127.0.0.1:8080`. Replace cert paths and `server_name` to taste.

```nginx
server {
    listen              443 ssl http2;
    server_name         mcp.example.com;

    ssl_certificate     /etc/letsencrypt/live/mcp.example.com/fullchain.pem;
    ssl_certificate_key /etc/letsencrypt/live/mcp.example.com/privkey.pem;
    ssl_protocols       TLSv1.2 TLSv1.3;
    ssl_ciphers         HIGH:!aNULL:!MD5;
    ssl_prefer_server_ciphers on;

    # The Streamable HTTP transport uses POST for JSON-RPC frames
    # and a long-lived GET for server-sent events. Disable buffering
    # so SSE chunks reach the client immediately.
    location / {
        proxy_pass         http://127.0.0.1:8080;
        proxy_http_version 1.1;
        proxy_buffering    off;
        proxy_read_timeout 1h;  # SSE streams may idle for minutes
        proxy_set_header   Host              $host;
        proxy_set_header   X-Forwarded-Proto $scheme;
        proxy_set_header   X-Forwarded-For   $proxy_add_x_forwarded_for;
        # Preserve Origin so cMCP's CMCP_HTTP_ALLOWED_ORIGINS check
        # sees what the browser/agent sent.
        proxy_set_header   Origin            $http_origin;
    }
}

server {
    # Force-redirect cleartext to TLS so cMCP never sees a non-HTTPS
    # request via the loopback path.
    listen      80;
    server_name mcp.example.com;
    return      308 https://$host$request_uri;
}
```

Pair with cMCP env at launch:

```sh
export CMCP_HTTP_ALLOWED_ORIGINS="https://agent.example.com"
export CMCP_HTTP_IDLE_TIMEOUT_MS=30000      # default
export CMCP_HTTP_DEADLINE_MS=120000          # default
export CMCP_HTTP_ACCEPT_RATE=100             # default; tune to fit
./my-server                                  # listens on 127.0.0.1:8080
```

## caddy (zero-config TLS)

Caddy auto-provisions certificates and handles HTTP/2 and HTTP/3
without ceremony. Caveat: caddy buffers responses by default, which
breaks SSE — `flush_interval -1` disables it.

```caddy
mcp.example.com {
    reverse_proxy 127.0.0.1:8080 {
        flush_interval -1
        header_up Host        {host}
        header_up Origin      {header.Origin}
        header_up X-Forwarded-Proto {scheme}
    }
}
```

## HAProxy

HAProxy is the most common terminator for high-throughput deployments.
The SSE path needs explicit "no body buffer" handling — without it,
HAProxy's `option httpclose` will close the stream on every event.

```haproxy
frontend mcp_in
    bind            *:443 ssl crt /etc/haproxy/certs/mcp.example.com.pem
    mode            http
    http-request    set-header X-Forwarded-Proto https
    default_backend mcp_out

backend mcp_out
    mode            http
    option          http-server-close          # keep stream alive for SSE
    timeout server  1h                          # SSE may idle for minutes
    server          cmcp 127.0.0.1:8080
```

## Mutual TLS (mTLS) — terminator does the work

For deployments where the *peer* must present a certificate, the
terminator validates the chain and forwards a synthesised header
(`X-Client-Cert-*`, `X-Client-Verify`, etc.). cMCP does not parse
these headers natively; the host's handler logic does.

Recipe (nginx):

```nginx
server {
    listen                       443 ssl;
    ssl_client_certificate       /etc/ssl/client-ca.pem;
    ssl_verify_client            on;
    ssl_verify_depth             2;

    location / {
        proxy_pass               http://127.0.0.1:8080;
        proxy_set_header         X-Client-Verify $ssl_client_verify;
        proxy_set_header         X-Client-DN     $ssl_client_s_dn;
        proxy_set_header         X-Client-SAN    $ssl_client_s_dn_legacy;
    }
}
```

The host's handler can then inspect those headers (via whatever
out-of-band mechanism it uses to access the inbound request — cMCP
does not currently expose request headers to handlers, so this part
needs deliberate host integration). Treat the headers as advisory
input; the terminator's `ssl_verify_client` is the authoritative
gate.

## What about local-loopback TLS?

If the terminator runs on a different host than cMCP, the loopback
hop is no longer loopback — and an attacker on the network between
them could MITM the cleartext side. Two options:

1. **Don't.** Run the terminator on the same host as cMCP, full stop.
   This is the documented and tested posture.
2. **Use a TCP-layer encrypted tunnel.** spiped, stunnel, or
   WireGuard between the terminator host and the cMCP host. cMCP
   still terminates plain HTTP/1.1 at its end; the tunnel handles
   the wire encryption transparently. This pattern is well-trodden
   for Redis / Postgres / other backends with the same "no TLS in
   the protocol library" posture.

No in-tree support is planned for either option. The terminator
on the same host is the supported deployment.

## Checklist before going to production

- [ ] cMCP bound to `127.0.0.1`, not `0.0.0.0`.
- [ ] Terminator listens on `:443` with a valid cert chain.
- [ ] Terminator returns `301`/`308` for any `:80` traffic.
- [ ] `CMCP_HTTP_ALLOWED_ORIGINS` set to the exact host(s) the agent
      will use (DNS-rebinding defense).
- [ ] SSE buffering disabled in the terminator (nginx
      `proxy_buffering off`, caddy `flush_interval -1`, HAProxy
      `http-server-close` + long server timeout).
- [ ] `CMCP_HTTP_DEADLINE_MS` and `CMCP_HTTP_ACCEPT_RATE` left at
      defaults unless the deployment has a specific reason to tune.
- [ ] Logs reviewed for redaction: confirm `CMCP_LOG_REDACT` is at
      its default (`1`); do not disable in production.
- [ ] Health-check path (e.g. terminator's `/healthz`) handled by
      the terminator itself, not by cMCP — keep the protocol path
      pure.
