/* schema_walk.h — internal traversal of every subschema of a JSON Schema.
 *
 * Not a public header: it lives in src/ and is linked into the core.
 *
 * Two checks need to visit every subschema: cmcp_schema_check (is the
 * schema well formed and within bounds?) and the x-mcp-header lint (is
 * each annotation on a statically reachable primitive property?). One
 * walker keeps the list of subschema-bearing keywords in one place. It
 * enforces the resource bounds the spec asks for (basic/index,
 * "Composition-Keyword Resource Use"): depth and total subschemas. */
#ifndef CMCP_SCHEMA_WALK_H
#define CMCP_SCHEMA_WALK_H

#include "cmcp_json.h"
#include "cmcp_schema.h"

#define CMCP_SCHEMA_WALK_MAX_DEPTH 64
#define CMCP_SCHEMA_WALK_MAX_NODES 10000

/* Called for every subschema, parents first. `path` is the JSON Pointer
 * of the node inside the root schema. `reachable` is non-zero when the
 * node is reached from the root through `properties` keys only (the
 * x-mcp-header "statically reachable" rule). Return CMCP_OK to go on;
 * anything else stops the walk and is returned by cmcp_schema_walk. */
typedef int (*cmcp_schema_visit_fn)(const cmcp_json_t *node, const char *path,
                                    int reachable, void *ctx);

/* Walk `root`. Returns the first non-OK visitor result, or CMCP_EINVAL
 * with *err filled when the schema is deeper or larger than the bounds
 * (err may be NULL). */
int cmcp_schema_walk(const cmcp_json_t *root, cmcp_schema_visit_fn visit,
                     void *ctx, cmcp_schema_error_t *err);

/* Fill *err (if non-NULL) and return rc. For visitors. */
int cmcp_schema_walk_fail(cmcp_schema_error_t *err, int rc, const char *path,
                          const char *keyword, const char *fmt, ...);

#endif
