/* src/fv_search.c
 * SPDX-License-Identifier: Apache-2.0 AND BSD-2-Clause
 * SPDX-FileCopyrightText: 2026 Daniel Gardiner d/b/a FractalSQLabs
 *
 *   FRACTAL.SEARCH  <corpus_key> <query_blob> [<K>]
 *   FRACTAL.EXPLORE <corpus_key> <query_blob> [<population_size>]
 *
 * corpus_key holds a packed little-endian float32 BLOB of dim * N floats;
 * dim is the query's length. Both commands reply with the JSON document
 * fsql_search_ptr produces. SEARCH (Sniper) returns the top-k nearest rows;
 * EXPLORE (Scout, walk=0) disperses the population across distinct basins
 * and adds a "population" array.
 *
 * The corpus is validated and decoded to doubles on the main thread, while
 * the key is open (the DMA pointer is only valid then). The search itself
 * runs on a worker against the job's own copy.
 */
#include "fv_common.h"

#include <strings.h>

#define DEFAULT_K     10
#define MAX_K         10000
#define DEFAULT_POP   50
#define MAX_POP       10000
#define MIN_POP       2

static const char SEARCH_PARAMS[] =
    "{\"max_generation\":30,"
    "\"population_size\":50,"
    "\"maximum_diffusion\":2,"
    "\"walk\":0.5,"
    "\"bound_clipping\":true}";

typedef struct {
    fv_job  base;            /* first */
    double *query;
    double *corpus;
    int     dim;
    int     n_corpus;
    int     k;
    fv_session *sess;        /* SESSION <id>: search through that session's context */
    char    params[256];
    size_t  params_len;
} search_job;

static void
search_dispose(fv_job *b)
{
    search_job *j = (search_job *) b;
    fv_session_release(j->sess);
    free(j->query);
    free(j->corpus);
    free(j->base.res);
    free(j);
}

static void
search_work(fv_job *b)
{
    search_job *j = (search_job *) b;
    fsql_ctx *fc = j->sess != NULL ? fv_session_lock(j->sess) : fv_thread_ctx();
    if (fc == NULL) { fv_job_fail(b, "fsql_new failed"); return; }

    const char *json = NULL;
    size_t json_len = 0;
    int rc = fsql_search_ptr(fc,
                             j->corpus, (size_t) j->n_corpus, (size_t) j->dim,
                             j->query, (size_t) j->dim,
                             j->k, j->params, j->params_len,
                             &json, &json_len);
    if (rc != 0 || json == NULL) {
        const char *e = fsql_last_error(fc);
        fv_job_fail(b, "fsql_search_ptr rc=%d: %s", rc, e != NULL ? e : "(no detail)");
        if (j->sess != NULL) fv_session_unlock(j->sess);
        return;
    }
    /* The core's buffer belongs to this worker's ctx and is overwritten by
     * its next call; the reply happens later, on the main thread. */
    char *copy = (char *) malloc(json_len + 1);
    if (copy == NULL) {
        fv_job_fail(b, "out of memory copying the result");
    } else {
        memcpy(copy, json, json_len);
        copy[json_len] = '\0';
        fv_job_bulk(b, copy, json_len);
    }
    if (j->sess != NULL) fv_session_unlock(j->sess);
}

/* Validate argv[1] (corpus key) and argv[2] (query) and decode them into a
 * new job. Returns the job, or NULL after replying with an error. */
static search_job *
search_job_load(RedisModuleCtx *ctx, RedisModuleString **argv, int allow_empty)
{
    RedisModuleKey *kh = RedisModule_OpenKey(ctx, argv[1], REDISMODULE_READ);
    if (kh == NULL) { fv_reply_err(ctx, "corpus key missing"); return NULL; }

    search_job *j = (search_job *) calloc(1, sizeof *j);
    if (j == NULL) { RedisModule_CloseKey(kh); fv_reply_err(ctx, "out of memory"); return NULL; }
    j->base.dispose = search_dispose;
    j->base.work = search_work;

    const char *why = NULL;
    size_t corpus_len = 0, query_len = 0;
    const char *corpus_blob = RedisModule_StringDMA(kh, &corpus_len, REDISMODULE_READ);
    const char *query_blob  = RedisModule_StringPtrLen(argv[2], &query_len);

    if (corpus_blob == NULL) why = "corpus key not a string/blob";
    else if (corpus_len > FV_MAX_CORPUS_BYTES)
        why = "corpus too large (max 268435456 bytes / 256 MiB)";
    else if (query_blob == NULL || query_len == 0 || query_len % 4 != 0)
        why = "query must be packed float32 BLOB (length % 4 == 0)";
    else if (query_len > FV_MAX_QUERY_BYTES)
        why = "query too large (max 4194304 bytes / 4 MiB)";
    if (why != NULL) goto fail;

    /* query_len <= 4 MiB, so dim <= 1 Mi and fits an int. */
    j->dim = (int) (query_len / 4);
    if (corpus_len % ((size_t) j->dim * 4) != 0) { why = "corpus length not a multiple of dim * 4"; goto fail; }
    /* corpus_len <= 256 MiB, so n_corpus * dim <= 64 Mi floats: the double
     * buffer is at most 512 MiB and cannot overflow size_t. */
    j->n_corpus = (int) (corpus_len / ((size_t) j->dim * 4));
    if (j->n_corpus < 1 && !allow_empty) { why = "empty corpus - nothing to explore"; goto fail; }

    j->query  = (double *) malloc((size_t) j->dim * sizeof(double));
    j->corpus = (double *) malloc((size_t) j->n_corpus * (size_t) j->dim * sizeof(double));
    if (j->query == NULL || (j->corpus == NULL && j->n_corpus > 0)) { why = "out of memory decoding inputs"; goto fail; }

    for (int i = 0; i < j->dim; i++) {
        float f;
        memcpy(&f, query_blob + (size_t) i * 4, 4);
        if (!isfinite(f)) { why = "query contains a non-finite value (NaN or Inf)"; goto fail; }
        j->query[i] = (double) f;
    }
    for (size_t i = 0; i < (size_t) j->n_corpus * (size_t) j->dim; i++) {
        float f;
        memcpy(&f, corpus_blob + i * 4, 4);
        if (!isfinite(f)) { why = "corpus contains a non-finite value (NaN or Inf)"; goto fail; }
        j->corpus[i] = (double) f;
    }
    j->base.bytes = ((size_t) j->dim + (size_t) j->n_corpus * (size_t) j->dim) * sizeof(double);
    RedisModule_CloseKey(kh);   /* everything the worker needs is now copied */
    return j;

fail:
    RedisModule_CloseKey(kh);
    search_dispose(&j->base);
    fv_reply_err(ctx, why);
    return NULL;
}

/* Index of a SESSION keyword in argv[3..], or -1. */
static int
take_session(RedisModuleString **argv, int argc)
{
    for (int i = 3; i < argc; i++) {
        size_t l;
        const char *z = RedisModule_StringPtrLen(argv[i], &l);
        if (l == 7 && strncasecmp(z, "SESSION", 7) == 0) return i;
    }
    return -1;
}

/* Attaches the named session to the job. 0 on success (or no SESSION);
 * -1 after replying with an error and disposing the job. */
static int
open_session_arg(RedisModuleCtx *ctx, search_job *j, RedisModuleString **argv, int at)
{
    if (at < 0) return 0;
    char err[160];
    j->sess = fv_session_acquire(argv[at + 1], 0, err, sizeof err);
    if (j->sess == NULL) {
        search_dispose(&j->base);
        fv_reply_err(ctx, err);
        return -1;
    }
    return 0;
}

static int
FractalSearch_Command(RedisModuleCtx *ctx, RedisModuleString **argv, int argc)
{
    if (argc < 3 || argc > 6) return RedisModule_WrongArity(ctx);

    search_job *j = search_job_load(ctx, argv, /*allow_empty*/ 1);
    if (j == NULL) return REDISMODULE_OK;

    /* [K] [SESSION id]. K: [1, MAX_K], then limited to the corpus size. */
    int sess_at = take_session(argv, argc);
    int kargs = (sess_at > 0 ? sess_at : argc) - 3;
    if (kargs > 1 || (sess_at > 0 && sess_at + 2 != argc) || (sess_at < 0 && argc > 4)) {
        search_dispose(&j->base);
        return RedisModule_WrongArity(ctx);
    }
    if (open_session_arg(ctx, j, argv, sess_at) != 0) return REDISMODULE_OK;
    int k = DEFAULT_K;
    if (kargs == 1) {
        long long kv = 0;
        if (RedisModule_StringToLongLong(argv[3], &kv) != REDISMODULE_OK) {
            search_dispose(&j->base);
            return fv_reply_err(ctx, "K must be an integer");
        }
        if (kv < 1 || kv > MAX_K) {
            search_dispose(&j->base);
            char m[96];
            snprintf(m, sizeof m, "K %lld out of range [1, %d]", kv, MAX_K);
            return fv_reply_err(ctx, m);
        }
        k = (int) kv;
    }
    if (k > j->n_corpus && j->n_corpus > 0) k = j->n_corpus;
    j->k = k;
    memcpy(j->params, SEARCH_PARAMS, sizeof SEARCH_PARAMS);
    j->params_len = sizeof SEARCH_PARAMS - 1;
    return fv_dispatch(ctx, &j->base);
}

static int
FractalExplore_Command(RedisModuleCtx *ctx, RedisModuleString **argv, int argc)
{
    if (argc < 3 || argc > 6) return RedisModule_WrongArity(ctx);

    search_job *j = search_job_load(ctx, argv, /*allow_empty*/ 0);
    if (j == NULL) return REDISMODULE_OK;

    int sess_at = take_session(argv, argc);
    int pargs = (sess_at > 0 ? sess_at : argc) - 3;
    if (pargs > 1 || (sess_at > 0 && sess_at + 2 != argc) || (sess_at < 0 && argc > 4)) {
        search_dispose(&j->base);
        return RedisModule_WrongArity(ctx);
    }
    if (open_session_arg(ctx, j, argv, sess_at) != 0) return REDISMODULE_OK;
    int pop_size = DEFAULT_POP;
    if (pargs == 1) {
        long long pv = 0;
        if (RedisModule_StringToLongLong(argv[3], &pv) != REDISMODULE_OK) {
            search_dispose(&j->base);
            return fv_reply_err(ctx, "population_size must be an integer");
        }
        if (pv < MIN_POP || pv > MAX_POP) {
            search_dispose(&j->base);
            char m[112];
            snprintf(m, sizeof m, "population_size %lld out of range [%d, %d]", pv, MIN_POP, MAX_POP);
            return fv_reply_err(ctx, m);
        }
        pop_size = (int) pv;
    }
    j->k = 1;
    int n = snprintf(j->params, sizeof j->params,
        "{\"max_generation\":15,\"population_size\":%d,"
        "\"maximum_diffusion\":2,\"walk\":0,"
        "\"return_population\":true,\"bound_clipping\":true}",
        pop_size);
    j->params_len = (n > 0 && (size_t) n < sizeof j->params) ? (size_t) n : 0;
    return fv_dispatch(ctx, &j->base);
}

int
fv_register_search(RedisModuleCtx *ctx)
{
    if (RedisModule_CreateCommand(ctx, "FRACTAL.SEARCH", FractalSearch_Command,
                                  "readonly allow-stale", 1, 1, 1) == REDISMODULE_ERR)
        return REDISMODULE_ERR;
    if (RedisModule_CreateCommand(ctx, "FRACTAL.EXPLORE", FractalExplore_Command,
                                  "readonly allow-stale", 1, 1, 1) == REDISMODULE_ERR)
        return REDISMODULE_ERR;
    if (fv_acl(ctx, "FRACTAL.SEARCH", "read slow") != 0 || fv_acl(ctx, "FRACTAL.EXPLORE", "read slow") != 0)
        return REDISMODULE_ERR;
    return REDISMODULE_OK;
}
