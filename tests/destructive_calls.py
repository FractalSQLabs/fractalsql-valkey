#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
# SPDX-FileCopyrightText: 2026 Daniel Gardiner d/b/a FractalSQLabs
"""Calls the ledger's reset_hard and flush on a running fractalsqld (SECURITY_AUDIT.md 3.12): the daemon's log shows the verdict.

usage: destructive_calls.py SOCKET KEYFILE [reset_only]
"""
import sys

import fsq_client as fc

c = fc.Client(sys.argv[1], fc.load_key(sys.argv[2]), timeout=10)
for fn in (["fractal_ledger_reset_hard"] if len(sys.argv) > 3 else ["fractal_ledger_reset_hard", "fractal_ledger_flush"]):
    try:
        c.call(fn, 1)
    except Exception:
        pass   # the library is not loaded here: only the refusal that comes before it matters
c.close()
