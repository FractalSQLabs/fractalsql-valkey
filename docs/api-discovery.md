<p align="center">
  <img src="../FractalSQLforValkey.jpg" alt="FractalSQL for Valkey" width="720">
</p>

# Discovery API Reference

Two complementary modes over a corpus stored as a Valkey string, each returning a JSON document, running in the Valkey
process with the FractalSQL core statically linked. Everything else on this page (table-backed search, trajectory search,
cross-modal search, diversification) is a way of using these two, or an agent built on them.

| | `fractal_search` (Sniper) | `FRACTAL.EXPLORE` (Scout) |
| --- | --- | --- |
| Question | "what is closest?" | "what distinct things are here?" |
| Returns | the top-k nearest rows | a dispersed population plus the best point |
| Third argument | `K`, default 10 | population size, default 50 |

## `fractal_search`

### Signature

```
FRACTAL.SEARCH <corpus_key> <query_blob> [K] [SESSION <session>] [INDEX <name>] [NPROBE <n>] [RERANK <n>] [USE_INDEX <0|1>]
```

- **`corpus_key`**: a string holding `N` rows of `dim` packed little-endian float32 values, back to back.
  `N = byte_length / (dim * 4)`.
- **`query_blob`**: packed float32. Its length defines `dim`.
- **`K`**: optional, default 10.
- **`SESSION <session>`**: optional; searches through a named [session](#diversify--repulsion-sessions) so the diversification state carries from call to call.
- **`INDEX <name>`**: optional; search the [native IVF index](#the-native-ivf-index) of this corpus instead of scanning it.
- **`NPROBE <n>`**: optional; how many index cells to probe (the default is the calibrated one).
- **`RERANK <n>`**: optional; how many candidates the index passes to the exact check (a larger shortlist buys recall back when INT8 or INT4 codes lose too much; the default is set at the build). Every candidate is still scored exactly, so a larger `RERANK` only reads more rows.
- **`USE_INDEX 0`**: optional; skip the index and scan every row exactly. The reference the index is checked against.
- Limits: the query is at most 4 MiB, the corpus at most 256 MiB, and the corpus length must be a multiple of `dim * 4`.
  **NaN and Inf are refused** in both.

### Search settings

`fractal_search` runs fixed settings (30 generations, a population
of 50, diffusion factor 2, walk 0.5, bound clipping on). `FRACTAL.EXPLORE` uses 15 generations, no walk, and the population size you pass.

### Ranking

`fractal_search` ranks by **cosine distance**, not Euclidean distance: the reported `dist` is `1 - cos(angle)`. Two vectors
pointing the same way are identical however long they are. Normalise or scale your data if magnitude matters to you, or use an
agent that takes an L2 metric (`OUTLIER_INTERCEPT ... METRIC l2`).

### Return

```json
{ "dim": 4, "n_corpus": 8, "best_fit": 0.0, "best_point": [0.6, 0.8, 0, 0],
  "top_k": [ {"idx": 0, "dist": 2.2e-16}, {"idx": 3, "dist": 0.2} ] }
```

`idx` is the row's index in the corpus key (0-based). The reply shape is stable.

### Getting a plain rows-of-(idx, dist) result

Valkey replies are not row sets, so read `top_k` from the JSON in your client. To turn an index into an id or a piece of content,
keep a companion hash whose field is the row index (the agents' `IDS` and `CONTENT` options read exactly that, and
`FRACTAL.AGENT.RECALL_HYBRID` returns the joined rows for you).

### Bounds

`K` must be in `[1, 10000]` and a population in `[2, 10000]`; a value outside the range is refused with an error that names it
(`K 0 out of range [1, 10000]`). `K` above the corpus size returns every row. A non-integer is an error. `fractal_search` accepts an empty corpus
(`n_corpus: 0`); `FRACTAL.EXPLORE` refuses one.

### Errors

| Error | Meaning |
| --- | --- |
| `corpus key missing` / `corpus key not a string/blob` | no such key, or the wrong type |
| `query must be packed float32 BLOB (length % 4 == 0)` | empty or misaligned query |
| `corpus length not a multiple of dim * 4` | the query's `dim` does not divide the corpus |
| `query too large` / `corpus too large` | over the 4 MiB / 256 MiB cap |
| `query contains a non-finite value (NaN or Inf)` (also `corpus`) | refused up front |
| `K n out of range [1, 10000]` / `population_size n out of range [2, 10000]` | the value is outside the allowed range |
| `too much work in flight` | the in-flight cap was reached; retry shortly |

## Determinism

`fractal_search` takes no seed argument; the commands that accept a `SEED` (`FRACTAL.OPTIMIZE.PORTFOLIO` and
`REBALANCE_SIBLING`) are deterministic with one. The corpus-free convergence inside `FRACTAL.AGENT.SCHEDULE_WORKLOAD` refines the
task vector and returns only the match.

**Use unit-length embeddings.** With embeddings of unit length the cosine ordering and the
L2 ordering of a query are the same, so one index answers both and the ranking does not
depend on the vector's magnitude. A corpus packed with vectors of mixed magnitude does not
have that property: normalise the vectors before packing them, and keep the query vector
in the same space.

## The native IVF index

A corpus can be indexed: the rows are partitioned into k-means cells and each row gets a compact code
(INT8, INT4, FP16 or FP32). A search then reads a few cells instead of the whole corpus, and the rows it
finds are still scored exactly. The index is optional: a corpus without one searches exactly as before.

### Building and managing an index

```
FRACTAL.INDEX.CREATE  <corpus_key> <name> [DIM n] [VECTOR_FORMAT INT8|INT4|FP16|FP32] [NLIST n] [PCA_DIM n] [ITERS n] [SAMPLE n] [SEED n]
FRACTAL.INDEX.REBUILD <corpus_key> <name> [the same options]
FRACTAL.INDEX.REFRESH <corpus_key> <name>
FRACTAL.INDEX.ENSURE  <corpus_key> <name> [the same options]
FRACTAL.INDEX.DROP    <corpus_key> <name>
FRACTAL.INDEX.INFO    <corpus_key> <name>
FRACTAL.INDEX.LIST    <corpus_key>
```

- **`DIM`** is required: the corpus key holds rows of a fixed width, and nothing but the query reveals it.
- **`NLIST`** is the number of cells (0 = sqrt(rows)); **`PCA_DIM`** is the basis size of the INT8 and INT4 codes
  (0 = dim/4); **`ITERS`** is the k-means iteration count (0 = 6); **`SAMPLE`** is the training sample size
  (0 = 50000); **`SEED`** makes the build repeatable (0 = 1).
- The build runs on a worker thread and returns a JSON summary: the format, the dimension, the row count,
  the number of cells and the generation.

`FRACTAL.INDEX.INFO` returns the same facts plus a **status**, the **advice** that goes with it, the rows added since the
build, and the statistics of the last search: the rows the index scanned and the cells it probed.

| Status | Meaning | What to do |
|---|---|---|
| `ready` | The corpus is what the index was built over, and the calibration reached its target recall. | Nothing. |
| `stale` | Rows were appended since the build, or the rows it covers were rewritten or removed. | The advice says which: `FRACTAL.INDEX.REFRESH` while the added rows are up to a quarter of the index, otherwise (and after a rewrite) `FRACTAL.INDEX.REBUILD`. |
| `degraded` | The build's calibration did not reach its target recall. | Rebuild with `VECTOR_FORMAT FP16` or `FP32`, or search with a larger `RERANK`. |
| `empty` | The corpus key holds no vectors. | Load rows and build again. |
| `orphaned` | The corpus key no longer exists. | `FRACTAL.INDEX.DROP` removes the index. |

A stale index still answers: rows appended after the build are scanned exactly alongside the index candidates, so the answer
covers the whole corpus. **`FRACTAL.INDEX.REFRESH`** folds those rows into the cells with the model the index already has
(no retraining) and writes a new generation. It reports `{name, added, rows, generation, refreshed}`, and does nothing
(`refreshed: false`) when there is nothing to add. It refuses a corpus whose existing rows changed: that needs a rebuild.

**`FRACTAL.INDEX.ENSURE <corpus_key> <name> [options]`** does whichever of those is needed and says what it did: it creates the
index when there is none, refreshes it when up to a quarter of its rows were appended, rebuilds it when more were added or rows
were rewritten or removed, and leaves it alone when it is current. It replies `{action, status, generation, rows, ...}` with
`action` one of `created`, `refreshed`, `rebuilt` or `unchanged`. The options are those of `FRACTAL.INDEX.CREATE`.

**`FRACTAL.INDEX.LIST <corpus_key>`** returns one summary per index of the corpus (name, format, dimension, rows indexed,
rows now, status, generation), sorted by name.

### Searching an index

```
FRACTAL.SEARCH <corpus_key> <query_blob> 10 INDEX my_index NPROBE 8
```

The reply has the same shape as an ordinary search, plus `used_index`, `rows_scanned` and `cells_probed`.
**The distances are exact.** The index only chooses which rows to look at: every candidate is read back from
the corpus and scored with the same double-precision cosine the ordinary path uses, and the answer is ordered
by distance and then by row position. An index never returns an approximate distance.

`USE_INDEX 0` runs the same exact scan without the index, which is the reference to compare against.

`IDS <positions>` restricts the search to a list of corpus positions (comma-separated, or a JSON array). A list of at most
4096 positions, or a search with no index, scans just those rows exactly. A longer list goes through the index and is
filtered; if that leaves fewer than `K` of its rows, the list is scanned exactly instead. An empty list returns no rows, and
a malformed one is an error.

`FRACTAL.INDEX.CANDIDATES <corpus_key> <query_blob> INDEX <name> [K] [NPROBE n] [RERANK n]` returns the corpus positions of the
`K` nearest rows (verified, nearest first, appended rows included) as a JSON array, for callers that want the shortlist rather
than the distances.

### Where the index lives

The index is stored in ordinary string keys beside the corpus, so it persists, replicates and restores like
any other data:

```
<corpus_key>:fsql-idx:<name>:gen                 the generation pointer
<corpus_key>:fsql-idx:<name>:meta:<generation>    the metadata
<corpus_key>:fsql-idx:<name>:model:<generation>   the model
<corpus_key>:fsql-idx:<name>:cell:<generation>:<cell>   the ids and the codes of one cell
```

A build writes a new generation and then flips the pointer with a single write, so a reader only ever sees a
complete generation, and the writes are replicated as they are: a replica receives the finished index, it does
not rebuild it. Because the index keys keep the corpus key as their prefix, the index lives in the corpus
key's slot; in cluster mode the corpus key therefore needs a hash tag (`corpus:{1}`).

The build records the corpus key's length and a checksum of its bytes. Rows appended to the end of the corpus leave those bytes
as they were, so they are reported as added (and refreshed in place); a change to the bytes the build covered is reported as a
rewrite, which needs a rebuild.

### Notes

- The indexed search does **not** apply Diversify repulsion. Use `fractal_search` with a session, or
  `FRACTAL.AGENT.RECOMMEND_DIVERSE`, when the diversity of the answer matters.
- The distances, norms and cosines of `FRACTAL.VECTOR.*` accumulate in double, in a fixed order, and are
  returned as doubles. The index's verified distances are the same arithmetic, bit for bit.

## The precision contract

Every SQL-visible distance, norm and similarity in this host is computed by **one**
implementation (`src/fsql_precision.h`):

* the sums accumulate in **double**, in a fixed sequential order, with no fused
  multiply-add and no vectorised reduction (the files that carry a float path are built
  with `-ffp-contract=off -fno-tree-vectorize`, `/fp:precise /Qvec-` on MSVC);
* the result is returned as a **double** -- the one rounding to float32 happens only where
  a float32 result is the contract (the canonical vector text, which round-trips through
  `%.9g`);
* so every SQL-visible cosine, L2 distance, dot product and norm agrees **to the last bit**
  with each other, with the indexed search's verified distances, and with every other
  FractalSQL host;
* the cosine is bounded to **[-1, 1]** by construction, and the comparison is
  cross-platform: the same corpus and the same query give the same ranking on amd64,
  arm64, ppc64le and s390x;
* ties break on the **row id**, so the same corpus and the same query give the same
  ordering everywhere.

The Sniper convergence itself (`fractal_sfs_converge`) runs inside the core, which keeps
its own double-precision accumulation over the unit box [-1, 1]^d; the distances it reports
are the core's.

## Table-backed search: a corpus key and its companion hashes

A corpus key holds only vectors; the id, content and filter attribute of each row live in **companion hashes** keyed by row index.
Three commands do the join for you:

- **`FRACTAL.AGENT.RECALL_HYBRID corpus query [K n] [IDS h] [CONTENT h] [FILTER h value]`** returns the
  nearest rows **inside a cohort** (`FILTER hash value` keeps rows whose
  entry equals `value`), returned with their id and content. See [api-agency.md](api-agency.md).
- **`FRACTAL.AGENT.PATIENT_DETERIORATION_TRIAGE`** searches a cohort and the drift of an entity from its baseline in one call.
- **`FRACTAL.AGENT.RECOMMEND_DIVERSE`** is the repulsion-diverse top-k.

A two-predicate cohort (for example year **and** component) is one hash whose value is the combined predicate (`2023|battery`).

## Search on a change

"What changed" is often a better query than "where am I". Search for the vector `current - baseline`: do the
subtraction in your client (or let an agent do it) and search for the result:

```
FRACTAL.SEARCH signatures <delta_blob> 1        # delta = current - baseline, packed float32
```

`FRACTAL.AGENT.TRACK_ANOMALY`, `DETOUR_CLASSIFY`, `PATIENT_DETERIORATION_TRIAGE`, `REBALANCE_SIBLING` and `TRAJECTORY_PREDICT` all do this
internally (the subtraction is done in float32). A baseline equal to the current state
gives a zero vector, which has no direction. It is not refused: the search returns an arbitrary row at cosine distance 1.0, so read a
`trajectory_distance` or `drift_distance` of exactly 1.0 as "no change", not as a match.

## Cross-modal search (`fractal_cross_modal_search`)

Blend two embeddings (content and behaviour, say) with weights, then search the result:

```
FRACTAL.VECTOR.CONCAT <content_blob> <alpha> <behaviour_blob> <beta>      # weighted concatenation
FRACTAL.SEARCH catalog <blended_blob> 10                                   # catalog rows are stored the same way
```

`FRACTAL.VECTOR.QUANTIZE.INT8` and `.BINARY` produce compact forms for a cheap Hamming pre-filter ahead of a full-precision re-rank. See
[Analytics: vector math](api-analytics.md#vector-math-and-quantization).

## `FRACTAL.SFS.CONVERGE`

The table-free **convergence** command (Sniper Mode): the Stochastic Fractal Search runs toward the query direction and answers where it converged. It reads no key and is not a retrieval command (the indexed search is `FRACTAL.SEARCH ... INDEX`).

### Signature
```
FRACTAL.SFS.CONVERGE query_blob [ITERATIONS n] [POPULATION n] [DIFFUSION n]
```
`ITERATIONS` 1 to 10000 (default 30), `POPULATION` 2 to 10000 (default 50), `DIFFUSION` 1 to 100 (default 2); `ITERATIONS` x `POPULATION` may not exceed 10,000,000.

### Return
A JSON document: `{"dim":..,"population_size":..,"best_point":[..],"best_fit":..}`. `best_point` is the converged vector and `best_fit` its cosine distance. The command runs on a worker thread, like the agents.

> The tool is `fractal_sfs_converge` in the MCP server.

## `FRACTAL.EXPLORE`

### Signature

```
FRACTAL.EXPLORE <corpus_key> <query_blob> [population_size] [SESSION <session>]
```

The corpus and query are as above. Scout disperses its particles across the data's distinct basins around the query, so it reaches
wider than top-k, and a larger population reaches further: in the recommendation kit (240 items in six clusters), a population of a few
dozen stayed in the query's own cluster and a few hundred reached all six. It scans the corpus on every call (O(N x dim)), which is its trade-off against an index; see
[features.md](features.md#-benchmarks--scaling).

### Return

The same document as `fractal_search` plus a `population` array, one entry per particle. Each particle is a **point in the space**, not a
row index, so to say which rows a run reached, credit each particle to the row nearest to it (every kit that uses `EXPLORE` does).

## Threading and your other clients

Both commands copy their inputs on the main thread and run on a worker, so a long search never blocks other clients: the server answered
`PING` in a few milliseconds during an 18-second `EXPLORE`. Inside `MULTI` or a Lua script, which may not block, they run inline and still
reply. Workers are set with the `threads` load option; the `max-inflight-mb` cap makes oversize requests fail fast instead of queueing memory.

## Diversify / Repulsion (sessions)
**Persisted ledgers.** The rejections a session records live in the module's memory. To keep
them across a restart, use the ledger surface of `fractalsqld` (the `FRACTAL.LEDGER.*`
commands, reachable through the MCP's ledger tools): a flush writes a QTL v2 snapshot holding
every entry's weight and, for a rejected row, the repulsion shape (intensity and sigma) it was
recorded with, and a load restores that snapshot exactly -- the same rejections, with the same
strength -- and **replaces** the in-memory ledger. A snapshot written by an older version
(which stored no weights) comes back with every entry at weight 1.0, taking its repulsion
shape from the current session's parameters.

Search can be made to *avoid* what it has already shown. The repulsion layer records the rows a caller rejected (negative feedback) and
pushes later searches away from their neighbourhoods; its diversity quotient (D_q) reads how widely the last searches spread. It is
**off by default**, and a search that does not name a session behaves exactly as it always did.

Valkey has no state that survives a reconnect or a proxy, so a session is
**named by you**: `FRACTAL.DIVERSIFY.ENABLE <session>` creates it, and `SESSION <session>` on `fractal_search` or `FRACTAL.EXPLORE`
searches through it. Any client that knows the name shares the state.

| Command | Reply | What it does |
| --- | --- | --- |
| `FRACTAL.DIVERSIFY.ENABLE <session>` | `OK` | Creates the session if it is new and turns the repulsion layer on. |
| `FRACTAL.DIVERSIFY.DISABLE <session>` | `OK` | Turns it off; the session stays. |
| `FRACTAL.DIVERSIFY.PARAMS <session> [json]` | the parameters, as JSON | Reads the tuning, or sets the fields you name (the rest are kept). |
| `FRACTAL.DIVERSIFY.DQ <session>` | a number, or nil | The D_q of the last search. Nil until the session has searched twice, and while diversification is off. |
| `FRACTAL.DIVERSIFY.EXPLAIN <session>` | `{"dq","diversify_enabled","overhead_p99_us"}` | A health readout for the session. It is not a per-row trace. |
| `FRACTAL.FEEDBACK <session> <row> DWELL\|POSITIVE\|NEGATIVE [dwell_ms]` | `OK` | Reports engagement on a row of a previous result. `row` is the 0-based row of the corpus (the `idx` in the reply). |
| `FRACTAL.ISOLATE_BACKGROUND <session> <row>` | `OK` | Negative feedback with no dwell time. |
| `FRACTAL.SESSION.CLOSE <session>` | `OK` | Drops the session and its state. |
| `FRACTAL.SESSION.LIST` | the session names | The live sessions. |

`PARAMS` takes a flat JSON object. Every field is optional, and a refused call changes nothing:

| Key | Meaning |
| --- | --- |
| `window_n` | How many recent searches the D_q window covers (1 to 32, default 5). |
| `stall_threshold` | D_q below this counts as a stall (default 0.15). |
| `repulsion_sigma` | Width of each rejected row's repulsion field (0 picks a value from the dimension). |
| `repulsion_weight` | Overall strength of the penalty (default 1). |
| `max_shadows_considered` | How many recorded rejections can influence one search (default 64). |
| `tail_buffer_cap` | Reserved. Range-checked (1 to 1024) and stored, with no effect yet. |

The loop:

```
FRACTAL.DIVERSIFY.ENABLE shopper-7
FRACTAL.EXPLORE catalog <query> 20 SESSION shopper-7        # the first, diverse result
FRACTAL.FEEDBACK shopper-7 18 NEGATIVE                       # the user rejected row 18
FRACTAL.EXPLORE catalog <query> 20 SESSION shopper-7        # the same query; row 18's neighbourhood is now avoided
FRACTAL.DIVERSIFY.EXPLAIN shopper-7                          # {"dq":0.66,"diversify_enabled":true,...}
FRACTAL.SESSION.CLOSE shopper-7
```

How a session behaves:

- **It is state, not data.** Nothing is written to a key, so nothing is replicated and nothing reaches the AOF. A restart drops every session.
- **It lives on one node.** In a cluster, send every call for a session to the node that created it.
- **It is bounded.** At most `max-sessions` exist at once (default 64, a load option) and `ENABLE` of a new one past the limit is refused with
  `too many sessions`. A session idle for `session-ttl-sec` (default 3600; 0 never expires) is dropped the next time the table is used.
- **Searches on one session run one at a time.** A control command (`PARAMS`, `FEEDBACK`, `DQ`, ...) never waits for a running search: it
  answers `session busy: a search is running on it; retry`, so a long search cannot stall the server.
- **Closing is safe at any moment.** A search already running on a session you close finishes and replies; the state is freed after it.
- **Names** are 1 to 64 characters of `A-Z a-z 0-9 _ . : -`. Searching through a session that does not exist is an error.
- **Access.** The mutating commands are in the `@write` ACL category and the readers in `@read`, so `+@read` alone cannot create a session or
  change one. `SESSION` on a search adds no extra permission beyond the search itself.

The two agents that use diversification do not need a session; they keep their state for a single call so one caller never affects another:

- **`FRACTAL.AGENT.RECOMMEND_DIVERSE`** switches repulsion on for its one search. It fails with "diversification is unavailable on this core
  build" rather than silently returning a plain search.
- **`FRACTAL.AGENT.FEEDBACK_AUDIT`** runs the whole loop (enable, warm up, search, report negative feedback on the top hit, read the
  diagnostics) on one private context and returns `{"diversity_quotient", "explanation":{"dq","diversify_enabled","overhead_p99_us"}}`. It
  is the observable proof that diversification ran.

## Which functions use the index

The `agent_use_index` setting (`off` by default; the key, GUC, module argument or property that carries it on each host is in the settings table in [Reasoning Setup](reasoning-setup.md)) decides whether the agents below search through a `fractal_index` when one covers the table and column. Every distance an indexed search returns is still exact, and rows added after the build are scanned exactly. This table lists what goes through the index and, for the rest, why not. The rows use the SQL names of the functions; here they are `FRACTAL.*` commands.

| Function or agent | Search it does | Index |
| --- | --- | --- |
| `fractal_search` | nearest rows, exact scan or index | Yes: this is the indexed search |
| `fractal_agent_route_task`, `fractal_agent_schedule_workload` | the nearest row | Yes, with `agent_use_index` |
| `fractal_agent_outlier_intercept` (cosine) | the nearest row | Yes, with `agent_use_index` (L2 scans exactly) |
| `fractal_search_agent`, `fractal_rag_agent` | the top rows for a question | Yes, with `agent_use_index`; a metadata filter restricts the indexed search to the matching rows |
| `fractal_agent_recall_hybrid` | the nearest rows within a cohort | Yes where the host has it and the cohort is a list of row ids: the cohort goes to the indexed search as its `ids` option (SQLite, PostgreSQL, DuckDB, MariaDB) |
| `fractal_hybrid_clinical_search` | the nearest rows within a cohort | No: exact scan of the cohort |
| `fractal_agent_rebalance_sibling`, `fractal_agent_detour_classify`, `fractal_agent_track_anomaly`, `fractal_agent_trajectory_predict`, `fractal_search_trajectory`, `fractal_agent_patient_deterioration_triage` | nearest row to a computed vector (current minus baseline) | No: exact scan. The index was tuned with queries taken from the rows themselves, and a computed query has not been measured against it |
| `fractal_cross_modal_search` | nearest rows to a combined vector | No: exact scan, for the same reason |
| `fractal_search_telemetry`, `fractal_agent_recommend_diverse`, `fractal_agent_feedback_audit` | exact scan with Diversify repulsion | No, by design: the repulsion needs every row's distance, and the indexed search does not apply it |
| `fractal_search_explore`, `fractal_agent_plan_explore` | population (Scout) search | No, by design: the search is exploratory, not nearest-row |
| `fractal_index_candidates` | a shortlist of ids from the index | It is the index; the shortlist is for a caller's own follow-up search |
| the other agents (allocate, anomaly triage, regime triage, loop detection, data analyst, SQL agent, and so on) | no vector search | Not applicable |
