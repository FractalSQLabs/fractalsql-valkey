#!/usr/bin/env bash
# demo/run_demo.sh
# SPDX-License-Identifier: Apache-2.0
# SPDX-FileCopyrightText: 2026 Daniel Gardiner d/b/a FractalSQLabs
#
# Runs every demo/*.py content demo against the Docker demo's Valkey, in one
# pass -- the "docker compose up -d; run_demo.sh" button for a from-scratch look
# at everything in demo/, instead of one `python3 demo/<file>.py` per file.
#
# Usage (either shape works):
#   ./demo/run_demo.sh            from the host, next to docker-compose.yml
#                                 (runs inside the compose "valkey" service)
#   docker compose exec valkey /demo/run_demo.sh
#                                 inside the demo container itself, matching
#                                 docs/docker-demo.md's own invocation style
#   ./demo/run_demo.sh --quiet
#   ./demo/run_demo.sh --only demo-vertical-fleet-logistics.py
#   ./demo/run_demo.sh --skip demo-agents.py,enterprise-qtl-audit.py
#   ./demo/run_demo.sh --include-response-modes
#   ./demo/run_demo.sh --host H --port P        another server
#
# Scope: the demos are Python 3 clients (standard library only) that speak RESP
# to a Valkey with the FractalSQL module loaded. The demo image carries python3
# and /demo, so the host shape just execs into it. A bare install works too:
# pass --host/--port and the demos run on this machine against that server.
#
# Env overrides:
#   FSQL_DEMO_CONTAINER     compose service to exec into (default: valkey)
#   FSQL_DEMO_HOST          server host (default: 127.0.0.1)
#   FSQL_DEMO_PORT          server port (default: 6379 inside the container,
#                           16379 -- the published port -- from the host)
#   FSQL_DEMO_RUN_LOG_DIR   per-file log directory
#                           (default: /tmp/fractalsql_demo_run)
#   FSQL_DEMO_RUN_TIMEOUT   per-file timeout in seconds (default: 300)
#
# Reasoning-dependent sections need the fractalsqld daemon behind the module and
# a model it can reach (docs/docker-demo.md, docs/reasoning-setup.md). Without
# them those sections report "skipped: needs a provider" and the file still
# exits 0 -- an unconfigured provider is a state, not a failure. Anything else
# that goes wrong makes the file exit non-zero, and this script counts it.
#
# A non-zero exit from one file is a "go read its log" signal, not proof of a
# bug. The enterprise-tier demo (enterprise-qtl-audit.py) exits 0 and says so
# when the enterprise library is not loaded.
#
# demo/response-modes.py is SKIPPED BY DEFAULT: FSQL_REASONING_HTTP_RESPONSE_MODE
# is read from the DAEMON's environment when it starts, so one file cannot show
# all three modes in a single pass. Pass --include-response-modes to run it
# against whatever mode the daemon is in; it tells you which one that is from
# the shape of the reply. To see another mode, restart fractalsqld with the
# variable set (text is the default; code and json are the others).
#
# demo/demo-workload.sh is NOT part of this: that is a separate concurrent
# load tool (p50/p95/p99 latency under sustained load), not a content demo --
# see its own header.

set -uo pipefail
ORIG_ARGS=("$@")

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

SERVICE="${FSQL_DEMO_CONTAINER:-valkey}"
HOST="${FSQL_DEMO_HOST:-127.0.0.1}"
if [[ -n "${FSQL_DEMO_PORT:-}" ]]; then PORT="$FSQL_DEMO_PORT"
elif [[ -f /.dockerenv ]]; then PORT=6379
else PORT=16379; fi
EXPLICIT_SERVER=0
[[ -n "${FSQL_DEMO_HOST:-}${FSQL_DEMO_PORT:-}" ]] && EXPLICIT_SERVER=1
LOG_DIR="${FSQL_DEMO_RUN_LOG_DIR:-/tmp/fractalsql_demo_run}"
TIMEOUT_SECS="${FSQL_DEMO_RUN_TIMEOUT:-300}"

QUIET=0
ONLY=""
SKIP=""
INCLUDE_RESPONSE_MODES=0

while [[ $# -gt 0 ]]; do
  case "$1" in
    --quiet)                  QUIET=1 ;;
    --only)                   ONLY="$2"; shift ;;
    --skip)                   SKIP="$2"; shift ;;
    --host)                   HOST="$2"; EXPLICIT_SERVER=1; shift ;;
    --port)                   PORT="$2"; EXPLICIT_SERVER=1; shift ;;
    --include-response-modes) INCLUDE_RESPONSE_MODES=1 ;;
    -h|--help)
      sed -n '2,52p' "$0"; exit 0 ;;
    *) echo "run_demo.sh: unknown arg: $1 (try --help)" >&2; exit 2 ;;
  esac
  shift
done

# Host shape: with a compose project next to this script and no explicit server, run the same
# script inside the demo container, where /demo, python3 and the server all live.
if [[ ! -f /.dockerenv && "$EXPLICIT_SERVER" -eq 0 && -f "$HERE/../docker-compose.yml" ]] \
   && command -v docker >/dev/null 2>&1 \
   && docker compose -f "$HERE/../docker-compose.yml" exec -T "$SERVICE" true >/dev/null 2>&1; then
  exec docker compose -f "$HERE/../docker-compose.yml" exec -T "$SERVICE" /demo/run_demo.sh "${ORIG_ARGS[@]}"
fi

command -v python3 >/dev/null 2>&1 || { echo "run_demo.sh: python3 is required." >&2; exit 2; }

# Sanity before the loop: is a Valkey with the module actually answering?
if ! python3 - "$HOST" "$PORT" <<'PY' >/dev/null 2>&1
import socket, sys
s = socket.create_connection((sys.argv[1], int(sys.argv[2])), 3)
cmd = b"FRACTAL.VERSION"
s.sendall(b"*1\r\n$%d\r\n%s\r\n" % (len(cmd), cmd))
r = s.recv(256)
sys.exit(0 if r.startswith((b"+", b"$")) else 1)
PY
then
  echo "run_demo.sh: no Valkey with the FractalSQL module answering at $HOST:$PORT." >&2
  echo "docker compose up -d first (see docs/docker-demo.md), or pass --host/--port." >&2
  exit 2
fi

mkdir -p "$LOG_DIR"

# response-modes.py is handled separately below, never through the main loop
# (see this script's own header for why).
ALWAYS_SKIP="response-modes.py"

IFS=',' read -r -a only_list <<< "$ONLY"
IFS=',' read -r -a skip_list <<< "$SKIP"

should_run() {
  local base="$1"
  [[ "$base" == "$ALWAYS_SKIP" ]] && return 1
  if [[ -n "$ONLY" ]]; then
    for o in "${only_list[@]}"; do [[ "$base" == "$o" ]] && return 0; done
    return 1
  fi
  for s in "${skip_list[@]}"; do [[ -n "$s" && "$base" == "$s" ]] && return 1; done
  return 0
}

run_py() {
  ( cd "$HERE" && timeout "$TIMEOUT_SECS" python3 "$1" --host "$HOST" --port "$PORT" )
}

pass=0
fail=0
declare -a failed_files=()

for pyfile in "$HERE"/*.py; do
  base="$(basename "$pyfile")"
  [[ "$base" == "fvlib.py" ]] && continue
  should_run "$base" || continue
  [[ "$QUIET" -eq 1 ]] || echo "=== $base ==="
  log="$LOG_DIR/$base.log"
  if run_py "$base" > "$log" 2>&1; then
    pass=$((pass + 1))
    [[ "$QUIET" -eq 1 ]] || echo "  ok ($log)"
  else
    fail=$((fail + 1))
    failed_files+=("$base")
    [[ "$QUIET" -eq 1 ]] || echo "  exit != 0 -- see $log (read the log before assuming a bug)"
  fi
done

if [[ "$INCLUDE_RESPONSE_MODES" -eq 1 ]]; then
  [[ "$QUIET" -eq 1 ]] || echo "=== response-modes.py (the daemon's current mode) ==="
  log="$LOG_DIR/response-modes.py.log"
  if run_py response-modes.py > "$log" 2>&1; then
    pass=$((pass + 1))
    [[ "$QUIET" -eq 1 ]] || echo "  ok ($log)"
  else
    fail=$((fail + 1))
    failed_files+=("response-modes.py")
    [[ "$QUIET" -eq 1 ]] || echo "  exit != 0 -- see $log"
  fi
elif [[ "$QUIET" -eq 1 ]]; then
  :
else
  echo "(skipped response-modes.py -- the mode is fixed by the daemon's environment"
  echo " when it starts; pass --include-response-modes to run it against the"
  echo " current mode, or see the file's own header)"
fi

echo
echo "=== $pass ok, $fail non-zero exit, logs in $LOG_DIR ==="
if [[ "$fail" -gt 0 ]]; then
  echo "Non-zero-exit files (read each log before assuming a bug):"
  printf '  %s\n' "${failed_files[@]}"
  exit 1
fi
exit 0
