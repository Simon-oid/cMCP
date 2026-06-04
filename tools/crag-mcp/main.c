/* crag-mcp — reference MCP server wrapping cRAG.
 *
 * Exposes two tools:
 *
 *   crag_search   { query: string, k?: integer 1..20 }
 *                 Hybrid retrieval (BM25 + cosine, RRF-fused) over the
 *                 cRAG index. Returns up to k chunks, one text content
 *                 item per chunk, gated on cosine relevance (see
 *                 CRAG_SEARCH_MIN_COSINE / CRAG_MIN_COSINE).
 *
 *   crag_stats    {}
 *                 One-line summary: db path, chunk count, file count,
 *                 embedding dim.
 *
 * Plus one resource:
 *
 *   crag://stats  Same diagnostics as the crag_stats tool, but exposed
 *                 as a resource so a host can pin it as ambient
 *                 context rather than calling a tool. The point isn't
 *                 to duplicate functionality — it's to exercise the
 *                 resources/list and /read surface end-to-end against
 *                 a real server.
 *
 * Indexing is deliberately NOT exposed as a tool. Indexing is a heavy,
 * stateful, admin-plane operation; an LLM has no business triggering
 * it. Run `crag index <dir>` from the cRAG CLI instead. (See
 * tools/crag-mcp/README.md for the full rationale.)
 *
 * Usage:
 *   crag-mcp [--db <path>]
 *
 * Resolution order for the DB path:
 *   1. --db <path>
 *   2. $CRAG_DB
 *   3. ./crag.db
 *
 * Environment used by cRAG's embedder (passed through unchanged):
 *   CRAG_EMBED_BACKEND   local|openai|ollama (default: local)
 *   CRAG_EMBED_URL       embedding endpoint
 *   OPENAI_API_KEY       for openai backend
 *
 * Environment specific to this server:
 *   CRAG_MIN_COSINE      crag_search relevance gate. Explicit override,
 *                        always wins (<= -1 disables gating). When unset,
 *                        the gate is calibrated per embedding model from
 *                        CRAG_EMBED_MODEL (bge-m3 -> 0.38, mxbai -> 0.50,
 *                        else 0.50 default). See crag_cutoff.{h,c}.
 */

#include "cmcp.h"
#include "cmcp_json.h"
#include "cmcp_server.h"

/* cRAG headers — sibling repo, linked statically by the Makefile rule. */
#include "crag.h"
#include "embed.h"
#include "store.h"

/* Per-embedder relevance-gate calibration (pure, hermetically tested). */
#include "crag_cutoff.h"

#include <curl/curl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ====================================================================== */
/* Server-wide state                                                       */
/* ====================================================================== */

typedef struct {
    crag_store_t           store;
    crag_embed_backend_t  *embed;
    const char            *db_path;
    float                  min_cosine;  /* relevance gate, see below */
} crag_mcp_ctx_t;

/* ====================================================================== */
/* Helpers                                                                 */
/* ====================================================================== */

/* Build a single content item: {"type":"text","text":<text>}. Returns
 * NULL on allocation failure. Caller transfers ownership into the
 * response array. */
static cmcp_json_t *text_item(const char *text) {
    cmcp_json_t *item = cmcp_json_new_object();
    if (!item) return NULL;
    cmcp_json_object_set(item, "type", cmcp_json_new_string("text"));
    cmcp_json_object_set(item, "text", cmcp_json_new_string(text));
    return item;
}

/* Build a content array containing a single text item. Used for the
 * error/empty path. */
static cmcp_json_t *single_text_array(const char *text) {
    cmcp_json_t *arr = cmcp_json_new_array();
    if (!arr) return NULL;
    cmcp_json_array_append(arr, text_item(text));
    return arr;
}

/* ====================================================================== */
/* crag_search                                                             */
/* ====================================================================== */

#define CRAG_SEARCH_DEFAULT_K  5
#define CRAG_SEARCH_MAX_K      20

/* The minimum cosine similarity for a hybrid-search result to count as a
 * real match is resolved at startup into ctx->min_cosine. The RRF fusion
 * score cannot be thresholded — its range is structural (~0.033 = ranked
 * #1 by both retrievers, ~0.016 = by one) — so we gate on the raw cosine,
 * a calibrated [-1,1] relevance. The cutoff is model-dependent (mxbai's
 * floor sits ~0.2 above bge-m3's); crag_resolve_min_cosine() picks it from
 * CRAG_EMBED_MODEL unless CRAG_MIN_COSINE overrides. See crag_cutoff.{h,c}. */

static int crag_search_handler(const cmcp_json_t *args, void *userdata,
                                cmcp_handler_ctx_t *hctx,
                                cmcp_json_t **out_content, int *out_is_error) {
    (void)hctx;
    crag_mcp_ctx_t *ctx = (crag_mcp_ctx_t *)userdata;

    /* Schema validator already enforced shape; safe to assume args is
     * an object with `query` string. */
    const cmcp_json_t *q = cmcp_json_object_get(args, "query");
    const cmcp_json_t *k_v = cmcp_json_object_get(args, "k");
    int k = (k_v && k_v->type == CMCP_JSON_INT) ? (int)k_v->i
                                                 : CRAG_SEARCH_DEFAULT_K;
    if (k < 1) k = 1;
    if (k > CRAG_SEARCH_MAX_K) k = CRAG_SEARCH_MAX_K;

    /* Embed the query. */
    float qvec[CRAG_EMBED_DIM_MAX];
    int dim = 0;
    int rc = ctx->embed->embed_one(q->str.s, qvec, &dim, ctx->embed->ctx);
    if (rc != CRAG_OK || dim <= 0) {
        *out_is_error = 1;
        *out_content = single_text_array("query embedding failed");
        return CMCP_OK;
    }

    /* Run hybrid retrieval. */
    crag_chunk_t *results = (crag_chunk_t *)calloc((size_t)k, sizeof *results);
    crag_score_t *scores  = (crag_score_t *)calloc((size_t)k, sizeof *scores);
    if (!results || !scores) {
        free(results); free(scores);
        *out_is_error = 1;
        *out_content = single_text_array("out of memory");
        return CMCP_OK;
    }

    int n = store_search_hybrid(&ctx->store, qvec, dim,
                                 q->str.s, k, results, scores);
    if (n < 0) {
        free(results); free(scores);
        *out_is_error = 1;
        *out_content = single_text_array("retrieval failed");
        return CMCP_OK;
    }

    if (n == 0) {
        free(results); free(scores);
        *out_is_error = 0;
        *out_content = single_text_array("(no matching chunks)");
        return CMCP_OK;
    }

    /* Format one content item per chunk that clears the relevance gate:
     *   [cos 0.612  bm25 8.40  fusion 0.033] path/to/source.md
     *   <chunk body>
     * cosine leads because it is the only calibrated relevance signal;
     * bm25 and fusion follow for transparency / debugging.            */
    cmcp_json_t *arr = cmcp_json_new_array();
    if (!arr) {
        for (int i = 0; i < n; i++) {
            free(results[i].source);
            free(results[i].text);
        }
        free(results); free(scores);
        *out_is_error = 1;
        *out_content = single_text_array("out of memory");
        return CMCP_OK;
    }

    int emitted = 0;
    for (int i = 0; i < n; i++) {
        /* Relevance gate. Results come back ranked by the RRF fusion
         * score, which can't be thresholded — a lexical-only or weak
         * hit lands at the same fusion floor as a real semantic match.
         * The raw cosine separates them, so drop anything below the
         * cutoff rather than hand the model the one-retriever floor. */
        if (scores[i].cosine >= ctx->min_cosine) {
            const char *src = results[i].source ? results[i].source
                                                : "(unknown)";
            const char *txt = results[i].text   ? results[i].text   : "";
            size_t need = strlen(src) + strlen(txt) + 96;
            char *buf = (char *)malloc(need);
            if (buf) {
                snprintf(buf, need,
                          "[cos %.3f  bm25 %.2f  fusion %.3f] %s\n%s",
                          (double)scores[i].cosine, (double)scores[i].bm25,
                          (double)scores[i].fusion, src, txt);
                cmcp_json_array_append(arr, text_item(buf));
                free(buf);
                emitted++;
            }
        }
        free(results[i].source);
        free(results[i].text);
    }
    free(results); free(scores);

    /* Every hit fell below the relevance gate: report "no match"
     * explicitly rather than returning the junk the fusion ranking
     * would otherwise surface. Not an error — an empty result is a
     * valid answer to a query nothing matches. */
    if (emitted == 0) {
        cmcp_json_free(arr);
        *out_is_error = 0;
        *out_content  = single_text_array(
            "(no chunk cleared the relevance threshold)");
        return CMCP_OK;
    }

    *out_is_error = 0;
    *out_content  = arr;
    return CMCP_OK;
}

/* ====================================================================== */
/* crag_stats                                                              */
/* ====================================================================== */

static int crag_stats_handler(const cmcp_json_t *args, void *userdata,
                               cmcp_handler_ctx_t *hctx,
                               cmcp_json_t **out_content, int *out_is_error) {
    (void)args; (void)hctx;
    crag_mcp_ctx_t *ctx = (crag_mcp_ctx_t *)userdata;

    crag_store_stats_t stats = {0};
    if (store_stats(&ctx->store, &stats) != 0) {
        *out_is_error = 1;
        *out_content = single_text_array("stats query failed");
        return CMCP_OK;
    }

    char buf[512];
    snprintf(buf, sizeof buf,
              "db:     %s\nchunks: %d\nfiles:  %d\ndim:    %d",
              ctx->db_path, stats.chunks, stats.files, stats.dim);

    *out_is_error = 0;
    *out_content  = single_text_array(buf);
    return CMCP_OK;
}

/* ====================================================================== */
/* crag://stats — resource form of crag_stats                              */
/* ====================================================================== */

static int crag_stats_resource(const char *uri, void *userdata,
                                cmcp_handler_ctx_t *hctx,
                                cmcp_json_t **out_contents, int *out_is_error) {
    (void)hctx;
    crag_mcp_ctx_t *ctx = (crag_mcp_ctx_t *)userdata;

    crag_store_stats_t stats = {0};
    if (store_stats(&ctx->store, &stats) != 0) {
        *out_is_error = 1;
        *out_contents = cmcp_resource_text_contents(uri, "text/plain",
                                                     "stats query failed");
        return CMCP_OK;
    }

    char buf[512];
    snprintf(buf, sizeof buf,
              "db:     %s\nchunks: %d\nfiles:  %d\ndim:    %d",
              ctx->db_path, stats.chunks, stats.files, stats.dim);

    *out_is_error = 0;
    *out_contents = cmcp_resource_text_contents(uri, "text/plain", buf);
    return *out_contents ? CMCP_OK : CMCP_ENOMEM;
}

/* ====================================================================== */
/* main                                                                    */
/* ====================================================================== */

static const char *resolve_db_path(int argc, char **argv) {
    for (int i = 1; i + 1 < argc; i++) {
        if (strcmp(argv[i], "--db") == 0) return argv[i + 1];
    }
    const char *env = getenv("CRAG_DB");
    if (env && *env) return env;
    return "./crag.db";
}

int main(int argc, char **argv) {
    /* libcurl is used by cRAG's HTTP-based embedders; init once. */
    curl_global_init(CURL_GLOBAL_DEFAULT);

    crag_mcp_ctx_t ctx = {0};
    ctx.db_path = resolve_db_path(argc, argv);

    /* Relevance gate for hybrid search. Precedence: an explicit, valid
     * CRAG_MIN_COSINE wins; otherwise the cutoff is calibrated per embedding
     * model from CRAG_EMBED_MODEL; otherwise a conservative default. A
     * malformed or above-1 CRAG_MIN_COSINE is rejected (warned) and we fall
     * back to the model/default. A value <= -1 disables gating. */
    const char *mc    = getenv("CRAG_MIN_COSINE");
    const char *model = getenv("CRAG_EMBED_MODEL");
    crag_cutoff_src_t cutoff_src;
    int mc_invalid = 0;
    ctx.min_cosine = crag_resolve_min_cosine(mc, model, &cutoff_src, &mc_invalid);
    if (mc_invalid)
        fprintf(stderr,
                "crag-mcp: ignoring invalid CRAG_MIN_COSINE '%s'\n", mc);
    switch (cutoff_src) {
    case CRAG_CUTOFF_SRC_ENV:
        if (ctx.min_cosine <= -1.0f)
            fprintf(stderr, "crag-mcp: relevance gate DISABLED "
                            "(CRAG_MIN_COSINE=%.3f)\n", (double)ctx.min_cosine);
        else
            fprintf(stderr, "crag-mcp: relevance gate cosine >= %.3f "
                            "(CRAG_MIN_COSINE)\n", (double)ctx.min_cosine);
        break;
    case CRAG_CUTOFF_SRC_MODEL:
        fprintf(stderr, "crag-mcp: relevance gate cosine >= %.3f "
                        "(calibrated for embedder '%s')\n",
                (double)ctx.min_cosine, model ? model : "");
        break;
    case CRAG_CUTOFF_SRC_DEFAULT:
        fprintf(stderr, "crag-mcp: relevance gate cosine >= %.3f (default; "
                        "embedder '%s' is uncalibrated — set CRAG_MIN_COSINE "
                        "if matches are wrongly dropped)\n",
                (double)ctx.min_cosine, (model && *model) ? model : "unset");
        break;
    }

    if (store_open(ctx.db_path, &ctx.store) != 0) {
        fprintf(stderr, "crag-mcp: cannot open DB at %s\n", ctx.db_path);
        return 1;
    }

    /* Pick the embedder from env (matches cRAG CLI's --backend/--url
     * fallbacks). embed_backend_create() defaults to openai when name
     * is NULL, which silently breaks if OPENAI_API_KEY isn't set —
     * read the env explicitly so the failure mode is deterministic. */
    const char *eb_name = getenv("CRAG_EMBED_BACKEND");
    const char *eb_url  = getenv("CRAG_EMBED_URL");
    const char *eb_key  = getenv("OPENAI_API_KEY");
    if (!eb_name || !*eb_name) eb_name = "local";
    ctx.embed = embed_backend_create(eb_name, eb_url, eb_key);
    if (!ctx.embed) {
        fprintf(stderr, "crag-mcp: cannot create embed backend (%s)\n", eb_name);
        store_close(&ctx.store);
        return 1;
    }

    cmcp_server_t *s = cmcp_server_new("crag-mcp", "0.1.0");
    if (!s) {
        fprintf(stderr, "crag-mcp: out of memory\n");
        embed_backend_free(ctx.embed);
        store_close(&ctx.store);
        return 1;
    }

    int rc = cmcp_server_add_tool(s, &(cmcp_tool_t){
        .name        = "crag_search",
        .description = "Hybrid retrieval (BM25 + cosine, RRF-fused) over "
                        "the cRAG index. `query` is plain English (no "
                        "special syntax). Returns up to k chunks ranked by "
                        "relevance, one text content item per chunk, each "
                        "prefixed with `[cos … bm25 … fusion …] <path>` "
                        "followed by the chunk body verbatim. Weak matches "
                        "are filtered by an operator-tuned cosine gate, so "
                        "a response of `(no chunk cleared the relevance "
                        "threshold)` means the index has no strong match "
                        "for this query — not that the index is empty.",
        .input_schema =
            "{"
              "\"type\":\"object\","
              "\"properties\":{"
                "\"query\":{\"type\":\"string\",\"minLength\":1},"
                "\"k\":{\"type\":\"integer\",\"minimum\":1,\"maximum\":20}"
              "},"
              "\"required\":[\"query\"],"
              "\"additionalProperties\":false"
            "}",
        .handler  = crag_search_handler,
        .userdata = &ctx,
    });
    if (rc != CMCP_OK) {
        fprintf(stderr, "crag-mcp: register crag_search failed (%d)\n", rc);
        cmcp_server_free(s);
        embed_backend_free(ctx.embed);
        store_close(&ctx.store);
        return 1;
    }

    rc = cmcp_server_add_tool(s, &(cmcp_tool_t){
        .name        = "crag_stats",
        .description = "Diagnostics: DB path, chunk count, file count, "
                        "embedding dimension.",
        .input_schema =
            "{\"type\":\"object\",\"additionalProperties\":false}",
        .handler  = crag_stats_handler,
        .userdata = &ctx,
    });
    if (rc != CMCP_OK) {
        fprintf(stderr, "crag-mcp: register crag_stats failed (%d)\n", rc);
        cmcp_server_free(s);
        embed_backend_free(ctx.embed);
        store_close(&ctx.store);
        return 1;
    }

    rc = cmcp_server_add_resource(s, &(cmcp_resource_t){
        .uri         = "crag://stats",
        .name        = "cRAG diagnostics",
        .description = "DB path, chunk count, file count, embedding dim. "
                        "Same payload as the `crag_stats` tool — prefer "
                        "reading this resource when you want the figures "
                        "as ambient context rather than as an explicit "
                        "tool-call turn.",
        .mime_type   = "text/plain",
        .read        = crag_stats_resource,
        .userdata    = &ctx,
    });
    if (rc != CMCP_OK) {
        fprintf(stderr, "crag-mcp: register crag://stats failed (%d)\n", rc);
        cmcp_server_free(s);
        embed_backend_free(ctx.embed);
        store_close(&ctx.store);
        return 1;
    }

    int run_rc = cmcp_server_run_stdio(s);

    cmcp_server_free(s);
    embed_backend_free(ctx.embed);
    store_close(&ctx.store);
    curl_global_cleanup();
    return run_rc == CMCP_OK ? 0 : 1;
}
