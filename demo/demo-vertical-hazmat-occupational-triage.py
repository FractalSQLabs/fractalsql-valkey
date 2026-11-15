#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Occupational exposure monitoring: hazmat response, plant crews, wearable-sensor workforce safety.

FRACTAL.AGENT.PATIENT_DETERIORATION_TRIAGE has a domain-neutral contract (a cohort search plus a baseline-to-
current drift), so the kit only supplies exposure columns [dose_rate_z, body_temp_z, air_quality_z,
heart_rate_z] in place of clinical vitals, and a worker's pre-shift baseline in place of an admission baseline.
The cohort is one crew, selected with FILTER on a companion hash."""
from fvlib import Kit, pack, cosd, unit

k = Kit("hazmat-occupational-triage")
rng = k.rng
PROFILES = [unit([1, 0.2, 0.1, 0.1]), unit([0.1, 1, 0.2, 0.9]), unit([0.2, 0.1, 1, 0.3])]   # heat load, air-quality load, dose
workers = [[x + rng.gauss(0, 0.04) for x in PROFILES[i % 3]] for i in range(36)]
k.load("kit:hazmat:workers", workers)
for i in range(36):
    k.call("HSET", "kit:hazmat:crew", str(i), "alpha" if i < 18 else "bravo")
    k.call("HSET", "kit:hazmat:ids", str(i), "W-%02d" % i)
baseline = workers[4]
current = [x + 0.5 * p + rng.gauss(0, 0.01) for x, p in zip(baseline, PROFILES[1])]      # heat load climbing during the shift

k.section("1. Who has had a similar exposure, and how far has this worker moved?")
r = k.js("FRACTAL.AGENT.PATIENT_DETERIORATION_TRIAGE", "kit:hazmat:workers", pack(current), pack(baseline), pack(current),
         "K", 4, "IDS", "kit:hazmat:ids")
k.show("nearest workers", [m["id"] for m in r["cohort_matches"]])
k.expect("the closest workers share the heat-load profile", all(int(m["id"][2:]) % 3 == 1 for m in r["cohort_matches"]))
k.expect("the drift away from the pre-shift baseline is reported", r["drift_distance"] >= 0)

k.section("2. Restrict the cohort to one crew")
bravo = k.js("FRACTAL.AGENT.PATIENT_DETERIORATION_TRIAGE", "kit:hazmat:workers", pack(current), pack(baseline), pack(current),
             "K", 4, "IDS", "kit:hazmat:ids", "FILTER", "kit:hazmat:crew", "bravo")
ids = [int(m["id"][2:]) for m in bravo["cohort_matches"]]
k.show("nearest workers in crew bravo", ids)
k.expect("every match is in crew bravo", all(i >= 18 for i in ids))
best = min(range(18, 36), key=lambda i: cosd(workers[i], current))
k.expect("the nearest bravo worker agrees with brute force", int(bravo["nearest_cohort_id"][2:]) == best)
k.done()
