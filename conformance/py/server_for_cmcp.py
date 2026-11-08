"""Conformance — Direction D witness: an MCP Python SDK *server* for the
cMCP client to talk to.

The SDK (`mcp` 2.x) is dual-era: the same tools answer a legacy
(2025-11-25) client through an `initialize` session and a modern
(2026-07-28) client statelessly. `greet` needs the user's name, which
the SDK obtains with a server-initiated elicitation on the legacy path
and with an InputRequiredResult (multi round-trip request) on the
modern one. `where` marks its argument with `x-mcp-header`, so over
Streamable HTTP the SDK checks the client's `Mcp-Param-Region` header.

    server_for_cmcp.py              serve stdio
    server_for_cmcp.py --http PORT  serve Streamable HTTP on 127.0.0.1:PORT/mcp
"""

import sys
from typing import Annotated

from pydantic import BaseModel, Field

from mcp.server.mcpserver import Elicit, MCPServer, Resolve


class Name(BaseModel):
    name: str


def ask_name() -> Elicit[Name]:
    return Elicit("What is your name?", Name)


server = MCPServer("py-witness", version="2.3.0")


@server.tool()
def echo(text: str) -> str:
    """Echo the text back."""
    return text


@server.tool()
def add(a: int, b: int) -> str:
    """Add two integers."""
    return str(a + b)


@server.tool()
def greet(who: Annotated[Name, Resolve(ask_name)]) -> str:
    """Greet the user, asking for the name first."""
    return f"Hello, {who.name}!"


@server.tool()
def where(region: Annotated[str, Field(json_schema_extra={"x-mcp-header": "Region"})]) -> str:
    """Report the region the request was routed for."""
    return f"region={region}"


if __name__ == "__main__":
    if len(sys.argv) == 3 and sys.argv[1] == "--http":
        server.run(transport="streamable-http", host="127.0.0.1", port=int(sys.argv[2]))
    else:
        server.run()
