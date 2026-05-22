/* Conformance — Direction B: the MCP TypeScript reference *client* vs
 * a cMCP server.
 *
 * Uses @modelcontextprotocol/sdk's Client + StdioClientTransport to
 * spawn the in-tree `examples/echo-server`, run the initialize
 * handshake, and exercise tools/list + tools/call. Every check proves
 * cMCP's *server* emits the wire format the reference client accepts —
 * the SDK validates each response against its own Zod schemas, so a
 * shape divergence throws before our asserts even run.
 *
 * Run by `make conformance` (after `npm install --prefix conformance`).
 * Exit code: 0 = all checks passed, 1 = a divergence. */

import { Client } from "@modelcontextprotocol/sdk/client/index.js";
import { StdioClientTransport } from "@modelcontextprotocol/sdk/client/stdio.js";

let pass = 0, fail = 0;
function check(cond, label) {
  if (cond) { pass++; console.error(`    ok    ${label}`); }
  else      { fail++; console.error(`    FAIL  ${label}`); }
}

/* Repo-root-relative; `make conformance` runs node from the repo root. */
const SERVER = process.env.CMCP_CONF_SERVER || "./examples/echo-server";

const textOf = (result) =>
  (result.content || [])
    .filter((c) => c.type === "text")
    .map((c) => c.text)
    .join("");

console.error("client_vs_cmcp (TS client vs cMCP echo-server):");

const transport = new StdioClientTransport({ command: SERVER, args: [] });
const client = new Client(
  { name: "cmcp-conformance-ts", version: "0.0.1" },
  { capabilities: {} },
);

try {
  /* connect() runs the full initialize → notifications/initialized
   * handshake; the SDK throws if the server's protocolVersion or
   * result shape is unacceptable. */
  await client.connect(transport);

  const info = client.getServerVersion();
  check(!!info && info.name === "echo-server",
        `handshake → serverInfo.name = ${info?.name}`);
  check(!!info && typeof info.version === "string",
        `handshake → serverInfo.version = ${info?.version}`);

  const { tools } = await client.listTools();
  const names = (tools || []).map((t) => t.name);
  check(names.includes("echo"), "tools/list includes echo");
  check(names.includes("add"),  "tools/list includes add");
  /* The SDK requires every tool to carry an inputSchema. */
  check((tools || []).every((t) => t.inputSchema && typeof t.inputSchema === "object"),
        "every tool advertises an inputSchema");

  const echo = await client.callTool({
    name: "echo",
    arguments: { text: "ts-conformance" },
  });
  check(textOf(echo).includes("ts-conformance"),
        `tools/call echo → "${textOf(echo)}"`);

  const add = await client.callTool({
    name: "add",
    arguments: { a: 2, b: 40 },
  });
  check(textOf(add).includes("42"),
        `tools/call add(2,40) → "${textOf(add)}"`);

  /* A schema violation must come back as a JSON-RPC error, which the
   * SDK surfaces as a thrown McpError. */
  let rejected = false;
  try {
    await client.callTool({ name: "add", arguments: { a: "not-a-number" } });
  } catch {
    rejected = true;
  }
  check(rejected, "tools/call add with bad arguments is rejected");
} catch (err) {
  fail++;
  console.error(`    FAIL  exception: ${err?.stack || err}`);
} finally {
  try { await client.close(); } catch { /* transport already gone */ }
}

console.error(`  ${pass}/${pass + fail} conformance checks passed`);
process.exit(fail === 0 ? 0 : 1);
