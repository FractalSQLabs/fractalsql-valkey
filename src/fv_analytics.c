/* src/fv_analytics.c
 * SPDX-License-Identifier: Apache-2.0 AND BSD-2-Clause
 * SPDX-FileCopyrightText: 2026 Daniel Gardiner d/b/a FractalSQLabs
 *
 * Pure-compute analytics over packed little-endian float32 BLOBs. Every
 * command decodes its arguments on the main thread into a job that owns the
 * copies, then runs the core on a worker (see fv_common.h).
 *
 *   FRACTAL.DIM.DFA            series                         -> double
 *   FRACTAL.DIM.DRIFT          series window                  -> JSON
 *   FRACTAL.DIM.BOXCOUNT       points dim                     -> double
 *   FRACTAL.SERIES.CHANGEPOINT series window threshold [max]  -> {"indices":[..]}
 *   FRACTAL.SERIES.PERIODOGRAM series [max_peaks]             -> {"freqs","power"}
 *   FRACTAL.SERIES.SPECTROGRAM series window hop [max_frames] -> [..]
 *   FRACTAL.SERIES.FINGERPRINT vec bits [seed]                -> BLOB
 *   FRACTAL.CYCLE.DETECT       fingerprints n_bytes [hamming] -> JSON
 *   FRACTAL.TDA.PERSISTENCE    points dim max_dim thresh [max]-> JSON
 *   FRACTAL.OPTIMIZE.PORTFOLIO mu cov k [seed [obl [mode]]]   -> JSON
 *   FRACTAL.MORPH.VASCULAR     coords edges arc               -> JSON
 *   FRACTAL.MORPH.CORTICAL     vertices faces                 -> JSON
 *   FRACTAL.MORPH.NERVE        coords dim edges               -> JSON
 *   FRACTAL.MORPH.COMPLEXITY   points dim                     -> JSON
 *
 * Index arrays (edges, faces) are packed little-endian uint32.
 */
#include "fv_common.h"

#include <limits.h>

#define ERRSZ 256
/* Most random draws one fingerprint (bits x vector length) may cost: about
 * 4 ns each, so 2^28 is roughly a second, and the loop cannot be
 * interrupted. The same bound the other engines apply. */
#define FINGERPRINT_MAX_WORK 268435456ULL

enum {
    OP_DFA, OP_DRIFT, OP_BOX, OP_CHANGE, OP_PERIOD, OP_SPECT, OP_FPRINT,
    OP_CYCLE, OP_TDA, OP_PORTFOLIO, OP_VASC, OP_CORT, OP_NERVE, OP_COMPLEX
};

typedef struct {
    fv_job   base;                    /* first */
    int      op;
    double  *a;  size_t na;           /* primary series / points / mu / coords */
    double  *b;  size_t nb;           /* cov / arc lengths / vertices */
    size_t  *ix; size_t nix;          /* edges / faces */
    uint8_t *raw; size_t nraw;        /* fingerprint bytes */
    long long i1, i2, i3, i4;
    double   d1;
    uint64_t seed;
} acmd;

static void
acmd_dispose(fv_job *b)
{
    acmd *j = (acmd *) b;
    free(j->a); free(j->b); free(j->ix); free(j->raw);
    free(j->base.res);
    free(j);
}

static void
finish_json(fv_job *b, fv_sb *sb)
{
    size_t len; char *s = fv_sb_take(sb, &len);
    if (s == NULL) fv_job_fail(b, "out of memory building the result");
    else           fv_job_bulk(b, s, len);
}

static void
core_fail(fv_job *b, const char *fn, int rc)
{
    fv_job_fail(b, "%s failed (rc=%d%s)", fn, rc,
                rc == FSQL_ERR_INVALID ? ": invalid input for this function" :
                rc == FSQL_ERR_OOM ? ": out of memory" : "");
}

static void
acmd_work(fv_job *b)
{
    acmd *j = (acmd *) b;
    fv_sb sb; fv_sb_init(&sb);
    int rc;

    switch (j->op) {
    case OP_DFA: {
        double alpha = 0;
        rc = fsql_dimension_dfa(j->a, j->na, &alpha);
        if (rc != 0) core_fail(b, "fsql_dimension_dfa", rc); else fv_job_double(b, alpha);
        break;
    }
    case OP_DRIFT: {
        double drift = 0, ra = 0, ba = 0;
        rc = fsql_dimension_drift(j->a, j->na, (size_t) j->i1, &drift, &ra, &ba);
        if (rc != 0) { core_fail(b, "fsql_dimension_drift", rc); break; }
        fv_sb_appendf(&sb, "{\"drift\":%.10f,\"recent_alpha\":%.10f,\"baseline_alpha\":%.10f}", drift, ra, ba);
        finish_json(b, &sb);
        break;
    }
    case OP_BOX: {
        double dim = 0;
        rc = fsql_dimension_boxcount(j->a, j->na / (size_t) j->i1, (size_t) j->i1, &dim);
        if (rc != 0) core_fail(b, "fsql_dimension_boxcount", rc); else fv_job_double(b, dim);
        break;
    }
    case OP_CHANGE: {
        size_t maxp = (size_t) j->i2, nout = 0;
        size_t *idx = (size_t *) malloc(maxp * sizeof(size_t));
        if (idx == NULL) { fv_job_fail(b, "out of memory"); break; }
        rc = fsql_change_point_detect(j->a, j->na, (size_t) j->i1, j->d1, idx, maxp, &nout);
        if (rc != 0) { free(idx); core_fail(b, "fsql_change_point_detect", rc); break; }
        fv_sb_append(&sb, "{\"indices\":[");
        for (size_t i = 0; i < nout; i++) fv_sb_appendf(&sb, "%s%zu", i ? "," : "", idx[i]);
        fv_sb_append(&sb, "]}");
        free(idx);
        finish_json(b, &sb);
        break;
    }
    case OP_PERIOD: {
        size_t maxp = (size_t) j->i1, np = 0;
        double *f = (double *) malloc(maxp * sizeof(double));
        double *p = (double *) malloc(maxp * sizeof(double));
        if (f == NULL || p == NULL) { free(f); free(p); fv_job_fail(b, "out of memory"); break; }
        rc = fsql_periodogram(j->a, j->na, f, p, maxp, &np);
        if (rc != 0) { free(f); free(p); core_fail(b, "fsql_periodogram", rc); break; }
        fv_sb_append(&sb, "{\"freqs\":[");
        for (size_t i = 0; i < np; i++) fv_sb_appendf(&sb, "%s%.10f", i ? "," : "", f[i]);
        fv_sb_append(&sb, "],\"power\":[");
        for (size_t i = 0; i < np; i++) fv_sb_appendf(&sb, "%s%.10g", i ? "," : "", p[i]);
        fv_sb_append(&sb, "]}");
        free(f); free(p);
        finish_json(b, &sb);
        break;
    }
    case OP_SPECT: {
        size_t maxf = (size_t) j->i3, nf = 0;
        double *e = (double *) malloc(maxf * sizeof(double));
        if (e == NULL) { fv_job_fail(b, "out of memory"); break; }
        rc = fsql_spectrogram_entropy(j->a, j->na, (size_t) j->i1, (size_t) j->i2, e, maxf, &nf);
        if (rc != 0) {
            free(e);
            fv_job_fail(b, "refused - needs window in [8,512], hop >= 1, max_frames in [1,4096], at least `window` samples (rc=%d)", rc);
            break;
        }
        fv_sb_append(&sb, "[");
        for (size_t i = 0; i < nf; i++) fv_sb_appendf(&sb, "%s%.10f", i ? "," : "", e[i]);
        fv_sb_append(&sb, "]");
        free(e);
        finish_json(b, &sb);
        break;
    }
    case OP_FPRINT: {
        size_t nbytes = ((size_t) j->i1 + 7) / 8;
        uint8_t *out = (uint8_t *) calloc(nbytes, 1);
        if (out == NULL) { fv_job_fail(b, "out of memory"); break; }
        rc = fsql_state_fingerprint(j->a, j->na, (size_t) j->i1, j->d1, out);
        if (rc != 0) { free(out); core_fail(b, "fsql_state_fingerprint", rc); break; }
        fv_job_bulk(b, (char *) out, nbytes);
        break;
    }
    case OP_CYCLE: {
        fsql_cycle_state_t cs;
        memset(&cs, 0, sizeof cs);
        rc = fsql_cycle_detect_init(&cs, (size_t) j->i1, (size_t) j->i2);
        if (rc != 0) { core_fail(b, "fsql_cycle_detect_init", rc); break; }
        size_t count = j->nraw / (size_t) j->i1;
        int detected = 0; size_t cycle_len = 0, at = 0;
        for (size_t i = 0; i < count && !detected; i++) {
            rc = fsql_cycle_detect_feed(&cs, j->raw + i * (size_t) j->i1, &detected, &cycle_len);
            if (rc != 0) break;
            if (detected) at = i;
        }
        fsql_cycle_detect_free(&cs);
        if (rc != 0) { core_fail(b, "fsql_cycle_detect_feed", rc); break; }
        if (detected) fv_sb_appendf(&sb, "{\"detected\":true,\"cycle_len\":%zu,\"at_index\":%zu}", cycle_len, at);
        else          fv_sb_append(&sb, "{\"detected\":false}");
        finish_json(b, &sb);
        break;
    }
    case OP_TDA: {
        size_t maxb = (size_t) j->i4, nbars = 0, betti1 = 0;
        fsql_tda_bar_t *bars = (fsql_tda_bar_t *) malloc(maxb * sizeof *bars);
        if (bars == NULL) { fv_job_fail(b, "out of memory"); break; }
        rc = fsql_tda_persistence_diagram(j->a, j->na / (size_t) j->i1, (size_t) j->i1,
                                          (int) j->i2, j->d1, bars, maxb, &nbars,
                                          j->i2 >= 1 ? &betti1 : NULL);
        if (rc != 0) { free(bars); core_fail(b, "fsql_tda_persistence_diagram", rc); break; }
        fv_sb_appendf(&sb, "{\"n_h0_bars\":%zu,\"h0_bars\":[", nbars);
        for (size_t i = 0; i < nbars && i < maxb; i++)
            fv_sb_appendf(&sb, "%s{\"birth\":%.10f,\"death\":%.10f}", i ? "," : "", bars[i].birth, bars[i].death);
        if (j->i2 >= 1) fv_sb_appendf(&sb, "],\"betti1\":%zu}", betti1);
        else            fv_sb_append(&sb, "],\"betti1\":null}");
        free(bars);
        finish_json(b, &sb);
        break;
    }
    case OP_PORTFOLIO: {
        size_t n = j->na;
        double *w = (double *) calloc(n, sizeof(double));
        double sharpe = 0;
        if (w == NULL) { fv_job_fail(b, "out of memory"); break; }
        rc = fsql_optimize_portfolio_ex(j->a, j->b, n, (size_t) j->i1, j->seed,
                                        (int) j->i2, (int) j->i3, w, &sharpe);
        if (rc != 0) { free(w); core_fail(b, "fsql_optimize_portfolio_ex", rc); break; }
        fv_sb_appendf(&sb, "{\"sharpe\":%.10f,\"weights\":[", sharpe);
        for (size_t i = 0; i < n; i++) fv_sb_appendf(&sb, "%s%.10f", i ? "," : "", w[i]);
        fv_sb_append(&sb, "]}");
        free(w);
        finish_json(b, &sb);
        break;
    }
    case OP_VASC: {
        double tort = 0, dens = 0, fd = 0;
        rc = fsql_vascular_network(j->a, j->na / 3, j->ix, j->b, j->nix / 2, &tort, &dens, &fd);
        if (rc != 0) { core_fail(b, "fsql_vascular_network", rc); break; }
        fv_sb_appendf(&sb, "{\"mean_tortuosity\":%.10f,\"branch_density\":%.10f,\"fractal_dimension\":%.10f}", tort, dens, fd);
        finish_json(b, &sb);
        break;
    }
    case OP_CORT: {
        double mesh = 0, hull = 0, gi = 0;
        rc = fsql_cortical_folding(j->a, j->na / 3, j->ix, j->nix / 3, &mesh, &hull, &gi);
        if (rc != 0) { core_fail(b, "fsql_cortical_folding", rc); break; }
        fv_sb_appendf(&sb, "{\"mesh_area\":%.10f,\"hull_area\":%.10f,\"gyrification_index\":%.10f}", mesh, hull, gi);
        finish_json(b, &sb);
        break;
    }
    case OP_NERVE: {
        double fld = 0, dens = 0, fd = 0;
        rc = fsql_nerve_plexus_metric(j->a, j->na / (size_t) j->i1, (size_t) j->i1, j->ix, j->nix / 2, &fld, &dens, &fd);
        if (rc != 0) { core_fail(b, "fsql_nerve_plexus_metric", rc); break; }
        fv_sb_appendf(&sb, "{\"fiber_length_density\":%.10f,\"branch_density\":%.10f,\"fractal_dimension\":%.10f}", fld, dens, fd);
        finish_json(b, &sb);
        break;
    }
    case OP_COMPLEX: {
        double dim = 0, lac = 0;
        rc = fsql_morphological_complexity(j->a, j->na / (size_t) j->i1, (size_t) j->i1, &dim, &lac);
        if (rc != 0) { core_fail(b, "fsql_morphological_complexity", rc); break; }
        fv_sb_appendf(&sb, "{\"dimension\":%.10f,\"lacunarity\":%.10f}", dim, lac);
        finish_json(b, &sb);
        break;
    }
    default:
        fv_job_fail(b, "internal error: unknown operation");
    }
    fv_sb_free(&sb);
}

/* ---------------------------------------------------------------- */
/* argument helpers                                                   */
/* ---------------------------------------------------------------- */
static acmd *
acmd_new(int op)
{
    acmd *j = (acmd *) calloc(1, sizeof *j);
    if (j == NULL) return NULL;
    j->op = op;
    j->base.work = acmd_work;
    j->base.dispose = acmd_dispose;
    return j;
}

static int
acmd_fail(RedisModuleCtx *ctx, acmd *j, const char *msg)
{
    if (j != NULL) acmd_dispose(&j->base);
    return fv_reply_err(ctx, msg);
}

static int
acmd_go(RedisModuleCtx *ctx, acmd *j)
{
    j->base.bytes = (j->na + j->nb) * sizeof(double) + (j->nix + j->nraw) * sizeof(size_t) + 4096;
    return fv_dispatch(ctx, &j->base);
}

#define NEED(argc_ok) do { if (!(argc_ok)) return RedisModule_WrongArity(ctx); } while (0)
#define NEWJOB(op) acmd *j = acmd_new(op); if (j == NULL) return fv_reply_err(ctx, "out of memory"); char err[ERRSZ]; (void) err
#define SERIES(arg) if (fv_blob_to_doubles(argv[arg], "series", FV_MAX_BLOB_BYTES, &j->a, &j->na, err, sizeof err) != 0) return acmd_fail(ctx, j, err)
#define POINTS(arg) if (fv_blob_to_doubles(argv[arg], "points", FV_MAX_BLOB_BYTES, &j->a, &j->na, err, sizeof err) != 0) return acmd_fail(ctx, j, err)
#define LL(var, arg, name, lo, hi) if (fv_arg_ll(argv[arg], name, lo, hi, &j->var, err, sizeof err) != 0) return acmd_fail(ctx, j, err)

static int
Dfa_Cmd(RedisModuleCtx *ctx, RedisModuleString **argv, int argc)
{
    NEED(argc == 2); NEWJOB(OP_DFA);
    SERIES(1);
    if (j->na < 16) return acmd_fail(ctx, j, "series needs at least 16 samples");
    return acmd_go(ctx, j);
}

static int
Drift_Cmd(RedisModuleCtx *ctx, RedisModuleString **argv, int argc)
{
    NEED(argc == 3); NEWJOB(OP_DRIFT);
    SERIES(1); LL(i1, 2, "window", 16, 1 << 24);
    if (j->na < (size_t) j->i1 + 16) return acmd_fail(ctx, j, "series needs at least window + 16 samples");
    return acmd_go(ctx, j);
}

static int
Box_Cmd(RedisModuleCtx *ctx, RedisModuleString **argv, int argc)
{
    NEED(argc == 3); NEWJOB(OP_BOX);
    POINTS(1); LL(i1, 2, "dim", 1, 1024);
    if (j->na % (size_t) j->i1 != 0) return acmd_fail(ctx, j, "points length is not a multiple of dim");
    if (j->na / (size_t) j->i1 < 8) return acmd_fail(ctx, j, "box counting needs at least 8 points");
    return acmd_go(ctx, j);
}

static int
Change_Cmd(RedisModuleCtx *ctx, RedisModuleString **argv, int argc)
{
    NEED(argc == 4 || argc == 5); NEWJOB(OP_CHANGE);
    SERIES(1); LL(i1, 2, "window", 1, 1 << 24);
    if (fv_arg_double(argv[3], "threshold", &j->d1, err, sizeof err) != 0) return acmd_fail(ctx, j, err);
    if (j->d1 <= 0) return acmd_fail(ctx, j, "threshold must be > 0");
    j->i2 = 64;
    if (argc == 5) { LL(i2, 4, "max_points", 1, 4096); }
    if (j->na < 2 * (size_t) j->i1) return acmd_fail(ctx, j, "series needs at least 2 * window samples");
    return acmd_go(ctx, j);
}

static int
Period_Cmd(RedisModuleCtx *ctx, RedisModuleString **argv, int argc)
{
    NEED(argc == 2 || argc == 3); NEWJOB(OP_PERIOD);
    SERIES(1);
    j->i1 = 8;
    if (argc == 3) { LL(i1, 2, "max_peaks", 1, 1024); }
    if (j->na < 4) return acmd_fail(ctx, j, "series needs at least 4 samples");
    return acmd_go(ctx, j);
}

static int
Spect_Cmd(RedisModuleCtx *ctx, RedisModuleString **argv, int argc)
{
    NEED(argc == 4 || argc == 5); NEWJOB(OP_SPECT);
    SERIES(1); LL(i1, 2, "window", 8, 512); LL(i2, 3, "hop", 1, 1 << 24);
    j->i3 = 64;
    if (argc == 5) { LL(i3, 4, "max_frames", 1, 4096); }
    if (j->na < (size_t) j->i1) return acmd_fail(ctx, j, "series is shorter than the window");
    return acmd_go(ctx, j);
}

static int
Fprint_Cmd(RedisModuleCtx *ctx, RedisModuleString **argv, int argc)
{
    NEED(argc == 3 || argc == 4); NEWJOB(OP_FPRINT);
    if (fv_blob_to_doubles(argv[1], "vector", FV_MAX_BLOB_BYTES, &j->a, &j->na, err, sizeof err) != 0)
        return acmd_fail(ctx, j, err);
    LL(i1, 2, "bits", 1, 4096);
    if (argc == 4 && fv_arg_double(argv[3], "seed", &j->d1, err, sizeof err) != 0) return acmd_fail(ctx, j, err);
    /* Most random draws one fingerprint may cost: about 4 ns each, so 2^28 is
     * roughly a second, and the loop cannot be interrupted. The same bound
     * the other engines apply. */
    if (j->na > 0 && (size_t) j->i1 > (size_t) FINGERPRINT_MAX_WORK / j->na)
        return acmd_fail(ctx, j, "bits x vector length must not exceed 268435456");
    return acmd_go(ctx, j);
}

static int
Cycle_Cmd(RedisModuleCtx *ctx, RedisModuleString **argv, int argc)
{
    NEED(argc == 3 || argc == 4); NEWJOB(OP_CYCLE);
    LL(i1, 2, "n_bytes", 1, 1024);
    j->i2 = 0;
    if (argc == 4) { LL(i2, 3, "hamming_threshold", 0, 8192); }
    size_t len;
    const char *p = RedisModule_StringPtrLen(argv[1], &len);
    if (len == 0 || len > FV_MAX_BLOB_BYTES) return acmd_fail(ctx, j, "fingerprints must be a non-empty blob");
    if (len % (size_t) j->i1 != 0) return acmd_fail(ctx, j, "fingerprints length is not a multiple of n_bytes");
    j->raw = (uint8_t *) malloc(len);
    if (j->raw == NULL) return acmd_fail(ctx, j, "out of memory");
    memcpy(j->raw, p, len);
    j->nraw = len;
    return acmd_go(ctx, j);
}

static int
Tda_Cmd(RedisModuleCtx *ctx, RedisModuleString **argv, int argc)
{
    NEED(argc == 5 || argc == 6); NEWJOB(OP_TDA);
    POINTS(1); LL(i1, 2, "dim", 1, 1024); LL(i2, 3, "max_dim", 0, 1);
    if (fv_arg_double(argv[4], "max_thresh", &j->d1, err, sizeof err) != 0) return acmd_fail(ctx, j, err);
    if (j->d1 <= 0) return acmd_fail(ctx, j, "max_thresh must be > 0");
    j->i4 = 1024;
    if (argc == 6) { LL(i4, 5, "max_bars", 1, 65536); }
    if (j->na % (size_t) j->i1 != 0) return acmd_fail(ctx, j, "points length is not a multiple of dim");
    size_t np = j->na / (size_t) j->i1;
    if (np < 2 || np > FSQL_TDA_MAX_POINTS) return acmd_fail(ctx, j, "needs between 2 and 512 points");
    return acmd_go(ctx, j);
}

static int
Portfolio_Cmd(RedisModuleCtx *ctx, RedisModuleString **argv, int argc)
{
    NEED(argc >= 4 && argc <= 7); NEWJOB(OP_PORTFOLIO);
    if (fv_blob_to_doubles(argv[1], "mu", FV_MAX_BLOB_BYTES, &j->a, &j->na, err, sizeof err) != 0 ||
        fv_blob_to_doubles(argv[2], "cov", FV_MAX_BLOB_BYTES, &j->b, &j->nb, err, sizeof err) != 0)
        return acmd_fail(ctx, j, err);
    if (j->na > 512) return acmd_fail(ctx, j, "at most 512 assets");
    if (j->nb != j->na * j->na) return acmd_fail(ctx, j, "cov must be n_assets x n_assets");
    LL(i1, 3, "k", 1, 512);
    if ((size_t) j->i1 > j->na) return acmd_fail(ctx, j, "k must be <= n_assets");
    if (argc >= 5) {
        long long s;
        if (fv_arg_ll(argv[4], "seed", 0, LLONG_MAX, &s, err, sizeof err) != 0) return acmd_fail(ctx, j, err);
        j->seed = (uint64_t) s;
    }
    if (argc >= 6) { LL(i2, 5, "use_obl", 0, 1); }
    if (argc == 7) {
        size_t l; const char *m = RedisModule_StringPtrLen(argv[6], &l);
        if (l == 8 && memcmp(m, "gaussian", 8) == 0) j->i3 = 0;
        else if (l == 4 && memcmp(m, "levy", 4) == 0) j->i3 = 1;
        else return acmd_fail(ctx, j, "diffusion_mode must be 'gaussian' or 'levy'");
    }
    return acmd_go(ctx, j);
}

/* Edge/face indices must name an existing node/vertex. */
static int
indices_in_range(const size_t *ix, size_t n, size_t limit)
{
    for (size_t i = 0; i < n; i++) if (ix[i] >= limit) return 0;
    return 1;
}

static int
Vasc_Cmd(RedisModuleCtx *ctx, RedisModuleString **argv, int argc)
{
    NEED(argc == 4); NEWJOB(OP_VASC);
    if (fv_blob_to_doubles(argv[1], "coords", FV_MAX_BLOB_BYTES, &j->a, &j->na, err, sizeof err) != 0 ||
        fv_blob_to_indices(argv[2], "edges", FV_MAX_BLOB_BYTES, &j->ix, &j->nix, err, sizeof err) != 0 ||
        fv_blob_to_doubles(argv[3], "arc_lengths", FV_MAX_BLOB_BYTES, &j->b, &j->nb, err, sizeof err) != 0)
        return acmd_fail(ctx, j, err);
    if (j->na % 3 != 0) return acmd_fail(ctx, j, "coords length is not a multiple of 3");
    if (j->nix % 2 != 0) return acmd_fail(ctx, j, "edges length is not a multiple of 2");
    if (j->nb != j->nix / 2) return acmd_fail(ctx, j, "need one arc length per edge");
    if (!indices_in_range(j->ix, j->nix, j->na / 3)) return acmd_fail(ctx, j, "an edge names a node that does not exist");
    return acmd_go(ctx, j);
}

static int
Cort_Cmd(RedisModuleCtx *ctx, RedisModuleString **argv, int argc)
{
    NEED(argc == 3); NEWJOB(OP_CORT);
    if (fv_blob_to_doubles(argv[1], "vertices", FV_MAX_BLOB_BYTES, &j->a, &j->na, err, sizeof err) != 0 ||
        fv_blob_to_indices(argv[2], "faces", FV_MAX_BLOB_BYTES, &j->ix, &j->nix, err, sizeof err) != 0)
        return acmd_fail(ctx, j, err);
    if (j->na % 3 != 0) return acmd_fail(ctx, j, "vertices length is not a multiple of 3");
    if (j->nix % 3 != 0) return acmd_fail(ctx, j, "faces length is not a multiple of 3");
    if (!indices_in_range(j->ix, j->nix, j->na / 3)) return acmd_fail(ctx, j, "a face names a vertex that does not exist");
    return acmd_go(ctx, j);
}

static int
Nerve_Cmd(RedisModuleCtx *ctx, RedisModuleString **argv, int argc)
{
    NEED(argc == 4); NEWJOB(OP_NERVE);
    if (fv_blob_to_doubles(argv[1], "coords", FV_MAX_BLOB_BYTES, &j->a, &j->na, err, sizeof err) != 0)
        return acmd_fail(ctx, j, err);
    LL(i1, 2, "dim", 1, 1024);
    if (fv_blob_to_indices(argv[3], "edges", FV_MAX_BLOB_BYTES, &j->ix, &j->nix, err, sizeof err) != 0)
        return acmd_fail(ctx, j, err);
    if (j->na % (size_t) j->i1 != 0) return acmd_fail(ctx, j, "coords length is not a multiple of dim");
    if (j->nix % 2 != 0) return acmd_fail(ctx, j, "edges length is not a multiple of 2");
    if (!indices_in_range(j->ix, j->nix, j->na / (size_t) j->i1)) return acmd_fail(ctx, j, "an edge names a node that does not exist");
    return acmd_go(ctx, j);
}

static int
Complex_Cmd(RedisModuleCtx *ctx, RedisModuleString **argv, int argc)
{
    NEED(argc == 3); NEWJOB(OP_COMPLEX);
    POINTS(1); LL(i1, 2, "dim", 1, 1024);
    if (j->na % (size_t) j->i1 != 0) return acmd_fail(ctx, j, "points length is not a multiple of dim");
    if (j->na / (size_t) j->i1 < 8) return acmd_fail(ctx, j, "needs at least 8 points");
    return acmd_go(ctx, j);
}

int
fv_register_analytics(RedisModuleCtx *ctx)
{
    static const struct { const char *name; RedisModuleCmdFunc fn; } T[] = {
        { "FRACTAL.DIM.DFA",            Dfa_Cmd },
        { "FRACTAL.DIM.DRIFT",          Drift_Cmd },
        { "FRACTAL.DIM.BOXCOUNT",       Box_Cmd },
        { "FRACTAL.SERIES.CHANGEPOINT", Change_Cmd },
        { "FRACTAL.SERIES.PERIODOGRAM", Period_Cmd },
        { "FRACTAL.SERIES.SPECTROGRAM", Spect_Cmd },
        { "FRACTAL.SERIES.FINGERPRINT", Fprint_Cmd },
        { "FRACTAL.CYCLE.DETECT",       Cycle_Cmd },
        { "FRACTAL.TDA.PERSISTENCE",    Tda_Cmd },
        { "FRACTAL.OPTIMIZE.PORTFOLIO", Portfolio_Cmd },
        { "FRACTAL.MORPH.VASCULAR",     Vasc_Cmd },
        { "FRACTAL.MORPH.CORTICAL",     Cort_Cmd },
        { "FRACTAL.MORPH.NERVE",        Nerve_Cmd },
        { "FRACTAL.MORPH.COMPLEXITY",   Complex_Cmd },
    };
    for (size_t i = 0; i < sizeof T / sizeof T[0]; i++)
        if (RedisModule_CreateCommand(ctx, T[i].name, T[i].fn, "readonly allow-stale", 0, 0, 0)
                == REDISMODULE_ERR)
            return REDISMODULE_ERR;
        else if (fv_acl(ctx, T[i].name, "read slow") != 0)
            return REDISMODULE_ERR;
    return REDISMODULE_OK;
}
