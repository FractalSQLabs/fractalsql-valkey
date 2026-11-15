#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Periodicity and selection: cycle-strength detection and risk-aware portfolio choice.

Six items x 96 days of sales velocity with a weekly demand cycle of differing strength. FRACTAL.SERIES.PERIODOGRAM
quantifies which items are genuinely periodic; FRACTAL.OPTIMIZE.PORTFOLIO picks the 3 items that get the scheduled action
(a Sharpe-style trade of margin recovered against demand volatility); FRACTAL.AGENT.RECOMMEND_DIVERSE proposes a diverse set
of substitute items. On the Enterprise edition FRACTAL.AGENT.DIVERSE_PORTFOLIOS also returns structurally different
schedules; on a Community install that step is skipped."""
import math
from fvlib import Kit, pack, noise

k = Kit("periodicity-portfolio-selection")
rng = k.rng
STRENGTH = [0.0, 4.0, 0.0, 6.0, 0.5, 3.0]
sales = [[50 + a * math.sin(2 * math.pi * d / 7.0) + x for d, x in enumerate(noise(rng, 96, 1.0))] for a in STRENGTH]

k.section("1. Which items are genuinely periodic?")
peak = []
for i, s in enumerate(sales):
    p = k.js("FRACTAL.SERIES.PERIODOGRAM", pack(s), 3)
    j = max(range(len(p["power"])), key=lambda t: p["power"][t])
    peak.append((p["power"][j], 1.0 / p["freqs"][j] if p["freqs"][j] else float("inf")))
    k.show("item %d: peak power / period" % i, "%.1f / %.2f days" % peak[-1])
strong = [i for i in range(6) if STRENGTH[i] >= 3.0]
k.expect("the periodic items peak at the weekly period", all(abs(peak[i][1] - 7) < 0.7 for i in strong), str([peak[i][1] for i in strong]))
ranked = sorted(range(6), key=lambda i: peak[i][0], reverse=True)
k.expect("the strongest weekly cycle (item 3) has the most periodogram power", ranked[0] == 3, str(ranked))

k.section("2. Scheduled action on 3 of 6 items")
mu = [0.05 + 0.02 * (STRENGTH[i] > 2) for i in range(6)]
cov = [[0.02 if i == j else 0.0 for j in range(6)] for i in range(6)]
opt = k.js("FRACTAL.OPTIMIZE.PORTFOLIO", pack(mu), pack([x for r in cov for x in r]), 3, 5)
chosen = {i for i, w in enumerate(opt["weights"]) if w > 0}
k.show("selected items", sorted(chosen))
k.expect("three items are selected, favouring the higher-return (periodic) ones", len(chosen) == 3 and chosen <= set(strong), str(chosen))

k.section("3. Diverse substitutes")
items = [[rng.uniform(-3, 3) for _ in range(6)] for _ in range(24)]
k.load("kit:period:items", items)
rec = k.js("FRACTAL.AGENT.RECOMMEND_DIVERSE", "kit:period:items", pack(items[2]), "K", 5)
k.expect("five distinct substitutes, the item itself first", len({x["item_id"] for x in rec}) == 5 and rec[0]["item_id"] == 2)

k.section("4. Structurally different schedules (Enterprise)")
mu6 = [0.05 + 0.02 * (STRENGTH[i] > 2) for i in range(6)]
div = k.optional_js("DIVERSE_PORTFOLIOS (enterprise tier)", "FRACTAL.AGENT.DIVERSE_PORTFOLIOS", pack(mu6),
                    pack([x for r in [[0.02 if i == j else 0.0 for j in range(6)] for i in range(6)] for x in r]), 3, "N_RESTARTS", 6, "SEED", 5)
if div is not None:
    k.expect("the enterprise optimizer returns more than one candidate, each holding at most 3 items", len(div) >= 1 and all(sum(1 for w in c["weights"] if w > 0) <= 3 for c in div))
k.done()
