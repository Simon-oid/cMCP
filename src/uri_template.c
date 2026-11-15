#include "uri_template.h"

#include <stdlib.h>
#include <string.h>

#define MAX_PARTS (2 * CMCP_URI_TEMPLATE_MAX_VARS + 1)

typedef struct {
    const char
        *lit; /* literal text (not NUL-terminated), or NULL for a variable */
    size_t len;
    int reserved; /* {+var} */
} part_t;

static int name_char(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
           (c >= '0' && c <= '9') || c == '_';
}

/* Split a template into parts; -1 if outside the supported subset. */
static int parse(const char *t, part_t *parts) {
    int n = 0, vars = 0;
    if (!t || !*t)
        return -1;
    const char *p = t;
    while (*p) {
        if (*p == '}')
            return -1;
        if (*p != '{') {
            const char *s = p;
            while (*p && *p != '{' && *p != '}')
                p++;
            if (n == MAX_PARTS)
                return -1;
            parts[n++] = (part_t){s, (size_t)(p - s), 0};
            continue;
        }
        p++; /* past '{' */
        int reserved = 0;
        if (*p == '+') {
            reserved = 1;
            p++;
        }
        const char *s = p;
        while (name_char(*p))
            p++;
        if (p == s || *p != '}')
            return -1; /* empty, bad char, operator, unclosed */
        p++;           /* past '}' */
        if (n > 0 && !parts[n - 1].lit)
            return -1; /* adjacent variables */
        if (++vars > CMCP_URI_TEMPLATE_MAX_VARS || n == MAX_PARTS)
            return -1;
        parts[n++] = (part_t){NULL, 0, reserved};
    }
    return n;
}

int cmcp_uri_template_valid(const char *tmpl) {
    part_t parts[MAX_PARTS];
    return parse(tmpl, parts) > 0;
}

typedef struct {
    const part_t *parts;
    int n;
    const char *uri;
    size_t ulen;
    unsigned char *failed; /* [n][ulen + 1]: state already known not to match */
} matcher_t;

static int match_from(matcher_t *m, int i, size_t pos) {
    if (i == m->n)
        return pos == m->ulen;
    const part_t *pt = &m->parts[i];
    if (pt->lit) {
        if (m->ulen - pos < pt->len ||
            memcmp(m->uri + pos, pt->lit, pt->len) != 0)
            return 0;
        return match_from(m, i + 1, pos + pt->len);
    }
    unsigned char *seen = &m->failed[(size_t)i * (m->ulen + 1) + pos];
    if (*seen)
        return 0;
    for (size_t end = pos + 1; end <= m->ulen; end++) {
        char c = m->uri[end - 1];
        if (!pt->reserved && (c == '/' || c == '?' || c == '#'))
            break;
        if (match_from(m, i + 1, end))
            return 1;
    }
    *seen = 1;
    return 0;
}

int cmcp_uri_template_match(const char *tmpl, const char *uri) {
    part_t parts[MAX_PARTS];
    int n = parse(tmpl, parts);
    if (n <= 0 || !uri)
        return 0;
    size_t ulen = strlen(uri);
    if (ulen > CMCP_URI_TEMPLATE_MAX_URI)
        return 0;
    matcher_t m = {parts, n, uri, ulen, calloc((size_t)n * (ulen + 1), 1)};
    if (!m.failed)
        return 0;
    int ok = match_from(&m, 0, 0);
    free(m.failed);
    return ok;
}
