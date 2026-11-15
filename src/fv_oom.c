/* src/fv_oom.c
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: 2026 Daniel Gardiner d/b/a FractalSQLabs
 *
 * Allocation-failure injection for the module's OWN allocations. Compiled
 * only into the test flavor (-DFV_OOM_INJECT, built by build_test.sh gate
 * 11); the shipped module has none of this and no debug command.
 *
 * In that flavor fv_common.h redirects the module's malloc/calloc/realloc to
 * the wrappers below. FRACTAL.DEBUG.OOM <n> makes the nth allocation after
 * the call fail (NULL with ENOMEM, and for realloc the original block is left
 * untouched, as realloc(3) says). FRACTAL.DEBUG.OOM 0 disarms and replies
 * with how many allocations were made since arming, which lets a sweep know
 * when it has covered every allocation point of a command.
 *
 * Why not an LD_PRELOAD shim: Valkey exports its allocator from the
 * executable, and symbol lookup checks the executable before preloaded
 * libraries, so a module's malloc never reaches a preloaded one.
 */
#ifdef FV_OOM_INJECT

#define FV_OOM_IMPL 1
#include "fv_common.h"

#include <errno.h>

static long g_fail_at = 0;      /* 0 = disarmed */
static long g_count   = 0;

static int
should_fail(void)
{
    long at = __atomic_load_n(&g_fail_at, __ATOMIC_SEQ_CST);
    if (at <= 0) return 0;
    long c = __atomic_add_fetch(&g_count, 1, __ATOMIC_SEQ_CST);
    return c == at;
}

void *fv_oom_malloc(size_t n)
{
    if (should_fail()) { errno = ENOMEM; return NULL; }
    return malloc(n);
}

void *fv_oom_calloc(size_t a, size_t b)
{
    if (should_fail()) { errno = ENOMEM; return NULL; }
    return calloc(a, b);
}

void *fv_oom_realloc(void *p, size_t n)
{
    if (should_fail()) { errno = ENOMEM; return NULL; }
    return realloc(p, n);
}

static int
Oom_Command(RedisModuleCtx *ctx, RedisModuleString **argv, int argc)
{
    if (argc != 2) return RedisModule_WrongArity(ctx);
    long long n;
    if (RedisModule_StringToLongLong(argv[1], &n) != REDISMODULE_OK || n < 0)
        return RedisModule_ReplyWithError(ctx, "ERR n must be a non-negative integer");
    long seen = __atomic_load_n(&g_count, __ATOMIC_SEQ_CST);
    __atomic_store_n(&g_count, 0, __ATOMIC_SEQ_CST);
    __atomic_store_n(&g_fail_at, (long) n, __ATOMIC_SEQ_CST);
    /* Arming counts from zero; disarming reports the previous run's total.
     * Counting only happens while armed, so arm with a huge n to count a
     * clean run. */
    return RedisModule_ReplyWithLongLong(ctx, n == 0 ? seen : 0);
}

int
fv_register_oom(RedisModuleCtx *ctx)
{
    return RedisModule_CreateCommand(ctx, "FRACTAL.DEBUG.OOM", Oom_Command, "readonly", 0, 0, 0);
}

#endif /* FV_OOM_INJECT */
