/**
 * @file cmcp_json.h
 * @brief Hand-rolled JSON value tree, parser, and emitter.
 *
 * Library-wide JSON representation. Every JSON-RPC message that
 * crosses a transport boundary is parsed into / built from a tree of
 * `cmcp_json_t` nodes. The parser is strict (RFC 8259 subset — no
 * trailing commas, no comments, no NaN/Inf) and the emitter has a
 * `_stable` variant that orders object keys deterministically for
 * test fixtures and replay gates.
 *
 * Memory ownership: every `cmcp_json_new_*` returns a freshly
 * allocated node the caller owns. `cmcp_json_array_append` and
 * `cmcp_json_object_set` **take ownership** of the value pointer
 * (the container will free it). `cmcp_json_free` recursively frees
 * a node and everything it contains.
 */
#ifndef CMCP_JSON_H
#define CMCP_JSON_H

#include <stddef.h>

/** @brief Tag of a JSON value tree node. */
typedef enum {
    CMCP_JSON_NULL,     /**< `null` */
    CMCP_JSON_BOOL,     /**< `true` / `false` */
    CMCP_JSON_INT,      /**< Integer that fit in `long long`. */
    CMCP_JSON_DOUBLE,   /**< Fractional or out-of-`int64` numeric. */
    CMCP_JSON_STRING,   /**< UTF-8 string (length-prefixed; embedded NULs allowed). */
    CMCP_JSON_ARRAY,    /**< Ordered list of values. */
    CMCP_JSON_OBJECT,   /**< Key/value bag (insertion order preserved). */
} cmcp_json_type_t;

/** @brief Opaque-by-convention JSON tree node. Treat as opaque except
 *  via the accessor functions below; struct layout is exposed for
 *  efficient construction in hot paths only. */
typedef struct cmcp_json cmcp_json_t;

struct cmcp_json {
    cmcp_json_type_t type;
    union {
        int       b;
        long long i;
        double    d;
        struct {
            char  *s;
            size_t len;
        } str;
        struct {
            cmcp_json_t **items;
            size_t        len;
            size_t        cap;
        } arr;
        struct {
            char         **keys;
            size_t        *key_lens;
            cmcp_json_t  **values;
            size_t         len;
            size_t         cap;
        } obj;
    };
};

/** @name Constructors
 *  Allocate a fresh node. Caller owns the returned pointer. Strings
 *  are copied in. NULL on allocation failure.
 *  @{ */
cmcp_json_t *cmcp_json_new_null(void);
cmcp_json_t *cmcp_json_new_bool(int b);
cmcp_json_t *cmcp_json_new_int(long long i);
cmcp_json_t *cmcp_json_new_double(double d);
cmcp_json_t *cmcp_json_new_string(const char *s);
cmcp_json_t *cmcp_json_new_string_n(const char *s, size_t n);
cmcp_json_t *cmcp_json_new_array(void);
cmcp_json_t *cmcp_json_new_object(void);
/** @} */

/** @name Mutators (take ownership of `v`)
 *  Append / insert. The container takes ownership of the value
 *  pointer — do not free `v` after a successful call. Return
 *  `CMCP_OK` or a negative error code.
 *  @{ */
int cmcp_json_array_append(cmcp_json_t *arr, cmcp_json_t *v);
int cmcp_json_object_set(cmcp_json_t *obj, const char *key, cmcp_json_t *v);
int cmcp_json_object_set_n(cmcp_json_t *obj, const char *key, size_t key_len,
                           cmcp_json_t *v);
/** @} */

/** @name Accessors (borrowed views; never free) */
/** @{ */
const cmcp_json_t *cmcp_json_object_get(const cmcp_json_t *obj, const char *key);
const cmcp_json_t *cmcp_json_array_at(const cmcp_json_t *arr, size_t i);
size_t             cmcp_json_array_len(const cmcp_json_t *arr);
size_t             cmcp_json_object_len(const cmcp_json_t *obj);

const char *cmcp_json_string(const cmcp_json_t *v);
size_t      cmcp_json_string_len(const cmcp_json_t *v);
long long   cmcp_json_int(const cmcp_json_t *v);
double      cmcp_json_double(const cmcp_json_t *v);
int         cmcp_json_bool(const cmcp_json_t *v);
int         cmcp_json_is_null(const cmcp_json_t *v);
/** @} */

/** @name Parse / emit
 *  `_stable` orders object keys lexicographically — use it for
 *  golden-file fixtures and replay gates. The default emitter
 *  preserves insertion order, which is cheaper.
 *  @{ */
cmcp_json_t *cmcp_json_parse(const char *text, size_t len);
cmcp_json_t *cmcp_json_parse_cstr(const char *text);

char *cmcp_json_emit(const cmcp_json_t *v);
char *cmcp_json_emit_stable(const cmcp_json_t *v);
/** @} */

/** @name Tree lifecycle */
/** @{ */
void         cmcp_json_free(cmcp_json_t *v);
cmcp_json_t *cmcp_json_clone(const cmcp_json_t *v);
int          cmcp_json_equal(const cmcp_json_t *a, const cmcp_json_t *b);
/** @} */

/** @name String escaping helpers (rarely needed by callers) */
/** @{ */
int   cmcp_json_escape(const char *in, char *out, size_t out_sz);
char *cmcp_json_escape_dup(const char *in);
/** @} */

/**
 * @brief In-place scrub of credential-shaped values (Tier 6 axis 6.5.4).
 *
 * Recursively walks `v`. For any object entry whose **key** matches a
 * sensitive name (`password`, `passwd`, `token`, `secret`, `apikey`,
 * `authorization`, `bearer`, `credential` — matched case-insensitively
 * against the key with non-alphanumeric characters stripped, so
 * `api_key`, `API-Key`, `apiKey` all hit), the value is replaced with
 * the string `"[REDACTED]"` regardless of its original type. Other
 * entries are unchanged and recursed into.
 *
 * The match is substring on the normalized key: `myApiKey` and
 * `customer_secret` both redact.
 *
 * Caller retains ownership of `v`. Safe on `NULL` or scalar values
 * (no-op). Allocation failure during replacement leaves the original
 * value in place (best-effort, never aborts).
 */
void cmcp_json_redact(cmcp_json_t *v);

#endif
