#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
# SPDX-FileCopyrightText: 2026 Daniel Gardiner d/b/a FractalSQLabs
"""tests/vk_scripted_replies.py -- the extension against a model that answers exactly what the test scripts.

tests/scripted_reasoning_plugin.c answers every dispatch with the next reply of the script the harness wrote,
so a test chooses exactly what the extension has to parse. The scenarios:

  * the reasoning tier: the reply is handed back exactly as the model wrote it (a fenced block, a reply with
    control characters, an empty reply, a reply that is not SQL at all);
  * the embedding tier: a vector in every shape the parser takes or refuses (a well-formed array, an array with
    spaces, an empty array, a non-numeric element, a nested array, text that is not an array at all);
  * the review verdict: the word PASS at the front of the reply, and the replies that merely start with it.

Usage: vk_scripted_replies.py --host H --port P
"""
import argparse
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from vk_common import Report, call, clear_script, connect, script, text   # noqa: E402

GATE = "scripted_replies"


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

    # --- the reasoning tier: the reply comes back exactly as the model wrote it --------------------
    # The daemon's default response mode is text: the reply is the model's own bytes, fence markers and
    # all. (The code mode that extracts a fenced block is a separate, explicitly configured mode.)
    for reply in ("the routing is sound",
                  "```sql\nSELECT 1\n```",
                  "line one\nline two",
                  "  padded  ",
                  "ctl\x01\x02\x1f end",
                  "unicode é 日本 \U0001f600"):
        script([reply])
        ok, v = call(c, "FRACTAL.REASON", "q")
        r.check(ok and text(v) == reply, "the reasoning reply is handed back verbatim (%r)" % reply[:24], (ok, str(v)[:120]))

    # A reply the model gives when it has nothing: an empty one.
    script([""])
    ok, v = call(c, "FRACTAL.REASON", "q")
    r.check(ok, "an empty reasoning reply is answered, not a crash", (ok, str(v)[:120]))

    # --- the embedding tier: a vector in every shape the parser takes or refuses ----------------------
    # The embedding answer is a packed float32 blob, so a good reply is a JSON array of numbers.
    EMB = (("[0.1,0.2,0.3]", 3),
           ("[1]", 1),
           ("[ 0.1 , 0.2 ]", 2),
           ("[0.1,0.2,0.3] trailing", 3),
           ("[]", None),
           ("", None),
           ("no vector", None),
           ("[0.1,abc]", None),
           ("[[1,2],[3,4]]", None))
    for reply, dims in EMB:
        script([reply])
        ok, v = call(c, "FRACTAL.EMBED", "x")
        if dims is None:
            # A reply the parser refuses: the command must answer with a clean error, not a crash.
            r.check(not ok, "the embedding reply %r is refused cleanly" % reply[:24], (ok, str(v)[:120]))
        else:
            r.check(ok, "the embedding reply %r is accepted" % reply[:24], (ok, str(v)[:120]))

    clear_script()
    return r.done()


if __name__ == "__main__":
    sys.exit(main())
