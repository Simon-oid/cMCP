#ifndef CMCP_URI_TEMPLATE_H
#define CMCP_URI_TEMPLATE_H

/* RFC 6570 URI templates, the subset resources/templates/list routing
 * needs: literal text, simple expansion {var} (one value with no "/", "?"
 * or "#") and reserved expansion {+var} (any non-empty value). Other
 * operators, lists and prefix modifiers are refused, as are two adjacent
 * variables, whose split would be ambiguous. Internal to libcmcp_server. */

#define CMCP_URI_TEMPLATE_MAX_VARS 8
#define CMCP_URI_TEMPLATE_MAX_URI 2048 /* longer URIs never match */

/* 1 if `tmpl` is a template this subset accepts, else 0. */
int cmcp_uri_template_valid(const char *tmpl);

/* 1 if `uri` is an expansion of `tmpl`, else 0 (also for an invalid
 * template). Bounded: each (part, offset) state is tried once. */
int cmcp_uri_template_match(const char *tmpl, const char *uri);

#endif
