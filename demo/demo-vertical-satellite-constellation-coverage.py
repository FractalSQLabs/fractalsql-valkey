#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Space and satellite communications: constellation coverage monitoring.

FRACTAL.AGENT.NETWORK_COVERAGE_ALERT scores the spatial morphology of the constellation's footprint grid (dimension and lacunarity)
plus the drift of a link-quality telemetry series, flagging coverage gaps and dead zones. FRACTAL.AGENT.SCHEDULE_WORKLOAD then
refines a workload's requirement vector and matches the nearest slot. This is the COVERAGE-GAP angle only; orbital
drift and collision detection are a different problem and are not covered."""
from fvlib import Kit, pack, flat, dist, noise, walk

k = Kit("satellite-constellation-coverage")
rng = k.rng
full = [(i + rng.uniform(-0.35, 0.35), j + rng.uniform(-0.35, 0.35)) for i in range(20) for j in range(20)]
holed = [p for p in full if (p[0] - 12) ** 2 + (p[1] - 8) ** 2 > 16]            # a dead zone: a 4-cell-radius hole
k.call("SET", "kit:sat:full", pack(flat(full)))
k.call("SET", "kit:sat:holed", pack(flat(holed)))
good = [30 + x for x in noise(rng, 170, 1.0)]
degrading = good[:100] + [good[99] + x for x in walk(rng, 70, 1.0)]
k.call("SET", "kit:sat:link:good", pack(good))
k.call("SET", "kit:sat:link:bad", pack(degrading))
k.show("footprint points: full / with a dead zone", "%d / %d" % (len(full), len(holed)))

k.section("1. Coverage morphology")
a = k.js("FRACTAL.AGENT.NETWORK_COVERAGE_ALERT", "kit:sat:full", "kit:sat:link:good", 2, 48)
b = k.js("FRACTAL.AGENT.NETWORK_COVERAGE_ALERT", "kit:sat:holed", "kit:sat:link:good", 2, 48)
k.show("lacunarity: full / with a dead zone", "%.4f / %.4f" % (a["lacunarity"], b["lacunarity"]))
k.expect("a dead zone makes the footprint gappier (higher lacunarity)", b["lacunarity"] > a["lacunarity"])

k.section("2. Link quality")
c = k.js("FRACTAL.AGENT.NETWORK_COVERAGE_ALERT", "kit:sat:full", "kit:sat:link:bad", 2, 48)
k.show("drift detected: healthy / degrading link", "%s / %s" % (a["drift_detected"], c["drift_detected"]))
k.expect("a healthy link raises no alert, a degrading one does", a["drift_detected"] is False and c["drift_detected"] is True)

k.section("3. Match a workload to a slot")
slots = [[rng.uniform(0, 10) for _ in range(5)] for _ in range(12)]
k.load("kit:sat:slots", slots)
task = slots[7]
r = k.js("FRACTAL.AGENT.SCHEDULE_WORKLOAD", "kit:sat:slots", pack(task))
k.show("assigned slot / confidence", "%s / %.3f" % (r["assigned_node"], r["confidence"]))
k.expect("the workload is assigned to the slot it matches (7)", r["assigned_node"] == "7" and r["confidence"] > 0.9)
k.done()
