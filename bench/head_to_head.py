#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
# SPDX-FileCopyrightText: 2026 Daniel Gardiner d/b/a FractalSQLabs
"""bench/head_to_head.py: valkey-search HNSW vs FractalSQL Scout (FRACTAL.EXPLORE).

The same HNSW-vs-Scout benchmark the SQL editions run.
Same question, same dataset shape: N points in Gaussian clusters, then for a few
queries, how long does each method take and how many distinct clusters does its
answer cover?

  - valkey-search: an HNSW index (FT.CREATE ... VECTOR HNSW), FT.SEARCH KNN top-K.
    It is built for fast nearest-neighbour lookup and, on clustered data, tends to
    return neighbours from the query's own cluster.
  - FractalSQL Scout: FRACTAL.EXPLORE over a plain string key. It scans the corpus
    on every call (O(N x dim)) and disperses a population across distinct basins.

They solve different problems; this reports the trade-off, it does not declare a
winner. Needs a server with BOTH modules loaded, which the bundle image provides:

    docker compose --profile search up -d            # valkey-bundle + fractalsql.so
    python3 bench/head_to_head.py --port 16380

Requires numpy. The default size is small on purpose (a shared box can be stalled by a
large scan); raise --n deliberately.
"""
import argparse
import os
import sys
import time

try:
    import numpy as np
except ImportError:
    sys.exit("numpy is required: pip install numpy")

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "demo"))
from fvlib import Conn, FvError  # noqa: E402

MAX_N, MAX_DIM = 200000, 2048


def positive_int(name, v, hi):
    v = int(v)
    if not 1 <= v <= hi:
        sys.exit("%s must be in [1, %d]" % (name, hi))
    return v


def gen(n, dim, clusters, sigma, seed):
    rng = np.random.default_rng(seed)
    centers = rng.normal(size=(clusters, dim)).astype(np.float32)
    centers /= np.linalg.norm(centers, axis=1, keepdims=True)
    label = rng.integers(0, clusters, n)
    x = centers[label] + rng.normal(scale=sigma, size=(n, dim)).astype(np.float32)
    return x.astype(np.float32), label, rng


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--host", default="127.0.0.1")
    ap.add_argument("--port", type=int, default=16380)
    ap.add_argument("--n", default="10000", help="stored vectors (default 10000)")
    ap.add_argument("--dim", default="128")
    ap.add_argument("--clusters", default="50")
    ap.add_argument("--sigma", type=float, default=0.05, help="intra-cluster std")
    ap.add_argument("--n-queries", default="5")
    ap.add_argument("--top-k", default="50", help="results per method, also the Scout population")
    ap.add_argument("--ef-runtime", default="40", help="HNSW search quality")
    ap.add_argument("--seed", type=int, default=7)
    a = ap.parse_args()

    n = positive_int("--n", a.n, MAX_N)
    dim = positive_int("--dim", a.dim, MAX_DIM)
    clusters = positive_int("--clusters", a.clusters, 10000)
    nq = positive_int("--n-queries", a.n_queries, 1000)
    topk = positive_int("--top-k", a.top_k, 10000)
    ef = positive_int("--ef-runtime", a.ef_runtime, 100000)

    c = Conn(a.host, a.port)
    mods = {m[1].decode().lower() if isinstance(m[1], bytes) else str(m[1]).lower()
            for m in (c.call("MODULE", "LIST") or [])}
    missing = [m for m in ("fractalsql", "search") if m not in mods]
    if missing:
        sys.exit("module(s) not loaded on %s:%d: %s (use the `search` compose profile)"
                 % (a.host, a.port, ", ".join(missing)))

    print("Generating %d points, %d clusters, dim=%d ..." % (n, clusters, dim))
    x, label, rng = gen(n, dim, clusters, a.sigma, a.seed)

    corpus_key, idx, prefix = "hh:corpus", "hh_idx", "hh:doc:"
    # ---- load both representations
    c.call("DEL", corpus_key)
    c.call("SET", corpus_key, x.tobytes())
    try:
        c.call("FT.DROPINDEX", idx)
    except FvError:
        pass
    c.call("FT.CREATE", idx, "ON", "HASH", "PREFIX", "1", prefix, "SCHEMA", "v", "VECTOR", "HNSW", "6",
           "TYPE", "FLOAT32", "DIM", str(dim), "DISTANCE_METRIC", "COSINE")
    t0 = time.perf_counter()
    for i in range(n):
        c.call("HSET", "%s%d" % (prefix, i), "v", x[i].tobytes())
    print("loaded in %.1f s (index builds as rows arrive)" % (time.perf_counter() - t0))
    # let HNSW finish ingesting before timing queries
    for _ in range(600):
        info = c.call("FT.INFO", idx)
        flat = {(info[i].decode() if isinstance(info[i], bytes) else info[i]): info[i + 1] for i in range(0, len(info) - 1, 2)}
        if int(flat.get("num_docs", 0)) >= n and str(flat.get("backfill_in_progress", "0")) in ("0", "b'0'"):
            break
        time.sleep(0.2)

    qi = rng.integers(0, n, nq)
    qs = x[qi] + rng.normal(scale=a.sigma / 2, size=(nq, dim)).astype(np.float32)

    print("\nBenchmark: %d stored vectors, %d clusters, dim=%d" % (n, clusters, dim))
    print("  valkey-search: HNSW, ef_runtime=%d, KNN %d (cosine)" % (ef, topk))
    print("  Scout: FRACTAL.EXPLORE population=%d (scans the corpus key on every call)\n" % topk)
    print("%3s %-12s | %10s %12s | %10s %12s" % ("qi", "anchor", "HNSW ms", "HNSW recall", "Scout ms", "Scout recall"))
    print("-" * 70)

    hn_t, hn_r, sc_t, sc_r = [], [], [], []
    for i in range(nq):
        qb = qs[i].tobytes()
        t0 = time.perf_counter()
        res = c.call("FT.SEARCH", idx, "*=>[KNN %d @v $q EF_RUNTIME %d]" % (topk, ef),
                     "PARAMS", "2", "q", qb, "NOCONTENT", "DIALECT", "2")
        hn = (time.perf_counter() - t0) * 1000
        ids = [int(k.decode().rsplit(":", 1)[1]) for k in res[1:] if isinstance(k, bytes)]
        hn_c = len({int(label[j]) for j in ids})

        t0 = time.perf_counter()
        import json
        pop = json.loads(c.call("FRACTAL.EXPLORE", corpus_key, qb, str(topk)))["population"]
        sc = (time.perf_counter() - t0) * 1000
        pts = np.array([p if isinstance(p, list) else p.get("point", p.get("x")) for p in pop], dtype=np.float32)
        # each particle is a point in space; credit it to the stored row nearest to it
        d2 = (pts ** 2).sum(1)[:, None] - 2 * pts @ x.T + (x ** 2).sum(1)[None, :]
        sc_c = len({int(label[j]) for j in d2.argmin(1)})

        hn_t.append(hn); hn_r.append(hn_c); sc_t.append(sc); sc_r.append(sc_c)
        print("%3d cluster %3d | %10.1f %8d / %-3d | %10.1f %8d / %-3d" %
              (i, label[qi[i]], hn, hn_c, topk, sc, sc_c, topk))

    m = lambda v: sum(v) / len(v)
    print("\nAverages over %d queries:" % nq)
    print("  valkey-search HNSW: %8.1f ms   clusters %.1f / %d" % (m(hn_t), m(hn_r), topk))
    print("  Scout:              %8.1f ms   clusters %.1f / %d" % (m(sc_t), m(sc_r), topk))
    if m(hn_t) > 0 and m(hn_r) > 0:
        print("  Scout is %.1fx %s and discovers %.1fx more distinct clusters"
              % (m(sc_t) / m(hn_t), "slower" if m(sc_t) >= m(hn_t) else "faster", m(sc_r) / m(hn_r)))

    # clean up exactly what this run created
    c.call("FT.DROPINDEX", idx)
    c.call("DEL", corpus_key)
    for s in range(0, n, 1000):
        c.call("DEL", *["%s%d" % (prefix, j) for j in range(s, min(n, s + 1000))])
    return 0


if __name__ == "__main__":
    sys.exit(main())
