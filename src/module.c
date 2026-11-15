/* src/module.c
 * fractalsql-valkey: FractalSQL as a Valkey module.
 * SPDX-License-Identifier: Apache-2.0 AND BSD-2-Clause
 * SPDX-FileCopyrightText: 2014 Hamid Salimi (SFS algorithmic lineage)
 * SPDX-FileCopyrightText: 2026 Daniel Gardiner d/b/a FractalSQLabs
 *
 * Stable ABI target: Valkey 7.2+ (REDISMODULE_APIVER_1, which 8.x keeps).
 *
 * Commands (see the per-file headers for the argument lists):
 *   fv_search.c     FRACTAL.SEARCH, FRACTAL.EXPLORE
 *   fv_vector.c     FRACTAL.VECTOR.*
 *   fv_analytics.c  FRACTAL.DIM.*, FRACTAL.SERIES.*, FRACTAL.CYCLE.DETECT,
 *                   FRACTAL.TDA.PERSISTENCE, FRACTAL.OPTIMIZE.PORTFOLIO,
 *                   FRACTAL.MORPH.*
 *   fv_daemon.c     FRACTAL.REASON, FRACTAL.EMBED, FRACTAL.LEDGER.*,
 *                   FRACTAL.AUDIT.*  (through the fractalsqld daemon)
 *   (here)          FRACTAL.EDITION, FRACTAL.VERSION
 *
 * Load-time arguments:
 *   loadmodule fractalsql.so [threads N] [max-inflight-mb M]
 *     threads          worker threads for long-running commands (default 4, 1..64)
 *     max-inflight-mb  cap on decoded input held by running jobs (default 1024,
 *                      0 = unlimited); requests over it are refused, not queued
 *     daemon-socket / daemon-key-file   the fractalsqld daemon (both or neither;
 *                      the key file must be mode 0600). Without them the
 *                      reasoning, embedding and ledger commands answer with
 *                      a "not configured" error and everything else works
 *     daemon-timeout-s per-call daemon timeout (default 60, 1..3600)
 *     agent-partial-reasoning-fail yes|no  search_agent / rag_agent: on a reasoning failure
 *                      return the retrieval plus "reasoning_step_failed" instead of an
 *                      error (default no)
 *
 * The statically linked core archive is the sovereign community build.
 */


#include "fv_common.h"

#include <ctype.h>
#include <limits.h>
#include <strings.h>

#define MODULE_NAME      "fractalsql"
#define MODULE_VERSION   1
#define EDITION_STRING   "Community"
#define VERSION_STRING   FRACTALSQL_VERSION

static int
Edition_Command(RedisModuleCtx *ctx, RedisModuleString **argv, int argc)
{
    (void) argv;
    if (argc != 1) return RedisModule_WrongArity(ctx);
    return RedisModule_ReplyWithSimpleString(ctx, EDITION_STRING);
}

static int
Version_Command(RedisModuleCtx *ctx, RedisModuleString **argv, int argc)
{
    (void) argv;
    if (argc != 1) return RedisModule_WrongArity(ctx);
    return RedisModule_ReplyWithSimpleString(ctx, VERSION_STRING);
}

static int
parse_int_arg(RedisModuleString *s, long long lo, long long hi, long long *out)
{
    long long v;
    if (RedisModule_StringToLongLong(s, &v) != REDISMODULE_OK || v < lo || v > hi) return -1;
    *out = v;
    return 0;
}

int
RedisModule_OnLoad(RedisModuleCtx *ctx, RedisModuleString **argv, int argc)
{
    if (RedisModule_Init(ctx, MODULE_NAME, MODULE_VERSION, REDISMODULE_APIVER_1) == REDISMODULE_ERR)
        return REDISMODULE_ERR;

    long long threads = 4, inflight_mb = 1024, daemon_timeout = 60, max_sessions = 64, session_ttl = 3600;
    int agent_partial = 0;
    char daemon_sock[256] = "", daemon_key[512] = "";
    for (int i = 0; i < argc; i += 2) {
        size_t l;
        const char *k = RedisModule_StringPtrLen(argv[i], &l);
        if (i + 1 >= argc) {
            RedisModule_Log(ctx, "warning", "fractalsql: option '%.32s' needs a value", k);
            return REDISMODULE_ERR;
        }
        if (l == 7 && strncasecmp(k, "threads", 7) == 0) {
            if (parse_int_arg(argv[i + 1], 1, 64, &threads) != 0) {
                RedisModule_Log(ctx, "warning", "fractalsql: threads must be an integer in [1, 64]");
                return REDISMODULE_ERR;
            }
        } else if (l == 15 && strncasecmp(k, "max-inflight-mb", 15) == 0) {
            if (parse_int_arg(argv[i + 1], 0, 1 << 20, &inflight_mb) != 0) {
                RedisModule_Log(ctx, "warning", "fractalsql: max-inflight-mb must be an integer in [0, 1048576]");
                return REDISMODULE_ERR;
            }
        } else if (l == 12 && strncasecmp(k, "max-sessions", 12) == 0) {
            if (parse_int_arg(argv[i + 1], 1, 4096, &max_sessions) != 0) {
                RedisModule_Log(ctx, "warning", "fractalsql: max-sessions must be an integer in [1, 4096]");
                return REDISMODULE_ERR;
            }
        } else if (l == 15 && strncasecmp(k, "session-ttl-sec", 15) == 0) {
            if (parse_int_arg(argv[i + 1], 0, 86400 * 30, &session_ttl) != 0) {
                RedisModule_Log(ctx, "warning", "fractalsql: session-ttl-sec must be an integer in [0, 2592000] (0 = never expire)");
                return REDISMODULE_ERR;
            }
        } else if (l == 13 && strncasecmp(k, "daemon-socket", 13) == 0) {
            size_t vl; const char *v = RedisModule_StringPtrLen(argv[i + 1], &vl);
            if (vl == 0 || vl >= sizeof daemon_sock) {
                RedisModule_Log(ctx, "warning", "fractalsql: daemon-socket is empty or too long");
                return REDISMODULE_ERR;
            }
            memcpy(daemon_sock, v, vl); daemon_sock[vl] = '\0';
        } else if (l == 15 && strncasecmp(k, "daemon-key-file", 15) == 0) {
            size_t vl; const char *v = RedisModule_StringPtrLen(argv[i + 1], &vl);
            if (vl == 0 || vl >= sizeof daemon_key) {
                RedisModule_Log(ctx, "warning", "fractalsql: daemon-key-file is empty or too long");
                return REDISMODULE_ERR;
            }
            memcpy(daemon_key, v, vl); daemon_key[vl] = '\0';
        } else if (l == 16 && strncasecmp(k, "daemon-timeout-s", 16) == 0) {
            if (parse_int_arg(argv[i + 1], 1, 3600, &daemon_timeout) != 0) {
                RedisModule_Log(ctx, "warning", "fractalsql: daemon-timeout-s must be an integer in [1, 3600]");
                return REDISMODULE_ERR;
            }
        } else if (l == 28 && strncasecmp(k, "agent-partial-reasoning-fail", 28) == 0) {
            size_t vl; const char *v = RedisModule_StringPtrLen(argv[i + 1], &vl);
            if (vl == 3 && strncasecmp(v, "yes", 3) == 0) agent_partial = 1;
            else if (vl == 2 && strncasecmp(v, "no", 2) == 0) agent_partial = 0;
            else {
                RedisModule_Log(ctx, "warning", "fractalsql: agent-partial-reasoning-fail must be yes or no");
                return REDISMODULE_ERR;
            }
        } else {
            RedisModule_Log(ctx, "warning", "fractalsql: unknown option '%.32s'", k);
            return REDISMODULE_ERR;
        }
    }

    /* The daemon is optional, but its two settings come as a pair. */
    if ((daemon_sock[0] != '\0') != (daemon_key[0] != '\0')) {
        RedisModule_Log(ctx, "warning", "fractalsql: daemon-socket and daemon-key-file must be given together");
        return REDISMODULE_ERR;
    }
    if (daemon_sock[0] != '\0') {
        char derr[256];
        if (fv_daemon_configure(daemon_sock, daemon_key, (unsigned) daemon_timeout, derr, sizeof derr) != 0) {
            RedisModule_Log(ctx, "warning", "fractalsql: %s", derr);
            return REDISMODULE_ERR;
        }
    }

    fv_agents_configure(agent_partial);
    fv_session_configure((int) max_sessions, (int) session_ttl);
    if (fv_pool_start((int) threads, (size_t) inflight_mb * 1024u * 1024u) != 0) {
        RedisModule_Log(ctx, "warning", "fractalsql: could not start the worker pool");
        return REDISMODULE_ERR;
    }

    if (fv_register_search(ctx) == REDISMODULE_ERR ||
        fv_register_vector(ctx) == REDISMODULE_ERR ||
        fv_register_analytics(ctx) == REDISMODULE_ERR ||
        fv_register_daemon(ctx) == REDISMODULE_ERR ||
        fv_register_agents(ctx) == REDISMODULE_ERR ||
        fv_register_session(ctx) == REDISMODULE_ERR ||
#ifdef FV_OOM_INJECT
        fv_register_oom(ctx) == REDISMODULE_ERR ||
#endif
        RedisModule_CreateCommand(ctx, "fractal.edition", Edition_Command, "readonly fast", 0, 0, 0) == REDISMODULE_ERR ||
        RedisModule_CreateCommand(ctx, "fractal.version", Version_Command, "readonly fast", 0, 0, 0) == REDISMODULE_ERR ||
        fv_acl(ctx, "fractal.edition", "read fast") != 0 ||
        fv_acl(ctx, "fractal.version", "read fast") != 0) {
        fv_pool_stop();
        return REDISMODULE_ERR;
    }
    return REDISMODULE_OK;
}

/* MODULE UNLOAD: refuse while any job is queued, running, or awaiting its
 * reply; otherwise stop the workers (which frees their core contexts) and
 * free the main thread's. */
int
RedisModule_OnUnload(RedisModuleCtx *ctx)
{
    (void) ctx;
    if (fv_pool_stop() != 0) return REDISMODULE_ERR;
    fv_session_shutdown();
    fv_thread_ctx_free();
    return REDISMODULE_OK;
}
