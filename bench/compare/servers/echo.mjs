/* Minimal MCP echo server using the official TypeScript SDK.
 *
 * Same tool surface as cMCP's examples/echo-server: `echo` returns its
 * `text` argument unchanged; `add` returns `a + b` as a decimal string.
 * Used by bench_compare to measure per-call latency against the cMCP
 * client — the client is held constant so the server is what's measured.
 *
 * Run:  node bench/compare/servers/echo.mjs   (drives stdio) */

import { McpServer } from "@modelcontextprotocol/sdk/server/mcp.js";
import { StdioServerTransport } from "@modelcontextprotocol/sdk/server/stdio.js";
import { z } from "zod";

const server = new McpServer({ name: "echo-ts", version: "0.0.1" });

server.tool(
  "echo",
  "Return the `text` argument unchanged.",
  { text: z.string() },
  async ({ text }) => ({ content: [{ type: "text", text }] }),
);

server.tool(
  "add",
  "Sum two integers and return the result as a decimal string.",
  { a: z.number().int(), b: z.number().int() },
  async ({ a, b }) => ({ content: [{ type: "text", text: String(a + b) }] }),
);

await server.connect(new StdioServerTransport());
