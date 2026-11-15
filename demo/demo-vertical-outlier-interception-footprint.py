#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Automated-traffic interception: bot-farm and invalid-traffic screening.

Proposed sessions are screened against a history of known-abuse signatures with FRACTAL.AGENT.OUTLIER_INTERCEPT (cosine
by default, exact Euclidean when magnitude should count). FRACTAL.DIM.BOXCOUNT on click-coordinate clouds then shows
the footprint of each traffic source: organic traffic fills the page, while an automated farm hammering a handful
of targets collapses onto a line (a dimension near 1)."""
from fvlib import Kit, pack, unit, meander, line

k = Kit("outlier-interception-footprint")
rng = k.rng
FARMS = [unit([1, 0.1, 0.1, 0, 0, 0]), unit([0.1, 1, 0.2, 0, 0, 0]), unit([0, 0.1, 1, 0.3, 0, 0])]
sigs = [[x * 5 + rng.gauss(0, 0.1) for x in FARMS[i % 3]] for i in range(24)]
k.load("kit:bots:sigs", sigs)

k.section("1. Hold sessions near a known bot-farm signature (cosine)")
bot = [x * 5 + rng.gauss(0, 0.1) for x in FARMS[0]]
human = unit([0, 0, 0, 0.2, 1, 0.6])
a = k.js("FRACTAL.AGENT.OUTLIER_INTERCEPT", "kit:bots:sigs", pack(bot), 0.05)
b = k.js("FRACTAL.AGENT.OUTLIER_INTERCEPT", "kit:bots:sigs", pack(human), 0.05)
k.expect("a session matching a farm signature is held", a["intercepted"] is True)
k.expect("an organic session is released", b["intercepted"] is False)

k.section("2. The same screen under exact Euclidean distance (threshold 0.30)")
scaled = [x * 3 for x in bot]                               # same direction, three times the magnitude
cos = k.js("FRACTAL.AGENT.OUTLIER_INTERCEPT", "kit:bots:sigs", pack(scaled), 0.05)
euc = k.js("FRACTAL.AGENT.OUTLIER_INTERCEPT", "kit:bots:sigs", pack(scaled), 0.30, "METRIC", "l2")
k.show("scaled session: cosine / L2 nearest distance", "%.4f / %.3f" % (cos["nearest_distance"], euc["nearest_distance"]))
k.expect("cosine ignores magnitude: the scaled session is still held", cos["intercepted"] is True)
k.expect("L2 counts magnitude: the scaled session is released", euc["intercepted"] is False)

k.section("3. Footprint of each traffic source's click map")
farm = float(k.call("FRACTAL.DIM.BOXCOUNT", pack(line(rng, 300)), 2))
organic = float(k.call("FRACTAL.DIM.BOXCOUNT", pack(meander(rng, 300)), 2))
k.show("box-counting dimension: farm / organic", "%.3f / %.3f" % (farm, organic))
k.expect("the bot farm collapses onto a line (about 1)", 0.7 < farm < 1.3)
k.expect("organic clicks cover the page more fully", organic > farm + 0.25)
k.done()
