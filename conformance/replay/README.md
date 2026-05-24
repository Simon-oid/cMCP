# conformance/replay/

Wire-fixture replay gate — closes phase 5.3's regression-bank loop.
For each fixture under `conformance/fixtures/`, the driver:

1. Spawns the configured server.
2. Streams every `dir:"in"` frame from the fixture into stdin.
3. Reads the server's stdout.
4. Asserts the observed frames equal the recorded `dir:"out"` frames
   under JSON-equality, with per-fixture **masks** to ignore values
   that legitimately vary across runs (DB paths, hostnames, chunk
   counts).

Anything that breaks an existing transcript without a deliberate spec
or API change is a regression.

Run locally:

```sh
make replay
```

Run a single fixture by substring:

```sh
python3 conformance/replay/replay.py symlink
```

CI runs `make replay` as a separate job alongside `test` / `test-asan`
/ `test-tsan`. Fixtures whose prerequisites aren't met (`crag-mcp` not
built, `CRAG_TEST_DB` unset) skip cleanly — the gate is non-fatal for
the *missing* fixtures, hard-fail for the *present-and-broken* ones.

## Adding a fixture

1. Capture a wire transcript with `tools/cmcp-tee/cmcp-tee` against
   the *fixed*, *correct* server. The recorded `dir:"out"` frames
   are what the gate will assert from then on, so they must already
   represent the right behaviour — never the bug you're documenting.
   (The fixture's *name* archives "this kind of bug was caught here";
   the bytes inside record the correct post-fix output.)

2. Trim the fixture down to just the interaction that matters —
   initialize → notifications/initialized → the call(s) under test
   → end. Drop unrelated turns.

3. Add an entry to `fixtures.json`:

   ```json
   {
     "name": "<server>/<symptom>",
     "fixture": "conformance/fixtures/<server>/<file>.jsonl",
     "server": "<relative-path-to-binary>",
     "args":  ["..."],
     "env":   {"FOO": "bar"},
     "setup":    ["bash command to set up state"],
     "teardown": ["bash command to clean up"],
     "output_masks": [
       {"kind": "set",   "path": "/result/foo",     "value": "<masked>"},
       {"kind": "regex", "path": "/result/content/0/text",
        "pattern": "db:\\s+\\S+", "replacement": "db: <masked>"}
     ],
     "prerequisites": [
       {"kind": "file", "path": "tools/x/y"},
       {"kind": "cmd",  "name": "ollama"},
       {"kind": "env",  "name": "MY_TEST_DB"}
     ]
   }
   ```

4. `make replay` should pass. If it fails immediately after capture,
   the server is non-deterministic in some field that needs a mask —
   add one to `output_masks` rather than relaxing the assertion.

## Mask kinds

| kind  | shape                                            | use case                          |
|-------|--------------------------------------------------|-----------------------------------|
| set   | `path`, `value`                                  | wholesale-replace a leaf value    |
| regex | `path`, `pattern`, `replacement`                 | partial in-string substitution    |

`path` is a slash-separated sequence of object keys / array indices
(JSON-pointer-ish, missing path = no-op). Masks apply to both
expected and actual frames so deterministic content is still strictly
checked.

## Prerequisite kinds

| kind  | skips when …                              |
|-------|-------------------------------------------|
| file  | `path` doesn't exist on disk              |
| cmd   | `name` isn't on `PATH`                    |
| env   | environment variable `name` is unset/empty |

Skips print the reason and do not fail the run. They're how the
hermetic CI lane coexists with the heavier local lane (Ollama-backed
crag-mcp, etc.) using the same registry.
