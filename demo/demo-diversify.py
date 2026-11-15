#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Sessions: a search that remembers what you rejected.

A named session holds the diversify/repulsion state between searches. The kit walks the loop a recommendation
screen runs: search, the user rejects the top result, search again through the same session. Two sessions
given the same calls end in the same state, and a session you never gave feedback to does not."""
import sys, os
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__))))
from fvlib import Kit, pack

k = Kit("demo-diversify")
rng = k.rng
rows = []
for i in range(60):
    base = [1, 0, 0, 0] if i < 30 else [0, 1, 0, 0]
    rows.append([x + rng.gauss(0, 0.05) for x in base])
k.load("demo:catalog", rows)
q = pack([1, 0.1, 0, 0])

def loop(name, feedback):
    k.call("FRACTAL.DIVERSIFY.ENABLE", name)
    k.call("FRACTAL.DIVERSIFY.PARAMS", name, '{"repulsion_weight": 50, "repulsion_sigma": 1.0}')
    first = k.js("FRACTAL.EXPLORE", "demo:catalog", q, 20, "SESSION", name)
    if feedback:
        k.call("FRACTAL.FEEDBACK", name, first["top_k"][0]["idx"], "NEGATIVE")
    k.js("FRACTAL.EXPLORE", "demo:catalog", q, 20, "SESSION", name)
    return float(k.call("FRACTAL.DIVERSIFY.DQ", name))

k.section("1. Search, reject, search again")
a, b, c = loop("demo-a", True), loop("demo-b", True), loop("demo-c", False)
k.show("D_q after rejecting the top result (two sessions)", "%.4f and %.4f" % (a, b))
k.show("D_q for a session that was never given feedback", "%.4f" % c)
k.expect("the same calls give the same state", a == b)
k.expect("feedback changed the state", a != c)

k.section("2. What a session looks like")
k.show("explain", k.call("FRACTAL.DIVERSIFY.EXPLAIN", "demo-a").decode())
k.show("live sessions", sorted(s.decode() for s in k.call("FRACTAL.SESSION.LIST")))

k.section("3. Clean up")
for s in ("demo-a", "demo-b", "demo-c"):
    k.call("FRACTAL.SESSION.CLOSE", s)
k.expect("the demo sessions are gone", not any(x.decode().startswith("demo-") for x in k.call("FRACTAL.SESSION.LIST")))
k.done()
