#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""RF spectrum sensing: a locked carrier that is jammed halfway through a capture.

FRACTAL.SERIES.PERIODOGRAM says WHICH frequency dominates; FRACTAL.SERIES.SPECTROGRAM
says HOW ORDERED the spectrum is, window by window (low = a few strong
frequencies, high = spread across the band); FRACTAL.SERIES.CHANGEPOINT then
localises the frame where order is lost."""
import math
from fvlib import Kit, pack

k = Kit("rf-spectrum-sensing")
rng = k.rng
N, PERIOD = 1024, 16

k.section("1. The capture")
carrier = [math.sin(2 * math.pi * i / PERIOD) + rng.gauss(0, 0.05) for i in range(N // 2)]
jammed = [rng.gauss(0, 1.0) for _ in range(N // 2)]
cap = carrier + jammed

k.section("2. Which frequency dominates the clean half")
pg = k.js("FRACTAL.SERIES.PERIODOGRAM", pack(carrier), 3)
k.show("top frequencies (cycles/sample)", ["%.4f" % f for f in pg["freqs"]])
k.expect("the carrier is found at 1/%d = %.4f" % (PERIOD, 1 / PERIOD), abs(pg["freqs"][0] - 1 / PERIOD) < 0.004)

k.section("3. How ordered is the spectrum, window by window")
ent = k.js("FRACTAL.SERIES.SPECTROGRAM", pack(cap), 64, 32, 64)
half = len(ent) // 2
before, after = ent[:half - 1], ent[half + 1:]
k.show("frames", len(ent))
k.show("mean entropy before / after the jam", "%.3f / %.3f" % (sum(before) / len(before), sum(after) / len(after)))
k.expect("every entropy value lies in [0, 1]", all(0 <= x <= 1 for x in ent))
k.expect("the locked carrier reads as ordered (mean < 0.5)", sum(before) / len(before) < 0.5)
k.expect("the jammed band reads as disordered (mean > 0.7)", sum(after) / len(after) > 0.7)

k.section("4. Where order was lost")
cp = k.js("FRACTAL.SERIES.CHANGEPOINT", pack(ent), 4, 3)["indices"]
k.show("change points in the entropy sequence (frame index)", cp)
k.expect("a change point lands within 3 frames of the jam", any(abs(i - half) <= 3 for i in cp), "jam at frame %d" % half)
k.done()
