/* src/fractalsql_textsql.c
 *
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: 2026 Daniel Gardiner d/b/a FractalSQLabs
 *
 * The model "generate" tier: fractal_t2s_generate, the daemon call behind
 * FRACTAL.AGENT.DATA_ANALYST's question-to-query step. It sends a prompt and
 * a context to the reasoning provider in code mode (the reply is the single
 * fenced block the model returned, with the caller's system tag) and hands
 * the text back. It never retries. What the agent does with the generated
 * query, and the validation of it, are the agent's.
 *
 * Valkey has no SQL: the SQL allowlist, the review pass and the text-to-SQL
 * configuration keys that an SQL engine's version of this file carries are not
 * part of it. The function keeps its registry name (the daemon protocol names
 * it) and its provider settings, which are the reasoning provider's: the
 * plugin, URL, token, model, think and context-size keys, pushed in by the
 * daemon from fractalsqld.conf (fractalsql_provider.h), with the process
 * environment as the boot fallback, read once.
 */

#include "fsql_udf_types.h"

#include <ctype.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "fractalsql.h"          /* FSQL_OK, fsql_last_error */
#include "fractalsql_sql.h"      /* fsql_ctx, fsql_dispatch_ai, fsql_load_reasoning */
#include "fractalsql_session.h"  /* fractal_session_acquire_reason/_t2s, ... */
#include "fractalsql_provider.h"     /* fractalsql_textsql_{check,apply}_provider */
#include "fractalsql_config.h"       /* the per-session configuration overlay */
#include "fv_plugin_verify.h"        /* the reasoning plugin's authenticity gate */

#  include <pthread.h>
#  define FRACTAL_EXPORT

/* Same stderr-mirroring SFS_INIT_ERROR as fractalsql_cognition.c
 * (duplicated per translation unit by this repo's established
 * precedent): on the UDF runtime path the formatted buffer would
 * otherwise be dropped, surfacing failures as bare NULLs. */
#define SFS_INIT_ERROR(msg, ...)                                              \
    do {                                                                      \
        snprintf((msg), FSQL_ERRMSG_SIZE, __VA_ARGS__);                      \
        fprintf(stderr, "fractalsql: %s\n", (msg));                           \
    } while (0)

/* The reasoning plugin's own diagnostic string is UNBOUNDED: a failed
 * HTTP round trip can echo a whole response body back into it. The
 * error buffer is FSQL_ERRMSG_SIZE (512) bytes, so a plain snprintf
 * cuts the message at an arbitrary byte with nothing to say so, and the
 * cut can land mid-UTF-8 or mid-URL -- a half line in the log reads
 * as a whole one. The unbounded operand is therefore shortened HERE,
 * with an explicit marker, before the fixed text is formatted around
 * it. Same rule as fractalsql_cognition.c's sfs_detail_bounded, which
 * this file's copy follows (duplicated per translation unit by this
 * repo's established precedent). */
#define T2S_DETAIL_MAX 256
#define T2S_DETAIL_MARK "...[truncated]"

/* Copies `detail` into `out` (capacity `cap`), cut at a UTF-8 scalar
 * boundary and marked when it does not fit. */
static void
t2s_detail_bounded(const char *detail, char *out, size_t cap)
{
    size_t len = strlen(detail);
    if (len + 1 <= cap) { memcpy(out, detail, len + 1); return; }
    {
        size_t mark = sizeof T2S_DETAIL_MARK - 1;
        size_t keep = cap - 1 - mark;      /* room for the marker + NUL */
        if (keep > len) keep = len;
        /* Never cut inside a UTF-8 sequence. */
        while (keep > 0 && ((unsigned char) detail[keep] & 0xC0) == 0x80)
            keep--;
        memcpy(out, detail, keep);
        memcpy(out + keep, T2S_DETAIL_MARK, mark + 1);
    }
}

/* Same DoS-guard reasoning as fractalsql_cognition.c's
 * FRACTAL_COGNITION_MAX_INPUT_BYTES: caller-controlled text drives
 * both an allocation and an outbound HTTP request body. */
#define FRACTAL_T2S_MAX_INPUT_BYTES ((unsigned long) 4u * 1024u * 1024u) /* 4 MiB */
#define FRACTAL_T2S_MAX_SQL_BYTES   ((unsigned long) 256u * 1024u)       /* 256 KiB, sized for a single generated statement */

/* Same bound/rationale as fractalsql_cognition.c's
 * FRACTAL_MAX_AI_RESPONSE_BYTES. */
#define FRACTAL_MAX_AI_RESPONSE_BYTES ((size_t) 16u * 1024u * 1024u) /* 16 MiB */

/* ------------------------------------------------------------------ */
/* Growable output buffer. Duplicated per translation unit, following  */
/* this repo's established precedent (see fractalsql_cognition.c's    */
/* identical comment on str_out_ctx).                                  */
/* ------------------------------------------------------------------ */
typedef struct str_out_ctx {
    char  *buf;
    size_t cap;
} str_out_ctx;

static bool
str_out_ensure(str_out_ctx *so, size_t need)
{
    size_t ncap;
    char  *nb;
    if (need <= so->cap) return true;
    ncap = so->cap ? so->cap : 256;
    while (ncap < need) ncap *= 2;
    nb = realloc(so->buf, ncap);
    if (nb == NULL) return false;
    so->buf = nb; so->cap = ncap;
    return true;
}

static bool
str_out_generic_init(UDF_INIT *initid, char *message, unsigned long max_length)
{
    str_out_ctx *so = calloc(1, sizeof(*so));
    if (so == NULL) {
        SFS_INIT_ERROR(message, "fractal_t2s: out of memory");
        return true;
    }
    initid->ptr        = (char *) so;
    initid->maybe_null = 1;
    initid->max_length = max_length;
    return false;
}

static void
str_out_generic_deinit(UDF_INIT *initid)
{
    str_out_ctx *so = (str_out_ctx *) initid->ptr;
    if (so == NULL) return;
    free(so->buf);
    free(so);
    initid->ptr = NULL;
}

static char *
str_out_set(str_out_ctx *so, const char *text, size_t len, unsigned long *out_len)
{
    if (!str_out_ensure(so, len + 1)) return NULL;
    memcpy(so->buf, text, len);
    so->buf[len] = '\0';
    *out_len = (unsigned long) len;
    return so->buf;
}

/* ------------------------------------------------------------------ */
/* Portable lock and once-init shim. See fractalsql_session.c's        */
/* identical comment; duplicated per translation unit by this repo's   */
/* established precedent rather than shared.                           */
/* ------------------------------------------------------------------ */
static pthread_mutex_t g_load_lock = PTHREAD_MUTEX_INITIALIZER;
static void load_lock(void)   { pthread_mutex_lock(&g_load_lock); }
static void load_unlock(void) { pthread_mutex_unlock(&g_load_lock); }

/* ------------------------------------------------------------------ */
/* Provider config. Per key, in priority order (fractalsql_provider.h */
/* for the rule): (1) fractalsqld.conf -- the daemon pushes values in */
/* via fractalsql_textsql_apply_provider(), under our load lock;      */
/* (2) the process environment -- the boot fallback, captured once;   */
/* (3) unset/default.                                                 */
/*                                                                    */
/* Unlike fractalsql_cognition.c, the environment fallback here reads */
/* only the LEGACY FRACTALSQL_* names (as it always did). Reasoning   */
/* plugin path/HTTP config is shared with fractalsql_cognition.c's    */
/* vars: same plugin, same endpoint, just a third dispatch purpose.   */
/* Re-reading the environment here, rather than sharing it across     */
/* translation units, matches this repo's convention of each source   */
/* file owning its own config (see that file's own loader). With the  */
/* daemon pushing conf-file values through, both tiers now get the    */
/* same file-stored settings even though their env fallbacks read the */
/* two different name families.                                       */
/*                                                                    */
/* g_env_fb is the boot-env fallback snapshot: owned strings, captured */
/* once, never freed. g_cfg holds the EFFECTIVE values -- always its  */
/* own owned copies, never aliases of g_env_fb's, so apply's           */
/* free-and-replace can never dangle. Until the first provider apply, */
/* g_cfg is initialized once from the snapshot, byte-identical to the */
/* old env-only behavior.                                             */
/* ------------------------------------------------------------------ */
typedef struct {
    char *reasoning_plugin;
    char *http_url;
    char *http_token;
    char *http_model;
    bool  http_allow_plaintext;
    char *http_think;
    char *http_think_provider;
    char *http_native_url;
    char *http_num_ctx;
} t2s_config;

static t2s_config g_cfg;     /* effective values */
static t2s_config g_env_fb;  /* boot-env fallback snapshot */
static bool g_env_captured;  /* g_env_fb filled (under load lock) */
static bool g_cfg_initialized; /* g_cfg filled from the snapshot (until apply) */
static bool g_provider_applied; /* g_cfg last written by apply_provider */

/* The fingerprint of the bridge environment each tier's plugin instance
 * was last loaded with (see generate_env_and_reload). A changed
 * provider invalidates it: the fingerprint describes the ENVIRONMENT,
 * and a provider reload can change the plugin PATH -- the thing the
 * instance was loaded from -- without changing the environment. */
static uint64_t g_t2s_fp = 0;
static bool     g_t2s_fp_valid = false;
/* Owned copy of an environment variable: 0 with *out set to the
 * value (NULL when the variable is unset or empty), or -1 when the
 * copy failed. The distinction is the point. A NULL that means "the
 * operator set nothing" and a NULL that means "the copy ran out of
 * memory" are different facts, and conflating them turns an
 * allocation failure into "not configured" -- which, in the boot
 * snapshot below, would be latched for the life of the process. */
static int
dup_env_into(const char *name, char **out)
{
    const char *v = getenv(name);
    *out = NULL;
    if (v == NULL || *v == '\0') return 0;
    *out = strdup(v);
    return *out ? 0 : -1;
}

/* Preferred name first, then the legacy FRACTALSQL_HTTP_* name. The
 * preferred names match the reasoning plugin's own FSQL_REASONING_HTTP_*
 * variable spellings, so the same configured names reach the plugin as
 * is. Legacy names keep existing installs working. */
static int
dup_env_pref_into(const char *preferred, const char *legacy, char **out)
{
    const char *v = getenv(preferred);
    *out = NULL;
    if (v && *v) {
        *out = strdup(v);
        return *out ? 0 : -1;
    }
    return dup_env_into(legacy, out);
}

/* strdup `s` (NULL stays NULL) into *out; 0, or -1 on allocation
 * failure. */
static int
dup_or(const char *s, char **out)
{
    if (!s) { *out = NULL; return 0; }
    *out = strdup(s);
    return *out ? 0 : -1;
}

static void
free_t2s_cfg(t2s_config *c)
{
    free(c->reasoning_plugin);
    free(c->http_url);
    free(c->http_token);
    free(c->http_model);
    free(c->http_think);
    free(c->http_think_provider);
    free(c->http_native_url);
    free(c->http_num_ctx);
    memset(c, 0, sizeof *c);
}

/* Owned duplicate of every `from` string into `to` (fixed-size fields
 * copied as-is). 0, or -1 with `to` fully freed and zeroed on failure. */
static int
dup_t2s_cfg(const t2s_config *from, t2s_config *to)
{
    int rc = 0;
    rc |= dup_or(from->reasoning_plugin,    &to->reasoning_plugin);
    rc |= dup_or(from->http_url,            &to->http_url);
    rc |= dup_or(from->http_token,          &to->http_token);
    rc |= dup_or(from->http_model,          &to->http_model);
    rc |= dup_or(from->http_think,          &to->http_think);
    rc |= dup_or(from->http_think_provider, &to->http_think_provider);
    rc |= dup_or(from->http_native_url,     &to->http_native_url);
    rc |= dup_or(from->http_num_ctx,        &to->http_num_ctx);
    to->http_allow_plaintext = from->http_allow_plaintext;
    if (rc != 0) {
        free_t2s_cfg(to);
        return -1;
    }
    return 0;
}

/* Called with the load lock held. Fills the boot-env fallback snapshot
 * from the process environment exactly once. A copy that FAILS leaves
 * the snapshot incomplete and g_env_captured false, so the next call
 * retries the capture instead of latching an allocation failure as a
 * permanent "the operator never set this". */
static bool
capture_t2s_env_fb(void)
{
    const char *plain;
    int rc = 0;
    if (g_env_captured) return true;

    rc |= dup_env_into("FRACTALSQL_REASONING_PLUGIN", &g_env_fb.reasoning_plugin);
    rc |= dup_env_pref_into("FSQL_REASONING_HTTP_URL", "FRACTALSQL_HTTP_URL", &g_env_fb.http_url);
    rc |= dup_env_pref_into("FSQL_REASONING_HTTP_TOKEN", "FRACTALSQL_HTTP_TOKEN", &g_env_fb.http_token);
    rc |= dup_env_pref_into("FSQL_REASONING_HTTP_MODEL", "FRACTALSQL_HTTP_MODEL", &g_env_fb.http_model);
    plain = getenv("FSQL_REASONING_HTTP_ALLOW_PLAINTEXT");
    if (!plain || !*plain) plain = getenv("FRACTALSQL_HTTP_ALLOW_PLAINTEXT");
    g_env_fb.http_allow_plaintext = (plain && *plain && strcmp(plain, "0") != 0);
    rc |= dup_env_pref_into("FSQL_REASONING_HTTP_THINK", "FRACTALSQL_HTTP_THINK", &g_env_fb.http_think);
    rc |= dup_env_pref_into("FSQL_REASONING_HTTP_THINK_PROVIDER", "FRACTALSQL_HTTP_THINK_PROVIDER", &g_env_fb.http_think_provider);
    rc |= dup_env_pref_into("FSQL_REASONING_HTTP_NATIVE_URL", "FRACTALSQL_HTTP_NATIVE_URL", &g_env_fb.http_native_url);
    rc |= dup_env_pref_into("FSQL_REASONING_HTTP_NUM_CTX", "FRACTALSQL_HTTP_NUM_CTX", &g_env_fb.http_num_ctx);

    if (rc != 0) {
        /* Drop the partial snapshot: the next capture starts from
         * scratch, and nothing half-filled is ever published. */
        free_t2s_cfg(&g_env_fb);
        return false;
    }
    g_env_captured = true;
    return true;
}

/* Fills g_cfg from the boot-env snapshot the first time it is needed.
 * Returns false only when the environment could not be read at all
 * (an allocation failure): the caller reports that as the out-of-
 * memory it is, rather than as missing configuration. */
static bool
ensure_env_config(char *errbuf)
{
    bool ok;
    /* Under the load lock: the snapshot capture would race a concurrent
     * apply_provider, and the copies below assume both are settled. */
    load_lock();
    ok = capture_t2s_env_fb();
    if (ok && !g_provider_applied && !g_cfg_initialized) {
        /* Byte-identical to the old env-only first read. On OOM g_cfg
         * stays zeroed and the dispatch path reports missing config;
         * the next call retries this copy. */
        if (dup_t2s_cfg(&g_env_fb, &g_cfg) == 0)
            g_cfg_initialized = true;
    }
    load_unlock();
    if (!ok) {
        SFS_INIT_ERROR(errbuf,
            "fractal_textsql: out of memory reading the process environment");
        return false;
    }
    return true;
}

/* ------------------------------------------------------------------ */
/* fractalsqld.conf push path (fractalsql_provider.h contract).       */
/* ------------------------------------------------------------------ */

int
fractalsql_textsql_check_provider(const struct fsq_provider_cfg *c)
{
    /* The t2s/review tiers re-load their plugin from the cleared
     * loaded-flags on the next dispatch, exactly like the cognition
     * tiers; no cross-state constrains a swap. */
    (void) c;
    return 0;
}

/* This consumer ignores the embed/RESPONSE_MODE keys (the embed tier and
 * the response-mode extractor are fractalsql_cognition.c's); every
 * otherwise-untouched field reverts to the boot-env fallback exactly
 * like a present-vs-absent key. */
int
fractalsql_textsql_apply_provider(const struct fsq_provider_cfg *c)
{
    t2s_config n;
    char *np_plugin = NULL, *nurl = NULL, *ntok = NULL, *nmodel = NULL;
    char *nthink = NULL, *nthink_provider = NULL, *nnative = NULL, *nnum_ctx = NULL;
    const char *src;

    /* We take the lock ourselves: the daemon calls this on startup and
     * on FSQ_OP_RELOAD, and the swap must be ordered against every
     * dispatch's read of g_cfg (apply_generate_env_locked et al). */
    load_lock();
    if (!capture_t2s_env_fb()) {
        /* The boot-environment fallback could not be captured, so the
         * provider's own values would be applied over a snapshot that
         * silently dropped whatever the operator set. The apply
         * refuses as a whole instead. */
        load_unlock();
        return -1;
    }

    src = (c->present & FSQL_PROV_REASONING_PLUGIN) ? c->reasoning_plugin
                                                    : g_env_fb.reasoning_plugin;
    if (dup_or(src, &np_plugin) != 0) goto oom;
    src = (c->present & FSQL_PROV_REASONING_URL) ? c->reasoning_url
                                                 : g_env_fb.http_url;
    if (dup_or(src, &nurl) != 0) goto oom;
    src = (c->present & FSQL_PROV_REASONING_TOKEN) ? c->reasoning_token
                                                   : g_env_fb.http_token;
    if (dup_or(src, &ntok) != 0) goto oom;
    src = (c->present & FSQL_PROV_REASONING_MODEL) ? c->reasoning_model
                                                   : g_env_fb.http_model;
    if (dup_or(src, &nmodel) != 0) goto oom;
    src = (c->present & FSQL_PROV_THINK) ? c->think : g_env_fb.http_think;
    if (dup_or(src, &nthink) != 0) goto oom;
    src = (c->present & FSQL_PROV_THINK_PROVIDER) ? c->think_provider
                                                  : g_env_fb.http_think_provider;
    if (dup_or(src, &nthink_provider) != 0) goto oom;
    src = (c->present & FSQL_PROV_NATIVE_URL) ? c->think_native_url
                                              : g_env_fb.http_native_url;
    if (dup_or(src, &nnative) != 0) goto oom;
    src = (c->present & FSQL_PROV_NUM_CTX) ? c->think_num_ctx_str
                                           : g_env_fb.http_num_ctx;
    if (dup_or(src, &nnum_ctx) != 0) goto oom;

    n = g_env_fb;                        /* scalars + fixed-size fields */
    n.reasoning_plugin    = np_plugin;   /* strings become the new copies */
    n.http_url            = nurl;
    n.http_token          = ntok;
    n.http_model          = nmodel;
    n.http_think          = nthink;
    n.http_think_provider = nthink_provider;
    n.http_native_url     = nnative;
    n.http_num_ctx        = nnum_ctx;
    if (c->present & FSQL_PROV_REASONING_PLAINTEXT)
        n.http_allow_plaintext = c->reasoning_allow_plaintext;

    free_t2s_cfg(&g_cfg);
    g_cfg              = n;
    g_provider_applied = true;
    g_cfg_initialized  = true;

    /* Plugin loads are never attempted here (fractalsql_provider.h): a
     * changed or failing path surfaces on the tier's next dispatch, when
     * these cleared flags force a fresh fsql_load_reasoning. The
     * fingerprints are cleared with them: a provider reload can change
     * the plugin PATH, and the fingerprint only describes the
     * environment -- a path change leaves it untouched. */
    fractal_session_forget_t2s_loaded();
    g_t2s_fp_valid    = false;
    load_unlock();
    return 0;

oom:
    free(np_plugin); free(nurl); free(ntok); free(nmodel);
    free(nthink); free(nthink_provider); free(nnative); free(nnum_ctx);
    load_unlock();
    return -1;
}

/* ==================================================================== */
/* Reasoning ctx bridge. See fractalsql_cognition.c's file header for   */
/* the full setenv() race rationale; this is the same pattern for a     */
/* third dispatch purpose (GENERATE, RESPONSE_MODE=code).               */
/* ==================================================================== */

/* "generate" mode: chat completions, RESPONSE_MODE=code so the
 * plugin's fenced-block extraction pulls the SQL out of the model's
 * ```sql ... ``` response. system_tag is caller-supplied (the
 * orchestrating procedure derives it from VERSION(), e.g.
 * "db1011") rather than computed here, since a plain UDF has no
 * access to the connected server's version the way a live SQL
 * session does. Caller MUST hold g_load_lock. */
static void
apply_generate_env_locked(const char *system_tag)
{
    /* Same key-removal contract as apply_reason_env_locked (cognition):
     * an absent g_cfg field is cleared, not left at its last applied
     * value, so removing a conf key reverts that setting live. */
    if (g_cfg.http_url)
        setenv("FSQL_REASONING_HTTP_URL", g_cfg.http_url, 1);
    else
        unsetenv("FSQL_REASONING_HTTP_URL");
    if (g_cfg.http_token)
        setenv("FSQL_REASONING_HTTP_TOKEN", g_cfg.http_token, 1);
    else
        unsetenv("FSQL_REASONING_HTTP_TOKEN");
    if (g_cfg.http_model)
        setenv("FSQL_REASONING_HTTP_MODEL", g_cfg.http_model, 1);
    else
        unsetenv("FSQL_REASONING_HTTP_MODEL");
    if (g_cfg.http_allow_plaintext)
        setenv("FSQL_REASONING_HTTP_ALLOW_PLAINTEXT", "1", 1);
    else
        unsetenv("FSQL_REASONING_HTTP_ALLOW_PLAINTEXT");
    if (g_cfg.http_think)
        setenv("FSQL_REASONING_HTTP_THINK", g_cfg.http_think, 1);
    else
        unsetenv("FSQL_REASONING_HTTP_THINK");
    if (g_cfg.http_think_provider)
        setenv("FSQL_REASONING_HTTP_THINK_PROVIDER", g_cfg.http_think_provider, 1);
    else
        unsetenv("FSQL_REASONING_HTTP_THINK_PROVIDER");
    if (g_cfg.http_native_url)
        setenv("FSQL_REASONING_HTTP_NATIVE_URL", g_cfg.http_native_url, 1);
    else
        unsetenv("FSQL_REASONING_HTTP_NATIVE_URL");
    if (g_cfg.http_num_ctx)
        setenv("FSQL_REASONING_HTTP_NUM_CTX", g_cfg.http_num_ctx, 1);
    else
        unsetenv("FSQL_REASONING_HTTP_NUM_CTX");
    unsetenv("FSQL_REASONING_HTTP_MODE");
    setenv("FSQL_REASONING_HTTP_RESPONSE_MODE", "code", 1);
    if (system_tag && *system_tag)
        setenv("FSQL_REASONING_HTTP_SYSTEM_TAG", system_tag, 1);
    else
        unsetenv("FSQL_REASONING_HTTP_SYSTEM_TAG");
}

/* ------------------------------------------------------------------- */
/* Publish the bridge environment, and reload the tier's instance when */
/* the published configuration is not the one the instance was loaded  */
/* with.                                                             */
/* ------------------------------------------------------------------- */

/* Publishes the text-to-SQL tier's settings plus this session's own
 * overrides, and (re)loads the tier's plugin instance when the
 * effective configuration no longer matches the loaded one.
 *
 * WHY THE RELOAD: the reasoning plugin reads its configuration ONCE,
 * when it is loaded, and bakes it into the loaded instance -- the
 * plugin's ABI has no per-dispatch configuration read, and the
 * vendored core never reads the environment at all. A session's
 * model/THINK override is published into the environment, so it only
 * reaches the plugin if the instance is loaded again with that
 * environment in place. That is what the fingerprint decides.
 *
 * THE COST: the tier holds ONE instance, shared by every session, so
 * two sessions asking for different models make the tier reload
 * between their calls. A session that overrides nothing never
 * triggers a reload.
 *
 * Caller MUST hold the tier's dispatch lock (g_load_lock): the ctx is
 * single-thread affine, and the load replaces the plugin attached to
 * it. Returns false with a one-line reason on a load failure. */
static bool
generate_env_and_reload(fsql_ctx *ctx, unsigned long long session_id,
                         const char *system_tag, char *errbuf)
{
    uint64_t fp;
    int rc;

    /* The environment lock is taken around the publish+load window and
     * released again before the dispatch: the plugin does not read the
     * environment during a dispatch, so holding it across the network
     * round trip would only serialise the tiers against each other. */
    fractal_config_env_lock();
    apply_generate_env_locked(system_tag);
    fractal_config_apply_env_overrides(session_id, FSQL_CFG_TIER_T2S);
    fp = fractal_config_env_fingerprint();
    if (!g_t2s_fp_valid || fp != g_t2s_fp) {
        char perr[512];
        /* The plugin is native code the daemon maps into its own
         * process, so its provenance is checked before the load and
         * again after it. */
        if (fv_plugin_gate(g_cfg.reasoning_plugin, perr, sizeof perr) != 0) {
            fractal_config_env_unlock();
            SFS_INIT_ERROR(errbuf, "fractal_t2s_generate: %s", perr);
            return false;
        }
        rc = fsql_load_reasoning(ctx, g_cfg.reasoning_plugin);
        if (rc != FSQL_OK) {
            fractal_config_env_unlock();
            {
                const char *err = fsql_last_error(ctx);
                char detail[T2S_DETAIL_MAX + sizeof T2S_DETAIL_MARK];
                t2s_detail_bounded(err && *err ? err : "(no detail)",
                                   detail, sizeof detail);
                SFS_INIT_ERROR(errbuf,
                    "fractal_t2s_generate: failed to load reasoning plugin (rc=%d): %s",
                    rc, detail);
            }
            return false;
        }
        if (fv_plugin_gate_after_load(g_cfg.reasoning_plugin, perr, sizeof perr) != 0) {
            fractal_config_env_unlock();
            SFS_INIT_ERROR(errbuf, "fractal_t2s_generate: %s", perr);
            return false;
        }
        /* Fingerprinted AFTER the load, so it describes the
         * environment the loaded instance actually saw. */
        g_t2s_fp = fractal_config_env_fingerprint();
        g_t2s_fp_valid = true;
    }
    fractal_config_env_unlock();
    return true;
}

static bool
ensure_generate_loaded(unsigned long long session_id, fsql_ctx *ctx,
                       bool already_loaded, const char *system_tag, char *errbuf)
{
    if (already_loaded) return true;
    if (!ensure_env_config(errbuf)) return false;
    /* The plugin path is read UNDER the load lock: apply_provider frees
     * the old copy before installing the new one, so an unlocked read of
     * the pointer races that free (a stale non-NULL value would send the
     * dispatch down the "plugin configured" path when the config says
     * otherwise). The value is only tested here -- the dispatch that
     * actually uses it re-reads the config under the lock. */
    {
        bool no_plugin;
        load_lock();
        no_plugin = (g_cfg.reasoning_plugin == NULL);
        load_unlock();
        if (no_plugin) {
            SFS_INIT_ERROR(errbuf,
                "fractal_t2s_generate: no reasoning plugin configured. Set reasoning_plugin "
                "in fractalsqld.conf and run `fsqlctl reload`, or set "
                "FRACTALSQL_REASONING_PLUGIN in fractalsqld's environment at startup");
            return false;
        }
    }
    load_lock();
    if (!generate_env_and_reload(ctx, session_id, system_tag, errbuf)) {
        load_unlock();
        return false;
    }
    load_unlock();
    fractal_session_mark_t2s_loaded(session_id);
    return true;
}

/* ==================================================================== */
/* fractal_t2s_generate(session_id, prompt, context_text, system_tag)   */
/*   -> TEXT (candidate SQL, code-mode-extracted)                       */
/* ==================================================================== */

FRACTAL_EXPORT bool
fractal_t2s_generate_init(UDF_INIT *initid, UDF_ARGS *args, char *message)
{
    if (args->arg_count != 4) {
        SFS_INIT_ERROR(message,
            "fractal_t2s_generate(session_id, prompt, context_text, system_tag): "
            "expected 4 arguments, got %u", args->arg_count);
        return true;
    }
    args->arg_type[0] = INT_RESULT;
    args->arg_type[1] = STRING_RESULT;
    args->arg_type[2] = STRING_RESULT;
    args->arg_type[3] = STRING_RESULT;
    return str_out_generic_init(initid, message, FRACTAL_MAX_AI_RESPONSE_BYTES);
}

FRACTAL_EXPORT void
fractal_t2s_generate_deinit(UDF_INIT *initid) { str_out_generic_deinit(initid); }

FRACTAL_EXPORT char *
fractal_t2s_generate(UDF_INIT *initid, UDF_ARGS *args, char *result,
                     unsigned long *length, char *is_null, char *error)
{
    str_out_ctx *so = (str_out_ctx *) initid->ptr;
    char    errbuf[FSQL_ERRMSG_SIZE];
    unsigned long long sid;
    const char *context_json; unsigned long context_len;
    const char *system_tag;
    bool    loaded = false;
    fsql_ctx *ctx;
    fsql_ai_response_t resp;
    char err_copy[T2S_DETAIL_MAX + 1];
    int     rc;
    char   *s;
    (void) result;

    if (args->args[0] == NULL || args->args[1] == NULL) { *is_null = 1; return NULL; }
    if (args->lengths[1] > FRACTAL_T2S_MAX_INPUT_BYTES) { *error = 1; return NULL; }

    if (args->args[2] != NULL) {
        if (args->lengths[2] > FRACTAL_T2S_MAX_INPUT_BYTES) { *error = 1; return NULL; }
        context_json = args->args[2];
        context_len  = args->lengths[2];
    } else {
        context_json = "{}";
        context_len  = 2;
    }
    /* Copy with the ABI-given length and NUL-terminate: the UDF ABI does
     * not terminate string arguments (the same fact the session/ctx
     * comment below concedes for args[1]/[2]), and a raw pointer here
     * would let setenv()'s strlen run off the end of the record buffer. */
    char system_tag_buf[64];
    if (args->args[3] != NULL && args->lengths[3] > 0) {
        if (args->lengths[3] >= sizeof system_tag_buf) {
            *error = 1; return NULL;
        }
        memcpy(system_tag_buf, args->args[3], args->lengths[3]);
        system_tag_buf[args->lengths[3]] = '\0';
        system_tag = system_tag_buf;
    } else {
        system_tag = NULL;   /* ensure_generate_loaded tolerates it */
    }

    sid = (unsigned long long) *(long long *) args->args[0];

    ctx = fractal_session_acquire_t2s(sid, &loaded);
    if (ctx == NULL) {
        SFS_INIT_ERROR(errbuf,
            "fractal_t2s_generate: session acquire failed for id %llu", sid);
        *error = 1; return NULL;
    }

    if (!ensure_generate_loaded(sid, ctx, loaded, system_tag, errbuf)) {
        fractal_session_release_tier();
        *error = 1; return NULL;
    }

    memset(&resp, 0, sizeof(resp));
    /* The plugin's configuration is baked in when the instance is
     * loaded, so every dispatch republishes the tier's settings (plus
     * this session's own overrides) and reloads the instance if that
     * configuration is not the one the instance was loaded with. */
    load_lock();
    if (!generate_env_and_reload(ctx, sid, system_tag, errbuf)) {
        load_unlock();
        fractal_session_release_tier();
        *error = 1; return NULL;
    }
    rc = fsql_dispatch_ai(ctx, args->args[1], args->lengths[1],
                          context_json, context_len, &resp);
    /* The core writes its error message into the context, and the tier's
     * context is shared by every connection that uses the tier: read the
     * message while the tier's lock is still held, or the read races
     * another thread's dispatch (ThreadSanitizer sees exactly that). */
    err_copy[0] = '\0';
    if (rc != FSQL_OK || resp.rc != 0) {
        const char *e = fsql_last_error(ctx);
        if (e) snprintf(err_copy, sizeof err_copy, "%s", e);
    }
    load_unlock();
    if (rc != FSQL_OK || resp.rc != 0) {
        const char *err = err_copy;
        char detail[T2S_DETAIL_MAX + sizeof T2S_DETAIL_MARK];
        t2s_detail_bounded(err && *err ? err : "(no detail)",
                           detail, sizeof detail);
        /* Mirrored to stderr via SFS_INIT_ERROR: a plain *error=1 here
         * surfaces only as the orchestrating procedure's generic
         * "generate dispatch failed" NULL, hiding the plugin's actual
         * reason (e.g. code-mode fence extraction failure). */
        SFS_INIT_ERROR(errbuf,
            "fractal_t2s_generate: dispatch failed (rc=%d, resp.rc=%d): %s",
            rc, resp.rc, detail);
        fsql_ai_response_free(&resp);
        fractal_session_release_tier();
        *error = 1; return NULL;
    }
    if (resp.summary_len > FRACTAL_MAX_AI_RESPONSE_BYTES) {
        SFS_INIT_ERROR(errbuf,
            "fractal_t2s_generate: response %zu bytes exceeds "
            "FRACTAL_MAX_AI_RESPONSE_BYTES", resp.summary_len);
        fsql_ai_response_free(&resp);
        fractal_session_release_tier();
        *error = 1; return NULL;
    }
    /* The rc==0 dispatch contract says summary is non-NULL, but the
     * copy below would crash on a contract-violating plugin -- a
     * plugin bug gets a clean error, not a segfault. */
    if (resp.summary == NULL) {
        SFS_INIT_ERROR(errbuf,
            "fractal_t2s_generate: reasoning plugin returned a NULL summary "
            "on a success status -- likely a plugin bug");
        fsql_ai_response_free(&resp);
        fractal_session_release_tier();
        *error = 1; return NULL;
    }

    s = str_out_set(so, resp.summary, resp.summary_len, length);
    fsql_ai_response_free(&resp);
    fractal_session_release_tier();
    if (s == NULL) { *error = 1; return NULL; }
    *is_null = 0;
    return s;
}
