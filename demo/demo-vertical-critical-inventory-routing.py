#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Critical-consumables inventory and replenishment across a depot network.

Eight depots report a daily-consumption series. FRACTAL.DIM.DFA flags the depots whose demand is
NON-LINEAR (bursty, runaway) rather than merely high. FRACTAL.AGENT.ALLOCATE then chooses which four
depots get the scheduled run, with the risk matrix inflated for the bursty ones: they fall out of the
risk-adjusted run and are flagged for expedited restock instead."""
from fvlib import Kit, pack, noise, walk

k = Kit("critical-inventory-routing")
rng = k.rng
N, BURSTY = 8, {2, 5}
demand = {}
for d in range(N):
    demand[d] = walk(rng, 128, 1.0) if d in BURSTY else noise(rng, 128, 1.0)

k.section("1. Which depots have non-linear demand?")
alpha = {d: float(k.call("FRACTAL.DIM.DFA", pack(demand[d]))) for d in range(N)}
for d in range(N):
    k.show("depot %d DFA exponent" % d, "%.3f%s" % (alpha[d], "   <- bursty" if d in BURSTY else ""))
flagged = {d for d in range(N) if alpha[d] > 0.9}
k.expect("the bursty depots are the ones flagged", flagged == BURSTY, str(sorted(flagged)))

k.section("2. Risk-adjusted replenishment run (4 of 8 depots)")
mu = [0.10] * N
cov = [[(0.04 * (50 if i in flagged else 1)) if i == j else 0.0 for j in range(N)] for i in range(N)]
flatcov = [x for row in cov for x in row]
r = k.js("FRACTAL.AGENT.ALLOCATE", pack(mu), pack(flatcov), 4)
w = r["allocation"]["weights"]
chosen = {i for i, x in enumerate(w) if x > 0}
k.show("depots in the scheduled run", sorted(chosen))
k.expect("the run holds exactly 4 depots", len(chosen) == 4, str(chosen))
k.expect("no bursty depot is in the scheduled run", not (chosen & flagged), str(chosen))
k.expect("the weights sum to 1", abs(sum(w) - 1.0) < 1e-6)
k.expect("the bursty depots are left for expedited restock", flagged == BURSTY)
k.done()
