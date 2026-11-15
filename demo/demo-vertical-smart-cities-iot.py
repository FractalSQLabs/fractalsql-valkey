#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Smart cities: sensor-grid coverage, a sensor whose readings change regime, zone sampling.

A 400-sensor jittered 20x20 grid. FRACTAL.MORPH.COMPLEXITY scores the spatial
coverage; FRACTAL.DIM.DRIFT finds the one sensor whose series shifted (an
air-quality event); FRACTAL.EXPLORE samples representative zones."""
from fvlib import Kit, pack, dist, flat

k = Kit("smart-cities-iot")
rng = k.rng

k.section("1. Spatial coverage of the grid")
grid = [(i + rng.uniform(-0.35, 0.35), j + rng.uniform(-0.35, 0.35)) for i in range(20) for j in range(20)]
m = k.js("FRACTAL.MORPH.COMPLEXITY", pack(flat(grid)), 2)
k.show("dimension / lacunarity", "%.3f / %.3f" % (m["dimension"], m["lacunarity"]))
k.expect("a dense jittered grid reads as area-filling (dimension about 2)", 1.5 < m["dimension"] < 2.3)
clump = [(rng.gauss(10, 0.8), rng.gauss(10, 0.8)) for _ in range(400)]
mc = k.js("FRACTAL.MORPH.COMPLEXITY", pack(flat(clump)), 2)
k.show("lacunarity: even grid / clumped sensors", "%.3f / %.3f" % (m["lacunarity"], mc["lacunarity"]))
k.expect("clumped placement is gappier (higher lacunarity) than the even grid", mc["lacunarity"] > m["lacunarity"])

k.section("2. The sensor whose readings changed regime")
def series(shift):
    # DFA measures correlation structure, so the event is a change in persistence:
    # uncorrelated readings that turn into a drifting excursion (a random walk).
    s = [40 + rng.gauss(0, 3) for _ in range(170)]
    acc = s[-1]
    for _ in range(86):
        if shift:
            acc += rng.gauss(0, 3)
            s.append(acc)
        else:
            s.append(40 + rng.gauss(0, 3))
    return s
ev = abs(k.js("FRACTAL.DIM.DRIFT", pack(series(True)), 64)["drift"])
ctl = [abs(k.js("FRACTAL.DIM.DRIFT", pack(series(False)), 64)["drift"]) for _ in range(6)]
k.show("|drift| event sensor / quietest, noisiest control", "%.3f / %.3f, %.3f" % (ev, min(ctl), max(ctl)))
k.expect("the event sensor drifts more than every control sensor", ev > max(ctl))

k.section("3. Representative zones")
zones = [[40, 10, 55], [90, 60, 70], [20, 5, 35]]
profile = [[z + rng.gauss(0, 3) for z in zones[i % 3]] for i in range(400)]
k.load("kit:zones", profile)
pop = k.js("FRACTAL.EXPLORE", "kit:zones", pack(zones[0]), 150)["population"]
pts = [p if isinstance(p, list) else p.get("point", p.get("x")) for p in pop]
kinds = {min(range(3), key=lambda z: dist(zones[z], p)) for p in pts}
k.show("zone types the population represents", sorted(kinds))
k.expect("representative sampling reaches more than one zone type", len(kinds) >= 2, str(sorted(kinds)))
k.done()
