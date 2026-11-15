/* src/fv_daemon.c
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: 2026 Daniel Gardiner d/b/a FractalSQLabs
 *
 * The fractalsqld client, and the commands that run through it. Anything
 * that can block on the network, load third-party code, or hold licensed
 * code (reasoning, embeddings, the enterprise ledger) goes to the daemon
 * over the FSQ v2 protocol (src/fsq/, spec: protocol SPEC.md), so a crash or
 * a stall there cannot take the datastore down with it.
 *
 *   FRACTAL.REASON  query [context_json]        -> text
 *   FRACTAL.EMBED   text                        -> packed float32 BLOB
 *   FRACTAL.LEDGER.FLUSH | LOAD | COMPACT | RESET.SOFT | RESET.HARD | LOCK
 *                                               -> integer
 *   FRACTAL.LEDGER.TRUTH.COUNT | SHADOW.COUNT   -> integer
 *   FRACTAL.LEDGER.VERIFY [kind]                -> JSON
 *   FRACTAL.AUDIT.LOG entry_type payload_json   -> integer
 *   FRACTAL.AUDIT.UNPACK blob                   -> JSON
 *
 * The daemon's functions take a session id first; the Valkey client id is
 * used for it. The ledger and audit commands need the enterprise tier loaded
 * in the daemon (enterprise_lib in fractalsqld.conf); on a community daemon
 * they fail with a clear error. The daemon is optional: with it absent or
 * down, every other command keeps working.
 *
 * Load options (operator territory, never settable from a command):
 *   daemon-socket PATH   daemon-key-file PATH   daemon-timeout-s N
 */
#include "fv_common.h"
#include <limits.h>
#include "fsq/fsq_protocol.h"
#include "fsq/fsq_sha256.h"

#include <errno.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

#ifndef MSG_NOSIGNAL
#  define MSG_NOSIGNAL 0
#endif

#define ENT_HINT "the enterprise tier is not loaded in fractalsqld (set enterprise_lib in fractalsqld.conf; see docs/enterprise.md)"

#define FV_DAEMON_MAX_REPLY   (32u * 1024u * 1024u)
#define FV_DAEMON_MAX_REQUEST (8u  * 1024u * 1024u)

static char     g_sock_path[sizeof(((struct sockaddr_un *) 0)->sun_path)];
static uint8_t  g_key[64];
static size_t   g_key_len = 0;
static unsigned g_timeout_s = 60;
static int      g_configured = 0;
static uint64_t g_idem_base = 0;
static uint64_t g_idem_ctr  = 0;
static uint32_t g_req_ctr   = 0;

/* ---------------------------------------------------------------- */
/* configuration                                                      */
/* ---------------------------------------------------------------- */
static int
hex_nibble(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static void
fill_random(void *dst, size_t n)
{
    FILE *f = fopen("/dev/urandom", "rb");
    size_t got = 0;
    if (f != NULL) { got = fread(dst, 1, n, f); fclose(f); }
    if (got != n) {                       /* degraded, never zero */
        uint64_t t = (uint64_t) time(NULL) * 6364136223846793005ULL + (uint64_t) getpid();
        memcpy(dst, &t, n < sizeof t ? n : sizeof t);
    }
}

int
fv_daemon_configure(const char *sock, const char *keyfile, unsigned timeout_s,
                    char *err, size_t errsz)
{
    if (sock == NULL || keyfile == NULL) {
        snprintf(err, errsz, "daemon-socket and daemon-key-file must be given together");
        return -1;
    }
    if (strlen(sock) >= sizeof g_sock_path || sock[0] != '/') {
        snprintf(err, errsz, "daemon-socket must be an absolute path shorter than %zu bytes", sizeof g_sock_path);
        return -1;
    }
    struct stat st;
    if (stat(keyfile, &st) != 0) {
        snprintf(err, errsz, "cannot read daemon-key-file: %s", strerror(errno));
        return -1;
    }
    if ((st.st_mode & 077) != 0) {
        snprintf(err, errsz, "daemon-key-file must not be accessible by group or others (chmod 600)");
        return -1;
    }
    FILE *f = fopen(keyfile, "r");
    if (f == NULL) { snprintf(err, errsz, "cannot open daemon-key-file: %s", strerror(errno)); return -1; }
    char buf[256]; size_t n = 0; int c;
    while (n < sizeof buf && (c = fgetc(f)) != EOF)
        if (c != '\n' && c != '\r' && c != ' ' && c != '\t') buf[n++] = (char) c;
    fclose(f);
    if (n == 0 || n % 2 != 0 || n / 2 > sizeof g_key) {
        snprintf(err, errsz, "daemon-key-file must hold an even number of hex digits (at most %zu bytes)", sizeof g_key);
        return -1;
    }
    size_t klen = 0; int hi = -1;
    for (size_t i = 0; i < n; i++) {
        int v = hex_nibble(buf[i]);
        if (v < 0) { snprintf(err, errsz, "daemon-key-file is not hexadecimal"); return -1; }
        if (hi < 0) hi = v; else { g_key[klen++] = (uint8_t) ((hi << 4) | v); hi = -1; }
    }
    memset(buf, 0, sizeof buf);
    g_key_len = klen;
    snprintf(g_sock_path, sizeof g_sock_path, "%s", sock);
    g_timeout_s = timeout_s;
    fill_random(&g_idem_base, sizeof g_idem_base);
    g_idem_base &= ~(uint64_t) 0xFFFFFF;       /* leave room for the counter */
    if (g_idem_base == 0) g_idem_base = (uint64_t) 1 << 40;
    g_configured = 1;
    return 0;
}

int fv_daemon_configured(void) { return g_configured; }

/* ---------------------------------------------------------------- */
/* transport                                                          */
/* ---------------------------------------------------------------- */
static int
write_all(int fd, const uint8_t *p, size_t n)
{
    while (n > 0) {
        ssize_t w = send(fd, p, n, MSG_NOSIGNAL);
        if (w < 0) { if (errno == EINTR) continue; return -1; }
        p += w; n -= (size_t) w;
    }
    return 0;
}

static int
read_all(int fd, uint8_t *p, size_t n)
{
    while (n > 0) {
        ssize_t r = recv(fd, p, n, 0);
        if (r < 0) { if (errno == EINTR) continue; return -1; }
        if (r == 0) return -1;
        p += r; n -= (size_t) r;
    }
    return 0;
}

static int
connect_daemon(char *err, size_t errsz)
{
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) { snprintf(err, errsz, "socket(): %s", strerror(errno)); return -1; }
    struct sockaddr_un sa;
    memset(&sa, 0, sizeof sa);
    sa.sun_family = AF_UNIX;
    memcpy(sa.sun_path, g_sock_path, strlen(g_sock_path));
    if (connect(fd, (struct sockaddr *) &sa, sizeof sa) != 0) {
        snprintf(err, errsz, "fractalsqld is not reachable at %s (%s)", g_sock_path, strerror(errno));
        close(fd);
        return -1;
    }
#ifdef SO_NOSIGPIPE
    { int one = 1; setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof one); }
#endif
    struct timeval tv = { (time_t) g_timeout_s, 0 };
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
    return fd;
}

static void
frame_tag(const uint8_t *hdr, const uint8_t *payload, size_t plen, uint8_t tag[32])
{
    uint8_t *msg = (uint8_t *) malloc(FSQ_HEADER_LEN + plen);
    if (msg == NULL) { memset(tag, 0, 32); return; }
    memcpy(msg, hdr, FSQ_HEADER_LEN);
    if (plen > 0) memcpy(msg + FSQ_HEADER_LEN, payload, plen);
    fsq_hmac_sha256(g_key, g_key_len, msg, FSQ_HEADER_LEN + plen, tag);
    free(msg);
}

/* One request and its response on an open connection.
 * Returns 0 with status, core_rc and body filled (caller frees the body), or -1
 * on a transport/protocol failure with a message in err. */
static int
roundtrip(int fd, uint16_t opcode, uint64_t context,
          const uint8_t *payload, size_t plen,
          uint32_t *status, int32_t *core_rc, uint8_t **body, size_t *blen,
          char *err, size_t errsz)
{
    struct fsq_header h;
    uint8_t hdr[FSQ_HEADER_LEN], tag[FSQ_TAG_LEN];
    uint32_t rid = __atomic_add_fetch(&g_req_ctr, 1, __ATOMIC_RELAXED);
    memset(&h, 0, sizeof h);
    h.version = FSQ_PROTOCOL_VERSION; h.opcode = opcode; h.flags = 0;
    h.request_id = rid; h.context = context;
    h.auth_len = FSQ_TAG_LEN; h.payload_len = (uint32_t) plen;
    fsq_header_encode(hdr, &h);
    frame_tag(hdr, payload, plen, tag);

    if (write_all(fd, hdr, sizeof hdr) != 0 || write_all(fd, tag, sizeof tag) != 0 ||
        (plen > 0 && write_all(fd, payload, plen) != 0)) {
        snprintf(err, errsz, "lost the connection to fractalsqld while sending (%s)", strerror(errno));
        return -1;
    }

    uint8_t rh[FSQ_HEADER_LEN], rtag[FSQ_TAG_LEN];
    if (read_all(fd, rh, sizeof rh) != 0 || read_all(fd, rtag, sizeof rtag) != 0) {
        snprintf(err, errsz, "fractalsqld did not answer (%s)",
                 errno == EAGAIN || errno == EWOULDBLOCK ? "timed out" : "connection closed");
        return -1;
    }
    struct fsq_header r;
    if (fsq_header_decode(rh, &r) != 0 || r.version != FSQ_PROTOCOL_VERSION ||
        (r.flags & FSQ_FLAG_RESPONSE) == 0 || r.auth_len != FSQ_TAG_LEN ||
        r.request_id != rid || r.opcode != opcode ||
        r.payload_len < 8 || r.payload_len > FV_DAEMON_MAX_REPLY) {
        snprintf(err, errsz, "fractalsqld sent a malformed reply");
        return -1;
    }
    uint8_t *rp = (uint8_t *) malloc(r.payload_len);
    if (rp == NULL) { snprintf(err, errsz, "out of memory"); return -1; }
    if (read_all(fd, rp, r.payload_len) != 0) {
        free(rp);
        snprintf(err, errsz, "fractalsqld closed the connection mid-reply");
        return -1;
    }
    uint8_t want[FSQ_TAG_LEN];
    frame_tag(rh, rp, r.payload_len, want);
    if (!fsq_ct_equal(want, rtag, FSQ_TAG_LEN)) {
        free(rp);
        snprintf(err, errsz, "fractalsqld reply failed authentication (wrong daemon-key-file?)");
        return -1;
    }
    *status = fsq_get_u32(rp);
    *core_rc = (int32_t) fsq_get_u32(rp + 4);
    *blen = r.payload_len - 8;
    *body = (uint8_t *) malloc(*blen ? *blen : 1);
    if (*body == NULL) { free(rp); snprintf(err, errsz, "out of memory"); return -1; }
    if (*blen > 0) memcpy(*body, rp + 8, *blen);
    free(rp);
    return 0;
}

/* ---------------------------------------------------------------- */
/* call                                                               */
/* ---------------------------------------------------------------- */
typedef struct {
    int         type;        /* FSQ_T_STRING, FSQ_T_INT or FSQ_T_REAL */
    double      d;           /* FSQ_T_REAL */
    const char *s;
    size_t      len;
    long long   i;
} fv_darg;

typedef struct {
    int       is_null;
    int       type;
    uint8_t  *bytes;         /* malloc'd */
    size_t    len;
    uint32_t  status;
    int32_t   core_rc;
} fv_dres;

static size_t
encode_args(uint8_t *p, const fv_darg *a, int n)
{
    uint8_t *o = p;
    for (int i = 0; i < n; i++) {
        uint32_t len = (a[i].type == FSQ_T_INT || a[i].type == FSQ_T_REAL) ? 8 : (uint32_t) a[i].len;
        *o++ = (uint8_t) a[i].type;
        *o++ = 0;
        fsq_put_u32(o, len); o += 4;
        if (a[i].type == FSQ_T_INT) fsq_put_u64(o, (uint64_t) a[i].i);
        else if (a[i].type == FSQ_T_REAL) { uint64_t bits; memcpy(&bits, &a[i].d, 8); fsq_put_u64(o, bits); }
        else if (len > 0) memcpy(o, a[i].s, len);
        o += len;
    }
    return (size_t) (o - p);
}

/* The body of a successful CALL reply: type, null flag, value length, value.
 * 0 with res filled; -1 when the body is malformed (res->status is then set to
 * FSQ_ERR_INTERNAL and err says so); -2 on out of memory. */
static int
parse_call_result(const uint8_t *body, size_t blen, fv_dres *res, char *err, size_t errsz)
{
    if (blen < 6) { snprintf(err, errsz, "fractalsqld sent a malformed result"); res->status = FSQ_ERR_INTERNAL; return -1; }
    res->type = body[0];
    res->is_null = body[1];
    uint32_t vlen = fsq_get_u32(body + 2);
    if (vlen > blen - 6) { snprintf(err, errsz, "fractalsqld sent a result of the wrong length"); res->status = FSQ_ERR_INTERNAL; return -1; }
    res->bytes = (uint8_t *) malloc(vlen ? vlen : 1);
    if (res->bytes == NULL) { snprintf(err, errsz, "out of memory"); return -2; }
    if (vlen > 0) memcpy(res->bytes, body + 6, vlen);
    res->len = vlen;
    return 0;
}

/* One attempt: connect, INIT, CALL, DEINIT. Transport failure => -1. */
static int
attempt(const char *fn, uint64_t token, const fv_darg *args, int nargs,
        fv_dres *res, char *err, size_t errsz)
{
    size_t namelen = strlen(fn), argbytes = 0;
    for (int i = 0; i < nargs; i++) argbytes += 6 + ((args[i].type == FSQ_T_INT || args[i].type == FSQ_T_REAL) ? 8 : args[i].len);
    if (argbytes > FV_DAEMON_MAX_REQUEST) {
        snprintf(err, errsz, "request too large (max %u bytes)", (unsigned) FV_DAEMON_MAX_REQUEST);
        res->status = FSQ_ERR_LIMIT;
        return 0;
    }
    int fd = connect_daemon(err, errsz);
    if (fd < 0) return -1;

    size_t cap = 1 + namelen + 8 + 4 + argbytes + 16;
    uint8_t *req = (uint8_t *) malloc(cap);
    if (req == NULL) { close(fd); snprintf(err, errsz, "out of memory"); return -1; }
    uint8_t *body = NULL; size_t blen = 0; uint32_t status = 0; int32_t core_rc = 0;
    int rc = -1;

    /* INIT: name, nargs, TLVs (the types declared here govern the call). */
    size_t o = 0;
    req[o++] = (uint8_t) namelen; memcpy(req + o, fn, namelen); o += namelen;
    fsq_put_u32(req + o, (uint32_t) nargs); o += 4;
    o += encode_args(req + o, args, nargs);
    if (roundtrip(fd, FSQ_OP_UDF_INIT, 0, req, o, &status, &core_rc, &body, &blen, err, errsz) != 0) goto done;
    if (status != FSQ_OK) {
        res->status = status; res->core_rc = core_rc;
        size_t m = blen < errsz - 1 ? blen : errsz - 1;
        memcpy(err, body, m); err[m] = '\0';
        rc = 0; goto done;
    }
    if (blen < 17) { snprintf(err, errsz, "fractalsqld sent a malformed init reply"); goto done; }
    uint64_t handle = fsq_get_u64(body);
    free(body); body = NULL;

    /* CALL */
    o = 0;
    req[o++] = (uint8_t) namelen; memcpy(req + o, fn, namelen); o += namelen;
    fsq_put_u64(req + o, handle); o += 8;
    fsq_put_u32(req + o, (uint32_t) nargs); o += 4;
    o += encode_args(req + o, args, nargs);
    if (roundtrip(fd, FSQ_OP_UDF_CALL, token, req, o, &status, &core_rc, &body, &blen, err, errsz) != 0) goto done;
    res->status = status; res->core_rc = core_rc;
    if (status == FSQ_OK) {
        int pr = parse_call_result(body, blen, res, err, errsz);
        if (pr == -2) goto done;
    }
    rc = 0;

    {   /* best effort: the daemon also drops a connection's handles on close */
        uint8_t dp[8]; char e2[64]; uint32_t s2; int32_t c2; uint8_t *b2 = NULL; size_t l2 = 0;
        fsq_put_u64(dp, handle);
        if (roundtrip(fd, FSQ_OP_UDF_DEINIT, 0, dp, 8, &s2, &c2, &b2, &l2, e2, sizeof e2) == 0) free(b2);
    }
done:
    free(body);
    free(req);
    close(fd);
    return rc;
}

/* retry_class: 'r' pure (up to 3 attempts), 's' stateful (one resend with
 * the same token), 'e' expensive (never retried). Only transport failures
 * are retried; a status the daemon answered with is final. */
static int
daemon_call(const char *fn, char retry_class, const fv_darg *args, int nargs,
            fv_dres *res, char *err, size_t errsz)
{
    int attempts = retry_class == 'r' ? 3 : retry_class == 's' ? 2 : 1;
    uint64_t token = g_idem_base + (__atomic_add_fetch(&g_idem_ctr, 1, __ATOMIC_RELAXED) & 0xFFFFFF);
    memset(res, 0, sizeof *res);
    for (int a = 0; a < attempts; a++) {
        free(res->bytes); memset(res, 0, sizeof *res);
        err[0] = '\0';
        if (attempt(fn, token, args, nargs, res, err, errsz) == 0) return 0;
    }
    return -1;
}

/* Worker-side reasoning call for the agents (fv_agents.c). Never retried
 * (class 'e'). On success *out is a malloc'd NUL-terminated string; on any
 * failure, including "no daemon configured", returns -1 with a message in err
 * so the caller can return its computed half with the error attached. */
int
fv_daemon_reason(const char *prompt, const char *context_json,
                 char **out, size_t *out_len, char *err, size_t errsz)
{
    if (!g_configured) {
        snprintf(err, errsz, "the fractalsqld daemon is not configured");
        return -1;
    }
    fv_darg args[3];
    memset(args, 0, sizeof args);
    args[0].type = FSQ_T_INT;    args[0].i = 0;
    args[1].type = FSQ_T_STRING; args[1].s = prompt;       args[1].len = strlen(prompt);
    args[2].type = FSQ_T_STRING; args[2].s = context_json; args[2].len = strlen(context_json);

    fv_dres res;
    if (daemon_call("fractal_reason", 'e', args, 3, &res, err, errsz) != 0) {
        if (err[0] == '\0') snprintf(err, errsz, "fractalsqld call failed");
        return -1;
    }
    if (res.status != FSQ_OK || res.is_null) {
        if (res.status == FSQ_ERR_CORE || res.is_null)
            snprintf(err, errsz, "fractal_reason failed in the daemon: is a reasoning provider configured in fractalsqld?");
        else if (err[0] == '\0')
            snprintf(err, errsz, "fractalsqld refused fractal_reason (status %u)", (unsigned) res.status);
        free(res.bytes);
        return -1;
    }
    char *s = (char *) malloc(res.len + 1);
    if (s == NULL) { free(res.bytes); snprintf(err, errsz, "out of memory"); return -1; }
    memcpy(s, res.bytes, res.len);
    s[res.len] = '\0';
    free(res.bytes);
    *out = s; *out_len = res.len;
    return 0;
}

/* Worker-side, best-effort audit record of an agent decision (see fv_common.h). The ledger needs the enterprise tier;
 * on a Community daemon every call would be refused, so a refusal or a transport failure silences further attempts
 * for FV_AUDIT_QUIET_S seconds. The idempotency token of daemon_call makes the one resend of a stateful call safe. */
#define FV_AUDIT_QUIET_S 30
#define FV_AUDIT_MAX_PAYLOAD (1u << 20)
static long long g_audit_quiet_until;      /* CLOCK_MONOTONIC seconds; atomic access */

void
fv_daemon_audit(const char *entry_type, const char *payload_json, size_t payload_len)
{
    if (!g_configured || entry_type == NULL || payload_json == NULL || payload_len == 0 ||
        payload_len > FV_AUDIT_MAX_PAYLOAD)
        return;
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    long long now = (long long) ts.tv_sec;
    if (now < __atomic_load_n(&g_audit_quiet_until, __ATOMIC_RELAXED)) return;

    fv_darg a[2];
    memset(a, 0, sizeof a);
    a[0].type = FSQ_T_STRING; a[0].s = entry_type;   a[0].len = strlen(entry_type);
    a[1].type = FSQ_T_STRING; a[1].s = payload_json; a[1].len = payload_len;
    fv_dres res;
    char err[256];
    err[0] = '\0';
    int ok = daemon_call("fractal_audit_log", 's', a, 2, &res, err, sizeof err) == 0 && res.status == FSQ_OK;
    if (!ok) __atomic_store_n(&g_audit_quiet_until, now + FV_AUDIT_QUIET_S, __ATOMIC_RELAXED);
    free(res.bytes);
}

/* Worker-side query generation for FRACTAL.AGENT.DATA_ANALYST: the daemon's text-to-query "generate" tier
 * (code-mode extraction of a single fenced block, with the caller-supplied system tag). Never retried. */
int
fv_daemon_generate(const char *prompt, const char *context, const char *system_tag,
                   char **out, size_t *out_len, char *err, size_t errsz)
{
    if (!g_configured) { snprintf(err, errsz, "the fractalsqld daemon is not configured"); return -1; }
    fv_darg a[4];
    memset(a, 0, sizeof a);
    a[0].type = FSQ_T_INT;    a[0].i = 0;
    a[1].type = FSQ_T_STRING; a[1].s = prompt;     a[1].len = strlen(prompt);
    a[2].type = FSQ_T_STRING; a[2].s = context;    a[2].len = strlen(context);
    a[3].type = FSQ_T_STRING; a[3].s = system_tag; a[3].len = strlen(system_tag);
    fv_dres res;
    if (daemon_call("fractal_t2s_generate", 'e', a, 4, &res, err, errsz) != 0) {
        if (err[0] == '\0') snprintf(err, errsz, "fractalsqld call failed");
        return -1;
    }
    if (res.status != FSQ_OK || res.is_null) {
        if (res.status == FSQ_ERR_CORE || res.is_null)
            snprintf(err, errsz, "the model did not return a usable query: is a reasoning provider configured in fractalsqld?");
        else if (err[0] == '\0')
            snprintf(err, errsz, "fractalsqld refused fractal_t2s_generate (status %u)", (unsigned) res.status);
        free(res.bytes);
        return -1;
    }
    char *s = (char *) malloc(res.len + 1);
    if (s == NULL) { free(res.bytes); snprintf(err, errsz, "out of memory"); return -1; }
    memcpy(s, res.bytes, res.len); s[res.len] = '\0';
    free(res.bytes);
    *out = s; *out_len = res.len;
    return 0;
}

/* Worker-side embedding call for the text-driven agents. Never retried (class
 * 'e'). On success *vec is a malloc'd double array of *n finite values. */
int
fv_daemon_embed(const char *text, double **vec, size_t *n, char *err, size_t errsz)
{
    if (!g_configured) {
        snprintf(err, errsz, "the fractalsqld daemon is not configured");
        return -1;
    }
    fv_darg args[2];
    memset(args, 0, sizeof args);
    args[0].type = FSQ_T_INT;    args[0].i = 0;
    args[1].type = FSQ_T_STRING; args[1].s = text; args[1].len = strlen(text);

    fv_dres res;
    if (daemon_call("fractal_embed", 'e', args, 2, &res, err, errsz) != 0) {
        if (err[0] == '\0') snprintf(err, errsz, "fractalsqld call failed");
        return -1;
    }
    if (res.status != FSQ_OK || res.is_null) {
        if (res.status == FSQ_ERR_CORE || res.is_null)
            snprintf(err, errsz, "fractal_embed failed in the daemon: is an embedding provider configured in fractalsqld?");
        else if (err[0] == '\0')
            snprintf(err, errsz, "fractalsqld refused fractal_embed (status %u)", (unsigned) res.status);
        free(res.bytes);
        return -1;
    }
    float *f = NULL; size_t fn = 0; char perr[128];
    int prc = fv_parse_vector_text((const char *) res.bytes, res.len, &f, &fn, perr, sizeof perr);
    free(res.bytes);
    if (prc != 0) { snprintf(err, errsz, "the embedding provider returned an unusable vector: %s", perr); return -1; }
    double *d = (double *) malloc(fn * sizeof(double) + 1);
    if (d == NULL) { free(f); snprintf(err, errsz, "out of memory"); return -1; }
    for (size_t i = 0; i < fn; i++) {
        if (!isfinite(f[i])) { free(f); free(d); snprintf(err, errsz, "the embedding contains a non-finite value"); return -1; }
        d[i] = (double) f[i];
    }
    free(f);
    *vec = d; *n = fn;
    return 0;
}

/* Enterprise multimodal portfolio optimizers (the daemon's fractal_optimize_portfolio_multimodal
 * and _pareto). The enterprise library lives only in the daemon, so a Community daemon answers
 * NULL and this reports "enterprise tier not loaded". mu is n values, cov n x n, both sent as
 * text. On success *out is the daemon's JSON ({"n_found":N,"candidates":[...]}), malloc'd. */
static char *
csv_of(const double *v, size_t n)
{
    char *s = (char *) malloc(n * 16 + 4), *o;
    if (s == NULL) return NULL;
    o = s;
    for (size_t i = 0; i < n; i++) o += sprintf(o, "%s%.9g", i ? "," : "", v[i]);   /* 9 digits round-trip a float32 */
    *o = '\0';
    return s;
}

int
fv_daemon_multimodal(int pareto, const double *mu, const double *cov, size_t n, long long k, long long restarts,
                     double overlap, double quality, long long max_front, long long seed,
                     char **out, size_t *out_len, char *err, size_t errsz)
{
    if (!g_configured) { snprintf(err, errsz, "the fractalsqld daemon is not configured"); return -1; }
    char *mus = csv_of(mu, n), *covs = csv_of(cov, n * n);
    if (mus == NULL || covs == NULL) { free(mus); free(covs); snprintf(err, errsz, "out of memory"); return -1; }
    fv_darg a[9];
    memset(a, 0, sizeof a);
    a[0].type = FSQ_T_STRING; a[0].s = mus;  a[0].len = strlen(mus);
    a[1].type = FSQ_T_STRING; a[1].s = covs; a[1].len = strlen(covs);
    a[2].type = FSQ_T_INT; a[2].i = k;
    a[3].type = FSQ_T_INT; a[3].i = restarts;
    int nargs, rc;
    const char *fn;
    if (pareto) {
        fn = "fractal_optimize_portfolio_multimodal_pareto"; nargs = 8;
        a[4].type = FSQ_T_INT; a[4].i = max_front;
        a[5].type = FSQ_T_INT; a[5].i = seed;
        a[6].type = FSQ_T_INT; a[6].i = 0;                                   /* use_obl */
        a[7].type = FSQ_T_STRING; a[7].s = "gaussian"; a[7].len = 8;         /* diffusion_mode */
    } else {
        fn = "fractal_optimize_portfolio_multimodal"; nargs = 7;
        a[4].type = FSQ_T_REAL; a[4].d = overlap;
        a[5].type = FSQ_T_REAL; a[5].d = quality;
        a[6].type = FSQ_T_INT; a[6].i = seed;
    }
    fv_dres res;
    rc = daemon_call(fn, 'r', a, nargs, &res, err, errsz);
    free(mus); free(covs);
    if (rc != 0) { if (err[0] == '\0') snprintf(err, errsz, "fractalsqld call failed"); return -1; }
    if (res.status != FSQ_OK || res.is_null) {
        if (res.status == FSQ_ERR_CORE || res.is_null)
            snprintf(err, errsz, "enterprise tier not loaded (set enterprise_lib in fractalsqld.conf), or the input was refused");
        else if (err[0] == '\0')
            snprintf(err, errsz, "fractalsqld refused %s (status %u)", fn, (unsigned) res.status);
        free(res.bytes);
        return -1;
    }
    char *s = (char *) malloc(res.len + 1);
    if (s == NULL) { free(res.bytes); snprintf(err, errsz, "out of memory"); return -1; }
    memcpy(s, res.bytes, res.len); s[res.len] = '\0';
    free(res.bytes);
    *out = s; *out_len = res.len;
    return 0;
}

typedef struct {
    fv_job  base;                 /* first */
    int     pareto;
    double *mu, *cov; size_t n;
    long long k, restarts, max_front, seed;
    double  overlap, quality;
} mm_job;

static void
mm_dispose(fv_job *b)
{
    mm_job *j = (mm_job *) b;
    free(j->mu); free(j->cov); free(j->base.res); free(j);
}

static void
mm_work(fv_job *b)
{
    mm_job *j = (mm_job *) b; char err[256] = ""; char *res = NULL; size_t rl = 0;
    if (fv_daemon_multimodal(j->pareto, j->mu, j->cov, j->n, j->k, j->restarts, j->overlap, j->quality,
                             j->max_front, j->seed, &res, &rl, err, sizeof err) != 0) { fv_job_fail(b, "%s", err[0] ? err : "failed"); return; }
    fv_job_bulk(b, res, rl);
}

/* FRACTAL.OPTIMIZE.MULTIMODAL        mu cov k n_restarts overlap quality [seed]
 * FRACTAL.OPTIMIZE.MULTIMODAL.PARETO mu cov k n_restarts [max_front [seed]] */
static int
multimodal_command(RedisModuleCtx *ctx, RedisModuleString **argv, int argc, int pareto)
{
    if (pareto ? (argc < 5 || argc > 7) : (argc < 7 || argc > 8)) return RedisModule_WrongArity(ctx);
    char err[256] = "";
    mm_job *j = (mm_job *) calloc(1, sizeof *j);
    if (j == NULL) return fv_reply_err(ctx, "out of memory");
    j->base.work = mm_work; j->base.dispose = mm_dispose; j->pareto = pareto;
    size_t nm = 0, nc = 0;
    if (fv_blob_to_doubles(argv[1], "mu", FV_MAX_BLOB_BYTES, &j->mu, &nm, err, sizeof err) != 0 ||
        fv_blob_to_doubles(argv[2], "cov", FV_MAX_BLOB_BYTES, &j->cov, &nc, err, sizeof err) != 0) { mm_dispose(&j->base); return fv_reply_err(ctx, err); }
    j->n = nm;
    if (nm > 512) { mm_dispose(&j->base); return fv_reply_err(ctx, "at most 512 assets"); }
    if (nc != nm * nm) { mm_dispose(&j->base); return fv_reply_err(ctx, "cov must be n_assets x n_assets"); }
    if (fv_arg_ll(argv[3], "k", 1, 512, &j->k, err, sizeof err) != 0 ||
        fv_arg_ll(argv[4], "n_restarts", 1, 64, &j->restarts, err, sizeof err) != 0) { mm_dispose(&j->base); return fv_reply_err(ctx, err); }
    if ((size_t) j->k > nm) { mm_dispose(&j->base); return fv_reply_err(ctx, "k must be <= n_assets"); }
    j->max_front = j->restarts;
    if (pareto) {
        if (argc >= 6 && fv_arg_ll(argv[5], "max_front", 1, 64, &j->max_front, err, sizeof err) != 0) { mm_dispose(&j->base); return fv_reply_err(ctx, err); }
        if (argc == 7 && fv_arg_ll(argv[6], "seed", 0, LLONG_MAX, &j->seed, err, sizeof err) != 0) { mm_dispose(&j->base); return fv_reply_err(ctx, err); }
    } else {
        if (fv_arg_double(argv[5], "overlap_threshold", &j->overlap, err, sizeof err) != 0 ||
            fv_arg_double(argv[6], "quality_frac", &j->quality, err, sizeof err) != 0) { mm_dispose(&j->base); return fv_reply_err(ctx, err); }
        if (!(j->overlap >= 0.0 && j->overlap <= 1.0)) { mm_dispose(&j->base); return fv_reply_err(ctx, "overlap_threshold must be in [0, 1]"); }
        if (!(j->quality > 0.0 && j->quality <= 1.0)) { mm_dispose(&j->base); return fv_reply_err(ctx, "quality_frac must be in (0, 1]"); }
        if (argc == 8 && fv_arg_ll(argv[7], "seed", 0, LLONG_MAX, &j->seed, err, sizeof err) != 0) { mm_dispose(&j->base); return fv_reply_err(ctx, err); }
    }
    if (!g_configured) { mm_dispose(&j->base); return fv_reply_err(ctx, "the fractalsqld daemon is not configured (load the module with: daemon-socket PATH daemon-key-file PATH)"); }
    j->base.bytes = (nm + nc) * sizeof(double) * 3 + 4096;
    return fv_dispatch(ctx, &j->base);
}
static int Multimodal_Cmd(RedisModuleCtx *c, RedisModuleString **v, int n)       { return multimodal_command(c, v, n, 0); }
static int MultimodalPareto_Cmd(RedisModuleCtx *c, RedisModuleString **v, int n) { return multimodal_command(c, v, n, 1); }

/* ---------------------------------------------------------------- */
/* commands                                                           */
/* ---------------------------------------------------------------- */
enum { RES_STR, RES_INT, RES_EMBED };

typedef struct {
    const char *cmd;
    const char *fn;
    char        cls;
    int         min_argc, max_argc;
    const char *argkinds;       /* C client id, s string arg, n integer arg */
    int         result;
    const char *failhint;       /* what a CORE failure most likely means */
    const char *acl;
} dcmd_desc;

static const dcmd_desc DESCS[] = {
    { "FRACTAL.REASON",              "fractal_reason",              'e', 2, 3, "Css", RES_STR,
      "is a reasoning provider configured in fractalsqld?", "slow dangerous blocking" },
    { "FRACTAL.EMBED",               "fractal_embed",               'e', 2, 2, "Cs",  RES_EMBED,
      "is an embedding provider configured in fractalsqld?", "slow dangerous blocking" },
    { "FRACTAL.LEDGER.FLUSH",        "fractal_ledger_flush",        's', 1, 1, "C",   RES_INT,
      ENT_HINT, "admin dangerous slow" },
    { "FRACTAL.LEDGER.LOAD",         "fractal_ledger_load",         's', 1, 1, "C",   RES_INT,
      ENT_HINT, "admin dangerous slow" },
    { "FRACTAL.LEDGER.COMPACT",      "fractal_ledger_compact",      's', 1, 1, "C",   RES_INT,
      ENT_HINT, "admin dangerous slow" },
    { "FRACTAL.LEDGER.RESET.SOFT",   "fractal_ledger_reset_soft",   's', 1, 1, "C",   RES_INT,
      ENT_HINT, "admin dangerous slow" },
    { "FRACTAL.LEDGER.RESET.HARD",   "fractal_ledger_reset_hard",   's', 1, 1, "C",   RES_INT,
      ENT_HINT, "admin dangerous slow" },
    { "FRACTAL.LEDGER.LOCK",         "fractal_lock_destructive",    's', 1, 1, "C",   RES_INT,
      ENT_HINT, "admin dangerous slow" },
    { "FRACTAL.LEDGER.TRUTH.COUNT",  "fractal_ledger_truth_count",  'r', 1, 1, "C",   RES_INT,
      ENT_HINT, "read slow" },
    { "FRACTAL.LEDGER.SHADOW.COUNT", "fractal_ledger_shadow_count", 'r', 1, 1, "C",   RES_INT,
      ENT_HINT, "read slow" },
    { "FRACTAL.LEDGER.VERIFY",       "fractal_ledger_verify",       'r', 1, 2, "Cn",  RES_STR,
      ENT_HINT, "read slow" },
    { "FRACTAL.AUDIT.LOG",           "fractal_audit_log",           's', 3, 3, "ss",  RES_INT,
      ENT_HINT, "admin dangerous slow" },
    { "FRACTAL.AUDIT.UNPACK",        "fractal_audit_unpack",        'r', 2, 2, "s",   RES_STR,
      ENT_HINT, "read slow" },
};
#define NDESC ((int) (sizeof DESCS / sizeof DESCS[0]))
_Static_assert(sizeof DESCS / sizeof DESCS[0] == 13, "one Dcmd_N wrapper per descriptor");

typedef struct {
    fv_job            base;
    const dcmd_desc  *d;
    fv_darg           args[4];
    int               nargs;
    char             *own[4];      /* copies of string arguments */
} dcmd;

static void
dcmd_dispose(fv_job *b)
{
    dcmd *j = (dcmd *) b;
    for (int i = 0; i < 4; i++) free(j->own[i]);
    free(j->base.res);
    free(j);
}

static void
dcmd_work(fv_job *b)
{
    dcmd *j = (dcmd *) b;
    fv_dres res; char err[256];
    if (daemon_call(j->d->fn, j->d->cls, j->args, j->nargs, &res, err, sizeof err) != 0) {
        fv_job_fail(b, "%s", err[0] ? err : "fractalsqld call failed");
        return;
    }
    if (res.status != FSQ_OK) {
        if (res.status == FSQ_ERR_CORE)
            fv_job_fail(b, "%s failed in the daemon (core_rc=%d): %s", j->d->fn, (int) res.core_rc, j->d->failhint);
        else if (err[0] != '\0')
            fv_job_fail(b, "%s", err);
        else
            fv_job_fail(b, "fractalsqld refused %s (status %u)", j->d->fn, (unsigned) res.status);
        free(res.bytes);
        return;
    }
    if (res.is_null) {
        fv_job_fail(b, "%s returned NULL: %s", j->d->fn, j->d->failhint);
        free(res.bytes);
        return;
    }
    switch (j->d->result) {
    case RES_INT:
        if (res.len == 8) fv_job_int(b, (long long) fsq_get_u64(res.bytes));
        else fv_job_fail(b, "%s returned a non-integer result", j->d->fn);
        break;
    case RES_EMBED: {
        float *v = NULL; size_t n = 0; char perr[128];
        if (fv_parse_vector_text((const char *) res.bytes, res.len, &v, &n, perr, sizeof perr) != 0) {
            fv_job_fail(b, "the embedding provider returned an unusable vector: %s", perr);
            break;
        }
        fv_job_bulk(b, (char *) v, n * sizeof(float));
        break;
    }
    default: {
        char *s = (char *) malloc(res.len + 1);
        if (s == NULL) { fv_job_fail(b, "out of memory"); break; }
        memcpy(s, res.bytes, res.len); s[res.len] = '\0';
        fv_job_bulk(b, s, res.len);
    }
    }
    free(res.bytes);
}

static int
daemon_command(RedisModuleCtx *ctx, RedisModuleString **argv, int argc, const dcmd_desc *d)
{
    if (argc < d->min_argc || argc > d->max_argc) return RedisModule_WrongArity(ctx);
    if (!g_configured)
        return fv_reply_err(ctx, "the fractalsqld daemon is not configured "
                                 "(load the module with: daemon-socket PATH daemon-key-file PATH)");
    dcmd *j = (dcmd *) calloc(1, sizeof *j);
    if (j == NULL) return fv_reply_err(ctx, "out of memory");
    j->d = d;
    j->base.work = dcmd_work;
    j->base.dispose = dcmd_dispose;

    int ai = 1;
    for (const char *k = d->argkinds; *k != '\0'; k++) {
        fv_darg *a = &j->args[j->nargs];
        if (*k == 'C') {
            a->type = FSQ_T_INT;
            a->i = (long long) RedisModule_GetClientId(ctx);
        } else if (ai >= argc) {
            break;                                  /* an optional argument that was not given */
        } else if (*k == 'n') {
            long long v;
            if (RedisModule_StringToLongLong(argv[ai], &v) != REDISMODULE_OK) {
                dcmd_dispose(&j->base);
                return fv_reply_err(ctx, "the argument must be an integer");
            }
            a->type = FSQ_T_INT; a->i = v; ai++;
        } else {
            size_t l; const char *p = RedisModule_StringPtrLen(argv[ai], &l);
            if (l > FV_DAEMON_MAX_REQUEST) { dcmd_dispose(&j->base); return fv_reply_err(ctx, "argument too large"); }
            if (strcmp(d->fn, "fractal_audit_log") == 0 && j->nargs == 1) {
                /* Attribute the audit entry. The payload gets an "_actor" block (the Valkey client id
                 * and ACL user, both server-assigned) spliced INSIDE its value; the daemon's {"type","entry"}
                 * chain shape is unchanged. A caller-supplied "_actor" key is refused so it cannot forge one. */
                if (memmem(p, l, "\"_actor\"", 8) != NULL) {
                    dcmd_dispose(&j->base);
                    return fv_reply_err(ctx, "the payload may not contain the reserved key \"_actor\"");
                }
                const char *pb = p, *pe = p + l;
                while (pb < pe && (*pb == ' ' || *pb == '\t' || *pb == '\n' || *pb == '\r')) pb++;
                while (pe > pb && (pe[-1] == ' ' || pe[-1] == '\t' || pe[-1] == '\n' || pe[-1] == '\r')) pe--;
                int is_obj = (pb < pe && *pb == '{' && pe[-1] == '}');
                int obj_empty = 0;
                if (is_obj) {
                    const char *q = pb + 1;
                    while (q < pe - 1 && (*q == ' ' || *q == '\t' || *q == '\n' || *q == '\r')) q++;
                    obj_empty = (q == pe - 1);
                }
                uint64_t cid = RedisModule_GetClientId(ctx);
                RedisModuleString *un = RedisModule_GetClientUserNameById(ctx, cid);
                const char *uname = "";
                if (un != NULL) { size_t ul; uname = RedisModule_StringPtrLen(un, &ul); }
                fv_sb blk; fv_sb_init(&blk);
                fv_sb_appendf(&blk, "{\"client_id\":%llu,\"user\":", (unsigned long long) cid);
                fv_sb_json_string(&blk, uname);
                /* GetClientUserNameById hands back a new string the caller owns: uname is not used past this point */
                if (un != NULL) RedisModule_FreeString(ctx, un);
                fv_sb_append(&blk, ",\"attested\":true}");
                fv_sb out; fv_sb_init(&out);
                if (is_obj && !obj_empty) {
                    fv_sb_appendn_raw(&out, pb, (size_t) ((pe - 1) - pb));
                    fv_sb_append(&out, ",\"_actor\":"); fv_sb_append(&out, blk.s); fv_sb_append(&out, "}");
                } else if (is_obj) {
                    fv_sb_append(&out, "{\"_actor\":"); fv_sb_append(&out, blk.s); fv_sb_append(&out, "}");
                } else {
                    fv_sb_append(&out, "{\"value\":"); fv_sb_appendn_raw(&out, pb, (size_t) (pe - pb));
                    fv_sb_append(&out, ",\"_actor\":"); fv_sb_append(&out, blk.s); fv_sb_append(&out, "}");
                }
                fv_sb_free(&blk);
                size_t ol = 0;
                char *ob = fv_sb_take(&out, &ol);
                if (ob == NULL || ol > FV_DAEMON_MAX_REQUEST) { free(ob); dcmd_dispose(&j->base); return fv_reply_err(ctx, "argument too large"); }
                j->own[j->nargs] = ob;
                a->type = FSQ_T_STRING; a->s = ob; a->len = ol; ai++;
            } else {
                j->own[j->nargs] = (char *) malloc(l ? l : 1);
                if (j->own[j->nargs] == NULL) { dcmd_dispose(&j->base); return fv_reply_err(ctx, "out of memory"); }
                memcpy(j->own[j->nargs], p, l);
                a->type = FSQ_T_STRING; a->s = j->own[j->nargs]; a->len = l; ai++;
            }
        }
        j->nargs++;
    }
    j->base.bytes = 4096;
    for (int i = 0; i < j->nargs; i++) if (j->args[i].type == FSQ_T_STRING) j->base.bytes += j->args[i].len;
    return fv_dispatch(ctx, &j->base);
}

#define DCMD(IDX) \
    static int Dcmd_##IDX(RedisModuleCtx *c, RedisModuleString **v, int n) { return daemon_command(c, v, n, &DESCS[IDX]); }
DCMD(0) DCMD(1) DCMD(2) DCMD(3) DCMD(4) DCMD(5) DCMD(6) DCMD(7) DCMD(8) DCMD(9) DCMD(10) DCMD(11) DCMD(12)

int
fv_register_daemon(RedisModuleCtx *ctx)
{
    static RedisModuleCmdFunc FN[] = {
        Dcmd_0, Dcmd_1, Dcmd_2, Dcmd_3, Dcmd_4, Dcmd_5, Dcmd_6, Dcmd_7, Dcmd_8, Dcmd_9, Dcmd_10, Dcmd_11, Dcmd_12
    };
    for (int i = 0; i < NDESC; i++) {
        if (RedisModule_CreateCommand(ctx, DESCS[i].cmd, FN[i], "readonly allow-stale", 0, 0, 0) == REDISMODULE_ERR)
            return REDISMODULE_ERR;
        if (fv_acl(ctx, DESCS[i].cmd, DESCS[i].acl) != 0) return REDISMODULE_ERR;
    }
    if (RedisModule_CreateCommand(ctx, "FRACTAL.OPTIMIZE.MULTIMODAL", Multimodal_Cmd, "readonly allow-stale", 0, 0, 0) == REDISMODULE_ERR ||
        RedisModule_CreateCommand(ctx, "FRACTAL.OPTIMIZE.MULTIMODAL.PARETO", MultimodalPareto_Cmd, "readonly allow-stale", 0, 0, 0) == REDISMODULE_ERR ||
        fv_acl(ctx, "FRACTAL.OPTIMIZE.MULTIMODAL", "slow dangerous blocking") != 0 ||
        fv_acl(ctx, "FRACTAL.OPTIMIZE.MULTIMODAL.PARETO", "slow dangerous blocking") != 0)
        return REDISMODULE_ERR;
    return REDISMODULE_OK;
}
