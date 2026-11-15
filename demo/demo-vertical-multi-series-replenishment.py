#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Multi-location replenishment: stockout prevention and non-linear demand forecasting.

Six locations x 64 days of unit demand. FRACTAL.DIM.DFA finds the location whose demand is ACCELERATING (a
viral item or a local event) rather than merely high. FRACTAL.OPTIMIZE.PORTFOLIO picks which 3 locations get the
scheduled replenishment from the central hub, with the risk matrix inflated by the DFA exponent; the surging
location is flagged for an expedited pick instead. FRACTAL.AGENT.ALLOCATE is the one-call form of the optimizer."""
from fvlib import Kit, pack, noise

k = Kit("multi-series-replenishment")
rng = k.rng
N, SURGE = 6, 3
demand = []
for loc in range(N):
    base = 40 + 25 * (loc == 1)                          # location 1 is merely HIGH
    s = [base + x for x in noise(rng, 64, 2.0)]
    if loc == SURGE:
        s = [v + (max(0, i - 24) ** 2) * 0.06 for i, v in enumerate(s)]     # location 3 is accelerating
    demand.append(s)

k.section("1. Non-linear vs merely high")
alpha = [float(k.call("FRACTAL.DIM.DFA", pack(s))) for s in demand]
for loc in range(N):
    k.show("location %d: mean demand / DFA exponent" % loc, "%.1f / %.3f" % (sum(demand[loc]) / 64, alpha[loc]))
k.expect("the accelerating location has the highest DFA exponent", max(range(N), key=lambda i: alpha[i]) == SURGE)
k.expect("the merely-high location is not flagged as non-linear", alpha[1] < alpha[SURGE] - 0.3)

k.section("2. Scheduled replenishment: 3 of 6, risk inflated by the DFA exponent")
mu = [0.10] * N
cov = [[0.04 * alpha[i] * (1 if i not in (SURGE,) else 25) if i == j else 0.0 for j in range(N)] for i in range(N)]
flat_cov = [x for row in cov for x in row]
opt = k.js("FRACTAL.OPTIMIZE.PORTFOLIO", pack(mu), pack(flat_cov), 3, 11)
chosen = {i for i, w in enumerate(opt["weights"]) if w > 0}
k.show("locations in the scheduled run", sorted(chosen))
k.expect("three locations are scheduled", len(chosen) == 3)
k.expect("the surging location is left out and flagged for an expedited pick", SURGE not in chosen)

k.section("3. The shipped one-call agent")
ag = k.js("FRACTAL.AGENT.ALLOCATE", pack(mu), pack(flat_cov), 3)
held = {i for i, w in enumerate(ag["allocation"]["weights"]) if w > 0}
k.expect("the agent also leaves the surging location out", len(held) == 3 and SURGE not in held, str(sorted(held)))
k.done()
