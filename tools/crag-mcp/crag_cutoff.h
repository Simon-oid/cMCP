/* crag_cutoff — per-embedder cosine relevance cutoff resolution.
 *
 * crag_search gates each hybrid result on the raw cosine similarity (the
 * only calibrated [-1,1] relevance signal; the RRF fusion score has a
 * structural range and can't be thresholded). But the cosine floor that
 * separates a genuine match from the one-retriever noise floor depends on
 * the embedding MODEL, not the query:
 *
 *   mxbai-embed-large : genuine 0.62-0.72, irrelevant 0.30-0.41 -> gate 0.50
 *   bge-m3            : genuine 0.43-0.58, irrelevant 0.24-0.32 -> gate 0.38
 *
 * (Both measured on the Italian municipal fixture; see cRAG's
 * project_crag_sota notes / docs/benchmarks.md.) A single global 0.50 — the
 * old compiled-in default — silently drops ~40% of genuine bge-m3 matches,
 * because bge-m3's whole cosine distribution sits ~0.2 lower than mxbai's.
 * That is a wrong-LAYER bug: the retriever is fine, the gate is miscalibrated.
 *
 * This module turns that one magic number into a small measured table keyed
 * on CRAG_EMBED_MODEL, with the operator's explicit CRAG_MIN_COSINE always
 * winning. It is deliberately a pure, dependency-free unit so the calibration
 * can be pinned by a hermetic test (tests/test_crag_cutoff.c) — the artifact
 * the audit found missing, so the gate can no longer silently regress.
 */
#ifndef CRAG_CUTOFF_H
#define CRAG_CUTOFF_H

/* Cutoff used when the embedding model is unrecognized. Conservative: kept
 * at the historical default (mxbai's floor) so behaviour is unchanged for
 * any model not yet in the table — but the caller is told (SRC_DEFAULT) so
 * it can nudge the operator to calibrate. */
#define CRAG_CUTOFF_DEFAULT  0.50f

/* Where the resolved cutoff came from. Lets the caller log an appropriate
 * note and lets the test assert the decision path, not just the number. */
typedef enum {
    CRAG_CUTOFF_SRC_ENV,      /* explicit, valid CRAG_MIN_COSINE         */
    CRAG_CUTOFF_SRC_MODEL,    /* matched the per-embedder table          */
    CRAG_CUTOFF_SRC_DEFAULT   /* model unrecognized -> CRAG_CUTOFF_DEFAULT */
} crag_cutoff_src_t;

/* Resolve the cosine relevance cutoff for crag_search.
 *
 * Precedence:
 *   1. env_min_cosine (CRAG_MIN_COSINE), if it parses as a float <= 1.0.
 *      Returned verbatim (a value <= -1 disables gating). src = SRC_ENV.
 *      A malformed or >1 value is rejected: *env_invalid_out is set and
 *      resolution falls through to the table / default.
 *   2. env_embed_model (CRAG_EMBED_MODEL) matched case-insensitively as a
 *      substring against the measured per-embedder table. src = SRC_MODEL.
 *   3. CRAG_CUTOFF_DEFAULT. src = SRC_DEFAULT.
 *
 * Either string may be NULL or empty. Pure: no I/O, no global state.
 *
 * Returns the cutoff to use. *src_out (if non-NULL) receives the decision
 * path; *env_invalid_out (if non-NULL) is set to 1 iff CRAG_MIN_COSINE was
 * present but unparseable/out-of-range (so the caller can warn), else 0.
 */
float crag_resolve_min_cosine(const char *env_min_cosine,
                              const char *env_embed_model,
                              crag_cutoff_src_t *src_out,
                              int *env_invalid_out);

#endif /* CRAG_CUTOFF_H */
