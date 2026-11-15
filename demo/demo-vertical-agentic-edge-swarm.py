#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Agentic edge and robotics swarm coordination.

Ten battery-constrained edge agents each carry a small local memory vector that must be compressed for radio-
constrained inter-agent sync (FRACTAL.VECTOR.QUANTIZE.INT8 and .BINARY, compared with FRACTAL.VECTOR.HAMMING). One agent is stuck
oscillating between two headings (a control-loop "wobble") that a fleet monitor catches from its raw state stream
with FRACTAL.AGENT.DETECT_LOOP. A battery-constrained pass chooses which agents take on high-value tasks."""
from fvlib import Kit, pack, unpack, cosd, unit

k = Kit("agentic-edge-swarm")
rng = k.rng
mem = [[rng.gauss(0, 1) for _ in range(64)] for _ in range(10)]

k.section("1. Compress the memory vectors for sync")
raw = 64 * 4
q8 = k.js("FRACTAL.VECTOR.QUANTIZE.INT8", pack(mem[0]))
b1 = k.call("FRACTAL.VECTOR.QUANTIZE.BINARY", pack(mem[0]))
k.show("bytes per vector: float32 / int8 / 1-bit", "%d / %d / %d" % (raw, len(q8["codes"]), len(b1)))
k.expect("int8 is 4x smaller and 1-bit 32x smaller", raw // len(q8["codes"]) == 4 and raw // len(b1) == 32)
near = [x + rng.gauss(0, 0.1) for x in mem[0]]
bn = k.call("FRACTAL.VECTOR.QUANTIZE.BINARY", pack(near))
bf = k.call("FRACTAL.VECTOR.QUANTIZE.BINARY", pack(mem[5]))
h_near, h_far = int(k.call("FRACTAL.VECTOR.HAMMING", b1, bn)), int(k.call("FRACTAL.VECTOR.HAMMING", b1, bf))
k.show("Hamming distance: similar memory / unrelated memory", "%d / %d" % (h_near, h_far))
k.expect("1-bit codes keep similarity: a near memory is closer than an unrelated one", h_near < h_far)

k.section("2. Catch the wobbling agent from its state stream")
heads = [unit([1, 0, 0.2]), unit([0, 1, 0.2])]
wobble = [heads[i % 2] for i in range(40)]
healthy = [[rng.gauss(0, 1) for _ in range(3)] for _ in range(150)]      # an unpredictable patrol: no repeated state, no persistence
k.call("SET", "kit:swarm:wobble", pack([x for s in wobble for x in s]))
k.call("SET", "kit:swarm:ok", pack([x for s in healthy for x in s]))
w = k.js("FRACTAL.AGENT.DETECT_LOOP", "agent-3", "kit:swarm:wobble", 3)
o = k.js("FRACTAL.AGENT.DETECT_LOOP", "agent-8", "kit:swarm:ok", 3)
k.show("loop detected: wobbling / healthy", "%s / %s" % (w["is_loop_detected"], o["is_loop_detected"]))
k.expect("the two-heading oscillation is flagged even though its DFA exponent is 0", w["is_loop_detected"] is True and w["dfa_exponent"] == 0)
k.expect("the healthy patrol is not a closed loop", o["is_loop_detected"] is False, str(o))

k.section("3. Battery-constrained task routing: 3 of 10 agents")
battery = [90, 85, 12, 70, 8, 95, 60, 50, 77, 66]
mu = [0.10] * 10
cov = [[(0.01 + (100 - battery[i]) / 100.0 * 0.2) if i == j else 0.0 for j in range(10)] for i in range(10)]
opt = k.js("FRACTAL.OPTIMIZE.PORTFOLIO", pack(mu), pack([x for r in cov for x in r]), 3, 3)
chosen = {i for i, x in enumerate(opt["weights"]) if x > 0}
k.show("agents taking the high-value tasks", sorted(chosen))
k.expect("three agents are chosen, none of them nearly out of battery", len(chosen) == 3 and not (chosen & {2, 4}))
k.done()
