# JSON Schema audit — gap vs Ajv (TS SDK reference)

Working document for Tier 6 axis 6.7. Enumerates every JSON Schema
keyword Ajv (the JSON Schema implementation the MCP TypeScript SDK
uses) recognises, against cMCP's current validator in `src/schema.c`.
Used to scope the 6.7.2 implementation pass and the 6.7.3 cross-check
corpus. Once 6.7 closes, this file is superseded by
`docs/schema-conformance.md` (the "what cMCP supports" reference) and
remains here as a record of how the gap was measured.

## Reference draft

Ajv defaults to **JSON Schema draft 2020-12** for new schemas; it also
supports draft-07 in compatibility mode. MCP's `inputSchema` field is
not pinned to a specific draft revision; in practice the TS SDK emits
draft-07-compatible shapes via `zod-to-json-schema`. cMCP targets
draft-2020-12 semantics with draft-07 wire compatibility — meaning we
implement the 2020-12 keyword names (`prefixItems`, `$defs`) and
accept the draft-07 aliases (`items` as tuple, `definitions`) without
emitting our own.

## Keyword inventory

Legend:
- ✅ implemented before 6.7 (pre-existing)
- 🟢 implemented in 6.7
- 🟡 implemented in 6.7, partial / documented departure
- ⛔ deliberately not implemented in 6.x — rationale in `schema-conformance.md`
- ◻️ deferred to a follow-up (post-6.7)

### Validation keywords for any instance type

| Keyword | Status | Notes |
|---|---|---|
| `type` | ✅ | string OR array-of-strings, all seven canonical tokens. |
| `enum` | ✅ | deep-equality compare. |
| `const` | 🟢 | added 6.7. |
| `$ref` | ◻️ | post-6.7. Requires a bounded ref resolver. |
| `$defs` | ◻️ | paired with `$ref`. |
| `definitions` (draft-07 alias) | ◻️ | post-6.7. |
| `$id` | ⛔ | cMCP validates at tool-registration time; ids serve composition use cases we don't expose. |
| `$schema` | ⛔ | ignored. Single-draft validator. |
| `$comment` | ⛔ | ignored (it's a comment). |

### Validation keywords for numeric instances

| Keyword | Status | Notes |
|---|---|---|
| `minimum` | ✅ | inclusive. |
| `maximum` | ✅ | inclusive. |
| `exclusiveMinimum` | 🟢 | added 6.7. Draft-2020-12 form (number). |
| `exclusiveMaximum` | 🟢 | added 6.7. Draft-2020-12 form (number). |
| `multipleOf` | 🟢 | added 6.7. Uses `fmod` on doubles + integer fast-path. |

### Validation keywords for strings

| Keyword | Status | Notes |
|---|---|---|
| `minLength` | ✅ | Unicode code points. |
| `maxLength` | ✅ | Unicode code points. |
| `pattern` | 🟢 | added 6.7. POSIX ERE via `<regex.h>` — see schema-conformance.md for the regex flavour notes. |
| `format` | ◻️ | post-6.7. Plan covers `date-time`, `email`, `uri`. |

### Validation keywords for arrays

| Keyword | Status | Notes |
|---|---|---|
| `items` (single subschema) | ✅ | applied to every element. |
| `items` (tuple, draft-07) | 🟢 | added 6.7 — array of subschemas, applied positionally. |
| `prefixItems` (draft-2020-12) | 🟢 | added 6.7. Same semantics as draft-07 tuple `items`. |
| `additionalItems` (draft-07) | 🟢 | added 6.7. Single subschema OR boolean. |
| `unevaluatedItems` | ⛔ | requires evaluation tracking through allOf/anyOf/etc. Out of scope until we'd actually use it. |
| `minItems` | 🟢 | added 6.7. |
| `maxItems` | 🟢 | added 6.7. |
| `uniqueItems` | 🟢 | added 6.7. Deep-equality. |
| `contains` | ◻️ | post-6.7 — needs `minContains`/`maxContains` for full semantics. |
| `minContains` | ◻️ | paired with `contains`. |
| `maxContains` | ◻️ | paired with `contains`. |

### Validation keywords for objects

| Keyword | Status | Notes |
|---|---|---|
| `properties` | ✅ | recurses into each named property. |
| `required` | ✅ | array of property names. |
| `additionalProperties: false` | ✅ | reject any unlisted property. |
| `additionalProperties` (subschema) | 🟢 | added 6.7. Apply subschema to properties not covered by `properties` or `patternProperties`. |
| `patternProperties` | 🟢 | added 6.7. POSIX ERE on each pattern key. |
| `propertyNames` | 🟢 | added 6.7. Subschema applied to each property name (treated as a string). |
| `minProperties` | 🟢 | added 6.7. |
| `maxProperties` | 🟢 | added 6.7. |
| `unevaluatedProperties` | ⛔ | same rationale as `unevaluatedItems`. |
| `dependentRequired` | ◻️ | post-6.7. |
| `dependentSchemas` | ◻️ | post-6.7. |
| `dependencies` (draft-07) | ◻️ | folded into 6.7 follow-up alongside `dependentRequired`/`dependentSchemas`. |

### Boolean schema combinators

| Keyword | Status | Notes |
|---|---|---|
| `oneOf` | 🟢 | added 6.7. Exactly-one semantics. |
| `anyOf` | 🟢 | added 6.7. At-least-one. Short-circuits on first match. |
| `allOf` | 🟢 | added 6.7. Each subschema must validate. |
| `not` | 🟢 | added 6.7. Subschema must NOT validate. |
| `if` / `then` / `else` | 🟢 | added 6.7. Conditional. `if` runs in "schema-only" mode (its failure is silent — only used to choose which branch). |

### Schema annotations (non-validating)

These are documentation hints — Ajv ignores them at validation time.
cMCP follows suit; they are not in scope for 6.7.

| Keyword | Status | Notes |
|---|---|---|
| `title`, `description` | ⛔ | doc-only, surfaced to peers via `tools/list`. |
| `default`, `examples` | ⛔ | doc-only. |
| `readOnly`, `writeOnly`, `deprecated` | ⛔ | doc-only. |

### Boolean schemas

| Schema | Status | Notes |
|---|---|---|
| `true` (always accept) | 🟢 | added 6.7. |
| `false` (always reject) | 🟢 | added 6.7. |
| `{}` (empty object → always accept) | ✅ | falls out of the existing validator. |

## Methodology

Keyword list derived from:

- [JSON Schema draft 2020-12 vocabulary](https://json-schema.org/draft/2020-12/json-schema-validation)
  (Validation and Applicator vocabularies — annotation keywords listed
  separately above).
- Ajv's [keyword reference](https://ajv.js.org/json-schema.html) for
  draft-07 and draft-2020-12.
- Manual inspection of the schemas declared by
  `@modelcontextprotocol/server-everything` v2025.11.x and the schemas
  emitted by `zod-to-json-schema` in the same install (their dist
  bundles under `conformance/node_modules/`).

## Deferred to a follow-up (post-6.7)

- `$ref` / `$defs` / `definitions` + bounded resolver.
- `format` for `date-time`, `email`, `uri`, `uuid`.
- `dependentRequired` / `dependentSchemas` / `dependencies`.
- `contains` / `minContains` / `maxContains`.
- Ajv cross-check harness (6.7.3) — `make schema-conformance`
  exercises a corpus of ≥500 (schema, value, expected) tuples
  comparing cMCP and Ajv outcomes.

Once those land, this file is fully superseded by `schema-conformance.md`.
