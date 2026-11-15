#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Spatial footprint analysis: box-counting dimension and portfolio risk.

Three districts' parcel footprints (a dense grid, a single arterial strip, a two-corridor district) are measured with
FRACTAL.DIM.BOXCOUNT: the lower the dimension, the more linear the frontage and the more a valuation depends on a few corridors.
Six asset classes x 64 months of index values feed FRACTAL.DIM.DFA and FRACTAL.OPTIMIZE.PORTFOLIO (pick 3 classes for the renewal
push; the accelerating one is flagged), and Scout (FRACTAL.EXPLORE) selects comparable parcels from across the market, not
near-twins."""
from fvlib import Kit, pack, dist, flat, noise, line

k = Kit("spatial-footprint-portfolio-risk")
rng = k.rng

k.section("1. Footprint of each district")
grid = [(i * 0.2, j * 0.2) for i in range(25) for j in range(25)]                # a dense raster of parcels
strip = [(i * 0.1, i * 0.05 + rng.gauss(0, 0.002)) for i in range(300)]            # one arterial road
corridors = [(i * 0.1, i * 0.05 + rng.gauss(0, 0.002)) for i in range(150)] + [(i * 0.1, 15.0 - i * 0.05 + rng.gauss(0, 0.002)) for i in range(150)]
dim = {}
for name, pts in (("dense grid", grid), ("single strip", strip), ("two corridors", corridors)):
    dim[name] = float(k.call("FRACTAL.DIM.BOXCOUNT", pack(flat(pts)), 2))
    k.show("%s: box-counting dimension" % name, "%.3f" % dim[name])
k.expect("one arterial strip reads as a line (about 1)", 0.7 < dim["single strip"] < 1.3)
k.expect("a dense parcel grid is more area-filling than the strip", dim["dense grid"] > dim["single strip"] + 0.25, str(dim))

k.section("2. Asset classes: 3 of 6 for the renewal push")
N, ACC = 6, 1
idx = []
for c in range(N):
    s = [100 + 8 * (c == 4) + x for x in noise(rng, 64, 1.5)]
    if c == ACC:
        s = [v + (max(0, i - 20) ** 2) * 0.05 for i, v in enumerate(s)]
    idx.append(s)
alpha = [float(k.call("FRACTAL.DIM.DFA", pack(s))) for s in idx]
k.expect("the accelerating class has the highest exponent", max(range(N), key=lambda i: alpha[i]) == ACC)
mu = [0.08] * N
cov = [[0.04 * alpha[i] * (25 if i == ACC else 1) if i == j else 0.0 for j in range(N)] for i in range(N)]
opt = k.js("FRACTAL.OPTIMIZE.PORTFOLIO", pack(mu), pack([x for r in cov for x in r]), 3, 6)
chosen = {i for i, w in enumerate(opt["weights"]) if w > 0}
k.expect("three classes are chosen, and the accelerating one is flagged instead", len(chosen) == 3 and ACC not in chosen)

k.section("3. Comparable parcels from across the market")
C = [[rng.uniform(-6, 6) for _ in range(8)] for _ in range(4)]
parcels = [[x + rng.gauss(0, 0.35) for x in C[i % 4]] for i in range(32)]
k.load("kit:spatial:parcels", parcels)
pop = k.js("FRACTAL.EXPLORE", "kit:spatial:parcels", pack(parcels[0]), 150)["population"]
pts = [p if isinstance(p, list) else p.get("point", p.get("x")) for p in pop]
seg = {min(range(32), key=lambda i: dist(parcels[i], p)) % 4 for p in pts}
near = {t["idx"] % 4 for t in k.js("FRACTAL.SEARCH", "kit:spatial:parcels", pack(parcels[0]), 6)["top_k"]}
k.show("market segments reached: discovery / six nearest", "%s / %s" % (sorted(seg), sorted(near)))
k.expect("discovery spans more of the market than the six nearest twins", len(seg) >= 3 and len(seg) > len(near))
k.done()
