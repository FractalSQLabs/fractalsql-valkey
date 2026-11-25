#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
# SPDX-FileCopyrightText: 2026 Daniel Gardiner d/b/a FractalSQLabs
"""tests/vk_oom_sweep.py -- every kit-shaped call re-run with an allocation failure injected inside the module.

This is the Valkey counterpart of the sibling columns' allocation-failure sweeps. Valkey exports its allocator
from the executable, so an LD_PRELOAD shim never sees a module's malloc; the module therefore has its own
injection hook (FRACTAL.DEBUG.OOM <n>, src/fv_oom.c, compiled in only into the test flavor). It fails the nth
allocation after the call, which is the same shape of failure an allocator interposer produces at the module's
own boundary: the module gets a NULL where it expected memory, and everything downstream of it has to cope.

So: each call is run once unarmed (to see it work), then with the nth allocation failed in turn. The server may
answer or refuse the call -- what must not happen is a crash or a hang.

Usage: vk_oom_sweep.py --host H --port P [--max N]
"""
import argparse
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), "demo"))
from vk_common import Report, call, connect    # noqa: E402
from fvlib import pack                         # noqa: E402

GATE = "oom_sweep"

# The kit-shaped calls the sweep runs: a corpus load, a search, an index build, a chunk, an agent and a
# ledger probe. Each is a (label, args) pair; the corpus keys they need are created first.
def calls():
    corpus = "vk:oom:corpus"
    series = "vk:oom:series"
    vec = pack([[0.1, 0.2, 0.3], [0.4, 0.5, 0.6], [0.7, 0.8, 0.9]])
    return corpus, series, vec, [
        ("search",        ("FRACTAL.SEARCH", corpus, vec, 2)),
        ("chunk",         ("FRACTAL.TEXT.CHUNK", "a paragraph here\n\nand another one", 10, 0)),
        ("index_info",    ("FRACTAL.INDEX.INFO", "vk:oom:ix")),
        ("ledger_verify", ("FRACTAL.LEDGER.VERIFY",)),
        ("agent",         ("FRACTAL.AGENT.TRACK_ANOMALY", corpus, vec, vec, series)),
    ]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--host", default="127.0.0.1")
    ap.add_argument("--port", default="6379")
    ap.add_argument("--max", type=int, default=8, help="the allocation ordinals tried per call")
    args = ap.parse_args()

    r = Report(GATE)
    try:
        c = connect(args.host, args.port)
    except Exception as e:
        print("SKIP %s: no server reachable (%s)" % (GATE, e))
        return 0

    corpus, series, vec, plan = calls()
    try:
        c.call("SET", corpus, vec)
        c.call("SET", series, vec)
    except Exception as e:
        print("SKIP %s: the fixture could not be written (%s)" % (GATE, e))
        return 0

    # The hook is only in the test flavor of the module.
    ok, v = call(c, "FRACTAL.DEBUG.OOM", 0)
    if not ok:
        print("SKIP %s: this build has no allocation-failure hook (FRACTAL.DEBUG.OOM): %s" % (GATE, str(v)[:120]))
        return 0

    runs = 0
    problems = 0
    for label, cmdargs in plan:
        for n in range(1, args.max + 1):
            # Arm: the nth allocation after the next call fails.
            armed, _ = call(c, "FRACTAL.DEBUG.OOM", n)
            if not armed:
                break
            runs += 1
            try:
                c.call(*cmdargs)
            except Exception:
                pass                      # a refusal is the expected outcome
            finally:
                call(c, "FRACTAL.DEBUG.OOM", 0)   # disarm
            # The server must still be there.
            try:
                c.call("PING")
            except Exception as e:
                problems += 1
                r.bad("the server stopped answering after %s with the %dth allocation failed" % (label, n), e)
                break
    call(c, "FRACTAL.DEBUG.OOM", 0)

    if problems == 0:
        r.ok("%d runs, %d injected allocation failures, no crash and no hang" % (runs, runs))
    return r.done()


if __name__ == "__main__":
    sys.exit(main())
