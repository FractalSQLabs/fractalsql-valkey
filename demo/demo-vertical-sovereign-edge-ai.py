#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Sovereign and edge AI: placement and allocation with no external service.

Everything here runs inside the Valkey process in pure C: no vector database, no
cloud call. A fleet of 50 edge nodes: find the best node for a workload
(FRACTAL.SEARCH), then use FRACTAL.OPTIMIZE.PORTFOLIO as a general on-device
resource allocator. The optional rationale step uses FRACTAL.REASON and is
skipped, not failed, when no fractalsqld daemon is configured."""
from fvlib import Kit, FvError, pack, cosd, flat

k = Kit("sovereign-edge-ai")
rng = k.rng

k.section("1. The best node for a workload")
nodes = [[rng.uniform(0, 1) for _ in range(6)] for _ in range(50)]     # cpu, mem, gpu, net, power, latency-headroom
k.load("kit:nodes", nodes)
need = [0.8, 0.6, 0.9, 0.5, 0.4, 0.7]
top = k.js("FRACTAL.SEARCH", "kit:nodes", pack(need), 3)["top_k"]
best = min(range(50), key=lambda i: cosd(nodes[i], need))     # SEARCH ranks by cosine distance
k.show("three best nodes", [t["idx"] for t in top])
k.expect("the best node agrees with a brute-force check", top[0]["idx"] == best)

k.section("2. A resource allocator: choose 5 of 20 candidates")
n, K = 20, 5
yld = [rng.uniform(0.02, 0.2) for _ in range(n)]
risk = [[(rng.uniform(0.01, 0.03) ** 2 if i == j else rng.uniform(-0.0002, 0.0004)) for j in range(n)] for i in range(n)]
for i in range(n):
    for j in range(i):
        risk[i][j] = risk[j][i]
res = k.js("FRACTAL.OPTIMIZE.PORTFOLIO", pack(yld), pack(flat(risk)), K, 3)
held = [i for i, w in enumerate(res["weights"]) if w > 1e-9]
k.show("allocation", {i: round(res["weights"][i], 3) for i in held})
k.expect("at most %d resources are used" % K, len(held) <= K)
k.expect("the allocation uses the whole budget (weights sum to 1)", abs(sum(res["weights"]) - 1) < 1e-6)

k.section("3. Footprint complexity")
fp = [(rng.gauss(0, 3), rng.gauss(0, 1)) for _ in range(300)]
d = float(k.call("FRACTAL.DIM.BOXCOUNT", pack(flat(fp)), 2))
k.show("box-counting dimension of the deployment footprint", "%.3f" % d)
k.expect("the footprint dimension is a plausible 1..2 value", 0.8 < d < 2.2)

k.section("4. Optional: a rationale from a local model")
try:
    text = k.call("FRACTAL.REASON", "In one sentence, why place a GPU workload on the node with the most GPU headroom?")
    k.show("reasoning", text[:80].decode("utf-8", "replace"))
    k.expect("the reasoning tier answered", len(text) > 0)
except FvError as e:
    k.show("skipped", str(e)[:70])
k.done()
