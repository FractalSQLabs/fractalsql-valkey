#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Response modes: what FRACTAL.REASON hands back.

The daemon's reasoning call returns the model's reply as-is (text, the default), or reduced to the single fenced
block inside it (code), or that block checked as JSON (json). The mode is set in the daemon's environment
(FSQL_REASONING_HTTP_RESPONSE_MODE). Embeddings and the DATA_ANALYST query step are fixed and unaffected.
This demo asks one question and tells you which mode you are in from the shape of the answer. It needs a
fractalsqld daemon behind the module and is skipped without one."""
import sys, os
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__))))
from fvlib import Kit, FvError

k = Kit("response-modes")
k.section("Ask for a fenced block")
try:
    out = k.call("FRACTAL.REASON", "Reply with ONLY one fenced json code block containing {\"ok\": true}.")
except FvError as e:
    k.show("FRACTAL.REASON", "skipped: %s" % str(e)[:70])
    k.done()
text = out.decode() if isinstance(out, bytes) else out
k.show("reply", repr(text[:70]))
if "```" in text:
    k.show("mode", "text (the default): the reply came back with its fence markers")
elif text.strip().startswith("{"):
    k.show("mode", "code or json: the fence was removed and the interior returned")
else:
    k.show("mode", "text: the model did not use a fenced block")
k.expect("the daemon answered", len(text) > 0)
k.section("Embeddings do not depend on it")
try:
    emb = k.call("FRACTAL.EMBED", "hello")
    k.expect("EMBED returns a packed vector", isinstance(emb, bytes) and len(emb) % 4 == 0 and len(emb) > 0)
except FvError as e:
    k.show("FRACTAL.EMBED", "skipped: %s" % str(e)[:70])
k.done()
