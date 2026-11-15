#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
# SPDX-FileCopyrightText: 2026 Daniel Gardiner d/b/a FractalSQLabs
"""bench/bench.py: what FractalSQL for Valkey costs, and what it does NOT cost your other clients.

Measures, against a server that has the module loaded:

  1. latency of FRACTAL.SEARCH and FRACTAL.EXPLORE by corpus size, next to a plain PING
     (the round trip itself) so the work is visible apart from the network;
  2. whether other clients are held up by a long search: PING latency while an EXPLORE runs;
  3. throughput with several clients searching at once.

It reports; it does not pass or fail. Numbers depend on your machine, so none are quoted in
the docs. Python standard library only.

    python3 bench/bench.py --port 6379
    python3 bench/bench.py --port 6379 --sizes 1000x64,20000x64 --repeat 30
"""
import argparse
import os
import random
import socket
import statistics
import struct
import sys
import threading
import time

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "demo"))
from fvlib import Conn, FvError, pack  # noqa: E402


def pct(xs, p):
    xs = sorted(xs)
    return xs[min(len(xs) - 1, int(round(p / 100.0 * (len(xs) - 1))))]


def timed(fn, n):
    out = []
    for _ in range(n):
        t = time.perf_counter()
        fn()
        out.append((time.perf_counter() - t) * 1000.0)
    return out


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--host", default="127.0.0.1")
    ap.add_argument("--port", type=int, default=6379)
    ap.add_argument("--sizes", default="1000x64,10000x64,50000x64", help="comma list of ROWSxDIM")
    ap.add_argument("--repeat", type=int, default=20)
    ap.add_argument("--clients", type=int, default=8)
    a = ap.parse_args()

    c = Conn(a.host, a.port)
    try:
        ver = c.call("FRACTAL.VERSION")
    except FvError:
        sys.exit("the fractalsql module is not loaded in that server")
    print("FractalSQL for Valkey %s on %s:%d\n" % (ver, a.host, a.port))
    rng = random.Random(1)

    print("%-14s %-10s %9s %9s %9s" % ("corpus", "command", "p50 ms", "p95 ms", "max ms"))
    ping = timed(lambda: c.call("PING"), 200)
    print("%-14s %-10s %9.3f %9.3f %9.3f" % ("-", "PING", pct(ping, 50), pct(ping, 95), max(ping)))
    last = None
    for spec in a.sizes.split(","):
        rows, dim = (int(x) for x in spec.lower().split("x"))
        data = pack([rng.random() for _ in range(rows * dim)])
        c.call("SET", "bench:corpus", data)
        queries = [pack([rng.random() for _ in range(dim)]) for _ in range(a.repeat)]
        it = iter(queries * 4)
        for name, fn in (("SEARCH", lambda: c.call("FRACTAL.SEARCH", "bench:corpus", next(it), 10)),
                         ("EXPLORE", lambda: c.call("FRACTAL.EXPLORE", "bench:corpus", next(it), 50))):
            ts = timed(fn, a.repeat)
            print("%-14s %-10s %9.2f %9.2f %9.2f" % ("%dx%d" % (rows, dim), name, pct(ts, 50), pct(ts, 95), max(ts)))
        last = (rows, dim, queries[0])

    rows, dim, q = last
    print("\nWhile a long EXPLORE runs on %dx%d, how long does a PING from another client take?" % (rows, dim))
    other = Conn(a.host, a.port)
    done = threading.Event()
    t0 = time.perf_counter()

    def heavy():
        Conn(a.host, a.port).call("FRACTAL.EXPLORE", "bench:corpus", q, 3000)
        done.set()
    threading.Thread(target=heavy).start()
    time.sleep(0.05)
    lat = []
    while not done.is_set():
        t = time.perf_counter()
        other.call("PING")
        lat.append((time.perf_counter() - t) * 1000.0)
        time.sleep(0.005)
    dur = time.perf_counter() - t0
    if lat:
        print("  EXPLORE took %.2f s; %d pings meanwhile: p50 %.2f ms, max %.2f ms" % (dur, len(lat), pct(lat, 50), max(lat)))
    else:
        print("  EXPLORE finished in %.2f s, too fast to sample; raise --sizes" % dur)

    print("\nThroughput: %d clients, SEARCH K=10 on %dx%d" % (a.clients, rows, dim))
    count = [0]
    lock = threading.Lock()

    def worker():
        w = Conn(a.host, a.port)
        for _ in range(a.repeat):
            w.call("FRACTAL.SEARCH", "bench:corpus", q, 10)
            with lock:
                count[0] += 1
    t = time.perf_counter()
    ths = [threading.Thread(target=worker) for _ in range(a.clients)]
    [x.start() for x in ths]
    [x.join() for x in ths]
    el = time.perf_counter() - t
    print("  %d searches in %.2f s = %.0f searches/s" % (count[0], el, count[0] / el))
    c.call("DEL", "bench:corpus")


if __name__ == "__main__":
    main()
