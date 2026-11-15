#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Coordinated-record detection: ring signatures and the footprint of staged events.

A review desk screens new submissions against known ring signatures (FRACTAL.AGENT.OUTLIER_INTERCEPT),
finds which open records moved the way a staged ring did (a search on the change in severity), and
measures the geographic footprint of event locations with FRACTAL.DIM.BOXCOUNT: organic events wander
across the city, while a staged ring repeats along one road."""
from fvlib import Kit, pack, cosd, unit, meander, line

k = Kit("coordinated-record-detection")
rng = k.rng
RINGS = [unit([1, 0.1, 0, 0, 0]), unit([0, 1, 0.2, 0, 0])]
sigs = [[x + rng.gauss(0, 0.04) for x in RINGS[i % 2]] for i in range(20)]
k.load("kit:rings:sigs", sigs)

k.section("1. Hold submissions that match a ring signature")
suspect = [x + rng.gauss(0, 0.03) for x in RINGS[0]]
clean = unit([0, 0, 0, 1, 0.5])
a = k.js("FRACTAL.AGENT.OUTLIER_INTERCEPT", "kit:rings:sigs", pack(suspect), 0.08)
b = k.js("FRACTAL.AGENT.OUTLIER_INTERCEPT", "kit:rings:sigs", pack(clean), 0.08)
k.show("nearest signature distance: suspect / clean", "%.4f / %.4f" % (a["nearest_distance"], b["nearest_distance"]))
k.expect("the submission matching a ring is held", a["intercepted"] is True)
k.expect("the unrelated submission passes", b["intercepted"] is False)

k.section("2. Which open records moved the way the ring did?")
MOVES = [unit([1, 1, 0, 0, 0]), unit([0, 0, 1, 1, 0]), unit([0, 0, 0, 0, 1])]
k.load("kit:rings:moves", MOVES)
base = [rng.gauss(0, 0.3) for _ in range(5)]
cur = [x + 0.8 * m + rng.gauss(0, 0.02) for x, m in zip(base, MOVES[0])]
delta = [c - b0 for c, b0 in zip(cur, base)]
hit = k.js("FRACTAL.SEARCH", "kit:rings:moves", pack(delta), 1)["top_k"][0]
k.expect("the change in severity matches the ring's movement pattern", hit["idx"] == 0 == min(range(3), key=lambda i: cosd(MOVES[i], delta)))

k.section("3. Footprint of the event locations")
road = float(k.call("FRACTAL.DIM.BOXCOUNT", pack(line(rng, 300)), 2))
organic = float(k.call("FRACTAL.DIM.BOXCOUNT", pack(meander(rng, 300)), 2))
k.show("box-counting dimension: ring (one road) / organic", "%.3f / %.3f" % (road, organic))
k.expect("a ring repeating along one road reads as a line (about 1)", 0.7 < road < 1.3)
k.expect("organic events cover the city more fully", organic > road + 0.25)
k.done()
