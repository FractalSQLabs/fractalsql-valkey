/* src/fv_search.c
 * SPDX-License-Identifier: Apache-2.0 AND BSD-2-Clause
 * SPDX-FileCopyrightText: 2026 Daniel Gardiner d/b/a FractalSQLabs
 *
 *   FRACTAL.SEARCH  <corpus_key> <query_blob> [<K>] [SESSION <id>] [INDEX <name>] [NPROBE <n>] [RERANK <n>] [USE_INDEX <0|1>]
 *   FRACTAL.EXPLORE <corpus_key> <query_blob> [<population_size>]
 *   FRACTAL.INDEX.CANDIDATES <corpus_key> <query_blob> INDEX <name> [<K>] [NPROBE <n>] [RERANK <n>]
 *
 * SEARCH also takes IDS <positions> (a comma-separated list or a JSON array of corpus positions): the search is then restricted to
 * those rows. A set of at most 4096 positions, or no index, is scanned exactly; a longer set goes through the index and is
 * filtered, with an exact fallback when fewer than K of its rows survive. Rows appended after an index was built are scanned
 * exactly alongside the index candidates, so a stale index still answers over the whole corpus.
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
 *
 * With INDEX <name> the search is the indexed one: the IVF index (src/fv_index.c) gives the candidates, every candidate is
 * verified against the corpus with the one shared double cosine (fsql_precision.h) and the answer is ordered by distance then
 * corpus position. With USE_INDEX 0 the same exact scan runs without the index (the reference the index is checked against).
 * The indexed search does not apply Diversify repulsion.
 */
#include "fv_common.h"
#include "fsql_precision.h"

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

/* The search modes. */
#define FV_MODE_SFS   (-1)   /* the core's Sniper search (the default) */
#define FV_MODE_EXACT 0      /* the exact scan over the corpus */
#define FV_MODE_INDEX 1      /* the IVF index */
#define FV_IDS_EXACT_MAX 4096 /* an IDS set up to this size is scanned exactly */

typedef struct {
    fv_job  base;            /* first */
    double *query;
    float  *query_f;         /* the query as float32 (the index and exact paths) */
    double *corpus;
    int     dim;
    int     n_corpus;
    int     k;
    int     mode;
    fv_index *index;         /* the resident index (FV_MODE_INDEX) */
    char    corpus_key[256];
    char    index_name[64];
    uint32_t nprobe;
    int     have_nprobe;
    size_t  rerank;
    int     have_rerank;
    int64_t *ids;            /* IDS: the sorted, de-duplicated corpus positions the search is restricted to */
    size_t  n_ids;
    int     have_ids;
    int     cands_only;      /* FRACTAL.INDEX.CANDIDATES: reply with the positions only */
    fv_session *sess;        /* SESSION <id>: search through that session's context */
    char    params[256];
    size_t  params_len;
} search_job;

static void
search_dispose(fv_job *b)
{
    search_job *j = (search_job *) b;
    fv_session_release(j->sess);
    fv_index_release(j->index);
    free(j->query);
    free(j->query_f);
    free(j->corpus);
    free(j->ids);
    free(j->base.res);
    free(j);
}

/* ------------------------------------------------------------------ */
/* the exact (verified) search paths                                  */
/* ------------------------------------------------------------------ */

/* A candidate: a corpus position and its distance. Ties break on the position. */
typedef struct { int64_t pos; double d; } cand_t;

static void
cand_ins(cand_t *a, int *n, int cap, cand_t c)
{
    int i;
    if (*n == cap) {
        cand_t *w = &a[cap - 1];
        if (!(c.d < w->d || (c.d == w->d && c.pos < w->pos))) return;
        i = cap - 1;
    } else i = (*n)++;
    while (i > 0 && (a[i - 1].d > c.d || (a[i - 1].d == c.d && a[i - 1].pos > c.pos))) { a[i] = a[i - 1]; i--; }
    a[i] = c;
}

static int
ids_has(const search_job *j, int64_t p)
{
    size_t lo = 0, hi = j->n_ids;
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        if (j->ids[mid] < p) lo = mid + 1; else hi = mid;
    }
    return lo < j->n_ids && j->ids[lo] == p;
}

static int
cmp_i64(const void *a, const void *b)
{
    int64_t x = *(const int64_t *) a, y = *(const int64_t *) b;
    return x < y ? -1 : (x > y);
}

/* An IDS value: corpus positions, comma or space separated, optionally in [ ]. An empty list is valid (it selects no rows). */
static int
parse_ids(const char *z, size_t l, search_job *j, char *err, size_t errsz)
{
    while (l > 0 && (*z == ' ' || *z == '\t' || *z == '[')) { z++; l--; }
    while (l > 0 && (z[l - 1] == ' ' || z[l - 1] == '\t' || z[l - 1] == ']')) l--;
    size_t cap = 16, n = 0;
    int64_t *v = (int64_t *) malloc(cap * sizeof *v);
    if (v == NULL) { snprintf(err, errsz, "out of memory (IDS)"); return -1; }
    size_t i = 0;
    while (i < l) {
        while (i < l && (z[i] == ',' || z[i] == ' ' || z[i] == '\t')) i++;
        if (i >= l) break;
        if (z[i] < '0' || z[i] > '9') { free(v); snprintf(err, errsz, "IDS must be a list of corpus positions (non-negative integers)"); return -1; }
        long long x = 0;
        while (i < l && z[i] >= '0' && z[i] <= '9') {
            x = x * 10 + (z[i] - '0');
            if (x > 4000000000LL) { free(v); snprintf(err, errsz, "an IDS position is out of range"); return -1; }
            i++;
        }
        if (i < l && z[i] != ',' && z[i] != ' ' && z[i] != '\t') { free(v); snprintf(err, errsz, "IDS must be a list of corpus positions (non-negative integers)"); return -1; }
        if (n == cap) {
            cap *= 2;
            if (cap > 8000000u) { free(v); snprintf(err, errsz, "IDS has too many positions"); return -1; }
            int64_t *t = (int64_t *) realloc(v, cap * sizeof *v);
            if (t == NULL) { free(v); snprintf(err, errsz, "out of memory (IDS)"); return -1; }
            v = t;
        }
        v[n++] = (int64_t) x;
    }
    if (n > 1) {
        qsort(v, n, sizeof *v, cmp_i64);
        size_t w = 1;
        for (size_t r = 1; r < n; r++) if (v[r] != v[w - 1]) v[w++] = v[r];
        n = w;
    }
    j->ids = v; j->n_ids = n; j->have_ids = 1;
    return 0;
}

/* The reply document of the exact and the indexed paths (the same shape the core's Sniper search returns, plus the
 * statistics of the path that produced it). */
static char *
exact_reply(const search_job *j, const cand_t *res, int nres, int used_index,
            uint64_t rows_scanned, uint32_t cells_probed)
{
    fv_sb sb;
    fv_sb_init(&sb);
    fv_sb_appendf(&sb, "{\"dim\":%d,\"n_corpus\":%d,\"best_fit\":%.17g,\"best_point\":[",
                  j->dim, j->n_corpus, nres > 0 ? res[0].d : 1.0);
    if (nres > 0) {
        const double *row = j->corpus + (size_t) res[0].pos * (size_t) j->dim;
        for (int i = 0; i < j->dim; i++)
            fv_sb_appendf(&sb, "%s%.17g", i ? "," : "", row[i]);
    }
    fv_sb_append(&sb, "],\"top_k\":[");
    for (int i = 0; i < nres; i++)
        fv_sb_appendf(&sb, "%s{\"idx\":%lld,\"dist\":%.17g}", i ? "," : "",
                      (long long) res[i].pos, res[i].d);
    fv_sb_appendf(&sb, "],\"used_index\":%s,\"rows_scanned\":%llu,\"cells_probed\":%u}",
                  used_index ? "true" : "false", (unsigned long long) rows_scanned, cells_probed);
    size_t len = 0;
    char *out = fv_sb_take(&sb, &len);
    return out;
}

/* FRACTAL.INDEX.CANDIDATES: the positions only, nearest first. */
static char *
cands_reply(const cand_t *res, int nres)
{
    fv_sb sb;
    fv_sb_init(&sb);
    fv_sb_append(&sb, "[");
    for (int i = 0; i < nres; i++) fv_sb_appendf(&sb, "%s%lld", i ? "," : "", (long long) res[i].pos);
    fv_sb_append(&sb, "]");
    size_t len = 0;
    return fv_sb_take(&sb, &len);
}

/* The exact scan: every row of the corpus, with the shared double cosine. */
static void
search_exact_path(fv_job *b)
{
    search_job *j = (search_job *) b;
    const size_t dim = (size_t) j->dim;
    const size_t n = (size_t) j->n_corpus;
    cand_t *res = (cand_t *) malloc(sizeof(cand_t) * (size_t) (j->k > 0 ? j->k : 1));
    if (res == NULL) { fv_job_fail(b, "out of memory"); return; }
    int nres = 0;
    uint64_t scanned = 0;
    if (j->have_ids) {
        for (size_t i = 0; i < j->n_ids; i++) {
            if (j->ids[i] >= (int64_t) n) break;
            size_t r = (size_t) j->ids[i];
            cand_t c = { (int64_t) r, fsqlp_cosine_distance_ff(j->query_f, j->corpus + r * dim, dim) };
            cand_ins(res, &nres, j->k, c);
            scanned++;
        }
    } else {
        for (size_t r = 0; r < n; r++) {
            cand_t c = { (int64_t) r, fsqlp_cosine_distance_ff(j->query_f, j->corpus + r * dim, dim) };
            cand_ins(res, &nres, j->k, c);
        }
        scanned = (uint64_t) n;
    }
    char *out = j->cands_only ? cands_reply(res, nres) : exact_reply(j, res, nres, 0, scanned, 0);
    free(res);
    if (out == NULL) { fv_job_fail(b, "out of memory"); return; }
    fv_job_bulk(b, out, strlen(out));
}

/* The indexed search: the index gives the candidates, the corpus gives the exact distances. */
static void
search_index_path(fv_job *b)
{
    search_job *j = (search_job *) b;
    char err[256];
    const size_t dim = (size_t) j->dim;
    const size_t n = (size_t) j->n_corpus;
    fv_index_info_t info;
    if (fv_index_get_info(j->index, &info) != 0) { fv_job_fail(b, "the index is not loaded"); return; }
    if ((size_t) info.dim != dim) {
        snprintf(err, sizeof err, "the query dimension %d does not match the index dimension %u", j->dim, info.dim);
        fv_job_fail(b, "%s", err);
        return;
    }
    uint32_t nprobe = j->have_nprobe ? j->nprobe : (info.nprobe_default > 0 ? info.nprobe_default : 16u);
    if (nprobe < 1) nprobe = 1;
    size_t shortlist = j->have_rerank ? j->rerank : info.rerank_default;
    if (shortlist < 1) shortlist = 1;
    size_t R = shortlist > (size_t) j->k ? shortlist : (size_t) j->k + 5u;
    if (R > 1000000u) R = 1000000u;

    int64_t *ids = (int64_t *) malloc(R * sizeof(int64_t));
    double *dists = (double *) malloc(R * sizeof(double));
    cand_t *res = (cand_t *) malloc(sizeof(cand_t) * (size_t) (j->k > 0 ? j->k : 1));
    if (ids == NULL || dists == NULL || res == NULL) {
        free(ids); free(dists); free(res);
        fv_job_fail(b, "out of memory");
        return;
    }
    size_t ncand = 0;
    uint64_t scanned = 0;
    uint32_t probed = 0;
    int rc = fv_index_search(j->index, j->query_f, nprobe, R, ids, dists, &ncand, &scanned, &probed, err, sizeof err);
    if (rc != 0) {
        free(ids); free(dists); free(res);
        fv_job_fail(b, "%s", err);
        return;
    }
    /* verify every candidate against the corpus: the index never returns an approximate distance as an answer */
    int nres = 0;
    for (size_t i = 0; i < ncand; i++) {
        int64_t p = ids[i];
        if (p < 0 || (size_t) p >= n) continue;
        if (j->have_ids && !ids_has(j, p)) continue;
        cand_t c = { p, fsqlp_cosine_distance_ff(j->query_f, j->corpus + (size_t) p * dim, dim) };
        cand_ins(res, &nres, j->k, c);
    }
    /* the rows appended after the build are not in the cells: scan them exactly */
    for (size_t r = (size_t) info.rows; r < n; r++) {
        if (j->have_ids && !ids_has(j, (int64_t) r)) continue;
        cand_t c = { (int64_t) r, fsqlp_cosine_distance_ff(j->query_f, j->corpus + r * dim, dim) };
        cand_ins(res, &nres, j->k, c);
        scanned++;
    }
    free(ids); free(dists);
    if (j->have_ids) {
        size_t valid = 0;
        for (size_t i = 0; i < j->n_ids && j->ids[i] < (int64_t) n; i++) valid++;
        size_t want = valid < (size_t) j->k ? valid : (size_t) j->k;
        if ((size_t) nres < want) {            /* the index shortlist left too few rows of the set: scan the set exactly */
            free(res);
            search_exact_path(b);
            return;
        }
    }
    char *out = j->cands_only ? cands_reply(res, nres) : exact_reply(j, res, nres, 1, scanned, probed);
    free(res);
    if (out == NULL) { fv_job_fail(b, "out of memory"); return; }
    fv_index_note_search(j->corpus_key, j->index_name, 1, scanned, probed);
    fv_job_bulk(b, out, strlen(out));
}

static void
search_work(fv_job *b)
{
    search_job *j = (search_job *) b;
    if (j->mode == FV_MODE_INDEX && j->have_ids && j->n_ids <= FV_IDS_EXACT_MAX && !j->cands_only) { search_exact_path(b); return; }
    if (j->mode == FV_MODE_INDEX) { search_index_path(b); return; }
    if (j->mode == FV_MODE_EXACT) { search_exact_path(b); return; }

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
    /* calloc leaves the mode at 0, which is the exact scan: every command that does not choose a path (EXPLORE) runs
     * the core's search. */
    j->mode = FV_MODE_SFS;

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

    j->query   = (double *) malloc((size_t) j->dim * sizeof(double));
    j->query_f = (float *) malloc((size_t) j->dim * sizeof(float));
    j->corpus  = (double *) malloc((size_t) j->n_corpus * (size_t) j->dim * sizeof(double));
    if (j->query == NULL || j->query_f == NULL || (j->corpus == NULL && j->n_corpus > 0)) {
        why = "out of memory decoding inputs"; goto fail;
    }

    for (int i = 0; i < j->dim; i++) {
        float f;
        memcpy(&f, query_blob + (size_t) i * 4, 4);
        if (!isfinite(f)) { why = "query contains a non-finite value (NaN or Inf)"; goto fail; }
        j->query[i] = (double) f;
        j->query_f[i] = f;
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

/* The trailing arguments of FRACTAL.SEARCH: an optional K, then keyword/value pairs. */
typedef struct {
    int   k;
    int   have_k;
    int   sess_at;
    char  index[64];
    int   have_index;
    long  nprobe;
    int   have_nprobe;
    long  rerank;
    int   have_rerank;
    long  use_index;
    int   have_use_index;
    int   ids_at;
} search_opts;

static int
kw_is(const char *z, size_t l, const char *kw)
{
    size_t n = strlen(kw);
    return l == n && strncasecmp(z, kw, n) == 0;
}

static int
parse_search_opts(RedisModuleString **argv, int argc, search_opts *o, char *err, size_t errsz)
{
    memset(o, 0, sizeof *o);
    o->sess_at = -1;
    o->ids_at = -1;
    o->use_index = 1;
    for (int i = 3; i < argc; ) {
        size_t l = 0;
        const char *z = RedisModule_StringPtrLen(argv[i], &l);
        size_t kl = l;
        if (kw_is(z, kl, "SESSION") || kw_is(z, kl, "INDEX") || kw_is(z, kl, "NPROBE") ||
            kw_is(z, kl, "RERANK") || kw_is(z, kl, "USE_INDEX") || kw_is(z, kl, "IDS")) {
            if (i + 1 >= argc) return -2;   /* a keyword with no value: wrong number of arguments */
            size_t vl = 0;
            const char *v = RedisModule_StringPtrLen(argv[i + 1], &vl);
            if (kw_is(z, kl, "SESSION")) {
                o->sess_at = i;
            } else if (kw_is(z, kl, "IDS")) {
                o->ids_at = i;
            } else if (kw_is(z, kl, "INDEX")) {
                if (vl == 0 || vl >= sizeof o->index) { snprintf(err, errsz, "the index name is missing or too long"); return -1; }
                memcpy(o->index, v, vl); o->index[vl] = '\0';
                o->have_index = 1;
            } else if (kw_is(z, kl, "NPROBE")) {
                o->nprobe = atol(v);
                if (o->nprobe < 1 || o->nprobe > 4096) { snprintf(err, errsz, "NPROBE must be in [1, 4096]"); return -1; }
                o->have_nprobe = 1;
            } else if (kw_is(z, kl, "RERANK")) {
                o->rerank = atol(v);
                if (o->rerank < 0 || o->rerank > 1000000) { snprintf(err, errsz, "RERANK must be in [0, 1000000]"); return -1; }
                o->have_rerank = 1;
            } else {
                o->use_index = atol(v);
                if (o->use_index != 0 && o->use_index != 1) { snprintf(err, errsz, "USE_INDEX must be 0 or 1"); return -1; }
                o->have_use_index = 1;
            }
            i += 2;
            continue;
        }
        /* otherwise it must be the (positional) K */
        if (o->have_k) return -2;   /* a second positional argument: wrong number of arguments */
        long long kv = 0;
        if (RedisModule_StringToLongLong(argv[i], &kv) != REDISMODULE_OK) {
            snprintf(err, errsz, "K must be an integer");
            return -1;
        }
        if (kv < 1 || kv > MAX_K) {
            snprintf(err, errsz, "K %lld out of range [1, %d]", kv, MAX_K);
            return -1;
        }
        o->k = (int) kv;
        o->have_k = 1;
        i++;
    }
    return 0;
}

static int
FractalSearch_Command(RedisModuleCtx *ctx, RedisModuleString **argv, int argc)
{
    if (argc < 3) return RedisModule_WrongArity(ctx);

    search_opts o;
    char err[192];
    int prc = parse_search_opts(argv, argc, &o, err, sizeof err);
    if (prc == -2) return RedisModule_WrongArity(ctx);
    if (prc != 0) return fv_reply_err(ctx, err);

    search_job *j = search_job_load(ctx, argv, /*allow_empty*/ 1);
    if (j == NULL) return REDISMODULE_OK;

    if (open_session_arg(ctx, j, argv, o.sess_at) != 0) return REDISMODULE_OK;

    int k = o.have_k ? o.k : DEFAULT_K;
    if (k > j->n_corpus && j->n_corpus > 0) k = j->n_corpus;
    j->k = k;
    j->mode = FV_MODE_SFS;
    j->have_nprobe = o.have_nprobe;
    j->nprobe = (uint32_t) o.nprobe;
    j->have_rerank = o.have_rerank;
    j->rerank = (size_t) o.rerank;

    /* the corpus key name (for the index statistics) */
    {
        size_t cl = 0;
        const char *ck = RedisModule_StringPtrLen(argv[1], &cl);
        if (cl > 0 && cl < sizeof j->corpus_key) { memcpy(j->corpus_key, ck, cl); j->corpus_key[cl] = '\0'; }
    }

    if (o.have_index) {
        if (!o.have_use_index || o.use_index != 0) {
            if (!fv_index_available()) {
                search_dispose(&j->base);
                return fv_reply_err(ctx, fv_index_missing_message());
            }
            char ierr[256];
            j->index = fv_index_load(ctx, j->corpus_key, o.index, ierr, sizeof ierr);
            if (j->index == NULL) {
                search_dispose(&j->base);
                return fv_reply_err(ctx, ierr);
            }
            snprintf(j->index_name, sizeof j->index_name, "%s", o.index);
            j->mode = FV_MODE_INDEX;
        }
    } else if (o.have_use_index && o.use_index == 0) {
        j->mode = FV_MODE_EXACT;
    }

    if (o.ids_at >= 0) {
        size_t il = 0;
        const char *iz = RedisModule_StringPtrLen(argv[o.ids_at + 1], &il);
        if (parse_ids(iz, il, j, err, sizeof err) != 0) {
            search_dispose(&j->base);
            return fv_reply_err(ctx, err);
        }
        if (j->mode == FV_MODE_SFS) j->mode = FV_MODE_EXACT;   /* a restricted set is always an exact scan */
    }

    memcpy(j->params, SEARCH_PARAMS, sizeof SEARCH_PARAMS);
    j->params_len = sizeof SEARCH_PARAMS - 1;
    return fv_dispatch(ctx, &j->base);
}

/* FRACTAL.INDEX.CANDIDATES <corpus_key> <query_blob> INDEX <name> [K] [NPROBE n] [RERANK n]: the corpus positions of the K nearest
 * rows (verified, nearest first, rows appended since the build included), as a JSON array. */
static int
IndexCandidates_Command(RedisModuleCtx *ctx, RedisModuleString **argv, int argc)
{
    if (argc < 5) return RedisModule_WrongArity(ctx);
    search_opts o;
    char err[192];
    int prc = parse_search_opts(argv, argc, &o, err, sizeof err);
    if (prc == -2) return RedisModule_WrongArity(ctx);
    if (prc != 0) return fv_reply_err(ctx, err);
    if (!o.have_index) return fv_reply_err(ctx, "INDEX <name> is required");
    if (o.ids_at >= 0 || o.sess_at >= 0) return fv_reply_err(ctx, "FRACTAL.INDEX.CANDIDATES takes INDEX, K, NPROBE and RERANK only");
    if (!fv_index_available()) return fv_reply_err(ctx, fv_index_missing_message());

    search_job *j = search_job_load(ctx, argv, /*allow_empty*/ 0);
    if (j == NULL) return REDISMODULE_OK;
    int k = o.have_k ? o.k : DEFAULT_K;
    if (k > j->n_corpus && j->n_corpus > 0) k = j->n_corpus;
    j->k = k;
    j->have_nprobe = o.have_nprobe;
    j->nprobe = (uint32_t) o.nprobe;
    j->have_rerank = o.have_rerank;
    j->rerank = (size_t) o.rerank;
    j->cands_only = 1;
    {
        size_t cl = 0;
        const char *ck = RedisModule_StringPtrLen(argv[1], &cl);
        if (cl > 0 && cl < sizeof j->corpus_key) { memcpy(j->corpus_key, ck, cl); j->corpus_key[cl] = '\0'; }
    }
    char ierr[256];
    j->index = fv_index_load(ctx, j->corpus_key, o.index, ierr, sizeof ierr);
    if (j->index == NULL) { search_dispose(&j->base); return fv_reply_err(ctx, ierr); }
    snprintf(j->index_name, sizeof j->index_name, "%s", o.index);
    j->mode = FV_MODE_INDEX;
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
    if (RedisModule_CreateCommand(ctx, "FRACTAL.INDEX.CANDIDATES", IndexCandidates_Command,
                                  "readonly allow-stale", 1, 1, 1) == REDISMODULE_ERR)
        return REDISMODULE_ERR;
    if (fv_acl(ctx, "FRACTAL.INDEX.CANDIDATES", "read slow") != 0) return REDISMODULE_ERR;
    if (fv_acl(ctx, "FRACTAL.SEARCH", "read slow") != 0 || fv_acl(ctx, "FRACTAL.EXPLORE", "read slow") != 0)
        return REDISMODULE_ERR;
    return REDISMODULE_OK;
}
