/* xheader.c — `x-mcp-header` lint and per-call check (see xheader.h). */
#include "xheader.h"

#include "cmcp.h"
#include "http_modern.h"
#include "schema_walk.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#define XHEADER_KEY    "x-mcp-header"
#define HEADER_PREFIX  "Mcp-Param-"
/* IEEE 754 doubles hold every integer up to 2^53 - 1 exactly. */
#define SAFE_INT_MAX   9007199254740991LL

/* RFC 9110 §5.6.2 tchar. */
static int is_tchar(char c) {
    if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
        (c >= '0' && c <= '9'))
        return 1;
    return c != '\0' && strchr("!#$%&'*+-.^_`|~", c) != NULL;
}

static int is_token(const char *s) {
    if (!*s) return 0;
    for (; *s; s++)
        if (!is_tchar(*s)) return 0;
    return 1;
}

/* ---------------------------------------------------------------- */
/* Lint                                                              */
/* ---------------------------------------------------------------- */

typedef struct {
    const char          *names[CMCP_XHEADER_MAX];
    size_t               n;
    cmcp_schema_error_t *err;
} lint_t;

static int primitive_type(const cmcp_json_t *node) {
    const cmcp_json_t *t = cmcp_json_object_get(node, "type");
    if (!t || t->type != CMCP_JSON_STRING) return 0;
    return strcmp(t->str.s, "string") == 0 || strcmp(t->str.s, "integer") == 0 ||
           strcmp(t->str.s, "boolean") == 0;
}

static int lint_visit(const cmcp_json_t *node, const char *path, int reachable,
                      void *ctx) {
    lint_t *l = (lint_t *)ctx;
    if (node->type != CMCP_JSON_OBJECT) return CMCP_OK;
    const cmcp_json_t *x = cmcp_json_object_get(node, XHEADER_KEY);
    if (!x) return CMCP_OK;

    if (x->type != CMCP_JSON_STRING || !is_token(x->str.s))
        return cmcp_schema_walk_fail(l->err, CMCP_EINVAL, path, XHEADER_KEY,
                                     "x-mcp-header must be a non-empty HTTP "
                                     "token");
    /* The root is reachable but is not a property. */
    if (!reachable || path[0] == '\0')
        return cmcp_schema_walk_fail(l->err, CMCP_EINVAL, path, XHEADER_KEY,
                                     "x-mcp-header is only allowed on a "
                                     "property reached through `properties` "
                                     "keys alone");
    if (!primitive_type(node))
        return cmcp_schema_walk_fail(l->err, CMCP_EINVAL, path, XHEADER_KEY,
                                     "x-mcp-header requires type string, "
                                     "integer or boolean");
    for (size_t i = 0; i < l->n; i++) {
        if (strcasecmp(l->names[i], x->str.s) == 0)
            return cmcp_schema_walk_fail(l->err, CMCP_EINVAL, path, XHEADER_KEY,
                                         "x-mcp-header '%s' is used twice",
                                         x->str.s);
    }
    if (l->n == CMCP_XHEADER_MAX)
        return cmcp_schema_walk_fail(l->err, CMCP_EINVAL, path, XHEADER_KEY,
                                     "more than %d x-mcp-header annotations",
                                     CMCP_XHEADER_MAX);
    l->names[l->n++] = x->str.s;
    return CMCP_OK;
}

int cmcp_xheader_lint(const cmcp_json_t *input_schema, cmcp_schema_error_t *err) {
    if (err) cmcp_schema_error_init(err);
    if (!input_schema) return CMCP_OK;
    lint_t l = {.n = 0, .err = err};
    return cmcp_schema_walk(input_schema, lint_visit, &l, err);
}

/* ---------------------------------------------------------------- */
/* Per-call check                                                    */
/* ---------------------------------------------------------------- */

static int mismatch(char **out_msg, const char *fmt, const char *a,
                    const char *b) {
    char msg[512];
    snprintf(msg, sizeof msg, fmt, a, b ? b : "");
    size_t n = strlen(msg);
    *out_msg = (char *)malloc(n + 1);
    if (*out_msg) memcpy(*out_msg, msg, n + 1);
    return 1;
}

/* Does the decoded header text equal the argument value? */
static int same_value(const char *text, const cmcp_json_t *v) {
    if (v->type == CMCP_JSON_STRING) return strcmp(text, v->str.s) == 0;
    if (v->type == CMCP_JSON_BOOL) return strcmp(text, v->b ? "true" : "false") == 0;
    if (v->type != CMCP_JSON_INT && v->type != CMCP_JSON_DOUBLE) return 0;
    /* Integers are compared numerically ("42.0" == 42), within the range
     * a double represents exactly. */
    double want = v->type == CMCP_JSON_INT ? (double)v->i : v->d;
    if (fabs(want) > (double)SAFE_INT_MAX) return 0;
    char *end = NULL;
    double got = strtod(text, &end);
    return end != text && *end == '\0' && got == want;
}

static int check_one(const char *name, const cmcp_json_t *value,
                     cmcp_header_get_fn get, void *get_ctx, char **out_msg) {
    char header[160];
    snprintf(header, sizeof header, HEADER_PREFIX "%s", name);
    const char *raw = get(get_ctx, header);
    if (!raw)
        return mismatch(out_msg, "Header mismatch: %s header is required "
                        "because the argument is present%s", header, NULL);
    char *text = cmcp_http_header_decode(raw);
    if (!text)
        return mismatch(out_msg, "Header mismatch: %s header value '%s' is not "
                        "valid base64", header, raw);
    int same = same_value(text, value);
    int rc = same ? 0
                  : mismatch(out_msg, "Header mismatch: %s header value '%s' "
                             "does not match the body value", header, text);
    free(text);
    return rc;
}

/* Walk the `properties` chains of `schema` alongside `args`. */
static int check_props(const cmcp_json_t *schema, const cmcp_json_t *args,
                       cmcp_header_get_fn get, void *get_ctx, char **out_msg) {
    const cmcp_json_t *props = cmcp_json_object_get(schema, "properties");
    if (!props || props->type != CMCP_JSON_OBJECT ||
        !args || args->type != CMCP_JSON_OBJECT)
        return 0;
    for (size_t i = 0; i < props->obj.len; i++) {
        const cmcp_json_t *sub = props->obj.values[i];
        const cmcp_json_t *val = cmcp_json_object_get(args, props->obj.keys[i]);
        if (sub->type != CMCP_JSON_OBJECT || !val) continue;
        const cmcp_json_t *x = cmcp_json_object_get(sub, XHEADER_KEY);
        /* A null or absent argument has no header (spec table). */
        if (x && x->type == CMCP_JSON_STRING && val->type != CMCP_JSON_NULL &&
            check_one(x->str.s, val, get, get_ctx, out_msg))
            return 1;
        if (check_props(sub, val, get, get_ctx, out_msg)) return 1;
    }
    return 0;
}

int cmcp_xheader_check(const cmcp_json_t *input_schema, const cmcp_json_t *args,
                       cmcp_header_get_fn get, void *get_ctx, char **out_msg) {
    *out_msg = NULL;
    if (!input_schema || input_schema->type != CMCP_JSON_OBJECT || !get) return 0;
    return check_props(input_schema, args, get, get_ctx, out_msg);
}

/* ---------------------------------------------------------------- */
/* Client side                                                       */
/* ---------------------------------------------------------------- */

/* Text form of an argument for its header; 0 when it has none. */
static int value_text(const cmcp_json_t *v, char *buf, size_t cap,
                      const char **out) {
    if (v->type == CMCP_JSON_STRING) { *out = v->str.s; return 1; }
    if (v->type == CMCP_JSON_BOOL) { *out = v->b ? "true" : "false"; return 1; }
    if (v->type == CMCP_JSON_INT) {
        snprintf(buf, cap, "%lld", v->i);
        *out = buf;
        return 1;
    }
    return 0;                 /* the lint allows only these three types */
}

static int collect_props(const cmcp_json_t *schema, const cmcp_json_t *args,
                         cmcp_xheader_emit_fn emit, void *ctx) {
    const cmcp_json_t *props = cmcp_json_object_get(schema, "properties");
    if (!props || props->type != CMCP_JSON_OBJECT ||
        !args || args->type != CMCP_JSON_OBJECT)
        return 0;
    for (size_t i = 0; i < props->obj.len; i++) {
        const cmcp_json_t *sub = props->obj.values[i];
        const cmcp_json_t *val = cmcp_json_object_get(args, props->obj.keys[i]);
        if (sub->type != CMCP_JSON_OBJECT || !val) continue;
        const cmcp_json_t *x = cmcp_json_object_get(sub, XHEADER_KEY);
        char num[32];
        const char *text = NULL;
        if (x && x->type == CMCP_JSON_STRING && value_text(val, num, sizeof num, &text)) {
            int rc = emit(ctx, x->str.s, text);
            if (rc) return rc;
        }
        int rc = collect_props(sub, val, emit, ctx);
        if (rc) return rc;
    }
    return 0;
}

int cmcp_xheader_collect(const cmcp_json_t *input_schema, const cmcp_json_t *args,
                         cmcp_xheader_emit_fn emit, void *ctx) {
    if (!input_schema || input_schema->type != CMCP_JSON_OBJECT || !emit) return 0;
    return collect_props(input_schema, args, emit, ctx);
}
