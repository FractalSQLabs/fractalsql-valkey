#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Fleet logistics: a vehicle taking a detour, and how much of the network a fleet covers.

40 vehicles with route-embedding vectors across four depots. One vehicle's GPS trace
meanders instead of following its route; FRACTAL.DIM.BOXCOUNT scores the trace's
complexity (a straight line is about 1, a meander is higher). A cohort is just
another key, so a route-3-only search is a SEARCH on that key."""
import math
from fvlib import Kit, pack, dist, cosd, flat

k = Kit("fleet-logistics")
rng = k.rng
DEPOTS = [[rng.uniform(-4, 4) for _ in range(8)] for _ in range(4)]
veh = [{"depot": i % 4, "route": (i // 4) % 4} for i in range(40)]
emb = [[x + rng.gauss(0, 0.3) for x in DEPOTS[v["depot"]]] for v in veh]
k.load("kit:fleet", emb)

k.section("1. Cohort search: today's route-3 vehicles only")
cohort = [i for i, v in enumerate(veh) if v["route"] == 3]
k.load("kit:fleet:route3", [emb[i] for i in cohort])
q = [x + rng.gauss(0, 0.1) for x in emb[cohort[2]]]
top = k.js("FRACTAL.SEARCH", "kit:fleet:route3", pack(q), 1)["top_k"][0]
best_local = min(range(len(cohort)), key=lambda j: cosd(emb[cohort[j]], q))     # SEARCH ranks by cosine distance
k.show("nearest route-3 vehicle", cohort[top["idx"]])
k.expect("the cohort search agrees with a brute-force check", top["idx"] == best_local)

k.section("2. Depot coverage")
pop = k.js("FRACTAL.EXPLORE", "kit:fleet", pack(emb[0]), 120)["population"]
pts = [p if isinstance(p, list) else p.get("point", p.get("x")) for p in pop]
depots = {veh[min(range(40), key=lambda i: dist(emb[i], p))]["depot"] for p in pts}
k.show("depots reached by the discovery population", sorted(depots))
k.expect("discovery reaches more than one depot", len(depots) >= 2, str(sorted(depots)))

k.section("3. A detour in the GPS trace")
straight = [(i * 0.1, i * 0.05 + rng.gauss(0, 0.002)) for i in range(300)]
x = y = 0.0
detour = []                                  # a meander that doubles back: no net heading
for i in range(300):
    x += rng.gauss(0, 0.15)
    y += rng.gauss(0, 0.15)
    detour.append((x, y))
ds = float(k.call("FRACTAL.DIM.BOXCOUNT", pack(flat(straight)), 2))
dd = float(k.call("FRACTAL.DIM.BOXCOUNT", pack(flat(detour)), 2))
k.show("box-counting dimension: straight / detouring", "%.3f / %.3f" % (ds, dd))
k.expect("the straight route is close to a line (about 1)", 0.7 < ds < 1.3)
k.expect("the detouring trace is clearly more complex", dd > ds + 0.25, "%.3f vs %.3f" % (dd, ds))
k.done()
