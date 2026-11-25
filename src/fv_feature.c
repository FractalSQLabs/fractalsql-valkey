/* src/fv_feature.c -- the named feature store: a per-doc_id vector store kept in an ordinary hash.
 *
 *   FRACTAL.FEATURE.STORE key doc_id vector      -> OK       (HSET key <doc_id> <packed float32>; the last write wins)
 *   FRACTAL.FEATURE.MINE  key vector k           -> JSON     ([{"doc_id":..,"distance":..}], nearest first, true Euclidean distance)
 *
 * `vector` is a packed little-endian float32 blob, as everywhere in this module. The hash is the caller's own key, so the keyspace
 * permissions, expiry, replication and persistence are the server's. MINE is a brute-force scan of the hash; a stored vector of another
 * dimension, or a field that is not a vector, is skipped. */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "fv_common.h"

#define FF_MAX_DIM_BYTES FV_MAX_BLOB_BYTES
#define FF_MAX_K 100000

typedef struct { long long doc; double dist; } ff_cand;

typedef struct {
    const float *q; size_t dim;
    ff_cand *c; size_t n, cap; int oom;
} ff_scan;

static int ff_cmp(const void *a, const void *b)
{
    const ff_cand *x = a, *y = b;
    if (x->dist < y->dist) return -1;
    if (x->dist > y->dist) return 1;
    return x->doc < y->doc ? -1 : x->doc > y->doc;
}

static void ff_visit(RedisModuleKey *key, RedisModuleString *field, RedisModuleString *value, void *pd)
{
    (void) key;
    ff_scan *s = pd;
    if (s->oom || value == NULL) return;
    size_t fl, vl;
    const char *fp = RedisModule_StringPtrLen(field, &fl);
    const char *vp = RedisModule_StringPtrLen(value, &vl);
    if (vl != s->dim * sizeof(float) || fl == 0 || fl > 20) return;
    char buf[24]; memcpy(buf, fp, fl); buf[fl] = '\0';
    char *end = NULL; long long doc = strtoll(buf, &end, 10);
    if (end == buf || *end != '\0' || doc < 0) return;
    if (s->n == s->cap) {
        size_t nc = s->cap ? s->cap * 2 : 64;
        ff_cand *t = realloc(s->c, nc * sizeof *t);
        if (t == NULL) { s->oom = 1; return; }
        s->c = t; s->cap = nc;
    }
    double sum = 0.0;
    for (size_t i = 0; i < s->dim; i++) {
        float f; memcpy(&f, vp + i * sizeof f, sizeof f);
        double d = (double) f - (double) s->q[i];
        sum += d * d;
    }
    if (!isfinite(sum)) return;
    s->c[s->n].doc = doc; s->c[s->n].dist = sqrt(sum); s->n++;
}

static int
Store_Cmd(RedisModuleCtx *ctx, RedisModuleString **argv, int argc)
{
    if (argc != 4) return RedisModule_WrongArity(ctx);
    char err[200];
    long long doc;
    if (fv_arg_ll(argv[2], "doc_id", 0, 9223372036854775807LL, &doc, err, sizeof err) != 0) return fv_reply_err(ctx, err);
    size_t bl; const char *bp = RedisModule_StringPtrLen(argv[3], &bl);
    float *f = NULL; size_t n = 0;
    if (fv_blob_to_floats(argv[3], "feature_array", FF_MAX_DIM_BYTES, &f, &n, err, sizeof err) != 0) return fv_reply_err(ctx, err);
    free(f);
    (void) bl; (void) bp;
    RedisModuleKey *k = RedisModule_OpenKey(ctx, argv[1], REDISMODULE_READ | REDISMODULE_WRITE);
    if (k == NULL) return fv_reply_err(ctx, "cannot open the key");
    int t = RedisModule_KeyType(k);
    if (t != REDISMODULE_KEYTYPE_EMPTY && t != REDISMODULE_KEYTYPE_HASH) {
        RedisModule_CloseKey(k);
        return RedisModule_ReplyWithError(ctx, "WRONGTYPE Operation against a key holding the wrong kind of value");
    }
    char field[24]; snprintf(field, sizeof field, "%lld", doc);
    int rc = RedisModule_HashSet(k, REDISMODULE_HASH_CFIELDS, field, argv[3], NULL);
    RedisModule_CloseKey(k);
    if (rc == REDISMODULE_ERR) return fv_reply_err(ctx, "could not write the vector");
    RedisModule_ReplicateVerbatim(ctx);
    return RedisModule_ReplyWithSimpleString(ctx, "OK");
}

static int
Mine_Cmd(RedisModuleCtx *ctx, RedisModuleString **argv, int argc)
{
    if (argc != 4) return RedisModule_WrongArity(ctx);
    char err[200];
    long long k;
    if (fv_arg_ll(argv[3], "k", 1, FF_MAX_K, &k, err, sizeof err) != 0) return fv_reply_err(ctx, err);
    float *q = NULL; size_t dim = 0;
    if (fv_blob_to_floats(argv[2], "surrogate_vector", FF_MAX_DIM_BYTES, &q, &dim, err, sizeof err) != 0) return fv_reply_err(ctx, err);
    RedisModuleKey *h = RedisModule_OpenKey(ctx, argv[1], REDISMODULE_READ);
    ff_scan s = { q, dim, NULL, 0, 0, 0 };
    if (h != NULL) {
        int t = RedisModule_KeyType(h);
        if (t != REDISMODULE_KEYTYPE_HASH && t != REDISMODULE_KEYTYPE_EMPTY) {
            RedisModule_CloseKey(h); free(q);
            return RedisModule_ReplyWithError(ctx, "WRONGTYPE Operation against a key holding the wrong kind of value");
        }
        if (t == REDISMODULE_KEYTYPE_HASH) {
            RedisModuleScanCursor *cur = RedisModule_ScanCursorCreate();
            while (RedisModule_ScanKey(h, cur, ff_visit, &s)) { if (s.oom) break; }
            RedisModule_ScanCursorDestroy(cur);
        }
        RedisModule_CloseKey(h);
    }
    free(q);
    if (s.oom) { free(s.c); return fv_reply_err(ctx, "out of memory"); }
    if (s.n > 1) qsort(s.c, s.n, sizeof *s.c, ff_cmp);
    size_t out = s.n < (size_t) k ? s.n : (size_t) k;
    size_t cap = 4 + out * 96;   /* a row is at most ~68 bytes: 19-digit id, 24-char %.17g */
    char *js = malloc(cap);
    if (js == NULL) { free(s.c); return fv_reply_err(ctx, "out of memory"); }
    size_t p = 0;
    js[p++] = '[';
    for (size_t i = 0; i < out; i++)
        p += (size_t) snprintf(js + p, cap - p, "%s{\"doc_id\":%lld,\"distance\":%.17g}", i ? "," : "", s.c[i].doc, s.c[i].dist);
    js[p++] = ']';
    int rc = RedisModule_ReplyWithStringBuffer(ctx, js, p);
    free(js); free(s.c);
    return rc;
}

int
fv_register_feature(RedisModuleCtx *ctx)
{
    if (RedisModule_CreateCommand(ctx, "FRACTAL.FEATURE.STORE", Store_Cmd, "write deny-oom", 1, 1, 1) == REDISMODULE_ERR ||
        RedisModule_CreateCommand(ctx, "FRACTAL.FEATURE.MINE", Mine_Cmd, "readonly", 1, 1, 1) == REDISMODULE_ERR ||
        fv_acl(ctx, "FRACTAL.FEATURE.STORE", "write slow") != 0 ||
        fv_acl(ctx, "FRACTAL.FEATURE.MINE", "read slow") != 0)
        return REDISMODULE_ERR;
    return REDISMODULE_OK;
}
