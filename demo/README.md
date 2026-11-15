<p align="center">
  <img src="../FractalSQLforValkey.jpg" alt="FractalSQL for Valkey" width="720">
</p>

# FractalSQL demo

Forty-four runnable starter kits, `demo-vertical-*.py` in this directory: forty-two industry verticals plus two that are specific to Valkey. Each is a problem you can recognise, solved
with the commands in this module, and each is **self-checking**: it prints its report and
exits non-zero unless its headline result holds. They are plain Python (standard library
only) talking RESP to a Valkey server that has the module loaded.

## Prerequisites

1. **The module is loaded.** `valkey-server --loadmodule /path/to/fractalsql.so`, or see
   the [Docker Demo](../docs/docker-demo.md) for a one-command version (`docker compose up -d`; the
   image carries these files at `/demo`).
2. **Python 3.9+.** Nothing to `pip install`.
3. Nothing else. A few steps use an optional piece (a reasoning or embedding provider) and are
   skipped, not failed, when no [`fractalsqld` daemon](../docs/fractalsql-daemon.md) is
   configured.

## Run them

```bash
python3 demo/demo-vertical-quant-finance.py --port 6379      # one kit
docker compose exec valkey /demo/run_demo.sh                 # everything, inside the demo container
demo/run_demo.sh --port 6379                                 # everything, against a server you point it at
demo/run_demo.sh --only demo-vertical-semantic-cache.py --port 6379
demo/run_demo.sh --skip demo-vertical-sovereign-edge-ai.py,demo-vertical-medtech-clinical.py --quiet
demo/run_demo.sh --include-response-modes                    # also response-modes.py (skipped by default)
demo/demo-workload.sh --duration 60 --concurrency 8          # sustained load, p50/p95/p99 per operation
```

A non-zero exit from a kit is a "go read its output" signal. All forty-four kits and the six short demos are also run against a
live server on every build.

## Short demos

Before the verticals, six short demos in this directory show one idea each. They are self-checking like the kits and
need nothing beyond the module; the two that talk to a model say so and skip themselves when no `fractalsqld` daemon is
configured.

| Demo | What it shows |
| --- | --- |
| `demo` | Discovery (Sniper and Scout), range-checked arguments, vector math, an analytics primitive and one agent, in about a minute. Start here. |
| `demo-agents` | A representative agent from each family, computed half only: triage, allocation, recall, recommendation, interception, the diversify proof. |
| `demo-fractal-vector` | Every `FRACTAL.VECTOR.*` family: distances, arithmetic, text round trips, int8 and binary forms, and the cheap-filter-then-re-rank pattern. |
| `demo-diversify` | Diversify/repulsion sessions: search, reject the top result, search again; two sessions given the same calls end in the same state. |
| `response-modes` | What `FRACTAL.REASON` hands back in `text`, `code` and `json` mode, and that embeddings do not depend on it. Needs a daemon. |
| `enterprise-qtl-audit` | The tamper-evident ledger: append decisions, flush, verify the chain. Needs the enterprise library behind the daemon; reports "skipped" on a Community install. |

```bash
python3 demo/demo.py --port 6379
```

## Industry-vertical demos

| Kit | Problem |
| --- | --- |
| `demo-vertical-adas-safety-validation.py` | ADAS / autonomous-driving safety validation: stop an edge case before the maneuver is committed. |
| `demo-vertical-agent-loop-detection.py` | Agent loop detection: notice that an autonomous agent is going around in circles. |
| `demo-vertical-agentic-budget-reallocation.py` | Agentic allocation: multi-source attribution and dynamic cross-channel budget reallocation. |
| `demo-vertical-agentic-customer-support.py` | Customer support: trajectory forecasting, similar-ticket recall and diverse next replies. |
| `demo-vertical-agentic-edge-swarm.py` | Agentic edge and robotics swarm coordination. |
| `demo-vertical-agentic-fintech-mcts.py` | Agentic fintech: planning, portfolio rebalance and a plain-language status question. |
| `demo-vertical-agentic-next-action-ranking.py` | Agentic ranking: autonomous candidate scoring and dynamic next-action routing. |
| `demo-vertical-agentic-ops-devops.py` | DevOps / SRE: dispatch, loop detection and safety checks for an autonomous operator. |
| `demo-vertical-agentic-recall-recommendation.py` | Agentic recommendation: recall of similar past sessions and diverse next-best offers. |
| `demo-vertical-avionics-fault-prediction.py` | Avionics and flight-path fault prediction across a fleet (3-D tracks). |
| `demo-vertical-biotech-genomics.py` | Biotech and genomics: does a point cloud have cluster structure? |
| `demo-vertical-climatech-emissions-monitoring.py` | Industrial emissions and utility-leak detection from a sensor network. |
| `demo-vertical-climatech-grid-balancing.py` | Climate tech: renewable and storage generation balancing on a distribution grid. |
| `demo-vertical-climatech-risk-exposure.py` | Physical climate-risk exposure: a hazard-sensor grid for an insurer or asset manager. |
| `demo-vertical-coordinated-record-detection.py` | Coordinated-record detection: ring signatures and the footprint of staged events. |
| `demo-vertical-critical-inventory-routing.py` | Critical-consumables inventory and replenishment across a depot network. |
| `demo-vertical-cybersecurity-threat-detection.py` | Cybersecurity: one host out of 35 starts beaconing to a command-and-control server. |
| `demo-vertical-document-risk-drift.py` | Document risk drift: clause-level drift detection and compliance review. |
| `demo-vertical-ev-battery-lifecycle.py` | Electric-vehicle fleet battery lifecycle: state of health (SoH) and thermal-regime monitoring. |
| `demo-vertical-event-stream-anomaly-triage.py` | Event-stream triage: a latency ramp, a retry storm, and which endpoints look alike. |
| `demo-vertical-fleet-logistics.py` | Fleet logistics: a vehicle taking a detour, and how much of the network a fleet covers. |
| `demo-vertical-hazmat-occupational-triage.py` | Occupational exposure monitoring: hazmat response, plant crews, wearable-sensor workforce safety. |
| `demo-vertical-heavy-industry-predictive-maintenance.py` | Heavy machinery and fleet predictive maintenance: marine vessels, mining equipment, powertrains. |
| `demo-vertical-intent-matching-discovery.py` | Intent matching: signal clustering and diverse candidate selection. |
| `demo-vertical-maritime-defense.py` | Maritime, aviation and defense: AIS-style vessel tracking. |
| `demo-vertical-medtech-clinical.py` | MedTech: cohort search over vitals, trend drift, and pre-extracted geometry. |
| `demo-vertical-multi-series-replenishment.py` | Multi-location replenishment: stockout prevention and non-linear demand forecasting. |
| `demo-vertical-outlier-interception-footprint.py` | Automated-traffic interception: bot-farm and invalid-traffic screening. |
| `demo-vertical-periodicity-portfolio-selection.py` | Periodicity and selection: cycle-strength detection and risk-aware portfolio choice. |
| `demo-vertical-quant-finance.py` | Quantitative finance: a cardinality-constrained book and a volatility regime change. |
| `demo-vertical-recommendation-search.py` | Recommendations and discovery: nearest matches AND a diverse "you might also like" set. |
| `demo-vertical-rf-spectrum-sensing.py` | RF spectrum sensing: a locked carrier that is jammed halfway through a capture. |
| `demo-vertical-satellite-constellation-coverage.py` | Space and satellite communications: constellation coverage monitoring. |
| `demo-vertical-scenario-planning-simulation.py` | Strategic scenario planning and simulation training: business continuity, logistics contingency, tabletop exercises. |
| `demo-vertical-semantic-cache.py` | Semantic cache: answer a repeated question from Valkey instead of calling the model. |
| `demo-vertical-sensor-telemetry-allocation.py` | Resource allocation: sensor telemetry regime shifts and non-linear demand. |
| `demo-vertical-simulated-economy-drift.py` | Simulated-economy balancing: market inflation loops and automated-actor detection. |
| `demo-vertical-smart-cities-iot.py` | Smart cities: sensor-grid coverage, a sensor whose readings change regime, zone sampling. |
| `demo-vertical-sovereign-edge-ai.py` | Sovereign and edge AI: placement and allocation with no external service. |
| `demo-vertical-spatial-footprint-portfolio-risk.py` | Spatial footprint analysis: box-counting dimension and portfolio risk. |
| `demo-vertical-stalled-process-triage.py` | Staged pipeline: velocity bottlenecks and stalled-item triage. |
| `demo-vertical-telecom-account-anomaly-screening.py` | Telecom: anomalous account-change screening (SIM swap, port-out, plan change, unusual device or location). |
| `demo-vertical-telecom-traffic-routing.py` | Telecom: dynamic traffic and bandwidth dispatch to the best-capable network node under a capacity budget. |
| `demo-vertical-v2x-warranty-triage.py` | Connected-vehicle (V2X) diagnostics: route a trouble-code event, recall similar warranty claims. |

The full table, with the commands each one shows, is in [Starter Kits](../docs/starter-kits.md).

## Writing your own

`demo/fvlib.py` is the small shared client: `Kit(name)` connects and seeds a
deterministic generator, `expect(what, ok)` records a check, `done()` exits non-zero if any
failed. A kit is a seeded dataset, a few commands, and the `expect` calls that state what
should be true.

## Benchmark

The benchmark is [`bench/`](../bench/README.md).
