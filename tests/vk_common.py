#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
# SPDX-FileCopyrightText: 2026 Daniel Gardiner d/b/a FractalSQLabs
"""tests/vk_common.py -- shared helpers for the scripted-reply, ledger-payload, id-emission, fault-sweep and
kit-mutation drivers.

The drivers talk RESP to a fractalsqld that already has fractalsql.so loaded, and they report the way
build_test.sh's run_py_driver reads:

    PASS <gate> <name>
    FAIL <gate> <name>: <detail>
    KNOWN <gate> <name>: <detail>      a documented gap; reported, never fails the run

Exit status is 1 if any FAIL, else 0. Python standard library only.
"""
import json
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, os.path.join(ROOT, "demo"))

from fvlib import Conn, FvError   # noqa: E402  (the demo kit helper: the RESP client)

# The scripted replies the plugin answers from. The file's first line is an EPOCH: a new epoch restarts the
# reply sequence, which is how a driver scripts a fresh set of replies for the next scenario even though
# the daemon -- and therefore the plugin -- is one long-lived process.
SCRIPT_FILE = os.environ.get("FSQL_SCRIPT", "/tmp/fractalsql_bt_script.txt")
RS = chr(0x1e)
_epoch = [0]


def script(replies):
    """Scripts the replies the next dispatches will find, in dispatch order."""
    _epoch[0] += 1
    d = os.path.dirname(SCRIPT_FILE)
    if d:
        try:
            os.makedirs(d, exist_ok=True)
        except OSError:
            pass
    with open(SCRIPT_FILE, "w", encoding="utf-8", newline="") as f:
        f.write("%d\n" % _epoch[0])
        f.write(RS.join(replies))
    return _epoch[0]


def clear_script():
    try:
        os.remove(SCRIPT_FILE)
    except OSError:
        pass


class Report:
    """The PASS/FAIL lines build_test.sh reads."""

    def __init__(self, gate):
        self.gate = gate
        self.passed = 0
        self.failed = 0

    def ok(self, name, detail=""):
        self.passed += 1
        print("PASS %s %s" % (self.gate, name))

    def bad(self, name, detail=""):
        self.failed += 1
        print("FAIL %s %s: %s" % (self.gate, name, str(detail)[:300]))

    def check(self, cond, name, detail=""):
        if cond:
            self.ok(name)
        else:
            self.bad(name, detail)
        return bool(cond)

    def done(self):
        print("%s: %d checks passed, %d failed" % (self.gate, self.passed, self.failed))
        return 1 if self.failed else 0


def connect(host, port):
    return Conn(host, int(port))


def call(conn, *args):
    """One command. Returns (ok, value): on a refusal the value is the error text."""
    try:
        return True, conn.call(*args)
    except FvError as e:
        return False, str(e)


def text(v):
    """A reply as text. The RESP client hands back bytes; a reply is compared
    with the text the model wrote, so it is decoded here (never ASCII: the
    reply may hold control characters and unicode)."""
    if isinstance(v, bytes):
        return v.decode("utf-8", "replace")
    return v


def js(conn, *args):
    """One command whose answer is JSON. Returns (ok, value)."""
    ok, v = call(conn, *args)
    if not ok:
        return False, v
    try:
        return True, json.loads(v)
    except Exception as e:
        return False, "the answer is not JSON (%s): %r" % (e, str(v)[:120])
