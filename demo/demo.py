#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""The 60-second demo: discovery, analytics and one agent, with no model.

Store a small clustered corpus under one key, then ask the three questions the rest of the product builds on:
what is closest (Sniper), what distinct things are here (Scout), and what does the shape of this series say.
Every step checks itself, so the demo doubles as a smoke test. Reasoning, if a fractalsqld daemon is
configured, is the last step and is skipped otherwise."""
import sys, os
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__))))
from fvlib import Kit, FvError, pack

txt = lambda v: v.decode() if isinstance(v, bytes) else v

k = Kit("demo")
rng = k.rng

k.section("0. Is the module here?")
k.show("version", txt(k.call("FRACTAL.VERSION")))
k.show("edition", txt(k.call("FRACTAL.EDITION")))

k.section("1. A corpus is rows of floats under one key")
centres = {"billing": [1, 0, 0, 0, 0, 0], "shipping": [0, 1, 0, 0, 0, 0], "login": [0, 0, 1, 0, 0, 0]}
rows, names = [], []
for name, c in centres.items():
    for _ in range(20):
        rows.append([x + rng.gauss(0, 0.06) for x in c])
        names.append(name)
k.load("demo:docs", rows)
k.show("rows x dim", "%d x %d (%d bytes)" % (len(rows), 6, len(rows) * 6 * 4))

k.section("2. Sniper: what is closest?")
q = [0.1, 0.95, 0.05, 0, 0, 0]
res = k.js("FRACTAL.SEARCH", "demo:docs", pack(q), 3)
tops = [names[t["idx"]] for t in res["top_k"]]
k.show("top 3", ", ".join("%s (dist %.3f)" % (names[t["idx"]], t["dist"]) for t in res["top_k"]))
k.expect("all three nearest rows are shipping rows", tops == ["shipping"] * 3, str(tops))

k.section("3. Scout: what distinct things are here?")
res = k.js("FRACTAL.EXPLORE", "demo:docs", pack([0.4, 0.4, 0.4, 0, 0, 0]), 60)
pop = res["population"]
k.show("population returned", len(pop))
k.expect("a population of 60 was returned", len(pop) == 60, str(len(pop)))

k.section("4. Out-of-range arguments are refused, not adjusted")
for args, want in (((0,), "K 0 out of range"), ((99999,), "K 99999 out of range")):
    try:
        k.call("FRACTAL.SEARCH", "demo:docs", pack(q), *args); err = ""
    except Exception as e:
        err = str(e)
    k.expect("SEARCH K=%d is refused" % args[0], want in err, err)

k.section("5. Vector math is a command too")
a, b = [1, 2, 3], [4, 5, 6]
k.expect("dot = 32", abs(float(k.call("FRACTAL.VECTOR.DOT", pack(a), pack(b))) - 32.0) < 1e-6)
k.expect("cosine distance of a vector with itself is 0", abs(float(k.call("FRACTAL.VECTOR.COSINE", pack(a), pack(a)))) < 1e-5)

k.section("6. Analytics: is this series persistent or noise?")
walk = [0.0]
for _ in range(255):
    walk.append(walk[-1] + rng.gauss(0, 1))
noise = [rng.gauss(0, 1) for _ in range(256)]
aw = float(k.call("FRACTAL.DIM.DFA", pack(walk)))
an = float(k.call("FRACTAL.DIM.DFA", pack(noise)))
k.show("DFA exponent: random walk / white noise", "%.2f / %.2f" % (aw, an))
k.expect("the walk scores clearly higher than the noise", aw > an + 0.4, "%.2f vs %.2f" % (aw, an))

k.section("7. One agent, computed first")
series = [x + (((i - 120) ** 2) / 40.0 if i > 120 else 0.0) for i, x in enumerate(noise * 1)]
k.call("SET", "demo:series", pack(series))
ag = k.js("FRACTAL.AGENT.ANOMALY_TRIAGE", "demo:series", 64)
k.show("threat_score / anomaly_type", "%s / %s" % (ag.get("threat_score"), ag.get("anomaly_type")))
k.expect("the agent computed a threat score (and a model summary, or a clear note that no model is configured)", "threat_score" in ag and ("triage_summary" in ag or "reasoning_step_failed" in ag), str(list(ag))[:80])

k.section("8. Reasoning, if a daemon is configured")
try:
    k.show("FRACTAL.REASON", txt(k.call("FRACTAL.REASON", "Say hello in five words."))[:60])
except FvError as e:
    k.show("FRACTAL.REASON", "skipped: %s" % str(e)[:60])
k.done()
