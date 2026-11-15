#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Agent loop detection: notice that an autonomous agent is going around in circles.

Each step's state vector is reduced to a 64-bit SimHash fingerprint
(FRACTAL.SERIES.FINGERPRINT), so nearly-identical states give nearly-identical
fingerprints. FRACTAL.CYCLE.DETECT runs cycle detection over the fingerprint
stream, tolerating a few differing bits, which an exact-hash check cannot do
(floating-point noise defeats it). A healthy trace never repeats; a stuck agent
revisits the same three states over and over."""
from fvlib import Kit, pack

k = Kit("agent-loop-detection")
rng, DIM, NBYTES = k.rng, 16, 8

def state():
    return [rng.gauss(0, 1) for _ in range(DIM)]

def fingerprints(trace):
    return b"".join(k.call("FRACTAL.SERIES.FINGERPRINT", pack(s), 64, 1) for s in trace)

k.section("1. Fingerprints tolerate noise but separate states")
A, B = state(), state()
fa, fa2 = (k.call("FRACTAL.SERIES.FINGERPRINT", pack(s), 64, 1) for s in (A, [x + rng.gauss(0, 0.01) for x in A]))
fb = k.call("FRACTAL.SERIES.FINGERPRINT", pack(B), 64, 1)
same, other = int(k.call("FRACTAL.VECTOR.HAMMING", fa, fa2)), int(k.call("FRACTAL.VECTOR.HAMMING", fa, fb))
k.show("bits differing: same state + noise / different state", "%d / %d" % (same, other))
k.expect("a noisy revisit of a state is within a few bits", same <= 6)
k.expect("a different state is far", other > 16)

k.section("2. A healthy agent never repeats")
# (Fingerprints capture a state's DIRECTION. A random walk's consecutive states
# point almost the same way, which correctly reads as "the same state", so a
# trace that is exploring uses states that are genuinely different.)
walk = [state() for _ in range(120)]
healthy = k.js("FRACTAL.CYCLE.DETECT", fingerprints(walk), NBYTES, 6)
k.show("verdict", healthy)
k.expect("no loop is reported for a healthy trace", healthy == {"detected": False})

k.section("3. A stuck agent revisits the same three states")
cycle = [state() for _ in range(3)]
trace = list(walk[:40])
for i in range(60):
    trace.append([x + rng.gauss(0, 0.01) for x in cycle[i % 3]])
stuck = k.js("FRACTAL.CYCLE.DETECT", fingerprints(trace), NBYTES, 6)
k.show("verdict", stuck)
k.expect("the loop is detected", stuck.get("detected") is True)
k.expect("its length is 3", stuck.get("cycle_len") == 3, str(stuck))
k.expect("it is caught after the loop begins (step 40) and well before the end", 40 <= stuck.get("at_index", -1) < 70, str(stuck))
k.done()
