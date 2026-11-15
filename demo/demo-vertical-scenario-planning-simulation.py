#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Strategic scenario planning and simulation training: business continuity, logistics contingency, tabletop exercises.

The job is a SPREAD of distinct, non-overlapping scenario variants. Twenty-four candidate scenarios sit in a four-
dimension option space (cost, risk, speed, resilience). Scout (FRACTAL.EXPLORE, the diversity-first search) picks scenarios
from across the space with no model. FRACTAL.AGENT.PLAN_EXPLORE, the shipped planner, embeds a seed-state description and
Scout-searches the embedded scenario descriptions; it needs an embedding provider behind fractalsqld and is skipped
without one. Embed your own descriptions with FRACTAL.EMBED and store the vectors as the corpus rows."""
from fvlib import Kit, pack, dist

k = Kit("scenario-planning-simulation")
rng = k.rng
STYLES = [[0.2, 0.8, 0.9, 0.2], [0.8, 0.2, 0.3, 0.9], [0.5, 0.5, 0.5, 0.5], [0.9, 0.9, 0.1, 0.1]]   # cost, risk, speed, resilience
scen = [[x + rng.gauss(0, 0.04) for x in STYLES[i % 4]] for i in range(24)]
style = [i % 4 for i in range(24)]
k.load("kit:scen:space", scen)
seed = [0.5, 0.5, 0.5, 0.5]

k.section("1. Six deliberately different scenarios")
pop = k.js("FRACTAL.EXPLORE", "kit:scen:space", pack(seed), 120)["population"]
pts = [p if isinstance(p, list) else p.get("point", p.get("x")) for p in pop]
picked = {min(range(24), key=lambda i: dist(scen[i], p)) for p in pts}
styles = {style[i] for i in picked}
near = {style[t["idx"]] for t in k.js("FRACTAL.SEARCH", "kit:scen:space", pack(seed), 6)["top_k"]}
k.expect("the discovery run returns its full population", len(pop) == 120)
k.show("scenario styles reached: discovery / six nearest", "%s / %s" % (sorted(styles), sorted(near)))
k.expect("discovery reaches at least 3 of the 4 scenario styles", len(styles) >= 3)
k.expect("the six nearest scenarios are near-twins of one style, and discovery spans more", len(near) < len(styles))

k.section("2. The shipped planner")
plan = k.optional_js("PLAN_EXPLORE (embeds 'a port closure halves throughput')", "FRACTAL.AGENT.PLAN_EXPLORE", "kit:scen:space", "a port closure halves throughput", 4, 6)
if plan is not None:
    k.expect("it returns distinct branches, each with a confidence score", len({b["branch_id"] for b in plan}) == len(plan) and all("confidence_score" in b for b in plan))
k.done()
