#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Telecom: anomalous account-change screening (SIM swap, port-out, plan change, unusual device or location).

A general-purpose pre-commit safety check, not a fraud-specific product: FRACTAL.AGENT.OUTLIER_INTERCEPT screens a PROPOSED event
against a history of known-anomalous change patterns (the same shape as the ADAS kit's known-bad-state history, applied to
account activity). Event vectors are [sim_change, port_out, new_device, geo_jump, off_hours] intensities in 0..1.
Cosine distance (the default) compares the SHAPE of an event, not its intensity; L2 compares both."""
from fvlib import Kit, pack, unit

k = Kit("telecom-account-anomaly-screening")
rng = k.rng
PATTERNS = [[0.9, 0.9, 0.7, 0.2, 0.8], [0.8, 0.1, 0.9, 0.9, 0.6], [0.2, 0.9, 0.8, 0.7, 0.9]]
hist = [[min(1.0, max(0.0, x + rng.gauss(0, 0.03))) for x in PATTERNS[i % 3]] for i in range(21)]
k.load("kit:telco:bad", hist)

k.section("1. Screen proposed account changes (cosine)")
swap = [0.88, 0.85, 0.72, 0.18, 0.78]                   # SIM swap + port-out + new device, off hours
plan_change = [0.0, 0.0, 0.0, 0.05, 0.0]                # a routine plan change
upgrade = [0.0, 0.0, 0.1, 0.0, 0.0]
a = k.js("FRACTAL.AGENT.OUTLIER_INTERCEPT", "kit:telco:bad", pack(swap), 0.05)
b = k.js("FRACTAL.AGENT.OUTLIER_INTERCEPT", "kit:telco:bad", pack(plan_change), 0.05)
k.show("nearest known-anomalous distance: swap / plan change", "%.4f / %.4f" % (a["nearest_distance"], b["nearest_distance"]))
k.expect("a SIM-swap + port-out burst is held", a["intercepted"] is True)
k.expect("a routine plan change passes", b["intercepted"] is False)

k.section("2. Shape vs intensity")
faint = [x * 0.15 for x in swap]                        # the same shape, a tenth of the intensity
c = k.js("FRACTAL.AGENT.OUTLIER_INTERCEPT", "kit:telco:bad", pack(faint), 0.05)
d = k.js("FRACTAL.AGENT.OUTLIER_INTERCEPT", "kit:telco:bad", pack(faint), 0.40, "METRIC", "l2")
k.show("faint copy: cosine / L2 nearest distance", "%.4f / %.3f" % (c["nearest_distance"], d["nearest_distance"]))
k.expect("cosine sees only the shape: the faint copy is still held", c["intercepted"] is True)
k.expect("L2 counts intensity: the faint copy is released", d["intercepted"] is False)
k.done()
