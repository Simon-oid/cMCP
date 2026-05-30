/* schema_ajv_runner — drives cmcp_schema_validate over a corpus of
 * (schema, value) pairs and emits one result line per pair to stdout.
 *
 * Companion process to conformance/schema_ajv_crosscheck.mjs, which
 * runs Ajv against the same corpus and asserts the two implementations
 * agree on accept/reject. Per the 6.7 axis acceptance criteria.
 *
 * Corpus shape (single JSON file, argv[1]):
 *   [ {"name": "…", "schema": {…}, "value": …, "expected": true/false}, … ]
 *
 * Output (stdout, one line per entry, in corpus order):
 *   <name>\t<ok|fail>\n
 *
 * "ok" means cmcp_schema_validate returned CMCP_OK; "fail" means it
 * returned CMCP_ESCHEMA. Any other return code (allocation failure,
 * malformed input) prints "ok" by convention — the JS driver will then
 * disagree with Ajv and surface it. */

#define _POSIX_C_SOURCE 200809L

#include "cmcp.h"
#include "cmcp_json.h"
#include "cmcp_schema.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static char *slurp(const char *path, size_t *out_len) {
    FILE *f = fopen(path, "rb");
    if (!f) {
        fprintf(stderr, "open %s: %s\n", path, strerror(errno));
        return NULL;
    }
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (n < 0) { fclose(f); return NULL; }
    char *buf = (char *)malloc((size_t)n + 1);
    if (!buf) { fclose(f); return NULL; }
    if (fread(buf, 1, (size_t)n, f) != (size_t)n) {
        free(buf); fclose(f); return NULL;
    }
    buf[n] = '\0';
    fclose(f);
    if (out_len) *out_len = (size_t)n;
    return buf;
}

int main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "usage: %s <corpus.json>\n", argv[0]);
        return 2;
    }
    size_t len = 0;
    char *src = slurp(argv[1], &len);
    if (!src) return 1;
    cmcp_json_t *corpus = cmcp_json_parse(src, len);
    free(src);
    if (!corpus || corpus->type != CMCP_JSON_ARRAY) {
        fprintf(stderr, "corpus must be a top-level JSON array\n");
        cmcp_json_free(corpus);
        return 1;
    }

    for (size_t i = 0; i < corpus->arr.len; i++) {
        const cmcp_json_t *entry = corpus->arr.items[i];
        if (!entry || entry->type != CMCP_JSON_OBJECT) {
            fprintf(stderr, "entry %zu: not an object\n", i); continue;
        }
        const cmcp_json_t *name   = cmcp_json_object_get(entry, "name");
        const cmcp_json_t *schema = cmcp_json_object_get(entry, "schema");
        const cmcp_json_t *value  = cmcp_json_object_get(entry, "value");
        if (!name || name->type != CMCP_JSON_STRING || !schema) {
            fprintf(stderr, "entry %zu: missing name/schema\n", i); continue;
        }
        int rc = cmcp_schema_validate(schema, value, NULL);
        printf("%s\t%s\n", name->str.s, rc == CMCP_OK ? "ok" : "fail");
    }

    cmcp_json_free(corpus);
    fflush(stdout);
    return 0;
}
