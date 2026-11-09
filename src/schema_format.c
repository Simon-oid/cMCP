/* schema_format.c — lexical checks behind the JSON Schema `format`
 * keyword (date-time, email, uri, uuid), split out of schema.c.
 *
 * "Fast" posture, as Ajv's ajv-formats: a shape check, not full RFC
 * parsing; unknown formats are accepted as annotations. See
 * docs/schema-conformance.md, "Format". */
#include "schema_format.h"

#include <ctype.h>
#include <string.h>

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

int cmcp_schema_format_ok(const char *format, const char *s) {
    if (!format || !s) return 1;
    if (strcmp(format, "date-time") == 0) return format_date_time(s);
    if (strcmp(format, "email") == 0)     return format_email(s);
    if (strcmp(format, "uri") == 0)       return format_uri(s);
    if (strcmp(format, "uuid") == 0)      return format_uuid(s);
    return 1;              /* unknown formats accepted (annotation posture) */
}
