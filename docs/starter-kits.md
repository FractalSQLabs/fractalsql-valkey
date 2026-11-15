<p align="center">
  <img src="../FractalSQLforValkey.jpg" alt="FractalSQL for Valkey" width="720">
</p>

# Starter Kits: Apply FractalSQL to Your Industry

You have the module running ([getting-started.md](getting-started.md)). Now:
**which end-to-end example do I run for *my* problem?**

FractalSQL for Valkey ships forty-four runnable industry walkthroughs: the forty-two
verticals the SQL editions ship (twenty-seven **domain
verticals**, mostly no-model, and fifteen **agentic verticals**, composed multi-step agents),
plus two that are specific to Valkey. Each kit is a self-contained Python script in
`demo/` (`demo-vertical-*.py`): it builds its own seeded synthetic dataset, loads it into keys, runs the
commands that genuinely fit that domain, and **checks its own headline result**, exiting
non-zero if it does not hold. They are re-runnable, deterministic (seeded by the kit's name),
standard-library-only Python, and all forty-four are run against a live server on every build.

> Run any kit against a server that has the module loaded:
> ```bash
> python3 demo/demo-vertical-quant-finance.py --port 6379      # one kit
> demo/run_demo.sh                                     # all of them (Docker Compose)
> ```
> `--quiet` prints only failures. The library every kit shares is `demo/fvlib.py`
> (a ~150-line RESP client plus `Kit`, `expect` and a few series generators).

**What the kits need.** The module, and nothing else. A few steps use an optional piece
behind `fractalsqld` (an embedding or reasoning provider); a kit says when it skips one.
A kit never fails because no model is installed: an agent that cannot reach a reasoner still
returns what it computed, with `reasoning_step_failed` naming the error (see
[api-agency.md](api-agency.md#partial-results-when-the-reasoner-is-not-available)).

## Which kit should I run?

| If your problem is… | Run this kit | Commands it exercises | Agent & recipe |
|---|---|---|---|
| Edge swarm / robotics coordination | `demo-vertical-agentic-edge-swarm.py` | `VECTOR.QUANTIZE.INT8`, `VECTOR.QUANTIZE.BINARY`, `VECTOR.HAMMING`, `OPTIMIZE.PORTFOLIO`, `AGENT.DETECT_LOOP` | [AGENT.DETECT_LOOP](api-agency.md#detect-loop-fractalagentdetect_loop) |
| Avionics / flight-path fault prediction | `demo-vertical-avionics-fault-prediction.py` | `DIM.DFA`, `AGENT.TRACK_ANOMALY` | [AGENT.TRACK_ANOMALY](api-agency.md#track-anomaly-fractalagenttrack_anomaly) |
| Structural biology / single-cell genomics | `demo-vertical-biotech-genomics.py` | `TDA.PERSISTENCE`, `VECTOR.LP`, `VECTOR.L2` | [api-analytics.md](api-analytics.md) |
| Industrial emissions / utility-leak detection | `demo-vertical-climatech-emissions-monitoring.py` | `DIM.DRIFT`, `AGENT.ANOMALY_TRIAGE` | [AGENT.ANOMALY_TRIAGE](api-agency.md#anomaly-triage-fractalagentanomaly_triage) |
| Physical climate-risk exposure (wildfire / flood / drought) | `demo-vertical-climatech-risk-exposure.py` | `MORPH.COMPLEXITY`, `AGENT.NETWORK_COVERAGE_ALERT` | [AGENT.NETWORK_COVERAGE_ALERT](api-agency.md#network-coverage-alert-fractalagentnetwork_coverage_alert) |
| InsurTech: claims-fraud rings | `demo-vertical-coordinated-record-detection.py` | `SEARCH`, `DIM.BOXCOUNT`, `AGENT.OUTLIER_INTERCEPT` | [AGENT.OUTLIER_INTERCEPT](api-agency.md#outlier-intercept-fractalagentoutlier_intercept) |
| Cybersecurity / network behavior analytics | `demo-vertical-cybersecurity-threat-detection.py` | `SEARCH`, `EXPLORE`, `DIM.DRIFT`, `SERIES.PERIODOGRAM` | [api-analytics.md](api-analytics.md) |
| LegalTech: contract clause drift and replacement language | `demo-vertical-document-risk-drift.py` | `DIM.DRIFT`, `EXPLORE`, `AGENT.ANOMALY_TRIAGE`, `AGENT.RECOMMEND_DIVERSE` | [AGENT.ANOMALY_TRIAGE](api-agency.md#anomaly-triage-fractalagentanomaly_triage), [AGENT.RECOMMEND_DIVERSE](api-agency.md#recommend-diverse-fractalagentrecommend_diverse-pure-retrieval-no-llm) |
| AdTech: bid-stream latency spikes and retry storms | `demo-vertical-event-stream-anomaly-triage.py` | `DIM.DRIFT`, `SERIES.PERIODOGRAM`, `SEARCH` | [api-analytics.md](api-analytics.md) |
| Fleet logistics / detour detection | `demo-vertical-fleet-logistics.py` | `SEARCH`, `EXPLORE`, `DIM.BOXCOUNT` | [api-analytics.md](api-analytics.md) |
| Hazmat / occupational exposure triage | `demo-vertical-hazmat-occupational-triage.py` | `AGENT.PATIENT_DETERIORATION_TRIAGE` | [AGENT.PATIENT_DETERIORATION_TRIAGE](api-agency.md#patient-deterioration-triage-fractalagentpatient_deterioration_triage) |
| Marketing: account discovery and diverse target lists | `demo-vertical-intent-matching-discovery.py` | `EXPLORE`, `SEARCH`, `AGENT.RECOMMEND_DIVERSE` | [AGENT.RECOMMEND_DIVERSE](api-agency.md#recommend-diverse-fractalagentrecommend_diverse-pure-retrieval-no-llm) |
| Maritime / aviation track anomaly | `demo-vertical-maritime-defense.py` | `SEARCH`, `AGENT.TRACK_ANOMALY` | [AGENT.TRACK_ANOMALY](api-agency.md#track-anomaly-fractalagenttrack_anomaly) |
| Patient monitoring / clinical telemetry | `demo-vertical-medtech-clinical.py` | `SEARCH`, `DIM.DRIFT`, `MORPH.VASCULAR`, `MORPH.CORTICAL`, `MORPH.NERVE` | [api-analytics.md](api-analytics.md) |
| AdTech: bot-farm / invalid-traffic screening | `demo-vertical-outlier-interception-footprint.py` | `DIM.BOXCOUNT`, `AGENT.OUTLIER_INTERCEPT` | [AGENT.OUTLIER_INTERCEPT](api-agency.md#outlier-intercept-fractalagentoutlier_intercept) |
| Retail: dynamic pricing and markdown selection | `demo-vertical-periodicity-portfolio-selection.py` | `SERIES.PERIODOGRAM`, `OPTIMIZE.PORTFOLIO`, `AGENT.RECOMMEND_DIVERSE`, `AGENT.DIVERSE_PORTFOLIOS` | [AGENT.RECOMMEND_DIVERSE](api-agency.md#recommend-diverse-fractalagentrecommend_diverse-pure-retrieval-no-llm), [AGENT.DIVERSE_PORTFOLIOS](api-agency.md#diverse-portfolios-fractalagentdiverse_portfolios-enterprise-tier) |
| Portfolio construction / regime detection | `demo-vertical-quant-finance.py` | `OPTIMIZE.PORTFOLIO`, `SERIES.CHANGEPOINT`, `DIM.DRIFT` | [api-analytics.md](api-analytics.md) |
| Recommendation / diverse search | `demo-vertical-recommendation-search.py` | `SEARCH`, `EXPLORE`, `VECTOR.CONCAT`, `VECTOR.QUANTIZE.INT8` | [api-analytics.md](api-analytics.md) |
| RF / signal-interference spectrum sensing | `demo-vertical-rf-spectrum-sensing.py` | `SERIES.PERIODOGRAM`, `SERIES.SPECTROGRAM`, `SERIES.CHANGEPOINT` | [api-analytics.md](api-analytics.md) |
| Satellite constellation coverage gaps | `demo-vertical-satellite-constellation-coverage.py` | `AGENT.NETWORK_COVERAGE_ALERT`, `AGENT.SCHEDULE_WORKLOAD` | [AGENT.NETWORK_COVERAGE_ALERT](api-agency.md#network-coverage-alert-fractalagentnetwork_coverage_alert), [AGENT.SCHEDULE_WORKLOAD](api-agency.md#schedule-workload-fractalagentschedule_workload) |
| Scenario planning & simulation training | `demo-vertical-scenario-planning-simulation.py` | `EXPLORE`, `SEARCH`, `AGENT.PLAN_EXPLORE` | [AGENT.PLAN_EXPLORE](api-agency.md#plan-explore-fractalagentplan_explore) |
| Smart cities / IoT sensor grids | `demo-vertical-smart-cities-iot.py` | `MORPH.COMPLEXITY`, `DIM.DRIFT`, `EXPLORE` | [api-analytics.md](api-analytics.md) |
| Edge / autonomous fleet allocation | `demo-vertical-sovereign-edge-ai.py` | `SEARCH`, `OPTIMIZE.PORTFOLIO`, `DIM.BOXCOUNT`, `REASON` | [api-analytics.md](api-analytics.md) |
| PropTech: spatial valuation and portfolio risk | `demo-vertical-spatial-footprint-portfolio-risk.py` | `DIM.BOXCOUNT`, `DIM.DFA`, `OPTIMIZE.PORTFOLIO`, `EXPLORE`, `SEARCH` | [api-analytics.md](api-analytics.md) |
| Sales: stalled pipeline / deal-health drift | `demo-vertical-stalled-process-triage.py` | `AGENT.PATIENT_DETERIORATION_TRIAGE` | [AGENT.PATIENT_DETERIORATION_TRIAGE](api-agency.md#patient-deterioration-triage-fractalagentpatient_deterioration_triage) |
| Telecom account-change screening (SIM swap, port-out) | `demo-vertical-telecom-account-anomaly-screening.py` | `AGENT.OUTLIER_INTERCEPT` | [AGENT.OUTLIER_INTERCEPT](api-agency.md#outlier-intercept-fractalagentoutlier_intercept) |
| Telecom traffic / bandwidth dispatch to the best node | `demo-vertical-telecom-traffic-routing.py` | `AGENT.ROUTE_TASK` | [AGENT.ROUTE_TASK](api-agency.md#route-task-fractalagentroute_task) |
| ADAS / autonomous-driving safety validation | `demo-vertical-adas-safety-validation.py` | `SEARCH`, `AGENT.OUTLIER_INTERCEPT` | [AGENT.OUTLIER_INTERCEPT](api-agency.md#outlier-intercept-fractalagentoutlier_intercept) |
| AdTech channel attribution and budget reallocation | `demo-vertical-agentic-budget-reallocation.py` | `DIM.DFA`, `OPTIMIZE.PORTFOLIO`, `AGENT.ALLOCATE`, `AGENT.ROUTE_TASK` | [AGENT.ALLOCATE](api-agency.md#allocate-fractalagentallocate), [AGENT.ROUTE_TASK](api-agency.md#route-task-fractalagentroute_task) |
| Customer support recall + recommend | `demo-vertical-agentic-customer-support.py` | `AGENT.TRAJECTORY_PREDICT`, `AGENT.RECALL_HYBRID`, `AGENT.RECOMMEND_DIVERSE` | [AGENT.TRAJECTORY_PREDICT](api-agency.md#trajectory-predict-fractalagenttrajectory_predict), [AGENT.RECALL_HYBRID](api-agency.md#recall-hybrid-fractalagentrecall_hybrid-pure-retrieval-no-llm), [AGENT.RECOMMEND_DIVERSE](api-agency.md#recommend-diverse-fractalagentrecommend_diverse-pure-retrieval-no-llm) |
| FinTech portfolio rebalance + MCTS | `demo-vertical-agentic-fintech-mcts.py` | `OPTIMIZE.PORTFOLIO`, `AGENT.REBALANCE_SIBLING`, `AGENT.PLAN_EXPLORE` | [AGENT.REBALANCE_SIBLING](api-agency.md#rebalance-sibling-fractalagentrebalance_sibling), [AGENT.PLAN_EXPLORE](api-agency.md#plan-explore-fractalagentplan_explore) |
| Sales lead scoring and next best action | `demo-vertical-agentic-next-action-ranking.py` | `AGENT.RECALL_HYBRID`, `AGENT.RECOMMEND_DIVERSE` | [AGENT.RECALL_HYBRID](api-agency.md#recall-hybrid-fractalagentrecall_hybrid-pure-retrieval-no-llm), [AGENT.RECOMMEND_DIVERSE](api-agency.md#recommend-diverse-fractalagentrecommend_diverse-pure-retrieval-no-llm) |
| DevOps / SRE dispatch + safety | `demo-vertical-agentic-ops-devops.py` | `AGENT.DETECT_LOOP`, `AGENT.ANOMALY_TRIAGE`, `AGENT.OUTLIER_INTERCEPT`, `AGENT.ROUTE_TASK`, `AGENT.RAG_AGENT` | [AGENT.DETECT_LOOP](api-agency.md#detect-loop-fractalagentdetect_loop), [AGENT.ANOMALY_TRIAGE](api-agency.md#anomaly-triage-fractalagentanomaly_triage), [AGENT.OUTLIER_INTERCEPT](api-agency.md#outlier-intercept-fractalagentoutlier_intercept), [AGENT.ROUTE_TASK](api-agency.md#route-task-fractalagentroute_task), [AGENT.RAG_AGENT](api-agency.md#rag-agent-fractalagentrag_agent) |
| E-commerce cart-abandonment recovery | `demo-vertical-agentic-recall-recommendation.py` | `EXPLORE`, `SEARCH`, `AGENT.RECALL_HYBRID`, `AGENT.RECOMMEND_DIVERSE` | [AGENT.RECALL_HYBRID](api-agency.md#recall-hybrid-fractalagentrecall_hybrid-pure-retrieval-no-llm), [AGENT.RECOMMEND_DIVERSE](api-agency.md#recommend-diverse-fractalagentrecommend_diverse-pure-retrieval-no-llm) |
| Grid balancing (renewables + storage dispatch) | `demo-vertical-climatech-grid-balancing.py` | `OPTIMIZE.PORTFOLIO`, `AGENT.ANOMALY_TRIAGE`, `AGENT.ALLOCATE` | [AGENT.ANOMALY_TRIAGE](api-agency.md#anomaly-triage-fractalagentanomaly_triage), [AGENT.ALLOCATE](api-agency.md#allocate-fractalagentallocate) |
| Critical-consumables inventory & depot replenishment | `demo-vertical-critical-inventory-routing.py` | `DIM.DFA`, `AGENT.ALLOCATE` | [AGENT.ALLOCATE](api-agency.md#allocate-fractalagentallocate) |
| EV fleet battery health / thermal runaway | `demo-vertical-ev-battery-lifecycle.py` | `AGENT.PATIENT_DETERIORATION_TRIAGE`, `AGENT.ANOMALY_TRIAGE` | [AGENT.PATIENT_DETERIORATION_TRIAGE](api-agency.md#patient-deterioration-triage-fractalagentpatient_deterioration_triage), [AGENT.ANOMALY_TRIAGE](api-agency.md#anomaly-triage-fractalagentanomaly_triage) |
| Heavy-machinery / fleet predictive maintenance | `demo-vertical-heavy-industry-predictive-maintenance.py` | `AGENT.ANOMALY_TRIAGE`, `AGENT.PATIENT_DETERIORATION_TRIAGE` | [AGENT.ANOMALY_TRIAGE](api-agency.md#anomaly-triage-fractalagentanomaly_triage), [AGENT.PATIENT_DETERIORATION_TRIAGE](api-agency.md#patient-deterioration-triage-fractalagentpatient_deterioration_triage) |
| Retail demand forecasting and replenishment | `demo-vertical-multi-series-replenishment.py` | `DIM.DFA`, `OPTIMIZE.PORTFOLIO`, `AGENT.ALLOCATE` | [AGENT.ALLOCATE](api-agency.md#allocate-fractalagentallocate) |
| AgTech soil-telemetry resource allocation | `demo-vertical-sensor-telemetry-allocation.py` | `DIM.DFA`, `OPTIMIZE.PORTFOLIO`, `AGENT.ALLOCATE` | [AGENT.ALLOCATE](api-agency.md#allocate-fractalagentallocate) |
| Gaming in-game economy inflation and bot loops | `demo-vertical-simulated-economy-drift.py` | `SERIES.PERIODOGRAM`, `DIM.DFA`, `OPTIMIZE.PORTFOLIO`, `AGENT.DETECT_LOOP` | [AGENT.DETECT_LOOP](api-agency.md#detect-loop-fractalagentdetect_loop) |
| Connected-vehicle warranty & repair routing | `demo-vertical-v2x-warranty-triage.py` | `AGENT.ROUTE_TASK`, `AGENT.RECALL_HYBRID` | [AGENT.ROUTE_TASK](api-agency.md#route-task-fractalagentroute_task), [AGENT.RECALL_HYBRID](api-agency.md#recall-hybrid-fractalagentrecall_hybrid-pure-retrieval-no-llm) |
| Cache answers next to the data: repeat questions without calling the model | `demo-vertical-semantic-cache.py` | `SEARCH`, `VECTOR.NORMALIZE` | |
| An autonomous agent going around in circles | `demo-vertical-agent-loop-detection.py` | `SERIES.FINGERPRINT`, `CYCLE.DETECT`, `VECTOR.HAMMING` | [detect_loop](api-agency.md#detect-loop-fractalagentdetect_loop) |

> Not sure which agent does what? The [decision table](api-agency.md#which-agent-should-i-use)
> maps every problem shape to its agent.

## Domain kits (run with no model)

Every domain kit runs without a reasoning endpoint. Where a kit calls an agent, the agent
returns its computed result and `reasoning_step_failed` instead of a rationale; configure a
provider behind `fractalsqld` and the same call also returns the narrative. Vectors travel as
packed little-endian float32 blobs in string keys, and a "table" is a corpus key (optionally
with companion hashes for ids, content and filters), which is the one storage convention all
forty-four kits share. See [getting-started.md](getting-started.md#2-your-first-search-no-model-needed).

### Agentic Edge Swarms — `demo-vertical-agentic-edge-swarm.py`
Agentic edge and robotics swarm coordination. Ten battery-constrained edge agents each carry a small local memory vector that must be compressed for radio- constrained inter-agent sync (FRACTAL.VECTOR.QUANTIZE.INT8 and .BINARY, compared with FRACTAL.VECTOR.HAMMING). One agent is stuck oscillating between two headings (a control-loop "wobble") that a fleet monitor catches from its raw state stream with FRACTAL.AGENT.DETECT_LOOP. A battery-constrained pass chooses which agents take on high-value tasks.
Commands: `VECTOR.QUANTIZE.INT8`, `VECTOR.QUANTIZE.BINARY`, `VECTOR.HAMMING`, `AGENT.DETECT_LOOP`, `OPTIMIZE.PORTFOLIO`.

### Avionics Fault Prediction — `demo-vertical-avionics-fault-prediction.py`
Avionics and flight-path fault prediction across a fleet (3-D tracks). Twelve flights each have a planned path and a latest state, [x_km, y_km, altitude_km]. AC-09 has a stuck control surface: its path has drifted off plan and its heading changes become erratic for a stretch. FRACTAL.AGENT.TRACK_ANOMALY finds which fleet deviation signature matches the change and reports the heading series' DFA exponent (erratic heading changes persist, so the exponent rises).
Commands: `AGENT.TRACK_ANOMALY`, `DIM.DFA`.

### Biotech / Genomics — `demo-vertical-biotech-genomics.py`
Biotech and genomics: does a point cloud have cluster structure? A synthetic stand-in for a low-dimensional embedding of single-cell data: two cell populations 6 units apart. FRACTAL.TDA.PERSISTENCE answers "how many clusters" with a persistence diagram: within-cluster merges happen early, the single merge between the clusters happens late. FRACTAL.VECTOR.LP compares profiles at different p (L1 for robust feature comparison, L2 for geometry).
Commands: `TDA.PERSISTENCE`, `VECTOR.LP`, `VECTOR.L2`.

### Climate Tech: Emissions Monitoring — `demo-vertical-climatech-emissions-monitoring.py`
Industrial emissions and utility-leak detection from a sensor network. Six sensors (methane, CO2, water, one pipeline-pressure sensor) each report 96 readings. A leak is an ACCELERATING ramp downstream of the source: DFA removes a constant offset, so only a change in the series' dynamics moves the exponent. FRACTAL.AGENT.ANOMALY_TRIAGE scores every sensor with the same call; the agent is indifferent to the physical quantity.
Commands: `AGENT.ANOMALY_TRIAGE`, `DIM.DRIFT`.

### Climate Tech: Risk Exposure — `demo-vertical-climatech-risk-exposure.py`
Physical climate-risk exposure: a hazard-sensor grid for an insurer or asset manager. A 20 x 20 grid of cells (three sensors each) carries a fuel-moisture deficit index. A cell is high-hazard above 0.40 in the dry season, and 0.25 in the wet season (where the index runs about half as high). FRACTAL.AGENT. NETWORK_COVERAGE_ALERT scores the spatial morphology of the hazard footprint (dimension and lacunarity) and whether a telemetry series has changed regime. Same call, either season.
Commands: `AGENT.NETWORK_COVERAGE_ALERT`, `MORPH.COMPLEXITY`.

### InsurTech / Claims Fraud Rings — `demo-vertical-coordinated-record-detection.py`
Coordinated-record detection: ring signatures and the footprint of staged events. A review desk screens new submissions against known ring signatures (FRACTAL.AGENT.OUTLIER_INTERCEPT), finds which open records moved the way a staged ring did (a search on the change in severity), and measures the geographic footprint of event locations with FRACTAL.DIM.BOXCOUNT: organic events wander across the city, while a staged ring repeats along one road.
Commands: `AGENT.OUTLIER_INTERCEPT`, `SEARCH`, `DIM.BOXCOUNT`.

### Cybersecurity — `demo-vertical-cybersecurity-threat-detection.py`
Cybersecurity: one host out of 35 starts beaconing to a command-and-control server. Each host has a baseline and a current traffic vector (connection volume, unique destination ports, DNS rate, failed-auth rate). The compromised host changes on the first three but not the fourth, which is a beaconing profile, not a brute-force attempt. Found by similarity search, confirmed by DFA drift, and its beacon cadence read from the periodogram.
Commands: `SEARCH`, `EXPLORE`, `DIM.DRIFT`, `SERIES.PERIODOGRAM`.

### LegalTech / Contract Clause Drift — `demo-vertical-document-risk-drift.py`
Document risk drift: clause-level drift detection and compliance review. Four agreements each have 96 weekly clause-risk reviews; one agreement's amended terms drift into a new regime (an accelerating risk creep). FRACTAL.DIM.DRIFT finds it and FRACTAL.AGENT.ANOMALY_TRIAGE is the one-call version. Scout (FRACTAL.EXPLORE) pulls clause variants from across the four clause families so the reviewer sees the spread rather than near-copies, and FRACTAL.AGENT.RECOMMEND_DIVERSE proposes approved replacement language.
Commands: `DIM.DRIFT`, `AGENT.ANOMALY_TRIAGE`, `EXPLORE`, `AGENT.RECOMMEND_DIVERSE`.

### AdTech / Latency & Retry Storms — `demo-vertical-event-stream-anomaly-triage.py`
Event-stream triage: a latency ramp, a retry storm, and which endpoints look alike. Four endpoints x 192 one-minute intervals. EP-C develops an accelerating latency ramp (a timeout cascade); EP-B has a retry-storm oscillation in request rate. FRACTAL.DIM.DRIFT finds the regime change, FRACTAL.SERIES.PERIODOGRAM the storm's period, and FRACTAL.SEARCH which of a 40-endpoint fleet has health like EP-C's.
Commands: `DIM.DRIFT`, `SERIES.PERIODOGRAM`, `SEARCH`.

### Fleet Logistics — `demo-vertical-fleet-logistics.py`
Fleet logistics: a vehicle taking a detour, and how much of the network a fleet covers. 40 vehicles with route-embedding vectors across four depots. One vehicle's GPS trace meanders instead of following its route; FRACTAL.DIM.BOXCOUNT scores the trace's complexity (a straight line is about 1, a meander is higher). A cohort is just another key, so a route-3-only search is a SEARCH on that key.
Commands: `SEARCH`, `EXPLORE`, `DIM.BOXCOUNT`.

### Hazmat / Occupational Exposure — `demo-vertical-hazmat-occupational-triage.py`
Occupational exposure monitoring: hazmat response, plant crews, wearable-sensor workforce safety. FRACTAL.AGENT.PATIENT_DETERIORATION_TRIAGE has a domain-neutral contract (a cohort search plus a baseline-to- current drift), so the kit only supplies exposure columns [dose_rate_z, body_temp_z, air_quality_z, heart_rate_z] in place of clinical vitals, and a worker's pre-shift baseline in place of an admission baseline. The cohort is one crew, selected with FILTER on a companion hash.
Commands: `AGENT.PATIENT_DETERIORATION_TRIAGE`.

### Marketing / Account Discovery — `demo-vertical-intent-matching-discovery.py`
Intent matching: signal clustering and diverse candidate selection. Twenty-four target entities fall into four intent clusters (category + stage signal vectors), and there is a thirty-item content library. Scout (FRACTAL.EXPLORE, the diversity-first search) picks entities from across the clusters around a target profile instead of six near-duplicates, and FRACTAL.AGENT.RECOMMEND_DIVERSE returns a diverse content set for the chosen entity. No model is needed for either.
Commands: `EXPLORE`, `SEARCH`, `AGENT.RECOMMEND_DIVERSE`.

### Maritime / Defense — `demo-vertical-maritime-defense.py`
Maritime, aviation and defense: AIS-style vessel tracking. Thirty vessels report a track vector [lat, lon, speed, heading] against a baseline; one vessel is given a deliberate course deviation. A search on the change (current - baseline) is a direct fit for track-deviation detection ("what changed"), and the DFA scaling exponent of a heading-change series fits maneuvering irregularity (a smooth transit vs an erratic track). FRACTAL.AGENT.TRACK_ANOMALY does both in one call.
Commands: `SEARCH`, `AGENT.TRACK_ANOMALY`.

### MedTech / Clinical — `demo-vertical-medtech-clinical.py`
MedTech: cohort search over vitals, trend drift, and pre-extracted geometry. Patient vitals are vectors, so "which patients look like this deteriorating one" is FRACTAL.SEARCH; a heart-rate series' regime change is FRACTAL.DIM.DRIFT. The geometry commands take PRE-EXTRACTED vessel graphs, meshes and nerve skeletons, not raw imaging.
Commands: `SEARCH`, `DIM.DRIFT`, `MORPH.VASCULAR`, `MORPH.CORTICAL`, `MORPH.NERVE`.

### AdTech / Bot Footprints — `demo-vertical-outlier-interception-footprint.py`
Automated-traffic interception: bot-farm and invalid-traffic screening. Proposed sessions are screened against a history of known-abuse signatures with FRACTAL.AGENT.OUTLIER_INTERCEPT (cosine by default, exact Euclidean when magnitude should count). FRACTAL.DIM.BOXCOUNT on click-coordinate clouds then shows the footprint of each traffic source: organic traffic fills the page, while an automated farm hammering a handful of targets collapses onto a line (a dimension near 1).
Commands: `AGENT.OUTLIER_INTERCEPT`, `DIM.BOXCOUNT`.

### Retail / Dynamic Pricing — `demo-vertical-periodicity-portfolio-selection.py`
Periodicity and selection: cycle-strength detection and risk-aware portfolio choice. Six items x 96 days of sales velocity with a weekly demand cycle of differing strength. FRACTAL.SERIES.PERIODOGRAM quantifies which items are genuinely periodic; FRACTAL.OPTIMIZE.PORTFOLIO picks the 3 items that get the scheduled action (a Sharpe-style trade of margin recovered against demand volatility); FRACTAL.AGENT.RECOMMEND_DIVERSE proposes a diverse set of substitute items. On the Enterprise edition FRACTAL.AGENT.DIVERSE_PORTFOLIOS also returns structurally different schedules; on a Community install that step is skipped.
Commands: `SERIES.PERIODOGRAM`, `OPTIMIZE.PORTFOLIO`, `AGENT.RECOMMEND_DIVERSE`, `AGENT.DIVERSE_PORTFOLIOS`.
Skipped here: `DIVERSE_PORTFOLIOS` needs a provider behind `fractalsqld` and is skipped without one.

### Quantitative Finance — `demo-vertical-quant-finance.py`
Quantitative finance: a cardinality-constrained book and a volatility regime change. 25 synthetic assets under a 4-factor covariance model, optimised down to an 8-asset book (FRACTAL.OPTIMIZE.PORTFOLIO), and a return series whose volatility triples at t=300, localised with FRACTAL.SERIES.CHANGEPOINT and confirmed with FRACTAL.DIM.DRIFT (DFA).
Commands: `OPTIMIZE.PORTFOLIO`, `SERIES.CHANGEPOINT`, `DIM.DRIFT`.

### Recommendation / Search — `demo-vertical-recommendation-search.py`
Recommendations and discovery: nearest matches AND a diverse "you might also like" set. A 240-item catalog in six taste clusters. FRACTAL.SEARCH returns the closest items; FRACTAL.EXPLORE returns items from DISTINCT regions of the catalog, which a plain top-k structurally cannot do. A cross-modal query blends content and behaviour vectors with FRACTAL.VECTOR.CONCAT; FRACTAL.VECTOR.QUANTIZE.INT8 shows the compact form.
Commands: `SEARCH`, `EXPLORE`, `VECTOR.CONCAT`, `VECTOR.QUANTIZE.INT8`.

### RF Spectrum Sensing — `demo-vertical-rf-spectrum-sensing.py`
RF spectrum sensing: a locked carrier that is jammed halfway through a capture. FRACTAL.SERIES.PERIODOGRAM says WHICH frequency dominates; FRACTAL.SERIES.SPECTROGRAM says HOW ORDERED the spectrum is, window by window (low = a few strong frequencies, high = spread across the band); FRACTAL.SERIES.CHANGEPOINT then localises the frame where order is lost.
Commands: `SERIES.PERIODOGRAM`, `SERIES.SPECTROGRAM`, `SERIES.CHANGEPOINT`.

### Satellite Constellation Coverage — `demo-vertical-satellite-constellation-coverage.py`
Space and satellite communications: constellation coverage monitoring. FRACTAL.AGENT.NETWORK_COVERAGE_ALERT scores the spatial morphology of the constellation's footprint grid (dimension and lacunarity) plus the drift of a link-quality telemetry series, flagging coverage gaps and dead zones. FRACTAL.AGENT.SCHEDULE_WORKLOAD then refines a workload's requirement vector and matches the nearest slot. This is the COVERAGE-GAP angle only; orbital drift and collision detection are a different problem and are not covered.
Commands: `AGENT.NETWORK_COVERAGE_ALERT`, `AGENT.SCHEDULE_WORKLOAD`.

### Scenario Planning & Simulation — `demo-vertical-scenario-planning-simulation.py`
Strategic scenario planning and simulation training: business continuity, logistics contingency, tabletop exercises. The job is a SPREAD of distinct, non-overlapping scenario variants. Twenty-four candidate scenarios sit in a four- dimension option space (cost, risk, speed, resilience). Scout (FRACTAL.EXPLORE, the diversity-first search) picks scenarios from across the space with no model. FRACTAL.AGENT.PLAN_EXPLORE, the shipped planner, embeds a seed-state description and Scout-searches the embedded scenario descriptions; it needs an embedding provider behind fractalsqld and is skipped without one. Embed your own descriptions with FRACTAL.EMBED and store the vectors as the corpus rows.
Commands: `EXPLORE`, `SEARCH`, `AGENT.PLAN_EXPLORE`.
Skipped here: `PLAN_EXPLORE` needs a provider behind `fractalsqld` and is skipped without one.

### Smart Cities / IoT — `demo-vertical-smart-cities-iot.py`
Smart cities: sensor-grid coverage, a sensor whose readings change regime, zone sampling. A 400-sensor jittered 20x20 grid. FRACTAL.MORPH.COMPLEXITY scores the spatial coverage; FRACTAL.DIM.DRIFT finds the one sensor whose series shifted (an air-quality event); FRACTAL.EXPLORE samples representative zones.
Commands: `MORPH.COMPLEXITY`, `DIM.DRIFT`, `EXPLORE`.

### Sovereign / Edge AI — `demo-vertical-sovereign-edge-ai.py`
Sovereign and edge AI: placement and allocation with no external service. Everything here runs inside the Valkey process in pure C: no vector database, no cloud call. A fleet of 50 edge nodes: find the best node for a workload (FRACTAL.SEARCH), then use FRACTAL.OPTIMIZE.PORTFOLIO as a general on-device resource allocator. The optional rationale step uses FRACTAL.REASON and is skipped, not failed, when no fractalsqld daemon is configured.
Commands: `SEARCH`, `OPTIMIZE.PORTFOLIO`, `DIM.BOXCOUNT`, `REASON`.

### Real Estate / Spatial Valuation — `demo-vertical-spatial-footprint-portfolio-risk.py`
Spatial footprint analysis: box-counting dimension and portfolio risk. Three districts' parcel footprints (a dense grid, a single arterial strip, a two-corridor district) are measured with FRACTAL.DIM.BOXCOUNT: the lower the dimension, the more linear the frontage and the more a valuation depends on a few corridors. Six asset classes x 64 months of index values feed FRACTAL.DIM.DFA and FRACTAL.OPTIMIZE.PORTFOLIO (pick 3 classes for the renewal push; the accelerating one is flagged), and Scout (FRACTAL.EXPLORE) selects comparable parcels from across the market, not near-twins.
Commands: `DIM.BOXCOUNT`, `DIM.DFA`, `OPTIMIZE.PORTFOLIO`, `EXPLORE`, `SEARCH`.

### Sales / Stalled Pipeline Items — `demo-vertical-stalled-process-triage.py`
Staged pipeline: velocity bottlenecks and stalled-item triage. Thirty-six open items across segments and stages; three tier-1 items in the review stage show the same stall signature (time-in-stage far above the healthy velocity, falling engagement, a silent owner, deepening concessions). FRACTAL.AGENT.PATIENT_DETERIORATION_TRIAGE is reused as-is (a cohort search plus a baseline-to-current drift, with item-health columns in place of clinical vitals): an item's stage-entry telemetry plays the patient's admission baseline. The cohort, tier-1 items in the review stage, is a companion hash selected with FILTER.
Commands: `AGENT.PATIENT_DETERIORATION_TRIAGE`.

### Telecom Account-Anomaly Screening — `demo-vertical-telecom-account-anomaly-screening.py`
Telecom: anomalous account-change screening (SIM swap, port-out, plan change, unusual device or location). A general-purpose pre-commit safety check, not a fraud-specific product: FRACTAL.AGENT.OUTLIER_INTERCEPT screens a PROPOSED event against a history of known-anomalous change patterns (the same shape as the ADAS kit's known-bad-state history, applied to account activity). Event vectors are [sim_change, port_out, new_device, geo_jump, off_hours] intensities in 0..1. Cosine distance (the default) compares the SHAPE of an event, not its intensity; L2 compares both.
Commands: `AGENT.OUTLIER_INTERCEPT`.

### Telecom Traffic Routing — `demo-vertical-telecom-traffic-routing.py`
Telecom: dynamic traffic and bandwidth dispatch to the best-capable network node under a capacity budget. FRACTAL.AGENT.ROUTE_TASK matches each incoming request to the best-capable node under a token budget. Scope note: this is nearest-capable-node dispatch, not multi-hop rerouting around a failed node (graph routing is a different problem). Load awareness comes from routing over a key holding only the nodes that still have headroom. Vectors are [bandwidth, latency_sensitivity, loss_tolerance, region_affinity]: a request says what the session needs, a node says what it is good at.
Commands: `AGENT.ROUTE_TASK`.

## Agentic kits (composed agents)

The agentic kits chain several of the agents (and the primitives under them) into a multi-step
decision, the way larger Domain Agents are built from the Universal Agents. On
Valkey the composition is a short script over the agent commands: the
[composition guide](composition-guide.md) shows the pattern, and
[api-agency.md → Building blocks](api-agency.md#building-blocks-the-primitives-agents-compose)
lists the primitives they are built from. Each kit shows the hand-composed chain first where the
you would by hand first, then the shipped one-call agent, and check the two agree.

### Automotive / ADAS Safety Validation — `demo-vertical-adas-safety-validation.py`
ADAS / autonomous-driving safety validation: stop an edge case before the maneuver is committed. Thirty known-bad vehicle states (prior disengagements and near-misses) cluster around three failure modes. FRACTAL.AGENT.OUTLIER_INTERCEPT screens a PROPOSED state against them and holds it when it is too close to one. A fleet vehicle whose routing vector has moved away from the fleet baseline is matched to the drift signature it resembles by searching for the change (current - baseline). Every step runs with no model; the agent adds a written reason when a reasoner is configured.
Commands: `AGENT.OUTLIER_INTERCEPT`, `SEARCH`.

### AdTech / Budget Reallocation — `demo-vertical-agentic-budget-reallocation.py`
Agentic allocation: multi-source attribution and dynamic cross-channel budget reallocation. Six channels x 64 days of attributed outcomes. FRACTAL.DIM.DFA finds the channel whose response is ACCELERATING (a non-linear surge a plain average would hide). FRACTAL.OPTIMIZE.PORTFOLIO picks which 3 channels receive the scheduled budget increase, with the risk matrix inflated by the DFA exponent; the surging channel is flagged for a capped, monitored test instead. FRACTAL.AGENT.ALLOCATE is the one-call form of the optimizer step, and FRACTAL.AGENT.ROUTE_TASK dispatches each customer journey to the best-matching model under a token budget.
Commands: `DIM.DFA`, `OPTIMIZE.PORTFOLIO`, `AGENT.ALLOCATE`, `AGENT.ROUTE_TASK`.

### Customer Support — `demo-vertical-agentic-customer-support.py`
Customer support: trajectory forecasting, similar-ticket recall and diverse next replies. A customer's ticket history is a sequence of state vectors. FRACTAL.AGENT.TRAJECTORY_PREDICT searches for the known state nearest to (latest - baseline), "where is this heading". FRACTAL.AGENT.RECALL_HYBRID recalls the most similar past tickets inside one product category (a strict metadata filter, as a hash), and FRACTAL.AGENT.RECOMMEND_DIVERSE proposes a diverse set of reply macros rather than six variations of one.
Commands: `AGENT.TRAJECTORY_PREDICT`, `AGENT.RECALL_HYBRID`, `AGENT.RECOMMEND_DIVERSE`.

### FinTech — `demo-vertical-agentic-fintech-mcts.py`
Agentic fintech: portfolio rebalance and strategy planning. FRACTAL.AGENT.REBALANCE_SIBLING optimizes a portfolio and finds the historical allocation the new weights most resemble. FRACTAL.AGENT.PLAN_EXPLORE embeds a seed-state description and Scout-searches a key of strategy embeddings for a spread of distinct branches; it needs an embedding provider behind fractalsqld and is skipped without one.
Commands: `OPTIMIZE.PORTFOLIO`, `AGENT.REBALANCE_SIBLING`, `AGENT.PLAN_EXPLORE`.
Skipped here: `PLAN_EXPLORE` needs a provider behind `fractalsqld` and is skipped without one.

### Sales / Next Best Action — `demo-vertical-agentic-next-action-ranking.py`
Agentic ranking: autonomous candidate scoring and dynamic next-action routing. Thirty candidates across three segments and a twelve-item action catalog. FRACTAL.AGENT.RECALL_HYBRID recalls the most similar past candidates inside one segment (a strict metadata filter, as a hash), and FRACTAL.AGENT.RECOMMEND_DIVERSE proposes a diverse set of next actions (not four variations of "send an email"). A status question such as "how many tier-1 candidates are there?" is a key lookup.
Commands: `AGENT.RECALL_HYBRID`, `AGENT.RECOMMEND_DIVERSE`.

### DevOps / SRE — `demo-vertical-agentic-ops-devops.py`
DevOps / SRE: dispatch, loop detection and safety checks for an autonomous operator. FRACTAL.AGENT.DETECT_LOOP catches a stuck remediation loop (a period-2 toggle that the DFA exponent alone cannot see); FRACTAL.AGENT.ANOMALY_TRIAGE scores a drifting latency series; FRACTAL.AGENT.OUTLIER_INTERCEPT screens a proposed command against known-bad runbook states; FRACTAL.AGENT.ROUTE_TASK dispatches an incident to the best-matching capability. The retrieve-then-reason step (RAG_AGENT) needs an embedding provider and is skipped without one.
Commands: `AGENT.DETECT_LOOP`, `AGENT.ANOMALY_TRIAGE`, `AGENT.OUTLIER_INTERCEPT`, `AGENT.ROUTE_TASK`, `AGENT.RAG_AGENT`.
Skipped here: `RAG_AGENT` needs a provider behind `fractalsqld` and is skipped without one.

### E-commerce / Cart Recovery Offers — `demo-vertical-agentic-recall-recommendation.py`
Agentic recommendation: recall of similar past sessions and diverse next-best offers. Twenty-four candidate offers fall into four clusters and thirty abandoned sessions into three categories. Scout (FRACTAL.EXPLORE) picks offers from across the clusters around the user's profile so the follow-up tests variety rather than near-identical offers; FRACTAL.AGENT.RECALL_HYBRID recalls the most similar past sessions inside the user's category (a strict metadata filter) with their follow-up notes; and FRACTAL.AGENT.RECOMMEND_DIVERSE proposes the diverse set. A status question such as "how many abandoned watches sessions are there?" is a key lookup.
Commands: `EXPLORE`, `SEARCH`, `AGENT.RECALL_HYBRID`, `AGENT.RECOMMEND_DIVERSE`.

### Climate Tech: Grid Balancing — `demo-vertical-climatech-grid-balancing.py`
Climate tech: renewable and storage generation balancing on a distribution grid. FRACTAL.DIM.DRIFT over grid-frequency telemetry detects a stability regime change (the volatility-regime recipe from the quant-finance kit, applied to a grid): the instability is an ACCELERATING oscillation envelope, not a level shift, because DFA removes a constant offset. FRACTAL.OPTIMIZE.PORTFOLIO then reallocates dispatch across six nodes under a cardinality constraint, using a ramp-risk matrix in which correlated renewables carry shared weather risk.
Commands: `AGENT.ANOMALY_TRIAGE`, `OPTIMIZE.PORTFOLIO`, `AGENT.ALLOCATE`.

### Critical Inventory Routing — `demo-vertical-critical-inventory-routing.py`
Critical-consumables inventory and replenishment across a depot network. Eight depots report a daily-consumption series. FRACTAL.DIM.DFA flags the depots whose demand is NON-LINEAR (bursty, runaway) rather than merely high. FRACTAL.AGENT.ALLOCATE then chooses which four depots get the scheduled run, with the risk matrix inflated for the bursty ones: they fall out of the risk-adjusted run and are flagged for expedited restock instead.
Commands: `DIM.DFA`, `AGENT.ALLOCATE`.

### Automotive / EV Battery Lifecycle — `demo-vertical-ev-battery-lifecycle.py`
Electric-vehicle fleet battery lifecycle: state of health (SoH) and thermal-regime monitoring. A vehicle's intake baseline plays the patient's admission baseline: FRACTAL.AGENT. PATIENT_DETERIORATION_TRIAGE finds the fleet vehicles whose SoH profile resembles this one's and how far it has drifted from its own baseline. FRACTAL.AGENT.ANOMALY_TRIAGE watches one vehicle's temperature series for a shift into a new regime (pre-thermal-runaway), independently of the SoH comparison.
Commands: `AGENT.PATIENT_DETERIORATION_TRIAGE`, `AGENT.ANOMALY_TRIAGE`.

### Heavy-Industry Predictive Maintenance — `demo-vertical-heavy-industry-predictive-maintenance.py`
Heavy machinery and fleet predictive maintenance: marine vessels, mining equipment, powertrains. An asset plays the patient. FRACTAL.AGENT.ANOMALY_TRIAGE watches one asset's vibration series for a shift into a pre-failure regime; FRACTAL.AGENT.PATIENT_DETERIORATION_TRIAGE compares the asset's current state against its intake baseline within the same asset class (FILTER on a companion hash).
Commands: `AGENT.ANOMALY_TRIAGE`, `AGENT.PATIENT_DETERIORATION_TRIAGE`.

### Retail / Demand Replenishment — `demo-vertical-multi-series-replenishment.py`
Multi-location replenishment: stockout prevention and non-linear demand forecasting. Six locations x 64 days of unit demand. FRACTAL.DIM.DFA finds the location whose demand is ACCELERATING (a viral item or a local event) rather than merely high. FRACTAL.OPTIMIZE.PORTFOLIO picks which 3 locations get the scheduled replenishment from the central hub, with the risk matrix inflated by the DFA exponent; the surging location is flagged for an expedited pick instead. FRACTAL.AGENT.ALLOCATE is the one-call form of the optimizer.
Commands: `DIM.DFA`, `OPTIMIZE.PORTFOLIO`, `AGENT.ALLOCATE`.

### AgTech / Soil Telemetry Allocation — `demo-vertical-sensor-telemetry-allocation.py`
Resource allocation: sensor telemetry regime shifts and non-linear demand. Six sites x 64 days of resource demand from soil-moisture sensors. FRACTAL.DIM.DFA finds the site whose demand is accelerating (a non-linear trend or a failing supply line) rather than merely high. FRACTAL.OPTIMIZE.PORTFOLIO picks which 3 sites get the scheduled service run, with the risk matrix inflated by the DFA exponent; the accelerating site is flagged for an inspection instead. FRACTAL.AGENT.ALLOCATE is the one-call form of the optimizer step.
Commands: `DIM.DFA`, `OPTIMIZE.PORTFOLIO`, `AGENT.ALLOCATE`.

### Gaming / Economy Inflation & Loops — `demo-vertical-simulated-economy-drift.py`
Simulated-economy balancing: market inflation loops and automated-actor detection. Six markets x 96 days of price index; every market carries a weekly demand cycle and one (mkt-d) enters an accelerating inflation spiral. FRACTAL.SERIES.PERIODOGRAM confirms the weekly cycle, FRACTAL.DIM.DFA flags the market inflating non-linearly, and FRACTAL.OPTIMIZE.PORTFOLIO picks 3 markets for the scheduled sink program (the inflating one goes to an emergency price-cap instead). FRACTAL.AGENT.DETECT_LOOP then watches trade-state logs: an automated actor that flips between the same two item states is caught, while a varied actor is not.
Commands: `SERIES.PERIODOGRAM`, `DIM.DFA`, `OPTIMIZE.PORTFOLIO`, `AGENT.DETECT_LOOP`.

### Automotive / V2X Warranty Triage — `demo-vertical-v2x-warranty-triage.py`
Connected-vehicle (V2X) diagnostics: route a trouble-code event, recall similar warranty claims. FRACTAL.AGENT.ROUTE_TASK matches an incoming telemetry event to the best-capable repair workflow under a token budget. FRACTAL.AGENT.RECALL_HYBRID fuses a strict metadata filter (component) with drift-vector search to recall similar historical claims. A two-predicate cohort (model year AND component) is a companion hash whose value is the combined predicate.
Commands: `AGENT.ROUTE_TASK`, `AGENT.RECALL_HYBRID`.

## Valkey-native kits

Two kits have no counterpart in the SQL products, because they are what a key-value store next to
your data is for.

### Semantic cache — `demo-vertical-semantic-cache.py`
Semantic cache: answer a repeated question from Valkey instead of calling the model. Before paying for an LLM call, ask Valkey whether a SIMILAR prompt already has an answer. Prompt vectors live in one key (the corpus), answers in a Valkey list at the same row index, and FRACTAL.SEARCH finds the closest cached prompt in-process. The embedder here is a small deterministic bag-of-words hash, so this kit needs no model or daemon. It is a stand-in: in production, FRACTAL.EMBED (through fractalsqld) or any embedding model produces the vectors, and nothing else changes. A hit needs the nearest cached prompt to be within a cosine-distance threshold.
Commands: `VECTOR.NORMALIZE`, `SEARCH`.

### Agent loop detection — `demo-vertical-agent-loop-detection.py`
Agent loop detection: notice that an autonomous agent is going around in circles. Each step's state vector is reduced to a 64-bit SimHash fingerprint (FRACTAL.SERIES.FINGERPRINT), so nearly-identical states give nearly-identical fingerprints. FRACTAL.CYCLE.DETECT runs cycle detection over the fingerprint stream, tolerating a few differing bits, which an exact-hash check cannot do (floating-point noise defeats it). A healthy trace never repeats; a stuck agent revisits the same three states over and over.
Commands: `SERIES.FINGERPRINT`, `VECTOR.HAMMING`, `CYCLE.DETECT`.

## What they teach about the primitives

Writing the kits turned up behaviours worth knowing, and each kit encodes them:

- `SEARCH` ranks by **cosine distance**; a brute-force check must too.
- `EXPLORE` explores the **neighbourhood of the query**; its reach grows with the population
  (40 particles reached two clusters, 200 reached all six).
- `DIM.DRIFT` detects a change in **correlation structure or dynamics** (white noise turning into a
  random walk, or an accelerating ramp), not a level shift.
- `SERIES.CHANGEPOINT` for a variance shift wants a window of 50 or more.
- A fingerprint captures a state's **direction**: a random walk's consecutive states point the same way,
  which correctly reads as "the same state".
- `MORPH.COMPLEXITY` needs on the order of a hundred points or more (86 were refused, 120 worked), and
  `DIM.BOXCOUNT` wants a path-like trace (a line or a meander), not independent random points.
- `DETECT_LOOP` reports a loop when the DFA exponent of the state norms exceeds 0.9 **or** a fingerprint
  cycle closes, so a smooth, persistent state stream reads as looping; a healthy stream is an unpredictable one.
- `FRACTAL.DIM.DFA` refuses a constant series; the agents that call it treat that as exponent 0.

## Coming from a SQL edition

The same forty-two industry kits ship in every edition. A handful of individual steps differ on Valkey:

| Step in the SQL editions | Kits | On Valkey |
|---|---|---|
| `fractal_sql_agent`, a question in plain English answered by generated SQL | next-action-ranking, recall-recommendation, customer-support, fintech-mcts | Not provided (no SQL engine). Where the question is a count, the kit answers it with a key lookup. See [text-to-sql-setup.md](text-to-sql-setup.md). |
| `fractal_vectorizer_*`, embed a table column through a queue | fintech-mcts, scenario-planning-simulation | The same job is `FRACTAL.EMBED` with a destination key; the kits do not exercise it. See [vectorizer-setup.md](vectorizer-setup.md). |
| `fractal_agent_plan_explore`, `fractal_rag_agent` | fintech-mcts, scenario-planning-simulation, ops-devops | Provided; they embed text, so they need an embedding provider behind `fractalsqld` and are skipped without one. |
| `fractal_agent_diverse_portfolios` | periodicity-portfolio-selection | Provided (`FRACTAL.AGENT.DIVERSE_PORTFOLIOS`); it needs the enterprise tier, and the kit skips it on a Community install. |

## Other industry labels, same kits

Some industries are an existing kit under a different label. These need no script of their own; run the
kit named and read the label as the use case.

| Industry label | Run | Exercises |
|---|---|---|
| Fire / Rescue: emergency fleet routing & detours | `demo-vertical-fleet-logistics.py` | `AGENT.DETOUR_CLASSIFY`, a search on the change (`SEARCH` on current - baseline), `DIM.BOXCOUNT` |
| Fire / Rescue: 911 unit & task dispatch | `demo-vertical-telecom-traffic-routing.py` or `demo-vertical-v2x-warranty-triage.py` | `AGENT.ROUTE_TASK` |
| Forestry: environmental monitoring | `demo-vertical-smart-cities-iot.py` or `demo-vertical-climatech-risk-exposure.py` | `AGENT.NETWORK_COVERAGE_ALERT` |
| Forestry: aerial drone & ranger deployment | `demo-vertical-agentic-edge-swarm.py` or `demo-vertical-sovereign-edge-ai.py` | `AGENT.DETECT_LOOP`, `AGENT.SCHEDULE_WORKLOAD` |
| Telecom: signal coverage & topology | `demo-vertical-smart-cities-iot.py` or `demo-vertical-satellite-constellation-coverage.py` | `AGENT.NETWORK_COVERAGE_ALERT` |
| Telecom: network anomaly & beaconing detection | `demo-vertical-cybersecurity-threat-detection.py` or `demo-vertical-rf-spectrum-sensing.py` | `DIM.DFA`/`DIM.DRIFT`, `SERIES.PERIODOGRAM`, `SERIES.SPECTROGRAM` |
| Telecom: customer churn / support-ticket routing | `demo-vertical-agentic-customer-support.py` | `AGENT.RECALL_HYBRID`, `AGENT.RECOMMEND_DIVERSE`, `AGENT.TRAJECTORY_PREDICT` |
| Secure cloud / zero-egress audit trail | the enterprise ledger commands (see [enterprise.md](enterprise.md)) | the QTL ledger functions (`LEDGER.LOAD` / `.FLUSH` / `.COMPACT` / `.VERIFY`); Enterprise edition |
| High-assurance communications equipment | `demo-vertical-telecom-traffic-routing.py` or `demo-vertical-smart-cities-iot.py` | `AGENT.ROUTE_TASK` token-budget accounting, `AGENT.NETWORK_COVERAGE_ALERT` |
| Uncrewed systems: land vehicles | `demo-vertical-fleet-logistics.py` | `AGENT.DETOUR_CLASSIFY` |
| Uncrewed systems: aerial | `demo-vertical-agentic-edge-swarm.py` | `AGENT.DETECT_LOOP` |
| Climate Tech: carbon-offset / ESG portfolio construction | `demo-vertical-quant-finance.py` | `OPTIMIZE.PORTFOLIO` |
| Climate Tech: climate-driven supply-chain disruption | `demo-vertical-fleet-logistics.py` or `demo-vertical-critical-inventory-routing.py` | a search on the change (`SEARCH` on current - baseline), `DIM.BOXCOUNT`, `DIM.DFA` |

## Where next

- **"How does a specific agent work, and what are its inputs?"** → the per-agent recipes in
  [api-agency.md](api-agency.md#the-recipes).
- **"How do I build a proprietary agent that isn't in the box?"** → [composition-guide.md](composition-guide.md):
  the primitives as building blocks and worked composition patterns.
- **All the agents end to end** → the agentic kits above each run a chain of them against your server.
- **A specific industry, against a domain-shaped dataset** → `python3 demo/demo-vertical-<name>.py --port 6379`, see
  [demo/README.md](../demo/README.md) for the full list of forty-four.

