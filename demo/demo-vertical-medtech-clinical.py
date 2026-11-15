#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""MedTech: cohort search over vitals, trend drift, and pre-extracted geometry.

Patient vitals are vectors, so "which patients look like this deteriorating one"
is FRACTAL.SEARCH; a heart-rate series' regime change is FRACTAL.DIM.DRIFT. The
geometry commands take PRE-EXTRACTED vessel graphs, meshes and nerve skeletons,
not raw imaging."""
import math
from fvlib import Kit, pack, pack_u32, flat

k = Kit("medtech-clinical")
rng = k.rng

k.section("1. Patients whose vitals look like the deteriorating one")
# [HR, SBP, SpO2, RR, temp, lactate], scaled to comparable ranges
scale = [1 / 100, 1 / 120, 1 / 100, 1 / 20, 1 / 37, 1.0]
def norm(v): return [a * s for a, s in zip(v, scale)]
patients = [[rng.uniform(60, 85), rng.uniform(105, 130), rng.uniform(96, 99), rng.uniform(12, 18), rng.uniform(36.4, 37.2), rng.uniform(0.6, 1.5)] for _ in range(50)]
sick = [118, 84, 88, 28, 38.9, 4.6]
for i in (7, 31):
    patients[i] = [a * rng.uniform(0.96, 1.04) for a in sick]
k.load("kit:vitals", [norm(p) for p in patients])
top = k.js("FRACTAL.SEARCH", "kit:vitals", pack(norm(sick)), 2)["top_k"]
k.show("two nearest patients", [t["idx"] for t in top])
k.expect("the two patients with the same trajectory are found", {t["idx"] for t in top} == {7, 31})

k.section("2. A heart-rate trend that changed regime")
hr_ok = [72 + rng.gauss(0, 2) for _ in range(256)]
hr_bad = [72 + rng.gauss(0, 2) for _ in range(160)] + [72 + (i * 0.6) + rng.gauss(0, 6) for i in range(96)]
d_bad = abs(k.js("FRACTAL.DIM.DRIFT", pack(hr_bad), 64)["drift"])
d_ok = abs(k.js("FRACTAL.DIM.DRIFT", pack(hr_ok), 64)["drift"])
k.show("|drift| deteriorating / stable", "%.3f / %.3f" % (d_bad, d_ok))
k.expect("the deteriorating trend drifts more than the stable one", d_bad > d_ok)

k.section("3. Pre-extracted geometry")
n = 3000
pts = [(rng.uniform(0, 10), rng.uniform(0, 10), rng.uniform(0, 10)) for _ in range(n)]
edges, arcs = [], []
for i in range(1, n):
    j = rng.randint(0, i - 1)
    edges += [j, i]
    arcs.append(math.dist(pts[i], pts[j]) * 1.05)
v = k.js("FRACTAL.MORPH.VASCULAR", pack(flat(pts)), pack_u32(edges), pack(arcs))
k.show("vessel tree: tortuosity / branch density / dimension",
       "%.3f / %.3f / %.3f" % (v["mean_tortuosity"], v["branch_density"], v["fractal_dimension"]))
k.expect("tortuosity is at least 1 (arc length >= straight line)", v["mean_tortuosity"] >= 1.0)
tet = [(0, 0, 0), (1, 0, 0), (0, 1, 0), (0, 0, 1)]
c = k.js("FRACTAL.MORPH.CORTICAL", pack(flat(tet)), pack_u32([0, 1, 2, 0, 1, 3, 0, 2, 3, 1, 2, 3]))
k.show("convex mesh gyrification index", "%.3f" % c["gyrification_index"])
k.expect("a convex mesh has a gyrification index near 1", 0.9 < c["gyrification_index"] < 1.1)
nodes2 = [(rng.uniform(0, 10), rng.uniform(0, 10)) for _ in range(n)]
e2 = []
for i in range(1, n):
    e2 += [rng.randint(0, i - 1), i]
nv = k.js("FRACTAL.MORPH.NERVE", pack(flat(nodes2)), 2, pack_u32(e2))
k.show("nerve skeleton fiber length density", "%.3f" % nv["fiber_length_density"])
k.expect("the nerve skeleton reports a positive fiber length density", nv["fiber_length_density"] > 0)
k.done()
