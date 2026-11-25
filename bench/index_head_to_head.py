#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""bench/index_head_to_head.py -- the native IVF index against the exact scan, on the same corpus.

Contestants, all on the same corpus key:
  exact     FRACTAL.SEARCH ... USE_INDEX 0 (the brute-force scan, the ground truth)
  index     FRACTAL.SEARCH ... INDEX <name>, for each vector_format, at the calibrated nprobe and at fixed
            nprobe values

For every contestant it reports recall@k against the exact top-k, the median latency per query, the bytes of
index per row, the build seconds and the rows scanned (the "index actually used" check: below the row count).
The distances the index returns are exact: every candidate is verified against the corpus.

    python3 bench/index_head_to_head.py --port 6379
    python3 bench/index_head_to_head.py --port 6379 --n 20000 --dim 128 --queries 40 --formats INT8,INT4,FP16,FP32
"""
import argparse
import json
import os
import random
import statistics
import struct
import sys
import time

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "demo"))
from fvlib import Conn, FvError, pack  # noqa: E402


def f32(x):
    return struct.unpack("<f", struct.pack("<f", x))[0]


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--host", default="127.0.0.1")
    ap.add_argument("--port", type=int, default=6379)
    ap.add_argument("--n", type=int, default=20000)
    ap.add_argument("--dim", type=int, default=128)
    ap.add_argument("--clusters", type=int, default=50)
    ap.add_argument("--queries", type=int, default=40)
    ap.add_argument("--formats", default="INT8,INT4,FP16,FP32")
    ap.add_argument("--nprobes", default="1,4,16,64")
    ap.add_argument("--seed", type=int, default=7)
    ap.add_argument("--k", type=int, default=10)
    a = ap.parse_args()

    c = Conn(a.host, a.port)
    try:
        c.call("FRACTAL.VERSION")
    except FvError:
        sys.exit("the fractalsql module is not loaded in that server")

    rng = random.Random(a.seed)
    dim, n = a.dim, a.n
    cent = [[rng.gauss(0, 1) for _ in range(dim)] for _ in range(a.clusters)]
    rows = []
    for _ in range(n):
        base = rng.choice(cent)
        rows.append([f32(x + 0.3 * rng.gauss(0, 1)) for x in base])
    key = "bench:index:corpus"
    c.call("SET", key, b"".join(pack(r) for r in rows))
    qi = [rng.randrange(n) for _ in range(min(a.queries, n))]
    Q = [pack(rows[i]) for i in qi]
    print("rows %d dim %d queries %d, recall@%d against the exact top-%d" % (n, dim, len(Q), a.k, a.k))

    def run(extra, label):
        lat = []
        res = []
        scanned = 0
        for q in Q:
            t0 = time.perf_counter()
            r = json.loads(c.call("FRACTAL.SEARCH", key, q, a.k, *extra))
            lat.append((time.perf_counter() - t0) * 1000.0)
            res.append([x["idx"] for x in r["top_k"]])
            scanned = r.get("rows_scanned", scanned)
        return res, statistics.median(lat), scanned

    truth, lat_exact, scanned = run(("USE_INDEX", 0), "exact")
    print("%-30s recall %.3f  median %7.2f ms  rows scanned %9d" % ("exact scan", 1.0, lat_exact, scanned))

    def recall(res):
        hit = 0
        for j, r in enumerate(res):
            hit += len(set(truth[j]) & set(r))
        return hit / float(a.k * max(len(res), 1))

    for fmt in [f.strip().upper() for f in a.formats.split(",")]:
        name = "ix_" + fmt
        c.call("FRACTAL.INDEX.DROP", key, name)
        t0 = time.time()
        try:
            info = json.loads(c.call("FRACTAL.INDEX.CREATE", key, name, "DIM", str(dim), "VECTOR_FORMAT", fmt))
        except FvError as e:
            print("%-30s skipped (%s)" % (fmt, e))
            continue
        build = time.time() - t0
        print("%s: nlist %d, %.1f bytes/row, build %.1fs, calibrated nprobe %s"
              % (fmt, info["nlist"], info["bytes"] / max(info["rows"], 1), build, info.get("calibrated_nprobe")))
        for np_ in [int(x) for x in a.nprobes.split(",")]:
            res, lat, scanned = run(("INDEX", name, "NPROBE", str(np_)), fmt)
            print("%-30s recall %.3f  median %7.2f ms  rows scanned %9d" %
                  ("%s nprobe %d" % (fmt, np_), recall(res), lat, scanned))
        res, lat, scanned = run(("INDEX", name), "%s (calibrated nprobe)" % fmt)
        print("%-30s recall %.3f  median %7.2f ms  rows scanned %9d" % (fmt, recall(res), lat, scanned))
        c.call("FRACTAL.INDEX.DROP", key, name)
    c.call("DEL", key)


if __name__ == "__main__":
    main()
