/* tests/scripted_reasoning_plugin.c
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: 2026 Daniel Gardiner d/b/a FractalSQLabs
 *
 * A scripted reasoning plugin: every dispatch is answered with the next reply
 * of the script the harness wrote, so a test chooses exactly what the extension
 * has to parse.
 *
 * The script lives in a file named by FSQL_SCRIPT (default
 * /tmp/fractalsql_bt_script.txt). Its first line is an EPOCH: whenever the
 * epoch changes the reply counter starts over, which is how a driver can
 * script a fresh set of replies for the next scenario even though the daemon
 * (and therefore the plugin) is one long-lived process.
 *
 * The replies are separated by the ASCII record separator (0x1e) and are
 * consumed in the order the dispatches happen -- a text-to-SQL call dispatches
 * to the model and then, with the review step on, to the model again, so the
 * driver scripts the candidate first and the verdict second.
 *
 * The text-to-SQL tier also models the real reasoning-http plugin's
 * RESPONSE_MODE=code post-processing (the extension itself does no fence
 * stripping any more), so a fenced reply is extracted the way the shipped
 * plugin extracts it. The default is the shipped plugin's default response
 * mode, text: the reply is handed back as the model wrote it.
 *
 * Test-harness use only; never shipped.
 *
 * Build: cc -shared -fPIC -std=c99 -Iinclude \
 *          tests/scripted_reasoning_plugin.c -o <tmp>/scripted_reasoning_plugin.so
 */
#define _POSIX_C_SOURCE 200809L

#include "fractalsql_sql.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define SCRIPT_FILE     "/tmp/fractalsql_bt_script.txt"
#define RS              '\x1e'
#define MAX_SCRIPT_BYTES (256 * 1024)

/* The script the harness pointed at, and the epoch it carried last time. */
static char  g_path[1024] = "";
static long  g_epoch = -1;
static long  g_call  = 0;

static char *
read_file(const char *path, size_t *out_len)
{
    FILE *f = fopen(path, "rb");
    if (f == NULL)
        return NULL;
    size_t cap = 4096, len = 0;
    char  *buf = malloc(cap);
    if (buf == NULL) { fclose(f); return NULL; }
    for (;;) {
        if (len + 1 >= cap) {
            size_t ncap = cap * 2;
            if (ncap > MAX_SCRIPT_BYTES) ncap = MAX_SCRIPT_BYTES;
            if (ncap == cap) break;
            char *nbuf = realloc(buf, ncap);
            if (nbuf == NULL) { free(buf); fclose(f); return NULL; }
            buf = nbuf; cap = ncap;
        }
        size_t n = fread(buf + len, 1, cap - len - 1, f);
        if (n == 0) break;
        len += n;
    }
    fclose(f);
    buf[len] = '\0';
    if (out_len) *out_len = len;
    return buf;
}

/* Picks the idx-th reply of the script (the epoch line is not a reply). */
static int
script_pick(const char *script, long idx, const char **out, size_t *out_len)
{
    const char *p = strchr(script, '\n');       /* the epoch line */
    if (p == NULL) return 0;
    p++;
    for (long i = 0;; i++) {
        const char *e = strchr(p, RS);
        size_t      len = e ? (size_t) (e - p) : strlen(p);
        if (i == idx) { *out = p; *out_len = len; return 1; }
        if (e == NULL) return 0;
        p = e + 1;
    }
}

/* The fenced-block extraction the shipped plugin does in code mode. */
static int
apply_code_mode(const char *reply, size_t reply_len, char **out, size_t *out_len)
{
    const char *end = reply + reply_len;
    const char *p = reply;
    const char *start = NULL;
    size_t      len = 0;
    int         blocks = 0;

    while (p < end) {
        const char *fence = NULL;
        for (const char *q = p; end - q >= 3; q++)
            if (q[0] == '`' && q[1] == '`' && q[2] == '`') { fence = q; break; }
        const char *inner = NULL, *close = NULL;
        if (fence) {
            inner = fence + 3;
            while (inner < end && *inner != '\n') inner++;
            if (inner < end) inner++;
            for (const char *q = inner; end - q >= 3; q++)
                if (q[0] == '`' && q[1] == '`' && q[2] == '`') { close = q; break; }
        }
        if (!fence || !close) break;
        blocks++;
        if (blocks == 1) {
            start = inner;
            len = (size_t) (close - inner);
            while (len > 0 && (start[len - 1] == '\n' || start[len - 1] == '\r')) len--;
        }
        p = close + 3;
    }
    if (blocks > 1) return -1;                 /* more than one block: not guessable */

    char *copy = malloc((blocks == 1 ? len : reply_len) + 1);
    if (!copy) return -1;
    if (blocks == 1) { memcpy(copy, start, len); copy[len] = '\0'; *out_len = len; }
    else             { memcpy(copy, reply, reply_len); copy[reply_len] = '\0'; *out_len = reply_len; }
    *out = copy;
    return 0;
}

static int
scripted_format(void *u, const char *q, size_t ql, const char *c, size_t cl,
                const char **prompt_out, size_t *prompt_len_out)
{
    (void) u; (void) q; (void) ql; (void) c; (void) cl;
    static char b[2] = { 'x', '\0' };
    *prompt_out = b;
    *prompt_len_out = 1;
    return 0;
}

static void
scripted_free(void *opaque)
{
    fsql_ai_response_t *r = (fsql_ai_response_t *) opaque;
    if (r != NULL && r->summary != NULL)
        free(r->summary);
}

static int
scripted_generate(void *u, const char *p, size_t pl,
                  char **response_out, size_t *response_len_out,
                  void (**response_free_fn_out)(void *))
{
    (void) u; (void) p; (void) pl;

    *response_out = NULL;
    *response_len_out = 0;
    *response_free_fn_out = scripted_free;

    const char *path = getenv("FSQL_SCRIPT");
    if (path == NULL || !*path) path = SCRIPT_FILE;

    size_t len = 0;
    char  *script = read_file(path, &len);
    if (script == NULL) {
        fprintf(stderr, "scripted_reasoning_plugin: no script at %s -- the dispatch fails\n", path);
        return 1;
    }

    /* The epoch line: a new epoch restarts the reply sequence. */
    long epoch = strtol(script, NULL, 10);
    if (epoch != g_epoch || strcmp(g_path, path) != 0) {
        g_epoch = epoch;
        g_call = 0;
        snprintf(g_path, sizeof g_path, "%s", path);
    }

    const char *reply = NULL;
    size_t      reply_len = 0;
    if (!script_pick(script, g_call++, &reply, &reply_len)) {
        fprintf(stderr, "scripted_reasoning_plugin: the script at %s ran out (call %ld) -- the dispatch fails\n",
                path, g_call);
        free(script);
        return 1;
    }

    const char *mode = getenv("FSQL_REASONING_HTTP_MODE");
    const char *rmode = getenv("FSQL_REASONING_HTTP_RESPONSE_MODE");
    /* The shipped reasoning-http plugin's default response mode is text: the
     * reply is handed back as the model wrote it, fence markers and all.
     * Only an explicit RESPONSE_MODE=code extracts a fenced block (the
     * text-to-SQL tier asserts exactly that for its own dispatch), and an
     * embedding reply is never a code block. */
    int code = (rmode != NULL && strcmp(rmode, "code") == 0);
    if (mode != NULL && strcmp(mode, "embedding") == 0) code = 0;

    /* The reply points into the script buffer, so the buffer is freed only
     * AFTER the reply has been copied out of it: freeing it first hands the
     * caller whatever the allocator has put into the freed block (the reply
     * then arrives as garbage of the right length). */
    char  *body = NULL;
    size_t body_len = 0;
    if (code) {
        if (apply_code_mode(reply, reply_len, &body, &body_len) < 0) {
            fprintf(stderr, "scripted_reasoning_plugin: the reply holds more than one fenced block -- the dispatch fails\n");
            free(script);
            return 1;
        }
    } else {
        body = malloc(reply_len + 1);
        if (body == NULL) { free(script); return -1; }
        memcpy(body, reply, reply_len);
        body[reply_len] = '\0';
        body_len = reply_len;
    }
    free(script);

    *response_out = body;
    *response_len_out = body_len;
    *response_free_fn_out = scripted_free;
    return 0;
}

int
fsql_reasoning_init(fsql_reasoning_vfs_t *vfs)
{
    vfs->abi_version   = FSQL_REASONING_ABI_VERSION;
    vfs->user_ctx      = NULL;
    vfs->format_prompt = scripted_format;
    vfs->generate      = scripted_generate;
    return 0;
}
