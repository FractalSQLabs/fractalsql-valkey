/* src/fv_vector.c
 * SPDX-License-Identifier: Apache-2.0 AND BSD-2-Clause
 * SPDX-FileCopyrightText: 2026 Daniel Gardiner d/b/a FractalSQLabs
 *
 * Vector math over packed little-endian float32 BLOBs (the same format the
 * corpus keys use). Cheap enough to run inline on the main thread.
 *
 *   FRACTAL.VECTOR.DOT|L2|L2SQ|COSINE|SIMILARITY  a b      -> double
 *   FRACTAL.VECTOR.NORM                            a        -> double
 *   FRACTAL.VECTOR.NORMALIZE                       a        -> BLOB
 *   FRACTAL.VECTOR.ADD|SUB                         a b      -> BLOB
 *   FRACTAL.VECTOR.SCALE                           a s      -> BLOB
 *   FRACTAL.VECTOR.LP                              a b p    -> double
 *   FRACTAL.VECTOR.CONCAT                          a alpha b beta -> BLOB
 *   FRACTAL.VECTOR.QUANTIZE.INT8                   a        -> {"scale","codes"}
 *   FRACTAL.VECTOR.QUANTIZE.BINARY                 a        -> BLOB (packed bits)
 *   FRACTAL.VECTOR.HAMMING                         a b      -> integer (bit blobs)
 *   FRACTAL.VECTOR.FROMTEXT  "1,2,3" | "[1,2,3]"            -> BLOB
 *   FRACTAL.VECTOR.TOTEXT                          a        -> "[1,2,3]"
 */
#include "fv_common.h"
#include "fsql_precision.h"

#define ERRSZ 256

static int
two_vectors(RedisModuleCtx *ctx, RedisModuleString **argv, float **a, float **b, size_t *dim)
{
    char err[ERRSZ];
    size_t na = 0, nb = 0;
    *a = *b = NULL;
    if (fv_blob_to_floats(argv[1], "a", FV_MAX_BLOB_BYTES, a, &na, err, sizeof err) != 0 ||
        fv_blob_to_floats(argv[2], "b", FV_MAX_BLOB_BYTES, b, &nb, err, sizeof err) != 0) {
        free(*a); free(*b); *a = *b = NULL;
        fv_reply_err(ctx, err);
        return -1;
    }
    if (na != nb) {
        char m[ERRSZ];
        snprintf(m, sizeof m, "dimension mismatch (a has %zu, b has %zu)", na, nb);
        free(*a); free(*b); *a = *b = NULL;
        fv_reply_err(ctx, m);
        return -1;
    }
    *dim = na;
    return 0;
}

static int
core_failed(RedisModuleCtx *ctx, const char *fn, int rc)
{
    char m[ERRSZ];
    snprintf(m, sizeof m, "%s failed (rc=%d)", fn, rc);
    return fv_reply_err(ctx, m);
}

typedef double (*dist2_fn)(const float *, const float *, size_t);

/* The SQL-visible distances come from the ONE shared implementation (fsql_precision.h): the sums accumulate in double in a
 * fixed sequential order and the result is returned as a double (no rounding to float32 on the way out), so every host agrees
 * bit for bit. */
static int
binary_double(RedisModuleCtx *ctx, RedisModuleString **argv, int argc, dist2_fn fn)
{
    if (argc != 3) return RedisModule_WrongArity(ctx);
    float *a, *b; size_t dim;
    if (two_vectors(ctx, argv, &a, &b, &dim) != 0) return REDISMODULE_OK;
    double out = fn(a, b, dim);
    free(a); free(b);
    return RedisModule_ReplyWithDouble(ctx, out);
}

#define BIN_DOUBLE(CMD, FN) \
    static int CMD(RedisModuleCtx *c, RedisModuleString **v, int n) { return binary_double(c, v, n, FN); }
BIN_DOUBLE(Dot_Cmd,    fsqlp_dot_f)
BIN_DOUBLE(L2_Cmd,     fsqlp_l2_f)
BIN_DOUBLE(L2sq_Cmd,   fsqlp_l2_sq_f)
BIN_DOUBLE(Cosd_Cmd,   fsqlp_cosine_distance_f)
BIN_DOUBLE(Coss_Cmd,   fsqlp_cosine_similarity_f)

static int
binary_blob(RedisModuleCtx *ctx, RedisModuleString **argv, int argc,
            int (*fn)(const float *, const float *, size_t, float *), const char *name)
{
    if (argc != 3) return RedisModule_WrongArity(ctx);
    float *a, *b; size_t dim;
    if (two_vectors(ctx, argv, &a, &b, &dim) != 0) return REDISMODULE_OK;
    float *out = (float *) malloc(dim * sizeof(float));
    if (out == NULL) { free(a); free(b); return fv_reply_err(ctx, "out of memory"); }
    int rc = fn(a, b, dim, out);
    free(a); free(b);
    int ret = rc != 0 ? core_failed(ctx, name, rc)
                      : RedisModule_ReplyWithStringBuffer(ctx, (const char *) out, dim * sizeof(float));
    free(out);
    return ret;
}

static int Add_Cmd(RedisModuleCtx *c, RedisModuleString **v, int n) { return binary_blob(c, v, n, fsql_vector_add, "fsql_vector_add"); }
static int Sub_Cmd(RedisModuleCtx *c, RedisModuleString **v, int n) { return binary_blob(c, v, n, fsql_vector_sub, "fsql_vector_sub"); }

static int
Norm_Cmd(RedisModuleCtx *ctx, RedisModuleString **argv, int argc)
{
    if (argc != 2) return RedisModule_WrongArity(ctx);
    char err[ERRSZ]; float *a; size_t n;
    if (fv_blob_to_floats(argv[1], "a", FV_MAX_BLOB_BYTES, &a, &n, err, sizeof err) != 0)
        return fv_reply_err(ctx, err);
    double out = fsqlp_norm_f(a, n);
    free(a);
    return RedisModule_ReplyWithDouble(ctx, out);
}

static int
Normalize_Cmd(RedisModuleCtx *ctx, RedisModuleString **argv, int argc)
{
    if (argc != 2) return RedisModule_WrongArity(ctx);
    char err[ERRSZ]; float *a; size_t n;
    if (fv_blob_to_floats(argv[1], "a", FV_MAX_BLOB_BYTES, &a, &n, err, sizeof err) != 0)
        return fv_reply_err(ctx, err);
    float *out = (float *) malloc(n * sizeof(float));
    if (out == NULL) { free(a); return fv_reply_err(ctx, "out of memory"); }
    int rc = fsql_vector_normalize(a, n, out);
    free(a);
    int ret = rc != 0 ? core_failed(ctx, "fsql_vector_normalize", rc)
                      : RedisModule_ReplyWithStringBuffer(ctx, (const char *) out, n * sizeof(float));
    free(out);
    return ret;
}

static int
Scale_Cmd(RedisModuleCtx *ctx, RedisModuleString **argv, int argc)
{
    if (argc != 3) return RedisModule_WrongArity(ctx);
    char err[ERRSZ]; float *a; size_t n; double s;
    if (fv_blob_to_floats(argv[1], "a", FV_MAX_BLOB_BYTES, &a, &n, err, sizeof err) != 0)
        return fv_reply_err(ctx, err);
    if (fv_arg_double(argv[2], "scalar", &s, err, sizeof err) != 0) { free(a); return fv_reply_err(ctx, err); }
    float *out = (float *) malloc(n * sizeof(float));
    if (out == NULL) { free(a); return fv_reply_err(ctx, "out of memory"); }
    int rc = fsql_vector_scale(a, n, (float) s, out);
    free(a);
    int ret = rc != 0 ? core_failed(ctx, "fsql_vector_scale", rc)
                      : RedisModule_ReplyWithStringBuffer(ctx, (const char *) out, n * sizeof(float));
    free(out);
    return ret;
}

static int
Lp_Cmd(RedisModuleCtx *ctx, RedisModuleString **argv, int argc)
{
    if (argc != 4) return RedisModule_WrongArity(ctx);
    char err[ERRSZ]; double p;
    if (fv_arg_double(argv[3], "p", &p, err, sizeof err) != 0) return fv_reply_err(ctx, err);
    if (p <= 0) return fv_reply_err(ctx, "p must be > 0");
    float *a, *b; size_t dim;
    if (two_vectors(ctx, argv, &a, &b, &dim) != 0) return REDISMODULE_OK;
    float out = 0;
    int rc = fsql_vector_lp_distance(a, b, dim, (float) p, &out);
    free(a); free(b);
    return rc != 0 ? core_failed(ctx, "fsql_vector_lp_distance", rc) : RedisModule_ReplyWithDouble(ctx, (double) out);
}

static int
Concat_Cmd(RedisModuleCtx *ctx, RedisModuleString **argv, int argc)
{
    if (argc != 5) return RedisModule_WrongArity(ctx);
    char err[ERRSZ]; float *a = NULL, *b = NULL; size_t na, nb; double alpha, beta;
    if (fv_blob_to_floats(argv[1], "a", FV_MAX_BLOB_BYTES, &a, &na, err, sizeof err) != 0 ||
        fv_arg_double(argv[2], "alpha", &alpha, err, sizeof err) != 0 ||
        fv_blob_to_floats(argv[3], "b", FV_MAX_BLOB_BYTES, &b, &nb, err, sizeof err) != 0 ||
        fv_arg_double(argv[4], "beta", &beta, err, sizeof err) != 0) {
        free(a); free(b);
        return fv_reply_err(ctx, err);
    }
    float *out = (float *) malloc((na + nb) * sizeof(float));
    if (out == NULL) { free(a); free(b); return fv_reply_err(ctx, "out of memory"); }
    int rc = fsql_vector_weighted_concat(a, na, (float) alpha, b, nb, (float) beta, out);
    free(a); free(b);
    int ret = rc != 0 ? core_failed(ctx, "fsql_vector_weighted_concat", rc)
                      : RedisModule_ReplyWithStringBuffer(ctx, (const char *) out, (na + nb) * sizeof(float));
    free(out);
    return ret;
}

static int
QuantInt8_Cmd(RedisModuleCtx *ctx, RedisModuleString **argv, int argc)
{
    if (argc != 2) return RedisModule_WrongArity(ctx);
    char err[ERRSZ]; float *a; size_t n;
    if (fv_blob_to_floats(argv[1], "a", FV_MAX_BLOB_BYTES, &a, &n, err, sizeof err) != 0)
        return fv_reply_err(ctx, err);
    int8_t *codes = (int8_t *) malloc(n);
    if (codes == NULL) { free(a); return fv_reply_err(ctx, "out of memory"); }
    float scale = 0;
    int rc = fsql_vector_quantize_int8(a, n, codes, &scale);
    free(a);
    if (rc != 0) { free(codes); return core_failed(ctx, "fsql_vector_quantize_int8", rc); }
    fv_sb sb; fv_sb_init(&sb);
    fv_sb_appendf(&sb, "{\"scale\":%.10g,\"codes\":[", (double) scale);
    for (size_t i = 0; i < n; i++) fv_sb_appendf(&sb, "%s%d", i ? "," : "", (int) codes[i]);
    fv_sb_append(&sb, "]}");
    free(codes);
    size_t len; char *s = fv_sb_take(&sb, &len);
    if (s == NULL) return fv_reply_err(ctx, "out of memory");
    int ret = RedisModule_ReplyWithStringBuffer(ctx, s, len);
    free(s);
    return ret;
}

static int
QuantBin_Cmd(RedisModuleCtx *ctx, RedisModuleString **argv, int argc)
{
    if (argc != 2) return RedisModule_WrongArity(ctx);
    char err[ERRSZ]; float *a; size_t n;
    if (fv_blob_to_floats(argv[1], "a", FV_MAX_BLOB_BYTES, &a, &n, err, sizeof err) != 0)
        return fv_reply_err(ctx, err);
    size_t nbytes = (n + 7) / 8;
    uint8_t *out = (uint8_t *) calloc(nbytes, 1);
    if (out == NULL) { free(a); return fv_reply_err(ctx, "out of memory"); }
    int rc = fsql_vector_quantize_binary(a, n, out);
    free(a);
    int ret = rc != 0 ? core_failed(ctx, "fsql_vector_quantize_binary", rc)
                      : RedisModule_ReplyWithStringBuffer(ctx, (const char *) out, nbytes);
    free(out);
    return ret;
}

static int
Hamming_Cmd(RedisModuleCtx *ctx, RedisModuleString **argv, int argc)
{
    if (argc != 3) return RedisModule_WrongArity(ctx);
    size_t la, lb;
    const uint8_t *a = (const uint8_t *) RedisModule_StringPtrLen(argv[1], &la);
    const uint8_t *b = (const uint8_t *) RedisModule_StringPtrLen(argv[2], &lb);
    if (la == 0 || lb == 0) return fv_reply_err(ctx, "hamming inputs must be non-empty bit blobs");
    if (la != lb) return fv_reply_err(ctx, "hamming inputs must have equal byte lengths");
    if (la > FV_MAX_BLOB_BYTES) return fv_reply_err(ctx, "hamming input too large");
    size_t dist = 0;
    int rc = fsql_vector_hamming_distance(a, b, la, &dist);
    return rc != 0 ? core_failed(ctx, "fsql_vector_hamming_distance", rc)
                   : RedisModule_ReplyWithLongLong(ctx, (long long) dist);
}

static const char *
skip_ws(const char *p)
{
    while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') p++;
    return p;
}

/* "1,2,3" or "[1, 2, 3]" -> malloc'd float array. Refuses an empty vector,
 * junk, trailing text, a non-finite element, and a value outside float range.
 * Shared with the daemon tier, whose EMBED reply is this same grammar. */
int
fv_parse_vector_text(const char *t, size_t len, float **out, size_t *n_out,
                     char *err, size_t errsz)
{
    if (len == 0 || len > FV_MAX_BLOB_BYTES) {
        snprintf(err, errsz, "text vector is empty or too large");
        return -1;
    }
    char *buf = (char *) malloc(len + 1);
    if (buf == NULL) { snprintf(err, errsz, "out of memory"); return -1; }
    memcpy(buf, t, len);
    buf[len] = '\0';

    size_t cap = 16, n = 0;
    float *v = (float *) malloc(cap * sizeof(float));
    char msg[ERRSZ] = "";
    const char *p = skip_ws(buf);
    int bracketed = (*p == '[');
    if (bracketed) p = skip_ws(p + 1);

    if (v == NULL) snprintf(msg, sizeof msg, "out of memory");
    else if (*p == '\0' || (bracketed && *p == ']'))
        snprintf(msg, sizeof msg, "vector must have at least one element");

    while (msg[0] == '\0') {
        char *end;
        /* strtof rounds the decimal straight to float32, which is what the corpus and the query blobs hold: the text round
         * trip (%.9g) is then bit-identical. Going through a double first would round twice. */
        float d = strtof(p, &end);
        if (end == p)                        { snprintf(msg, sizeof msg, "invalid number near '%.16s'", p); break; }
        if (!isfinite(d))                    { snprintf(msg, sizeof msg, "non-finite vector element near '%.16s'", p); break; }
        if (fabs((double) d) > 3.4028234663852886e38) { snprintf(msg, sizeof msg, "value out of range near '%.16s'", p); break; }
        if (n == cap) {
            float *nv = (float *) realloc(v, cap * 2 * sizeof(float));
            if (nv == NULL) { snprintf(msg, sizeof msg, "out of memory"); break; }
            v = nv; cap *= 2;
        }
        v[n++] = d;
        p = skip_ws(end);
        if (*p != ',') break;
        p = skip_ws(p + 1);
    }
    if (msg[0] == '\0' && bracketed) {
        if (*p != ']') snprintf(msg, sizeof msg, "missing closing ']' near '%.16s'", p);
        else p = skip_ws(p + 1);
    }
    if (msg[0] == '\0' && *p != '\0') snprintf(msg, sizeof msg, "unexpected text near '%.16s'", p);

    free(buf);
    if (msg[0] != '\0') { free(v); snprintf(err, errsz, "%s", msg); return -1; }
    *out = v;
    *n_out = n;
    return 0;
}

static int
FromText_Cmd(RedisModuleCtx *ctx, RedisModuleString **argv, int argc)
{
    if (argc != 2) return RedisModule_WrongArity(ctx);
    size_t len; char err[ERRSZ]; float *v; size_t n;
    const char *t = RedisModule_StringPtrLen(argv[1], &len);
    if (fv_parse_vector_text(t, len, &v, &n, err, sizeof err) != 0) return fv_reply_err(ctx, err);
    int ret = RedisModule_ReplyWithStringBuffer(ctx, (const char *) v, n * sizeof(float));
    free(v);
    return ret;
}

static int
ToText_Cmd(RedisModuleCtx *ctx, RedisModuleString **argv, int argc)
{
    if (argc != 2) return RedisModule_WrongArity(ctx);
    char err[ERRSZ]; float *a; size_t n;
    if (fv_blob_to_floats(argv[1], "a", FV_MAX_BLOB_BYTES, &a, &n, err, sizeof err) != 0)
        return fv_reply_err(ctx, err);
    fv_sb sb; fv_sb_init(&sb);
    fv_sb_append(&sb, "[");
    for (size_t i = 0; i < n; i++) fv_sb_appendf(&sb, "%s%.9g", i ? "," : "", (double) a[i]);
    fv_sb_append(&sb, "]");
    free(a);
    size_t len; char *s = fv_sb_take(&sb, &len);
    if (s == NULL) return fv_reply_err(ctx, "out of memory");
    int ret = RedisModule_ReplyWithStringBuffer(ctx, s, len);
    free(s);
    return ret;
}

int
fv_register_vector(RedisModuleCtx *ctx)
{
    static const struct { const char *name; RedisModuleCmdFunc fn; } T[] = {
        { "FRACTAL.VECTOR.DOT",             Dot_Cmd },
        { "FRACTAL.VECTOR.L2",              L2_Cmd },
        { "FRACTAL.VECTOR.L2SQ",            L2sq_Cmd },
        { "FRACTAL.VECTOR.COSINE",          Cosd_Cmd },
        { "FRACTAL.VECTOR.SIMILARITY",      Coss_Cmd },
        { "FRACTAL.VECTOR.NORM",            Norm_Cmd },
        { "FRACTAL.VECTOR.NORMALIZE",       Normalize_Cmd },
        { "FRACTAL.VECTOR.ADD",             Add_Cmd },
        { "FRACTAL.VECTOR.SUB",             Sub_Cmd },
        { "FRACTAL.VECTOR.SCALE",           Scale_Cmd },
        { "FRACTAL.VECTOR.LP",              Lp_Cmd },
        { "FRACTAL.VECTOR.CONCAT",          Concat_Cmd },
        { "FRACTAL.VECTOR.QUANTIZE.INT8",   QuantInt8_Cmd },
        { "FRACTAL.VECTOR.QUANTIZE.BINARY", QuantBin_Cmd },
        { "FRACTAL.VECTOR.HAMMING",         Hamming_Cmd },
        { "FRACTAL.VECTOR.FROMTEXT",        FromText_Cmd },
        { "FRACTAL.VECTOR.TOTEXT",          ToText_Cmd },
    };
    for (size_t i = 0; i < sizeof T / sizeof T[0]; i++)
        if (RedisModule_CreateCommand(ctx, T[i].name, T[i].fn, "readonly fast allow-stale", 0, 0, 0)
                == REDISMODULE_ERR)
            return REDISMODULE_ERR;
        else if (fv_acl(ctx, T[i].name, "read fast") != 0)
            return REDISMODULE_ERR;
    return REDISMODULE_OK;
}
