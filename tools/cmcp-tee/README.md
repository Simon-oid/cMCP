# cmcp-tee

Transparent stdio MCP proxy with JSONL wire capture. Wraps any cMCP
server so a host can drive it normally while every frame in both
directions is recorded byte-for-byte. Feeds `conformance/fixtures/` and
the replay-based regression gate.

```
cmcp-tee <log.jsonl> <server-path> [server-args...]
```

`cmcp-tee` deliberately links **no** cMCP libraries — only pthreads — so
it can wrap servers built against any cMCP revision without ABI
surprises.

## Log format

One JSON object per line:

```json
{"t":<unix-seconds-float>,"dir":"in"|"out","frame":"<raw JSON frame>"}
```

- `dir":"in"` — host → server (client request / notification)
- `dir":"out"` — server → host (response / notification)

The trailing newline is stripped before logging. The file is opened in
append mode, so multiple sessions accumulate and are separated by `t`.
The wrapped server's stderr is not touched.

## Environment

| Variable | Purpose |
|---|---|
| `CMCP_TEE_MAX_FRAME` | Per-frame byte cap on the **log record** (not the wire — forwarding stays faithful). Default `1048576` (1 MiB); `0` disables the cap. A frame over the cap is logged clipped to the cap with extra fields `"truncated":true,"orig_len":<true-bytes>,"cap":<cap>`. Snapshotted once at startup. This stops a hostile upstream server from turning the wire log into a memory bomb — `cmcp-tee` can't reuse the server-side `CMCP_JSON_MAX_DEPTH`/`_MAX_ELEMENTS` caps because it links no cMCP code. |

Regression coverage: `tests/test_tee_frame_cap.c`.
