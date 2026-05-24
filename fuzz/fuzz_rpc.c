/* libFuzzer harness for cmcp_rpc_parse.
 *
 * Goal: any byte sequence (single frame, batch, malformed, truncated)
 * must either round-trip through the parser to a normalised
 * cmcp_rpc_message_t array, or return an error code without leaking or
 * corrupting memory. */

#include <stddef.h>
#include <stdint.h>
#include "cmcp_types.h"

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    cmcp_rpc_message_t *msgs = NULL;
    size_t count = 0;
    if (cmcp_rpc_parse((const char *)data, size, &msgs, &count) == 0) {
        cmcp_rpc_messages_free(msgs, count);
    }
    return 0;
}
