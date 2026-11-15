/* tests/fuzz/fuzz_fv.c
 * SPDX-License-Identifier: Apache-2.0 AND BSD-2-Clause
 * SPDX-FileCopyrightText: 2026 Daniel Gardiner d/b/a FractalSQLabs
 *
 * libFuzzer harness for the parsers that read bytes the module does not
 * control. The first input byte picks the target:
 *
 *   0  fv_parse_vector_text   text vectors, and the daemon's EMBED reply
 *   1  parse_params           FRACTAL.DIVERSIFY.PARAMS JSON
 *   2  parse_call_result      the result TLV of a fractalsqld CALL reply
 *   3  roundtrip              a whole fractalsqld reply frame (header, tag,
 *                             payload) over a socket pair; valid and corrupt
 *   4  fv_sb_json_string      every string the module embeds in JSON
 *
 * The harness includes the source files so it can reach their static
 * functions. No Valkey is involved: nothing here calls RedisModule_*.
 */
#include "../../src/fv_daemon.c"
#include "../../src/fv_session.c"

#include <ctype.h>

static void
t_vector_text(const uint8_t *d, size_t n)
{
    float *v = NULL; size_t cnt = 0; char err[128];
    if (fv_parse_vector_text((const char *) d, n, &v, &cnt, err, sizeof err) == 0) {
        if (cnt == 0 || v == NULL) abort();
        for (size_t i = 0; i < cnt; i++) if (!isfinite(v[i])) abort();
        free(v);
    }
}

static void
t_params(const uint8_t *d, size_t n)
{
    fsql_diversify_params_t p;
    memset(&p, 0, sizeof p);
    char err[160];
    if (parse_params((const char *) d, n, &p, err, sizeof err) == 0) {
        if (!isfinite(p.stall_threshold) || !isfinite(p.repulsion_sigma) || !isfinite(p.repulsion_weight)) abort();
    }
}

static void
t_call_result(const uint8_t *d, size_t n)
{
    fv_dres res; char err[128];
    memset(&res, 0, sizeof res);
    int rc = parse_call_result(d, n, &res, err, sizeof err);
    if (rc == 0) {
        if (res.len > n) abort();
        free(res.bytes);
    }
}

static void
t_roundtrip(const uint8_t *d, size_t n)
{
    if (n < 1) return;
    uint8_t variant = d[0];
    const uint8_t *pl = d + 1;
    size_t plen = n - 1;
    if (plen > 60000) plen = 60000;

    int sv[2];
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv) != 0) return;

    static const uint8_t key[32] = { 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16,
                                     17, 18, 19, 20, 21, 22, 23, 24, 25, 26, 27, 28, 29, 30, 31, 32 };
    memcpy(g_key, key, sizeof key);
    g_key_len = sizeof key;

    const uint16_t opcode = FSQ_OP_UDF_CALL;
    uint32_t rid = g_req_ctr + 1;                 /* the id roundtrip() will use */
    struct fsq_header h;
    memset(&h, 0, sizeof h);
    h.version = FSQ_PROTOCOL_VERSION;
    h.opcode = opcode;
    h.flags = (variant & 8) ? 0 : FSQ_FLAG_RESPONSE;
    h.request_id = (variant & 16) ? rid + 1 : rid;
    h.auth_len = FSQ_TAG_LEN;
    h.payload_len = (variant & 2) ? (uint32_t) plen + 1 : (uint32_t) plen;
    uint8_t hdr[FSQ_HEADER_LEN], tag[FSQ_TAG_LEN];
    fsq_header_encode(hdr, &h);
    frame_tag(hdr, pl, plen, tag);
    if (variant & 1) tag[0] ^= 0x80;

    /* The reply is queued before the call; the write end is then closed so a
     * short read sees EOF instead of blocking. */
    if (write(sv[1], hdr, sizeof hdr) < 0 || write(sv[1], tag, sizeof tag) < 0) {}
    size_t send = (variant & 4) && plen > 0 ? plen / 2 : plen;
    if (send > 0 && write(sv[1], pl, send) < 0) {}
    shutdown(sv[1], SHUT_WR);

    uint32_t status = 0; int32_t core_rc = 0; uint8_t *body = NULL; size_t blen = 0; char err[128];
    uint8_t req[8] = { 0 };
    if (roundtrip(sv[0], opcode, 0, req, sizeof req, &status, &core_rc, &body, &blen, err, sizeof err) == 0) {
        if (blen + 8 != plen) abort();
        free(body);
    }
    close(sv[0]);
    close(sv[1]);
}

static void
t_json_string(const uint8_t *d, size_t n)
{
    char *z = (char *) malloc(n + 1);
    if (z == NULL) return;
    memcpy(z, d, n);
    z[n] = '\0';
    fv_sb sb;
    fv_sb_init(&sb);
    fv_sb_json_string(&sb, z);
    if (!sb.bad) {
        if (sb.len < 2 || sb.s[0] != '"' || sb.s[sb.len - 1] != '"') abort();
        for (size_t i = 1; i + 1 < sb.len; i++) {
            unsigned char c = (unsigned char) sb.s[i];
            if (c < 0x20) abort();                       /* a raw control byte */
            if (c == '"') abort();                       /* an unescaped quote */
            if (c == '\\') {
                if (i + 2 >= sb.len + 0 && i + 1 >= sb.len - 1) abort();
                char e = sb.s[++i];
                if (e == 'u') { for (int k = 0; k < 4; k++) if (!isxdigit((unsigned char) sb.s[++i])) abort(); }
                else if (!strchr("\"\\/bfnrt", e)) abort();
            }
        }
    }
    fv_sb_free(&sb);
    free(z);
}

int
LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    if (size < 1 || size > 70000) return 0;
    switch (data[0] % 5) {
    case 0: t_vector_text(data + 1, size - 1); break;
    case 1: t_params(data + 1, size - 1); break;
    case 2: t_call_result(data + 1, size - 1); break;
    case 3: t_roundtrip(data + 1, size - 1); break;
    default: t_json_string(data + 1, size - 1); break;
    }
    return 0;
}
