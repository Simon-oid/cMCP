# Playbook: filesystem-mcp

filesystem-mcp exposes four tools over a sandboxed root directory:

- `fs_list(path?: string) → text` — list directory entries
- `fs_read(path: string, offset?: int, limit?: int) → text` — read a UTF-8
  text file, optionally a 1-based line range
- `fs_stat(path: string) → text` — type, size, mtime, mode
- `fs_write(path: string, content: string) → text` — create or overwrite

`fs_write` is omitted from the registry when `FS_READONLY=1`.

Compared to echo-server this surface has *mutating* tools, *path*
arguments (sandbox surface), and *more* tools — so the playbook
also exercises the host's tool-choice behaviour, not just one tool's
ergonomics.

Setup: see [`../claude_code_setup.md`](../claude_code_setup.md).

Recommended registration (with an isolated sandbox so tasks can't
touch anything else on disk):

```sh
mkdir -p /tmp/fs-mcp-sandbox
claude mcp add --scope user cmcp-fs \
    /absolute/path/to/cMCP/tools/filesystem-mcp/filesystem-mcp \
    --root /tmp/fs-mcp-sandbox
```

For sessions you want to capture, wrap in cmcp-tee:

```sh
claude mcp add --scope user cmcp-fs-tee \
    /absolute/path/to/cMCP/tools/cmcp-tee/cmcp-tee \
    /tmp/cmcp-fs-wire.jsonl \
    /absolute/path/to/cMCP/tools/filesystem-mcp/filesystem-mcp \
    --root /tmp/fs-mcp-sandbox
```

Pre-populate the sandbox before each run so the tasks have something
to read:

```sh
rm -rf /tmp/fs-mcp-sandbox && mkdir -p /tmp/fs-mcp-sandbox/sub
echo "line 1
line 2
line 3
line 4
line 5" > /tmp/fs-mcp-sandbox/poem.txt
echo "hello" > /tmp/fs-mcp-sandbox/sub/greeting.txt
```

---

## T1. Discovery

> Without calling any tool, list every tool this filesystem server
> exposes with its description.

**Expected:** four tools (or three if you set `FS_READONLY=1`).

**Watch for:** model conflates `fs_read` and `fs_stat`, or doesn't
know which to call when asked "what's in this file" — descriptions
need to disambiguate read-content vs read-metadata.

---

## T2. Listing the root

> Use the filesystem server to list everything at the root of its sandbox.

**Expected:** one `fs_list` call with `path` absent or empty. Result
includes `poem.txt`, `sub` (dir).

**Watch for:** model sends an absolute path (e.g. `/`) → description
should clarify "paths are relative to the sandbox root, and the root
itself is `""` or omitted".

---

## T3. Reading a file end-to-end

> Read the full contents of poem.txt.

**Expected:** one `fs_read` call with `path:"poem.txt"`, no offset/limit.

**Watch for:** model passes `offset:0` or `limit:0` (both schema-rejected
since `minimum:1`) → schema's `minimum` constraint propagation issue
or description ambiguity around "1-based".

---

## T4. Reading a line range

> From poem.txt, give me only lines 2 and 3.

**Expected:** `fs_read(path:"poem.txt", offset:2, limit:2)` → "line 2\nline 3".

**Watch for:** off-by-one (returns lines 2-4 or 1-2) → server bug;
model picks `offset:1, limit:3` and slices client-side → tool description
should advertise the range support more visibly.

---

## T5. Stat a file

> What's the size of poem.txt?

**Expected:** `fs_stat(path:"poem.txt")`, model reads the `size:` line
out of the text response.

**Watch for:** model uses `fs_read` to count bytes itself → `fs_stat`
description must say "use this instead of fs_read when you only need
metadata".

---

## T6. Stat a directory

> Is "sub" a file or a directory?

**Expected:** `fs_stat(path:"sub")` returning `type: directory`.

**Watch for:** model uses `fs_list` and infers from "did it succeed"
→ stat description must clarify it works on dirs too.

---

## T7. Sandbox escape attempt (intentional)

> Try to read /etc/passwd through this server.

**Expected:** server rejects the request with a clear path-out-of-root
error. Model surfaces the failure to the user, does not retry with
`../../etc/passwd` etc. — or if it does, those attempts also fail.

**Watch for:** any path that *succeeds* and returns content outside
`/tmp/fs-mcp-sandbox/` is a critical sandbox-bypass bug. File
immediately under `conformance/fixtures/filesystem-mcp/` with name
indicating the bypass class.

**Shell-mode variants** (always run all of these; T7 is the load-bearing
sandbox test — driving via shell hits every branch without depending on
the agent's choices):

| # | Tool     | path                              | Expected           |
|---|----------|-----------------------------------|--------------------|
| 1 | fs_read  | `/etc/passwd`                     | escapes root       |
| 2 | fs_read  | `../../etc/passwd`                | escapes root       |
| 3 | fs_read  | `../../../../../../etc/passwd`    | escapes root       |
| 4 | fs_read  | (symlink `escape → /etc/passwd`)  | escapes root       |
| 5 | fs_list  | (symlink `etcdir → /etc`)         | escapes root       |
| 6 | fs_read  | (symlink in sub/ → /etc/passwd)   | escapes root       |
| 7 | fs_write | `/tmp/pwn`                        | escapes root       |
| 8 | fs_write | `../../tmp/pwn`                   | escapes root       |
| 9 | fs_write | (symlink `s → /tmp/non-existent`) | `symlink — refused`|

Variant 9 is the regression for the `O_NOFOLLOW`/lstat fix in
`fs_write_handler` — see `tests/test_fs_server.c::test_write_symlink_leaf_escape_rejected`.
Captured fixture: `conformance/fixtures/filesystem-mcp/fs_write_symlink_leaf_escape.jsonl`
(records the pre-fix attempt verbatim; replaying against the fixed
binary yields the refusal frame).

**Non-bug edge case to be aware of:** a symlink inside the sandbox
pointing to another file inside the sandbox is *transparently followed*
by `fs_write`. The write stays contained, and the response text names
the resolved path (e.g. `wrote 3 bytes to poem.txt` for an input path
of `inside-link`) — so the agent always sees where bytes actually
landed. Not a sandbox escape; just worth noting for tasks that depend
on path-identity invariants.

---

## T8. Write a new file (skip if FS_READONLY=1)

> Create a file called note.txt in the root with the content "test".

**Expected:** `fs_write(path:"note.txt", content:"test")`. Verify with
`ls /tmp/fs-mcp-sandbox/note.txt`.

**Watch for:** model tries `fs_write` to a non-existent subdirectory
(e.g. `nested/note.txt`) → description should clarify "parent must
exist; fs_write does not mkdir -p". Server should reject cleanly.

---

## T9. Overwrite vs append

> Replace the contents of poem.txt with just the word "new".

**Expected:** model uses `fs_write` (overwrite). If it tries to read +
concat + write to "append" or "preserve", the description doesn't
clearly say "fs_write fully replaces".

**Watch for:** model invents a non-existent `fs_append` tool → description
needs to be explicit about the overwrite semantic.

---

## T10. Tool-choice under ambiguity

> What's in the file at "sub/greeting.txt"?

**Expected:** model picks `fs_read`. (The natural-language phrase
"what's in" is ambiguous between read and stat.) Result: "hello\n".

**Watch for:** model calls `fs_stat` first then `fs_read`, or asks
the user to clarify → mild description ambiguity. Acceptable but
worth noting.

---

## After running

If any T7 variant succeeded in reading outside the sandbox, that is
treated as a P0 cMCP/filesystem-mcp bug — the sandbox is the whole
point of this server.

For everything else, apply the triage table from
[`../claude_code_setup.md`](../claude_code_setup.md). Drop captured
wire transcripts under `conformance/fixtures/filesystem-mcp/` named
for the symptom.
