/* src/fv_pool.c
 * SPDX-License-Identifier: Apache-2.0 AND BSD-2-Clause
 * SPDX-FileCopyrightText: 2026 Daniel Gardiner d/b/a FractalSQLabs
 *
 * Worker pool, in-flight accounting, string builder and argument decoding.
 * See fv_common.h for the threading rules.
 */
#include "fv_common.h"

#include <pthread.h>

/* ---------------------------------------------------------------- */
/* string builder                                                     */
/* ---------------------------------------------------------------- */
void fv_sb_init(fv_sb *b) { memset(b, 0, sizeof *b); }

void fv_sb_free(fv_sb *b)
{
    free(b->s);
    memset(b, 0, sizeof *b);
}

static int sb_reserve(fv_sb *b, size_t extra)
{
    if (b->bad) return -1;
    if (extra > (size_t) 1 << 30 || b->len > ((size_t) 1 << 30) - extra) { b->bad = 1; return -1; }
    size_t need = b->len + extra + 1;
    if (need <= b->cap) return 0;
    size_t cap = b->cap ? b->cap : 256;
    while (cap < need) cap *= 2;
    char *p = (char *) realloc(b->s, cap);
    if (p == NULL) { b->bad = 1; return -1; }
    b->s = p;
    b->cap = cap;
    return 0;
}

void fv_sb_append(fv_sb *b, const char *z)
{
    size_t n = strlen(z);
    if (sb_reserve(b, n) != 0) return;
    memcpy(b->s + b->len, z, n + 1);
    b->len += n;
}

void fv_sb_appendf(fv_sb *b, const char *fmt, ...)
{
    /* The formatted result is measured first: a result that does not fit the stack
     * buffer is formatted into a heap buffer instead of failing the whole document.
     * (The 512-byte limit used to mark the buffer bad, so any JSON document over
     * that size -- the index metadata, a reply with a population -- came out as
     * "out of memory".) */
    va_list ap, ap2;
    va_start(ap, fmt);
    va_copy(ap2, ap);
    int n = vsnprintf(NULL, 0, fmt, ap);
    va_end(ap);
    if (n < 0) { va_end(ap2); b->bad = 1; return; }
    if ((size_t) n >= 512) {
        char *t = (char *) malloc((size_t) n + 1);
        if (t == NULL) { va_end(ap2); b->bad = 1; return; }
        vsnprintf(t, (size_t) n + 1, fmt, ap2);
        va_end(ap2);
        fv_sb_append(b, t);
        free(t);
        return;
    }
    char tmp[512];
    vsnprintf(tmp, sizeof tmp, fmt, ap2);
    va_end(ap2);
    fv_sb_append(b, tmp);
}

/* Appends n bytes of z verbatim (no NUL needed). */
void fv_sb_appendn_raw(fv_sb *b, const char *z, size_t n)
{
    char *t = (char *) malloc(n + 1);
    if (t == NULL) { b->bad = 1; return; }
    memcpy(t, z, n); t[n] = '\0';
    fv_sb_append(b, t);
    free(t);
}

/* Appends z as a quoted JSON string. Control characters are \u-escaped, so
 * text from a model or a key name cannot break the document. */
void fv_sb_json_string(fv_sb *b, const char *z)
{
    size_t n = strlen(z);
    char *t = (char *) malloc(n * 6 + 3);      /* worst case \u00XX per byte */
    if (t == NULL) { b->bad = 1; return; }
    char *o = t;
    *o++ = '"';
    for (size_t i = 0; i < n; i++) {
        unsigned char c = (unsigned char) z[i];
        if (c == '"' || c == '\\') { *o++ = '\\'; *o++ = (char) c; }
        else if (c == '\n') { *o++ = '\\'; *o++ = 'n'; }
        else if (c == '\r') { *o++ = '\\'; *o++ = 'r'; }
        else if (c == '\t') { *o++ = '\\'; *o++ = 't'; }
        else if (c < 0x20)  { o += sprintf(o, "\\u%04x", c); }
        else *o++ = (char) c;
    }
    *o++ = '"';
    *o = '\0';
    fv_sb_append(b, t);
    free(t);
}

char *fv_sb_take(fv_sb *b, size_t *len_out)
{
    if (b->bad) { fv_sb_free(b); return NULL; }
    if (b->s == NULL) { if (sb_reserve(b, 0) != 0) return NULL; b->s[0] = '\0'; }
    char *s = b->s;
    if (len_out != NULL) *len_out = b->len;
    memset(b, 0, sizeof *b);
    return s;
}

/* ---------------------------------------------------------------- */
/* argument decoding                                                  */
/* ---------------------------------------------------------------- */
static void set_err(char *err, size_t errsz, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(err, errsz, fmt, ap);
    va_end(ap);
}

static int blob_check(RedisModuleString *a, const char *what, size_t max_bytes,
                      size_t elem, const char **p, size_t *len,
                      char *err, size_t errsz)
{
    *p = RedisModule_StringPtrLen(a, len);
    if (*p == NULL || *len == 0 || *len % elem != 0) {
        set_err(err, errsz, "%s must be a non-empty packed %s BLOB (length %% %zu == 0)",
                what, "32-bit", elem);
        return -1;
    }
    if (*len > max_bytes) {
        set_err(err, errsz, "%s too large (max %zu bytes)", what, max_bytes);
        return -1;
    }
    return 0;
}

int fv_blob_to_doubles(RedisModuleString *a, const char *what, size_t max_bytes,
                       double **out, size_t *n, char *err, size_t errsz)
{
    const char *p; size_t len;
    if (blob_check(a, what, max_bytes, 4, &p, &len, err, errsz) != 0) return -1;
    size_t cnt = len / 4;
    double *d = (double *) malloc(cnt * sizeof(double));
    if (d == NULL) { set_err(err, errsz, "out of memory decoding %s", what); return -1; }
    for (size_t i = 0; i < cnt; i++) {
        float f;
        memcpy(&f, p + i * 4, 4);
        if (!isfinite(f)) {
            free(d);
            set_err(err, errsz, "%s contains a non-finite value (NaN or Inf)", what);
            return -1;
        }
        d[i] = (double) f;
    }
    *out = d; *n = cnt;
    return 0;
}

int fv_blob_to_floats(RedisModuleString *a, const char *what, size_t max_bytes,
                      float **out, size_t *n, char *err, size_t errsz)
{
    const char *p; size_t len;
    if (blob_check(a, what, max_bytes, 4, &p, &len, err, errsz) != 0) return -1;
    size_t cnt = len / 4;
    float *f = (float *) malloc(len);
    if (f == NULL) { set_err(err, errsz, "out of memory decoding %s", what); return -1; }
    memcpy(f, p, len);
    for (size_t i = 0; i < cnt; i++) {
        if (!isfinite(f[i])) {
            free(f);
            set_err(err, errsz, "%s contains a non-finite value (NaN or Inf)", what);
            return -1;
        }
    }
    *out = f; *n = cnt;
    return 0;
}

int fv_blob_to_indices(RedisModuleString *a, const char *what, size_t max_bytes,
                       size_t **out, size_t *n, char *err, size_t errsz)
{
    const char *p; size_t len;
    if (blob_check(a, what, max_bytes, 4, &p, &len, err, errsz) != 0) return -1;
    size_t cnt = len / 4;
    size_t *v = (size_t *) malloc(cnt * sizeof(size_t));
    if (v == NULL) { set_err(err, errsz, "out of memory decoding %s", what); return -1; }
    for (size_t i = 0; i < cnt; i++) {
        uint32_t u;
        memcpy(&u, p + i * 4, 4);
        v[i] = (size_t) u;
    }
    *out = v; *n = cnt;
    return 0;
}

int fv_arg_ll(RedisModuleString *a, const char *name, long long lo, long long hi,
              long long *out, char *err, size_t errsz)
{
    long long v = 0;
    if (RedisModule_StringToLongLong(a, &v) != REDISMODULE_OK) {
        set_err(err, errsz, "%s must be an integer", name);
        return -1;
    }
    if (v < lo || v > hi) {
        set_err(err, errsz, "%s must be in [%lld, %lld]", name, lo, hi);
        return -1;
    }
    *out = v;
    return 0;
}

int fv_arg_double(RedisModuleString *a, const char *name, double *out,
                  char *err, size_t errsz)
{
    double v = 0;
    if (RedisModule_StringToDouble(a, &v) != REDISMODULE_OK || !isfinite(v)) {
        set_err(err, errsz, "%s must be a finite number", name);
        return -1;
    }
    *out = v;
    return 0;
}

/* ---------------------------------------------------------------- */
/* per-thread core context                                            */
/* ---------------------------------------------------------------- */
static __thread fsql_ctx *tls_ctx = NULL;

fsql_ctx *fv_thread_ctx(void)
{
    if (tls_ctx == NULL) tls_ctx = fsql_new_minimal();
    return tls_ctx;
}

void fv_thread_ctx_free(void)
{
    if (tls_ctx != NULL) { fsql_free(tls_ctx); tls_ctx = NULL; }
}

/* ---------------------------------------------------------------- */
/* error reply                                                        */
/* ---------------------------------------------------------------- */
int fv_reply_err(RedisModuleCtx *ctx, const char *msg)
{
    char buf[320];
    snprintf(buf, sizeof buf, "ERR %s", msg);
    /* Error text can embed bytes copied from the caller's arguments (for
     * example "invalid number near '...'"). A RESP error line cannot carry
     * CR/LF, and raw binary in it is a log-injection and invalid-UTF-8
     * hazard for clients, so the line is forced to printable ASCII. */
    for (char *p = buf; *p != '\0'; p++)
        if ((unsigned char) *p < 0x20 || (unsigned char) *p > 0x7e) *p = '?';
    return RedisModule_ReplyWithError(ctx, buf);
}

/* ---------------------------------------------------------------- */
/* worker pool                                                        */
/* ---------------------------------------------------------------- */
typedef struct qnode { fv_job *job; struct qnode *next; } qnode;

#define FV_QUEUE_MAX 1024

static pthread_mutex_t g_mu   = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t  g_cv   = PTHREAD_COND_INITIALIZER;
static pthread_t      *g_thr  = NULL;
static int             g_nthr = 0;
static int             g_stop = 0;
static qnode          *g_head = NULL, *g_tail = NULL;
static int             g_qlen = 0;
static size_t          g_inflight = 0;       /* bytes, guarded by g_mu */
static size_t          g_inflight_max = 0;
static int             g_running_jobs = 0;   /* queued + executing + awaiting reply */

static void *worker_main(void *arg)
{
    (void) arg;
    for (;;) {
        pthread_mutex_lock(&g_mu);
        while (g_head == NULL && !g_stop) pthread_cond_wait(&g_cv, &g_mu);
        if (g_head == NULL && g_stop) { pthread_mutex_unlock(&g_mu); break; }
        qnode *n = g_head;
        g_head = n->next;
        if (g_head == NULL) g_tail = NULL;
        g_qlen--;
        pthread_mutex_unlock(&g_mu);

        fv_job *j = n->job;
        free(n);
        j->work(j);
        /* Publish the result. The server's own unblock machinery orders this
         * too, but it is not visible to a race detector (or to a reader of
         * this code); the explicit release/acquire pair is. */
        __atomic_store_n(&j->done, 1, __ATOMIC_RELEASE);
        /* The only RedisModule_* call a worker makes. */
        RedisModule_UnblockClient(j->bc, j);
    }
    fv_thread_ctx_free();
    return NULL;
}

int fv_pool_start(int nthreads, size_t max_inflight_bytes)
{
    pthread_mutex_lock(&g_mu);
    if (g_thr != NULL) { pthread_mutex_unlock(&g_mu); return 0; }
    g_stop = 0;
    g_inflight_max = max_inflight_bytes;
    g_thr = (pthread_t *) calloc((size_t) nthreads, sizeof(pthread_t));
    pthread_mutex_unlock(&g_mu);
    if (g_thr == NULL) return -1;
    int started = 0;
    for (int i = 0; i < nthreads; i++) {
        if (pthread_create(&g_thr[i], NULL, worker_main, NULL) != 0) break;
        started++;
    }
    g_nthr = started;
    if (started == 0) { free(g_thr); g_thr = NULL; return -1; }
    return 0;
}

int fv_pool_stop(void)
{
    pthread_mutex_lock(&g_mu);
    if (g_running_jobs > 0 || g_inflight > 0) {
        pthread_mutex_unlock(&g_mu);
        return -1;
    }
    g_stop = 1;
    pthread_cond_broadcast(&g_cv);
    pthread_mutex_unlock(&g_mu);
    for (int i = 0; i < g_nthr; i++) pthread_join(g_thr[i], NULL);
    free(g_thr);
    g_thr = NULL;
    g_nthr = 0;
    return 0;
}

void fv_pool_config_get(int *nthreads, size_t *max_inflight_bytes)
{
    if (nthreads != NULL) *nthreads = g_nthr;
    if (max_inflight_bytes != NULL) *max_inflight_bytes = g_inflight_max;
}

/* ---------------------------------------------------------------- */
/* job plumbing                                                       */
/* ---------------------------------------------------------------- */
void fv_job_fail(fv_job *j, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(j->err, sizeof j->err, fmt, ap);
    va_end(ap);
    j->kind = FV_R_ERR;
}

void fv_job_bulk(fv_job *j, char *res, size_t len)
{
    j->kind = FV_R_BULK; j->res = res; j->res_len = len;
}
void fv_job_double(fv_job *j, double v) { j->kind = FV_R_DOUBLE; j->dres = v; }
void fv_job_int(fv_job *j, long long v) { j->kind = FV_R_INT; j->ires = v; }

static int job_reply(RedisModuleCtx *ctx, fv_job *j)
{
    /* A job with its own reply hook builds the answer on the main thread (the index build writes its keys here). */
    if (j->reply != NULL) return j->reply(ctx, j);
    switch (j->kind) {
    case FV_R_BULK:
        if (j->res == NULL) return fv_reply_err(ctx, "internal error: empty result");
        return RedisModule_ReplyWithStringBuffer(ctx, j->res, j->res_len);
    case FV_R_DOUBLE: return RedisModule_ReplyWithDouble(ctx, j->dres);
    case FV_R_INT:    return RedisModule_ReplyWithLongLong(ctx, j->ires);
    default:          return fv_reply_err(ctx, j->err[0] ? j->err : "internal error");
    }
}

static void inflight_release(size_t bytes)
{
    pthread_mutex_lock(&g_mu);
    g_inflight -= bytes;
    g_running_jobs--;
    pthread_mutex_unlock(&g_mu);
}

static int blocked_reply_cb(RedisModuleCtx *ctx, RedisModuleString **argv, int argc)
{
    (void) argv; (void) argc;
    fv_job *j = (fv_job *) RedisModule_GetBlockedClientPrivateData(ctx);
    if (j == NULL) return fv_reply_err(ctx, "internal error: no result");
    (void) __atomic_load_n(&j->done, __ATOMIC_ACQUIRE);
    return job_reply(ctx, j);
}

/* Runs after the reply is sent, or when the client disconnected first. */
static void blocked_free_cb(RedisModuleCtx *ctx, void *priv)
{
    (void) ctx;
    fv_job *j = (fv_job *) priv;
    if (j == NULL) return;
    (void) __atomic_load_n(&j->done, __ATOMIC_ACQUIRE);
    size_t bytes = j->bytes;
    j->dispose(j);
    inflight_release(bytes);
}

int fv_dispatch(RedisModuleCtx *ctx, fv_job *job)
{
    /* Account first: refuse rather than queue unbounded memory. */
    pthread_mutex_lock(&g_mu);
    int pool_up = (g_thr != NULL && !g_stop);
    if (g_inflight_max != 0 && g_inflight + job->bytes > g_inflight_max) {
        pthread_mutex_unlock(&g_mu);
        job->dispose(job);
        return fv_reply_err(ctx,
            "too much work in flight (in-flight limit reached); retry shortly");
    }
    g_inflight += job->bytes;
    g_running_jobs++;
    pthread_mutex_unlock(&g_mu);

    int flags = RedisModule_GetContextFlags(ctx);
    if (!pool_up || (flags & (REDISMODULE_CTX_FLAGS_DENY_BLOCKING | REDISMODULE_CTX_FLAGS_MULTI |
                              REDISMODULE_CTX_FLAGS_LUA)) != 0) {
        job->work(job);
        int ret = job_reply(ctx, job);
        size_t bytes = job->bytes;
        job->dispose(job);
        inflight_release(bytes);
        return ret;
    }

    job->bc = RedisModule_BlockClient(ctx, blocked_reply_cb, NULL, blocked_free_cb, 0);
    if (job->bc == NULL) {
        size_t bytes = job->bytes;
        job->dispose(job);
        inflight_release(bytes);
        return fv_reply_err(ctx, "could not block the client");
    }

    qnode *n = (qnode *) malloc(sizeof *n);
    pthread_mutex_lock(&g_mu);
    if (n == NULL || g_qlen >= FV_QUEUE_MAX) {
        pthread_mutex_unlock(&g_mu);
        free(n);
        fv_job_fail(job, "server busy: the work queue is full; retry shortly");
        RedisModule_UnblockClient(job->bc, job);   /* replies with the error */
        return REDISMODULE_OK;
    }
    n->job = job; n->next = NULL;
    if (g_tail != NULL) g_tail->next = n; else g_head = n;
    g_tail = n;
    g_qlen++;
    pthread_cond_signal(&g_cv);
    pthread_mutex_unlock(&g_mu);
    return REDISMODULE_OK;
}

/* ---------------------------------------------------------------- */
/* ACL categories                                                     */
/* ---------------------------------------------------------------- */
int fv_acl(RedisModuleCtx *ctx, const char *name, const char *cats)
{
    if (RedisModule_GetCommand == NULL || RedisModule_SetCommandACLCategories == NULL) return 0;
    RedisModuleCommand *cmd = RedisModule_GetCommand(ctx, name);
    if (cmd == NULL) return -1;
    return RedisModule_SetCommandACLCategories(cmd, cats) == REDISMODULE_OK ? 0 : -1;
}
