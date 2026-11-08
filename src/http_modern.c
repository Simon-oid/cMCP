/* http_modern.c — Streamable HTTP request-metadata rules for MCP
 * 2026-07-28 (see http_modern.h). */
#include "http_modern.h"

#include "cmcp.h"
#include "cmcp_types.h"
#include "era.h"
#include "xheader.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SENTINEL_PREFIX     "=?base64?"
#define SENTINEL_PREFIX_LEN 9u
#define SENTINEL_SUFFIX     "?="
#define SENTINEL_SUFFIX_LEN 2u

static const cmcp_json_t *field(const cmcp_json_t *o, const char *k) {
    return (o && o->type == CMCP_JSON_OBJECT) ? cmcp_json_object_get(o, k)
                                              : NULL;
}

static const char *str_field(const cmcp_json_t *o, const char *k) {
    const cmcp_json_t *v = field(o, k);
    return (v && v->type == CMCP_JSON_STRING) ? cmcp_json_string(v) : NULL;
}

/* object_set that frees `v` when it cannot be inserted. 0 on success. */
static int set_owned(cmcp_json_t *o, const char *k, cmcp_json_t *v) {
    if (!v || cmcp_json_object_set(o, k, v) != 0) {
        cmcp_json_free(v);
        return -1;
    }
    return 0;
}

int cmcp_http_modern_is(const cmcp_json_t *body,
                        const cmcp_http_request_t *req) {
    const char *pv = cmcp_http_header_get(req, "MCP-Protocol-Version");
    if (pv && strcmp(pv, CMCP_PROTOCOL_VERSION_MODERN) == 0) return 1;
    return body ? cmcp_era_is_modern(field(body, "params")) : 0;
}

/* ---------------------------------------------------------------- */
/* Base64 sentinel (RFC 4648 §4, padded)                             */
/* ---------------------------------------------------------------- */

static int b64_value(char c) {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    return -1;
}

/* Decode one 4-character group into out; returns bytes written (1..3)
 * or -1. '=' padding is allowed only in the final group. */
static int b64_group(const char *in, int is_last, char *out) {
    unsigned w = 0;
    int pad = 0;
    for (unsigned k = 0; k < 4; k++) {
        int v;
        if (in[k] == '=' && is_last && k >= 2) {
            pad++;
            v = 0;
        } else {
            v = pad ? -1 : b64_value(in[k]);
            if (v < 0) return -1;
        }
        w = (w << 6u) | (unsigned)v;
    }
    unsigned char bytes[3] = {(unsigned char)(w >> 16u),
                              (unsigned char)(w >> 8u), (unsigned char)w};
    for (int k = 0; k < 3 - pad; k++) {
        if (bytes[k] == 0) return -1;     /* no NUL inside a header value */
        out[k] = (char)bytes[k];
    }
    return 3 - pad;
}

static char *b64_decode(const char *in, size_t len) {
    if (len % 4 != 0) return NULL;
    char *out = (char *)malloc(len / 4 * 3 + 1);
    if (!out) return NULL;
    size_t o = 0;
    for (size_t i = 0; i < len; i += 4) {
        int n = b64_group(in + i, i + 4 == len, out + o);
        if (n < 0) { free(out); return NULL; }
        o += (size_t)n;
    }
    out[o] = '\0';
    return out;
}

char *cmcp_http_header_decode(const char *value) {
    if (!value) return NULL;
    size_t n = strlen(value);
    if (n >= SENTINEL_PREFIX_LEN + SENTINEL_SUFFIX_LEN &&
        strncmp(value, SENTINEL_PREFIX, SENTINEL_PREFIX_LEN) == 0 &&
        strcmp(value + n - SENTINEL_SUFFIX_LEN, SENTINEL_SUFFIX) == 0) {
        return b64_decode(value + SENTINEL_PREFIX_LEN,
                          n - SENTINEL_PREFIX_LEN - SENTINEL_SUFFIX_LEN);
    }
    char *copy = (char *)malloc(n + 1);
    if (copy) memcpy(copy, value, n + 1);
    return copy;
}

static const char B64[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

/* A value that can travel in a header without encoding (RFC 9110). */
static int header_safe(const char *v) {
    size_t n = strlen(v);
    if (n == 0) return 1;
    if (v[0] == ' ' || v[0] == '\t' || v[n - 1] == ' ' || v[n - 1] == '\t')
        return 0;
    /* A plain value shaped like the sentinel must be encoded too. */
    if (n >= SENTINEL_PREFIX_LEN + SENTINEL_SUFFIX_LEN &&
        strncmp(v, SENTINEL_PREFIX, SENTINEL_PREFIX_LEN) == 0 &&
        strcmp(v + n - SENTINEL_SUFFIX_LEN, SENTINEL_SUFFIX) == 0)
        return 0;
    for (size_t i = 0; i < n; i++) {
        unsigned char c = (unsigned char)v[i];
        if (c < 0x20u || c > 0x7Eu) return 0;
    }
    return 1;
}

char *cmcp_http_header_encode(const char *value) {
    if (!value) return NULL;
    size_t n = strlen(value);
    if (header_safe(value)) {
        char *copy = (char *)malloc(n + 1);
        if (copy) memcpy(copy, value, n + 1);
        return copy;
    }
    size_t cap = SENTINEL_PREFIX_LEN + (n + 2) / 3 * 4 + SENTINEL_SUFFIX_LEN + 1;
    char *out = (char *)malloc(cap);
    if (!out) return NULL;
    const unsigned char *in = (const unsigned char *)value;
    size_t o = (size_t)snprintf(out, cap, "%s", SENTINEL_PREFIX);
    for (size_t i = 0; i < n; i += 3) {
        unsigned w = (unsigned)in[i] << 16u;
        if (i + 1 < n) w |= (unsigned)in[i + 1] << 8u;
        if (i + 2 < n) w |= (unsigned)in[i + 2];
        out[o++] = B64[(w >> 18u) & 63u];
        out[o++] = B64[(w >> 12u) & 63u];
        out[o++] = i + 1 < n ? B64[(w >> 6u) & 63u] : '=';
        out[o++] = i + 2 < n ? B64[w & 63u] : '=';
    }
    snprintf(out + o, cap - o, "%s", SENTINEL_SUFFIX);
    return out;
}

/* ---------------------------------------------------------------- */
/* Validation                                                        */
/* ---------------------------------------------------------------- */

/* {"jsonrpc":"2.0","id":<id or null>,"error":{code,message,data?}} as
 * text. Consumes `data`. NULL on allocation failure. */
static char *error_frame(const cmcp_json_t *body, int code, const char *msg,
                         cmcp_json_t *data) {
    const cmcp_json_t *id = field(body, "id");
    cmcp_json_t *f = cmcp_json_new_object();
    cmcp_json_t *e = cmcp_json_new_object();
    char *text = NULL;
    if (!f || !e) {
        cmcp_json_free(data);
        cmcp_json_free(e);
        cmcp_json_free(f);
        return NULL;
    }
    int ok = set_owned(e, "code", cmcp_json_new_int(code)) == 0 &&
             set_owned(e, "message", cmcp_json_new_string(msg)) == 0;
    if (data) {
        if (ok) ok = set_owned(e, "data", data) == 0;   /* consumes data */
        else cmcp_json_free(data);
    }
    ok = ok &&
         set_owned(f, "jsonrpc", cmcp_json_new_string("2.0")) == 0 &&
         set_owned(f, "id", id ? cmcp_json_clone(id) : cmcp_json_new_null()) == 0;
    if (ok) {
        int rc = set_owned(f, "error", e);   /* consumes e */
        e = NULL;
        if (rc == 0) text = cmcp_json_emit(f);
    }
    cmcp_json_free(e);
    cmcp_json_free(f);
    return text;
}

static int mismatch(const cmcp_json_t *body, char **out_body,
                    const char *fmt, const char *a, const char *b) {
    char msg[512];
    snprintf(msg, sizeof msg, fmt, a ? a : "", b ? b : "");
    *out_body = error_frame(body, CMCP_RPC_HEADER_MISMATCH, msg, NULL);
    return 400;
}

/* MCP-Protocol-Version names a revision this build does not serve. */
static int unsupported_version(const cmcp_json_t *body, const char *pv,
                               char **out_body) {
    cmcp_json_t *data = cmcp_json_new_object();
    if (data && (set_owned(data, "supported", cmcp_era_supported_versions()) ||
                 set_owned(data, "requested", cmcp_json_new_string(pv)))) {
        cmcp_json_free(data);
        data = NULL;
    }
    *out_body = error_frame(body, CMCP_RPC_UNSUPPORTED_PROTOCOL_VERSION,
                            "Unsupported protocol version", data);
    return 400;
}

static int needs_name(const char *method) {
    return strcmp(method, "tools/call") == 0 ||
           strcmp(method, "resources/read") == 0 ||
           strcmp(method, "prompts/get") == 0;
}

/* Mcp-Name against params.name / params.uri, after sentinel decoding. */
static int check_name(const cmcp_json_t *body, const cmcp_http_request_t *req,
                      const char *method, char **out_body) {
    const cmcp_json_t *params = field(body, "params");
    const char *want = str_field(params, "name");
    if (!want) want = str_field(params, "uri");
    const char *raw = cmcp_http_header_get(req, "Mcp-Name");
    if (!raw)
        return mismatch(body, out_body,
                        "Header mismatch: Mcp-Name header is required for %s%s",
                        method, NULL);
    char *got = cmcp_http_header_decode(raw);
    int status = 0;
    if (!got) {
        status = mismatch(body, out_body,
                          "Header mismatch: Mcp-Name header '%s' is not valid "
                          "base64%s", raw, NULL);
    } else if (!want || strcmp(got, want) != 0) {
        status = mismatch(body, out_body,
                          "Header mismatch: Mcp-Name header value '%s' does "
                          "not match body value '%s'", got, want);
    }
    free(got);
    return status;
}

int cmcp_http_modern_check(const cmcp_json_t *body,
                           const cmcp_http_request_t *req, char **out_body) {
    *out_body = NULL;
    const char *pv = cmcp_http_header_get(req, "MCP-Protocol-Version");
    if (!pv)
        return mismatch(body, out_body,
                        "Header mismatch: MCP-Protocol-Version header is "
                        "required%s%s", NULL, NULL);
    const char *body_pv = str_field(field(field(body, "params"), "_meta"),
                                    CMCP_META_PROTOCOL_VERSION);
    if (!body_pv || strcmp(pv, body_pv) != 0)
        return mismatch(body, out_body,
                        "Header mismatch: MCP-Protocol-Version header value "
                        "'%s' does not match body value '%s'", pv, body_pv);
    if (strcmp(pv, CMCP_PROTOCOL_VERSION_MODERN) != 0)
        return unsupported_version(body, pv, out_body);

    const char *method = str_field(body, "method");
    if (!method) return 0;          /* not a request: server.c rejects it */
    const char *mh = cmcp_http_header_get(req, "Mcp-Method");
    if (!mh)
        return mismatch(body, out_body,
                        "Header mismatch: Mcp-Method header is required%s%s",
                        NULL, NULL);
    if (strcmp(mh, method) != 0)
        return mismatch(body, out_body,
                        "Header mismatch: Mcp-Method header value '%s' does "
                        "not match body value '%s'", mh, method);
    return needs_name(method) ? check_name(body, req, method, out_body) : 0;
}

int cmcp_http_modern_status(const char *resp, size_t len) {
    cmcp_json_t *j = cmcp_json_parse(resp, len);
    const cmcp_json_t *code = field(field(j, "error"), "code");
    long long c = (code && code->type == CMCP_JSON_INT) ? cmcp_json_int(code) : 0;
    cmcp_json_free(j);
    if (c == CMCP_RPC_METHOD_NOT_FOUND) return 404;
    if (c == CMCP_RPC_HEADER_MISMATCH ||
        c == CMCP_RPC_MISSING_CLIENT_CAPABILITY ||
        c == CMCP_RPC_UNSUPPORTED_PROTOCOL_VERSION)
        return 400;
    return 200;
}

/* ---------------------------------------------------------------- */
/* Client side: request headers                                      */
/* ---------------------------------------------------------------- */

/* add("<name>: <encoded value>"). */
static int add_header(cmcp_http_line_fn add, void *ctx, const char *name,
                      const char *value) {
    char *enc = cmcp_http_header_encode(value);
    if (!enc) return CMCP_ENOMEM;
    size_t n = strlen(name) + 2 + strlen(enc) + 1;
    char *line = (char *)malloc(n);
    int rc = CMCP_ENOMEM;
    if (line) {
        snprintf(line, n, "%s: %s", name, enc);
        rc = add(ctx, line);
        free(line);
    }
    free(enc);
    return rc;
}

typedef struct {
    cmcp_http_line_fn add;
    void             *ctx;
} param_ctx_t;

static int emit_param(void *ctx, const char *name, const char *value) {
    param_ctx_t *pc = (param_ctx_t *)ctx;
    char hname[160];
    snprintf(hname, sizeof hname, "Mcp-Param-%s", name);
    return add_header(pc->add, pc->ctx, hname, value);
}

int cmcp_http_modern_request_headers(const cmcp_json_t *body,
                                     const cmcp_json_t *tool_schema,
                                     cmcp_http_line_fn add, void *ctx) {
    const cmcp_json_t *params = field(body, "params");
    const char *pv = str_field(field(params, "_meta"), CMCP_META_PROTOCOL_VERSION);
    const char *method = str_field(body, "method");
    int rc = CMCP_OK;
    if (pv) rc = add_header(add, ctx, "MCP-Protocol-Version", pv);
    if (rc == CMCP_OK && method && field(body, "id"))
        rc = add_header(add, ctx, "Mcp-Method", method);
    if (rc != CMCP_OK || !method || !needs_name(method)) return rc;

    const char *name = str_field(params, "name");
    if (!name) name = str_field(params, "uri");
    if (name) rc = add_header(add, ctx, "Mcp-Name", name);
    if (rc == CMCP_OK && tool_schema && strcmp(method, "tools/call") == 0) {
        param_ctx_t pc = {add, ctx};
        rc = cmcp_xheader_collect(tool_schema, field(params, "arguments"),
                                  emit_param, &pc);
    }
    return rc;
}
