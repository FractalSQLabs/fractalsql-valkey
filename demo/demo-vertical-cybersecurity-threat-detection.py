#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Cybersecurity: one host out of 35 starts beaconing to a command-and-control server.

Each host has a baseline and a current traffic vector (connection volume, unique
destination ports, DNS rate, failed-auth rate). The compromised host changes on
the first three but not the fourth, which is a beaconing profile, not a
brute-force attempt. Found by similarity search, confirmed by DFA drift, and its
beacon cadence read from the periodogram."""
import math
from fvlib import Kit, pack, dist

k = Kit("cybersecurity-threat-detection")
rng = k.rng
ZONES = ["dmz", "corp", "lab"]
hosts = []
for i in range(35):
    zone = ZONES[i % 3]
    base = [rng.uniform(40, 60), rng.uniform(5, 12), rng.uniform(20, 40), rng.uniform(0.5, 1.5)]
    hosts.append({"zone": zone, "base": base, "cur": [x * rng.uniform(0.95, 1.08) for x in base]})
BAD = 22
hosts[BAD]["cur"] = [hosts[BAD]["base"][0] * 3.1, hosts[BAD]["base"][1] * 4.0, hosts[BAD]["base"][2] * 3.6, hosts[BAD]["base"][3] * 1.05]

# normalise each dimension by the fleet baseline so no one unit dominates
dimscale = [sum(h["base"][d] for h in hosts) / len(hosts) for d in range(4)]
norm = lambda v: [x / s for x, s in zip(v, dimscale)]

k.section("1. Hunt by the beaconing signature")
signature = norm([hosts[BAD]["base"][0] * 3.0, hosts[BAD]["base"][1] * 4.0, hosts[BAD]["base"][2] * 3.5, hosts[BAD]["base"][3]])
k.load("kit:cur", [norm(h["cur"]) for h in hosts])
top = k.js("FRACTAL.SEARCH", "kit:cur", pack(signature), 3)["top_k"]
k.show("nearest hosts to the signature", [(t["idx"], hosts[t["idx"]]["zone"]) for t in top])
k.expect("the compromised host is the nearest match", top[0]["idx"] == BAD)

k.section("2. Diverse traffic profiles across the fleet")
pop = k.js("FRACTAL.EXPLORE", "kit:cur", pack(norm(hosts[0]["cur"])), 24)["population"]
k.expect("the discovery run returns its full population", len(pop) == 24)

k.section("3. Did this host's connection rate change regime")
def conn_series(bad):
    s = [50 + rng.gauss(0, 4) for _ in range(256)]
    if bad:
        for i in range(160, 256):
            s[i] += 90 * (1 if (i % 12) < 2 else 0.1)        # quiet, then bursts every 12 ticks
    return s
bad_d = abs(k.js("FRACTAL.DIM.DRIFT", pack(conn_series(True)), 64)["drift"])
ok_d = [abs(k.js("FRACTAL.DIM.DRIFT", pack(conn_series(False)), 64)["drift"]) for _ in range(5)]
k.show("|drift| compromised / healthy (max of 5)", "%.3f / %.3f" % (bad_d, max(ok_d)))
k.expect("the compromised host drifts more than any healthy one", bad_d > max(ok_d))

k.section("4. The beacon's cadence")
beacon = [(1.0 if i % 12 == 0 else 0.0) + rng.gauss(0, 0.05) for i in range(240)]
pg = k.js("FRACTAL.SERIES.PERIODOGRAM", pack(beacon), 1)
k.show("beacon period", "%.1f ticks" % (1 / pg["freqs"][0]))
k.expect("the beacon repeats every 12 ticks", abs(1 / pg["freqs"][0] - 12) < 0.8)
k.done()
