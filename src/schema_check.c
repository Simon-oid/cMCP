/* schema_check.c — registration-time checks on a JSON Schema, and the
 * shared subschema walker (see schema_walk.h).
 *
 * cmcp_schema_validate checks a value against a schema at call time and
 * is lenient about the schema itself. A tool's schema is fixed when the
 * tool is registered, so that is the moment to reject one that cMCP
 * would otherwise validate wrongly: an unsupported dialect, a $ref that
 * points outside the document or nowhere, a keyword cMCP does not
 * implement, malformed keyword values, or a size that could turn the
 * validator into a denial-of-service vector. */
#include "schema_walk.h"

#include "cmcp.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define PATH_CAP 512

/* ---------------------------------------------------------------- */
/* Walker                                                            */
/* ---------------------------------------------------------------- */

typedef struct {
    cmcp_schema_visit_fn visit;
    void                *ctx;
    cmcp_schema_error_t *err;
    int                  nodes;
} walk_t;

static char *dup_str(const char *s) {
    size_t n = strlen(s);
    char *d = (char *)malloc(n + 1);
    if (d) memcpy(d, s, n + 1);
    return d;
}

int cmcp_schema_walk_fail(cmcp_schema_error_t *err, int rc, const char *path,
                          const char *keyword, const char *fmt, ...) {
    if (!err) return rc;
    char msg[256];
    va_list ap;
    va_start(ap, fmt);
    /* ap is initialised by va_start directly above; the analyzer loses
     * track of it through the variadic boundary (false positive, same
     * as fail() in schema.c).
     * NOLINTNEXTLINE(clang-analyzer-valist.Uninitialized) */
    vsnprintf(msg, sizeof msg, fmt, ap);
    va_end(ap);
    cmcp_schema_error_clear(err);
    err->path = dup_str(path);
    err->keyword = dup_str(keyword);
    err->message = dup_str(msg);
    return rc;
}

/* Child pointer: parent + "/" + key, with RFC 6901 escapes. */
static void child_path(char *out, const char *parent, const char *key) {
    size_t n = (size_t)snprintf(out, PATH_CAP, "%s/", parent);
    if (n >= PATH_CAP) n = PATH_CAP - 1;
    for (const char *c = key; *c && n + 3 < PATH_CAP; c++) {
        if (*c == '~')      { out[n++] = '~'; out[n++] = '0'; }
        else if (*c == '/') { out[n++] = '~'; out[n++] = '1'; }
        else                  out[n++] = *c;
    }
    out[n] = '\0';
}

static int walk_node(walk_t *w, const cmcp_json_t *node, const char *path,
                     int reachable, int depth);

static int walk_child(walk_t *w, const cmcp_json_t *child, const char *parent,
                      const char *k1, const char *k2, int reachable, int depth) {
    char p1[PATH_CAP], p2[PATH_CAP];
    child_path(p1, parent, k1);
    if (k2) child_path(p2, p1, k2);
    return walk_node(w, child, k2 ? p2 : p1, reachable, depth + 1);
}

/* Keywords whose value is one subschema. */
static const char *const SINGLE[] = {
    "additionalProperties", "propertyNames", "additionalItems", "contains",
    "not", "if", "then", "else", "unevaluatedProperties", "unevaluatedItems",
};
/* Keywords whose value is an object of subschemas. */
static const char *const MAPS[] = {
    "properties", "patternProperties", "$defs", "definitions",
    "dependentSchemas", "dependencies",
};
/* Keywords whose value is an array of subschemas. */
static const char *const ARRAYS[] = {"allOf", "anyOf", "oneOf", "prefixItems"};

#define COUNT(a) (sizeof(a) / sizeof((a)[0]))

static int is_schema(const cmcp_json_t *v) {
    return v->type == CMCP_JSON_OBJECT || v->type == CMCP_JSON_BOOL;
}

static int walk_maps(walk_t *w, const cmcp_json_t *node, const char *path,
                     int reachable, int depth) {
    for (size_t k = 0; k < COUNT(MAPS); k++) {
        const cmcp_json_t *m = cmcp_json_object_get(node, MAPS[k]);
        if (!m || m->type != CMCP_JSON_OBJECT) continue;
        /* Only a `properties` step keeps static reachability. */
        int child_reach = reachable && k == 0;
        for (size_t i = 0; i < m->obj.len; i++) {
            const cmcp_json_t *v = m->obj.values[i];
            /* draft-07 `dependencies` also maps to name arrays; any other
             * non-schema value is visited, so the check reports it. */
            if (v->type == CMCP_JSON_ARRAY &&
                strcmp(MAPS[k], "dependencies") == 0)
                continue;
            int rc = walk_child(w, v, path, MAPS[k], m->obj.keys[i],
                                child_reach, depth);
            if (rc != CMCP_OK) return rc;
        }
    }
    return CMCP_OK;
}

/* allOf / anyOf / oneOf / prefixItems, and tuple-form (draft-07) items. */
static int walk_array_kw(walk_t *w, const cmcp_json_t *a, const char *path,
                         const char *kw, int depth) {
    if (!a || a->type != CMCP_JSON_ARRAY) return CMCP_OK;
    for (size_t i = 0; i < a->arr.len; i++) {
        if (!is_schema(a->arr.items[i])) continue;
        char idx[24];
        snprintf(idx, sizeof idx, "%zu", i);
        int rc = walk_child(w, a->arr.items[i], path, kw, idx, 0, depth);
        if (rc != CMCP_OK) return rc;
    }
    return CMCP_OK;
}

static int walk_node(walk_t *w, const cmcp_json_t *node, const char *path,
                     int reachable, int depth) {
    if (depth > CMCP_SCHEMA_WALK_MAX_DEPTH)
        return cmcp_schema_walk_fail(w->err, CMCP_EINVAL, path, "depth",
                                     "schema nests deeper than %d levels",
                                     CMCP_SCHEMA_WALK_MAX_DEPTH);
    if (++w->nodes > CMCP_SCHEMA_WALK_MAX_NODES)
        return cmcp_schema_walk_fail(w->err, CMCP_EINVAL, path, "size",
                                     "schema has more than %d subschemas",
                                     CMCP_SCHEMA_WALK_MAX_NODES);
    int rc = w->visit(node, path, reachable, w->ctx);
    if (rc != CMCP_OK || node->type != CMCP_JSON_OBJECT) return rc;

    for (size_t k = 0; k < COUNT(SINGLE); k++) {
        const cmcp_json_t *v = cmcp_json_object_get(node, SINGLE[k]);
        if (v && is_schema(v) &&
            (rc = walk_child(w, v, path, SINGLE[k], NULL, 0, depth)) != CMCP_OK)
            return rc;
    }
    const cmcp_json_t *items = cmcp_json_object_get(node, "items");
    if (items && is_schema(items) &&
        (rc = walk_child(w, items, path, "items", NULL, 0, depth)) != CMCP_OK)
        return rc;
    if ((rc = walk_maps(w, node, path, reachable, depth)) != CMCP_OK) return rc;
    for (size_t k = 0; k < COUNT(ARRAYS); k++) {
        rc = walk_array_kw(w, cmcp_json_object_get(node, ARRAYS[k]), path,
                           ARRAYS[k], depth);
        if (rc != CMCP_OK) return rc;
    }
    return walk_array_kw(w, items, path, "items", depth);
}

int cmcp_schema_walk(const cmcp_json_t *root, cmcp_schema_visit_fn visit,
                     void *ctx, cmcp_schema_error_t *err) {
    if (!root || !visit) return CMCP_EINVAL;
    walk_t w = {visit, ctx, err, 0};
    return walk_node(&w, root, "", 1, 0);
}

/* ---------------------------------------------------------------- */
/* cmcp_schema_check                                                 */
/* ---------------------------------------------------------------- */

typedef struct {
    const cmcp_json_t   *root;
    cmcp_schema_error_t *err;
} check_t;

static const char *const TYPE_NAMES[] = {
    "null", "boolean", "object", "array", "number", "integer", "string",
};

static int is_type_name(const cmcp_json_t *v) {
    if (v->type != CMCP_JSON_STRING) return 0;
    for (size_t i = 0; i < COUNT(TYPE_NAMES); i++)
        if (strcmp(v->str.s, TYPE_NAMES[i]) == 0) return 1;
    return 0;
}

static int check_type_kw(const cmcp_json_t *t) {
    if (is_type_name(t)) return 1;
    if (t->type != CMCP_JSON_ARRAY || t->arr.len == 0) return 0;
    for (size_t i = 0; i < t->arr.len; i++)
        if (!is_type_name(t->arr.items[i])) return 0;
    return 1;
}

static int all_strings(const cmcp_json_t *a) {
    if (a->type != CMCP_JSON_ARRAY) return 0;
    for (size_t i = 0; i < a->arr.len; i++)
        if (a->arr.items[i]->type != CMCP_JSON_STRING) return 0;
    return 1;
}

/* Keywords cMCP does not implement: validating without them would be
 * more permissive than the schema's author intended. */
static const char *const UNSUPPORTED[] = {
    "unevaluatedProperties", "unevaluatedItems", "$dynamicRef",
    "$recursiveRef",
};

static const char *const COUNTS[] = {
    "minLength", "maxLength", "minItems", "maxItems", "minProperties",
    "maxProperties", "minContains", "maxContains",
};

static int check_ref(check_t *c, const cmcp_json_t *ref, const char *path) {
    if (ref->type != CMCP_JSON_STRING)
        return cmcp_schema_walk_fail(c->err, CMCP_EINVAL, path, "$ref",
                                     "$ref must be a string");
    /* Never dereference a network or file URI (spec: $ref Resolution);
     * reject instead of validating as if the reference were absent. */
    if (ref->str.s[0] != '#')
        return cmcp_schema_walk_fail(c->err, CMCP_EUNSUPPORTED, path, "$ref",
                                     "external $ref '%s' is not supported",
                                     ref->str.s);
    if (!cmcp_schema_resolve_pointer(c->root, ref->str.s))
        return cmcp_schema_walk_fail(c->err, CMCP_EINVAL, path, "$ref",
                                     "$ref '%s' does not resolve", ref->str.s);
    return CMCP_OK;
}

static int check_lists(check_t *c, const cmcp_json_t *n, const char *path) {
    for (size_t i = 0; i < COUNT(ARRAYS); i++) {
        const cmcp_json_t *v = cmcp_json_object_get(n, ARRAYS[i]);
        if (v && (v->type != CMCP_JSON_ARRAY || v->arr.len == 0))
            return cmcp_schema_walk_fail(c->err, CMCP_EINVAL, path, ARRAYS[i],
                                         "%s must be a non-empty array",
                                         ARRAYS[i]);
    }
    for (size_t i = 0; i < COUNT(MAPS); i++) {
        const cmcp_json_t *v = cmcp_json_object_get(n, MAPS[i]);
        if (v && v->type != CMCP_JSON_OBJECT)
            return cmcp_schema_walk_fail(c->err, CMCP_EINVAL, path, MAPS[i],
                                         "%s must be an object", MAPS[i]);
    }
    for (size_t i = 0; i < COUNT(COUNTS); i++) {
        const cmcp_json_t *v = cmcp_json_object_get(n, COUNTS[i]);
        if (v && (v->type != CMCP_JSON_INT || v->i < 0))
            return cmcp_schema_walk_fail(c->err, CMCP_EINVAL, path, COUNTS[i],
                                         "%s must be a non-negative integer",
                                         COUNTS[i]);
    }
    return CMCP_OK;
}

static int check_shapes(check_t *c, const cmcp_json_t *n, const char *path) {
    const cmcp_json_t *v = cmcp_json_object_get(n, "type");
    if (v && !check_type_kw(v))
        return cmcp_schema_walk_fail(c->err, CMCP_EINVAL, path, "type",
                                     "type must be a type name or a non-empty "
                                     "array of type names");
    v = cmcp_json_object_get(n, "required");
    if (v && !all_strings(v))
        return cmcp_schema_walk_fail(c->err, CMCP_EINVAL, path, "required",
                                     "required must be an array of strings");
    v = cmcp_json_object_get(n, "enum");
    if (v && v->type != CMCP_JSON_ARRAY)
        return cmcp_schema_walk_fail(c->err, CMCP_EINVAL, path, "enum",
                                     "enum must be an array");
    return check_lists(c, n, path);
}

static int check_visit(const cmcp_json_t *node, const char *path,
                       int reachable, void *ctx) {
    (void)reachable;
    check_t *c = (check_t *)ctx;
    if (node->type == CMCP_JSON_BOOL) return CMCP_OK;
    if (node->type != CMCP_JSON_OBJECT)
        return cmcp_schema_walk_fail(c->err, CMCP_EINVAL, path, "schema",
                                     "a schema must be an object or a boolean");
    for (size_t i = 0; i < COUNT(UNSUPPORTED); i++) {
        if (cmcp_json_object_get(node, UNSUPPORTED[i]))
            return cmcp_schema_walk_fail(c->err, CMCP_EUNSUPPORTED, path,
                                         UNSUPPORTED[i],
                                         "%s is not supported by cMCP",
                                         UNSUPPORTED[i]);
    }
    const cmcp_json_t *ref = cmcp_json_object_get(node, "$ref");
    int rc = ref ? check_ref(c, ref, path) : CMCP_OK;
    return rc != CMCP_OK ? rc : check_shapes(c, node, path);
}

int cmcp_schema_check(const cmcp_json_t *schema, cmcp_schema_error_t *err) {
    if (err) cmcp_schema_error_init(err);
    if (!schema || (schema->type != CMCP_JSON_OBJECT &&
                    schema->type != CMCP_JSON_BOOL))
        return cmcp_schema_walk_fail(err, CMCP_EINVAL, "", "schema",
                                     "a schema must be an object or a boolean");
    if (cmcp_schema_dialect(schema) < 0)
        return cmcp_schema_walk_fail(err, CMCP_EUNSUPPORTED, "", "$schema",
                                     "unsupported dialect (cMCP supports "
                                     "2020-12 and draft-07)");
    check_t c = {schema, err};
    return cmcp_schema_walk(schema, check_visit, &c, err);
}
