#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Heavy machinery and fleet predictive maintenance: marine vessels, mining equipment, powertrains.

An asset plays the patient. FRACTAL.AGENT.ANOMALY_TRIAGE watches one asset's vibration series for a shift into a
pre-failure regime; FRACTAL.AGENT.PATIENT_DETERIORATION_TRIAGE compares the asset's current state against its
intake baseline within the same asset class (FILTER on a companion hash)."""
from fvlib import Kit, pack, cosd, unit, noise, ramp

k = Kit("heavy-industry-predictive-maintenance")
rng = k.rng
CLASSES = ["diesel", "turbine", "gearbox"]
PROFILE = [unit([1, 0.3, 0.1, 0.1, 0]), unit([0.1, 1, 0.4, 0, 0.2]), unit([0, 0.2, 1, 0.5, 0.1])]
assets = [[x + rng.gauss(0, 0.03) for x in PROFILE[i % 3]] for i in range(30)]
k.load("kit:heavy:assets", assets)
for i in range(30):
    k.call("HSET", "kit:heavy:class", str(i), CLASSES[i % 3])
    k.call("HSET", "kit:heavy:ids", str(i), "A-%02d" % i)

k.section("1. Vibration regime of two assets")
steady = noise(rng, 96, 1.0)
failing = ramp(rng, 96, 48, rate=1 / 40.0)
k.call("SET", "kit:heavy:vib:ok", pack(steady))
k.call("SET", "kit:heavy:vib:bad", pack(failing))
ok = k.js("FRACTAL.AGENT.ANOMALY_TRIAGE", "kit:heavy:vib:ok", 32)
bad = k.js("FRACTAL.AGENT.ANOMALY_TRIAGE", "kit:heavy:vib:bad", 32)
k.show("|threat score|: steady / pre-failure", "%.3f / %.3f" % (abs(ok["threat_score"]), abs(bad["threat_score"])))
k.expect("the pre-failure asset scores above the steady one", abs(bad["threat_score"]) > abs(ok["threat_score"]))

k.section("2. Same-class comparison against intake baseline")
base = assets[7]                                   # a gearbox-like asset (profile 1)
cur = [x + 0.4 * p + rng.gauss(0, 0.01) for x, p in zip(base, PROFILE[1])]
r = k.js("FRACTAL.AGENT.PATIENT_DETERIORATION_TRIAGE", "kit:heavy:assets", pack(cur), pack(base), pack(cur), "K", 4,
         "IDS", "kit:heavy:ids", "FILTER", "kit:heavy:class", "turbine")
ids = [int(m["id"][2:]) for m in r["cohort_matches"]]
k.show("nearest turbines", ids)
k.expect("the cohort holds only turbines (asset class 1)", all(i % 3 == 1 for i in ids))
k.expect("the nearest turbine agrees with brute force",
         int(r["nearest_cohort_id"][2:]) == min((i for i in range(30) if i % 3 == 1), key=lambda i: cosd(assets[i], cur)))
k.done()
