#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Intent matching: signal clustering and diverse candidate selection.

Twenty-four target entities fall into four intent clusters (category + stage signal vectors), and there is a
thirty-item content library. Scout (FRACTAL.EXPLORE, the diversity-first search) picks entities from across
the clusters around a target profile instead of six near-duplicates, and FRACTAL.AGENT.RECOMMEND_DIVERSE returns a
diverse content set for the chosen entity. No model is needed for either."""
from fvlib import Kit, pack, dist, cosd

k = Kit("intent-matching-discovery")
rng = k.rng
C = [[rng.uniform(-6, 6) for _ in range(8)] for _ in range(4)]
ents = [[x + rng.gauss(0, 0.35) for x in C[i % 4]] for i in range(24)]
cat = [i % 4 for i in range(24)]
k.load("kit:intent:ents", ents)
content = [[x + rng.gauss(0, 0.3) for x in C[i % 4]] for i in range(30)]
k.load("kit:intent:content", content)
ideal = [sum(col) / 4 for col in zip(*C)]

k.section("1. Discovery around the ideal profile")
pop = k.js("FRACTAL.EXPLORE", "kit:intent:ents", pack(ideal), 150)["population"]
pts = [p if isinstance(p, list) else p.get("point", p.get("x")) for p in pop]
picked = {min(range(24), key=lambda i: dist(ents[i], p)) for p in pts}
cats = {cat[i] for i in picked}
top = [t["idx"] for t in k.js("FRACTAL.SEARCH", "kit:intent:ents", pack(ideal), 6)["top_k"]]
k.show("entities / categories reached by discovery", "%d / %s" % (len(picked), sorted(cats)))
k.show("categories reached by the 6 nearest", sorted({cat[i] for i in top}))
k.expect("discovery spans at least 3 of the 4 intent categories", len(cats) >= 3, str(sorted(cats)))
k.expect("it spans more categories than the six nearest entities do", len(cats) > len({cat[i] for i in top}))

k.section("2. Diverse content for the chosen entity")
target = min(picked)
rec = k.js("FRACTAL.AGENT.RECOMMEND_DIVERSE", "kit:intent:content", pack(ents[target]), "K", 6)
k.show("content items", [x["item_id"] for x in rec])
k.expect("six distinct content items", len({x["item_id"] for x in rec}) == 6)
k.expect("the best item is the brute-force nearest", rec[0]["item_id"] == min(range(30), key=lambda i: cosd(content[i], ents[target])))
k.done()
