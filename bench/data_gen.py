#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
# SPDX-FileCopyrightText: 2026 Daniel Gardiner d/b/a FractalSQLabs
"""bench/data_gen.py: load a clustered corpus into a Valkey key, for the benchmarks and for trying things by hand.

    python3 bench/data_gen.py --port 6379 --key bench:corpus --n 10000 --dim 128 --clusters 50
    python3 bench/data_gen.py --port 6379 --key bench:corpus --n 10000 --dim 128 --delete

The rows are Gaussian clusters around random unit centres (the same shape head_to_head.py uses), packed as
little-endian float32 and stored under ONE key, which is what FRACTAL.SEARCH and FRACTAL.EXPLORE read. It also
stores the cluster id of every row in a companion hash (`<key>:cluster`, field = row index) so a result can be
scored by how many distinct clusters it covers. The key must be new (or pass --force); the corpus is capped at
256 MiB like the module's own limit. Python standard library only."""
import argparse
import math
import os
import random
import sys

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "demo"))
from fvlib import Conn, FvError, pack  # noqa: E402

MAX_BYTES = 256 * 1024 * 1024


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--host", default="127.0.0.1")
    ap.add_argument("--port", type=int, default=6379)
    ap.add_argument("--key", default="bench:corpus", help="the corpus key (the companion hash is <key>:cluster)")
    ap.add_argument("--n", type=int, default=10000, help="rows")
    ap.add_argument("--dim", type=int, default=128)
    ap.add_argument("--clusters", type=int, default=50)
    ap.add_argument("--spread", type=float, default=0.05, help="standard deviation inside a cluster")
    ap.add_argument("--seed", type=int, default=7)
    ap.add_argument("--force", action="store_true", help="overwrite an existing key")
    ap.add_argument("--delete", action="store_true", help="remove the key and its companion hash, then exit")
    a = ap.parse_args()
    if not (1 <= a.n <= 10_000_000 and 1 <= a.dim <= 4096 and 1 <= a.clusters <= a.n):
        sys.exit("n must be 1..10,000,000, dim 1..4096 and clusters 1..n")
    if not (a.key and len(a.key) <= 256 and all(c.isalnum() or c in "_.:-" for c in a.key)):
        sys.exit("--key may use letters, digits and _ . : - (at most 256 characters)")
    c = Conn(a.host, a.port)
    side = a.key + ":cluster"
    if a.delete:
        c.call("DEL", a.key, side)
        print("deleted %s and %s" % (a.key, side))
        return 0
    if a.n * a.dim * 4 > MAX_BYTES:
        sys.exit("%d x %d float32 is %d MiB, over the module's 256 MiB corpus limit" % (a.n, a.dim, a.n * a.dim * 4 // 2 ** 20))
    if int(c.call("EXISTS", a.key)) and not a.force:
        sys.exit("%s already exists (use --force to overwrite, or --delete)" % a.key)
    rng = random.Random(a.seed)
    centres = []
    for _ in range(a.clusters):
        v = [rng.gauss(0, 1) for _ in range(a.dim)]
        norm = math.sqrt(sum(x * x for x in v)) or 1.0
        centres.append([x / norm for x in v])
    rows, which = [], []
    for i in range(a.n):
        k = i % a.clusters
        rows.extend(x + rng.gauss(0, a.spread) for x in centres[k])
        which.append(k)
    blob = pack(rows)
    c.call("DEL", a.key, side)
    c.call("SET", a.key, blob)
    pipe = []
    for i in range(0, a.n, 500):
        args = ["HSET", side]
        for j in range(i, min(i + 500, a.n)):
            args += [str(j), str(which[j])]
        c.call(*args)
    print("stored %d x %d float32 (%d MiB) at %s, %d clusters; cluster ids in %s" % (a.n, a.dim, len(blob) // 2 ** 20, a.key, a.clusters, side))
    try:
        top = c.call("FRACTAL.SEARCH", a.key, pack(centres[0]), 1)
        print("check: FRACTAL.SEARCH answered (%d bytes)" % len(top))
    except FvError as e:
        print("note: the module is not answering here (%s); the data is stored anyway" % str(e)[:60])
    return 0


if __name__ == "__main__":
    sys.exit(main())
