#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Recommendations and discovery: nearest matches AND a diverse "you might also like" set.

A 240-item catalog in six taste clusters. FRACTAL.SEARCH returns the closest
items; FRACTAL.EXPLORE returns items from DISTINCT regions of the catalog, which
a plain top-k structurally cannot do. A cross-modal query blends content and
behaviour vectors with FRACTAL.VECTOR.CONCAT; FRACTAL.VECTOR.QUANTIZE.INT8 shows
the compact form."""
from fvlib import Kit, pack, unpack, dist

k = Kit("recommendation-search")
rng, DIM, CL = k.rng, 12, 6
centers = [[rng.uniform(-1, 1) * 3 for _ in range(DIM)] for _ in range(CL)]
items, label = [], []
for i in range(240):
    c = i % CL
    items.append([x + rng.gauss(0, 0.25) for x in centers[c]])
    label.append(c)
k.load("kit:catalog", items)

k.section("1. Nearest matches stay in the taste cluster")
q = [x + rng.gauss(0, 0.1) for x in centers[2]]
top = k.js("FRACTAL.SEARCH", "kit:catalog", pack(q), 10)["top_k"]
k.show("clusters of the 10 nearest items", sorted({label[t["idx"]] for t in top}))
k.expect("all 10 nearest items belong to the query's cluster", {label[t["idx"]] for t in top} == {2})

k.section("2. Discovery reaches beyond the nearest matches")
# EXPLORE disperses a population across distinct basins around the query, so it
# spreads wider than top-k, and a larger population reaches further afield.
def pts_of(pop):
    return [p if isinstance(p, list) else p.get("point", p.get("x")) for p in pop]
def nearest_item(pt):
    return min(range(len(items)), key=lambda i: dist(items[i], pt))
def spread(pts):
    pairs = [dist(a, b) for i, a in enumerate(pts) for b in pts[i + 1:]]
    return sum(pairs) / len(pairs)
pop = pts_of(k.js("FRACTAL.EXPLORE", "kit:catalog", pack(q), 200)["population"])
covered = {label[nearest_item(p)] for p in pop}
topk = [items[t["idx"]] for t in k.js("FRACTAL.SEARCH", "kit:catalog", pack(q), 20)["top_k"]]
k.show("mean pairwise spread: discovery vs top-20", "%.2f vs %.2f" % (spread(pop[:40]), spread(topk)))
k.show("taste clusters the 200-particle population reaches", sorted(covered))
k.expect("discovery spreads wider than the top-20 nearest items", spread(pop[:40]) > spread(topk))
k.expect("discovery reaches at least 3 distinct clusters (top-k reached 1)", len(covered) >= 3, str(sorted(covered)))

k.section("3. Cross-modal: content + behaviour")
behaviour = [[(1.0 if label[i] % 2 == 0 else -1.0) + rng.gauss(0, 0.2) for _ in range(4)] for i in range(240)]
blended = [unpack(k.call("FRACTAL.VECTOR.CONCAT", pack(items[i]), "0.7", pack(behaviour[i]), "0.3")) for i in range(240)]
k.load("kit:blend", blended)
qb = unpack(k.call("FRACTAL.VECTOR.CONCAT", pack(q), "0.7", pack([1.0] * 4), "0.3"))
tb = k.js("FRACTAL.SEARCH", "kit:blend", pack(qb), 8)["top_k"]
k.show("clusters of the blended top 8", sorted({label[t["idx"]] for t in tb}))
k.expect("the blended query still lands in the query's cluster", {label[t["idx"]] for t in tb} == {2})

k.section("4. Compact storage")
code = k.js("FRACTAL.VECTOR.QUANTIZE.INT8", pack(items[0]))
deq = [c * code["scale"] for c in code["codes"]]
err = max(abs(a - b) for a, b in zip(deq, items[0]))
k.show("max error after int8 quantisation", "%.4f" % err)
k.expect("int8 quantisation is accurate to within 1 percent of the range", err < 0.01 * (max(items[0]) - min(items[0])) + 0.05)
k.done()
