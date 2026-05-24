/* libFuzzer harness for cmcp_json_parse.
 *
 * Goal: any byte sequence must either parse to a valid tree or return
 * NULL — no crashes, no UAF, no UB. ASan + UBSan are wired in via the
 * Makefile's fuzz target.
 *
 * Run a single 24h baseline:
 *   make fuzz-json
 *   ./fuzz/fuzz_json fuzz/corpus_json -max_total_time=86400 -print_final_stats=1
 * Smoke for 60s:
 *   ./fuzz/fuzz_json fuzz/corpus_json -max_total_time=60
 * Reproduce a crash:
 *   ./fuzz/fuzz_json crash-<hash>
 */

#include <stddef.h>
#include <stdint.h>
#include "cmcp_json.h"

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    cmcp_json_t *j = cmcp_json_parse((const char *)data, size);
    if (j) cmcp_json_free(j);
    return 0;
}
