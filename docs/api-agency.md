<p align="center">
  <img src="../FractalSQLforValkey.jpg" alt="FractalSQL for Valkey" width="720">
</p>

# Sovereign Agency

FractalSQL for Valkey ships **21 agent commands**: autonomous, self-correcting routines that compose the module's
Discovery, Analytics and Cognition primitives into end-to-end workflows. Sixteen are ready-made recipes and five are the
building blocks the recipes are made from. (`DATA_ANALYST` answers a question from a [valkey-search](https://github.com/valkey-io/valkey-search) index, so it needs that module loaded.) Each agent is a *recipe* for a recurring agentic pattern: "drift on a metric
series", "match a task to the best capable target", "block an action near a known-bad state". You pick the recipe that
matches your problem, point it at your keys, and get back real computed results plus a human-readable read.

This page is written for the **driver**, not the mechanic. For each agent you will find what problem it solves, the
inputs it needs, what it returns, a runnable example with its real output, and the non-obvious notes. If you want to
build a recipe that is not in the box, see [Composing your own](#composing-your-own) and
[Building blocks](#building-blocks-the-primitives-agents-compose) further down.

> **Every agent here is a command, and its "tables" are keys.** Valkey has no SQL layer, so there is no table to name and
> no column to point at. An agent reads its inputs from **keys**: a series is a string key of packed little-endian
> float32; a table of rows is a **corpus key** (`dim` floats per row, back to back); and the columns the SQL products
> read beside the vector (an id, some content, a filter attribute) are **companion hashes** whose field is the row
> index. A vector argument is a packed float32 blob, exactly as `FRACTAL.SEARCH` takes it. Every example on this page is
> a plain command; the replies shown were produced by running it.

---

## Install

Nothing to install: the agents are part of `fractalsql.so`, and load with it
([getting-started.md](getting-started.md#5-install-without-docker)). Three things to know:

- **Reasoning is optional.** Thirteen agents (eleven recipes, plus `SEARCH_AGENT` and `RAG_AGENT`) call a reasoner through `fractalsqld`
  ([reasoning-setup.md](reasoning-setup.md)); without one they still return everything they computed (see
  [Partial results](#partial-results-when-the-reasoner-is-not-available)). Three need an embedding provider
  (`PLAN_EXPLORE`, `SEARCH_AGENT`, `RAG_AGENT`) and fail with the daemon's "not configured" error without one.
- **ACL.** An agent that sends data to a provider is in the `@dangerous` category (like `FRACTAL.REASON`), so a user
  granted only `+@read` cannot run it. The retrieval and analytics agents are `@read`. Only one key is a declared key (the first
  argument; for `DETECT_LOOP`, the state log); every other key an agent reads (a companion hash, a second series) is checked
  against the caller's key patterns too, and answers `NOPERM` if it is not covered. `COMMAND GETKEYS` reports only the declared one.
- **Cluster.** An agent that reads more than one key needs them in the same hash slot (use a hash tag, for example
  `{fleet}:corpus` and `{fleet}:ids`).

---

## Which agent should I use?

Pick by the problem shape, not by the command name.

| Your problem | Agent | Recipe in one line | Model? |
| --- | --- | --- | --- |
| "Is this entity's metric series drifting into a new regime?" (one key per entity) | `ANOMALY_TRIAGE` | drift on one entity's series, then a triage | reasoner |
| "Is this single series in a regime change?" (a series, no entity) | `REGIME_TRIAGE` | DFA + drift over one series, then a triage | reasoner |
| "Did this vessel/host track deviate from the fleet?" | `TRACK_ANOMALY` | search on the change + heading DFA, then a triage | reasoner |
| "Did this vehicle detour, and how complex is its GPS trace?" | `DETOUR_CLASSIFY` | search on the change + box-counting, then a classification | reasoner |
| "Is my sensor grid's coverage degrading?" | `NETWORK_COVERAGE_ALERT` | spatial morphology + telemetry drift, then an alert | reasoner |
| "What is the best cardinality-constrained allocation?" | `ALLOCATE` | SFS Sharpe optimizer, then a rationale | reasoner |
| "I want several genuinely different allocations" (enterprise tier) | `DIVERSE_PORTFOLIOS` | multi-restart SFS + diverse select, then one comparison | reasoner |
| "Rebalance, and compare to the nearest historical allocation?" | `REBALANCE_SIBLING` | optimizer + search on the change, then a rationale | reasoner |
| "Which target should handle this incoming task?" | `ROUTE_TASK` | nearest-capability search + budget accounting | reasoner |
| "Which node should this workload land on (with vector refinement)?" | `SCHEDULE_WORKLOAD` | SFS refine + nearest node, then a rationale | reasoner |
| "Should I block this proposed action (is it near a known-bad state)?" | `OUTLIER_INTERCEPT` | distance-to-bad-state barrier, then a reason | reasoner |
| "Triage an entity against a cohort + baseline-to-current drift" | `PATIENT_DETERIORATION_TRIAGE` | cohort search + drift on the change, then a triage | reasoner |
| "Ask a natural-language question over my data" | `DATA_ANALYST` | question to query to result to analysis | reasoner, valkey-search |
| "Recall similar past memories, restricted by a metadata filter" | `RECALL_HYBRID` | cohort-restricted vector recall | no |
| "Recommend diverse items, avoiding recently-rejected ones" | `RECOMMEND_DIVERSE` | repulsion-diverse top-k | no |
| "Audit my recommender's diversity health" | `FEEDBACK_AUDIT` | diversify loop + collapse diagnostics | no |
| "Where is this state sequence heading?" | `TRAJECTORY_PREDICT` | nearest known state to (latest - baseline) | no |
| "Is this agent stuck in a loop?" | `DETECT_LOOP` | fingerprint cycle + DFA of the state norms | no |
| "Plan a spread of distinct strategies from a seed state" | `PLAN_EXPLORE` | embed + Scout search | embedding |
| "Answer a question from my documents, with sources" | `SEARCH_AGENT` / `RAG_AGENT` | embed, retrieve, then reason | embedding + reasoner |

> **A guarantee about the outputs.** Every field in a reply is a **real primitive result, not a literal**. `threat_score`
> is the computed drift (the number `FRACTAL.DIM.DRIFT` gives for the same series); `allocation`/`sharpe` are the
> optimizer's own `{"sharpe","weights"}` JSON; `routed_to` is the real nearest capability id and `confidence` is
> `1/(1+distance)`; `intercepted` is a real distance-versus-threshold comparison; `mem_id`/`content` are the real
> recalled row; `item_id`/`score` are the real catalog id and `1 - cosine distance`. The agents' narrative fields
> (`triage_summary`, `rationale`, `reason`, `answer`) are real `FRACTAL.REASON` output. Every agent answers a bad argument
> with a clean, agent-specific error (a missing key, a key of the wrong type, a non-finite value, a window that is too
> long for the series, a filter that matches no row).

## Partial results when the reasoner is not available

Every reasoning agent does its **computation first** and calls the reasoner last. If the reasoner is not configured,
cannot be reached, or refuses, the agent does not throw the computation away: it returns the computed fields and replaces
the narrative with `null`, adding `reasoning_step_failed` with the error:

```json
{"threat_score":-0.0512548103,"anomaly_type":"vector_drift","triage_summary":null,"reasoning_step_failed":"the fractalsqld daemon is not configured"}
```

This is the shape of every example reply below, because the examples were run without a reasoner. With one configured the
narrative field holds its text and `reasoning_step_failed` is absent. `SEARCH_AGENT` and `RAG_AGENT` are the exception, as in the
the other editions: by default a reasoning failure after retrieval is an **error**; load the module with
`agent-partial-reasoning-fail yes` to get the retrieval plus `reasoning_step_failed` instead.

---

## The recipes

### Anomaly triage: `FRACTAL.AGENT.ANOMALY_TRIAGE`
**Problem it solves.** Scores how far one entity's metric series has drifted into a new regime and, when a reasoner is configured, writes a one-sentence triage.

```
FRACTAL.AGENT.ANOMALY_TRIAGE series_key [baseline_window]
```

**Inputs.**

- `series_key`: a string key holding the entity's metric series, packed little-endian float32, oldest first (one key per entity)
- `baseline_window`: optional, 16 to 16777216, default 32; the series needs at least `baseline_window + 16` samples

**Returns** a JSON document:

- `threat_score`: the `drift` value of `FRACTAL.DIM.DRIFT` for the same series and window
- `anomaly_type`: always `"vector_drift"`
- `triage_summary`: the reasoner's sentence, or `null`
- `reasoning_step_failed`: present, with the error, when `triage_summary` is `null`

**Example.** (the reply is real, from a server with no reasoner)

```
> FRACTAL.AGENT.ANOMALY_TRIAGE host:17:latency 32
{"threat_score":1.2653238663,"anomaly_type":"vector_drift","triage_summary":null,"reasoning_step_failed":"the fractalsqld daemon is not configured"}
```

**Notes.** Category: `@dangerous` (it sends data to a provider). Without a reasoner the computed fields are still returned (see [Partial results](#partial-results-when-the-reasoner-is-not-available)).

### Allocate: `FRACTAL.AGENT.ALLOCATE`
**Problem it solves.** Chooses the best `k` of `n` assets under a cardinality constraint with the SFS optimizer and explains the risk/return.

```
FRACTAL.AGENT.ALLOCATE mu cov cardinality [context_json]
```

**Inputs.**

- `mu`: expected returns, `n` packed float32 values (inline blob, no key)
- `cov`: the `n x n` covariance matrix, row-major packed float32
- `cardinality`: `k`, 1 to `n` (at most 512 assets)
- `context_json`: optional JSON passed to the reasoner verbatim (1 to 65536 bytes)

**Returns** a JSON document:

- `allocation`: the optimizer's own `{"sharpe","weights"}` document
- `sharpe`: the same Sharpe ratio
- `rationale`: the reasoner's explanation, or `null` with `reasoning_step_failed`

**Example.** (the reply is real, from a server with no reasoner)

```
> FRACTAL.AGENT.ALLOCATE <mu> <cov> 3
{"allocation":{"sharpe":1.0567622504,"weights":[0.0,0.3130193785,0.0,0.4044321471,0.0,0.2825484745]},"sharpe":1.0567622504,"rationale":null,"reasoning_step_failed":"the fractalsqld daemon is not configured"}
```

**Notes.** Category: `@dangerous` (it sends data to a provider). Without a reasoner the computed fields are still returned (see [Partial results](#partial-results-when-the-reasoner-is-not-available)).

### Route task: `FRACTAL.AGENT.ROUTE_TASK`
**Problem it solves.** Matches a task embedding to the best-capable target and tracks a token budget.

```
FRACTAL.AGENT.ROUTE_TASK capability_key task budget [COST_PER_ROUTE n] [IDS hash]
```

**Inputs.**

- `capability_key`: a corpus key: one capability embedding per row
- `task`: the task embedding, packed float32; its length is the corpus dimension
- `budget`: integer tokens available
- `COST_PER_ROUTE`: tokens spent per routing, default 150
- `IDS`: a hash mapping row index to the capability's name; without it the id is the row index

**Returns** a JSON document:

- `routed_to`: the nearest capability's id **as a string**
- `confidence`: `1 / (1 + cosine distance)`
- `remaining_budget`: `budget - COST_PER_ROUTE`
- `rationale`: the reasoner's justification, or `null` with `reasoning_step_failed`

**Example.** (the reply is real, from a server with no reasoner)

```
> FRACTAL.AGENT.ROUTE_TASK caps <task> 1000
{"routed_to":"3","confidence":0.9984771772,"remaining_budget":850,"rationale":null,"reasoning_step_failed":"the fractalsqld daemon is not configured"}
```

**Notes.** Category: `@dangerous` (it sends data to a provider). Without a reasoner the computed fields are still returned (see [Partial results](#partial-results-when-the-reasoner-is-not-available)).

### Outlier intercept: `FRACTAL.AGENT.OUTLIER_INTERCEPT`
**Problem it solves.** A pre-commit safety barrier: screens a proposed state against a history of known-bad states and holds it when it is too close to one.

```
FRACTAL.AGENT.OUTLIER_INTERCEPT history_key state threshold [METRIC cosine|l2]
```

**Inputs.**

- `history_key`: a corpus key of known-bad states
- `state`: the proposed state, packed float32
- `threshold`: a finite number; the state is intercepted when the nearest distance is **below** it
- `METRIC`: `cosine` (default, compares shape) or `l2` (exact Euclidean, compares magnitude too)

**Returns** a JSON document:

- `intercepted`: boolean
- `nearest_distance`: distance to the closest known-bad state under the chosen metric
- `threshold`: echoed
- `metric`: `"cosine"` or `"l2"`
- `reason`: the reasoner's one-sentence reason (this agent names the field `reason`, not `rationale`), or `null` with `reasoning_step_failed`

**Example.** (the reply is real, from a server with no reasoner)

```
> FRACTAL.AGENT.OUTLIER_INTERCEPT bad <proposed> 0.1
{"intercepted":true,"nearest_distance":0.0033884901,"threshold":0.1,"metric":"cosine","reason":null,"reasoning_step_failed":"the fractalsqld daemon is not configured"}
```

**Notes.** Category: `@dangerous` (it sends data to a provider). Without a reasoner the computed fields are still returned (see [Partial results](#partial-results-when-the-reasoner-is-not-available)).

### Recall hybrid: `FRACTAL.AGENT.RECALL_HYBRID` (pure retrieval, no LLM)
**Problem it solves.** Recalls the most similar past rows inside a cohort selected by a strict metadata filter.

```
FRACTAL.AGENT.RECALL_HYBRID corpus_key query [K n] [IDS hash] [CONTENT hash] [FILTER hash value]
```

**Inputs.**

- `corpus_key`: a string key of `dim x N` packed float32; `dim` is the query's length
- `query`: the query vector, packed float32
- `K`: how many to return, default 5, at most 10000
- `IDS`: a hash: field = row index, value = the row's id (default: the index)
- `CONTENT`: a hash: field = row index, value = the row's content
- `FILTER`: `hash value`: only rows whose entry in `hash` equals `value` are in the cohort

**Returns** a JSON document:

- `mem_id`: the row's id (a string from `IDS`, otherwise an integer index)
- `content`: the row's content, or `null`

**Example.** (the reply is real, from a server with no reasoner)

```
> FRACTAL.AGENT.RECALL_HYBRID mem <q> K 3
[{"mem_id":7,"content":null},{"mem_id":26,"content":null},{"mem_id":6,"content":null}]
```

**Notes.** Category: `@read`.

### Recommend diverse: `FRACTAL.AGENT.RECOMMEND_DIVERSE` (pure retrieval, no LLM)
**Problem it solves.** Returns the nearest items to a query with the core's diversity (repulsion) switched on, so the set is not six near-copies.

```
FRACTAL.AGENT.RECOMMEND_DIVERSE catalog_key query [K n] [IDS hash]
```

**Inputs.**

- `catalog_key`: a corpus key
- `query`: the query vector
- `K`: default 10
- `IDS`: as above

**Returns** a JSON document:

- `item_id`: the id
- `score`: `1 - cosine distance`, nearest first

**Example.** (the reply is real, from a server with no reasoner)

```
> FRACTAL.AGENT.RECOMMEND_DIVERSE catalog <q> K 4
[{"item_id":10,"score":0.9200803574},{"item_id":20,"score":0.9049950771},{"item_id":33,"score":0.9030684397},{"item_id":15,"score":0.8767347074}]
```

**Notes.** Category: `@read`.

### Data analyst: `FRACTAL.AGENT.DATA_ANALYST`
**Problem it solves.** Answer a plain-language question about the documents in a valkey-search index: the model writes the query, the module runs it, and the model reads the result.

```
FRACTAL.AGENT.DATA_ANALYST index question [MAX_ATTEMPTS n] [LIMIT n] [CONTEXT json]
```

**Inputs.**

- `index`: the name of a valkey-search index (letters, digits and `_ : . -`, up to 128 bytes)
- `question`: the question, 1 to 4096 bytes
- `MAX_ATTEMPTS`: how many times a rejected query is fed back to the model to correct, 1 to 5 (default 2)
- `LIMIT`: the most rows returned, 1 to 100 (default 20); `total` still reports how many matched
- `CONTEXT`: optional JSON of extra facts for the model

**Returns** a JSON document:

- `generated_query`: the filter expression the model wrote
- `result_json`: `{"total": n, "rows": [{"key": ..., "fields": {...}}]}`, as valkey-search returned it. Binary fields such as vectors are reported as `"<binary N bytes>"`, not printed
- `analysis`: the model's reading of the result, or `null` with `reasoning_step_failed` set if the reasoner could not be reached (the rows are still returned)

```
> FRACTAL.AGENT.DATA_ANALYST people "who is older than thirty?" LIMIT 2
{"analysis":"...","generated_query":"@age:[(30 +inf]","result_json":{"total":3,"rows":[{"key":"d:1","fields":{"__v_score":"1","name":"bob","age":"31","v":"<binary 8 bytes>"}},...]}}
```

**Notes.**

- **The model never chooses a command.** It supplies only a valkey-search filter expression. The module builds the command itself, always `FT.SEARCH <index> <expression> ... LIMIT 0 <n> DIALECT 2`, and passes the expression as one argument. An expression that is empty, longer than 512 bytes, contains a control or non-ASCII character, or contains `=>` is refused and fed back; if no attempt is accepted the command returns an error, and nothing else is run.
- **It runs as you.** The query is executed with the calling user's own ACL, so the agent adds no privilege. The caller needs `+ft.info` and `+ft.search`, and valkey-search additionally requires the user to be allowed its `@search` category.
- **It needs a vector field.** valkey-search requires every query to include a nearest-neighbour clause, so the index must have a vector field; the module adds the clause for you with a neutral vector, and the filter is what selects rows.
- **Where it will not run.** Inside `MULTI`, a script or a function (there is no calling client to take the user from), or without a configured `fractalsqld` daemon, and it needs the valkey-search module loaded.
- Category: `@dangerous`, like the other agents that send data to a provider.

### Patient deterioration triage: `FRACTAL.AGENT.PATIENT_DETERIORATION_TRIAGE`
**Problem it solves.** A cohort search plus a baseline-to-current drift. Domain-neutral: an entity's intake baseline plays the admission baseline.

```
FRACTAL.AGENT.PATIENT_DETERIORATION_TRIAGE corpus_key query baseline current [K n] [IDS hash] [FILTER hash value]
```

**Inputs.**

- `corpus_key`: the cohort corpus
- `query`: the entity's current profile, searched against the cohort
- `baseline, current`: the entity's intake and present vectors (float32); the **change** `current - baseline` is searched over the whole corpus
- `K`: default 5
- `IDS / FILTER`: as in recall_hybrid; `FILTER` restricts the cohort search, not the drift search

**Returns** a JSON document:

- `nearest_cohort_id`: id of the closest cohort row
- `cohort_distance`: its cosine distance
- `drift_distance`: distance from the change to the nearest corpus row
- `rationale`: or `null` with `reasoning_step_failed`
- `cohort_matches`: `[{"id","distance"}]`, nearest first

**Example.** (the reply is real, from a server with no reasoner)

```
> FRACTAL.AGENT.PATIENT_DETERIORATION_TRIAGE cohort <q> <base> <cur> K 3
{"nearest_cohort_id":7,"cohort_distance":0.0012317156,"drift_distance":0.0,"rationale":null,"reasoning_step_failed":"the fractalsqld daemon is not configured","cohort_matches":[{"id":7,"distance":0.0012317156},{"id":26,"distance":0.0234473236},{"id":6,"distance":0.0914190089}]}
```

**Notes.** Category: `@dangerous` (it sends data to a provider). Without a reasoner the computed fields are still returned (see [Partial results](#partial-results-when-the-reasoner-is-not-available)).

### Feedback audit: `FRACTAL.AGENT.FEEDBACK_AUDIT` (pure analytics, no LLM)
**Problem it solves.** Warms up the core's diversification monitor on a catalog, reports negative feedback on the query's top hit, and returns the diversity diagnostics.

```
FRACTAL.AGENT.FEEDBACK_AUDIT catalog_key query warmup_key [WARMUP_COUNT n] [K n]
```

**Inputs.**

- `catalog_key`: the catalog corpus
- `query`: the audited query
- `warmup_key`: a corpus key whose first `WARMUP_COUNT` rows (default 8, at most 256) are searched first
- `K`: default 3

**Returns** a JSON document:

- `diversity_quotient`: the monitor's D_q, 0 if not yet computed
- `explanation`: `{"dq","diversify_enabled","overhead_p99_us"}`

**Example.** (the reply is real, from a server with no reasoner)

```
> FRACTAL.AGENT.FEEDBACK_AUDIT cat <q> warm
{"diversity_quotient":0.6816469128,"explanation":{"dq":0.681647,"diversify_enabled":true,"overhead_p99_us":null}}
```

**Notes.** Category: `@read`.

### Schedule workload: `FRACTAL.AGENT.SCHEDULE_WORKLOAD`
**Problem it solves.** Refines a task vector with an SFS convergence, then places it on the nearest node.

```
FRACTAL.AGENT.SCHEDULE_WORKLOAD node_key task [ITERATIONS n] [POPULATION n] [CONTEXT json] [IDS hash]
```

**Inputs.**

- `node_key`: a corpus key of node capability vectors
- `task`: the workload's requirement vector (at most 4096 dimensions)
- `ITERATIONS`: 1 to 10000, default 30
- `POPULATION`: 1 to 100000, default 50; `ITERATIONS x POPULATION` is capped at 10,000,000 to bound the work one call can do
- `CONTEXT`: JSON for the reasoner
- `IDS`: as above

**Returns** a JSON document:

- `assigned_node`: the node's id as a string
- `confidence`: `1 / (1 + cosine distance)`
- `rationale`: or `null` with `reasoning_step_failed`

**Example.** (the reply is real, from a server with no reasoner)

```
> FRACTAL.AGENT.SCHEDULE_WORKLOAD nodes <task>
{"assigned_node":"9","confidence":0.4970546416,"rationale":null,"reasoning_step_failed":"the fractalsqld daemon is not configured"}
```

**Notes.** Category: `@dangerous` (it sends data to a provider). Without a reasoner the computed fields are still returned (see [Partial results](#partial-results-when-the-reasoner-is-not-available)).

### Rebalance sibling: `FRACTAL.AGENT.REBALANCE_SIBLING`
**Problem it solves.** Optimizes a portfolio, then finds the historical allocation the new weights most resemble.

```
FRACTAL.AGENT.REBALANCE_SIBLING alloc_key mu cov cardinality baseline [SEED n] [IDS hash] [CONTEXT json]
```

**Inputs.**

- `alloc_key`: a corpus key of past allocations, one weight vector per row (`n` assets wide)
- `mu, cov, cardinality`: as in allocate
- `baseline`: the baseline weights, `n` float32 values; the change `weights - baseline` is searched
- `SEED`: makes the optimizer deterministic
- `IDS / CONTEXT`: as above

**Returns** a JSON document:

- `sharpe`: the new portfolio's Sharpe ratio
- `weights`: the new weights
- `nearest_distance`: distance to the most similar historical allocation
- `rationale`: or `null` with `reasoning_step_failed`

**Example.** (the reply is real, from a server with no reasoner)

```
> FRACTAL.AGENT.REBALANCE_SIBLING hist <mu> <cov> 3 <base> SEED 7
{"sharpe":1.0567622504,"weights":[0.0,0.3130193854,0.0,0.4044321458,0.0,0.2825484688],"nearest_distance":0.107568292,"rationale":null,"reasoning_step_failed":"the fractalsqld daemon is not configured"}
```

**Notes.** Category: `@dangerous` (it sends data to a provider). Without a reasoner the computed fields are still returned (see [Partial results](#partial-results-when-the-reasoner-is-not-available)).

### Detour classify: `FRACTAL.AGENT.DETOUR_CLASSIFY`
**Problem it solves.** Finds the fleet peer whose deviation a vehicle's change resembles and scores its GPS trace's complexity.

```
FRACTAL.AGENT.DETOUR_CLASSIFY corpus_key baseline current gps_trace_key [BOXCOUNT_DIM d] [IDS hash]
```

**Inputs.**

- `corpus_key`: fleet deviation vectors, one per row
- `baseline, current`: the vehicle's route-embedding vectors (float32)
- `gps_trace_key`: a key of the flattened trace, `d` values per point
- `BOXCOUNT_DIM`: `d`, default 2; the trace needs at least 8 points

**Returns** a JSON document:

- `nearest_fleet_id`: the closest peer
- `trajectory_distance`: its distance to the change
- `trace_complexity`: `FRACTAL.DIM.BOXCOUNT` of the trace (about 1 for a line, higher for a meander)
- `rationale`: or `null` with `reasoning_step_failed`

**Example.** (the reply is real, from a server with no reasoner)

```
> FRACTAL.AGENT.DETOUR_CLASSIFY fleet <base> <cur> trace
{"nearest_fleet_id":6,"trajectory_distance":0.4022266908,"trace_complexity":0.8966312573,"rationale":null,"reasoning_step_failed":"the fractalsqld daemon is not configured"}
```

**Notes.** Category: `@dangerous` (it sends data to a provider). Without a reasoner the computed fields are still returned (see [Partial results](#partial-results-when-the-reasoner-is-not-available)).

### Track anomaly: `FRACTAL.AGENT.TRACK_ANOMALY`
**Problem it solves.** Finds the fleet signature a track's change resembles and the DFA exponent of its heading-change series.

```
FRACTAL.AGENT.TRACK_ANOMALY corpus_key baseline current heading_series_key [IDS hash]
```

**Inputs.**

- `corpus_key`: fleet deviation signatures
- `baseline, current`: the track's state vectors (float32)
- `heading_series_key`: the heading-change series, packed float32

**Returns** a JSON document:

- `nearest_fleet_id`: closest signature
- `trajectory_distance`: its distance
- `dfa_exponent`: `FRACTAL.DIM.DFA` of the heading series, or `-1` if it has fewer than 16 samples
- `rationale`: or `null` with `reasoning_step_failed`

**Example.** (the reply is real, from a server with no reasoner)

```
> FRACTAL.AGENT.TRACK_ANOMALY sig <base> <cur> heading
{"nearest_fleet_id":6,"trajectory_distance":0.4022266908,"dfa_exponent":1.1200691529,"rationale":null,"reasoning_step_failed":"the fractalsqld daemon is not configured"}
```

**Notes.** Category: `@dangerous` (it sends data to a provider). Without a reasoner the computed fields are still returned (see [Partial results](#partial-results-when-the-reasoner-is-not-available)).

### Network coverage alert: `FRACTAL.AGENT.NETWORK_COVERAGE_ALERT`
**Problem it solves.** The spatial morphology of a sensor grid (dimension and lacunarity) plus whether a telemetry series has changed regime.

```
FRACTAL.AGENT.NETWORK_COVERAGE_ALERT point_cloud_key drift_series_key [boxcount_dim [drift_win [drift_threshold [context_json]]]]
```

**Inputs.**

- `point_cloud_key`: the sensors' coordinates, flattened, `boxcount_dim` values per point (default 2, at least ~100 points in practice)
- `drift_series_key`: the telemetry series; needs `drift_win + 16` samples
- `drift_win`: default 48
- `drift_threshold`: default 0.5

**Returns** a JSON document:

- `morph_dimension`: `FRACTAL.MORPH.COMPLEXITY` dimension
- `lacunarity`: its lacunarity (higher = gappier coverage)
- `drift_detected`: `|drift| > drift_threshold`
- `rationale`: or `null` with `reasoning_step_failed`

**Example.** (the reply is real, from a server with no reasoner)

```
> FRACTAL.AGENT.NETWORK_COVERAGE_ALERT grid linkq 2 48
{"morph_dimension":1.6239637567,"lacunarity":1.4459375,"drift_detected":true,"rationale":null,"reasoning_step_failed":"the fractalsqld daemon is not configured"}
```

**Notes.** Category: `@dangerous` (it sends data to a provider). Without a reasoner the computed fields are still returned (see [Partial results](#partial-results-when-the-reasoner-is-not-available)).

### Regime triage: `FRACTAL.AGENT.REGIME_TRIAGE` (general-purpose, no table args)
**Problem it solves.** The DFA exponent and drift of one series, and whether it crossed a threshold.

```
FRACTAL.AGENT.REGIME_TRIAGE series_key [win [drift_threshold [context_json]]]
```

**Inputs.**

- `series_key`: the series
- `win`: default 64, at least 16; the series needs `win + 16` samples
- `drift_threshold`: default 0.5
- `context_json`: optional JSON for the reasoner

**Returns** a JSON document:

- `dfa_exponent`: `FRACTAL.DIM.DFA`
- `drift_detected`: `|drift| > drift_threshold`
- `recent_alpha, baseline_alpha`: from `FRACTAL.DIM.DRIFT`
- `rationale`: or `null` with `reasoning_step_failed`

**Example.** (the reply is real, from a server with no reasoner)

```
> FRACTAL.AGENT.REGIME_TRIAGE px 64
{"dfa_exponent":1.8326651824,"drift_detected":true,"recent_alpha":2.03584856,"baseline_alpha":0.6054153594,"rationale":null,"reasoning_step_failed":"the fractalsqld daemon is not configured"}
```

**Notes.** Category: `@dangerous` (it sends data to a provider). Without a reasoner the computed fields are still returned (see [Partial results](#partial-results-when-the-reasoner-is-not-available)).

### Diverse portfolios: `FRACTAL.AGENT.DIVERSE_PORTFOLIOS` (enterprise-tier)
**Problem it solves.** Several structurally different portfolios from the multimodal optimizer, with one reasoned comparison. It runs in the enterprise library, so a Community install answers "enterprise tier not loaded".

```
FRACTAL.AGENT.DIVERSE_PORTFOLIOS mu cov cardinality [N_RESTARTS n] [OVERLAP x] [QUALITY x] [SEED n] [CONTEXT json] [OBJECTIVE_MODE sharpe|pareto]
```

**Inputs.**

- `mu, cov, cardinality`: as in allocate
- `N_RESTARTS`: independent searches, 1 to 64, default 8
- `OVERLAP`: 0 to 1, default 0.15: how much two portfolios may share and still count as different
- `QUALITY`: above 0 and at most 1, default 0.90: how close to the best Sharpe a candidate must be
- `SEED`: makes the search repeatable
- `CONTEXT`: JSON for the reasoner
- `OBJECTIVE_MODE`: `sharpe` (default) or `pareto` (the non-dominated return-versus-risk front)

**Returns** a JSON document:

- `candidate_id`: the candidate's position, 0 first
- `sharpe`: its Sharpe ratio
- `weights`: the weights (in `pareto` mode an object `{"weights","return","risk"}`)
- `rationale`: one comparison of all the candidates, repeated on every row; if the reasoner is unavailable the reply is instead `{"candidates","objective_mode","n_found","rationale":null,"reasoning_step_failed"}`

**Example.** (the enterprise library is licensed, so the reply is shown as its shape)

```
> FRACTAL.AGENT.DIVERSE_PORTFOLIOS <mu> <cov> 3 N_RESTARTS 6 SEED 7
[{"candidate_id":0,"sharpe":<sharpe>,"weights":[<w0>,...,<w5>],"rationale":"<one comparison of all candidates>"}, ...]
```

**Notes.** Category: `@dangerous` (it sends data to a provider). Without a reasoner the computed fields are still returned (see [Partial results](#partial-results-when-the-reasoner-is-not-available)).

### A note on id resolution

The SQL products resolve a result's id from an `id_col` of the table. On Valkey a result row is an **index into the corpus
key**, and its id comes from a companion hash you name with `IDS`: field `"<row index>"`, value the id. Without `IDS` the id
is the integer index. `CONTENT` works the same way for a row's text, and `FILTER hash value` keeps only the rows whose entry in
`hash` equals `value` (a cohort). Rows missing from a hash are treated as having no id, no content, or (for `FILTER`) as not in
the cohort. A command that uses any of the three accepts at most 200,000 rows, because the hashes are read on the main thread.
A two-predicate cohort is one hash whose value is the combined predicate (for example `2023|battery`).

---

## Building blocks: the primitives agents compose

Every agent above is a short composition of the module's commands. If a recipe is close but not right, build your own from
these (the [composition guide](composition-guide.md) has worked patterns):

| Need | Command |
| --- | --- |
| nearest rows to a vector | `FRACTAL.SEARCH` |
| a diverse set around a vector | `FRACTAL.EXPLORE` |
| a search on "what changed" | `FRACTAL.SEARCH` on `current - baseline` (the agents do this internally) |
| regime change in a series | `FRACTAL.DIM.DFA`, `FRACTAL.DIM.DRIFT`, `FRACTAL.SERIES.CHANGEPOINT` |
| spatial structure | `FRACTAL.DIM.BOXCOUNT`, `FRACTAL.MORPH.COMPLEXITY` |
| periodicity | `FRACTAL.SERIES.PERIODOGRAM`, `FRACTAL.SERIES.SPECTROGRAM` |
| a state log's identity and cycles | `FRACTAL.SERIES.FINGERPRINT`, `FRACTAL.CYCLE.DETECT` |
| allocation under a constraint | `FRACTAL.OPTIMIZE.PORTFOLIO` |
| reasoning and embeddings | `FRACTAL.REASON`, `FRACTAL.EMBED` (through `fractalsqld`) |
| an audit trail | `FRACTAL.AUDIT.LOG`, `FRACTAL.LEDGER.*` (enterprise) |

The five building-block agents are commands in their own right:

### Plan explore: `FRACTAL.AGENT.PLAN_EXPLORE`
**What it does.** Embeds a seed state description and Scout-searches a strategy corpus for a spread of distinct branches.

```
FRACTAL.AGENT.PLAN_EXPLORE strategy_key state_text dim max_branches
```

**Inputs.**

- `strategy_key`: a corpus key of strategy embeddings
- `state_text`: the seed state, 1 to 65536 bytes of text, embedded through `fractalsqld`
- `dim`: the corpus dimension, 1 to 4096; a key has no schema, so the width is given, and the call fails if the embedding's width differs
- `max_branches`: 1 to 100000

**Returns:**

- `branch_id`: the row index
- `plan_trajectory`: the row's vector
- `confidence_score`: `1 - cosine distance`

**Example.** `FRACTAL.AGENT.PLAN_EXPLORE strategies "tighten risk limits" 8 5`

Needs an embedding provider behind `fractalsqld`.

**Notes.** Category: `@dangerous` (it sends data to a provider). A key has no schema, so the corpus width is given as `dim`.

### Trajectory predict: `FRACTAL.AGENT.TRAJECTORY_PREDICT`
**What it does.** Projects where a state sequence is heading: the known state nearest to (latest row - baseline row).

```
FRACTAL.AGENT.TRAJECTORY_PREDICT corpus_key dim baseline_row
```

**Inputs.**

- `corpus_key`: a key of `dim`-wide state rows in time order; the last row is the current state
- `dim`: the row width
- `baseline_row`: the index of the baseline row

**Returns:**

- `predicted_state_vector`: the nearest known state
- `projected_drift_delta`: its cosine distance to the change
- `risk_threshold_exceeded`: `projected_drift_delta > 0.5`

**Example.**

```
> FRACTAL.AGENT.TRAJECTORY_PREDICT states 4 2
{"predicted_state_vector":[0.7649580240249634,0.6923948526382446,0.010567640885710716,0.17800451815128326],"projected_drift_delta":0.01633990161925969,"risk_threshold_exceeded":false}
```

**Notes.** Category: `@read`. A key has no schema, so the corpus width is given as `dim`.

### Detect loop: `FRACTAL.AGENT.DETECT_LOOP`
**What it does.** Watches an agent's state log for a loop: a SimHash fingerprint per state fed to Brent's cycle detector, plus the DFA exponent of the state norms.

```
FRACTAL.AGENT.DETECT_LOOP agent_id state_log_key dim [N_BITS n] [SEED x] [HAMMING_THRESHOLD n]
```

**Inputs.**

- `agent_id`: a label echoed back (1 to 256 bytes)
- `state_log_key`: the log, `dim` floats per state, at least 2 states
- `dim`: the state width
- `N_BITS`: fingerprint bits, 1 to 4096, default 64
- `SEED`: fingerprint seed, default 42
- `HAMMING_THRESHOLD`: 0 to `N_BITS`, default 0: how many bits two fingerprints may differ and still count as the same state

**Returns:**

- `agent_id`: echoed
- `dfa_exponent`: of the state norms (0 if the series cannot be scored)
- `is_loop_detected`: `true` if the exponent exceeds 0.9 **or** a fingerprint cycle closes

**Example.**

```
> FRACTAL.AGENT.DETECT_LOOP agent-7 log 3
{"agent_id":"agent-7","dfa_exponent":0.7351700532696842,"is_loop_detected":false}
```

**Notes.** Category: `@read`. A key has no schema, so the corpus width is given as `dim`.

### Search agent: `FRACTAL.SEARCH.AGENT`
**What it does.** Retrieve-then-reason: embeds a question, Scout-searches a corpus, hands the hit rows to the reasoner and returns the answer with its sources.

```
FRACTAL.SEARCH.AGENT corpus_key query_text dim [POPULATION n] [ITERATIONS n] [CONTENT hash] [IDS hash]
```

**Inputs.**

- `corpus_key, dim`: as in plan_explore
- `query_text`: the question
- `POPULATION / ITERATIONS`: default 50 and 15
- `CONTENT / IDS`: companion hashes; the reasoner is given up to 64 distinct hit rows as `[{"id","content"}]`

**Returns:**

- `answer`: the reasoner's answer
- `source_doc_ids`: the hit row indexes
- `execution_time_ms`: the elapsed time of the call, in milliseconds

**Example.** `FRACTAL.SEARCH.AGENT kb "why did latency rise?" 768 POPULATION 50 CONTENT kb:text IDS kb:id`

Needs an embedding provider behind `fractalsqld` and a reasoner.

**Notes.** Category: `@dangerous` (it sends data to a provider). A key has no schema, so the corpus width is given as `dim`.

### RAG agent: `FRACTAL.RAG.AGENT`
**What it does.** `SEARCH_AGENT` with a fixed search (population 50, 15 iterations).

```
FRACTAL.RAG.AGENT corpus_key query_text dim [CONTENT hash] [IDS hash]
```

**Inputs.**

- `corpus_key, query_text, dim, CONTENT, IDS`: as in search_agent

**Returns:**

- `answer / source_doc_ids / execution_time_ms`: as in search_agent

**Example.** `FRACTAL.RAG.AGENT kb "why did latency rise?" 768 CONTENT kb:text`

Needs an embedding provider behind `fractalsqld` and a reasoner.

**Notes.** Category: `@dangerous` (it sends data to a provider). A key has no schema, so the corpus width is given as `dim`.

---

## Reference blueprints: Domain Agents

Larger **Domain Agents** for named industries are built by composing these agents. The fifteen **agentic kits** in `demo/` (`demo-vertical-agentic-*.py`)
are working examples, each a short script over these commands that checks its own headline result; see
[starter-kits.md](starter-kits.md#agentic-kits-composed-agents) for the list (DevOps / SRE, FinTech, Customer Support, EV battery,
ADAS, V2X warranty, critical inventory, heavy-industry maintenance, grid balancing, budget reallocation, sensor-telemetry allocation,
replenishment, next-action ranking, recall and recommendation, simulated economy).

### Getting started with the blueprints

```bash
python3 demo/demo-vertical-agentic-ops-devops.py --port 6379
```

---

## Composing your own

An agent is the same four steps every time, and you can write one in a few lines in your own application:

1. **Fetch** the inputs from keys.
2. **Compute** with one or more primitives.
3. **Reason** (optional): one `FRACTAL.REASON` call with the computed result as context.
4. **Emit** a JSON decision, and if you have the enterprise tier, an `FRACTAL.AUDIT.LOG` entry.

```python
drift = json.loads(r.call("FRACTAL.DIM.DRIFT", series_blob, 32))           # 2. compute
note = None
try:
    note = r.call("FRACTAL.REASON", "Triage this drift: %.3f" % drift["drift"], json.dumps({"entity": key}))  # 3. reason
except RespError as e:                                                      # keep the computed half if it fails
    err = str(e)
decision = {"threat_score": drift["drift"], "triage_summary": note, "reasoning_step_failed": None if note else err}
```

Keep the partial-result habit: a decision that loses its numbers because the narrative failed is worse than one that says why the
narrative is missing.

## Try it

Run any agentic kit to see agents work end to end on a seeded dataset; each prints its report and exits non-zero unless its headline
result holds:

```bash
python3 demo/demo-vertical-agentic-ops-devops.py --port 6379
python3 demo/demo-vertical-agentic-budget-reallocation.py --port 6379
```

