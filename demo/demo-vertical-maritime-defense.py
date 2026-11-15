#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Maritime, aviation and defense: AIS-style vessel tracking.

Thirty vessels report a track vector [lat, lon, speed, heading] against a baseline; one vessel is given a
deliberate course deviation. A search on the change (current - baseline) is a direct fit for track-deviation
detection ("what changed"), and the DFA scaling exponent of a heading-change series fits maneuvering
irregularity (a smooth transit vs an erratic track). FRACTAL.AGENT.TRACK_ANOMALY does both in one call."""
from fvlib import Kit, pack, cosd, noise, walk

k = Kit("maritime-defense")
rng = k.rng
base = [[rng.uniform(40, 50), rng.uniform(-10, 0), rng.uniform(8, 18), rng.uniform(0, 360)] for _ in range(30)]
dev = [[rng.gauss(0, 0.02), rng.gauss(0, 0.02), rng.gauss(0, 0.2), rng.gauss(0, 1.0)] for _ in range(30)]
dev[17] = [0.9, -0.7, 5.5, 38.0]                                      # the vessel that left its lane
cur = [[b + d for b, d in zip(base[i], dev[i])] for i in range(30)]
k.load("kit:maritime:dev", dev)

k.section("1. Whose deviation is this?")
delta = [c - b0 for c, b0 in zip(cur[17], base[17])]
hit = k.js("FRACTAL.SEARCH", "kit:maritime:dev", pack(delta), 1)["top_k"][0]
k.show("vessel whose deviation signature matches", hit["idx"])
k.expect("the search on the change finds vessel 17 and agrees with brute force",
         hit["idx"] == 17 == min(range(30), key=lambda i: cosd(dev[i], delta)))

k.section("2. Smooth transit vs erratic track")
smooth = noise(rng, 96, 0.05)
erratic = noise(rng, 36, 0.05) + [x * 0.05 for x in walk(rng, 60, 1.0)]
k.call("SET", "kit:maritime:h:smooth", pack(smooth))
k.call("SET", "kit:maritime:h:erratic", pack(erratic))
a = k.js("FRACTAL.AGENT.TRACK_ANOMALY", "kit:maritime:dev", pack(base[17]), pack(cur[17]), "kit:maritime:h:erratic")
b = k.js("FRACTAL.AGENT.TRACK_ANOMALY", "kit:maritime:dev", pack(base[3]), pack(cur[3]), "kit:maritime:h:smooth")
k.show("heading DFA exponent: erratic / smooth", "%.3f / %.3f" % (a["dfa_exponent"], b["dfa_exponent"]))
k.expect("the agent finds the same vessel (17) as the hand-composed search", a["nearest_fleet_id"] == 17)
k.expect("an erratic track persists more than a smooth transit", a["dfa_exponent"] > b["dfa_exponent"] + 0.2)
k.done()
