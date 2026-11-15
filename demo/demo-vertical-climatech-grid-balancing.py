#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Climate tech: renewable and storage generation balancing on a distribution grid.

FRACTAL.DIM.DRIFT over grid-frequency telemetry detects a stability regime change (the volatility-regime recipe from the
quant-finance kit, applied to a grid): the instability is an ACCELERATING oscillation envelope, not a level shift,
because DFA removes a constant offset. FRACTAL.OPTIMIZE.PORTFOLIO then reallocates dispatch across six nodes under a cardinality
constraint, using a ramp-risk matrix in which correlated renewables carry shared weather risk."""
from fvlib import Kit, pack, noise, ramp

k = Kit("climatech-grid-balancing")
rng = k.rng
AREAS = ["north", "south", "east", "west"]
tele = {a: noise(rng, 96, 1.0) for a in AREAS}
tele["east"] = ramp(rng, 96, 48, rate=1 / 40.0)

k.section("1. Which area's stability changed regime?")
score = {}
for a in AREAS:
    k.call("SET", "kit:grid:" + a, pack(tele[a]))
    score[a] = abs(k.js("FRACTAL.AGENT.ANOMALY_TRIAGE", "kit:grid:" + a, 32)["threat_score"])
for a in AREAS:
    k.show("%s |drift|" % a, "%.3f" % score[a])
k.expect("the unstable area (east) scores highest", max(score, key=score.get) == "east")
k.expect("it scores above every stable area", all(score["east"] > score[a] for a in AREAS if a != "east"))

k.section("2. Dispatch 3 of 6 nodes with shared weather risk")
N = 6
mu = [0.10] * N
cov = [[0.0] * N for _ in range(N)]
for i in range(N):
    cov[i][i] = 0.04
for i in range(3):                                       # nodes 0-2 are renewables on the same weather front
    for j in range(3):
        if i != j:
            cov[i][j] = 0.036
opt = k.js("FRACTAL.OPTIMIZE.PORTFOLIO", pack(mu), pack([x for r in cov for x in r]), 3, 21)
chosen = {i for i, w in enumerate(opt["weights"]) if w > 0}
k.show("nodes dispatched", sorted(chosen))
k.expect("three nodes are dispatched", len(chosen) == 3)
k.expect("at most one of the three correlated renewables is among them", len(chosen & {0, 1, 2}) <= 1, str(sorted(chosen)))
ag = k.js("FRACTAL.AGENT.ALLOCATE", pack(mu), pack([x for r in cov for x in r]), 3)
held = {i for i, w in enumerate(ag["allocation"]["weights"]) if w > 0}
k.expect("the one-call agent diversifies away from the shared weather risk too", len(held) == 3 and len(held & {0, 1, 2}) <= 1, str(sorted(held)))
k.done()
