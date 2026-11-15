/* libFuzzer harness for the RFC 6570 subset in src/uri_template.c.
 *
 * resources/read URIs come from the client, so cmcp_uri_template_match
 * runs on attacker-controlled bytes (and templates on the server
 * author's). Input layout: template, a newline, URI. Checks:
 *
 *   1. neither call crashes or runs away (libFuzzer's timeout);
 *   2. an invalid template never matches;
 *   3. a valid template without variables matches exactly itself. */

#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "../src/uri_template.h"

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    char *buf = (char *)malloc(size + 1);
    if (!buf)
        return 0;
    memcpy(buf, data, size);
    buf[size] = '\0';
    char *uri = memchr(buf, '\n', size);
    if (uri)
        *uri++ = '\0';
    else
        uri = buf;

    int valid = cmcp_uri_template_valid(buf);
    int match = cmcp_uri_template_match(buf, uri);
    if (!valid && match)
        abort();
    if (valid && !strchr(buf, '{') &&
        strlen(uri) <= CMCP_URI_TEMPLATE_MAX_URI &&
        match != (strcmp(buf, uri) == 0))
        abort();
    free(buf);
    return 0;
}
