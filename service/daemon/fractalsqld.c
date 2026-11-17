/* SPDX-License-Identifier: Apache-2.0 */
/* SPDX-FileCopyrightText: 2026 Daniel Gardiner d/b/a FractalSQLabs
 *
 * fractalsqld: separate process that serves the FractalSQL UDFs to the
 * GPL-2.0-only shim over the FSQ protocol (../protocol). The spike build
 * links no core library. Handlers are added per phase.
 */

#define _GNU_SOURCE

#include <errno.h>
#include <signal.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>

#include "fsql_udf_types.h"
#include <stdbool.h>

#include "../common/fsq_sha256.h"
#include "../protocol/fsq_protocol.h"
#include "fractalsql_config.h"     /* src/: the shared path rules */
#include "fractalsql_interrupt.h"  /* src/: the cancel seams the UDF bodies poll */
#include "fractalsql_provider.h"   /* src/: the conf -> provider-consumer push */

/* Platform layer: AF_UNIX sockets and pthreads. */
#  include <pthread.h>
#  include <sys/socket.h>
#  include <sys/un.h>
#  include <unistd.h>
#  define FSQ_PRINTF(fmt_idx, arg_idx) __attribute__((format(printf, fmt_idx, arg_idx)))
#  define FSQ_TLS __thread
#  define FSQ_CONFIG_DEFAULT "/etc/fractalsql/fractalsqld.conf"
#  define FSQ_SLEEP_MS(ms) usleep((ms) * 1000)
#  define FSQ_THREAD_RET void *
#  define FSQ_THREAD_EXIT NULL
#  define fsq_strtok_r strtok_r
#  define fsq_pid() ((long) getpid())
typedef pthread_mutex_t fsq_mutex;
#  define FSQ_MUTEX_INIT PTHREAD_MUTEX_INITIALIZER
#  define FSQ_LOCK(m) pthread_mutex_lock(&(m))
#  define FSQ_UNLOCK(m) pthread_mutex_unlock(&(m))
typedef int fsq_sock;
#  define FSQ_SOCK_INVALID (-1)
#  define FSQ_SOCK_VALID(s) ((s) >= 0)
typedef volatile int fsq_atomic_int;
#  define FSQ_ATOMIC_GET(p)   __atomic_load_n((p), __ATOMIC_SEQ_CST)
#  define FSQ_ATOMIC_SET(p,v) __atomic_store_n((p), (v), __ATOMIC_SEQ_CST)

#define FSQL_DAEMON_VERSION     "2.0.2"
#define FSQ_CONFIG_ENV          "FRACTALSQL_CONFIG"
#define FSQ_MAX_ALLOWED_UIDS    16
#define FSQ_PEER_MAX            160
#define FSQ_DEFAULT_MAX_CONNECTIONS   64
#define FSQ_MAX_CONNECTIONS_LIMIT     1024
#define FSQ_DEFAULT_IDLE_TIMEOUT_SECS 300

struct fsq_config {
    char socket_path[256];
    char key_path[512];
    char log_path[512];
    uid_t allowed_uids[FSQ_MAX_ALLOWED_UIDS];
    size_t n_allowed;
    uint8_t key[64];
    size_t key_len;
    int max_connections;
    int idle_timeout_secs;
    /* Provider settings (fractalsql_provider.h): the reasoning/embed/
     * think/t2s/enterprise keys, with prov_present remembering which are
     * present in the file. Fixed buffers: a published snapshot is
     * immutable and freed wholesale, so a provider push can point
     * straight at them; the consumers deep-copy what they keep.
     * reasoning_token and enterprise_ledger_key are plaintext
     * credentials -- never printed, only "set"/"unset". */
    char reasoning_plugin[512];
    char reasoning_url[512];
    char reasoning_token[1024];
    char reasoning_model[128];
    bool reasoning_allow_plaintext;
    char embed_url[512];
    char embed_model[128];
    char think[64];
    char think_provider[64];
    char think_native_url[512];
    long think_num_ctx;
    char  think_num_ctx_str[24];
    char  enterprise_lib[512];
    char  enterprise_ledger_path[1024];
    char  enterprise_ledger_key[1024];
    bool  enterprise_require_signature;
    uint32_t prov_present;
};

/* A reply is a status, a core return code, and an opaque body. */
struct fsq_reply {
    uint32_t status;
    int32_t core_rc;
    uint8_t *body;
    uint32_t body_len;
};

static volatile sig_atomic_t g_stop = 0;
static fsq_sock g_listen_fd = FSQ_SOCK_INVALID;

/* One line per event on stderr, which the container runtime collects.
 * Payloads and query text are never logged; only opcodes, sizes,
 * peer uids and the adapter's own error text. */
static FILE *g_logf = NULL;

static void fsq_log(const char *level, const char *fmt, ...) FSQ_PRINTF(2, 3);
static void fsq_log(const char *level, const char *fmt, ...) {
    char ts[32], msg[768];
    va_list ap;
    struct timespec now;
    struct tm tmv;
    clock_gettime(CLOCK_REALTIME, &now);
    gmtime_r(&now.tv_sec, &tmv);
    strftime(ts, sizeof(ts), "%Y-%m-%dT%H:%M:%SZ", &tmv);
    va_start(ap, fmt);
    vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);
    FILE *out = g_logf ? g_logf : stderr;
    fprintf(out, "%s fractalsqld[%ld] %s %s\n", ts, fsq_pid(), level, msg);
    fflush(out);
}

/* ------------------------------------------------------------------ */
/* Configuration                                                       */
/* ------------------------------------------------------------------ */

static int hex_nibble(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static int load_key(struct fsq_config *cfg) {
    FILE *f = fopen(cfg->key_path, "r");
    char buf[256];
    size_t n = 0;
    int hi = -1;
    if (!f) return -1;
    while (n < sizeof(buf)) {
        int c = fgetc(f);
        if (c == EOF) break;
        if (c == '\n' || c == '\r' || c == ' ' || c == '\t') continue;
        buf[n++] = (char) c;
    }
    fclose(f);
    /* A key shorter than 16 bytes (128 bits) is not an authentication secret: a one-byte key would be guessed in 256 tries. */
    if (n / 2 < 16 && n != 0 && n % 2 == 0)
        fprintf(stderr, "fractalsqld: the HMAC key in %s is %zu bytes; at least 16 are required\n", cfg->key_path, n / 2);
    if (n == 0 || n % 2 != 0 || n / 2 > sizeof(cfg->key) || n / 2 < 16) return -1;
    cfg->key_len = 0;
    for (size_t i = 0; i < n; i++) {
        int v = hex_nibble(buf[i]);
        if (v < 0) return -1;
        if (hi < 0) {
            hi = v;
        } else {
            cfg->key[cfg->key_len++] = (uint8_t) ((hi << 4) | v);
            hi = -1;
        }
    }
    return 0;
}


/* Case-insensitive equality against a lower-case ASCII word. */
static int cfg_word_is(const char *v, const char *word) {
    for (; *v && *word; v++, word++) {
        char c = *v;
        if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
        if (c != *word) return 0;
    }
    return *v == '\0' && *word == '\0';
}

/* A conf boolean: empty or "0" is false, and the words false / off / no
 * (any case) are false too, so that "reasoning_allow_plaintext = off"
 * cannot silently mean the opposite. Any other non-empty value is
 * true, which is how the matching FRACTALSQL_* environment variable
 * reads. */
static int cfg_bool_true(const char *v) {
    if (v[0] == '\0' || strcmp(v, "0") == 0) return 0;
    if (cfg_word_is(v, "false") || cfg_word_is(v, "off") || cfg_word_is(v, "no")) return 0;
    return 1;
}

static int load_config(const char *path, struct fsq_config *cfg) {
    FILE *f = fopen(path, "r");
    char line[1024];
    if (!f) return -1;
    memset(cfg, 0, sizeof(*cfg));
    cfg->max_connections = FSQ_DEFAULT_MAX_CONNECTIONS;
    cfg->idle_timeout_secs = FSQ_DEFAULT_IDLE_TIMEOUT_SECS;
    while (fgets(line, sizeof(line), f)) {
        char *eq = strchr(line, '=');
        char *k, *v;
        if (!eq || line[0] == '#') continue;
        *eq = '\0';
        k = line;
        v = eq + 1;
        while (*k == ' ' || *k == '\t') k++;
        for (char *e = k + strlen(k); e > k && (e[-1] == ' ' || e[-1] == '\t'); e--) e[-1] = '\0';
        while (*v == ' ' || *v == '\t') v++;
        for (char *e = v + strlen(v); e > v && (e[-1] == '\n' || e[-1] == '\r' || e[-1] == ' '); e--) e[-1] = '\0';
        /* Copy a value into a fixed config buffer, failing the whole
         * parse on truncation: a silently clipped token or path would
         * surface later as a confusing provider error. */
#define CFG_SET_STR(field) \
        do { \
            if (strlen(v) >= sizeof(cfg->field)) { fclose(f); return -1; } \
            strcpy(cfg->field, v); \
        } while (0)
        /* A boolean value reads like the corresponding FRACTALSQL_* env
         * name: any non-empty value except "0" is true, empty is false --
         * and, so that "reasoning_allow_plaintext = off" cannot silently
         * mean the opposite, the words false / off / no (any case) are
         * false too. */
#define CFG_SET_BOOL(field) \
        do { cfg->field = cfg_bool_true(v); } while (0)
        if (strcmp(k, "socket_path") == 0) {
            CFG_SET_STR(socket_path);
        } else if (strcmp(k, "hmac_key_file") == 0) {
            CFG_SET_STR(key_path);
        } else if (strcmp(k, "log_file") == 0) {
            CFG_SET_STR(log_path);
        } else if (strcmp(k, "max_connections") == 0) {
            cfg->max_connections = atoi(v);
        } else if (strcmp(k, "idle_timeout_secs") == 0) {
            cfg->idle_timeout_secs = atoi(v);
        } else if (strcmp(k, "allowed_uids") == 0) {
            char *save = NULL;
            for (char *tok = fsq_strtok_r(v, ",", &save); tok; tok = fsq_strtok_r(NULL, ",", &save)) {
                if (cfg->n_allowed >= FSQ_MAX_ALLOWED_UIDS) { fclose(f); return -1; }
                cfg->allowed_uids[cfg->n_allowed++] = (uid_t) strtoul(tok, NULL, 10);
            }
        /* Provider keys (fractalsql_provider.h). Parse is lenient here;
         * validate_provider_cfg does the loud, specific range checks a
         * deliberate file edit deserves -- and it knows which keys were
         * present, so absent keys' silent env defaults stay untouched. */
        } else if (strcmp(k, "reasoning_plugin") == 0) {
            CFG_SET_STR(reasoning_plugin);
            cfg->prov_present |= FSQL_PROV_REASONING_PLUGIN;
        } else if (strcmp(k, "reasoning_url") == 0) {
            CFG_SET_STR(reasoning_url);
            cfg->prov_present |= FSQL_PROV_REASONING_URL;
        } else if (strcmp(k, "reasoning_token") == 0) {
            CFG_SET_STR(reasoning_token);
            cfg->prov_present |= FSQL_PROV_REASONING_TOKEN;
        } else if (strcmp(k, "reasoning_model") == 0) {
            CFG_SET_STR(reasoning_model);
            cfg->prov_present |= FSQL_PROV_REASONING_MODEL;
        } else if (strcmp(k, "reasoning_allow_plaintext") == 0) {
            CFG_SET_BOOL(reasoning_allow_plaintext);
            cfg->prov_present |= FSQL_PROV_REASONING_PLAINTEXT;
        } else if (strcmp(k, "embed_url") == 0) {
            CFG_SET_STR(embed_url);
            cfg->prov_present |= FSQL_PROV_EMBED_URL;
        } else if (strcmp(k, "embed_model") == 0) {
            CFG_SET_STR(embed_model);
            cfg->prov_present |= FSQL_PROV_EMBED_MODEL;
        } else if (strcmp(k, "think") == 0) {
            CFG_SET_STR(think);
            cfg->prov_present |= FSQL_PROV_THINK;
        } else if (strcmp(k, "think_provider") == 0) {
            CFG_SET_STR(think_provider);
            cfg->prov_present |= FSQL_PROV_THINK_PROVIDER;
        } else if (strcmp(k, "think_native_url") == 0) {
            CFG_SET_STR(think_native_url);
            cfg->prov_present |= FSQL_PROV_NATIVE_URL;
        } else if (strcmp(k, "think_num_ctx") == 0) {
            /* Lenient parse: validate_provider_cfg refuses garbage and
             * out-of-range values by name. Trailing garbage after the
             * digits ("10abc") is rejected here too, not just a value
             * with no digits at all -- *endp must land on the NUL. */
            char *endp;
            cfg->think_num_ctx = strtol(v, &endp, 10);
            if (endp == v || *endp != '\0') cfg->think_num_ctx = -1;
            cfg->prov_present |= FSQL_PROV_NUM_CTX;
        } else if (strcmp(k, "enterprise_lib") == 0) {
            CFG_SET_STR(enterprise_lib);
            cfg->prov_present |= FSQL_PROV_ENT_LIB;
        } else if (strcmp(k, "enterprise_ledger_path") == 0) {
            CFG_SET_STR(enterprise_ledger_path);
            cfg->prov_present |= FSQL_PROV_ENT_LEDGER_PATH;
        } else if (strcmp(k, "enterprise_ledger_key") == 0) {
            CFG_SET_STR(enterprise_ledger_key);
            cfg->prov_present |= FSQL_PROV_ENT_LEDGER_KEY;
        } else if (strcmp(k, "enterprise_require_signature") == 0) {
            CFG_SET_BOOL(enterprise_require_signature);
            cfg->prov_present |= FSQL_PROV_ENT_REQUIRE_SIG;
        } else {
            /* With the provider keys live, an unknown key almost always
             * means a typo -- a token meant for the daemon quietly
             * orphaned from it. Say so at WARN. */
            fsq_log("WARN", "config: unknown key '%s' ignored", k);
        }
#undef CFG_SET_STR
#undef CFG_SET_BOOL
    }
    fclose(f);
    if (cfg->socket_path[0] == '\0' || cfg->key_path[0] == '\0') return -1;
    if (strlen(cfg->socket_path) >= sizeof(((struct sockaddr_un *)0)->sun_path)) return -1;
    if (cfg->n_allowed == 0) cfg->allowed_uids[cfg->n_allowed++] = geteuid();
    if (cfg->max_connections <= 0 || cfg->max_connections > FSQ_MAX_CONNECTIONS_LIMIT) return -1;
    if (cfg->idle_timeout_secs <= 0) return -1;
    return load_key(cfg);
}

/* ------------------------------------------------------------------ */
/* Refcounted configuration snapshots                                  */
/* ------------------------------------------------------------------ */

/* The platform checks a config must pass to be usable. main() runs them
 * at startup and RELOAD runs them again before applying anything, so a
 * reload can never publish a config the daemon would refuse at boot.
 * Defined per platform below (the key-file privacy check differs). */
static int validate_cfg(const struct fsq_config *cfg);

/* One immutable configuration under a refcount. run_listener and every
 * serve thread borrow the config they read; RELOAD swaps in a fresh
 * snapshot and the previous one is freed when its last borrower releases.
 * A published snapshot is never mutated. */
struct fsq_config_snap {
    struct fsq_config cfg;
    int refs;
};

static fsq_mutex g_cfg_mu = FSQ_MUTEX_INIT;
static struct fsq_config_snap *g_snap = NULL;
/* Where re-reading happens on RELOAD: the resolved -c / env / default
 * path. A getenv or argv pointer is stable for the process's lifetime;
 * RELOAD re-reads exactly the file this daemon started with. */
static const char *g_config_path = NULL;

static struct fsq_config_snap *cfg_acquire(void) {
    struct fsq_config_snap *snap;
    FSQ_LOCK(g_cfg_mu);
    g_snap->refs++;
    snap = g_snap;
    FSQ_UNLOCK(g_cfg_mu);
    return snap;
}

static void cfg_release(struct fsq_config_snap *snap) {
    int live;
    FSQ_LOCK(g_cfg_mu);
    if (--snap->refs > 0) { FSQ_UNLOCK(g_cfg_mu); return; }
    live = g_snap == snap;
    FSQ_UNLOCK(g_cfg_mu);
    if (!live) free(snap);
}

/* Publishes a copy as the current snapshot, freeing the previous one when
 * no borrower still holds it (and deferring when one does -- cfg_release
 * frees then). Startup and RELOAD both arrive here. */
static int cfg_publish(const struct fsq_config *cfg) {
    struct fsq_config_snap *snap = malloc(sizeof(*snap)), *old;
    if (!snap) return -1;
    snap->cfg = *cfg;
    snap->refs = 0;
    FSQ_LOCK(g_cfg_mu);
    old = g_snap;
    g_snap = snap;
    if (old && old->refs == 0) { FSQ_UNLOCK(g_cfg_mu); free(old); }
    else FSQ_UNLOCK(g_cfg_mu);
    return 0;
}

/* Platform-specific checks a config must pass before this daemon runs it. */
static int validate_cfg(const struct fsq_config *cfg) {
    struct stat kst, cst;
    if (g_config_path && stat(g_config_path, &cst) == 0 && (cst.st_mode & 0007) != 0) {
        fsq_log("ERROR", "config file %s grants access to other users: remove world permissions", g_config_path);
        return -1;
    }
    if (stat(cfg->key_path, &kst) == 0 && (kst.st_mode & 0007) != 0) {
        fsq_log("ERROR", "hmac_key_file %s grants access to other users: remove world permissions", cfg->key_path);
        return -1;
    }
    return 0;
}

/* Loud, specific validation of the provider keys in a parsed config
 * (fractalsql_provider.h). A config-file edit is a deliberate act, so
 * unlike the environment fallback's historical silent clamps, garbage or
 * out-of-range values refuse the load/reload with a named key and the
 * expected range. Runs after validate_cfg: startup refuses to start and
 * RELOAD refuses to publish. */
static int validate_provider_cfg(const struct fsq_config *cfg) {
    uint32_t p = cfg->prov_present;
    struct {
        uint32_t    bit;
        const char *val;
        const char *name;
    } strs[] = {
        { FSQL_PROV_REASONING_PLUGIN, cfg->reasoning_plugin, "reasoning_plugin" },
        { FSQL_PROV_REASONING_URL,    cfg->reasoning_url,    "reasoning_url" },
        { FSQL_PROV_REASONING_TOKEN,  cfg->reasoning_token,  "reasoning_token" },
        { FSQL_PROV_REASONING_MODEL,  cfg->reasoning_model,  "reasoning_model" },
        { FSQL_PROV_EMBED_URL,        cfg->embed_url,        "embed_url" },
        { FSQL_PROV_EMBED_MODEL,      cfg->embed_model,      "embed_model" },
        { FSQL_PROV_THINK,            cfg->think,            "think" },
        { FSQL_PROV_THINK_PROVIDER,   cfg->think_provider,   "think_provider" },
        { FSQL_PROV_NATIVE_URL,       cfg->think_native_url, "think_native_url" },
        { FSQL_PROV_ENT_LIB,          cfg->enterprise_lib,          "enterprise_lib" },
        { FSQL_PROV_ENT_LEDGER_PATH,  cfg->enterprise_ledger_path,  "enterprise_ledger_path" },
        { FSQL_PROV_ENT_LEDGER_KEY,   cfg->enterprise_ledger_key,   "enterprise_ledger_key" },
    };
    for (size_t i = 0; i < sizeof(strs) / sizeof(strs[0]); i++) {
        if ((p & strs[i].bit) && strs[i].val[0] == '\0') {
            fsq_log("ERROR", "provider key %s: value must not be empty", strs[i].name);
            return -1;
        }
    }
    /* Path keys are checked for shape HERE, at configuration time, rather
     * than left to fail later at load time with no context. The rules
     * (absolute, no ".." segment) live in one place --
     * fractalsql_config.c's fractal_config_validate_path -- so the
     * daemon's config loader, the session overlay and the MCP's config
     * writer all refuse the same values for the same reasons.
     *
     * A relative path is refused because the daemon's working directory
     * is not a contract: the same conf file would name a different file
     * depending on how the daemon was started. A ".." segment is
     * refused because a path that walks out of its directory is a path
     * whose meaning is decided by whatever the operator's tree happens
     * to contain, not by the conf file. */
    {
        struct {
            uint32_t    bit;
            const char *val;
            const char *name;
        } paths[] = {
            { FSQL_PROV_REASONING_PLUGIN, cfg->reasoning_plugin,        "reasoning_plugin" },
            { FSQL_PROV_ENT_LIB,         cfg->enterprise_lib,          "enterprise_lib" },
            { FSQL_PROV_ENT_LEDGER_PATH, cfg->enterprise_ledger_path,  "enterprise_ledger_path" },
        };
        for (size_t i = 0; i < sizeof(paths) / sizeof(paths[0]); i++) {
            char err[256];
            if (!(p & paths[i].bit)) continue;
            if (fractal_config_validate_path(paths[i].val, err, sizeof err) != 0) {
                fsq_log("ERROR", "provider key %s: %s (got '%s')",
                        paths[i].name, err, paths[i].val);
                return -1;
            }
        }
    }
    /* Upper bound is generous (no real provider's context window comes
     * close), but a bound still beats none: an unchecked value here was
     * the one provider integer with no ceiling -- strtol saturates an absurd/overflowing config value
     * to LONG_MAX rather than erroring, and LONG_MAX > 1 passed the old
     * ">= 1"-only check. */
    if ((p & FSQL_PROV_NUM_CTX) &&
        (cfg->think_num_ctx < 1 || cfg->think_num_ctx > 10000000)) {
        fsq_log("ERROR", "provider key think_num_ctx: must be an integer in [1,10000000] (got %ld)", cfg->think_num_ctx);
        return -1;
    }
    return 0;
}

/* Push the provider settings out of a parsed config into the three
 * consumers (fractalsql_provider.h). Pointers reference cfg's fixed
 * buffers; the consumers own deep copies. CHECK first (read-only: any
 * refusal aborts before any consumer mutates), then APPLY -- but a
 * mid-apply consumer failure (allocation) leaves the earlier consumers
 * updated while the reload as a whole is refused; each consumer's own
 * rollback keeps even that partial state internally consistent. */
static int daemon_check_provider(const struct fsq_config *cfg) {
    struct fsq_provider_cfg pc;
    memset(&pc, 0, sizeof(pc));
    pc.present = cfg->prov_present;
    pc.reasoning_plugin = cfg->reasoning_plugin;
    pc.reasoning_url = cfg->reasoning_url;
    pc.reasoning_token = cfg->reasoning_token;
    pc.reasoning_model = cfg->reasoning_model;
    pc.reasoning_allow_plaintext = cfg->reasoning_allow_plaintext;
    pc.embed_url = cfg->embed_url;
    pc.embed_model = cfg->embed_model;
    pc.think = cfg->think;
    pc.think_provider = cfg->think_provider;
    pc.think_native_url = cfg->think_native_url;
    pc.think_num_ctx = cfg->think_num_ctx;
    pc.enterprise_lib = cfg->enterprise_lib;
    pc.enterprise_ledger_path = cfg->enterprise_ledger_path;
    pc.enterprise_ledger_key = cfg->enterprise_ledger_key;
    pc.enterprise_require_signature = cfg->enterprise_require_signature;

    if (validate_provider_cfg(cfg) != 0) return -1;
    if (fractalsql_cognition_check_provider(&pc) != 0 ||
        fractalsql_textsql_check_provider(&pc) != 0 ||
        fractalsql_enterprise_check_provider(&pc) != 0)
        return -1;
    return 0;
}

static int daemon_apply_provider(const struct fsq_config *cfg) {
    struct fsq_provider_cfg pc;
    char num_ctx_str[24];
    /* Keys whose value is on their provider face -- set names only,
     * never values: reasoning_token and enterprise_ledger_key are
     * plaintext credentials. */
    static const struct { uint32_t bit; const char *name; } keys[] = {
        { FSQL_PROV_REASONING_PLUGIN,    "reasoning_plugin" },
        { FSQL_PROV_REASONING_URL,       "reasoning_url" },
        { FSQL_PROV_REASONING_TOKEN,     "reasoning_token" },
        { FSQL_PROV_REASONING_MODEL,     "reasoning_model" },
        { FSQL_PROV_REASONING_PLAINTEXT, "reasoning_allow_plaintext" },
        { FSQL_PROV_EMBED_URL,           "embed_url" },
        { FSQL_PROV_EMBED_MODEL,         "embed_model" },
        { FSQL_PROV_THINK,               "think" },
        { FSQL_PROV_THINK_PROVIDER,      "think_provider" },
        { FSQL_PROV_NATIVE_URL,          "think_native_url" },
        { FSQL_PROV_NUM_CTX,             "think_num_ctx" },
        { FSQL_PROV_ENT_LIB,             "enterprise_lib" },
        { FSQL_PROV_ENT_LEDGER_PATH,     "enterprise_ledger_path" },
        { FSQL_PROV_ENT_LEDGER_KEY,      "enterprise_ledger_key" },
        { FSQL_PROV_ENT_REQUIRE_SIG,     "enterprise_require_signature" },
    };
    /* Zeroed, not left uninitialized: when prov_present has no bits set
     * (no provider keys configured at all -- the common case), the
     * loop below never calls snprintf even once, and the %s in the
     * fsq_log call at the end of this function would otherwise read
     * this buffer's uninitialized stack contents as a C string. */
    char names[384] = { 0 };
    size_t off = 0;

    memset(&pc, 0, sizeof(pc));
    pc.present = cfg->prov_present;
    /* (strings/bools/longs exactly as in daemon_check_provider) */
    pc.reasoning_plugin = cfg->reasoning_plugin;
    pc.reasoning_url = cfg->reasoning_url;
    pc.reasoning_token = cfg->reasoning_token;
    pc.reasoning_model = cfg->reasoning_model;
    pc.reasoning_allow_plaintext = cfg->reasoning_allow_plaintext;
    pc.embed_url = cfg->embed_url;
    pc.embed_model = cfg->embed_model;
    pc.think = cfg->think;
    pc.think_provider = cfg->think_provider;
    pc.think_native_url = cfg->think_native_url;
    pc.think_num_ctx = cfg->think_num_ctx;
    pc.enterprise_lib = cfg->enterprise_lib;
    pc.enterprise_ledger_path = cfg->enterprise_ledger_path;
    pc.enterprise_ledger_key = cfg->enterprise_ledger_key;
    pc.enterprise_require_signature = cfg->enterprise_require_signature;
    if (cfg->prov_present & FSQL_PROV_NUM_CTX) {
        snprintf(num_ctx_str, sizeof num_ctx_str, "%ld", cfg->think_num_ctx);
        pc.think_num_ctx_str = num_ctx_str;
    }

    if (fractalsql_cognition_apply_provider(&pc) != 0 ||
        fractalsql_textsql_apply_provider(&pc) != 0 ||
        fractalsql_enterprise_apply_provider(&pc) != 0)
        return -1;

    for (size_t i = 0; i < sizeof(keys) / sizeof(keys[0]); i++) {
        if (!(cfg->prov_present & keys[i].bit)) continue;
        int w = snprintf(names + off, sizeof(names) - off, "%s%s",
                         off ? ", " : "", keys[i].name);
        if (w < 0 || (size_t) w >= sizeof(names) - off) break;
        off += (size_t) w;
    }
    fsq_log("INFO", "provider settings applied (keys present: [%s], absent keys fall back to boot environment)", names);
    return 0;
}

/* ------------------------------------------------------------------ */
/* Frame I/O                                                           */
/* ------------------------------------------------------------------ */

static void fsq_set_idle(fsq_sock fd, int secs) {
    struct timeval idle;
    idle.tv_sec = secs;
    idle.tv_usec = 0;
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &idle, sizeof(idle));
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &idle, sizeof(idle));
}

static void fsq_close(fsq_sock fd) {
    close(fd);
}

static int read_all(fsq_sock fd, uint8_t *p, size_t n) {
    while (n > 0) {
        ssize_t r = recv(fd, p, n, 0);
        if (r < 0) { if (errno == EINTR) continue; return -1; }
        if (r == 0) return -1;
        p += r;
        n -= (size_t) r;
    }
    return 0;
}

/* MSG_NOSIGNAL (suppress SIGPIPE on a write to a peer that already
 * closed) is Linux-only. Darwin has no such send() flag; the socket
 * itself is set SO_NOSIGPIPE instead, once, where it is created
 * (open_listener's accept() below). */
#ifndef MSG_NOSIGNAL
#  define MSG_NOSIGNAL 0
#endif

static int write_all(fsq_sock fd, const uint8_t *p, size_t n) {
    while (n > 0) {
        ssize_t w = send(fd, p, n, MSG_NOSIGNAL);
        if (w < 0) { if (errno == EINTR) continue; return -1; }
        p += w;
        n -= (size_t) w;
    }
    return 0;
}

/* The frame tag: HMAC-SHA256 over the header and the payload, computed without copying either (no allocation, so it cannot
 * fail; it used to build a joined copy, and on allocation failure zeroed the tag, which made an all-zero tag valid). */
static void mac(const struct fsq_config *cfg, const uint8_t *hdr,
                const uint8_t *payload, size_t plen, uint8_t tag[FSQ_TAG_LEN]) {
    fsq_hmac_sha256_2(cfg->key, cfg->key_len, hdr, FSQ_HEADER_LEN, payload, plen, tag);
}

/* Reads the declared payload before the frame is authenticated, so the buffer grows only as bytes actually arrive (1 MiB at a
 * time): a peer that declares 80 MiB and sends nothing, or a little, makes the daemon hold what it sent, not what it claimed. */
static int read_payload(fsq_sock fd, uint32_t len, uint8_t **out) {
    const size_t step = 1u << 20;
    uint8_t *buf = NULL;
    size_t have = 0;
    while (have < len) {
        size_t want = len - have < step ? len - have : step;
        uint8_t *nb = realloc(buf, have + want);
        if (!nb) { free(buf); return -1; }
        buf = nb;
        if (read_all(fd, buf + have, want) != 0) { free(buf); return -1; }
        have += want;
    }
    *out = buf;
    return 0;
}

static int send_reply(fsq_sock fd, const struct fsq_config *cfg, const struct fsq_header *req,
                      const struct fsq_reply *rep) {
    uint32_t plen = 8 + rep->body_len;
    uint8_t *payload = malloc(plen);
    uint8_t hdr[FSQ_HEADER_LEN], tag[FSQ_TAG_LEN];
    struct fsq_header h;
    int rc;
    if (!payload) return -1;
    fsq_put_u32(payload, rep->status);
    fsq_put_u32(payload + 4, (uint32_t) rep->core_rc);
    if (rep->body_len) memcpy(payload + 8, rep->body, rep->body_len);

    memset(&h, 0, sizeof(h));
    h.version = FSQ_PROTOCOL_VERSION;
    h.opcode = req->opcode;
    h.flags = FSQ_FLAG_RESPONSE | (rep->status != FSQ_OK ? FSQ_FLAG_ERROR : 0);
    h.request_id = req->request_id;
    h.context = req->context;
    h.auth_len = FSQ_TAG_LEN;
    h.payload_len = plen;
    fsq_header_encode(hdr, &h);
    mac(cfg, hdr, payload, plen, tag);
    rc = write_all(fd, hdr, FSQ_HEADER_LEN) || write_all(fd, tag, FSQ_TAG_LEN) ||
         write_all(fd, payload, plen);
    free(payload);
    return rc ? -1 : 0;
}

static void set_text(struct fsq_reply *rep, uint32_t status, const char *text) {
    size_t n = strlen(text);
    if (n >= FSQ_MSG_MAX) n = FSQ_MSG_MAX - 1;
    rep->status = status;
    /* NUL-terminated (body_len still excludes it, wire format unchanged):
     * serve_connection logs a failed request's body with "%.200s", which
     * reads past the end of a buffer with no NUL to find. */
    rep->body = malloc(n + 1);
    if (!rep->body) { rep->body_len = 0; return; }
    memcpy(rep->body, text, n);
    rep->body[n] = '\0';
    rep->body_len = (uint32_t) n;
}

/* ------------------------------------------------------------------ */
/* Function table: generated by scripts/gen_udf.py                      */
/* ------------------------------------------------------------------ */

enum fsq_kind { FSQ_KIND_CTL, FSQ_KIND_STR, FSQ_KIND_INT, FSQ_KIND_REAL };

struct fsq_fn {
    const char *name;
    enum fsq_kind kind;
    void *init;
    void *deinit;
    void *main;
};

#include "daemon_udfs.h"

typedef char *(*fsq_str_fn)(UDF_INIT *, UDF_ARGS *, char *, unsigned long *, char *, char *);
typedef long long (*fsq_int_fn)(UDF_INIT *, UDF_ARGS *, char *, char *);
typedef double (*fsq_real_fn)(UDF_INIT *, UDF_ARGS *, char *, char *);
typedef void (*fsq_deinit_fn)(UDF_INIT *);

static const struct fsq_fn *find_fn(const char *name, uint32_t len) {
    for (size_t i = 0; i < sizeof(g_fn_table) / sizeof(g_fn_table[0]); i++)
        if (strlen(g_fn_table[i].name) == len && memcmp(g_fn_table[i].name, name, len) == 0)
            return &g_fn_table[i];
    return NULL;
}

/* ------------------------------------------------------------------ */
/* Handles: one per statement, from UDF_INIT until UDF_DEINIT.          */
/* The adapter's init may park state in initid->ptr, so the UDF_INIT   */
/* and UDF_ARGS for a statement live here until its deinit.            */
/* ------------------------------------------------------------------ */

struct fsq_handle {
    uint64_t id;
    const struct fsq_fn *fn;
    UDF_INIT init;
    UDF_ARGS args;
    /* Owned copies of constant argument values, visible to init only. */
    char *const_buf[FSQ_MAX_ARGS];
    uint64_t owner;     /* connection that created the handle */
    /* CANCEL's flag (opcodes.def): read lock-free by the running call
     * itself and by the interrupt-hooked loops, written under
     * g_handles_mu (or by the atomic on the between-calls checkpoint). */
    fsq_atomic_int cancelled;
    struct fsq_handle *next;
};

static fsq_mutex g_handles_mu = FSQ_MUTEX_INIT;
static FSQ_TLS uint64_t t_peer_uid = 0;   /* the authenticated uid of the connection's peer (peer_allowed sets it) */
static FSQ_TLS uint64_t t_conn = 0;   /* connection serving this thread */
/* The handle the interrupt-hooked computation loops run for, if any. The
 * loops are called on the same thread that serves the UDF_CALL, so the
 * hook reads this TLS pointer and its cancel flag (with no active call --
 * e.g. during INIT/DEINIT body execution -- or before the hook is
 * installed, polls report "not interrupted"). */
static FSQ_TLS struct fsq_handle *t_active_handle = NULL;
static struct fsq_handle *g_handles = NULL;
static uint64_t g_next_handle = 1;

/* The interruption hook installed at startup (src/fractalsql_interrupt.h):
 * nonzero aborts the polling loop. This is what makes CANCEL from another
 * connection reach a busy in-repo computation without any signal from the
 * caller. */
static int fsq_check_cancelled(void) {
    struct fsq_handle *h = t_active_handle;
    return h ? FSQ_ATOMIC_GET(&h->cancelled) : 0;
}

static enum Item_result wire_to_item(uint8_t t) {
    switch (t) {
        case FSQ_T_INT:     return INT_RESULT;
        case FSQ_T_REAL:    return REAL_RESULT;
        case FSQ_T_DECIMAL: return DECIMAL_RESULT;
        default:            return STRING_RESULT;
    }
}

static uint8_t item_to_wire(enum Item_result t) {
    switch (t) {
        case INT_RESULT:     return FSQ_T_INT;
        case REAL_RESULT:    return FSQ_T_REAL;
        case DECIMAL_RESULT: return FSQ_T_DECIMAL;
        default:             return FSQ_T_STRING;
    }
}

static void handle_free_args(struct fsq_handle *h) {
    for (uint32_t i = 0; i < FSQ_MAX_ARGS; i++) {
        free(h->const_buf[i]);
        h->const_buf[i] = NULL;
    }
    free(h->args.arg_type);
    free(h->args.args);
    free(h->args.lengths);
    free(h->args.maybe_null);
    h->args.arg_type = NULL;
    h->args.args = NULL;
    h->args.lengths = NULL;
    h->args.maybe_null = NULL;
}

/* Handles are sequential, so IDs are guessable. A connection may only reach
 * handles it created: a foreign ID is "not found", never a use-after-free
 * of another connection's statement. */
static struct fsq_handle *handle_find_locked(uint64_t id) {
    for (struct fsq_handle *h = g_handles; h; h = h->next)
        if (h->id == id && h->owner == t_conn) return h;
    return NULL;
}

struct fsq_arg {
    uint8_t type;
    uint8_t is_null;
    const uint8_t *bytes;
    uint32_t len;
};

static int parse_args(const uint8_t *p, uint32_t n, uint32_t nargs, struct fsq_arg *out) {
    uint32_t off = 0;
    for (uint32_t i = 0; i < nargs; i++) {
        uint32_t len;
        if (n - off < 6) return -1;
        out[i].type = p[off];
        out[i].is_null = p[off + 1];
        len = fsq_get_u32(p + off + 2);
        off += 6;
        if (n - off < len) return -1;
        out[i].len = len;
        out[i].bytes = p + off;
        off += len;
    }
    return off == n ? 0 : -1;
}

static void handle_init(const uint8_t *p, uint32_t n, struct fsq_reply *rep) {
    uint8_t namelen;
    const char *name;
    uint32_t nargs;
    const struct fsq_fn *f;
    struct fsq_handle *h;
    char msg[FSQ_MSG_MAX];
    bool failed;
    uint8_t *body;
    struct fsq_arg in[FSQ_MAX_ARGS];

    /* namelen u8, name[namelen], nargs u32, TLV args. */
    if (n < 1) { set_text(rep, FSQ_ERR_ARGS, "malformed init request"); return; }
    namelen = p[0];
    if (n < 1u + namelen + 4u) { set_text(rep, FSQ_ERR_ARGS, "malformed init request"); return; }
    name = (const char *) (p + 1);
    nargs = fsq_get_u32(p + 1 + namelen);
    if (nargs > FSQ_MAX_ARGS) { set_text(rep, FSQ_ERR_ARGS, "malformed init request"); return; }
    if (parse_args(p + 1 + namelen + 4, n - (1u + namelen + 4u), nargs, in) != 0) {
        set_text(rep, FSQ_ERR_ARGS, "malformed init request");
        return;
    }
    f = find_fn(name, namelen);
    if (!f) { set_text(rep, FSQ_ERR_NOTSUP, "unknown function"); return; }

    h = calloc(1, sizeof(*h));
    if (!h) { set_text(rep, FSQ_ERR_INTERNAL, "out of memory"); return; }
    h->fn = f;
    h->args.arg_count = nargs;
    h->args.arg_type = calloc(nargs ? nargs : 1, sizeof(enum Item_result));
    h->args.args = calloc(nargs ? nargs : 1, sizeof(char *));
    h->args.lengths = calloc(nargs ? nargs : 1, sizeof(unsigned long));
    h->args.maybe_null = calloc(nargs ? nargs : 1, 1);
    if (!h->args.arg_type || !h->args.args || !h->args.lengths || !h->args.maybe_null) {
        handle_free_args(h);
        free(h);
        set_text(rep, FSQ_ERR_INTERNAL, "out of memory");
        return;
    }
    for (uint32_t i = 0; i < nargs; i++) {
        enum Item_result t = wire_to_item(in[i].type);
        h->args.arg_type[i] = t;
        if (in[i].is_null) continue;
        /* A constant argument's value is visible to init in the host database, and the
         * adapter validates on it (for example fractal_search's k bounds). */
        h->const_buf[i] = malloc((size_t) in[i].len + 1);
        if (!h->const_buf[i]) {
            handle_free_args(h);
            free(h);
            set_text(rep, FSQ_ERR_INTERNAL, "out of memory");
            return;
        }
        if (t == INT_RESULT) {
            long long v;
            if (in[i].len != 8) { handle_free_args(h); free(h); set_text(rep, FSQ_ERR_ARGS, "malformed init request"); return; }
            v = (long long) fsq_get_u64(in[i].bytes);
            memcpy(h->const_buf[i], &v, 8);
            h->args.lengths[i] = 8;
        } else if (t == REAL_RESULT) {
            uint64_t bits;
            double d;
            if (in[i].len != 8) { handle_free_args(h); free(h); set_text(rep, FSQ_ERR_ARGS, "malformed init request"); return; }
            bits = fsq_get_u64(in[i].bytes);
            memcpy(&d, &bits, 8);
            memcpy(h->const_buf[i], &d, 8);
            h->args.lengths[i] = 8;
        } else {
            memcpy(h->const_buf[i], in[i].bytes, in[i].len);
            h->const_buf[i][in[i].len] = '\0';
            h->args.lengths[i] = in[i].len;
        }
        h->args.args[i] = h->const_buf[i];
    }

    msg[0] = '\0';
    failed = ((bool (*)(UDF_INIT *, UDF_ARGS *, char *)) f->init)(&h->init, &h->args, msg);
    if (failed) {
        handle_free_args(h);
        free(h);
        set_text(rep, FSQ_ERR_ARGS, msg[0] ? msg : "initialization failed");
        return;
    }

    /* The handle id is allocated first (it goes into the reply body),
     * but the handle is only LINKED into the registry once the body
     * has been allocated. Registering it and then failing left a handle
     * in the registry whose id the caller never learned, with the
     * adapter's init already run: its resources needed a deinit that
     * nothing would ever call, for as long as the connection lived. */
    FSQ_LOCK(g_handles_mu);
    h->id = g_next_handle++;
    FSQ_UNLOCK(g_handles_mu);

    /* Body: handle u64, max_length u32, maybe_null u8, nargs u32, types[nargs] u8. */
    rep->status = FSQ_OK;
    rep->body_len = 17 + nargs;
    body = calloc(rep->body_len, 1);
    if (!body) {
        ((fsq_deinit_fn) h->fn->deinit)(&h->init);
        handle_free_args(h);
        free(h);
        set_text(rep, FSQ_ERR_INTERNAL, "out of memory");
        return;
    }
    fsq_put_u64(body, h->id);
    fsq_put_u32(body + 8, (uint32_t) h->init.max_length);
    body[12] = h->init.maybe_null ? 1 : 0;
    fsq_put_u32(body + 13, nargs);
    for (uint32_t i = 0; i < nargs; i++) body[17 + i] = item_to_wire(h->args.arg_type[i]);
    rep->body = body;

    FSQ_LOCK(g_handles_mu);
    h->owner = t_conn;
    h->next = g_handles;
    g_handles = h;
    FSQ_UNLOCK(g_handles_mu);
}

static void tlv_result(struct fsq_reply *rep, uint8_t type, int is_null,
                       const uint8_t *bytes, uint32_t len) {
    rep->status = FSQ_OK;
    rep->body_len = 6 + len;
    rep->body = malloc(rep->body_len);
    if (!rep->body) { rep->body_len = 0; set_text(rep, FSQ_ERR_INTERNAL, "out of memory"); return; }
    rep->body[0] = type;
    rep->body[1] = (uint8_t) (is_null ? 1 : 0);
    fsq_put_u32(rep->body + 2, len);
    if (len) memcpy(rep->body + 6, bytes, len);
}

/* A CANCEL that arrived while this call was running is what aborted
 * it: surface the cancellation instead of a bare adapter error, and
 * consume the one-shot flag (so `h` must be writable), so the
 * statement's next row starts fresh (the between-calls checkpoint
 * consumes identically). */
static void core_fail(struct fsq_reply *rep, struct fsq_handle *h) {
    if (FSQ_ATOMIC_GET(&h->cancelled)) {
        FSQ_ATOMIC_SET(&h->cancelled, 0);
        fsq_log("INFO", "%s: cancelled mid-call", h->fn->name);
        set_text(rep, FSQ_ERR_BUSY, "cancelled");
        return;
    }
    fsq_log("WARN", "%s: adapter reported an error for this call", h->fn->name);
    set_text(rep, FSQ_ERR_CORE, "");
}

/* ------------------------------------------------------------------ */
/* Idempotency response cache (UDF_CALL, token-keyed)                   */
/* ------------------------------------------------------------------ */

/* A UDF_CALL whose header context u64 is nonzero carries the shim's
 * idempotency token; when the transport drops mid-flight (or the HMAC
 * check fails on the reply), the shim resends the SAME token and the
 * daemon answers from here instead of re-executing -- which is the only
 * safe move for stateful functions (ledger loads, diversify toggles...).
 * The cache is cross-connection on purpose: the resend nearly always
 * arrives on a NEW connection because the old socket is the thing that
 * died, so no per-connection map would ever be hit.
 *
 * Keyed by the token alone. Only FSQ_OK and FSQ_ERR_CORE responses are
 * cached: every other status is an effect-free rejection (ARGS/BUSY/
 * LIMIT/NOTSUP) that simply re-running the call reproduces. Token 0 is
 * never cached -- it is not a token.
 *
 * The ring is a fixed FIFO of FSQ_CACHE_ENTRIES slots under one mutex;
 * the aggregate byte cap bounds memory. This is process memory only: a
 * daemon restart loses the cache, and the shim's CSPRNG token base makes
 * any token from a previous the host server boot unable to reach a stale entry. */

#define FSQ_CACHE_ENTRIES   256
#define FSQ_CACHE_MAX_BYTES ((uint64_t) 64u * 1024u * 1024u)
#define FSQ_CACHE_ENTRY_MAX ((uint32_t) 8u * 1024u * 1024u)

struct fsq_cache_entry {
    uint64_t token;
    uint64_t peer_uid;  /* the authenticated uid that executed the call: a replay is for that uid only */
    uint32_t status;
    int32_t  core_rc;
    uint32_t body_len;
    uint8_t *body;      /* NULL when the slot is empty */
    /* The function the cached reply belongs to. The token alone is
     * enough to identify a resend (the shim mints one per logical
     * call), but the name is checked as well so a token that somehow
     * arrives with a different function cannot be answered with
     * another function's result. */
    uint8_t  name_len;
    char     name[64];
};

static fsq_mutex g_cache_mu = FSQ_MUTEX_INIT;
static struct fsq_cache_entry g_cache[FSQ_CACHE_ENTRIES];
static size_t g_cache_next;     /* oldest slot; the next eviction/insertion target */
static uint64_t g_cache_bytes;

static struct fsq_cache_entry *cache_find_locked(uint64_t token) {
    for (size_t i = 0; i < FSQ_CACHE_ENTRIES; i++)
        if (g_cache[i].body && g_cache[i].token == token) return &g_cache[i];
    return NULL;
}

/* Returns 1 and fills rep when token has a cached response. The body is
 * cloned: rep->body is the caller's to free like any dispatch result.
 * An out-of-memory clone is reported as a miss -- the call re-executes,
 * which the retry classes say is safe for everything that got this far.
 * A token that belongs to a DIFFERENT function is a miss too: the reply
 * belongs to the call that was made, not to the token alone. */
static int cache_replay(uint64_t token, struct fsq_reply *rep,
                        const char *name, uint8_t name_len) {
    struct fsq_cache_entry *e;
    uint32_t status, len;
    int32_t core_rc;
    uint8_t *body;
    FSQ_LOCK(g_cache_mu);
    e = cache_find_locked(token);
    if (!e) { FSQ_UNLOCK(g_cache_mu); return 0; }
    /* A token is replayed to the principal that minted it, not to any peer that presents the number. The scope is the peer's
     * uid and not the connection, because a client replays across a reconnect. */
    if (e->peer_uid != t_peer_uid ||
        e->name_len != name_len || (name_len && memcmp(e->name, name, name_len) != 0)) {
        FSQ_UNLOCK(g_cache_mu);
        return 0;
    }
    status = e->status; core_rc = e->core_rc; len = e->body_len;
    body = malloc(len ? len : 1);
    if (body && len) memcpy(body, e->body, len);
    FSQ_UNLOCK(g_cache_mu);
    if (!body) return 0;
    rep->status = status;
    rep->core_rc = core_rc;
    rep->body = body;
    rep->body_len = len;
    fsq_log("INFO", "UDF_CALL replayed from the idempotency cache (token %llu, %u bytes)",
            (unsigned long long) token, len);
    return 1;
}

static void cache_put(uint64_t token, const struct fsq_reply *rep,
                      const char *name, uint8_t name_len) {
    uint32_t len = rep->body_len;
    uint8_t *copy;
    struct fsq_cache_entry *slot;
    if (token == 0 || len > FSQ_CACHE_ENTRY_MAX) return;
    /* A name too long for the slot's field cannot be remembered, and a
     * reply that cannot be matched to its function is not safe to
     * replay -- so such a call is simply not cached. Every name in the
     * daemon's table is far shorter than the field. */
    if (name_len == 0 || name_len >= sizeof slot->name) return;
    copy = malloc(len ? len : 1);
    if (!copy) return;
    if (len) memcpy(copy, rep->body, len);

    FSQ_LOCK(g_cache_mu);
    if ((slot = cache_find_locked(token)) != NULL) {
        /* Same token answered again outside a retry (an operator re-sent
         * it deliberately): replace in place, ring order untouched. The
         * aggregate cap may transiently exceed FSQ_CACHE_MAX_BYTES here;
         * the next fresh insert's eviction loop trims it. */
        g_cache_bytes -= slot->body_len;
        free(slot->body);
    } else {
        /* Evict oldest-first until the aggregate cap admits the new body.
         * len is bounded by the 8 MiB entry cap against a 64 MiB total, so
         * emptying the ring always leaves room; the scan bound is a belt-
         * and-braces guard against an accounting slip turning this into a
         * spin, in which case the insert is skipped below. */
        for (size_t scanned = 0;
             g_cache_bytes + len > FSQ_CACHE_MAX_BYTES && scanned <= FSQ_CACHE_ENTRIES;
             scanned++) {
            struct fsq_cache_entry *ev = &g_cache[g_cache_next];
            if (ev->body) {
                g_cache_bytes -= ev->body_len;
                free(ev->body);
                ev->body = NULL;
            }
            g_cache_next = (g_cache_next + 1) % FSQ_CACHE_ENTRIES;
        }
        slot = &g_cache[g_cache_next];
        if (g_cache_bytes + len > FSQ_CACHE_MAX_BYTES) {
            /* The scan bound was exhausted and the aggregate cap still
             * does not admit the new body: an accounting slip. Skip the
             * insert rather than exceed the cap -- the comment above
             * promises exactly this, and the code did not do it. */
            FSQ_UNLOCK(g_cache_mu);
            free(copy);
            return;
        }
        if (slot->body) {
            g_cache_bytes -= slot->body_len;
            free(slot->body);
        }
        g_cache_next = (g_cache_next + 1) % FSQ_CACHE_ENTRIES;
    }
    slot->token = token;
    slot->peer_uid = t_peer_uid;
    slot->status = rep->status;
    slot->core_rc = rep->core_rc;
    slot->body_len = len;
    slot->body = copy;
    slot->name_len = name_len;
    memcpy(slot->name, name, name_len);
    g_cache_bytes += len;
    FSQ_UNLOCK(g_cache_mu);
}

static void handle_call(uint64_t token, const uint8_t *p, uint32_t n, struct fsq_reply *rep) {
    uint8_t namelen;
    const char *name;
    uint64_t id;
    uint32_t nargs;
    struct fsq_handle *h;
    struct fsq_arg in[FSQ_MAX_ARGS];
    long long ivals[FSQ_MAX_ARGS];
    double rvals[FSQ_MAX_ARGS];
    char *bufs[FSQ_MAX_ARGS];
    uint32_t i;
    char is_null = 0, err = 0;

    memset(bufs, 0, sizeof(bufs));
    /* namelen u8, name[namelen], handle u64, nargs u32, TLV args. */
    if (n < 1) { set_text(rep, FSQ_ERR_ARGS, ""); return; }
    namelen = p[0];
    if (n < 1u + namelen + 12u) { set_text(rep, FSQ_ERR_ARGS, ""); return; }
    name = (const char *) (p + 1);
    id = fsq_get_u64(p + 1 + namelen);
    nargs = fsq_get_u32(p + 1 + namelen + 8);
    if (nargs > FSQ_MAX_ARGS ||
        parse_args(p + 1 + namelen + 12, n - (1u + namelen + 12u), nargs, in) != 0) {
        set_text(rep, FSQ_ERR_ARGS, "");
        return;
    }

    /* Idempotent resend: a stateful call's response, answered from the
     * cache rather than re-executed. This check comes BEFORE the handle
     * lookup, and that order is the whole point: the resend arrives on
     * some other, newer connection (the old socket is the thing that
     * died), and a handle belongs to the connection that created it --
     * the daemon frees a connection's handles when it closes. So the
     * handle the resend carries is GONE by the time the resend lands,
     * and the handle gate below would refuse the call with
     * FSQ_ERR_ARGS: the retry could never be answered, and for a
     * stateful function the side effect had already happened, so the
     * caller was told "failed" about work that was really done. The
     * cache is keyed by the token (minted once per logical call), and
     * the function name is checked as well, so a replay can only
     * answer the call it belongs to. */
    if (token != 0 && cache_replay(token, rep, name, namelen)) return;

    FSQ_LOCK(g_handles_mu);
    h = handle_find_locked(id);
    FSQ_UNLOCK(g_handles_mu);
    if (!h || strlen(h->fn->name) != namelen || memcmp(h->fn->name, name, namelen) != 0 ||
        (uint32_t) h->args.arg_count != nargs) {
        set_text(rep, FSQ_ERR_ARGS, "");
        return;
    }

    /* Between-calls cancellation checkpoint: a row-by-row statement
     * reuses this handle across many UDF_CALLs, so refusing to start the
     * next one here stops the remaining rows without touching the core. A
     * call already running when CANCEL arrived is instead stopped by the
     * interrupt-hooked loops (see opcodes.def's CANCEL entry). */
    if (FSQ_ATOMIC_GET(&h->cancelled)) {
        FSQ_ATOMIC_SET(&h->cancelled, 0);
        set_text(rep, FSQ_ERR_BUSY, "cancelled");
        return;
    }

    for (i = 0; i < nargs; i++) {
        enum Item_result t = h->args.arg_type[i];
        h->args.lengths[i] = 0;
        h->args.maybe_null[i] = 0;
        if (in[i].is_null) { h->args.args[i] = NULL; continue; }
        if (t == INT_RESULT) {
            if (in[i].len != 8) { set_text(rep, FSQ_ERR_ARGS, ""); goto done; }
            ivals[i] = (long long) fsq_get_u64(in[i].bytes);
            h->args.args[i] = (char *) &ivals[i];
            h->args.lengths[i] = 8;
        } else if (t == REAL_RESULT) {
            uint64_t bits;
            if (in[i].len != 8) { set_text(rep, FSQ_ERR_ARGS, ""); goto done; }
            bits = fsq_get_u64(in[i].bytes);
            memcpy(&rvals[i], &bits, 8);
            h->args.args[i] = (char *) &rvals[i];
            h->args.lengths[i] = 8;
        } else {
            bufs[i] = malloc((size_t) in[i].len + 1);
            if (!bufs[i]) { set_text(rep, FSQ_ERR_INTERNAL, ""); goto done; }
            if (in[i].len) memcpy(bufs[i], in[i].bytes, in[i].len);
            bufs[i][in[i].len] = '\0';
            h->args.args[i] = bufs[i];
            h->args.lengths[i] = in[i].len;
        }
    }

    /* The call itself starts here: from this point the interrupt hooks in
     * the in-repo loops (src/fractalsql_interrupt.h) consult THIS handle's
     * cancel flag for the duration. */
    t_active_handle = h;
    if (h->fn->kind == FSQ_KIND_STR) {
        size_t cap = (size_t) h->init.max_length + 1;
        char *result = malloc(cap);
        unsigned long len = 0;
        char *ret;
        if (!result) { set_text(rep, FSQ_ERR_INTERNAL, ""); goto done; }
        ret = ((fsq_str_fn) h->fn->main)(&h->init, &h->args, result, &len, &is_null, &err);
        if (err) {
            core_fail(rep, h);
        } else if (is_null) {
            tlv_result(rep, FSQ_T_STRING, 1, NULL, 0);
        } else if (len > h->init.max_length) {
            set_text(rep, FSQ_ERR_LIMIT, "");
        } else {
            tlv_result(rep, FSQ_T_STRING, 0, (const uint8_t *) (ret ? ret : result), (uint32_t) len);
        }
        free(result);
    } else if (h->fn->kind == FSQ_KIND_INT) {
        long long v = ((fsq_int_fn) h->fn->main)(&h->init, &h->args, &is_null, &err);
        uint8_t out[8];
        if (err) { core_fail(rep, h); }
        else {
            fsq_put_u64(out, (uint64_t) v);
            tlv_result(rep, FSQ_T_INT, is_null, out, 8);
        }
    } else if (h->fn->kind == FSQ_KIND_REAL) {
        double v = ((fsq_real_fn) h->fn->main)(&h->init, &h->args, &is_null, &err);
        uint64_t bits;
        uint8_t out[8];
        if (err) { core_fail(rep, h); }
        else {
            memcpy(&bits, &v, 8);
            fsq_put_u64(out, bits);
            tlv_result(rep, FSQ_T_REAL, is_null, out, 8);
        }
    } else {
        set_text(rep, FSQ_ERR_NOTSUP, "");
    }

    /* A replayable response outcome: remember it under this call's token.
     * FSQ_ERR_CORE is cached too (a stateful function erroring twice should
     * stay one execution); effect-free statuses are not. */
    if (token != 0 && (rep->status == FSQ_OK || rep->status == FSQ_ERR_CORE))
        cache_put(token, rep, h->fn->name, (uint8_t) strlen(h->fn->name));

done:
    t_active_handle = NULL;
    for (i = 0; i < nargs; i++) {
        free(bufs[i]);
        /* The per-call argument values are valid only for the duration of
         * the call: the string buffers are freed right here, and the
         * numeric ones point into this frame's stack. Leaving those
         * pointers in the handle would leave it holding freed memory that
         * the next reader would take for a live argument value. */
        h->args.args[i] = NULL;
        h->args.lengths[i] = 0;
    }
}

static void handle_deinit(const uint8_t *p, uint32_t n, struct fsq_reply *rep) {
    uint64_t id;
    struct fsq_handle **link, *h = NULL;
    if (n < 8) { set_text(rep, FSQ_ERR_ARGS, ""); return; }
    id = fsq_get_u64(p);
    FSQ_LOCK(g_handles_mu);
    for (link = &g_handles; *link; link = &(*link)->next) {
        if ((*link)->id == id && (*link)->owner == t_conn) { h = *link; *link = h->next; break; }
    }
    FSQ_UNLOCK(g_handles_mu);
    if (h) {
        ((fsq_deinit_fn) h->fn->deinit)(&h->init);
        handle_free_args(h);
        free(h);
    }
    set_text(rep, FSQ_OK, "");
}

/* Unlike handle_find_locked, this does not restrict by owner: the whole
 * point is cancelling a statement from a different connection (an operator
 * on an admin connection, cancelling a stuck the host server backend's handle) --
 * the owning connection's thread is the one busy inside the call. Handle
 * ids are already sequential and guessable by design (see
 * handle_find_locked's comment); the worst a wrong guess does here is make
 * someone else's next row fail with ERR_BUSY, not read their data. */
static void handle_cancel(const uint8_t *p, uint32_t n, struct fsq_reply *rep) {
    uint64_t id;
    struct fsq_handle *h, *target = NULL;
    if (n < 8) { set_text(rep, FSQ_ERR_ARGS, ""); return; }
    id = fsq_get_u64(p);
    FSQ_LOCK(g_handles_mu);
    for (h = g_handles; h; h = h->next) {
        if (h->id == id) { target = h; FSQ_ATOMIC_SET(&target->cancelled, 1); break; }
    }
    FSQ_UNLOCK(g_handles_mu);
    set_text(rep, target ? FSQ_OK : FSQ_ERR_ARGS, target ? "" : "unknown handle");
}

/* RELOAD (fsqlctl reload; opcodes.def): re-parse the config file into a
 * fresh snapshot and swap it in. All-or-nothing: a failure at any point
 * below leaves the running configuration untouched. socket_path and
 * hmac_key_file changes are refused -- the shim reads its side once, at
 * first call, so applied changes there would strand every running
 * the host server; re-reading the same key file picks up a rotated key, which
 * breaks clients whose connections still use the old key until they
 * reconnect. The provider keys (reasoning/embed/think/t2s/enterprise,
 * fractalsql_provider.h) ARE reloadable live: validated, checked, then
 * pushed into the src/ consumers before the snapshot publishes. */
static void handle_reload(const struct fsq_config *cur, struct fsq_reply *rep) {
    struct fsq_config next;
    if (load_config(g_config_path, &next) != 0) {
        fsq_log("WARN", "reload: re-reading %s failed; keeping the running configuration",
                g_config_path ? g_config_path : "(unset)");
        set_text(rep, FSQ_ERR_ARGS, "reload failed: config file unreadable");
        return;
    }
    if (strcmp(next.socket_path, cur->socket_path) != 0 ||
        strcmp(next.key_path, cur->key_path) != 0) {
        fsq_log("WARN", "reload: socket_path/hmac_key_file changed in %s; that requires a restart",
                g_config_path ? g_config_path : "(unset)");
        set_text(rep, FSQ_ERR_ARGS, "socket_path and hmac_key_file changes require a restart");
        return;
    }
    if (validate_cfg(&next) != 0) {
        fsq_log("WARN", "reload: the new configuration would be refused at startup; keeping the running configuration");
        set_text(rep, FSQ_ERR_ARGS, "reload failed: key/config file permissions rejected");
        return;
    }
    /* Provider settings: loud daemon-side validation, then the consumers'
     * cross-state checks (read-only), all BEFORE anything is rebuilt or
     * applied below. A refusal here keeps the running configuration. */
    if (daemon_check_provider(&next) != 0) {
        fsq_log("WARN", "reload: provider settings rejected; keeping the running configuration");
        set_text(rep, FSQ_ERR_ARGS, "reload failed: provider settings rejected (see the daemon log)");
        return;
    }
    /* POSIX logs to stderr (the container runtime collects it); log_file
     * is parsed into the config but carries no runtime effect here. */
    /* Apply the provider push AFTER the SD/log swap, BEFORE publishing:
     * each consumer swaps its effective settings under its own lock (with
     * per-consumer rollback) and clears its tiers' plugin-loaded flags;
     * a new/changed plugin or enterprise lib path is loaded on the tier's
     * NEXT call, naming its path in that call's error if it fails -- a
     * reload itself never attempts a load. */
    if (daemon_apply_provider(&next) != 0) {
        fsq_log("WARN", "reload: applying provider settings failed; keeping the running configuration");
        set_text(rep, FSQ_ERR_ARGS, "reload failed: applying provider settings (see the daemon log)");
        return;
    }
    if (cfg_publish(&next) != 0) {
        set_text(rep, FSQ_ERR_INTERNAL, "out of memory");
        return;
    }
    fsq_log("INFO", "reload: configuration reloaded (max_connections=%d, idle_timeout_secs=%d, allowed=%zu)",
            next.max_connections, next.idle_timeout_secs, next.n_allowed);
    set_text(rep, FSQ_OK, "reloaded");
}

static void dispatch(const struct fsq_config *cfg, const struct fsq_header *h,
                     const uint8_t *payload, uint32_t plen, struct fsq_reply *rep) {
    memset(rep, 0, sizeof(*rep));
    switch (h->opcode) {
        case FSQ_OP_PING: {
            uint8_t v[2];
            fsq_put_u16(v, FSQ_PROTOCOL_VERSION);
            rep->status = FSQ_OK;
            rep->body = malloc(2);
            if (rep->body) { memcpy(rep->body, v, 2); rep->body_len = 2; }
            return;
        }
        case FSQ_OP_VERSION: {
            char text[64];
            snprintf(text, sizeof(text), "fractalsqld %s protocol %u", FSQL_DAEMON_VERSION, FSQ_PROTOCOL_VERSION);
            set_text(rep, FSQ_OK, text);
            return;
        }
        case FSQ_OP_UDF_INIT:
            handle_init(payload, plen, rep);
            return;
        case FSQ_OP_UDF_CALL:
            handle_call(h->context, payload, plen, rep);
            return;
        case FSQ_OP_UDF_DEINIT:
            handle_deinit(payload, plen, rep);
            return;
        case FSQ_OP_CANCEL:
            handle_cancel(payload, plen, rep);
            return;
        case FSQ_OP_RELOAD:
            /* RELOAD is a payload-less request: anything attached is a
             * malformed reload, refused without touching the running
             * config (future revisions could carry options here). */
            if (plen != 0) { set_text(rep, FSQ_ERR_ARGS, "RELOAD takes no payload"); return; }
            handle_reload(cfg, rep);
            return;
        default:
            set_text(rep, FSQ_ERR_NOTSUP, "");
            return;
    }
}

/* ------------------------------------------------------------------ */
/* Connections                                                         */
/* ------------------------------------------------------------------ */

struct conn_arg {
    fsq_sock fd;
    /* No config pointer here: the serving thread borrows the current
     * snapshot itself on entry (see serve_connection). */
};

/* Fills peer with a log-safe description of the caller and reports whether
 * the caller is on the allow-list, against allowed_uids. Linux checks
 * SO_PEERCRED/struct ucred; Darwin has neither, so it uses getpeereid()
 * instead. */
static int peer_allowed(fsq_sock fd, const struct fsq_config *cfg, char *peer, size_t peerlen) {
#if defined(__APPLE__)
    /* Darwin has no SO_PEERCRED/struct ucred (that pair is Linux-only).
     * getpeereid() is the BSD equivalent: it reads the credentials of
     * the process on the other end of a connected AF_UNIX socket
     * directly, with no setsockopt dance and no special privilege. */
    uid_t euid;
    gid_t egid;
    int ok = 0;
    if (getpeereid(fd, &euid, &egid) != 0) {
        snprintf(peer, peerlen, "unknown peer");
        return 0;
    }
    snprintf(peer, peerlen, "uid %ld", (long) euid);
    t_peer_uid = (uint64_t) euid;
    for (size_t i = 0; i < cfg->n_allowed; i++)
        if (cfg->allowed_uids[i] == euid) ok = 1;
    return ok;
#else
    struct ucred cred;
    socklen_t len = sizeof(cred);
    int ok = 0;
    if (getsockopt(fd, SOL_SOCKET, SO_PEERCRED, &cred, &len) != 0) {
        snprintf(peer, peerlen, "unknown peer");
        return 0;
    }
    snprintf(peer, peerlen, "uid %ld", (long) cred.uid);
    t_peer_uid = (uint64_t) cred.uid;
    for (size_t i = 0; i < cfg->n_allowed; i++)
        if (cfg->allowed_uids[i] == cred.uid) ok = 1;
    return ok;
#endif
}

static fsq_mutex g_conn_mu = FSQ_MUTEX_INIT;
static int g_conns = 0;
static uint64_t g_next_conn = 1;

static uint64_t next_conn_id(void) {
    uint64_t id;
    FSQ_LOCK(g_conn_mu);
    id = g_next_conn++;
    FSQ_UNLOCK(g_conn_mu);
    return id;
}

static void conn_released(void) {
    FSQ_LOCK(g_conn_mu);
    g_conns--;
    FSQ_UNLOCK(g_conn_mu);
}

/* A connection that drops without UDF_DEINIT must not keep its handles
 * forever. Each handle is freed through its own deinit, as UDF_DEINIT does. */
static void handle_release_owner(uint64_t conn) {
    struct fsq_handle *list = NULL, **link, *h;
    int released = 0;
    FSQ_LOCK(g_handles_mu);
    for (link = &g_handles; *link;) {
        if ((*link)->owner == conn) {
            h = *link;
            *link = h->next;
            h->next = list;
            list = h;
        } else {
            link = &(*link)->next;
        }
    }
    FSQ_UNLOCK(g_handles_mu);
    while (list) {
        h = list;
        list = h->next;
        ((fsq_deinit_fn) h->fn->deinit)(&h->init);
        handle_free_args(h);
        free(h);
        released++;
    }
    if (released)
        fsq_log("INFO", "released %d statement handle(s) from connection %llu",
                released, (unsigned long long) conn);
}

static FSQ_THREAD_RET serve_connection_body(void *arg) {
    struct conn_arg *ca = arg;
    fsq_sock fd = ca->fd;
    /* Borrow the current snapshot: idle timeout, vetting and this
     * connection's HMACs all come from it, and RELOAD leaves the frames
     * of an established connection on its borrowed (old) key. */
    struct fsq_config_snap *snap = cfg_acquire();
    const struct fsq_config *cfg = &snap->cfg;
    uint8_t hdr[FSQ_HEADER_LEN], tag[FSQ_TAG_LEN], want[FSQ_TAG_LEN];
    struct fsq_header h;
    char peer[FSQ_PEER_MAX];
    free(ca);

    t_conn = next_conn_id();
    fsq_set_idle(fd, cfg->idle_timeout_secs);

    if (!peer_allowed(fd, cfg, peer, sizeof(peer))) {
        fsq_log("WARN", "connection from %s refused: not in the allow-list", peer);
        fsq_close(fd);
        conn_released();
        cfg_release(snap);
        return FSQ_THREAD_EXIT;
    }

    for (;;) {
        uint8_t *payload = NULL;
        struct fsq_reply rep;
        if (read_all(fd, hdr, FSQ_HEADER_LEN) != 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK)
                fsq_log("INFO", "connection %llu from %s idle for %d s, closing",
                        (unsigned long long) t_conn, peer, cfg->idle_timeout_secs);
            break;
        }
        if (fsq_header_decode(hdr, &h) != 0) {
            fsq_log("WARN", "connection %llu from %s sent a malformed header",
                    (unsigned long long) t_conn, peer);
            break;
        }
        if (h.version != FSQ_PROTOCOL_VERSION || h.auth_len != FSQ_TAG_LEN) {
            fsq_log("WARN", "connection %llu from %s sent protocol version %u",
                    (unsigned long long) t_conn, peer, (unsigned) h.version);
            break;
        }
        if (h.payload_len > FSQ_MAX_PAYLOAD) {
            fsq_log("WARN", "connection %llu from %s sent a %u-byte payload over the limit",
                    (unsigned long long) t_conn, peer, (unsigned) h.payload_len);
            break;
        }
        if (h.flags & FSQ_FLAG_RESPONSE) break;
        if (read_all(fd, tag, FSQ_TAG_LEN) != 0) break;
        if (h.payload_len && read_payload(fd, h.payload_len, &payload) != 0) break;
        mac(cfg, hdr, payload, h.payload_len, want);
        if (!fsq_ct_equal(tag, want, FSQ_TAG_LEN)) {
            fsq_log("WARN", "connection %llu from %s failed authentication (opcode 0x%04x, request %u)",
                    (unsigned long long) t_conn, peer, (unsigned) h.opcode, (unsigned) h.request_id);
            free(payload);
            break;
        }

        dispatch(cfg, &h, payload, h.payload_len, &rep);
        free(payload);
        if (rep.status != FSQ_OK)
            fsq_log("WARN", "opcode 0x%04x failed: status %u, core rc %d, %.200s",
                    (unsigned) h.opcode, (unsigned) rep.status, (int) rep.core_rc,
                    rep.body_len ? (const char *) rep.body : "");
        if (send_reply(fd, cfg, &h, &rep) != 0) {
            fsq_log("WARN", "connection %llu: could not send reply, closing",
                    (unsigned long long) t_conn);
            free(rep.body);
            break;
        }
        free(rep.body);
    }
    handle_release_owner(t_conn);
    fsq_close(fd);
    conn_released();
    cfg_release(snap);
    return FSQ_THREAD_EXIT;
}

/* The connection thread's entry point.
 *
 * The body is wrapped rather than tidied up inline so that EVERY exit
 * path -- the early refusal, an error return, the normal end of the
 * request loop -- releases this connection thread's per-thread ledger
 * read buffers exactly once. Those buffers are handed to the engine,
 * which never frees them; normally the next read of the same kind
 * frees the previous one, but a connection that reads a ledger kind
 * once and then closes would strand them for the life of the daemon.
 * The daemon runs one thread per connection, so the stranding would
 * grow with the number of connections ever served. */
extern void fractal_enterprise_release_thread_read_buffers(void);

static FSQ_THREAD_RET serve_connection(void *arg) {
    FSQ_THREAD_RET r = serve_connection_body(arg);
    fractal_enterprise_release_thread_read_buffers();
    return r;
}

/* ------------------------------------------------------------------ */
/* Main                                                                */
/* ------------------------------------------------------------------ */

static void log_exit(void) {
    fsq_log("INFO", "process exiting");
}

/* Signal-number to name lookup by table (not by index): the C signal
 * macros' values differ per platform (e.g. SIGUSR1 is 10 on Linux and 30
 * on macOS), so indexing an
 * array by `sig` misnames or falls off the end exactly when it matters
 * -- in a crash we have one chance to print the right name. */
static const char *fsq_sig_name(int sig) {
    static const struct { int num; const char *name; } names[] = {
        { SIGINT, "SIGINT" },   { SIGILL, "SIGILL" },  { SIGABRT, "SIGABRT" },
        { SIGFPE, "SIGFPE" },   { SIGSEGV, "SIGSEGV" }, { SIGTERM, "SIGTERM" },
#ifdef SIGHUP
        { SIGHUP, "SIGHUP" },
#endif
#ifdef SIGQUIT
        { SIGQUIT, "SIGQUIT" },
#endif
#ifdef SIGTRAP
        { SIGTRAP, "SIGTRAP" },
#endif
#ifdef SIGBUS
        { SIGBUS, "SIGBUS" },
#endif
#ifdef SIGPIPE
        { SIGPIPE, "SIGPIPE" },
#endif
    };
    size_t i;
    for (i = 0; i < sizeof(names) / sizeof(names[0]); i++)
        if (names[i].num == sig) return names[i].name;
    return "fatal signal";
}

/* The fatal-signal handler runs in signal context, so it may call only
 * async-signal-safe functions: snprintf is not one (it can allocate and take
 * locks, and the signal may have interrupted code that holds them). The line
 * is built by hand into a stack buffer and written with write(2). */
static size_t fatal_put(char *d, size_t cap, size_t n, const char *s) {
    while (*s && n + 1 < cap) d[n++] = *s++;
    return n;
}

static size_t fatal_put_num(char *d, size_t cap, size_t n, long v) {
    char t[24];
    size_t k = 0;
    unsigned long u = v < 0 ? 0UL - (unsigned long) v : (unsigned long) v;
    if (v < 0 && n + 1 < cap) d[n++] = '-';
    do { t[k++] = (char) ('0' + (u % 10)); u /= 10; } while (u && k < sizeof t);
    while (k && n + 1 < cap) d[n++] = t[--k];
    return n;
}

static void on_fatal(int sig) {
    char line[128];
    size_t n = 0;
    n = fatal_put(line, sizeof line, n, "fractalsqld[");
    n = fatal_put_num(line, sizeof line, n, (long) fsq_pid());
    n = fatal_put(line, sizeof line, n, "] FATAL ");
    n = fatal_put(line, sizeof line, n, fsq_sig_name(sig));
    n = fatal_put(line, sizeof line, n, " (");
    n = fatal_put_num(line, sizeof line, n, (long) sig);
    n = fatal_put(line, sizeof line, n, "), exiting\n");
    { ssize_t w = write(STDERR_FILENO, line, n); (void) w; }
    signal(sig, SIG_DFL);
    raise(sig);
}

static void on_signal(int sig) {
    (void) sig;
    g_stop = 1;
    if (FSQ_SOCK_VALID(g_listen_fd)) close(g_listen_fd);
}

static int open_listener(const struct fsq_config *cfg) {
    struct sockaddr_un sa;
    struct stat st;
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    if (stat(cfg->socket_path, &st) == 0 && S_ISSOCK(st.st_mode)) unlink(cfg->socket_path);
    memset(&sa, 0, sizeof(sa));
    sa.sun_family = AF_UNIX;
    strcpy(sa.sun_path, cfg->socket_path);
    if (bind(fd, (struct sockaddr *) &sa, sizeof(sa)) != 0) { close(fd); return -1; }
    chmod(cfg->socket_path, 0660);
    if (listen(fd, 64) != 0) { close(fd); unlink(cfg->socket_path); return -1; }
    return fd;
}

static void run_listener(void) {
    struct fsq_config_snap *lsnap = cfg_acquire();
    const struct fsq_config *cfg = &lsnap->cfg;
    fsq_log("INFO", "fractalsqld %s listening on %s (max_connections=%d, idle_timeout_secs=%d, allowed_uids=%zu)",
            FSQL_DAEMON_VERSION, cfg->socket_path, cfg->max_connections, cfg->idle_timeout_secs, cfg->n_allowed);
    cfg_release(lsnap);
    while (!g_stop) {
        int fd = accept(g_listen_fd, NULL, NULL);
        pthread_t tid;
        struct conn_arg *ca;
        int over;
        if (fd < 0) {
            if (g_stop) break;
            if (errno == EINTR || errno == ECONNABORTED || errno == EPROTO) continue;
            if (errno == EMFILE || errno == ENFILE || errno == ENOBUFS || errno == ENOMEM) {
                fsq_log("WARN", "accept: %s, backing off", strerror(errno));
                FSQ_SLEEP_MS(100);
                continue;
            }
            fsq_log("ERROR", "accept failed: %s", strerror(errno));
            break;
        }
#ifdef __APPLE__
        { int one = 1; setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof(one)); }
#endif
        {
            /* Re-read the limit per accept, so a reload applies it to the
             * next connection rather than whenever this loop restarted.
             * The check and the increment happen in ONE critical section:
             * reading g_conns outside the lock and incrementing inside it
             * let two concurrent accepts both see "room for one more" and
             * push the daemon past max_connections. */
            struct fsq_config_snap *ssnap = cfg_acquire();
            int max_conns = ssnap->cfg.max_connections;
            cfg_release(ssnap);
            FSQ_LOCK(g_conn_mu);
            over = (g_conns >= max_conns);
            if (!over) g_conns++;
            FSQ_UNLOCK(g_conn_mu);
        }
        if (over) {
            fsq_log("WARN", "connection refused: max_connections reached");
            close(fd);
            continue;
        }
        ca = malloc(sizeof(*ca));
        if (!ca) { conn_released(); close(fd); continue; }
        ca->fd = fd;
        if (pthread_create(&tid, NULL, serve_connection, ca) != 0) {
            fsq_log("ERROR", "cannot start a thread for a connection: %s", strerror(errno));
            free(ca); conn_released(); close(fd); continue;
        }
        pthread_detach(tid);
    }
    fsq_log("INFO", "shutting down: no longer accepting connections");
    if (FSQ_SOCK_VALID(g_listen_fd)) close(g_listen_fd);
    {
        /* The socket path cannot change under a running daemon (RELOAD
         * refuses that change): unlink via the current snapshot. */
        struct fsq_config_snap *ssnap = cfg_acquire();
        const struct fsq_config *cfg = &ssnap->cfg;
        unlink(cfg->socket_path);
        cfg_release(ssnap);
    }
}


int main(int argc, char **argv) {
    const char *config_path = getenv(FSQ_CONFIG_ENV);
    struct fsq_config cfg;
    int service_mode = 0;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-c") == 0 && i + 1 < argc) config_path = argv[++i];
        else { fprintf(stderr, "usage: fractalsqld [-c config]\n"); return 2; }
    }
    if (!config_path) config_path = FSQ_CONFIG_DEFAULT;
    if (load_config(config_path, &cfg) != 0) {
        fprintf(stderr, "fractalsqld: cannot read config %s\n", config_path);
        return 1;
    }
    g_config_path = config_path;
    /* The in-repo computation loops (src/fractalsql_interrupt.h) poll this
     * process-wide hook; a NULL check per loop boundary is the cost when
     * nothing is active, a cancel flag read when a call is in flight and a
     * CANCEL has arrived. */
    fractalsql_interrupt_hook_set(fsq_check_cancelled);

    (void) service_mode;
    {
        struct sigaction sa;
        if (geteuid() == 0) {
            fsq_log("ERROR", "refusing to run as root: run as the service user (the demo image uses valkey)");
            return 1;
        }
        if (validate_cfg(&cfg) != 0) return 1;
        if (daemon_check_provider(&cfg) != 0) return 1;
        signal(SIGPIPE, SIG_IGN);
        memset(&sa, 0, sizeof(sa));
        sa.sa_handler = on_signal;
        sigaction(SIGTERM, &sa, NULL);
        sigaction(SIGINT, &sa, NULL);
        signal(SIGSEGV, on_fatal);
        signal(SIGABRT, on_fatal);
        signal(SIGBUS, on_fatal);
        signal(SIGFPE, on_fatal);
    }
    atexit(log_exit);

    umask(0007);   /* the socket is never created wider than owner and group */
    /* The apply must have finished before anything can talk to the
     * consumers. */
    if (daemon_apply_provider(&cfg) != 0) {
        fprintf(stderr, "fractalsqld: out of memory applying provider settings\n");
        return 1;
    }
    if (cfg_publish(&cfg) != 0) {
        fprintf(stderr, "fractalsqld: out of memory publishing the configuration\n");
        return 1;
    }
    g_listen_fd = open_listener(&cfg);
    if (!FSQ_SOCK_VALID(g_listen_fd)) {
        fsq_log("ERROR", "cannot listen on %s: %s", cfg.socket_path, strerror(errno));
        return 1;
    }
    run_listener();
    return 0;
}
