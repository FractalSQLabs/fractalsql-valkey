#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Avionics and flight-path fault prediction across a fleet (3-D tracks).

Twelve flights each have a planned path and a latest state, [x_km, y_km, altitude_km]. AC-09 has a stuck
control surface: its path has drifted off plan and its heading changes become erratic for a stretch.
FRACTAL.AGENT.TRACK_ANOMALY finds which fleet deviation signature matches the change and reports the
heading series' DFA exponent (erratic heading changes persist, so the exponent rises)."""
from fvlib import Kit, pack, cosd, noise, walk

k = Kit("avionics-fault-prediction")
rng = k.rng
planned = [[rng.uniform(0, 400), rng.uniform(0, 400), rng.uniform(8, 12)] for _ in range(12)]
dev = [[rng.gauss(0, 0.5), rng.gauss(0, 0.5), rng.gauss(0, 0.05)] for _ in range(12)]
dev[9] = [6.0, -4.0, 0.9]                                   # AC-09's off-plan drift
latest = [[p + d for p, d in zip(planned[i], dev[i])] for i in range(12)]
k.load("kit:avionics:dev", dev)
calm = noise(rng, 96, 0.1)
erratic = noise(rng, 40, 0.1) + [x * 0.1 for x in walk(rng, 56, 1.0)]
k.call("SET", "kit:avionics:h01", pack(calm))
k.call("SET", "kit:avionics:h09", pack(erratic))

k.section("1. Which flight's deviation does AC-09's change match?")
r = k.js("FRACTAL.AGENT.TRACK_ANOMALY", "kit:avionics:dev", pack(planned[9]), pack(latest[9]), "kit:avionics:h09")
k.show("nearest fleet deviation / its distance", "%s / %.5f" % (r["nearest_fleet_id"], r["trajectory_distance"]))
k.expect("the change matches flight 9's own deviation signature", r["nearest_fleet_id"] == 9 and r["trajectory_distance"] < 1e-3, str(r))
quiet = k.js("FRACTAL.AGENT.TRACK_ANOMALY", "kit:avionics:dev", pack(planned[1]), pack(latest[1]), "kit:avionics:h01")
k.expect("a flight that stayed on plan matches its own small deviation", quiet["nearest_fleet_id"] == 1, str(quiet))

k.section("2. Erratic heading changes")
k.show("heading DFA exponent: AC-09 / AC-01", "%.3f / %.3f" % (r["dfa_exponent"], quiet["dfa_exponent"]))
k.expect("the agent's exponent is FRACTAL.DIM.DFA's value", abs(r["dfa_exponent"] - float(k.call("FRACTAL.DIM.DFA", pack(erratic)))) < 1e-9)
k.expect("AC-09's erratic stretch persists more than AC-01's steady heading", r["dfa_exponent"] > quiet["dfa_exponent"] + 0.2)
k.done()
