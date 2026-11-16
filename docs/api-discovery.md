<p align="center">
  <img src="../FractalSQLforValkey.jpg" alt="FractalSQL for Valkey" width="720">
</p>

# Discovery API Reference

Two complementary modes over a corpus stored as a Valkey string, each returning a JSON document, running in the Valkey
process with the FractalSQL core statically linked. Everything else on this page (table-backed search, trajectory search,
cross-modal search, diversification) is a way of using these two, or an agent built on them.

| | `FRACTAL.SEARCH` (Sniper) | `FRACTAL.EXPLORE` (Scout) |
| --- | --- | --- |
| Question | "what is closest?" | "what distinct things are here?" |
| Returns | the top-k nearest rows | a dispersed population plus the best point |
| Third argument | `K`, default 10 | population size, default 50 |

## `FRACTAL.SEARCH`

### Signature

```
FRACTAL.SEARCH <corpus_key> <query_blob> [K] [SESSION <session>]
```

- **`corpus_key`**: a string holding `N` rows of `dim` packed little-endian float32 values, back to back.
  `N = byte_length / (dim * 4)`.
- **`query_blob`**: packed float32. Its length defines `dim`.
- **`K`**: optional, default 10.
- **`SESSION <session>`**: optional; searches through a named [session](#diversify--repulsion-sessions) so the diversification state carries from call to call.
- Limits: the query is at most 4 MiB, the corpus at most 256 MiB, and the corpus length must be a multiple of `dim * 4`.
  **NaN and Inf are refused** in both.

### Search settings

`FRACTAL.SEARCH` runs fixed settings (30 generations, a population
of 50, diffusion factor 2, walk 0.5, bound clipping on). `FRACTAL.EXPLORE` uses 15 generations, no walk, and the population size you pass.

### Ranking

`FRACTAL.SEARCH` ranks by **cosine distance**, not Euclidean distance: the reported `dist` is `1 - cos(angle)`. Two vectors
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
(`K 0 out of range [1, 10000]`). `K` above the corpus size returns every row. A non-integer is an error. `FRACTAL.SEARCH` accepts an empty corpus
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

`FRACTAL.SEARCH` takes no seed argument; the commands that accept a `SEED` (`FRACTAL.OPTIMIZE.PORTFOLIO` and
`REBALANCE_SIBLING`) are deterministic with one. The corpus-free convergence inside `FRACTAL.AGENT.SCHEDULE_WORKLOAD` refines the
task vector and returns only the match.

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

The same document as `FRACTAL.SEARCH` plus a `population` array, one entry per particle. Each particle is a **point in the space**, not a
row index, so to say which rows a run reached, credit each particle to the row nearest to it (every kit that uses `EXPLORE` does).

## Threading and your other clients

Both commands copy their inputs on the main thread and run on a worker, so a long search never blocks other clients: the server answered
`PING` in a few milliseconds during an 18-second `EXPLORE`. Inside `MULTI` or a Lua script, which may not block, they run inline and still
reply. Workers are set with the `threads` load option; the `max-inflight-mb` cap makes oversize requests fail fast instead of queueing memory.

## Diversify / Repulsion (sessions)

Search can be made to *avoid* what it has already shown. The repulsion layer records the rows a caller rejected (negative feedback) and
pushes later searches away from their neighbourhoods; its diversity quotient (D_q) reads how widely the last searches spread. It is
**off by default**, and a search that does not name a session behaves exactly as it always did.

Valkey has no state that survives a reconnect or a proxy, so a session is
**named by you**: `FRACTAL.DIVERSIFY.ENABLE <session>` creates it, and `SESSION <session>` on `FRACTAL.SEARCH` or `FRACTAL.EXPLORE`
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
