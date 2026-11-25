/* src/fv_common.h
 * SPDX-License-Identifier: Apache-2.0 AND BSD-2-Clause
 * SPDX-FileCopyrightText: 2026 Daniel Gardiner d/b/a FractalSQLabs
 *
 * Shared declarations for the fractalsql-valkey module: argument decoding,
 * a small string builder, and the job layer that moves long-running work
 * off Valkey's main thread.
 *
 * THREADING RULES (the module's whole safety argument):
 *   - A command handler runs on the main thread. It parses and validates
 *     every argument and COPIES everything it needs into a job it owns.
 *   - A worker thread touches ONLY its job. It never calls a RedisModule_*
 *     function except RedisModule_UnblockClient, and never reads a pointer
 *     obtained from RedisModule_StringDMA / StringPtrLen (those point into
 *     Valkey's memory and are invalid once the handler returns).
 *   - The reply is produced back on the main thread by the reply callback.
 */
#ifndef FV_COMMON_H
#define FV_COMMON_H

#include "redismodule.h"
#include "fractalsql_sql.h"

#include <math.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Comma-joined %.9g text of n doubles (9 digits round-trip a float32), malloc'd; NULL on failure. */
static inline char *
fv_csv_of(const double *v, size_t n)
{
    /* %.9g of a double is at most 16 characters ("-1.23456789e-308") plus the comma: 24 per element
     * covers it with room to spare, and every write is bounded by what is left of the buffer. */
    if (n > (SIZE_MAX - 8) / 24) return NULL;
    size_t cap = n * 24 + 8;
    char *s = (char *) malloc(cap), *o;
    if (s == NULL) return NULL;
    o = s;
    for (size_t i = 0; i < n; i++) {
        size_t left = cap - (size_t) (o - s);
        int    k    = snprintf(o, left, "%s%.9g", i ? "," : "", v[i]);   /* 9 digits round-trip a float32 */
        if (k < 0 || (size_t) k >= left) { free(s); return NULL; }
        o += k;
    }
    *o = '\0';
    return s;
}

/* Bounded substring search. memmem() is a GNU extension: without _GNU_SOURCE it is implicitly declared (its pointer
 * result truncated to int) and it does not exist on MinGW, and both call sites are checks that reject input. */
static inline int
fv_contains(const void *hay, size_t hlen, const char *needle, size_t nlen)
{
    const unsigned char *h = (const unsigned char *) hay;
    if (nlen == 0) return 1;
    if (nlen > hlen) return 0;
    for (size_t i = 0; i + nlen <= hlen; i++)
        if (memcmp(h + i, needle, nlen) == 0) return 1;
    return 0;
}

#ifndef FRACTALSQL_VERSION
#  error "FRACTALSQL_VERSION must come from the build (the Makefile reads ./VERSION)"
#endif

/* Hard caps on caller-controlled sizes. Every size is bounded BEFORE it is
 * used in arithmetic or an allocation. */
#define FV_MAX_QUERY_BYTES   (4u   * 1024u * 1024u)    /* 4 MiB   */
#define FV_MAX_CORPUS_BYTES  (256u * 1024u * 1024u)    /* 256 MiB */
#define FV_MAX_BLOB_BYTES    (16u  * 1024u * 1024u)    /* series / matrix args */

/* ---------------------------------------------------------------- */
/* string builder (append failures are sticky in .bad)                */
/* ---------------------------------------------------------------- */
typedef struct {
    char  *s;
    size_t len, cap;
    int    bad;
} fv_sb;

void fv_sb_init(fv_sb *b);
void fv_sb_free(fv_sb *b);
void fv_sb_append(fv_sb *b, const char *z);
void fv_sb_appendf(fv_sb *b, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
/* Appends n bytes of z verbatim. */
void fv_sb_appendn_raw(fv_sb *b, const char *z, size_t n);
/* Appends z as a quoted, escaped JSON string (no length limit). */
void fv_sb_json_string(fv_sb *b, const char *z);
/* Hands the buffer to the caller (malloc'd, NUL-terminated); NULL if bad. */
char *fv_sb_take(fv_sb *b, size_t *len_out);

/* ---------------------------------------------------------------- */
/* argument decoding (main thread only)                               */
/* ---------------------------------------------------------------- */

/* Packed little-endian float32 blob -> malloc'd double array. Refuses an
 * empty or oversized blob, a length that is not a multiple of 4, and any
 * NaN/Inf. On failure writes a message to err and returns -1. */
int fv_blob_to_doubles(RedisModuleString *a, const char *what, size_t max_bytes,
                       double **out, size_t *n, char *err, size_t errsz);

/* Same, but keeps float32 (copied to an aligned buffer). */
int fv_blob_to_floats(RedisModuleString *a, const char *what, size_t max_bytes,
                      float **out, size_t *n, char *err, size_t errsz);

/* Packed little-endian uint32 blob -> malloc'd size_t array. */
int fv_blob_to_indices(RedisModuleString *a, const char *what, size_t max_bytes,
                       size_t **out, size_t *n, char *err, size_t errsz);

int fv_arg_ll(RedisModuleString *a, const char *name, long long lo, long long hi,
              long long *out, char *err, size_t errsz);
int fv_arg_double(RedisModuleString *a, const char *name, double *out,
                  char *err, size_t errsz);

/* ---------------------------------------------------------------- */
/* jobs                                                               */
/* ---------------------------------------------------------------- */
enum { FV_R_BULK = 0, FV_R_DOUBLE, FV_R_INT, FV_R_ERR };

typedef struct fv_job fv_job;
struct fv_job {
    RedisModuleBlockedClient *bc;
    void  (*work)(fv_job *);      /* runs on a worker (or inline) */
    void  (*dispose)(fv_job *);   /* frees the job and everything it owns */
    int   (*reply)(RedisModuleCtx *ctx, fv_job *job);  /* optional: builds the reply on the main thread */
    size_t bytes;                 /* counted against the in-flight cap */
    const char *audit;            /* an agent job: the audit-ledger entry type for its decision document, or NULL */
    int    done;                  /* worker -> main handoff flag (release/acquire) */
    int    kind;
    char  *res;                   /* FV_R_BULK payload, malloc'd */
    size_t res_len;
    double dres;
    long long ires;
    char   err[256];
};

/* Takes ownership of `job` in every case. Runs it on the worker pool and
 * replies when it finishes; or inline if the client may not block (MULTI,
 * Lua) or the pool is not running. */
int  fv_dispatch(RedisModuleCtx *ctx, fv_job *job);

/* Worker-side helpers. */
void fv_job_fail(fv_job *j, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
void fv_job_bulk(fv_job *j, char *res, size_t len);       /* takes ownership */
void fv_job_double(fv_job *j, double v);
void fv_job_int(fv_job *j, long long v);

int  fv_pool_start(int nthreads, size_t max_inflight_bytes);
int  fv_pool_stop(void);          /* 0 = stopped, -1 = refused: work in flight */
void fv_pool_config_get(int *nthreads, size_t *max_inflight_bytes);

/* Per-thread core context (created lazily, freed with the thread). */
fsql_ctx *fv_thread_ctx(void);
void      fv_thread_ctx_free(void);

/* Named sessions (src/fv_session.c): the diversify/repulsion state kept between searches. */
typedef struct fv_session fv_session;
void        fv_session_configure(int max_sessions, int ttl_sec);
fv_session *fv_session_acquire(RedisModuleString *name, int create, char *err, size_t errsz);
void        fv_session_release(fv_session *s);
fsql_ctx   *fv_session_lock(fv_session *s);      /* worker side: the context, session locked */
void        fv_session_unlock(fv_session *s);
void        fv_session_shutdown(void);

/* Reply with an error assembled from a message (no CR/LF allowed in RESP
 * errors, so those are folded to spaces). */
int fv_reply_err(RedisModuleCtx *ctx, const char *msg);

/* "1,2,3" / "[1,2,3]" text -> float array (malloc'd). */
int fv_parse_vector_text(const char *t, size_t len, float **out, size_t *n,
                         char *err, size_t errsz);

/* Assign ACL categories ("read fast", "read slow", ...) to a registered
 * command. Module commands belong to no category by default, so without
 * this an operator could only grant them one command at a time. Skips quietly
 * on a server too old to have the API. */
int fv_acl(RedisModuleCtx *ctx, const char *name, const char *cats);

/* Command registration, one per source file. */
int fv_register_search(RedisModuleCtx *ctx);
int fv_register_vector(RedisModuleCtx *ctx);
int fv_register_analytics(RedisModuleCtx *ctx);
int fv_register_daemon(RedisModuleCtx *ctx);
int fv_register_agents(RedisModuleCtx *ctx);
int fv_register_session(RedisModuleCtx *ctx);
int fv_register_index(RedisModuleCtx *ctx);
int fv_register_feature(RedisModuleCtx *ctx);

/* ---------------------------------------------------------------- */
/* the native IVF index (src/fv_index.c)                            */
/* ---------------------------------------------------------------- */

/* A resident index: the model, the cells and the metadata of one generation. */
typedef struct fv_index fv_index;

typedef struct {
    char     name[64];
    uint32_t dim, nlist, pca_dim, code_bytes;
    int      fmt, generation;
    uint32_t nprobe_default;
    size_t   rerank_default;
    uint64_t rows;           /* the rows the index was built over (corpus positions 0 .. rows-1) */
} fv_index_info_t;

/* Non-zero when the vendored core carries the IVF maths. When it does not, every index command answers with
 * fv_index_missing_message() (the SQL surface is the same on every platform). */
int         fv_index_available(void);
const char *fv_index_missing_message(void);

/* Loads (or returns the resident) index of (corpus_key, name). NULL and a message on failure. */
fv_index *fv_index_load(RedisModuleCtx *ctx, const char *corpus_key, const char *name, char *err, size_t errsz);
void      fv_index_release(fv_index *x);
int       fv_index_get_info(const fv_index *x, fv_index_info_t *out);
/* The first index (by name) of a corpus key: 1 with the name in `name`, 0 when there is none. Main thread only. */
int       fv_index_first_name(RedisModuleCtx *ctx, const char *corpus, char *name, size_t namesz);

/* The candidate corpus positions for a query (worker side). The distances are the core's approximate ones: the caller
 * verifies every candidate against the corpus before it becomes an answer. */
int fv_index_search(fv_index *x, const float *query, uint32_t nprobe, size_t shortlist,
                    int64_t *ids, double *dists, size_t *n_out, uint64_t *rows_scanned, uint32_t *cells_probed,
                    char *err, size_t errsz);

/* Records the statistics of the last search of an index (FRACTAL.INDEX.INFO reports them). */
void fv_index_note_search(const char *corpus_key, const char *name, int used, uint64_t scanned, uint32_t probed);

/* One reasoning call from a worker thread (the agents' step 3). Returns 0 with
 * a malloc'd string in *out, or -1 with a message in err (including "no daemon
 * configured"); the caller then returns what it computed plus the error. */
int fv_daemon_reason(const char *prompt, const char *context_json,
                     char **out, size_t *out_len, char *err, size_t errsz);

/* Query generation for DATA_ANALYST, through the daemon (worker side). Returns 0 with the model's single
 * fenced block (or its whole reply when it had none) in *out, or -1 with err. */
int fv_daemon_generate(const char *prompt, const char *context, const char *system_tag,
                       char **out, size_t *out_len, char *err, size_t errsz);

/* The enterprise multimodal optimizers, through the daemon (worker side). pareto selects
 * fractal_optimize_portfolio_multimodal_pareto. Returns 0 with the daemon's JSON in *out
 * (malloc'd), or -1 with err ("enterprise tier not loaded ..." on a Community daemon). */
int fv_daemon_multimodal(int pareto, const double *mu, const double *cov, size_t n, long long k, long long restarts,
                         double overlap, double quality, long long max_front, long long seed,
                         char **out, size_t *out_len, char *err, size_t errsz);

/* Best-effort audit-ledger record of an agent decision (worker side): FRACTAL.AUDIT.LOG through the daemon,
 * entry_type "agent_<name>", payload the same JSON document the agent returned. Never fails the caller: with no
 * daemon, a Community daemon (enterprise tier not loaded), an oversized document or any error it does nothing,
 * and after a refusal it stays quiet for 30 seconds so a Community deployment pays no per-call round trip. */
void fv_daemon_audit(const char *entry_type, const char *payload_json, size_t payload_len);

/* One embedding call from a worker thread (the text-driven agents' first step).
 * Returns 0 with a malloc'd double array (*n finite values) or -1 with err. */
int fv_daemon_embed(const char *text, double **vec, size_t *n, char *err, size_t errsz);

/* Load-time setting for search_agent / rag_agent: when set, a reasoning failure
 * after a successful retrieval returns the retrieval plus "reasoning_step_failed"
 * instead of an error. Off by default. */
/* agent_use_index: 0 no, 1 yes, 2 auto (an index only when it is ready -- nothing appended since the build -- and the key has at least min_rows rows) */
void fv_agents_configure(int partial_reasoning_fail, int agent_use_index, int index_min_rows);

/* Reads a series from the string key `keyname` (packed little-endian float32)
 * into a malloc'd double array. Main thread only: it opens the key. */
int fv_key_series(RedisModuleCtx *ctx, RedisModuleString *keyname, const char *what,
                  size_t max_bytes, double **out, size_t *n, char *err, size_t errsz);

/* The fractalsqld client. Configured once at load, before any worker runs. */
int fv_daemon_configure(const char *sock, const char *keyfile, unsigned timeout_s,
                        char *err, size_t errsz);
int fv_daemon_configured(void);

/* Test flavor only: route the module's own allocations through the
 * failure-injection wrappers in fv_oom.c. */
#ifdef FV_OOM_INJECT
void *fv_oom_malloc(size_t n);
void *fv_oom_calloc(size_t a, size_t b);
void *fv_oom_realloc(void *p, size_t n);
int   fv_register_oom(RedisModuleCtx *ctx);
#  ifndef FV_OOM_IMPL
#    define malloc  fv_oom_malloc
#    define calloc  fv_oom_calloc
#    define realloc fv_oom_realloc
#  endif
#endif

#endif
