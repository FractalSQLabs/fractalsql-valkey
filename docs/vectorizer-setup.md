<p align="center">
  <img src="../FractalSQLforValkey.jpg" alt="FractalSQL for Valkey" width="720">
</p>

# Vectorizer Setup Guide

The vectorizer in the SQL editions is a trigger-driven pipeline: it watches a table's text column, queues changed rows, calls an embedding model, and
writes the vector back into another column. It depends on tables, triggers and a queue table, so **FractalSQL for Valkey does not provide the
vectorizer** (`fractal_vectorizer_*`). Embedding itself is provided: it is `FRACTAL.EMBED`. This page keeps the layout of the SQL editions' guide and
says what applies on Valkey.

## Prerequisites

- The `fractalsql` module loaded, and a `fractalsqld` daemon with an **embedding provider** configured (`embed_url` and `embed_model` in
  `fractalsqld.conf`; see [reasoning-setup.md](reasoning-setup.md)). Without them `FRACTAL.EMBED` answers "not configured".
- A corpus key per vector dimension: the dimension is whatever your embedding model returns (768 for many open models, 1536 for some hosted ones).

## Quick Start: embed at write time

Embed in your application when you write a document, and store the vector beside your data:

```python
vec = embed("reset my password")                    # FRACTAL.EMBED, through fractalsqld; a packed float32 blob
conn.hset("doc:42", mapping={"text": "reset my password", "vec": vec})
conn.append("corpus:rows", vec)                     # a corpus key is the rows back to back, so appending adds a row
```

### Keeping a corpus key in step

A **corpus key** (the packed rows `FRACTAL.SEARCH` reads) is just a string, so `APPEND` adds a row. Row `i` is bytes `[i*dim*4, (i+1)*dim*4)`.
Keep a companion hash (row index to document id) beside it, which is what the agents' `IDS` option reads, and rewrite the key when you delete a
document. Use one key per cohort instead of a `WHERE` clause.

### Monitor progress

There is no queue to inspect. The row count of a corpus key is `STRLEN key / (dim * 4)`; compare it with your document count.

## Storage: blob vs. text

A vector is a packed little-endian float32 blob: 4 bytes per value, no parsing, binary-safe in any Valkey string. `FRACTAL.VECTOR.FROMTEXT` and
`TOTEXT` convert to and from `[1,2,3]` text for interchange. Prefer the blob for storage. See [api-analytics.md](api-analytics.md#vector-math-and-quantization)
for the vector math and 4x / 32x quantization.

## Endpoint Providers

Embeddings use the same plugin as reasoning, with their own endpoint and model so a chat model is never mistaken for an embedding model:
`embed_url`, `embed_model` and the shared credentials in `fractalsqld.conf`. Ollama, vLLM and any OpenAI-compatible service work, and so do the
hosted providers the plugin supports; the full list and the settings for each are in [reasoning-setup.md](reasoning-setup.md).

## Command Reference

### `FRACTAL.EMBED text`

Returns the embedding as a packed float32 blob, or an error naming what is missing. See [api-cognition.md](api-cognition.md).

The SQL editions' `fractal_vectorizer_create`, `_pause`, `_resume`, `_drop`, `_enqueue` and `_process_queue`, and their rate capping, have no
Valkey equivalent.

## Design & Safety

Embedding runs in `fractalsqld`, never in the Valkey process, so a slow provider cannot stall other clients and a provider's credentials never pass
through a command, `MONITOR` or the slowlog. A provider outage makes `FRACTAL.EMBED` fail; every other command keeps working.

## Known Constraints & Roadmap

- No automatic pipeline: you decide when to embed and where to store the result.
- The embedding dimension must match the corpus key's dimension; the agents that embed text (`PLAN_EXPLORE`, `SEARCH_AGENT`, `RAG_AGENT`) take
  the dimension as an argument and refuse a mismatch.

## When to use the Vectorizer

On Valkey: when you want semantic retrieval over text, call `FRACTAL.EMBED` at write time and at query time. If you need triggers and a managed queue
over a table, use a FractalSQL SQL
edition.
