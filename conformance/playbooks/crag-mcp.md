# Playbook: crag-mcp

crag-mcp wraps cRAG (the user's BM25+cosine hybrid retriever) as MCP. It
exposes:

- `crag_search(query: string, k?: int) → text` — hybrid retrieval over
  the index, returning ranked chunks
- `crag_stats() → text` — index size, chunk count, model id, etc.
- resource `crag://stats` — the same diagnostics, exposed as a resource
  so hosts that pin resources can keep stats in ambient context

Indexing is deliberately **not** exposed — admin-plane operations don't
belong on a retrieval surface. The playbook respects this: every task
is read-only.

This is the most realistic of the three reference servers because the
workload (text retrieval) is what real agents actually do.

## Setup

crag-mcp is NOT built by `make all`. Build it explicitly:

```sh
make crag-mcp                              # assumes ../cRAG exists
make crag-mcp CRAG_DIR=/path/to/cRAG       # otherwise
```

You also need a pre-built cRAG index — see cRAG's own README for the
indexing flow. Once you have an index path (e.g.
`/home/you/notes.crag`), register:

```sh
claude mcp add --scope user cmcp-crag \
    /absolute/path/to/cMCP/tools/crag-mcp/crag-mcp \
    --index /home/you/notes.crag
```

For captured-fixture sessions:

```sh
claude mcp add --scope user cmcp-crag-tee \
    /absolute/path/to/cMCP/tools/cmcp-tee/cmcp-tee \
    /tmp/cmcp-crag-wire.jsonl \
    /absolute/path/to/cMCP/tools/crag-mcp/crag-mcp \
    --index /home/you/notes.crag
```

---

## T1. Discovery

> What can you do with the cmcp-crag server?

**Expected:** model lists `crag_search` + `crag_stats` and mentions the
`crag://stats` resource. Result framing matches the descriptions in
`tools/crag-mcp/main.c`.

**Watch for:** model misses the resource entirely → host doesn't surface
resources alongside tools by default, OR description is too tool-focused.

---

## T2. Bounded ambient context

> Without making any tool call, tell me how many chunks the index holds.

**Expected:** model reads the `crag://stats` resource (no tools/call).
Demonstrates the read-without-acting path.

**Watch for:** model issues `tools/call(crag_stats)` instead → the
resource is invisible to the host, OR resource descriptions don't make
clear they're cheaper than the tool.

---

## T3. Plain-English query

> Search the index for content about authentication.

**Expected:** `crag_search(query:"authentication")` with default k.
Returns ranked chunks.

**Watch for:** model wraps the query in quotes or syntax it thinks is
required → description should explicitly say "plain English, no special
syntax".

---

## T4. Top-k respected

> Give me the top 3 most relevant chunks about indexing.

**Expected:** `crag_search(query:"indexing", k:3)` returning exactly 3
chunks (or fewer if the corpus is small).

**Watch for:** k exceeds CRAG's documented max → server clamps and the
response shape needs to advertise actual k returned vs requested.

---

## T5. Empty-query rejection (intentional)

> Search the index using an empty query string.

**Expected:** `-32602` schema error (the query schema should have
`minLength:1`).

**Watch for:** server happily runs an empty query → schema needs
strengthening. Also a good fuzz-corpus seed.

---

## T6. Cross-tool reasoning

> Search for "ranking", and then tell me what model embedded those
> results.

**Expected:** model calls `crag_search` then either `crag_stats` or
reads `crag://stats` to find the embed-model id. Two-step reasoning
across the surface.

**Watch for:** model invents an answer instead of reading stats →
description should hint that `model` is in stats.

---

## T7. Long query

> Search using the following 200-word paragraph as the query: <paste>.

**Expected:** call succeeds, retrieval still reasonable. Tests the
long-string path through the server's JSON parser + cRAG's tokeniser.

**Watch for:** truncation, segfault, "too long" rejection. Any of those
is a server-level robustness issue.

---

## T8. Stats stability across calls

> Call crag_stats twice in a row and tell me if anything differs.

**Expected:** identical responses (the server is read-only; nothing
should mutate). Useful canary for any accidental state in the stats
path.

**Watch for:** any non-equal field → either real bug or a misleading
"last query" stat that wasn't documented.

---

## T9. Unicode in queries

> Search for "café" — does the index find anything with this exact
> accented character?

**Expected:** byte-faithful query reaches cRAG, retrieval runs against
whatever the tokeniser does with diacritics.

**Watch for:** query mangled before it hits cRAG (encoding bug in our
JSON-to-cRAG handoff); zero results when a manual `crag-cli search`
finds something → the wrapper is mishandling the string.

---

## T10. Result faithfulness

> Pick any chunk crag_search returned in T3 and find it verbatim in
> the source file the chunk says it came from.

**Expected:** the chunk text appears in the cited file at the cited
offset.

**Watch for:** any chunk that DOESN'T match its citation → either cRAG
bug or, more interestingly, an MCP-layer bug where the wrapper is
re-ordering or stringifying the result incorrectly.

---

## After running

crag-mcp is the most "real" workload of the three. Failures here are
the ones that matter most for butlerbot's eventual integration.

Two extra triage rules on top of the standard table:

- **Empty / wrong results when manual cRAG works:** cMCP wrapper bug
  (look at `tools/crag-mcp/main.c`).
- **Identical to manual cRAG but the model can't use it:** tool
  description or schema problem.

Drop captured transcripts under `conformance/fixtures/crag-mcp/`.
