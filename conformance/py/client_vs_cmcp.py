"""Conformance — Direction C: the MCP Python SDK *client* vs a cMCP server.

The Python SDK (`mcp` 2.x) is a dual-era implementation: it speaks the
legacy, handshake-based revisions (2025-11-25 and earlier) and the
modern, stateless revision 2026-07-28. That makes it the reference
witness for cMCP's own dual-era upgrade.

The same checks run once per client connect mode:

  legacy      initialize handshake (2025-11-25 behaviour)
  auto        probe `server/discover`, fall back to legacy on failure
  2026-07-28  modern per-request `_meta`, no handshake

Each mode also runs an input round-trip against examples/ask-server,
whose one tool asks for the user's name: by a server-initiated
`elicitation/create` on the legacy path, by an InputRequiredResult and a
retry with `inputResponses` (multi round-trip requests) on the modern
path. The SDK answers through the same elicitation callback either way.

The modern modes then repeat the input round-trip over Streamable HTTP
(`ask-server --http PORT`): no session, mirrored Mcp-* headers, and the
InputRequiredResult in a plain POST response. Legacy mode is not run
over HTTP: cMCP's HTTP server does not yet deliver server-initiated
requests on the legacy GET stream.

EXPECTED below records what each mode should do against the current
cMCP build. A mode that behaves differently from its expectation fails
the run, whether it unexpectedly passes or unexpectedly fails, so the
table has to be updated on purpose as the upgrade lands.

Run by `make conformance-py`. Exit code: 0 = every mode matched its
expectation, 1 = a divergence.
"""

import asyncio
import os
import socket
import subprocess
import sys
import time

from mcp import Client, StdioServerParameters, types

SERVER = os.environ.get("CMCP_CONF_SERVER", "./examples/echo-server")
ASK_SERVER = os.environ.get("CMCP_CONF_ASK_SERVER", "./examples/ask-server")

# mode -> (expected outcome, protocol version the session must report)
EXPECTED = {
    "legacy": ("pass", "2025-11-25"),
    "auto": ("pass", "2026-07-28"),  # discover succeeds, client stays modern
    "2026-07-28": ("pass", "2026-07-28"),  # stateless, no handshake
}


def text_of(result) -> str:
    return "".join(c.text for c in result.content if getattr(c, "type", "") == "text")


async def run_checks(mode: str) -> list[tuple[bool, str]]:
    checks: list[tuple[bool, str]] = []
    params = StdioServerParameters(command=SERVER, args=[])
    async with Client(params, mode=mode, read_timeout_seconds=10) as client:
        checks.append((True, f"connected, protocol {client.protocol_version}"))
        _, want_version = EXPECTED[mode]
        checks.append(
            (client.protocol_version == want_version, f"protocol version is {want_version}")
        )

        tools = (await client.list_tools()).tools
        names = [t.name for t in tools]
        checks.append(("echo" in names and "add" in names, f"tools/list -> {names}"))

        echo = await client.call_tool("echo", {"text": "py-conformance"})
        checks.append(("py-conformance" in text_of(echo), f"echo -> {text_of(echo)!r}"))

        add = await client.call_tool("add", {"a": 2, "b": 40})
        checks.append(("42" in text_of(add), f"add(2,40) -> {text_of(add)!r}"))

        bad = await client.call_tool("add", {"a": "not-a-number"})
        checks.append((bool(bad.is_error), "schema violation -> isError result"))

    checks.extend(await run_input_checks(mode))
    if mode != "legacy":
        checks.extend(await run_http_checks(mode))
    return checks


async def run_input_checks(mode: str) -> list[tuple[bool, str]]:
    """greet asks for a name; the callback answers it."""
    asked: list[str] = []

    async def on_elicit(context, params):
        asked.append(params.message)
        return types.ElicitResult(action="accept", content={"name": "Ada"})

    params = StdioServerParameters(command=ASK_SERVER, args=[])
    async with Client(
        params, mode=mode, read_timeout_seconds=10, elicitation_callback=on_elicit
    ) as client:
        greet = await client.call_tool("greet", {})
    return [
        (asked == ["What is your name?"], f"elicitation asked once -> {asked}"),
        (text_of(greet) == "Hello, Ada!", f"greet -> {text_of(greet)!r}"),
    ]


def free_port() -> int:
    with socket.socket() as s:
        s.bind(("127.0.0.1", 0))
        return s.getsockname()[1]


def wait_listening(port: int, timeout: float = 5.0) -> None:
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        with socket.socket() as s:
            if s.connect_ex(("127.0.0.1", port)) == 0:
                return
        time.sleep(0.05)
    raise TimeoutError(f"ask-server did not listen on {port}")


async def run_http_checks(mode: str) -> list[tuple[bool, str]]:
    """greet over Streamable HTTP: the MRTR round-trip in plain POSTs."""
    port = free_port()
    proc = subprocess.Popen([ASK_SERVER, "--http", str(port)])
    try:
        wait_listening(port)
        asked: list[str] = []

        async def on_elicit(context, params):
            asked.append(params.message)
            return types.ElicitResult(action="accept", content={"name": "Lin"})

        url = f"http://127.0.0.1:{port}/mcp"
        async with Client(
            url, mode=mode, read_timeout_seconds=10, elicitation_callback=on_elicit
        ) as client:
            names = [t.name for t in (await client.list_tools()).tools]
            greet = await client.call_tool("greet", {})
        return [
            (names == ["greet"], f"http tools/list -> {names}"),
            (asked == ["What is your name?"], f"http elicitation asked once -> {asked}"),
            (text_of(greet) == "Hello, Lin!", f"http greet -> {text_of(greet)!r}"),
        ]
    finally:
        proc.terminate()
        proc.wait(timeout=5)


async def run_mode(mode: str) -> bool:
    expected, _ = EXPECTED[mode]
    try:
        checks = await asyncio.wait_for(run_checks(mode), timeout=20)
        for ok, label in checks:
            print(f"    {'ok  ' if ok else 'FAIL'}  {label}", file=sys.stderr)
        outcome = "pass" if all(ok for ok, _ in checks) else "fail"
    except Exception as exc:  # any transport or protocol failure counts as "fail"
        # anyio wraps the real cause in an ExceptionGroup; show the leaf.
        while isinstance(exc, BaseExceptionGroup) and exc.exceptions:
            exc = exc.exceptions[0]
        print(f"    error {type(exc).__name__}: {exc}", file=sys.stderr)
        outcome = "fail"

    matched = outcome == expected
    verdict = "as expected" if matched else "UNEXPECTED"
    print(f"  mode {mode:<11} {outcome} (expected {expected}) -> {verdict}", file=sys.stderr)
    return matched


async def main() -> int:
    print("client_vs_cmcp (Python SDK client vs cMCP echo-server):", file=sys.stderr)
    results = [await run_mode(mode) for mode in EXPECTED]
    return 0 if all(results) else 1


if __name__ == "__main__":
    sys.exit(asyncio.run(main()))
