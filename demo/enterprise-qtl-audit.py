#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""The enterprise ledger: a tamper-evident record of decisions.

With the licensed enterprise library behind fractalsqld this demo appends decisions, flushes the ledger and
verifies the chain. On a Community install every ledger command answers "enterprise tier not loaded" (or "not
configured" with no daemon), which is the correct behaviour; this demo reports that and is skipped, not failed."""
import sys, os, json
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__))))
from fvlib import Kit, FvError

k = Kit("enterprise-qtl-audit")
k.section("Is the enterprise tier behind the daemon?")
try:
    before = k.call("FRACTAL.LEDGER.TRUTH.COUNT")
except FvError as e:
    k.show("FRACTAL.LEDGER.TRUTH.COUNT", "skipped: %s" % str(e)[:80])
    k.done()
k.show("truth entries", before)

k.section("Record two decisions")
k.expect("first decision appended", k.call("FRACTAL.AUDIT.LOG", "demo_allocate", '{"k": 2}') == 0)
k.expect("second decision appended", k.call("FRACTAL.AUDIT.LOG", "demo_allocate", '{"k": 3}') == 0)

k.section("Flush, then verify the chain")
k.expect("flush persists the ledger", k.call("FRACTAL.LEDGER.FLUSH") == 0)
v = json.loads(k.call("FRACTAL.LEDGER.VERIFY", 2))
k.show("verify the decision chain", v)
k.expect("the decision chain verifies and has at least our two records", v.get("ok") is True and v.get("rows_verified", 0) >= 2, str(v))

k.section("A forged request is refused")
try:
    k.call("FRACTAL.AUDIT.LOG", "demo_bad", "{not json")
    refused = False
except FvError:
    refused = True
k.expect("a malformed payload is rejected", refused)
k.done()
