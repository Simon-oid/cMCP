/**
 * @file cmcp_schema.h
 * @brief JSON Schema validator for tool inputs.
 *
 * cMCP's `tools/call` handler validates the incoming `arguments`
 * object against the tool's declared `inputSchema` before dispatch.
 * Validator surface near-parity with Ajv (the JSON Schema
 * implementation the TypeScript MCP SDK uses); see
 * `docs/schema-conformance.md` for the full keyword list and the
 * documented deliberate departures (regex flavour, integer-vs-number
 * distinction). Failures surface to the peer as JSON-RPC error
 * -32602 with structured `{path, keyword, message}` data.
 */
#ifndef CMCP_SCHEMA_H
#define CMCP_SCHEMA_H

#include "cmcp_json.h"

/* ====================================================================== */
/* JSON Schema validator                                                   */
/* ====================================================================== */
/* Validates a `cmcp_json_t` value against a `cmcp_json_t` schema. The
 * schema is itself a JSON document; it is parsed at tool-registration
 * time and kept as a parsed tree so validation is allocation-light.
 *
 * Supported keywords (full list + semantics in docs/schema-conformance.md):
 *
 *   type, enum, const
 *   minLength / maxLength / pattern              on strings
 *   minimum / maximum / exclusive{Min,Max}imum   on numbers
 *   multipleOf                                   on numbers
 *   minItems / maxItems / uniqueItems            on arrays
 *   items (single + tuple), prefixItems          on arrays
 *   additionalItems                              on arrays
 *   properties, required                         on objects
 *   patternProperties, additionalProperties      on objects
 *   propertyNames                                on objects
 *   minProperties / maxProperties                on objects
 *   allOf / anyOf / oneOf / not                  combinators
 *   if / then / else                             conditional
 *   true / false                                 boolean schemas
 *
 * Not yet implemented (silently accepted; landing post-6.7):
 *   $ref / $defs / definitions, format, dependentRequired/Schemas,
 *   contains, unevaluatedProperties/Items. See docs/schema-conformance.md
 *   for the deferred list and rationale. */

typedef struct {
    /* JSON Pointer (RFC 6901) into the offending value. "" = root. */
    char *path;
    /* Schema keyword that rejected the value, e.g. "type", "required". */
    char *keyword;
    /* Human-readable reason. */
    char *message;
} cmcp_schema_error_t;

void cmcp_schema_error_init(cmcp_schema_error_t *e);
void cmcp_schema_error_clear(cmcp_schema_error_t *e);

/* Validate `value` against `schema`.
 *
 *   schema  must be a non-NULL JSON object. If it isn't, returns
 *           CMCP_EINVAL.
 *   value   the value to check. May be NULL — treated as a JSON null.
 *   err     OUT, optional. If non-NULL it is initialised by this call;
 *           on validation failure it is populated with the path,
 *           offending keyword, and a message. Caller frees with
 *           cmcp_schema_error_clear().
 *
 * Returns:
 *   CMCP_OK      value matches the schema
 *   CMCP_ESCHEMA value violates the schema (err populated if requested)
 *   CMCP_EINVAL  schema itself is malformed (NULL or not an object)
 *   CMCP_ENOMEM  allocation failed during validation
 */
int cmcp_schema_validate(const cmcp_json_t *schema,
                          const cmcp_json_t *value,
                          cmcp_schema_error_t *err);

/* Convert a schema error to a JSON object suitable for the `data`
 * field of a JSON-RPC -32602 INVALID_PARAMS response. The shape is:
 *
 *   { "path": "/foo/3", "keyword": "type", "message": "..." }
 *
 * Caller owns the result. Returns NULL on allocation failure or if
 * `e` has no populated fields. */
cmcp_json_t *cmcp_schema_error_to_json(const cmcp_schema_error_t *e);

#endif
