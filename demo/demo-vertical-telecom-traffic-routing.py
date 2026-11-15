#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Telecom: dynamic traffic and bandwidth dispatch to the best-capable network node under a capacity budget.

FRACTAL.AGENT.ROUTE_TASK matches each incoming request to the best-capable node under a token budget. Scope note: this is
nearest-capable-node dispatch, not multi-hop rerouting around a failed node (graph routing is a different problem).
Load awareness comes from routing over a key holding only the nodes that still have headroom. Vectors are
[bandwidth, latency_sensitivity, loss_tolerance, region_affinity]: a request says what the session needs, a node says
what it is good at."""
from fvlib import Kit, pack, cosd

k = Kit("telecom-traffic-routing")
rng = k.rng
NODES = ["edge-east", "edge-west", "core-1", "core-2", "peering-a"]
cap = [[0.9, 0.9, 0.1, 0.8], [0.9, 0.8, 0.2, 0.2], [0.7, 0.3, 0.7, 0.5], [0.6, 0.2, 0.8, 0.4], [0.3, 0.1, 0.9, 0.9]]
k.load("kit:route:all", cap)
req = [0.88, 0.85, 0.12, 0.75]                          # a latency-sensitive video session near the east region

k.section("1. Dispatch against every node")
r = k.js("FRACTAL.AGENT.ROUTE_TASK", "kit:route:all", pack(req), 1000, "COST_PER_ROUTE", 100)
want = min(range(5), key=lambda i: cosd(cap[i], req))
k.show("routed to / confidence / budget left", "%s / %.3f / %d" % (NODES[int(r["routed_to"])], r["confidence"], r["remaining_budget"]))
k.expect("the session goes to the best-capable node (edge-east) and agrees with brute force", r["routed_to"] == str(want) == "0")
k.expect("the routing spends its cost", r["remaining_budget"] == 900)

k.section("2. Load awareness: edge-east is full")
hr = [1, 2, 3, 4]                                       # the nodes that still have headroom
k.load("kit:route:headroom", [cap[i] for i in hr])
r2 = k.js("FRACTAL.AGENT.ROUTE_TASK", "kit:route:headroom", pack(req), 900, "COST_PER_ROUTE", 100)
k.show("routed to", NODES[hr[int(r2["routed_to"])]])
k.expect("the session moves to the next best node with headroom (edge-west)", hr[int(r2["routed_to"])] == 1)
k.expect("its confidence is lower than on the full node set", r2["confidence"] < r["confidence"])
k.done()
