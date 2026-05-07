/* crag-mcp — reference MCP server wrapping cRAG.
 *
 * Exposes two tools:
 *
 *   crag_search   { query: string, k?: integer 1..20 }
 *                 Hybrid retrieval (BM25 + cosine, RRF-fused) over the
 *                 cRAG index. Returns up to k chunks, one text content
 *                 item per chunk.
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
 */

#include "cmcp.h"
#include "cmcp_json.h"
#include "cmcp_server.h"

/* cRAG headers — sibling repo, linked statically by the Makefile rule. */
#include "crag.h"
#include "embed.h"
#include "store.h"

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

static int crag_search_handler(const cmcp_json_t *args, void *userdata,
                                cmcp_json_t **out_content, int *out_is_error) {
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
    float        *scores  = (float *)calloc((size_t)k, sizeof *scores);
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

    /* Format one content item per chunk:
     *   [score 0.842] path/to/source.md
     *   <chunk body>                                                  */
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

    for (int i = 0; i < n; i++) {
        const char *src = results[i].source ? results[i].source : "(unknown)";
        const char *txt = results[i].text   ? results[i].text   : "";
        size_t need = strlen(src) + strlen(txt) + 64;
        char *buf = (char *)malloc(need);
        if (buf) {
            snprintf(buf, need, "[score %.3f] %s\n%s",
                      (double)scores[i], src, txt);
            cmcp_json_array_append(arr, text_item(buf));
            free(buf);
        }
        free(results[i].source);
        free(results[i].text);
    }
    free(results); free(scores);

    *out_is_error = 0;
    *out_content  = arr;
    return CMCP_OK;
}

/* ====================================================================== */
/* crag_stats                                                              */
/* ====================================================================== */

static int crag_stats_handler(const cmcp_json_t *args, void *userdata,
                               cmcp_json_t **out_content, int *out_is_error) {
    (void)args;
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
                                cmcp_json_t **out_contents, int *out_is_error) {
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
        .description = "Hybrid retrieval (BM25 + cosine) over the cRAG "
                        "index. Returns up to k chunks ranked by relevance.",
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
        .description = "DB path, chunk count, file count, embedding dim.",
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
