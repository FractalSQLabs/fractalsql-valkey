#!/usr/bin/env bash
# demo/demo-workload.sh
# SPDX-License-Identifier: Apache-2.0
# SPDX-FileCopyrightText: 2026 Daniel Gardiner d/b/a FractalSQLabs
#
# A production-shaped mixed workload against the docker-compose demo --
# concurrent simulated clients, each issuing a realistic MIX of calls (mostly
# cheap search, some analytics and agents, occasionally an expensive reasoning
# call) sustained over real wall-clock time, with p50/p95/p99 latency per
# operation type at the end.
#
# This answers a different question than demo.py does. That proves the pipeline
# is CORRECT (right answer, one call at a time). This proves it HOLDS UP --
# concurrent load, sustained duration, a realistic operation mix, not a
# single-shot demo. Neither one substitutes for the other.
#
# The operation mix (weights):
#   70  FRACTAL.SEARCH         the Sniper over a 20,000-row corpus
#   10  FRACTAL.DIM.DFA        an analytics primitive
#   10  FRACTAL.AGENT.RECALL_HYBRID   a retrieval agent, no model involved
#    5  FRACTAL.AGENT.REGIME_TRIAGE   an agent that also asks the model for its narrative
#    5  FRACTAL.REASON         a model call
# The last two run only with --reason; without it their weight goes to search, so a Valkey
# with no daemon is measured on what it can do. With --reason and no daemon configured,
# those calls fail cleanly and are counted as failures in the summary, same as any other
# error under load would be -- this script does not hide that. (REGIME_TRIAGE is slow by
# design when a daemon is attached: its latency is the model's.)
#
# Baseline hardware assumption: reasoning latency is only meaningful against the
# model you actually run. Point the daemon at it (docs/reasoning-setup.md); this
# script cannot manufacture the number "is my hardware in that class".
#
# Usage:
#   ./demo/demo-workload.sh
#   ./demo/demo-workload.sh --duration 300 --concurrency 10
#   ./demo/demo-workload.sh --reason
#   ./demo/demo-workload.sh --host H --port P
#
# Env overrides:
#   WORKLOAD_DURATION      seconds per run (default 120)
#   WORKLOAD_CONCURRENCY   concurrent simulated clients (default 5)
#   WORKLOAD_HOST / WORKLOAD_PORT   the server (default 127.0.0.1:16379 from the host, 6379 inside the
#                          demo container, where it runs as: docker compose exec valkey /demo/demo-workload.sh)
#
# Keys written: wl:* (removed again on exit). The server's own `threads` and
# `max-inflight-mb` module options bound how much of this runs at once.

set -uo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

DURATION="${WORKLOAD_DURATION:-120}"
CONCURRENCY="${WORKLOAD_CONCURRENCY:-5}"
HOST="${WORKLOAD_HOST:-127.0.0.1}"
if [[ -n "${WORKLOAD_PORT:-}" ]]; then PORT="$WORKLOAD_PORT"; elif [[ -f /.dockerenv ]]; then PORT=6379; else PORT=16379; fi
REASON=0

while [[ $# -gt 0 ]]; do
  case "$1" in
    --duration)     DURATION="$2"; shift ;;
    --concurrency)  CONCURRENCY="$2"; shift ;;
    --host)         HOST="$2"; shift ;;
    --port)         PORT="$2"; shift ;;
    --reason)       REASON=1 ;;
    -h|--help)
      sed -n '2,44p' "$0"; exit 0 ;;
    *) echo "unknown arg: $1" >&2; exit 2 ;;
  esac
  shift
done

command -v python3 >/dev/null 2>&1 || { echo "demo-workload.sh: python3 is required." >&2; exit 2; }

exec python3 - "$HERE" "$HOST" "$PORT" "$DURATION" "$CONCURRENCY" "$REASON" <<'PY'
import sys, time, random, threading, struct, collections
here, host, port, duration, conc, reason = sys.argv[1], sys.argv[2], int(sys.argv[3]), float(sys.argv[4]), int(sys.argv[5]), sys.argv[6] == "1"
sys.path.insert(0, here)
from fvlib import Conn, FvError

DIM, N = 16, 20000
def pack(v): return struct.pack("<%df" % len(v), *v)

setup = Conn(host, port)
try:
    setup.call("FRACTAL.VERSION")
except Exception as e:
    sys.exit("demo-workload.sh: no Valkey with the FractalSQL module at %s:%d (%s)" % (host, port, e))
rng = random.Random(7)
setup.call("SET", "wl:corpus", b"".join(pack([rng.gauss(0, 1) for _ in range(DIM)]) for _ in range(N)))
SERIES = pack([rng.gauss(0, 1) for _ in range(256)])
setup.call("SET", "wl:series", SERIES)

ops = [("search", 70 if reason else 80), ("dim.dfa", 10), ("agent.recall_hybrid", 10)]
if reason:
    ops += [("agent.regime_triage", 5), ("reason", 5)]
names = [o for o, w in ops for _ in range(w)]
lat = collections.defaultdict(list)
errs = collections.Counter()
lock = threading.Lock()
stop = time.time() + duration

def call(c, op, r):
    if op == "search":
        return c.call("FRACTAL.SEARCH", "wl:corpus", pack([r.gauss(0, 1) for _ in range(DIM)]), 5)
    if op == "dim.dfa":
        return c.call("FRACTAL.DIM.DFA", SERIES)
    if op == "agent.recall_hybrid":
        return c.call("FRACTAL.AGENT.RECALL_HYBRID", "wl:corpus", pack([r.gauss(0, 1) for _ in range(DIM)]), "K", 5)
    if op == "agent.regime_triage":
        return c.call("FRACTAL.AGENT.REGIME_TRIAGE", "wl:series", 64)
    return c.call("FRACTAL.REASON", "reply with one word: ok")

def worker(i):
    r = random.Random(1000 + i)
    c = Conn(host, port)
    while time.time() < stop:
        op = r.choice(names)
        t = time.perf_counter()
        try:
            call(c, op, r)
            ok = True
        except FvError as e:
            ok = False
            msg = str(e)[:60]
        except Exception as e:
            ok = False
            msg = "%s" % type(e).__name__
            c = Conn(host, port)
        dt = (time.perf_counter() - t) * 1000.0
        with lock:
            if ok: lat[op].append(dt)
            else: errs[(op, msg)] += 1

print("demo-workload: %d clients for %ds against %s:%d (%s)" % (conc, duration, host, port, "with reasoning" if reason else "no reasoning calls"))
ts = [threading.Thread(target=worker, args=(i,), daemon=True) for i in range(conc)]
t0 = time.time()
for t in ts: t.start()
for t in ts: t.join()
wall = time.time() - t0

def pct(v, p):
    v = sorted(v); return v[min(len(v) - 1, int(round(p / 100.0 * (len(v) - 1))))]
total_ok = sum(len(v) for v in lat.values())
print("\n%-22s %7s %9s %9s %9s %9s" % ("operation", "ok", "p50 ms", "p95 ms", "p99 ms", "max ms"))
for op, _ in ops:
    v = lat.get(op)
    if v: print("%-22s %7d %9.1f %9.1f %9.1f %9.1f" % (op, len(v), pct(v, 50), pct(v, 95), pct(v, 99), max(v)))
nerr = sum(errs.values())
print("\n%d ok, %d failed, %.1f ops/s over %.0fs" % (total_ok, nerr, total_ok / wall, wall))
for (op, msg), n in errs.most_common(5):
    print("  failed x%d  %s: %s" % (n, op, msg))
for k in ("wl:corpus", "wl:series"):
    setup.call("DEL", k)
sys.exit(1 if nerr else 0)
PY
