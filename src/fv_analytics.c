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
 *   FRACTAL.TEXT.CHUNK         text size overlap [mode]       -> JSON array of {no,start,end,text}
 *
 * Index arrays (edges, faces) are packed little-endian uint32.
 */
#include "fv_common.h"

#include <limits.h>
#include <stdio.h>
#include <string.h>

#define ERRSZ 256
/* Most random draws one fingerprint (bits x vector length) may cost: about
 * 4 ns each, so 2^28 is roughly a second, and the loop cannot be
 * interrupted. The same bound the other engines apply. */
#define FINGERPRINT_MAX_WORK 268435456ULL

enum {
    OP_DFA, OP_DRIFT, OP_BOX, OP_CHANGE, OP_PERIOD, OP_SPECT, OP_FPRINT,
    OP_CYCLE, OP_TDA, OP_PORTFOLIO, OP_VASC, OP_CORT, OP_NERVE, OP_COMPLEX, OP_SUBSET
};

typedef struct {
    fv_job   base;                    /* first */
    int      op;
    double  *a;  size_t na;           /* primary series / points / mu / coords */
    double  *b;  size_t nb;           /* cov / arc lengths / vertices / upper bounds */
    double  *c;  size_t nc;           /* previous weights (OPTIMIZE.SUBSET) */
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
    free(j->a); free(j->b); free(j->c); free(j->ix); free(j->raw);
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

/* The objective FRACTAL.OPTIMIZE.SUBSET maximises: sum(weight[i] * item_value[i]). The core's search minimises, so the objective is the
 * negated sum (the reported score is the positive sum). */
static double
subset_value_objective(const double *weights, size_t n, void *ctx)
{
    const double *item_values = (const double *) ctx;
    double sum = 0.0;
    for (size_t i = 0; i < n; i++) sum += weights[i] * item_values[i];
    return -sum;
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
    case OP_SUBSET: {
        size_t n = j->na;
        double *w = (double *) calloc(n, sizeof(double));
        double score = 0;
        if (w == NULL) { fv_job_fail(b, "out of memory"); break; }
        rc = fsql_optimize_subset(subset_value_objective, j->a, n, (size_t) j->i1, NULL, j->b, j->c, j->d1, j->seed, w, &score);
        if (rc != 0) { free(w); core_fail(b, "fsql_optimize_subset", rc); break; }
        fv_sb_appendf(&sb, "{\"score\":%.10f,\"weights\":[", -score);
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

/* FRACTAL.OPTIMIZE.SUBSET item_values upper_bounds k [PREV prev_weights] [TURNOVER penalty] [SEED n]
 * Value-weighted k-subset allocation: maximise sum(weight[i] * item_values[i]) with at most k of the items weighted, each weight at most its
 * upper bound (0 to 1), the weights summing to 1. prev_weights and turnover_penalty steer the search away from reallocating (rebalancing). */
static int
Subset_Cmd(RedisModuleCtx *ctx, RedisModuleString **argv, int argc)
{
    NEED(argc >= 4 && argc <= 10); NEWJOB(OP_SUBSET);
    if (fv_blob_to_doubles(argv[1], "item_values", FV_MAX_BLOB_BYTES, &j->a, &j->na, err, sizeof err) != 0 ||
        fv_blob_to_doubles(argv[2], "upper_bounds", FV_MAX_BLOB_BYTES, &j->b, &j->nb, err, sizeof err) != 0)
        return acmd_fail(ctx, j, err);
    if (j->na == 0 || j->na > 100000) return acmd_fail(ctx, j, "item_values must hold 1 to 100000 items");
    if (j->nb != j->na) return acmd_fail(ctx, j, "upper_bounds length must match item_values");
    LL(i1, 3, "k", 1, 100000);
    if ((size_t) j->i1 > j->na) return acmd_fail(ctx, j, "k must be <= the number of items");
    if ((unsigned long long) j->na * (unsigned long long) j->i1 > 16777216ULL)
        return acmd_fail(ctx, j, "items x k must not exceed 16777216 (about a minute of work)");
    for (size_t i = 0; i < j->nb; i++)
        if (!(j->b[i] >= 0.0 && j->b[i] <= 1.0)) return acmd_fail(ctx, j, "every upper_bounds entry must be in [0, 1]");
    for (int i = 4; i < argc; i++) {
        size_t l; const char *w = RedisModule_StringPtrLen(argv[i], &l);
        if (i + 1 >= argc) { snprintf(err, sizeof err, "%.32s needs a value", w); return acmd_fail(ctx, j, err); }
        if (l == 4 && strncasecmp(w, "PREV", 4) == 0) {
            if (fv_blob_to_doubles(argv[++i], "prev_weights", FV_MAX_BLOB_BYTES, &j->c, &j->nc, err, sizeof err) != 0) return acmd_fail(ctx, j, err);
            if (j->nc != j->na) return acmd_fail(ctx, j, "prev_weights length must match item_values");
        } else if (l == 8 && strncasecmp(w, "TURNOVER", 8) == 0) {
            if (fv_arg_double(argv[++i], "turnover_penalty", &j->d1, err, sizeof err) != 0) return acmd_fail(ctx, j, err);
            if (!(j->d1 >= 0.0)) return acmd_fail(ctx, j, "turnover_penalty must be >= 0");
        } else if (l == 4 && strncasecmp(w, "SEED", 4) == 0) {
            long long s;
            if (fv_arg_ll(argv[++i], "seed", 0, LLONG_MAX, &s, err, sizeof err) != 0) return acmd_fail(ctx, j, err);
            j->seed = (uint64_t) s;
        } else {
            snprintf(err, sizeof err, "unknown option '%.32s'", w);
            return acmd_fail(ctx, j, err);
        }
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


/* ------------------------------------------------------------------ fractal chunk text */
#define FV_CHUNK_MAX_SIZE   100000
#define FV_CHUNK_MAX_CHUNKS 100000
#define FV_CHUNK_MAX_TEXT   (4u << 20)
#define FV_CHUNK_MAX_OUT    ((size_t) 64 * 1024 * 1024)   /* the whole JSON result: size x chunks would otherwise reach gigabytes */
/* ------------------------------------------------------------------ chunking */

typedef struct { char *b; size_t n, cap; int oom; int big; } fvck_ChunkBuf;

static void fvck_cb_put(fvck_ChunkBuf *o, const char *s, size_t len)
{
    if (o->oom || o->big) return;
    if (len > FV_CHUNK_MAX_OUT || o->n > FV_CHUNK_MAX_OUT - len) { o->big = 1; return; }
    if (o->n + len + 1 > o->cap) {
        size_t nc = o->cap ? o->cap : 256;
        char *nb;
        while (nc < o->n + len + 1) nc *= 2;
        nb = (char *) realloc(o->b, nc);
        if (!nb) { o->oom = 1; return; }
        o->b = nb; o->cap = nc;
    }
    memcpy(o->b + o->n, s, len); o->n += len; o->b[o->n] = '\0';
}

typedef struct { size_t *cp; size_t n; const unsigned char *t; size_t blen; } fvck_ChunkCps;

static int fvck_cps_build(fvck_ChunkCps *c, const unsigned char *t, size_t blen)
{
    size_t i = 0;
    c->t = t; c->blen = blen; c->n = 0;
    c->cp = (size_t *) malloc((blen + 1) * sizeof(size_t));
    if (!c->cp) return -1;
    while (i < blen) {
        unsigned char b = t[i];
        size_t len = b < 0x80 ? 1 : (b >= 0xF0 && b <= 0xF4) ? 4 : (b >= 0xE0) ? 3 : (b >= 0xC2) ? 2 : 1, k = 1;
        c->cp[c->n++] = i;
        if (b >= 0xF5) len = 1;
        while (k < len && i + k < blen && (t[i + k] & 0xC0) == 0x80) k++;
        i += (k == len) ? len : 1;       /* a truncated or invalid sequence is one character per byte */
    }
    c->cp[c->n] = blen;
    return 0;
}

static int fvck_chunk_is_ws(unsigned char b) { return b == ' ' || b == '\t' || b == '\r' || b == '\n'; }

/* the character at index i as a single byte when it is ASCII, else 0xFF */
static unsigned char fvck_cps_ascii(const fvck_ChunkCps *c, size_t i)
{
    size_t b = c->cp[i];
    return c->t[b] < 0x80 ? c->t[b] : 0xFF;
}

static void fvck_chunk_emit(fvck_ChunkBuf *o, const fvck_ChunkCps *c, size_t no, size_t a, size_t b)
{
    char tmp[96];
    int l = snprintf(tmp, sizeof tmp, "%s{\"no\":%lld,\"start\":%lld,\"end\":%lld,\"text\":\"", no ? "," : "", (long long) no, (long long) a, (long long) b);
    fvck_cb_put(o, tmp, (size_t) l);
    for (size_t i = c->cp[a]; i < c->cp[b]; i++) {
        unsigned char ch = c->t[i];
        if (ch >= 0x80) {
            /* a byte that does not belong to a well-formed UTF-8 character is written as U+FFFD, so the JSON stays valid UTF-8 */
            size_t need = ch >= 0xF0 ? 4 : ch >= 0xE0 ? 3 : 2, ln = 1;
            while (i + ln < c->cp[b] && (c->t[i + ln] & 0xC0) == 0x80 && ln < need) ln++;
            int okc = 0;
            if (ch >= 0xC2 && ch <= 0xDF) okc = ln == 2;
            else if (ch >= 0xE0 && ch <= 0xEF) okc = ln == 3 && !(ch == 0xE0 && c->t[i + 1] < 0xA0) && !(ch == 0xED && c->t[i + 1] >= 0xA0);
            else if (ch >= 0xF0 && ch <= 0xF4) okc = ln == 4 && !(ch == 0xF0 && c->t[i + 1] < 0x90) && !(ch == 0xF4 && c->t[i + 1] >= 0x90);
            if (!okc) { fvck_cb_put(o, "\\ufffd", 6); continue; }
            fvck_cb_put(o, (const char *) &c->t[i], ln);
            i += ln - 1;
            continue;
        }
        if (ch == '"') fvck_cb_put(o, "\\\"", 2);
        else if (ch == '\\') fvck_cb_put(o, "\\\\", 2);
        else if (ch == '\n') fvck_cb_put(o, "\\n", 2);
        else if (ch == '\r') fvck_cb_put(o, "\\r", 2);
        else if (ch == '\t') fvck_cb_put(o, "\\t", 2);
        else if (ch < 0x20) { l = snprintf(tmp, sizeof tmp, "\\u%04x", ch); fvck_cb_put(o, tmp, (size_t) l); }
        else fvck_cb_put(o, (const char *) &c->t[i], 1);   /* a raw byte: UTF-8 passes through unchanged */
    }
    fvck_cb_put(o, "\"}", 2);
}

/* the span [a,b) split into pieces of at most size characters, overlap shared; the new chunk count, or (size_t)-1 over the cap */
static size_t fvck_chunk_split(fvck_ChunkBuf *o, const fvck_ChunkCps *c, size_t no, size_t a, size_t b, size_t size, size_t overlap)
{
    size_t pos = a;
    for (;;) {
        size_t e = pos + size < b ? pos + size : b;
        if (no >= FV_CHUNK_MAX_CHUNKS || o->big) return (size_t) -1;
        fvck_chunk_emit(o, c, no++, pos, e);
        if (e >= b) break;
        pos = e - overlap;
    }
    return no;
}

int fv_chunk_text(const unsigned char *t, size_t blen, long size, long overlap, int mode, char **out)
{
    fvck_ChunkCps c;
    fvck_ChunkBuf o = { NULL, 0, 0, 0, 0 };
    size_t no = 0, S = (size_t) size, V = (size_t) overlap, n;
    int over = 0;
    *out = NULL;
    if (fvck_cps_build(&c, t, blen) != 0) return -1;
    n = c.n;
    fvck_cb_put(&o, "[", 1);
    if (mode == 0) {
        if (n > 0) { no = fvck_chunk_split(&o, &c, 0, 0, n, S, V); over = no == (size_t) -1; }
    } else {
        size_t cur_a = 0, cur_b = 0, i = 0;
        int have = 0;
        while (i < n && !over) {
            size_t ua, ub;
            while (i < n && fvck_chunk_is_ws(fvck_cps_ascii(&c, i))) i++;
            if (i >= n) break;
            ua = i;
            for (;;) {                       /* extend to the end of the unit */
                unsigned char ch;
                if (i >= n) { ub = n; break; }
                ch = fvck_cps_ascii(&c, i);
                if (fvck_chunk_is_ws(ch)) {       /* whitespace: a paragraph break, or (sentence mode) just a separator? */
                    size_t j = i; int lf = 0;
                    while (j < n && fvck_chunk_is_ws(fvck_cps_ascii(&c, j))) { if (fvck_cps_ascii(&c, j) == '\n') lf++; j++; }
                    if (lf >= 2) { ub = i; i = j; break; }
                    i = j;
                    continue;
                }
                i++;
                if (mode == 2 && (ch == '.' || ch == '!' || ch == '?')) {
                    while (i < n && (fvck_cps_ascii(&c, i) == '.' || fvck_cps_ascii(&c, i) == '!' || fvck_cps_ascii(&c, i) == '?')) i++;
                    if (i >= n || fvck_chunk_is_ws(fvck_cps_ascii(&c, i))) { ub = i; break; }
                }
            }
            while (ub > ua && fvck_chunk_is_ws(fvck_cps_ascii(&c, ub - 1))) ub--;
            if (ub == ua) continue;
            if (ub - ua > S) {               /* longer than a chunk: flush, then split it */
                if (have) { fvck_chunk_emit(&o, &c, no++, cur_a, cur_b); have = 0; }
                no = fvck_chunk_split(&o, &c, no, ua, ub, S, V);
                if (no == (size_t) -1) over = 1;
            } else if (!have) { cur_a = ua; cur_b = ub; have = 1; }
            else if (ub - cur_a <= S) { cur_b = ub; }
            else { fvck_chunk_emit(&o, &c, no++, cur_a, cur_b); cur_a = ua; cur_b = ub; }
            if (!over && (no >= FV_CHUNK_MAX_CHUNKS || o.big)) over = 1;
        }
        if (have && !over) fvck_chunk_emit(&o, &c, no++, cur_a, cur_b);
    }
    free(c.cp);
    fvck_cb_put(&o, "]", 1);
    if (o.oom) { free(o.b); return -1; }
    if (over || o.big) { free(o.b); return 1; }
    *out = o.b;
    return 0;
}

/* FRACTAL.TEXT.CHUNK text size overlap [mode] -> JSON array of {no, start, end, text}. Pure: no model, the same output on every FractalSQL host. */
static int
Chunk_Cmd(RedisModuleCtx *ctx, RedisModuleString **argv, int argc)
{
    if (argc < 4 || argc > 5) return RedisModule_WrongArity(ctx);
    long long size = 0, overlap = 0;
    int mode = 1;
    size_t tl, ml;
    const char *t = RedisModule_StringPtrLen(argv[1], &tl);
    if (RedisModule_StringToLongLong(argv[2], &size) != REDISMODULE_OK || size < 1 || size > FV_CHUNK_MAX_SIZE)
        return fv_reply_err(ctx, "size must be an integer in [1, 100000]");
    if (RedisModule_StringToLongLong(argv[3], &overlap) != REDISMODULE_OK || overlap < 0 || overlap >= size)
        return fv_reply_err(ctx, "overlap must be an integer in [0, size-1]");
    if (argc == 5) {
        const char *m = RedisModule_StringPtrLen(argv[4], &ml);
        if (ml == 5 && memcmp(m, "fixed", 5) == 0) mode = 0;
        else if (ml == 9 && memcmp(m, "paragraph", 9) == 0) mode = 1;
        else if (ml == 8 && memcmp(m, "sentence", 8) == 0) mode = 2;
        else return fv_reply_err(ctx, "mode must be one of: fixed, paragraph, sentence");
    }
    if (tl > FV_CHUNK_MAX_TEXT) return fv_reply_err(ctx, "text must be at most 4194304 bytes");
    char *out = NULL;
    int rc = fv_chunk_text((const unsigned char *) t, tl, (long) size, (long) overlap, mode, &out);
    if (rc == 1) return fv_reply_err(ctx, "more than 100000 chunks or 64 MiB of output (raise size or lower overlap)");
    if (rc != 0 || out == NULL) return fv_reply_err(ctx, "out of memory");
    RedisModule_ReplyWithStringBuffer(ctx, out, strlen(out));
    free(out);
    return REDISMODULE_OK;
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
        { "FRACTAL.OPTIMIZE.SUBSET",    Subset_Cmd },
        { "FRACTAL.MORPH.VASCULAR",     Vasc_Cmd },
        { "FRACTAL.MORPH.CORTICAL",     Cort_Cmd },
        { "FRACTAL.MORPH.NERVE",        Nerve_Cmd },
        { "FRACTAL.MORPH.COMPLEXITY",   Complex_Cmd },
        { "FRACTAL.TEXT.CHUNK",         Chunk_Cmd },
    };
    for (size_t i = 0; i < sizeof T / sizeof T[0]; i++)
        if (RedisModule_CreateCommand(ctx, T[i].name, T[i].fn, "readonly allow-stale", 0, 0, 0)
                == REDISMODULE_ERR)
            return REDISMODULE_ERR;
        else if (fv_acl(ctx, T[i].name, "read slow") != 0)
            return REDISMODULE_ERR;
    return REDISMODULE_OK;
}
