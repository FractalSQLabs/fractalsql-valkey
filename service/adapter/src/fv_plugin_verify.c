/* SPDX-License-Identifier: Apache-2.0 */
/* fv_plugin_verify.c -- the reasoning plugin's authenticity gate.
 *
 * See fv_plugin_verify.h for the rule this file enforces and for the
 * two entry points it exposes.
 */
#include "fv_plugin_verify.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#include <openssl/evp.h>

#include "../../common/fsq_sha256.h"

#if defined(_WIN32)
#  include <windows.h>
#else
#  include <pthread.h>
#endif

/* The FractalSQLabs reasoning-plugin signing public key. A plugin release is
 * signed with the matching private key; its detached <plugin>.sig (64 raw
 * bytes) must verify against this key before the plugin is loaded. It is a
 * different key from the enterprise-core key. Compiled in; no setting can
 * replace it. */
static const unsigned char FV_PLUGIN_PUBKEY[32] = {
    0xbb, 0x24, 0x79, 0x25, 0x84, 0xe8, 0x9b, 0x1d, 0x1b, 0x48, 0x49, 0xba,
    0x70, 0x1d, 0xa1, 0x71, 0xa3, 0x4f, 0x8a, 0x29, 0x47, 0x83, 0xd1, 0xd4,
    0x74, 0xdf, 0x9b, 0x07, 0x44, 0xc5, 0x94, 0x53
};

/* The plugin that was verified last, and its digest at the time. The
 * after-load check compares the file against this, so a file swapped
 * between the check and the load is caught. */
#if defined(_WIN32)
static INIT_ONCE        g_lock_once = INIT_ONCE_STATIC_INIT;
static CRITICAL_SECTION g_lock;
static BOOL CALLBACK
init_lock_once(PINIT_ONCE ip, PVOID param, PVOID *ctx)
{
    (void) ip; (void) param; (void) ctx;
    InitializeCriticalSection(&g_lock);
    return TRUE;
}
static void fv_lock(void)
{
    InitOnceExecuteOnce(&g_lock_once, init_lock_once, NULL, NULL);
    EnterCriticalSection(&g_lock);
}
static void fv_unlock(void) { LeaveCriticalSection(&g_lock); }
#else
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static void fv_lock(void)   { pthread_mutex_lock(&g_lock); }
static void fv_unlock(void) { pthread_mutex_unlock(&g_lock); }
#endif

static char         g_verified_path[4096];
static unsigned char g_verified_digest[32];
static int          g_verified_valid;

/* Writes a refusal reason into the caller's buffer. The arguments must
 * match the conversions in the format string exactly: a format with
 * more conversions than arguments makes the formatting read past the
 * argument list. */
#if defined(__GNUC__) || defined(__clang__)
__attribute__((format(printf, 3, 4)))
#endif
static void
fv_set_err(char *err, size_t err_cap, const char *fmt, ...)
{
    va_list ap;
    if (err == NULL || err_cap == 0) return;
    va_start(ap, fmt);
    vsnprintf(err, err_cap, fmt, ap);
    va_end(ap);
}

static void
fv_to_hex(const unsigned char *in, size_t n, char *out)
{
    static const char digits[] = "0123456789abcdef";
    size_t i;
    for (i = 0; i < n; i++) {
        out[i * 2]     = digits[(in[i] >> 4) & 0x0f];
        out[i * 2 + 1] = digits[in[i] & 0x0f];
    }
    out[n * 2] = '\0';
}

/* Reads the whole file. On success the caller owns *out and must free
 * it. */
static int
fv_read_file(const char *path, unsigned char **out, size_t *out_len)
{
    FILE *f;
    unsigned char *bytes;
    long len;

    *out = NULL;
    *out_len = 0;
    f = fopen(path, "rb");
    if (f == NULL) return -1;
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return -1; }
    len = ftell(f);
    if (len < 0 || fseek(f, 0, SEEK_SET) != 0) { fclose(f); return -1; }
    bytes = (unsigned char *) malloc((size_t) len ? (size_t) len : 1);
    if (bytes == NULL) { fclose(f); return -1; }
    if (fread(bytes, 1, (size_t) len, f) != (size_t) len) {
        free(bytes);
        fclose(f);
        return -1;
    }
    fclose(f);
    *out = bytes;
    *out_len = (size_t) len;
    return 0;
}

/* Verifies a detached Ed25519 signature over the message. */
static int
fv_ed25519_verify(const unsigned char *sig, size_t sig_len,
                   const unsigned char *msg, size_t msg_len)
{
    EVP_PKEY   *pkey  = NULL;
    EVP_MD_CTX *mdctx = NULL;
    int         ok    = 0;

    if (sig_len != 64) return 0;
    pkey = EVP_PKEY_new_raw_public_key(EVP_PKEY_ED25519, NULL,
                                       FV_PLUGIN_PUBKEY, sizeof FV_PLUGIN_PUBKEY);
    if (pkey == NULL) return 0;
    mdctx = EVP_MD_CTX_new();
    if (mdctx == NULL) { EVP_PKEY_free(pkey); return 0; }

    /* Ed25519 is "PureEdDSA" in OpenSSL's EVP API: one-shot verification
     * over the whole message, no Update() calls, no pre-hash type. */
    if (EVP_DigestVerifyInit(mdctx, NULL, NULL, NULL, pkey) == 1 &&
        EVP_DigestVerify(mdctx, sig, sig_len, msg, msg_len) == 1)
        ok = 1;

    EVP_MD_CTX_free(mdctx);
    EVP_PKEY_free(pkey);
    return ok;
}

int
fv_plugin_gate(const char *path, char *err, size_t err_cap)
{
    const char   *rq;
    int           require;
    unsigned char *bytes = NULL;
    size_t        len   = 0;
    unsigned char digest[32];
    char          sig_path[4096];
    unsigned char sig[64];
    FILE         *f;
    size_t        n;
    int           extra;

    if (path == NULL || *path == '\0') {
        fv_set_err(err, err_cap, "no reasoning plugin is configured");
        return -1;
    }

    /* A signature is required unless FSQL_REASONING_REQUIRE_SIGNATURE=0 is in the
     * environment. With it off, a plugin that has no .sig still loads (one warning);
     * a .sig that is present is always checked, and a wrong one is always refused. */
    rq = getenv("FSQL_REASONING_REQUIRE_SIGNATURE");
    require = !(rq != NULL && rq[0] == '0');

    if (fv_read_file(path, &bytes, &len) != 0) {
        if (!require) return 0;      /* the loader reports an unreadable plugin itself */
        fv_set_err(err, err_cap,
                   "the reasoning plugin \"%s\" cannot be read for verification", path);
        return -1;
    }
    fsq_sha256(bytes, len, digest);

    /* Otherwise a detached release signature over the exact bytes. */
    if (snprintf(sig_path, sizeof sig_path, "%s.sig", path) >= (int) sizeof sig_path) {
        free(bytes);
        fv_set_err(err, err_cap,
                   "the reasoning plugin's path is too long to locate its .sig");
        return -1;
    }
    f = fopen(sig_path, "rb");
    if (f == NULL) {
        free(bytes);
        if (!require) {
            static int warned = 0;
            if (!warned) {
                warned = 1;
                fprintf(stderr,
                        "fractalsql: loading reasoning plugin \"%s\" without a signature "
                        "(FSQL_REASONING_REQUIRE_SIGNATURE=0)\n", path);
            }
            return 0;
        }
        fv_set_err(err, err_cap,
                   "no signature found for reasoning plugin \"%s\" (expected \"%s.sig\"); place the .sig beside the plugin, or set FSQL_REASONING_REQUIRE_SIGNATURE=0 to load an unsigned plugin",
                   path, path);
        return -1;
    }
    n = fread(sig, 1, sizeof sig, f);
    /* The .sig must be EXACTLY 64 bytes: a longer file silently
     * truncated to 64 by fread would verify against the wrong
     * (partial) signature. */
    extra = fgetc(f);
    fclose(f);
    if (n != sizeof sig || extra != EOF ||
        !fv_ed25519_verify(sig, sizeof sig, bytes, len)) {
        free(bytes);
        fv_set_err(err, err_cap,
                   "reasoning plugin \"%s\" failed signature verification -- refusing to load (the plugin or its .sig is corrupt or not from FractalSQLabs)",
                   path);
        return -1;
    }

    free(bytes);

    /* Remember what was verified, so the after-load check can tell
     * whether the file changed underneath the load. */
    fv_lock();
    {
        size_t plen = strlen(path);
        if (plen < sizeof g_verified_path) {
            memcpy(g_verified_path, path, plen + 1);
            memcpy(g_verified_digest, digest, sizeof digest);
            g_verified_valid = 1;
        }
    }
    fv_unlock();
    return 0;
}

int
fv_plugin_gate_after_load(const char *path, char *err, size_t err_cap)
{
    unsigned char *bytes = NULL;
    size_t        len   = 0;
    unsigned char digest[32];
    char          known[4096];
    unsigned char known_digest[32];
    int           known_valid;
    int           same = 0;

    if (path == NULL || *path == '\0') return 0;

    fv_lock();
    {
        size_t klen = strlen(g_verified_path);
        if (klen < sizeof known) memcpy(known, g_verified_path, klen + 1);
        else known[0] = '\0';
    }
    memcpy(known_digest, g_verified_digest, sizeof known_digest);
    known_valid = g_verified_valid;
    fv_unlock();

    /* Nothing was verified for this path (the operator's opt-out, or a
     * load that did not come through this gate): there is nothing to
     * compare against, so the file is trusted as it was. */
    if (!known_valid || strcmp(known, path) != 0) return 0;

    if (fv_read_file(path, &bytes, &len) != 0) {
        fv_set_err(err, err_cap,
                   "the reasoning plugin \"%s\" cannot be read after the load", path);
        return -1;
    }
    fsq_sha256(bytes, len, digest);
    free(bytes);

    same = (memcmp(digest, known_digest, sizeof digest) == 0);
    if (!same) {
        fv_set_err(err, err_cap,
                   "the reasoning plugin \"%s\" changed during the load -- refusing to use it",
                   path);
        return -1;
    }
    return 0;
}
