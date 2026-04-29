# JSON Schema subset supported by cMCP

cMCP ships a small JSON Schema validator (`src/schema.c`) used to
check `tools/call` arguments against a tool's declared `inputSchema`
before the handler runs. The subset is deliberately narrow: enough
to express the constraints MCP tools actually use (object shape,
required fields, simple ranges and enums), without dragging in the
parts of JSON Schema that are awkward to implement in plain C.

## What's supported

| Keyword                | Where it applies          | Notes                                                   |
|------------------------|---------------------------|---------------------------------------------------------|
| `type`                 | any value                 | string OR array-of-strings (any of)                     |
| `properties`           | objects                   | recurses into each named property                       |
| `required`             | objects                   | array of property names                                 |
| `additionalProperties` | objects                   | only `false` is meaningful; other values are ignored    |
| `enum`                 | any value                 | deep-equality against each item                         |
| `minLength`            | strings                   | counts Unicode code points, not bytes                   |
| `maxLength`            | strings                   | counts Unicode code points, not bytes                   |
| `minimum`              | integers and numbers      | inclusive                                               |
| `maximum`              | integers and numbers      | inclusive                                               |
| `items`                | arrays                    | single subschema applied to every element               |

### Type tokens

`type` accepts the seven canonical JSON Schema strings:

- `"string"`, `"boolean"`, `"array"`, `"object"`, `"null"`
- `"integer"` — a JSON integer literal (e.g. `7`)
- `"number"` — a JSON integer **or** floating-point literal

cMCP keeps integer and floating-point literals as separate types
internally (`CMCP_JSON_INT` vs `CMCP_JSON_DOUBLE`), so `1` is an
integer but `1.0` is **not**: it matches `"number"` only. If you
want to accept either form for the same field, use
`"type": ["integer", "number"]` or just `"type": "number"`.

### `additionalProperties`

`"additionalProperties": false` rejects any property in the value
that is not named in `properties`. Other shapes documented by the
JSON Schema specification — schemas as the value, `patternProperties`
interaction, etc. — are not modelled in v0.1; anything other than
the literal `false` is treated as "no constraint."

## What's NOT supported (v0.1)

Anything not in the table above is silently ignored. In particular
the validator does not understand:

- `$ref`, `$defs`, `$id` (no inter-schema references)
- `oneOf` / `anyOf` / `allOf` / `not`
- `format` (`email`, `uri`, etc.)
- `pattern` (regex matching on strings)
- `multipleOf`
- `minItems` / `maxItems` / `uniqueItems`
- `minProperties` / `maxProperties`
- `propertyNames`
- `if` / `then` / `else`
- Tuple-form `items` (an array of subschemas)
- `const` (use a single-element `enum` instead)

This is a forward-compatible posture: schemas can include these
keywords without breaking validation; cMCP simply doesn't enforce
them. A stricter validator can be wired in later without changing
caller code.

## Error shape

When validation fails, the validator populates a
`cmcp_schema_error_t` with three fields:

```c
typedef struct {
    char *path;     /* RFC 6901 JSON Pointer to the offending value */
    char *keyword;  /* "type", "required", "enum", ...                */
    char *message;  /* human-readable reason                          */
} cmcp_schema_error_t;
```

`path` is the empty string for the root, otherwise a JSON Pointer
into the value being validated, with `~` and `/` escaped per
RFC 6901 (`~` → `~0`, `/` → `~1`).

The server wires this into the JSON-RPC error so wire-level callers
can react programmatically. A `tools/call` rejected by the validator
returns `-32602 INVALID_PARAMS` with `data` set to:

```json
{
  "path": "/text",
  "keyword": "type",
  "message": "expected type string"
}
```

## Where validation runs

Validation is invoked by `src/server.c` immediately before a tool
handler is dispatched. Tools registered with `input_schema = NULL`
opt out — the handler receives whatever `arguments` the client sent,
unchecked. (This is convenient for tools whose argument shape can't
be expressed in this subset, but most tools should declare a schema.)

The same validator is suitable for use on the client side ahead of a
`tools/call` send; that wiring lands with the full client polish in
Phase 1.9.
