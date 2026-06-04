/* crag_cutoff — see crag_cutoff.h for the rationale. */
#include "crag_cutoff.h"

#include <stddef.h>
#include <stdlib.h>
#include <string.h>

/* Measured per-embedder cosine cutoffs. Only models whose distribution has
 * actually been measured belong here — a guessed cutoff is worse than the
 * honest SRC_DEFAULT fallback, which at least tells the operator to calibrate.
 * Keys are matched as case-insensitive substrings so they survive the model
 * decorations ollama/llama.cpp add (e.g. "bge-m3:latest", "registry/bge-m3").
 */
typedef struct {
    const char *model_substr;
    float       cutoff;
} crag_cutoff_row_t;

static const crag_cutoff_row_t CRAG_CUTOFF_TABLE[] = {
    { "bge-m3", 0.38f },  /* multilingual; the Italian-deployment embedder */
    { "mxbai",  0.50f },  /* mxbai-embed-large; English-centric, high floor */
};

/* Case-insensitive "does haystack contain needle?" — needle is ASCII
 * (model-name fragments), so a byte-wise lower-fold is sufficient. */
static int contains_ci(const char *haystack, const char *needle) {
    if (!haystack || !needle || !*needle) return 0;
    size_t nlen = strlen(needle);
    for (const char *h = haystack; *h; h++) {
        size_t i = 0;
        while (i < nlen && h[i]) {
            unsigned char a = (unsigned char)h[i];
            unsigned char b = (unsigned char)needle[i];
            if (a >= 'A' && a <= 'Z') a = (unsigned char)(a - 'A' + 'a');
            if (b >= 'A' && b <= 'Z') b = (unsigned char)(b - 'A' + 'a');
            if (a != b) break;
            i++;
        }
        if (i == nlen) return 1;
    }
    return 0;
}

float crag_resolve_min_cosine(const char *env_min_cosine,
                              const char *env_embed_model,
                              crag_cutoff_src_t *src_out,
                              int *env_invalid_out) {
    crag_cutoff_src_t src = CRAG_CUTOFF_SRC_DEFAULT;
    int env_invalid = 0;
    float cutoff = CRAG_CUTOFF_DEFAULT;

    /* 1. Explicit operator override. */
    if (env_min_cosine && *env_min_cosine) {
        char *end = NULL;
        float v = strtof(env_min_cosine, &end);
        if (end != env_min_cosine && *end == '\0' && v <= 1.0f) {
            if (src_out)         *src_out = CRAG_CUTOFF_SRC_ENV;
            if (env_invalid_out) *env_invalid_out = 0;
            return v;
        }
        env_invalid = 1;  /* present but unusable — warn, then fall through */
    }

    /* 2. Measured per-embedder table. */
    for (size_t i = 0; i < sizeof CRAG_CUTOFF_TABLE / sizeof CRAG_CUTOFF_TABLE[0]; i++) {
        if (contains_ci(env_embed_model, CRAG_CUTOFF_TABLE[i].model_substr)) {
            cutoff = CRAG_CUTOFF_TABLE[i].cutoff;
            src = CRAG_CUTOFF_SRC_MODEL;
            break;
        }
    }

    /* 3. else: cutoff/src already hold the default. */
    if (src_out)         *src_out = src;
    if (env_invalid_out) *env_invalid_out = env_invalid;
    return cutoff;
}
