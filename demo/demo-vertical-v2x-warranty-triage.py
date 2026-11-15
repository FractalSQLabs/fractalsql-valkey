#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Connected-vehicle (V2X) diagnostics: route a trouble-code event, recall similar warranty claims.

FRACTAL.AGENT.ROUTE_TASK matches an incoming telemetry event to the best-capable repair workflow under a token budget.
FRACTAL.AGENT.RECALL_HYBRID fuses a strict metadata filter (component) with drift-vector search to recall similar historical
claims. A two-predicate cohort (model year AND component) is a companion hash whose value is the combined predicate."""
from fvlib import Kit, pack, cosd, unit

k = Kit("v2x-warranty-triage")
rng = k.rng

k.section("1. Route the incoming event")
WF = ["battery-thermal", "brake-pressure", "adas-camera", "infotainment", "powertrain"]
caps = [unit([rng.uniform(-1, 1) for _ in range(6)]) for _ in WF]
k.load("kit:v2x:workflows", caps)
event = [x + rng.gauss(0, 0.03) for x in caps[2]]
r = k.js("FRACTAL.AGENT.ROUTE_TASK", "kit:v2x:workflows", pack(event), 800, "COST_PER_ROUTE", 120)
k.show("workflow / tokens left", "%s / %d" % (WF[int(r["routed_to"])], r["remaining_budget"]))
k.expect("the event goes to the adas-camera workflow (brute-force nearest)", r["routed_to"] == "2" == str(min(range(5), key=lambda i: cosd(caps[i], event))))
k.expect("the routing spends 120 tokens", r["remaining_budget"] == 680)

k.section("2. Recall similar historical claims")
COMP = ["battery", "brake", "camera"]
claims = [unit([rng.uniform(-1, 1) for _ in range(6)]) for _ in range(45)]
for i in range(45):
    k.call("HSET", "kit:v2x:comp", str(i), COMP[i % 3])
    k.call("HSET", "kit:v2x:yearcomp", str(i), "%d|%s" % (2022 + i % 2, COMP[i % 3]))
    k.call("HSET", "kit:v2x:id", str(i), "CLM-%03d" % i)
    k.call("HSET", "kit:v2x:note", str(i), "claim %d: %s" % (i, COMP[i % 3]))
k.load("kit:v2x:claims", claims)
q = [x + rng.gauss(0, 0.02) for x in claims[9]]            # claim 9: a 2023 battery claim
one = k.js("FRACTAL.AGENT.RECALL_HYBRID", "kit:v2x:claims", pack(q), "K", 4, "IDS", "kit:v2x:id", "CONTENT", "kit:v2x:note", "FILTER", "kit:v2x:comp", "brake")
two = k.js("FRACTAL.AGENT.RECALL_HYBRID", "kit:v2x:claims", pack(q), "K", 4, "IDS", "kit:v2x:id", "FILTER", "kit:v2x:yearcomp", "2023|battery")
ids1 = [int(x["mem_id"][4:]) for x in one]
ids2 = [int(x["mem_id"][4:]) for x in two]
k.show("component = brake / year AND component", "%s / %s" % (ids1, ids2))
k.expect("the single-predicate cohort holds only brake claims", all(i % 3 == 1 for i in ids1))
k.expect("the two-predicate cohort holds only 2023 battery claims", all(i % 3 == 0 and i % 2 == 1 for i in ids2))
k.expect("claim 9 itself is the best match in its own cohort", ids2[0] == 9)
k.done()
