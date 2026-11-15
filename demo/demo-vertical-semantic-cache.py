#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Semantic cache: answer a repeated question from Valkey instead of calling the model.

Before paying for an LLM call, ask Valkey whether a SIMILAR prompt already has an
answer. Prompt vectors live in one key (the corpus), answers in a Valkey list at
the same row index, and FRACTAL.SEARCH finds the closest cached prompt in-process.

The embedder here is a small deterministic bag-of-words hash, so this kit needs no
model or daemon. It is a stand-in: in production, FRACTAL.EMBED (through
fractalsqld) or any embedding model produces the vectors, and nothing else changes.
A hit needs the nearest cached prompt to be within a cosine-distance threshold."""
import hashlib, re
from fvlib import Kit, pack, unpack

k = Kit("semantic-cache")
STOP = {"a", "an", "the", "i", "my", "me", "to", "do", "how", "can", "is", "of", "on", "for", "and", "it", "what", "when", "where", "you", "your", "please", "need"}
DIM, THRESHOLD = 64, 0.45

def embed(text):
    v = [0.0] * DIM
    for tok in re.findall(r"[a-z]+", text.lower()):
        if tok in STOP:
            continue
        h = int(hashlib.sha256(tok.encode()).hexdigest(), 16)
        v[h % DIM] += 1.0 if (h >> 8) & 1 else -1.0
    return unpack(k.call("FRACTAL.VECTOR.NORMALIZE", pack(v)))

k.call("DEL", "kit:cache:vecs", "kit:cache:answers")
cache = []

def lookup(prompt):
    """Return (answer, cosine distance) on a hit, or (None, distance) on a miss."""
    if not cache:
        return None, 1.0
    q = embed(prompt)
    top = k.js("FRACTAL.SEARCH", "kit:cache:vecs", pack(q), 1)["top_k"][0]
    dist = top["dist"]
    return (k.call("LINDEX", "kit:cache:answers", top["idx"]).decode(), dist) if dist < THRESHOLD else (None, dist)

def store(prompt, answer):
    cache.append(embed(prompt))
    k.call("SET", "kit:cache:vecs", pack([x for row in cache for x in row]))
    k.call("RPUSH", "kit:cache:answers", answer)

k.section("1. Warm the cache")
store("How do I reset my password?", "Use the 'Forgot password' link on the sign-in page.")
store("What is your refund policy for annual plans?", "Annual plans are refundable within 30 days.")
store("How long does standard shipping take?", "Standard shipping takes 3 to 5 business days.")
k.show("cached prompts", len(cache))

k.section("2. Paraphrases hit")
for p, want in (("reset password how to", "Forgot password"), ("Please tell me the refund policy on annual plans", "refundable"),
                ("how long is standard shipping", "business days")):
    ans, d = lookup(p)
    k.show(repr(p), "dist %.3f -> %s" % (d, (ans or "MISS")[:34]))
    k.expect("hit with the right answer: %r" % p, ans is not None and want in ans, "dist %.3f" % d)

k.section("3. Unrelated prompts miss")
for p in ("Explain quantum entanglement simply", "Write a haiku about autumn leaves", "Translate good morning into German"):
    ans, d = lookup(p)
    k.show(repr(p), "dist %.3f -> %s" % (d, "MISS" if ans is None else "HIT?"))
    k.expect("miss: %r" % p, ans is None, "dist %.3f" % d)

k.section("4. The cache grows")
store("Explain quantum entanglement simply", "Two particles share a state; measuring one fixes the other.")
ans, d = lookup("quantum entanglement explained simply")
k.expect("a prompt that missed is a hit once its answer is stored", ans is not None and "particles" in ans, "dist %.3f" % d)
k.done()
