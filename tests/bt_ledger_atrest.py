#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
# SPDX-FileCopyrightText: 2026 Daniel Gardiner d/b/a FractalSQLabs
#
# tests/bt_ledger_atrest.py: the audit trail's own storage checks -- the
# head file next to the ledger, the anchor log, the redaction of the text
# that goes into the chain, and the permissions of the files the ledger
# writes.
#
# Usage: bt_ledger_atrest.py <phase> --host H --port P --ledger PATH
#                              --daemon-log PATH
#
# Phases:
#   atrest           the daemon has a ledger key and publishes the anchor
#                    log to stderr
#   redact-hash      the daemon stores a fingerprint of the audit text
#   redact-truncate  the daemon stores the first 64 characters of it
#   anchor-off       the daemon has no anchor log
#
# Prints one line per check:
#
#     PASS enterprise: <name>
#     FAIL enterprise: <name>: <detail>
#     KNOWN enterprise: <name>: <detail>
#
# Exit status is 1 if any FAIL, else 0. Python standard library only.

import argparse
import base64
import hashlib
import json
import os
import re
import stat
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from bt_gates import Client, jload   # noqa: E402  (the RESP client and the JSON loader)

LEDGER_MAGIC_LEN = 8
LEDGER_VERSION = 1
LEDGER_HEADER_LEN = LEDGER_MAGIC_LEN + 1
LEDGER_HASH_LEN = 32
LEDGER_RECORD_HEADER_LEN = 4 + 4 + 1 + 1 + 2 + (3 * LEDGER_HASH_LEN) + 8

LABEL = "enterprise"

# The text put into the audit trail by the redaction checks. It is longer
# than the 64 characters the truncate mode keeps, and its tail is
# distinctive enough that "the tail is gone" is a real check and not a
# coincidence.
QUESTION = ("what did the top account buy last quarter, and how much did it "
            "pay, END-OF-QUESTION-MARKER")
QUESTION_TAIL = "END-OF-QUESTION-MARKER"


class Run(object):
    def __init__(self):
        self.fails = 0

    def ok(self, name):
        print("PASS %s: %s" % (LABEL, name))

    def bad(self, name, detail):
        self.fails += 1
        print("FAIL %s: %s: %s" % (LABEL, name, detail))

    def known(self, name, detail):
        print("KNOWN %s: %s: %s" % (LABEL, name, detail))

    def check(self, name, cond, detail=""):
        if cond:
            self.ok(name)
        else:
            self.bad(name, detail)


def csv_rows(path):
    """The CSV mirror's rows: id, kind, the base64 payload and the hashes."""
    rows = []
    with open(path, "r") as f:
        for line in f:
            line = line.strip()
            if not line:
                continue
            p = line.split(",")
            if len(p) < 6:
                continue
            rows.append({
                "id": int(p[0]),
                "kind": int(p[1]),
                "blob": base64.b64decode(p[2]),
                "mac": p[3],
                "prev": p[4],
                "entry": p[5],
            })
    return rows


def head_records(path):
    """The head file's records: kind -> (rows, mac-hex or None)."""
    recs = {}
    with open(path, "r") as f:
        for line in f:
            line = line.strip()
            if not line or line.startswith("#"):
                continue
            kind = rows = mac = None
            for tok in line.split():
                if tok.startswith("kind="):
                    kind = int(tok[len("kind="):])
                elif tok.startswith("rows="):
                    rows = int(tok[len("rows="):])
                elif tok.startswith("mac="):
                    mac = tok[len("mac="):]
            if kind is not None:
                recs[kind] = (rows, mac)
    return recs


def chain_records(path):
    """(offset, kind) of every record in the binary chain, in file order."""
    with open(path, "rb") as f:
        data = f.read()
    if len(data) < LEDGER_HEADER_LEN:
        return []
    out = []
    off = LEDGER_HEADER_LEN
    while off + LEDGER_RECORD_HEADER_LEN <= len(data):
        kind, blen = struct.unpack_from("<II", data, off)
        out.append((off, kind))
        off += LEDGER_RECORD_HEADER_LEN + blen
    return out


def head_edit(head_bytes, kind, what):
    """The head file's bytes with one field of one record changed by hand:
    `what` is "mac" (one hex digit of the MAC) or "rows" (the row count)."""
    out = []
    for line in head_bytes.split(b"\n"):
        if line.startswith(b"kind=%d " % kind):
            if what == "mac":
                line = re.sub(rb"mac=([0-9a-f])",
                              lambda m: b"mac=" + (b"0" if m.group(1) != b"0" else b"1"),
                              line, count=1)
            else:
                line = re.sub(rb"rows=\d+", b"rows=99", line, count=1)
        out.append(line)
    return b"\n".join(out)


def anchor_lines(path):
    """The anchor-log lines in the daemon's log."""
    if not os.path.exists(path):
        return []
    out = []
    with open(path, "r", errors="replace") as f:
        for line in f:
            line = line.strip()
            if not line.startswith('{"event":"ledger_head"'):
                continue
            try:
                out.append(json.loads(line))
            except ValueError:
                out.append({"_unparsed": line})
    return out


def phase_atrest(r, a):
    c = Client(a.host, a.port)
    ledger = a.ledger
    csv_path = ledger + ".csv"
    head_path = ledger + ".head"

    # One flush (the truth chain) and two audit records (the decision
    # chain): three appends, so three anchor-log lines.
    r.check("the truth chain flushes to the ledger",
            c.call("FRACTAL.LEDGER.FLUSH") == 0)
    r.check("a decision record is appended to the audit chain",
            c.call("FRACTAL.AUDIT.LOG", "portfolio_optimize", '{"k":2}') == 0)
    r.check("a second decision record is appended",
            c.call("FRACTAL.AUDIT.LOG", "portfolio_optimize", '{"k":3}') == 0)

    v = jload(c.call("FRACTAL.LEDGER.VERIFY", "2"))
    r.check("the decision chain verifies", v == {"ok": True, "rows_verified": 2}, str(v))

    # --- the head file ---
    r.check("the head file sits next to the ledger", os.path.exists(head_path), head_path)
    recs = head_records(head_path) if os.path.exists(head_path) else {}
    r.check("the head file counts the decision chain's rows",
            recs.get(2, (None, None))[0] == 2, str(recs))
    r.check("the head record carries a MAC when a ledger key is set",
            recs.get(2, (None, None))[1] is not None and len(recs[2][1]) == 64, str(recs))

    with open(head_path, "rb") as f:
        head_bytes = f.read()

    # (a) a head record edited by hand fails the verify. The edit is made
    # to the decision chain's own record, so the verify of that chain is
    # the one that has to notice it.
    tampered = head_edit(head_bytes, 2, "mac")
    r.check("the head file can be edited by hand", tampered != head_bytes)
    with open(head_path, "wb") as f:
        f.write(tampered)
    v = jload(c.call("FRACTAL.LEDGER.VERIFY", "2"))
    r.check("an edited head record fails the verify",
            v is not None and v.get("ok") is False and "head" in str(v.get("reason", "")), str(v))

    # ... and the verify passes again once the head is put back
    with open(head_path, "wb") as f:
        f.write(head_bytes)
    v = jload(c.call("FRACTAL.LEDGER.VERIFY", "2"))
    r.check("the verify passes again when the head is intact",
            v == {"ok": True, "rows_verified": 2}, str(v))

    # a head record that says more rows than the chain holds
    rows_bigger = head_edit(head_bytes, 2, "rows")
    with open(head_path, "wb") as f:
        f.write(rows_bigger)
    v = jload(c.call("FRACTAL.LEDGER.VERIFY", "2"))
    r.check("a head that claims more rows than the chain holds fails the verify",
            v is not None and v.get("ok") is False, str(v))
    with open(head_path, "wb") as f:
        f.write(head_bytes)

    # the same thing done to the CHAIN: the newest record is removed and
    # the head still says it was written
    with open(ledger, "rb") as f:
        chain_bytes = f.read()
    recs_in_chain = chain_records(ledger)
    # the real core's flush adds a record of the QTL chain (kind 1); the mock core writes none, so the chain holds the audit records only
    n_flush = 1 if os.environ.get("FSQL_ENT_KIND", "real") == "real" else 0
    r.check("the chain holds the appended records (%d)" % (2 + n_flush),
            len(recs_in_chain) == 2 + n_flush, str(recs_in_chain))
    if recs_in_chain:
        cut = recs_in_chain[-1][0]
        with open(ledger, "r+b") as f:
            f.truncate(cut)
        v = jload(c.call("FRACTAL.LEDGER.VERIFY", "2"))
        r.check("a chain whose tail was cut off fails the verify",
                v is not None and v.get("ok") is False, str(v))
        with open(ledger, "wb") as f:
            f.write(chain_bytes)
    v = jload(c.call("FRACTAL.LEDGER.VERIFY", "2"))
    r.check("the chain verifies again once the tail is put back",
            v == {"ok": True, "rows_verified": 2}, str(v))

    # --- the anchor log ---
    lines = anchor_lines(a.daemon_log)
    r.check("one anchor-log line per append", len(lines) == 2 + n_flush, str(lines))
    kinds = [x.get("kind") for x in lines]
    r.check("the anchor log names the chain kind of each append",
            kinds == ([1, 2, 2] if n_flush else [2, 2]), str(kinds))
    mirror = csv_rows(csv_path)
    newest = {}
    for row in mirror:
        newest[row["kind"]] = row
    r.check("the anchor log's hash is the newest entry hash of that chain",
            len(lines) == 2 + n_flush and newest.get(2, {}).get("entry") == lines[-1].get("hash"),
            "csv=%s anchor=%s" % (newest.get(2, {}).get("entry"), lines[-1].get("hash") if lines else None))
    r.check("the anchor log counts the rows of that chain",
            len(lines) == 2 + n_flush and lines[-1].get("rows") == 2, str(lines))
    r.check("the anchor log's hash is lowercase hex",
            all(re.match(r"^[0-9a-f]{64}$", str(x.get("hash", ""))) for x in lines), str(lines))

    # --- the permissions ---
    for path, what in ((ledger, "the ledger file"),
                       (csv_path, "the CSV mirror"),
                       (head_path, "the head file")):
        mode = stat.S_IMODE(os.stat(path).st_mode)
        r.check("%s is not group- or world-readable" % what, (mode & 0o077) == 0,
                "%s mode=%o" % (path, mode))

    r.check("the server survived", c.call("PING") == "PONG")
    c.close()


def phase_redact(r, a, mode):
    c = Client(a.host, a.port)
    payload = json.dumps({"question": QUESTION, "rows_returned": 3})
    r.check("a decision record with free text is appended",
            c.call("FRACTAL.AUDIT.LOG", "portfolio_optimize", payload) == 0)

    mirror = csv_rows(a.ledger + ".csv")
    blobs = [x["blob"] for x in mirror if x["kind"] == 2]
    r.check("the decision record reached the chain", len(blobs) == 1, str(len(blobs)))
    if not blobs:
        c.close()
        return
    stored = blobs[0].decode("utf-8", "replace")
    try:
        json.loads(stored)
        valid = True
    except ValueError:
        valid = False
    r.check("the stored record is still valid JSON", valid, stored[:160])

    if mode == "hash":
        digest = hashlib.sha256(QUESTION.encode()).hexdigest()
        r.check("the stored record does not hold the question text", QUESTION not in stored, stored[:160])
        r.check("the stored record holds the text's fingerprint",
                ("sha256:%s len=%d" % (digest, len(QUESTION.encode()))) in stored, stored[:200])
    else:
        r.check("the stored record keeps the first 64 characters of the text",
                QUESTION[:64] in stored, stored[:200])
        r.check("the stored record does not keep the rest of the text",
                QUESTION_TAIL not in stored and QUESTION[64:] not in stored, stored[:200])
    r.check("the fields that carry no free text are left alone",
            '"rows_returned":3' in stored.replace(" ", ""), stored[:200])

    r.check("the server survived", c.call("PING") == "PONG")
    c.close()


def phase_anchor_off(r, a):
    lines = anchor_lines(a.daemon_log)
    r.check("nothing is published while the anchor log is off", len(lines) == 0, str(lines))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("phase", choices=["atrest", "redact-hash", "redact-truncate", "anchor-off"])
    ap.add_argument("--host", default="127.0.0.1")
    ap.add_argument("--port", type=int, required=True)
    ap.add_argument("--ledger", required=True)
    ap.add_argument("--daemon-log", required=True)
    a = ap.parse_args()

    r = Run()
    try:
        if a.phase == "atrest":
            phase_atrest(r, a)
        elif a.phase == "redact-hash":
            phase_redact(r, a, "hash")
        elif a.phase == "redact-truncate":
            phase_redact(r, a, "truncate")
        else:
            phase_anchor_off(r, a)
    except (OSError, ConnectionError) as e:
        r.bad("the driver could not reach the server", str(e))
    return 1 if r.fails else 0


if __name__ == "__main__":
    sys.exit(main())
