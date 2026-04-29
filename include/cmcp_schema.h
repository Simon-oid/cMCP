#ifndef CMCP_SCHEMA_H
#define CMCP_SCHEMA_H

#include "cmcp_json.h"

/* ====================================================================== */
/* JSON Schema validator — strict subset                                   */
/* ====================================================================== */
/* Validates a `cmcp_json_t` value against a `cmcp_json_t` schema. The
 * schema is itself a JSON document; it is parsed at tool-registration
 * time and kept as a parsed tree so validation is allocation-light.
 *
 * The supported subset for v0.1 is documented in
 * `docs/schema-subset.md`. Quick reference:
 *
 *   type                    string OR array-of-strings:
 *                           "string", "number", "integer", "boolean",
 *                           "array", "object", "null"
 *   properties              { name: <subschema>, ... }
 *   required                [ "name", ... ]
 *   enum                    [ value, ... ]      (deep-equal compare)
 *   minLength / maxLength   on strings (counts Unicode code points)
 *   minimum / maximum       on numbers (inclusive)
 *   items                   single subschema applied to every element
 *   additionalProperties    only `false` is meaningful in v0.1
 *
 * Anything else in the schema (e.g. $ref, oneOf, format, description,
 * pattern, …) is silently ignored; this is forwards-compatible — a
 * stricter validator can be wired in later without breaking callers. */

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
