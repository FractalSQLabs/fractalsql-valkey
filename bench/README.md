<p align="center">
  <img src="../FractalSQLforValkey.jpg" alt="FractalSQL for Valkey" width="720">
</p>

# FractalSQL benchmark

`bench.py` measures what the module costs and, just as important, what it does **not** cost
your other clients.

```bash
python3 bench/bench.py --port 6379
python3 bench/bench.py --port 6379 --sizes 1000x64,20000x64 --repeat 30 --clients 8
```

It reports three things against a server that has the module loaded:

1. **Latency by corpus size** for `FRACTAL.SEARCH` and `FRACTAL.EXPLORE`, printed next to a
   plain `PING` so the round trip is visible apart from the work.
2. **Responsiveness during a long search**: how long a `PING` from another client takes while
   an `EXPLORE` runs. The search runs on a worker thread, so this should stay in the
   millisecond range however long the search takes.
3. **Throughput** with several clients searching at once.

It reports; it does not pass or fail, and the docs quote no numbers, because they depend on
your hardware, your corpus and your Valkey version. Run it on yours.

## The other scripts

| Script | What it does |
| --- | --- |
| `scale_sweep.sh` | Runs `bench.py` across a list of corpus sizes, one at a time (`--sizes "1000 5000 20000" --dim 64`), so you can see how cost grows with rows x dimension. |
| `data_gen.py` | Loads a clustered corpus into a key (`--n`, `--dim`, `--clusters`) plus a companion hash of cluster ids, for the benchmarks and for trying commands by hand. `--delete` removes it. |
| `vector_type_head_to_head.py` | The storage question: the same rows as a packed float32 blob and as text, compared on memory (`MEMORY USAGE`) and on the cost of `FRACTAL.VECTOR.FROMTEXT`. |
| `head_to_head.py` | valkey-search's HNSW index against Scout (below). |

```bash
bench/scale_sweep.sh --port 6379 --dim 64 --sizes "1000 5000 20000"
python3 bench/data_gen.py --port 6379 --key bench:corpus --n 10000 --dim 128 --clusters 50
python3 bench/vector_type_head_to_head.py --port 6379 --n 2000 --dim 128
```

## What the numbers mean

- A corpus is decoded from float32 to double on the main thread (a few milliseconds per
  hundred thousand values) before the search is handed to a worker, so a very large corpus
  adds a short, bounded main-thread cost per call. The in-flight cap (`max-inflight-mb`)
  bounds how much decoded data can be held at once.
- `SEARCH` ranks by cosine distance. A brute-force scan that ranks by Euclidean distance is a
  different question and will not return the same rows.
## Head to head: valkey-search HNSW vs Scout

`head_to_head.py` compares valkey-search's HNSW index with Scout
over one clustered dataset, asking both the same question. valkey-search's HNSW index answers
a nearest-neighbour query quickly and tends to return neighbours from the query's own
cluster. `FRACTAL.EXPLORE` scans the corpus key on every call and disperses a population
across distinct basins, so it covers more clusters and costs more time. They solve different
problems; the script reports the trade-off and does not declare a winner.

```bash
docker compose --profile search up -d          # valkey-bundle (search, json, ...) + fractalsql.so
python3 bench/head_to_head.py --port 16380     # defaults: 10,000 x 128, 50 clusters, 5 queries
docker compose --profile search down
```

Needs numpy and a server with both `fractalsql` and `search` loaded; it checks and says so
if not. FractalSQL does not depend on valkey-search: the default Compose service is plain
Valkey plus this module. The `search` profile exists only for this benchmark and the
composition examples.

- The default size is small. Scout's cost grows with rows x dimension, so raise `--n` gradually, and avoid a machine
  that other work shares.
- "recall" in the table is the number of distinct clusters a method's answer covers, not nearest-neighbour recall against ground truth.
- The script creates only `hh:*` keys and one `hh_idx` index, and removes them when it ends.
- Numbers are not quoted here: they depend on your hardware, corpus and Valkey version, so run it on yours. The pattern to
  expect: HNSW returns about one cluster in a few milliseconds, and Scout returns
  several clusters in more time.
