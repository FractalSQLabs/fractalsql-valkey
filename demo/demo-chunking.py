#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Long text, chunked: split documents into passages, embed the passages, and answer a question from the right ones.

  1. FRACTAL.TEXT.CHUNK on its own: how a text is cut (no model needed).
  2. Each passage is embedded with FRACTAL.EMBED and stored as a row of a corpus key, with its text and its parent document beside it.
  3. FRACTAL.INDEX.ENSURE builds the index (and keeps it fit as passages are added).
  4. FRACTAL.RAG.AGENT answers a question from the nearest passages; with agent-use-index on, they come through the index.

Needs a server with fractalsql.so loaded and a fractalsqld that has a reasoning and an embedding provider
(without them, sections 2 to 4 say so and are skipped):

    python3 demo/demo-chunking.py --host 127.0.0.1 --port 6379
"""
import sys, os
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__))))
from fvlib import Kit, pack, FvError

k = Kit("demo-chunking")

DOCS = {
    1: "A node that stops heartbeating is first marked suspect. The coordinator waits for three missed beats before it acts.\n\n"
       "If the node is still silent, drain its work to the other nodes and open an incident. Do not restart it until the drain has finished, "
       "or its in-flight work is lost.\n\nAfter the restart, watch the node for ten minutes. If it misses another beat, replace the hardware.",
    2: "Backups run nightly at two in the morning. Each backup is verified by restoring it into a scratch database and comparing row counts.\n\n"
       "A failed verification pages the on-call engineer. Never delete the previous good backup until the next one has verified.",
    3: "Certificates are rotated every ninety days. The rotation job writes the new certificate beside the old one and reloads the proxy.\n\n"
       "If the reload fails, the old certificate stays in place and an alert is raised.",
}

k.section("1. How FRACTAL.TEXT.CHUNK cuts a text")
chunks = k.js("FRACTAL.TEXT.CHUNK", "First paragraph here.\n\nSecond one is short.\n\nThird paragraph is a bit longer than the others are.", 40, 0)
for c in chunks:
    k.show("chunk %d (%d-%d)" % (c["no"], c["start"], c["end"]), repr(c["text"]))
k.expect("every chunk fits the size", all(len(c["text"]) <= 40 for c in chunks))
sent = k.js("FRACTAL.TEXT.CHUNK", "One. Two sentences! Is this three? Yes.  Four is a long one that keeps on going and going.", 30, 5, "sentence")
k.expect("a long sentence is cut into overlapping pieces", len(sent) >= 4)

k.section("2. Chunk, embed and store the passages")
KEY, TXT, PAR = "demo:chunks:corpus", "demo:chunks:text", "demo:chunks:parent"
k.c.call("DEL", KEY, TXT, PAR)
passages = []
for doc_id, text in DOCS.items():
    for c in k.js("FRACTAL.TEXT.CHUNK", text, 200, 20):
        passages.append((doc_id, c["no"], c["text"]))
k.show("documents / passages", "%d / %d" % (len(DOCS), len(passages)))
blob, dim = b"", 0
try:
    for i, (doc_id, no, text) in enumerate(passages):
        emb = k.c.call("FRACTAL.EMBED", text)
        dim = len(emb) // 4
        blob += emb
        k.c.call("HSET", TXT, str(i), text)
        k.c.call("HSET", PAR, str(i), str(doc_id))
    k.c.call("SET", KEY, blob)
    k.show("embedding dimension", str(dim))
except FvError as e:
    k.show("embedding", "skipped: needs an embedding provider behind fractalsqld (%s)" % str(e)[:80])
    k.done()

k.section("3. The index")
en = k.js("FRACTAL.INDEX.ENSURE", KEY, "demo_chunks", "DIM", str(dim), "VECTOR_FORMAT", "FP32")
k.show("ensure", "%s, %d rows, status %s" % (en["action"], en["rows"], en["status"]))
k.expect("the index was created", en["action"] == "created" and en["rows"] == len(passages))
en = k.js("FRACTAL.INDEX.ENSURE", KEY, "demo_chunks", "DIM", str(dim), "VECTOR_FORMAT", "FP32")
k.expect("calling it again changes nothing", en["action"] == "unchanged")

k.section("4. A question answered from the passages")
q = "What should I do before restarting a node that stopped heartbeating?"
ans = k.optional_js("RAG_AGENT", "FRACTAL.RAG.AGENT", KEY, q, str(dim), "CONTENT", TXT)
if ans is not None:
    k.show("answer", str(ans.get("answer"))[:300])
    k.show("passages used", str(ans["source_row_ids"]))
    k.show("through the index", str(ans.get("used_index")))
    top_parents = [k.c.call("HGET", PAR, str(i)) for i in ans["source_row_ids"][:2]]
    if len({blob[j:j + dim * 4] for j in range(0, len(blob), dim * 4)}) > 1:      # a stand-in embedder gives every text the same vector
        k.expect("the nearest passages come from the heartbeat runbook", b"1" in top_parents, str(top_parents))

k.c.call("DEL", KEY, TXT, PAR)
k.done()
