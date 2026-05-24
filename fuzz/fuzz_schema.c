/* libFuzzer harness for cmcp_schema_validate.
 *
 * Schema validation takes two inputs: the schema itself + the value to
 * check. The fuzz input encodes both: a 2-byte length prefix gives the
 * schema-source byte count, the rest is the value source. Both are
 * parsed with cmcp_json_parse first; we only call schema_validate when
 * both parsed successfully, otherwise we still exercise the JSON
 * parser on adversarial bytes (so a single corpus stresses both
 * surfaces). */

#include <stddef.h>
#include <stdint.h>
#include "cmcp_json.h"
#include "cmcp_schema.h"

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    if (size < 2) return 0;

    size_t schema_len = ((size_t)data[0] << 8) | (size_t)data[1];
    if (schema_len > size - 2) schema_len = size - 2;
    size_t value_len  = size - 2 - schema_len;

    cmcp_json_t *schema = cmcp_json_parse((const char *)data + 2,
                                           schema_len);
    cmcp_json_t *value  = cmcp_json_parse((const char *)data + 2 + schema_len,
                                           value_len);

    if (schema && value) {
        cmcp_schema_error_t err;
        cmcp_schema_error_init(&err);
        cmcp_schema_validate(schema, value, &err);
        cmcp_schema_error_clear(&err);
    }

    if (schema) cmcp_json_free(schema);
    if (value)  cmcp_json_free(value);
    return 0;
}
