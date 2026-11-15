#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Industrial emissions and utility-leak detection from a sensor network.

Six sensors (methane, CO2, water, one pipeline-pressure sensor) each report 96 readings. A leak is an
ACCELERATING ramp downstream of the source: DFA removes a constant offset, so only a change in the series'
dynamics moves the exponent. FRACTAL.AGENT.ANOMALY_TRIAGE scores every sensor with the same call; the
agent is indifferent to the physical quantity."""
from fvlib import Kit, pack, noise, ramp

k = Kit("climatech-emissions-monitoring")
rng = k.rng
NAMES = ["CH4-01", "CH4-02", "CO2-03", "CO2-04", "H2O-05", "PRS-07"]
LEAKS = {"CH4-02": 1.0, "PRS-07": -1.0}                     # a pressure sensor drops as the pipe leaks
series = {}
for n in NAMES:
    base = noise(rng, 96, 1.0)
    if n in LEAKS:
        base = ramp(rng, 96, 48, rate=LEAKS[n] / 40.0)
    series[n] = base
    k.call("SET", "kit:emis:" + n, pack(base))

k.section("1. Score every sensor")
score = {}
for n in NAMES:
    r = k.js("FRACTAL.AGENT.ANOMALY_TRIAGE", "kit:emis:" + n, 32)
    score[n] = abs(r["threat_score"])
    k.show(n, "%.3f" % score[n])
    ref = k.js("FRACTAL.DIM.DRIFT", pack(series[n]), 32)["drift"]
    k.expect("%s: the agent's score is FRACTAL.DIM.DRIFT's drift" % n, abs(r["threat_score"] - ref) < 1e-9)
leaky = sorted(score, key=score.get, reverse=True)[:2]
k.expect("the two leaking sensors score highest", set(leaky) == set(LEAKS), str(leaky))
k.expect("every calm sensor scores below both leaking ones",
         max(score[n] for n in NAMES if n not in LEAKS) < min(score[n] for n in LEAKS))
k.done()
