#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Agentic recommendation: recall of similar past sessions and diverse next-best offers.

Twenty-four candidate offers fall into four clusters and thirty abandoned sessions into three categories. Scout
(FRACTAL.EXPLORE) picks offers from across the clusters around the user's profile so the follow-up tests variety rather than
near-identical offers; FRACTAL.AGENT.RECALL_HYBRID recalls the most similar past sessions inside the user's category (a strict
metadata filter) with their follow-up notes; and FRACTAL.AGENT.RECOMMEND_DIVERSE proposes the diverse set. A status question such as "how many abandoned watches
sessions are there?" is a key lookup."""
from fvlib import Kit, pack, dist, cosd, unit

k = Kit("agentic-recall-recommendation")
rng = k.rng
C = [[rng.uniform(-5, 5) for _ in range(8)] for _ in range(4)]
offers = [[x + rng.gauss(0, 0.3) for x in C[i % 4]] for i in range(24)]
cluster = [i % 4 for i in range(24)]
k.load("kit:rr:offers", offers)
user = [sum(col) / 4 for col in zip(*C)]

k.section("1. Six different offers around the user's profile")
pop = k.js("FRACTAL.EXPLORE", "kit:rr:offers", pack(user), 150)["population"]
pts = [p if isinstance(p, list) else p.get("point", p.get("x")) for p in pop]
reached = {cluster[min(range(24), key=lambda i: dist(offers[i], p))] for p in pts}
nearest = {cluster[t["idx"]] for t in k.js("FRACTAL.SEARCH", "kit:rr:offers", pack(user), 6)["top_k"]}
k.show("offer clusters reached: discovery / six nearest", "%s / %s" % (sorted(reached), sorted(nearest)))
k.expect("discovery spans at least 3 of the 4 clusters", len(reached) >= 3)
k.expect("the six nearest offers span fewer", len(nearest) < len(reached))

k.section("2. Similar abandoned sessions in the user's category")
CAT = ["shoes", "bags", "watches"]
PRO = [unit([1, 0.2, 0, 0, 0, 0]), unit([0, 1, 0.3, 0, 0, 0]), unit([0, 0, 1, 0.4, 0, 0])]
sess = [[x + rng.gauss(0, 0.05) for x in PRO[i % 3]] for i in range(30)]
k.load("kit:rr:sess", sess)
for i in range(30):
    k.call("HSET", "kit:rr:cat", str(i), CAT[i % 3])
    k.call("HSET", "kit:rr:note", str(i), "follow up on %s" % CAT[i % 3])
    k.call("HSET", "kit:rr:id", str(i), "S-%02d" % i)
q = [x + rng.gauss(0, 0.04) for x in PRO[2]]
rec = k.js("FRACTAL.AGENT.RECALL_HYBRID", "kit:rr:sess", pack(q), "K", 3, "IDS", "kit:rr:id", "CONTENT", "kit:rr:note", "FILTER", "kit:rr:cat", "watches")
k.show("recalled sessions", [(x["mem_id"], x["content"]) for x in rec])
k.expect("only watches sessions are recalled, each with its follow-up note", all(int(x["mem_id"][2:]) % 3 == 2 and "watches" in x["content"] for x in rec))
k.expect("the best one agrees with brute force", int(rec[0]["mem_id"][2:]) == min((i for i in range(30) if i % 3 == 2), key=lambda i: cosd(sess[i], q)))

k.section("3. Diverse next-best offers")
div = k.js("FRACTAL.AGENT.RECOMMEND_DIVERSE", "kit:rr:offers", pack(user), "K", 6)
k.expect("six distinct offers", len({x["item_id"] for x in div}) == 6)

k.section("4. How many abandoned watches sessions?")
n = sum(1 for v in k.call("HGETALL", "kit:rr:cat")[1::2] if v == b"watches")
k.show("watches sessions (a key lookup)", n)
k.expect("the count the data holds", n == 10)
k.done()
