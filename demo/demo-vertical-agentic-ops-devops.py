#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""DevOps / SRE: dispatch, loop detection and safety checks for an autonomous operator.

FRACTAL.AGENT.DETECT_LOOP catches a stuck remediation loop (a period-2 toggle that the DFA exponent alone cannot see);
FRACTAL.AGENT.ANOMALY_TRIAGE scores a drifting latency series; FRACTAL.AGENT.OUTLIER_INTERCEPT screens a proposed command against
known-bad runbook states; FRACTAL.AGENT.ROUTE_TASK dispatches an incident to the best-matching capability. The
retrieve-then-reason step (RAG_AGENT) needs an embedding provider and is skipped without one."""
from fvlib import Kit, pack, cosd, unit, noise, ramp

k = Kit("agentic-ops-devops")
rng = k.rng

k.section("1. A stuck remediation loop")
toggle = [unit([1, 0]) if i % 2 == 0 else unit([0, 1]) for i in range(30)]       # restart <-> rollback, forever
k.call("SET", "kit:ops:loop", pack([x for s in toggle for x in s]))
r = k.js("FRACTAL.AGENT.DETECT_LOOP", "remediator-1", "kit:ops:loop", 2)
k.show("loop detected / DFA exponent", "%s / %s" % (r["is_loop_detected"], r["dfa_exponent"]))
k.expect("the period-2 toggle is flagged through the fingerprint cycle", r["is_loop_detected"] is True)
k.expect("its DFA exponent is 0, so the cycle check is what caught it", r["dfa_exponent"] == 0)

k.section("2. Drifting latency")
k.call("SET", "kit:ops:lat:ok", pack(noise(rng, 96, 1.0)))
k.call("SET", "kit:ops:lat:bad", pack(ramp(rng, 96, 48, rate=1 / 40.0)))
ok = k.js("FRACTAL.AGENT.ANOMALY_TRIAGE", "kit:ops:lat:ok", 32)
bad = k.js("FRACTAL.AGENT.ANOMALY_TRIAGE", "kit:ops:lat:bad", 32)
k.expect("the drifting service scores above the steady one", abs(bad["threat_score"]) > abs(ok["threat_score"]))

k.section("3. Screen a proposed command")
known_bad = [[x + rng.gauss(0, 0.05) for x in unit([1, 0.3, 0, 0, 0])] for _ in range(12)]
k.load("kit:ops:bad", known_bad)
risky = k.js("FRACTAL.AGENT.OUTLIER_INTERCEPT", "kit:ops:bad", pack(unit([1, 0.3, 0, 0.02, 0])), 0.05)
safe = k.js("FRACTAL.AGENT.OUTLIER_INTERCEPT", "kit:ops:bad", pack(unit([0, 0, 0, 1, 0.4])), 0.05)
k.expect("a command resembling a known-bad runbook state is held, an unrelated one is not", risky["intercepted"] is True and safe["intercepted"] is False)

k.section("4. Dispatch an incident")
caps = [unit([rng.uniform(-1, 1) for _ in range(6)]) for _ in range(6)]
k.load("kit:ops:caps", caps)
inc = [x + rng.gauss(0, 0.02) for x in caps[4]]
d = k.js("FRACTAL.AGENT.ROUTE_TASK", "kit:ops:caps", pack(inc), 600)
k.expect("the incident goes to capability 4 and spends the default 150 tokens", d["routed_to"] == "4" and d["remaining_budget"] == 450)

k.section("5. Retrieve then reason")
rag = k.optional_js("RAG_AGENT (embeds the question)", "FRACTAL.RAG.AGENT", "kit:ops:caps", "why did the checkout latency rise?", 6)
if rag is not None:
    k.expect("the answer cites source rows", len(rag["source_row_ids"]) >= 1)
k.done()
