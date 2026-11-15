#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Event-stream triage: a latency ramp, a retry storm, and which endpoints look alike.

Four endpoints x 192 one-minute intervals. EP-C develops an accelerating latency
ramp (a timeout cascade); EP-B has a retry-storm oscillation in request rate.
FRACTAL.DIM.DRIFT finds the regime change, FRACTAL.SERIES.PERIODOGRAM the storm's
period, and FRACTAL.SEARCH which of a 40-endpoint fleet has health like EP-C's."""
import math
from fvlib import Kit, pack

k = Kit("event-stream-anomaly-triage")
rng, T = k.rng, 192

def latency(ramp):
    base = [50 + rng.gauss(0, 3) for _ in range(T)]
    if ramp:
        for i in range(120, T):
            base[i] += ((i - 120) ** 2) / 40.0
    return base

k.section("1. Which endpoint's latency changed regime")
drifts = {}
for name, ramp in (("EP-A", False), ("EP-B", False), ("EP-C", True), ("EP-D", False)):
    drifts[name] = abs(k.js("FRACTAL.DIM.DRIFT", pack(latency(ramp)), 64)["drift"])
    k.show(name + " |DFA drift|", "%.3f" % drifts[name])
worst = max(drifts, key=drifts.get)
k.expect("EP-C is the endpoint whose latency changed regime", worst == "EP-C", worst)

k.section("2. The retry storm's period")
storm = [100 + 30 * math.sin(2 * math.pi * i / 8) + rng.gauss(0, 4) for i in range(T)]
pg = k.js("FRACTAL.SERIES.PERIODOGRAM", pack(storm), 2)
k.show("dominant frequency / period", "%.4f / %.1f samples" % (pg["freqs"][0], 1 / pg["freqs"][0]))
k.expect("the storm repeats every 8 intervals", abs(1 / pg["freqs"][0] - 8) < 0.5)

k.section("3. Which endpoints' health moved the way EP-C's did")
fleet = []
for i in range(40):
    fleet.append([rng.uniform(0.97, 1.0), rng.uniform(40, 70), rng.uniform(0, 0.01), rng.uniform(0.2, 0.5)])
cascade = [0.62, 410.0, 0.31, 0.93]                 # EP-C: success down, latency/timeouts/fill way up
fleet[17] = [x * rng.uniform(0.97, 1.03) for x in cascade]
fleet[29] = [x * rng.uniform(0.97, 1.03) for x in cascade]
k.load("kit:fleet", fleet)
# scale each health dimension to a comparable range before comparing
scale = [1.0, 1 / 400.0, 1.0, 1.0]
k.load("kit:fleet:n", [[a * s for a, s in zip(r, scale)] for r in fleet])
q = [a * s for a, s in zip(cascade, scale)]
top = k.js("FRACTAL.SEARCH", "kit:fleet:n", pack(q), 2)["top_k"]
k.show("two nearest endpoints", [t["idx"] for t in top])
k.expect("the two endpoints with EP-C's signature are found", {t["idx"] for t in top} == {17, 29})
k.done()
