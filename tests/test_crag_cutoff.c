/* test_crag_cutoff — pins crag-mcp's per-embedder relevance-gate calibration.
 *
 * This is the artifact the cRAG↔cMCP seam audit found missing: nothing
 * asserted that an answerable query survives the cosine gate, so a
 * miscalibrated cutoff (the 0.50 mxbai default silently dropping ~40% of
 * genuine bge-m3 matches) could regress unnoticed. The cutoff logic is a
 * pure unit, so we test the decision directly — no embedder, no DB, no
 * network. It runs in every `make test`.
 *
 * The teeth: for each (embedder, score) pair we assert the gate's verdict
 * matches the MEASURED genuine/irrelevant bands. If someone resets the
 * bge-m3 cutoff back to 0.50, the "genuine bge-m3 match at 0.45 must pass"
 * cases fail — exactly the silent over-refusal the audit caught by hand.
 */
#include "crag_cutoff.h"
#include "test.h"

#include <math.h>

static int feq(float a, float b) { return fabsf(a - b) < 1e-6f; }

/* A score >= the resolved cutoff is admitted by crag_search's gate
 * (scores[i].cosine >= ctx->min_cosine). */
static int admits(float cutoff, float cosine) { return cosine >= cutoff; }

/* --- The per-embedder table is correct and substring/case robust ------- */
static void test_model_table(void) {
    crag_cutoff_src_t src;
    int inv;

    /* bge-m3: the Italian-deployment embedder, low cosine floor -> 0.38. */
    TEST_ASSERT(feq(crag_resolve_min_cosine(NULL, "bge-m3", &src, &inv), 0.38f));
    TEST_ASSERT(src == CRAG_CUTOFF_SRC_MODEL);
    TEST_ASSERT(inv == 0);

    /* Survives ollama/llama.cpp decorations and case. */
    TEST_ASSERT(feq(crag_resolve_min_cosine(NULL, "bge-m3:latest", &src, &inv), 0.38f));
    TEST_ASSERT(src == CRAG_CUTOFF_SRC_MODEL);
    TEST_ASSERT(feq(crag_resolve_min_cosine(NULL, "registry/BGE-M3", &src, &inv), 0.38f));
    TEST_ASSERT(src == CRAG_CUTOFF_SRC_MODEL);

    /* mxbai: English-centric, high floor -> 0.50. */
    TEST_ASSERT(feq(crag_resolve_min_cosine(NULL, "mxbai-embed-large", &src, &inv), 0.50f));
    TEST_ASSERT(src == CRAG_CUTOFF_SRC_MODEL);
}

/* --- Unknown / unset model falls back to the conservative default ------ */
static void test_default_fallback(void) {
    crag_cutoff_src_t src;
    int inv;

    TEST_ASSERT(feq(crag_resolve_min_cosine(NULL, "nomic-embed-text", &src, &inv),
                    CRAG_CUTOFF_DEFAULT));
    TEST_ASSERT(src == CRAG_CUTOFF_SRC_DEFAULT);

    TEST_ASSERT(feq(crag_resolve_min_cosine(NULL, NULL, &src, &inv),
                    CRAG_CUTOFF_DEFAULT));
    TEST_ASSERT(src == CRAG_CUTOFF_SRC_DEFAULT);

    TEST_ASSERT(feq(crag_resolve_min_cosine(NULL, "", &src, &inv),
                    CRAG_CUTOFF_DEFAULT));
    TEST_ASSERT(src == CRAG_CUTOFF_SRC_DEFAULT);
}

/* --- Explicit CRAG_MIN_COSINE always wins ------------------------------ */
static void test_env_override(void) {
    crag_cutoff_src_t src;
    int inv;

    /* A valid override beats the model table. */
    TEST_ASSERT(feq(crag_resolve_min_cosine("0.42", "bge-m3", &src, &inv), 0.42f));
    TEST_ASSERT(src == CRAG_CUTOFF_SRC_ENV);
    TEST_ASSERT(inv == 0);

    /* <= -1 disables gating: returned verbatim, admits everything. */
    float c = crag_resolve_min_cosine("-1", "mxbai", &src, &inv);
    TEST_ASSERT(feq(c, -1.0f));
    TEST_ASSERT(src == CRAG_CUTOFF_SRC_ENV);
    TEST_ASSERT(admits(c, -1.0f) && admits(c, 0.0f) && admits(c, 0.99f));

    /* Boundary: exactly 1.0 is accepted (gates out ~everything but valid). */
    TEST_ASSERT(feq(crag_resolve_min_cosine("1.0", NULL, &src, &inv), 1.0f));
    TEST_ASSERT(src == CRAG_CUTOFF_SRC_ENV);
}

/* --- Invalid CRAG_MIN_COSINE is rejected, table still consulted -------- */
static void test_env_invalid(void) {
    crag_cutoff_src_t src;
    int inv;

    /* Garbage env -> flagged invalid, but bge-m3's table value still used
     * (NOT the bare default — a typo'd override must not silently re-break
     * the bge-m3 calibration). */
    TEST_ASSERT(feq(crag_resolve_min_cosine("garbage", "bge-m3", &src, &inv), 0.38f));
    TEST_ASSERT(inv == 1);
    TEST_ASSERT(src == CRAG_CUTOFF_SRC_MODEL);

    /* Out-of-range (> 1.0) is rejected the same way. */
    TEST_ASSERT(feq(crag_resolve_min_cosine("1.5", "bge-m3", &src, &inv), 0.38f));
    TEST_ASSERT(inv == 1);

    /* Trailing junk is rejected (strtof would otherwise accept the prefix). */
    TEST_ASSERT(feq(crag_resolve_min_cosine("0.4x", NULL, &src, &inv),
                    CRAG_CUTOFF_DEFAULT));
    TEST_ASSERT(inv == 1);
    TEST_ASSERT(src == CRAG_CUTOFF_SRC_DEFAULT);
}

/* --- The whole point: measured bands -> correct admit/refuse verdicts --- */
static void test_measured_bands(void) {
    /* Measured on the Italian fixture (project_crag_sota):
     *   bge-m3 genuine 0.43-0.58, irrelevant 0.24-0.32
     *   mxbai  genuine 0.62-0.72, irrelevant 0.30-0.41                  */
    float bge   = crag_resolve_min_cosine(NULL, "bge-m3", NULL, NULL);
    float mxbai = crag_resolve_min_cosine(NULL, "mxbai-embed-large", NULL, NULL);

    /* bge-m3: genuine matches near the floor must NOT be refused... */
    TEST_ASSERT(admits(bge, 0.45f));   /* genuine, just above band bottom */
    TEST_ASSERT(admits(bge, 0.43f));   /* genuine, band bottom            */
    /* ...and irrelevant ones must be. */
    TEST_ASSERT(!admits(bge, 0.32f));  /* irrelevant, band top            */
    TEST_ASSERT(!admits(bge, 0.24f));

    /* Regression sentinel: the OLD global 0.50 would have refused these
     * genuine bge-m3 matches. If the table is reset to 0.50 these flip. */
    TEST_ASSERT(admits(bge, 0.45f) && bge < 0.50f);

    /* mxbai: its higher floor keeps the irrelevant 0.30-0.41 band out... */
    TEST_ASSERT(!admits(mxbai, 0.41f));
    /* ...while genuine 0.62-0.72 sails through. */
    TEST_ASSERT(admits(mxbai, 0.62f));
}

int main(void) {
    fprintf(stderr, "test_crag_cutoff\n");
    TEST_RUN(test_model_table);
    TEST_RUN(test_default_fallback);
    TEST_RUN(test_env_override);
    TEST_RUN(test_env_invalid);
    TEST_RUN(test_measured_bands);
    TEST_DONE();
}
