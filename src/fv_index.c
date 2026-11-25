/* src/fv_index.c -- the native IVF index for the Valkey host.
 * SPDX-License-Identifier: Apache-2.0 AND BSD-2-Clause
 * SPDX-FileCopyrightText: 2026 Daniel Gardiner d/b/a FractalSQLabs
 *
 * The index is a k-means (IVF) partition of the corpus that lives in ONE Valkey string key (packed little-endian float32).
 * The core does the maths (fsql_ivf_*: the k-means cells, the INT8 / INT4 / FP16 / FP32 codes, the candidate search and the
 * recall calibration); this file owns the storage and the command surface:
 *
 *   FRACTAL.INDEX.CREATE  <corpus_key> <name> [options]   build an index and store it
 *   FRACTAL.INDEX.REBUILD <corpus_key> <name> [options]   rebuild it from the current corpus
 *   FRACTAL.INDEX.DROP    <corpus_key> <name>              remove it
 *   FRACTAL.INDEX.ENSURE  <corpus_key> <name> [options]   create, rebuild, refresh or leave the index, whichever is needed
 *   FRACTAL.INDEX.REFRESH <corpus_key> <name>              fold the rows appended since the build into the cells (no retraining)
 *   FRACTAL.INDEX.INFO    <corpus_key> <name>              its status and advice, format, sizes, added rows and last search
 *   FRACTAL.INDEX.LIST    <corpus_key>                     one summary per index of the corpus
 *   FRACTAL.SEARCH ... INDEX <name>                        the indexed search (an INDEX option)
 *
 * Storage (ordinary string keys, so RDB and AOF persist them with no custom data type):
 *
 *   <corpus_key>:fsql-idx:<name>:gen                "3"                     the generation pointer
 *   <corpus_key>:fsql-idx:<name>:meta:<gen>          JSON metadata
 *   <corpus_key>:fsql-idx:<name>:model:<gen>         the model blob
 *   <corpus_key>:fsql-idx:<name>:cell:<gen>:<cell>   [u32 count][int64 ids][codes]
 *
 * A build writes a NEW generation and flips the generation pointer with one SET, so a reader only ever sees a complete
 * generation. The keys are written from the reply callback (the main thread) and replicated with RedisModule_Replicate, so a
 * replica receives the bytes and never re-runs the build. The key names keep the corpus key as their prefix, so in cluster mode
 * the index lives in the corpus key's slot (which needs a hash tag; a corpus key without one is refused there).
 *
 * Ids are corpus POSITIONS (the blob has no row ids). The build records the corpus key's length and an FNV-1a checksum. Rows
 * appended after it leave the covered bytes unchanged, so they are reported as added, scanned exactly by the search and folded
 * into the cells by FRACTAL.INDEX.REFRESH; a change to the covered bytes is a rewrite and needs a rebuild.
 *
 * Every candidate is verified: the shortlist rows are read back from the corpus and their exact double cosine distances are
 * computed with the one shared implementation (fsql_precision.h), ordered by distance then position. The index never returns
 * an approximate distance as an answer. The indexed search does not apply Diversify repulsion (FRACTAL.SEARCH keeps the
 * Sniper behaviour of the core).
 */
#include "fv_common.h"
#include "fsql_precision.h"

#include <pthread.h>
#include <strings.h>

#define FI_MAXDIM         8192
#define FI_MAX_CACHED     8
#define FI_MAX_LAST       16
#define FI_FORMAT_VERSION 1
/* The calibration holds a normalised copy of the corpus for the exact ground truth; above this it is skipped. */
#define FI_CALIB_MAX_BYTES ((size_t)128 * 1024 * 1024)

static const char *const FI_FMT_NAME[4] = { "INT8", "INT4", "FP16", "FP32" };

/* --------------------------------------------------------------------------------------------------------------- */
/* The core's IVF maths. The vendored core archive must carry it: the linker extracts an archive member only to */
/* resolve an undefined STRONG reference, so a weak reference would leave the symbol NULL -- the index would then be  */
/* silently unavailable even with a fresh archive. The declarations below therefore refer to the core's own header */
/* (a strong reference), and a build against a core that predates the index is opted out of explicitly with          */
/* -DFSQL_NO_IVF: the commands stay registered and report that this core has no index.                                 */
/* --------------------------------------------------------------------------------------------------------------- */
#if defined(FSQL_NO_IVF)
/* 0: this build was told the core has no IVF maths. */
static int fi_ivf_available(void) { return 0; }
#else
/* 1: the core's IVF maths is linked in. */
static int fi_ivf_available(void) { return 1; }
#endif

static const char FI_NO_IVF_MSG[] =
    "fractalsql: this build of fractalsql-core does not carry the IVF index "
    "(rebuild the core and refresh include/<platform>)";

int fv_index_available(void) { return fi_ivf_available(); }

/* The model blob belongs to the core's allocator, and the symbol itself may be absent on a core that predates the
 * index: both are handled here, so the failure paths can free unconditionally. */
#if defined(__GNUC__)
#  pragma GCC diagnostic push
#  pragma GCC diagnostic ignored "-Waddress"
#endif
static void fi_free_blob(void *p)
{
    if (p == NULL) return;
    if (fsql_ivf_free == NULL) return;
    fsql_ivf_free(p);
}
#if defined(__GNUC__)
#  pragma GCC diagnostic pop
#endif
const char *fv_index_missing_message(void) { return FI_NO_IVF_MSG; }

/* --------------------------------------------------------------------------------------------------------------- */
/* small helpers                                                                                                          */
/* --------------------------------------------------------------------------------------------------------------- */

static int
fi_ident_ok(const char *s)
{
    if (s == NULL || *s == '\0') return 0;
    size_t n = strlen(s);
    if (n > 63) return 0;
    for (size_t i = 0; i < n; i++) {
        char c = s[i];
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_' ||
              (i > 0 && c >= '0' && c <= '9'))) return 0;
    }
    return 1;
}

static uint64_t
fi_fnv1a(const void *bytes, size_t n)
{
    const uint8_t *p = (const uint8_t *) bytes;
    uint64_t h = 0xcbf29ce484222325ULL;
    for (size_t i = 0; i < n; i++) { h ^= p[i]; h *= 0x100000001b3ULL; }
    return h;
}

static void
fi_hex64(char *out, size_t cap, uint64_t v)
{
    snprintf(out, cap, "%016llx", (unsigned long long) v);
}

/* The metadata is a flat JSON object written by this file; this finds a top-level value. */
static const char *
fi_json_get(const char *json, const char *key)
{
    if (json == NULL || key == NULL) return NULL;
    size_t klen = strlen(key);
    const char *p = json;
    int depth = 0;
    while (*p != '\0') {
        if (*p == '"') {
            const char *s = ++p;
            while (*p != '\0' && *p != '"') { if (*p == '\\' && p[1] != '\0') p++; p++; }
            if (*p != '"') return NULL;
            size_t n = (size_t) (p - s);
            p++;
            if (depth == 1 && n == klen && memcmp(s, key, klen) == 0) {
                while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') p++;
                if (*p == ':') {
                    p++;
                    while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') p++;
                    return p;
                }
            }
            continue;
        }
        if (*p == '{' || *p == '[') depth++;
        else if (*p == '}' || *p == ']') depth--;
        p++;
    }
    return NULL;
}

static double
fi_json_num(const char *json, const char *key, double dflt)
{
    const char *p = fi_json_get(json, key);
    if (p == NULL) return dflt;
    char *e = NULL;
    double v = strtod(p, &e);
    if (e == p || v != v) return dflt;
    return v;
}

static int
fi_json_str(const char *json, const char *key, char *out, size_t cap)
{
    out[0] = '\0';
    const char *p = fi_json_get(json, key);
    if (p == NULL || *p != '"') return 0;
    p++;
    size_t n = 0;
    while (*p != '\0' && *p != '"' && n + 1 < cap) out[n++] = *p++;
    out[n] = '\0';
    return 1;
}

/* The key names. The corpus key is the prefix, so every key of one index shares its hash tag (cluster mode). */
static int
fi_key(char *out, size_t cap, const char *corpus, const char *name, const char *suffix)
{
    int n = snprintf(out, cap, "%s:fsql-idx:%s%s", corpus, name, suffix);
    return (n > 0 && (size_t) n < cap) ? 0 : -1;
}

static size_t
fi_default_rerank(int fmt) { return fmt == FSQL_IVF_FMT_INT4 ? 40u : (fmt == FSQL_IVF_FMT_INT8 ? 30u : 0u); }

/* --------------------------------------------------------------------------------------------------------------- */
/* the resident index                                                                                                      */
/* --------------------------------------------------------------------------------------------------------------- */

struct fv_index {
    char      corpus[256];
    char      name[64];
    uint32_t  dim, nlist, pca_dim, code_bytes;
    int       fmt, generation;
    uint32_t  nprobe_default;
    size_t    rerank_default;
    uint64_t  rows;                 /* the rows the build covered: corpus positions 0 .. rows-1 */
    int       refs;                 /* the cache holds one, every user holds one */
    fsql_ivf_model *m;
    fsql_ivf_index *x;
};

static pthread_mutex_t g_mu = PTHREAD_MUTEX_INITIALIZER;
static struct fv_index *g_cache[FI_MAX_CACHED];
static int              g_ncache;

static struct {
    char      corpus[256];
    char      name[64];
    int       used;
    uint64_t  rows_scanned;
    uint32_t  cells_probed;
} g_last[FI_MAX_LAST];

static void
fi_entry_free(struct fv_index *x)
{
    if (x == NULL) return;
    if (x->x) fsql_ivf_index_free(x->x);
    if (x->m) fsql_ivf_model_free(x->m);
    free(x);
}

static void
fi_unref(struct fv_index *x)
{
    if (x == NULL) return;
    int dead = 0;
    pthread_mutex_lock(&g_mu);
    x->refs--;
    if (x->refs <= 0) dead = 1;
    pthread_mutex_unlock(&g_mu);
    if (dead) fi_entry_free(x);
}

static void
fi_last_set(const char *corpus, const char *name, int used, uint64_t scanned, uint32_t probed)
{
    pthread_mutex_lock(&g_mu);
    int slot = -1;
    for (int i = 0; i < FI_MAX_LAST; i++)
        if (!strcmp(g_last[i].corpus, corpus) && !strcmp(g_last[i].name, name)) { slot = i; break; }
    if (slot < 0) {
        for (int i = 0; i < FI_MAX_LAST; i++)
            if (g_last[i].corpus[0] == '\0') { slot = i; break; }
    }
    if (slot < 0) slot = 0;
    snprintf(g_last[slot].corpus, sizeof g_last[slot].corpus, "%s", corpus);
    snprintf(g_last[slot].name, sizeof g_last[slot].name, "%s", name);
    g_last[slot].used = used;
    g_last[slot].rows_scanned = scanned;
    g_last[slot].cells_probed = probed;
    pthread_mutex_unlock(&g_mu);
}

static int
fi_last_get(const char *corpus, const char *name, uint64_t *scanned, uint32_t *probed)
{
    int used = 0;
    pthread_mutex_lock(&g_mu);
    for (int i = 0; i < FI_MAX_LAST; i++)
        if (!strcmp(g_last[i].corpus, corpus) && !strcmp(g_last[i].name, name)) {
            used = g_last[i].used;
            if (scanned) *scanned = g_last[i].rows_scanned;
            if (probed)  *probed  = g_last[i].cells_probed;
            break;
        }
    pthread_mutex_unlock(&g_mu);
    return used;
}

/* Reads a string key into a malloc'd buffer. Returns 0 and sets *out (malloc'd, NUL-terminated) or -1 with err. */
static int
fi_read_key(RedisModuleCtx *ctx, const char *key, char **out, size_t *len_out, char *err, size_t errsz)
{
    *out = NULL;
    if (len_out) *len_out = 0;
    RedisModuleString *ks = RedisModule_CreateString(ctx, key, strlen(key));
    if (ks == NULL) { snprintf(err, errsz, "out of memory (site 280)"); return -1; }
    RedisModuleKey *kh = RedisModule_OpenKey(ctx, ks, REDISMODULE_READ);
    if (kh == NULL) { RedisModule_FreeString(ctx, ks); snprintf(err, errsz, "out of memory (site 282)"); return -1; }
    size_t len = 0;
    const char *p = RedisModule_StringDMA(kh, &len, REDISMODULE_READ);
    int rc = 0;
    if (p == NULL) {
        snprintf(err, errsz, "no such key");
        rc = -1;
    } else {
        char *buf = (char *) malloc(len + 1);
        if (buf == NULL) { snprintf(err, errsz, "out of memory (site 291)"); rc = -1; }
        else {
            memcpy(buf, p, len);
            buf[len] = '\0';
            *out = buf;
            if (len_out) *len_out = len;
        }
    }
    RedisModule_CloseKey(kh);
    RedisModule_FreeString(ctx, ks);
    return rc;
}

/* Writes one ordinary string key and replicates the write (the replica gets the bytes, it never re-runs the build). */
static int
fi_write_key(RedisModuleCtx *ctx, const char *key, const void *val, size_t len)
{
    RedisModuleString *ks = RedisModule_CreateString(ctx, key, strlen(key));
    if (ks == NULL) return -1;
    RedisModuleKey *kh = RedisModule_OpenKey(ctx, ks, REDISMODULE_WRITE);
    if (kh == NULL) { RedisModule_FreeString(ctx, ks); return -1; }
    RedisModuleString *vs = RedisModule_CreateString(ctx, (const char *) val, len);
    int rc = (vs == NULL) ? -1 : RedisModule_StringSet(kh, vs);
    RedisModule_CloseKey(kh);
    RedisModule_FreeString(ctx, ks);
    if (rc != REDISMODULE_OK) return -1;
    /* The write is replicated as it is: the replica receives the finished bytes and never re-runs the build. A
     * replication that the server refuses is logged, not fatal: the local write has already happened. */
    if (RedisModule_Replicate(ctx, "SET", "sb", key, val, len) != REDISMODULE_OK)
        RedisModule_Log(ctx, "warning", "fractalsql: the write of '%s' could not be replicated", key);
    return 0;
}

static int
fi_delete_key(RedisModuleCtx *ctx, const char *key)
{
    RedisModuleString *ks = RedisModule_CreateString(ctx, key, strlen(key));
    if (ks == NULL) return -1;
    RedisModuleKey *kh = RedisModule_OpenKey(ctx, ks, REDISMODULE_WRITE);
    if (kh == NULL) { RedisModule_FreeString(ctx, ks); return -1; }
    int rc = RedisModule_DeleteKey(kh);
    RedisModule_CloseKey(kh);
    RedisModule_FreeString(ctx, ks);
    if (rc == REDISMODULE_OK) RedisModule_Replicate(ctx, "DEL", "s", key);
    return rc == REDISMODULE_OK ? 0 : -1;
}

/* Loads the index (or returns the resident one). The generation pointer names the generation to load. */
struct fv_index *
fv_index_load(RedisModuleCtx *ctx, const char *corpus_key, const char *name, char *err, size_t errsz)
{
    if (!fv_index_available()) { snprintf(err, errsz, "%s", FI_NO_IVF_MSG); return NULL; }

    char key[512];
    if (fi_key(key, sizeof key, corpus_key, name, ":gen") != 0) {
        snprintf(err, errsz, "the index key name is too long");
        return NULL;
    }
    char *genbuf = NULL;
    size_t genlen = 0;
    if (fi_read_key(ctx, key, &genbuf, &genlen, err, errsz) != 0) {
        snprintf(err, errsz, "no index named '%s' for corpus '%s'", name, corpus_key);
        return NULL;
    }
    long gen = strtol(genbuf, NULL, 10);
    free(genbuf);
    if (gen <= 0) {
        snprintf(err, errsz, "index '%s' has a corrupt generation pointer", name);
        return NULL;
    }

    /* resident? */
    pthread_mutex_lock(&g_mu);
    for (int i = 0; i < g_ncache; i++)
        if (g_cache[i] != NULL && g_cache[i]->generation == (int) gen &&
            !strcmp(g_cache[i]->corpus, corpus_key) && !strcmp(g_cache[i]->name, name)) {
            struct fv_index *hit = g_cache[i];
            hit->refs++;
            pthread_mutex_unlock(&g_mu);
            return hit;
        }
    pthread_mutex_unlock(&g_mu);

    /* load it */
    struct fv_index *x = (struct fv_index *) calloc(1, sizeof *x);
    if (x == NULL) { snprintf(err, errsz, "out of memory (site 376)"); return NULL; }
    snprintf(x->corpus, sizeof x->corpus, "%s", corpus_key);
    snprintf(x->name, sizeof x->name, "%s", name);
    x->generation = (int) gen;
    x->refs = 1;                       /* the caller's reference */

    char base[512], meta_key[640], model_key[640], cell_key[640];
    if (fi_key(base, sizeof base, corpus_key, name, "") != 0) { fi_entry_free(x); snprintf(err, errsz, "the index key name is too long"); return NULL; }
    snprintf(meta_key, sizeof meta_key, "%s:meta:%ld", base, gen);
    snprintf(model_key, sizeof model_key, "%s:model:%ld", base, gen);

    char *meta = NULL;
    size_t metalen = 0;
    if (fi_read_key(ctx, meta_key, &meta, &metalen, err, errsz) != 0) {
        fi_entry_free(x);
        snprintf(err, errsz, "index '%s' has no metadata for generation %ld", name, gen);
        return NULL;
    }
    x->dim = (uint32_t) fi_json_num(meta, "dim", 0);
    x->nlist = (uint32_t) fi_json_num(meta, "nlist", 0);
    x->pca_dim = (uint32_t) fi_json_num(meta, "pca_dim", 0);
    x->code_bytes = (uint32_t) fi_json_num(meta, "code_bytes", 0);
    x->fmt = (int) fi_json_num(meta, "format_id", -1);
    x->nprobe_default = (uint32_t) fi_json_num(meta, "nprobe_default", 0);
    x->rows = (uint64_t) fi_json_num(meta, "rows", 0);
    free(meta);
    if (x->dim == 0 || x->nlist == 0 || x->fmt < 0 || x->fmt > 3) {
        fi_entry_free(x);
        snprintf(err, errsz, "index '%s' has corrupt metadata", name);
        return NULL;
    }
    x->rerank_default = fi_default_rerank(x->fmt);

    char *model = NULL;
    size_t modellen = 0;
    if (fi_read_key(ctx, model_key, &model, &modellen, err, errsz) != 0) {
        fi_entry_free(x);
        snprintf(err, errsz, "index '%s' has no model for generation %ld", name, gen);
        return NULL;
    }
    int rc = 0;
    x->m = fsql_ivf_model_load((const uint8_t *) model, modellen, &rc);
    free(model);
    if (x->m == NULL) {
        fi_entry_free(x);
        snprintf(err, errsz, "index '%s' has a corrupt model; rebuild it", name);
        return NULL;
    }
    fsql_ivf_info_t info;
    if (fsql_ivf_model_info(x->m, &info) != FSQL_OK || info.dim != x->dim || info.nlist != x->nlist) {
        fi_entry_free(x);
        snprintf(err, errsz, "index '%s' has a corrupt model; rebuild it", name);
        return NULL;
    }
    x->code_bytes = info.code_bytes;
    x->x = fsql_ivf_index_new(x->m);
    if (x->x == NULL) {
        fi_entry_free(x);
        snprintf(err, errsz, "out of memory (site 433) loading index '%s'", name);
        return NULL;
    }

    /* the cells */
    int64_t total = 0;
    for (uint32_t c = 0; c < x->nlist; c++) {
        snprintf(cell_key, sizeof cell_key, "%s:fsql-idx:%s:cell:%ld:%u", corpus_key, name, gen, c);
        char *cell = NULL;
        size_t celllen = 0;
        if (fi_read_key(ctx, cell_key, &cell, &celllen, err, errsz) != 0) {
            fi_entry_free(x);
            snprintf(err, errsz, "index '%s' is incomplete (cell %u of %u is missing)", name, c, x->nlist);
            return NULL;
        }
        if (celllen < 4) {
            free(cell); fi_entry_free(x);
            snprintf(err, errsz, "index '%s' is corrupt (cell %u)", name, c);
            return NULL;
        }
        uint32_t n = 0;
        memcpy(&n, cell, 4);
        size_t want = 4 + (size_t) n * 8 + (size_t) n * x->code_bytes;
        if (celllen != want) {
            free(cell); fi_entry_free(x);
            snprintf(err, errsz, "index '%s' is corrupt (cell %u has %zu bytes, expected %zu)", name, c, celllen, want);
            return NULL;
        }
        int64_t *ids = NULL;
        if (n > 0) {
            ids = (int64_t *) malloc((size_t) n * sizeof(int64_t));
            if (ids == NULL) { free(cell); fi_entry_free(x); snprintf(err, errsz, "out of memory (site 464)"); return NULL; }
            const uint8_t *ip = (const uint8_t *) cell + 4;
            for (uint32_t i = 0; i < n; i++) {
                uint64_t u = 0;
                for (int b = 0; b < 8; b++) u |= (uint64_t) ip[(size_t) i * 8 + (size_t) b] << (8 * b);
                ids[i] = (int64_t) u;
            }
        }
        const uint8_t *codes = (const uint8_t *) cell + 4 + (size_t) n * 8;
        int src = fsql_ivf_index_set_cell(x->x, c, ids, codes, (size_t) n);
        free(ids);
        free(cell);
        if (src != FSQL_OK) {
            fi_entry_free(x);
            snprintf(err, errsz, "out of memory (site 478) loading index '%s'", name);
            return NULL;
        }
        total += (int64_t) n;
    }
    (void) total;

    /* publish it in the cache (replacing an older generation of the same index) */
    pthread_mutex_lock(&g_mu);
    struct fv_index *old = NULL;
    for (int i = 0; i < g_ncache; i++)
        if (g_cache[i] != NULL && !strcmp(g_cache[i]->corpus, corpus_key) && !strcmp(g_cache[i]->name, name)) {
            old = g_cache[i];
            g_cache[i] = x;
            break;
        }
    if (old == NULL) {
        if (g_ncache < FI_MAX_CACHED) g_cache[g_ncache++] = x;
        else {
            /* evict the first entry nobody is using */
            for (int i = 0; i < g_ncache; i++)
                if (g_cache[i]->refs <= 1) { old = g_cache[i]; g_cache[i] = x; break; }
            if (old == NULL) {
                pthread_mutex_unlock(&g_mu);
                fi_entry_free(x);
                snprintf(err, errsz, "too many resident indexes (limit %d)", FI_MAX_CACHED);
                return NULL;
            }
        }
    }
    x->refs++;                        /* the cache's reference */
    pthread_mutex_unlock(&g_mu);
    if (old != NULL) fi_unref(old);
    return x;
}

void
fv_index_release(struct fv_index *x) { fi_unref(x); }

/* Records the statistics of the last search of an index (FRACTAL.INDEX.INFO reports them). */
void
fv_index_note_search(const char *corpus_key, const char *name, int used, uint64_t scanned, uint32_t probed)
{
    if (corpus_key == NULL || name == NULL) return;
    fi_last_set(corpus_key, name, used, scanned, probed);
}

int
fv_index_get_info(const struct fv_index *x, fv_index_info_t *out)
{
    if (x == NULL || out == NULL) return -1;
    memset(out, 0, sizeof *out);
    snprintf(out->name, sizeof out->name, "%s", x->name);
    out->dim = x->dim;
    out->nlist = x->nlist;
    out->pca_dim = x->pca_dim;
    out->code_bytes = x->code_bytes;
    out->fmt = x->fmt;
    out->generation = x->generation;
    out->nprobe_default = x->nprobe_default;
    out->rerank_default = x->rerank_default;
    out->rows = x->rows;
    return 0;
}

/* The candidates (corpus positions) for a query. The distances are the core's approximate ones: the caller verifies them. */
int
fv_index_search(struct fv_index *x, const float *query, uint32_t nprobe, size_t shortlist,
                int64_t *ids, double *dists, size_t *n_out, uint64_t *rows_scanned, uint32_t *cells_probed,
                char *err, size_t errsz)
{
    if (!fv_index_available()) { snprintf(err, errsz, "%s", FI_NO_IVF_MSG); return -1; }
    if (x == NULL || x->x == NULL) { snprintf(err, errsz, "the index is not loaded"); return -1; }
    if (nprobe < 1) nprobe = 1;
    if (nprobe > x->nlist) nprobe = x->nlist;
    if (shortlist < 1) shortlist = 1;
    if (shortlist > 1000000u) shortlist = 1000000u;
    fsql_ivf_stats_t st;
    memset(&st, 0, sizeof st);
    size_t n = 0;
    int rc = fsql_ivf_search(x->x, query, nprobe, (uint32_t) shortlist, ids, dists, &n, &st);
    if (rc != FSQL_OK) { snprintf(err, errsz, "the index search failed"); return -1; }
    if (rows_scanned) *rows_scanned = st.rows_scanned;
    if (cells_probed) *cells_probed = st.cells_probed;
    *n_out = n;
    return 0;
}

/* --------------------------------------------------------------------------------------------------------------- */
/* the options                                                                                                             */
/* --------------------------------------------------------------------------------------------------------------- */

typedef struct {
    char      name[64];
    int       have_dim;
    uint32_t  dim;
    int       fmt;
    uint32_t  nlist, pca_dim, iters, sample;
    uint64_t  seed;
    double    target_recall;
    uint32_t  calib_samples;
} fi_options;

static int
fi_parse_fmt(const char *s)
{
    for (int i = 0; i < 4; i++) if (!strcmp(s, FI_FMT_NAME[i])) return i;
    return -1;
}

static int
fi_parse_options(RedisModuleString **argv, int from, int argc, fi_options *o, char *err, size_t errsz)
{
    memset(o, 0, sizeof *o);
    o->fmt = FSQL_IVF_FMT_INT8;
    o->seed = 1;
    o->target_recall = 0.95;
    o->calib_samples = 200;
    for (int i = from; i < argc; i += 2) {
        if (i + 1 >= argc) { snprintf(err, errsz, "%s needs a value", "an option"); return -1; }
        size_t l = 0, vl = 0;
        const char *k = RedisModule_StringPtrLen(argv[i], &l);
        const char *v = RedisModule_StringPtrLen(argv[i + 1], &vl);
        char key[32], val[64];
        if (l >= sizeof key) { snprintf(err, errsz, "unknown option"); return -1; }
        memcpy(key, k, l); key[l] = '\0';
        if (vl >= sizeof val) { snprintf(err, errsz, "the value of %s is too long", key); return -1; }
        memcpy(val, v, vl); val[vl] = '\0';

        if (!strcmp(key, "DIM")) {
            long long n = atoll(val);
            if (n < 1 || n > FI_MAXDIM) { snprintf(err, errsz, "DIM must be in [1, %d]", FI_MAXDIM); return -1; }
            o->dim = (uint32_t) n; o->have_dim = 1;
        } else if (!strcmp(key, "VECTOR_FORMAT")) {
            int f = fi_parse_fmt(val);
            if (f < 0) { snprintf(err, errsz, "VECTOR_FORMAT must be INT8, INT4, FP16 or FP32 (got '%s')", val); return -1; }
            o->fmt = f;
        } else if (!strcmp(key, "NLIST")) {
            long long n = atoll(val);
            if (n < 0 || n > 4096) { snprintf(err, errsz, "NLIST must be in [0, 4096]"); return -1; }
            o->nlist = (uint32_t) n;
        } else if (!strcmp(key, "PCA_DIM")) {
            long long n = atoll(val);
            if (n < 0 || n > FI_MAXDIM) { snprintf(err, errsz, "PCA_DIM must be in [0, %d]", FI_MAXDIM); return -1; }
            o->pca_dim = (uint32_t) n;
        } else if (!strcmp(key, "ITERS")) {
            long long n = atoll(val);
            if (n < 0 || n > 100) { snprintf(err, errsz, "ITERS must be in [0, 100]"); return -1; }
            o->iters = (uint32_t) n;
        } else if (!strcmp(key, "SAMPLE")) {
            long long n = atoll(val);
            if (n < 0 || n > 1000000) { snprintf(err, errsz, "SAMPLE must be in [0, 1000000]"); return -1; }
            o->sample = (uint32_t) n;
        } else if (!strcmp(key, "SEED")) {
            long long n = atoll(val);
            if (n < 0) { snprintf(err, errsz, "SEED must be >= 0"); return -1; }
            o->seed = (uint64_t) n;
            if (o->seed == 0) o->seed = 1;
        } else if (!strcmp(key, "TARGET_RECALL")) {
            double d = atof(val);
            if (!(d > 0.0 && d <= 1.0)) { snprintf(err, errsz, "TARGET_RECALL must be in (0, 1]"); return -1; }
            o->target_recall = d;
        } else if (!strcmp(key, "CALIBRATION_SAMPLES")) {
            long long n = atoll(val);
            if (n < 0 || n > 2000) { snprintf(err, errsz, "CALIBRATION_SAMPLES must be in [0, 2000]"); return -1; }
            o->calib_samples = (uint32_t) n;
        } else {
            snprintf(err, errsz, "unknown option '%s'", key);
            return -1;
        }
    }
    return 0;
}

/* --------------------------------------------------------------------------------------------------------------- */
/* the build                                                                                                               */
/* --------------------------------------------------------------------------------------------------------------- */

typedef struct {
    fv_job    base;
    char      corpus[256];
    char      name[64];
    fi_options opt;
    uint32_t  generation;
    /* the corpus (the raw float32 copy) */
    float    *vecs;
    size_t    n_rows;
    size_t    dim;
    size_t    corpus_bytes;
    uint64_t  corpus_hash;
    /* the results (worker) */
    uint8_t  *model;
    size_t    model_len;
    uint32_t  code_bytes;
    uint32_t  nlist;
    uint8_t **cells;          /* one payload per cell */
    size_t   *cell_len;
    size_t    n_cells;
    char     *meta;           /* the metadata JSON */
    uint32_t  calibrated_nprobe;
    int       calibrated;
    /* a refresh: the generation it extends (read on the main thread) */
    uint8_t  *old_model;
    size_t    old_model_len;
    uint8_t **old_cells;
    size_t   *old_cell_len;
    size_t    old_ncells;
    size_t    old_rows;
    char     *old_meta;
    char      action[12];     /* FRACTAL.INDEX.ENSURE: created | rebuilt | refreshed (empty for the plain commands) */
} index_job;

/* FRACTAL.INDEX.ENSURE: the command that ran (created / rebuilt / refreshed) is named in the reply. */
static const char *fi_pending_action = NULL;

/* Takes ownership of `summary` (a JSON object) and returns it with "action" and "status" first when the job came from ENSURE. */
static char *
fi_tag_summary(const index_job *j, char *summary, size_t *len)
{
    if (j->action[0] == '\0' || summary == NULL || summary[0] != '{') return summary;
    char head[64];
    int hn = snprintf(head, sizeof head, "{\"action\":\"%s\",\"status\":\"ready\",", j->action);
    char *out = (char *) malloc((size_t) hn + *len + 1);
    if (out == NULL) return summary;
    memcpy(out, head, (size_t) hn);
    memcpy(out + hn, summary + 1, *len);          /* the rest of the object, including its closing brace and NUL */
    free(summary);
    *len += (size_t) hn - 1;
    return out;
}

static void
index_dispose(fv_job *b)
{
    index_job *j = (index_job *) b;
    free(j->vecs);
    fi_free_blob(j->model);
    if (j->cells != NULL) {
        for (size_t i = 0; i < j->n_cells; i++) free(j->cells[i]);
        free(j->cells);
    }
    free(j->cell_len);
    free(j->meta);
    free(j->old_model);
    if (j->old_cells != NULL) {
        for (size_t i = 0; i < j->old_ncells; i++) free(j->old_cells[i]);
        free(j->old_cells);
    }
    free(j->old_cell_len);
    free(j->old_meta);
    free(j->base.res);
    free(j);
}

/* Monte-Carlo calibration: sample rows as queries (each left out of its own ground truth), compare the index with the exact
 * search at nprobe 1, 2, 4 ... nlist and recommend the smallest nprobe whose 95% lower bound reaches the target. */
static int
fi_calibrate(fsql_ivf_index *x, const fsql_ivf_info_t *info, const float *Xn, const int64_t *ids, size_t n, size_t dim,
             double target, unsigned samples, uint64_t seed, uint32_t shortlist, char *calib, size_t ccap, uint32_t *nprobe_out)
{
    const unsigned K = 10;
    if (samples < 20u) samples = 20u;
    if (samples > 2000u) samples = 2000u;
    if (n < (size_t) K + 2u) return 1;
    if ((size_t) samples > n) samples = (unsigned) n;

    uint64_t *pos = (uint64_t *) malloc(samples * sizeof(uint64_t));
    float *Q = (float *) malloc((size_t) samples * dim * sizeof(float));
    int64_t *self = (int64_t *) malloc(samples * sizeof(int64_t));
    int64_t *truth = (int64_t *) malloc((size_t) samples * K * sizeof(int64_t));
    if (pos == NULL || Q == NULL || self == NULL || truth == NULL) {
        free(pos); free(Q); free(self); free(truth);
        return -1;
    }
    int rc = fsql_ivf_sample_rows((uint64_t) n, samples, seed, pos);
    for (unsigned i = 0; rc == FSQL_OK && i < samples; i++) {
        memcpy(Q + (size_t) i * dim, Xn + (size_t) pos[i] * dim, dim * sizeof(float));
        self[i] = ids[pos[i]];
        size_t nt = 0;
        rc = fsql_ivf_exact_topk(Xn, ids, n, dim, Xn + (size_t) pos[i] * dim, self[i], 1, K, truth + (size_t) i * K, &nt);
        if (rc == FSQL_OK && nt != K) rc = FSQL_ERR_INVALID;
    }
    uint32_t np[24];
    size_t nnp = 0;
    for (uint32_t v = 1; v < info->nlist && nnp < 22; v *= 2u) np[nnp++] = v;
    np[nnp++] = info->nlist;
    double mean[24], se[24], lo[24], sc[24];
    uint32_t rec = 0;
    int er = FSQL_ERR_INVALID;
    if (rc == FSQL_OK)
        er = fsql_ivf_estimate_recall(x, Q, self, truth, samples, K, np, nnp, shortlist, target, mean, se, lo, sc, &rec);
    int ret = (er == FSQL_OK || er == 1) ? 0 : -1;
    if (ret == 0) {
        size_t len = 0;
        len += (size_t) snprintf(calib + len, ccap - len,
            "{\"samples\":%u,\"k\":%u,\"target\":%g,\"shortlist\":%u,\"estimates\":[", samples, K, target, shortlist);
        for (size_t a = 0; a < nnp && len + 160 < ccap; a++)
            len += (size_t) snprintf(calib + len, ccap - len,
                "%s{\"nprobe\":%u,\"recall\":%.4f,\"se\":%.4f,\"ci95_low\":%.4f,\"mean_rows_scanned\":%.0f}",
                a ? "," : "", np[a], mean[a], se[a], lo[a], sc[a]);
        snprintf(calib + len, ccap - len, "],\"recommended_nprobe\":%u,\"reaches_target\":%s}",
                 rec, er == FSQL_OK ? "true" : "false");
        *nprobe_out = rec;
    }
    free(pos); free(Q); free(self); free(truth);
    return ret;
}

static void
index_work(fv_job *b)
{
    index_job *j = (index_job *) b;
    if (!fv_index_available()) { fv_job_fail(b, "%s", FI_NO_IVF_MSG); return; }

    const size_t dim = j->dim;
    const size_t n = j->n_rows;
    if (n == 0 || dim == 0) { fv_job_fail(b, "the corpus is empty"); return; }

    /* the corpus must be finite throughout: a NaN would poison every distance it takes part in */
    for (size_t i = 0; i < n * dim; i++) {
        if (!isfinite(j->vecs[i])) {
            fv_job_fail(b, "the corpus contains a non-finite value (NaN or Inf)");
            return;
        }
    }

    /* pass 1: the training sample */
    fsql_ivf_options_t opt;
    memset(&opt, 0, sizeof opt);
    opt.struct_size = sizeof opt;
    opt.dim = (uint32_t) dim;
    opt.format = (uint32_t) j->opt.fmt;
    opt.nlist = j->opt.nlist;
    opt.pca_dim = (j->opt.fmt <= FSQL_IVF_FMT_INT4) ? j->opt.pca_dim : 0u;
    opt.kmeans_iters = j->opt.iters;
    opt.sample = j->opt.sample;
    opt.seed = j->opt.seed;
    int rc = 0;
    fsql_ivf_trainer *t = fsql_ivf_trainer_new(&opt, &rc);
    if (t == NULL) {
        fv_job_fail(b, "the index options are not valid for dimension %zu "
                      "(INT8 and INT4 need a dimension of at most 4096)", dim);
        return;
    }
    if (fsql_ivf_trainer_add(t, j->vecs, n) != FSQL_OK) {
        fsql_ivf_trainer_free(t);
        fv_job_fail(b, "the training sample could not be built");
        return;
    }
    uint8_t *model = NULL;
    size_t model_len = 0;
    int frc = fsql_ivf_trainer_finish(t, &model, &model_len);
    fsql_ivf_trainer_free(t);
    if (frc != FSQL_OK || model == NULL) {
        fv_job_fail(b, "the index model could not be trained");
        return;
    }
    fsql_ivf_model *m = fsql_ivf_model_load(model, model_len, &rc);
    if (m == NULL) {
        fi_free_blob(model);
        fv_job_fail(b, "the trained model is not valid");
        return;
    }
    fsql_ivf_info_t info;
    if (fsql_ivf_model_info(m, &info) != FSQL_OK) {
        fsql_ivf_model_free(m);
        fi_free_blob(model);
        fv_job_fail(b, "the trained model is not valid");
        return;
    }

    /* pass 2: encode every row */
    fsql_ivf_index *x = fsql_ivf_index_new(m);
    if (x == NULL) {
        fsql_ivf_model_free(m);
        fi_free_blob(model);
        fv_job_fail(b, "out of memory (site 818) (the resident index could not be allocated)");
        return;
    }
    size_t code_bytes = info.code_bytes;
    uint32_t nlist = info.nlist;
    size_t *counts = (size_t *) calloc(nlist ? nlist : 1, sizeof(size_t));
    uint32_t *cells_of = (uint32_t *) malloc(n * sizeof(uint32_t));
    uint8_t  *codes = (uint8_t *) malloc(n * code_bytes + 1);
    int64_t  *ids = (int64_t *) malloc(n * sizeof(int64_t));
    if (counts == NULL || cells_of == NULL || codes == NULL || ids == NULL) {
        free(counts); free(cells_of); free(codes); free(ids);
        fsql_ivf_index_free(x); fsql_ivf_model_free(m); fi_free_blob(model);
        fv_job_fail(b, "out of memory (site 830) (the cell buffers could not be allocated)");
        return;
    }
    for (size_t r = 0; r < n; r++) {
        uint32_t cell = 0;
        if (fsql_ivf_encode(m, j->vecs + r * dim, &cell, codes + r * code_bytes) != FSQL_OK || cell >= nlist) {
            free(counts); free(cells_of); free(codes); free(ids);
            fsql_ivf_index_free(x); fsql_ivf_model_free(m); fi_free_blob(model);
            fv_job_fail(b, "a corpus row could not be encoded");
            return;
        }
        cells_of[r] = cell;
        counts[cell]++;
        ids[r] = (int64_t) r;                 /* the id is the corpus position */
    }

    /* the per-cell payloads */
    uint8_t **payloads = (uint8_t **) calloc(nlist ? nlist : 1, sizeof(uint8_t *));
    size_t *plen = (size_t *) calloc(nlist ? nlist : 1, sizeof(size_t));
    if (payloads == NULL || plen == NULL) {
        free(payloads); free(plen);
        free(counts); free(cells_of); free(codes); free(ids);
        fsql_ivf_index_free(x); fsql_ivf_model_free(m); fi_free_blob(model);
        fv_job_fail(b, "out of memory (site 853) (the per-cell payloads could not be allocated)");
        return;
    }
    size_t *fill = (size_t *) calloc(nlist ? nlist : 1, sizeof(size_t));
    if (fill == NULL) {
        free(payloads); free(plen);
        free(counts); free(cells_of); free(codes); free(ids);
        fsql_ivf_index_free(x); fsql_ivf_model_free(m); fi_free_blob(model);
        fv_job_fail(b, "out of memory (site 861)");
        return;
    }
    for (uint32_t c = 0; c < nlist; c++) {
        size_t k = counts[c];
        size_t bytes = 4 + k * 8 + k * code_bytes;
        uint8_t *buf = (uint8_t *) malloc(bytes ? bytes : 1);
        if (buf == NULL) {
            for (uint32_t d = 0; d < c; d++) free(payloads[d]);
            free(payloads); free(plen); free(fill);
            free(counts); free(cells_of); free(codes); free(ids);
            fsql_ivf_index_free(x); fsql_ivf_model_free(m); fi_free_blob(model);
            fv_job_fail(b, "out of memory (site 873)");
            return;
        }
        uint32_t n32 = (uint32_t) k;
        memcpy(buf, &n32, 4);
        payloads[c] = buf;
        plen[c] = bytes;
    }
    for (size_t r = 0; r < n; r++) {
        uint32_t c = cells_of[r];
        size_t k = counts[c];                     /* the rows in this cell */
        size_t off = 4 + fill[c] * 8;             /* this row's slot in the id block */
        uint64_t u = (uint64_t) ids[r];
        for (int bb = 0; bb < 8; bb++) payloads[c][off + (size_t) bb] = (uint8_t) (u >> (8 * bb));
        memcpy(payloads[c] + 4 + k * 8 + fill[c] * code_bytes, codes + r * code_bytes, code_bytes);
        fill[c]++;
    }
    free(fill);

    /* The resident index must carry the same cells the keys do. Without this the calibration
     * measures an EMPTY index (recall 0.0), concludes that no probe count reaches the target
     * and recommends probing every cell -- so every search scanned the whole corpus. */
    for (uint32_t c = 0; c < nlist; c++) {
        size_t k = counts[c];
        const int64_t *cids = k ? (const int64_t *)(const void *)(payloads[c] + 4) : NULL;
        const uint8_t *ccodes = k ? payloads[c] + 4 + k * 8 : NULL;
        if (fsql_ivf_index_set_cell(x, c, cids, ccodes, k) != FSQL_OK) {
            for (uint32_t d = 0; d < nlist; d++) free(payloads[d]);
            free(payloads); free(plen);
            free(counts); free(cells_of); free(codes); free(ids);
            fsql_ivf_index_free(x); fsql_ivf_model_free(m); fi_free_blob(model);
            fv_job_fail(b, "the resident index could not be filled with the built cells");
            return;
        }
    }

    free(counts); free(cells_of); free(codes); free(ids);

    /* the calibration (the exact ground truth needs a normalised copy of the corpus) */
    char calib[4096];
    calib[0] = '\0';
    uint32_t calibrated_nprobe = 0;
    int calibrated = 0;
    if (j->opt.calib_samples > 0 && j->opt.target_recall > 0.0 && j->corpus_bytes <= FI_CALIB_MAX_BYTES) {
        float *Xn = (float *) malloc(n * dim * sizeof(float));
        int64_t *xids = (int64_t *) malloc(n * sizeof(int64_t));
        if (Xn != NULL && xids != NULL) {
            int ok = 1;
            for (size_t r = 0; r < n && ok; r++)
                if (fsql_ivf_normalize(j->vecs + r * dim, dim, Xn + r * dim) != FSQL_OK) ok = 0;
            if (ok) {
                for (size_t r = 0; r < n; r++) xids[r] = (int64_t) r;
                uint32_t shortlist = (uint32_t) fi_default_rerank(j->opt.fmt);
                if (shortlist < 15u) shortlist = 15u;
                int crc = fi_calibrate(x, &info, Xn, xids, n, dim, j->opt.target_recall,
                                       j->opt.calib_samples, j->opt.seed, shortlist, calib, sizeof calib, &calibrated_nprobe);
                if (crc == 0) calibrated = 1;
                else if (crc == 1) snprintf(calib, sizeof calib, "{\"skipped\":\"the corpus is too small to calibrate\"}");
            }
            free(Xn);
            free(xids);
        }
    } else if (j->opt.calib_samples > 0) {
        snprintf(calib, sizeof calib, "{\"skipped\":\"the corpus is too large to hold for the exact ground truth\"}");
    }

    /* the metadata */
    char sum[32];
    fi_hex64(sum, sizeof sum, j->corpus_hash);
    fv_sb mb;
    fv_sb_init(&mb);
    fv_sb_appendf(&mb, "{\"name\":\"%s\",\"corpus_key\":\"%s\",\"format\":\"%s\",\"format_id\":%d,\"dim\":%zu,"
                       "\"rows\":%zu,\"nlist\":%u,\"pca_dim\":%u,\"code_bytes\":%zu,\"generation\":%u,"
                       "\"format_version\":%d,\"seed\":%llu,\"corpus_len\":%zu,\"corpus_checksum\":\"%s\","
                       "\"nprobe_default\":%u,\"calibration\":%s",
                 j->name, j->corpus, FI_FMT_NAME[j->opt.fmt], j->opt.fmt, dim, n, nlist, info.pca_dim, code_bytes,
                 j->generation, FI_FORMAT_VERSION, (unsigned long long) j->opt.seed, j->corpus_bytes, sum,
                 calibrated ? calibrated_nprobe : 0u, calib[0] ? calib : "{}");
    fv_sb_append(&mb, "}");
    size_t mlen = 0;
    char *meta = fv_sb_take(&mb, &mlen);
    if (meta == NULL) {
        for (uint32_t d = 0; d < nlist; d++) free(payloads[d]);
        free(payloads); free(plen);
        fsql_ivf_index_free(x); fsql_ivf_model_free(m); fi_free_blob(model);
        fv_job_fail(b, "out of memory (site 940)");
        return;
    }

    j->model = model;
    j->model_len = model_len;
    j->code_bytes = code_bytes;
    j->nlist = nlist;
    j->cells = payloads;
    j->cell_len = plen;
    j->n_cells = nlist;
    j->meta = meta;
    j->calibrated = calibrated;
    j->calibrated_nprobe = calibrated_nprobe;

    fsql_ivf_index_free(x);
    fsql_ivf_model_free(m);

    /* the summary the command replies with */
    fv_sb sb;
    fv_sb_init(&sb);
    fv_sb_appendf(&sb, "{\"name\":\"%s\",\"corpus_key\":\"%s\",\"format\":\"%s\",\"dim\":%zu,\"rows\":%zu,"
                       "\"nlist\":%u,\"pca_dim\":%u,\"code_bytes\":%zu,\"generation\":%u,\"corpus_len\":%zu,"
                       "\"corpus_checksum\":\"%s\",\"bytes\":%zu",
                 j->name, j->corpus, FI_FMT_NAME[j->opt.fmt], dim, n, nlist, info.pca_dim, code_bytes,
                 j->generation, j->corpus_bytes, sum, n * (8 + code_bytes));
    if (calibrated && calibrated_nprobe > 0)
        fv_sb_appendf(&sb, ",\"calibrated_nprobe\":%u", calibrated_nprobe);
    fv_sb_append(&sb, "}");
    size_t slen = 0;
    char *summary = fv_sb_take(&sb, &slen);
    if (summary == NULL) { fv_job_fail(b, "out of memory (site 971)"); return; }
    summary = fi_tag_summary(j, summary, &slen);
    fv_job_bulk(b, summary, slen);
}

/* Deletes every key of one generation of an index: its cells (the count is in its metadata), its model and its
 * metadata. The generation pointer is not touched. */
static void
fi_delete_generation(RedisModuleCtx *ctx, const char *corpus, const char *name, unsigned long long gen)
{
    char key[640], err[64], *meta = NULL;
    size_t metalen = 0;
    uint32_t nlist = 0;
    snprintf(key, sizeof key, "%s:fsql-idx:%s:meta:%llu", corpus, name, gen);
    if (fi_read_key(ctx, key, &meta, &metalen, err, sizeof err) == 0) {
        nlist = (uint32_t) fi_json_num(meta, "nlist", 0);
        free(meta);
    }
    for (uint32_t c = 0; c < nlist; c++) {
        snprintf(key, sizeof key, "%s:fsql-idx:%s:cell:%llu:%u", corpus, name, gen, c);
        fi_delete_key(ctx, key);
    }
    snprintf(key, sizeof key, "%s:fsql-idx:%s:model:%llu", corpus, name, gen);
    fi_delete_key(ctx, key);
    snprintf(key, sizeof key, "%s:fsql-idx:%s:meta:%llu", corpus, name, gen);
    fi_delete_key(ctx, key);
}

/* The reply callback (the main thread): the keys are written here and replicated. */
static int
index_reply(RedisModuleCtx *ctx, fv_job *b)
{
    index_job *j = (index_job *) b;
    if (j->base.kind != FV_R_BULK) return fv_reply_err(ctx, j->base.err[0] ? j->base.err : "the index build failed");

    char key[640];
    const unsigned long long gen = (unsigned long long) j->generation;

    /* the model */
    snprintf(key, sizeof key, "%s:fsql-idx:%s:model:%llu", j->corpus, j->name, gen);
    if (fi_write_key(ctx, key, j->model, j->model_len) != 0)
        return fv_reply_err(ctx, "the index model could not be written");
    /* the cells */
    for (uint32_t c = 0; c < j->n_cells; c++) {
        snprintf(key, sizeof key, "%s:fsql-idx:%s:cell:%llu:%u", j->corpus, j->name, gen, c);
        if (fi_write_key(ctx, key, j->cells[c], j->cell_len[c]) != 0)
            return fv_reply_err(ctx, "an index cell could not be written");
    }
    /* the metadata */
    snprintf(key, sizeof key, "%s:fsql-idx:%s:meta:%llu", j->corpus, j->name, gen);
    if (fi_write_key(ctx, key, j->meta, strlen(j->meta)) != 0)
        return fv_reply_err(ctx, "the index metadata could not be written");
    /* the generation pointer: the flip that publishes the generation */
    snprintf(key, sizeof key, "%s:fsql-idx:%s:gen", j->corpus, j->name);
    char genbuf[32];
    int glen = snprintf(genbuf, sizeof genbuf, "%llu", gen);
    if (fi_write_key(ctx, key, genbuf, (size_t) glen) != 0)
        return fv_reply_err(ctx, "the index generation pointer could not be written");

    /* the pointer has flipped: the generation it replaced is garbage now (a rebuild would otherwise leak every
     * previous generation's cells) */
    if (gen > 1) fi_delete_generation(ctx, j->corpus, j->name, gen - 1);

    return RedisModule_ReplyWithStringBuffer(ctx, j->base.res, j->base.res_len);
}

/* --------------------------------------------------------------------------------------------------------------- */
/* the commands                                                                                                            */
/* --------------------------------------------------------------------------------------------------------------- */

/* The corpus key's bytes, copied out (the job owns them). */
static int
fi_load_corpus(RedisModuleCtx *ctx, RedisModuleString *keyname, index_job *j, char *err, size_t errsz)
{
    RedisModuleKey *kh = RedisModule_OpenKey(ctx, keyname, REDISMODULE_READ);
    if (kh == NULL) { snprintf(err, errsz, "the corpus key is missing"); return -1; }
    size_t len = 0;
    const char *p = RedisModule_StringDMA(kh, &len, REDISMODULE_READ);
    int rc = 0;
    if (p == NULL) {
        snprintf(err, errsz, "the corpus key is not a string");
        rc = -1;
    } else if (len == 0) {
        snprintf(err, errsz, "the corpus key is empty");
        rc = -1;
    } else if (len > FV_MAX_CORPUS_BYTES) {
        snprintf(err, errsz, "the corpus is too large (max %u bytes)", (unsigned) FV_MAX_CORPUS_BYTES);
        rc = -1;
    } else {
        float *buf = (float *) malloc(len);
        if (buf == NULL) { snprintf(err, errsz, "out of memory (site 1033) copying the corpus"); rc = -1; }
        else {
            memcpy(buf, p, len);
            j->vecs = buf;
            j->corpus_bytes = len;
            j->corpus_hash = fi_fnv1a(buf, len);
        }
    }
    RedisModule_CloseKey(kh);
    return rc;
}

/* In cluster mode every key a command touches must be in one slot: the index keys keep the corpus key's prefix, so the
 * corpus key needs a hash tag. */
static int
fi_cluster_ok(RedisModuleCtx *ctx, const char *corpus_key, char *err, size_t errsz)
{
    int flags = RedisModule_GetContextFlags(ctx);
    if ((flags & REDISMODULE_CTX_FLAGS_CLUSTER) == 0) return 0;
    const char *open = strchr(corpus_key, '{');
    if (open != NULL) {
        const char *close = strchr(open + 1, '}');
        if (close != NULL && close > open + 1) return 0;
    }
    snprintf(err, errsz, "in cluster mode the corpus key must carry a hash tag "
                         "(for example 'corpus:{1}'), so that the index lives in the same slot");
    return -1;
}

static int
fi_prepare(RedisModuleCtx *ctx, RedisModuleString **argv, int argc, int opt_from, index_job *j,
           char *err, size_t errsz, uint32_t dim_hint)
{
    memset(j, 0, sizeof *j);
    j->base.dispose = index_dispose;
    j->base.work = index_work;
    j->base.reply = index_reply;
    if (fi_pending_action != NULL) { snprintf(j->action, sizeof j->action, "%s", fi_pending_action); fi_pending_action = NULL; }

    if (!fv_index_available()) { snprintf(err, errsz, "%s", FI_NO_IVF_MSG); return -1; }
    if (fi_cluster_ok(ctx, RedisModule_StringPtrLen(argv[1], NULL), err, errsz) != 0) return -1;

    size_t cl = 0;
    const char *corpus = RedisModule_StringPtrLen(argv[1], &cl);
    if (cl == 0 || cl >= sizeof j->corpus) { snprintf(err, errsz, "the corpus key name is missing or too long"); return -1; }
    memcpy(j->corpus, corpus, cl); j->corpus[cl] = '\0';

    size_t nl = 0;
    const char *name = RedisModule_StringPtrLen(argv[2], &nl);
    if (nl == 0 || nl >= sizeof j->name) { snprintf(err, errsz, "the index name is missing or too long"); return -1; }
    memcpy(j->name, name, nl); j->name[nl] = '\0';
    if (!fi_ident_ok(j->name)) { snprintf(err, errsz, "the index name must be a plain identifier"); return -1; }

    if (fi_parse_options(argv, opt_from, argc, &j->opt, err, errsz) != 0) return -1;
    if (!j->opt.have_dim) {
        /* A rebuild reuses the dimension of the index it replaces; a first build must be told it
         * (the corpus key holds rows of a fixed dimension, which the query length does not reveal). */
        if (dim_hint == 0) {
            snprintf(err, errsz, "DIM is required: the corpus key holds rows of a fixed dimension, "
                                 "which the query length does not reveal");
            return -1;
        }
        j->opt.dim = dim_hint;
        j->opt.have_dim = 1;
    }
    j->dim = j->opt.dim;

    if (fi_load_corpus(ctx, argv[1], j, err, errsz) != 0) return -1;
    if (j->corpus_bytes % (j->dim * 4) != 0) {
        snprintf(err, errsz, "the corpus length (%zu bytes) is not a multiple of the dimension (%zu) times four",
                 j->corpus_bytes, j->dim);
        return -1;
    }
    j->n_rows = j->corpus_bytes / (j->dim * 4);
    if (j->n_rows == 0) { snprintf(err, errsz, "the corpus is empty"); return -1; }
    j->base.bytes = j->corpus_bytes;
    return 0;
}

/* The current generation of an index (0 when there is none). */
static long
fi_generation(RedisModuleCtx *ctx, const char *corpus, const char *name)
{
    char key[512], *buf = NULL;
    size_t len = 0;
    char err[64];
    if (fi_key(key, sizeof key, corpus, name, ":gen") != 0) return 0;
    if (fi_read_key(ctx, key, &buf, &len, err, sizeof err) != 0) return 0;
    long gen = strtol(buf, NULL, 10);
    free(buf);
    return gen > 0 ? gen : 0;
}

static int
IndexCreate_Command(RedisModuleCtx *ctx, RedisModuleString **argv, int argc)
{
    if (argc < 3) return RedisModule_WrongArity(ctx);
    index_job *j = (index_job *) calloc(1, sizeof *j);
    if (j == NULL) return fv_reply_err(ctx, "out of memory (site 1124)");
    j->base.dispose = index_dispose;
    char err[256];
    if (fi_prepare(ctx, argv, argc, 3, j, err, sizeof err, 0) != 0) {
        index_dispose(&j->base);
        return fv_reply_err(ctx, err);
    }
    long gen = fi_generation(ctx, j->corpus, j->name);
    if (gen > 0) {
        index_dispose(&j->base);
        return fv_reply_err(ctx, "an index with that name already exists (use FRACTAL.INDEX.REBUILD or drop it first)");
    }
    j->generation = 1;
    return fv_dispatch(ctx, &j->base);
}

static int
IndexRebuild_Command(RedisModuleCtx *ctx, RedisModuleString **argv, int argc)
{
    if (argc < 3) return RedisModule_WrongArity(ctx);
    index_job *j = (index_job *) calloc(1, sizeof *j);
    if (j == NULL) return fv_reply_err(ctx, "out of memory (site 1145)");
    j->base.dispose = index_dispose;
    char err[256];
    /* A rebuild takes the dimension from the index it replaces, so the caller does not have to
     * repeat it (and cannot get it wrong). */
    uint32_t dim_hint = 0;
    {
        char corpus[256], name[64];
        size_t cl = 0, nl = 0;
        const char *cp = RedisModule_StringPtrLen(argv[1], &cl);
        const char *np = RedisModule_StringPtrLen(argv[2], &nl);
        if (cl > 0 && cl < sizeof corpus && nl > 0 && nl < sizeof name) {
            memcpy(corpus, cp, cl); corpus[cl] = '\0';
            memcpy(name, np, nl); name[nl] = '\0';
            long g = fi_generation(ctx, corpus, name);
            if (g > 0) {
                char base[512], mkey[640];
                if (fi_key(base, sizeof base, corpus, name, "") == 0) {
                    snprintf(mkey, sizeof mkey, "%s:meta:%ld", base, g);
                    char *meta = NULL; size_t ml = 0; char e2[128];
                    if (fi_read_key(ctx, mkey, &meta, &ml, e2, sizeof e2) == 0 && meta != NULL) {
                        dim_hint = (uint32_t) fi_json_num(meta, "dim", 0);
                        free(meta);
                    }
                }
            }
        }
    }
    if (fi_prepare(ctx, argv, argc, 3, j, err, sizeof err, dim_hint) != 0) {
        index_dispose(&j->base);
        return fv_reply_err(ctx, err);
    }
    long gen = fi_generation(ctx, j->corpus, j->name);
    if (gen <= 0) {
        index_dispose(&j->base);
        return fv_reply_err(ctx, "no index with that name for that corpus");
    }
    j->generation = (uint32_t) (gen + 1);
    return fv_dispatch(ctx, &j->base);
}

static int
IndexDrop_Command(RedisModuleCtx *ctx, RedisModuleString **argv, int argc)
{
    if (argc != 3) return RedisModule_WrongArity(ctx);
    if (!fv_index_available()) return fv_reply_err(ctx, FI_NO_IVF_MSG);
    size_t cl = 0;
    const char *corpus = RedisModule_StringPtrLen(argv[1], &cl);
    size_t nl = 0;
    const char *name = RedisModule_StringPtrLen(argv[2], &nl);
    if (cl == 0 || nl == 0 || !fi_ident_ok(name)) return fv_reply_err(ctx, "the index name must be a plain identifier");
    long gen = fi_generation(ctx, corpus, name);
    if (gen <= 0) return fv_reply_err(ctx, "no index with that name for that corpus");

    char key[640];
    fi_delete_generation(ctx, corpus, name, (unsigned long long) gen);
    snprintf(key, sizeof key, "%s:fsql-idx:%s:gen", corpus, name);
    fi_delete_key(ctx, key);

    /* the resident copy must go too */
    struct fv_index *old = NULL;
    pthread_mutex_lock(&g_mu);
    for (int i = 0; i < g_ncache; i++)
        if (g_cache[i] != NULL && !strcmp(g_cache[i]->corpus, corpus) && !strcmp(g_cache[i]->name, name)) {
            old = g_cache[i];
            for (int j = i; j + 1 < g_ncache; j++) g_cache[j] = g_cache[j + 1];
            g_cache[--g_ncache] = NULL;
            break;
        }
    pthread_mutex_unlock(&g_mu);
    if (old != NULL) fi_unref(old);

    return RedisModule_ReplyWithLongLong(ctx, 1);
}

/* ---------------------------------------------------------------- health, status and advice */

/* Advice thresholds (advice, never an error): refresh while the rows added since the build are at most a quarter of the index,
 * otherwise rebuild. */
#define FI_REFRESH_MAX_FRAC 0.25

typedef struct {
    uint64_t built_rows, built_len, now_len, rows_now, added;
    int      have_now;          /* the corpus key exists and is a string */
    int      changed;           /* the rows the build covered are not what they were (rewritten or shortened) */
    int      have_calib, reaches_target;
} fi_health;

/* The raw text of a top-level value of the metadata (a number, a string with its quotes, an object or an array). */
static size_t
fi_json_raw(const char *json, const char *key, char *out, size_t cap)
{
    const char *s = fi_json_get(json, key);
    if (s == NULL || cap == 0) { if (cap) out[0] = '\0'; return 0; }
    const char *e = s;
    int depth = 0, instr = 0;
    for (;; e++) {
        if (*e == '\0') break;
        if (instr) { if (*e == '\\' && e[1]) e++; else if (*e == '"') { instr = 0; if (depth == 0) { e++; break; } } continue; }
        if (*e == '"') { instr = 1; continue; }
        if (*e == '{' || *e == '[') depth++;
        else if (*e == '}' || *e == ']') { if (depth == 0) break; depth--; if (depth == 0) { e++; break; } }
        else if (*e == ',' && depth == 0) break;
    }
    size_t n = (size_t) (e - s);
    if (n >= cap) n = cap - 1;
    memcpy(out, s, n);
    out[n] = '\0';
    return n;
}

static void
fi_health_of(RedisModuleCtx *ctx, const char *corpus, const char *meta, fi_health *h)
{
    memset(h, 0, sizeof *h);
    h->built_len = (uint64_t) fi_json_num(meta, "corpus_len", 0);
    h->built_rows = (uint64_t) fi_json_num(meta, "rows", 0);
    const uint64_t dim = (uint64_t) fi_json_num(meta, "dim", 0);
    char built_sum[32];
    fi_json_str(meta, "corpus_checksum", built_sum, sizeof built_sum);
    const char *cal = fi_json_get(meta, "calibration");
    const char *rt = cal != NULL ? strstr(cal, "\"reaches_target\":") : NULL;
    if (rt != NULL) { h->have_calib = 1; h->reaches_target = strncmp(rt + 17, "true", 4) == 0; }

    RedisModuleString *ks = RedisModule_CreateString(ctx, corpus, strlen(corpus));
    if (ks == NULL) return;
    RedisModuleKey *kh = RedisModule_OpenKey(ctx, ks, REDISMODULE_READ);
    if (kh != NULL) {
        size_t len = 0;
        const char *p = RedisModule_StringDMA(kh, &len, REDISMODULE_READ);
        if (p != NULL) {
            h->have_now = 1;
            h->now_len = (uint64_t) len;
            h->rows_now = dim > 0 ? (uint64_t) len / (dim * 4u) : 0;
            char now_sum[32];
            if (len >= h->built_len) {
                fi_hex64(now_sum, sizeof now_sum, fi_fnv1a(p, (size_t) h->built_len));
                if (strcmp(now_sum, built_sum) != 0) h->changed = 1;
                else if (len > h->built_len && dim > 0) h->added = ((uint64_t) len - h->built_len) / (dim * 4u);
            } else {
                h->changed = 1;
            }
        }
        RedisModule_CloseKey(kh);
    }
    RedisModule_FreeString(ctx, ks);
}

/* ready | stale | degraded | empty | orphaned: the most important thing to know first. */
static const char *
fi_status(const fi_health *h)
{
    if (!h->have_now) return "orphaned";
    if (h->now_len == 0) return "empty";
    if (h->have_calib && !h->reaches_target) return "degraded";
    if (h->added > 0 || h->changed) return "stale";
    return "ready";
}

static void
fi_advice(fv_sb *sb, const fi_health *h)
{
    int n = 0;
    const double rows = h->built_rows > 0 ? (double) h->built_rows : 1.0;
    if (!h->have_now) {
        fv_sb_append(sb, "\"the corpus key no longer exists; FRACTAL.INDEX.DROP removes the index\"");
        return;
    }
    if (h->now_len == 0) {
        fv_sb_append(sb, "\"the corpus key holds no vectors; load rows and build the index again\""); n++;
    }
    if (h->have_calib && !h->reaches_target) {
        fv_sb_appendf(sb, "%s\"the build's calibration did not reach its target recall; rebuild with VECTOR_FORMAT FP16 or FP32, "
                          "or search with a larger RERANK\"", n++ ? "," : "");
    }
    if (h->changed) {
        fv_sb_appendf(sb, "%s\"rebuild: the rows the index was built over were rewritten or removed, so its cells no longer match them\"",
                      n++ ? "," : "");
    } else if (h->added > 0 && (double) h->added / rows > FI_REFRESH_MAX_FRAC) {
        fv_sb_appendf(sb, "%s\"rebuild: %.0f%% of the rows (%llu) were added since the build, more than the cells were trained for\"",
                      n++ ? "," : "", 100.0 * (double) h->added / rows, (unsigned long long) h->added);
    } else if (h->added > 0) {
        fv_sb_appendf(sb, "%s\"refresh: %llu rows (%.0f%% of the index) were added since the build; FRACTAL.INDEX.REFRESH folds them in\"",
                      n++ ? "," : "", (unsigned long long) h->added, 100.0 * (double) h->added / rows);
    }
}

static int
IndexInfo_Command(RedisModuleCtx *ctx, RedisModuleString **argv, int argc)
{
    if (argc != 3) return RedisModule_WrongArity(ctx);
    char err[256];
    if (!fv_index_available()) return fv_reply_err(ctx, FI_NO_IVF_MSG);
    size_t cl = 0;
    const char *corpus = RedisModule_StringPtrLen(argv[1], &cl);
    size_t nl = 0;
    const char *name = RedisModule_StringPtrLen(argv[2], &nl);
    if (cl == 0 || nl == 0 || !fi_ident_ok(name)) return fv_reply_err(ctx, "the index name must be a plain identifier");
    long gen = fi_generation(ctx, corpus, name);
    if (gen <= 0) return fv_reply_err(ctx, "no index with that name for that corpus");

    char key[640], *meta = NULL;
    size_t metalen = 0;
    snprintf(key, sizeof key, "%s:fsql-idx:%s:meta:%ld", corpus, name, gen);
    if (fi_read_key(ctx, key, &meta, &metalen, err, sizeof err) != 0)
        return fv_reply_err(ctx, "the index metadata could not be read");

    fi_health h;
    fi_health_of(ctx, corpus, meta, &h);
    const int stale = h.have_now && (h.added > 0 || h.changed);
    char built_sum[32];
    fi_json_str(meta, "corpus_checksum", built_sum, sizeof built_sum);

    uint64_t scanned = 0;
    uint32_t probed = 0;
    int used = fi_last_get(corpus, name, &scanned, &probed);

    fv_sb sb;
    fv_sb_init(&sb);
    fv_sb_append(&sb, "{\"name\":");
    fv_sb_json_string(&sb, name);
    fv_sb_append(&sb, ",\"corpus_key\":");
    fv_sb_json_string(&sb, corpus);
    fv_sb_appendf(&sb, ",\"generation\":%ld,\"metric\":\"cosine\"", gen);
    fv_sb_appendf(&sb, ",\"format\":\"%s\"", FI_FMT_NAME[(int) fi_json_num(meta, "format_id", 0)]);
    fv_sb_appendf(&sb, ",\"dim\":%g,\"rows_indexed\":%g,\"nlist\":%g,\"pca_dim\":%g,\"code_bytes\":%g",
                  fi_json_num(meta, "dim", 0), fi_json_num(meta, "rows", 0), fi_json_num(meta, "nlist", 0),
                  fi_json_num(meta, "pca_dim", 0), fi_json_num(meta, "code_bytes", 0));
    fv_sb_appendf(&sb, ",\"corpus_len\":%g,\"corpus_len_now\":%llu", fi_json_num(meta, "corpus_len", 0),
                  (unsigned long long) h.now_len);
    fv_sb_appendf(&sb, ",\"rows_now\":%llu,\"rows_added_since_build\":%llu,\"has_delta\":%s",
                  (unsigned long long) h.rows_now, (unsigned long long) h.added, h.added > 0 ? "true" : "false");
    fv_sb_append(&sb, ",\"corpus_checksum\":");
    fv_sb_json_string(&sb, built_sum);
    fv_sb_append(&sb, ",\"stale\":");
    fv_sb_append(&sb, stale ? "true" : "false");
    fv_sb_appendf(&sb, ",\"corpus_present\":%s", h.have_now ? "true" : "false");
    fv_sb_appendf(&sb, ",\"status\":\"%s\",\"advice\":[", fi_status(&h));
    fi_advice(&sb, &h);
    fv_sb_append(&sb, "]");
    fv_sb_appendf(&sb, ",\"nprobe_default\":%g", fi_json_num(meta, "nprobe_default", 0));
    fv_sb_append(&sb, ",\"calibration\":");
    {
        char raw[4200];
        if (fi_json_raw(meta, "calibration", raw, sizeof raw) > 0) fv_sb_appendn_raw(&sb, raw, strlen(raw));
        else fv_sb_append(&sb, "{}");
    }
    fv_sb_appendf(&sb, ",\"last_search\":{\"used_index\":%s,\"rows_scanned\":%llu,\"cells_probed\":%u}}",
                  used ? "true" : "false", (unsigned long long) scanned, probed);
    free(meta);

    size_t len = 0;
    char *out = fv_sb_take(&sb, &len);
    if (out == NULL) return fv_reply_err(ctx, "out of memory (site 1305)");
    int rc = RedisModule_ReplyWithStringBuffer(ctx, out, len);
    free(out);
    return rc;
}

/* ---------------------------------------------------------------- list */

static int
fi_cmp_str(const void *a, const void *b) { return strcmp(*(char *const *) a, *(char *const *) b); }

/* The names of the indexes of a corpus key, sorted (malloc'd strings; *out is malloc'd, NULL when there are none). Returns -1 when
 * the keyspace scan fails. Main thread only. */
static int
fi_scan_names(RedisModuleCtx *ctx, const char *corpus, size_t cl, char ***out, size_t *nout)
{
    /* SCAN MATCH <escaped corpus>:fsql-idx:*:gen  (the generation pointer exists once per index) */
    char pat[640];
    size_t pn = 0;
    for (size_t i = 0; i < cl && pn + 2 < sizeof pat; i++) {
        char c = corpus[i];
        if (c == '*' || c == '?' || c == '[' || c == ']' || c == '\\') pat[pn++] = '\\';
        pat[pn++] = c;
    }
    pat[pn] = '\0';
    snprintf(pat + pn, sizeof pat - pn, ":fsql-idx:*:gen");
    const size_t prefix = strlen(corpus) + strlen(":fsql-idx:");

    char **names = NULL;
    size_t nn = 0, cap = 0;
    char cursor[64] = "0";
    int guard = 0;
    do {
        RedisModuleCallReply *r = RedisModule_Call(ctx, "SCAN", "ccccc", cursor, "MATCH", pat, "COUNT", "1000");
        if (r == NULL || RedisModule_CallReplyType(r) != REDISMODULE_REPLY_ARRAY) {
            if (r) RedisModule_FreeCallReply(r);
            for (size_t i = 0; i < nn; i++) free(names[i]);
            free(names);
            return -1;
        }
        RedisModuleCallReply *cur = RedisModule_CallReplyArrayElement(r, 0);
        RedisModuleCallReply *keys = RedisModule_CallReplyArrayElement(r, 1);
        size_t curl = 0;
        const char *cs = cur ? RedisModule_CallReplyStringPtr(cur, &curl) : NULL;
        if (cs == NULL || curl == 0 || curl >= sizeof cursor) { memcpy(cursor, "0", 2); } else { memcpy(cursor, cs, curl); cursor[curl] = '\0'; }
        size_t nk = keys ? RedisModule_CallReplyLength(keys) : 0;
        for (size_t i = 0; i < nk; i++) {
            RedisModuleCallReply *e = RedisModule_CallReplyArrayElement(keys, i);
            size_t kl = 0;
            const char *k = e ? RedisModule_CallReplyStringPtr(e, &kl) : NULL;
            if (k == NULL || kl <= prefix + 4) continue;
            if (memcmp(k, corpus, cl) != 0 || memcmp(k + cl, ":fsql-idx:", 10) != 0 || memcmp(k + kl - 4, ":gen", 4) != 0) continue;
            size_t nlen = kl - prefix - 4;
            if (nlen == 0 || nlen > 63) continue;
            char nm[64];
            memcpy(nm, k + prefix, nlen); nm[nlen] = '\0';
            if (!fi_ident_ok(nm)) continue;
            if (nn == cap) {
                size_t nc = cap ? cap * 2 : 16;
                char **t = (char **) realloc(names, nc * sizeof *names);
                if (t == NULL) continue;
                names = t; cap = nc;
            }
            names[nn] = strdup(nm);
            if (names[nn] != NULL) nn++;
        }
        RedisModule_FreeCallReply(r);
    } while (strcmp(cursor, "0") != 0 && ++guard < 100000);
    if (nn > 1) qsort(names, nn, sizeof *names, fi_cmp_str);
    *out = names; *nout = nn;
    return 0;
}

/* The first index (by name) of a corpus key, for the agents' agent_use_index path. Returns 1 with the name in `name`, 0 when the
 * corpus has no index. Main thread only. */
int
fv_index_first_name(RedisModuleCtx *ctx, const char *corpus, char *name, size_t namesz)
{
    char **names = NULL;
    size_t nn = 0;
    if (!fv_index_available() || fi_scan_names(ctx, corpus, strlen(corpus), &names, &nn) != 0) return 0;
    int found = 0;
    for (size_t i = 0; i < nn; i++) {
        if (!found && fi_generation(ctx, corpus, names[i]) > 0) { snprintf(name, namesz, "%s", names[i]); found = 1; }
        free(names[i]);
    }
    free(names);
    return found;
}

/* FRACTAL.INDEX.LIST <corpus_key>: a JSON array with one summary object per index of the corpus, sorted by name. */
static int
IndexList_Command(RedisModuleCtx *ctx, RedisModuleString **argv, int argc)
{
    if (argc != 2) return RedisModule_WrongArity(ctx);
    if (!fv_index_available()) return fv_reply_err(ctx, FI_NO_IVF_MSG);
    size_t cl = 0;
    const char *corpus = RedisModule_StringPtrLen(argv[1], &cl);
    if (cl == 0 || cl > 255) return fv_reply_err(ctx, "the corpus key name is missing or too long");

    char **names = NULL;
    size_t nn = 0;
    if (fi_scan_names(ctx, corpus, cl, &names, &nn) != 0) return fv_reply_err(ctx, "the keyspace scan failed");

    fv_sb sb;
    fv_sb_init(&sb);
    fv_sb_append(&sb, "[");
    int first = 1;
    for (size_t i = 0; i < nn; i++) {
        if (i > 0 && strcmp(names[i], names[i - 1]) == 0) continue;
        long gen = fi_generation(ctx, corpus, names[i]);
        if (gen <= 0) continue;
        char key[640], e2[64], *meta = NULL;
        snprintf(key, sizeof key, "%s:fsql-idx:%s:meta:%ld", corpus, names[i], gen);
        if (fi_read_key(ctx, key, &meta, NULL, e2, sizeof e2) != 0) continue;
        fi_health h;
        fi_health_of(ctx, corpus, meta, &h);
        if (!first) fv_sb_append(&sb, ",");
        first = 0;
        fv_sb_append(&sb, "{\"name\":");
        fv_sb_json_string(&sb, names[i]);
        fv_sb_appendf(&sb, ",\"format\":\"%s\",\"dim\":%g,\"rows\":%g,\"rows_now\":%llu,\"stale\":%s,\"status\":\"%s\",\"generation\":%ld}",
                      FI_FMT_NAME[(int) fi_json_num(meta, "format_id", 0)], fi_json_num(meta, "dim", 0), fi_json_num(meta, "rows", 0),
                      (unsigned long long) h.rows_now, (h.have_now && (h.added > 0 || h.changed)) ? "true" : "false", fi_status(&h), gen);
        free(meta);
    }
    fv_sb_append(&sb, "]");
    for (size_t i = 0; i < nn; i++) free(names[i]);
    free(names);
    size_t len = 0;
    char *out = fv_sb_take(&sb, &len);
    if (out == NULL) return fv_reply_err(ctx, "out of memory (list)");
    int rc = RedisModule_ReplyWithStringBuffer(ctx, out, len);
    free(out);
    return rc;
}

/* ---------------------------------------------------------------- refresh */

/* The worker of FRACTAL.INDEX.REFRESH: each corpus row after the ones the build covered is encoded with the index's existing
 * model (no retraining) and appended to its cell. The result is written as a NEW generation by index_reply, like a build. */
static void
refresh_work(fv_job *b)
{
    index_job *j = (index_job *) b;
    if (!fv_index_available()) { fv_job_fail(b, "%s", FI_NO_IVF_MSG); return; }
    const size_t dim = j->dim, n = j->n_rows, old = j->old_rows;
    int rc = 0;
    fsql_ivf_model *m = fsql_ivf_model_load(j->old_model, j->old_model_len, &rc);
    fsql_ivf_info_t info;
    if (m == NULL || fsql_ivf_model_info(m, &info) != FSQL_OK) {
        if (m) fsql_ivf_model_free(m);
        fv_job_fail(b, "the index model is not valid; rebuild the index");
        return;
    }
    const uint32_t nlist = info.nlist;
    const size_t cb = info.code_bytes;
    for (size_t i = old * dim; i < n * dim; i++)
        if (!isfinite(j->vecs[i])) { fsql_ivf_model_free(m); fv_job_fail(b, "the corpus contains a non-finite value (NaN or Inf)"); return; }

    const size_t add = n - old;
    uint32_t *cell_of = (uint32_t *) malloc((add ? add : 1) * sizeof(uint32_t));
    uint8_t *codes = (uint8_t *) malloc(add * cb + 1);
    size_t *addn = (size_t *) calloc(nlist ? nlist : 1, sizeof(size_t));
    size_t *fill = (size_t *) calloc(nlist ? nlist : 1, sizeof(size_t));
    uint8_t **cells = (uint8_t **) calloc(nlist ? nlist : 1, sizeof(uint8_t *));
    size_t *clen = (size_t *) calloc(nlist ? nlist : 1, sizeof(size_t));
    int ok = cell_of && codes && addn && fill && cells && clen;
    for (size_t r = 0; ok && r < add; r++) {
        uint32_t c = 0;
        if (fsql_ivf_encode(m, j->vecs + (old + r) * dim, &c, codes + r * cb) != FSQL_OK || c >= nlist) ok = 0;
        else { cell_of[r] = c; addn[c]++; }
    }
    for (uint32_t c = 0; ok && c < nlist; c++) {
        uint32_t k_old = 0;
        if (j->old_cell_len[c] >= 4) memcpy(&k_old, j->old_cells[c], 4);
        if (j->old_cell_len[c] < 4 + (size_t) k_old * (8 + cb)) { ok = 0; break; }
        size_t k = (size_t) k_old + addn[c];
        size_t bytes = 4 + k * 8 + k * cb;
        uint8_t *buf = (uint8_t *) malloc(bytes ? bytes : 1);
        if (buf == NULL) { ok = 0; break; }
        uint32_t n32 = (uint32_t) k;
        memcpy(buf, &n32, 4);
        memcpy(buf + 4, j->old_cells[c] + 4, (size_t) k_old * 8);
        memcpy(buf + 4 + k * 8, j->old_cells[c] + 4 + (size_t) k_old * 8, (size_t) k_old * cb);
        fill[c] = k_old;
        cells[c] = buf;
        clen[c] = bytes;
    }
    for (size_t r = 0; ok && r < add; r++) {
        uint32_t c = cell_of[r];
        size_t k = (size_t) (*(uint32_t *) (void *) cells[c]);      /* the cell's final count */
        uint64_t u = (uint64_t) (old + r);                          /* the id is the corpus position */
        for (int bb = 0; bb < 8; bb++) cells[c][4 + fill[c] * 8 + (size_t) bb] = (uint8_t) (u >> (8 * bb));
        memcpy(cells[c] + 4 + k * 8 + fill[c] * cb, codes + r * cb, cb);
        fill[c]++;
    }
    free(cell_of); free(codes); free(addn); free(fill);
    fsql_ivf_model_free(m);
    if (!ok) {
        if (cells) for (uint32_t c = 0; c < nlist; c++) free(cells[c]);
        free(cells); free(clen);
        fv_job_fail(b, "the index could not be refreshed (a cell is damaged or memory ran out); rebuild the index");
        return;
    }

    /* the metadata: the old one with the rows, the corpus length and checksum, and the generation replaced */
    char seed[40], calib[4200], nprobe[24], pca[24];
    fi_json_raw(j->old_meta, "seed", seed, sizeof seed);
    fi_json_raw(j->old_meta, "calibration", calib, sizeof calib);
    fi_json_raw(j->old_meta, "nprobe_default", nprobe, sizeof nprobe);
    fi_json_raw(j->old_meta, "pca_dim", pca, sizeof pca);
    char sum[32];
    fi_hex64(sum, sizeof sum, j->corpus_hash);
    fv_sb mb;
    fv_sb_init(&mb);
    fv_sb_appendf(&mb, "{\"name\":\"%s\",\"corpus_key\":\"%s\",\"format\":\"%s\",\"format_id\":%d,\"dim\":%zu,"
                       "\"rows\":%zu,\"nlist\":%u,\"pca_dim\":%s,\"code_bytes\":%zu,\"generation\":%u,"
                       "\"format_version\":%d,\"seed\":%s,\"corpus_len\":%zu,\"corpus_checksum\":\"%s\","
                       "\"nprobe_default\":%s,\"calibration\":%s}",
                  j->name, j->corpus, FI_FMT_NAME[j->opt.fmt], j->opt.fmt, dim, n, nlist, pca[0] ? pca : "0", cb,
                  j->generation, FI_FORMAT_VERSION, seed[0] ? seed : "0", j->corpus_bytes, sum,
                  nprobe[0] ? nprobe : "0", calib[0] ? calib : "{}");
    size_t mlen = 0;
    char *meta = fv_sb_take(&mb, &mlen);
    uint8_t *model = (uint8_t *) malloc(j->old_model_len ? j->old_model_len : 1);
    if (meta == NULL || model == NULL) {
        free(meta); free(model);
        for (uint32_t c = 0; c < nlist; c++) free(cells[c]);
        free(cells); free(clen);
        fv_job_fail(b, "out of memory (refresh)");
        return;
    }
    memcpy(model, j->old_model, j->old_model_len);
    j->model = model;
    j->model_len = j->old_model_len;
    j->code_bytes = (uint32_t) cb;
    j->nlist = nlist;
    j->cells = cells;
    j->cell_len = clen;
    j->n_cells = nlist;
    j->meta = meta;

    char summary[320];
    int sn = snprintf(summary, sizeof summary, "{\"name\":\"%s\",\"added\":%zu,\"rows\":%zu,\"generation\":%u,\"refreshed\":true}",
                      j->name, add, n, j->generation);
    char *copy = (char *) malloc((size_t) sn + 1);
    if (copy == NULL) { fv_job_fail(b, "out of memory (refresh)"); return; }
    memcpy(copy, summary, (size_t) sn + 1);
    size_t cl2 = (size_t) sn;
    copy = fi_tag_summary(j, copy, &cl2);
    fv_job_bulk(b, copy, cl2);
}

/* FRACTAL.INDEX.REFRESH <corpus_key> <name>: folds the rows appended since the build into the cells, without retraining. Rows
 * that were rewritten or removed need FRACTAL.INDEX.REBUILD (FRACTAL.INDEX.INFO says which one it recommends). */
static int
IndexRefresh_Command(RedisModuleCtx *ctx, RedisModuleString **argv, int argc)
{
    if (argc != 3) return RedisModule_WrongArity(ctx);
    char err[256];
    if (!fv_index_available()) return fv_reply_err(ctx, FI_NO_IVF_MSG);
    size_t cl = 0, nl = 0;
    const char *corpus = RedisModule_StringPtrLen(argv[1], &cl);
    const char *name = RedisModule_StringPtrLen(argv[2], &nl);
    if (cl == 0 || nl == 0 || !fi_ident_ok(name)) return fv_reply_err(ctx, "the index name must be a plain identifier");
    long gen = fi_generation(ctx, corpus, name);
    if (gen <= 0) return fv_reply_err(ctx, "no index with that name for that corpus");

    char key[640], *meta = NULL;
    snprintf(key, sizeof key, "%s:fsql-idx:%s:meta:%ld", corpus, name, gen);
    if (fi_read_key(ctx, key, &meta, NULL, err, sizeof err) != 0) return fv_reply_err(ctx, "the index metadata could not be read");
    const uint32_t dim = (uint32_t) fi_json_num(meta, "dim", 0);
    const uint32_t nlist = (uint32_t) fi_json_num(meta, "nlist", 0);
    const int fmt = (int) fi_json_num(meta, "format_id", -1);
    const size_t built_len = (size_t) fi_json_num(meta, "corpus_len", 0);
    const size_t built_rows = (size_t) fi_json_num(meta, "rows", 0);
    char built_sum[32];
    fi_json_str(meta, "corpus_checksum", built_sum, sizeof built_sum);
    if (dim == 0 || nlist == 0 || fmt < 0 || fmt > 3) { free(meta); return fv_reply_err(ctx, "the index has corrupt metadata; rebuild it"); }

    index_job *j = (index_job *) calloc(1, sizeof *j);
    if (j == NULL) { free(meta); return fv_reply_err(ctx, "out of memory (refresh)"); }
    j->base.dispose = index_dispose;
    if (fi_prepare(ctx, argv, 3, 3, j, err, sizeof err, dim) != 0) {
        free(meta);
        index_dispose(&j->base);
        return fv_reply_err(ctx, err);
    }
    j->opt.fmt = fmt;
    char sum[32];
    if (j->corpus_bytes < built_len || (fi_hex64(sum, sizeof sum, fi_fnv1a(j->vecs, built_len)), strcmp(sum, built_sum) != 0)) {
        free(meta);
        index_dispose(&j->base);
        return fv_reply_err(ctx, "the rows the index was built over were rewritten or removed; use FRACTAL.INDEX.REBUILD");
    }
    if (j->corpus_bytes == built_len) {
        char out[320];
        int on = snprintf(out, sizeof out, "{\"name\":\"%s\",\"added\":0,\"rows\":%zu,\"generation\":%ld,\"refreshed\":false}", name, built_rows, gen);
        free(meta);
        index_dispose(&j->base);
        return RedisModule_ReplyWithStringBuffer(ctx, out, (size_t) on);
    }

    /* the current generation's model and cells, read here (the main thread) for the worker */
    char *model = NULL;
    size_t model_len = 0;
    snprintf(key, sizeof key, "%s:fsql-idx:%s:model:%ld", corpus, name, gen);
    if (fi_read_key(ctx, key, &model, &model_len, err, sizeof err) != 0) {
        free(meta);
        index_dispose(&j->base);
        return fv_reply_err(ctx, "the index model could not be read; rebuild it");
    }
    j->old_model = (uint8_t *) model;
    j->old_model_len = model_len;
    j->old_cells = (uint8_t **) calloc(nlist, sizeof(uint8_t *));
    j->old_cell_len = (size_t *) calloc(nlist, sizeof(size_t));
    j->old_ncells = nlist;
    if (j->old_cells == NULL || j->old_cell_len == NULL) { free(meta); index_dispose(&j->base); return fv_reply_err(ctx, "out of memory (refresh)"); }
    for (uint32_t c = 0; c < nlist; c++) {
        snprintf(key, sizeof key, "%s:fsql-idx:%s:cell:%ld:%u", corpus, name, gen, c);
        char *cb = NULL;
        size_t cbl = 0;
        if (fi_read_key(ctx, key, &cb, &cbl, err, sizeof err) != 0) {
            free(meta);
            index_dispose(&j->base);
            return fv_reply_err(ctx, "an index cell could not be read; rebuild it");
        }
        j->old_cells[c] = (uint8_t *) cb;
        j->old_cell_len[c] = cbl;
    }
    j->old_meta = meta;
    j->old_rows = built_rows;
    j->generation = (uint32_t) (gen + 1);
    j->base.work = refresh_work;
    return fv_dispatch(ctx, &j->base);
}

/* FRACTAL.INDEX.ENSURE <corpus_key> <name> [options]: brings the index up to date with the least work and says what it did.
 * No index: it is created. Rows were rewritten or removed, or more than 25% were added since the build: rebuilt. A smaller
 * number of rows added: refreshed. Otherwise nothing is done ("unchanged"). The options are those of FRACTAL.INDEX.CREATE. */
static int
IndexEnsure_Command(RedisModuleCtx *ctx, RedisModuleString **argv, int argc)
{
    if (argc < 3) return RedisModule_WrongArity(ctx);
    if (!fv_index_available()) return fv_reply_err(ctx, FI_NO_IVF_MSG);
    size_t cl = 0, nl = 0;
    const char *corpus = RedisModule_StringPtrLen(argv[1], &cl);
    const char *name = RedisModule_StringPtrLen(argv[2], &nl);
    if (cl == 0 || cl >= 256 || nl == 0 || !fi_ident_ok(name)) return fv_reply_err(ctx, "the index name must be a plain identifier");
    char cbuf[256]; memcpy(cbuf, corpus, cl); cbuf[cl] = '\0';
    long gen = fi_generation(ctx, cbuf, name);
    if (gen <= 0) { fi_pending_action = "created"; return IndexCreate_Command(ctx, argv, argc); }

    char key[640], err[256], *meta = NULL;
    snprintf(key, sizeof key, "%s:fsql-idx:%s:meta:%ld", cbuf, name, gen);
    if (fi_read_key(ctx, key, &meta, NULL, err, sizeof err) != 0) return fv_reply_err(ctx, "the index metadata could not be read");
    fi_health h;
    fi_health_of(ctx, cbuf, meta, &h);
    free(meta);
    const double rows = h.built_rows > 0 ? (double) h.built_rows : 1.0;
    if (!h.have_now || h.changed || (h.added > 0 && (double) h.added / rows > FI_REFRESH_MAX_FRAC)) {
        fi_pending_action = "rebuilt";
        return IndexRebuild_Command(ctx, argv, argc);
    }
    if (h.added > 0) { fi_pending_action = "refreshed"; return IndexRefresh_Command(ctx, argv, 3); }
    char out[320];
    int on = snprintf(out, sizeof out, "{\"name\":\"%s\",\"action\":\"unchanged\",\"status\":\"%s\",\"generation\":%ld,\"rows\":%llu}",
                      name, fi_status(&h), gen, (unsigned long long) h.built_rows);
    return RedisModule_ReplyWithStringBuffer(ctx, out, (size_t) on);
}

int
fv_register_index(RedisModuleCtx *ctx)
{
    static const struct { const char *name; RedisModuleCmdFunc fn; const char *flags; } T[] = {
        { "FRACTAL.INDEX.CREATE",  IndexCreate_Command,  "write deny-oom" },
        { "FRACTAL.INDEX.REBUILD", IndexRebuild_Command, "write deny-oom" },
        { "FRACTAL.INDEX.DROP",    IndexDrop_Command,    "write" },
        { "FRACTAL.INDEX.INFO",    IndexInfo_Command,    "readonly fast allow-stale" },
        { "FRACTAL.INDEX.LIST",    IndexList_Command,    "readonly allow-stale" },
        { "FRACTAL.INDEX.REFRESH", IndexRefresh_Command, "write deny-oom" },
        { "FRACTAL.INDEX.ENSURE",  IndexEnsure_Command,  "write deny-oom" },
    };
    for (size_t i = 0; i < sizeof T / sizeof T[0]; i++) {
        if (RedisModule_CreateCommand(ctx, T[i].name, T[i].fn, T[i].flags, 1, 1, 1) == REDISMODULE_ERR)
            return REDISMODULE_ERR;
        if (fv_acl(ctx, T[i].name, (i == 3) ? "read fast" : (i == 4) ? "read slow" : "write slow") != 0)
            return REDISMODULE_ERR;
    }
    return REDISMODULE_OK;
}
