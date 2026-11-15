#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""ADAS / autonomous-driving safety validation: stop an edge case before the maneuver is committed.

Thirty known-bad vehicle states (prior disengagements and near-misses) cluster around three failure
modes. FRACTAL.AGENT.OUTLIER_INTERCEPT screens a PROPOSED state against them and holds it when it is
too close to one. A fleet vehicle whose routing vector has moved away from the fleet baseline is
matched to the drift signature it resembles by searching for the change (current - baseline).
Every step runs with no model; the agent adds a written reason when a reasoner is configured."""
from fvlib import Kit, pack, cosd, unit

k = Kit("adas-safety-validation")
rng = k.rng
MODES = [unit([1, 0.2, 0, 0, 0, 0]), unit([0, 1, 0.3, 0, 0, 0]), unit([0, 0, 1, 0.4, 0, 0])]
bad = [[x + rng.gauss(0, 0.05) for x in MODES[i % 3]] for i in range(30)]
k.load("kit:adas:bad", bad)

k.section("1. Screen proposed maneuvers against known-bad states")
near = [x + rng.gauss(0, 0.04) for x in MODES[1]]
safe = unit([0, 0, 0, 0, 1, 0.2])
a = k.js("FRACTAL.AGENT.OUTLIER_INTERCEPT", "kit:adas:bad", pack(near), 0.1)
b = k.js("FRACTAL.AGENT.OUTLIER_INTERCEPT", "kit:adas:bad", pack(safe), 0.1)
k.show("nearest known-bad distance: risky / safe", "%.4f / %.4f" % (a["nearest_distance"], b["nearest_distance"]))
k.expect("the maneuver resembling a failure mode is intercepted", a["intercepted"] is True)
k.expect("the unrelated maneuver is allowed", b["intercepted"] is False)
brute = min(cosd(s, near) for s in bad)
k.expect("the nearest distance is the brute-force cosine value", abs(a["nearest_distance"] - brute) < 1e-3, "%s vs %s" % (a["nearest_distance"], brute))
l2 = k.js("FRACTAL.AGENT.OUTLIER_INTERCEPT", "kit:adas:bad", pack(near), 0.2, "METRIC", "l2")
k.expect("the same screen under the L2 metric also intercepts", l2["intercepted"] is True and l2["metric"] == "l2")

k.section("2. Which drift signature has this vehicle moved along?")
SIGS = [unit([1, 0, 0, 0, 0, 0]), unit([0, 1, 1, 0, 0, 0]), unit([0, 0, 0, 1, -1, 0]), unit([-1, 0, 0, 0, 0, 1])]
k.load("kit:adas:sigs", SIGS)
baseline = [rng.gauss(0, 0.2) for _ in range(6)]
current = [x + 0.6 * s + rng.gauss(0, 0.02) for x, s in zip(baseline, SIGS[2])]
delta = [c - b0 for c, b0 in zip(current, baseline)]
hit = k.js("FRACTAL.SEARCH", "kit:adas:sigs", pack(delta), 1)["top_k"][0]
k.show("closest drift signature", hit["idx"])
k.expect("the change matches signature 2 and agrees with brute force", hit["idx"] == 2 == min(range(4), key=lambda i: cosd(SIGS[i], delta)))
k.done()
