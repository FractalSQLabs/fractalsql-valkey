#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0 AND BSD-2-Clause
# SPDX-FileCopyrightText: 2026 Daniel Gardiner d/b/a FractalSQLabs

"""End-to-end MCP session against a live Valkey that has the FractalSQL module loaded.

It builds its own world (mcp_fixtures.setup), starts the server over stdio, and checks
the protocol (handshake, tool catalog, annotations), every tool once with a valid call
(and what that call must return with NO daemon and NO valkey-search behind the module),
the numbers a few tools must produce, the error taxonomy, resources, prompts, the
key-prefix and read-only guards, $/cancel and shutdown. It finishes by running the
robustness sweep and the mutation sweep against fresh servers.

  MCP_SERVER_EXE   the server binary just built      (required)
  MCP_SERVER_CWD   directory to run it from          (default: the binary's)
  MCP_HERE         the directory holding the built probes (optional)
  VALKEY_HOST / VALKEY_PORT   the Valkey to use      (default 127.0.0.1:6379)

Zero dependencies beyond the stdlib."""

from __future__ import annotations

import json
import math
import os
import sys
import threading
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import mcp_fixtures as fx          # noqa: E402
import mcp_robustness as rb        # noqa: E402
import mcp_mutate as mm            # noqa: E402

FAILS = 0
LAST_TOOL_COUNT = 0


def check(label, cond, detail=""):
    global FAILS
    print(("PASS " if cond else "FAIL ") + label + ((" -- " + str(detail)[:200]) if (not cond and detail != "") else ""))
    if not cond:
        FAILS += 1
    return bool(cond)


def tool(s, name, args, timeout=120):
    """Returns (isError, structuredContent) for one tools/call."""
    r = s.call("tools/call", {"name": name, "arguments": args}, timeout=timeout)
    if "error" in r:
        return True, {"error_class": "protocol", "message": json.dumps(r["error"])}
    res = r["result"]
    return bool(res.get("isError")), res.get("structuredContent")


def klass(sc):
    return (sc or {}).get("error_class") if isinstance(sc, dict) else None


def main():
    exe = os.environ["MCP_SERVER_EXE"]
    cwd = os.environ.get("MCP_SERVER_CWD") or os.path.dirname(os.path.abspath(exe)) or "."
    host = os.environ.get("VALKEY_HOST", "127.0.0.1")
    port = os.environ.get("VALKEY_PORT", "6379")
    fx.setup(host, port)

    s = rb._Session(exe, cwd, {})
    # ---------------------------------------------------------------- protocol
    r = s.call("initialize", {"protocolVersion": "2025-03-26", "capabilities": {}, "clientInfo": {"name": "smoke", "version": "0"}})
    res = r.get("result", {})
    check("initialize handshake echoes the requested protocolVersion", res.get("protocolVersion") == "2025-03-26", res)
    check("serverInfo names fractalsql-valkey-mcp", (res.get("serverInfo") or {}).get("name") == "fractalsql-valkey-mcp")
    s.send("notifications/initialized", {}, notify=True)
    check("ping liveness probe responds with an empty {}", s.call("ping", {}).get("result") == {})

    tools = s.call("tools/list", {})["result"]["tools"]
    names = [t["name"] for t in tools]
    want = [n for n, _ in fx.FIXTURE_CALLS]
    check("tools/list: %d tools, every name unique" % len(tools), len(names) == len(set(names)) and len(names) == len(want), (len(names), len(want)))
    check("tools/list: the catalog and the fixtures agree (a new tool needs a fixture)", set(names) == set(want),
          (sorted(set(names) - set(want)), sorted(set(want) - set(names))))
    by = {t["name"]: t for t in tools}

    bad = [t["name"] for t in tools if not (t.get("description") and t["inputSchema"].get("type") == "object"
                                            and t["inputSchema"].get("additionalProperties") is False)]
    check("accessibility: every tool has a description and a closed object inputSchema", not bad, bad[:5])
    noprop = [t["name"] for t in tools for p, v in t["inputSchema"].get("properties", {}).items() if not v.get("description")]
    check("accessibility: every parameter has a description", not noprop, noprop[:5])
    zero = [t["name"] for t in tools if not t["inputSchema"].get("properties") and not t["inputSchema"].get("x-fractalsql-no-arguments")]
    check("accessibility: zero-argument tools say so", not zero, zero[:5])

    hints = ("readOnlyHint", "destructiveHint", "idempotentHint", "openWorldHint")
    miss = [t["name"] for t in tools if not all(isinstance(t.get("annotations", {}).get(h), bool) for h in hints)]
    check("every tool sends all four hints explicitly", not miss, miss[:5])
    ann = {t["name"]: t["annotations"] for t in tools}
    destructive = {n for n, a in ann.items() if a["destructiveHint"]}
    want_destructive = {"fractal_ledger_flush", "fractal_ledger_load", "fractal_ledger_compact", "fractal_ledger_reset_soft",
                        "fractal_ledger_reset_hard", "fractal_session_close", "fractal_store_vectors", "fractal_store_series",
                        "fractal_store_hash", "fractal_delete_key",
                        # State-mutating tools (the read-only gate keys off this hint): the audit chain, the
                        # diversify/feedback state, and the agents that append a
                        # decision record to the audit chain.
                        "fractal_audit_log", "fractal_diversify_disable", "fractal_diversify_enable",
                        "fractal_diversify_params", "fractal_feedback_report", "fractal_isolate_background",
                        "fractal_agent_data_analyst", "fractal_agent_feedback_audit",
                        "fractal_agent_recommend_diverse"}
    check("destructiveHint marks exactly the tools that erase or overwrite", destructive == want_destructive,
          (sorted(destructive - want_destructive), sorted(want_destructive - destructive)))
    check("every read-only tool is idempotent", all(a["idempotentHint"] for a in ann.values() if a["readOnlyHint"]))
    check("the diversify pair is idempotent without being read-only",
          all(ann[n]["idempotentHint"] and not ann[n]["readOnlyHint"] for n in ("fractal_diversify_enable", "fractal_diversify_disable")))
    check("no destructive tool is read-only", not [n for n in destructive if ann[n]["readOnlyHint"]])
    check("openWorldHint marks the model-backed tools (fractal_reason, fractal_embed, the reasoning agents)",
          ann["fractal_reason"]["openWorldHint"] and ann["fractal_embed"]["openWorldHint"] and ann["fractal_agent_allocate"]["openWorldHint"]
          and not ann["fractal_agent_recall_hybrid"]["openWorldHint"] and not ann["fractal_vector_lp_distance"]["openWorldHint"])
    check("tool classes are cheap or expensive", all(t.get("_meta", {}).get("x-fractalsql-class", "cheap") in ("cheap", "expensive") for t in tools))

    # ---------------------------------------------------------------- every tool once
    recorded = []
    outcomes = {}
    for name, args in fx.FIXTURE_CALLS:
        err, sc = tool(s, name, args)
        recorded.append((name, args))
        outcomes[name] = (err, sc)
        exp = fx.EXPECT[name]
        if exp == "ok":
            check("tool %s: a valid call succeeds" % name, not err, sc)
        else:
            check("tool %s: refused cleanly (engine class) with nothing behind the module" % name, err and klass(sc) == "engine", sc)
    # tools that must NOT have taken the server down
    check("the server is still answering after the sweep", s.call("ping", {}).get("result") == {} and s.alive())

    # ---------------------------------------------------------------- numbers
    def ok(name, args):
        e, sc = tool(s, name, args)
        return None if e else sc

    sc = ok("fractal_search", {"corpus_key": "mcpt:corpus", "query": [1, 0.02, 0, 0], "k": 3})
    check("search over a stored corpus finds a row of the matching cluster first", bool(sc) and sc["top_k"][0]["idx"] % 4 == 0 and len(sc["top_k"]) == 3, sc)
    inline = [[1, 0], [0, 1], [0.7, 0.7], [0.1, 0.9]]
    sc = ok("fractal_search", {"corpus": inline, "query": [0.05, 1.0], "k": 2})
    check("search over an INLINE corpus ranks the nearest row first", bool(sc) and sc["top_k"][0]["idx"] in (1, 3), sc)
    e, sc = tool(s, "fractal_search", {"corpus": inline, "corpus_key": "mcpt:corpus", "query": [1, 0], "k": 1})
    check("search with both corpus and corpus_key is refused (schema)", e and klass(sc) == "schema", sc)
    e, sc = tool(s, "fractal_search", {"query": [1, 0], "k": 1})
    check("search with neither corpus nor corpus_key is refused (schema)", e and klass(sc) == "schema", sc)
    e, sc = tool(s, "fractal_search", {"corpus": [[1, 0], [0, 1, 2]], "query": [1, 0]})
    check("search with rows of different widths is refused (schema)", e and klass(sc) == "schema", sc)

    check("lp_distance p=1 is 7", abs((ok("fractal_vector_lp_distance", {"a": [0, 0], "b": [3, 4], "p": 1}) or {}).get("value", 0) - 7) < 1e-4)
    check("hamming_distance of 0f and f0 is 8", (ok("fractal_vector_hamming_distance", {"a": "0f", "b": "f0"}) or {}).get("value") == 8)
    sc = ok("fractal_vector_quantize_int8", {"vector": [1, -2, 3]})
    check("quantize_int8 returns a scale and three codes", bool(sc) and len(sc.get("codes", [])) == 3 and sc.get("scale", 0) > 0, sc)
    sc = ok("fractal_vector_quantize_binary", {"vector": [1, -2, 3, 4, -1, -1, 1, 1]})
    check("quantize_binary packs 8 dimensions into 1 byte", bool(sc) and sc.get("bytes") == 1 and len(sc.get("hex", "")) == 2, sc)
    fp1 = ok("fractal_state_fingerprint", {"vector": [1, 0, 0, 0], "n_bits": 64, "seed": 7})
    fp2 = ok("fractal_state_fingerprint", {"vector": [1, 0, 0, 0], "n_bits": 64, "seed": 7})
    check("state_fingerprint 64 bits is 8 bytes and deterministic", bool(fp1) and fp1["bytes"] == 8 and fp1 == fp2, fp1)
    sine = [math.sin(2 * math.pi * i / 8.0) for i in range(128)]
    sc = ok("fractal_periodogram", {"series": sine, "max_peaks": 2})
    check("periodogram finds the true 0.125 frequency", bool(sc) and abs(sc["freqs"][0] - 0.125) < 0.01, sc)
    sc = ok("fractal_cycle_detect", {"fingerprints": ["aa55", "bb66", "aa55", "bb66", "aa55", "bb66"], "hamming_threshold": 0})
    check("cycle_detect closes the period-2 stream", bool(sc) and sc.get("detected") is True and sc.get("cycle_len") == 2, sc)
    sc = ok("fractal_dimension_dfa", {"series": [math.sin(i / 3.0) + 0.01 * (i % 7) for i in range(200)]})
    check("dimension_dfa returns a finite exponent", bool(sc) and math.isfinite(sc["value"]) and 0 < sc["value"] < 4, sc)
    sc = ok("fractal_change_point_detect", {"series": [0.0] * 60 + [5.0] * 60, "window": 10, "threshold": 1.0})
    check("change_point_detect flags a boundary near the step", bool(sc) and any(50 <= i <= 70 for i in sc.get("indices", [])), sc)
    sc = ok("fractal_optimize_portfolio", {"mu": [0.05, 0.08, 0.03, 0.12, 0.07, 0.01], "cov": [[0.02 if i == j else 0.0 for j in range(6)] for i in range(6)], "k": 2, "seed": 3})
    check("optimize_portfolio honours the cardinality (at most 2 nonzero weights summing to 1)",
          bool(sc) and sum(1 for w in sc["weights"] if w > 1e-9) <= 2 and abs(sum(sc["weights"]) - 1) < 1e-4, sc)
    sc = ok("fractal_agent_outlier_intercept", {"history_key": "mcpt:corpus", "state": [1, 0, 0, 0], "threshold": 0.5})
    check("outlier_intercept intercepts a state that is in the history", bool(sc) and sc.get("intercepted") is True, sc)
    sc = ok("fractal_agent_recall_hybrid", {"corpus_key": "mcpt:corpus", "query": [1, 0, 0, 0], "k": 3, "content": "mcpt:content", "ids": "mcpt:ids"})
    check("recall_hybrid returns the real ids and content", bool(sc) and "items" in sc and "doc-" in json.dumps(sc) and "topic" in json.dumps(sc), sc)
    sc = ok("fractal_agent_recall_hybrid", {"corpus_key": "mcpt:corpus", "query": [1, 0, 0, 0], "k": 20, "ids": "mcpt:ids",
                                            "filter": {"hash": "mcpt:cohort", "value": "b"}})
    check("recall_hybrid's FILTER restricts the cohort (cohort b is the odd rows)",
          bool(sc) and len(sc["items"]) > 0 and all(int(str(x["mem_id"]).split("-")[-1]) % 2 == 1 for x in sc["items"]), sc)
    sc = ok("fractal_agent_detect_loop", {"agent_id": "a", "state_log_key": "mcpt:log", "dim": 3})
    check("detect_loop closes a cycle on the A,B,A,B log", bool(sc) and json.dumps(sc).count("true") >= 1, sc)

    # the sessions
    ok("fractal_diversify_enable", {"session": "smoke-sess"})
    ok("fractal_search", {"corpus_key": "mcpt:corpus", "query": [1, 0, 0, 0], "k": 5, "session": "smoke-sess"})
    ok("fractal_feedback_report", {"session": "smoke-sess", "result_handle": 0, "kind": "negative"})
    ok("fractal_search", {"corpus_key": "mcpt:corpus", "query": [1, 0, 0, 0], "k": 5, "session": "smoke-sess"})
    sc = ok("fractal_session_list", {})
    check("session_list names the session", bool(sc) and "smoke-sess" in json.dumps(sc), sc)
    ok("fractal_session_close", {"session": "smoke-sess"})
    sc = ok("fractal_session_list", {})
    check("session_close removes it", bool(sc) and "smoke-sess" not in json.dumps(sc), sc)

    # the keyspace helpers
    sc = ok("fractal_store_vectors", {"key": "mcpt:rt", "rows": [[1, 2, 3], [4, 5, 6]]})
    check("store_vectors reports 2 rows of dim 3", bool(sc) and sc["rows"] == 2 and sc["dim"] == 3 and sc["bytes"] == 24, sc)
    sc = ok("fractal_key_info", {"key": "mcpt:rt"})
    check("key_info sees 6 float32 values", bool(sc) and sc["float32_values"] == 6 and sc["type"] == "string", sc)
    e, sc = tool(s, "fractal_store_vectors", {"key": "mcpt:rt", "rows": [[9, 9]], "overwrite": False})
    check("overwrite:false refuses an existing key (engine)", e and klass(sc) == "engine", sc)
    check("delete_key reports 1 deleted", (ok("fractal_delete_key", {"key": "mcpt:rt"}) or {}).get("deleted") == 1)
    check("key_info on a missing key says type none", (ok("fractal_key_info", {"key": "mcpt:rt"}) or {}).get("type") == "none")
    ok("fractal_delete_key", {"key": "mcpt:scratch2"})
    ok("fractal_delete_key", {"key": "mcpt:scratch3"})

    # ---------------------------------------------------------------- the taxonomy
    e, sc = tool(s, "fractal_vector_lp_distance", {"a": "nope", "b": [1], "p": 1})
    check("a wrong-typed argument is a schema error", e and klass(sc) == "schema", sc)
    e, sc = tool(s, "fractal_vector_lp_distance", {"a": [1, 2], "p": 1})
    check("a missing required argument is a schema error", e and klass(sc) == "schema" and "missing required" in sc["message"], sc)
    e, sc = tool(s, "fractal_vector_lp_distance", {"a": [1, "x"], "b": [1, 2], "p": 1})
    check("a non-numeric element is a schema error naming it", e and klass(sc) == "schema" and "non-numeric" in sc["message"], sc)
    e, sc = tool(s, "fractal_vector_lp_distance", {"a": [], "b": [], "p": 1})
    check("an empty vector is a schema error", e and klass(sc) == "schema", sc)
    e, sc = tool(s, "fractal_vector_lp_distance", {"a": [1, 2], "b": [1, 2, 3], "p": 1})
    check("mismatched vector lengths come back from the engine, not as a crash", e and klass(sc) in ("engine", "schema"), sc)
    e, sc = tool(s, "fractal_agent_outlier_intercept", {"history_key": "mcpt:corpus", "state": [1, 0, 0, 0], "threshold": 0.5, "metric": "manhattan"})
    check("an enum value outside its list is a schema error naming the choices", e and klass(sc) == "schema" and "cosine" in sc["message"], sc)
    e, sc = tool(s, "fractal_vector_hamming_distance", {"a": "zz", "b": "00"})
    check("invalid hex is a schema error", e and klass(sc) == "schema", sc)
    e, sc = tool(s, "fractal_dimension_boxcount", {"points": [0.1, 0.2, 0.3, 0.4]})
    check("a flat array where the width must be derived is a schema error", e and klass(sc) == "schema", sc)
    e, sc = tool(s, "fractal_agent_regime_triage", {"series_key": "mcpt:no-such-key"})
    check("a missing key is an engine error carrying the module's own text", e and klass(sc) == "engine" and "key" in sc["message"].lower(), sc)
    e, sc = tool(s, "fractal_nope", {})
    check("an unknown tool is refused naming tools/list", e and "tools/list" in json.dumps(sc), sc)
    r = s.call("no/such/method", {})
    check("an unknown method returns -32601", (r.get("error") or {}).get("code") == -32601, r)
    e, sc = tool(s, "fractal_key_info", {"key": ""})
    check("an empty key is a schema error", e and klass(sc) == "schema", sc)

    # ---------------------------------------------------------------- resources and prompts
    rl = s.call("resources/list", {})["result"]["resources"]
    check("resources/list: agents and config", {x["uri"] for x in rl} == {"fractalsql://agents", "fractalsql://config"}, rl)
    rt = s.call("resources/templates/list", {})["result"]["resourceTemplates"]
    check("resource templates: ledger and key", {x["uriTemplate"] for x in rt} == {"fractalsql://ledger/{kind}", "fractalsql://key/{key}"}, rt)
    txt = s.call("resources/read", {"uri": "fractalsql://agents"})["result"]["contents"][0]["text"]
    check("fractalsql://agents is a catalog of the sixteen recipes", len(json.loads(txt)["recipes"]) == 16)
    cfg = json.loads(s.call("resources/read", {"uri": "fractalsql://config"})["result"]["contents"][0]["text"])
    check("fractalsql://config reports the module version and that no daemon is attached",
          cfg.get("module_version") == "2.0.2" and cfg.get("daemon_attached") is False and "password" not in json.dumps(cfg).lower(), cfg)
    kinfo = json.loads(s.call("resources/read", {"uri": "fractalsql://key/mcpt%3Acorpus"})["result"]["contents"][0]["text"])
    check("fractalsql://key/{key} describes a corpus (160 float32 values)", kinfo.get("float32_values") == 160, kinfo)
    check("an unknown resource is an error", "error" in s.call("resources/read", {"uri": "fractalsql://nope"}))
    check("a bad ledger kind is an error", "error" in s.call("resources/read", {"uri": "fractalsql://ledger/9"}))
    pl = s.call("prompts/list", {})["result"]["prompts"]
    check("prompts/list: the ten prompts", len(pl) == 10, len(pl))
    pg = s.call("prompts/get", {"name": "incident_triage", "arguments": {"series_key": "mcpt:series"}})["result"]
    check("prompts/get fills its arguments", "mcpt:series" in pg["messages"][0]["content"]["text"], pg)
    check("prompts/get without a required argument is an error", "error" in s.call("prompts/get", {"name": "incident_triage", "arguments": {}}))
    pg = s.call("prompts/get", {"name": "ciso_ledger_check", "arguments": {"kind": "{kind}"}})
    check("a prompt argument that contains its own placeholder is substituted once, not looped on", "result" in pg)

    # ---------------------------------------------------------------- $/cancel
    sid = s.send("tools/call", {"name": "fractal_search_explore", "arguments": {"corpus_key": "mcpt:big", "query": [0.1] * 16, "population_size": 4000}})
    time.sleep(0.15)
    s.send("notifications/cancelled", {"requestId": sid, "reason": "smoke"}, notify=True)
    got = None
    try:
        got = s.recv(sid, timeout=60)
    except RuntimeError:
        got = None
    delivered_real = bool(got) and "result" in got and not got["result"].get("isError") and "top_k" in json.dumps(got["result"])
    check("$/cancel: the server stays responsive", s.call("ping", {}, timeout=60).get("result") == {} and s.alive())
    check("$/cancel: the cancelled call's real result is never delivered (it ends cancelled, or it had already finished)",
          not delivered_real or got is not None)
    e, sc = tool(s, "fractal_version", {})
    check("a call after a cancel works (the connection came back)", not e and sc.get("module_version") == "2.0.2", sc)

    rc = s.close()
    check("the stdio server exits cleanly (rc 0) when its input ends", rc == 0, rc)

    # ---------------------------------------------------------------- the guards
    s2 = rb._Session(exe, cwd, {"FSQL_MCP_KEY_PREFIX": "mcpt:"})
    rb._init(s2)
    e, sc = tool(s2, "fractal_store_vectors", {"key": "elsewhere:x", "rows": [[1, 2]]})
    check("FSQL_MCP_KEY_PREFIX: a write outside the prefix is refused (schema), naming the prefix", e and klass(sc) == "schema" and "mcpt:" in sc["message"], sc)
    e, sc = tool(s2, "fractal_store_vectors", {"key": "mcpt:inside", "rows": [[1, 2]]})
    check("FSQL_MCP_KEY_PREFIX: a write inside it works", not e, sc)
    e, sc = tool(s2, "fractal_delete_key", {"key": "elsewhere:x"})
    check("FSQL_MCP_KEY_PREFIX: a delete outside it is refused", e and klass(sc) == "schema", sc)
    tool(s2, "fractal_delete_key", {"key": "mcpt:inside"})
    s2.close()

    s3 = rb._Session(exe, cwd, {"FSQL_MCP_READ_ONLY": "true"})
    rb._init(s3)
    e, sc = tool(s3, "fractal_delete_key", {"key": "mcpt:corpus"})
    check("read-only mode refuses a destructive tool before it reaches the engine", e and "read-only" in json.dumps(sc), sc)
    e, sc = tool(s3, "fractal_vector_lp_distance", {"a": [0, 0], "b": [3, 4], "p": 1})
    check("read-only mode still serves a read", not e and abs(sc["value"] - 7) < 1e-4, sc)
    e, sc = tool(s3, "fractal_ledger_reset_hard", {})
    check("read-only mode refuses the ledger reset", e and "read-only" in json.dumps(sc), sc)
    s3.close()

    # the committed locks, and the narrowing options (no database is touched by these)
    import subprocess, tempfile
    here = os.environ.get("MCP_HERE") or os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    tmp = tempfile.mkdtemp(prefix="fsqlmcp-locks-")
    env = dict(os.environ, FSQL_MCP_SCHEMA_LOCK=os.path.join(tmp, "schema"), FSQL_MCP_SAFETY_LOCK=os.path.join(tmp, "safety"),
               FSQL_MCP_MANIFEST=os.path.join(tmp, "manifest.json"))
    for flag in ("--write-schema-lock", "--write-safety-annotations-lock", "--write-manifest"):
        subprocess.run([exe, flag], env=env, cwd=cwd, capture_output=True, timeout=60)
    def same(a, b):
        try:
            return open(a).read() == open(b).read()
        except OSError:
            return False
    check("phase0: the committed .mcp-schema-lock matches the registered tool set", same(os.path.join(here, ".mcp-schema-lock"), env["FSQL_MCP_SCHEMA_LOCK"]))
    check("phase0: the safety annotations match the registered surface", same(os.path.join(here, ".mcp-safety-annotations-lock"), env["FSQL_MCP_SAFETY_LOCK"]))
    check("phase0: manifest.json matches the registered surface", same(os.path.join(here, "manifest.json"), env["FSQL_MCP_MANIFEST"]))
    lt = subprocess.run([exe, "--list-tools", "--tool", "fractal_vector_lp_distance,fractal_vector_hamming_distance"], env=dict(os.environ), cwd=cwd, capture_output=True, text=True, timeout=60)
    check("--tool narrows the catalog to the named tools", sorted(l.split()[0] for l in lt.stdout.splitlines()) == ["fractal_vector_hamming_distance", "fractal_vector_lp_distance"], lt.stdout)
    lc = subprocess.run([exe, "--list-tools", "--category", "keyspace"], env=dict(os.environ), cwd=cwd, capture_output=True, text=True, timeout=60)
    check("--category narrows the catalog to that category (the five keyspace tools)", len(lc.stdout.splitlines()) == 5, lc.stdout)

    # ---------------------------------------------------------------- the sweeps
    global LAST_TOOL_COUNT
    LAST_TOOL_COUNT = len(tools)
    os.environ.setdefault("MCP_HERE", os.environ.get("MCP_HERE", ""))
    if os.environ.get("MCP_SMOKE_SKIP_SWEEPS"):
        # Set by build_test.sh --tsan / --coverage: the two sweeps are tens of thousands of calls, 10-20x slower
        # under ThreadSanitizer. They run in full in the normal build; this leg is about the server's threading.
        print("SKIP robustness and mutation sweeps (MCP_SMOKE_SKIP_SWEEPS set)")
    else:
        rb.run(check)
        mm.run(check, recorded)

    c = fx.Resp(host, port)
    try:
        c.call("DEL", *(fx.KEYS + ["mcpt:scratch", "mcpt:scratch2", "mcpt:scratch3", "mcpt:inside", "mcpt:rt"]))
    finally:
        c.close()

    print("mcp-smoke-client: %s; %d failure(s)" % ("ALL GREEN" if not FAILS else "FAILED", FAILS))
    return 1 if FAILS else 0


if __name__ == "__main__":
    sys.exit(main())
