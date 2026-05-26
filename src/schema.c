#include "cmcp.h"
#include "cmcp_schema.h"
#include "cmcp_json.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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

static int validate(const cmcp_json_t *schema, const cmcp_json_t *value,
                     path_buf_t *path, cmcp_schema_error_t *err);

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
    return CMCP_OK;
}

static int check_number(const cmcp_json_t *schema, const cmcp_json_t *value,
                         path_buf_t *path, cmcp_schema_error_t *err) {
    if (value->type != CMCP_JSON_INT && value->type != CMCP_JSON_DOUBLE)
        return CMCP_OK;
    double v = value->type == CMCP_JSON_INT
                  ? (double)value->i : value->d;
    const cmcp_json_t *mn = cmcp_json_object_get(schema, "minimum");
    const cmcp_json_t *mx = cmcp_json_object_get(schema, "maximum");
    if (mn) {
        double bound = mn->type == CMCP_JSON_INT
                          ? (double)mn->i : mn->d;
        if (v < bound) {
            return fail(err, path, "minimum", "value below minimum");
        }
    }
    if (mx) {
        double bound = mx->type == CMCP_JSON_INT
                          ? (double)mx->i : mx->d;
        if (v > bound) {
            return fail(err, path, "maximum", "value above maximum");
        }
    }
    return CMCP_OK;
}

static int check_object(const cmcp_json_t *schema, const cmcp_json_t *value,
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

    /* properties + additionalProperties: false */
    const cmcp_json_t *props = cmcp_json_object_get(schema, "properties");
    const cmcp_json_t *addl  = cmcp_json_object_get(schema, "additionalProperties");
    int reject_addl = addl && addl->type == CMCP_JSON_BOOL && !addl->b;
    int has_props = props && props->type == CMCP_JSON_OBJECT;

    for (size_t i = 0; i < value->obj.len; i++) {
        const char *key = value->obj.keys[i];
        const cmcp_json_t *child = value->obj.values[i];
        const cmcp_json_t *subschema = NULL;
        if (has_props) subschema = cmcp_json_object_get(props, key);

        if (!subschema && reject_addl) {
            size_t saved = path_push_prop(path, key);
            int rc = fail(err, path, "additionalProperties",
                           "unexpected property");
            path_pop(path, saved);
            return rc;
        }
        if (subschema && subschema->type == CMCP_JSON_OBJECT) {
            size_t saved = path_push_prop(path, key);
            int rc = validate(subschema, child, path, err);
            path_pop(path, saved);
            if (rc != CMCP_OK) return rc;
        }
    }
    return CMCP_OK;
}

static int check_array(const cmcp_json_t *schema, const cmcp_json_t *value,
                        path_buf_t *path, cmcp_schema_error_t *err) {
    if (value->type != CMCP_JSON_ARRAY) return CMCP_OK;
    const cmcp_json_t *items = cmcp_json_object_get(schema, "items");
    if (!items || items->type != CMCP_JSON_OBJECT) return CMCP_OK;
    for (size_t i = 0; i < value->arr.len; i++) {
        size_t saved = path_push_idx(path, i);
        int rc = validate(items, value->arr.items[i], path, err);
        path_pop(path, saved);
        if (rc != CMCP_OK) return rc;
    }
    return CMCP_OK;
}

static int validate(const cmcp_json_t *schema, const cmcp_json_t *value,
                     path_buf_t *path, cmcp_schema_error_t *err) {
    int rc;
    if ((rc = check_type(schema, value, path, err))   != CMCP_OK) return rc;
    if ((rc = check_enum(schema, value, path, err))   != CMCP_OK) return rc;
    if ((rc = check_string(schema, value, path, err)) != CMCP_OK) return rc;
    if ((rc = check_number(schema, value, path, err)) != CMCP_OK) return rc;
    if ((rc = check_object(schema, value, path, err)) != CMCP_OK) return rc;
    if ((rc = check_array(schema, value, path, err))  != CMCP_OK) return rc;
    return CMCP_OK;
}

/* ====================================================================== */
/* Public entry                                                            */
/* ====================================================================== */

int cmcp_schema_validate(const cmcp_json_t *schema,
                          const cmcp_json_t *value,
                          cmcp_schema_error_t *err) {
    if (!schema || schema->type != CMCP_JSON_OBJECT) return CMCP_EINVAL;
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

    int rc = validate(schema, value, &path, err);

    free(path.data);
    cmcp_json_free(null_node);
    return rc;
}
