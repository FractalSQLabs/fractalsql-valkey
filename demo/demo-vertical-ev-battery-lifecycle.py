#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Electric-vehicle fleet battery lifecycle: state of health (SoH) and thermal-regime monitoring.

A vehicle's intake baseline plays the patient's admission baseline: FRACTAL.AGENT.
PATIENT_DETERIORATION_TRIAGE finds the fleet vehicles whose SoH profile resembles this one's and how far it
has drifted from its own baseline. FRACTAL.AGENT.ANOMALY_TRIAGE watches one vehicle's temperature series for
a shift into a new regime (pre-thermal-runaway), independently of the SoH comparison."""
from fvlib import Kit, pack, cosd, unit, noise, ramp

k = Kit("ev-battery-lifecycle")
rng = k.rng
PROFILES = [unit([1.0, 0.9, 0.8, 0.7]), unit([1.0, 0.6, 0.3, 0.1]), unit([0.4, 0.4, 0.9, 0.9])]
fleet = [[x + rng.gauss(0, 0.03) for x in PROFILES[i % 3]] for i in range(45)]
k.load("kit:ev:fleet", fleet)
baseline = fleet[1]
current = [x - 0.25 * y + rng.gauss(0, 0.01) for x, y in zip(baseline, PROFILES[1])]   # capacity fade along profile 1

k.section("1. Who looks like this vehicle, and how far has it drifted?")
r = k.js("FRACTAL.AGENT.PATIENT_DETERIORATION_TRIAGE", "kit:ev:fleet", pack(current), pack(baseline), pack(current), "K", 5)
ids = [m["id"] for m in r["cohort_matches"]]
k.show("nearest fleet vehicles", ids)
k.expect("the closest fleet vehicles share this vehicle's degradation profile (profile 1)", all(i % 3 == 1 for i in ids), str(ids))
brute = min(range(45), key=lambda i: cosd(fleet[i], current))
k.expect("the nearest vehicle agrees with brute force", r["nearest_cohort_id"] == brute, "%s vs %s" % (r["nearest_cohort_id"], brute))
k.expect("the drift distance is a finite cosine distance", 0 <= r["drift_distance"] <= 2)

k.section("2. Thermal regime of one vehicle")
healthy = noise(rng, 96, 1.0)
runaway = ramp(rng, 96, 48, rate=1 / 40.0)
k.call("SET", "kit:ev:t:ok", pack(healthy))
k.call("SET", "kit:ev:t:hot", pack(runaway))
ok = k.js("FRACTAL.AGENT.ANOMALY_TRIAGE", "kit:ev:t:ok", 32)
hot = k.js("FRACTAL.AGENT.ANOMALY_TRIAGE", "kit:ev:t:hot", 32)
k.show("|threat score|: healthy / pre-runaway", "%.3f / %.3f" % (abs(ok["threat_score"]), abs(hot["threat_score"])))
k.expect("the pre-runaway pack scores above the healthy one", abs(hot["threat_score"]) > abs(ok["threat_score"]))
k.done()
