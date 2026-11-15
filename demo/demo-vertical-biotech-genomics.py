#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Biotech and genomics: does a point cloud have cluster structure?

A synthetic stand-in for a low-dimensional embedding of single-cell data: two cell
populations 6 units apart. FRACTAL.TDA.PERSISTENCE answers "how many clusters"
with a persistence diagram: within-cluster merges happen early, the single merge
between the clusters happens late. FRACTAL.VECTOR.LP compares profiles at
different p (L1 for robust feature comparison, L2 for geometry)."""
import statistics
from fvlib import Kit, pack, unpack, flat

k = Kit("biotech-genomics")
rng = k.rng

def cloud(centers, n_each, sd=0.4):
    return [[c[0] + rng.gauss(0, sd), c[1] + rng.gauss(0, sd)] for c in centers for _ in range(n_each)]

def merge_ratio(points):
    r = k.js("FRACTAL.TDA.PERSISTENCE", pack(flat(points)), 2, 0, 12.0, 512)
    deaths = sorted(b["death"] for b in r["h0_bars"])
    return r, deaths[-1] / statistics.median(deaths)

k.section("1. One population versus two")
one, r1 = merge_ratio(cloud([(0, 0)], 60))
two, r2 = merge_ratio(cloud([(0, 0), (6, 0)], 30))
k.show("H0 bars (one / two populations)", "%d / %d" % (one["n_h0_bars"], two["n_h0_bars"]))
k.show("latest merge vs median merge (one / two)", "%.1fx / %.1fx" % (r1, r2))
k.expect("every cell but one merges into a cluster (n-1 bars)", one["n_h0_bars"] == 59 and two["n_h0_bars"] == 59)
k.expect("two populations show one very late merge; one population does not", r2 > 3 * r1, "%.1f vs %.1f" % (r2, r1))
k.expect("the late merge is about the gap between the populations", 3.0 < max(b["death"] for b in two["h0_bars"]) < 7.5)

k.section("2. Expression profiles at different p")
a = [rng.uniform(0, 5) for _ in range(32)]
b = [x + rng.gauss(0, 0.5) for x in a]
d1 = float(k.call("FRACTAL.VECTOR.LP", pack(a), pack(b), "1"))
d2 = float(k.call("FRACTAL.VECTOR.LP", pack(a), pack(b), "2"))
want1 = sum(abs(x - y) for x, y in zip(unpack(pack(a)), unpack(pack(b))))
k.show("L1 / L2 distance", "%.3f / %.3f" % (d1, d2))
k.expect("L1 matches the sum of absolute differences", abs(d1 - want1) < 1e-3 * want1)
k.expect("L1 >= L2 for the same pair", d1 >= d2)
k.expect("the L2 route agrees with FRACTAL.VECTOR.L2", abs(d2 - float(k.call("FRACTAL.VECTOR.L2", pack(a), pack(b)))) < 1e-4)
k.done()
