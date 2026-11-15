<p align="center">
  <img src="../FractalSQLforValkey.jpg" alt="FractalSQL for Valkey" width="720">
</p>

# Docker Demo: The Learning Path

A one-command, end-to-end demo: two containers sharing a socket. `fractalsqld` holds the core and talks to a model; `valkey` is the official
Valkey image with the FractalSQL module installed, plus `/demo` and `/bench` and the `python3`
they need (standard library only; `numpy` for one benchmark). No compiler on your machine.

## 1. Start the stack

```bash
export FSQL_HMAC_KEY=$(openssl rand -hex 32)      # shared secret between the two containers
docker compose up -d
docker compose exec valkey valkey-cli FRACTAL.VERSION
```

`docker compose up` builds both images from this repository (`docker/Dockerfile`, targets
`fractalsqld` and `demo`), starts the daemon, waits for its socket, then starts Valkey with the module
loaded and pointed at that socket. The server listens on `127.0.0.1:16379`. The demo server has no password and the model hop is plain HTTP, so it publishes its ports on the loopback address only: it is a local demo and must not be exposed to an untrusted network. For anything else, set `requirepass` or an ACL on the server and point `FRACTALSQL_HTTP_URL` at an `https://` endpoint. Choose the Valkey release with
`VALKEY_VERSION` (default 8.1; 7.2 and 8.0 also work):

```bash
VALKEY_VERSION=7.2 docker compose up -d --build
```

The daemon container is read-only, drops every capability and runs as the same unprivileged user as
Valkey; the socket and key live in the `fsqld_run` volume and nowhere else.

## 2. Run the demos

Everything in `demo/` runs inside the container, in one pass:

```bash
docker compose exec valkey /demo/run_demo.sh                 # every demo, one log per file
docker compose exec valkey /demo/run_demo.sh --quiet
docker compose exec valkey /demo/run_demo.sh --only demo-vertical-semantic-cache.py
docker compose exec valkey /demo/run_demo.sh --skip demo-agents.py,enterprise-qtl-audit.py
docker compose exec valkey /demo/run_demo.sh --include-response-modes
```

`./demo/run_demo.sh` from the repository root does the same by exec'ing into the running `valkey`
service. A single demo is `docker compose exec valkey python3 /demo/demo.py`, or from the host,
`python3 demo/demo.py --port 16379`. Each demo is self-checking: it prints its report and exits non-zero
unless its headline result holds. See [Starter Kits](starter-kits.md). A reasoning-dependent step that
has no model behind it says `skipped: needs a provider` and the file still exits 0.

`demo/demo-workload.sh` is a separate concurrent load tool, not a content demo: it sustains a mix of
searches, analytics and agents from several clients and prints p50/p95/p99 latency per operation.

## 3. Optional: turn on reasoning

The daemon is already running; it just needs a model. Point it at any OpenAI-compatible server (a local
Ollama or vLLM, or a model on your own network) with `OLLAMA_HOST`, expecting `gpt-oss:20b` and
`nomic-embed-text`:

```bash
OLLAMA_HOST=my-ollama-box docker compose up -d                  # an Ollama you already run
docker compose --profile ollama up -d                           # or the bundled one...
docker compose --profile pull-model run --rm pull-model         # ...and a one-time model download
```

Without a model, `FRACTAL.REASON` and `FRACTAL.EMBED` answer with the daemon's provider error and the rest
of the demo is unaffected. The model settings are environment variables on the `fractalsqld` service in
`docker-compose.yml`; see [Reasoning Setup](reasoning-setup.md) and the
[FractalSQL Daemon](fractalsql-daemon.md). The reply shape (`text`, `code` or `json`) is the daemon's
`FSQL_REASONING_HTTP_RESPONSE_MODE`; set it on that service and recreate it.

## 4. Start with the short demos

Before the learning path, six short demos show one idea each: `demo.py` (the 60-second tour), `demo-agents.py`, `demo-fractal-vector.py`, `demo-diversify.py`, `response-modes.py` and `enterprise-qtl-audit.py`.

```bash
python3 demo/demo.py --port 16379
python3 demo/demo-agents.py --port 16379
python3 demo/demo-diversify.py --port 16379
```

The two that need a model (`response-modes.py`) or the enterprise library (`enterprise-qtl-audit.py`) say so and skip themselves when they are not there.

## 5. Stop

```bash
docker compose down
```

## 🎓 The Learning Path

Run the kits in this order to see the progression from a vector-search module to an agentic one. The Discovery and Analytics kits need no model; where a step
uses an optional provider, the kit says so and skips it cleanly.

### Level 1: Geometric Discovery
*Focus: using the fractal core and the geometry primitives to find structure in noise.*
- **Goal**: learn Sniper and Scout search, and the dimension, spectrum and shape commands.
- **Kits**:
  ```bash
  python3 demo/demo-vertical-recommendation-search.py --port 16379      # nearest matches vs a diverse set
  python3 demo/demo-vertical-medtech-clinical.py --port 16379           # clinical telemetry, vessel / mesh / nerve geometry
  python3 demo/demo-vertical-maritime-defense.py --port 16379           # AIS-style tracks and a deviating vessel
  python3 demo/demo-vertical-fleet-logistics.py --port 16379            # a detour in a GPS trace
  python3 demo/demo-vertical-smart-cities-iot.py --port 16379           # sensor-grid coverage and a regime change
  ```

### Level 2: Cognitive Synthesis
*Focus: composing search with model reasoning to produce readable insight.*
- **Goal**: feed discovery results into `FRACTAL.REASON`, and embed text with `FRACTAL.EMBED` (these need the daemon from step 3 above).
- **Kits**:
  ```bash
  python3 demo/demo-vertical-semantic-cache.py --port 16379             # answer repeats from Valkey instead of calling the model
  python3 demo/demo-vertical-sovereign-edge-ai.py --port 16379          # on-device search and allocation, an optional rationale
  python3 demo/demo-vertical-scenario-planning-simulation.py --port 16379   # a spread of scenarios; planning if an embedding provider is configured
  ```

### Level 3: Autonomous Agency
*Focus: self-correcting, safe and predictive agent workflows.*
- **Goal**: use loop detection (fingerprints plus cycle detection), trajectory prediction, interception and allocation agents, and see each return its computed result even
  without a reasoner.
- **Kits**:
  ```bash
  python3 demo/demo-vertical-agentic-ops-devops.py --port 16379                # incident triage, loop detection, safety screening
  python3 demo/demo-vertical-agentic-customer-support.py --port 16379          # trajectory forecast, cohort recall, diverse macros
  python3 demo/demo-vertical-agentic-fintech-mcts.py --port 16379              # rebalance and strategy planning
  python3 demo/demo-vertical-cybersecurity-threat-detection.py --port 16379    # threat detection and triage
  python3 demo/demo-vertical-agentic-edge-swarm.py --port 16379                # quantized swarm memory, loop detection, battery-aware routing
  demo/run_demo.sh --port 16379                                       # all forty-four, one after another
  ```

See [Agent Recipes](api-agency.md#which-agent-should-i-use) for what each agent does and when to use it.

### Other short demos

```bash
python3 demo/demo-agents.py --port 16379          # one agent from each family, computed half only
python3 demo/demo-diversify.py --port 16379        # a search that remembers what you rejected
python3 demo/demo-fractal-vector.py --port 16379     # distances, arithmetic, int8 and binary forms, re-rank
```

### Other things in the repository

`bench/head_to_head.py` compares valkey-search's HNSW index with `FRACTAL.EXPLORE` (see the section below), and the enterprise ledger commands are demonstrated in
[enterprise.md](enterprise.md).

---

## 📊 Validation & Benchmarks

### Scout Discovery vs. naive top-K
`demo/demo-vertical-recommendation-search.py` runs the comparison on a clustered catalog: `FRACTAL.SEARCH` returns the *k* nearest rows, which usually sit in the
query's own cluster, while `FRACTAL.EXPLORE` returns a population dispersed across the distinct clusters. The kit asserts the Scout answer covers more clusters.

### Full API surface
`demo.py`, `demo-agents.py`, `demo-fractal-vector.py` and `demo-diversify.py` between them touch every family of command: discovery,
sessions, vector math, analytics and the agents. `./build_test.sh` runs them, and the 44 kits, on every build.

### Latency, responsiveness and throughput

```bash
python3 bench/bench.py --port 16379                          # latency by corpus size, PING during a long search, throughput
bench/scale_sweep.sh --port 16379                           # the same, across a range of corpus sizes
```

`bench/bench.py` reports what the module costs and what it does not cost your other clients: a `PING` from another client stays in the millisecond range
while an `EXPLORE` runs on a worker. The numbers depend on your hardware, so none are quoted here; see [`bench/README.md`](../bench/README.md).

### Head-to-head against valkey-search
`bench/head_to_head.py` runs the same clustered dataset through valkey-search's HNSW index and through Scout (see the section below).

---

## 🧹 Cleanup

```bash
docker compose down                       # stop the demo server (add --profile search for the bundle)
docker compose down --volumes             # and remove any volumes
docker compose exec valkey valkey-cli FLUSHALL          # the demos leave only keys named kit:* and demo:* behind
```

---

## Running next to valkey-search and valkey-json (optional)

FractalSQL needs neither. The default `valkey` service is the official Valkey image with this
module mounted, and everything in this guide works on it. If you also want an indexed
nearest-neighbour search or JSON documents beside it (for example to run
`bench/head_to_head.py`, or to narrow a large collection with `FT.SEARCH` and then run
`FRACTAL.EXPLORE` on the candidates), start the `search` profile, which uses the official
`valkey/valkey-bundle` image:

```bash
docker compose --profile search up -d     # port 16380; modules: search, json, bloom, ldap, fractalsql
docker compose exec valkey-search valkey-cli MODULE LIST
```

FractalSQL loads next to these modules and every FractalSQL command behaves the same there.

