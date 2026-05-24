# Playbook: echo-server

Echo-server has two tools:

- `echo(text: string) → text` — returns its input unchanged
- `add(a: integer, b: integer) → text` — returns the sum as a decimal string

It's the simplest possible MCP server, which is why it's the gold-standard
playbook: any failure here is a clear signal about the cMCP library or the
tool description (there's no tool logic to blame).

Setup: see [`../claude_code_setup.md`](../claude_code_setup.md). Run each
task in a fresh session (or after `/clear`). For surprising results,
capture the wire transcript via cmcp-tee and drop it under
`conformance/fixtures/echo-server/`.

---

## T1. Single straightforward echo

> Use the cmcp-echo "echo" tool to repeat the phrase "hello world" exactly.

**Expected:** one `tools/call(echo, {text:"hello world"})`, result content
is `"hello world"`.

**Watch for:** model adds quoting or markdown around the string → tool
description should be sharper about "verbatim, no formatting".

---

## T2. Empty string

> Use the echo tool on an empty string.

**Expected:** `tools/call(echo, {text:""})`, result content is empty.

**Watch for:** validator rejects empty string → check `minLength` was
NOT added to the schema (it isn't, today); model refuses to send `""`
→ description ambiguity.

---

## T3. Unicode + emoji

> Use the echo tool on the string `café 🚀 日本語`.

**Expected:** byte-identical round-trip. Exercises UTF-8 in the JSON
parser, the framing layer (no UTF-8 byte gets confused for `\n`), and
the cmcp_tool_text_content emit path.

**Watch for:** mojibake on the way back; result truncated at a multi-byte
boundary; surrogate pairs garbled. Any of those is a JSON encoder bug.

---

## T4. Embedded newlines

> Use the echo tool on a string containing two lines: "first line" and
> "second line", separated by a newline character.

**Expected:** model encodes the newline as `\n` inside the JSON string;
server returns the literal two-line text. Tests escape-aware string
emission in both `cmcp_json_emit` and the host's request builder.

**Watch for:** framing desync (server hangs) → stdio transport is letting
a raw newline through somewhere; result mangled → escape handling bug.

---

## T5. Boundary-size string  **— Mode: shell**

Do NOT run this in-band through a Claude Code session — 8000 repeated
chars × 4 context passes is 30K+ tokens for zero added signal (there's
no agent reasoning being tested, just server-side framing).

Run via the standard shell incantation from
[`../claude_code_setup.md`](../claude_code_setup.md):

```sh
python3 -c 'print("a"*8000, end="")' > /tmp/big.txt
( printf '{"jsonrpc":"2.0","id":1,"method":"initialize","params":{"protocolVersion":"2025-06-18","capabilities":{},"clientInfo":{"name":"t","version":"0"}}}\n'
  printf '{"jsonrpc":"2.0","method":"notifications/initialized"}\n'
  printf '{"jsonrpc":"2.0","id":2,"method":"tools/call","params":{"name":"echo","arguments":{"text":"%s"}}}\n' "$(cat /tmp/big.txt)"
  sleep 0.2
) | ./examples/echo-server > /tmp/echo.out
# Pull out the text field of the second response and check its length
python3 -c '
import json, sys
for line in open("/tmp/echo.out"):
    msg = json.loads(line)
    if msg.get("id") == 2:
        text = msg["result"]["content"][0]["text"]
        print("len =", len(text))
        print("all-a =", set(text) == {"a"})
'
```

**Expected:** `len = 8000`, `all-a = True`.

**Watch for:** read-buffer boundary in the stdio transport at 4 KB or
8 KB chunks. If `len` < 8000, the line accumulator truncated. If
`all-a = False`, something corrupted the payload mid-stream.

---

## T6. Simple arithmetic

> Use the cmcp-echo "add" tool to compute 17 + 25.

**Expected:** `tools/call(add, {a:17, b:25})`, result text is `"42"`.

**Watch for:** model wraps the integers as strings → `add`'s schema
should make `integer` impossible to miss; -32602 returned but model
doesn't auto-correct → error message should name the keyword.

---

## T7. Negative numbers

> Use the add tool to compute -1000 + 1.

**Expected:** result text is `"-999"`. Tests signed-int handling end
to end (host JSON encoder, server JSON parser, snprintf with `%lld`).

---

## T8. Zero

> Use the add tool to compute 0 + 0.

**Expected:** result text is `"0"`. Trivial but checks no special-cases
short-circuit the zero path.

---

## T9. Schema-violating call (intentional)

> Try to use the add tool with a="hello" and b="world".

**Expected:** server returns `-32602` with a structured `data` object
naming the `type` keyword that failed; model surfaces the failure to
the user instead of retrying blindly.

**Watch for:** model retries the same call ≥3 times → either the error
message doesn't say "must be integer", or the host doesn't relay
structured error data to the model. Both are fixable. Note which.

**Known host quirk (Claude Code):** Claude Code's MCP client both (a)
pre-validates `tools/call` arguments against the advertised schema
and short-circuits with a generic `-32602` *before sending*, and (b)
when the server itself returns `-32602`, only surfaces `error.message`
to the assistant — `error.data` is dropped. Net effect: running T9
in-band tells you nothing about cMCP's error-data quality. To verify
the *server*'s error shape, run via shell and inspect the wire
transcript directly. Captured canonical shapes:

- `conformance/fixtures/echo-server/add_schema_type_mismatch.jsonl`
- `conformance/fixtures/echo-server/add_schema_missing_required.jsonl`

Both show `data: {keyword, path, message}` populated correctly.

---

## T10. Tool discovery

> Without calling any tool, list every tool exposed by cmcp-echo with
> its description.

**Expected:** model returns both tools (`echo`, `add`) with the
descriptions registered in `examples/echo-server.c`, *including the
output contract* (both descriptions name what the result block
contains).

**Host quirk (Claude Code):** the host issues `tools/list` itself at
session start and exposes results as native function schemas — the
assistant never sends `tools/list` from inside the loop. T10 is
therefore an *introspection* test, not a `tools/list` wire test. To
test the wire path, drive the server via shell.

**Watch for:** model invents extra detail (e.g. "echo also lowercases
the text" or "add only accepts positive numbers") → descriptions are
too sparse. In particular, descriptions that don't specify the
*output shape* tempt the model to guess (e.g. assume `add` returns a
number rather than a decimal string in a text content block). Both
echo-server descriptions were tightened on the first pass through this
playbook for exactly that reason.

---

## After running

Tally the failures by triage column in
[`../claude_code_setup.md`](../claude_code_setup.md). Each row that
turns up should produce either:

- a tracked issue (cMCP layer bugs)
- a one-line edit in `examples/echo-server.c` (description/schema/error)
- a captured fixture under `conformance/fixtures/echo-server/` for the
  replay gate (regardless of who's at fault).
