#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Resource allocation: sensor telemetry regime shifts and non-linear demand.

Six sites x 64 days of resource demand from soil-moisture sensors. FRACTAL.DIM.DFA finds the site whose demand is accelerating
(a non-linear trend or a failing supply line) rather than merely high. FRACTAL.OPTIMIZE.PORTFOLIO picks which 3 sites get the
scheduled service run, with the risk matrix inflated by the DFA exponent; the accelerating site is flagged for an
inspection instead. FRACTAL.AGENT.ALLOCATE is the one-call form of the optimizer step."""
from fvlib import Kit, pack, noise, walk

k = Kit("sensor-telemetry-allocation")
rng = k.rng
N, FAILING = 6, 2
demand = []
for s in range(N):
    base = [60 + 30 * (s == 5) + x for x in noise(rng, 64, 2.0)]
    if s == FAILING:
        base = [60 + x for x in walk(rng, 64, 2.5)]              # a failing supply line wanders off
    demand.append(base)

k.section("1. Non-linear trend vs merely high demand")
alpha = [float(k.call("FRACTAL.DIM.DFA", pack(s))) for s in demand]
for s in range(N):
    k.show("site %d: mean / DFA exponent" % s, "%.1f / %.3f" % (sum(demand[s]) / 64, alpha[s]))
k.expect("the failing site has the highest exponent", max(range(N), key=lambda i: alpha[i]) == FAILING)
k.expect("the merely-high site (5) does not", alpha[5] < alpha[FAILING] - 0.3)

k.section("2. Scheduled service run: 3 of 6 sites")
mu = [0.10] * N
cov = [[0.04 * alpha[i] * (25 if i == FAILING else 1) if i == j else 0.0 for j in range(N)] for i in range(N)]
fc = pack([x for r in cov for x in r])
opt = k.js("FRACTAL.OPTIMIZE.PORTFOLIO", pack(mu), fc, 3, 4)
chosen = {i for i, w in enumerate(opt["weights"]) if w > 0}
k.show("sites in the service run", sorted(chosen))
k.expect("three sites, and the failing one is flagged for inspection instead", len(chosen) == 3 and FAILING not in chosen)
ag = k.js("FRACTAL.AGENT.ALLOCATE", pack(mu), fc, 3)
held = {i for i, w in enumerate(ag["allocation"]["weights"]) if w > 0}
k.expect("the one-call agent agrees", len(held) == 3 and FAILING not in held)
k.done()
