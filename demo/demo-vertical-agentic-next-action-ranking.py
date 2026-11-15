#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Agentic ranking: autonomous candidate scoring and dynamic next-action routing.

Thirty candidates across three segments and a twelve-item action catalog. FRACTAL.AGENT.RECALL_HYBRID recalls the most
similar past candidates inside one segment (a strict metadata filter, as a hash), and FRACTAL.AGENT.RECOMMEND_DIVERSE proposes a
diverse set of next actions (not four variations of "send an email"). A status question such as "how many tier-1
candidates are there?" is a key lookup."""
from fvlib import Kit, pack, cosd, unit

k = Kit("agentic-next-action-ranking")
rng = k.rng
SEG = ["enterprise", "midmarket", "smb"]
PROFILE = [unit([1, 0.2, 0, 0, 0, 0]), unit([0, 1, 0.3, 0, 0, 0]), unit([0, 0, 1, 0.5, 0, 0])]
cand = [[x + rng.gauss(0, 0.05) for x in PROFILE[i % 3]] for i in range(30)]
k.load("kit:nba:cand", cand)
for i in range(30):
    k.call("HSET", "kit:nba:seg", str(i), SEG[i % 3])
    k.call("HSET", "kit:nba:id", str(i), "C-%02d" % i)
    k.call("HSET", "kit:nba:tier", str(i), "1" if i % 5 == 0 else "2")

k.section("1. Similar past candidates inside one segment")
q = [x + rng.gauss(0, 0.04) for x in PROFILE[1]]
rec = k.js("FRACTAL.AGENT.RECALL_HYBRID", "kit:nba:cand", pack(q), "K", 4, "IDS", "kit:nba:id", "FILTER", "kit:nba:seg", "midmarket")
ids = [int(x["mem_id"][2:]) for x in rec]
k.show("recalled midmarket candidates", ids)
k.expect("every recalled candidate is in the midmarket segment", all(i % 3 == 1 for i in ids))
k.expect("the best one agrees with brute force", ids[0] == min((i for i in range(30) if i % 3 == 1), key=lambda i: cosd(cand[i], q)))

k.section("2. A diverse next-action set")
catalog = [[x + rng.gauss(0, 0.2) for x in PROFILE[i % 3]] for i in range(12)]
k.load("kit:nba:actions", catalog)
div = k.js("FRACTAL.AGENT.RECOMMEND_DIVERSE", "kit:nba:actions", pack(q), "K", 4)
k.show("proposed actions", [x["item_id"] for x in div])
k.expect("four distinct actions", len({x["item_id"] for x in div}) == 4)
k.expect("scores fall from the best match down", all(div[i]["score"] >= div[i + 1]["score"] - 1e-9 for i in range(3)))

k.section("3. A status question: how many tier-1 candidates?")
tier1 = sum(1 for v in k.call("HGETALL", "kit:nba:tier")[1::2] if v == b"1")
k.show("tier-1 candidates (a key lookup)", tier1)
k.expect("the answer is the count the data holds", tier1 == 6)
k.done()
