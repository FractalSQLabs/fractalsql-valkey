/* src/fv_session.c
 * SPDX-License-Identifier: Apache-2.0 AND BSD-2-Clause
 * SPDX-FileCopyrightText: 2026 Daniel Gardiner d/b/a FractalSQLabs
 *
 * Sessions: the state the diversify/repulsion layer keeps between searches.
 *
 * The SQL editions hold it per connection. Valkey has no per-connection
 * state that survives a reconnect or a proxy, so a session is NAMED by the
 * caller: FRACTAL.DIVERSIFY.ENABLE <session> creates it, and
 * FRACTAL.SEARCH / FRACTAL.EXPLORE ... SESSION <session> search through it.
 * A session owns one sovereign core context (the shadows and the D_q window
 * live in it) and a mutex that serialises the searches run through it.
 *
 * Nothing here touches the keyspace, so nothing is propagated to replicas or
 * written to the AOF. Sessions are per node (in a cluster, send every call
 * for one session to the same node). They are bounded: at most `max` live
 * sessions, and one idle for `ttl` seconds is dropped the next time the table
 * is touched.
 *
 * Control commands run on the main thread and never wait for a search: if a
 * search holds the session they answer "session busy" instead of blocking
 * the server.
 */
#include "fv_common.h"

#include <ctype.h>
#include <errno.h>
#include <pthread.h>
#include <strings.h>
#include <time.h>

#define SESSION_ID_MAX 64

struct fv_session {
    char              id[SESSION_ID_MAX + 1];
    fsql_ctx         *ctx;
    pthread_mutex_t   mu;           /* serialises use of ctx */
    int               enabled;      /* diversification on (mirrors the core) */
    int               refs;         /* guarded by g_smu */
    int               closing;      /* guarded by g_smu: unlinked, free at refs == 0 */
    time_t            last_used;    /* guarded by g_smu */
    struct fv_session *next;
};

static pthread_mutex_t g_smu = PTHREAD_MUTEX_INITIALIZER;
static fv_session     *g_head = NULL;
static int             g_count = 0;
static int             g_max = 64;
static int             g_ttl = 3600;

void
fv_session_configure(int max_sessions, int ttl_sec)
{
    pthread_mutex_lock(&g_smu);
    g_max = max_sessions;
    g_ttl = ttl_sec;
    pthread_mutex_unlock(&g_smu);
}

static void
session_free(fv_session *s)
{
    if (s->ctx != NULL) fsql_free(s->ctx);
    pthread_mutex_destroy(&s->mu);
    free(s);
}

/* Caller holds g_smu. Unlinks s; frees it now if nothing uses it. */
static void
session_unlink(fv_session *s)
{
    fv_session **pp = &g_head;
    while (*pp != NULL && *pp != s) pp = &(*pp)->next;
    if (*pp == s) { *pp = s->next; g_count--; }
    s->closing = 1;
    if (s->refs == 0) session_free(s);
}

/* Caller holds g_smu. */
static void
sweep_idle(time_t now)
{
    if (g_ttl <= 0) return;
    fv_session *s = g_head;
    while (s != NULL) {
        fv_session *nx = s->next;
        if (s->refs == 0 && now - s->last_used > g_ttl) session_unlink(s);
        s = nx;
    }
}

static int
valid_id(const char *id, size_t len)
{
    if (len == 0 || len > SESSION_ID_MAX) return 0;
    for (size_t i = 0; i < len; i++) {
        unsigned char c = (unsigned char) id[i];
        if (!(isalnum(c) || c == '_' || c == '.' || c == ':' || c == '-')) return 0;
    }
    return 1;
}

fv_session *
fv_session_acquire(RedisModuleString *name, int create, char *err, size_t errsz)
{
    size_t len = 0;
    const char *id = RedisModule_StringPtrLen(name, &len);
    if (!valid_id(id, len)) {
        snprintf(err, errsz, "session id must be 1 to %d characters of A-Z a-z 0-9 _ . : -", SESSION_ID_MAX);
        return NULL;
    }
    time_t now = time(NULL);
    pthread_mutex_lock(&g_smu);
    sweep_idle(now);
    for (fv_session *s = g_head; s != NULL; s = s->next) {
        if (strlen(s->id) == len && memcmp(s->id, id, len) == 0) {
            s->refs++;
            s->last_used = now;
            pthread_mutex_unlock(&g_smu);
            return s;
        }
    }
    if (!create) {
        pthread_mutex_unlock(&g_smu);
        snprintf(err, errsz, "no such session (create it with FRACTAL.DIVERSIFY.ENABLE)");
        return NULL;
    }
    if (g_count >= g_max) {
        pthread_mutex_unlock(&g_smu);
        snprintf(err, errsz, "too many sessions (the limit is %d; close one with FRACTAL.SESSION.CLOSE)", g_max);
        return NULL;
    }
    fv_session *s = (fv_session *) calloc(1, sizeof *s);
    if (s != NULL) {
        memcpy(s->id, id, len);
        s->ctx = fsql_new_sovereign(NULL, NULL);
        if (s->ctx == NULL) { free(s); s = NULL; }
    }
    if (s == NULL) {
        pthread_mutex_unlock(&g_smu);
        snprintf(err, errsz, "could not create the session (out of memory)");
        return NULL;
    }
    pthread_mutex_init(&s->mu, NULL);
    s->refs = 1;
    s->last_used = now;
    s->next = g_head;
    g_head = s;
    g_count++;
    pthread_mutex_unlock(&g_smu);
    return s;
}

void
fv_session_release(fv_session *s)
{
    if (s == NULL) return;
    pthread_mutex_lock(&g_smu);
    s->refs--;
    if (s->closing && s->refs == 0) session_free(s);
    pthread_mutex_unlock(&g_smu);
}

/* Worker side: the session's context, with the session locked. */
fsql_ctx *
fv_session_lock(fv_session *s)
{
    pthread_mutex_lock(&s->mu);
    return s->ctx;
}

void
fv_session_unlock(fv_session *s)
{
    pthread_mutex_unlock(&s->mu);
}

/* MODULE UNLOAD: the pool is stopped first, so no job holds a session. */
void
fv_session_shutdown(void)
{
    pthread_mutex_lock(&g_smu);
    while (g_head != NULL) {
        fv_session *s = g_head;
        g_head = s->next;
        g_count--;
        s->closing = 1;
        if (s->refs == 0) session_free(s);
    }
    pthread_mutex_unlock(&g_smu);
}

/* ---------------------------------------------------------------- */
/* commands (main thread; never wait for a search)                    */
/* ---------------------------------------------------------------- */

static fv_session *
open_session(RedisModuleCtx *ctx, RedisModuleString *name, int create)
{
    char err[160];
    fv_session *s = fv_session_acquire(name, create, err, sizeof err);
    if (s == NULL) fv_reply_err(ctx, err);
    return s;
}

static int
try_lock(RedisModuleCtx *ctx, fv_session *s)
{
    if (pthread_mutex_trylock(&s->mu) == 0) return 0;
    fv_session_release(s);
    fv_reply_err(ctx, "session busy: a search is running on it; retry");
    return -1;
}

static int
core_fail(RedisModuleCtx *ctx, fv_session *s, const char *what)
{
    const char *e = fsql_last_error(s->ctx);
    char m[256];
    snprintf(m, sizeof m, "%s failed: %s", what, e != NULL && *e != '\0' ? e : "(no detail)");
    pthread_mutex_unlock(&s->mu);
    fv_session_release(s);
    return fv_reply_err(ctx, m);
}

static int
Enable_Command(RedisModuleCtx *ctx, RedisModuleString **argv, int argc)
{
    if (argc != 2) return RedisModule_WrongArity(ctx);
    fv_session *s = open_session(ctx, argv[1], 1);
    if (s == NULL) return REDISMODULE_OK;
    if (try_lock(ctx, s) != 0) return REDISMODULE_OK;
    if (fsql_diversify_enable(s->ctx) != 0) return core_fail(ctx, s, "diversify enable");
    s->enabled = 1;
    pthread_mutex_unlock(&s->mu);
    fv_session_release(s);
    return RedisModule_ReplyWithSimpleString(ctx, "OK");
}

static int
Disable_Command(RedisModuleCtx *ctx, RedisModuleString **argv, int argc)
{
    if (argc != 2) return RedisModule_WrongArity(ctx);
    fv_session *s = open_session(ctx, argv[1], 0);
    if (s == NULL) return REDISMODULE_OK;
    if (try_lock(ctx, s) != 0) return REDISMODULE_OK;
    if (fsql_diversify_disable(s->ctx) != 0) return core_fail(ctx, s, "diversify disable");
    s->enabled = 0;
    pthread_mutex_unlock(&s->mu);
    fv_session_release(s);
    return RedisModule_ReplyWithSimpleString(ctx, "OK");
}

/* A flat JSON object of numbers -> the named fields; anything else is refused. */
static int
parse_params(const char *t, size_t n, fsql_diversify_params_t *p, char *err, size_t errsz)
{
    size_t i = 0;
#define WS() while (i < n && (t[i] == ' ' || t[i] == '\t' || t[i] == '\n' || t[i] == '\r')) i++
    WS();
    if (i >= n || t[i] != '{') { snprintf(err, errsz, "params must be a JSON object"); return -1; }
    i++;
    WS();
    if (i < n && t[i] == '}') return 0;
    for (;;) {
        WS();
        if (i >= n || t[i] != '"') { snprintf(err, errsz, "params: expected a key"); return -1; }
        i++;
        char key[48]; size_t kl = 0;
        while (i < n && t[i] != '"') {
            if (t[i] == '\\' || kl + 1 >= sizeof key) { snprintf(err, errsz, "params: bad key"); return -1; }
            key[kl++] = t[i++];
        }
        if (i >= n) { snprintf(err, errsz, "params: unterminated key"); return -1; }
        key[kl] = '\0';
        i++;
        WS();
        if (i >= n || t[i] != ':') { snprintf(err, errsz, "params: expected ':' after \"%.40s\"", key); return -1; }
        i++;
        WS();
        char num[64]; size_t nl = 0;
        while (i < n && nl + 1 < sizeof num && (isdigit((unsigned char) t[i]) || t[i] == '-' || t[i] == '+' ||
               t[i] == '.' || t[i] == 'e' || t[i] == 'E')) num[nl++] = t[i++];
        num[nl] = '\0';
        char *end = NULL;
        double v = nl ? strtod(num, &end) : 0;
        if (nl == 0 || end == NULL || *end != '\0' || !isfinite(v)) {
            snprintf(err, errsz, "params: \"%.40s\" must be a finite number", key);
            return -1;
        }
        if (strcmp(key, "stall_threshold") == 0)        p->stall_threshold = v;
        else if (strcmp(key, "repulsion_sigma") == 0)   p->repulsion_sigma = v;
        else if (strcmp(key, "repulsion_weight") == 0)  p->repulsion_weight = v;
        else if (strcmp(key, "window_n") == 0 || strcmp(key, "max_shadows_considered") == 0 ||
                 strcmp(key, "tail_buffer_cap") == 0) {
            if (v < 0 || v > 4294967295.0 || v != floor(v)) {
                snprintf(err, errsz, "params: \"%.40s\" must be a whole number", key);
                return -1;
            }
            if (key[0] == 'w')      p->window_n = (uint32_t) v;
            else if (key[0] == 'm') p->max_shadows_considered = (uint32_t) v;
            else                    p->tail_buffer_cap = (uint32_t) v;
        } else {
            snprintf(err, errsz, "params: unknown key \"%.40s\"", key);
            return -1;
        }
        WS();
        if (i < n && t[i] == ',') { i++; continue; }
        if (i < n && t[i] == '}') { i++; break; }
        snprintf(err, errsz, "params: expected ',' or '}'");
        return -1;
    }
    WS();
    if (i != n) { snprintf(err, errsz, "params: trailing text after the object"); return -1; }
#undef WS
    return 0;
}

static int
Params_Command(RedisModuleCtx *ctx, RedisModuleString **argv, int argc)
{
    if (argc != 2 && argc != 3) return RedisModule_WrongArity(ctx);
    fv_session *s = open_session(ctx, argv[1], 1);
    if (s == NULL) return REDISMODULE_OK;
    if (try_lock(ctx, s) != 0) return REDISMODULE_OK;
    fsql_diversify_params_t p;
    memset(&p, 0, sizeof p);
    if (fsql_diversify_get_params(s->ctx, &p) != 0) return core_fail(ctx, s, "reading the parameters");
    if (argc == 3) {
        size_t jl = 0;
        const char *j = RedisModule_StringPtrLen(argv[2], &jl);
        char err[160];
        if (jl > 4096 || parse_params(j, jl, &p, err, sizeof err) != 0) {
            pthread_mutex_unlock(&s->mu);
            fv_session_release(s);
            return fv_reply_err(ctx, jl > 4096 ? "params too long" : err);
        }
        if (fsql_diversify_set_params(s->ctx, &p) != 0) return core_fail(ctx, s, "set params");
        if (fsql_diversify_get_params(s->ctx, &p) != 0) return core_fail(ctx, s, "reading the parameters");
    }
    char buf[320];
    snprintf(buf, sizeof buf,
             "{\"window_n\":%u,\"stall_threshold\":%.10g,\"repulsion_sigma\":%.10g,"
             "\"repulsion_weight\":%.10g,\"max_shadows_considered\":%u,\"tail_buffer_cap\":%u}",
             (unsigned) p.window_n, p.stall_threshold, p.repulsion_sigma, p.repulsion_weight,
             (unsigned) p.max_shadows_considered, (unsigned) p.tail_buffer_cap);
    pthread_mutex_unlock(&s->mu);
    fv_session_release(s);
    return RedisModule_ReplyWithStringBuffer(ctx, buf, strlen(buf));
}

static int
Dq_Command(RedisModuleCtx *ctx, RedisModuleString **argv, int argc)
{
    if (argc != 2) return RedisModule_WrongArity(ctx);
    fv_session *s = open_session(ctx, argv[1], 0);
    if (s == NULL) return REDISMODULE_OK;
    if (try_lock(ctx, s) != 0) return REDISMODULE_OK;
    double dq = fsql_diversify_current_dq(s->ctx);
    pthread_mutex_unlock(&s->mu);
    fv_session_release(s);
    if (!isfinite(dq)) return RedisModule_ReplyWithNull(ctx);
    return RedisModule_ReplyWithDouble(ctx, dq);
}

static int
Explain_Command(RedisModuleCtx *ctx, RedisModuleString **argv, int argc)
{
    if (argc != 2) return RedisModule_WrongArity(ctx);
    fv_session *s = open_session(ctx, argv[1], 0);
    if (s == NULL) return REDISMODULE_OK;
    if (try_lock(ctx, s) != 0) return REDISMODULE_OK;
    double dq = fsql_diversify_current_dq(s->ctx);
    double p99 = fsql_diversify_overhead_p99_us(s->ctx);
    int en = s->enabled;
    pthread_mutex_unlock(&s->mu);
    fv_session_release(s);
    char a[40], b[40], buf[160];
    if (isfinite(dq)) snprintf(a, sizeof a, "%.10g", dq); else snprintf(a, sizeof a, "null");
    if (isfinite(p99)) snprintf(b, sizeof b, "%.10g", p99); else snprintf(b, sizeof b, "null");
    snprintf(buf, sizeof buf, "{\"dq\":%s,\"diversify_enabled\":%s,\"overhead_p99_us\":%s}", a, en ? "true" : "false", b);
    return RedisModule_ReplyWithStringBuffer(ctx, buf, strlen(buf));
}

static int
report(RedisModuleCtx *ctx, fv_session *s, long long row, fsql_engagement_kind_t kind, long long dwell)
{
    if (try_lock(ctx, s) != 0) return REDISMODULE_OK;
    if (fsql_feedback_report(s->ctx, (uint64_t) row, kind, (uint32_t) dwell) != 0)
        return core_fail(ctx, s, "feedback");
    pthread_mutex_unlock(&s->mu);
    fv_session_release(s);
    return RedisModule_ReplyWithSimpleString(ctx, "OK");
}

static int
Feedback_Command(RedisModuleCtx *ctx, RedisModuleString **argv, int argc)
{
    if (argc != 4 && argc != 5) return RedisModule_WrongArity(ctx);
    char err[160];
    long long row = 0, dwell = 0;
    if (fv_arg_ll(argv[2], "row", 0, 4294967295LL, &row, err, sizeof err) != 0) return fv_reply_err(ctx, err);
    size_t kl = 0;
    const char *k = RedisModule_StringPtrLen(argv[3], &kl);
    fsql_engagement_kind_t kind;
    if (kl == 5 && strncasecmp(k, "dwell", 5) == 0)             kind = FSQL_ENGAGE_DWELL;
    else if (kl == 8 && strncasecmp(k, "positive", 8) == 0)     kind = FSQL_ENGAGE_POSITIVE;
    else if (kl == 8 && strncasecmp(k, "negative", 8) == 0)     kind = FSQL_ENGAGE_NEGATIVE;
    else return fv_reply_err(ctx, "kind must be one of DWELL, POSITIVE, NEGATIVE");
    if (argc == 5 && fv_arg_ll(argv[4], "dwell_ms", 0, 4294967295LL, &dwell, err, sizeof err) != 0) return fv_reply_err(ctx, err);
    fv_session *s = open_session(ctx, argv[1], 0);
    if (s == NULL) return REDISMODULE_OK;
    return report(ctx, s, row, kind, dwell);
}

static int
Isolate_Command(RedisModuleCtx *ctx, RedisModuleString **argv, int argc)
{
    if (argc != 3) return RedisModule_WrongArity(ctx);
    char err[160];
    long long row = 0;
    if (fv_arg_ll(argv[2], "row", 0, 4294967295LL, &row, err, sizeof err) != 0) return fv_reply_err(ctx, err);
    fv_session *s = open_session(ctx, argv[1], 0);
    if (s == NULL) return REDISMODULE_OK;
    return report(ctx, s, row, FSQL_ENGAGE_NEGATIVE, 0);
}

static int
Close_Command(RedisModuleCtx *ctx, RedisModuleString **argv, int argc)
{
    if (argc != 2) return RedisModule_WrongArity(ctx);
    fv_session *s = open_session(ctx, argv[1], 0);
    if (s == NULL) return REDISMODULE_OK;
    pthread_mutex_lock(&g_smu);
    session_unlink(s);          /* freed by the last release (ours, or a running search's) */
    pthread_mutex_unlock(&g_smu);
    fv_session_release(s);
    return RedisModule_ReplyWithSimpleString(ctx, "OK");
}

static int
List_Command(RedisModuleCtx *ctx, RedisModuleString **argv, int argc)
{
    (void) argv;
    if (argc != 1) return RedisModule_WrongArity(ctx);
    pthread_mutex_lock(&g_smu);
    sweep_idle(time(NULL));
    RedisModule_ReplyWithArray(ctx, (long) g_count);
    for (fv_session *s = g_head; s != NULL; s = s->next)
        RedisModule_ReplyWithStringBuffer(ctx, s->id, strlen(s->id));
    pthread_mutex_unlock(&g_smu);
    return REDISMODULE_OK;
}

int
fv_register_session(RedisModuleCtx *ctx)
{
    static const struct { const char *name; RedisModuleCmdFunc fn; const char *acl; } cmds[] = {
        { "FRACTAL.DIVERSIFY.ENABLE",  Enable_Command,   "write fast" },
        { "FRACTAL.DIVERSIFY.DISABLE", Disable_Command,  "write fast" },
        { "FRACTAL.DIVERSIFY.PARAMS",  Params_Command,   "write fast" },
        { "FRACTAL.DIVERSIFY.DQ",      Dq_Command,       "read fast" },
        { "FRACTAL.DIVERSIFY.EXPLAIN", Explain_Command,  "read fast" },
        { "FRACTAL.FEEDBACK",          Feedback_Command, "write fast" },
        { "FRACTAL.ISOLATE_BACKGROUND", Isolate_Command, "write fast" },
        { "FRACTAL.SESSION.CLOSE",     Close_Command,    "write fast" },
        { "FRACTAL.SESSION.LIST",      List_Command,     "read fast" },
    };
    for (size_t i = 0; i < sizeof cmds / sizeof cmds[0]; i++) {
        if (RedisModule_CreateCommand(ctx, cmds[i].name, cmds[i].fn, "fast allow-stale", 0, 0, 0) == REDISMODULE_ERR) return REDISMODULE_ERR;
        if (fv_acl(ctx, cmds[i].name, cmds[i].acl) != 0) return REDISMODULE_ERR;
    }
    return REDISMODULE_OK;
}
