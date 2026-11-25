#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
# SPDX-FileCopyrightText: 2026 Daniel Gardiner d/b/a FractalSQLabs
"""tests/vk_ledger_payloads.py -- what FRACTAL.AUDIT.LOG writes into the decision-audit chain for every shape
of entry type and payload.

The ledger entry is built from the caller's text: {"type": ..., "entry": ...} with an "_actor" block added.
Whatever the text holds must never break the JSON or the chain: an object payload takes the actor inside it, a
scalar or an array is wrapped as {"value": ...}, and the entry type is escaped (quote, backslash, control
characters, unicode). Each entry must be accepted or refused cleanly, and the chain must still verify.

Usage: vk_ledger_payloads.py --host H --port P
"""
import argparse
import json
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from vk_common import Report, call, connect, js   # noqa: E402

GATE = "ledger_payloads"

ESC = "q\" b\\ bs\b ff\f nl\n cr\r tab\t c1\x01 c1f\x1f end"

# (what, payload, accepted). Every payload is already JSON text, so the shapes the entry builder
# distinguishes are: an object (the actor goes inside it) and everything else (wrapped as a value).
PAYLOADS = (
    ("an object",                    '{"n":1}',                 True),
    ("a nested object",              '{"a":{"b":[1,2,{"c":null}]}}', True),
    ("an object with a null value",  '{"a":null}',              True),
    ("an empty object",              '{}',                      True),
    ("an array",                     '[1,2,3]',                 True),
    ("an array of objects",          '[{"a":1},{"b":2}]',       True),
    ("a string",                     '"text"',                  True),
    ("a number",                     '12',                      True),
    ("true",                         'true',                    True),
    ("null",                         'null',                    True),
    ("an object that is not closed", '{"a":1',                  False),
    ("plain text",                   'not json',                False),
    ("the empty string",             '',                        False),
)

# (what, entry type)
# The enterprise tier gates the entry type at 256 bytes (fractal_audit_log's
# shape gate: the type is spliced into the chain record, so an unbounded
# free-text field would let any session land a malformed record). A type
# within the cap is taken; one over it is refused cleanly.
TYPES = (
    ("a plain name",        "plain"),
    ("a quote",             'a"b'),
    ("a backslash",         "a\\b"),
    ("every escape and control character", ESC),
    ("a long type",         "t" * 200),
    ("a unicode type",      "tipo é 日本 \U0001f600"),
    ("an empty type",       ""),
    ("a JSON-looking type", '{"type":"forged"}'),
)

# The entry types the tier refuses: over the 256-byte cap. (The community
# hosts' own audit_log has no such cap, so this is a divergence between the
# hosts worth an entry in the consistency plan.)
OVER_CAP_TYPES = (
    ("a type over the 256-byte cap", "t" * 300),
)


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

    # --- the payload shapes ------------------------------------------------------------------------
    for what, payload, accepted in PAYLOADS:
        ok, v = call(c, "FRACTAL.AUDIT.LOG", "type.payload", payload)
        if accepted:
            r.check(ok, "audit_log accepts %s" % what, v)
        else:
            # A payload that is not JSON at all: the command must refuse it cleanly.
            r.check(not ok, "audit_log refuses %s cleanly" % what, (ok, str(v)[:120]))

    # --- the entry types ---------------------------------------------------------------------------
    for what, etype in TYPES:
        ok, v = call(c, "FRACTAL.AUDIT.LOG", etype, '{"k":1}')
        r.check(ok, "audit_log takes an entry type with %s" % what, v)

    # --- the entry types over the tier's cap -------------------------------------------------------
    # The refusal is clean (an error reply, no crash). Its text is the daemon's
    # generic hint for a refused core call -- the tier's own reason for the
    # refusal does not reach the client -- so only the refusal itself is
    # asserted here.
    for what, etype in OVER_CAP_TYPES:
        ok, v = call(c, "FRACTAL.AUDIT.LOG", etype, '{"k":1}')
        r.check(not ok, "audit_log refuses %s cleanly" % what, (ok, str(v)[:120]))

    # --- the chain still verifies --------------------------------------------------------------------
    ok, v = js(c, "FRACTAL.LEDGER.VERIFY", 2)
    if r.check(ok, "the chain verifies after every entry", str(v)[:200]):
        r.check(isinstance(v, dict) and v.get("ok") is True, "the verification report says ok", str(v)[:200])

    # --- the entries that were written are valid JSON -------------------------------------------------
    ok, v = call(c, "FRACTAL.LEDGER.TRUTH.COUNT")
    r.check(ok, "the truth count answers", v)

    return r.done()


if __name__ == "__main__":
    sys.exit(main())
