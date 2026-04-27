#ifndef CMCP_JSON_H
#define CMCP_JSON_H

#include <stddef.h>

typedef enum {
    CMCP_JSON_NULL,
    CMCP_JSON_BOOL,
    CMCP_JSON_INT,
    CMCP_JSON_DOUBLE,
    CMCP_JSON_STRING,
    CMCP_JSON_ARRAY,
    CMCP_JSON_OBJECT,
} cmcp_json_type_t;

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

cmcp_json_t *cmcp_json_new_null(void);
cmcp_json_t *cmcp_json_new_bool(int b);
cmcp_json_t *cmcp_json_new_int(long long i);
cmcp_json_t *cmcp_json_new_double(double d);
cmcp_json_t *cmcp_json_new_string(const char *s);
cmcp_json_t *cmcp_json_new_string_n(const char *s, size_t n);
cmcp_json_t *cmcp_json_new_array(void);
cmcp_json_t *cmcp_json_new_object(void);

int cmcp_json_array_append(cmcp_json_t *arr, cmcp_json_t *v);
int cmcp_json_object_set(cmcp_json_t *obj, const char *key, cmcp_json_t *v);
int cmcp_json_object_set_n(cmcp_json_t *obj, const char *key, size_t key_len,
                           cmcp_json_t *v);

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

cmcp_json_t *cmcp_json_parse(const char *text, size_t len);
cmcp_json_t *cmcp_json_parse_cstr(const char *text);

char *cmcp_json_emit(const cmcp_json_t *v);
char *cmcp_json_emit_stable(const cmcp_json_t *v);

void         cmcp_json_free(cmcp_json_t *v);
cmcp_json_t *cmcp_json_clone(const cmcp_json_t *v);
int          cmcp_json_equal(const cmcp_json_t *a, const cmcp_json_t *b);

int   cmcp_json_escape(const char *in, char *out, size_t out_sz);
char *cmcp_json_escape_dup(const char *in);

#endif
