#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""A demo of the agents, computed half only.

Each agent fetches its inputs from keys, computes with the analytics primitives, and only then asks a model
for a narrative. This demo runs a representative agent from each family with no model, so the number is
always there; a "reasoning_step_failed" field says the narrative was skipped, not that the agent failed.
With a fractalsqld daemon configured the narratives appear as well."""
import sys, os
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__))))
from fvlib import Kit, pack

k = Kit("demo-agents")
rng = k.rng
A = "FRACTAL.AGENT."

rows = [[1 + rng.gauss(0, .05), rng.gauss(0, .05), 0, 0] for _ in range(20)] + [[rng.gauss(0, .05), 1 + rng.gauss(0, .05), 0, 0] for _ in range(20)]
k.load("demo:cohort", rows)
k.call("SET", "demo:calm", pack([rng.gauss(0, 1) for _ in range(192)]))
k.call("SET", "demo:shift", pack([rng.gauss(0, 1) + (((i - 120) ** 2) / 40.0 if i > 120 else 0.0) for i in range(192)]))
q = pack([1, 0.1, 0, 0])

k.section("Series -> a decision")
r = k.js(A + "REGIME_TRIAGE", "demo:calm", 64)
k.show("REGIME_TRIAGE on a calm series", "dfa %.2f, drift_detected=%s" % (r["dfa_exponent"], r["drift_detected"]))
k.expect("a calm series shows no regime change", r["drift_detected"] is False)
r = k.js(A + "ANOMALY_TRIAGE", "demo:shift", 64)
k.show("ANOMALY_TRIAGE on a drifting series", "threat %s, %s" % (r["threat_score"], r["anomaly_type"]))
k.expect("a drifting series scores above zero", float(r["threat_score"]) > 0)

k.section("Cohort -> allocation")
mu = pack([.10, .12, .08, .15])
cov = pack([0.04 if i == j else 0.002 for i in range(4) for j in range(4)])
r = k.js(A + "ALLOCATE", mu, cov, 2)
w = r["allocation"]["weights"]
k.show("ALLOCATE two of four assets", "weights %s" % [round(x, 3) for x in w])
k.expect("exactly two assets carry weight", sum(1 for x in w if x > 0) == 2 and abs(sum(w) - 1) < 1e-6)

k.section("Retrieval (no model involved)")
r = k.js(A + "RECALL_HYBRID", "demo:cohort", q, "K", 3)
k.expect("recall returns three rows", len(r) == 3)
r = k.js(A + "RECOMMEND_DIVERSE", "demo:cohort", q, "K", 3)
k.show("RECOMMEND_DIVERSE", ", ".join("%s (%.3f)" % (x["item_id"], x["score"]) for x in r))
k.expect("recommend returns three items", len(r) == 3)

k.section("Interception and loops")
r = k.js(A + "OUTLIER_INTERCEPT", "demo:cohort", pack(rows[3]), "0.05")
k.expect("a state identical to a known one is intercepted", r["intercepted"] is True)
r = k.js(A + "OUTLIER_INTERCEPT", "demo:cohort", pack([0, 0, 1, 0]), "0.05")
k.expect("a state unlike anything known is not", r["intercepted"] is False)

k.section("Diversification, proven")
r = k.js(A + "FEEDBACK_AUDIT", "demo:cohort", q, "demo:cohort")
k.show("FEEDBACK_AUDIT", "diversity_quotient %s, enabled %s" % (r["diversity_quotient"], r["explanation"]["diversify_enabled"]))
k.expect("the diversify layer ran", r["explanation"]["diversify_enabled"] is True)

k.section("A question over your hashes (needs a model; valkey-search is optional)")
for i, (age, name, city) in enumerate([(25, "ann", "london"), (31, "bob", "paris"), (47, "cy", "london"), (52, "di", "berlin")]):
    k.call("HSET", "demo:people:%d" % i, "age", age, "name", name, "city", city)
# MODE SCAN never needs valkey-search: the model writes a small JSON filter and the module scans the keys under the prefix, as you.
r = k.optional_js("DATA_ANALYST in scan mode", A + "DATA_ANALYST", "demo:people:", "who is older than thirty?", "MODE", "SCAN")
if r is not None:
    k.show("the filter the model wrote", r["generated_query"])
    k.show("rows", ", ".join(sorted(x["key"] for x in r["result_json"]["rows"])) or "(none)")
    k.expect("the scan engine answered and read all four hashes", r["engine"] == "scan" and r["result_json"]["scanned"] == 4 and r["result_json"]["complete"] is True)
    k.expect("every row is one of the hashes under the prefix", all(x["key"].startswith("demo:people:") for x in r["result_json"]["rows"]))

k.section("The narrative, if a daemon is configured")
r = k.js(A + "REGIME_TRIAGE", "demo:shift", 64)
k.show("rationale", (r.get("rationale") or ("skipped: " + str(r.get("reasoning_step_failed", ""))[:50]))[:70])
k.done()
