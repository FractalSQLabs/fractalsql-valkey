#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Document risk drift: clause-level drift detection and compliance review.

Four agreements each have 96 weekly clause-risk reviews; one agreement's amended terms drift into a new
regime (an accelerating risk creep). FRACTAL.DIM.DRIFT finds it and FRACTAL.AGENT.ANOMALY_TRIAGE is the
one-call version. Scout (FRACTAL.EXPLORE) pulls clause variants from across the four clause families so the
reviewer sees the spread rather than near-copies, and FRACTAL.AGENT.RECOMMEND_DIVERSE proposes approved
replacement language."""
from fvlib import Kit, pack, dist, flat, noise, ramp

k = Kit("document-risk-drift")
rng = k.rng
series = [noise(rng, 96, 1.0) for _ in range(4)]
series[2] = ramp(rng, 96, 48, rate=1 / 40.0)
for i, s in enumerate(series):
    k.call("SET", "kit:doc:agr%d" % i, pack(s))

k.section("1. Whose clause risk changed regime?")
drift = [abs(k.js("FRACTAL.DIM.DRIFT", pack(s), 32)["drift"]) for s in series]
k.show("|drift| per agreement", ["%.3f" % d for d in drift])
k.expect("the amended agreement (2) drifts most", max(range(4), key=lambda i: drift[i]) == 2)
tri = k.js("FRACTAL.AGENT.ANOMALY_TRIAGE", "kit:doc:agr2", 32)
k.expect("the one-call triage reports the same score", abs(tri["threat_score"] - k.js("FRACTAL.DIM.DRIFT", pack(series[2]), 32)["drift"]) < 1e-9)

k.section("2. Clause variants across the four families")
FAM = [[rng.uniform(-5, 5) for _ in range(8)] for _ in range(4)]
clauses = [[x + rng.gauss(0, 0.3) for x in FAM[i % 4]] for i in range(24)]
fam = [i % 4 for i in range(24)]
k.load("kit:doc:clauses", clauses)
pop = k.js("FRACTAL.EXPLORE", "kit:doc:clauses", pack(clauses[0]), 120)["population"]
pts = [p if isinstance(p, list) else p.get("point", p.get("x")) for p in pop]
reached = {fam[min(range(24), key=lambda i: dist(clauses[i], p))] for p in pts}
k.show("clause families the discovery run reaches", sorted(reached))
k.expect("discovery spans more than one clause family", len(reached) >= 2, str(sorted(reached)))

k.section("3. Diverse approved replacement language")
rec = k.js("FRACTAL.AGENT.RECOMMEND_DIVERSE", "kit:doc:clauses", pack(clauses[5]), "K", 6)
k.show("replacement clauses", [x["item_id"] for x in rec])
k.expect("six distinct clauses are proposed", len({x["item_id"] for x in rec}) == 6)
k.expect("the closest one is the clause itself", rec[0]["item_id"] == 5 and rec[0]["score"] > 0.999)
k.done()
