<p align="center">
  <img src="../FractalSQLforValkey.jpg" alt="FractalSQL for Valkey" width="720">
</p>

# Composition Guide: Build Your Own Agent

The twenty-one [agents](api-agency.md) cover the common recipes. When one is close but not right, you build your own from the same pieces. The SQL editions
compose primitives with stored procedures; Valkey has no stored procedures, so composition happens in your application, and the module's job is to make each
step a single cheap command next to the data. These are the patterns the [starter kits](starter-kits.md) use.

## The building blocks

| You need to... | Use |
| --- | --- |
| find the nearest rows to a vector | `FRACTAL.SEARCH` |
| cover distinct regions instead of near-copies | `FRACTAL.EXPLORE` |
| decide whether a series changed regime | `FRACTAL.DIM.DRIFT`, `FRACTAL.DIM.DFA` |
| localise a change, find a cadence | `FRACTAL.SERIES.CHANGEPOINT`, `PERIODOGRAM`, `SPECTROGRAM` |
| notice a state repeating | `FRACTAL.SERIES.FINGERPRINT`, `FRACTAL.CYCLE.DETECT` |
| choose at most k of n | `FRACTAL.OPTIMIZE.PORTFOLIO` |
| measure a shape | `FRACTAL.DIM.BOXCOUNT`, `FRACTAL.MORPH.*`, `FRACTAL.TDA.PERSISTENCE` |
| embed text, or write a narrative | `FRACTAL.EMBED`, `FRACTAL.REASON` |
| prove what was decided | `FRACTAL.AUDIT.LOG`, `FRACTAL.LEDGER.*` (enterprise) |

## The composition principle

Every agent is the same four steps, and so should yours be:

1. **Fetch** the inputs from keys (a series, a corpus, a cohort).
2. **Compute** with one or more primitives.
3. **Reason** (optional): one `FRACTAL.REASON` call with the computed result as context.
4. **Emit** a decision document, and if you have the enterprise tier an `FRACTAL.AUDIT.LOG` entry.

Compute first and reason last, so a model outage costs you the narrative and never the numbers.

## Worked patterns

### Pattern 1: Retrieve, then decide: cache in front of a model

```
embedding = embed(prompt)                       # FRACTAL.EMBED, or your own model
hit = FRACTAL.SEARCH cache:vecs <embedding> 1
if hit.top_k[0].dist < 0.45:  return LINDEX cache:answers hit.top_k[0].idx
answer = call_model(prompt)
SET cache:vecs <old rows + embedding> ; RPUSH cache:answers answer
```

See `semantic-cache`. The threshold is a cosine distance you choose from your own data.

### Pattern 2: Detect, localise, explain

A monitoring loop is three commands in order: **detect** a regime change
(`DIM.DRIFT`), **localise** it (`SERIES.CHANGEPOINT`), **characterise** it
(`SERIES.PERIODOGRAM` for a cadence, `SERIES.SPECTROGRAM` for order). See
`event-stream-anomaly-triage` and `rf-spectrum-sensing`.

### Pattern 3: Find the signature, then confirm it

Use `SEARCH` over current-state vectors with a known-bad signature as the query, then
confirm with an independent signal (`DIM.DRIFT` on that host's series). Two unrelated
signals agreeing is much stronger than either alone. See `cybersecurity-threat-detection`.

### Pattern 4: Diverse context for a model

`EXPLORE` returns regions, not near-duplicates. Feed a model the best point of each
region instead of the ten nearest rows, and a RAG prompt stops being ten copies of the
same paragraph. See `recommendation-search`.

### Pattern 5: Watch an agent for loops

Fingerprint each state (`SERIES.FINGERPRINT`), keep the byte stream, and ask
`CYCLE.DETECT`. It tolerates a few differing bits, so floating-point noise does not hide a
loop. See `agent-loop-detection`.

### Pattern 6: Cohorts are just keys

A SQL `WHERE` that restricts a search becomes a key per cohort (`fleet:route3`).
Keep the cohort key up to date when membership changes; `SEARCH` then needs no filter.

### Pattern 7: Allocate under a cardinality constraint

`OPTIMIZE.PORTFOLIO` is a general "choose at most k of n, weights summing to 1"
optimiser, not only for finance: resource placement, ensemble selection, budget
allocation. Same seed, same answer, so a decision can be reproduced.

### Pattern 8: "Answer my corpus" (single-turn RAG)

When the question is text, `FRACTAL.RAG.AGENT` does the whole turn: embed the question, Scout-search the corpus key, hand the hit rows to the
reasoner, return the answer with its sources. To own the steps instead (a different retrieval, a different prompt), compose them:

```
q = FRACTAL.EMBED "why did latency rise?"
hits = FRACTAL.EXPLORE docs:vecs <q> 8            # regions, not near-duplicates
context = [HGET docs:text <row index of each region's nearest row>, ...]
answer = FRACTAL.REASON "why did latency rise?" '{"docs": [...]}'
```

### Pattern 9: A multi-step agent with a safety barrier

Put an interception step in front of an action and a decision step behind it. `OUTLIER_INTERCEPT` holds a proposed state that sits too close to a known-bad one;
only a state that passes goes on to `ALLOCATE` or `ROUTE_TASK`:

```
verdict = FRACTAL.AGENT.OUTLIER_INTERCEPT bad:states <proposed> 0.1
if verdict.intercepted:  return hold(verdict)
plan = FRACTAL.AGENT.ROUTE_TASK capabilities <task> 1000
```

See `agentic-ops-devops` and `adas-safety-validation` for complete versions with their checks.

### Pattern 10: Keep the computed half

Every agent returns what it computed even if the reasoner fails. Do the same in yours:

```python
drift = json.loads(r.call("FRACTAL.DIM.DRIFT", series_blob, 32))
try:
    note = r.call("FRACTAL.REASON", "Triage this drift: %.3f" % drift["drift"])
except RespError as e:
    note, err = None, str(e)
decision = {"threat_score": drift["drift"], "triage_summary": note, "reasoning_step_failed": None if note else err}
```

## Safe Agency & Guardrails

- **Treat model output as untrusted.** Validate anything a model writes before acting on it, and never run it as a command without an allow-list
  ([text-to-sql-setup.md](text-to-sql-setup.md)).
- **Use ACLs.** Anything that sends data to a model is `@dangerous`; give an application account only the `+command` grants it needs, on only the key patterns it needs
  ([reasoning-setup.md](reasoning-setup.md#-security--governance)).
- **Bound the work.** Cap sizes and iteration counts you accept from callers, as the agents do.
- **Leave a trail.** On the enterprise tier, write each decision to the ledger so you can prove later what was decided and in what order
  ([enterprise.md](enterprise.md)).

## Practical rules


- Work in **cosine** terms. If magnitude matters, put it in another dimension or compare it
  separately.
- Keep a corpus key to a **single dimension**. A different dimension is a different key.
- Treat `FRACTAL.REASON` and `FRACTAL.EMBED` as the only commands that cost money or can
  be slow, and fall back gracefully when they return an error.
- Run long analytics from a connection of their own; they no longer block other clients,
  but they do occupy a worker, so size `threads` for the concurrency you need.

## Notes

- A corpus key is one dimension. A different embedding model is a different key.
- Agents are commands: you can call them from Lua, `MULTI` or a script. Inside those, which may not block, they run inline on the main thread, so keep heavy ones
  out of scripts.

## Where next

- **The agents themselves** → [api-agency.md](api-agency.md).
- **Complete worked examples** → [starter-kits.md](starter-kits.md) (forty-four scripts that check their own results).
- **Every primitive's arguments** → [Discovery](api-discovery.md), [Analytics](api-analytics.md), [Cognition](api-cognition.md).

