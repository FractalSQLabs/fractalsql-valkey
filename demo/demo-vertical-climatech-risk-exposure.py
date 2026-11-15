#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Physical climate-risk exposure: a hazard-sensor grid for an insurer or asset manager.

A 20 x 20 grid of cells (three sensors each) carries a fuel-moisture deficit index. A cell is high-hazard above 0.40 in the dry
season, and 0.25 in the wet season (where the index runs about half as high). FRACTAL.AGENT.
NETWORK_COVERAGE_ALERT scores the spatial morphology of the hazard footprint (dimension and lacunarity)
and whether a telemetry series has changed regime. Same call, either season."""
import math
from fvlib import Kit, pack, flat, noise, walk

k = Kit("climatech-risk-exposure")
rng = k.rng
HOT = [(5, 6, 3.0), (14, 13, 2.5)]


def index(i, j):
    return max(math.exp(-((i - cx) ** 2 + (j - cy) ** 2) / (2 * s * s)) for cx, cy, s in HOT)


def footprint(scale, threshold):
    return [(i + rng.uniform(-0.45, 0.45), j + rng.uniform(-0.45, 0.45)) for i in range(20) for j in range(20)
            for _ in range(3) if scale * index(i, j) > threshold]


dry = footprint(1.0, 0.40)
wet = footprint(0.5, 0.25)
k.call("SET", "kit:risk:dry", pack(flat(dry)))
k.call("SET", "kit:risk:wet", pack(flat(wet)))
calm = [40 + x for x in noise(rng, 170, 3)]
event = calm[:100] + [calm[99] + x for x in walk(rng, 70, 3)]
k.call("SET", "kit:risk:calm", pack(calm))
k.call("SET", "kit:risk:event", pack(event))
k.show("high-hazard sensors: dry season / wet season", "%d / %d" % (len(dry), len(wet)))
k.expect("the wet season has fewer high-hazard sensors", len(wet) < len(dry))

k.section("1. Morphology of the hazard footprint")
for season, key, pts in (("dry", "kit:risk:dry", dry), ("wet", "kit:risk:wet", wet)):
    r = k.js("FRACTAL.AGENT.NETWORK_COVERAGE_ALERT", key, "kit:risk:calm", 2, 48)
    m = k.js("FRACTAL.MORPH.COMPLEXITY", pack(flat(pts)), 2)
    k.show("%s season: dimension / lacunarity" % season, "%.3f / %.3f" % (r["morph_dimension"], r["lacunarity"]))
    k.expect("%s: the agent's morphology is FRACTAL.MORPH.COMPLEXITY's" % season,
             abs(r["morph_dimension"] - m["dimension"]) < 1e-9 and abs(r["lacunarity"] - m["lacunarity"]) < 1e-6)

k.section("2. Has the hazard telemetry changed regime?")
steady = k.js("FRACTAL.AGENT.NETWORK_COVERAGE_ALERT", "kit:risk:dry", "kit:risk:calm", 2, 48)
shift = k.js("FRACTAL.AGENT.NETWORK_COVERAGE_ALERT", "kit:risk:dry", "kit:risk:event", 2, 48)
k.show("drift detected: steady / shifted telemetry", "%s / %s" % (steady["drift_detected"], shift["drift_detected"]))
k.expect("steady telemetry raises no alert", steady["drift_detected"] is False)
k.expect("telemetry that turns into a drifting excursion raises one", shift["drift_detected"] is True)
k.done()
