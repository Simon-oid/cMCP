# crag-mcp

A reference MCP server that exposes a [cRAG](https://github.com/Simon-oid/cRAG)
index over the Model Context Protocol. Built on top of `libcmcp_server`,
this is the first real cMCP consumer and the canonical example of how to
wrap an existing C library as an MCP tool surface.

`crag-mcp` is **not** built by `make all` — it depends on cRAG headers
and `.o` files at `$(CRAG_DIR)` (default `../cRAG`). Build it explicitly:

```sh
make crag-mcp                       # cRAG at ../cRAG
make crag-mcp CRAG_DIR=/path/to/cRAG
```

## Tools

| Tool          | Purpose                                                    |
|---------------|------------------------------------------------------------|
| `crag_search` | Hybrid retrieval (BM25 + cosine, RRF-fused) over the index |
| `crag_stats`  | Diagnostics: DB path, chunk count, file count, dim         |

### `crag_search`

```json
{
  "type": "object",
  "properties": {
    "query": { "type": "string", "minLength": 1 },
    "k":     { "type": "integer", "minimum": 1, "maximum": 20 }
  },
  "required": ["query"],
  "additionalProperties": false
}
```

Returns one MCP `text` content item per retrieved chunk, fusion-ranked:

```
[cos 0.715  bm25 12.62  fusion 0.033] path/to/source.md
<chunk body>
```

Three scores lead each item:

- `cos` — raw cosine similarity, the only *calibrated* relevance signal
  (`[-1, 1]`, higher = better).
- `bm25` — lexical score; `0.00` means no lexical hit (cosine-only).
- `fusion` — the RRF score results are *ranked* by. Its range is
  structural (`~0.033` = ranked #1 by both retrievers, `~0.016` = by
  one), so it **cannot** be read as relevance — that's what `cos` is for.

Results are gated on `cos`: a chunk below the cutoff (default `0.50`,
see `CRAG_MIN_COSINE`) is dropped as the one-retriever floor rather than
a real match. If every hit falls below it, the tool returns a single
`(no chunk cleared the relevance threshold)` item — a valid, non-error
answer to a query nothing matches.

Default `k` is 5; the server also clamps to `[1, 20]` defensively.

### `crag_stats`

No arguments. Returns a single `text` item:

```
db:     /path/to/crag.db
chunks: 1234
files:  56
dim:    768
```

## Why no `crag_index` tool

Indexing is **deliberately not exposed**. cRAG's CLI has `index`,
`reindex`, `import`, and `export` — none of these are MCP tools here.
Reasons, in priority order:

1. **Principle of least authority.** A retrieval surface should only
   retrieve. Letting an LLM trigger a re-index against a path it
   chooses is an admin-plane operation pretending to be a tool call.
2. **Latency / state shape.** Indexing is long-running, partially
   failable, and changes the DB *for everyone using it*. MCP tool
   calls are short, stateless, idempotent-friendly. Indexing is none
   of those.
3. **Layering.** The MCP wrapper is a read-only view of an already-
   built index. Building the index is an out-of-band operation owned
   by whoever runs cRAG.

Use `crag index <dir>` from the cRAG CLI to populate the DB. cRAG's
own write-side already has `--workers`, `--force`, `--include-known-docs`,
and progress reporting — none of which translates well to a JSON-RPC
tool.

If you have a use case that genuinely needs runtime indexing through
MCP, the right fix is a separate `crag-admin-mcp` server with its own
auth gate, not a tool on this one.

## Configuration

DB path resolution (first hit wins):

1. `--db <path>` argument
2. `$CRAG_DB` environment variable
3. `./crag.db`

Embedder selection — passes through to cRAG unchanged:

| Variable               | Meaning                                  | Default       |
|------------------------|------------------------------------------|---------------|
| `CRAG_EMBED_BACKEND`   | `local`, `openai`, or `ollama`           | `local`       |
| `CRAG_EMBED_URL`       | HTTP endpoint for the chosen backend     | backend default |
| `OPENAI_API_KEY`       | Required for `openai` backend            | —             |

The embedder model used at search time **must match** the one used to
build the index (same dim, same provider). cRAG itself doesn't enforce
this — pick one combination and keep it.

### Relevance gating

`crag_search` drops results whose cosine similarity falls below a
cutoff, so a query that matches nothing returns an explicit "no match"
instead of the junk the fusion ranking would otherwise surface.

| Variable          | Meaning                                       | Default |
|-------------------|-----------------------------------------------|---------|
| `CRAG_MIN_COSINE` | Minimum cosine for a result to count as a hit | `0.50`  |

The default is calibrated for `mxbai-embed-large` (genuine matches land
at cosine 0.62–0.72, irrelevant queries still score 0.30–0.41 — that
model has a high cosine floor). A different embedding model has a
different floor — recalibrate. Set `CRAG_MIN_COSINE` to a value `<= -1`
to disable gating and return raw fusion-ranked results.

## Quick start

Index a directory with the cRAG CLI, then point `crag-mcp` at the same DB:

```sh
# 1. Build the index out-of-band.
crag index ./docs --db ./crag.db --backend ollama \
    --url http://localhost:11434/api/embeddings

# 2. Drive the MCP server through cmcp-inspect.
export CRAG_EMBED_BACKEND=ollama
export CRAG_EMBED_URL=http://localhost:11434/api/embeddings

./tools/cmcp-inspect/cmcp-inspect \
    -- ./tools/crag-mcp/crag-mcp --db ./crag.db

./tools/cmcp-inspect/cmcp-inspect -c crag_stats \
    -- ./tools/crag-mcp/crag-mcp --db ./crag.db

./tools/cmcp-inspect/cmcp-inspect \
    -c crag_search -a '{"query":"hybrid retrieval","k":3}' \
    -- ./tools/crag-mcp/crag-mcp --db ./crag.db
```

Wired into a real MCP host, the server speaks newline-delimited
JSON-RPC 2.0 over stdio. The host spawns the binary, runs the
`initialize` handshake, calls `tools/list`, and dispatches `tools/call`
as the model invokes them.

## Errors

Schema-rejected calls surface as JSON-RPC error `-32602` with
structured `data`:

```json
{
  "code": -32602,
  "message": "arguments failed schema validation",
  "data": { "path": "/k", "keyword": "maximum", "message": "value above maximum" }
}
```

Runtime failures (embedding, retrieval, OOM) come back as a normal
`tools/call` result with `isError: true` and a single `text` content
item describing what went wrong. The server never crashes the
connection on a bad call.
