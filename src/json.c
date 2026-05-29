#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <math.h>
#include <pthread.h>
#include "cmcp_json.h"

/* ===========================================================================
 * Parser DoS caps (Tier 6 axis 6.5.3)
 * ===========================================================================
 *
 * The parser is recursive descent over arbitrary peer-controlled bytes.
 * Without bounds, a hostile peer can craft `{{{...{1}...}}}` (stack
 * exhaustion via deep recursion) or `[1,1,1,...]` (memory blowup via
 * huge container). Two env-tunable caps, snapshotted once via
 * pthread_once and read by every parse call after that — zero
 * per-call getenv() cost on the hot path.
 *
 *   CMCP_JSON_MAX_DEPTH     default 64    (<=0 disables)
 *   CMCP_JSON_MAX_ELEMENTS  default 65536 (<=0 disables)
 *
 * On trip the parser returns NULL (caller surfaces CMCP_EPARSE → -32700).
 */
#define CMCP_JSON_MAX_DEPTH_DEFAULT     64
#define CMCP_JSON_MAX_ELEMENTS_DEFAULT  65536

static int g_json_max_depth    = CMCP_JSON_MAX_DEPTH_DEFAULT;
static int g_json_max_elements = CMCP_JSON_MAX_ELEMENTS_DEFAULT;
static pthread_once_t g_json_caps_once = PTHREAD_ONCE_INIT;

static void json_caps_init(void) {
    const char *d = getenv("CMCP_JSON_MAX_DEPTH");
    if (d && *d) {
        char *end; long v = strtol(d, &end, 10);
        if (end != d && *end == '\0') g_json_max_depth = (int)v;
    }
    const char *e = getenv("CMCP_JSON_MAX_ELEMENTS");
    if (e && *e) {
        char *end; long v = strtol(e, &end, 10);
        if (end != e && *end == '\0') g_json_max_elements = (int)v;
    }
}

/* ===========================================================================
 * Utility: UTF-8 emission for parsed \uXXXX escapes
 * ======================================================================== */

static int utf8_emit(unsigned cp, char *out) {
    if (cp <= 0x7F) {
        out[0] = (char)cp;
        return 1;
    }
    if (cp <= 0x7FF) {
        out[0] = (char)(0xC0 | (cp >> 6));
        out[1] = (char)(0x80 | (cp & 0x3F));
        return 2;
    }
    if (cp <= 0xFFFF) {
        out[0] = (char)(0xE0 | (cp >> 12));
        out[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
        out[2] = (char)(0x80 | (cp & 0x3F));
        return 3;
    }
    if (cp <= 0x10FFFF) {
        out[0] = (char)(0xF0 | (cp >> 18));
        out[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
        out[2] = (char)(0x80 | ((cp >> 6) & 0x3F));
        out[3] = (char)(0x80 | (cp & 0x3F));
        return 4;
    }
    return -1;
}

static int hex_nibble(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return 10 + (c - 'a');
    if (c >= 'A' && c <= 'F') return 10 + (c - 'A');
    return -1;
}

/* ===========================================================================
 * String escape (replaces cRAG's util.c::json_escape)
 * ======================================================================== */

int cmcp_json_escape(const char *in, char *out, size_t out_sz) {
    if (!in || !out || out_sz == 0) return -1;
    size_t wi = 0;
#define NEED(n) do { if (wi + (n) + 1 > out_sz) return -1; } while (0)
    for (const char *p = in; *p; p++) {
        unsigned char c = (unsigned char)*p;
        if      (c == '"')  { NEED(2); out[wi++] = '\\'; out[wi++] = '"';  }
        else if (c == '\\') { NEED(2); out[wi++] = '\\'; out[wi++] = '\\'; }
        else if (c == '\n') { NEED(2); out[wi++] = '\\'; out[wi++] = 'n';  }
        else if (c == '\r') { NEED(2); out[wi++] = '\\'; out[wi++] = 'r';  }
        else if (c == '\t') { NEED(2); out[wi++] = '\\'; out[wi++] = 't';  }
        else if (c == '\b') { NEED(2); out[wi++] = '\\'; out[wi++] = 'b';  }
        else if (c == '\f') { NEED(2); out[wi++] = '\\'; out[wi++] = 'f';  }
        else if (c < 0x20) {
            NEED(6);
            snprintf(out + wi, 7, "\\u%04x", c);
            wi += 6;
        } else {
            NEED(1); out[wi++] = (char)c;
        }
    }
#undef NEED
    out[wi] = '\0';
    return (int)wi;
}

char *cmcp_json_escape_dup(const char *in) {
    if (!in) return NULL;
    size_t n = strlen(in);
    size_t out_sz = n * 6 + 1;
    char *out = malloc(out_sz);
    if (!out) return NULL;
    if (cmcp_json_escape(in, out, out_sz) < 0) { free(out); return NULL; }
    return out;
}

/* ===========================================================================
 * Constructors
 * ======================================================================== */

static cmcp_json_t *node_new(cmcp_json_type_t t) {
    cmcp_json_t *v = calloc(1, sizeof *v);
    if (!v) return NULL;
    v->type = t;
    return v;
}

cmcp_json_t *cmcp_json_new_null(void)        { return node_new(CMCP_JSON_NULL); }
cmcp_json_t *cmcp_json_new_array(void)       { return node_new(CMCP_JSON_ARRAY); }
cmcp_json_t *cmcp_json_new_object(void)      { return node_new(CMCP_JSON_OBJECT); }

cmcp_json_t *cmcp_json_new_bool(int b) {
    cmcp_json_t *v = node_new(CMCP_JSON_BOOL);
    if (v) v->b = b ? 1 : 0;
    return v;
}

cmcp_json_t *cmcp_json_new_int(long long i) {
    cmcp_json_t *v = node_new(CMCP_JSON_INT);
    if (v) v->i = i;
    return v;
}

cmcp_json_t *cmcp_json_new_double(double d) {
    cmcp_json_t *v = node_new(CMCP_JSON_DOUBLE);
    if (v) v->d = d;
    return v;
}

cmcp_json_t *cmcp_json_new_string_n(const char *s, size_t n) {
    if (!s) return NULL;
    cmcp_json_t *v = node_new(CMCP_JSON_STRING);
    if (!v) return NULL;
    v->str.s = malloc(n + 1);
    if (!v->str.s) { free(v); return NULL; }
    memcpy(v->str.s, s, n);
    v->str.s[n] = '\0';
    v->str.len = n;
    return v;
}

cmcp_json_t *cmcp_json_new_string(const char *s) {
    if (!s) return NULL;
    return cmcp_json_new_string_n(s, strlen(s));
}

/* ===========================================================================
 * Builders
 * ======================================================================== */

static int arr_grow(cmcp_json_t *arr, size_t want) {
    if (want <= arr->arr.cap) return 0;
    size_t cap = arr->arr.cap ? arr->arr.cap : 4;
    while (cap < want) cap *= 2;
    cmcp_json_t **n = realloc(arr->arr.items, cap * sizeof *n);
    if (!n) return -1;
    arr->arr.items = n;
    arr->arr.cap   = cap;
    return 0;
}

int cmcp_json_array_append(cmcp_json_t *arr, cmcp_json_t *v) {
    if (!arr || arr->type != CMCP_JSON_ARRAY || !v) return -1;
    if (arr_grow(arr, arr->arr.len + 1) < 0) return -1;
    arr->arr.items[arr->arr.len++] = v;
    return 0;
}

static int obj_grow(cmcp_json_t *obj, size_t want) {
    if (want <= obj->obj.cap) return 0;
    size_t cap = obj->obj.cap ? obj->obj.cap : 4;
    while (cap < want) cap *= 2;
    char         **nk = realloc(obj->obj.keys,     cap * sizeof *nk);
    if (!nk) return -1;
    obj->obj.keys = nk;
    size_t        *nl = realloc(obj->obj.key_lens, cap * sizeof *nl);
    if (!nl) return -1;
    obj->obj.key_lens = nl;
    cmcp_json_t  **nv = realloc(obj->obj.values,   cap * sizeof *nv);
    if (!nv) return -1;
    obj->obj.values = nv;
    obj->obj.cap = cap;
    return 0;
}

int cmcp_json_object_set_n(cmcp_json_t *obj, const char *key, size_t key_len,
                           cmcp_json_t *v) {
    if (!obj || obj->type != CMCP_JSON_OBJECT || !key || !v) return -1;

    /* Replace if key already present. */
    for (size_t k = 0; k < obj->obj.len; k++) {
        if (obj->obj.key_lens[k] == key_len &&
            memcmp(obj->obj.keys[k], key, key_len) == 0) {
            cmcp_json_free(obj->obj.values[k]);
            obj->obj.values[k] = v;
            return 0;
        }
    }

    if (obj_grow(obj, obj->obj.len + 1) < 0) return -1;
    char *kdup = malloc(key_len + 1);
    if (!kdup) return -1;
    memcpy(kdup, key, key_len);
    kdup[key_len] = '\0';

    obj->obj.keys[obj->obj.len]     = kdup;
    obj->obj.key_lens[obj->obj.len] = key_len;
    obj->obj.values[obj->obj.len]   = v;
    obj->obj.len++;
    return 0;
}

int cmcp_json_object_set(cmcp_json_t *obj, const char *key, cmcp_json_t *v) {
    if (!key) return -1;
    return cmcp_json_object_set_n(obj, key, strlen(key), v);
}

/* ===========================================================================
 * Accessors
 * ======================================================================== */

const cmcp_json_t *cmcp_json_object_get(const cmcp_json_t *obj, const char *key) {
    if (!obj || obj->type != CMCP_JSON_OBJECT || !key) return NULL;
    size_t kl = strlen(key);
    for (size_t k = 0; k < obj->obj.len; k++) {
        if (obj->obj.key_lens[k] == kl &&
            memcmp(obj->obj.keys[k], key, kl) == 0) {
            return obj->obj.values[k];
        }
    }
    return NULL;
}

const cmcp_json_t *cmcp_json_array_at(const cmcp_json_t *arr, size_t i) {
    if (!arr || arr->type != CMCP_JSON_ARRAY || i >= arr->arr.len) return NULL;
    return arr->arr.items[i];
}

size_t cmcp_json_array_len(const cmcp_json_t *arr) {
    return (arr && arr->type == CMCP_JSON_ARRAY) ? arr->arr.len : 0;
}

size_t cmcp_json_object_len(const cmcp_json_t *obj) {
    return (obj && obj->type == CMCP_JSON_OBJECT) ? obj->obj.len : 0;
}

const char *cmcp_json_string(const cmcp_json_t *v) {
    return (v && v->type == CMCP_JSON_STRING) ? v->str.s : NULL;
}

size_t cmcp_json_string_len(const cmcp_json_t *v) {
    return (v && v->type == CMCP_JSON_STRING) ? v->str.len : 0;
}

long long cmcp_json_int(const cmcp_json_t *v) {
    if (!v) return 0;
    if (v->type == CMCP_JSON_INT)    return v->i;
    if (v->type == CMCP_JSON_DOUBLE) return (long long)v->d;
    return 0;
}

double cmcp_json_double(const cmcp_json_t *v) {
    if (!v) return 0.0;
    if (v->type == CMCP_JSON_DOUBLE) return v->d;
    if (v->type == CMCP_JSON_INT)    return (double)v->i;
    return 0.0;
}

int cmcp_json_bool(const cmcp_json_t *v) {
    return (v && v->type == CMCP_JSON_BOOL) ? v->b : 0;
}

int cmcp_json_is_null(const cmcp_json_t *v) {
    return v && v->type == CMCP_JSON_NULL;
}

/* ===========================================================================
 * Credential redactor (Tier 6 axis 6.5.4)
 * ===========================================================================
 *
 * Walks an arbitrary JSON tree and replaces values under sensitive
 * keys with the literal string "[REDACTED]". The match is on the
 * normalized key (lowercase, alphanumeric-only) and is a substring
 * test against a fixed list — so `api_key`, `apiKey`, `API-Key`,
 * `myApiKey`, `customer_secret`, `Authorization` all hit.
 *
 * Only key-based matching: scalar strings that *look* like tokens
 * (long random sequences, JWTs, etc.) are not detected. Heuristic
 * scrubbing would either miss real tokens or false-positive on
 * legitimate payloads; key-based is the documented contract.
 *
 * On allocation failure during replacement, the original value stays
 * — best-effort, never aborts.
 */

static int is_sensitive_key(const char *key, size_t key_len) {
    if (!key || key_len == 0) return 0;
    char norm[64];
    size_t n = 0;
    for (size_t i = 0; i < key_len && n < sizeof(norm) - 1; i++) {
        char c = key[i];
        if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
        if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9'))
            norm[n++] = c;
    }
    norm[n] = '\0';
    if (n == 0) return 0;

    static const char *const patterns[] = {
        "password", "passwd", "token", "secret",
        "apikey", "authorization", "bearer", "credential",
        NULL,
    };
    for (const char *const *p = patterns; *p; p++) {
        if (strstr(norm, *p)) return 1;
    }
    return 0;
}

void cmcp_json_redact(cmcp_json_t *v) {
    if (!v) return;
    if (v->type == CMCP_JSON_ARRAY) {
        for (size_t i = 0; i < v->arr.len; i++)
            cmcp_json_redact(v->arr.items[i]);
        return;
    }
    if (v->type != CMCP_JSON_OBJECT) return;
    for (size_t i = 0; i < v->obj.len; i++) {
        if (is_sensitive_key(v->obj.keys[i], v->obj.key_lens[i])) {
            cmcp_json_t *rep = cmcp_json_new_string("[REDACTED]");
            if (rep) {
                cmcp_json_free(v->obj.values[i]);
                v->obj.values[i] = rep;
            }
            /* Do not recurse into the replaced (or unchanged) value. */
        } else {
            cmcp_json_redact(v->obj.values[i]);
        }
    }
}

/* ===========================================================================
 * Free / clone / equal
 * ======================================================================== */

void cmcp_json_free(cmcp_json_t *v) {
    if (!v) return;
    switch (v->type) {
    case CMCP_JSON_STRING:
        free(v->str.s);
        break;
    case CMCP_JSON_ARRAY:
        for (size_t k = 0; k < v->arr.len; k++) cmcp_json_free(v->arr.items[k]);
        free(v->arr.items);
        break;
    case CMCP_JSON_OBJECT:
        for (size_t k = 0; k < v->obj.len; k++) {
            free(v->obj.keys[k]);
            cmcp_json_free(v->obj.values[k]);
        }
        free(v->obj.keys);
        free(v->obj.key_lens);
        free(v->obj.values);
        break;
    default:
        break;
    }
    free(v);
}

cmcp_json_t *cmcp_json_clone(const cmcp_json_t *v) {
    if (!v) return NULL;
    switch (v->type) {
    case CMCP_JSON_NULL:   return cmcp_json_new_null();
    case CMCP_JSON_BOOL:   return cmcp_json_new_bool(v->b);
    case CMCP_JSON_INT:    return cmcp_json_new_int(v->i);
    case CMCP_JSON_DOUBLE: return cmcp_json_new_double(v->d);
    case CMCP_JSON_STRING: return cmcp_json_new_string_n(v->str.s, v->str.len);
    case CMCP_JSON_ARRAY: {
        cmcp_json_t *c = cmcp_json_new_array();
        if (!c) return NULL;
        for (size_t k = 0; k < v->arr.len; k++) {
            cmcp_json_t *child = cmcp_json_clone(v->arr.items[k]);
            if (!child || cmcp_json_array_append(c, child) < 0) {
                cmcp_json_free(child);
                cmcp_json_free(c);
                return NULL;
            }
        }
        return c;
    }
    case CMCP_JSON_OBJECT: {
        cmcp_json_t *c = cmcp_json_new_object();
        if (!c) return NULL;
        for (size_t k = 0; k < v->obj.len; k++) {
            cmcp_json_t *child = cmcp_json_clone(v->obj.values[k]);
            if (!child ||
                cmcp_json_object_set_n(c, v->obj.keys[k],
                                       v->obj.key_lens[k], child) < 0) {
                cmcp_json_free(child);
                cmcp_json_free(c);
                return NULL;
            }
        }
        return c;
    }
    }
    return NULL;
}

int cmcp_json_equal(const cmcp_json_t *a, const cmcp_json_t *b) {
    if (a == b) return 1;
    if (!a || !b) return 0;
    if (a->type != b->type) return 0;
    switch (a->type) {
    case CMCP_JSON_NULL:   return 1;
    case CMCP_JSON_BOOL:   return a->b == b->b;
    case CMCP_JSON_INT:    return a->i == b->i;
    case CMCP_JSON_DOUBLE: return a->d == b->d;
    case CMCP_JSON_STRING:
        return a->str.len == b->str.len &&
               memcmp(a->str.s, b->str.s, a->str.len) == 0;
    case CMCP_JSON_ARRAY:
        if (a->arr.len != b->arr.len) return 0;
        for (size_t k = 0; k < a->arr.len; k++)
            if (!cmcp_json_equal(a->arr.items[k], b->arr.items[k])) return 0;
        return 1;
    case CMCP_JSON_OBJECT:
        if (a->obj.len != b->obj.len) return 0;
        /* Order-independent: every key in a must appear in b with equal value. */
        for (size_t k = 0; k < a->obj.len; k++) {
            int found = 0;
            for (size_t j = 0; j < b->obj.len; j++) {
                if (a->obj.key_lens[k] == b->obj.key_lens[j] &&
                    memcmp(a->obj.keys[k], b->obj.keys[j],
                           a->obj.key_lens[k]) == 0) {
                    if (!cmcp_json_equal(a->obj.values[k], b->obj.values[j]))
                        return 0;
                    found = 1;
                    break;
                }
            }
            if (!found) return 0;
        }
        return 1;
    }
    return 0;
}

/* ===========================================================================
 * Parser (recursive descent)
 * ======================================================================== */

typedef struct {
    const char *src;
    size_t      pos;
    size_t      len;
    int         depth;          /* current recursion depth */
    int         max_depth;      /* snapshot of g_json_max_depth; <=0 disables */
    int         max_elements;   /* snapshot of g_json_max_elements; <=0 disables */
} parser_t;

static cmcp_json_t *parse_value(parser_t *p);

static void skip_ws(parser_t *p) {
    while (p->pos < p->len) {
        char c = p->src[p->pos];
        if (c == ' ' || c == '\t' || c == '\n' || c == '\r') p->pos++;
        else break;
    }
}

static int peek(parser_t *p) {
    return p->pos < p->len ? (unsigned char)p->src[p->pos] : -1;
}

static int eat(parser_t *p, char c) {
    if (p->pos >= p->len || p->src[p->pos] != c) return -1;
    p->pos++;
    return 0;
}

static int eat_literal(parser_t *p, const char *lit) {
    size_t n = strlen(lit);
    if (p->pos + n > p->len) return -1;
    if (memcmp(p->src + p->pos, lit, n) != 0) return -1;
    p->pos += n;
    return 0;
}

/* Parse a JSON string (already past opening "). Caller frees *out. */
static int parse_string_body(parser_t *p, char **out, size_t *out_len) {
    /* Bound: parsed string is at most as long as input remaining. */
    size_t cap = 16, len = 0;
    char *buf = malloc(cap);
    if (!buf) return -1;
#define APPEND(c) do {                                                         \
    if (len + 1 >= cap) {                                                      \
        size_t ncap = cap * 2;                                                 \
        char *nb = realloc(buf, ncap);                                         \
        if (!nb) { free(buf); return -1; }                                     \
        buf = nb; cap = ncap;                                                  \
    }                                                                          \
    buf[len++] = (c);                                                          \
} while (0)

    while (p->pos < p->len) {
        unsigned char c = (unsigned char)p->src[p->pos++];
        if (c == '"') {
            buf[len] = '\0';
            *out = buf;
            *out_len = len;
            return 0;
        }
        if (c == '\\') {
            if (p->pos >= p->len) goto fail;
            char esc = p->src[p->pos++];
            switch (esc) {
            case '"':  APPEND('"');  break;
            case '\\': APPEND('\\'); break;
            case '/':  APPEND('/');  break;
            case 'b':  APPEND('\b'); break;
            case 'f':  APPEND('\f'); break;
            case 'n':  APPEND('\n'); break;
            case 'r':  APPEND('\r'); break;
            case 't':  APPEND('\t'); break;
            case 'u': {
                if (p->pos + 4 > p->len) goto fail;
                int h0 = hex_nibble(p->src[p->pos+0]);
                int h1 = hex_nibble(p->src[p->pos+1]);
                int h2 = hex_nibble(p->src[p->pos+2]);
                int h3 = hex_nibble(p->src[p->pos+3]);
                if (h0 < 0 || h1 < 0 || h2 < 0 || h3 < 0) goto fail;
                p->pos += 4;
                unsigned cp = (unsigned)((h0 << 12) | (h1 << 8) | (h2 << 4) | h3);
                /* Surrogate pair */
                if (cp >= 0xD800 && cp <= 0xDBFF) {
                    if (p->pos + 6 > p->len) goto fail;
                    if (p->src[p->pos] != '\\' || p->src[p->pos+1] != 'u') goto fail;
                    int g0 = hex_nibble(p->src[p->pos+2]);
                    int g1 = hex_nibble(p->src[p->pos+3]);
                    int g2 = hex_nibble(p->src[p->pos+4]);
                    int g3 = hex_nibble(p->src[p->pos+5]);
                    if (g0 < 0 || g1 < 0 || g2 < 0 || g3 < 0) goto fail;
                    unsigned low = (unsigned)((g0 << 12) | (g1 << 8) | (g2 << 4) | g3);
                    if (low < 0xDC00 || low > 0xDFFF) goto fail;
                    p->pos += 6;
                    cp = 0x10000u + (((cp - 0xD800u) << 10) | (low - 0xDC00u));
                } else if (cp >= 0xDC00 && cp <= 0xDFFF) {
                    /* lone low surrogate */
                    goto fail;
                }
                char ub[4];
                int n = utf8_emit(cp, ub);
                if (n < 0) goto fail;
                for (int k = 0; k < n; k++) APPEND(ub[k]);
                break;
            }
            default: goto fail;
            }
        } else if (c < 0x20) {
            /* Unescaped control char in string is a JSON parse error. */
            goto fail;
        } else {
            APPEND((char)c);
        }
    }
fail:
    free(buf);
    return -1;
#undef APPEND
}

static cmcp_json_t *parse_string(parser_t *p) {
    if (eat(p, '"') < 0) return NULL;
    char *s = NULL;
    size_t slen = 0;
    if (parse_string_body(p, &s, &slen) < 0) return NULL;
    cmcp_json_t *v = node_new(CMCP_JSON_STRING);
    if (!v) { free(s); return NULL; }
    v->str.s = s;
    v->str.len = slen;
    return v;
}

static cmcp_json_t *parse_number(parser_t *p) {
    size_t start = p->pos;
    int is_float = 0;

    if (peek(p) == '-') p->pos++;
    if (peek(p) == '0') {
        p->pos++;
    } else if (peek(p) >= '1' && peek(p) <= '9') {
        while (peek(p) >= '0' && peek(p) <= '9') p->pos++;
    } else {
        return NULL;
    }

    if (peek(p) == '.') {
        is_float = 1;
        p->pos++;
        if (!(peek(p) >= '0' && peek(p) <= '9')) return NULL;
        while (peek(p) >= '0' && peek(p) <= '9') p->pos++;
    }
    if (peek(p) == 'e' || peek(p) == 'E') {
        is_float = 1;
        p->pos++;
        if (peek(p) == '+' || peek(p) == '-') p->pos++;
        if (!(peek(p) >= '0' && peek(p) <= '9')) return NULL;
        while (peek(p) >= '0' && peek(p) <= '9') p->pos++;
    }

    /* strtoll / strtod want a NUL-terminated buffer. Copy slice. */
    size_t n = p->pos - start;
    char tmp[64];
    if (n >= sizeof tmp) return NULL;
    memcpy(tmp, p->src + start, n);
    tmp[n] = '\0';

    if (is_float) {
        char *end;
        double d = strtod(tmp, &end);
        if (end == tmp) return NULL;
        return cmcp_json_new_double(d);
    } else {
        char *end;
        long long i = strtoll(tmp, &end, 10);
        if (end == tmp) return NULL;
        return cmcp_json_new_int(i);
    }
}

static cmcp_json_t *parse_array(parser_t *p) {
    if (eat(p, '[') < 0) return NULL;
    if (p->max_depth > 0 && p->depth >= p->max_depth) return NULL;
    p->depth++;
    cmcp_json_t *arr = cmcp_json_new_array();
    if (!arr) { p->depth--; return NULL; }
    skip_ws(p);
    if (peek(p) == ']') { p->pos++; p->depth--; return arr; }
    int n = 0;
    for (;;) {
        skip_ws(p);
        cmcp_json_t *child = parse_value(p);
        if (!child) { cmcp_json_free(arr); p->depth--; return NULL; }
        if (cmcp_json_array_append(arr, child) < 0) {
            cmcp_json_free(child);
            cmcp_json_free(arr);
            p->depth--;
            return NULL;
        }
        n++;
        if (p->max_elements > 0 && n > p->max_elements) {
            cmcp_json_free(arr); p->depth--; return NULL;
        }
        skip_ws(p);
        int c = peek(p);
        if (c == ',') { p->pos++; continue; }
        if (c == ']') { p->pos++; p->depth--; return arr; }
        cmcp_json_free(arr);
        p->depth--;
        return NULL;
    }
}

static cmcp_json_t *parse_object(parser_t *p) {
    if (eat(p, '{') < 0) return NULL;
    if (p->max_depth > 0 && p->depth >= p->max_depth) return NULL;
    p->depth++;
    cmcp_json_t *obj = cmcp_json_new_object();
    if (!obj) { p->depth--; return NULL; }
    skip_ws(p);
    if (peek(p) == '}') { p->pos++; p->depth--; return obj; }
    int n = 0;
    for (;;) {
        skip_ws(p);
        if (eat(p, '"') < 0) { cmcp_json_free(obj); p->depth--; return NULL; }
        char *key = NULL;
        size_t key_len = 0;
        if (parse_string_body(p, &key, &key_len) < 0) {
            cmcp_json_free(obj); p->depth--; return NULL;
        }
        skip_ws(p);
        if (eat(p, ':') < 0) { free(key); cmcp_json_free(obj); p->depth--; return NULL; }
        skip_ws(p);
        cmcp_json_t *child = parse_value(p);
        if (!child) { free(key); cmcp_json_free(obj); p->depth--; return NULL; }
        if (cmcp_json_object_set_n(obj, key, key_len, child) < 0) {
            free(key); cmcp_json_free(child); cmcp_json_free(obj);
            p->depth--;
            return NULL;
        }
        free(key);
        n++;
        if (p->max_elements > 0 && n > p->max_elements) {
            cmcp_json_free(obj); p->depth--; return NULL;
        }
        skip_ws(p);
        int c = peek(p);
        if (c == ',') { p->pos++; continue; }
        if (c == '}') { p->pos++; p->depth--; return obj; }
        cmcp_json_free(obj);
        p->depth--;
        return NULL;
    }
}

static cmcp_json_t *parse_value(parser_t *p) {
    skip_ws(p);
    int c = peek(p);
    if (c < 0) return NULL;
    switch (c) {
    case '{': return parse_object(p);
    case '[': return parse_array(p);
    case '"': return parse_string(p);
    case 't': if (eat_literal(p, "true")  == 0) return cmcp_json_new_bool(1); return NULL;
    case 'f': if (eat_literal(p, "false") == 0) return cmcp_json_new_bool(0); return NULL;
    case 'n': if (eat_literal(p, "null")  == 0) return cmcp_json_new_null();  return NULL;
    case '-':
    case '0': case '1': case '2': case '3': case '4':
    case '5': case '6': case '7': case '8': case '9':
        return parse_number(p);
    default: return NULL;
    }
}

cmcp_json_t *cmcp_json_parse(const char *text, size_t len) {
    if (!text) return NULL;
    pthread_once(&g_json_caps_once, json_caps_init);
    parser_t p = {
        .src = text, .pos = 0, .len = len,
        .depth = 0,
        .max_depth    = g_json_max_depth,
        .max_elements = g_json_max_elements,
    };
    cmcp_json_t *v = parse_value(&p);
    if (!v) return NULL;
    skip_ws(&p);
    if (p.pos != p.len) {
        cmcp_json_free(v);
        return NULL;
    }
    return v;
}

cmcp_json_t *cmcp_json_parse_cstr(const char *text) {
    return text ? cmcp_json_parse(text, strlen(text)) : NULL;
}

/* ===========================================================================
 * Emitter
 * ======================================================================== */

typedef struct {
    char  *buf;
    size_t len;
    size_t cap;
} emit_t;

static int emit_reserve(emit_t *e, size_t want) {
    if (e->len + want + 1 <= e->cap) return 0;
    size_t cap = e->cap ? e->cap : 64;
    while (cap < e->len + want + 1) cap *= 2;
    char *nb = realloc(e->buf, cap);
    if (!nb) return -1;
    e->buf = nb;
    e->cap = cap;
    return 0;
}

static int emit_raw(emit_t *e, const char *s, size_t n) {
    if (emit_reserve(e, n) < 0) return -1;
    memcpy(e->buf + e->len, s, n);
    e->len += n;
    e->buf[e->len] = '\0';
    return 0;
}

static int emit_char(emit_t *e, char c) {
    return emit_raw(e, &c, 1);
}

static int emit_quoted(emit_t *e, const char *s, size_t n) {
    if (emit_char(e, '"') < 0) return -1;
    /* Batch runs of "normal" characters (printable ASCII that isn't
     * `"` or `\`) into one emit_raw call rather than emitting each
     * one through emit_char. For typical strings the savings are
     * substantial — profile (6.6.3) showed emit_raw single-byte
     * calls accounting for ~13% of CPU on bench_server_inline. */
    size_t run = 0;
    for (size_t k = 0; k < n; k++) {
        unsigned char c = (unsigned char)s[k];
        if (c >= 0x20 && c != '"' && c != '\\') { run++; continue; }
        if (run > 0) {
            if (emit_raw(e, s + k - run, run) < 0) return -1;
            run = 0;
        }
        if (c == '"')       { if (emit_raw(e, "\\\"", 2) < 0) return -1; }
        else if (c == '\\') { if (emit_raw(e, "\\\\", 2) < 0) return -1; }
        else if (c == '\n') { if (emit_raw(e, "\\n",  2) < 0) return -1; }
        else if (c == '\r') { if (emit_raw(e, "\\r",  2) < 0) return -1; }
        else if (c == '\t') { if (emit_raw(e, "\\t",  2) < 0) return -1; }
        else if (c == '\b') { if (emit_raw(e, "\\b",  2) < 0) return -1; }
        else if (c == '\f') { if (emit_raw(e, "\\f",  2) < 0) return -1; }
        else {
            char tmp[8];
            int wn = snprintf(tmp, sizeof tmp, "\\u%04x", c);
            if (emit_raw(e, tmp, (size_t)wn) < 0) return -1;
        }
    }
    if (run > 0) {
        if (emit_raw(e, s + n - run, run) < 0) return -1;
    }
    return emit_char(e, '"');
}

static int emit_value(emit_t *e, const cmcp_json_t *v, int stable);

static int cmp_obj_idx(const void *aa, const void *bb, void *ctx) {
    const cmcp_json_t *obj = ctx;
    size_t a = *(const size_t *)aa, b = *(const size_t *)bb;
    size_t la = obj->obj.key_lens[a], lb = obj->obj.key_lens[b];
    size_t m = la < lb ? la : lb;
    int r = memcmp(obj->obj.keys[a], obj->obj.keys[b], m);
    if (r != 0) return r;
    if (la < lb) return -1;
    if (la > lb) return  1;
    return 0;
}

/* Portable qsort_r is non-standard; do a small insertion sort on indices.
   Object key counts in MCP messages are tiny (< 32). */
static void sort_obj_indices(size_t *idx, size_t n, const cmcp_json_t *obj) {
    for (size_t i = 1; i < n; i++) {
        size_t v = idx[i];
        size_t j = i;
        while (j > 0 && cmp_obj_idx(&v, &idx[j-1], (void *)obj) < 0) {
            idx[j] = idx[j-1];
            j--;
        }
        idx[j] = v;
    }
}

static int emit_object(emit_t *e, const cmcp_json_t *v, int stable) {
    if (emit_char(e, '{') < 0) return -1;
    size_t n = v->obj.len;
    size_t *idx = NULL;
    if (n > 0) {
        idx = malloc(n * sizeof *idx);
        if (!idx) return -1;
        for (size_t k = 0; k < n; k++) idx[k] = k;
        if (stable) sort_obj_indices(idx, n, v);
    }
    for (size_t k = 0; k < n; k++) {
        if (k > 0 && emit_char(e, ',') < 0) { free(idx); return -1; }
        size_t i = idx[k];
        if (emit_quoted(e, v->obj.keys[i], v->obj.key_lens[i]) < 0) {
            free(idx); return -1;
        }
        if (emit_char(e, ':') < 0) { free(idx); return -1; }
        if (emit_value(e, v->obj.values[i], stable) < 0) {
            free(idx); return -1;
        }
    }
    free(idx);
    return emit_char(e, '}');
}

static int emit_value(emit_t *e, const cmcp_json_t *v, int stable) {
    if (!v) return emit_raw(e, "null", 4);
    switch (v->type) {
    case CMCP_JSON_NULL: return emit_raw(e, "null", 4);
    case CMCP_JSON_BOOL:
        return v->b ? emit_raw(e, "true", 4) : emit_raw(e, "false", 5);
    case CMCP_JSON_INT: {
        char tmp[32];
        int n = snprintf(tmp, sizeof tmp, "%lld", v->i);
        return emit_raw(e, tmp, (size_t)n);
    }
    case CMCP_JSON_DOUBLE: {
        char tmp[32];
        int n;
        if (isnan(v->d) || isinf(v->d)) {
            /* JSON has no NaN/Inf — emit null per common convention. */
            return emit_raw(e, "null", 4);
        }
        n = snprintf(tmp, sizeof tmp, "%.17g", v->d);
        return emit_raw(e, tmp, (size_t)n);
    }
    case CMCP_JSON_STRING:
        return emit_quoted(e, v->str.s, v->str.len);
    case CMCP_JSON_ARRAY:
        if (emit_char(e, '[') < 0) return -1;
        for (size_t k = 0; k < v->arr.len; k++) {
            if (k > 0 && emit_char(e, ',') < 0) return -1;
            if (emit_value(e, v->arr.items[k], stable) < 0) return -1;
        }
        return emit_char(e, ']');
    case CMCP_JSON_OBJECT:
        return emit_object(e, v, stable);
    }
    return -1;
}

static char *emit_root(const cmcp_json_t *v, int stable) {
    emit_t e = {0};
    if (emit_value(&e, v, stable) < 0) {
        free(e.buf);
        return NULL;
    }
    if (!e.buf) {
        /* Empty input case — return an allocated "null" so callers always get
           a freeable buffer. */
        e.buf = malloc(5);
        if (e.buf) memcpy(e.buf, "null", 5);
    }
    return e.buf;
}

char *cmcp_json_emit(const cmcp_json_t *v)        { return emit_root(v, 0); }
char *cmcp_json_emit_stable(const cmcp_json_t *v) { return emit_root(v, 1); }
