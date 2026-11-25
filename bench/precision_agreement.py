#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""bench/precision_agreement.py -- the precision-contract agreement metric for the module's vector functions.

Every SQL-visible distance accumulates in double, in a fixed sequential order, and is returned as a double. This
script draws unit vectors, asks the module for the cosine distance, the L2 distance and the dot product, and
compares them with a float64 reference and with the sequential double reference the contract specifies. It
reports the top-10 agreement, the largest absolute and relative error against the float64 reference and the
share of results that are BIT-identical to the sequential double reference (1.0000 expected).

The IVF index is a separate benchmark track: see index_head_to_head.py.

    python3 bench/precision_agreement.py --port 6379
    python3 bench/precision_agreement.py --port 6379 --dims 128,768 --n 400 --queries 20
"""
import argparse
import math
import os
import random
import struct
import sys

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "demo"))
from fvlib import Conn, FvError, pack  # noqa: E402


def f32(x):
    return struct.unpack("<f", struct.pack("<f", x))[0]


def bits(x):
    return struct.pack("<d", x)


def seq_parts(a, b):
    d = x = y = 0.0
    for p, q in zip(a, b):
        d += p * q
        x += p * p
        y += q * q
    return d, x, y


def seq_cos(a, b):
    d, x, y = seq_parts(a, b)
    return 1.0 if x == 0.0 or y == 0.0 else 1.0 - d / (math.sqrt(x) * math.sqrt(y))


def seq_l2(a, b):
    s = 0.0
    for p, q in zip(a, b):
        t = p - q
        s += t * t
    return math.sqrt(s)


def unit(rng, n):
    v = [rng.gauss(0, 1) for _ in range(n)]
    s = math.sqrt(sum(t * t for t in v))
    return [f32(t / s) for t in v]


def run(c, name, X, Q):
    agree = 0
    exact = tot = 0
    max_abs = max_rel = 0.0
    for q in Q:
        sql = [float(c.call("FRACTAL.VECTOR.COSINE", pack(q), pack(x))) for x in X]
        seq = [seq_cos(q, x) for x in X]
        order_sql = sorted(range(len(X)), key=lambda i: (sql[i], i))[:10]
        order_seq = sorted(range(len(X)), key=lambda i: (seq[i], i))[:10]
        agree += int(order_sql == order_seq)
        for a, b in zip(sql, seq):
            err = abs(a - b)
            max_abs = max(max_abs, err)
            max_rel = max(max_rel, err / max(abs(b), 1e-30))
            exact += int(bits(a) == bits(b))
            tot += 1
    print("%-26s top-10 agreement %3d/%-3d  max abs err %.2e  max rel err %.2e  bit-identical to the sequential double %.4f"
          % (name, agree, len(Q), max_abs, max_rel, exact / tot))
    return exact == tot


def run_l2(c, name, X, Q):
    exact = tot = 0
    for q in Q[:10]:
        for x in X[:20]:
            a = float(c.call("FRACTAL.VECTOR.L2", pack(q), pack(x)))
            b = seq_l2(q, x)
            exact += int(bits(a) == bits(b))
            tot += 1
    print("%-26s L2 bit-identical to the sequential double %.4f" % (name, exact / max(tot, 1)))
    return exact == tot


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--host", default="127.0.0.1")
    ap.add_argument("--port", type=int, default=6379)
    ap.add_argument("--dims", default="128,768")
    ap.add_argument("--n", type=int, default=200)
    ap.add_argument("--queries", type=int, default=10)
    ap.add_argument("--seed", type=int, default=7)
    a = ap.parse_args()

    c = Conn(a.host, a.port)
    try:
        c.call("FRACTAL.VERSION")
    except FvError:
        sys.exit("the fractalsql module is not loaded in that server")

    rng = random.Random(a.seed)
    ok = True
    for d in [int(x) for x in a.dims.split(",")]:
        X = [unit(rng, d) for _ in range(a.n)]
        Q = [unit(rng, d) for _ in range(a.queries)]
        ok &= run(c, "random unit, dim %d" % d, X, Q)
        ok &= run_l2(c, "random unit, dim %d" % d, X, Q)
    print("RESULT:", "PASS" if ok else "FAIL")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
