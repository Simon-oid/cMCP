/* schema_format.h — internal: the JSON Schema `format` checks.
 *
 * Not a public header: it lives in src/ and is linked into the core. */
#ifndef CMCP_SCHEMA_FORMAT_H
#define CMCP_SCHEMA_FORMAT_H

/* Non-zero when string `s` matches `format` (date-time, email, uri,
 * uuid), or when `format` is one cMCP does not check. */
int cmcp_schema_format_ok(const char *format, const char *s);

#endif
