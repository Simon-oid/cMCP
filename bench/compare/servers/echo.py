"""Minimal MCP echo server using the official Python SDK (FastMCP).

Same tool surface as cMCP's examples/echo-server: `echo` returns its
`text` argument unchanged; `add` returns `a + b` as a decimal string.
Used by bench_compare to measure per-call latency against the cMCP
client — the client is held constant so the server is what's measured.

Run:  python3 bench/compare/servers/echo.py    (drives stdio)
"""

from mcp.server.fastmcp import FastMCP

mcp = FastMCP("echo-py")


@mcp.tool()
def echo(text: str) -> str:
    """Return the `text` argument unchanged."""
    return text


@mcp.tool()
def add(a: int, b: int) -> str:
    """Sum two integers and return the result as a decimal string."""
    return str(a + b)


if __name__ == "__main__":
    mcp.run()
