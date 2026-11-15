#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Staged pipeline: velocity bottlenecks and stalled-item triage.

Thirty-six open items across segments and stages; three tier-1 items in the review stage show the same stall signature
(time-in-stage far above the healthy velocity, falling engagement, a silent owner, deepening concessions).
FRACTAL.AGENT.PATIENT_DETERIORATION_TRIAGE is reused as-is (a cohort search plus a baseline-to-current drift, with item-health
columns in place of clinical vitals): an item's stage-entry telemetry plays the patient's admission baseline. The
cohort, tier-1 items in the review stage, is a companion hash selected with FILTER."""
from fvlib import Kit, pack, cosd, unit

k = Kit("stalled-process-triage")
rng = k.rng
HEALTHY = unit([0.2, 1.0, 1.0, 0.1])               # time-in-stage, engagement, owner activity, concessions
STALL = unit([1.0, 0.1, 0.05, 1.0])
STALLED = {5, 14, 29}
items = []
for i in range(36):
    base = STALL if i in STALLED else HEALTHY
    items.append([x + rng.gauss(0, 0.04) for x in base])
k.load("kit:stall:items", items)
for i in range(36):
    k.call("HSET", "kit:stall:id", str(i), "I-%02d" % i)
    k.call("HSET", "kit:stall:cohort", str(i), "tier1-review" if i % 3 == 2 else "other")   # plain filter picks the cohort
cohort = [i for i in range(36) if i % 3 == 2]
k.expect("the three stalled items are all in the tier-1 review cohort", STALLED <= set(cohort), str(cohort))

k.section("1. Cohort-restricted search")
q = [x + rng.gauss(0, 0.02) for x in STALL]
base = items[11]                                   # a healthy item's stage-entry telemetry
cur = [x + 0.6 * s for x, s in zip(base, STALL)]   # it is moving toward the stall signature
r = k.js("FRACTAL.AGENT.PATIENT_DETERIORATION_TRIAGE", "kit:stall:items", pack(q), pack(base), pack(cur),
         "K", 3, "IDS", "kit:stall:id", "FILTER", "kit:stall:cohort", "tier1-review")
ids = [int(m["id"][2:]) for m in r["cohort_matches"]]
k.show("closest cohort items to the stall signature", ids)
k.expect("the three nearest tier-1 review items are exactly the stalled ones", set(ids) == STALLED, str(ids))

k.section("2. Which items have moved from the healthy baseline the way the stalled one did?")
k.show("drift distance", "%.4f" % r["drift_distance"])
delta = [c - b for c, b in zip(cur, base)]
best = min(range(36), key=lambda i: cosd(items[i], delta))
k.expect("the change points at a stalled item, and the agent agrees with brute force", best in STALLED, str(best))
k.expect("the drift distance is the nearest-to-change distance", abs(r["drift_distance"] - cosd(items[best], delta)) < 1e-4)
k.done()
