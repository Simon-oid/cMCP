# filesystem-mcp

A reference MCP server that exposes sandboxed filesystem operations over
the Model Context Protocol. Built on top of `libcmcp_server`, it is the
second reference server in the cMCP tree.

Unlike [`crag-mcp`](../crag-mcp/README.md) it wraps no external library —
it is pure libc on top of `libcmcp_server`. Its job in the repo is a
**forcing function**: structurally unlike cRAG (no heavy ambient
context, *mutating* tools, four tools instead of two), it proves the
cMCP server API isn't accidentally cRAG-shaped. A second consumer that
looks nothing like the first keeps the library honest.

`filesystem-mcp` has no external dependency, so unlike `crag-mcp` it
**is** built by `make all`:

```sh
make                                # builds filesystem-mcp among the rest
```

## Tools

| Tool       | Purpose                                            |
|------------|----------------------------------------------------|
| `fs_list`  | List the entries of a directory                    |
| `fs_read`  | Read a UTF-8 text file, optionally a line range    |
| `fs_stat`  | Report metadata (type, size, mtime, mode)          |
| `fs_write` | Create or overwrite a file (omitted in read-only)  |

Every path argument is resolved and confined to the server root — see
[The sandbox](#the-sandbox) below.

### `fs_list`

```json
{
  "type": "object",
  "properties": { "path": { "type": "string" } },
  "additionalProperties": false
}
```

`path` is optional; absent or empty lists the root itself. Returns a
single `text` item, one line per entry, sorted by name:

```
dir            4096  sub
file            260  poem.txt
link             11  escape
```

### `fs_read`

```json
{
  "type": "object",
  "properties": {
    "path":   { "type": "string", "minLength": 1 },
    "offset": { "type": "integer", "minimum": 1 },
    "limit":  { "type": "integer", "minimum": 1 }
  },
  "required": ["path"],
  "additionalProperties": false
}
```

`offset` and `limit` select a **1-based line range**; defaults read the
whole file. Returns the selected text as one `text` content item.

The file must be UTF-8 text — a file containing NUL bytes, or invalid
UTF-8, is refused with a tool-level error rather than dumped as binary
onto the wire. The selected range is also capped at `FS_MAX_READ` bytes;
an oversized window asks the caller to narrow it with `offset`/`limit`.

### `fs_stat`

```json
{
  "type": "object",
  "properties": { "path": { "type": "string", "minLength": 1 } },
  "required": ["path"],
  "additionalProperties": false
}
```

Returns a single `text` item:

```
path:     poem.txt
type:     file
size:     260
modified: 2026-05-17T09:30:00Z
mode:     0644
```

The path is canonical when stat'd (symlinks already resolved by the
sandbox), so `type` reflects the symlink *target*, never `link`.

### `fs_write`

```json
{
  "type": "object",
  "properties": {
    "path":    { "type": "string", "minLength": 1 },
    "content": { "type": "string" }
  },
  "required": ["path", "content"],
  "additionalProperties": false
}
```

Creates or overwrites a file with the given text. The parent directory
must already exist — `fs_write` does not `mkdir -p`. It refuses to
overwrite an existing directory.

`fs_write` is **not registered at all** when `FS_READONLY=1`; in that
mode the server advertises three tools and a `tools/call` for `fs_write`
comes back as a JSON-RPC method-not-found error.

## The sandbox

The whole point of this server is that it cannot touch anything outside
its root. The root is canonicalised once at startup; every
client-supplied path is then run through `resolve_path()`, which:

1. Builds an absolute candidate (a leading `/` is taken literally — it
   simply fails the containment check below).
2. Canonicalises it with `realpath()`. This is the load-bearing step:
   `realpath` collapses `..` *and* follows symlinks, so the result is a
   true physical path with no traversal tricks left in it.
3. Checks the result is the root or sits below `root + "/"`. The
   trailing slash matters — a bare prefix test would match `/home/userX`
   against root `/home/user`.

That single check defeats three escape classes at once:

- **`..` traversal** — `realpath` collapses the `..` segments.
- **Symlink escape** — a symlink pointing out of the root resolves to
  its target, which then fails containment.
- **Absolute paths outside the root** — fail containment directly.

`fs_write` to a *new* file is the one wrinkle: the leaf doesn't exist
yet, so `realpath` can't resolve it. The parent directory is
canonicalised instead and the literal basename re-attached, so a new
file still can't be created through a symlinked or `..`-laden parent.

## Configuration

Root directory resolution (first hit wins):

1. `--root <path>` argument
2. `$FS_ROOT` environment variable
3. `.` (the current working directory)

| Variable      | Meaning                                       | Default      |
|---------------|-----------------------------------------------|--------------|
| `FS_ROOT`     | Sandbox root (see resolution order above)     | `.`          |
| `FS_MAX_READ` | Cap, in bytes, on a single `fs_read` response | `1048576`    |
| `FS_READONLY` | If `1`, `fs_write` is not registered          | unset (off)  |

When an MCP host advertises [roots](https://modelcontextprotocol.io/),
the host is expected to pick `--root`/`$FS_ROOT` to match. The server
enforces its own boundary regardless — roots are advisory; the sandbox
is not.

## Quick start

```sh
make

# List, read, and stat inside a sandbox rooted at ./docs.
./tools/cmcp-inspect/cmcp-inspect \
    -- ./tools/filesystem-mcp/filesystem-mcp --root ./docs

./tools/cmcp-inspect/cmcp-inspect -c fs_read \
    -a '{"path":"intro.md","offset":1,"limit":20}' \
    -- ./tools/filesystem-mcp/filesystem-mcp --root ./docs

# Read-only: fs_write disappears from tools/list.
FS_READONLY=1 ./tools/cmcp-inspect/cmcp-inspect \
    -- ./tools/filesystem-mcp/filesystem-mcp --root ./docs
```

Wired into a real MCP host, the server speaks newline-delimited
JSON-RPC 2.0 over stdio. The host spawns the binary, runs the
`initialize` handshake, calls `tools/list`, and dispatches `tools/call`
as the model invokes them.

## Errors

Schema-rejected calls surface as JSON-RPC error `-32602` with structured
`data`:

```json
{
  "code": -32602,
  "message": "arguments failed schema validation",
  "data": { "path": "/path", "keyword": "required", "message": "missing required property" }
}
```

Everything else — a path that escapes the root, a missing file, a
directory passed where a file was expected, a non-UTF-8 file, a failed
write — comes back as a normal `tools/call` result with `isError: true`
and a single `text` content item describing what went wrong. The server
never crashes the connection on a bad call.
