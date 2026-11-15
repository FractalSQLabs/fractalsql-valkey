#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Agentic fintech: portfolio rebalance and strategy planning.

FRACTAL.AGENT.REBALANCE_SIBLING optimizes a portfolio and finds the historical allocation the new weights most resemble.
FRACTAL.AGENT.PLAN_EXPLORE embeds a seed-state description and Scout-searches a key of strategy embeddings for a spread of
distinct branches; it needs an embedding provider behind fractalsqld and is skipped without one."""
from fvlib import Kit, pack, unit

k = Kit("agentic-fintech-mcts")
rng = k.rng
N = 6
mu = [0.06, 0.10, 0.08, 0.12, 0.07, 0.09]
cov = [[(0.04 if i == j else 0.004) for j in range(N)] for i in range(N)]
fc = pack([x for r in cov for x in r])

k.section("1. Rebalance and find the historical allocation it resembles")
ref = k.js("FRACTAL.OPTIMIZE.PORTFOLIO", pack(mu), fc, 3, 17)
hist = [unit([rng.random() for _ in range(N)]) for _ in range(24)]
hist[11] = ref["weights"]                                       # one past allocation equals today's optimum
k.load("kit:fintech:hist", hist)
r = k.js("FRACTAL.AGENT.REBALANCE_SIBLING", "kit:fintech:hist", pack(mu), fc, 3, pack([0.0] * N), "SEED", 17)
k.show("sharpe / nearest historical distance", "%.4f / %.6f" % (r["sharpe"], r["nearest_distance"]))
k.expect("the agent reproduces the seeded optimizer", abs(r["sharpe"] - ref["sharpe"]) < 1e-9 and r["weights"] == ref["weights"])
k.expect("it finds the past allocation that equals the new weights", r["nearest_distance"] < 1e-4)
k.expect("the new portfolio holds three assets", sum(1 for w in r["weights"] if w > 0) == 3)

k.section("2. Planning branches from an embedded seed state")
strategies = [[rng.uniform(-1, 1) for _ in range(8)] for _ in range(30)]
k.load("kit:fintech:strategies", strategies)
plan = k.optional_js("PLAN_EXPLORE (embeds 'tighten risk limits')", "FRACTAL.AGENT.PLAN_EXPLORE", "kit:fintech:strategies", "tighten risk limits", 8, 5)
if plan is not None:
    k.expect("planning returns branches with a confidence score", 1 <= len(plan) <= 5 and all("confidence_score" in b for b in plan))

k.done()
