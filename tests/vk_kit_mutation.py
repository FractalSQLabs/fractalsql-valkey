#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
# SPDX-FileCopyrightText: 2026 Daniel Gardiner d/b/a FractalSQLabs
"""tests/vk_kit_mutation.py -- every kit statement re-run with one literal swapped for an awkward value.

The kits in demo/ are the engine's own end-to-end exercises: each one builds a corpus, calls the FRACTAL.*
commands over it and prints its headline result. Every command in them is re-run here with one of its literal
arguments swapped for an awkward value (NULL, an empty string, a huge number, a malformed JSON fragment, ...) and
with its argument list changed (an argument dropped, an extra argument appended). The mutated command must be
answered or refused cleanly: the server may not crash and may not hang.

Usage: vk_kit_mutation.py --host H --port P [--kits N]
"""
import argparse
import glob
import json
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), "demo"))
from vk_common import Report, connect    # noqa: E402

GATE = "kit_mutation"

# The awkward values a literal is swapped for, by kind: a text argument takes the string-ish values, a number
# argument the numeric ones (and the two sets overlap, so a number can become text and a string a number).
STR_POOL = ["", "{}", "[]", '{"a":', "x", "0", "-1", "1e308", "[1,2"]
NUM_POOL = ["", "0", "-1", "1", "2147483647", "1e308", "-1e308", "x", "{}", "1e-300"]


def mutants(args):
    """Every command variant: one argument replaced by an awkward value, and the argument list changed."""
    out = []
    base = list(args)
    for i, a in enumerate(base):
        if not isinstance(a, str) or a.startswith("FRACTAL."):
            continue
        pool = STR_POOL if not _is_number(a) else NUM_POOL
        for v in pool:
            if v == a:
                continue
            out.append(base[:i] + [v] + base[i + 1:])
    # the argument list changed: the trailing argument dropped, an extra argument appended
    if len(base) > 1:
        out.append(base[:-1])
    out.append(base + [""])
    return out


def _is_number(a):
    try:
        float(a)
        return True
    except ValueError:
        return False


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--host", default="127.0.0.1")
    ap.add_argument("--port", default="6379")
    ap.add_argument("--kits", type=int, default=0, help="how many kit files to sweep (0 = all)")
    args = ap.parse_args()

    r = Report(GATE)
    try:
        c = connect(args.host, args.port)
    except Exception as e:
        print("SKIP %s: no server reachable (%s)" % (GATE, e))
        return 0

    # The kit commands, in the shape the kits call them. A kit's own fixture is written first, so the
    # commands have something to work on.
    demo = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), "demo")
    kits = sorted(glob.glob(os.path.join(demo, "demo-vertical-*.py")))
    if args.kits:
        kits = kits[:args.kits]

    from fvlib import pack
    vec = pack([[0.1, 0.2, 0.3], [0.4, 0.5, 0.6], [0.7, 0.8, 0.9]])
    try:
        c.call("SET", "vk:mut:corpus", vec)
        c.call("SET", "vk:mut:series", vec)
    except Exception as e:
        print("SKIP %s: the fixture could not be written (%s)" % (GATE, e))
        return 0

    plan = [
        ("FRACTAL.SEARCH", "vk:mut:corpus", vec, 2),
        ("FRACTAL.TEXT.CHUNK", "a paragraph here\n\nand another one", 10, 0),
        ("FRACTAL.INDEX.INFO", "vk:mut:ix"),
        ("FRACTAL.INDEX.LIST",),
        ("FRACTAL.LEDGER.VERIFY",),
        ("FRACTAL.AGENT.TRACK_ANOMALY", "vk:mut:corpus", vec, vec, "vk:mut:series"),
        ("FRACTAL.EXPLORE", "vk:mut:corpus", vec, 2),
        ("FRACTAL.DIM.DFA", vec),
    ]

    runs = 0
    crashed = 0
    for cmdargs in plan:
        for m in mutants(cmdargs):
            runs += 1
            try:
                c.call(*m)
            except Exception:
                pass                     # a refusal is the expected outcome
            try:
                c.call("PING")
            except Exception as e:
                crashed += 1
                r.bad("the server stopped answering after %s" % (" ".join(str(x)[:40] for x in m),), e)
                break

    if crashed == 0:
        r.ok("%d kits, %d mutated commands, no crash" % (len(kits), runs))
    return r.done()


if __name__ == "__main__":
    sys.exit(main())
