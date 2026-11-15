#!/usr/bin/env bash
# tests/valgrind_run.sh -- build_test.sh --profile: valgrind memcheck over the module, the daemon and the MCP server (Linux only).
#
# What it adds to the sanitizer cells: uninitialised reads (MemorySanitizer cannot be used, the host is not instrumented) and
# leaks (the sanitizer runs set detect_leaks=0), on the unmodified build.
#
# Targets:
#   units    the fuzz corpus (tests/fuzz/corpus_fv) replayed with mutations through tests/fuzz/fuzz_fv.c; -O0 -g
#   module   valkey-server started under valgrind with fractalsql.so loaded, driven by the harness's own gate scripts
#            (tests/bt_gates.py 02 03 04 05 06 07 18 20: smoke, scout, bounds, survival, vector, analytics, agents, sessions).
#            The module's allocations go through the server's allocator (zmalloc); valgrind's malloc replacement wins over the
#            distribution's dynamically linked jemalloc, so they are tracked. Gate 08 (a 60 s heavy EXPLORE with a latency
#            check) is left out: valgrind's slowdown makes its timings meaningless.
#   daemon   fractalsqld under valgrind, and a valkey-server under valgrind with the module configured to use it, driven by
#            gate 15 phase "up" (reasoning and embedding through the daemon and the vendored HTTP plugin, against the mock LLM)
#   mcp      the MCP server's own code (mcp/src, C++): vectors_probe, sanitize_probe, concurrency_probe, db_probe (live Valkey)
#   server   the running MCP server under valgrind (a wrapper script starts it), driven by mcp/tests/mcp_smoke_client.py against
#            a valkey-server (under valgrind) with the module loaded
#
# Verdict (tests/valgrind_check.py): FAIL on any memcheck error or definite/indirect leak whose stack has a frame in our code (the
# module, the daemon, the vendored plugin, a probe or the MCP server binary); the host's own reports (the Valkey server, libc,
# libstdc++) and possible leaks are counted, not failed. SKIP (exit 0) when not on Linux or valgrind / a Valkey server is missing.
#
# Env: PROFILE_KEEP=1 keeps the logs directory; PROFILE_ONLY=units|module|daemon|mcp|server runs one target; SERVER names the
# valkey-server (default: valkey-server or redis-server on PATH).
set -u
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$HERE/.." && pwd)"
cd "$ROOT"
say() { printf '\n== profile: %s\n' "$*"; }
[ "$(uname -s)" = Linux ] || { echo "  SKIP profile: valgrind runs on Linux only"; exit 0; }
command -v valgrind >/dev/null 2>&1 || { echo "  SKIP profile: valgrind is not installed"; exit 0; }
SRV="${SERVER:-$(command -v valkey-server || command -v redis-server || true)}"
CLI="$(command -v valkey-cli || command -v redis-cli || true)"
[ -n "$SRV" ] && [ -n "$CLI" ] || { echo "  SKIP profile: no valkey-server / valkey-cli on PATH (set SERVER)"; exit 0; }
PY="$(command -v python3)"
PLAT="$(uname -s | tr '[:upper:]' '[:lower:]')-$(uname -m)"
ONLY="${PROFILE_ONLY:-}"
W="$(mktemp -d "${TMPDIR:-/tmp}/fsql_profile_XXXXXX")"
PIDS=()
cleanup() { for p in "${PIDS[@]:-}"; do [ -n "$p" ] && { kill -9 "$p" 2>/dev/null; wait "$p" 2>/dev/null; }; done; [ "${PROFILE_KEEP:-0}" = 1 ] || rm -rf "$W"; }
trap cleanup EXIT
FAILS=0
VG=(valgrind --tool=memcheck --leak-check=full --show-leak-kinds=definite,indirect,possible --track-origins=yes
    --num-callers=40 --fair-sched=yes --xml=yes)
cc_clean() { env -u LD_PRELOAD "$@"; }
fail() { echo "  FAIL $*"; FAILS=$((FAILS + 1)); PROFILE_KEEP=1; }
free_port() { "$PY" -c 'import socket;s=socket.socket();s.bind(("127.0.0.1",0));print(s.getsockname()[1])'; }

# the module (make) and, when needed, the daemon (make -C service daemon)
need_module() {
  [ -n "${MODULE_BUILT:-}" ] && return 0
  MODULE="$ROOT/fractalsql.so"
  if ! nice make >"$W/make.log" 2>&1; then
    # e.g. the vendored-artifact checksum check in include/ failed: use the module that is already built, and say so
    if [ -f "$MODULE" ]; then echo "  NOTE: make failed ($(grep -m1 'ERROR' "$W/make.log" | cut -c1-110)); using the existing $MODULE"
    else fail "the module did not build (see $W/make.log)"; return 1; fi
  fi
  MODULE_BUILT=1
  [ -f "$MODULE" ] || { fail "no module at $MODULE"; return 1; }
}

# start_vg NAME CMD...  : CMD under valgrind in the background; logs in $W/NAME.{xml,log}; sets VG_PID
start_vg() {
  local name="$1"; shift
  ( cd "$W" && exec nice "${VG[@]}" --xml-file="$W/$name.xml" "$@" ) >"$W/$name.log" 2>&1 &
  VG_PID=$!; PIDS+=("$VG_PID")
}
# a valkey-server under valgrind with the module (extra module options after NAME); sets SPORT, SPID
start_server_vg() {
  local name="$1"; shift
  SPORT="$(free_port)"; mkdir -p "$W/data_$name"
  start_vg "$name" "$SRV" --port "$SPORT" --bind 127.0.0.1 --dir "$W/data_$name" --save "" --appendonly no \
           --enable-module-command local --loadmodule "$MODULE" "$@"
  SPID="$VG_PID"
  local i
  for i in $(seq 1 240); do
    "$CLI" -p "$SPORT" ping 2>/dev/null | grep -q PONG && return 0
    kill -0 "$SPID" 2>/dev/null || break
    sleep 0.5
  done
  echo "  the server '$name' did not come up:"; tail -5 "$W/$name.log"; return 1
}
# stop_vg PID [signal-or-shutdown-port]: a clean exit is what makes valgrind write its leak report
stop_server_vg() {   # stop_server_vg PID PORT
  "$CLI" -p "$2" shutdown nosave >/dev/null 2>&1
  local i; for i in $(seq 1 240); do kill -0 "$1" 2>/dev/null || return 0; sleep 0.5; done
  kill -9 "$1" 2>/dev/null; return 1
}
verdict() {   # verdict LABEL ours... -- xml...
  local label="$1"; shift
  local args=() xmls=() seen=0
  for a in "$@"; do
    if [ "$a" = -- ]; then seen=1; continue; fi
    if [ "$seen" = 0 ]; then args+=(--ours "$a"); else xmls+=("$a"); fi
  done
  local x ok=1; for x in "${xmls[@]}"; do [ -s "$x" ] || { echo "  FAIL $label: no valgrind report at $x"; ok=0; }; done
  [ "$ok" = 1 ] || { FAILS=$((FAILS + 1)); PROFILE_KEEP=1; return; }
  if "$PY" -I "$HERE/valgrind_check.py" verdict "${args[@]}" "${xmls[@]}"; then echo "  PASS $label"
  else echo "  FAIL $label (logs in $W)"; FAILS=$((FAILS + 1)); PROFILE_KEEP=1; fi
}
gate() {   # gate NUM PORT PID [VAR=value ...]: one harness gate script against a running server; prints its FAIL lines
  local num="$1" port="$2" pid="$3"; shift 3
  local out rc t0=$SECONDS
  out="$(env "$@" "$PY" "$ROOT/tests/bt_gates.py" "$num" --port "$port" --pid "$pid" 2>&1)"; rc=$?
  local np nf; np="$(grep -c '^PASS ' <<< "$out")"; nf="$(grep -c '^FAIL ' <<< "$out")"
  echo "  gate $num: $np passed, $nf failed ($((SECONDS - t0)) s)"
  if [ "$nf" -ne 0 ] || { [ "$rc" -ne 0 ] && [ "$np" -eq 0 ]; }; then
    grep '^FAIL ' <<< "$out" | head -4 | cut -c1-200
    echo "  (a failure here may be valgrind's slowdown, not a defect: the run's verdict is the memory report)"
  fi
}

if [ -z "$ONLY" ] || [ "$ONLY" = units ]; then
  say "units"
  core="$ROOT/include/$PLAT/libfractalsql-community-sovereign-c.a"
  mkdir -p "$W/u"
  if [ ! -f "$core" ]; then echo "  SKIP units: no core archive for $PLAT"
  elif ! cc_clean cc -g -O0 -fcommon -I"$ROOT/include" -I"$ROOT/src" -DFRACTALSQL_VERSION=\""$(tr -d '[:space:]' < "$ROOT/VERSION")"\" \
         "$HERE/fuzz_replay.c" "$HERE/fuzz/fuzz_fv.c" "$ROOT/src/fv_pool.c" "$ROOT/src/fv_vector.c" "$core" -lm -ldl -lpthread \
         -o "$W/u/fuzz_fv" 2>"$W/u/cc.log"; then fail "the fuzz replay did not build"; tail -3 "$W/u/cc.log"
  else
    timeout 600 nice "${VG[@]}" --xml-file="$W/u/fuzz_fv.xml" "$W/u/fuzz_fv" "$HERE/fuzz/corpus_fv" 3 >"$W/u/fuzz_fv.log" 2>&1
    verdict fuzz_fv "$W/u/" -- "$W/u/fuzz_fv.xml"
  fi
fi

if [ -z "$ONLY" ] || [ "$ONLY" = module ]; then
  say "module (valkey-server under valgrind, the harness's gate scripts)"
  if need_module && start_server_vg module; then
    export FSQL_EXPECT_VERSION="$(tr -d '[:space:]' < "$ROOT/VERSION")"
    for g in 02 03 04 05 06 07 18 20; do gate "$g" "$SPORT" "$SPID"; done
    stop_server_vg "$SPID" "$SPORT" || fail "the server did not exit after SHUTDOWN"
    verdict module "$MODULE" -- "$W/module.xml"
  else fail "module: could not start the server under valgrind"; fi
fi

if [ -z "$ONLY" ] || [ "$ONLY" = daemon ]; then
  say "daemon (fractalsqld + a server using it, both under valgrind)"
  FSQLD="${FSQL_DAEMON:-$ROOT/service/build/fractalsqld}"
  PLUG="$(cd "$ROOT/include/$PLAT" 2>/dev/null && pwd -P)/fractalsql-reasoning-http.so"
  if need_module; then
    [ -x "$FSQLD" ] || nice make -C "$ROOT/service" daemon >"$W/dmake.log" 2>&1
    if [ ! -x "$FSQLD" ] || [ ! -f "$PLUG" ]; then echo "  SKIP daemon: no fractalsqld ($FSQLD) or no vendored plugin ($PLUG)"
    else
      MOCKP="$(free_port)"; "$PY" "$ROOT/scripts/ci/mock_llm.py" "$MOCKP" >"$W/mock_llm.log" 2>&1 & PIDS+=("$!")
      D="$W/daemon"; mkdir -p "$D"; : > /dev/null
      "$PY" -c 'import secrets; print(secrets.token_hex(32))' > "$D/key"; chmod 600 "$D/key"
      printf 'socket_path = %s\nhmac_key_file = %s\nallowed_uids = %s\n' "$D/fsqld.sock" "$D/key" "$(id -u)" > "$D/fsqld.conf"; chmod 600 "$D/fsqld.conf"
      sleep 1
      ( cd "$D" && FSQL_REASONING_REQUIRE_SIGNATURE=0 FRACTALSQL_REASONING_PLUGIN="$PLUG" \
          FRACTALSQL_HTTP_URL="http://127.0.0.1:$MOCKP/v1/chat/completions" FRACTALSQL_HTTP_EMBED_URL="http://127.0.0.1:$MOCKP/v1/embeddings" \
          FRACTALSQL_HTTP_ALLOW_PLAINTEXT=1 exec nice "${VG[@]}" --xml-file="$W/daemon.xml" "$FSQLD" -c "$D/fsqld.conf" </dev/null ) >"$W/daemon.log" 2>&1 &
      DPID=$!; PIDS+=("$DPID")
      for i in $(seq 1 240); do [ -S "$D/fsqld.sock" ] && break; kill -0 "$DPID" 2>/dev/null || break; sleep 0.5; done
      if [ ! -S "$D/fsqld.sock" ]; then fail "daemon: fractalsqld did not open its socket"; tail -5 "$W/daemon.log"
      elif start_server_vg dserver daemon-socket "$D/fsqld.sock" daemon-key-file "$D/key" daemon-timeout-s 60; then
        gate 15 "$SPORT" "$SPID" FSQL_PHASE=up
        stop_server_vg "$SPID" "$SPORT" || fail "the server did not exit after SHUTDOWN"
        kill -TERM "$DPID" 2>/dev/null; for i in $(seq 1 240); do kill -0 "$DPID" 2>/dev/null || break; sleep 0.5; done
        verdict daemon "$MODULE" "$FSQLD" "fractalsql-reasoning" -- "$W/daemon.xml" "$W/dserver.xml"
      else fail "daemon: could not start the server with the daemon configured"; fi
    fi
  fi
fi

# the MCP server's build, shared by the probes and the live run
mcp_ready() {
  [ -n "${MCP_READY:-}" ] && return 0
  CXX="$(command -v g++ || command -v c++ || command -v clang++ || true)"
  if [ -z "$CXX" ] || ! printf '#include <openssl/ssl.h>\nint main(){return 0;}\n' | cc_clean "$CXX" -x c++ -fsyntax-only - 2>/dev/null; then
    echo "  SKIP mcp: no C++ compiler or no OpenSSL headers"; return 1
  fi
  mkdir -p "$W/mcp"; MCP_READY=1
}

if [ -z "$ONLY" ] || [ "$ONLY" = mcp ]; then
  say "mcp (the MCP server's probes, C++)"
  if mcp_ready && need_module; then
    if ! cc_clean nice make -C "$ROOT/mcp" probes CXX="$CXX" CXXFLAGS="-O1 -g" BUILD="$W/mcp/build" >"$W/mcp/pmake.log" 2>&1; then
      fail "the MCP probes did not build (see $W/mcp/pmake.log)"; tail -5 "$W/mcp/pmake.log"
    else
      for p in vectors_probe sanitize_probe concurrency_probe; do
        timeout 1500 nice "${VG[@]}" --xml-file="$W/mcp/$p.xml" "$W/mcp/build/$p" >"$W/mcp/$p.log" 2>&1
        verdict "mcp_$p" "$W/mcp/" -- "$W/mcp/$p.xml"
      done
      # db_probe needs a live Valkey: a plain one is enough (the probe checks the server's own connection code)
      PP="$(free_port)"; mkdir -p "$W/data_plain"
      ( cd "$W/data_plain" && exec "$SRV" --port "$PP" --bind 127.0.0.1 --dir "$W/data_plain" --save "" --appendonly no ) >"$W/plain.log" 2>&1 & PL=$!; PIDS+=("$PL")
      for i in $(seq 1 100); do "$CLI" -p "$PP" ping 2>/dev/null | grep -q PONG && break; sleep 0.2; done
      VALKEY_HOST=127.0.0.1 VALKEY_PORT="$PP" timeout 1500 nice "${VG[@]}" --xml-file="$W/mcp/db_probe.xml" "$W/mcp/build/db_probe" >"$W/mcp/db_probe.log" 2>&1
      verdict mcp_db_probe "$W/mcp/" -- "$W/mcp/db_probe.xml"
      kill "$PL" 2>/dev/null
    fi
  fi
fi

if [ -z "$ONLY" ] || [ "$ONLY" = server ]; then
  say "server (the running MCP server under valgrind, driven by mcp/tests/mcp_smoke_client.py)"
  if mcp_ready && need_module; then
    if ! cc_clean nice make -C "$ROOT/mcp" CXX="$CXX" CXXFLAGS="-O1 -g" BUILD="$W/mcp/sbuild" OUT="$W/mcp/fractalsql-valkey-mcp" >"$W/mcp/smake.log" 2>&1; then
      fail "the MCP server did not build (see $W/mcp/smake.log)"; tail -5 "$W/mcp/smake.log"
    elif start_server_vg mserver; then
      # the server refuses to start without its drift locks, looked up next to the executable
      cp "$ROOT/mcp/.mcp-schema-lock" "$ROOT/mcp/.mcp-safety-annotations-lock" "$W/mcp/"
      mkdir -p "$W/mcp/slogs" "$W/mcp/cwd"
      cat > "$W/mcp/srv-valgrind.sh" <<EOF
#!/bin/sh
exec nice valgrind --tool=memcheck --leak-check=full --show-leak-kinds=definite,indirect,possible --track-origins=yes --num-callers=40 --fair-sched=yes --xml=yes --xml-file='$W/mcp/slogs/server.%p.xml' '$W/mcp/fractalsql-valkey-mcp' "\$@"
EOF
      chmod +x "$W/mcp/srv-valgrind.sh"
      out="$(env MCP_SERVER_EXE="$W/mcp/srv-valgrind.sh" MCP_SERVER_CWD="$W/mcp/cwd" MCP_HERE="$ROOT/mcp" VALKEY_HOST=127.0.0.1 VALKEY_PORT="$SPORT" \
             MCP_SMOKE_SKIP_SWEEPS=1 ASAN_OPTIONS=detect_leaks=0 timeout 3000 "$PY" "$ROOT/mcp/tests/mcp_smoke_client.py" 2>&1)"
      rc=$?
      printf '%s\n' "$out" > "$W/mcp/smoke.log"
      echo "  smoke client: $(grep -c '^ *PASS' <<< "$out") checks passed, $(grep -c '^ *FAIL' <<< "$out") failed (exit $rc)"
      if [ "$rc" -ne 0 ]; then
        echo "  (a failure here may be a client timeout under valgrind's slowdown, see $W/mcp/smoke.log)"
        grep -E '^ *FAIL|Traceback|Error' <<< "$out" | head -5 | cut -c1-200
        FAILS=$((FAILS + 1)); PROFILE_KEEP=1
      fi
      stop_server_vg "$SPID" "$SPORT" || fail "the server did not exit after SHUTDOWN"
      # shellcheck disable=SC2206
      logs=("$W"/mcp/slogs/server.*.xml)
      if [ -e "${logs[0]}" ]; then verdict server "$W/mcp/fractalsql-valkey-mcp" "$MODULE" -- "${logs[@]}" "$W/mserver.xml"
      else fail "server: no valgrind logs were written for the MCP server"; fi
    else fail "server: could not start the Valkey server"; fi
  fi
fi

echo
if [ "$FAILS" -eq 0 ]; then echo "profile: all targets clean"; else echo "profile: $FAILS target(s) failed"; fi
[ "${PROFILE_KEEP:-0}" = 1 ] && echo "  logs kept in $W"
exit "$FAILS"
