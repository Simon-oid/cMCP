# JSON Schema conformance — cMCP vs Ajv

This document is the **public surface** of cMCP's schema validator
(`src/schema.c`). It supersedes the earlier `schema-subset.md` and is
the reference for what an MCP tool author can rely on when declaring
`inputSchema` against a cMCP-hosted server.

Tier 6 axis 6.7 expanded the validator from a narrow subset (9
keywords) to near-parity with Ajv (the JSON Schema implementation the
TypeScript MCP SDK uses). The full audit and gap-tracking lives in
`schema-audit.md`.

## Target draft

cMCP implements **JSON Schema draft 2020-12** semantics with **draft-
07 wire compatibility**. Concretely: we use the 2020-12 keyword names
internally (`prefixItems`, `$defs`) and accept the draft-07 aliases
(`items` as tuple, `additionalItems`, `definitions`) without emitting
them ourselves. A schema written for either draft will validate
identically on cMCP and Ajv for every keyword in this document.

## Supported keywords

### Any instance

| Keyword | Behaviour |
|---|---|
| `type` | One of `string`, `number`, `integer`, `boolean`, `array`, `object`, `null`, or an array of these for any-of-types semantics. |
| `enum` | Deep-equality compare against each item. |
| `const` | Deep-equality compare against the single value. |

### Numeric

| Keyword | Behaviour |
|---|---|
| `minimum`, `maximum` | Inclusive bounds. |
| `exclusiveMinimum`, `exclusiveMaximum` | Strict bounds (draft-2020-12 numeric form). |
| `multipleOf` | Integer fast-path (modulo); fallback to `fmod` with a 1e-9 relative epsilon for fractional divisors. |

### Strings

| Keyword | Behaviour |
|---|---|
| `minLength`, `maxLength` | Unicode code points, not bytes. |
| `pattern` | **POSIX ERE** via `<regex.h>` — see the [Regex flavour](#regex-flavour) note below. A pattern that fails to compile is silently dropped (schema-author error); the value is not rejected on that account. |

### Arrays

| Keyword | Behaviour |
|---|---|
| `items` (single subschema) | Applied to every element. |
| `items` (array, draft-07 tuple) | Applied positionally to the leading entries. |
| `prefixItems` (draft-2020-12) | Same as draft-07 tuple `items`. |
| `additionalItems` (draft-07) | Subschema or boolean applied to entries past the tuple length. |
| `items` (draft-2020-12, paired with `prefixItems`) | Subschema or boolean applied to entries past the prefix. |
| `minItems`, `maxItems` | Inclusive bounds on length. |
| `uniqueItems` | Deep-equality check across all pairs. |

### Objects

| Keyword | Behaviour |
|---|---|
| `properties` | Recurses into each named property. |
| `required` | Array of property names — all must be present. |
| `additionalProperties: false` | Reject any property not covered by `properties` or `patternProperties`. |
| `additionalProperties: <subschema>` | Apply the subschema to every uncovered property. |
| `patternProperties` | POSIX ERE patterns → subschema applied to every value whose key matches. |
| `propertyNames` | Subschema applied to each key (the key is materialised as a string value). |
| `minProperties`, `maxProperties` | Inclusive bounds on key count. |

### Boolean combinators

| Keyword | Behaviour |
|---|---|
| `allOf` | Every subschema must validate. First failure surfaces. |
| `anyOf` | At least one subschema must validate. Short-circuits on first match. |
| `oneOf` | Exactly one subschema must validate (stops counting after the second match). |
| `not` | The subschema must NOT validate. |
| `if` / `then` / `else` | Conditional. `if` runs in schema-only mode — its outcome only steers the branch. |

### Boolean schemas

The literal schemas `true` (always accept) and `false` (always reject)
are honoured at every position — top-level, in `properties`, in
`additionalProperties`, in `items`, inside combinators. The empty
schema `{}` also accepts everything (it falls out of the keyword set
being empty).

## Annotation keywords

The following do not affect validation. They flow through to consumers
via `tools/list` etc. (`title`, `description`) but are not enforced:

`title`, `description`, `default`, `examples`, `readOnly`, `writeOnly`,
`deprecated`, `$schema`, `$comment`, `$id`.

## Deliberate departures

### Regex flavour {#regex-flavour}

`pattern` and `patternProperties` use **POSIX Extended Regular
Expressions** (`regcomp(..., REG_EXTENDED | REG_NOSUB)`), not
ECMAScript 262 regex.

Differences that may affect cross-implementation behaviour:

- POSIX ERE has no `\d`, `\w`, `\s` character class shortcuts. Use
  `[[:digit:]]`, `[[:alnum:]_]`, `[[:space:]]` instead.
- POSIX ERE has no lookahead / lookbehind (`(?=...)`, `(?<=...)`).
- POSIX ERE has no non-capturing-group syntax (`(?:...)`). Plain
  groups `(...)` work but capture (we discard captures via
  `REG_NOSUB` anyway, so this is purely a syntax restriction).

Schemas that use only ASCII character classes and simple
quantifiers (`?`, `*`, `+`, `{n}`, `{n,m}`), anchors (`^`, `$`),
and `[]` classes will behave identically under Ajv and cMCP. The
TS SDK's `zod-to-json-schema` defaults emit ASCII-class patterns.

If you need ECMAScript-flavour regex semantics (lookahead, `\d`,
etc.), pin them in the schema using POSIX equivalents OR move the
constraint into the handler.

### Number-vs-integer distinction

cMCP keeps integer and floating-point literals as separate JSON
types internally (`CMCP_JSON_INT` vs `CMCP_JSON_DOUBLE`). The
literal `1` is an `integer`, but the literal `1.0` is **not** —
it matches `"number"` only. This matches Ajv's behaviour when the
JSON parser preserves the lexical form.

If you want to accept either form for the same field, use
`"type": ["integer", "number"]` or just `"type": "number"`.

## Not yet implemented (deferred post-6.7)

These keywords parse without error (schemas containing them remain
valid) but are not enforced. Each lands in a follow-up commit when
needed:

| Keyword | Why deferred |
|---|---|
| `$ref` / `$defs` / `definitions` | Requires a bounded ref resolver. No in-tree consumer needs it yet. |
| `format` (`date-time`, `email`, `uri`, `uuid`, …) | Each format value needs a separate validator; landing as a batch in a follow-up. |
| `dependentRequired` / `dependentSchemas` / `dependencies` (draft-07) | Rarely used; deferred. |
| `contains` / `minContains` / `maxContains` | Pending demand. |
| `unevaluatedProperties` / `unevaluatedItems` | Requires evaluation tracking through combinators — significant complexity for marginal benefit. |

A schema that uses these keywords still validates successfully against
the rest of the schema; cMCP just doesn't enforce them. This is the
same forward-compatible posture the validator has carried since v0.1.

## Cross-check methodology

Tier 6 axis 6.7.3 (post-6.7.2) wires a `make schema-conformance`
target that runs cMCP's validator and Ajv side-by-side over a corpus
of (schema, value, expected) triples and asserts agreement on every
pair. The corpus draws from:

- The supported-keyword test cases in `tests/test_schema.c`.
- A subset of the JSON Schema test suite's draft-2020-12 fixtures.
- Schemas extracted from `@modelcontextprotocol/server-everything`
  v2025.11.x.

Disagreements (where they exist) are documented above as deliberate
departures. The harness is the gate that flags new disagreements on
future schema changes.

## Error shape

A validation failure populates `cmcp_schema_error_t` with three
fields:

```c
typedef struct {
    char *path;     /* RFC 6901 JSON Pointer to the offending value */
    char *keyword;  /* "type", "required", "oneOf", ...               */
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

### Combinator error semantics

For `anyOf` / `oneOf` / `not`, the validator surfaces the
**combinator keyword itself** as the failure (e.g. `"keyword":
"anyOf"`), not the inner failure of one of the branches. The
intuition: when none of the disjuncts match, no single branch is
"the" reason — only the disjunction failed. For `allOf`, the first
failing branch's inner error surfaces verbatim (it IS "the" reason).

## Where validation runs

Validation is invoked by `src/server.c` immediately before a tool
handler is dispatched. Tools registered with `input_schema = NULL`
opt out — the handler receives whatever `arguments` the client sent,
unchecked. (This is convenient for tools whose argument shape can't
be expressed in this subset; most tools should declare a schema.)
