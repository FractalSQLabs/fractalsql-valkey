#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
# SPDX-FileCopyrightText: 2026 Daniel Gardiner d/b/a FractalSQLabs
"""bench/vector_type_head_to_head.py: the storage question, measured. A corpus as a packed float32 blob, or as text?

FractalSQL reads a corpus from ONE string key of packed little-endian float32 (4 bytes per value, no parsing). The
SQL editions' counterpart is the native fractal_vector type against a text column. Here the alternative is the text
form ("[0.1,0.2,...]"), which FRACTAL.VECTOR.FROMTEXT parses. This script measures, for the same rows:

  - memory the key takes (MEMORY USAGE) in each form;
  - the cost of turning the text form back into a blob (FROMTEXT, per row), which is the work a text-stored corpus
    would add to every query;
  - that both forms give the same search answer once converted.

It reports; it does not pass or fail, and quotes no numbers. Python standard library only.

    python3 bench/vector_type_head_to_head.py --port 6379 --n 2000 --dim 128
"""
import argparse
import os
import random
import sys
import time

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "demo"))
from fvlib import Conn, FvError, pack, unpack  # noqa: E402


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--host", default="127.0.0.1")
    ap.add_argument("--port", type=int, default=6379)
    ap.add_argument("--n", type=int, default=2000)
    ap.add_argument("--dim", type=int, default=128)
    ap.add_argument("--seed", type=int, default=3)
    a = ap.parse_args()
    if not (1 <= a.n <= 200000 and 1 <= a.dim <= 4096):
        sys.exit("n must be 1..200000 and dim 1..4096")
    c = Conn(a.host, a.port)
    rng = random.Random(a.seed)
    rows = [[rng.gauss(0, 1) for _ in range(a.dim)] for _ in range(a.n)]
    keys = ("vt:blob", "vt:text", "vt:round")
    c.call("DEL", *keys)
    try:
        c.call("SET", "vt:blob", pack([x for r in rows for x in r]))
        texts = [c.call("FRACTAL.VECTOR.TOTEXT", pack(r)) for r in rows[: min(a.n, 500)]]
        sample = len(texts)
        text_per_row = sum(len(t) for t in texts) / sample
        c.call("SET", "vt:text", b"\n".join(texts))
        mb = int(c.call("MEMORY", "USAGE", "vt:blob"))
        mt_est = text_per_row * a.n
        print("rows x dim:                  %d x %d" % (a.n, a.dim))
        print("blob key (MEMORY USAGE):     %d bytes  (%.1f bytes/value)" % (mb, mb / (a.n * a.dim)))
        print("text form, measured sample:  %.0f bytes/row  (%.1f bytes/value; %.0f bytes for all rows)" % (text_per_row, text_per_row / a.dim, mt_est))
        print("text is %.1fx the size of the blob" % (mt_est / (a.n * a.dim * 4)))
        t0 = time.perf_counter()
        for t in texts:
            c.call("FRACTAL.VECTOR.FROMTEXT", t)
        dt = (time.perf_counter() - t0) / sample
        print("FROMTEXT per row:            %.1f us (round trip included); %.0f ms to convert all %d rows" % (dt * 1e6, dt * a.n * 1e3, a.n))
        blob_round = b"".join(c.call("FRACTAL.VECTOR.FROMTEXT", t) for t in texts)
        c.call("SET", "vt:round", blob_round)
        q = pack(rows[0])
        r1 = c.call("FRACTAL.SEARCH", "vt:blob", q, 1)
        r2 = c.call("FRACTAL.SEARCH", "vt:round", q, 1)
        import json
        same = json.loads(r1)["top_k"][0]["idx"] == json.loads(r2)["top_k"][0]["idx"] == 0
        print("same nearest row from both forms (sample of %d rows): %s" % (sample, "yes" if same else "NO"))
    except FvError as e:
        sys.exit("the module returned an error: %s" % e)
    finally:
        c.call("DEL", *keys)


if __name__ == "__main__":
    main()
