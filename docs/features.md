<p align="center">
  <img src="../FractalSQLforValkey.jpg" alt="FractalSQL for Valkey" width="720">
</p>

# FractalSQL Feature Specification

FractalSQL for Valkey is a module: commands you call from any Valkey client, running in the server
process with the FractalSQL core statically linked. Reasoning and embedding go through the
separate `fractalsqld` daemon. Vectors live in ordinary string keys as packed
little-endian float32, and what you would keep beside a vector (an id, some content, a filter attribute) lives in
companion hashes whose field is the row index. Search, reasoning and the agents are the building blocks for agentic
workflows that run beside your data.

## 🏗️ Capability Tiering Model

FractalSQL ships in two editions, each unlocking more of the four capability tiers described below
(Discovery → Cognition → Agency → Analytics). The editions are a *build/licensing* axis; the capability tiers are a
*functional* axis. A single install belongs to one edition and exposes whichever capability tiers that edition
includes.

| Tier | Focus | Key Capabilities | Build / Requirement |
| --- | --- | --- | --- |
| **Community** | Discovery, Cognition, Agency, Analytics | SFS Core, Sniper Search, Scout Discovery, Reasoning and Embeddings (through the daemon), 16 ready-made agents and 5 building blocks, the full Analytics set | `fractalsql.so`, everything most installs need; the reasoning and embedding commands use the optional `fractalsqld` daemon |
| **Enterprise** | Governance | Activation gating plus a real, tamper-evident ledger storage layer. See [Enterprise Tier](enterprise.md) for the full scope | the licensed core, held by the daemon and never loaded into the Valkey process |

The split keeps anything that can block on the network, load third-party code, or hold licensed code out of your
datastore's process. See the [FractalSQL Daemon](fractalsql-daemon.md).

---

## 🔍 Tier 1: Discovery

The foundation of FractalSQL is the **Stochastic Fractal Search (SFS)** engine, which treats vector search as a
continuous optimization problem rather than an index lookup like standard HNSW or IVFFlat.

### Sniper Search (`FRACTAL.SEARCH`)
Pure SFS convergence to the minimum cosine distance to a query, returning the top-k nearest rows of a corpus key
(`dim x N` packed float32; `dim` is the query's length). It is a "precision" tool for finding the nearest rows. It
ranks by **cosine distance**, so a brute-force check must too.

### Scout Discovery (`FRACTAL.EXPLORE`)
An SFS population search that disperses its particles across the data's distinct basins of attraction, instead of
the "mode collapse" common in top-K search, and returns the population. Its reach grows with the population (in the
recommendation kit, a population of a few dozen stayed in the query's own cluster and a few hundred reached all six). The corpus is a key, scanned on every call;
see [Benchmarks & Scaling](#-benchmarks--scaling) for what that costs.

### Diversify / Repulsion Sessions
A named session remembers what a caller rejected and pushes later searches away from it (`FRACTAL.DIVERSIFY.ENABLE`, `FRACTAL.FEEDBACK`, and
`SESSION <name>` on a search). It is named by you, so it survives reconnects and
proxies, bounded in number and idle lifetime, and never touching a key. See [Discovery](api-discovery.md#diversify--repulsion-sessions).

### Ground-Truth Retrieval
`FRACTAL.SEARCH` over a corpus key returns the *K* nearest real rows, and the agents add `IDS`, `CONTENT` and `FILTER`
companion hashes to resolve ids, content and a cohort. This is the ground-truth layer used by every retrieval-backed agent.

---

## 🧠 Tier 2: Cognition

The Cognition tier adds a reasoning bridge to the SFS core, allowing it to call LLMs and embedding models through a
pluggable provider. On Valkey the provider lives in the **`fractalsqld` daemon**, so a slow or crashing model can
never stall or take down the datastore; without a daemon every other command works and these answer with a clear
"not configured" error. See [reasoning-setup.md](reasoning-setup.md).

### Reasoning (`FRACTAL.REASON`)
Sends a query (and optional JSON context) to the configured reasoning provider and returns its text. Never retried
automatically, and the provider settings are operator-only (in `fractalsqld.conf`), never settable through a command.
See [api-cognition.md](api-cognition.md).

### Semantic Embeddings (`FRACTAL.EMBED`)
Returns an embedding as a packed float32 vector, ready to `SET` as a corpus row or search with. See
[api-cognition.md](api-cognition.md).

---

## 🤖 Tier 3: Agency

The Agency tier composes the Discovery and Cognition primitives into autonomous routines: sixteen ready-made
recipes plus five reusable building blocks, as commands. See [`docs/api-agency.md`](api-agency.md) for the full
argument reference, real example output, and notes.

### Agents

| Agent Command | Recipe | Reasoning |
| --- | --- | --- |
| `FRACTAL.AGENT.ANOMALY_TRIAGE` | drift on one entity's series → reasoning triage | ✓ |
| `FRACTAL.AGENT.REGIME_TRIAGE` | DFA + drift over one series → reasoning triage | ✓ |
| `FRACTAL.AGENT.TRACK_ANOMALY` | search on the change + heading DFA → reasoning triage | ✓ |
| `FRACTAL.AGENT.DETOUR_CLASSIFY` | search on the change + box-counting → reasoning classify | ✓ |
| `FRACTAL.AGENT.NETWORK_COVERAGE_ALERT` | spatial morphology + telemetry drift → reasoning alert | ✓ |
| `FRACTAL.AGENT.ALLOCATE` | SFS Sharpe optimizer → reasoning rationale | ✓ |
| `FRACTAL.AGENT.REBALANCE_SIBLING` | optimizer + search on the change → reasoning rationale | ✓ |
| `FRACTAL.AGENT.DIVERSE_PORTFOLIOS` (enterprise) | multi-restart SFS + diverse-select → reasoning comparison | ✓ |
| `FRACTAL.AGENT.ROUTE_TASK` | nearest-capability search + budget accounting → reasoning rationale | ✓ |
| `FRACTAL.AGENT.SCHEDULE_WORKLOAD` | SFS refine + nearest node → reasoning rationale | ✓ |
| `FRACTAL.AGENT.OUTLIER_INTERCEPT` | distance-to-bad-state safety barrier → reasoning justification | ✓ |
| `FRACTAL.AGENT.PATIENT_DETERIORATION_TRIAGE` | cohort search + drift on the change → reasoning triage | ✓ |
| `FRACTAL.AGENT.DATA_ANALYST` | question → valkey-search query → result → analysis | ✓ (needs valkey-search) |
| `FRACTAL.AGENT.RECALL_HYBRID` | cohort-restricted vector recall | — |
| `FRACTAL.AGENT.RECOMMEND_DIVERSE` | repulsion-diverse top-k | — |
| `FRACTAL.AGENT.FEEDBACK_AUDIT` | diversify loop + collapse diagnostics | — |

### Universal Agents

The five building blocks the agents above compose. Call them directly to build your own recipe.

| Command | What it does |
| --- | --- |
| `FRACTAL.SEARCH.AGENT` | Embed a query, Scout-search a corpus key, and reason over the matched rows. |
| `FRACTAL.RAG.AGENT` | Focused single-turn RAG: the same, with the fixed 50 x 15 search. |
| `FRACTAL.AGENT.PLAN_EXPLORE` | Scout-search-driven exploration of multiple non-overlapping strategy branches. |
| `FRACTAL.AGENT.TRAJECTORY_PREDICT` | Forecasts where a state sequence is heading from a delta against a key's historical rows. |
| `FRACTAL.AGENT.DETECT_LOOP` | Flags repetitive agent loops via SimHash state fingerprints and Brent's cycle detector, with a DFA check. |

### Safe Agency & Guardrails

Valkey's guardrails are the server's own, not a parser's:

1. **ACL categories.** An agent that sends data to a provider is `@dangerous`, so granting `+@read` never grants a call
   out to a model. Every key an agent reads, not just the declared first one, is checked against the caller's key
   patterns.
2. **Computation first, reasoning last.** A reasoning agent returns what it computed even when the model is down
   (`reasoning_step_failed`), so an outage degrades the narrative and never the decision's numbers.
3. **Bounded work.** Every size is capped before it is used, every call counts against an in-flight memory cap, and the
   heaviest agent parameters carry a work cap.

---

## 📐 Tier 4: Analytics

The final tier provides mathematical primitives for analyzing the "shape" of data and state, turning raw vectors into
actionable structural insights.

### Fractal Dimension Analysis
- **DFA (`FRACTAL.DIM.DFA`)**: Analyzes the scaling exponent of a time series to distinguish between noise, random
  walks, and structured signals.
- **Box-Counting (`FRACTAL.DIM.BOXCOUNT`)**: Measures the Minkowski-Bouligand dimension of a path-like point trace to
  evaluate spatial complexity.
- **Drift (`FRACTAL.DIM.DRIFT`)**: Detects regime changes by comparing the DFA exponent of a recent window against a
  baseline.

### Time-Series and Topology
- **Change-Point Localization (`FRACTAL.SERIES.CHANGEPOINT`)**: Localizes where a series' mean or variance shifted.
- **Periodogram (`FRACTAL.SERIES.PERIODOGRAM`)**: Exact power spectrum (direct DFT) for cadence detection such as
  network beaconing or retry loops.
- **Spectrogram (`FRACTAL.SERIES.SPECTROGRAM`)**: Per-window spectral entropy; pair with change-point localization to
  find when order is lost.
- **Persistence (`FRACTAL.TDA.PERSISTENCE`)**: Exact 0-dimensional persistence over a point cloud's Vietoris-Rips
  filtration, plus the graph cycle rank as an informational Betti-number estimate.
- **SimHash (`FRACTAL.SERIES.FINGERPRINT`)**: Folds a state vector into a Hamming-comparable fingerprint where
  nearly-identical states collide.
- **Cycle Detection (`FRACTAL.CYCLE.DETECT`)**: Brent's algorithm over a fingerprint stream.

### Domain-Specific Geometry
Optimized routines for pre-extracted biological and technical geometry: vascular networks (tortuosity, branch
density), cortical folding (Gyrification Index), nerve plexus (density, dimension), and morphological complexity
(box-counting + lacunarity for a point cloud; give it on the order of a hundred points or more).

### Portfolio Optimization
`FRACTAL.OPTIMIZE.PORTFOLIO` uses the SFS engine to solve cardinality-constrained Sharpe-ratio maximization: the best
*K* assets in a large universe without the exponential cost of a brute-force search.
On the enterprise tier, `FRACTAL.OPTIMIZE.MULTIMODAL` and `FRACTAL.OPTIMIZE.MULTIMODAL.PARETO` return several structurally different
portfolios, or the non-dominated return-versus-risk front, instead of one.

### Vector Math
- **L_p Distance (`FRACTAL.VECTOR.LP`)**: Generalized distance for any p > 0 (for 0 < p < 1 this is not a proper metric;
  use it explicitly, never as a silent default).
- **Quantization (`FRACTAL.VECTOR.QUANTIZE.INT8` / `.BINARY`)**: 4x/32x per-vector compression with Hamming pre-filtering
  (`FRACTAL.VECTOR.HAMMING`) ahead of a full-precision re-rank.
- **Canonical form**: a vector is a packed little-endian float32 blob, byte-stable across architectures and binary-safe in
  any Valkey string; `FRACTAL.VECTOR.FROMTEXT` / `TOTEXT` convert to and from `[1,2,3]` text.

---

## Safe by construction

- **Every size is bounded before it is used**: 4 MiB query, 256 MiB corpus, 16 MiB per series or matrix argument, 64 KiB of
  text or JSON context, and an in-flight cap on decoded memory (`max-inflight-mb`).
- **NaN and Inf are refused** everywhere a number comes from a key or an argument.
- **Out-of-range numbers are refused, not adjusted**: `K` outside 1 to 10000 or a population outside 2 to 10000 is an error that names the value, the
  same ranges the core enforces.
- **The parsers are fuzzed**: text vectors, the session parameters JSON, the daemon's reply frames (valid and corrupt) and the JSON writer run
  under libFuzzer with ASan and UBSan (`./build_test.sh --fuzz`).
- **Allocation failure is survivable**: the module was tested by failing every allocation point of every command, agents
  included, in turn; the server survived each time.
- **Error text is ASCII-only** and never echoes raw caller bytes.
- **The static core is not exported** into the server's symbol table.
- **ACL categories**: the commands belong to `@read` / `@slow` / `@fast`, and the external and administrative ones (and every
  agent that calls a model) to `@dangerous`, so you can grant `+@read` without granting a call out to a model provider. Keys an
  agent reads beyond its first are checked against the caller's key patterns too.
- **No FRACTAL command is propagated to replicas**, and the read-only ones run on replicas.
- **Long work never blocks other clients**: searches, analytics and agents run on worker threads, and the server keeps
  answering `PING` while they do.

---

## 📈 Benchmarks & Scaling

### valkey-search (HNSW) vs. Scout Discovery

FractalSQL is not an index. `FRACTAL.SEARCH` and `FRACTAL.EXPLORE` evaluate their fitness against the whole corpus on
every call, which is O(N x d) by design, and it is the only way here to guarantee the model receives a genuinely diverse set
of perspectives rather than a single collapsed cluster. A vector index answers a nearest-neighbour query far faster and
tends to return neighbours from the query's own cluster. They solve different problems; FractalSQL is **not** a drop-in
replacement for an index. Use it where diversity, analytics or an agent matters more than latency (curated sub-corpora,
not full-corpus top-k at scale), and use valkey-search for retrieval at scale.

`bench/head_to_head.py` runs the same clustered dataset through valkey-search's HNSW and `FRACTAL.EXPLORE`, and reports latency and
how many distinct clusters each answer covers ([bench/README.md](../bench/README.md)). Expect this pattern: HNSW
returns about one cluster in single-digit milliseconds, and Scout returns several clusters in more time. Run it on your own hardware and
corpus before relying on a number.

### Storage: float32 blob vs. text

A corpus is `dim x N` float32 values in one string key, 4 bytes per value and no parsing: the module decodes it with a
bounded copy. The text form (`FRACTAL.VECTOR.FROMTEXT`) is roughly twelve bytes per value (about three times the blob; `bench/vector_type_head_to_head.py` measures it on your data) and has to be parsed, so it is for
interchange, not storage. A corpus key is capped at 256 MiB, and a call that uses `IDS`, `CONTENT` or `FILTER` accepts at
most 200,000 rows.

---

## 📚 API Reference

(Detailed argument tables, defaults, and ranges are in the per-tier references.)

**Discovery**
- `FRACTAL.SEARCH corpus_key query [K]`: Sniper Mode, top-k nearest rows. → **[api-discovery.md](api-discovery.md)**
- `FRACTAL.EXPLORE corpus_key query [population]`: Scout Mode diverse exploration. → **[api-discovery.md](api-discovery.md)**

**Cognition**
- `FRACTAL.REASON query [context_json]`: LLM dispatch. → **[api-cognition.md](api-cognition.md)**
- `FRACTAL.EMBED text`: Semantic vector generation. → **[api-cognition.md](api-cognition.md)**

**Agency**
- `FRACTAL.AGENT.ROUTE_TASK ...`: Sub-agent dispatch. → **[api-agency.md](api-agency.md)**
- `FRACTAL.AGENT.REGIME_TRIAGE ...` / `ANOMALY_TRIAGE ...`: Drift/regime detection. → **[api-agency.md](api-agency.md)**
- `FRACTAL.AGENT.RECOMMEND_DIVERSE ...` / `RECALL_HYBRID ...`: Diverse/cohort-restricted retrieval. → **[api-agency.md](api-agency.md)**
- 15 more, full list → **[api-agency.md](api-agency.md#which-agent-should-i-use)**

**Analytics**
- `FRACTAL.DIM.DFA series`: DFA scaling exponent. → **[api-analytics.md](api-analytics.md)**
- `FRACTAL.DIM.BOXCOUNT points dim`: Box-counting dimension. → **[api-analytics.md](api-analytics.md)**
- `FRACTAL.DIM.DRIFT series window`: Regime change detection. → **[api-analytics.md](api-analytics.md)**
- `FRACTAL.OPTIMIZE.PORTFOLIO ...`: Cardinality-constrained optimization. → **[api-analytics.md](api-analytics.md)**
- `FRACTAL.MORPH.VASCULAR` / `CORTICAL` / `NERVE` / `COMPLEXITY`: Geometry. → **[api-analytics.md](api-analytics.md)**

**Enterprise**
- `FRACTAL.LEDGER.*`, `FRACTAL.AUDIT.*`: the tamper-evident ledger. → **[enterprise.md](enterprise.md)**
