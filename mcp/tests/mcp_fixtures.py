#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0 AND BSD-2-Clause
# SPDX-FileCopyrightText: 2026 Daniel Gardiner d/b/a FractalSQLabs

"""The world the smoke client builds in Valkey, and one valid call for every tool.

setup() writes the keys (all under mcpt:) over a plain RESP connection, using only the
standard library. FIXTURE_CALLS is (tool, arguments) for every tool in the catalog;
EXPECT says what a correct server returns for each against a Valkey with the module
but NO fractalsqld daemon and no valkey-search:

    "ok"      the call succeeds
    "engine"  the call reaches the engine and is refused with a clean engine-class error
              (a daemon, the enterprise tier or valkey-search is needed)

mcp_mutate.py changes one argument of each at a time and replays them.

Zero dependencies beyond the stdlib."""

from __future__ import annotations

import math
import os
import random
import socket
import struct


class Resp:
    """A minimal blocking RESP2 client for fixture setup and for killing connections."""

    def __init__(self, host, port):
        self.s = socket.create_connection((host, int(port)), 10)
        self.buf = b""

    def close(self):
        self.s.close()

    def _line(self):
        while b"\r\n" not in self.buf:
            d = self.s.recv(65536)
            if not d:
                raise RuntimeError("connection closed")
            self.buf += d
        line, self.buf = self.buf.split(b"\r\n", 1)
        return line

    def _exact(self, n):
        while len(self.buf) < n + 2:
            d = self.s.recv(65536)
            if not d:
                raise RuntimeError("connection closed")
            self.buf += d
        out, self.buf = self.buf[:n], self.buf[n + 2:]
        return out

    def _read(self):
        line = self._line()
        t, rest = line[:1], line[1:]
        if t in (b"+", b":"):
            return rest if t == b"+" else int(rest)
        if t == b"-":
            raise RuntimeError(rest.decode("utf-8", "replace"))
        if t == b"$":
            n = int(rest)
            return None if n < 0 else self._exact(n)
        if t == b"*":
            n = int(rest)
            return None if n < 0 else [self._read() for _ in range(n)]
        raise RuntimeError("bad reply")

    def call(self, *args):
        parts = [b"*%d\r\n" % len(args)]
        for a in args:
            a = a if isinstance(a, bytes) else str(a).encode()
            parts.append(b"$%d\r\n%s\r\n" % (len(a), a))
        self.s.sendall(b"".join(parts))
        return self._read()


def pack(values):
    return struct.pack("<%df" % len(values), *values)


def rows(r):
    return b"".join(pack(x) for x in r)


KEYS = ["mcpt:corpus", "mcpt:big", "mcpt:ids", "mcpt:content", "mcpt:series", "mcpt:series2", "mcpt:pc", "mcpt:gps",
        "mcpt:log", "mcpt:alloc", "mcpt:traj", "mcpt:strat", "mcpt:cohort"]


def setup(host, port):
    rng = random.Random(20261101)
    c = Resp(host, port)
    try:
        c.call("DEL", *KEYS)
        centers = [[1, 0, 0, 0], [0, 1, 0, 0], [0, 0, 1, 0], [0, 0, 0, 1]]
        corpus = [[x + rng.gauss(0, 0.05) for x in centers[i % 4]] for i in range(40)]
        c.call("SET", "mcpt:corpus", rows(corpus))
        c.call("SET", "mcpt:big", rows([[rng.gauss(0, 1) for _ in range(16)] for _ in range(6000)]))
        for i in range(40):
            c.call("HSET", "mcpt:ids", str(i), "doc-%d" % i)
            c.call("HSET", "mcpt:content", str(i), "document number %d about topic %d" % (i, i % 4))
            c.call("HSET", "mcpt:cohort", str(i), "a" if i % 2 == 0 else "b")
        walk, x = [], 0.0
        for i in range(200):
            x += rng.gauss(0, 1) + (0.4 if i > 120 else 0.0)
            walk.append(x)
        c.call("SET", "mcpt:series", pack(walk))
        c.call("SET", "mcpt:series2", pack([math.sin(i / 3.0) + rng.gauss(0, 0.1) for i in range(160)]))
        c.call("SET", "mcpt:pc", pack([rng.random() for _ in range(800)]))          # 400 points, 2 wide
        c.call("SET", "mcpt:gps", pack([v for i in range(400) for v in (math.cos(i / 9.0) + 0.01 * i, math.sin(i / 7.0))]))
        c.call("SET", "mcpt:log", rows([[1, 0, 0], [0, 1, 0]] * 3))
        c.call("SET", "mcpt:alloc", rows([[rng.random() for _ in range(6)] for _ in range(10)]))
        c.call("SET", "mcpt:traj", rows([[i, i * 0.5, 1.0] for i in range(8)]))
        c.call("SET", "mcpt:strat", rows([[rng.gauss(0, 1) for _ in range(4)] for _ in range(20)]))
    finally:
        c.close()


_SERIES = [round(math.sin(i / 3.0) * 2 + (3.0 if i > 70 else 0.0) + (i % 5) * 0.1, 3) for i in range(120)]
_R = random.Random(11)
_PTS = [[_R.random(), _R.random()] for _ in range(400)]   # the fractal-dimension commands need a few hundred points


def _skeleton(n=300):
    """A random-walk tree: n nodes (3-D), flat node-index pairs, one arc length per edge."""
    pts = [[0.0, 0.0, 0.0]]
    for _ in range(1, n):
        pts.append([pts[-1][k] + _R.gauss(0, 1) for k in range(3)])
    edges, arcs = [], []
    for i in range(1, n):
        j = max(0, i - 1 - (1 if _R.random() < 0.2 else 0))
        edges += [j, i]
        arcs.append(math.dist(pts[j], pts[i]))
    return [[round(v, 4) for v in p] for p in pts], edges, [round(a, 4) for a in arcs]


_SK_PTS, _SK_EDGES, _SK_ARCS = _skeleton()
_Q = [1.0, 0.0, 0.0, 0.0]
_B = [0.0, 1.0, 0.0, 0.0]
_MU = [0.05, 0.08, 0.03, 0.12, 0.07, 0.01]
_COV = [[0.02 if i == j else 0.001 for j in range(6)] for i in range(6)]
_CUBE_V = [[0, 0, 0], [1, 0, 0], [1, 1, 0], [0, 1, 0], [0, 0, 1], [1, 0, 1], [1, 1, 1], [0, 1, 1]]
_CUBE_F = [0, 1, 2, 0, 2, 3, 4, 5, 6, 4, 6, 7, 0, 1, 5, 0, 5, 4, 3, 2, 6, 3, 6, 7, 0, 3, 7, 0, 7, 4, 1, 2, 6, 1, 6, 5]

FIXTURE_CALLS = [
    ("fractal_version", {}),
    ("fractal_search", {"corpus_key": "mcpt:corpus", "query": _Q, "k": 3}),
    ("fractal_search_explore", {"corpus_key": "mcpt:corpus", "query": _Q, "population_size": 16}),
    ("fractal_diversify_enable", {"session": "mcpt-s1"}),
    ("fractal_diversify_params", {"session": "mcpt-s1", "params": {"repulsion_weight": 0.8}}),
    ("fractal_explain_result", {"session": "mcpt-s1"}),
    ("fractal_feedback_report", {"session": "mcpt-s1", "result_handle": 3, "kind": "negative"}),
    ("fractal_isolate_background", {"session": "mcpt-s1", "result_handle": 5}),
    ("fractal_diversify_disable", {"session": "mcpt-s1"}),
    ("fractal_session_list", {}),
    ("fractal_session_close", {"session": "mcpt-s1"}),
    ("fractal_reason", {"query": "reply with one word"}),
    ("fractal_embed", {"input": "hello world"}),
    ("fractal_agent_anomaly_triage", {"series_key": "mcpt:series", "baseline_window": 32}),
    ("fractal_agent_allocate", {"mu": _MU, "cov": _COV, "cardinality": 2}),
    ("fractal_agent_route_task", {"capability_key": "mcpt:corpus", "task": _Q, "budget": 1000}),
    ("fractal_agent_outlier_intercept", {"history_key": "mcpt:corpus", "state": _Q, "threshold": 0.3, "metric": "l2"}),
    ("fractal_agent_recall_hybrid", {"corpus_key": "mcpt:corpus", "query": _Q, "k": 3, "ids": "mcpt:ids", "content": "mcpt:content"}),
    ("fractal_agent_recommend_diverse", {"catalog_key": "mcpt:corpus", "query": _Q, "k": 3, "ids": "mcpt:ids"}),
    ("fractal_agent_data_analyst", {"index": "mcpt_no_such_index", "question": "how many documents are there"}),
    ("fractal_agent_patient_deterioration_triage", {"corpus_key": "mcpt:corpus", "query": _Q, "baseline": _B, "current": _Q, "k": 3,
                                                    "filter": {"hash": "mcpt:cohort", "value": "a"}}),
    ("fractal_agent_feedback_audit", {"catalog_key": "mcpt:corpus", "query": _Q, "warmup_key": "mcpt:corpus", "warmup_count": 4}),
    ("fractal_agent_schedule_workload", {"node_key": "mcpt:corpus", "task": _Q, "iterations": 5, "population": 8}),
    ("fractal_agent_rebalance_sibling", {"alloc_key": "mcpt:alloc", "mu": _MU, "cov": _COV, "cardinality": 2,
                                         "baseline": [0.2, 0.2, 0.2, 0.2, 0.1, 0.1], "seed": 7}),
    ("fractal_agent_detour_classify", {"corpus_key": "mcpt:corpus", "baseline": _Q, "current": _B, "gps_trace_key": "mcpt:gps"}),
    ("fractal_agent_track_anomaly", {"corpus_key": "mcpt:corpus", "baseline": _Q, "current": _B, "heading_series_key": "mcpt:series"}),
    ("fractal_agent_network_coverage_alert", {"point_cloud_key": "mcpt:pc", "drift_series_key": "mcpt:series"}),
    ("fractal_agent_regime_triage", {"series_key": "mcpt:series", "win": 32}),
    ("fractal_agent_diverse_portfolios", {"mu": _MU, "cov": _COV, "cardinality": 2, "n_restarts": 3}),
    ("fractal_agent_plan_explore", {"strategy_key": "mcpt:strat", "state_text": "a seed state", "dim": 4, "max_branches": 3}),
    ("fractal_agent_trajectory_predict", {"corpus_key": "mcpt:traj", "dim": 3, "baseline_row": 2}),
    ("fractal_agent_detect_loop", {"agent_id": "mcpt-agent", "state_log_key": "mcpt:log", "dim": 3}),
    ("fractal_search_agent", {"corpus_key": "mcpt:corpus", "query_text": "what is topic 1", "dim": 4}),
    ("fractal_rag_agent", {"corpus_key": "mcpt:corpus", "query_text": "what is topic 1", "dim": 4, "content": "mcpt:content"}),
    ("fractal_dimension_dfa", {"series": _SERIES}),
    ("fractal_dimension_drift", {"series": _SERIES, "win": 32}),
    ("fractal_dimension_boxcount", {"points": _PTS}),
    ("fractal_periodogram", {"series": _SERIES, "max_peaks": 3}),
    ("fractal_change_point_detect", {"series": _SERIES, "window": 16, "threshold": 1.0}),
    ("fractal_state_fingerprint", {"vector": _Q, "n_bits": 64, "seed": 7}),
    ("fractal_spectrogram_entropy", {"series": _SERIES, "window": 32, "hop": 16}),
    ("fractal_cycle_detect", {"fingerprints": ["aa55", "bb66", "aa55", "bb66", "aa55", "bb66"], "hamming_threshold": 0}),
    ("fractal_tda_persistence_diagram", {"points": _PTS[:30], "max_dim": 1, "max_thresh": 0.5}),
    ("fractal_morphological_complexity", {"points": _PTS}),
    ("fractal_cortical_folding", {"vertices": _CUBE_V, "faces": _CUBE_F}),
    ("fractal_nerve_plexus_metric", {"coords": _SK_PTS, "edges": _SK_EDGES}),
    ("fractal_vascular_network", {"coords": _SK_PTS, "edges": _SK_EDGES, "arc_lengths": _SK_ARCS}),
    ("fractal_optimize_portfolio", {"mu": _MU, "cov": _COV, "k": 2, "seed": 42}),
    ("fractal_vector_lp_distance", {"a": [0, 0], "b": [3, 4], "p": 1}),
    ("fractal_vector_hamming_distance", {"a": "0f", "b": "f0"}),
    ("fractal_vector_quantize_int8", {"vector": [1, -2, 3]}),
    ("fractal_vector_quantize_binary", {"vector": [1, -2, 3, 4]}),
    ("fractal_ledger_verify", {"kind": 2}),
    ("fractal_ledger_truth_count", {}),
    ("fractal_ledger_shadow_count", {}),
    ("fractal_ledger_flush", {}),
    ("fractal_ledger_load", {}),
    ("fractal_ledger_compact", {}),
    ("fractal_ledger_reset_soft", {}),
    ("fractal_ledger_reset_hard", {}),
    ("fractal_audit_log", {"entry_type": "probe", "payload": "{\"a\": 1}"}),
    ("fractal_audit_unpack", {"blob": "00ff"}),
    ("fractal_optimize_portfolio_multimodal", {"mu": _MU, "cov": _COV, "k": 2, "n_restarts": 3, "overlap_threshold": 0.5, "quality_frac": 0.9}),
    ("fractal_optimize_portfolio_multimodal_pareto", {"mu": _MU, "cov": _COV, "k": 2, "n_restarts": 3}),
    ("fractal_store_vectors", {"key": "mcpt:scratch", "rows": [[1, 2], [3, 4]]}),
    ("fractal_store_series", {"key": "mcpt:scratch2", "values": [1, 2, 3, 4]}),
    ("fractal_store_hash", {"key": "mcpt:scratch3", "fields": {"0": "a", "1": "b"}}),
    ("fractal_key_info", {"key": "mcpt:corpus"}),
    ("fractal_delete_key", {"key": "mcpt:scratch"}),
]

# What a correct server returns for each call with the module loaded but no daemon and no valkey-search.
_ENGINE = {
    "fractal_reason", "fractal_embed", "fractal_agent_data_analyst", "fractal_agent_plan_explore", "fractal_search_agent",
    "fractal_rag_agent", "fractal_ledger_verify", "fractal_ledger_truth_count", "fractal_ledger_shadow_count",
    "fractal_ledger_flush", "fractal_ledger_load", "fractal_ledger_compact", "fractal_ledger_reset_soft", "fractal_ledger_reset_hard",
    "fractal_audit_log", "fractal_audit_unpack", "fractal_agent_diverse_portfolios",
    "fractal_optimize_portfolio_multimodal", "fractal_optimize_portfolio_multimodal_pareto",
}
EXPECT = {name: ("engine" if name in _ENGINE else "ok") for name, _ in FIXTURE_CALLS}

# Calls that change state outside the mcpt: scratch keys or latch something for the connection: the mutation
# sweep replays everything else.
RISKY = {"fractal_ledger_flush", "fractal_ledger_load", "fractal_ledger_compact", "fractal_ledger_reset_soft",
         "fractal_ledger_reset_hard"}

if __name__ == "__main__":
    setup(os.environ.get("VALKEY_HOST", "127.0.0.1"), os.environ.get("VALKEY_PORT", "6379"))
    print("fixtures written under mcpt:*; %d calls" % len(FIXTURE_CALLS))
