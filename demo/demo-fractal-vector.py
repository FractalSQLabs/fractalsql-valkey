#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Vector math as commands: distance, normalisation, text round trips and the compact forms.

Vectors are packed little-endian float32 strings. Everything here runs in the server process; nothing needs
a model. The last section shows the cheap-filter-then-rerank pattern the compact forms exist for."""
import sys, os, math
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__))))
from fvlib import Kit, pack, unpack

k = Kit("demo-fractal-vector")
f = lambda *a: float(k.call(*a))
a, b = [1.0, 2.0, 3.0], [4.0, 5.0, 6.0]

k.section("Distances")
k.expect("dot = 32", abs(f("FRACTAL.VECTOR.DOT", pack(a), pack(b)) - 32) < 1e-6)
k.expect("l2([0,0,0],[3,4,0]) = 5", abs(f("FRACTAL.VECTOR.L2", pack([0, 0, 0]), pack([3, 4, 0])) - 5) < 1e-6)
k.expect("l2sq = 25", abs(f("FRACTAL.VECTOR.L2SQ", pack([0, 0, 0]), pack([3, 4, 0])) - 25) < 1e-6)
k.expect("norm([3,4]) = 5", abs(f("FRACTAL.VECTOR.NORM", pack([3, 4])) - 5) < 1e-6)
k.expect("cosine similarity of orthogonal vectors is 0", abs(f("FRACTAL.VECTOR.SIMILARITY", pack([1, 0]), pack([0, 1]))) < 1e-6)
k.show("lp (p=3) of [1,2,3] vs [4,5,6]", k.call("FRACTAL.VECTOR.LP", pack(a), pack(b), "3").decode())

k.section("Arithmetic")
k.expect("add", unpack(k.call("FRACTAL.VECTOR.ADD", pack(a), pack(b))) == [5, 7, 9])
k.expect("sub", unpack(k.call("FRACTAL.VECTOR.SUB", pack(b), pack(a))) == [3, 3, 3])
k.expect("scale", unpack(k.call("FRACTAL.VECTOR.SCALE", pack(a), "2")) == [2, 4, 6])
n = unpack(k.call("FRACTAL.VECTOR.NORMALIZE", pack([3, 4, 0])))
k.expect("normalize([3,4,0]) = [0.6,0.8,0]", all(abs(x - y) < 1e-6 for x, y in zip(n, [0.6, 0.8, 0])))

k.section("Text in, text out")
packed = k.call("FRACTAL.VECTOR.FROMTEXT", "[0.5, 1.5, -2]")
k.expect("FROMTEXT packs", unpack(packed) == [0.5, 1.5, -2])
k.show("TOTEXT", k.call("FRACTAL.VECTOR.TOTEXT", packed).decode())

k.section("Compact forms")
rng = k.rng
v = [rng.gauss(0, 1) for _ in range(64)]
q8 = k.js("FRACTAL.VECTOR.QUANTIZE.INT8", pack(v))
k.show("int8: scale and code count", "%.4f, %d" % (q8["scale"], len(q8["codes"])))
err = max(abs(c * q8["scale"] - x) for c, x in zip(q8["codes"], v))
k.expect("int8 reconstruction error stays within one step", err <= q8["scale"] + 1e-6, "%.4f" % err)
bits = k.call("FRACTAL.VECTOR.QUANTIZE.BINARY", pack(v))
k.show("binary form", "%d bytes for %d floats" % (len(bits), len(v)))
k.expect("binary form is 8 bytes for 64 floats", len(bits) == 8)
w = list(v); w[0] = -w[0]; w[1] = -w[1]
bw = k.call("FRACTAL.VECTOR.QUANTIZE.BINARY", pack(w))
h = int(k.call("FRACTAL.VECTOR.HAMMING", bits, bw))
k.expect("flipping two signs moves two bits", h == 2, str(h))

k.section("Cheap filter, then re-rank")
rows = [[rng.gauss(0, 1) for _ in range(64)] for _ in range(200)]
target = rows[77]
codes = [k.call("FRACTAL.VECTOR.QUANTIZE.BINARY", pack(r)) for r in rows]
tq = k.call("FRACTAL.VECTOR.QUANTIZE.BINARY", pack([x + rng.gauss(0, 0.05) for x in target]))
cand = sorted(range(len(rows)), key=lambda i: int(k.call("FRACTAL.VECTOR.HAMMING", tq, codes[i])))[:10]
best = min(cand, key=lambda i: f("FRACTAL.VECTOR.L2", pack(target), pack(rows[i])))
k.show("Hamming shortlist of 10 from 200; re-ranked best", best)
k.expect("the shortlist contains the true neighbour and the re-rank picks it", 77 in cand and best == 77)
k.done()
