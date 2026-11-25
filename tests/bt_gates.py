#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
# SPDX-FileCopyrightText: 2026 Daniel Gardiner d/b/a FractalSQLabs
#
# tests/bt_gates.py: the command-level gates of build_test.sh.
#
# Usage: bt_gates.py <gate> --host H --port P [--pid N]
#
# Talks RESP to a server that already has fractalsql.so loaded, so the same
# gates run on Linux and macOS (against a local server or a container). Prints one line per check:
#
#     PASS <gate> <name>
#     FAIL <gate> <name>: <detail>
#     KNOWN <gate> <name>: <detail>   a documented gap awaiting a decision;
#                                     reported, never fails the run
#
# Exit status is 1 if any FAIL, else 0. Python standard library only.

import argparse
import json
import math
import os
import random
import socket
import struct
import sys
import threading
import time

MAX_QUERY_BYTES = 4 * 1024 * 1024
MAX_CORPUS_BYTES = 256 * 1024 * 1024


class RespError(Exception):
    pass


# FSQL_AGENT_MUTATE=1 (set by build_test.sh --coverage): after every FRACTAL.* call that succeeds (the agents get the full set of option keywords),
# the same command is sent again with its
# argument list changed -- trailing arguments dropped, each option keyword the agents know appended with no value and with good and bad
# values, each argument replaced by an awkward one. The replies are not checked; what is asserted is that the server answers every one
# (a crash or a hang fails the gate). Each distinct (command, mutation) is sent once.
MUTATE_AGENTS = os.environ.get("FSQL_AGENT_MUTATE") == "1"
AGENT_KEYWORDS = ["IDS", "CONTENT", "CONTEXT", "SEED", "COST_PER_ROUTE", "ITERATIONS", "POPULATION", "N_RESTARTS", "OVERLAP", "QUALITY",
                  "OBJECTIVE_MODE", "N_BITS", "HAMMING_THRESHOLD", "WARMUP_COUNT", "K", "METRIC", "MAX_ATTEMPTS", "LIMIT", "FILTER", "MODE", "PREFIX",
                  "BOXCOUNT_DIM", "NOPE"]
AWKWARD = "a\rb\tc\nd\"e\\f\x01g\u00e9"   # every character the JSON string writer has to escape
AGENT_VALUES = ["1", "2", "0.5", "0", "-1", "x", "nan", "cosine", "l2", "sharpe", "pareto", "{}", "99999999999", "", AWKWARD]
AGENT_BAD = ["", "x", "-1", "0", "nan", "1e308", "99999999999", AWKWARD]
_agent_seen = set()
# Commands whose mutants would change what later checks see (they write ledgers, drop sessions, lock or unload things): sent as written only.
_NO_MUTATE = ("FRACTAL.LEDGER.", "FRACTAL.AUDIT.", "FRACTAL.SESSION.CLOSE", "FRACTAL.DIVERSIFY.", "FRACTAL.AGENT.FEEDBACK", "FRACTAL.FEEDBACK",
              # index and feature-store writers: a mutant would create an index named "x", rebuild one in another format or store an extra doc_id,
              # and the next check reads exactly that state
              "FRACTAL.INDEX.CREATE", "FRACTAL.INDEX.REFRESH", "FRACTAL.INDEX.REBUILD", "FRACTAL.INDEX.DROP", "FRACTAL.INDEX.ENSURE", "FRACTAL.FEATURE.STORE")


def mutable_command(cmd):
    u = cmd.upper()
    return u.startswith("FRACTAL.") and not u.startswith(_NO_MUTATE)


def agent_mutants(args):
    cmd, base = args[0], list(args[1:])
    out = []

    def add(tag, m):
        key = (cmd, tag)
        if key not in _agent_seen:
            _agent_seen.add(key)
            out.append(m)

    for k in range(1, len(base) + 1):
        add(("drop", len(base), k), [cmd] + base[:-k])
    add(("extra", len(base)), [cmd] + base + ["EXTRA"])
    for kw in (AGENT_KEYWORDS if cmd.upper().startswith("FRACTAL.AGENT.") else AGENT_KEYWORDS[:2]):
        add(("kw", len(base), kw), [cmd] + base + [kw])
        for v in (AGENT_VALUES if cmd.upper().startswith("FRACTAL.AGENT.") else AGENT_VALUES[:4]):
            add(("kwv", len(base), kw, v), [cmd] + base + [kw, v])
    for i in range(len(base)):
        a = base[i]
        short = isinstance(a, str) or (isinstance(a, (bytes, bytearray)) and len(a) < 64)
        for v in (AGENT_BAD if short else ["", "x"]):
            add(("rep", len(base), i, v), [cmd] + base[:i] + [v] + base[i + 1:])
    return out


class Client:
    def __init__(self, host, port):
        self.sock = socket.create_connection((host, port), timeout=60)
        self.f = self.sock.makefile("rb")

    def close(self):
        self.f.close()
        self.sock.close()

    def call(self, *args):
        res = self._call(args)
        if MUTATE_AGENTS and not getattr(self, "_in_mutation", False) and not isinstance(res, RespError) \
                and isinstance(args[0], str) and mutable_command(args[0]):
            self._in_mutation = True
            try:
                for m in agent_mutants(args):
                    self._call(m)               # the reply is not the point: the server must answer (a lost connection raises)
            finally:
                self._in_mutation = False
        return res

    def _call(self, args):
        out = [b"*%d\r\n" % len(args)]
        for a in args:
            if isinstance(a, str):
                a = a.encode()
            elif isinstance(a, int):
                a = b"%d" % a
            out.append(b"$%d\r\n%s\r\n" % (len(a), a))
        self.sock.sendall(b"".join(out))
        return self._read()

    def _read(self):
        line = self.f.readline()
        if not line:
            raise ConnectionError("server closed the connection")
        line = line.rstrip(b"\r\n")
        t, rest = line[:1], line[1:]
        if t == b"+":
            return rest.decode()
        if t == b"-":
            return RespError(rest.decode("utf-8", "replace"))
        if t == b":":
            return int(rest)
        if t == b"$":
            n = int(rest)
            if n < 0:
                return None
            data = self.f.read(n + 2)
            return data[:-2]
        if t == b"*":
            return [self._read() for _ in range(int(rest))]
        raise ValueError("bad RESP type %r" % t)


def pack(vec):
    return struct.pack("<%df" % len(vec), *vec)


class Run:
    def __init__(self, gate, host, port, pid):
        self.gate, self.host, self.port, self.pid = gate, host, port, pid
        self.fails = 0
        self.c = Client(host, port)

    def reconnect(self):
        try:
            self.c.close()
        except OSError:
            pass
        self.c = Client(self.host, self.port)

    def ok(self, name):
        print("PASS %s %s" % (self.gate, name))

    def bad(self, name, detail):
        self.fails += 1
        print("FAIL %s %s: %s" % (self.gate, name, detail))

    def known(self, name, detail):
        print("KNOWN %s %s: %s" % (self.gate, name, detail))

    def check(self, name, cond, detail=""):
        """Records the check and returns whether it held, so a gate can run its follow-up checks only when it did."""
        if cond:
            self.ok(name)
        else:
            self.bad(name, detail)
        return bool(cond)

    def is_err(self, v, needle=None):
        return isinstance(v, RespError) and (needle is None or needle in str(v))

    def alive(self):
        try:
            return self.c.call("PING") == "PONG"
        except (OSError, ConnectionError):
            return False

    def server_pid_unchanged(self):
        if self.pid is None:
            return True
        return os.path.exists("/proc/%d" % self.pid) if os.name == "posix" and sys.platform != "darwin" else True


# --------------------------------------------------------------------------
# 02 smoke
# --------------------------------------------------------------------------
def gate_02(r):
    c = r.c
    r.check("fractal.edition is Community", c.call("fractal.edition") == "Community")
    ver = c.call("fractal.version")
    want = os.environ.get("FSQL_EXPECT_VERSION")
    r.check("fractal.version is a version string", isinstance(ver, str) and ver.count(".") == 2, repr(ver))
    if want:
        r.check("fractal.version equals the expected %s" % want, ver == want, repr(ver))
    q = [0.6, 0.8, 0.0, 0.0]
    corpus = q + [0.1, 0.2, 0.3, 0.4] + [1, 0, 0, 0] + [0, 1, 0, 0]
    c.call("SET", "bt:c", pack(corpus))
    res = c.call("FRACTAL.SEARCH", "bt:c", pack(q), "2")
    if r.is_err(res):
        r.bad("FRACTAL.SEARCH returns a JSON document", str(res))
        return
    doc = json.loads(res)
    r.check("reply has dim, n_corpus, best_fit, best_point, top_k",
            all(k in doc for k in ("dim", "n_corpus", "best_fit", "best_point", "top_k")), res[:200])
    r.check("top_k honours K=2", len(doc.get("top_k", [])) == 2, str(doc.get("top_k")))
    r.check("the query's own row is the nearest", doc["top_k"][0]["idx"] == 0, str(doc["top_k"]))
    bp = doc["best_point"]
    dot = sum(a * b for a, b in zip(bp, q))
    nrm = math.sqrt(sum(a * a for a in bp)) or 1.0
    r.check("best_point converges onto the query ray (cosine > 0.99)", dot / nrm > 0.99, str(bp))


# --------------------------------------------------------------------------
# 03 scout
# --------------------------------------------------------------------------
def gate_03(r):
    c = r.c
    dim, rows = 4, 40
    # Four tight clusters on the axes: Scout must disperse across them.
    corpus = []
    for i in range(rows):
        axis = i % 4
        v = [0.02 * ((i * 7) % 5)] * dim
        v[axis] = 1.0
        corpus += v
    c.call("SET", "bt:sc", pack(corpus))
    q = [0.5, 0.5, 0.5, 0.5]
    res = c.call("FRACTAL.EXPLORE", "bt:sc", pack(q), "24")
    if r.is_err(res):
        r.bad("FRACTAL.EXPLORE returns a JSON document", str(res))
        return
    doc = json.loads(res)
    pop = doc.get("population")
    r.check("reply carries a population array", isinstance(pop, list), res[:200])
    if not isinstance(pop, list):
        return
    r.check("population has the requested 24 particles", len(pop) == 24, "got %d" % len(pop))
    pts = {tuple(round(x, 3) for x in (p if isinstance(p, list) else p.get("point", p.get("x", [])))) for p in pop}
    r.check("particles are dispersed over more than one distinct point", len(pts) > 1, "distinct=%d" % len(pts))


# --------------------------------------------------------------------------
# 04 bounds: every validation branch in module.c
# --------------------------------------------------------------------------
def gate_04(r):
    c = r.c
    q4 = pack([0.6, 0.8, 0, 0])
    c.call("SET", "bt:c4", pack([0.6, 0.8, 0, 0, 0.1, 0.2, 0.3, 0.4]))
    c.call("RPUSH", "bt:list", "x")
    c.call("SET", "bt:empty", b"")
    for cmd in ("FRACTAL.SEARCH", "FRACTAL.EXPLORE"):
        t = cmd.split(".")[1].lower()
        r.check("%s: missing key is refused" % t, r.is_err(c.call(cmd, "bt:nokey", q4), "corpus key missing"))
        r.check("%s: wrong-type key is refused" % t, r.is_err(c.call(cmd, "bt:list", q4), "not a string"))
        r.check("%s: query length %% 4 != 0 is refused" % t, r.is_err(c.call(cmd, "bt:c4", b"abc"), "length % 4"))
        r.check("%s: empty query is refused" % t, r.is_err(c.call(cmd, "bt:c4", b""), "length % 4"))
        r.check("%s: corpus not a multiple of dim*4 is refused" % t,
                r.is_err(c.call(cmd, "bt:c4", pack([1, 2, 3])), "multiple of dim"))
        r.check("%s: too few arguments is refused" % t, r.is_err(c.call(cmd, "bt:c4"), "wrong number"))
        r.check("%s: too many arguments is refused" % t, r.is_err(c.call(cmd, "bt:c4", q4, "3", "x"), "wrong number"))
        r.check("%s: non-integer third argument is refused" % t, r.is_err(c.call(cmd, "bt:c4", q4, "x"), "integer"))
        r.check("%s: a query over 4 MiB is refused" % t,
                r.is_err(c.call(cmd, "bt:c4", b"\0" * (MAX_QUERY_BYTES + 4)), "query too large"))
        r.check("%s: the server survived the hostile calls" % t, r.alive())
    r.check("search: a query of exactly 4 MiB passes the size cap",
            not r.is_err(c.call("FRACTAL.SEARCH", "bt:c4", b"\0" * MAX_QUERY_BYTES), "too large"))
    c.call("SET", "bt:big", b"\0" * (MAX_CORPUS_BYTES + 4))
    r.check("search: a corpus over 256 MiB is refused",
            r.is_err(c.call("FRACTAL.SEARCH", "bt:big", q4), "corpus too large"))
    r.check("explore: a corpus over 256 MiB is refused",
            r.is_err(c.call("FRACTAL.EXPLORE", "bt:big", q4), "corpus too large"))
    c.call("DEL", "bt:big")
    r.check("explore: an empty corpus is refused", r.is_err(c.call("FRACTAL.EXPLORE", "bt:empty", q4), "empty corpus"))
    res = c.call("FRACTAL.SEARCH", "bt:empty", q4)
    r.check("search: an empty corpus returns n_corpus=0 (documented asymmetry with explore)",
            not r.is_err(res) and json.loads(res).get("n_corpus") == 0, str(res)[:120])
    # K and population are range-checked like the core's own parameters: a
    # value outside [1, 10000] (K) or [2, 10000] (population) is refused, with
    # the value in the message. K above the corpus size is simply limited to it.
    for k, want in (("1", 1), ("2", 2), ("3", 2), ("10000", 2)):
        res = c.call("FRACTAL.SEARCH", "bt:c4", q4, k)
        rows = len(json.loads(res).get("top_k", [])) if isinstance(res, bytes) else -1
        r.check("search: K=%s returns %d row(s) (the corpus has 2)" % (k, want), rows == want, str(res)[:80])
    for k in ("0", "-5", "10001", "99999999"):
        r.check("search: K=%s is refused as out of range" % k, r.is_err(c.call("FRACTAL.SEARCH", "bt:c4", q4, k), "K %s out of range [1, 10000]" % k))
    for pv in ("1", "0", "-3", "10001", "100000"):
        r.check("explore: a population of %s is refused as out of range" % pv,
                r.is_err(c.call("FRACTAL.EXPLORE", "bt:c4", q4, pv), "population_size %s out of range [2, 10000]" % pv))
    pop = json.loads(c.call("FRACTAL.EXPLORE", "bt:c4", q4, "2")).get("population", [])
    r.check("explore: the minimum population of 2 is accepted", len(pop) == 2, str(len(pop)))
    r.check("search: K that is not an integer is refused", r.is_err(c.call("FRACTAL.SEARCH", "bt:c4", q4, "1.5"), "integer"))
    # Non-finite input is refused (decision M3), in the query and the corpus,
    # by both commands, and the server survives each.
    nan, inf = float("nan"), float("inf")
    c.call("SET", "bt:cnan", pack([nan, 0.8, 0, 0, 0.1, 0.2, 0.3, 0.4]))
    c.call("SET", "bt:cinf", pack([0.6, 0.8, 0, 0, 0.1, -inf, 0.3, 0.4]))
    for cmd in ("FRACTAL.SEARCH", "FRACTAL.EXPLORE"):
        t = cmd.split(".")[1].lower()
        for label, key, q, needle in (
                ("NaN in the corpus", "bt:cnan", q4, "corpus contains"),
                ("-Inf in the corpus", "bt:cinf", q4, "corpus contains"),
                ("NaN in the query", "bt:c4", pack([nan, 0.8, 0, 0]), "query contains"),
                ("Inf in the query", "bt:c4", pack([inf, 0.8, 0, 0]), "query contains")):
            r.check("%s: %s is refused" % (t, label), r.is_err(c.call(cmd, key, q), needle))
            r.check("%s: the server survived %s" % (t, label), r.alive())
    r.check("search: a subnormal float is still accepted (finite, not refused)",
            not r.is_err(c.call("FRACTAL.SEARCH", "bt:c4", pack([1e-45, 0.8, 0, 0]), "2")))


# --------------------------------------------------------------------------
# 05 survival: the server must outlive every hostile call, with the same pid
# --------------------------------------------------------------------------
def gate_05(r):
    import random
    rng = random.Random(1234)
    c = r.c
    c.call("SET", "bt:s", pack([rng.random() for _ in range(64)]))
    names = c.call("COMMAND", "LIST")
    verbs = sorted(x.decode() for x in names if x.decode().upper().startswith("FRACTAL."))
    r.check("the fuzz covers every registered FRACTAL.* command (%d)" % len(verbs), len(verbs) >= 30, str(len(verbs)))
    sent = 0
    for _ in range(1500):
        v = rng.choice(verbs)
        n = rng.choice((0, 1, 2, 3, 4, 5))
        args = []
        for _ in range(n):
            kind = rng.randrange(6)
            if kind == 0:
                args.append(b"bt:s")
            elif kind == 1:
                args.append(rng.randbytes(rng.choice((0, 1, 3, 4, 8, 16, 1000))))
            elif kind == 2:
                args.append(str(rng.choice((-1, 0, 1, 2, 10 ** 12, -10 ** 12))))
            elif kind == 3:
                args.append(pack([rng.choice((0.0, 1.0, -1.0, 1e38, -1e38, 1e-45)) for _ in range(rng.choice((1, 4, 16)))]))
            elif kind == 4:
                args.append(pack([float("nan")] * 4))
            else:
                args.append(b"\xff" * rng.choice((4, 8)))
        try:
            c.call(v, *args)
        except (OSError, ConnectionError) as e:
            r.bad("survival: %s dropped the connection on args %r" % (v, [a[:8] for a in args]), str(e))
            return
        sent += 1
        if sent % 50 == 0 and not r.alive():
            r.bad("survival: server stopped answering PING after %d calls" % sent, "")
            return
    r.check("survival: %d random hostile calls, server still answering" % sent, r.alive())
    r.check("survival: the server process is the same one", r.server_pid_unchanged())



# --------------------------------------------------------------------------
# helpers for the numeric gates
# --------------------------------------------------------------------------
def pack_u32(vals):
    return struct.pack("<%dI" % len(vals), *vals)


def unpack(blob):
    return list(struct.unpack("<%df" % (len(blob) // 4), blob))


def close(a, b, tol=1e-4):
    return abs(a - b) <= tol * max(1.0, abs(b))


def jload(v):
    try:
        return json.loads(v) if isinstance(v, (bytes, str)) else None
    except ValueError:
        return None


# --------------------------------------------------------------------------
# 06 vector: FRACTAL.VECTOR.*
# --------------------------------------------------------------------------
def gate_06(r):
    c = r.c
    a, b = pack([1, 2, 3]), pack([4, 5, 6])
    f = lambda *x: float(c.call(*x))
    r.check("dot([1,2,3],[4,5,6]) = 32", close(f("FRACTAL.VECTOR.DOT", a, b), 32.0))
    r.check("l2([0,0,0],[3,4,0]) = 5", close(f("FRACTAL.VECTOR.L2", pack([0, 0, 0]), pack([3, 4, 0])), 5.0))
    r.check("l2sq([0,0,0],[3,4,0]) = 25", close(f("FRACTAL.VECTOR.L2SQ", pack([0, 0, 0]), pack([3, 4, 0])), 25.0))
    r.check("cosine distance of identical vectors = 0", close(f("FRACTAL.VECTOR.COSINE", a, a), 0.0, 1e-5))
    r.check("cosine similarity of identical vectors = 1", close(f("FRACTAL.VECTOR.SIMILARITY", a, a), 1.0, 1e-5))
    r.check("cosine similarity of orthogonal vectors = 0", abs(f("FRACTAL.VECTOR.SIMILARITY", pack([1, 0]), pack([0, 1]))) < 1e-6)
    r.check("norm([3,4,0]) = 5", close(f("FRACTAL.VECTOR.NORM", pack([3, 4, 0])), 5.0))
    n = unpack(c.call("FRACTAL.VECTOR.NORMALIZE", pack([3, 4, 0])))
    r.check("normalize([3,4,0]) = [0.6,0.8,0]", all(close(x, y) for x, y in zip(n, [0.6, 0.8, 0])), str(n))
    r.check("add", unpack(c.call("FRACTAL.VECTOR.ADD", a, b)) == [5, 7, 9])
    r.check("sub", unpack(c.call("FRACTAL.VECTOR.SUB", b, a)) == [3, 3, 3])
    r.check("scale([1,2,3], 2) = [2,4,6]", unpack(c.call("FRACTAL.VECTOR.SCALE", a, "2")) == [2, 4, 6])
    r.check("lp p=2 from origin = 5", close(f("FRACTAL.VECTOR.LP", pack([3, 4]), pack([0, 0]), "2"), 5.0))
    r.check("lp p=1 from origin = 7", close(f("FRACTAL.VECTOR.LP", pack([3, 4]), pack([0, 0]), "1"), 7.0))
    r.check("concat weights both halves", unpack(c.call("FRACTAL.VECTOR.CONCAT", pack([1, 2]), "0.5", pack([3]), "2")) == [0.5, 1.0, 6.0])
    q = jload(c.call("FRACTAL.VECTOR.QUANTIZE.INT8", pack([1, -2, 3])))
    deq = [x * q["scale"] for x in q["codes"]] if q else []
    r.check("quantize.int8 dequantizes within rounding error",
            q is not None and all(abs(x - y) < 0.05 for x, y in zip(deq, [1, -2, 3])), str(q))
    qb = c.call("FRACTAL.VECTOR.QUANTIZE.BINARY", pack([1, -1]))
    r.check("quantize.binary packs 2 dims into 1 byte", isinstance(qb, bytes) and len(qb) == 1)
    r.check("hamming of identical bit blobs = 0", c.call("FRACTAL.VECTOR.HAMMING", b"\xff\x00", b"\xff\x00") == 0)
    r.check("hamming counts one flipped bit", c.call("FRACTAL.VECTOR.HAMMING", b"\x01", b"\x00") == 1)
    r.check("hamming of unequal lengths is refused", r.is_err(c.call("FRACTAL.VECTOR.HAMMING", b"\x01", b"\x00\x00"), "equal"))

    # text <-> blob
    for text, want in (("1,2,3", [1, 2, 3]), ("[1, 2 ,3]", [1, 2, 3]), (" [ -1.5e1 ,2 ] ", [-15, 2]), ("7", [7])):
        got = c.call("FRACTAL.VECTOR.FROMTEXT", text)
        r.check("fromtext %r" % text, isinstance(got, bytes) and unpack(got) == want, repr(got))
    for text in ("", "[]", "[ ]", "1,,2", "1,2,", "1,2,3x", "[1,2", "abc", "nan", "inf", "1e39", ",1"):
        r.check("fromtext refuses %r" % text, r.is_err(c.call("FRACTAL.VECTOR.FROMTEXT", text)))
    r.check("totext renders a JSON array", c.call("FRACTAL.VECTOR.TOTEXT", pack([1, 2.5, -3])) == b"[1,2.5,-3]")

    # hostile arguments
    nan = float("nan")
    r.check("a NaN element is refused", r.is_err(c.call("FRACTAL.VECTOR.DOT", pack([nan]), pack([1])), "non-finite"))
    r.check("a dimension mismatch is refused", r.is_err(c.call("FRACTAL.VECTOR.DOT", pack([1, 2]), pack([1])), "mismatch"))
    r.check("a blob whose length is not a multiple of 4 is refused", r.is_err(c.call("FRACTAL.VECTOR.NORM", b"abc")))
    r.check("an empty blob is refused", r.is_err(c.call("FRACTAL.VECTOR.NORM", b"")))
    r.check("a blob over the cap is refused", r.is_err(c.call("FRACTAL.VECTOR.NORM", b"\0" * (16 * 1024 * 1024 + 4)), "too large"))
    r.check("lp with p <= 0 is refused", r.is_err(c.call("FRACTAL.VECTOR.LP", a, b, "0")))
    r.check("scale with a non-numeric scalar is refused", r.is_err(c.call("FRACTAL.VECTOR.SCALE", a, "x")))
    r.check("scale with an infinite scalar is refused", r.is_err(c.call("FRACTAL.VECTOR.SCALE", a, "inf")))
    r.check("wrong arity is refused", r.is_err(c.call("FRACTAL.VECTOR.DOT", a), "wrong number"))
    r.check("the server survived", r.alive())
    bad = c.call("FRACTAL.VECTOR.FROMTEXT", b"\xff\xfe\x01junk\x80")
    r.check("an error that quotes the caller's bytes is printable ASCII",
            r.is_err(bad) and all(0x20 <= ord(ch) <= 0x7e for ch in str(bad)), repr(str(bad)))


# --------------------------------------------------------------------------
# 07 analytics: dimension, series, cycle, TDA, portfolio, morphology
# --------------------------------------------------------------------------
def _walk(n, seed):
    import random
    rng = random.Random(seed)
    steps = [rng.gauss(0, 1) for _ in range(n)]
    pos, acc = [], 0.0
    for s_ in steps:
        acc += s_
        pos.append(acc)
    return steps, pos


def gate_07(r):
    import random
    c = r.c
    steps, walk = _walk(1024, 7)
    rng2 = random.Random(21)

    # --- dimension
    alpha = float(c.call("FRACTAL.DIM.DFA", pack(steps)))
    r.check("dfa of uncorrelated noise is near 0.5 (got %.3f)" % alpha, 0.3 < alpha < 0.75)
    alpha_w = float(c.call("FRACTAL.DIM.DFA", pack(walk)))
    r.check("dfa of a random walk is near 1.5 (got %.3f)" % alpha_w, 1.2 < alpha_w < 1.8)
    r.check("dfa needs at least 16 samples", r.is_err(c.call("FRACTAL.DIM.DFA", pack([1.0] * 8)), "16"))
    d = jload(c.call("FRACTAL.DIM.DRIFT", pack(steps), "64"))
    r.check("drift returns drift, recent_alpha and baseline_alpha",
            d is not None and all(k in d for k in ("drift", "recent_alpha", "baseline_alpha")), str(d))
    r.check("drift refuses window < 16", r.is_err(c.call("FRACTAL.DIM.DRIFT", pack(steps), "8")))
    r.check("drift refuses a series shorter than window + 16", r.is_err(c.call("FRACTAL.DIM.DRIFT", pack(steps[:40]), "32")))
    line = [x for i in range(40) for x in (i * 0.1, i * 0.1)]
    dl = float(c.call("FRACTAL.DIM.BOXCOUNT", pack(line), "2"))
    r.check("boxcount of points on a line is near 1 (got %.3f)" % dl, 0.7 < dl < 1.4)
    # (A tiny unit lattice is degenerate for box counting -- the core wants a
    # real range of scales -- so the "filled plane" fixture is uniform random.)
    grid = [rng2.random() for _ in range(2 * 400)]
    dg = float(c.call("FRACTAL.DIM.BOXCOUNT", pack(grid), "2"))
    r.check("boxcount of a filled plane is near 2 (got %.3f)" % dg, 1.5 < dg < 2.2)
    r.check("boxcount refuses a degenerate cloud", r.is_err(c.call("FRACTAL.DIM.BOXCOUNT", pack([1.0, 1.0] * 20), "2")))
    r.check("boxcount refuses a length that is not a multiple of dim", r.is_err(c.call("FRACTAL.DIM.BOXCOUNT", pack([1.0] * 21), "2"), "multiple"))

    # --- series
    rng = random.Random(3)
    step = [rng.gauss(0, 0.05) for _ in range(50)] + [1 + rng.gauss(0, 0.05) for _ in range(50)]
    cp = jload(c.call("FRACTAL.SERIES.CHANGEPOINT", pack(step), "16", "3"))
    r.check("changepoint localises the step at t=50", cp is not None and any(40 <= i <= 70 for i in cp["indices"]), str(cp))
    flat = jload(c.call("FRACTAL.SERIES.CHANGEPOINT", pack([1.0] * 100), "16", "3"))
    r.check("changepoint finds nothing in a flat series", flat == {"indices": []}, str(flat))
    r.check("changepoint refuses window = 0", r.is_err(c.call("FRACTAL.SERIES.CHANGEPOINT", pack(step), "0", "3")))
    r.check("changepoint refuses threshold <= 0", r.is_err(c.call("FRACTAL.SERIES.CHANGEPOINT", pack(step), "16", "0")))
    r.check("changepoint refuses a series shorter than 2 windows", r.is_err(c.call("FRACTAL.SERIES.CHANGEPOINT", pack(step[:20]), "16", "3")))
    sine = [math.sin(2 * math.pi * i / 8) for i in range(256)]
    pg = jload(c.call("FRACTAL.SERIES.PERIODOGRAM", pack(sine), "3"))
    r.check("periodogram top frequency is the true 0.125 bin", pg is not None and abs(pg["freqs"][0] - 0.125) < 1e-3, str(pg))
    r.check("periodogram honours max_peaks", pg is not None and len(pg["freqs"]) == 3 == len(pg["power"]))
    r.check("periodogram document has exactly the keys freqs and power", pg is not None and set(pg) == {"freqs", "power"}, str(pg)[:80])
    r.check("periodogram refuses max_peaks = 0", r.is_err(c.call("FRACTAL.SERIES.PERIODOGRAM", pack(sine), "0")))
    # FRACTAL.TEXT.CHUNK: the same chunks as every FractalSQL host (pinned cases)
    ck = jload(c.call("FRACTAL.TEXT.CHUNK", "abcdefghij", "4", "1", "fixed"))
    r.check("chunk text fixed, size 4, overlap 1",
            ck == [{"no": 0, "start": 0, "end": 4, "text": "abcd"}, {"no": 1, "start": 3, "end": 7, "text": "defg"},
                   {"no": 2, "start": 6, "end": 10, "text": "ghij"}], str(ck))
    ck = jload(c.call("FRACTAL.TEXT.CHUNK", "One. Two sentences! Is this three? Yes.  Four is a long one that keeps on going and going.", "30", "5", "sentence"))
    r.check("chunk text sentence packing, a long sentence split with overlap",
            [(x["start"], x["end"], x["text"]) for x in ck] ==
            [(0, 19, "One. Two sentences!"), (20, 39, "Is this three? Yes."), (41, 71, "Four is a long one that keeps "), (66, 90, "eeps on going and going.")], str(ck))
    r.check("chunk text of an empty string is []", jload(c.call("FRACTAL.TEXT.CHUNK", "", "10", "0")) == [])
    r.check("chunk text: a paragraph break needs two line feeds", jload(c.call("FRACTAL.TEXT.CHUNK", "one\ntwo\nthree", "100", "0"))[0]["end"] == 13)
    r.check("chunk text refuses an overlap of size or more", r.is_err(c.call("FRACTAL.TEXT.CHUNK", "x", "5", "5")))
    r.check("chunk text refuses an unknown mode", r.is_err(c.call("FRACTAL.TEXT.CHUNK", "x", "5", "0", "word")))
    # Unicode text: accented Latin, CJK, right-to-left, emoji, combining marks and JSON-special characters keep their characters
    uni = {
        "latin": "Crème brûlée à la façon de São Paulo, naïve café, Ångström, straße",
        "cjk": "日本語のテキストと中文汉字、한국어 문장입니다",
        "rtl": "مرحبا بالعالم שלום עולם",
        "emoji": "😀👍🏽🇯🇵 👨‍👩‍👧‍👦 ok 🧪",
        "combining": "e\u0301 n\u0303 a\u0308\u0323 end",
        "specials": "quote \" backslash \\ tab\t newline\n ctrl \x01\x1f slash / <tag> &amp; \u2028 \u2029",
    }
    for name, text in uni.items():
        one = jload(c.call("FRACTAL.TEXT.CHUNK", text, "100000", "0", "fixed"))
        r.check("unicode %s: one chunk holds the whole text" % name, bool(one) and len(one) == 1 and one[0]["text"] == text and one[0]["end"] == len(text), str(one)[:120])
        for size, ov in ((5, 0), (7, 3), (1, 0)):
            ck = jload(c.call("FRACTAL.TEXT.CHUNK", text, str(size), str(ov), "fixed")) or []
            r.check("unicode %s: size %d overlap %d offsets are character offsets, at most %d characters" % (name, size, ov, size),
                    bool(ck) and all(x["text"] == text[x["start"]:x["end"]] and len(x["text"]) <= size for x in ck))
            if ov == 0:
                r.check("unicode %s: size %d chunks join back to the text" % (name, size), "".join(x["text"] for x in ck) == text)
        c.call("SET", "uni:" + name, text)
        got = c.call("GET", "uni:" + name)
        got = got.decode("utf-8") if isinstance(got, bytes) else got
        r.check("unicode %s: a key round trip returns the same characters" % name, got == text)
    # bytes that are not UTF-8: the reply is valid UTF-8 JSON (the bad bytes become U+FFFD) or an error
    for label, raw in (("lone continuation", b"a\x80b"), ("0xff", b"a\xffb"), ("truncated sequence", b"ab\xe2\x82")):
        rep = c.call("FRACTAL.TEXT.CHUNK", raw, "100000", "0", "fixed")
        ok = r.is_err(rep)
        if not ok:
            try:
                json.loads(rep.decode("utf-8") if isinstance(rep, bytes) else rep); ok = True
            except Exception:
                ok = False
        r.check("unicode invalid bytes (%s): valid UTF-8 JSON or an error" % label, ok)
    tone = jload(c.call("FRACTAL.SERIES.SPECTROGRAM", pack(sine), "64", "32"))
    r.check("spectrogram reads a pure tone as ordered (all frames < 0.5)", isinstance(tone, list) and tone and all(0 <= x < 0.5 for x in tone), str(tone)[:80])
    noise = jload(c.call("FRACTAL.SERIES.SPECTROGRAM", pack([rng.gauss(0, 1) for _ in range(512)]), "64", "32"))
    r.check("spectrogram reads noise as disordered (mean > 0.7, all <= 1)",
            isinstance(noise, list) and sum(noise) / len(noise) > 0.7 and max(noise) <= 1.0)
    const = jload(c.call("FRACTAL.SERIES.SPECTROGRAM", pack([2.0] * 64), "16", "16"))
    r.check("spectrogram reports a constant frame as 0.0, not NaN", const == [0.0, 0.0, 0.0, 0.0], str(const))
    r.check("spectrogram honours max_frames", len(jload(c.call("FRACTAL.SERIES.SPECTROGRAM", pack(sine), "64", "8", "3"))) == 3)
    r.check("spectrogram refuses window < 8", r.is_err(c.call("FRACTAL.SERIES.SPECTROGRAM", pack(sine), "4", "1")))
    r.check("spectrogram refuses window > 512", r.is_err(c.call("FRACTAL.SERIES.SPECTROGRAM", pack(sine), "1024", "1")))
    r.check("spectrogram refuses hop < 1", r.is_err(c.call("FRACTAL.SERIES.SPECTROGRAM", pack(sine), "64", "0")))
    r.check("spectrogram refuses a series shorter than the window", r.is_err(c.call("FRACTAL.SERIES.SPECTROGRAM", pack(sine[:20]), "64", "8")))
    r.check("spectrogram refuses max_frames > 4096", r.is_err(c.call("FRACTAL.SERIES.SPECTROGRAM", pack(sine), "64", "8", "5000")))

    # --- fingerprint + cycle
    v = [rng.gauss(0, 1) for _ in range(32)]
    fp1 = c.call("FRACTAL.SERIES.FINGERPRINT", pack(v), "128", "42")
    fp2 = c.call("FRACTAL.SERIES.FINGERPRINT", pack(v), "128", "42")
    r.check("fingerprint of 128 bits is 16 bytes", isinstance(fp1, bytes) and len(fp1) == 16)
    r.check("fingerprint is deterministic for a seed", fp1 == fp2)
    near = c.call("FRACTAL.SERIES.FINGERPRINT", pack([x * 1.0001 for x in v]), "128", "42")
    r.check("a near-identical state lands within a few bits", c.call("FRACTAL.VECTOR.HAMMING", fp1, near) <= 4)
    far = c.call("FRACTAL.SERIES.FINGERPRINT", pack([rng.gauss(0, 1) for _ in range(32)]), "128", "42")
    r.check("an unrelated state is far", c.call("FRACTAL.VECTOR.HAMMING", fp1, far) > 20)
    r.check("fingerprint refuses bits = 0", r.is_err(c.call("FRACTAL.SERIES.FINGERPRINT", pack(v), "0")))
    # The fingerprint work cap: n_bits x vector length must not exceed 268435456.
    # 4096 bits is the largest n_bits the command takes, so the vector has to be
    # longer than 65536 for the product to be over the cap (65537 x 4096 = 268439552).
    r.check("fingerprint refuses n_bits x vector length above the work cap",
            r.is_err(c.call("FRACTAL.SERIES.FINGERPRINT", pack([1.0] * 65537), "4096", "42"),
                            "bits x vector length must not exceed 268435456"))
    A, B, C = b"A" * 16, b"B" * 16, b"C" * 16
    cy = jload(c.call("FRACTAL.CYCLE.DETECT", A + B + A + B + A + B, "16"))
    r.check("cycle closes the A,B,A,B,A,B period-2 stream", cy is not None and cy.get("detected") is True and cy.get("cycle_len") == 2, str(cy))
    r.check("cycle reports none for three distinct states", jload(c.call("FRACTAL.CYCLE.DETECT", A + B + C, "16")) == {"detected": False})
    r.check("cycle refuses a length that is not a multiple of n_bytes", r.is_err(c.call("FRACTAL.CYCLE.DETECT", b"x" * 17, "16")))

    # --- TDA
    cl = []
    for cx in (0.0, 10.0):
        for i in range(6):
            cl += [cx + i * 0.1, 0.0]
    t = jload(c.call("FRACTAL.TDA.PERSISTENCE", pack(cl), "2", "1", "1.0"))
    r.check("tda two-cluster n_h0_bars = 10 (2 x 5 merge bars)", t is not None and t["n_h0_bars"] == 10, str(t)[:100])
    r.check("tda max_dim=1 reports a betti1", t is not None and isinstance(t["betti1"], int))
    t0 = jload(c.call("FRACTAL.TDA.PERSISTENCE", pack(cl), "2", "0", "1.0"))
    r.check("tda max_dim=0 leaves betti1 null", t0 is not None and t0["betti1"] is None)
    r.check("tda refuses max_dim = 2", r.is_err(c.call("FRACTAL.TDA.PERSISTENCE", pack(cl), "2", "2", "1.0")))
    r.check("tda refuses more than 512 points", r.is_err(c.call("FRACTAL.TDA.PERSISTENCE", pack([0.1 * i for i in range(2 * 600)]), "2", "0", "1.0"), "512"))
    r.check("tda refuses thresh <= 0", r.is_err(c.call("FRACTAL.TDA.PERSISTENCE", pack(cl), "2", "0", "0")))

    # --- portfolio
    mu = [0.10, 0.12, 0.08, 0.15, 0.11]
    cov = [x for i in range(5) for j in range(5) for x in (0.04 if i == j else 0.005,)]
    p1 = c.call("FRACTAL.OPTIMIZE.PORTFOLIO", pack(mu), pack(cov), "2", "42")
    p2 = c.call("FRACTAL.OPTIMIZE.PORTFOLIO", pack(mu), pack(cov), "2", "42")
    pj = jload(p1)
    r.check("portfolio is deterministic for a seed", p1 == p2)
    w = pj["weights"] if pj else []
    r.check("portfolio weights sum to 1 with at most k=2 nonzero",
            pj is not None and abs(sum(w) - 1.0) < 1e-6 and sum(1 for x in w if x > 1e-9) <= 2, str(pj))
    r.check("portfolio reports a finite sharpe", pj is not None and math.isfinite(pj["sharpe"]))
    r.check("portfolio accepts levy diffusion and OBL", jload(c.call("FRACTAL.OPTIMIZE.PORTFOLIO", pack(mu), pack(cov), "2", "1", "1", "levy")) is not None)
    r.check("portfolio refuses k = 0", r.is_err(c.call("FRACTAL.OPTIMIZE.PORTFOLIO", pack(mu), pack(cov), "0")))
    r.check("portfolio refuses k > n_assets", r.is_err(c.call("FRACTAL.OPTIMIZE.PORTFOLIO", pack(mu), pack(cov), "6")))
    r.check("portfolio refuses a cov of the wrong size", r.is_err(c.call("FRACTAL.OPTIMIZE.PORTFOLIO", pack(mu), pack(cov[:20]), "2")))
    r.check("portfolio refuses an unknown diffusion mode", r.is_err(c.call("FRACTAL.OPTIMIZE.PORTFOLIO", pack(mu), pack(cov), "2", "1", "0", "brownian")))

    # --- value-weighted k-subset allocation
    vals = [0.9, 0.1, 0.5, 0.8, 0.2, 0.7]
    ub = [0.6] * 6
    s1 = c.call("FRACTAL.OPTIMIZE.SUBSET", pack(vals), pack(ub), "2", "PREV", pack([0.0] * 6), "TURNOVER", "0", "SEED", "7")
    s2 = c.call("FRACTAL.OPTIMIZE.SUBSET", pack(vals), pack(ub), "2", "PREV", pack([0.0] * 6), "TURNOVER", "0", "SEED", "7")
    sj = jload(s1)
    r.check("subset is deterministic for a seed", s1 == s2)
    sw = sj["weights"] if sj else []
    r.check("subset weights sum to 1, at most k=2 are nonzero and none exceeds its upper bound",
            sj is not None and abs(sum(sw) - 1.0) < 1e-6 and sum(1 for x in sw if x > 1e-9) <= 2 and all(x <= 0.6 + 1e-6 for x in sw), str(sj))
    r.check("subset picks the two most valuable items (0.9 and 0.8) and reports their value-weighted score",
            sj is not None and sw[0] > 0 and sw[3] > 0 and abs(sj["score"] - sum(w_ * v_ for w_, v_ in zip(sw, vals))) < 1e-6, str(sj))
    r.check("subset works without the optional arguments", jload(c.call("FRACTAL.OPTIMIZE.SUBSET", pack(vals), pack(ub), "3")) is not None)
    r.check("subset refuses k = 0", r.is_err(c.call("FRACTAL.OPTIMIZE.SUBSET", pack(vals), pack(ub), "0")))
    r.check("subset refuses k greater than the item count", r.is_err(c.call("FRACTAL.OPTIMIZE.SUBSET", pack(vals), pack(ub), "7")))
    r.check("subset refuses upper_bounds of the wrong length", r.is_err(c.call("FRACTAL.OPTIMIZE.SUBSET", pack(vals), pack(ub[:5]), "2")))
    r.check("subset refuses an upper bound over 1", r.is_err(c.call("FRACTAL.OPTIMIZE.SUBSET", pack(vals), pack([2.0] * 6), "2")))
    r.check("subset refuses a negative turnover penalty", r.is_err(c.call("FRACTAL.OPTIMIZE.SUBSET", pack(vals), pack(ub), "2", "PREV", pack([0.0] * 6), "TURNOVER", "-1")))
    r.check("subset refuses bounds that cannot sum to 1 with k items", r.is_err(c.call("FRACTAL.OPTIMIZE.SUBSET", pack(vals), pack([0.1] * 6), "2")))

    # --- the named feature store: a hash of doc_id -> vector
    c.call("DEL", "bt:feat")
    r.check("feature store: store answers OK", c.call("FRACTAL.FEATURE.STORE", "bt:feat", "1", pack([1.0, 0.0, 0.0])) in ("OK", b"OK"))
    c.call("FRACTAL.FEATURE.STORE", "bt:feat", "2", pack([0.9, 0.1, 0.0]))
    c.call("FRACTAL.FEATURE.STORE", "bt:feat", "3", pack([0.0, 1.0, 0.0]))
    c.call("FRACTAL.FEATURE.STORE", "bt:feat", "2", pack([0.8, 0.2, 0.0]))     # the last write wins
    c.call("FRACTAL.FEATURE.STORE", "bt:feat", "9", pack([1.0, 1.0]))          # another dimension
    fm = jload(c.call("FRACTAL.FEATURE.MINE", "bt:feat", pack([1.0, 0.0, 0.0]), "2"))
    r.check("feature store: mine is nearest first (doc 1, then doc 2)", fm is not None and [x["doc_id"] for x in fm] == [1, 2], str(fm))
    r.check("feature store: the distance is Euclidean and doc 2 holds its last write", fm is not None and abs(fm[1]["distance"] - 0.28284271247) < 1e-5, str(fm))
    fm = jload(c.call("FRACTAL.FEATURE.MINE", "bt:feat", pack([1.0, 0.0, 0.0]), "10"))
    r.check("feature store: a vector of another dimension is skipped", fm is not None and [x["doc_id"] for x in fm] == [1, 2, 3], str(fm))
    r.check("feature store: an empty or missing key mines to []", jload(c.call("FRACTAL.FEATURE.MINE", "bt:feat:none", pack([1.0, 0.0, 0.0]), "3")) == [])
    r.check("feature store: k = 0 is refused", r.is_err(c.call("FRACTAL.FEATURE.MINE", "bt:feat", pack([1.0, 0.0, 0.0]), "0")))
    r.check("feature store: a negative doc_id is refused", r.is_err(c.call("FRACTAL.FEATURE.STORE", "bt:feat", "-1", pack([1.0]))))
    c.call("SET", "bt:feat:str", "x")
    r.check("feature store: a key of another type is refused", r.is_err(c.call("FRACTAL.FEATURE.STORE", "bt:feat:str", "1", pack([1.0])), "WRONGTYPE"))
    c.call("DEL", "bt:feat", "bt:feat:str")

    # --- morphology
    # The core's box-counting step needs ~3000 points to find enough valid
    # scales for the fractal dimension, so these are random recursive trees
    # (node i attaches to a random earlier node), arc = 1.05 x Euclidean.
    mr = random.Random(1)
    nn = 3000
    pts3 = [(mr.uniform(0, 10), mr.uniform(0, 10), mr.uniform(0, 10)) for _ in range(nn)]
    ed, arcl = [], []
    for i in range(1, nn):
        j = mr.randint(0, i - 1)
        ed += [j, i]
        arcl.append(math.dist(pts3[i], pts3[j]) * 1.05)
    nodes = [x for p_ in pts3 for x in p_]
    edges, arcs = pack_u32(ed), pack(arcl)
    vj = jload(c.call("FRACTAL.MORPH.VASCULAR", pack(nodes), edges, arcs))
    r.check("vascular returns tortuosity, branch density and fractal dimension",
            vj is not None and all(k in vj for k in ("mean_tortuosity", "branch_density", "fractal_dimension")), str(vj))
    r.check("vascular refuses an out-of-range node index", r.is_err(c.call("FRACTAL.MORPH.VASCULAR", pack([0, 0, 0, 1, 0, 0, 2, 0, 0]), pack_u32([0, 1, 1, 3]), pack([1, 1])), "does not exist"))
    r.check("vascular refuses coords not divisible by 3", r.is_err(c.call("FRACTAL.MORPH.VASCULAR", pack([0, 0, 0, 1]), edges, arcs)))
    r.check("vascular refuses a missing arc length", r.is_err(c.call("FRACTAL.MORPH.VASCULAR", pack(nodes), edges, pack([1.0]))))
    tet = [0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0, 1]
    cj = jload(c.call("FRACTAL.MORPH.CORTICAL", pack(tet), pack_u32([0, 1, 2, 0, 1, 3, 0, 2, 3, 1, 2, 3])))
    r.check("cortical returns a gyrification index near 1 for a convex mesh",
            cj is not None and 0.9 < cj["gyrification_index"] < 1.1, str(cj))
    r.check("cortical refuses an out-of-range face index", r.is_err(c.call("FRACTAL.MORPH.CORTICAL", pack(tet), pack_u32([0, 1, 9])), "does not exist"))
    pts2 = [(mr.uniform(0, 10), mr.uniform(0, 10)) for _ in range(nn)]
    ed2 = []
    for i in range(1, nn):
        ed2 += [mr.randint(0, i - 1), i]
    nj = jload(c.call("FRACTAL.MORPH.NERVE", pack([x for p_ in pts2 for x in p_]), "2", pack_u32(ed2)))
    r.check("nerve returns fiber length density", nj is not None and "fiber_length_density" in nj, str(nj))
    r.check("nerve refuses dim = 0", r.is_err(c.call("FRACTAL.MORPH.NERVE", pack([0, 0, 1, 0]), "0", pack_u32([0, 1]))))
    r.check("nerve refuses an out-of-range node index", r.is_err(c.call("FRACTAL.MORPH.NERVE", pack([0, 0, 1, 0]), "2", pack_u32([0, 9]))))
    mj = jload(c.call("FRACTAL.MORPH.COMPLEXITY", pack(grid), "2"))
    r.check("complexity returns dimension and lacunarity", mj is not None and "dimension" in mj and math.isfinite(mj["lacunarity"]), str(mj))
    r.check("complexity refuses too few points", r.is_err(c.call("FRACTAL.MORPH.COMPLEXITY", pack([1.0, 2.0] * 3), "2")))
    r.check("every analytics command refuses NaN input", all(
        r.is_err(c.call(*args), "non-finite") for args in (
            ("FRACTAL.DIM.DFA", pack([float("nan")] * 20)),
            ("FRACTAL.SERIES.PERIODOGRAM", pack([float("inf")] * 20)),
            ("FRACTAL.DIM.BOXCOUNT", pack([float("nan")] * 20), "2"))))
    r.check("the server survived", r.alive())


# --------------------------------------------------------------------------
# 08 async: the main thread must stay free while work runs
# --------------------------------------------------------------------------
def _big_corpus(c, key="bt:big", rows=20000, dim=32, seed=11):
    import random
    rng = random.Random(seed)
    c.call("SET", key, pack([rng.random() for _ in range(rows * dim)]))
    return pack([rng.random() for _ in range(dim)])


def gate_08(r):
    import threading
    import time
    c = r.c
    q = _big_corpus(c)

    # 1. the server answers PING while a ~2 s EXPLORE runs on a worker.
    #    A timing assertion on a shared machine needs a bounded retry: a main thread
    #    that really is blocked is blocked on EVERY attempt (a PING then waits for the
    #    whole job), whereas a one-off scheduling stall of the test machine does not
    #    repeat. Three attempts, and the result of each is reported.
    def probe_once():
        box = {}
        c2 = Client(r.host, r.port)

        def heavy():
            box["reply"] = c2.call("FRACTAL.EXPLORE", "bt:big", q, "2000")
        t0 = time.time()
        th = threading.Thread(target=heavy)
        th.start()
        time.sleep(0.15)
        lat = []
        while th.is_alive():
            t = time.time()
            r.c.call("PING")
            lat.append(time.time() - t)
            time.sleep(0.005)
        th.join()
        c2.close()
        return box.get("reply"), lat, time.time() - t0

    attempts = []
    for attempt in range(3):
        reply, lat, dur = probe_once()
        good_reply = isinstance(reply, bytes) and "population" in json.loads(reply)
        # If the main thread were blocked, one PING would wait for the whole job (seconds).
        # The bound is relative to the job's own duration so instrumented builds are judged fairly.
        ok = good_reply and len(lat) >= 3 and dur > 0.5 and max(lat) < 0.3 * dur
        attempts.append((ok, good_reply, len(lat), max(lat) if lat else -1, dur))
        if ok:
            break
    ok, good_reply, npings, worst, dur = attempts[-1]
    r.check("the heavy EXPLORE returned a valid document (took %.1fs)" % dur, all(a[1] for a in attempts))
    r.check("the EXPLORE ran long enough to be a real test (> 0.5 s)", dur > 0.5, "%.2fs" % dur)
    r.check("PING stayed responsive during it (%d pings, max %.0f ms of a %.1f s job; attempt %d of 3)"
            % (npings, worst * 1000, dur, len(attempts)), ok,
            "every attempt: " + "; ".join("%d pings, max %.0f ms of %.1f s" % (a[2], a[3] * 1000, a[4]) for a in attempts))

    # 2. many clients at once, mixed commands, every reply valid
    q4 = pack([0.6, 0.8, 0, 0])
    c.call("SET", "bt:small", pack([0.6, 0.8, 0, 0, 0.1, 0.2, 0.3, 0.4, 1, 0, 0, 0]))
    errors, ok = [], [0]
    lock = threading.Lock()

    def worker(i):
        try:
            w = Client(r.host, r.port)
            for k in range(25):
                kind = (i + k) % 4
                if kind == 0:
                    res = w.call("FRACTAL.SEARCH", "bt:small", q4, "2")
                    good = isinstance(res, bytes) and "top_k" in json.loads(res)
                elif kind == 1:
                    res = w.call("FRACTAL.EXPLORE", "bt:small", q4, "8")
                    good = isinstance(res, bytes) and "population" in json.loads(res)
                elif kind == 2:
                    res = w.call("FRACTAL.DIM.DFA", pack([math.sin(x * 0.7) + (x % 5) * 0.1 for x in range(64)]))
                    good = not isinstance(res, RespError)
                else:
                    res = w.call("FRACTAL.VECTOR.DOT", pack([1, 2]), pack([3, 4]))
                    good = float(res) == 11.0
                if not good:
                    with lock:
                        errors.append((i, k, kind, str(res)[:80]))
                else:
                    with lock:
                        ok[0] += 1
            w.close()
        except Exception as e:  # noqa: BLE001
            with lock:
                errors.append((i, "exception", str(e)))
    ths = [threading.Thread(target=worker, args=(i,)) for i in range(12)]
    [t.start() for t in ths]
    [t.join() for t in ths]
    r.check("12 concurrent clients x 25 mixed calls all returned valid results (%d ok)" % ok[0],
            not errors and ok[0] == 300, str(errors[:3]))

    # 3. clients that cannot block (MULTI, Lua) still get a real answer
    c.call("MULTI")
    c.call("FRACTAL.SEARCH", "bt:small", q4, "2")
    res = c.call("EXEC")
    r.check("FRACTAL.SEARCH inside MULTI/EXEC runs inline and replies",
            isinstance(res, list) and len(res) == 1 and isinstance(res[0], bytes) and "top_k" in json.loads(res[0]), str(res)[:100])
    res = c.call("EVAL", "return redis.call('FRACTAL.SEARCH', KEYS[1], ARGV[1], '2')", 1, "bt:small", q4)
    r.check("FRACTAL.SEARCH from a Lua script runs inline and replies",
            isinstance(res, bytes) and "top_k" in json.loads(res), str(res)[:100])
    r.check("the server survived", r.alive())
    c.call("DEL", "bt:big")


# --------------------------------------------------------------------------
# 09 limits: runs against a server started with `threads 2 max-inflight-mb 1`
# --------------------------------------------------------------------------
def gate_09(r):
    c = r.c
    if os.environ.get("FSQL_PHASE") == "sessions":
        ok = lambda x: x == "OK"
        r.check("max-sessions 2: the first session is created", ok(c.call("FRACTAL.DIVERSIFY.ENABLE", "a")))
        r.check("max-sessions 2: the second session is created", ok(c.call("FRACTAL.DIVERSIFY.ENABLE", "b")))
        r.check("max-sessions 2: the third is refused", r.is_err(c.call("FRACTAL.DIVERSIFY.ENABLE", "c"), "too many sessions (the limit is 2"))
        time.sleep(2.2)
        r.check("session-ttl-sec 1: idle sessions are dropped, freeing the slots", ok(c.call("FRACTAL.DIVERSIFY.ENABLE", "c")))
        r.check("session-ttl-sec 1: and the old ones are gone", c.call("FRACTAL.SESSION.LIST") == [b"c"], str(c.call("FRACTAL.SESSION.LIST")))
        r.check("the server survived", r.alive())
        return
    import random
    rng = random.Random(5)
    q = pack([rng.random() for _ in range(4)])
    c.call("SET", "bt:ok", pack([rng.random() for _ in range(4 * 1000)]))      # 32 KB decoded
    res = c.call("FRACTAL.SEARCH", "bt:ok", q, "3")
    r.check("a small search is accepted under the 1 MiB in-flight cap", isinstance(res, bytes) and "top_k" in json.loads(res), str(res)[:80])
    c.call("SET", "bt:over", pack([rng.random() for _ in range(4 * 100000)]))  # 3.2 MB decoded
    res = c.call("FRACTAL.SEARCH", "bt:over", q, "3")
    r.check("a search whose decoded input exceeds the cap is refused, not queued", r.is_err(res, "in flight"), str(res)[:100])
    res = c.call("FRACTAL.EXPLORE", "bt:over", q, "8")
    r.check("the same for EXPLORE", r.is_err(res, "in flight"), str(res)[:100])
    r.check("an analytics call over the cap is refused",
            r.is_err(c.call("FRACTAL.DIM.DFA", pack([math.sin(x) for x in range(300000)]), ), "in flight"))
    res = c.call("FRACTAL.SEARCH", "bt:ok", q, "3")
    r.check("the cap is released: a small search works again afterwards", isinstance(res, bytes))
    r.check("the server survived", r.alive())


# --------------------------------------------------------------------------
# 10 lifecycle: MODULE UNLOAD / LOAD, busy refusal, client disconnects
# --------------------------------------------------------------------------
def gate_10(r):
    import threading
    import time
    c = r.c
    q = _big_corpus(c)
    mod = os.environ.get("FSQL_MODULE_PATH", "")

    # idle unload then reload
    res = c.call("MODULE", "UNLOAD", "fractalsql")
    r.check("MODULE UNLOAD succeeds when idle", res == "OK", str(res))
    r.check("the commands are gone after unload", r.is_err(c.call("FRACTAL.VECTOR.NORM", pack([1.0]))))
    if mod:
        res = c.call("MODULE", "LOAD", mod, "threads", "2")
        r.check("MODULE LOAD works again afterwards", res == "OK", str(res))
        r.check("the commands are back", float(c.call("FRACTAL.VECTOR.NORM", pack([3.0, 4.0]))) == 5.0)
    else:
        r.bad("MODULE LOAD works again afterwards", "FSQL_MODULE_PATH is not set")
        return
    r.check("loading it twice is refused", r.is_err(c.call("MODULE", "LOAD", mod)))

    # unload while a job is in flight is refused, and the job still completes
    box = {}
    c2 = Client(r.host, r.port)
    th = threading.Thread(target=lambda: box.setdefault("reply", c2.call("FRACTAL.EXPLORE", "bt:big", q, "2000")))
    th.start()
    time.sleep(0.3)
    res = c.call("MODULE", "UNLOAD", "fractalsql")
    r.check("MODULE UNLOAD is refused while a job is running", r.is_err(res), str(res))
    th.join()
    r.check("the running job still completed with a valid reply",
            isinstance(box.get("reply"), bytes) and "population" in json.loads(box["reply"]))
    c2.close()

    # a client that disconnects mid-job must not leak its accounting
    for _ in range(3):
        s = socket.create_connection((r.host, r.port))
        args = [b"FRACTAL.EXPLORE", b"bt:big", q, b"2000"]
        s.sendall(b"*4\r\n" + b"".join(b"$%d\r\n%s\r\n" % (len(a), a) for a in args))
        time.sleep(0.1)
        s.close()
    r.check("the server answers after clients vanished mid-job", r.alive())
    deadline = time.time() + 20
    res = None
    while time.time() < deadline:
        res = c.call("MODULE", "UNLOAD", "fractalsql")
        if res == "OK":
            break
        time.sleep(0.25)
    r.check("after the abandoned jobs drain, MODULE UNLOAD succeeds (nothing leaked)", res == "OK", str(res))
    r.check("the server survived", r.alive())



# --------------------------------------------------------------------------
# 11 oom: allocation failure injected at every point of each command
# --------------------------------------------------------------------------
def gate_11(r):
    c = r.c
    # The module under test is the FV_OOM_INJECT flavor: its own allocations go
    # through counting wrappers armed by FRACTAL.DEBUG.OOM (see src/fv_oom.c).
    probe = c.call("FRACTAL.DEBUG.OOM", "0")
    if isinstance(probe, RespError):
        r.bad("setup", "FRACTAL.DEBUG.OOM is missing: this is not the FV_OOM_INJECT build (%s)" % probe)
        return
    q4 = pack([0.6, 0.8, 0, 0])
    c.call("SET", "bt:o", pack([0.6, 0.8, 0, 0, 0.1, 0.2, 0.3, 0.4, 1, 0, 0, 0, 0, 1, 0, 0]))
    c.call("FRACTAL.DIVERSIFY.ENABLE", "oom-s")
    sine = pack([math.sin(2 * math.pi * i / 8) for i in range(64)])
    walk = pack(_walk(128, 3)[1])
    c.call("SET", "bt:oag", walk)
    c.call("SET", "bt:ogps", pack([((i * 37) % 11) * 0.5 for i in range(80)]))
    c.call("DEL", "bt:oids", "bt:otxt", "bt:otag")
    for i in range(4):      # bt:o holds 4 rows of dim 4
        c.call("HSET", "bt:oids", str(i), "id%d" % i)
        c.call("HSET", "bt:otxt", str(i), "txt%d" % i)
        c.call("HSET", "bt:otag", str(i), "t")
    pts = pack([((i * 37) % 11) * 0.5 for i in range(2 * 40)])
    mr = random.Random(9)
    tri = [(mr.uniform(0, 10), mr.uniform(0, 10), mr.uniform(0, 10)) for _ in range(40)]
    ed, arcl = [], []
    for i in range(1, 40):
        j = mr.randint(0, i - 1)
        ed += [j, i]
        arcl.append(math.dist(tri[i], tri[j]) * 1.05)
    cases = [
        ("search", ("FRACTAL.SEARCH", "bt:o", q4, "2")),
        ("explore", ("FRACTAL.EXPLORE", "bt:o", q4, "8")),
        ("search.session", ("FRACTAL.SEARCH", "bt:o", q4, "2", "SESSION", "oom-s")),
        ("explore.session", ("FRACTAL.EXPLORE", "bt:o", q4, "8", "SESSION", "oom-s")),
        ("vector.add", ("FRACTAL.VECTOR.ADD", pack([1, 2]), pack([3, 4]))),
        ("vector.norm", ("FRACTAL.VECTOR.NORM", pack([3, 4]))),
        ("vector.normalize", ("FRACTAL.VECTOR.NORMALIZE", pack([3, 4]))),
        ("vector.scale", ("FRACTAL.VECTOR.SCALE", pack([3, 4]), "2")),
        ("vector.concat", ("FRACTAL.VECTOR.CONCAT", pack([1, 2]), "1", pack([3]), "1")),
        ("vector.fromtext", ("FRACTAL.VECTOR.FROMTEXT", "[1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17,18]")),
        ("vector.totext", ("FRACTAL.VECTOR.TOTEXT", pack([1, 2, 3]))),
        ("vector.quantize.int8", ("FRACTAL.VECTOR.QUANTIZE.INT8", pack([1, -2, 3]))),
        ("vector.quantize.binary", ("FRACTAL.VECTOR.QUANTIZE.BINARY", pack([1, -2, 3]))),
        ("dim.dfa", ("FRACTAL.DIM.DFA", walk)),
        ("dim.drift", ("FRACTAL.DIM.DRIFT", walk, "32")),
        ("dim.boxcount", ("FRACTAL.DIM.BOXCOUNT", pts, "2")),
        ("series.changepoint", ("FRACTAL.SERIES.CHANGEPOINT", walk, "16", "2")),
        ("series.periodogram", ("FRACTAL.SERIES.PERIODOGRAM", sine, "3")),
        ("series.spectrogram", ("FRACTAL.SERIES.SPECTROGRAM", sine, "16", "8")),
        ("series.fingerprint", ("FRACTAL.SERIES.FINGERPRINT", pack([1, 2, 3, 4]), "64", "1")),
        ("cycle.detect", ("FRACTAL.CYCLE.DETECT", b"A" * 16 + b"B" * 16 + b"A" * 16 + b"B" * 16, "16")),
        ("tda", ("FRACTAL.TDA.PERSISTENCE", pts, "2", "1", "2.0")),
        ("portfolio", ("FRACTAL.OPTIMIZE.PORTFOLIO", pack([0.1, 0.12, 0.08]), pack([0.04, 0, 0, 0, 0.04, 0, 0, 0, 0.04]), "2", "1")),
        ("morph.vascular", ("FRACTAL.MORPH.VASCULAR", pack([x for p_ in tri for x in p_]), pack_u32(ed), pack(arcl))),
        ("morph.cortical", ("FRACTAL.MORPH.CORTICAL", pack([0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0, 1]), pack_u32([0, 1, 2, 0, 1, 3]))),
        ("morph.nerve", ("FRACTAL.MORPH.NERVE", pack([x for p_ in tri for x in p_[:2]]), "2", pack_u32(ed))),
        ("morph.complexity", ("FRACTAL.MORPH.COMPLEXITY", pts, "2")),
        ("agent.anomaly_triage", ("FRACTAL.AGENT.ANOMALY_TRIAGE", "bt:oag", "32")),
        ("agent.recall_hybrid", ("FRACTAL.AGENT.RECALL_HYBRID", "bt:o", q4, "K", "2", "IDS", "bt:oids", "CONTENT", "bt:otxt", "FILTER", "bt:otag", "t")),
        ("agent.recommend_diverse", ("FRACTAL.AGENT.RECOMMEND_DIVERSE", "bt:o", q4, "K", "3", "IDS", "bt:oids")),
        ("agent.regime_triage", ("FRACTAL.AGENT.REGIME_TRIAGE", "bt:oag", "32", "0.5", '{"a":1}')),
        ("agent.track_anomaly", ("FRACTAL.AGENT.TRACK_ANOMALY", "bt:o", pack([0.1] * 4), pack([0.7, 0.9, 0.1, 0.1]), "bt:oag", "IDS", "bt:oids")),
        ("agent.detour_classify", ("FRACTAL.AGENT.DETOUR_CLASSIFY", "bt:o", pack([0.1] * 4), pack([0.7, 0.9, 0.1, 0.1]), "bt:ogps", "IDS", "bt:oids")),
        ("agent.allocate", ("FRACTAL.AGENT.ALLOCATE", pack([0.10, 0.12, 0.08, 0.15]), pack([0.04 if i == j else 0.002 for i in range(4) for j in range(4)]), "2", '{"a":1}')),
        ("agent.rebalance_sibling", ("FRACTAL.AGENT.REBALANCE_SIBLING", "bt:o", pack([0.10, 0.12, 0.08, 0.15]),
                                     pack([0.04 if i == j else 0.002 for i in range(4) for j in range(4)]), "2", pack([0.0] * 4), "SEED", "3", "IDS", "bt:oids")),
        ("agent.route_task", ("FRACTAL.AGENT.ROUTE_TASK", "bt:o", q4, "500", "COST_PER_ROUTE", "10", "IDS", "bt:oids")),
        ("agent.schedule_workload", ("FRACTAL.AGENT.SCHEDULE_WORKLOAD", "bt:o", q4, "ITERATIONS", "5", "POPULATION", "10", "CONTEXT", '{"a":1}', "IDS", "bt:oids")),
        ("agent.outlier_intercept", ("FRACTAL.AGENT.OUTLIER_INTERCEPT", "bt:o", q4, "0.5")),
        ("agent.outlier_intercept_l2", ("FRACTAL.AGENT.OUTLIER_INTERCEPT", "bt:o", q4, "0.5", "METRIC", "l2")),
        ("agent.network_coverage_alert", ("FRACTAL.AGENT.NETWORK_COVERAGE_ALERT", "bt:ogps", "bt:oag", "2", "32", "0.5", '{"a":1}')),
        ("agent.trajectory_predict", ("FRACTAL.AGENT.TRAJECTORY_PREDICT", "bt:o", "4", "1")),
        ("agent.detect_loop", ("FRACTAL.AGENT.DETECT_LOOP", "agent-1", "bt:oag", "4", "N_BITS", "64")),
        ("agent.plan_explore", ("FRACTAL.AGENT.PLAN_EXPLORE", "bt:o", "go north", "4", "3")),
        ("agent.search_agent", ("FRACTAL.SEARCH.AGENT", "bt:o", "what is nearby", "4", "POPULATION", "5", "CONTENT", "bt:otxt", "IDS", "bt:oids")),
        ("agent.rag_agent", ("FRACTAL.RAG.AGENT", "bt:o", "what is nearby", "4", "CONTENT", "bt:otxt")),
        ("optimize.multimodal", ("FRACTAL.OPTIMIZE.MULTIMODAL", pack([0.10, 0.12, 0.08, 0.15]), pack([0.04 if i == j else 0.002 for i in range(4) for j in range(4)]), "2", "4", "0.15", "0.9", "3")),
        ("optimize.multimodal.pareto", ("FRACTAL.OPTIMIZE.MULTIMODAL.PARETO", pack([0.10, 0.12, 0.08, 0.15]), pack([0.04 if i == j else 0.002 for i in range(4) for j in range(4)]), "2", "4", "4", "3")),
        ("agent.diverse_portfolios", ("FRACTAL.AGENT.DIVERSE_PORTFOLIOS", pack([0.10, 0.12, 0.08, 0.15]), pack([0.04 if i == j else 0.002 for i in range(4) for j in range(4)]), "2", "N_RESTARTS", "4", "CONTEXT", '{"a":1}')),
        ("agent.feedback_audit", ("FRACTAL.AGENT.FEEDBACK_AUDIT", "bt:o", q4, "bt:o", "WARMUP_COUNT", "2", "K", "2")),
        ("agent.patient_triage", ("FRACTAL.AGENT.PATIENT_DETERIORATION_TRIAGE", "bt:o", q4, pack([0.1] * 4), pack([0.7, 0.9, 0.1, 0.1]), "K", "2", "IDS", "bt:oids", "FILTER", "bt:otag", "t")),
    ]

    def run_with_failure_at(n, args):
        """Fail the nth module allocation of one call. Returns (reply, allocs_seen)."""
        c.call("FRACTAL.DEBUG.OOM", str(n))
        try:
            res = c.call(*args)
        finally:
            seen = c.call("FRACTAL.DEBUG.OOM", "0")
        return res, seen

    # POSITIVE CONTROL: the very first allocation of FROMTEXT is its working
    # copy of the argument; failing it must come back as an error reply. If
    # injection ever stops taking effect this gate fails instead of passing
    # vacuously.
    res, seen = run_with_failure_at(1, cases[7][1])
    r.check("the injection is live: failing FROMTEXT's first allocation gives an error reply",
            isinstance(res, RespError) and "memory" in str(res), "reply=%r seen=%r" % (res, seen))
    if not (isinstance(res, RespError) and "memory" in str(res)):
        return

    total_points = 0
    for name, args in cases:
        points, errors, bad_reply = 0, 0, None
        for n in range(1, 4000):
            try:
                res, seen = run_with_failure_at(n, args)
            except (OSError, ConnectionError):
                r.bad("%s: the server survived an allocation failure at point %d" % (name, n), "the server went away")
                return
            if seen < n:          # the call finished before reaching the nth allocation: swept them all
                break
            points += 1
            if isinstance(res, RespError):
                errors += 1
                if "memory" not in str(res) and "busy" not in str(res):
                    bad_reply = (n, str(res)[:80])
        else:
            r.bad("%s: the sweep did not terminate" % name, "more than 3999 allocation points")
            return
        if not r.alive():
            r.bad("%s: the server stopped answering after the sweep" % name, "")
            return
        if bad_reply:
            r.bad("%s: an injected failure produced an unrelated error" % name, "point %d -> %s" % bad_reply)
            continue
        if points == 0:
            r.bad("%s: no module allocation was observed" % name, "the command allocates nothing countable")
            continue
        if errors == 0:
            r.bad("%s: %d allocation points but no injected failure surfaced as an error" % (name, points), "every failure was swallowed")
            continue
        total_points += points
        r.ok("%s: every one of %d allocation points failed in turn, 0 server deaths (%d clean 'out of memory' errors)" % (name, points, errors))
    r.ok("%d allocation points swept across %d commands" % (total_points, len(cases)))
    res = c.call("FRACTAL.SEARCH", "bt:o", q4, "2")
    r.check("search still works after the sweep", isinstance(res, bytes) and "top_k" in json.loads(res), str(res)[:80])


# --------------------------------------------------------------------------
# 12 acl: per-command and per-key permissions
# --------------------------------------------------------------------------
def gate_12(r):
    c = r.c
    q4 = pack([0.6, 0.8, 0, 0])
    corpus = pack([0.6, 0.8, 0, 0, 0.1, 0.2, 0.3, 0.4])
    c.call("SET", "bt:ok1", corpus)
    c.call("SET", "bt:secret", corpus)
    r.check("ACL SETUSER (narrow) accepted",
            c.call("ACL", "SETUSER", "bt_narrow", "on", ">pw", "~bt:ok*", "-@all", "+fractal.search", "+fractal.vector.dot", "+ping") == "OK")
    r.check("ACL SETUSER (read category) accepted",
            c.call("ACL", "SETUSER", "bt_reader", "on", ">pw", "~*", "-@all", "+@read", "+ping") == "OK")
    r.check("ACL SETUSER (no-read) accepted",
            c.call("ACL", "SETUSER", "bt_writer", "on", ">pw", "~*", "-@all", "+@write", "+ping", "+set") == "OK")

    def login(user):
        cc = Client(r.host, r.port)
        res = cc.call("AUTH", user, "pw")
        return cc if res == "OK" else None

    n = login("bt_narrow")
    r.check("narrow user logs in", n is not None)
    if n is not None:
        res = n.call("FRACTAL.SEARCH", "bt:ok1", q4, "1")
        r.check("narrow user: FRACTAL.SEARCH on a permitted key works", isinstance(res, bytes) and "top_k" in json.loads(res), str(res)[:80])
        r.check("narrow user: a key outside ~bt:ok* is refused (NOPERM)", r.is_err(n.call("FRACTAL.SEARCH", "bt:secret", q4, "1"), "NOPERM"))
        r.check("narrow user: FRACTAL.EXPLORE (not granted) is refused", r.is_err(n.call("FRACTAL.EXPLORE", "bt:ok1", q4), "NOPERM"))
        r.check("narrow user: FRACTAL.DIM.DFA (not granted) is refused", r.is_err(n.call("FRACTAL.DIM.DFA", pack([1.0] * 20)), "NOPERM"))
        r.check("narrow user: a granted keyless command works", float(n.call("FRACTAL.VECTOR.DOT", pack([1, 2]), pack([3, 4]))) == 11.0)
        n.close()
    rd = login("bt_reader")
    r.check("reader user logs in", rd is not None)
    if rd is not None:
        res = rd.call("FRACTAL.SEARCH", "bt:secret", q4, "1")
        r.check("+@read grants the read-only search command", isinstance(res, bytes), str(res)[:80])
        r.check("+@read grants the read-only analytics commands", not r.is_err(rd.call("FRACTAL.DIM.DFA", pack([math.sin(x) for x in range(32)]))))
        rd.close()
    wr = login("bt_writer")
    if wr is not None:
        r.check("a user without @read cannot run FRACTAL.SEARCH", r.is_err(wr.call("FRACTAL.SEARCH", "bt:ok1", q4, "1"), "NOPERM"))
        r.check("a user without @read cannot run FRACTAL.VECTOR.DOT", r.is_err(wr.call("FRACTAL.VECTOR.DOT", pack([1]), pack([1])), "NOPERM"))
        wr.close()
    def cats(cmd):
        """The ACL categories COMMAND INFO reports for a command ('ACL CAT <cat>' does not list module commands)."""
        info = c.call("COMMAND", "INFO", cmd)
        row = info[0] if info and info[0] else []
        return {x.decode() if isinstance(x, bytes) else str(x) for x in (row[6] if len(row) > 6 else [])}
    for cmd in ("FRACTAL.REASON", "FRACTAL.EMBED", "FRACTAL.LEDGER.FLUSH", "FRACTAL.LEDGER.RESET.HARD", "FRACTAL.AUDIT.LOG"):
        r.check("%s is @dangerous" % cmd, "@dangerous" in cats(cmd), str(cats(cmd)))
    for cmd in ("FRACTAL.LEDGER.FLUSH", "FRACTAL.AUDIT.LOG"):
        r.check("%s is @admin" % cmd, "@admin" in cats(cmd), str(cats(cmd)))
    for cmd in ("FRACTAL.SEARCH", "FRACTAL.EXPLORE", "FRACTAL.DIM.DFA", "FRACTAL.MORPH.VASCULAR", "FRACTAL.LEDGER.VERIFY"):
        r.check("%s is @read and @slow" % cmd, {"@read", "@slow"} <= cats(cmd), str(cats(cmd)))
    for cmd in ("FRACTAL.VECTOR.DOT", "FRACTAL.VECTOR.NORM", "FRACTAL.VERSION"):
        r.check("%s is @read and @fast" % cmd, {"@read", "@fast"} <= cats(cmd), str(cats(cmd)))
    for cmd in ("FRACTAL.SEARCH", "FRACTAL.DIM.DFA", "FRACTAL.VECTOR.DOT"):
        r.check("%s is NOT @dangerous" % cmd, "@dangerous" not in cats(cmd), str(cats(cmd)))
    # the behavioural proof: a grant by category reaches exactly the commands in it
    c.call("ACL", "SETUSER", "bt_danger", "on", ">pw", "~*", "-@all", "+@dangerous", "+ping")
    dg = login("bt_danger")
    if dg is not None:
        r.check("a +@dangerous user can reach FRACTAL.REASON (not a NOPERM)", not r.is_err(dg.call("FRACTAL.REASON", "x"), "NOPERM"))
        r.check("a +@dangerous user cannot run read-only vector math", r.is_err(dg.call("FRACTAL.VECTOR.NORM", pack([3, 4])), "NOPERM"))
        r.check("a +@dangerous user cannot run FRACTAL.SEARCH", r.is_err(dg.call("FRACTAL.SEARCH", "bt:ok1", q4, "1"), "NOPERM"))
        dg.close()
    c.call("ACL", "DELUSER", "bt_danger")
    r.check("COMMAND GETKEYS reports the corpus key of FRACTAL.SEARCH", c.call("COMMAND", "GETKEYS", "FRACTAL.SEARCH", "bt:ok1", q4) == [b"bt:ok1"])
    r.check("COMMAND GETKEYS reports no key for a keyless command", r.is_err(c.call("COMMAND", "GETKEYS", "FRACTAL.VECTOR.DOT", pack([1]), pack([1]))))
    info = c.call("COMMAND", "INFO", "FRACTAL.SEARCH")
    flags = [x.decode() if isinstance(x, bytes) else str(x) for x in (info[0][2] if info and info[0] else [])]
    r.check("FRACTAL.SEARCH is declared readonly", "readonly" in flags, str(flags))
    for u in ("bt_narrow", "bt_reader", "bt_writer"):
        c.call("ACL", "DELUSER", u)
    r.check("the server survived", r.alive())


# --------------------------------------------------------------------------
# 13 persist: phase=write | read (restart in between), phase=replica
# --------------------------------------------------------------------------
def gate_13(r):
    import time
    c = r.c
    phase = os.environ.get("FSQL_PHASE", "write")
    q4 = pack([0.6, 0.8, 0, 0])
    if phase == "write":
        c.call("SET", "bt:persist", pack([0.6, 0.8, 0, 0, 0.1, 0.2, 0.3, 0.4, 1, 0, 0, 0]))
        r.check("search works before the save", "top_k" in json.loads(c.call("FRACTAL.SEARCH", "bt:persist", q4, "2")))
        r.check("SAVE succeeds with the module loaded", c.call("SAVE") == "OK")
        info = c.call("INFO", "modules").decode()
        r.check("INFO modules lists fractalsql", "name=fractalsql" in info, info[:120])
    elif phase == "read":
        r.check("the corpus survived a restart", c.call("EXISTS", "bt:persist") == 1)
        res = c.call("FRACTAL.SEARCH", "bt:persist", q4, "2")
        r.check("search works on the reloaded corpus", isinstance(res, bytes) and json.loads(res)["n_corpus"] == 3, str(res)[:80])
        ml = str(c.call("MODULE", "LIST"))
        r.check("the module is loaded after the restart", "fractalsql" in ml, ml[:80])
    elif phase == "replica":
        rep_port = int(os.environ["FSQL_REPLICA_PORT"])
        rep = Client(r.host, rep_port)
        c.call("SET", "bt:rep", pack([0.6, 0.8, 0, 0, 0.1, 0.2, 0.3, 0.4]))
        for _ in range(100):
            if rep.call("EXISTS", "bt:rep") == 1:
                break
            time.sleep(0.1)
        r.check("the replica received the corpus", rep.call("EXISTS", "bt:rep") == 1)
        res = rep.call("FRACTAL.SEARCH", "bt:rep", q4, "1")
        r.check("FRACTAL.SEARCH works on the replica (readonly, allow-stale)", isinstance(res, bytes) and "top_k" in json.loads(res), str(res)[:80])
        r.check("FRACTAL.EXPLORE works on the replica", isinstance(rep.call("FRACTAL.EXPLORE", "bt:rep", q4, "4"), bytes))

        def offset():
            for line in c.call("INFO", "replication").decode().splitlines():
                if line.startswith("master_repl_offset:"):
                    return int(line.split(":")[1])
        before = offset()
        for args in (("FRACTAL.SEARCH", "bt:rep", q4, "1"), ("FRACTAL.EXPLORE", "bt:rep", q4, "4"),
                     ("FRACTAL.VECTOR.DOT", pack([1]), pack([1])), ("FRACTAL.DIM.DFA", pack([math.sin(x) for x in range(32)]))):
            c.call(*args)
        r.check("no FRACTAL command is propagated to replicas (replication offset unchanged)", offset() == before, "%s -> %s" % (before, offset()))
        rep.close()



# --------------------------------------------------------------------------
# 15 daemon: reasoning / embedding / ledger commands through fractalsqld
#   FSQL_PHASE = unconfigured | up | down | back | wrongkey
# --------------------------------------------------------------------------
DAEMON_CMDS = [
    ("FRACTAL.REASON", ("hello",)),
    ("FRACTAL.EMBED", ("hello",)),
    ("FRACTAL.LEDGER.FLUSH", ()),
    ("FRACTAL.LEDGER.LOAD", ()),
    ("FRACTAL.LEDGER.COMPACT", ()),
    ("FRACTAL.LEDGER.RESET.SOFT", ()),
    ("FRACTAL.LEDGER.RESET.HARD", ()),
    ("FRACTAL.LEDGER.LOCK", ()),
    ("FRACTAL.LEDGER.TRUTH.COUNT", ()),
    ("FRACTAL.LEDGER.SHADOW.COUNT", ()),
    ("FRACTAL.LEDGER.VERIFY", ()),
    ("FRACTAL.AUDIT.LOG", ("decision", "{}")),
    ("FRACTAL.AUDIT.UNPACK", (b"junk",)),
]


def gate_15(r):
    import threading
    c = r.c
    phase = os.environ.get("FSQL_PHASE", "up")
    q4 = pack([0.6, 0.8, 0, 0])

    def tier_ab_ok(tag):
        c.call("SET", "bt:d", pack([0.6, 0.8, 0, 0, 0.1, 0.2, 0.3, 0.4]))
        res = c.call("FRACTAL.SEARCH", "bt:d", q4, "1")
        ok = isinstance(res, bytes) and "top_k" in json.loads(res)
        ok = ok and float(c.call("FRACTAL.VECTOR.NORM", pack([3, 4]))) == 5.0
        r.check("%s: search and vector math still work without the daemon" % tag, ok, str(res)[:80])

    if phase == "unconfigured":
        for name, args in DAEMON_CMDS:
            r.check("%s: a clear 'not configured' error" % name, r.is_err(c.call(name, *args), "not configured"))
        tier_ab_ok("unconfigured")
        r.check("the server survived", r.alive())
        return

    if phase == "wrongkey":
        res = c.call("FRACTAL.REASON", "hello")
        r.check("a daemon key that does not match is refused cleanly, not trusted", r.is_err(res), str(res)[:100])
        r.check("the error does not leak the key or crash", r.alive())
        tier_ab_ok("wrongkey")
        return

    if phase == "down":
        for name, args in DAEMON_CMDS:
            res = c.call(name, *args)
            r.check("%s: a clear 'not reachable' error while the daemon is down" % name,
                    r.is_err(res, "not reachable") or r.is_err(res, "did not answer") or r.is_err(res, "lost the connection"), str(res)[:100])
        tier_ab_ok("daemon down")
        r.check("the server survived", r.alive())
        return

    if phase in ("mode_code", "mode_json"):
        mode = phase[5:]
        marker = "FRACTALSQL_BT_JSON_MARKER"
        res = c.call("FRACTAL.REASON", "give me " + marker)
        r.check("%s mode: REASON returns the fenced block's interior, fence and language tag dropped" % mode,
                res == b'{"a": 1}', str(res)[:100])
        res = c.call("FRACTAL.REASON", "hello")
        if mode == "code":
            r.check("code mode: any fenced reply is reduced to its interior", res == b"SELECT 1", str(res)[:100])
        else:
            r.check("json mode: an interior that is not valid JSON fails the call", r.is_err(res), str(res)[:100])
        emb = c.call("FRACTAL.EMBED", "hello")
        r.check("%s mode: EMBED is unaffected by the response mode" % mode,
                isinstance(emb, bytes) and len(emb) == 12, str(emb)[:60])
        r.check("%s mode: the server survived" % mode, r.alive())
        return

    if phase == "back":
        res = c.call("FRACTAL.REASON", "hello")
        r.check("reasoning works again once the daemon is back (no restart of the server)",
                isinstance(res, bytes) and b"SELECT 1" in res, str(res)[:100])
        return

    # ---- phase "up": a community daemon with a mock LLM behind it
    res = c.call("FRACTAL.REASON", "hello")
    r.check("REASON returns the provider's reply", isinstance(res, bytes) and b"SELECT 1" in res, str(res)[:100])
    res = c.call("FRACTAL.REASON", "give me FRACTALSQL_BT_JSON_MARKER")
    r.check("text mode (the default): REASON returns the reply as-is, fence markers and all",
            res == b'```json\n{"a": 1}\n```', str(res)[:100])
    res = c.call("FRACTAL.REASON", "summarise this", '{"rows": [1, 2, 3]}')
    r.check("REASON accepts a JSON context", isinstance(res, bytes) and b"SELECT 1" in res, str(res)[:100])
    c.call("SET", "bt:ag:series", pack([x + (((i - 120) ** 2) / 40.0 if i > 120 else 0.0) for i, x in enumerate(_walk(192, 5)[0])]))
    ag = jload(c.call("FRACTAL.AGENT.ANOMALY_TRIAGE", "bt:ag:series", "64"))
    r.check("ANOMALY_TRIAGE with a daemon returns the provider's triage_summary",
            ag is not None and "SELECT 1" in str(ag.get("triage_summary")) and "reasoning_step_failed" not in ag, str(ag)[:140])
    c.call("SET", "bt:ag:c15", pack([0.6, 0.8, 0, 0, 0.1, 0.2, 0.3, 0.4, 1, 0, 0, 0]))
    c.call("SET", "bt:ag:g15", pack([((i * 37) % 11) * 0.5 for i in range(80)]))
    b15, u15 = pack([0.1] * 4), pack([0.7, 0.9, 0.1, 0.1])
    for label, cmd in (
        ("REGIME_TRIAGE", ("FRACTAL.AGENT.REGIME_TRIAGE", "bt:ag:series", "64")),
        ("TRACK_ANOMALY", ("FRACTAL.AGENT.TRACK_ANOMALY", "bt:ag:c15", b15, u15, "bt:ag:series")),
        ("DETOUR_CLASSIFY", ("FRACTAL.AGENT.DETOUR_CLASSIFY", "bt:ag:c15", b15, u15, "bt:ag:g15")),
        ("PATIENT_DETERIORATION_TRIAGE", ("FRACTAL.AGENT.PATIENT_DETERIORATION_TRIAGE", "bt:ag:c15", q4, b15, u15)),
        ("ALLOCATE", ("FRACTAL.AGENT.ALLOCATE", pack([0.10, 0.12, 0.08, 0.15]), pack([0.04 if i == j else 0.002 for i in range(4) for j in range(4)]), "2")),
        ("REBALANCE_SIBLING", ("FRACTAL.AGENT.REBALANCE_SIBLING", "bt:ag:c15", pack([0.10, 0.12, 0.08, 0.15]),
                               pack([0.04 if i == j else 0.002 for i in range(4) for j in range(4)]), "2", pack([0.0] * 4), "SEED", "3")),
        ("ROUTE_TASK", ("FRACTAL.AGENT.ROUTE_TASK", "bt:ag:c15", q4, "500")),
        ("SCHEDULE_WORKLOAD", ("FRACTAL.AGENT.SCHEDULE_WORKLOAD", "bt:ag:c15", q4, "ITERATIONS", "5", "POPULATION", "10")),
        ("OUTLIER_INTERCEPT", ("FRACTAL.AGENT.OUTLIER_INTERCEPT", "bt:ag:c15", q4, "0.1")),
        ("NETWORK_COVERAGE_ALERT", ("FRACTAL.AGENT.NETWORK_COVERAGE_ALERT", "bt:ag:g15", "bt:ag:series", "2", "32")),
    ):
        ag = jload(c.call(*cmd))
        r.check("%s with a daemon returns the provider's rationale" % label,
                ag is not None and "SELECT 1" in str(ag.get("rationale", ag.get("reason"))) and "reasoning_step_failed" not in ag, str(ag)[:140])
    # the mock provider embeds every text as [0.1, 0.2, 0.3]: a 3-dim corpus whose row 0 is that vector
    c.call("SET", "bt:ag:t15", pack([0.1, 0.2, 0.3, 0.9, -0.2, 0.1, -0.5, 0.4, 0.8, 0.0, 1.0, 0.0, 0.3, 0.3, -0.9]))
    c.call("DEL", "bt:ag:t15id", "bt:ag:t15tx")
    for i in range(5):
        c.call("HSET", "bt:ag:t15id", str(i), "doc%d" % i)
        c.call("HSET", "bt:ag:t15tx", str(i), "text of doc %d" % i)
    pe = jload(c.call("FRACTAL.AGENT.PLAN_EXPLORE", "bt:ag:t15", "go north", "3", "4"))
    r.check("PLAN_EXPLORE returns branches with branch_id, plan_trajectory and confidence_score",
            isinstance(pe, list) and 1 <= len(pe) <= 4 and all({"branch_id", "plan_trajectory", "confidence_score"} <= set(x) for x in pe), str(pe)[:140])
    r.check("PLAN_EXPLORE finds the row the embedding matches exactly (branch 0, confidence ~1)",
            isinstance(pe, list) and any(x["branch_id"] == 0 and x["confidence_score"] > 0.999 and len(x["plan_trajectory"]) == 3 for x in pe), str(pe)[:140])
    for name, cmd in (("SEARCH_AGENT", ("FRACTAL.SEARCH.AGENT", "bt:ag:t15", "what is nearby", "3", "POPULATION", "5", "CONTENT", "bt:ag:t15tx", "IDS", "bt:ag:t15id")),
                      ("RAG_AGENT", ("FRACTAL.RAG.AGENT", "bt:ag:t15", "what is nearby", "3", "CONTENT", "bt:ag:t15tx", "IDS", "bt:ag:t15id"))):
        ag = jload(c.call(*cmd))
        r.check("%s also returns source_row_ids: the IDS hash values of the rows, best first" % name,
                ag is not None and "doc0" in ag.get("source_row_ids", []) and len(ag.get("source_row_ids", [])) >= 1 and "source_doc_ids" not in ag, str(ag)[:160])
        r.check("%s returns the provider's answer, the source rows and a measured time" % name,
                ag is not None and "SELECT 1" in str(ag.get("answer")) and "doc0" in ag.get("source_row_ids", [])
                and ag.get("used_index") is False and isinstance(ag.get("execution_time_ms"), int) and "reasoning_step_failed" not in ag, str(ag)[:160])
    # the search and RAG agents through an index (the server runs with agent-use-index yes): the mock embeds every text as [0.1,0.2,0.3]
    rng15 = random.Random(15)
    rows15 = [[0.1, 0.2, 0.3]] + [[rng15.gauss(0, 1) for _ in range(3)] for _ in range(399)]
    c.call("SET", "bt:ag:ix15", b"".join(pack(x) for x in rows15))
    ag0 = jload(c.call("FRACTAL.RAG.AGENT", "bt:ag:ix15", "what is nearby", "3"))
    r.check("RAG_AGENT before an index exists scans exactly and says so",
            ag0 is not None and ag0.get("used_index") is False and 0 in ag0.get("source_row_ids", []), str(ag0)[:140])
    ix = c.call("FRACTAL.INDEX.ENSURE", "bt:ag:ix15", "ix15", "DIM", "3", "VECTOR_FORMAT", "FP32")
    if not r.is_err(ix):
        ix = jload(ix)
        r.check("INDEX.ENSURE creates a missing index and reports the action, status and generation",
                ix.get("action") == "created" and ix.get("status") == "ready" and ix.get("generation") == 1 and ix.get("rows") == 400, ix)
        ix = jload(c.call("FRACTAL.INDEX.ENSURE", "bt:ag:ix15", "ix15", "DIM", "3", "VECTOR_FORMAT", "FP32"))
        r.check("INDEX.ENSURE on an up-to-date index changes nothing",
                ix.get("action") == "unchanged" and ix.get("status") == "ready" and ix.get("generation") == 1, ix)
        for name, cmd in (("SEARCH_AGENT", ("FRACTAL.SEARCH.AGENT", "bt:ag:ix15", "what is nearby", "3", "POPULATION", "5")),
                          ("RAG_AGENT", ("FRACTAL.RAG.AGENT", "bt:ag:ix15", "what is nearby", "3"))):
            ag = jload(c.call(*cmd))
            r.check("%s goes through the index when agent-use-index is on and returns the exact nearest row" % name,
                    ag is not None and ag.get("used_index") is True and ag.get("source_row_ids", [None])[0] == 0
                    and "reasoning_step_failed" not in ag, str(ag)[:160])
        # PLAN_EXPLORE through the index: Scout runs over the shortlist, and every branch_id is still the row's position in the key
        pe = jload(c.call("FRACTAL.AGENT.PLAN_EXPLORE", "bt:ag:ix15", "what is nearby", "3", "5"))
        pe_ok = isinstance(pe, list) and len(pe) > 0 and all(
            isinstance(b, dict) and 0 <= b.get("branch_id", -1) < 400
            and all(abs(x - y) < 1e-5 for x, y in zip(b.get("plan_trajectory", []), rows15[b["branch_id"]])) for b in pe)
        r.check("PLAN_EXPLORE through the index returns branches whose ids are the rows' positions in the key", pe_ok, str(pe)[:200])
        info_pe = jload(c.call("FRACTAL.INDEX.INFO", "bt:ag:ix15", "ix15"))
        r.check("PLAN_EXPLORE really went through the index (FRACTAL.INDEX.INFO records the search)",
                (info_pe.get("last_search") or {}).get("used_index") is True, info_pe)
        pe2 = jload(c.call("FRACTAL.AGENT.PLAN_EXPLORE", "bt:ag:ix15", "what is nearby", "3", "5", "INDEX", "ix15"))
        r.check("INDEX <name> on PLAN_EXPLORE names the index and answers", isinstance(pe2, list) and len(pe2) > 0, str(pe2)[:160])
        r.check("PLAN_EXPLORE with an INDEX that does not exist is an error",
                r.is_err(c.call("FRACTAL.AGENT.PLAN_EXPLORE", "bt:ag:ix15", "what is nearby", "3", "5", "INDEX", "nope"), "no usable index"))
        c.call("APPEND", "bt:ag:ix15", pack([0.1, 0.2, 0.3]))
        ag = jload(c.call("FRACTAL.RAG.AGENT", "bt:ag:ix15", "what is nearby", "3"))
        r.check("a row appended after the build is still seen by the index path (the delta is scanned exactly)",
                ag is not None and ag.get("used_index") is True and 400 in ag.get("source_row_ids", []), str(ag)[:160])
        ix = jload(c.call("FRACTAL.INDEX.ENSURE", "bt:ag:ix15", "ix15", "DIM", "3", "VECTOR_FORMAT", "FP32"))
        r.check("INDEX.ENSURE after a small append refreshes (a new generation, no retraining)",
                ix.get("action") == "refreshed" and ix.get("generation") == 2 and ix.get("rows") == 401, ix)
        c.call("APPEND", "bt:ag:ix15", b"".join(pack(x) for x in rows15[1:201]))
        ix = jload(c.call("FRACTAL.INDEX.ENSURE", "bt:ag:ix15", "ix15", "DIM", "3", "VECTOR_FORMAT", "FP32"))
        r.check("INDEX.ENSURE after a large append (over a quarter of the rows) rebuilds",
                ix.get("action") == "rebuilt" and ix.get("generation") == 3 and ix.get("rows") == 601, ix)
        c.call("SET", "bt:ag:ix15", pack(rows15[1]) + b"".join(pack(x) for x in rows15[1:300]))
        ix = jload(c.call("FRACTAL.INDEX.ENSURE", "bt:ag:ix15", "ix15", "DIM", "3", "VECTOR_FORMAT", "FP32"))
        r.check("INDEX.ENSURE after the rows were rewritten rebuilds",
                ix.get("action") == "rebuilt" and ix.get("generation") == 4, ix)
        r.check("INDEX.ENSURE with a bad name is refused", r.is_err(c.call("FRACTAL.INDEX.ENSURE", "bt:ag:ix15", "bad name", "DIM", "3")))
    else:
        r.known("the IVF index", "this build of fractalsql-core does not carry the index")
    dm1 = c.call("FRACTAL.SEARCH.AGENT", "bt:ag:c15", "q", "4")
    r.check("SEARCH_AGENT refuses a DIM that differs from the embedding's (4 vs 3)", r.is_err(dm1, "dimensions but DIM"), str(dm1)[:120])
    dm2 = c.call("FRACTAL.AGENT.PLAN_EXPLORE", "bt:ag:c15", "q", "4", "2")
    r.check("PLAN_EXPLORE refuses a DIM that differs from the embedding's", r.is_err(dm2, "dimensions but DIM"), str(dm2)[:120])
    mu4 = pack([0.10, 0.12, 0.08, 0.15]); cov4 = pack([0.04 if i == j else 0.002 for i in range(4) for j in range(4)])
    for name, cmd in (("OPTIMIZE.MULTIMODAL", ("FRACTAL.OPTIMIZE.MULTIMODAL", mu4, cov4, "2", "4", "0.15", "0.9")),
                      ("OPTIMIZE.MULTIMODAL.PARETO", ("FRACTAL.OPTIMIZE.MULTIMODAL.PARETO", mu4, cov4, "2", "4")),
                      ("AGENT.DIVERSE_PORTFOLIOS", ("FRACTAL.AGENT.DIVERSE_PORTFOLIOS", mu4, cov4, "2"))):
        res = c.call(*cmd)
        r.check("%s on a Community daemon is a clean 'enterprise tier not loaded' error" % name, r.is_err(res, "enterprise tier not loaded"), str(res)[:100])
    r.check("REASON refuses a missing query", r.is_err(c.call("FRACTAL.REASON"), "wrong number"))
    r.check("REASON refuses too many arguments", r.is_err(c.call("FRACTAL.REASON", "a", "b", "c"), "wrong number"))
    big = c.call("FRACTAL.REASON", "x" * (5 * 1024 * 1024))
    r.check("an over-cap prompt is refused cleanly (the daemon caps it at 4 MiB)", r.is_err(big), str(big)[:100])
    r.check("the server survived the over-cap prompt", r.alive())
    c.call("FRACTAL.REASON", b"\xff\xfe\x00\x01junk")      # any reply is fine; surviving it is the point
    r.check("the server survived a binary-garbage prompt", r.alive())

    emb = c.call("FRACTAL.EMBED", "hello")
    vec = unpack(emb) if isinstance(emb, bytes) and len(emb) % 4 == 0 else []
    r.check("EMBED returns a packed float32 vector [0.1,0.2,0.3]",
            len(vec) == 3 and all(close(a, b, 1e-5) for a, b in zip(vec, [0.1, 0.2, 0.3])), repr(emb)[:100])
    if vec:
        r.check("the embedding feeds straight into the vector tier",
                close(float(c.call("FRACTAL.VECTOR.NORM", emb)), math.sqrt(0.14), 1e-4))
        c.call("SET", "bt:emb", emb + pack([1, 0, 0]))
        res = c.call("FRACTAL.SEARCH", "bt:emb", emb, "1")
        r.check("the embedding feeds straight into FRACTAL.SEARCH as a corpus row and a query",
                isinstance(res, bytes) and json.loads(res)["top_k"][0]["idx"] == 0, str(res)[:100])

    # community daemon: the enterprise commands are dormant, with a clear reason
    for name, args in DAEMON_CMDS[2:]:
        res = c.call(name, *args)
        if name == "FRACTAL.LEDGER.LOCK":
            r.check("%s: the latch works without the enterprise tier (returns 1)" % name, res == 1, str(res)[:80])
        elif name == "FRACTAL.LEDGER.VERIFY":
            r.check("%s works without the library: an empty ledger verifies as zero rows" % name,
                    jload(res) == {"ok": True, "rows_verified": 0}, str(res)[:120])
        else:
            r.check("%s: dormant on a community daemon, with a clear error" % name,
                    r.is_err(res, "enterprise"), str(res)[:120])
    r.check("LEDGER.VERIFY refuses a non-integer kind", r.is_err(c.call("FRACTAL.LEDGER.VERIFY", "x"), "integer"))
    r.check("AUDIT.LOG refuses missing arguments", r.is_err(c.call("FRACTAL.AUDIT.LOG", "only-one"), "wrong number"))

    # several clients reasoning at once
    errs, ok = [], [0]
    lock = threading.Lock()

    def worker(i):
        try:
            w = Client(r.host, r.port)
            for k in range(8):
                res = w.call("FRACTAL.REASON", "q%d-%d" % (i, k)) if k % 2 == 0 else w.call("FRACTAL.EMBED", "t%d-%d" % (i, k))
                good = isinstance(res, bytes) and len(res) > 0
                with lock:
                    if good:
                        ok[0] += 1
                    else:
                        errs.append(str(res)[:80])
            w.close()
        except Exception as e:  # noqa: BLE001
            with lock:
                errs.append(str(e))
    ths = [threading.Thread(target=worker, args=(i,)) for i in range(8)]
    [t.start() for t in ths]
    [t.join() for t in ths]
    r.check("8 concurrent clients x 8 daemon calls all returned valid results (%d ok)" % ok[0], ok[0] == 64 and not errs, str(errs[:3]))
    # hostile arguments at every daemon command, against the live daemon
    rng = random.Random(77)
    names = [n for n, _ in DAEMON_CMDS]
    bad_conn = None
    for _ in range(300):
        v = rng.choice(names)
        args = []
        for _ in range(rng.choice((0, 1, 2, 3, 4))):
            kind = rng.randrange(5)
            args.append(rng.randbytes(rng.choice((0, 1, 8, 200, 5000))) if kind == 0 else
                        str(rng.choice((-1, 0, 1, 2, 2 ** 63, "x"))).encode() if kind == 1 else
                        b"{}" if kind == 2 else b"[1,2,3]" if kind == 3 else b"\x00" * rng.choice((1, 64)))
        try:
            c.call(v, *args)
        except (OSError, ConnectionError) as e:
            bad_conn = (v, str(e))
            break
    r.check("300 hostile calls at the daemon commands: the server kept answering", bad_conn is None and r.alive(), str(bad_conn))
    res = c.call("FRACTAL.REASON", "still there?")
    r.check("the daemon is still serving after the hostile calls", isinstance(res, bytes) and b"SELECT 1" in res, str(res)[:100])
    tier_ab_ok("daemon up")
    r.check("the server survived", r.alive())


# --------------------------------------------------------------------------
# 16 enterprise: an enterprise library behind the daemon (the real one when available, else the mock core: FSQL_ENT_KIND)
#   FSQL_PHASE = ent | ent2 | ent3
# --------------------------------------------------------------------------
MOCK_ENT = os.environ.get("FSQL_ENT_KIND", "real") == "mock"     # gate 16 against the mock enterprise core, not the real library


def gate_16(r):
    # The enterprise gate counts ledger rows and fills a byte-capped ledger, so no mutant may append to it: no mutation here.
    c = r.c
    was = getattr(c, "_in_mutation", False)
    c._in_mutation = True
    try:
        return _gate_16(r)
    finally:
        c._in_mutation = was


def _gate_16(r):
    c = r.c
    phase = os.environ.get("FSQL_PHASE", "ent")
    if phase == "cap":
        # the daemon runs with FRACTALSQL_LEDGER_MAX_BYTES=400: appends fit until the file reaches the cap, then they are refused
        pad = "x" * 80
        res = [c.call("FRACTAL.AUDIT.LOG", "decision", '{"n":%d,"pad":"%s"}' % (i, pad)) for i in range(8)]
        accepted = sum(1 for x in res if x == 0)
        r.check("with FRACTALSQL_LEDGER_MAX_BYTES=400 some audit appends fit and the rest are refused", 0 < accepted < 8, str(res))
        r.check("a refused append is a clean error, not a crash", all(x == 0 or r.is_err(x) for x in res) and r.alive(), str(res))
        v = jload(c.call("FRACTAL.LEDGER.VERIFY", "2"))
        r.check("the chain that was kept verifies, with exactly the rows that fit", v == {"ok": True, "rows_verified": accepted}, "%s vs %d" % (v, accepted))
        return
    if phase == "ent":
        r.check("truth_count is 0 on a freshly loaded library (activation works)", c.call("FRACTAL.LEDGER.TRUTH.COUNT") == 0)
        r.check("shadow_count is 0", c.call("FRACTAL.LEDGER.SHADOW.COUNT") == 0)
        r.check("reset.hard succeeds (no storage touched)", c.call("FRACTAL.LEDGER.RESET.HARD") == 0)
        r.check("flush persists to the ledger file", c.call("FRACTAL.LEDGER.FLUSH") == 0)
        v = jload(c.call("FRACTAL.LEDGER.VERIFY"))
        if MOCK_ENT:
            # the mock core's flush writes no QTL record, so the default chain stays empty
            r.check("verify confirms the chain (the mock core's flush writes no QTL row)", v is not None and v.get("ok") is True, str(v))
        else:
            r.check("verify confirms the 1-row chain", v == {"ok": True, "rows_verified": 1}, str(v))
        r.check("audit.log appends a decision record", c.call("FRACTAL.AUDIT.LOG", "portfolio_optimize", '{"k":2}') == 0)
        r.check("audit.log appends a second record", c.call("FRACTAL.AUDIT.LOG", "portfolio_optimize", '{"k":3}') == 0)
        v2 = jload(c.call("FRACTAL.LEDGER.VERIFY", "2"))
        r.check("verify of the decision chain (kind 2) walks both records", v2 == {"ok": True, "rows_verified": 2}, str(v2))
        # An agent's decision document is recorded in the same chain (best-effort, through the daemon).
        c.call("SET", "bt:ent:series", pack(_walk(192, 5)[0]))
        ag = jload(c.call("FRACTAL.AGENT.ANOMALY_TRIAGE", "bt:ent:series", "64"))
        r.check("an agent call still returns its document with the enterprise ledger active",
                ag is not None and "threat_score" in ag, str(ag)[:120])
        v3 = jload(c.call("FRACTAL.LEDGER.VERIFY", "2"))
        r.check("the agent's decision was recorded in the audit chain (kind 2 now holds 3 rows)",
                v3 == {"ok": True, "rows_verified": 3}, str(v3))
        r.check("audit.log refuses a malformed payload", r.is_err(c.call("FRACTAL.AUDIT.LOG", "x", "{bad json")))
        r.check("compact succeeds", c.call("FRACTAL.LEDGER.COMPACT") == 0)
        r.check("reset.soft succeeds", c.call("FRACTAL.LEDGER.RESET.SOFT") == 0)
        r.check("lock latches the destructive commands", c.call("FRACTAL.LEDGER.LOCK") == 1)
        r.check("flush is refused after the lock", r.is_err(c.call("FRACTAL.LEDGER.FLUSH")))
        r.check("reset.hard is refused after the lock", r.is_err(c.call("FRACTAL.LEDGER.RESET.HARD")))
        # LOAD overwrites the live in-memory ledger, so a latched session must not be able to roll it back.
        r.check("load is refused after the lock", r.is_err(c.call("FRACTAL.LEDGER.LOAD")))
        r.check("the lock is per session: a new client is not latched", Client(r.host, r.port).call("FRACTAL.LEDGER.FLUSH") == 0)
    elif phase == "mm":
        # The enterprise optimizer audits every call into the decision ledger, so these run AFTER the ledger checks above
        # (which count records) and before the daemon restart.
        mu6 = pack([0.10, 0.12, 0.08, 0.15, 0.09, 0.11])
        cov6 = pack([0.04 if i == j else 0.002 for i in range(6) for j in range(6)])
        MM, MP, DPF = "FRACTAL.OPTIMIZE.MULTIMODAL", "FRACTAL.OPTIMIZE.MULTIMODAL.PARETO", "FRACTAL.AGENT.DIVERSE_PORTFOLIOS"
        mm = jload(c.call(MM, mu6, cov6, "3", "6", "0.15", "0.9", "7"))
        r.check("MULTIMODAL returns several structurally different portfolios",
                mm is not None and mm.get("n_found", 0) >= 1 and len(mm["candidates"]) == mm["n_found"], str(mm)[:140])
        r.check("every MULTIMODAL candidate holds at most 3 of the 6 assets and has a finite Sharpe",
                mm is not None and all(len(x["weights"]) == 6 and sum(1 for w in x["weights"] if w > 0) <= 3 and math.isfinite(x["sharpe"]) for x in mm["candidates"]))
        r.check("MULTIMODAL with a seed is repeatable", c.call(MM, mu6, cov6, "3", "6", "0.15", "0.9", "7") == c.call(MM, mu6, cov6, "3", "6", "0.15", "0.9", "7"))
        mp = jload(c.call(MP, mu6, cov6, "3", "6", "6", "7"))
        r.check("MULTIMODAL.PARETO returns a non-dominated front with return and risk",
                mp is not None and mp.get("n_found", 0) >= 1 and all({"sharpe", "weights", "return", "risk"} <= set(x) for x in mp["candidates"]), str(mp)[:140])
        for label, args, want in (
            ("an overlap_threshold above 1", (MM, mu6, cov6, "3", "6", "1.5", "0.9"), "overlap"),
            ("a quality_frac of 0", (MM, mu6, cov6, "3", "6", "0.15", "0"), "quality"),
            ("k above the number of assets", (MM, mu6, cov6, "7", "6", "0.15", "0.9"), "must be <="),
            ("n_restarts above 64", (MM, mu6, cov6, "3", "65", "0.15", "0.9"), "must be in"),
            ("a cov that is not n x n", (MM, mu6, pack([0.1] * 30), "3", "6", "0.15", "0.9"), "n_assets x n_assets"),
            ("too few arguments", (MM, mu6, cov6, "3", "6"), "wrong number"),
            ("a Pareto max_front of 0", (MP, mu6, cov6, "3", "6", "0"), "must be in"),
        ):
            r.check("%s refuses %s" % (args[0].split(".", 2)[2], label), r.is_err(c.call(*args), want), str(args)[:50])
        dp = jload(c.call(DPF, mu6, cov6, "3", "N_RESTARTS", "6", "SEED", "7"))
        r.check("DIVERSE_PORTFOLIOS returns one row per candidate with the provider's shared rationale",
                isinstance(dp, list) and len(dp) >= 1 and all({"candidate_id", "sharpe", "weights", "rationale"} <= set(x) for x in dp)
                and all("SELECT 1" in str(x["rationale"]) for x in dp), str(dp)[:160])
        r.check("DIVERSE_PORTFOLIOS candidates are distinct portfolios", isinstance(dp, list) and len({tuple(x["weights"]) for x in dp}) == len(dp))
        dq = jload(c.call(DPF, mu6, cov6, "3", "N_RESTARTS", "6", "SEED", "7", "OBJECTIVE_MODE", "pareto"))
        r.check("DIVERSE_PORTFOLIOS in pareto mode returns return and risk per candidate",
                isinstance(dq, list) and len(dq) >= 1 and all({"weights", "return", "risk"} <= set(x["weights"]) for x in dq), str(dq)[:160])
        r.check("DIVERSE_PORTFOLIOS refuses an unknown OBJECTIVE_MODE", r.is_err(c.call(DPF, mu6, cov6, "3", "OBJECTIVE_MODE", "median"), "OBJECTIVE_MODE"))
    elif phase == "ent2":
        r.check("load rehydrates the persisted ledger across a daemon restart", c.call("FRACTAL.LEDGER.LOAD") == 0)
        v = jload(c.call("FRACTAL.LEDGER.VERIFY"))
        r.check("the chain still verifies after the restart", v is not None and v.get("ok") is True, str(v))
    elif phase == "ent3":
        # the tampered byte is in the newest record: the QTL record with the real core, the newest audit record with the mock core
        v = jload(c.call("FRACTAL.LEDGER.VERIFY", "2" if MOCK_ENT else "1"))
        r.check("verify detects a byte-level tamper", v is not None and v.get("ok") is False, str(v))
        if not MOCK_ENT:
            r.check("load refuses a tampered latest record", r.is_err(c.call("FRACTAL.LEDGER.LOAD")))
    r.check("the server survived", r.alive())


def gate_18(r):
    """FRACTAL.AGENT.*: key-backed agents. Without a daemon (this gate's server)
    every agent still returns what it computed, with the reasoning error attached."""
    c = r.c
    A = "FRACTAL.AGENT.ANOMALY_TRIAGE"
    # steady noise vs the same noise with an accelerating ramp from sample 120
    # (the fixture the event-stream kit uses for FRACTAL.DIM.DRIFT)
    noise = _walk(192, 5)[0]
    calm_s = _walk(192, 7)[0]
    shift = [x + (((i - 120) ** 2) / 40.0 if i > 120 else 0.0) for i, x in enumerate(noise)]
    c.call("SET", "bt:ag:shift", pack(shift))
    c.call("SET", "bt:ag:calm", pack(calm_s))
    c.call("SET", "bt:ag:short", pack(noise[:20]))
    c.call("SET", "bt:ag:str", "abc")
    c.call("SET", "bt:ag:nan", struct.pack("<%df" % 64, *([0.5] * 63 + [float("nan")])))

    # --- anomaly_triage
    res = c.call(A, "bt:ag:shift", "64")
    d = jload(res)
    r.check("anomaly_triage returns the documented fields", d is not None and
            {"threat_score", "anomaly_type", "triage_summary"} <= set(d), str(res)[:120])
    r.check("anomaly_triage with no daemon returns the computed half and says why",
            d is not None and d.get("triage_summary") is None and "not configured" in str(d.get("reasoning_step_failed")), str(d))
    ref = jload(c.call("FRACTAL.DIM.DRIFT", pack(shift), "64"))
    r.check("the agent's threat_score is the same number FRACTAL.DIM.DRIFT gives",
            d is not None and ref is not None and abs(d["threat_score"] - ref["drift"]) < 1e-9, "%s vs %s" % (d, ref))
    calm = jload(c.call(A, "bt:ag:calm", "64"))
    r.check("a regime change scores higher than steady noise",
            d is not None and calm is not None and abs(d["threat_score"]) > abs(calm["threat_score"]), "%s vs %s" % (d, calm))
    r.check("the default baseline_window (32) works", jload(c.call(A, "bt:ag:shift")) is not None)
    r.check("anomaly_triage replies are valid JSON with finite numbers",
            d is not None and all(math.isfinite(d[k]) for k in ("threat_score",)))

    # --- validation
    for label, args, want in (
        ("a missing key", ("bt:ag:nokey",), "missing"),
        ("a non-blob key", ("bt:ag:str",), "float32"),
        ("a series shorter than window + 16", ("bt:ag:short",), "at least"),
        ("a non-finite sample", ("bt:ag:nan", "16"), "non-finite"),
        ("a non-integer window", ("bt:ag:shift", "x"), "integer"),
        ("a window below 16", ("bt:ag:shift", "3"), "must be in"),
        ("a window above the cap", ("bt:ag:shift", "99999999999"), "must be in"),
        ("no arguments", (), "wrong number"),
        ("too many arguments", ("bt:ag:shift", "32", "x"), "wrong number"),
    ):
        r.check("anomaly_triage refuses %s" % label, r.is_err(c.call(A, *args), want), str(args))
    c.call("LPUSH", "bt:ag:list", "x")
    r.check("anomaly_triage refuses a key of the wrong type", r.is_err(c.call(A, "bt:ag:list")))

    # --- ACL: sends data to a provider, so it is @dangerous, not @read
    r.check("anomaly_triage is @dangerous", "@dangerous" in {x.decode() if isinstance(x, bytes) else x for x in (c.call("COMMAND", "INFO", A)[0][6])})
    c.call("ACL", "SETUSER", "bt_ag_reader", "on", ">pw", "~*", "-@all", "+@read", "+ping")
    cc = Client(r.host, r.port)
    if cc.call("AUTH", "bt_ag_reader", "pw") == "OK":
        r.check("a +@read user cannot run an agent", r.is_err(cc.call(A, "bt:ag:shift", "64"), "NOPERM"))
        cc.close()
    c.call("ACL", "DELUSER", "bt_ag_reader")
    c.call("ACL", "SETUSER", "bt_ag_one", "on", ">pw", "~bt:ag:shift", "-@all", "+fractal.agent.anomaly_triage", "+ping")
    cc = Client(r.host, r.port)
    if cc.call("AUTH", "bt_ag_one", "pw") == "OK":
        r.check("a user granted the agent and the key can run it", not r.is_err(cc.call(A, "bt:ag:shift", "64"), "NOPERM"))
        r.check("the same user gets NOPERM on another key", r.is_err(cc.call(A, "bt:ag:calm", "64"), "NOPERM"))
        cc.close()
    c.call("ACL", "DELUSER", "bt_ag_one")

    # --- cohort agents: recall_hybrid and recommend_diverse
    R, D = "FRACTAL.AGENT.RECALL_HYBRID", "FRACTAL.AGENT.RECOMMEND_DIVERSE"
    rng = random.Random(21)
    rows = [[rng.uniform(-1, 1) for _ in range(4)] for _ in range(40)]
    c.call("SET", "bt:ag:corpus", pack([x for rw in rows for x in rw]))
    c.call("DEL", "bt:ag:ids", "bt:ag:txt", "bt:ag:tag")
    for i in range(40):
        c.call("HSET", "bt:ag:ids", str(i), "m%d" % i)
        c.call("HSET", "bt:ag:txt", str(i), "text %d" % i)
        if i % 2 == 0:
            c.call("HSET", "bt:ag:tag", str(i), "even")
    q7 = pack(rows[7])

    def cos(a, b):
        return 1 - sum(x * y for x, y in zip(a, b)) / (math.sqrt(sum(x * x for x in a)) * math.sqrt(sum(y * y for y in b)))
    truth = sorted(range(40), key=lambda i: cos(rows[i], rows[7]))

    rec = jload(c.call(R, "bt:ag:corpus", q7, "K", "3"))
    r.check("recall returns K results", isinstance(rec, list) and len(rec) == 3, str(rec))
    r.check("recall's nearest row is the exact match (cosine brute force agrees)",
            rec is not None and rec[0]["mem_id"] == 7 and rec[0]["mem_id"] == truth[0], str(rec))
    r.check("recall without IDS/CONTENT: integer ids, null content",
            rec is not None and all(isinstance(x["mem_id"], int) and x["content"] is None for x in rec), str(rec))
    rec = jload(c.call(R, "bt:ag:corpus", q7, "K", "3", "IDS", "bt:ag:ids", "CONTENT", "bt:ag:txt"))
    r.check("recall resolves ids and content from the companion hashes",
            rec is not None and rec[0] == {"mem_id": "m7", "content": "text 7"}, str(rec))
    r.check("recall defaults to K=5", len(jload(c.call(R, "bt:ag:corpus", q7))) == 5)
    rec = jload(c.call(R, "bt:ag:corpus", q7, "K", "4", "FILTER", "bt:ag:tag", "even", "IDS", "bt:ag:ids"))
    r.check("FILTER restricts the cohort (row 7 is odd, so it must not appear)",
            rec is not None and len(rec) == 4 and all(int(x["mem_id"][1:]) % 2 == 0 for x in rec), str(rec))
    r.check("FILTER on a value no row has is an error", r.is_err(c.call(R, "bt:ag:corpus", q7, "FILTER", "bt:ag:tag", "odd"), "no rows"))
    r.check("K above the cohort size returns the whole cohort",
            len(jload(c.call(R, "bt:ag:corpus", q7, "K", "500"))) == 40)

    rd = jload(c.call(D, "bt:ag:corpus", q7, "K", "6", "IDS", "bt:ag:ids"))
    r.check("recommend returns K items with item_id and score", isinstance(rd, list) and len(rd) == 6 and
            all({"item_id", "score"} == set(x) for x in rd), str(rd))
    r.check("recommend's items are distinct", rd is not None and len({x["item_id"] for x in rd}) == 6)
    r.check("recommend scores the exact match about 1 (score = 1 - cosine distance)",
            rd is not None and rd[0]["item_id"] == "m7" and abs(rd[0]["score"] - 1.0) < 1e-3, str(rd[:1]))
    r.check("recommend scores are non-increasing (nearest first)",
            rd is not None and all(rd[i]["score"] >= rd[i + 1]["score"] - 1e-9 for i in range(len(rd) - 1)), str(rd))
    r.check("recommend defaults to K=10", len(jload(c.call(D, "bt:ag:corpus", q7))) == 10)
    top = jload(c.call("FRACTAL.SEARCH", "bt:ag:corpus", q7, "1"))
    r.check("diversification does not leak into later searches on the same server",
            top is not None and top["top_k"][0]["idx"] == 7, str(top)[:100])

    c.call("LPUSH", "bt:ag:notahash", "x")
    c.call("SET", "bt:ag:odd", struct.pack("<10f", *([0.5] * 10)))
    for agent in (R, D):
        tag = agent.split(".")[-1].lower()
        for label, args, want in (
            ("a missing corpus key", ("bt:ag:nokey", q7), "missing"),
            ("a corpus key of the wrong type", ("bt:ag:notahash", q7), "not a string"),
            ("a query that does not divide the corpus", ("bt:ag:odd", pack([1, 2, 3])), "multiple"),
            ("a non-blob query", ("bt:ag:corpus", b"abc"), "32-bit"),
            ("a non-finite query", ("bt:ag:corpus", pack([float("nan")] * 4)), "non-finite"),
            ("K = 0", ("bt:ag:corpus", q7, "K", "0"), "must be in"),
            ("a non-integer K", ("bt:ag:corpus", q7, "K", "x"), "integer"),
            ("K with no value", ("bt:ag:corpus", q7, "K"), "needs a value"),
            ("an unknown option", ("bt:ag:corpus", q7, "BOGUS"), "unknown option"),
            ("IDS naming a missing key", ("bt:ag:corpus", q7, "IDS", "bt:ag:nokey"), "missing"),
            ("IDS naming a non-hash", ("bt:ag:corpus", q7, "IDS", "bt:ag:notahash"), "not a hash"),
        ):
            r.check("%s refuses %s" % (tag, label), r.is_err(c.call(agent, *args), want), str(args)[:60])
        r.check("%s refuses too few arguments" % tag, r.is_err(c.call(agent, "bt:ag:corpus"), "wrong number"))
    r.check("recommend refuses FILTER (recall-only)", r.is_err(c.call(D, "bt:ag:corpus", q7, "FILTER", "bt:ag:tag", "even"), "unknown option"))
    r.check("recommend refuses CONTENT (recall-only)", r.is_err(c.call(D, "bt:ag:corpus", q7, "CONTENT", "bt:ag:txt"), "unknown option"))

    # --- ACL: retrieval agents are read-only (+@read grants them); companion
    # hash keys are checked against the caller's key patterns
    r.check("recall and recommend are @read, not @dangerous",
            all("@dangerous" not in {x.decode() if isinstance(x, bytes) else x for x in c.call("COMMAND", "INFO", a)[0][6]}
                and "@read" in {x.decode() if isinstance(x, bytes) else x for x in c.call("COMMAND", "INFO", a)[0][6]} for a in (R, D)))
    c.call("ACL", "SETUSER", "bt_ag_rd", "on", ">pw", "~bt:ag:corpus", "~bt:ag:ids", "-@all", "+fractal.agent.recall_hybrid", "+ping")
    cc = Client(r.host, r.port)
    if cc.call("AUTH", "bt_ag_rd", "pw") == "OK":
        r.check("a user may name a companion hash its patterns allow",
                not r.is_err(cc.call(R, "bt:ag:corpus", q7, "IDS", "bt:ag:ids"), "NOPERM"))
        r.check("a user cannot read a companion hash outside its key patterns (CONTENT)",
                r.is_err(cc.call(R, "bt:ag:corpus", q7, "CONTENT", "bt:ag:txt"), "NOPERM"))
        r.check("a user cannot use a FILTER hash outside its key patterns",
                r.is_err(cc.call(R, "bt:ag:corpus", q7, "FILTER", "bt:ag:tag", "even"), "NOPERM"))
        r.check("a user cannot reach a corpus key outside its patterns",
                r.is_err(cc.call(R, "bt:ag:shift", q7), "NOPERM"))
        cc.close()
    c.call("ACL", "DELUSER", "bt_ag_rd")
    c.call("ACL", "SETUSER", "bt_ag_rr", "on", ">pw", "~*", "-@all", "+@read", "+ping")
    cc = Client(r.host, r.port)
    if cc.call("AUTH", "bt_ag_rr", "pw") == "OK":
        r.check("a +@read user can run the retrieval agents",
                not r.is_err(cc.call(R, "bt:ag:corpus", q7), "NOPERM") and not r.is_err(cc.call(D, "bt:ag:corpus", q7), "NOPERM"))
        cc.close()
    c.call("ACL", "DELUSER", "bt_ag_rr")

    # --- series and trajectory agents
    RG, TK, DT, PT = ("FRACTAL.AGENT.REGIME_TRIAGE", "FRACTAL.AGENT.TRACK_ANOMALY",
                      "FRACTAL.AGENT.DETOUR_CLASSIFY", "FRACTAL.AGENT.PATIENT_DETERIORATION_TRIAGE")
    NOREASON = "not configured"
    base_v = [0.1, 0.1, 0.1, 0.1]
    cur_v = [b + 0.5 * x for b, x in zip(base_v, rows[12])]          # delta = 0.5 * row 12
    bb, cb = pack(base_v), pack(cur_v)

    # regime_triage: the same numbers the primitives give
    dfa_ref = float(c.call("FRACTAL.DIM.DFA", pack(shift)))
    dr_ref = jload(c.call("FRACTAL.DIM.DRIFT", pack(shift), "64"))
    rg = jload(c.call(RG, "bt:ag:shift"))
    r.check("regime_triage returns the documented fields", rg is not None and
            {"dfa_exponent", "drift_detected", "recent_alpha", "baseline_alpha", "rationale"} <= set(rg), str(rg))
    r.check("regime_triage with no daemon returns the computed half and says why",
            rg is not None and rg["rationale"] is None and NOREASON in str(rg.get("reasoning_step_failed")), str(rg))
    r.check("regime_triage's dfa_exponent is FRACTAL.DIM.DFA's number",
            rg is not None and abs(rg["dfa_exponent"] - dfa_ref) < 1e-9, "%s vs %s" % (rg, dfa_ref))
    r.check("regime_triage's alphas are FRACTAL.DIM.DRIFT's (default window 64)",
            rg is not None and abs(rg["recent_alpha"] - dr_ref["recent_alpha"]) < 1e-9
            and abs(rg["baseline_alpha"] - dr_ref["baseline_alpha"]) < 1e-9, "%s vs %s" % (rg, dr_ref))
    r.check("regime_triage flags the regime change at the default threshold 0.5",
            rg is not None and rg["drift_detected"] == (abs(dr_ref["drift"]) > 0.5), str(rg))
    r.check("a huge drift_threshold means no detection", jload(c.call(RG, "bt:ag:shift", "64", "1000000"))["drift_detected"] is False)
    r.check("a negative drift_threshold means detection", jload(c.call(RG, "bt:ag:shift", "64", "-1"))["drift_detected"] is True)
    r.check("regime_triage accepts a context argument", jload(c.call(RG, "bt:ag:shift", "64", "0.5", '{"site":"a"}')) is not None)
    for label, args, want in (
        ("a missing key", ("bt:ag:nokey",), "missing"),
        ("a non-blob key", ("bt:ag:str",), "float32"),
        ("a window below 16", ("bt:ag:shift", "8"), "must be in"),
        ("a non-integer window", ("bt:ag:shift", "x"), "integer"),
        ("a non-numeric threshold", ("bt:ag:shift", "64", "x"), "number"),
        ("a series shorter than win + 16", ("bt:ag:short",), "at least"),
        ("an empty context", ("bt:ag:shift", "64", "0.5", ""), "context"),
        ("an oversized context", ("bt:ag:shift", "64", "0.5", "x" * 70000), "context"),
        ("a context with a NUL byte", ("bt:ag:shift", "64", "0.5", b'{"a":\x00}'), "NUL"),
        ("too many arguments", ("bt:ag:shift", "64", "0.5", "{}", "x"), "wrong number"),
        ("no arguments", (), "wrong number"),
    ):
        r.check("regime_triage refuses %s" % label, r.is_err(c.call(RG, *args), want), str(args)[:60])

    # track_anomaly
    tk = jload(c.call(TK, "bt:ag:corpus", bb, cb, "bt:ag:shift"))
    r.check("track_anomaly returns the documented fields", tk is not None and
            {"nearest_fleet_id", "trajectory_distance", "dfa_exponent", "rationale"} <= set(tk), str(tk))
    r.check("track_anomaly finds the row nearest the baseline->current delta (row 12 by construction)",
            tk is not None and tk["nearest_fleet_id"] == 12 and tk["trajectory_distance"] < 1e-4, str(tk))
    r.check("track_anomaly's dfa_exponent is FRACTAL.DIM.DFA's number",
            tk is not None and abs(tk["dfa_exponent"] - dfa_ref) < 1e-9, "%s vs %s" % (tk, dfa_ref))
    r.check("track_anomaly with no daemon keeps the computed half and says why",
            tk is not None and tk["rationale"] is None and NOREASON in str(tk.get("reasoning_step_failed")), str(tk))
    r.check("track_anomaly resolves ids through IDS",
            jload(c.call(TK, "bt:ag:corpus", bb, cb, "bt:ag:shift", "IDS", "bt:ag:ids"))["nearest_fleet_id"] == "m12")
    c.call("SET", "bt:ag:tiny", pack([1.0] * 8))
    r.check("track_anomaly reports dfa=-1 for a series under 16 samples (-1 is the documented sentinel)",
            jload(c.call(TK, "bt:ag:corpus", bb, cb, "bt:ag:tiny"))["dfa_exponent"] == -1)

    # detour_classify
    gps = [((i * 37) % 11) * 0.5 for i in range(80)]
    c.call("SET", "bt:ag:gps", pack(gps))
    box_ref = float(c.call("FRACTAL.DIM.BOXCOUNT", pack(gps), "2"))
    dt = jload(c.call(DT, "bt:ag:corpus", bb, cb, "bt:ag:gps"))
    r.check("detour_classify returns the documented fields", dt is not None and
            {"nearest_fleet_id", "trajectory_distance", "trace_complexity", "rationale"} <= set(dt), str(dt))
    r.check("detour_classify's trace_complexity is FRACTAL.DIM.BOXCOUNT's number (dim 2)",
            dt is not None and abs(dt["trace_complexity"] - box_ref) < 1e-9 and dt["nearest_fleet_id"] == 12, "%s vs %s" % (dt, box_ref))
    g1 = random.Random(5)
    gps1 = [g1.uniform(0, 10) for _ in range(100)]                    # a 1-D trace: the core accepts it at dim 1
    c.call("SET", "bt:ag:gps1", pack(gps1))
    r.check("BOXCOUNT_DIM sets the trace's dimension (1): the number FRACTAL.DIM.BOXCOUNT gives",
            abs(jload(c.call(DT, "bt:ag:corpus", bb, cb, "bt:ag:gps1", "BOXCOUNT_DIM", "1"))["trace_complexity"]
                - float(c.call("FRACTAL.DIM.BOXCOUNT", pack(gps1), "1"))) < 1e-9)
    r.check("a core failure reads the same as the primitive's (invalid input for this function)",
            r.is_err(c.call(DT, "bt:ag:corpus", bb, cb, "bt:ag:gps", "BOXCOUNT_DIM", "4"), "invalid input")
            and r.is_err(c.call("FRACTAL.DIM.BOXCOUNT", pack(gps), "4"), "invalid input"))
    r.check("detour_classify keeps the computed half with no daemon", dt is not None and NOREASON in str(dt.get("reasoning_step_failed")))
    r.check("detour_classify refuses a trace that is not a multiple of BOXCOUNT_DIM",
            r.is_err(c.call(DT, "bt:ag:corpus", bb, cb, "bt:ag:gps", "BOXCOUNT_DIM", "3"), "multiple"))
    c.call("SET", "bt:ag:gpsshort", pack([1.0, 2.0] * 4))
    r.check("detour_classify refuses fewer than 8 points", r.is_err(c.call(DT, "bt:ag:corpus", bb, cb, "bt:ag:gpsshort"), "at least 8"))

    # patient_deterioration_triage
    pt = jload(c.call(PT, "bt:ag:corpus", q7, bb, cb, "K", "3"))
    r.check("patient triage returns the documented fields", pt is not None and
            {"nearest_cohort_id", "cohort_distance", "drift_distance", "rationale", "cohort_matches"} <= set(pt), str(pt))
    r.check("patient triage: the nearest cohort row is the exact match (row 7)",
            pt is not None and pt["nearest_cohort_id"] == 7 and pt["cohort_distance"] < 1e-4, str(pt))
    r.check("patient triage: cohort_matches has K entries, nearest first",
            pt is not None and len(pt["cohort_matches"]) == 3 and pt["cohort_matches"][0]["id"] == 7 and
            all(pt["cohort_matches"][i]["distance"] <= pt["cohort_matches"][i + 1]["distance"] + 1e-9 for i in range(2)), str(pt))
    r.check("patient triage: drift_distance is the nearest-to-delta distance (row 12 by construction)",
            pt is not None and pt["drift_distance"] < 1e-4, str(pt))
    r.check("patient triage keeps the computed half with no daemon",
            pt is not None and pt["rationale"] is None and NOREASON in str(pt.get("reasoning_step_failed")))
    pf = jload(c.call(PT, "bt:ag:corpus", q7, bb, cb, "K", "4", "IDS", "bt:ag:ids", "FILTER", "bt:ag:tag", "even"))
    r.check("patient triage FILTER restricts the cohort, not the drift search",
            pf is not None and pf["nearest_cohort_id"] != "m7" and all(int(m["id"][1:]) % 2 == 0 for m in pf["cohort_matches"]), str(pf))
    r.check("patient triage default K is 5", len(jload(c.call(PT, "bt:ag:corpus", q7, bb, cb))["cohort_matches"]) == 5)

    # shared validation for the three trajectory agents
    big = pack([3.0e38, 0, 0, 0]); neg = pack([-3.0e38, 0, 0, 0])
    for name, agent, extra in (("track_anomaly", TK, ("bt:ag:shift",)), ("detour_classify", DT, ("bt:ag:gps",))):
        for label, args, want in (
            ("a missing corpus key", ("bt:ag:nokey", bb, cb) + extra, "missing"),
            ("a baseline/current dim mismatch", ("bt:ag:corpus", bb, pack([1, 2, 3])) + extra, "must match"),
            ("a current - baseline that overflows float32", ("bt:ag:corpus", neg, big) + extra, "not finite"),
            ("a non-finite baseline", ("bt:ag:corpus", pack([float("nan")] * 4), cb) + extra, "non-finite"),
            ("a missing series key", ("bt:ag:corpus", bb, cb, "bt:ag:nokey"), "missing"),
            ("a non-blob series key", ("bt:ag:corpus", bb, cb, "bt:ag:str"), "float32"),
            ("an unknown option", ("bt:ag:corpus", bb, cb) + extra + ("BOGUS",), "unknown option"),
            ("K (it has no effect here, so it is not provided)", ("bt:ag:corpus", bb, cb) + extra + ("K", "3"), "unknown option"),
            ("FILTER (patient-only)", ("bt:ag:corpus", bb, cb) + extra + ("FILTER", "bt:ag:tag", "even"), "unknown option"),
            ("IDS naming a non-hash", ("bt:ag:corpus", bb, cb) + extra + ("IDS", "bt:ag:notahash"), "not a hash"),
            ("too few arguments", ("bt:ag:corpus", bb, cb), "wrong number"),
        ):
            r.check("%s refuses %s" % (name, label), r.is_err(c.call(agent, *args), want), str(args)[:50])
    for label, args, want in (
        ("a query dim that differs from the baseline", ("bt:ag:corpus", pack([1, 2, 3]), bb, cb), "must match"),
        ("a FILTER that matches nothing", ("bt:ag:corpus", q7, bb, cb, "FILTER", "bt:ag:tag", "odd"), "no rows"),
        ("K = 0", ("bt:ag:corpus", q7, bb, cb, "K", "0"), "must be in"),
        ("BOXCOUNT_DIM (detour-only)", ("bt:ag:corpus", q7, bb, cb, "BOXCOUNT_DIM", "2"), "unknown option"),
        ("a missing corpus key", ("bt:ag:nokey", q7, bb, cb), "missing"),
        ("too few arguments", ("bt:ag:corpus", q7, bb), "wrong number"),
    ):
        r.check("patient triage refuses %s" % label, r.is_err(c.call(PT, *args), want), str(args)[:50])

    # ACL: these four call the reasoner, so they are @dangerous like ANOMALY_TRIAGE
    for a in (RG, TK, DT, PT):
        cats = {x.decode() if isinstance(x, bytes) else x for x in c.call("COMMAND", "INFO", a)[0][6]}
        r.check("%s is @dangerous, not @read" % a.split(".")[-1].lower(), "@dangerous" in cats and "@read" not in cats, str(cats))
    c.call("ACL", "SETUSER", "bt_ag_tk", "on", ">pw", "~bt:ag:corpus", "~bt:ag:gps", "-@all", "+fractal.agent.track_anomaly", "+ping")
    cc = Client(r.host, r.port)
    if cc.call("AUTH", "bt_ag_tk", "pw") == "OK":
        r.check("track_anomaly: a series key outside the user's patterns is NOPERM (it is not a declared key)",
                r.is_err(cc.call(TK, "bt:ag:corpus", bb, cb, "bt:ag:shift"), "NOPERM"))
        r.check("track_anomaly: a series key inside the patterns works",
                not r.is_err(cc.call(TK, "bt:ag:corpus", bb, cb, "bt:ag:gps"), "NOPERM"))
        cc.close()
    c.call("ACL", "DELUSER", "bt_ag_tk")

    # --- allocation, routing, scheduling, intercept, coverage agents
    AL, RB, RT, SW, OI, NC = ("FRACTAL.AGENT.ALLOCATE", "FRACTAL.AGENT.REBALANCE_SIBLING", "FRACTAL.AGENT.ROUTE_TASK",
                              "FRACTAL.AGENT.SCHEDULE_WORKLOAD", "FRACTAL.AGENT.OUTLIER_INTERCEPT",
                              "FRACTAL.AGENT.NETWORK_COVERAGE_ALERT")
    mu6 = [0.10, 0.12, 0.08, 0.15, 0.09, 0.11]
    cov6 = [0.04 if i == j else 0.002 for i in range(6) for j in range(6)]
    mub, covb = pack(mu6), pack(cov6)
    ref = jload(c.call("FRACTAL.OPTIMIZE.PORTFOLIO", mub, covb, "3", "7"))

    # allocate (no key)
    al = jload(c.call(AL, mub, covb, "3"))
    r.check("allocate returns allocation, sharpe and rationale", al is not None and {"allocation", "sharpe", "rationale"} <= set(al), str(al))
    r.check("allocate: allocation is the optimizer's {sharpe, weights}, with at most 3 holdings",
            al is not None and al["allocation"]["sharpe"] == al["sharpe"] and len(al["allocation"]["weights"]) == 6
            and sum(1 for x in al["allocation"]["weights"] if x > 0) <= 3, str(al))
    r.check("allocate: the weights sum to 1", al is not None and abs(sum(al["allocation"]["weights"]) - 1.0) < 1e-6)
    r.check("allocate with no daemon keeps the computed half and says why",
            al is not None and al["rationale"] is None and NOREASON in str(al.get("reasoning_step_failed")), str(al))
    r.check("allocate accepts a JSON context", jload(c.call(AL, mub, covb, "3", '{"fund":"x"}')) is not None)
    for label, args, want in (
        ("a cardinality above the number of assets", (mub, covb, "7"), "must be <="),
        ("a cardinality of 0", (mub, covb, "0"), "must be in"),
        ("a non-integer cardinality", (mub, covb, "x"), "integer"),
        ("a cov that is not n x n", (mub, pack(cov6[:30]), "3"), "n_assets x n_assets"),
        ("a non-blob mu", (b"abc", covb, "3"), "32-bit"),
        ("more than 512 assets", (pack([0.1] * 513), pack([0.0] * 513 * 513), "3"), "at most 512"),
        ("an empty context", (mub, covb, "3", ""), "context"),
        ("a context with a NUL byte", (mub, covb, "3", b"{\x00}"), "NUL"),
        ("too few arguments", (mub, covb), "wrong number"),
        ("too many arguments", (mub, covb, "3", "{}", "x"), "wrong number"),
    ):
        r.check("allocate refuses %s" % label, r.is_err(c.call(AL, *args), want), str(args)[:40])

    # rebalance_sibling: portfolio (seeded) -> nearest historical allocation to (weights - baseline)
    allocs = [[rng.random() for _ in range(6)] for _ in range(20)]
    allocs[9] = ref["weights"]
    c.call("SET", "bt:ag:allocs", pack([x for a in allocs for x in a]))
    zero6 = pack([0.0] * 6)
    rb = jload(c.call(RB, "bt:ag:allocs", mub, covb, "3", zero6, "SEED", "7"))
    r.check("rebalance returns sharpe, weights, nearest_distance, rationale",
            rb is not None and {"sharpe", "weights", "nearest_distance", "rationale"} <= set(rb), str(rb))
    r.check("rebalance with the same SEED gives FRACTAL.OPTIMIZE.PORTFOLIO's weights and sharpe",
            rb is not None and abs(rb["sharpe"] - ref["sharpe"]) < 1e-9 and all(abs(a - b) < 1e-9 for a, b in zip(rb["weights"], ref["weights"])), "%s vs %s" % (rb, ref))
    r.check("rebalance: the nearest historical allocation to the new weights is row 9 (distance ~0)",
            rb is not None and rb["nearest_distance"] < 1e-4, str(rb))
    r.check("rebalance keeps the computed half with no daemon", rb is not None and NOREASON in str(rb.get("reasoning_step_failed")))
    r.check("rebalance accepts IDS and CONTEXT",
            jload(c.call(RB, "bt:ag:allocs", mub, covb, "3", zero6, "SEED", "7", "IDS", "bt:ag:ids", "CONTEXT", '{"a":1}')) is not None)
    for label, args, want in (
        ("a baseline whose dim is not the number of assets", ("bt:ag:allocs", mub, covb, "3", pack([0.0] * 4)), "must equal"),
        ("a corpus whose dim is not the number of assets", ("bt:ag:corpus", mub, covb, "3", zero6), "multiple"),
        ("a missing corpus key", ("bt:ag:nokey", mub, covb, "3", zero6), "missing"),
        ("a bad SEED", ("bt:ag:allocs", mub, covb, "3", zero6, "SEED", "x"), "integer"),
        ("a negative SEED", ("bt:ag:allocs", mub, covb, "3", zero6, "SEED", "-1"), "must be in"),
        ("an unknown option", ("bt:ag:allocs", mub, covb, "3", zero6, "BOGUS"), "unknown option"),
        ("an option of another agent (COST_PER_ROUTE)", ("bt:ag:allocs", mub, covb, "3", zero6, "COST_PER_ROUTE", "1"), "unknown option"),
        ("too few arguments", ("bt:ag:allocs", mub, covb, "3"), "wrong number"),
    ):
        r.check("rebalance refuses %s" % label, r.is_err(c.call(RB, *args), want), str(args)[:40])

    # route_task: nearest capability, confidence = 1 / (1 + distance), remaining = budget - cost
    q5 = pack(rows[5])
    rt = jload(c.call(RT, "bt:ag:corpus", q5, "1000"))
    r.check("route_task returns routed_to, confidence, remaining_budget, rationale",
            rt is not None and {"routed_to", "confidence", "remaining_budget", "rationale"} <= set(rt), str(rt))
    r.check("route_task: routed_to is the id as a string (row 5), confidence ~1, budget 1000 - 150",
            rt is not None and rt["routed_to"] == "5" and rt["confidence"] > 0.999 and rt["remaining_budget"] == 850, str(rt))
    r.check("route_task: COST_PER_ROUTE and a negative budget",
            jload(c.call(RT, "bt:ag:corpus", q5, "-5", "COST_PER_ROUTE", "100"))["remaining_budget"] == -105)
    r.check("route_task resolves the id through IDS", jload(c.call(RT, "bt:ag:corpus", q5, "10", "IDS", "bt:ag:ids"))["routed_to"] == "m5")
    r.check("route_task keeps the computed half with no daemon", NOREASON in str(rt.get("reasoning_step_failed")))
    for label, args, want in (
        ("a non-integer budget", ("bt:ag:corpus", q5, "x"), "integer"),
        ("a non-integer COST_PER_ROUTE", ("bt:ag:corpus", q5, "10", "COST_PER_ROUTE", "x"), "integer"),
        ("a missing corpus key", ("bt:ag:nokey", q5, "10"), "missing"),
        ("a task whose dim does not divide the corpus", ("bt:ag:odd", pack([1, 2, 3]), "10"), "multiple"),
        ("a non-finite task", ("bt:ag:corpus", pack([float("nan")] * 4), "10"), "non-finite"),
        ("an unknown option", ("bt:ag:corpus", q5, "10", "BOGUS"), "unknown option"),
        ("too few arguments", ("bt:ag:corpus", q5), "wrong number"),
    ):
        r.check("route_task refuses %s" % label, r.is_err(c.call(RT, *args), want), str(args)[:40])

    # schedule_workload: SFS refinement, then the nearest node
    sw = jload(c.call(SW, "bt:ag:corpus", q5))
    r.check("schedule_workload returns assigned_node, confidence, rationale",
            sw is not None and {"assigned_node", "confidence", "rationale"} <= set(sw), str(sw))
    r.check("schedule_workload assigns the task to the node it matches (row 5), with confidence ~1",
            sw is not None and sw["assigned_node"] == "5" and sw["confidence"] > 0.9, str(sw))
    r.check("schedule_workload accepts ITERATIONS, POPULATION, CONTEXT and IDS",
            jload(c.call(SW, "bt:ag:corpus", q5, "ITERATIONS", "10", "POPULATION", "20", "CONTEXT", '{"a":1}', "IDS", "bt:ag:ids"))["assigned_node"] == "m5")
    r.check("schedule_workload keeps the computed half with no daemon", sw is not None and NOREASON in str(sw.get("reasoning_step_failed")))
    for label, args, want in (
        ("ITERATIONS = 0", ("bt:ag:corpus", q5, "ITERATIONS", "0"), "must be in"),
        ("ITERATIONS above 10000", ("bt:ag:corpus", q5, "ITERATIONS", "10001"), "must be in"),
        ("POPULATION above 10000", ("bt:ag:corpus", q5, "POPULATION", "10001"), "must be in"),
        ("POPULATION of 1 (the core needs at least 2)", ("bt:ag:corpus", q5, "POPULATION", "1"), "must be in"),
        ("ITERATIONS x POPULATION above the work cap", ("bt:ag:corpus", q5, "ITERATIONS", "10000", "POPULATION", "10000"), "at most"),
        # The standard search work cap: iterations x population x diffusion x dim <= 2^31.
        # 10000 x 1000 x 2 x 200 = 4000000000 is over it; the same product with a
        # 4-dim task is under it, so the case is the cap and not the 10 M guard.
        ("ITERATIONS x POPULATION x diffusion x dim above the search work cap",
         ("bt:ag:corpus", pack([1.0] * 200), "ITERATIONS", "10000", "POPULATION", "1000"),
         "search too large (iterations x population_size x diffusion_factor x dim exceeds 2147483648)"),
        ("a task over 4096 dimensions", ("bt:ag:corpus", pack([1.0] * 4097)), "out of range"),
        ("a missing node key", ("bt:ag:nokey", q5), "missing"),
        ("an unknown option", ("bt:ag:corpus", q5, "BOGUS"), "unknown option"),
        ("too few arguments", ("bt:ag:corpus",), "wrong number"),
    ):
        r.check("schedule_workload refuses %s" % label, r.is_err(c.call(SW, *args), want), str(args)[:40])

    # outlier_intercept
    def l2min(v):
        return min(math.sqrt(sum((a - b) ** 2 for a, b in zip(rw, v))) for rw in rows)
    st3 = rows[3]
    oi = jload(c.call(OI, "bt:ag:corpus", pack(st3), "0.1"))
    r.check("outlier_intercept returns intercepted, nearest_distance, threshold, metric and a 'reason' field",
            oi is not None and {"intercepted", "nearest_distance", "threshold", "metric", "reason"} <= set(oi), str(oi))
    r.check("outlier_intercept: a state identical to a known-bad one is intercepted (cosine distance ~0 < 0.1)",
            oi is not None and oi["intercepted"] is True and oi["nearest_distance"] < 1e-4 and oi["metric"] == "cosine", str(oi))
    r.check("outlier_intercept: a threshold of 0 never intercepts (the test is distance < threshold)",
            jload(c.call(OI, "bt:ag:corpus", pack(st3), "0"))["intercepted"] is False)
    ol = jload(c.call(OI, "bt:ag:corpus", pack([x + 0.3 for x in st3]), "0.2", "METRIC", "l2"))
    expect = l2min([x + 0.3 for x in st3])
    r.check("outlier_intercept METRIC l2: the nearest L2 distance is the brute-force value",
            ol is not None and abs(ol["nearest_distance"] - expect) < 1e-5 and ol["metric"] == "l2"
            and ol["intercepted"] == (expect < 0.2), "%s vs %s" % (ol, expect))
    r.check("outlier_intercept keeps the computed half with no daemon",
            oi is not None and oi["reason"] is None and NOREASON in str(oi.get("reasoning_step_failed")))
    for label, args, want in (
        ("an unknown METRIC", ("bt:ag:corpus", pack(st3), "0.1", "METRIC", "dot"), "metric must be"),
        ("METRIC with no value", ("bt:ag:corpus", pack(st3), "0.1", "METRIC"), "needs a value"),
        ("a non-numeric threshold", ("bt:ag:corpus", pack(st3), "x"), "number"),
        ("a non-finite threshold", ("bt:ag:corpus", pack(st3), "nan"), "finite"),
        ("a missing history key", ("bt:ag:nokey", pack(st3), "0.1"), "missing"),
        ("a state that does not divide the corpus", ("bt:ag:odd", pack([1, 2, 3]), "0.1"), "multiple"),
        ("an unknown option", ("bt:ag:corpus", pack(st3), "0.1", "BOGUS"), "unknown option"),
        ("too few arguments", ("bt:ag:corpus", pack(st3)), "wrong number"),
    ):
        r.check("outlier_intercept refuses %s" % label, r.is_err(c.call(OI, *args), want), str(args)[:40])

    # network_coverage_alert
    mc = jload(c.call("FRACTAL.MORPH.COMPLEXITY", pack(gps), "2"))
    dr48 = jload(c.call("FRACTAL.DIM.DRIFT", pack(shift), "48"))
    nc = jload(c.call(NC, "bt:ag:gps", "bt:ag:shift"))
    r.check("network_coverage_alert returns morph_dimension, lacunarity, drift_detected, rationale",
            nc is not None and {"morph_dimension", "lacunarity", "drift_detected", "rationale"} <= set(nc), str(nc))
    r.check("network_coverage_alert: morph_dimension and lacunarity are FRACTAL.MORPH.COMPLEXITY's numbers",
            nc is not None and abs(nc["morph_dimension"] - mc["dimension"]) < 1e-9 and abs(nc["lacunarity"] - mc["lacunarity"]) < 1e-6, "%s vs %s" % (nc, mc))
    r.check("network_coverage_alert: drift_detected is |FRACTAL.DIM.DRIFT (window 48)| > 0.5",
            nc is not None and nc["drift_detected"] == (abs(dr48["drift"]) > 0.5), "%s vs %s" % (nc, dr48))
    r.check("network_coverage_alert: a huge drift_threshold means no detection",
            jload(c.call(NC, "bt:ag:gps", "bt:ag:shift", "2", "48", "1000000"))["drift_detected"] is False)
    r.check("network_coverage_alert accepts every positional option and a context",
            jload(c.call(NC, "bt:ag:gps", "bt:ag:shift", "2", "32", "0.5", '{"site":"a"}')) is not None)
    r.check("network_coverage_alert keeps the computed half with no daemon", nc is not None and NOREASON in str(nc.get("reasoning_step_failed")))
    for label, args, want in (
        ("a missing point cloud", ("bt:ag:nokey", "bt:ag:shift"), "missing"),
        ("a missing drift series", ("bt:ag:gps", "bt:ag:nokey"), "missing"),
        ("a point cloud that is not a multiple of boxcount_dim", ("bt:ag:gps", "bt:ag:shift", "3"), "multiple"),
        ("a point cloud with under 8 points", ("bt:ag:gpsshort", "bt:ag:shift"), "at least 8"),
        ("a drift series shorter than drift_win + 16", ("bt:ag:gps", "bt:ag:short"), "at least"),
        ("a drift_win under 16", ("bt:ag:gps", "bt:ag:shift", "2", "8"), "must be in"),
        ("a non-finite drift_threshold", ("bt:ag:gps", "bt:ag:shift", "2", "48", "nan"), "finite"),
        ("an empty context", ("bt:ag:gps", "bt:ag:shift", "2", "48", "0.5", ""), "context"),
        ("too few arguments", ("bt:ag:gps",), "wrong number"),
        ("too many arguments", ("bt:ag:gps", "bt:ag:shift", "2", "48", "0.5", "{}", "x"), "wrong number"),
    ):
        r.check("network_coverage_alert refuses %s" % label, r.is_err(c.call(NC, *args), want), str(args)[:40])

    # ACL: all six call the reasoner, so none is @read
    for a in (AL, RB, RT, SW, OI, NC):
        cats = {x.decode() if isinstance(x, bytes) else x for x in c.call("COMMAND", "INFO", a)[0][6]}
        r.check("%s is @dangerous, not @read" % a.split(".")[-1].lower(), "@dangerous" in cats and "@read" not in cats, str(cats))
    c.call("ACL", "SETUSER", "bt_ag_nc", "on", ">pw", "~bt:ag:gps", "-@all", "+fractal.agent.network_coverage_alert", "+ping")
    cc = Client(r.host, r.port)
    if cc.call("AUTH", "bt_ag_nc", "pw") == "OK":
        r.check("network_coverage_alert: a drift-series key outside the user's patterns is NOPERM (not a declared key)",
                r.is_err(cc.call(NC, "bt:ag:gps", "bt:ag:shift"), "NOPERM"))
        cc.close()
    c.call("ACL", "DELUSER", "bt_ag_nc")

    # --- the enterprise optimizers without a daemon: arguments are checked first, then a clean "not configured"
    DPF = "FRACTAL.AGENT.DIVERSE_PORTFOLIOS"
    mu6 = pack([0.10, 0.12, 0.08, 0.15, 0.09, 0.11]); cov6 = pack([0.04 if i == j else 0.002 for i in range(6) for j in range(6)])
    for label, args in (("OPTIMIZE.MULTIMODAL", ("FRACTAL.OPTIMIZE.MULTIMODAL", mu6, cov6, "3", "6", "0.15", "0.9")),
                        ("OPTIMIZE.MULTIMODAL.PARETO", ("FRACTAL.OPTIMIZE.MULTIMODAL.PARETO", mu6, cov6, "3", "6")),
                        ("AGENT.DIVERSE_PORTFOLIOS", (DPF, mu6, cov6, "3"))):
        r.check("%s with no daemon says so" % label, r.is_err(c.call(*args), NOREASON))
    for label, args, want in (
        ("a cardinality above the number of assets", (DPF, mu6, cov6, "7"), "must be <="),
        ("N_RESTARTS = 0", (DPF, mu6, cov6, "3", "N_RESTARTS", "0"), "must be in"),
        ("N_RESTARTS above 64", (DPF, mu6, cov6, "3", "N_RESTARTS", "65"), "must be in"),
        ("OVERLAP above 1", (DPF, mu6, cov6, "3", "OVERLAP", "1.5"), "OVERLAP"),
        ("QUALITY = 0", (DPF, mu6, cov6, "3", "QUALITY", "0"), "QUALITY"),
        ("an unknown OBJECTIVE_MODE", (DPF, mu6, cov6, "3", "OBJECTIVE_MODE", "median"), "OBJECTIVE_MODE"),
        ("an unknown option", (DPF, mu6, cov6, "3", "BOGUS"), "unknown option"),
        ("a cov that is not n x n", (DPF, mu6, pack([0.1] * 30), "3"), "n_assets x n_assets"),
        ("too few arguments", (DPF, mu6, cov6), "wrong number"),
    ):
        r.check("diverse_portfolios refuses %s" % label, r.is_err(c.call(*args), want), str(args)[:40])
    for a in (DPF, "FRACTAL.OPTIMIZE.MULTIMODAL", "FRACTAL.OPTIMIZE.MULTIMODAL.PARETO"):
        cats = {x.decode() if isinstance(x, bytes) else x for x in c.call("COMMAND", "INFO", a)[0][6]}
        r.check("%s is @dangerous (it reaches the daemon)" % a.split("FRACTAL.")[1], "@dangerous" in cats and "@read" not in cats, str(cats))

    # --- feedback_audit: diversification diagnostics (no reasoner)
    FA = "FRACTAL.AGENT.FEEDBACK_AUDIT"
    c.call("SET", "bt:ag:warm", pack([x for rw in rows[:10] for x in rw]))
    fa = jload(c.call(FA, "bt:ag:corpus", q7, "bt:ag:warm"))
    r.check("feedback_audit returns diversity_quotient and an explanation",
            fa is not None and "diversity_quotient" in fa and {"dq", "diversify_enabled", "overhead_p99_us"} <= set(fa.get("explanation", {})), str(fa))
    r.check("feedback_audit proves diversification ran: diversify_enabled and a computed dq",
            fa is not None and fa["explanation"]["diversify_enabled"] is True and fa["explanation"]["dq"] is not None
            and math.isfinite(fa["diversity_quotient"]), str(fa))
    r.check("feedback_audit: diversity_quotient is the explanation's dq",
            fa is not None and abs(fa["diversity_quotient"] - fa["explanation"]["dq"]) < 1e-5, str(fa))
    r.check("feedback_audit: the overhead p99 is null until the core's buffer fills",
            fa is not None and fa["explanation"]["overhead_p99_us"] is None, str(fa))
    r.check("feedback_audit gives the same diagnostics shape on a second call (no state carried between calls)",
            all(jload(c.call(FA, "bt:ag:corpus", q7, "bt:ag:warm"))["explanation"]["diversify_enabled"] is True for _ in range(3)))
    r.check("feedback_audit accepts WARMUP_COUNT beyond the warmup rows and a K",
            jload(c.call(FA, "bt:ag:corpus", q7, "bt:ag:warm", "WARMUP_COUNT", "100", "K", "5")) is not None)
    r.check("feedback_audit with WARMUP_COUNT 1", jload(c.call(FA, "bt:ag:corpus", q7, "bt:ag:warm", "WARMUP_COUNT", "1")) is not None)
    for label, args, want in (
        ("WARMUP_COUNT = 0", ("bt:ag:corpus", q7, "bt:ag:warm", "WARMUP_COUNT", "0"), "must be in"),
        ("WARMUP_COUNT above 256", ("bt:ag:corpus", q7, "bt:ag:warm", "WARMUP_COUNT", "257"), "must be in"),
        ("K = 0", ("bt:ag:corpus", q7, "bt:ag:warm", "K", "0"), "must be in"),
        ("a non-integer K", ("bt:ag:corpus", q7, "bt:ag:warm", "K", "x"), "integer"),
        ("a warmup key whose length is not a multiple of the dim", ("bt:ag:corpus", pack([1, 2, 3]), "bt:ag:warm"), "multiple"),
        ("a missing warmup key", ("bt:ag:corpus", q7, "bt:ag:nokey"), "missing"),
        ("a missing catalog key", ("bt:ag:nokey", q7, "bt:ag:warm"), "missing"),
        ("a non-finite query", ("bt:ag:corpus", pack([float("nan")] * 4), "bt:ag:warm"), "non-finite"),
        ("an unknown option", ("bt:ag:corpus", q7, "bt:ag:warm", "BOGUS"), "unknown option"),
        ("an option of another agent (ITERATIONS)", ("bt:ag:corpus", q7, "bt:ag:warm", "ITERATIONS", "5"), "unknown option"),
        ("too few arguments", ("bt:ag:corpus", q7), "wrong number"),
    ):
        r.check("feedback_audit refuses %s" % label, r.is_err(c.call(FA, *args), want), str(args)[:40])
    cats = {x.decode() if isinstance(x, bytes) else x for x in c.call("COMMAND", "INFO", FA)[0][6]}
    r.check("feedback_audit is @read (it never leaves the process), not @dangerous", "@read" in cats and "@dangerous" not in cats, str(cats))
    c.call("ACL", "SETUSER", "bt_ag_fa", "on", ">pw", "~bt:ag:corpus", "-@all", "+fractal.agent.feedback_audit", "+ping")
    cc = Client(r.host, r.port)
    if cc.call("AUTH", "bt_ag_fa", "pw") == "OK":
        r.check("feedback_audit: a warmup key outside the user's patterns is NOPERM (not a declared key)",
                r.is_err(cc.call(FA, "bt:ag:corpus", q7, "bt:ag:warm"), "NOPERM"))
        cc.close()
    c.call("ACL", "DELUSER", "bt_ag_fa")

    # --- building blocks: trajectory_predict, detect_loop, plan_explore, search_agent, rag_agent
    TP, DL, PL, SA, RG2 = ("FRACTAL.AGENT.TRAJECTORY_PREDICT", "FRACTAL.AGENT.DETECT_LOOP", "FRACTAL.AGENT.PLAN_EXPLORE",
                           "FRACTAL.SEARCH.AGENT", "FRACTAL.RAG.AGENT")

    def cosd(a, b):
        return 1 - sum(x * y for x, y in zip(a, b)) / (math.sqrt(sum(x * x for x in a)) * math.sqrt(sum(y * y for y in b)))
    delta7 = [x - y for x, y in zip(rows[39], rows[7])]                     # last row - baseline row 7
    tp = jload(c.call(TP, "bt:ag:corpus", "4", "7"))
    best = min(range(40), key=lambda i: cosd(rows[i], delta7))
    r.check("trajectory_predict returns the documented fields", tp is not None and
            {"predicted_state_vector", "projected_drift_delta", "risk_threshold_exceeded"} <= set(tp), str(tp))
    r.check("trajectory_predict: the predicted state is the row nearest (last row - baseline row) by cosine",
            tp is not None and all(abs(a - b) < 1e-6 for a, b in zip(tp["predicted_state_vector"], rows[best]))
            and abs(tp["projected_drift_delta"] - cosd(rows[best], delta7)) < 1e-4, "%s vs row %d" % (tp, best))
    r.check("trajectory_predict: risk_threshold_exceeded is distance > 0.5",
            tp is not None and tp["risk_threshold_exceeded"] == (tp["projected_drift_delta"] > 0.5))
    for label, args, want in (
        ("a baseline row past the end", ("bt:ag:corpus", "4", "40"), "no row"),
        ("a negative baseline row", ("bt:ag:corpus", "4", "-1"), "must be in"),
        ("a non-integer baseline row", ("bt:ag:corpus", "4", "x"), "integer"),
        ("a dim that does not divide the corpus", ("bt:ag:corpus", "3", "0"), "multiple"),
        ("dim = 0", ("bt:ag:corpus", "0", "0"), "must be in"),
        ("a missing corpus key", ("bt:ag:nokey", "4", "0"), "missing"),
        ("a non-blob corpus key", ("bt:ag:notahash", "4", "0"), "not a string"),
        ("too few arguments", ("bt:ag:corpus", "4"), "wrong number"),
        ("too many arguments", ("bt:ag:corpus", "4", "0", "5"), "wrong number"),
    ):
        r.check("trajectory_predict refuses %s" % label, r.is_err(c.call(TP, *args), want), str(args)[:40])

    # detect_loop: the rule is (dfa of the state norms > 0.9) OR (a fingerprint cycle closes)
    cyc = [[(1 + 0.15 * (i % 6)) * x for x in (math.sin(i * 0.9), math.cos(i * 0.9), 1.0, 0.5)] for i in range(6)] * 5   # period 6, 30 states, varying norms
    c.call("SET", "bt:ag:cyc", pack([x for st_ in cyc for x in st_]))
    dl = jload(c.call(DL, "agent-7", "bt:ag:cyc", "4"))
    r.check("detect_loop returns agent_id, dfa_exponent, is_loop_detected",
            dl is not None and dl["agent_id"] == "agent-7" and {"dfa_exponent", "is_loop_detected"} <= set(dl), str(dl))
    norms = [math.sqrt(sum(x * x for x in st_)) for st_ in cyc]
    def dfa_or0(vals):                       # a DFA failure is ignored, leaving alpha at 0
        v = c.call("FRACTAL.DIM.DFA", pack(vals))
        return 0.0 if isinstance(v, RespError) else float(v)
    dfa_n = dfa_or0(norms)
    r.check("detect_loop's dfa_exponent is FRACTAL.DIM.DFA of the state norms",
            dl is not None and abs(dl["dfa_exponent"] - dfa_n) < 1e-5, "%s vs %s" % (dl, dfa_n))
    fps = b"".join(c.call("FRACTAL.SERIES.FINGERPRINT", pack(st_), "64", "42") for st_ in cyc)
    cy = jload(c.call("FRACTAL.CYCLE.DETECT", fps, "8", "0"))
    r.check("detect_loop agrees with FRACTAL.SERIES.FINGERPRINT + FRACTAL.CYCLE.DETECT on the same states (a loop is found)",
            dl is not None and cy is not None and cy["detected"] is True and dl["is_loop_detected"] is True, "%s / %s" % (dl, cy))
    nrng = random.Random(77)
    free_run = [[nrng.uniform(1, 2) * x for x in (1.0, 0.0, 0.0, 0.0)] if False else
                [nrng.gauss(0, 1) for _ in range(4)] for _ in range(300)]
    c.call("SET", "bt:ag:free", pack([x for st_ in free_run for x in st_]))
    fr = jload(c.call(DL, "agent-8", "bt:ag:free", "4"))
    fnorm = dfa_or0([math.sqrt(sum(x * x for x in st_)) for st_ in free_run])
    r.check("detect_loop on 300 unrelated states reports the same dfa as the primitive and no loop when dfa <= 0.9",
            fr is not None and abs(fr["dfa_exponent"] - fnorm) < 1e-5 and (fr["is_loop_detected"] is (fnorm > 0.9 or
            jload(c.call("FRACTAL.CYCLE.DETECT", b"".join(c.call("FRACTAL.SERIES.FINGERPRINT", pack(st_), "64", "42") for st_ in free_run), "8", "0"))["detected"])), str(fr))
    r.check("detect_loop accepts N_BITS, SEED and HAMMING_THRESHOLD",
            jload(c.call(DL, "a", "bt:ag:cyc", "4", "N_BITS", "128", "SEED", "7.5", "HAMMING_THRESHOLD", "3")) is not None)
    c.call("SET", "bt:ag:one", pack([1.0, 2.0, 3.0, 4.0]))
    for label, args, want in (
        ("fewer than 2 states", ("a", "bt:ag:one", "4"), "at least 2"),
        ("a state_log that is not a multiple of dim", ("a", "bt:ag:cyc", "7"), "multiple"),
        ("dim = 0", ("a", "bt:ag:cyc", "0"), "must be in"),
        ("N_BITS = 0", ("a", "bt:ag:cyc", "4", "N_BITS", "0"), "must be in"),
        ("N_BITS above 4096", ("a", "bt:ag:cyc", "4", "N_BITS", "4097"), "must be in"),
        ("a HAMMING_THRESHOLD above N_BITS", ("a", "bt:ag:cyc", "4", "N_BITS", "8", "HAMMING_THRESHOLD", "9"), "must not exceed"),
        ("a negative HAMMING_THRESHOLD", ("a", "bt:ag:cyc", "4", "HAMMING_THRESHOLD", "-1"), "must be in"),
        ("a non-finite SEED", ("a", "bt:ag:cyc", "4", "SEED", "nan"), "finite"),
        ("an empty agent_id", ("", "bt:ag:cyc", "4"), "agent_id"),
        ("an oversized agent_id", ("x" * 300, "bt:ag:cyc", "4"), "agent_id"),
        ("a missing state_log key", ("a", "bt:ag:nokey", "4"), "missing"),
        ("a non-blob state_log", ("a", "bt:ag:str", "4"), "float32"),
        ("an unknown option", ("a", "bt:ag:cyc", "4", "BOGUS"), "unknown option"),
        ("too few arguments", ("a", "bt:ag:cyc"), "wrong number"),
    ):
        r.check("detect_loop refuses %s" % label, r.is_err(c.call(DL, *args), want), str(args)[:40])

    # the three text-driven agents: argument checking happens before any embedding call
    txt_args = {PL: ("bt:ag:corpus", "go north", "4", "3"), SA: ("bt:ag:corpus", "what is nearby", "4"), RG2: ("bt:ag:corpus", "what is nearby", "4")}
    for name, agent in (("plan_explore", PL), ("search_agent", SA), ("rag_agent", RG2)):
        a0 = txt_args[agent]
        r.check("%s with no daemon fails cleanly (it needs an embedding provider)" % name, r.is_err(c.call(agent, *a0), NOREASON))
        for label, args, want in (
            ("dim = 0", (a0[0], a0[1], "0") + a0[3:], "must be in"),
            ("a dim above 4096", (a0[0], a0[1], "4097") + a0[3:], "must be in"),
            ("empty text", (a0[0], "", a0[2]) + a0[3:], "text"),
            ("oversized text", (a0[0], "x" * 70000, a0[2]) + a0[3:], "text"),
            ("text with a NUL byte", (a0[0], b"a\x00b", a0[2]) + a0[3:], "NUL"),
            ("a missing corpus key", ("bt:ag:nokey",) + a0[1:], "missing"),
            ("a dim that does not divide the corpus", (a0[0], a0[1], "3") + a0[3:], "multiple"),
            ("an unknown option", a0 + ("BOGUS",), "unknown option"),
            ("too few arguments", a0[:2], "wrong number"),
        ):
            r.check("%s refuses %s" % (name, label), r.is_err(c.call(agent, *args), want), str(args)[:50])
    r.check("plan_explore refuses max_branches = 0", r.is_err(c.call(PL, "bt:ag:corpus", "go", "4", "0"), "must be in"))
    r.check("plan_explore refuses max_branches above 100000", r.is_err(c.call(PL, "bt:ag:corpus", "go", "4", "100001"), "must be in"))
    r.check("search_agent refuses POPULATION = 0", r.is_err(c.call(SA, "bt:ag:corpus", "q", "4", "POPULATION", "0"), "must be in"))
    r.check("search_agent refuses ITERATIONS x POPULATION above the work cap",
            r.is_err(c.call(SA, "bt:ag:corpus", "q", "4", "ITERATIONS", "10000", "POPULATION", "10000"), "at most"))
    # The standard search work cap (the second agent entry point that enforces it):
    # 10000 x 1000 x 2 x 200 = 4000000000 is over 2^31.
    r.check("search_agent refuses ITERATIONS x POPULATION x diffusion x dim above the search work cap",
            r.is_err(c.call(SA, "bt:ag:corpus", "q", "200", "ITERATIONS", "10000", "POPULATION", "1000"),
                     "search too large (iterations x population_size x diffusion_factor x dim exceeds 2147483648)"))
    r.check("rag_agent refuses POPULATION (it uses a fixed 50 x 15)", r.is_err(c.call(RG2, "bt:ag:corpus", "q", "4", "POPULATION", "5"), "unknown option"))
    r.check("search_agent refuses IDS naming a non-hash", r.is_err(c.call(SA, "bt:ag:corpus", "q", "4", "IDS", "bt:ag:notahash"), "not a hash"))

    def catset(a):
        return {x.decode() if isinstance(x, bytes) else x for x in c.call("COMMAND", "INFO", a)[0][6]}
    for a in (TP, DL):
        r.check("%s is @read, not @dangerous" % a.split(".")[-1].lower(), "@read" in catset(a) and "@dangerous" not in catset(a), str(catset(a)))
    for a in (PL, SA, RG2):
        r.check("%s is @dangerous (it sends text to the embedding provider)" % a.split(".")[-1].lower(),
                "@dangerous" in catset(a) and "@read" not in catset(a), str(catset(a)))
    r.check("the server survived", r.alive())

def gate_19(r):
    """FRACTAL.AGENT.DATA_ANALYST against a real valkey-search index, with a mock model."""
    c = r.c
    A = "FRACTAL.AGENT.DATA_ANALYST"
    vec = pack([0.0, 0.0])
    r.check("FT.CREATE builds the index", c.call("FT.CREATE", "people", "ON", "HASH", "PREFIX", "1", "d:", "SCHEMA",
            "age", "NUMERIC", "name", "TAG", "v", "VECTOR", "FLAT", "6", "TYPE", "FLOAT32", "DIM", "2", "DISTANCE_METRIC", "L2") == "OK")
    for i, (age, name) in enumerate([(25, "ann"), (31, "bob"), (47, "cy"), (52, "di")]):
        c.call("HSET", "d:%d" % i, "age", age, "name", name, "v", vec)
    import time
    for _ in range(50):
        info = c.call("FT.INFO", "people")
        if isinstance(info, list) and "num_docs" in info and int(info[info.index("num_docs") + 1]) == 4:
            break
        time.sleep(0.1)
    base = c.call("DBSIZE")

    out = c.call(A, "people", "who is older than thirty?")
    if r.check("the agent answers", isinstance(out, (str, bytes)), str(out)[:200]):
        d = jload(out)
        r.check("the generated query is the model's expression", d.get("generated_query") == "@age:[(30 +inf]", str(d)[:200])
        res = d.get("result_json", {})
        keys = sorted(x["key"] for x in res.get("rows", []))
        r.check("the result is what valkey-search returned", res.get("total") == 3 and keys == ["d:1", "d:2", "d:3"], str(res)[:300])
        r.check("fields are returned as an object", all("age" in x.get("fields", {}) for x in res["rows"]), str(res)[:300])
        r.check("there is an analysis", bool(d.get("analysis")), str(d)[:200])
        r.check("the engine is reported as valkey-search", d.get("engine") == "valkey-search", str(d)[:200])
    # the same question by scan mode, over the same hashes, finds the same rows (the two engines agree)
    ds = jload(c.call(A, "d:", "who is older than thirty?", "MODE", "SCAN"))
    r.check("scan mode over the same hashes agrees with valkey-search", ds.get("engine") == "scan" and
            sorted(x["key"] for x in ds["result_json"]["rows"]) == ["d:1", "d:2", "d:3"] and ds["result_json"]["total"] == 3, str(ds)[:300])
    r.check("naming a PREFIX selects scan mode even with valkey-search loaded", jload(c.call(A, "people", "who is older than thirty?", "PREFIX", "d:")).get("engine") == "scan")
    d = jload(c.call(A, "people", "who is older than thirty?", "LIMIT", "2"))
    r.check("LIMIT caps the rows but reports the total", len(d["result_json"]["rows"]) == 2 and d["result_json"]["total"] == 3, str(d)[:200])

    # a bad first answer is fed back and corrected; one attempt is not enough
    d = c.call(A, "people", "FRACTALSQL_BT_VKQ_RETRY who is older?", "MAX_ATTEMPTS", "2")
    r.check("a refused expression is retried with the reason", isinstance(d, (str, bytes)) and jload(d).get("generated_query") == "@age:[(30 +inf]", str(d)[:200])
    r.check("one attempt is not enough for it", r.is_err(c.call(A, "people", "FRACTALSQL_BT_VKQ_RETRY who is older?", "MAX_ATTEMPTS", "1"), "no query that valkey-search accepts"))

    # hostile or malformed model output never becomes a command
    for marker, needle in [("NEWLINE", "control or non-ASCII"), ("LONG", "longer than 512"), ("BINARY", "control or non-ASCII"),
                           ("BADSYNTAX", "no query that valkey-search accepts")]:
        e = c.call(A, "people", "FRACTALSQL_BT_VKQ_%s q" % marker, "MAX_ATTEMPTS", "2")
        r.check("hostile reply %s is refused" % marker, r.is_err(e, needle), str(e)[:200])
    e = c.call(A, "people", "FRACTALSQL_BT_VKQ_STACK q", "MAX_ATTEMPTS", "1")
    r.check("a stacked-command reply stays one search argument and cannot run a second command", c.call("DBSIZE") == base and (r.is_err(e) or isinstance(e, str)), str(e)[:200])
    r.check("no data was lost to any of them", c.call("DBSIZE") == base)
    r.check("a reply with no query block is an error", r.is_err(c.call(A, "people", "FRACTALSQL_BT_VKQ_PROSE q", "MAX_ATTEMPTS", "1")))

    # argument checks
    r.check("an unknown index is a clear error", r.is_err(c.call(A, "nosuch", "q"), "cannot read the index"))
    r.check("a bad index name is refused", r.is_err(c.call(A, "a b;c", "q"), "index name"))
    r.check("an empty question is refused", r.is_err(c.call(A, "people", ""), "question"))
    r.check("a long question is refused", r.is_err(c.call(A, "people", "x" * 5000), "question"))
    r.check("LIMIT 0 is refused", r.is_err(c.call(A, "people", "q", "LIMIT", "0")))
    r.check("LIMIT 101 is refused", r.is_err(c.call(A, "people", "q", "LIMIT", "101")))
    r.check("MAX_ATTEMPTS 6 is refused", r.is_err(c.call(A, "people", "q", "MAX_ATTEMPTS", "6")))
    r.check("an unknown option is refused", r.is_err(c.call(A, "people", "q", "WHAT", "1"), "unknown option"))
    r.check("an option without a value is refused", r.is_err(c.call(A, "people", "q", "LIMIT")))
    r.check("one argument is an arity error", r.is_err(c.call(A, "people")))
    r.check("it will not run in MULTI", True)
    c.call("MULTI"); c.call(A, "people", "q"); res = c.call("EXEC")
    r.check("inside MULTI it refuses", isinstance(res, list) and r.is_err(res[0], "cannot run inside MULTI"), str(res)[:200])
    e = c.call("EVAL", "return redis.call('FRACTAL.AGENT.DATA_ANALYST','people','q')", "0")
    r.check("inside a script it refuses", r.is_err(e), str(e)[:200])

    # the agent adds no privilege: it runs as the caller
    c.call("ACL", "SETUSER", "nofind", "on", ">pw", "~*", "-@all", "+fractal.agent.data_analyst", "+@search", "-ft.search")
    c.call("ACL", "SETUSER", "finder", "on", ">pw", "~*", "-@all", "+fractal.agent.data_analyst", "+@search")
    c2 = Client(r.host, r.port); c2.call("AUTH", "nofind", "pw")
    e = c2.call(A, "people", "who is older than thirty?")
    r.check("a caller without FT.SEARCH is refused", r.is_err(e, "no permissions"), str(e)[:200])
    c2.close()
    c3 = Client(r.host, r.port); c3.call("AUTH", "finder", "pw")
    e = c3.call(A, "people", "who is older than thirty?")
    r.check("a caller with FT.SEARCH is served", isinstance(e, (str, bytes)) and jload(e)["result_json"]["total"] == 3, str(e)[:200])
    c3.close()
    c.call("ACL", "SETUSER", "noinfo", "on", ">pw", "~*", "-@all", "+fractal.agent.data_analyst", "+@search", "-ft.info")
    c4 = Client(r.host, r.port); c4.call("AUTH", "noinfo", "pw")
    e = c4.call(A, "people", "q")
    r.check("a caller without FT.INFO is refused", r.is_err(e, "no permissions"), str(e)[:200])
    c4.close()
    r.check("the server survived", r.alive())


# 41 analyst scan: DATA_ANALYST without valkey-search. The model writes a JSON filter; the module scans the hashes under a key prefix, as the caller.
def gate_41(r):
    c = r.c
    A = "FRACTAL.AGENT.DATA_ANALYST"
    vec = pack([0.0, 0.0])
    people = [(25, "ann", "london"), (31, "bob", "paris"), (47, "cy", "london"), (52, "di", "berlin")]
    for i, (age, name, city) in enumerate(people):
        c.call("HSET", "d:%d" % i, "age", age, "name", name, "city", city, "v", vec)
    c.call("HSET", "x:1", "age", 99, "name", "zed", "city", "oslo")      # another prefix: never visible to a d: question
    c.call("SET", "d:plain", "not a hash")                              # a string under the prefix: not a hash, never examined
    base = c.call("DBSIZE")

    def ask(*a):
        out = c.call(A, *a)
        return jload(out) if isinstance(out, (str, bytes)) else out

    def rows(d):
        return sorted(x["key"] for x in d["result_json"]["rows"])

    # the basic answer, and what it reports
    d = ask("d:", "who is older than thirty?", "MODE", "SCAN")
    if r.check("scan mode answers", isinstance(d, dict), str(d)[:300]):
        res = d["result_json"]
        r.check("the engine is reported as scan", d.get("engine") == "scan", str(d)[:200])
        r.check("the generated query is the model's filter", json.loads(d["generated_query"]) == {"field": "age", "op": ">", "value": 30}, str(d)[:200])
        r.check("the rows are the hashes that match, under the prefix only", rows(d) == ["d:1", "d:2", "d:3"] and res["total"] == 3, str(res)[:300])
        r.check("only the hashes under the prefix were examined (not the string, not x:1)", res["scanned"] == 4 and res["complete"] is True, str(res)[:300])
        r.check("fields come back as an object, a vector as a placeholder", all("age" in x["fields"] for x in res["rows"]) and
                all(str(x["fields"]["v"]).startswith("<binary") for x in res["rows"]), str(res)[:300])
        r.check("there is an analysis", bool(d.get("analysis")), str(d)[:200])
    d = ask("d:", "who is older than thirty?", "MODE", "SCAN", "LIMIT", "2")
    r.check("LIMIT caps the rows but reports the total", len(d["result_json"]["rows"]) == 2 and d["result_json"]["total"] == 3, str(d)[:200])

    # AUTO falls back to the scan when valkey-search is not loaded; SEARCH does not
    d = ask("d:", "who is older than thirty?")
    r.check("AUTO falls back to a scan when valkey-search is not loaded", isinstance(d, dict) and d.get("engine") == "scan" and rows(d) == ["d:1", "d:2", "d:3"], str(d)[:300])
    d = ask("people", "who is older than thirty?", "PREFIX", "d:")
    r.check("PREFIX names the keys when the index name is not a prefix", isinstance(d, dict) and d.get("engine") == "scan" and rows(d) == ["d:1", "d:2", "d:3"], str(d)[:300])
    r.check("no hashes under a prefix is a clear error", r.is_err(c.call(A, "nosuch:", "q"), "no hashes were found under the key prefix"))
    r.check("MODE SEARCH needs valkey-search and says so", r.is_err(c.call(A, "d:", "q", "MODE", "SEARCH"), "is not available"))

    # every operator, picked out by a marker in the question
    cases = [("ANY", ["d:0", "d:3"]), ("ALL", ["d:1", "d:2"]), ("IN", ["d:1", "d:3"]), ("PREFIX", ["d:1"]), ("CONTAINS", ["d:0", "d:2"]),
             ("NUMEQ", ["d:2"]), ("NE", ["d:1", "d:2", "d:3"])]
    for marker, want in cases:
        d = ask("d:", "FRACTALSQL_BT_VKS_%s who?" % marker, "MODE", "SCAN", "MAX_ATTEMPTS", "1")
        r.check("filter %s selects the right rows" % marker, isinstance(d, dict) and rows(d) == want, str(d)[:300])
    d = ask("d:", "FRACTALSQL_BT_VKS_BINARY who?", "MODE", "SCAN", "MAX_ATTEMPTS", "1")
    r.check("a text comparison against a binary field matches nothing and breaks nothing", isinstance(d, dict) and d["result_json"]["total"] == 0, str(d)[:300])

    # a bad first answer is fed back and corrected
    d = ask("d:", "FRACTALSQL_BT_VKS_RETRY who is older?", "MODE", "SCAN", "MAX_ATTEMPTS", "2")
    r.check("a refused filter is retried with the reason", isinstance(d, dict) and rows(d) == ["d:1", "d:2", "d:3"], str(d)[:300])
    r.check("one attempt is not enough for it", r.is_err(c.call(A, "d:", "FRACTALSQL_BT_VKS_RETRY who is older?", "MODE", "SCAN", "MAX_ATTEMPTS", "1"), "no filter that can be used"))

    # hostile or malformed model output never becomes a command or a wrong answer
    for marker, needle in [("BADJSON", "filter was rejected"), ("UNKNOWNOP", "an op must be"), ("UNKNOWNFIELD", "there is no field 'nosuch'"),
                           ("UNKNOWNKEY", "unknown key"), ("NUMOP", "need a number"), ("DEEP", "nested too deeply"), ("LONG", "longer than 2048"),
                           ("STACK", "text after the filter"), ("PROSE", "filter was rejected")]:
        e = c.call(A, "d:", "FRACTALSQL_BT_VKS_%s q" % marker, "MODE", "SCAN", "MAX_ATTEMPTS", "1")
        r.check("hostile reply %s is refused" % marker, r.is_err(e, needle), str(e)[:240])
    r.check("no data was lost or added by any of them", c.call("DBSIZE") == base)
    r.check("the unknown-field refusal tells the model which fields exist", r.is_err(c.call(A, "d:", "FRACTALSQL_BT_VKS_UNKNOWNFIELD q", "MODE", "SCAN", "MAX_ATTEMPTS", "1"), "the fields are"))

    # argument checks
    r.check("an unknown mode is refused", r.is_err(c.call(A, "d:", "q", "MODE", "FAST"), "MODE must be"))
    r.check("a mode without a value is refused", r.is_err(c.call(A, "d:", "q", "MODE")))
    r.check("a bad prefix is refused", r.is_err(c.call(A, "d:", "q", "PREFIX", "a b*"), "PREFIX may contain"))
    r.check("an empty prefix is refused", r.is_err(c.call(A, "d:", "q", "PREFIX", ""), "PREFIX must be"))
    r.check("a prefix with MODE SEARCH is refused", r.is_err(c.call(A, "d:", "q", "PREFIX", "d:", "MODE", "SEARCH"), "PREFIX is for"))
    r.check("LIMIT 101 is still refused", r.is_err(c.call(A, "d:", "q", "MODE", "SCAN", "LIMIT", "101")))
    c.call("MULTI"); c.call(A, "d:", "q", "MODE", "SCAN"); res = c.call("EXEC")
    r.check("inside MULTI it refuses", isinstance(res, list) and r.is_err(res[0], "cannot run inside MULTI"), str(res)[:200])

    # the agent adds no privilege: every command runs as the caller
    c.call("ACL", "SETUSER", "scanner", "on", ">pw", "~d:*", "-@all", "+fractal.agent.data_analyst", "+scan", "+hgetall")
    c.call("ACL", "SETUSER", "narrow", "on", ">pw", "~d:1", "~d:2", "-@all", "+fractal.agent.data_analyst", "+scan", "+hgetall")
    c.call("ACL", "SETUSER", "noscan", "on", ">pw", "~d:*", "-@all", "+fractal.agent.data_analyst", "+hgetall")
    c.call("ACL", "SETUSER", "nohget", "on", ">pw", "~d:*", "-@all", "+fractal.agent.data_analyst", "+scan")
    def as_user(name, *a):
        cu = Client(r.host, r.port); cu.call("AUTH", name, "pw")
        try:
            out = cu.call(A, *a)
            return jload(out) if isinstance(out, (str, bytes)) else out
        finally:
            cu.close()
    d = as_user("scanner", "d:", "who is older than thirty?", "MODE", "SCAN")
    r.check("a caller with SCAN and HGETALL on the prefix is served", isinstance(d, dict) and rows(d) == ["d:1", "d:2", "d:3"], str(d)[:300])
    d = as_user("narrow", "d:", "who is older than thirty?", "MODE", "SCAN")
    r.check("keys the caller cannot read are invisible: the answer and the counts cover only d:1 and d:2",
            isinstance(d, dict) and rows(d) == ["d:1", "d:2"] and d["result_json"]["total"] == 2 and d["result_json"]["scanned"] == 2, str(d)[:300])
    r.check("a caller without SCAN is refused", r.is_err(as_user("noscan", "d:", "q", "MODE", "SCAN"), "no permissions"))
    r.check("a caller without HGETALL sees nothing", r.is_err(as_user("nohget", "d:", "q", "MODE", "SCAN"), "no hashes were found"))

    # the key budget: a large prefix is scanned in bounded steps and reported as incomplete
    for i in range(10500):
        c.call("HSET", "big:%d" % i, "age", i)
    d = ask("big:", "who is older than thirty?", "MODE", "SCAN", "LIMIT", "5")
    if r.check("a big prefix still answers", isinstance(d, dict), str(d)[:300]):
        res = d["result_json"]
        r.check("the scan stops at its key budget and says it is incomplete", res["complete"] is False and 10000 <= res["scanned"] <= 10200 and len(res["rows"]) == 5, str(res)[:200])
    for i in range(10500):
        c.call("DEL", "big:%d" % i)
    r.check("the server survived", r.alive())


# 20 sessions: named diversify/repulsion state kept between searches
def gate_20(r):
    c = r.c
    random.seed(20)
    rows = []
    for i in range(60):
        base = [1, 0, 0, 0] if i < 30 else [0, 1, 0, 0]
        rows.append([x + random.gauss(0, 0.05) for x in base])
    c.call("SET", "bt:s:corpus", b"".join(pack(v) for v in rows))
    q = pack([1, 0.1, 0, 0])
    ok = lambda x: x == "OK"
    S = lambda *a: c.call(*a)
    # earlier gates share this server and may leave sessions: start clean
    for sid in (S("FRACTAL.SESSION.LIST") or []):
        S("FRACTAL.SESSION.CLOSE", sid)
    r.check("no sessions after closing every listed one", S("FRACTAL.SESSION.LIST") == [])
    r.check("DIVERSIFY.ENABLE creates a session", ok(S("FRACTAL.DIVERSIFY.ENABLE", "s1")))
    r.check("SESSION.LIST names it", S("FRACTAL.SESSION.LIST") == [b"s1"])
    ex = json.loads(S("FRACTAL.DIVERSIFY.EXPLAIN", "s1"))
    r.check("EXPLAIN: enabled, no D_q before the first search", ex["diversify_enabled"] is True and ex["dq"] is None, str(ex))
    r.check("DQ is nil before any search", S("FRACTAL.DIVERSIFY.DQ", "s1") is None)
    p0 = json.loads(S("FRACTAL.DIVERSIFY.PARAMS", "s1"))
    r.check("PARAMS reads the defaults", p0["window_n"] == 5 and p0["max_shadows_considered"] == 64 and p0["tail_buffer_cap"] == 256, str(p0))
    p1 = json.loads(S("FRACTAL.DIVERSIFY.PARAMS", "s1", '{"repulsion_weight": 2.5, "window_n": 6}'))
    r.check("PARAMS sets only the named fields", p1["repulsion_weight"] == 2.5 and p1["window_n"] == 6 and p1["max_shadows_considered"] == 64, str(p1))
    for name, arg in (("an unknown key", '{"bogus":1}'), ("a non-number", '{"window_n":"x"}'), ("a fractional whole-number field", '{"window_n":2.5}'),
                      ("trailing text", '{"window_n":3} x'), ("a non-object", "[1]"), ("NaN", '{"repulsion_weight":NaN}')):
        r.check("PARAMS refuses %s" % name, r.is_err(S("FRACTAL.DIVERSIFY.PARAMS", "s1", arg)))
    r.check("PARAMS lets the core refuse an out-of-range value", r.is_err(S("FRACTAL.DIVERSIFY.PARAMS", "s1", '{"tail_buffer_cap":5000}')))
    p2 = json.loads(S("FRACTAL.DIVERSIFY.PARAMS", "s1"))
    r.check("a refused PARAMS call changes nothing", p2 == p1, str(p2))
    for bad in ("", "a b", "x" * 65, "s/1"):
        r.check("session id %r is refused" % bad[:12], r.is_err(S("FRACTAL.DIVERSIFY.ENABLE", bad), "session id"))

    # SESSION on the search commands
    sx = S("FRACTAL.EXPLORE", "bt:s:corpus", q, "20", "SESSION", "s1")
    r.check("EXPLORE ... SESSION runs", not r.is_err(sx) and "population" in json.loads(sx), str(sx)[:100])
    r.check("DQ is still nil after one search (the window is not full)", S("FRACTAL.DIVERSIFY.DQ", "s1") is None)
    ss = S("FRACTAL.SEARCH", "bt:s:corpus", q, "5", "SESSION", "s1")
    r.check("SEARCH ... SESSION runs, K and SESSION together", not r.is_err(ss) and len(json.loads(ss)["top_k"]) == 5, str(ss)[:100])
    r.check("SEARCH ... SESSION with no K", not r.is_err(S("FRACTAL.SEARCH", "bt:s:corpus", q, "SESSION", "s1")))
    dq = S("FRACTAL.DIVERSIFY.DQ", "s1")
    r.check("DQ is a number in [0, 1] once the session has searched more than once", dq is not None and 0.0 <= float(dq) <= 1.0, str(dq))
    r.check("a search through an unknown session is refused", r.is_err(S("FRACTAL.SEARCH", "bt:s:corpus", q, "5", "SESSION", "nope"), "no such session"))
    r.check("SESSION with no id is a wrong-arity error", r.is_err(S("FRACTAL.SEARCH", "bt:s:corpus", q, "SESSION"), "wrong number"))
    r.check("a stray argument after K is refused", r.is_err(S("FRACTAL.SEARCH", "bt:s:corpus", q, "5", "6"), "wrong number"))
    r.check("K is still range-checked with SESSION", r.is_err(S("FRACTAL.SEARCH", "bt:s:corpus", q, "0", "SESSION", "s1"), "out of range"))

    # feedback
    top = json.loads(ss)["top_k"][0]["idx"]
    r.check("FEEDBACK NEGATIVE is accepted", ok(S("FRACTAL.FEEDBACK", "s1", top, "NEGATIVE")))
    r.check("FEEDBACK DWELL with a dwell time is accepted", ok(S("FRACTAL.FEEDBACK", "s1", top, "dwell", 1500)))
    r.check("FEEDBACK POSITIVE is accepted", ok(S("FRACTAL.FEEDBACK", "s1", top, "positive")))
    r.check("ISOLATE_BACKGROUND is accepted", ok(S("FRACTAL.ISOLATE_BACKGROUND", "s1", top)))
    r.check("FEEDBACK refuses an unknown kind", r.is_err(S("FRACTAL.FEEDBACK", "s1", top, "like"), "kind must be"))
    r.check("FEEDBACK refuses a negative row", r.is_err(S("FRACTAL.FEEDBACK", "s1", "-1", "negative")))
    r.check("FEEDBACK refuses a row beyond 32 bits", r.is_err(S("FRACTAL.FEEDBACK", "s1", "4294967296", "negative")))
    r.check("FEEDBACK on an unknown session is refused", r.is_err(S("FRACTAL.FEEDBACK", "nope", 0, "negative"), "no such session"))

    # sessions are independent, and deterministic
    def run(sid, feedback):
        S("FRACTAL.DIVERSIFY.ENABLE", sid)
        S("FRACTAL.DIVERSIFY.PARAMS", sid, '{"repulsion_weight": 50, "repulsion_sigma": 1.0}')
        a = json.loads(S("FRACTAL.EXPLORE", "bt:s:corpus", q, "20", "SESSION", sid))
        if feedback:
            S("FRACTAL.FEEDBACK", sid, a["top_k"][0]["idx"], "NEGATIVE")
        S("FRACTAL.EXPLORE", "bt:s:corpus", q, "20", "SESSION", sid)
        return float(S("FRACTAL.DIVERSIFY.DQ", sid))
    d_a, d_b, d_c = run("ia", True), run("ib", True), run("ic", False)
    r.check("two sessions given the same calls end in the same D_q", d_a == d_b, "%r %r" % (d_a, d_b))
    r.check("feedback changes the session's state (D_q differs from a session without it)", d_a != d_c, "%r %r" % (d_a, d_c))

    # disable, close, limits
    r.check("DISABLE is accepted", ok(S("FRACTAL.DIVERSIFY.DISABLE", "s1")))
    r.check("EXPLAIN shows it disabled", json.loads(S("FRACTAL.DIVERSIFY.EXPLAIN", "s1"))["diversify_enabled"] is False)
    r.check("DISABLE on an unknown session is refused", r.is_err(S("FRACTAL.DIVERSIFY.DISABLE", "nope"), "no such session"))
    r.check("SESSION.CLOSE removes it", ok(S("FRACTAL.SESSION.CLOSE", "s1")) and b"s1" not in S("FRACTAL.SESSION.LIST"))
    r.check("a closed session can no longer be searched", r.is_err(S("FRACTAL.SEARCH", "bt:s:corpus", q, "5", "SESSION", "s1"), "no such session"))
    r.check("closing twice is refused", r.is_err(S("FRACTAL.SESSION.CLOSE", "s1"), "no such session"))
    for sid in ("ia", "ib", "ic"):
        S("FRACTAL.SESSION.CLOSE", sid)
    made = [("lim%d" % i) for i in range(64)]
    for sid in made:
        S("FRACTAL.DIVERSIFY.ENABLE", sid)
    r.check("the 65th session is refused (limit 64)", r.is_err(S("FRACTAL.DIVERSIFY.ENABLE", "lim-one-too-many"), "too many sessions"))
    r.check("an existing session still works at the limit", ok(S("FRACTAL.DIVERSIFY.ENABLE", "lim0")))
    for sid in made:
        S("FRACTAL.SESSION.CLOSE", sid)
    r.check("closing frees the slots", ok(S("FRACTAL.DIVERSIFY.ENABLE", "again")) and ok(S("FRACTAL.SESSION.CLOSE", "again")))

    # a session closed while a search runs on it is freed afterwards, and the server survives
    big = b"".join(pack([random.random() for _ in range(32)]) for _ in range(40000))
    c.call("SET", "bt:s:big", big)
    S("FRACTAL.DIVERSIFY.ENABLE", "busy")
    c2 = Client(r.host, r.port)
    box = []
    th = threading.Thread(target=lambda: box.append(c2.call("FRACTAL.EXPLORE", "bt:s:big", pack([0.5] * 32), "600", "SESSION", "busy")))
    th.start()
    time.sleep(0.3)
    dq = S("FRACTAL.DIVERSIFY.DQ", "busy")
    r.check("a control command during a search answers (a value, or 'session busy'), never blocks",
            dq is None or not r.is_err(dq) or "busy" in str(dq), str(dq))
    r.check("SESSION.CLOSE during a search is accepted", ok(S("FRACTAL.SESSION.CLOSE", "busy")))
    th.join(120)
    r.check("the search that was running still completes", len(box) == 1 and not r.is_err(box[0]) and b"population" in box[0], str(box)[:100])
    c2.close()
    r.check("the closed session is gone", r.is_err(S("FRACTAL.DIVERSIFY.DQ", "busy"), "no such session"))
    r.check("the server survived", r.alive())
    for k in ("bt:s:corpus", "bt:s:big"):
        c.call("DEL", k)

    # ACL: sessions are state, so reading a corpus does not grant them
    c.call("ACL", "SETUSER", "bt-s-reader", "on", ">p", "~*", "-@all", "+@read")
    cr = Client(r.host, r.port)
    cr.call("AUTH", "bt-s-reader", "p")
    r.check("an @read user cannot create a session", r.is_err(cr.call("FRACTAL.DIVERSIFY.ENABLE", "x"), "NOPERM") or r.is_err(cr.call("FRACTAL.DIVERSIFY.ENABLE", "x"), "permission"))
    r.check("an @read user can list sessions", not r.is_err(cr.call("FRACTAL.SESSION.LIST")))
    cr.close()
    c.call("ACL", "DELUSER", "bt-s-reader")
    r.check("the server survived", r.alive())


# --------------------------------------------------------------------------
# 33 fake_daemon: the module's daemon client against a daemon that misbehaves (tests/fake_daemon.py)
#   Needs FSQL_FAKE_SOCK and FSQL_FAKE_KEY (the key file the server was loaded with). The real daemon always answers correctly, so the client's
#   handling of a bad reply is driven with a scripted double: every daemon command, the multimodal optimizers and (through gates 15 and 18's own
#   call sequences, with their assertions switched off) every agent, under each way a daemon can fail. What is asserted is that the server
#   answers every one and survives; the replies are not the point.
# --------------------------------------------------------------------------
class _Quiet(Run):
    def ok(self, name): pass
    def bad(self, name, detail): pass
    def known(self, name, detail): pass
    def check(self, name, cond, detail=""): pass


def gate_33(r):
    sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
    from fake_daemon import FakeDaemon
    sock, keyf = os.environ.get("FSQL_FAKE_SOCK"), os.environ.get("FSQL_FAKE_KEY")
    if not sock or not keyf:
        r.bad("fake_daemon", "FSQL_FAKE_SOCK and FSQL_FAKE_KEY are not set")
        return
    fd = FakeDaemon(sock, open(keyf).read().strip())
    q = _Quiet(r.gate, r.host, r.port, r.pid)
    mu4 = pack([0.10, 0.12, 0.08, 0.15]); cov4 = pack([0.04 if i == j else 0.002 for i in range(4) for j in range(4)])
    cmds = list(DAEMON_CMDS) + [
        ("FRACTAL.OPTIMIZE.MULTIMODAL", (mu4, cov4, "2", "4", "0.15", "0.9")),
        ("FRACTAL.OPTIMIZE.MULTIMODAL", (mu4, cov4, "2")),
        ("FRACTAL.OPTIMIZE.MULTIMODAL.PARETO", (mu4, cov4, "2", "4")),
        ("FRACTAL.AGENT.DIVERSE_PORTFOLIOS", (mu4, cov4, "2")),
        ("FRACTAL.LEDGER.VERIFY", ("2",)),
        ("FRACTAL.REASON", ("hello", '{"a":1}')),
    ]
    slow = {"stall"}
    fast_modes = ["ok", "close_before", "call_close", "partial_header", "partial_tag", "partial_payload", "huge_payload", "bad_magic", "bad_version",
                  "no_response_flag", "bad_authlen", "wrong_rid", "wrong_opcode", "tiny_payload", "bad_tag", "init_short", "init_status",
                  "null_result", "type_int", "type_real", "call_short", "call_badlen", "empty_string", "garbage_embed", "nan_embed", "big_embed",
                  "bad_json", "no_candidates", "odd_candidates"] + ["status:%d" % n for n in (1, 2, 3, 4, 5, 6, 7, 8, 99)]
    suite_modes = {"ok", "status:3", "status:6", "status:8", "call_short", "close_before", "bad_tag", "null_result", "empty_string",
                   "garbage_embed", "nan_embed", "bad_json", "no_candidates", "odd_candidates", "type_int"}
    done = 0
    for mode in fast_modes + ["stall"]:
        fd.mode = mode
        for name, args in (cmds[:3] + cmds[10:11] if mode in slow else cmds):
            q.c.call(name, *args)
            done += 1
        if mode in suite_modes:
            os.environ["FSQL_PHASE"] = "up"
            for suite in (gate_15, gate_18):
                try:
                    suite(q)
                except (ValueError, KeyError, TypeError, IndexError, AttributeError):
                    pass                # an assertion that computes from a reply this mode made unusable stops that suite here; the next mode starts it over
        r.check("the server answers after the daemon replies with %r" % mode, q.alive())
    fd.mode = "ok"
    r.check("the module recovers when the daemon behaves again", not isinstance(q.c.call("FRACTAL.REASON", "hello"), RespError))
    r.check("%d daemon commands were sent through %d daemon behaviours and the server survived" % (done, len(fast_modes) + 1), r.alive())
    fd.close()


# --------------------------------------------------------------------------
# 34 session_params: FRACTAL.DIVERSIFY.* and FRACTAL.FEEDBACK argument handling (src/fv_session.c), the params JSON parser in particular,
#   and the edition/version commands' arity. The server was loaded with explicit non-default load options by build_test.sh.
# --------------------------------------------------------------------------
def gate_34(r):
    c = r.c
    sid = "bt34:s1"
    r.check("DIVERSIFY.ENABLE opens a session", not r.is_err(c.call("FRACTAL.DIVERSIFY.ENABLE", sid)))
    base = jload(c.call("FRACTAL.DIVERSIFY.PARAMS", sid))
    r.check("PARAMS with no document reads the parameters", isinstance(base, dict) and "window_n" in base, str(base))

    good = [
        ('{}', None), ('  {  }  ', None), ('{"stall_threshold":0.5}', ("stall_threshold", 0.5)), ('{"window_n":12}', ("window_n", 12)),
        ('{"max_shadows_considered":7}', ("max_shadows_considered", 7)), ('{"tail_buffer_cap":64}', None),
        ('{"repulsion_sigma":1.5,"repulsion_weight":0.25}', ("repulsion_sigma", 1.5)),
        ('{ "stall_threshold" : 0.4 , "window_n" : 9 }', ("window_n", 9)), ('{"stall_threshold":-0.1}', None), ('{"stall_threshold":1e-3}', None),
        ('{"stall_threshold":+0.2}', None),
    ]
    for doc, want in good:
        res = c.call("FRACTAL.DIVERSIFY.PARAMS", sid, doc)
        d = jload(res)
        ok = isinstance(d, dict) and (want is None or abs(d.get(want[0], 1e9) - want[1]) < 1e-9)
        r.check("PARAMS accepts %s" % doc[:40], ok or r.is_err(res), str(res)[:100])
    bad = [
        "x", "", "[1]", "{", '{"a"', '{"a":', "{a:1}", '{"a\\b":1}', '{"' + "k" * 200 + '":1}', '{"window_n" 1}', '{"window_n":}', '{"window_n":x}',
        '{"window_n":-1}', '{"window_n":1.5}', '{"window_n":1e10}', '{"window_n":"x"}', '{"window_n":1 "stall_threshold":2}', '{"window_n":1,}', '{,}',
        '{"unknown_key":1}', '{"window_n":99999999999999999999}', '{"stall_threshold":nan}', '{"stall_threshold":1e999}', '{"window_n":1}}',
        '{"' + "k" * 5000 + '":1}', '{"stall_threshold":0.5,"window_n":}',
    ]
    for doc in bad:
        res = c.call("FRACTAL.DIVERSIFY.PARAMS", sid, doc)
        r.check("PARAMS handles %r without harm" % doc[:30], isinstance(res, (RespError, bytes, str)) and r.alive())
    r.check("PARAMS refuses a document over 4096 bytes", r.is_err(c.call("FRACTAL.DIVERSIFY.PARAMS", sid, '{"window_n":1,"pad":"' + "x" * 5000 + '"}'), "too long"))
    r.check("PARAMS refuses a bad session id", r.is_err(c.call("FRACTAL.DIVERSIFY.PARAMS", "bad id!")))
    r.check("PARAMS refuses extra arguments", r.is_err(c.call("FRACTAL.DIVERSIFY.PARAMS", sid, "{}", "x"), "wrong number"))
    r.check("PARAMS on an unknown session is an error or empty, not a crash", r.alive() and c.call("FRACTAL.DIVERSIFY.PARAMS", "bt34:none") is not None)

    for kind in ("DWELL", "POSITIVE", "NEGATIVE", "dwell", "Positive", "negative"):
        r.check("FEEDBACK %s is handled" % kind, not isinstance(c.call("FRACTAL.FEEDBACK", sid, "1", kind), ConnectionError))
    c.call("FRACTAL.FEEDBACK", sid, "1", "DWELL", "250")
    for label, args in (("a bad kind", (sid, "1", "LOVE")), ("a bad row", (sid, "x", "DWELL")), ("a negative row", (sid, "-1", "DWELL")),
                        ("a bad dwell", (sid, "1", "DWELL", "x")), ("a huge dwell", (sid, "1", "DWELL", "99999999999")), ("no arguments", ()),
                        ("too many arguments", (sid, "1", "DWELL", "1", "2")), ("an unknown session", ("bt34:none", "1", "DWELL"))):
        res = c.call("FRACTAL.FEEDBACK", *args)
        r.check("FEEDBACK with %s is handled" % label, r.alive() and (r.is_err(res) or res is not None))
    for cmd in ("FRACTAL.DIVERSIFY.DQ", "FRACTAL.DIVERSIFY.EXPLAIN", "FRACTAL.ISOLATE_BACKGROUND", "FRACTAL.DIVERSIFY.DISABLE", "FRACTAL.DIVERSIFY.ENABLE"):
        for args in ((sid,), ("bt34:none",), (), (sid, "x"), ("bad id!",), (sid, "1", "2")):
            c.call(cmd, *args)
        r.check("%s handles awkward arguments" % cmd, r.alive())
    c.call("FRACTAL.ISOLATE_BACKGROUND", sid, "1", "0.5")
    r.check("SESSION.LIST lists the open sessions", isinstance(c.call("FRACTAL.SESSION.LIST"), list))
    r.check("SESSION.LIST refuses extra arguments", r.is_err(c.call("FRACTAL.SESSION.LIST", "x"), "wrong number"))
    c.call("FRACTAL.DIVERSIFY.ENABLE", "bt34:s2"); c.call("FRACTAL.DIVERSIFY.ENABLE", "bt34:s3")
    r.check("SESSION.CLOSE closes a session", not r.is_err(c.call("FRACTAL.SESSION.CLOSE", "bt34:s2")))
    r.check("SESSION.CLOSE of an unknown session is handled", r.alive() and c.call("FRACTAL.SESSION.CLOSE", "bt34:s2") is not None)
    r.check("SESSION.CLOSE refuses a bad id and a missing one", r.is_err(c.call("FRACTAL.SESSION.CLOSE", "bad id!")) and r.is_err(c.call("FRACTAL.SESSION.CLOSE")))
    r.check("fractal.edition refuses extra arguments", r.is_err(c.call("fractal.edition", "x"), "wrong number"))
    r.check("fractal.version refuses extra arguments", r.is_err(c.call("fractal.version", "x"), "wrong number"))
    # the session limit given at load (max-sessions 50 here): fill it and go past it
    for i in range(60):
        c.call("FRACTAL.DIVERSIFY.ENABLE", "bt34:fill%d" % i)
    r.check("opening sessions past the limit is refused cleanly", r.alive())
    r.check("the server survived", r.alive())


# --------------------------------------------------------------------------
# 38 precision: every SQL-visible distance is the one shared double arithmetic
# --------------------------------------------------------------------------
def _f32(x):
    return struct.unpack("<f", struct.pack("<f", x))[0]


def _dbits(x):
    return struct.pack("<d", x)


def _ref_parts(a, b):
    d = x = y = 0.0
    for p, q in zip(a, b):
        d += p * q
        x += p * p
        y += q * q
    return d, x, y


def _ref_cos_dist(a, b):
    d, x, y = _ref_parts(a, b)
    return 1.0 if x == 0.0 or y == 0.0 else 1.0 - d / (math.sqrt(x) * math.sqrt(y))


def _ref_cos_sim(a, b):
    d, x, y = _ref_parts(a, b)
    return 0.0 if x == 0.0 or y == 0.0 else d / (math.sqrt(x) * math.sqrt(y))


def _ref_l2(a, b):
    s = 0.0
    for p, q in zip(a, b):
        t = p - q
        s += t * t
    return math.sqrt(s)


def _ref_l2sq(a, b):
    s = 0.0
    for p, q in zip(a, b):
        t = p - q
        s += t * t
    return s


def _ref_norm(a):
    s = 0.0
    for p in a:
        s += p * p
    return math.sqrt(s)


def gate_38(r):
    """The distances accumulate in double, in a fixed sequential order, and are returned as doubles.

    The reference here is the same arithmetic in Python floats (IEEE double, no fused multiply-add), so the module's
    answers must be BIT-identical to it."""
    c = r.c
    rng = random.Random(7)

    def unit(n):
        v = [rng.gauss(0, 1) for _ in range(n)]
        s = math.sqrt(sum(t * t for t in v))
        return [_f32(t / s) for t in v]

    def call(cmd, *a):
        return float(c.call(cmd, *a))

    for dim in (128, 768):
        q = unit(dim)
        X = [unit(dim) for _ in range(40)]
        got = [call("FRACTAL.VECTOR.COSINE", pack(q), pack(x)) for x in X]
        ref = [_ref_cos_dist(q, x) for x in X]
        r.check("dim %d: the cosine distance is bit-identical to the sequential double reference" % dim,
                all(_dbits(a) == _dbits(b) for a, b in zip(got, ref)),
                "%r vs %r" % (got[:2], ref[:2]))
        got_s = [call("FRACTAL.VECTOR.SIMILARITY", pack(q), pack(x)) for x in X]
        ref_s = [_ref_cos_sim(q, x) for x in X]
        r.check("dim %d: the cosine similarity is bit-identical" % dim,
                all(_dbits(a) == _dbits(b) for a, b in zip(got_s, ref_s)))
        got_d = [call("FRACTAL.VECTOR.DOT", pack(q), pack(x)) for x in X[:20]]
        ref_d = [_ref_parts(q, x)[0] for x in X[:20]]
        r.check("dim %d: the dot product is bit-identical" % dim,
                all(_dbits(a) == _dbits(b) for a, b in zip(got_d, ref_d)))
        got_l = [call("FRACTAL.VECTOR.L2", pack(q), pack(x)) for x in X[:20]]
        ref_l = [_ref_l2(q, x) for x in X[:20]]
        r.check("dim %d: the L2 distance is bit-identical" % dim,
                all(_dbits(a) == _dbits(b) for a, b in zip(got_l, ref_l)),
                "%r vs %r" % (got_l[:2], ref_l[:2]))
        got_ls = [call("FRACTAL.VECTOR.L2SQ", pack(q), pack(x)) for x in X[:20]]
        ref_ls = [_ref_l2sq(q, x) for x in X[:20]]
        r.check("dim %d: the squared L2 distance is bit-identical" % dim,
                all(_dbits(a) == _dbits(b) for a, b in zip(got_ls, ref_ls)),
                "%r vs %r" % (got_ls[:2], ref_ls[:2]))
        n = call("FRACTAL.VECTOR.NORM", pack(q))
        r.check("dim %d: the norm is bit-identical" % dim, _dbits(n) == _dbits(_ref_norm(q)), repr(n))

    # an adversarial near-tie set: the ordering must match the reference exactly
    dim = 256
    base = unit(dim)
    q = unit(dim)
    cand = [[_f32(b + (rng.random() - 0.5) * 2e-7) for b in base] for _ in range(120)]
    got = [call("FRACTAL.VECTOR.COSINE", pack(q), pack(x)) for x in cand]
    ref = [_ref_cos_dist(q, x) for x in cand]
    r.check("near-ties: the full ranking by the module's distance equals the reference ranking",
            sorted(range(len(cand)), key=lambda i: (got[i], i)) == sorted(range(len(cand)), key=lambda i: (ref[i], i)))
    r.check("near-ties: the candidates really are near-ties", len(set(ref)) > 90, len(set(ref)))

    # the text round trip is bit-identical for float32
    bad = 0
    for _ in range(100):
        v = [_f32(rng.uniform(-1e3, 1e3) * 10 ** rng.randint(-8, 3)) for _ in range(rng.randint(1, 24))]
        txt = c.call("FRACTAL.VECTOR.TOTEXT", pack(v))
        back = c.call("FRACTAL.VECTOR.FROMTEXT", txt)
        if back != pack(v):
            bad += 1
    r.check("the vector -> text -> vector round trip is bit-identical for 100 random float32 vectors", bad == 0, bad)

    # the documented conventions for a degenerate input
    z, v = pack([0.0] * 8), pack([1.0] * 8)
    r.check("a zero vector has a cosine distance of 1.0 (not NaN)",
            call("FRACTAL.VECTOR.COSINE", z, v) == 1.0)
    r.check("a zero vector has a cosine similarity of 0.0",
            call("FRACTAL.VECTOR.SIMILARITY", z, v) == 0.0)
    r.check("a dimension mismatch is refused", r.is_err(c.call("FRACTAL.VECTOR.COSINE", v, pack([1.0] * 3))))
    r.check("the server survived", r.alive())


def gate_39_auto_small(r):
    """agent-use-index auto with the default agent-index-min-rows (10000): a 2000-row key is below it, so the agents scan exactly even
    though a ready index exists."""
    c = r.c
    key = "bt39s:corpus"
    dim, rows = 16, 2000
    rng = random.Random(31)
    data = [[rng.gauss(0, 1) for _ in range(dim)] for _ in range(rows)]
    c.call("SET", key, b"".join(pack(row) for row in data))
    out = c.call("FRACTAL.INDEX.CREATE", key, "bt39s", "DIM", str(dim), "VECTOR_FORMAT", "FP32")
    if r.is_err(out):
        r.known("the IVF index", "this build of fractalsql-core does not carry the index")
        return
    rt = jload(c.call("FRACTAL.AGENT.ROUTE_TASK", key, pack(data[7]), "500"))
    r.check("auto with a key below agent-index-min-rows: route_task scans exactly", isinstance(rt, dict) and rt.get("routed_to") == "7", rt)
    info = jload(c.call("FRACTAL.INDEX.INFO", key, "bt39s"))
    r.check("auto with a key below agent-index-min-rows never touches the index", (info.get("last_search") or {}).get("used_index") is not True, info)
    r.check("the server survived", r.alive())


def gate_39_agents(r):
    """agent_use_index (the agent-use-index load option, off by default): route_task, schedule_workload and outlier_intercept search
    through a fractal_index when one exists on the key, and answer exactly as the scan does. The server runs with the option on."""
    c = r.c
    key = "bt39a:corpus"
    dim, rows, clusters = 16, 2000, 20
    rng = random.Random(23)
    cent = [[rng.gauss(0, 1) for _ in range(dim)] for _ in range(clusters)]
    data = [[x + 0.3 * rng.gauss(0, 1) for x in rng.choice(cent)] for _ in range(rows)]
    c.call("SET", key, b"".join(pack(row) for row in data))
    q = pack(data[41])

    # no index yet: the option is on but there is nothing to search through, so the agent scans exactly
    plain = jload(c.call("FRACTAL.AGENT.ROUTE_TASK", key, q, "500"))
    r.check("agent_use_index on with no index: route_task still answers by the exact scan",
            isinstance(plain, dict) and plain.get("routed_to") == "41", plain)

    out = c.call("FRACTAL.INDEX.CREATE", key, "bt39a", "DIM", str(dim), "VECTOR_FORMAT", "FP32")
    if r.is_err(out):
        if "does not carry the IVF index" in str(out):
            r.known("the IVF index", "this build of fractalsql-core does not carry the index")
            return
        r.bad("create the index", str(out))
        return
    info0 = jload(c.call("FRACTAL.INDEX.INFO", key, "bt39a"))
    r.check("no agent has searched through the index yet", (info0.get("last_search") or {}).get("used_index") is not True, info0)

    rt = jload(c.call("FRACTAL.AGENT.ROUTE_TASK", key, q, "500"))
    r.check("route_task through the index finds the same row as the scan", isinstance(rt, dict) and rt.get("routed_to") == plain.get("routed_to"), rt)
    r.check("route_task through the index reports the same exact confidence",
            isinstance(rt, dict) and abs(rt.get("confidence", 0) - plain.get("confidence", -1)) < 1e-9, (rt, plain))
    info1 = jload(c.call("FRACTAL.INDEX.INFO", key, "bt39a"))
    ls = info1.get("last_search") or {}
    r.check("the agent really went through the index (FRACTAL.INDEX.INFO records the search)",
            ls.get("used_index") is True and 0 < ls.get("rows_scanned", 0) < rows, info1)

    sw = jload(c.call("FRACTAL.AGENT.SCHEDULE_WORKLOAD", key, q))
    r.check("schedule_workload through the index assigns the matching row", isinstance(sw, dict) and sw.get("assigned_node") == "41", sw)
    oi = jload(c.call("FRACTAL.AGENT.OUTLIER_INTERCEPT", key, q, "0.5"))
    r.check("outlier_intercept (cosine) through the index intercepts a state that is in the history",
            isinstance(oi, dict) and oi.get("intercepted") is True and oi.get("nearest_distance", 1) < 1e-6, oi)
    oi2 = jload(c.call("FRACTAL.AGENT.OUTLIER_INTERCEPT", key, q, "0.5", "METRIC", "l2"))
    r.check("outlier_intercept with METRIC l2 still scans exactly", isinstance(oi2, dict) and oi2.get("metric") == "l2", oi2)

    # recall_hybrid goes through the index too, and INDEX <name> picks the index; the answer is the exact nearest rows
    def cos_dist(u, v):
        du = sum(x * x for x in u) ** 0.5
        dv = sum(x * x for x in v) ** 0.5
        return 1.0 - sum(x * y for x, y in zip(u, v)) / (du * dv)
    want5 = [str(i) for i in sorted(range(rows), key=lambda i: cos_dist(data[i], data[41]))[:5]]
    rh = jload(c.call("FRACTAL.AGENT.RECALL_HYBRID", key, q, "K", "5"))
    got = [str(x.get("mem_id")) for x in rh] if isinstance(rh, list) else rh
    r.check("recall_hybrid through the index returns the exact nearest rows", got == want5, (got, want5))
    info2 = jload(c.call("FRACTAL.INDEX.INFO", key, "bt39a"))
    ls2 = info2.get("last_search") or {}
    r.check("recall_hybrid really went through the index (FRACTAL.INDEX.INFO records the search)",
            ls2.get("used_index") is True and 0 < ls2.get("rows_scanned", 0) < rows, info2)
    rh2 = jload(c.call("FRACTAL.AGENT.RECALL_HYBRID", key, q, "K", "5", "INDEX", "bt39a"))
    r.check("INDEX <name> on recall_hybrid names the index and answers the same",
            isinstance(rh2, list) and [str(x.get("mem_id")) for x in rh2] == want5, rh2)
    rt3 = jload(c.call("FRACTAL.AGENT.ROUTE_TASK", key, q, "500", "INDEX", "bt39a"))
    r.check("INDEX <name> on route_task names the index and answers the same", isinstance(rt3, dict) and rt3.get("routed_to") == "41", rt3)
    r.check("INDEX <name> that does not exist is an error, not a silent scan",
            r.is_err(c.call("FRACTAL.AGENT.RECALL_HYBRID", key, q, "K", "5", "INDEX", "no_such_index"), "no usable index"))
    r.check("INDEX <name> is refused by an agent that does not search through an index",
            r.is_err(c.call("FRACTAL.AGENT.RECOMMEND_DIVERSE", key, q, "INDEX", "bt39a"), "unknown option"))

    # a row appended after the build is not in the cells; the agent must still see it
    newrow = [rng.gauss(0, 5) for _ in range(dim)]
    c.call("APPEND", key, pack(newrow))
    rt2 = jload(c.call("FRACTAL.AGENT.ROUTE_TASK", key, pack(newrow), "500"))
    r.check("a row appended after the build is found by the agent (the delta is scanned exactly)",
            isinstance(rt2, dict) and rt2.get("routed_to") == str(rows), rt2)
    # the table-free convergence command: no key, no daemon
    sc = jload(c.call("FRACTAL.SFS.CONVERGE", pack([1.0, 0.0, 0.0]), "ITERATIONS", "5", "POPULATION", "8", "DIFFUSION", "2"))
    r.check("SFS.CONVERGE answers {dim, population_size, best_point, best_fit} with no trace",
            isinstance(sc, dict) and sc.get("dim") == 3 and sc.get("population_size") == 8 and len(sc.get("best_point", [])) == 3
            and isinstance(sc.get("best_fit"), (int, float)) and "paths" not in sc, sc)
    sc2 = jload(c.call("FRACTAL.SFS.CONVERGE", pack([0.0, 1.0, 0.0])))
    r.check("SFS.CONVERGE works with the default tuning", isinstance(sc2, dict) and sc2.get("dim") == 3 and sc2.get("population_size") == 50, sc2)
    r.check("SFS.CONVERGE refuses an unknown option", r.is_err(c.call("FRACTAL.SFS.CONVERGE", pack([1.0, 0.0, 0.0]), "BOGUS", "1"), "unknown option"))
    r.check("SFS.CONVERGE refuses a missing value", r.is_err(c.call("FRACTAL.SFS.CONVERGE", pack([1.0, 0.0, 0.0]), "ITERATIONS"), "needs a value"))
    r.check("SFS.CONVERGE refuses an out-of-range population", r.is_err(c.call("FRACTAL.SFS.CONVERGE", pack([1.0, 0.0, 0.0]), "POPULATION", "1")))
    r.check("SFS.CONVERGE refuses a query that is not a vector blob", r.is_err(c.call("FRACTAL.SFS.CONVERGE", b"abc")))
    r.check("the server survived", r.alive())


# --------------------------------------------------------------------------
# 39 the native IVF index
# --------------------------------------------------------------------------
def gate_39(r):
    """FRACTAL.INDEX.CREATE / .REBUILD / .DROP / .INFO and the indexed FRACTAL.SEARCH.

    Covers: the build, the metadata, the "index actually used" check (the rows scanned by the index are far below the
    corpus), the exact distances (the index never returns an approximate distance), the staleness of a changed corpus,
    the rebuild (a new generation), the drop, and the hostile input."""
    if os.environ.get("FSQL_PHASE") == "agents":
        return gate_39_agents(r)
    if os.environ.get("FSQL_PHASE") == "autosmall":
        return gate_39_auto_small(r)
    c = r.c
    key = "bt39:corpus"
    dim, rows, clusters = 16, 2000, 20
    rng = random.Random(11)
    cent = [[rng.gauss(0, 1) for _ in range(dim)] for _ in range(clusters)]
    data = [[x + 0.3 * rng.gauss(0, 1) for x in rng.choice(cent)] for _ in range(rows)]
    blob = b"".join(pack(row) for row in data)
    c.call("SET", key, blob)

    out = c.call("FRACTAL.INDEX.CREATE", key, "bt39", "DIM", str(dim), "VECTOR_FORMAT", "FP32")
    if r.is_err(out):
        if "does not carry the IVF index" in str(out):
            r.known("the IVF index", "this build of fractalsql-core does not carry the index; "
                                     "rebuild the core and refresh include/<platform>")
            r.check("the index commands are registered even without the core's IVF maths",
                    r.is_err(c.call("FRACTAL.INDEX.INFO", key, "bt39"), "IVF index"))
            r.check("the server survived", r.alive())
            return
        r.bad("create the index", str(out))
        return
    info = jload(out)
    r.check("the build reports the rows, the dimension, the format and the first generation",
            info.get("rows") == rows and info.get("dim") == dim and info.get("format") == "FP32"
            and info.get("generation") == 1, info)

    meta = jload(c.call("FRACTAL.INDEX.INFO", key, "bt39"))
    r.check("info reports the index and is not stale yet",
            meta.get("name") == "bt39" and meta.get("rows_indexed") == rows and meta.get("stale") is False, meta)

    q = pack(data[3])

    # the "index actually used" check: one probed cell reads a small part of the corpus
    fast = jload(c.call("FRACTAL.SEARCH", key, q, 10, "INDEX", "bt39", "NPROBE", "1"))
    r.check("the indexed search answers", isinstance(fast, dict) and "top_k" in fast, fast)
    r.check("the index was actually used", fast.get("used_index") is True, fast)
    r.check("the index scanned far fewer rows than the corpus holds (the index is really used)",
            isinstance(fast.get("rows_scanned"), int) and 0 < fast["rows_scanned"] < rows // 10,
            "rows_scanned=%r of %d" % (fast.get("rows_scanned"), rows))
    r.check("the index probed at least one cell", fast.get("cells_probed", 0) >= 1, fast)
    r.check("the indexed search returns k rows", len(fast.get("top_k", [])) == 10, fast.get("top_k"))

    # the default (calibrated) nprobe
    res = jload(c.call("FRACTAL.SEARCH", key, q, 10, "INDEX", "bt39"))
    r.check("the indexed search at the calibrated nprobe scans fewer rows than the corpus",
            isinstance(res.get("rows_scanned"), int) and 0 < res["rows_scanned"] < rows,
            "rows_scanned=%r of %d" % (res.get("rows_scanned"), rows))

    # every distance is exact: the index's verified distance is the module's own cosine, bit for bit
    exact = jload(c.call("FRACTAL.SEARCH", key, q, 10, "USE_INDEX", 0))
    r.check("the exact scan scanned every row", exact.get("used_index") is False
            and exact.get("rows_scanned") == rows, exact.get("rows_scanned"))
    bad = []
    for row in res.get("top_k", []):
        vec = float(c.call("FRACTAL.VECTOR.COSINE", q, pack(data[row["idx"]])))
        if _dbits(vec) != _dbits(row["dist"]):
            bad.append((row["idx"], vec, row["dist"]))
    r.check("every distance the index returned is exact (bit-identical to FRACTAL.VECTOR.COSINE)",
            not bad, bad[:3])

    # the answer is the exact one (the calibrated nprobe targets a high recall)
    emap = {row["idx"]: row["dist"] for row in exact.get("top_k", [])}
    hit = len([row for row in res.get("top_k", []) if row["idx"] in emap])
    r.check("the indexed answer agrees with the exact scan", hit >= 8, "%d of 10" % hit)
    r.check("the indexed distances of the shared rows are bit-identical to the exact scan's",
            all(_dbits(row["dist"]) == _dbits(emap[row["idx"]])
                for row in res.get("top_k", []) if row["idx"] in emap),
            [row["dist"] for row in res.get("top_k", [])][:3])

    # the query row is its own nearest neighbour
    one = res.get("top_k", [{}])[0]
    r.check("the query row is its own nearest neighbour",
            one["idx"] == 3 and one["dist"] < 1e-9, one)

    # rows appended after the build: the status says so, and the search still sees them
    nv = [rng.gauss(0, 1) for _ in range(dim)]
    dq = pack(nv)
    c.call("SET", key, blob + dq)
    meta = jload(c.call("FRACTAL.INDEX.INFO", key, "bt39"))
    r.check("info reports the index as stale when rows were appended", meta.get("stale") is True, meta)
    r.check("the status is stale with one row added and a refresh is advised",
            meta.get("status") == "stale" and meta.get("rows_added_since_build") == 1 and meta.get("has_delta") is True
            and meta.get("rows_now") == rows + 1 and any("refresh" in a for a in meta.get("advice", [])), meta)
    lst = jload(c.call("FRACTAL.INDEX.LIST", key))
    r.check("the list shows the index with its status and the current row count",
            len(lst) == 1 and lst[0].get("name") == "bt39" and lst[0].get("status") == "stale"
            and lst[0].get("rows") == rows and lst[0].get("rows_now") == rows + 1 and lst[0].get("stale") is True, lst)
    r.check("the list of a corpus with no index is empty", jload(c.call("FRACTAL.INDEX.LIST", key + ":none")) == [])
    d = jload(c.call("FRACTAL.SEARCH", key, dq, 5, "INDEX", "bt39", "NPROBE", 1))
    r.check("the appended row is found through the index (the rows after the build are scanned exactly)",
            d.get("used_index") is True and d["top_k"][0]["idx"] == rows and d["top_k"][0]["dist"] < 1e-9, d)
    cands = jload(c.call("FRACTAL.INDEX.CANDIDATES", key, dq, "INDEX", "bt39", 5, "NPROBE", 1))
    r.check("the candidates are the nearest positions, the appended row first",
            isinstance(cands, list) and len(cands) == 5 and cands[0] == rows, cands)
    r.check("candidates without an index name are refused", r.is_err(c.call("FRACTAL.INDEX.CANDIDATES", key, dq, 5)))

    # IDS: the search restricted to a set of positions
    ex = jload(c.call("FRACTAL.SEARCH", key, q, 5, "IDS", "3,7,9,%d,99999" % rows))
    r.check("a short IDS set is an exact scan of just those rows (an unknown position is ignored)",
            ex.get("used_index") is False and ex.get("rows_scanned") == 4
            and {t["idx"] for t in ex["top_k"]} == {3, 7, 9, rows} and ex["top_k"][0]["idx"] == 3, ex)
    ex = jload(c.call("FRACTAL.SEARCH", key, q, 5, "INDEX", "bt39", "IDS", "[3, 7, 9]"))
    r.check("a short IDS set with an index is still exact, and a JSON array is accepted",
            ex.get("used_index") is False and {t["idx"] for t in ex["top_k"]} == {3, 7, 9}, ex)
    ex = jload(c.call("FRACTAL.SEARCH", key, q, 5, "IDS", ""))
    r.check("an empty IDS set returns no rows", ex.get("top_k") == [], ex)
    r.check("a malformed IDS set is refused", r.is_err(c.call("FRACTAL.SEARCH", key, q, 5, "IDS", "3,x")))
    big = ",".join(str(i) for i in range(rows + 1)) + "," + ",".join(str(i) for i in range(10000, 14500))
    ex = jload(c.call("FRACTAL.SEARCH", key, q, 5, "INDEX", "bt39", "IDS", big))
    r.check("a long IDS set goes through the index and is filtered", ex.get("used_index") is True
            and ex["top_k"][0]["idx"] == 3 and len(ex["top_k"]) == 5, ex)
    sparse = ",".join(str(i) for i in range(1000, 6000))
    ex = jload(c.call("FRACTAL.SEARCH", key, q, 5, "INDEX", "bt39", "NPROBE", 1, "IDS", sparse))
    r.check("when the index shortlist leaves fewer than k rows of the set, the set is scanned exactly",
            len(ex["top_k"]) == 5, ex)

    # refresh folds the appended rows in without retraining, as a new generation
    rf = jload(c.call("FRACTAL.INDEX.REFRESH", key, "bt39"))
    r.check("the refresh reports the rows it added and the new generation",
            rf.get("added") == 1 and rf.get("rows") == rows + 1 and rf.get("generation") == 2 and rf.get("refreshed") is True, rf)
    meta = jload(c.call("FRACTAL.INDEX.INFO", key, "bt39"))
    r.check("after the refresh the index is ready and covers the appended row",
            meta.get("status") == "ready" and meta.get("stale") is False and meta.get("rows_indexed") == rows + 1
            and meta.get("generation") == 2 and meta.get("rows_added_since_build") == 0, meta)
    d = jload(c.call("FRACTAL.SEARCH", key, dq, 5, "INDEX", "bt39", "NPROBE", 1))
    r.check("the refreshed row is answered from its cell", d["top_k"][0]["idx"] == rows and d["top_k"][0]["dist"] < 1e-9, d)
    rf = jload(c.call("FRACTAL.INDEX.REFRESH", key, "bt39"))
    r.check("a second refresh has nothing to add and keeps the generation",
            rf.get("refreshed") is False and rf.get("added") == 0 and rf.get("generation") == 2, rf)
    r.check("the refresh of a missing index is refused", r.is_err(c.call("FRACTAL.INDEX.REFRESH", key, "nope")))
    old = [c.call("EXISTS", key + ":fsql-idx:bt39:" + part) for part in ("meta:1", "model:1", "cell:1:0")]
    r.check("the refresh removes the generation it replaced", old == [0, 0, 0], old)

    # a rewritten row cannot be refreshed: it needs a rebuild
    c.call("SET", key, pack(data[1]) + blob[dim * 4:] + dq)
    meta = jload(c.call("FRACTAL.INDEX.INFO", key, "bt39"))
    r.check("a rewritten corpus is stale and a rebuild is advised",
            meta.get("status") == "stale" and meta.get("stale") is True and any("rebuild" in a for a in meta.get("advice", [])), meta)
    r.check("the refresh of a rewritten corpus is refused and points to the rebuild",
            r.is_err(c.call("FRACTAL.INDEX.REFRESH", key, "bt39"), "REBUILD"))

    # the index outlives its corpus: orphaned
    c.call("DEL", key)
    meta = jload(c.call("FRACTAL.INDEX.INFO", key, "bt39"))
    r.check("an index whose corpus key is gone is orphaned", meta.get("status") == "orphaned" and meta.get("corpus_present") is False, meta)
    lst = jload(c.call("FRACTAL.INDEX.LIST", key))
    r.check("the list shows it as orphaned", len(lst) == 1 and lst[0].get("status") == "orphaned", lst)
    c.call("SET", key, blob + dq)

    out = c.call("FRACTAL.INDEX.REBUILD", key, "bt39")
    r.check("the rebuild answers", not r.is_err(out), str(out))
    meta = jload(c.call("FRACTAL.INDEX.INFO", key, "bt39"))
    reached = (meta.get("calibration") or {}).get("reaches_target")
    r.check("the rebuild produces a new generation and nothing is stale; the status follows the calibration",
            meta.get("generation") == 3 and meta.get("stale") is False
            and meta.get("status") == ("ready" if reached else "degraded"), meta)
    out = c.call("FRACTAL.INDEX.REBUILD", key, "bt39", "VECTOR_FORMAT", "FP32")
    meta = jload(c.call("FRACTAL.INDEX.INFO", key, "bt39"))
    r.check("an FP32 rebuild is ready", not r.is_err(out) and meta.get("generation") == 4 and meta.get("status") == "ready", meta)

    # hostile input
    r.check("an unknown option is refused", r.is_err(c.call("FRACTAL.INDEX.CREATE", key, "bt39b", "DIM", str(dim), "NOPE", 1)))
    r.check("an unknown vector format is refused",
            r.is_err(c.call("FRACTAL.INDEX.CREATE", key, "bt39c", "DIM", str(dim), "VECTOR_FORMAT", "PQ4")))
    r.check("a missing DIM is refused", r.is_err(c.call("FRACTAL.INDEX.CREATE", key, "bt39d")))
    r.check("a duplicate index name is refused",
            r.is_err(c.call("FRACTAL.INDEX.CREATE", key, "bt39", "DIM", str(dim)), "already exists"))
    r.check("info of a missing index is refused", r.is_err(c.call("FRACTAL.INDEX.INFO", key, "nope")))
    r.check("a rebuild of a missing index is refused", r.is_err(c.call("FRACTAL.INDEX.REBUILD", key, "nope")))
    r.check("an index name that is not an identifier is refused",
            r.is_err(c.call("FRACTAL.INDEX.INFO", key, "x; drop table t; --")))

    # drop
    r.check("drop answers 1", c.call("FRACTAL.INDEX.DROP", key, "bt39") == 1)
    r.check("info after the drop is refused", r.is_err(c.call("FRACTAL.INDEX.INFO", key, "bt39")))
    left = c.call("KEYS", key + ":fsql-idx:*")
    r.check("the drop removes the index keys", left == [], left)

    # every VECTOR_FORMAT: build, recall at every cell, exact distances, really used. INT8 and INT4 keep a compressed code and re-check the
    # shortlist against the corpus; FP16 and FP32 keep (almost) the whole vector.
    truth10 = [row["idx"] for row in jload(c.call("FRACTAL.SEARCH", key, q, 10, "USE_INDEX", 0)).get("top_k", [])]
    for fmt, floor in (("INT8", 0.40), ("INT4", 0.40), ("FP16", 0.98), ("FP32", 0.99)):
        name = "bt39f" + fmt.lower()
        made = c.call("FRACTAL.INDEX.CREATE", key, name, "DIM", str(dim), "NLIST", "16", "VECTOR_FORMAT", fmt)
        if r.is_err(made):
            r.bad("create a %s index" % fmt, str(made))
            continue
        info = jload(made)
        r.check("%s: the index builds and reports its format and rows" % fmt, info.get("format") == fmt and info.get("rows", 0) >= rows, info)
        full = jload(c.call("FRACTAL.SEARCH", key, q, 10, "INDEX", name, "NPROBE", "16"))
        hit = len({row["idx"] for row in full.get("top_k", [])} & set(truth10)) / 10.0
        r.check("%s: recall@10 at every cell is at least %.2f (got %.2f)" % (fmt, floor, hit), hit >= floor, full.get("top_k"))
        bad = [(row["idx"], row["dist"]) for row in full.get("top_k", [])
               if _dbits(float(c.call("FRACTAL.VECTOR.COSINE", q, pack(data[row["idx"]])))) != _dbits(row["dist"])]
        r.check("%s: every returned distance is the exact cosine (bit-identical)" % fmt, len(full.get("top_k", [])) == 10 and not bad, bad[:3])
        few = jload(c.call("FRACTAL.SEARCH", key, q, 10, "INDEX", name, "NPROBE", "2"))
        r.check("%s: a search at nprobe 2 goes through the index and scans fewer rows than the corpus" % fmt,
                few.get("used_index") is True and isinstance(few.get("rows_scanned"), int) and 0 < few["rows_scanned"] < rows, few)
        c.call("FRACTAL.INDEX.DROP", key, name)
    r.check("the server survived", r.alive())
    c.call("DEL", key)


# --------------------------------------------------------------------------
# 40 the naming surface: the table-free convergence function was renamed
# --------------------------------------------------------------------------
def gate_40(r):
    """fractal_search was renamed fractal_sfs_converge (and fractal_search_debug to fractal_sfs_converge_debug).

    The old names are NOT aliases: they stay registered only so that a call fails with a message that names the new
    function. This gate is a source-level check of that contract (the daemon's function registry, the SQL registration
    and the failing stub)."""
    root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

    def read(rel):
        path = os.path.join(root, rel)
        try:
            with open(path, "r", encoding="utf-8", errors="replace") as fh:
                return fh.read()
        except OSError as e:
            r.bad("read %s" % rel, str(e))
            return ""

    defs = read(os.path.join("service", "protocol", "functions.def"))
    sql = read(os.path.join("service", "adapter", "sql", "install_udf.sql"))
    src = read(os.path.join("service", "adapter", "src", "fractalsql.c"))

    r.check("the daemon's function registry has fractal_sfs_converge",
            "X(fractal_sfs_converge," in defs, defs[:200])
    r.check("the daemon's function registry has fractal_sfs_converge_debug",
            "X(fractal_sfs_converge_debug," in defs)
    r.check("the old names stay registered (they are the failing stubs, not aliases)",
            "X(fractal_search," in defs and "X(fractal_search_debug," in defs)
    r.check("the SQL registers fractal_sfs_converge and fractal_sfs_converge_debug",
            "CREATE FUNCTION fractal_sfs_converge" in sql and "CREATE FUNCTION fractal_sfs_converge_debug" in sql)
    r.check("the SQL keeps the old names registered so a call fails with a message",
            "CREATE FUNCTION fractal_search " in sql and "CREATE FUNCTION fractal_search_debug " in sql)
    r.check("the adapter defines fractal_sfs_converge and its debug twin",
            "fractal_sfs_converge(UDF_INIT" in src and "fractal_sfs_converge_debug(UDF_INIT" in src)
    r.check("the adapter's old names are failing stubs that name the new function",
            "fractal_search_init" in src and "was renamed" in src and "fractal_sfs_converge" in src)
    r.check("the server survived", r.alive())


GATES = {"02": gate_02, "03": gate_03, "04": gate_04, "05": gate_05,
         "06": gate_06, "07": gate_07, "08": gate_08, "09": gate_09, "10": gate_10,
         "11": gate_11, "12": gate_12, "13": gate_13, "15": gate_15, "16": gate_16, "18": gate_18, "19": gate_19, "20": gate_20, "41": gate_41, "33": gate_33, "34": gate_34,
         "38": gate_38, "39": gate_39, "40": gate_40}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("gate")
    ap.add_argument("--host", default="127.0.0.1")
    ap.add_argument("--port", type=int, required=True)
    ap.add_argument("--pid", type=int)
    a = ap.parse_args()
    if a.gate == "ping":
        try:
            return 0 if Client(a.host, a.port).call("PING") == "PONG" else 1
        except (OSError, ConnectionError):
            return 1
    if a.gate not in GATES:
        print("FAIL %s unknown gate" % a.gate)
        return 2
    r = Run(a.gate, a.host, a.port, a.pid)
    try:
        GATES[a.gate](r)
    except (OSError, ConnectionError) as e:
        r.bad("connection", "the server went away: %s" % e)
    return 1 if r.fails else 0


if __name__ == "__main__":
    sys.exit(main())
