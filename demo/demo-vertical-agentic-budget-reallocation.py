#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Agentic allocation: multi-source attribution and dynamic cross-channel budget reallocation.

Six channels x 64 days of attributed outcomes. FRACTAL.DIM.DFA finds the channel whose response is ACCELERATING (a
non-linear surge a plain average would hide). FRACTAL.OPTIMIZE.PORTFOLIO picks which 3 channels receive the
scheduled budget increase, with the risk matrix inflated by the DFA exponent; the surging channel is flagged
for a capped, monitored test instead. FRACTAL.AGENT.ALLOCATE is the one-call form of the optimizer step, and
FRACTAL.AGENT.ROUTE_TASK dispatches each customer journey to the best-matching model under a token budget."""
from fvlib import Kit, pack, cosd, noise

k = Kit("agentic-budget-reallocation")
rng = k.rng
N, SURGE = 6, 4
out = []
for ch in range(N):
    s = [30 + 20 * (ch == 0) + x for x in noise(rng, 64, 2.0)]
    if ch == SURGE:
        s = [v + (max(0, i - 20) ** 2) * 0.07 for i, v in enumerate(s)]
    out.append(s)

k.section("1. Non-linear response vs merely high")
alpha = [float(k.call("FRACTAL.DIM.DFA", pack(s))) for s in out]
for ch in range(N):
    k.show("channel %d: mean / DFA exponent" % ch, "%.1f / %.3f" % (sum(out[ch]) / 64, alpha[ch]))
k.expect("the accelerating channel has the highest exponent", max(range(N), key=lambda i: alpha[i]) == SURGE)

k.section("2. Scheduled budget increase: 3 of 6 channels")
mu = [0.10] * N
cov = [[0.04 * alpha[i] * (25 if i == SURGE else 1) if i == j else 0.0 for j in range(N)] for i in range(N)]
fc = [x for row in cov for x in row]
opt = k.js("FRACTAL.OPTIMIZE.PORTFOLIO", pack(mu), pack(fc), 3, 9)
chosen = {i for i, w in enumerate(opt["weights"]) if w > 0}
k.show("channels receiving the increase", sorted(chosen))
k.expect("three channels, and not the surging one (capped test instead)", len(chosen) == 3 and SURGE not in chosen)
ag = k.js("FRACTAL.AGENT.ALLOCATE", pack(mu), pack(fc), 3)
held = {i for i, w in enumerate(ag["allocation"]["weights"]) if w > 0}
k.expect("the one-call agent agrees on leaving the surging channel out", len(held) == 3 and SURGE not in held)

k.section("3. Route journeys to the best-matching model under a token budget")
models = [[rng.uniform(-1, 1) for _ in range(6)] for _ in range(5)]
k.load("kit:budget:models", models)
journey = [x + rng.gauss(0, 0.02) for x in models[3]]
budget = 1000
for step in range(3):
    r = k.js("FRACTAL.AGENT.ROUTE_TASK", "kit:budget:models", pack(journey), budget, "COST_PER_ROUTE", 150)
    budget = r["remaining_budget"]
k.show("route / remaining token budget", "%s / %d" % (r["routed_to"], budget))
k.expect("the journey goes to model 3 (brute-force nearest)", r["routed_to"] == "3" == str(min(range(5), key=lambda i: cosd(models[i], journey))))
k.expect("each routing spends its cost: 1000 - 3 x 150 = 550", budget == 550)
k.done()
