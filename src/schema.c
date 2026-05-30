#include "cmcp.h"
#include "cmcp_schema.h"
#include "cmcp_json.h"

#include <ctype.h>
#include <regex.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ====================================================================== */
/* Validation context                                                       */
/* ====================================================================== */
/* Threaded through every validate() call so $ref can resolve against
 * the root schema and so deep recursion (real or attacker-induced via
 * a ref cycle) can be bounded. The depth bound is the same defense the
 * JSON parser applies — see CMCP_JSON_MAX_DEPTH in CLAUDE.md. */

typedef struct {
    const cmcp_json_t *root;      /* root schema (for `#/$defs/x` paths) */
    int                depth;     /* current validate() recursion depth */
    int                max_depth; /* abort with -32602-shape error past this */
} validate_ctx_t;

#define CMCP_SCHEMA_MAX_DEPTH 64

/* ====================================================================== */
/* Path buffer (RFC 6901 JSON Pointer)                                     */
/* ====================================================================== */

typedef struct {
    char  *data;
    size_t len;
    size_t cap;
} path_buf_t;

static int path_init(path_buf_t *p) {
    p->cap = 64;
    p->data = (char *)malloc(p->cap);
    if (!p->data) return -1;
    p->data[0] = '\0';
    p->len = 0;
    return 0;
}

static int path_grow(path_buf_t *p, size_t need) {
    if (p->cap >= need) return 0;
    size_t nc = p->cap;
    while (nc < need) nc *= 2;
    char *nd = (char *)realloc(p->data, nc);
    if (!nd) return -1;
    p->data = nd;
    p->cap  = nc;
    return 0;
}

static int path_append(path_buf_t *p, const char *s, size_t n) {
    if (path_grow(p, p->len + n + 1) < 0) return -1;
    memcpy(p->data + p->len, s, n);
    p->len += n;
    p->data[p->len] = '\0';
    return 0;
}

/* Push "/<escaped name>". Returns the previous length so callers can
 * pop back to it after recursing. */
static size_t path_push_prop(path_buf_t *p, const char *name) {
    size_t saved = p->len;
    path_append(p, "/", 1);
    for (const char *c = name; *c; c++) {
        if (*c == '~')      path_append(p, "~0", 2);
        else if (*c == '/') path_append(p, "~1", 2);
        else                path_append(p, c, 1);
    }
    return saved;
}

static size_t path_push_idx(path_buf_t *p, size_t idx) {
    size_t saved = p->len;
    char tmp[32];
    int n = snprintf(tmp, sizeof tmp, "/%zu", idx);
    if (n > 0) path_append(p, tmp, (size_t)n);
    return saved;
}

static void path_pop(path_buf_t *p, size_t saved) {
    p->len = saved;
    p->data[p->len] = '\0';
}

/* ====================================================================== */
/* UTF-8 code-point counter                                                */
/* ====================================================================== */

/* Counts Unicode code points in a UTF-8 buffer. Treats malformed bytes
 * as one code point each — the JSON parser already rejects invalid
 * UTF-8, so this branch should never fire on accepted input. */
static size_t utf8_cp_count(const char *s, size_t n) {
    size_t count = 0;
    for (size_t i = 0; i < n; ) {
        unsigned char c = (unsigned char)s[i];
        size_t step;
        /* The ASCII branch and the malformed-leading-byte fallback both
         * advance by one byte but represent different things: a valid
         * codepoint vs. an invalid one we recover from by skipping. The
         * shared step is the intent, not duplication.
         * NOLINTBEGIN(bugprone-branch-clone) */
        if      (c < 0x80)        step = 1;
        else if ((c & 0xE0) == 0xC0) step = 2;
        else if ((c & 0xF0) == 0xE0) step = 3;
        else if ((c & 0xF8) == 0xF0) step = 4;
        else                          step = 1;
        /* NOLINTEND(bugprone-branch-clone) */
        if (i + step > n) step = n - i;
        i += step;
        count++;
    }
    return count;
}

/* ====================================================================== */
/* Type matching                                                           */
/* ====================================================================== */

/* INT only matches "integer"; both INT and DOUBLE match "number".
 * (1.0 is a number, not an integer, in this validator. The JSON parser
 * preserves the lexical form, so 1 and 1.0 are distinguishable here.) */
static int type_matches(const char *type, const cmcp_json_t *v) {
    cmcp_json_type_t t = v->type;
    if (strcmp(type, "string") == 0)  return t == CMCP_JSON_STRING;
    if (strcmp(type, "boolean") == 0) return t == CMCP_JSON_BOOL;
    if (strcmp(type, "array") == 0)   return t == CMCP_JSON_ARRAY;
    if (strcmp(type, "object") == 0)  return t == CMCP_JSON_OBJECT;
    if (strcmp(type, "null") == 0)    return t == CMCP_JSON_NULL;
    if (strcmp(type, "integer") == 0) return t == CMCP_JSON_INT;
    if (strcmp(type, "number") == 0)
        return t == CMCP_JSON_INT || t == CMCP_JSON_DOUBLE;
    return 0;
}

/* ====================================================================== */
/* Error reporting                                                         */
/* ====================================================================== */

void cmcp_schema_error_init(cmcp_schema_error_t *e) {
    if (!e) return;
    e->path = NULL;
    e->keyword = NULL;
    e->message = NULL;
}

void cmcp_schema_error_clear(cmcp_schema_error_t *e) {
    if (!e) return;
    free(e->path);
    free(e->keyword);
    free(e->message);
    e->path = e->keyword = e->message = NULL;
}

static char *xstrdup(const char *s) {
    if (!s) return NULL;
    size_t n = strlen(s);
    char *o = (char *)malloc(n + 1);
    if (!o) return NULL;
    memcpy(o, s, n + 1);
    return o;
}

/* Populate err and return CMCP_ESCHEMA. err may be NULL — in which
 * case we skip allocations. The fmt-string takes printf args. */
static int fail(cmcp_schema_error_t *err, const path_buf_t *p,
                const char *keyword, const char *fmt, ...) {
    if (!err) return CMCP_ESCHEMA;
    /* Don't overwrite an already-populated error. Combinator code
     * walks subschemas speculatively (anyOf/oneOf) and reuses the
     * same err scratch; only the *first* substantive failure should
     * end up in the surfaced error. */
    if (err->keyword) return CMCP_ESCHEMA;
    err->path    = xstrdup(p ? p->data : "");
    err->keyword = xstrdup(keyword);

    char msg[256];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof msg, fmt, ap);
    va_end(ap);
    err->message = xstrdup(msg);
    return CMCP_ESCHEMA;
}

cmcp_json_t *cmcp_schema_error_to_json(const cmcp_schema_error_t *e) {
    if (!e || (!e->path && !e->keyword && !e->message)) return NULL;
    cmcp_json_t *o = cmcp_json_new_object();
    if (!o) return NULL;
    if (e->path)
        cmcp_json_object_set(o, "path",    cmcp_json_new_string(e->path));
    if (e->keyword)
        cmcp_json_object_set(o, "keyword", cmcp_json_new_string(e->keyword));
    if (e->message)
        cmcp_json_object_set(o, "message", cmcp_json_new_string(e->message));
    return o;
}

/* ====================================================================== */
/* Recursive validation                                                    */
/* ====================================================================== */

static int validate(validate_ctx_t *ctx,
                     const cmcp_json_t *schema, const cmcp_json_t *value,
                     path_buf_t *path, cmcp_schema_error_t *err);

/* Speculative validation: same as validate(), but never populates
 * err. Used by combinators that probe N subschemas and only surface
 * the *substantive* failure (or success) at the top level. */
static int validate_quiet(validate_ctx_t *ctx,
                          const cmcp_json_t *schema, const cmcp_json_t *value,
                          path_buf_t *path) {
    return validate(ctx, schema, value, path, NULL);
}

/* `type` keyword: string OR array of strings. */
static int check_type(const cmcp_json_t *schema, const cmcp_json_t *value,
                       path_buf_t *path, cmcp_schema_error_t *err) {
    const cmcp_json_t *type = cmcp_json_object_get(schema, "type");
    if (!type) return CMCP_OK;

    if (type->type == CMCP_JSON_STRING) {
        if (!type_matches(type->str.s, value)) {
            return fail(err, path, "type", "expected type %s", type->str.s);
        }
        return CMCP_OK;
    }
    if (type->type == CMCP_JSON_ARRAY) {
        for (size_t i = 0; i < type->arr.len; i++) {
            const cmcp_json_t *t = type->arr.items[i];
            if (t && t->type == CMCP_JSON_STRING && type_matches(t->str.s, value))
                return CMCP_OK;
        }
        return fail(err, path, "type", "no listed type matches");
    }
    /* Schema author error — silently accept rather than reject the value. */
    return CMCP_OK;
}

static int check_enum(const cmcp_json_t *schema, const cmcp_json_t *value,
                       path_buf_t *path, cmcp_schema_error_t *err) {
    const cmcp_json_t *e = cmcp_json_object_get(schema, "enum");
    if (!e || e->type != CMCP_JSON_ARRAY) return CMCP_OK;
    for (size_t i = 0; i < e->arr.len; i++) {
        if (cmcp_json_equal(e->arr.items[i], value)) return CMCP_OK;
    }
    return fail(err, path, "enum", "value not in enum");
}

static int check_const(const cmcp_json_t *schema, const cmcp_json_t *value,
                        path_buf_t *path, cmcp_schema_error_t *err) {
    const cmcp_json_t *c = cmcp_json_object_get(schema, "const");
    if (!c) return CMCP_OK;
    if (cmcp_json_equal(c, value)) return CMCP_OK;
    return fail(err, path, "const", "value does not match const");
}

/* ====================================================================== */
/* String keywords                                                         */
/* ====================================================================== */

static int check_pattern(const cmcp_json_t *schema, const cmcp_json_t *value,
                          path_buf_t *path, cmcp_schema_error_t *err) {
    if (value->type != CMCP_JSON_STRING) return CMCP_OK;
    const cmcp_json_t *pat = cmcp_json_object_get(schema, "pattern");
    if (!pat || pat->type != CMCP_JSON_STRING) return CMCP_OK;

    /* POSIX ERE — see docs/schema-conformance.md for flavour notes.
     * JSON values may contain embedded NULs; POSIX regex APIs are NUL-
     * terminated. We accept the truncation here because tool-input
     * strings carrying NULs would already be a wire oddity. */
    regex_t re;
    int rc = regcomp(&re, pat->str.s, REG_EXTENDED | REG_NOSUB);
    if (rc != 0) {
        /* Schema author error — invalid regex. Per spec we still
         * validate the rest of the schema; this keyword is dropped. */
        return CMCP_OK;
    }
    int matched = regexec(&re, value->str.s, 0, NULL, 0) == 0;
    regfree(&re);
    if (!matched) {
        return fail(err, path, "pattern", "string does not match pattern");
    }
    return CMCP_OK;
}

static int check_string(const cmcp_json_t *schema, const cmcp_json_t *value,
                         path_buf_t *path, cmcp_schema_error_t *err) {
    if (value->type != CMCP_JSON_STRING) return CMCP_OK;
    const cmcp_json_t *minl = cmcp_json_object_get(schema, "minLength");
    const cmcp_json_t *maxl = cmcp_json_object_get(schema, "maxLength");
    if (minl && minl->type == CMCP_JSON_INT) {
        size_t n = utf8_cp_count(value->str.s, value->str.len);
        if ((long long)n < minl->i) {
            return fail(err, path, "minLength",
                         "string too short (min %lld)", minl->i);
        }
    }
    if (maxl && maxl->type == CMCP_JSON_INT) {
        size_t n = utf8_cp_count(value->str.s, value->str.len);
        if ((long long)n > maxl->i) {
            return fail(err, path, "maxLength",
                         "string too long (max %lld)", maxl->i);
        }
    }
    return check_pattern(schema, value, path, err);
}

/* ====================================================================== */
/* Number keywords                                                         */
/* ====================================================================== */

static double json_num(const cmcp_json_t *v) {
    return v->type == CMCP_JSON_INT ? (double)v->i : v->d;
}

static int check_number(const cmcp_json_t *schema, const cmcp_json_t *value,
                         path_buf_t *path, cmcp_schema_error_t *err) {
    if (value->type != CMCP_JSON_INT && value->type != CMCP_JSON_DOUBLE)
        return CMCP_OK;
    double v = json_num(value);

    const cmcp_json_t *mn  = cmcp_json_object_get(schema, "minimum");
    const cmcp_json_t *mx  = cmcp_json_object_get(schema, "maximum");
    const cmcp_json_t *emn = cmcp_json_object_get(schema, "exclusiveMinimum");
    const cmcp_json_t *emx = cmcp_json_object_get(schema, "exclusiveMaximum");
    const cmcp_json_t *mo  = cmcp_json_object_get(schema, "multipleOf");

    if (mn && (mn->type == CMCP_JSON_INT || mn->type == CMCP_JSON_DOUBLE)) {
        if (v < json_num(mn))
            return fail(err, path, "minimum", "value below minimum");
    }
    if (mx && (mx->type == CMCP_JSON_INT || mx->type == CMCP_JSON_DOUBLE)) {
        if (v > json_num(mx))
            return fail(err, path, "maximum", "value above maximum");
    }
    if (emn && (emn->type == CMCP_JSON_INT || emn->type == CMCP_JSON_DOUBLE)) {
        if (v <= json_num(emn))
            return fail(err, path, "exclusiveMinimum",
                         "value not strictly above exclusiveMinimum");
    }
    if (emx && (emx->type == CMCP_JSON_INT || emx->type == CMCP_JSON_DOUBLE)) {
        if (v >= json_num(emx))
            return fail(err, path, "exclusiveMaximum",
                         "value not strictly below exclusiveMaximum");
    }
    if (mo && (mo->type == CMCP_JSON_INT || mo->type == CMCP_JSON_DOUBLE)) {
        double m = json_num(mo);
        if (m <= 0.0) return CMCP_OK; /* schema author error */
        if (value->type == CMCP_JSON_INT && mo->type == CMCP_JSON_INT
            && mo->i != 0) {
            /* Integer fast-path avoids fp error. */
            if (value->i % mo->i != 0)
                return fail(err, path, "multipleOf",
                             "value is not a multiple of %lld", mo->i);
        } else {
            /* Floating-point modulo, with a tolerance for representation
             * error. Ajv uses a similar epsilon when comparing fmod
             * results to zero. */
            double r = fmod(v, m);
            double eps = 1e-9 * (fabs(v) > 1.0 ? fabs(v) : 1.0);
            if (fabs(r) > eps && fabs(r - m) > eps)
                return fail(err, path, "multipleOf",
                             "value is not a multiple of %g", m);
        }
    }
    return CMCP_OK;
}

/* ====================================================================== */
/* Object keywords                                                         */
/* ====================================================================== */

/* Returns whether the value-side key `key` is covered by `properties`
 * (literal key match) or by any `patternProperties` regex. */
static int key_covered(const cmcp_json_t *schema, const char *key) {
    const cmcp_json_t *props = cmcp_json_object_get(schema, "properties");
    if (props && props->type == CMCP_JSON_OBJECT
        && cmcp_json_object_get(props, key))
        return 1;
    const cmcp_json_t *pp = cmcp_json_object_get(schema, "patternProperties");
    if (pp && pp->type == CMCP_JSON_OBJECT) {
        for (size_t i = 0; i < pp->obj.len; i++) {
            regex_t re;
            if (regcomp(&re, pp->obj.keys[i], REG_EXTENDED | REG_NOSUB) != 0)
                continue;
            int m = regexec(&re, key, 0, NULL, 0) == 0;
            regfree(&re);
            if (m) return 1;
        }
    }
    return 0;
}

static int check_object(validate_ctx_t *ctx,
                         const cmcp_json_t *schema, const cmcp_json_t *value,
                         path_buf_t *path, cmcp_schema_error_t *err) {
    if (value->type != CMCP_JSON_OBJECT) return CMCP_OK;

    /* required */
    const cmcp_json_t *required = cmcp_json_object_get(schema, "required");
    if (required && required->type == CMCP_JSON_ARRAY) {
        for (size_t i = 0; i < required->arr.len; i++) {
            const cmcp_json_t *r = required->arr.items[i];
            if (!r || r->type != CMCP_JSON_STRING) continue;
            if (!cmcp_json_object_get(value, r->str.s)) {
                size_t saved = path_push_prop(path, r->str.s);
                int rc = fail(err, path, "required",
                               "missing required property");
                path_pop(path, saved);
                return rc;
            }
        }
    }

    /* minProperties / maxProperties */
    const cmcp_json_t *minp = cmcp_json_object_get(schema, "minProperties");
    const cmcp_json_t *maxp = cmcp_json_object_get(schema, "maxProperties");
    if (minp && minp->type == CMCP_JSON_INT
        && (long long)value->obj.len < minp->i)
        return fail(err, path, "minProperties",
                     "too few properties (min %lld)", minp->i);
    if (maxp && maxp->type == CMCP_JSON_INT
        && (long long)value->obj.len > maxp->i)
        return fail(err, path, "maxProperties",
                     "too many properties (max %lld)", maxp->i);

    /* propertyNames: subschema applied to each key (as a string value).
     * The subschema validates against a synthetic string node so a
     * caller can constrain keys to e.g. /^[a-z]+$/. */
    const cmcp_json_t *pn = cmcp_json_object_get(schema, "propertyNames");
    if (pn) {
        for (size_t i = 0; i < value->obj.len; i++) {
            cmcp_json_t *key_node = cmcp_json_new_string(value->obj.keys[i]);
            if (!key_node) return CMCP_ENOMEM;
            size_t saved = path_push_prop(path, value->obj.keys[i]);
            int rc = validate(ctx, pn, key_node, path, err);
            path_pop(path, saved);
            cmcp_json_free(key_node);
            if (rc != CMCP_OK) {
                /* Reframe the keyword: the constraint that failed is
                 * propertyNames, not whatever sub-keyword the inner
                 * validate() reported. */
                if (err && err->keyword) {
                    free(err->keyword);
                    err->keyword = xstrdup("propertyNames");
                }
                return rc;
            }
        }
    }

    /* properties + patternProperties + additionalProperties */
    const cmcp_json_t *props = cmcp_json_object_get(schema, "properties");
    const cmcp_json_t *pp    = cmcp_json_object_get(schema, "patternProperties");
    const cmcp_json_t *addl  = cmcp_json_object_get(schema, "additionalProperties");

    /* additionalProperties: false  → reject any unlisted key
     * additionalProperties: subschema → apply to any key not in
     *                                   properties / patternProperties */
    int addl_false = addl && addl->type == CMCP_JSON_BOOL && !addl->b;
    int addl_is_subschema = addl &&
        (addl->type == CMCP_JSON_OBJECT || addl->type == CMCP_JSON_BOOL);

    for (size_t i = 0; i < value->obj.len; i++) {
        const char *key = value->obj.keys[i];
        const cmcp_json_t *child = value->obj.values[i];

        /* Direct properties match. */
        const cmcp_json_t *sub = NULL;
        if (props && props->type == CMCP_JSON_OBJECT)
            sub = cmcp_json_object_get(props, key);

        if (sub) {
            size_t saved = path_push_prop(path, key);
            int rc = validate(ctx, sub, child, path, err);
            path_pop(path, saved);
            if (rc != CMCP_OK) return rc;
        }

        /* patternProperties: every matching pattern's subschema must
         * pass. (Note: this can compose with `properties` on the same
         * key — both are applied.) */
        int matched_pp = 0;
        if (pp && pp->type == CMCP_JSON_OBJECT) {
            for (size_t j = 0; j < pp->obj.len; j++) {
                regex_t re;
                if (regcomp(&re, pp->obj.keys[j], REG_EXTENDED | REG_NOSUB) != 0)
                    continue;
                int m = regexec(&re, key, 0, NULL, 0) == 0;
                regfree(&re);
                if (m) {
                    matched_pp = 1;
                    const cmcp_json_t *ps = pp->obj.values[j];
                    size_t saved = path_push_prop(path, key);
                    int rc = validate(ctx, ps, child, path, err);
                    path_pop(path, saved);
                    if (rc != CMCP_OK) return rc;
                }
            }
        }

        /* additionalProperties is only consulted for keys NOT covered
         * by either properties or patternProperties. */
        int covered = sub != NULL || matched_pp;
        if (!covered && addl_false) {
            size_t saved = path_push_prop(path, key);
            int rc = fail(err, path, "additionalProperties",
                           "unexpected property");
            path_pop(path, saved);
            return rc;
        }
        if (!covered && addl_is_subschema && !addl_false) {
            size_t saved = path_push_prop(path, key);
            int rc = validate(ctx, addl, child, path, err);
            path_pop(path, saved);
            if (rc != CMCP_OK) return rc;
        }
        (void)key_covered; /* silence unused if future change shadows it */
    }
    return CMCP_OK;
}

/* ====================================================================== */
/* Array keywords                                                          */
/* ====================================================================== */

static int check_array(validate_ctx_t *ctx,
                        const cmcp_json_t *schema, const cmcp_json_t *value,
                        path_buf_t *path, cmcp_schema_error_t *err) {
    if (value->type != CMCP_JSON_ARRAY) return CMCP_OK;

    /* minItems / maxItems */
    const cmcp_json_t *mi = cmcp_json_object_get(schema, "minItems");
    const cmcp_json_t *xi = cmcp_json_object_get(schema, "maxItems");
    if (mi && mi->type == CMCP_JSON_INT
        && (long long)value->arr.len < mi->i)
        return fail(err, path, "minItems",
                     "array too short (min %lld)", mi->i);
    if (xi && xi->type == CMCP_JSON_INT
        && (long long)value->arr.len > xi->i)
        return fail(err, path, "maxItems",
                     "array too long (max %lld)", xi->i);

    /* uniqueItems */
    const cmcp_json_t *uniq = cmcp_json_object_get(schema, "uniqueItems");
    if (uniq && uniq->type == CMCP_JSON_BOOL && uniq->b) {
        for (size_t i = 0; i < value->arr.len; i++) {
            for (size_t j = i + 1; j < value->arr.len; j++) {
                if (cmcp_json_equal(value->arr.items[i], value->arr.items[j]))
                    return fail(err, path, "uniqueItems",
                                 "duplicate items at indices %zu and %zu",
                                 i, j);
            }
        }
    }

    /* prefixItems (2020-12) OR tuple-form items (draft-07): an array
     * of subschemas applied positionally. */
    const cmcp_json_t *prefix = cmcp_json_object_get(schema, "prefixItems");
    const cmcp_json_t *items  = cmcp_json_object_get(schema, "items");
    const cmcp_json_t *tuple  = NULL;
    if (prefix && prefix->type == CMCP_JSON_ARRAY) tuple = prefix;
    else if (items && items->type == CMCP_JSON_ARRAY) tuple = items;

    size_t prefix_len = tuple ? tuple->arr.len : 0;
    if (tuple) {
        size_t bound = prefix_len < value->arr.len
                          ? prefix_len : value->arr.len;
        for (size_t i = 0; i < bound; i++) {
            size_t saved = path_push_idx(path, i);
            int rc = validate(ctx, tuple->arr.items[i], value->arr.items[i],
                              path, err);
            path_pop(path, saved);
            if (rc != CMCP_OK) return rc;
        }
    }

    /* items beyond the prefix:
     *   - draft-07 with tuple `items`: `additionalItems` is the schema
     *     (or boolean) for entries past `items.length`
     *   - draft-2020-12 with `prefixItems`: `items` (now scalar) is
     *     the schema for entries past `prefixItems.length`
     *   - single-subschema `items` (legacy / non-tuple): applied to
     *     every element */
    const cmcp_json_t *rest = NULL;
    int rest_false = 0;
    if (prefix) {
        if (items && items->type != CMCP_JSON_ARRAY) {
            if (items->type == CMCP_JSON_BOOL && !items->b) rest_false = 1;
            else rest = items;
        }
    } else if (items && items->type == CMCP_JSON_ARRAY) {
        const cmcp_json_t *ai = cmcp_json_object_get(schema,
                                                     "additionalItems");
        if (ai) {
            if (ai->type == CMCP_JSON_BOOL && !ai->b) rest_false = 1;
            else rest = ai;
        }
    } else if (items && items->type != CMCP_JSON_ARRAY) {
        rest = items;
    }

    if (rest_false && value->arr.len > prefix_len)
        return fail(err, path, "additionalItems",
                     "additional items not allowed past index %zu",
                     prefix_len);

    if (rest) {
        for (size_t i = prefix_len; i < value->arr.len; i++) {
            size_t saved = path_push_idx(path, i);
            int rc = validate(ctx, rest, value->arr.items[i], path, err);
            path_pop(path, saved);
            if (rc != CMCP_OK) return rc;
        }
    }
    return CMCP_OK;
}

/* ====================================================================== */
/* Combinators: allOf / anyOf / oneOf / not                                */
/* ====================================================================== */

static int check_combinators(validate_ctx_t *ctx,
                              const cmcp_json_t *schema, const cmcp_json_t *value,
                              path_buf_t *path, cmcp_schema_error_t *err) {
    /* allOf — every subschema must pass. The first failure surfaces. */
    const cmcp_json_t *allof = cmcp_json_object_get(schema, "allOf");
    if (allof && allof->type == CMCP_JSON_ARRAY) {
        for (size_t i = 0; i < allof->arr.len; i++) {
            int rc = validate(ctx, allof->arr.items[i], value, path, err);
            if (rc != CMCP_OK) return rc;
        }
    }

    /* anyOf — at least one subschema must pass. We probe quietly; if
     * none match, surface a single "no branch matched" error. */
    const cmcp_json_t *anyof = cmcp_json_object_get(schema, "anyOf");
    if (anyof && anyof->type == CMCP_JSON_ARRAY) {
        int found = 0;
        for (size_t i = 0; i < anyof->arr.len; i++) {
            if (validate_quiet(ctx, anyof->arr.items[i], value, path) == CMCP_OK) {
                found = 1;
                break;
            }
        }
        if (!found)
            return fail(err, path, "anyOf", "no anyOf branch matched");
    }

    /* oneOf — exactly one must pass. */
    const cmcp_json_t *oneof = cmcp_json_object_get(schema, "oneOf");
    if (oneof && oneof->type == CMCP_JSON_ARRAY) {
        size_t hits = 0;
        for (size_t i = 0; i < oneof->arr.len && hits < 2; i++) {
            if (validate_quiet(ctx, oneof->arr.items[i], value, path) == CMCP_OK)
                hits++;
        }
        if (hits == 0)
            return fail(err, path, "oneOf", "no oneOf branch matched");
        if (hits > 1)
            return fail(err, path, "oneOf", "multiple oneOf branches matched");
    }

    /* not — subschema must NOT pass. */
    const cmcp_json_t *neg = cmcp_json_object_get(schema, "not");
    if (neg) {
        if (validate_quiet(ctx, neg, value, path) == CMCP_OK)
            return fail(err, path, "not", "value matched a forbidden subschema");
    }

    return CMCP_OK;
}

/* if / then / else conditional. `if` is evaluated in schema-only mode
 * — its failure is silent and only steers the branch selection. */
static int check_conditional(validate_ctx_t *ctx,
                              const cmcp_json_t *schema, const cmcp_json_t *value,
                              path_buf_t *path, cmcp_schema_error_t *err) {
    const cmcp_json_t *if_sch   = cmcp_json_object_get(schema, "if");
    if (!if_sch) return CMCP_OK;
    const cmcp_json_t *then_sch = cmcp_json_object_get(schema, "then");
    const cmcp_json_t *else_sch = cmcp_json_object_get(schema, "else");
    int if_matched = validate_quiet(ctx, if_sch, value, path) == CMCP_OK;
    if (if_matched && then_sch)
        return validate(ctx, then_sch, value, path, err);
    if (!if_matched && else_sch)
        return validate(ctx, else_sch, value, path, err);
    return CMCP_OK;
}

/* ====================================================================== */
/* $ref resolver                                                           */
/* ====================================================================== */
/* Supports JSON Pointer references rooted at `#` (the root schema):
 *   "#"                   → root
 *   "#/$defs/Foo"         → root.$defs.Foo
 *   "#/definitions/Foo"   → root.definitions.Foo  (draft-07 alias)
 * Per RFC 6901 the tokens unescape `~0` → `~` and `~1` → `/`. Remote
 * refs (`http://...#`) and refs into a non-root document are
 * deliberately not supported — MCP `inputSchema` is self-contained.
 *
 * Returns NULL on any failure (malformed pointer, missing key, wrong
 * shape); callers translate that into a fail(). */

static const cmcp_json_t *resolve_json_pointer(const cmcp_json_t *root,
                                                 const char *ref) {
    if (!root || !ref || ref[0] != '#') return NULL;
    if (ref[1] == '\0') return root;
    if (ref[1] != '/')  return NULL;

    const cmcp_json_t *node = root;
    const char *p = ref + 1;        /* sits on the first '/' */
    while (*p == '/') {
        p++;
        /* Unescape one segment into a stack-friendly buffer. Schema
         * key names are bounded in practice; cap at 256 to bound the
         * stack frame and refuse pathological inputs. */
        char seg[256];
        size_t slen = 0;
        while (*p && *p != '/') {
            if (slen + 1 >= sizeof seg) return NULL;
            char c = *p++;
            if (c == '~') {
                if      (*p == '0') { seg[slen++] = '~'; p++; }
                else if (*p == '1') { seg[slen++] = '/'; p++; }
                else return NULL;
            } else {
                seg[slen++] = c;
            }
        }
        seg[slen] = '\0';
        if (!node || node->type != CMCP_JSON_OBJECT) return NULL;
        node = cmcp_json_object_get(node, seg);
        if (!node) return NULL;
    }
    return node;
}

static int check_ref(validate_ctx_t *ctx,
                      const cmcp_json_t *schema, const cmcp_json_t *value,
                      path_buf_t *path, cmcp_schema_error_t *err,
                      int *consumed) {
    *consumed = 0;
    const cmcp_json_t *ref = cmcp_json_object_get(schema, "$ref");
    if (!ref || ref->type != CMCP_JSON_STRING) return CMCP_OK;
    *consumed = 1;

    const cmcp_json_t *target = resolve_json_pointer(ctx->root, ref->str.s);
    if (!target)
        return fail(err, path, "$ref", "could not resolve %s", ref->str.s);

    /* Per draft-2020-12, sibling keywords next to $ref are valid and
     * should be honoured. We treat the target like another schema in
     * an implicit allOf — validate against it, then fall back to
     * check the siblings in the caller. The combinator path already
     * walks every keyword on the local schema, so all we need to do
     * here is run the target through validate() and report its rc. */
    return validate(ctx, target, value, path, err);
}

/* ====================================================================== */
/* `format` keyword (date-time, email, uri, uuid)                          */
/* ====================================================================== */
/* These are best-effort lexical validators chosen to match what Ajv
 * accepts/rejects for the common formats. They are not full
 * RFC validators: see docs/schema-conformance.md for the trade-off.
 * Unknown `format` values are an *annotation* per the spec — we
 * accept them silently rather than failing the schema. */

static int is_2digit(const char *s) {
    return isdigit((unsigned char)s[0]) && isdigit((unsigned char)s[1]);
}

static int is_4digit(const char *s) {
    return is_2digit(s) && is_2digit(s + 2);
}

static int format_date_time(const char *s) {
    /* RFC 3339 date-time: 1985-04-12T23:20:50.52Z (or +offset).
     * Shape: YYYY-MM-DDTHH:MM:SS[.frac][Z|±HH:MM]. We don't validate
     * day-in-month bounds; Ajv's default mode does, but for MCP
     * tool-input use the cheap shape check matches the agent
     * round-trip in practice. */
    size_t n = strlen(s);
    if (n < 20) return 0;
    if (!is_4digit(s) || s[4] != '-' || !is_2digit(s + 5) || s[7] != '-' ||
        !is_2digit(s + 8) || (s[10] != 'T' && s[10] != 't') ||
        !is_2digit(s + 11) || s[13] != ':' || !is_2digit(s + 14) ||
        s[16] != ':' || !is_2digit(s + 17))
        return 0;
    size_t i = 19;
    if (s[i] == '.') {
        i++;
        size_t frac_start = i;
        while (i < n && isdigit((unsigned char)s[i])) i++;
        if (i == frac_start) return 0;
    }
    if (i == n) return 0;
    if (s[i] == 'Z' || s[i] == 'z') return s[i + 1] == '\0';
    if (s[i] == '+' || s[i] == '-') {
        if (n - i != 6) return 0;
        return is_2digit(s + i + 1) && s[i + 3] == ':' &&
               is_2digit(s + i + 4);
    }
    return 0;
}

static int format_email(const char *s) {
    /* Cheap mailbox check: one `@`, non-empty local + domain, no
     * spaces or control chars. Matches Ajv's "fast" mode. */
    const char *at = strchr(s, '@');
    if (!at || at == s || at[1] == '\0' || strchr(at + 1, '@')) return 0;
    /* Domain must contain at least one dot, no leading/trailing dot,
     * no consecutive dots. */
    const char *domain = at + 1;
    const char *dot = strchr(domain, '.');
    if (!dot || dot == domain || domain[strlen(domain) - 1] == '.') return 0;
    for (const char *p = domain; *p; p++) {
        if (*p == '.' && p[1] == '.') return 0;
    }
    for (const char *p = s; *p; p++) {
        unsigned char c = (unsigned char)*p;
        if (c <= ' ' || c == 127) return 0;
    }
    return 1;
}

static int format_uri(const char *s) {
    /* RFC 3986 URI: scheme ":" hier-part. The scheme must be
     * ALPHA *( ALPHA / DIGIT / "+" / "-" / "." ) and there must be
     * something after the colon. */
    const char *p = s;
    if (!isalpha((unsigned char)*p)) return 0;
    p++;
    while (*p && (isalnum((unsigned char)*p) ||
                  *p == '+' || *p == '-' || *p == '.')) p++;
    if (*p != ':' || p[1] == '\0') return 0;
    /* Reject embedded whitespace/controls. */
    for (const char *q = s; *q; q++) {
        unsigned char c = (unsigned char)*q;
        if (c <= ' ' || c == 127) return 0;
    }
    return 1;
}

static int format_uuid(const char *s) {
    /* 8-4-4-4-12 lowercase or uppercase hex with hyphens. Total 36. */
    if (strlen(s) != 36) return 0;
    static const int dashes[] = { 8, 13, 18, 23, -1 };
    for (int i = 0; i < 36; i++) {
        int is_dash = 0;
        for (int k = 0; dashes[k] >= 0; k++) {
            if (dashes[k] == i) { is_dash = 1; break; }
        }
        if (is_dash) { if (s[i] != '-') return 0; }
        else         { if (!isxdigit((unsigned char)s[i])) return 0; }
    }
    return 1;
}

static int check_format(const cmcp_json_t *schema, const cmcp_json_t *value,
                         path_buf_t *path, cmcp_schema_error_t *err) {
    /* Only applies to strings. The keyword is a no-op for non-strings
     * — same posture as `pattern`, and matches Ajv. */
    if (value->type != CMCP_JSON_STRING) return CMCP_OK;
    const cmcp_json_t *fmt = cmcp_json_object_get(schema, "format");
    if (!fmt || fmt->type != CMCP_JSON_STRING) return CMCP_OK;

    const char *f = fmt->str.s;
    const char *s = value->str.s;
    int ok = 1;       /* unknown formats accepted (annotation posture) */
    if      (!strcmp(f, "date-time")) ok = format_date_time(s);
    else if (!strcmp(f, "email"))     ok = format_email(s);
    else if (!strcmp(f, "uri"))       ok = format_uri(s);
    else if (!strcmp(f, "uuid"))      ok = format_uuid(s);
    if (!ok)
        return fail(err, path, "format", "value does not match format %s", f);
    return CMCP_OK;
}

/* ====================================================================== */
/* Top-level dispatch                                                      */
/* ====================================================================== */

static int validate(validate_ctx_t *ctx,
                     const cmcp_json_t *schema, const cmcp_json_t *value,
                     path_buf_t *path, cmcp_schema_error_t *err) {
    /* Boolean schemas — JSON Schema allows `true` (always pass) and
     * `false` (always fail) as a whole schema. They appear most often
     * as subschemas inside additionalProperties / items / patternProperties. */
    if (schema->type == CMCP_JSON_BOOL) {
        if (schema->b) return CMCP_OK;
        return fail(err, path, "false", "schema rejects all values");
    }
    if (schema->type != CMCP_JSON_OBJECT)
        /* Schema author error — silently accept. */
        return CMCP_OK;

    /* Bound recursion depth — attacker schemas can hide ref cycles
     * (`{"$defs":{"x":{"$ref":"#/$defs/x"}}}`) or genuine deep
     * `allOf` chains. Same defense as the JSON parser's depth cap. */
    if (++ctx->depth > ctx->max_depth) {
        ctx->depth--;
        return fail(err, path, "$ref",
                     "schema recursion exceeded %d levels",
                     ctx->max_depth);
    }

    int rc = CMCP_OK;
    int ref_consumed = 0;
    rc = check_ref(ctx, schema, value, path, err, &ref_consumed);
    if (rc != CMCP_OK) goto out;
    /* draft-2020-12: $ref siblings are honoured. Fall through into the
     * rest of the keywords. (Draft-07 would early-return if $ref was
     * present; we don't, to match Ajv's 2020-12 mode.) */
    (void)ref_consumed;

    if ((rc = check_type(schema, value, path, err))                != CMCP_OK) goto out;
    if ((rc = check_enum(schema, value, path, err))                != CMCP_OK) goto out;
    if ((rc = check_const(schema, value, path, err))               != CMCP_OK) goto out;
    if ((rc = check_string(schema, value, path, err))              != CMCP_OK) goto out;
    if ((rc = check_format(schema, value, path, err))              != CMCP_OK) goto out;
    if ((rc = check_number(schema, value, path, err))              != CMCP_OK) goto out;
    if ((rc = check_object(ctx, schema, value, path, err))         != CMCP_OK) goto out;
    if ((rc = check_array(ctx, schema, value, path, err))          != CMCP_OK) goto out;
    if ((rc = check_combinators(ctx, schema, value, path, err))    != CMCP_OK) goto out;
    if ((rc = check_conditional(ctx, schema, value, path, err))    != CMCP_OK) goto out;

out:
    ctx->depth--;
    return rc;
}

/* ====================================================================== */
/* Public entry                                                            */
/* ====================================================================== */

int cmcp_schema_validate(const cmcp_json_t *schema,
                          const cmcp_json_t *value,
                          cmcp_schema_error_t *err) {
    /* Allow boolean top-level schemas (true / false). */
    if (!schema) return CMCP_EINVAL;
    if (schema->type != CMCP_JSON_OBJECT && schema->type != CMCP_JSON_BOOL)
        return CMCP_EINVAL;
    if (err) cmcp_schema_error_init(err);

    /* Treat NULL value as a JSON null so the inner functions can deref
     * `value->type` without a NULL check on every line. */
    cmcp_json_t *null_node = NULL;
    if (!value) {
        null_node = cmcp_json_new_null();
        if (!null_node) return CMCP_ENOMEM;
        value = null_node;
    }

    path_buf_t path;
    if (path_init(&path) < 0) {
        cmcp_json_free(null_node);
        return CMCP_ENOMEM;
    }

    validate_ctx_t ctx = {
        .root      = schema,
        .depth     = 0,
        .max_depth = CMCP_SCHEMA_MAX_DEPTH,
    };
    int rc = validate(&ctx, schema, value, &path, err);

    free(path.data);
    cmcp_json_free(null_node);
    return rc;
}
