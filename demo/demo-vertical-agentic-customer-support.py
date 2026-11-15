#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Customer support: trajectory forecasting, similar-ticket recall and diverse next replies.

A customer's ticket history is a sequence of state vectors. FRACTAL.AGENT.TRAJECTORY_PREDICT searches for the known
state nearest to (latest - baseline), "where is this heading". FRACTAL.AGENT.RECALL_HYBRID recalls the most similar past
tickets inside one product category (a strict metadata filter, as a hash), and FRACTAL.AGENT.RECOMMEND_DIVERSE proposes a
diverse set of reply macros rather than six variations of one."""
from fvlib import Kit, pack, cosd, unit

k = Kit("agentic-customer-support")
rng = k.rng
PATH = [unit([1, 0, 0, 0, 0]), unit([0.7, 0.7, 0, 0, 0]), unit([0, 1, 0.3, 0, 0]), unit([0, 0.3, 1, 0.5, 0]), unit([0, 0, 0.3, 1, 1])]
history = [[x + rng.gauss(0, 0.02) for x in PATH[i // 3]] for i in range(15)]
k.load("kit:support:history", history)

k.section("1. Where is this customer heading?")
r = k.js("FRACTAL.AGENT.TRAJECTORY_PREDICT", "kit:support:history", 5, 0)
delta = [a - b for a, b in zip(history[-1], history[0])]
best = min(range(15), key=lambda i: cosd(history[i], delta))
k.show("projected drift / risk exceeded", "%.4f / %s" % (r["projected_drift_delta"], r["risk_threshold_exceeded"]))
k.expect("the predicted state is the brute-force nearest to the change", all(abs(a - b) < 1e-5 for a, b in zip(r["predicted_state_vector"], history[best])))

k.section("2. Similar past tickets in one category")
tickets = [[x + rng.gauss(0, 0.05) for x in PATH[i % 5]] for i in range(40)]
k.load("kit:support:tickets", tickets)
for i in range(40):
    k.call("HSET", "kit:support:cat", str(i), "billing" if i % 2 == 0 else "login")
    k.call("HSET", "kit:support:tid", str(i), "T-%03d" % i)
q = [x + rng.gauss(0, 0.03) for x in PATH[2]]
rec = k.js("FRACTAL.AGENT.RECALL_HYBRID", "kit:support:tickets", pack(q), "K", 4, "IDS", "kit:support:tid", "FILTER", "kit:support:cat", "billing")
ids = [int(x["mem_id"][2:]) for x in rec]
k.show("recalled billing tickets", ids)
k.expect("every recalled ticket is in the billing category", all(i % 2 == 0 for i in ids))
k.expect("the best one is the brute-force nearest billing ticket", ids[0] == min((i for i in range(40) if i % 2 == 0), key=lambda i: cosd(tickets[i], q)))

k.section("3. A diverse set of reply macros")
macros = [[x + rng.gauss(0, 0.1) for x in PATH[i % 5]] for i in range(20)]
k.load("kit:support:macros", macros)
div = k.js("FRACTAL.AGENT.RECOMMEND_DIVERSE", "kit:support:macros", pack(q), "K", 5)
k.expect("five distinct macros are proposed", len({x["item_id"] for x in div}) == 5)
k.done()
