/* Conformance — JSON Schema validator: cMCP vs Ajv.
 *
 * Closes the Tier 6 axis 6.7 acceptance criterion: the cMCP validator
 * and Ajv (the JSON Schema implementation the TypeScript MCP SDK uses)
 * must agree on accept/reject for every (schema, value) pair in the
 * corpus.
 *
 * Pipeline:
 *   1) Load conformance/corpus_schema.json — a curated list of
 *      (name, schema, value, expected) triples covering each keyword
 *      we support.
 *   2) Run Ajv against each pair. (We let Ajv's own correctness be
 *      authoritative for `expected`.)
 *   3) Spawn the C runner (conformance/schema_ajv_runner) with the
 *      corpus path; collect its tab-delimited <name>\t<ok|fail> output.
 *   4) Compare per-name. Report disagreements; exit 1 on any.
 *
 * Disagreements come in three flavours:
 *   - Ajv-only-rejects: the corpus exposes a keyword cMCP doesn't
 *     enforce. Add it to the audit or implement it.
 *   - Both-reject-different-paths: both fail; that's still agreement
 *     on accept/reject so it's not flagged.
 *   - cMCP-rejects-Ajv-accepts: a real bug in cMCP. Fix it.
 *
 * Run from the repo root via `make schema-conformance`. */

import { readFile } from "node:fs/promises";
import { spawn } from "node:child_process";
import { fileURLToPath } from "node:url";
import { dirname, resolve } from "node:path";
import Ajv from "ajv/dist/2020.js";
import addFormats from "ajv-formats";

const here     = dirname(fileURLToPath(import.meta.url));
const REPO     = resolve(here, "..");
const RUNNER   = process.env.CMCP_SCHEMA_RUNNER
              || resolve(REPO, "conformance/schema_ajv_runner");
const CORPUS   = process.env.CMCP_SCHEMA_CORPUS
              || resolve(here, "corpus_schema.json");

console.error("schema_ajv_crosscheck:");
console.error(`  runner  = ${RUNNER}`);
console.error(`  corpus  = ${CORPUS}`);

const corpus = JSON.parse(await readFile(CORPUS, "utf8"));
console.error(`  loaded ${corpus.length} corpus entries`);

const ajv = new Ajv({
  strict: false,         /* corpus entries may use draft-07-shaped schemas */
  allErrors: false,
});
addFormats(ajv);

/* ---------------------------------------------------------------------- */
/* Run Ajv                                                                */
/* ---------------------------------------------------------------------- */

const ajvResults = new Map();
let ajvCompileErrors = 0;
for (const entry of corpus) {
  let validate;
  try {
    validate = ajv.compile(entry.schema);
  } catch (e) {
    ajvCompileErrors++;
    console.error(`  ajv compile error on ${entry.name}: ${e.message}`);
    /* Treat compile error as "Ajv rejects" so the diff vs cMCP surfaces. */
    ajvResults.set(entry.name, "fail");
    continue;
  }
  const ok = validate(entry.value);
  ajvResults.set(entry.name, ok ? "ok" : "fail");
}

/* ---------------------------------------------------------------------- */
/* Run the cMCP runner                                                    */
/* ---------------------------------------------------------------------- */

const cmcpResults = await new Promise((resolveP, reject) => {
  const child = spawn(RUNNER, [CORPUS], { stdio: ["ignore", "pipe", "inherit"] });
  let buf = "";
  child.stdout.on("data", (chunk) => { buf += chunk.toString("utf8"); });
  child.on("error", reject);
  child.on("close", (code) => {
    if (code !== 0) {
      reject(new Error(`runner exited ${code}`));
      return;
    }
    const m = new Map();
    for (const line of buf.split("\n")) {
      if (!line) continue;
      const tab = line.indexOf("\t");
      if (tab < 0) continue;
      m.set(line.slice(0, tab), line.slice(tab + 1));
    }
    resolveP(m);
  });
});

console.error(`  cmcp returned ${cmcpResults.size} results`);

/* ---------------------------------------------------------------------- */
/* Compare                                                                */
/* ---------------------------------------------------------------------- */

let pass = 0;
let disagree = 0;
let missing = 0;
const expectedAgree = [];     // ajv == corpus.expected, cMCP agrees
const flagged = [];

for (const entry of corpus) {
  const a = ajvResults.get(entry.name);
  const c = cmcpResults.get(entry.name);
  if (c === undefined) {
    missing++;
    flagged.push({ name: entry.name, ajv: a, cmcp: "(missing)", expected: entry.expected });
    continue;
  }
  if (a === c) {
    pass++;
    /* Sanity-check the corpus author against Ajv: if `expected` is set,
     * make sure it matches Ajv. Mismatches surface as a sanity warning
     * but don't fail the gate. */
    if (entry.expected !== undefined) {
      const wanted = entry.expected ? "ok" : "fail";
      if (wanted !== a) {
        expectedAgree.push({ name: entry.name, expected: entry.expected, ajv: a });
      }
    }
    continue;
  }
  disagree++;
  flagged.push({ name: entry.name, ajv: a, cmcp: c, expected: entry.expected });
}

console.error("");
console.error(`  passed:   ${pass} / ${corpus.length}`);
if (missing > 0) console.error(`  missing:  ${missing}   (cMCP runner skipped these)`);
if (disagree > 0) console.error(`  diverge:  ${disagree}`);
if (expectedAgree.length > 0) {
  console.error(`  warn: ${expectedAgree.length} corpus entries' "expected" disagrees with Ajv (corpus bug, not gate failure)`);
}

if (flagged.length > 0) {
  console.error("");
  console.error("  --- disagreements ---");
  for (const d of flagged) {
    console.error(`    ${d.name}   ajv=${d.ajv}   cmcp=${d.cmcp}   expected=${d.expected}`);
  }
}

if (ajvCompileErrors > 0) {
  console.error(`  note: ${ajvCompileErrors} corpus schemas failed to compile under Ajv (treated as 'fail').`);
}

process.exit(disagree === 0 && missing === 0 ? 0 : 1);
