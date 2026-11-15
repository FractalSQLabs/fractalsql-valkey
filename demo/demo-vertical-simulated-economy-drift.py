#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Simulated-economy balancing: market inflation loops and automated-actor detection.

Six markets x 96 days of price index; every market carries a weekly demand cycle and one (mkt-d) enters an accelerating
inflation spiral. FRACTAL.SERIES.PERIODOGRAM confirms the weekly cycle, FRACTAL.DIM.DFA flags the market inflating non-linearly,
and FRACTAL.OPTIMIZE.PORTFOLIO picks 3 markets for the scheduled sink program (the inflating one goes to an emergency price-cap
instead). FRACTAL.AGENT.DETECT_LOOP then watches trade-state logs: an automated actor that flips between the same two item
states is caught, while a varied actor is not."""
import math
from fvlib import Kit, pack, noise, unit

k = Kit("simulated-economy-drift")
rng = k.rng
MK = ["mkt-a", "mkt-b", "mkt-c", "mkt-d", "mkt-e", "mkt-f"]
D = 3
idx = []
for m in range(6):
    s = [100 + 5 * math.sin(2 * math.pi * d / 7.0) + x for d, x in enumerate(noise(rng, 96, 0.7))]
    if m == D:
        s = [v + (max(0, d - 30) ** 2) * 0.08 for d, v in enumerate(s)]            # the inflation spiral
    idx.append(s)

k.section("1. The weekly cycle in every market")
for m in (0, 1):
    p = k.js("FRACTAL.SERIES.PERIODOGRAM", pack(idx[m]), 3)
    j = max(range(len(p["power"])), key=lambda t: p["power"][t])
    k.show("%s dominant period" % MK[m], "%.2f days" % (1.0 / p["freqs"][j]))
    k.expect("%s peaks at the weekly period" % MK[m], abs(1.0 / p["freqs"][j] - 7) < 0.7)

k.section("2. Which market is inflating non-linearly?")
alpha = [float(k.call("FRACTAL.DIM.DFA", pack(s))) for s in idx]
for m in range(6):
    k.show("%s DFA exponent" % MK[m], "%.3f" % alpha[m])
k.expect("mkt-d has the highest exponent", max(range(6), key=lambda i: alpha[i]) == D)

k.section("3. Sink program: 3 of 6 markets")
mu = [0.10] * 6
cov = [[0.04 * alpha[i] * (25 if i == D else 1) if i == j else 0.0 for j in range(6)] for i in range(6)]
opt = k.js("FRACTAL.OPTIMIZE.PORTFOLIO", pack(mu), pack([x for r in cov for x in r]), 3, 8)
chosen = {i for i, w in enumerate(opt["weights"]) if w > 0}
k.expect("three markets, and the inflating one goes to an emergency price cap instead", len(chosen) == 3 and D not in chosen)

k.section("4. Automated actors")
bot = [unit([1, 0, 0]) if i % 2 == 0 else unit([0, 1, 0]) for i in range(40)]      # buy sword, sell sword, forever
human = [[rng.gauss(0, 1) for _ in range(3)] for _ in range(150)]
k.call("SET", "kit:econ:bot", pack([x for s in bot for x in s]))
k.call("SET", "kit:econ:human", pack([x for s in human for x in s]))
b = k.js("FRACTAL.AGENT.DETECT_LOOP", "actor-bot", "kit:econ:bot", 3)
h = k.js("FRACTAL.AGENT.DETECT_LOOP", "actor-human", "kit:econ:human", 3)
k.show("loop detected: two-state actor / varied actor", "%s / %s" % (b["is_loop_detected"], h["is_loop_detected"]))
k.expect("the actor flipping between two item states is detected", b["is_loop_detected"] is True)
k.expect("the varied actor is not", h["is_loop_detected"] is False)
k.done()
