#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
# SPDX-FileCopyrightText: 2026 Daniel Gardiner d/b/a FractalSQLabs
"""tests/vk_id_emission.py -- how the agents write a row's id into their JSON result.

The id is the caller's data (the IDS hash of the corpus), so what an agent writes must never become JSON
structure and must never be altered: the id comes back byte for byte -- quotes, backslashes, control
characters, unicode, an empty id, a 400-digit id -- and the result document stays valid JSON.

Usage: vk_id_emission.py --host H --port P
"""
import argparse
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), "demo"))
from vk_common import Report, connect, js   # noqa: E402
from fvlib import pack                       # noqa: E402  (the corpus/series packing)

GATE = "id_emission"

IDS = ("42", "0", "-7", "10.5", "007", "", "true", "null", "NaN",
       '{"x":1', '["a"', 'quote " and backslash \\', "tab\there", "line\nbreak", "cr\rhere",
       "back\bspace and form\ffeed", "ctl\x01\x02\x1f end", "slash / and unicode é 日本 \U0001f600",
       "-" + "9" * 400, "1" + "0" * 400)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--host", default="127.0.0.1")
    ap.add_argument("--port", default="6379")
    args = ap.parse_args()

    r = Report(GATE)
    try:
        c = connect(args.host, args.port)
    except Exception as e:
        print("SKIP %s: no server reachable (%s)" % (GATE, e))
        return 0

    # --- the id comes back exactly -------------------------------------------------------------------
    # One corpus per id (a single row), so the agent's answer is about that row and not a neighbour. The
    # IDS hash carries the row's id: the agent writes it into the result verbatim, as a JSON string.
    for i, ident in enumerate(IDS):
        corpus = "vk:id:corpus:%d" % i
        ids_key = "vk:id:hash:%d" % i
        series = "vk:id:series:%d" % i
        # One corpus per id (a single row of three dimensions), so the agent's answer is about that row and
        # not a neighbour. The heading series is a key holding samples of that dimension (the agent reads
        # the series by key, not as a blob argument), and the IDS hash carries the row's id: the agent
        # writes it into the result verbatim, as a JSON string.
        row = [0.1 + i * 0.001, 0.2, 0.3]
        try:
            c.call("SET", corpus, pack([row]))
            c.call("SET", series, pack([row] * 32))
            c.call("HSET", ids_key, "0", ident)
        except Exception as e:
            r.bad("the row %d could not be written" % i, e)
            continue
        ok, v = js(c, "FRACTAL.AGENT.TRACK_ANOMALY", corpus,
                   pack(row), pack(row), series, "IDS", ids_key)
        if not r.check(ok, "the agent answers for the id %r" % ident[:24], str(v)[:160]):
            continue
        got = v.get("nearest_fleet_id") if isinstance(v, dict) else None
        # What matters is that the id is the caller's text, byte for byte (the agent writes it as a JSON
        # string, so it comes back as one).
        r.check(got == ident, "the id %r comes back byte for byte" % ident[:24], (got,))

    return r.done()


if __name__ == "__main__":
    sys.exit(main())
