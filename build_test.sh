#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
# SPDX-FileCopyrightText: 2026 Daniel Gardiner d/b/a FractalSQLabs
#
# build_test.sh: post-build validation gate runner for fractalsql-valkey.
# Mirrors what CI runs, so local == CI.
#
# Builds (or accepts a prebuilt) fractalsql.so, then loads it into a
# THROWAWAY valkey-server (private port, private temp dir, no root, nothing
# persisted) and drives it through tests/bt_gates.py.
#
# Gates:
#   01  build      module builds; exports only RedisModule_OnLoad; no
#                  unexpected undefined symbols
#   02  smoke      load, edition/version, FRACTAL.SEARCH convergence
#   03  scout      FRACTAL.EXPLORE population size and dispersion
#   04  bounds     every validation branch in src/module.c, the size caps at
#                  their boundaries, K and population range checks, and
#                  non-finite floats in the query and the corpus
#   05  survival   400 random hostile calls; the server must keep answering
#                  PING, with the same pid
#   06  vector     every FRACTAL.VECTOR.* command: values, text round trips,
#                  hostile arguments
#   07  analytics  dimension, series, cycle, TDA, portfolio and morphology
#                  commands: values, bounds, non-finite input
#   08  async      the main thread stays free during a long EXPLORE; 12
#                  concurrent clients; MULTI and Lua run inline
#   09  limits     (own server: threads 2, max-inflight-mb 1) over-cap work is
#                  refused, not queued; bad load arguments refuse to load
#   10  lifecycle  (own server) MODULE UNLOAD/LOAD, unload refused while busy,
#                  clients that vanish mid-job leak nothing
#   11  oom        (uninstrumented only) every allocation point of every command
#                  fails in turn (module-side injection build): the server must
#                  survive each, and each command must surface a clean error
#   12  acl        per-command, per-key and @read category permissions
#   13  persist    a corpus survives a restart; replicas can search it; no
#                  FRACTAL command is propagated to replicas
#   14  versions   Valkey 8.0 and 8.1 (official images, needs docker)
#   15  daemon     reasoning, embedding and ledger commands through a real
#                  fractalsqld with a mock LLM: community (dormant ledger),
#                  daemon down and back, wrong key, bad load options
#   16  enterprise a real enterprise library behind the daemon: ledger flush,
#                  verify, audit chain, lock, restart, tamper detection.
#                  Skips unless FSQL_ENTERPRISE_LIB (or a library vendored in
#                  include/) is available; never ships one.
#   17  demos      every demo/*.py: the 44 demo-vertical-* starter kits in demo/ (42 industry verticals plus the
#                  two Valkey-specific ones, semantic-cache and agent-loop-detection): each is self-checking
#                  and must run clean and reproduce its headline result
#   18  agents     FRACTAL.AGENT.*: key-backed agents; computed result with no
#                  daemon, validation, ACL (they are @dangerous), the same numbers
#                  as the primitives they compose
#
#   20  sessions   named diversify/repulsion sessions: enable, params, SESSION on
#                  the search commands, feedback, D_q, close, limits, ACL, and a
#                  session closed while a search runs on it
#   22  mcp_probes the MCP server (mcp/, a C++17 client of the module): build it, then the unit
#                  probes: RESP parsing of well-formed and hostile bytes, error classification, the
#                  connection layer against a fake server (stall, hang-up, lying length, flood,
#                  refused password) and against this server, plus argument coercion, the
#                  concurrency caps and secret redaction
#   23  mcp_smoke  a full MCP session over stdio: handshake, the catalog and its annotations,
#                  every tool once, the numbers the tools must produce, the error taxonomy,
#                  resources, prompts, FSQL_MCP_KEY_PREFIX and read-only mode, $/cancel, shutdown,
#                  then the robustness and mutation sweeps
#   24  mcp_https  the HTTP transport over TLS: fail-fast misconfiguration, TLS-only, bearer token,
#                  a full authenticated tool call
#   25  mcp_cli    the server started in every way its command line, environment and connection
#                  allow (139 runs), and a connection killed under it
#   26  mcp_db_probe       the RESP client (parser, reconnect, error classification) against a live server
#   27  mcp_concurrency_probe  the per-class concurrency caps on their own
#   28  mcp_faults  FAULT ONLY (--fault, not in DEFAULT/QUICK) (Linux) the MCP server under an
#                  allocation/file/socket fault shim: one call failed at a
#                  time, the server must keep answering
#   30  mcp_tls    the server's TLS connection to Valkey (VALKEY_TLS*): trusted CA, insecure, client
#                  certificate, and every refusal, against a TLS-terminating proxy
#   31  mcp_conformance  the server driven through the whole published protocol surface by the
#                  shared conformance driver (mcp/tests/mcp_conformance.py): both protocol eras,
#                  the authentication and framing rules, the catalog's shape and annotations, the
#                  operator switches and the session-state handles
#   29  mcp_fuzz   (--fuzz only) libFuzzer + ASan over the RESP reply parser, the JSON-RPC front door and the
#                  argument coercion
#   21  fuzz       (--fuzz only) libFuzzer + ASan + UBSan over the code that parses bytes the
#                  module does not control: text vectors and the daemon's EMBED reply, the
#                  DIVERSIFY.PARAMS JSON, a daemon CALL result, a whole daemon reply frame
#                  (valid and corrupt, over a socket pair), and the JSON string writer
#
# Gate sets:
#   QUICK   = 01 02
#   DEFAULT = 01 02 03 04 05 06 07 08 09 10 11 12 13 14 15 16 17 18 19 20
#   FAULT   = 28                    --fault runs DEFAULT, then ADDS gate 28 after it
#                                    (28 injects one allocation, file or socket failure inside
#                                    each MCP tool call; Linux only, and it skips under a
#                                    sanitizer -- the fault shim and the sanitizer runtime cannot
#                                    both interpose the allocator. Deliberately NOT in DEFAULT and
#                                    not in CI: --fault is a local, on-demand run.)
#
# Platforms: Linux and macOS, amd64 and arm64.
#
# Usage:
#   ./build_test.sh                  # DEFAULT
#   ./build_test.sh --quick          # QUICK
#   ./build_test.sh --gate mcp       # only the MCP server gates (22-27, 30, 31; plus 28 with
#                                    # --fault); add --asan/--ubsan/--tsan/--coverage
#   ./build_test.sh --fault          # DEFAULT, then gate 28 -- the MCP server under injected
#                                    # allocation, file and socket failures (Linux only; a
#                                    # local run, not part of CI)
#   ./build_test.sh --gate 04        # ONE gate (gate 01 builds, so it runs
#                                    # first when the module is missing)
#   ./build_test.sh --asan           # or --ubsan, composable with each other
#   ./build_test.sh --tsan           # NOT composable with --asan/--ubsan
#   ./build_test.sh --coverage       # gcov-instrumented build + a per-file report;
#                                    # NOT composable with a sanitizer
#   ./build_test.sh --profile        # Linux: valgrind memcheck over the module (valkey-server under valgrind, the gate
#                                    # scripts), the daemon, the MCP server's probes and the running MCP server;
#                                    # tests/valgrind_run.sh. Fails on a memcheck error or definite/indirect leak with
#                                    # a frame in our code. Not in any gate set (about 15 minutes).
#   ./build_test.sh --fuzz           # gate 21 only: libFuzzer on the parsers (needs clang
#                                    # with libFuzzer; FSQL_FUZZ_CC, FSQL_FUZZ_TIME seconds per run)
#   ./build_test.sh --prebuilt       # test the existing ./fractalsql.so as is
#                                    # (e.g. one built by build.sh), no rebuild
#   ./build_test.sh --rebuild        # force a fresh build instead of trusting the staleness check:
#                                    # rebuilds ./fractalsql.so, and on a Linux amd64 host with docker
#                                    # also refreshes dist/amd64 (./build.sh amd64) for gate 14's bundle
#                                    # step. Heavy: run it from a local copy, not a shared folder.
#                                    # NOT composable with --prebuilt.
#   ./build_test.sh --fractalsqld P  # the fractalsqld daemon for gates 15/16
#                                    # (default: $FSQL_DAEMON, else service/build/fractalsqld)
#   ./build_test.sh --server PATH    # valkey-server (default: valkey-server,
#                                    # else redis-server)
#   ./build_test.sh --list
#
# Isolation: every run gets its own port and temp dir, so two runs (or a run
# and a developer's own server) never collide.
#
# Sanitizers: the module is built instrumented and the matching runtime is
# LD_PRELOADed into the (uninstrumented) server. Passing --asan or --ubsan
# always rebuilds the module, so a stale uninstrumented binary is never
# tested by mistake.

set -uo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$HERE"

ALL_GATES=(01 02 03 04 05 06 07 08 09 10 11 12 13 14 15 16 17 18 19 20 22 23 24 25 26 27 30 31 32 33 34 35 36)
MCP_GATES=(22 23 24 25 26 27 30 31)   # --gate mcp
FUZZ_GATES=(21 29)                    # --fuzz
QUICK_GATES=(01 02)
# The fault gates. A fault gate injects an allocation, file or socket failure and proves the
# server survives it, so it needs a fault shim (Linux only) and cannot run under a sanitizer
# (the shim and the sanitizer runtime cannot both interpose the allocator). They are
# deliberately NOT in ALL_GATES and not in CI: --fault is a local, on-demand run.
FAULT_GATES=(28)
MCP_FAULT_GATES=(28)   # the fault gates that are also MCP server gates (--gate mcp --fault)
# Captured before the arg-parsing loop consumes "$@": --tsan re-executes this
# same invocation under setarch (see below).
ORIG_ARGV=("$@")
GATES=("${ALL_GATES[@]}")
ONE_GATE=""
ASAN=0
UBSAN=0
TSAN=0
COVERAGE=0
SERVER_BIN=""
PREBUILT=0
REBUILD=0
FSQLD_BIN=""
FUZZ=0
FAULT=0
MCP_SET=0

while [ $# -gt 0 ]; do
  case "$1" in
    --quick)  GATES=("${QUICK_GATES[@]}") ;;
    --gate)
      shift
      ONE_GATE="${1:-}"
      case "$ONE_GATE" in
        *,*|*" "*) echo "ERROR: --gate takes ONE gate number, not a list ('$ONE_GATE')" >&2; exit 2 ;;
      esac
      if [ "$ONE_GATE" = "mcp" ]; then ONE_GATE=""; GATES=("${MCP_GATES[@]}"); MCP_SET=1; fi
      ;;
    --asan)   ASAN=1 ;;
    --ubsan)  UBSAN=1 ;;
    --tsan)   TSAN=1 ;;
    --coverage) COVERAGE=1 ;;
    --profile)  exec "$HERE/tests/valgrind_run.sh" ;;
    --server) shift; SERVER_BIN="${1:-}" ;;
    --prebuilt) PREBUILT=1 ;;
    --rebuild)  REBUILD=1 ;;
    --fuzz)   FUZZ=1; GATES=("${FUZZ_GATES[@]}") ;;
    --fault)  FAULT=1 ;;
    --fractalsqld) shift; FSQLD_BIN="${1:-}" ;;
    --list)   echo "gates: ${ALL_GATES[*]}"; echo "quick: ${QUICK_GATES[*]}"; echo "mcp (--gate mcp): ${MCP_GATES[*]}"; echo "fuzz (--fuzz only): ${FUZZ_GATES[*]}"; echo "fault (--fault only): ${FAULT_GATES[*]}"; exit 0 ;;
    -h|--help) sed -n '2,50p' "$0"; exit 0 ;;
    *) echo "ERROR: unknown argument '$1'" >&2; exit 2 ;;
  esac
  shift
done

if [ "$REBUILD" -eq 1 ] && [ "$PREBUILT" -eq 1 ]; then
  echo "ERROR: --rebuild cannot be combined with --prebuilt" >&2; exit 2
fi

if [ -n "$ONE_GATE" ]; then
  found=0
  # A gate that is not in ALL_GATES still exists: the fuzz and fault sets are opt-in, so
  # `--gate 21` (fuzz) and `--gate 28` (fault) must still be accepted.
  for g in "${ALL_GATES[@]}" "${FUZZ_GATES[@]}" "${FAULT_GATES[@]}"; do [ "$g" = "$ONE_GATE" ] && found=1; done
  [ "$found" -eq 1 ] || { echo "ERROR: no gate '$ONE_GATE' (have: ${ALL_GATES[*]})" >&2; exit 2; }
  GATES=("$ONE_GATE")
fi

# The fault gates run only when they are asked for. They are in neither ALL_GATES nor the
# MCP set, so --fault appends them to whichever set was chosen (--gate mcp --fault gets the
# MCP fault gates, so the server is not fault-tested against a cluster it does not use).
if [ "$FAULT" -eq 1 ]; then
  if [ "$MCP_SET" -eq 1 ]; then
    GATES+=("${MCP_FAULT_GATES[@]}")
  else
    for g in "${FAULT_GATES[@]}"; do
      case " ${GATES[*]} " in *" $g "*) ;; *) GATES+=("$g") ;; esac
    done
  fi
fi

PASS=0; FAIL=0; SKIP=0; KNOWN=0
pass()  { printf '  [PASS] %s\n' "$*"; PASS=$((PASS + 1)); }
fail()  { printf '  [FAIL] %s\n' "$*"; FAIL=$((FAIL + 1)); }
skip()  { printf '  [SKIP] %s\n' "$*"; SKIP=$((SKIP + 1)); }
known() { printf '  [KNOWN] %s\n' "$*"; KNOWN=$((KNOWN + 1)); }

# The core archive is built per C library, so the variant follows the library
# the host runs on: a musl host cannot link the glibc archive. An explicit
# CORE_VARIANT in the environment still wins.
if [ -z "${CORE_VARIANT:-}" ]; then
  ldd_banner="$(ldd --version 2>&1 || true)"
  case "$ldd_banner" in
    *musl*) CORE_VARIANT="community-sovereign-c-musl" ;;
  esac
  if [ -z "${CORE_VARIANT:-}" ] && ls /lib/ld-musl-* >/dev/null 2>&1; then
    CORE_VARIANT="community-sovereign-c-musl"
  fi
  [ -n "${CORE_VARIANT:-}" ] && export CORE_VARIANT
fi

# --tsan: a TSan-instrumented binary (the MCP server, the probes) can die at
# startup with "ThreadSanitizer: unexpected memory mapping" on a kernel whose
# ASLR entropy is higher than TSan's shadow layout assumes. valkey-server is
# already started under `setarch -R`, but the MCP gates start their own TSan
# binaries, so the whole script is re-executed with ASLR off before anything
# else starts. The probe runs a real setarch rather than trusting PATH (a
# seccomp profile can make a present setarch fail). FSQL_BT_TSAN_SETARCHED
# stops a second re-exec.
if [ "$TSAN" -eq 1 ] && [ "$(uname -s)" != "Darwin" ] && [ -z "${FSQL_BT_TSAN_SETARCHED:-}" ]; then
  if setarch "$(uname -m)" -R true >/dev/null 2>&1; then
    export FSQL_BT_TSAN_SETARCHED=1
    exec setarch "$(uname -m)" -R "$0" "${ORIG_ARGV[@]}"
  else
    echo "WARNING: --tsan requested but 'setarch $(uname -m) -R' did not succeed;" >&2
    echo "         continuing with ASLR on. TSan may FATAL on this kernel." >&2
  fi
fi

TMPROOT="$(mktemp -d "${TMPDIR:-/tmp}/fractalsql_valkey_bt.XXXXXX")"
# Canonical path: on macOS $TMPDIR sits under /var, a symlink to /private/var, and anything that compares or re-resolves a path sees the real one.
TMPROOT="$(cd "$TMPROOT" && pwd -P)"

# macOS has no timeout(1). Callers run it behind env, so a shell function would not be found: put an executable
# on PATH instead (GNU coreutils' gtimeout when installed, otherwise a perl alarm wrapper).
if ! command -v timeout >/dev/null 2>&1; then
  mkdir -p "$TMPROOT/bin"
  if command -v gtimeout >/dev/null 2>&1; then
    ln -s "$(command -v gtimeout)" "$TMPROOT/bin/timeout"
  else
    printf '%s\n' '#!/bin/sh' 's="${1%s}"; shift' "exec perl -e 'alarm shift; exec @ARGV or exit 127' \"\$s\" \"\$@\"" > "$TMPROOT/bin/timeout"
    chmod +x "$TMPROOT/bin/timeout"
  fi
  PATH="$TMPROOT/bin:$PATH"
fi

# A short directory for Unix socket paths: sun_path is 104 bytes on macOS and macOS's $TMPDIR is already about 80.
SHORT_TMP=""
short_tmp() {
  [ -n "$SHORT_TMP" ] || SHORT_TMP="$(mktemp -d /tmp/fsqlbt.XXXXXX)"
  printf '%s' "$SHORT_TMP"
}
SERVER_PID=""
PORT=""
ALL_PIDS=()
cleanup() {
  local p
  for p in "${ALL_PIDS[@]+"${ALL_PIDS[@]}"}"; do
    kill "$p" 2>/dev/null
  done
  for p in "${ALL_PIDS[@]+"${ALL_PIDS[@]}"}"; do
    for _ in $(seq 1 50); do kill -0 "$p" 2>/dev/null || break; sleep 0.1; done
    kill -9 "$p" 2>/dev/null
  done
  rm -rf "$TMPROOT"
  [ -n "$SHORT_TMP" ] && rm -rf "$SHORT_TMP"
}
trap cleanup EXIT

if [ "$COVERAGE" -eq 1 ] && { [ "$ASAN" -eq 1 ] || [ "$UBSAN" -eq 1 ] || [ "$TSAN" -eq 1 ]; }; then
  echo "ERROR: --coverage cannot be combined with a sanitizer" >&2
  exit 2
fi

if [ "$TSAN" -eq 1 ] && { [ "$ASAN" -eq 1 ] || [ "$UBSAN" -eq 1 ]; }; then
  echo "ERROR: --tsan cannot be combined with --asan/--ubsan (the runtimes cannot share a process)" >&2
  exit 2
fi

DARWIN_SAN_DYLIBS=""
if [ "$(uname -s)" = "Darwin" ] && { [ "$ASAN" -eq 1 ] || [ "$UBSAN" -eq 1 ] || [ "$TSAN" -eq 1 ]; }; then
  # --asan/--ubsan: the instrumented module is dlopen'd into an uninstrumented
  # server, so the clang runtime dylibs are injected with DYLD_INSERT_LIBRARIES
  # (the Darwin counterpart of the Linux LD_PRELOAD). Only the server is
  # injected, never the python client. dyld strips the variable from Apple
  # system binaries (SIP) and from hardened-runtime ones, so this needs a
  # server that is not one, such as Homebrew's; anything else is skipped with
  # the reason rather than run unsanitized and reported clean.
  # --tsan is not attempted: the runtime cannot be retrofitted reliably.
  darwin_skip() { echo "SKIP: $1" >&2; exit 0; }
  [ "$TSAN" -eq 1 ] && darwin_skip "--tsan is not supported on Darwin (Linux covers it); --asan/--ubsan are"
  _srv="${SERVER_BIN:-$(command -v valkey-server || command -v redis-server || true)}"
  [ -n "$_srv" ] || darwin_skip "no valkey-server or redis-server to inject the sanitizer runtime into (use --server PATH)"
  case "$(command -v "$_srv" 2>/dev/null || echo "$_srv")" in
    /usr/bin/*|/bin/*|/sbin/*|/usr/sbin/*|/usr/libexec/*|/System/*)
      darwin_skip "'$_srv' is an Apple system binary: SIP strips DYLD_INSERT_LIBRARIES, so it would run unsanitized. Use Homebrew's valkey (or --server PATH)" ;;
  esac
  _rtdir="$(cc -print-resource-dir 2>/dev/null)/lib/darwin"
  for _n in $([ "$ASAN" -eq 1 ] && echo asan) $([ "$UBSAN" -eq 1 ] && echo ubsan); do
    _rt="$(cc -print-file-name="libclang_rt.${_n}_osx_dynamic.dylib" 2>/dev/null)"
    [ -f "$_rt" ] || _rt="$_rtdir/libclang_rt.${_n}_osx_dynamic.dylib"
    [ -f "$_rt" ] || darwin_skip "libclang_rt.${_n}_osx_dynamic.dylib was not found for this compiler"
    DARWIN_SAN_DYLIBS="$DARWIN_SAN_DYLIBS${DARWIN_SAN_DYLIBS:+:}$_rt"
  done
fi

PY="$(command -v python3 || true)"
SAN_NOTE=""
[ "$ASAN" -eq 1 ] && SAN_NOTE="$SAN_NOTE asan"
[ "$UBSAN" -eq 1 ] && SAN_NOTE="$SAN_NOTE ubsan"
[ "$TSAN" -eq 1 ] && SAN_NOTE="$SAN_NOTE tsan"
[ "$COVERAGE" -eq 1 ] && SAN_NOTE="$SAN_NOTE coverage"
[ "$COVERAGE" -eq 1 ] && export FSQL_AGENT_MUTATE=1   # tests/bt_gates.py: every agent call is also sent with its arguments changed (see agent_mutants)

# ---------------------------------------------------------------------------
# helpers
# ---------------------------------------------------------------------------
pick_server() {
  if [ -n "${SP_BIN:-}" ]; then echo "$SP_BIN"; return; fi
  if [ -n "$SERVER_BIN" ]; then command -v "$SERVER_BIN" || echo "$SERVER_BIN"; return; fi
  command -v valkey-server || command -v redis-server || true
}

free_port() {
  "$PY" - <<'E'
import socket
s = socket.socket()
s.bind(("127.0.0.1", 0))
print(s.getsockname()[1])
E
}

# Resolves a sanitizer runtime's real .so path for LD_PRELOAD (libasan.so is
# a linker script on some distros, which LD_PRELOAD rejects).
san_runtime() {
  local name="$1" p
  p="$(cc -print-file-name="$name" 2>/dev/null)"
  [ -n "$p" ] && [ "$p" != "$name" ] && readlink -f "$p" || true
}

server_preload() {
  local list=""
  if [ -n "$DARWIN_SAN_DYLIBS" ]; then echo "$DARWIN_SAN_DYLIBS"; return; fi
  if [ "$ASAN" -eq 1 ];  then list="$(san_runtime libasan.so)"; fi
  if [ "$UBSAN" -eq 1 ]; then list="$list${list:+:}$(san_runtime libubsan.so)"; fi
  if [ "$TSAN" -eq 1 ];  then list="$(san_runtime libtsan.so)"; fi
  echo "$list"
}

# Starts a throwaway server with the module loaded, extra args after the name
# being module load options. Sets SP_PID, SP_PORT and SP_LOG.
spawn_server() {
  local name="$1"; shift
  local bin; bin="$(pick_server)"
  if [ -z "$bin" ]; then
    echo "no valkey-server or redis-server on PATH (use --server PATH)" >&2
    return 1
  fi
  SP_PORT="$(free_port)"
  SP_LOG="$TMPROOT/server_$name.log"
  local dir="$TMPROOT/data_$name"; mkdir -p "$dir"
  local pre; pre="$(server_preload)"
  # TSan refuses to start with the kernel's default ASLR layout; run the
  # server with randomisation off (the standard workaround for TSan under ASLR).
  # The preload must reach the server only, not setarch itself, so it is set
  # by `env` after randomisation is off.
  local launch="" outer_pre="$pre" prevar=LD_PRELOAD
  [ "$(uname -s)" = "Darwin" ] && prevar=DYLD_INSERT_LIBRARIES
  if [ "$TSAN" -eq 1 ]; then launch="setarch $(uname -m) -R env LD_PRELOAD=$pre"; outer_pre=""; fi
  # detect_leaks=0: the server itself is uninstrumented and leaks by design
  # at exit; the module's own leaks are checked by the lifecycle gate instead.
  ( cd "$dir" && \
    ASAN_OPTIONS="detect_leaks=0:abort_on_error=0:halt_on_error=1" \
    UBSAN_OPTIONS="print_stacktrace=1:halt_on_error=1" \
    TSAN_OPTIONS="halt_on_error=1:second_deadlock_stack=1" \
    exec env "$prevar=$outer_pre${SP_PRELOAD_EXTRA:+${outer_pre:+:}$SP_PRELOAD_EXTRA}" \
    $launch "$bin" --port "$SP_PORT" --bind 127.0.0.1 --dir "$dir" --save "" --appendonly no \
         --enable-module-command local ${SP_SERVER_ARGS:-} --loadmodule "${SP_MODULE:-$HERE/fractalsql.so}" "$@" \
    ) >"$SP_LOG" 2>&1 &
  SP_PID=$!
  ALL_PIDS+=("$SP_PID")
  local i
  for i in $(seq 1 100); do
    if "$PY" "$HERE/tests/bt_gates.py" ping --port "$SP_PORT" >/dev/null 2>&1; then return 0; fi
    kill -0 "$SP_PID" 2>/dev/null || break
    sleep 0.1
  done
  echo "server '$name' did not come up; log:" >&2
  tail -20 "$SP_LOG" >&2
  return 1
}

start_server() {
  spawn_server main || return 1
  SERVER_PID="$SP_PID"; PORT="$SP_PORT"
}

# Runs one python gate and folds its PASS/FAIL/KNOWN lines into the totals.
run_py_gate() {
  local gate="$1" label="$2" gport="${3:-$PORT}" gpid="${4:-$SERVER_PID}" glog="${5:-$TMPROOT/server_main.log}"
  local out line status=0
  out="$("$PY" "$HERE/tests/bt_gates.py" "$gate" --port "$gport" --pid "$gpid" 2>&1)" || status=$?
  while IFS= read -r line; do
    case "$line" in
      PASS\ *)  pass  "${line#PASS }" ;;
      FAIL\ *)  fail  "${line#FAIL }" ;;
      KNOWN\ *) known "${line#KNOWN }" ;;
      "")       ;;
      *)        printf '         %s\n' "$line" ;;
    esac
  done <<< "$out"
  if [ "$status" -ne 0 ] && ! grep -q '^FAIL ' <<< "$out"; then
    fail "$gate $label: the gate script exited $status without a verdict"
  fi
  if ! kill -0 "$gpid" 2>/dev/null; then
    fail "$gate $label: the server process died (see the log below)"
    tail -20 "$glog" | sed 's/^/         /'
  fi
}

# Runs one python driver script and folds its PASS/FAIL/KNOWN lines into the
# totals. Used for the checks that need a driver of their own (the audit
# trail's storage has one, so the gate file does not grow another hundred
# lines of python).
run_py_driver() {
  local script="$1" label="$2"; shift 2
  local out line status=0
  out="$("$PY" "$HERE/$script" "$@" 2>&1)" || status=$?
  while IFS= read -r line; do
    case "$line" in
      PASS\ *)  pass  "${line#PASS }" ;;
      FAIL\ *)  fail  "${line#FAIL }" ;;
      KNOWN\ *) known "${line#KNOWN }" ;;
      "")       ;;
      *)        printf '         %s\n' "$line" ;;
    esac
  done <<< "$out"
  if [ "$status" -ne 0 ] && ! grep -q '^FAIL ' <<< "$out"; then
    fail "$label: the driver script exited $status without a verdict"
  fi
}

# ---------------------------------------------------------------------------
# gates
# ---------------------------------------------------------------------------
# The sanitizer flavor being tested.
build_flavor() {
  local f="plain"
  [ "$ASAN" -eq 1 ]  && f="asan"
  [ "$UBSAN" -eq 1 ] && f="${f/plain/}ubsan"
  [ "$TSAN" -eq 1 ]  && f="tsan"
  [ "$COVERAGE" -eq 1 ] && f="coverage"
  echo "$f"
}

# A build is only reused if it was made for the flavor being tested: a
# sanitizer-instrumented module left over from an earlier run must never be
# loaded by a plain run (or the reverse), or the run tests the wrong binary.
build_needed() {
  [ -f "$HERE/fractalsql.so" ] || return 0
  [ "$REBUILD" -eq 1 ] && return 0
  [ "$PREBUILT" -eq 1 ] && return 1
  [ "$(cat "$HERE/.build_test_flavor" 2>/dev/null)" != "$(build_flavor)" ] && return 0
  # A source, header, vendored core archive, VERSION or the Makefile newer than the module means it is stale: without
  # this a bumped VERSION or an edited command was tested against the old binary.
  [ -n "$(find "$HERE/src" "$HERE/include" \( -name '*.c' -o -name '*.h' -o -name '*.a' \) -newer "$HERE/fractalsql.so" -print -quit 2>/dev/null)" ] && return 0
  [ "$HERE/VERSION" -nt "$HERE/fractalsql.so" ] || [ "$HERE/Makefile" -nt "$HERE/fractalsql.so" ]
}

gate_01_build() {
  local need=0 flavor stamp="$HERE/.build_test_flavor"
  flavor="$(build_flavor)"
  build_needed && need=1
  [ "$PREBUILT" -eq 1 ] && [ "$flavor" != "plain" ] && { fail "01 build: --prebuilt cannot be combined with a sanitizer flag"; return; }
  if [ "$need" -eq 1 ]; then
    local san="" ld="-shared -Wl,--exclude-libs,ALL -lm -ldl -lpthread"
    if [ "$(uname -s)" = "Darwin" ]; then
      ld="-dynamiclib -undefined dynamic_lookup -Wl,-exported_symbol,_RedisModule_OnLoad -lm -lpthread"
    fi
    [ "$ASAN" -eq 1 ]  && san="$san -fsanitize=address"
    [ "$UBSAN" -eq 1 ] && san="$san -fsanitize=undefined -fno-sanitize-recover=undefined"
    [ "$TSAN" -eq 1 ]  && san="$san -fsanitize=thread"
    [ "$COVERAGE" -eq 1 ] && san="$san --coverage -fprofile-update=atomic"
    [ "$COVERAGE" -eq 1 ] && find "$HERE/src" -name '*.gc??' -delete
    rm -f "$HERE/fractalsql.so" "$HERE"/src/*.o
    local log="$TMPROOT/build.log"
    if [ -n "$san" ]; then
      if ! make -C "$HERE" CFLAGS="-Wall -Wextra -O1 -g -fPIC -fno-omit-frame-pointer -Iinclude$san" \
              LDFLAGS="$ld$san" >"$log" 2>&1; then
        fail "01 build: make failed${SAN_NOTE:+ ($SAN_NOTE)}"; sed 's/^/         /' "$log" | tail -15; return
      fi
    elif ! make -C "$HERE" >"$log" 2>&1; then
      fail "01 build: make failed"; sed 's/^/         /' "$log" | tail -15; return
    fi
  fi
  [ -f "$HERE/fractalsql.so" ] || { fail "01 build: fractalsql.so was not produced"; return; }
  [ "$PREBUILT" -eq 1 ] || printf '%s' "$flavor" > "$stamp"
  pass "01 build: fractalsql.so built${SAN_NOTE:+ ($SAN_NOTE)}"

  # The version lives in ./VERSION. Files that cannot read it at build time
  # carry a copy, so each must agree or the release ships mismatched labels.
  local ver drift=""; ver="$(tr -d '[:space:]' < "$HERE/VERSION")"
  for f in packaging/debian/postinst packaging/rpm/fractalsql.spec \
           .github/workflows/release.yml tests/smoke.sh; do
    grep -q "$ver" "$HERE/$f" || drift="$drift $f"
  done
  if [ -z "$drift" ]; then pass "01 build: VERSION $ver agrees with the packaging, CI and smoke files"
  else fail "01 build: VERSION is $ver but these files do not mention it:$drift"; fi

  if [ "$(uname -s)" = "Darwin" ]; then
    skip "01 build: symbol scan is Linux-only (Mach-O exports differ)"
  elif ! command -v nm >/dev/null 2>&1; then
    skip "01 build: no nm on PATH, symbol scan skipped"
  else
    # The static core archive must not leak into the server's symbol table:
    # no fsql_* / fsqli_* exports. (RedisModule_* are the API function-pointer
    # variables redismodule.h defines, and RedisModule_OnLoad is the entry.)
    local leaked
    leaked="$(nm -D --defined-only "$HERE/fractalsql.so" | awk '{print $3}' \
      | grep -E '^(fsql_|fsqli_)' | sort -u | tr '\n' ' ')"
    if [ -z "$leaked" ]; then
      pass "01 build: the core's fsql_* symbols are not exported"
    else
      fail "01 build: the core leaks into the server's symbol table (${leaked%% *} ...); link with -Wl,--exclude-libs,ALL"
    fi
    # (captured first: grep -q closing the pipe early would trip pipefail)
    local exports; exports="$(nm -D --defined-only "$HERE/fractalsql.so" | awk '{print $3}')"
    if grep -qx RedisModule_OnLoad <<< "$exports"; then
      pass "01 build: RedisModule_OnLoad is exported"
    else
      fail "01 build: RedisModule_OnLoad is not exported"
    fi
    # Every undefined symbol must come from the server (RedisModule_*), a
    # sanitizer runtime, or libc/libm/libdl/libpthread, checked against what
    # those libraries actually export rather than a hand-kept list.
    local provided="$TMPROOT/provided.syms" lib path und
    : > "$provided"
    for lib in libc.so.6 libm.so.6 libdl.so.2 libpthread.so.0; do
      path="$(cc -print-file-name="$lib" 2>/dev/null)"
      [ -f "$path" ] && nm -D --defined-only "$path" 2>/dev/null | awk '{print $3}' | sed 's/@.*//' >> "$provided"
    done
    und="$(nm -D --undefined-only "$HERE/fractalsql.so" | awk '{print $2}' | sed 's/@.*//' \
      | grep -vE '^(RedisModule_|_ITM_|__gmon_start__|__cxa_finalize|__tls_get_addr|__stack_chk_guard|__asan|__ubsan|__sanitizer|__tsan)' \
      | sort -u | grep -vxFf "$provided" | tr '\n' ' ')"
    if [ -z "$und" ]; then
      pass "01 build: every undefined symbol is provided by the server or libc/libm"
    else
      fail "01 build: unresolved symbols: $und"
    fi
  fi
}

# Anti-false-pass: a sanitizer run only means something if the runtime really
# is inside the server process. Without this, a failed LD_PRELOAD would test
# the uninstrumented module and report green.
gate_sanitizer_loaded() {
  [ "$ASAN" -eq 1 ] || [ "$UBSAN" -eq 1 ] || [ "$TSAN" -eq 1 ] || return 0
  [ -r "/proc/$SERVER_PID/maps" ] || { skip "sanitizer: cannot read /proc/$SERVER_PID/maps"; return; }
  if [ "$ASAN" -eq 1 ]; then
    if grep -q 'libasan' "/proc/$SERVER_PID/maps"; then pass "sanitizer: the ASan runtime is loaded in the server"
    else fail "sanitizer: --asan but libasan is not in the server process (the run would be vacuous)"; fi
  fi
  if [ "$UBSAN" -eq 1 ]; then
    if grep -q 'libubsan' "/proc/$SERVER_PID/maps"; then pass "sanitizer: the UBSan runtime is loaded in the server"
    else fail "sanitizer: --ubsan but libubsan is not in the server process (the run would be vacuous)"; fi
  fi
  if [ "$TSAN" -eq 1 ]; then
    if grep -q 'libtsan' "/proc/$SERVER_PID/maps"; then pass "sanitizer: the TSan runtime is loaded in the server"
    else fail "sanitizer: --tsan but libtsan is not in the server process (the run would be vacuous)"; fi
  fi
}

gate_02_smoke() {
  FSQL_EXPECT_VERSION="$(tr -d '[:space:]' < "$HERE/VERSION")" run_py_gate 02 smoke
}
gate_03_scout() { run_py_gate 03 scout; }
gate_04_bounds() { run_py_gate 04 bounds; }
gate_05_survival() { run_py_gate 05 survival; }
gate_06_vector()   { run_py_gate 06 vector; }
gate_07_analytics() { run_py_gate 07 analytics; }
gate_08_async()    { run_py_gate 08 async; }

# A bad load option must stop the server from starting with the module, not
# be ignored. The server is expected to exit quickly with a failure.
expect_load_refused() {
  local label="$1"; shift
  local bin; bin="$(pick_server)"
  local dir="$TMPROOT/data_bad"; mkdir -p "$dir"
  local port; port="$(free_port)"
  ( cd "$dir" && LD_PRELOAD="$(server_preload)" ASAN_OPTIONS="detect_leaks=0" \
      exec "$bin" --port "$port" --bind 127.0.0.1 --dir "$dir" --save "" \
      --loadmodule "$HERE/fractalsql.so" "$@" ) >"$TMPROOT/bad.log" 2>&1 &
  local pid=$! i
  for i in $(seq 1 50); do kill -0 "$pid" 2>/dev/null || break; sleep 0.1; done
  if kill -0 "$pid" 2>/dev/null; then
    kill -9 "$pid" 2>/dev/null; wait "$pid" 2>/dev/null
    fail "${ELR_PREFIX:-09 limits}: load options '$*' were accepted ($label)"
  else
    wait "$pid" 2>/dev/null
    if [ "$?" -ne 0 ]; then pass "${ELR_PREFIX:-09 limits}: load options '$*' are refused ($label)"
    else fail "${ELR_PREFIX:-09 limits}: '$*' ended the server with exit status 0 ($label)"; fi
  fi
}

gate_09_limits() {
  spawn_server limits threads 2 max-inflight-mb 1 || { fail "09 limits: could not start the limited server"; return; }
  run_py_gate 09 limits "$SP_PORT" "$SP_PID" "$SP_LOG"
  expect_load_refused "zero threads" threads 0
  expect_load_refused "non-numeric threads" threads many
  expect_load_refused "too many threads" threads 65
  expect_load_refused "negative cap" max-inflight-mb -1
  expect_load_refused "unknown option" bogus 1
  expect_load_refused "option without a value" threads
  expect_load_refused "agent-partial-reasoning-fail needs yes or no" agent-partial-reasoning-fail maybe
  expect_load_refused "zero max-sessions" max-sessions 0
  expect_load_refused "too many max-sessions" max-sessions 5000
  expect_load_refused "non-numeric max-sessions" max-sessions lots
  expect_load_refused "negative session-ttl-sec" session-ttl-sec -1
  expect_load_refused "non-numeric session-ttl-sec" session-ttl-sec forever
  # a session cap of 2 is enforced, and a 1-second TTL drops an idle session
  if spawn_server sessions max-sessions 2 session-ttl-sec 1; then
    FSQL_PHASE=sessions run_py_gate 09 limits "$SP_PORT" "$SP_PID" "$SP_LOG"
  else
    fail "09 limits: could not start a server with max-sessions 2"
  fi
  local v
  for v in yes no; do
    if spawn_server "partial_$v" agent-partial-reasoning-fail "$v"; then
      pass "09 limits: agent-partial-reasoning-fail $v is accepted and the module loads"
      stop_server "$SP_PID"
    else
      fail "09 limits: agent-partial-reasoning-fail $v was refused"
    fi
  done
}


stop_server() {   # stop_server PID: graceful, then wait
  kill "$1" 2>/dev/null
  local i
  for i in $(seq 1 100); do kill -0 "$1" 2>/dev/null || return 0; sleep 0.1; done
  kill -9 "$1" 2>/dev/null
}

gate_11_oom() {
  if [ "$ASAN" -eq 1 ] || [ "$UBSAN" -eq 1 ] || [ "$TSAN" -eq 1 ]; then
    skip "11 oom (runs on the uninstrumented build; the sanitizer runs cover other ground)"; return
  fi
  # A separate module built with the allocation-failure wrappers
  # (-DFV_OOM_INJECT, src/fv_oom.c). The shipped module is untouched.
  local ver plat so="$TMPROOT/fractalsql_oom.so" ldf core
  ver="$(tr -d '[:space:]' < "$HERE/VERSION")"
  plat="$(uname -s | tr '[:upper:]' '[:lower:]')-$(uname -m)"
  # The injection module links the same core archive the shipped module was
  # built from, so it loads on the host that built it.
  core="$HERE/include/$plat/libfractalsql-${CORE_VARIANT:-community-sovereign-c}.a"
  [ -f "$core" ] || { fail "11 oom: no core archive for this host ($core)"; return; }
  ldf="-shared -Wl,--exclude-libs,ALL"
  [ "$(uname -s)" = "Darwin" ] && ldf="-dynamiclib -undefined dynamic_lookup"
  local covf="" f
  if [ "$COVERAGE" -eq 1 ]; then   # the injection flavor is instrumented too; the coverage report unions its counters with the shipped flavor's.
    # Its counters have to outlive this run's temporary directory, so the module is built in .oomcov/ (a coverage-only directory).
    covf="--coverage -fprofile-update=atomic"
    mkdir -p "$HERE/.oomcov"
    so="$HERE/.oomcov/fractalsql_oom.so"
    for f in "$HERE"/.oomcov/fractalsql_oom.so-*.gcno "$HERE"/.oomcov/fractalsql_oom.so-*.gcda; do [ -e "$f" ] && rm -f "$f"; done
  fi
  if ! cc -O1 -g -fPIC $ldf $covf -DFV_OOM_INJECT "-DFRACTALSQL_VERSION=\"$ver\"" -I"$HERE/include" \
        "$HERE"/src/*.c "$core" \
        -lm -ldl -lpthread -o "$so" 2>"$TMPROOT/oom_build.log"; then
    sed 's/^/         /' "$TMPROOT/oom_build.log" | tail -15; fail "11 oom: the injection build failed"; return
  fi
  SP_MODULE="$so" spawn_server oom threads 2 || { fail "11 oom: could not start the server"; return; }
  run_py_gate 11 oom "$SP_PORT" "$SP_PID" "$SP_LOG"
}

gate_12_acl() { run_py_gate 12 acl; }

gate_13_persist() {
  spawn_server persist || { fail "13 persist: could not start the server"; return; }
  local pid1="$SP_PID"
  FSQL_PHASE=write run_py_gate 13 persist "$SP_PORT" "$SP_PID" "$SP_LOG"
  stop_server "$pid1"
  spawn_server persist || { fail "13 persist: the server did not come back up after a restart"; tail -5 "$SP_LOG" | sed 's/^/         /'; return; }
  FSQL_PHASE=read run_py_gate 13 persist "$SP_PORT" "$SP_PID" "$SP_LOG"
  local pport="$SP_PORT" ppid="$SP_PID"
  SP_SERVER_ARGS="--replicaof 127.0.0.1 $pport" spawn_server replica || { fail "13 persist: the replica did not start"; return; }
  local rport="$SP_PORT"
  FSQL_PHASE=replica FSQL_REPLICA_PORT="$rport" run_py_gate 13 persist "$pport" "$ppid" "$TMPROOT/server_persist.log"
}

gate_14_versions() {
  if [ "$ASAN" -eq 1 ] || [ "$UBSAN" -eq 1 ] || [ "$TSAN" -eq 1 ]; then
    skip "14 versions (run on the uninstrumented build)"; return
  fi
  if ! command -v docker >/dev/null 2>&1 || ! docker info >/dev/null 2>&1; then
    skip "14 versions (docker is not available)"; return
  fi
  local tag id bin
  for tag in 7.2 8.0 8.1; do
    bin="$TMPROOT/valkey-server-$tag"
    if ! timeout 180 docker pull -q "valkey/valkey:$tag" >/dev/null 2>&1; then
      skip "14 versions: could not pull valkey/valkey:$tag"; continue
    fi
    id="$(docker create "valkey/valkey:$tag" 2>/dev/null)"
    docker cp "$id:/usr/local/bin/valkey-server" "$bin" >/dev/null 2>&1; docker rm "$id" >/dev/null 2>&1
    if [ ! -x "$bin" ]; then skip "14 versions: no valkey-server in valkey/valkey:$tag"; continue; fi
    if SP_BIN="$bin" spawn_server "v$tag" 2>/dev/null; then
      local ver; ver="$("$bin" --version | sed -E 's/.*v=([^ ]+).*/\1/')"
      pass "14 versions: the module loads into Valkey $ver"
      FSQL_EXPECT_VERSION="$(tr -d '[:space:]' < "$HERE/VERSION")" run_py_gate 02 "versions($tag)" "$SP_PORT" "$SP_PID" "$SP_LOG"
      run_py_gate 06 "versions($tag)" "$SP_PORT" "$SP_PID" "$SP_LOG"
      run_py_gate 08 "versions($tag)" "$SP_PORT" "$SP_PID" "$SP_LOG"
    else
      skip "14 versions: the Valkey $tag binary could not start on this host (missing shared libraries?)"
    fi
  done
  gate_14_bundle
}

# Coexistence with the official valkey-bundle image (valkey-search, valkey-json,
# valkey-bloom, valkey-ldap). FractalSQL does not need any of them; this proves the
# module loads next to them and the core gates still pass, because the README tells
# people to run valkey-search beside it. Uses the container-built dist/ module (the
# bundle is Debian; the host build may need a newer glibc than the image has).
gate_14_bundle() {
  local so="$HERE/dist/amd64/fractalsql.so" cid port mods
  if [ "$REBUILD" -eq 1 ] && [ "$(uname -s)" = "Linux" ] && [ "$(uname -m)" = "x86_64" ] && command -v docker >/dev/null 2>&1; then
    echo "  14 bundle: --rebuild: refreshing dist/amd64 with ./build.sh amd64 (docker; this takes a while)"
    if ! ( cd "$HERE" && ./build.sh amd64 ) >"$TMPROOT/build_dist.log" 2>&1; then
      tail -15 "$TMPROOT/build_dist.log" >&2; fail "14 bundle: --rebuild could not build dist/amd64/fractalsql.so"; return
    fi
  fi
  if [ ! -f "$so" ]; then skip "14 bundle (no container-built dist/amd64/fractalsql.so; run ./build.sh amd64 or ./build_test.sh --rebuild)"; return; fi
  if ! grep -qaF "$(tr -d '[:space:]' < "$HERE/VERSION")" "$so"; then
    skip "14 bundle: dist/amd64/fractalsql.so does not carry VERSION $(tr -d '[:space:]' < "$HERE/VERSION") (stale; run ./build_test.sh --rebuild or ./build.sh amd64)"; return
  fi
  if ! timeout 180 docker pull -q valkey/valkey-bundle:8.1 >/dev/null 2>&1; then
    skip "14 bundle: could not pull valkey/valkey-bundle:8.1"; return
  fi
  cid="$(docker run -d -p 127.0.0.1::6379 -v "$so:/modules/fractalsql.so:ro" valkey/valkey-bundle:8.1 \
         valkey-server --loadmodule /modules/fractalsql.so 2>/dev/null)"
  if [ -z "$cid" ]; then skip "14 bundle: the bundle container did not start"; return; fi
  port="$(docker port "$cid" 6379/tcp 2>/dev/null | head -1 | sed 's/.*://')"
  local i up=0
  for i in $(seq 1 40); do
    if [ -n "$port" ] && "$PY" "$HERE/tests/bt_gates.py" ping --port "$port" >/dev/null 2>&1; then up=1; break; fi
    sleep 0.25
  done
  if [ "$up" -eq 1 ]; then
    mods="$(docker exec "$cid" valkey-cli MODULE LIST 2>/dev/null | grep -A1 '^name' | grep -v '^--' | grep -v '^name' | tr '\n' ' ')"
    for m in fractalsql search json; do
      case " $mods " in *" $m "*) pass "14 bundle: module '$m' is loaded next to the others" ;; *) fail "14 bundle: module '$m' is not loaded (got: $mods)" ;; esac
    done
    # the harness's own pid stands in for the server's: the container's process is not ours to signal
    FSQL_EXPECT_VERSION="$(tr -d '[:space:]' < "$HERE/VERSION")" run_py_gate 02 "bundle" "$port" "$$"
    run_py_gate 06 "bundle" "$port" "$$"
    if docker exec "$cid" valkey-cli FT.CREATE fsql_coexist ON HASH PREFIX 1 c: SCHEMA v VECTOR HNSW 6 TYPE FLOAT32 DIM 4 DISTANCE_METRIC COSINE 2>/dev/null | grep -q OK \
       && docker exec "$cid" valkey-cli JSON.SET fsql_j '$' '{"a":1}' 2>/dev/null | grep -q OK \
       && [ "$(docker exec "$cid" valkey-cli FRACTAL.EDITION 2>/dev/null)" = "Community" ]; then
      pass "14 bundle: valkey-search, valkey-json and FractalSQL all answer on one server"
    else
      fail "14 bundle: a search/json/fractalsql command failed on the shared server"
    fi
  else
    fail "14 bundle: the server in the bundle container did not come up"; docker logs "$cid" 2>&1 | tail -5 | sed 's/^/         /'
  fi
  docker rm -f "$cid" >/dev/null 2>&1
}


# --- fractalsqld + mock LLM helpers (gates 15, 16) -------------------------
find_daemon() {
  if [ -n "$FSQLD_BIN" ]; then echo "$FSQLD_BIN"; return; fi
  if [ -n "${FSQL_DAEMON:-}" ]; then echo "$FSQL_DAEMON"; return; fi
  local d="$HERE/service/build/fractalsqld"
  [ -x "$d" ] && echo "$d"
}

# start_mock_llm: serves chat completions and embeddings on a free port.
start_mock_llm() {
  MOCK_PORT="$(free_port)"
  "$PY" "$HERE/scripts/ci/mock_llm.py" "$MOCK_PORT" >"$TMPROOT/mock_llm.log" 2>&1 &
  MOCK_PID=$!
  ALL_PIDS+=("$MOCK_PID")
  local i
  for i in $(seq 1 50); do
    (exec 3<>"/dev/tcp/127.0.0.1/$MOCK_PORT") 2>/dev/null && { exec 3>&-; return 0; }
    sleep 0.1
  done
  return 1
}

# start_daemon NAME: a fractalsqld on a private socket with a private key.
# Sets D_SOCK, D_KEY, D_PID. Re-running with the same NAME restarts it with the
# same key and socket (the module keeps what it read at load).
start_daemon() {
  local name="$1" fsqld plat plug
  fsqld="$(find_daemon)" || return 1
  plat="$(uname -s | tr '[:upper:]' '[:lower:]')-$(uname -m)"
  plug="$(cd "$HERE/include/$plat" && pwd -P)/fractalsql-reasoning-http.so"
  D_DIR="$TMPROOT/daemon_$name"; mkdir -p "$D_DIR"
  D_SOCK="$D_DIR/fsqld.sock"; D_KEY="$D_DIR/key"
  [ -s "$D_KEY" ] || "$PY" -c 'import secrets; print(secrets.token_hex(32))' > "$D_KEY"
  chmod 600 "$D_KEY"
  printf 'socket_path = %s\nhmac_key_file = %s\nallowed_uids = %s\n' "$D_SOCK" "$D_KEY" "$(id -u)" > "$D_DIR/fsqld.conf"
  # The vendored reasoning plugin and the mock enterprise core ship unsigned; the daemon's
  # product default (refuse unsigned native code) is the daemon's own default.
  if [ -n "${FRACTALSQL_ENTERPRISE_LIB:-}" ]; then
    printf 'enterprise_require_signature = 0\n' >> "$D_DIR/fsqld.conf"
  fi
  chmod 600 "$D_DIR/fsqld.conf"
  rm -f "$D_SOCK"
  ( cd "$D_DIR" && \
    { if [ -n "${D_RESPONSE_MODE:-}" ]; then export FSQL_REASONING_HTTP_RESPONSE_MODE="$D_RESPONSE_MODE"; fi; } && \
    FSQL_REASONING_REQUIRE_SIGNATURE=0 \
    FRACTALSQL_REASONING_PLUGIN="$plug" \
    FRACTALSQL_HTTP_URL="http://127.0.0.1:$MOCK_PORT/v1/chat/completions" \
    FRACTALSQL_HTTP_EMBED_URL="http://127.0.0.1:$MOCK_PORT/v1/embeddings" \
    FRACTALSQL_HTTP_ALLOW_PLAINTEXT=1 \
    exec "$fsqld" -c "$D_DIR/fsqld.conf" </dev/null ) >>"$D_DIR/fsqld.log" 2>&1 &
  D_PID=$!
  ALL_PIDS+=("$D_PID")
  local i
  for i in $(seq 1 100); do [ -S "$D_SOCK" ] && return 0; sleep 0.1; done
  echo "fractalsqld did not open $D_SOCK:" >&2; tail -5 "$D_DIR/fsqld.log" >&2
  return 1
}

gate_15_daemon() {
  if [ -z "$(find_daemon)" ]; then
    skip "15 daemon (no fractalsqld: pass --fractalsqld PATH, set FSQL_DAEMON, or run make -C service daemon)"; return
  fi
  # (a) a module loaded without daemon options answers 'not configured'.
  FSQL_PHASE=unconfigured run_py_gate 15 daemon
  start_mock_llm || { fail "15 daemon: the mock LLM did not start"; return; }
  start_daemon community || { fail "15 daemon: fractalsqld did not start"; return; }
  spawn_server daemon daemon-socket "$D_SOCK" daemon-key-file "$D_KEY" daemon-timeout-s 20 \
    || { fail "15 daemon: could not start the server with the daemon configured"; return; }
  local sport="$SP_PORT" spid="$SP_PID" slog="$SP_LOG"
  FSQL_PHASE=up run_py_gate 15 daemon "$sport" "$spid" "$slog"
  # (b) the daemon dies: clear errors, the rest of the module keeps working.
  stop_server "$D_PID"
  FSQL_PHASE=down run_py_gate 15 daemon "$sport" "$spid" "$slog"
  # (c) and it comes back without restarting the server.
  start_daemon community || { fail "15 daemon: fractalsqld did not restart"; return; }
  FSQL_PHASE=back run_py_gate 15 daemon "$sport" "$spid" "$slog"
  # (d) a server holding the WRONG key must be refused by the authenticated reply path.
  local wk="$TMPROOT/wrong.key"
  "$PY" -c 'import secrets; print(secrets.token_hex(32))' > "$wk"; chmod 600 "$wk"
  spawn_server wrongkey daemon-socket "$D_SOCK" daemon-key-file "$wk" daemon-timeout-s 5 \
    || { fail "15 daemon: could not start the wrong-key server"; return; }
  FSQL_PHASE=wrongkey run_py_gate 15 daemon "$SP_PORT" "$SP_PID" "$SP_LOG"
  # (e) bad daemon load options refuse to load.
  local old="${ELR_PREFIX:-}"; ELR_PREFIX="15 daemon"
  expect_load_refused "socket without a key" daemon-socket "$D_SOCK"
  expect_load_refused "key without a socket" daemon-key-file "$D_KEY"
  expect_load_refused "relative socket path" daemon-socket rel.sock daemon-key-file "$D_KEY"
  expect_load_refused "missing key file" daemon-socket "$D_SOCK" daemon-key-file "$TMPROOT/none.key"
  cp "$D_KEY" "$TMPROOT/open.key"; chmod 644 "$TMPROOT/open.key"
  expect_load_refused "key file readable by others" daemon-socket "$D_SOCK" daemon-key-file "$TMPROOT/open.key"
  printf 'not-hex-at-all\n' > "$TMPROOT/bad.key"; chmod 600 "$TMPROOT/bad.key"
  expect_load_refused "non-hex key file" daemon-socket "$D_SOCK" daemon-key-file "$TMPROOT/bad.key"
  expect_load_refused "bad timeout" daemon-socket "$D_SOCK" daemon-key-file "$D_KEY" daemon-timeout-s 0
  ELR_PREFIX="$old"
  # (f) response modes: FRACTAL.REASON follows the daemon's FSQL_REASONING_HTTP_RESPONSE_MODE (text by default,
  # code or json when the operator sets it); FRACTAL.EMBED is unaffected by it.
  local mode
  for mode in code json; do
    D_RESPONSE_MODE="$mode" start_daemon "mode_$mode" || { fail "15 daemon: fractalsqld ($mode mode) did not start"; return; }
    spawn_server "mode_$mode" daemon-socket "$D_SOCK" daemon-key-file "$D_KEY" daemon-timeout-s 20 \
      || { fail "15 daemon: could not start the server for $mode mode"; return; }
    FSQL_PHASE="mode_$mode" run_py_gate 15 daemon "$SP_PORT" "$SP_PID" "$SP_LOG"
  done
}

gate_16_enterprise() {
  if [ -z "$(find_daemon)" ]; then skip "16 enterprise (no fractalsqld, see gate 15)"; return; fi
  local plat ent
  plat="$(uname -s | tr '[:upper:]' '[:lower:]')-$(uname -m)"
  ent="${FSQL_ENTERPRISE_LIB:-$HERE/include/$plat/libfractalsql-enterprise-sovereign-c.so}"
  if [ ! -f "$ent" ]; then
    skip "16 enterprise (no enterprise library: set FSQL_ENTERPRISE_LIB; the library is licensed and is never vendored here)"; return
  fi
  local ledger="$TMPROOT/ledger16.dat"; rm -f "$ledger"
  start_mock_llm || { fail "16 enterprise: the mock LLM did not start"; return; }
  FRACTALSQL_ENTERPRISE_LIB="$ent" FRACTALSQL_ENTERPRISE_LEDGER_PATH="$ledger" start_daemon ent \
    || { fail "16 enterprise: fractalsqld did not start with the enterprise library"; return; }
  spawn_server ent daemon-socket "$D_SOCK" daemon-key-file "$D_KEY" daemon-timeout-s 20 \
    || { fail "16 enterprise: could not start the server"; return; }
  local sport="$SP_PORT" spid="$SP_PID" slog="$SP_LOG"
  FSQL_PHASE=ent run_py_gate 16 enterprise "$sport" "$spid" "$slog"
  [ -s "$ledger" ] && pass "16 enterprise: the ledger file really was written" || fail "16 enterprise: no ledger file was written"
  # cross-process rehydration: restart the daemon, then load must read the file
  stop_server "$D_PID"
  FRACTALSQL_ENTERPRISE_LIB="$ent" FRACTALSQL_ENTERPRISE_LEDGER_PATH="$ledger" start_daemon ent \
    || { fail "16 enterprise: fractalsqld did not restart"; return; }
  FSQL_PHASE=ent2 run_py_gate 16 enterprise "$sport" "$spid" "$slog"
  # tamper with the file: verify must notice, load must refuse
  "$PY" - "$ledger" <<'TAMPER'
import sys
with open(sys.argv[1], "r+b") as f:
    f.seek(-5, 2); b = f.read(1); f.seek(-5, 2); f.write(bytes([b[0] ^ 0xFF]))
TAMPER
  FSQL_PHASE=ent3 run_py_gate 16 enterprise "$sport" "$spid" "$slog"
  # The audit trail's own storage: the head file next to the ledger, the
  # anchor log, the redaction of the text that goes into the chain, and the
  # permissions of the files the ledger writes.
  run_py_driver tests/bt_ledger_atrest.py anchor-off \
    --port "$sport" --ledger "$ledger" --daemon-log "$TMPROOT/daemon_ent/fsqld.log"
  # A daemon with a ledger key and the anchor log on stderr: the head
  # record's MAC, the anchor lines, and the file permissions.
  local ledger_at="$TMPROOT/ledger16at.dat"; rm -f "$ledger_at" "$ledger_at.csv" "$ledger_at.head"
  local key_at; key_at="$("$PY" -c 'import secrets; print(secrets.token_hex(32))')"
  FRACTALSQL_ENTERPRISE_LIB="$ent" FRACTALSQL_ENTERPRISE_LEDGER_PATH="$ledger_at" \
  FRACTALSQL_ENTERPRISE_LEDGER_KEY="$key_at" FSQL_LEDGER_ANCHOR_LOG=stderr \
    start_daemon ent_at || { fail "16 enterprise: fractalsqld did not start for the audit-trail storage checks"; return; }
  spawn_server ent_at daemon-socket "$D_SOCK" daemon-key-file "$D_KEY" daemon-timeout-s 20 \
    || { fail "16 enterprise: could not start the server for the audit-trail storage checks"; return; }
  run_py_driver tests/bt_ledger_atrest.py atrest \
    --port "$SP_PORT" --ledger "$ledger_at" --daemon-log "$TMPROOT/daemon_ent_at/fsqld.log"
  # The same chain with the audit text redacted instead of stored verbatim.
  local ledger_rh="$TMPROOT/ledger16red.dat"; rm -f "$ledger_rh" "$ledger_rh.csv" "$ledger_rh.head"
  FRACTALSQL_ENTERPRISE_LIB="$ent" FRACTALSQL_ENTERPRISE_LEDGER_PATH="$ledger_rh" FSQL_AUDIT_REDACT=hash \
    start_daemon ent_rh || { fail "16 enterprise: fractalsqld did not start for the redaction checks"; return; }
  spawn_server ent_rh daemon-socket "$D_SOCK" daemon-key-file "$D_KEY" daemon-timeout-s 20 \
    || { fail "16 enterprise: could not start the server for the redaction checks"; return; }
  run_py_driver tests/bt_ledger_atrest.py redact-hash \
    --port "$SP_PORT" --ledger "$ledger_rh" --daemon-log "$TMPROOT/daemon_ent_rh/fsqld.log"
  local ledger_rt="$TMPROOT/ledger16redt.dat"; rm -f "$ledger_rt" "$ledger_rt.csv" "$ledger_rt.head"
  FRACTALSQL_ENTERPRISE_LIB="$ent" FRACTALSQL_ENTERPRISE_LEDGER_PATH="$ledger_rt" FSQL_AUDIT_REDACT=truncate \
    start_daemon ent_rt || { fail "16 enterprise: fractalsqld did not start for the truncation checks"; return; }
  spawn_server ent_rt daemon-socket "$D_SOCK" daemon-key-file "$D_KEY" daemon-timeout-s 20 \
    || { fail "16 enterprise: could not start the server for the truncation checks"; return; }
  run_py_driver tests/bt_ledger_atrest.py redact-truncate \
    --port "$SP_PORT" --ledger "$ledger_rt" --daemon-log "$TMPROOT/daemon_ent_rt/fsqld.log"
  # The multimodal optimizers audit every call into the decision ledger, so they get their own daemon and ledger
  # (the checks above count records and tamper with the file).
  local ledger_mm="$TMPROOT/ledger16mm.dat"; rm -f "$ledger_mm"
  FRACTALSQL_ENTERPRISE_LIB="$ent" FRACTALSQL_ENTERPRISE_LEDGER_PATH="$ledger_mm" start_daemon ent_mm \
    || { fail "16 enterprise: fractalsqld did not start for the optimizer checks"; return; }
  spawn_server ent_mm daemon-socket "$D_SOCK" daemon-key-file "$D_KEY" daemon-timeout-s 20 \
    || { fail "16 enterprise: could not start the server for the optimizer checks"; return; }
  FSQL_PHASE=mm run_py_gate 16 enterprise "$SP_PORT" "$SP_PID" "$SP_LOG"
  # the enterprise-qtl-audit demo against a fresh enterprise daemon
  local ledger_t="$TMPROOT/ledger16tour.dat"; rm -f "$ledger_t"
  FRACTALSQL_ENTERPRISE_LIB="$ent" FRACTALSQL_ENTERPRISE_LEDGER_PATH="$ledger_t" start_daemon ent_tour \
    && spawn_server ent_tour daemon-socket "$D_SOCK" daemon-key-file "$D_KEY" daemon-timeout-s 20 \
    || { fail "16 enterprise: could not start the server for the enterprise-qtl-audit demo"; return; }
  if (cd "$HERE/demo" && "$PY" enterprise-qtl-audit.py --port "$SP_PORT" --quiet >"$TMPROOT/tour_ledger.log" 2>&1); then
    pass "16 enterprise: the enterprise-qtl-audit demo runs clean against a real enterprise library"
  else
    tail -8 "$TMPROOT/tour_ledger.log" >&2; fail "16 enterprise: the enterprise-qtl-audit demo failed"
  fi
}

gate_18_agents() { run_py_gate 18 agents; }
gate_20_sessions() { run_py_gate 20 sessions; }

# 19: DATA_ANALYST against a real valkey-search index. Needs docker (for the 8.1 server and the bundle's
# search module), a fractalsqld, and the mock model.
gate_19_analyst() {
  if [ "$ASAN" -eq 1 ] || [ "$UBSAN" -eq 1 ] || [ "$TSAN" -eq 1 ]; then skip "19 analyst (run on the uninstrumented build)"; return; fi
  if [ -z "$(find_daemon)" ]; then skip "19 analyst (no fractalsqld)"; return; fi
  if ! command -v docker >/dev/null 2>&1 || ! docker info >/dev/null 2>&1; then skip "19 analyst (docker is not available)"; return; fi
  local bin="$TMPROOT/valkey-server-8.1" lib="$TMPROOT/libsearch.so" id
  if ! timeout 180 docker pull -q valkey/valkey-bundle:8.1 >/dev/null 2>&1; then skip "19 analyst: could not pull valkey/valkey-bundle:8.1"; return; fi
  id="$(docker create valkey/valkey-bundle:8.1 2>/dev/null)"
  docker cp "$id:/usr/local/bin/valkey-server" "$bin" >/dev/null 2>&1
  docker cp "$id:/usr/lib/valkey/libsearch.so" "$lib" >/dev/null 2>&1
  docker rm "$id" >/dev/null 2>&1
  if [ ! -x "$bin" ] || [ ! -f "$lib" ]; then skip "19 analyst: the bundle has no server or search module"; return; fi
  start_mock_llm || { fail "19 analyst: the mock LLM did not start"; return; }
  start_daemon analyst || { fail "19 analyst: fractalsqld did not start"; return; }
  if SP_BIN="$bin" SP_SERVER_ARGS="--loadmodule $lib" spawn_server analyst daemon-socket "$D_SOCK" daemon-key-file "$D_KEY" daemon-timeout-s 20; then
    run_py_gate 19 analyst "$SP_PORT" "$SP_PID" "$SP_LOG"
  else
    skip "19 analyst: the Valkey 8.1 server could not start with valkey-search on this host"
  fi
}

# 21: libFuzzer over tests/fuzz/fuzz_fv.c. Fuzz only (--fuzz): it builds its own instrumented binary from
# the sources, so it needs no server and no module build.

# --- the MCP server (mcp/): gates 22-25 ---------------------------------------
# The server is a plain C++17 client of the module. It is built once per run into the temp tree, with the
# active sanitizer, and every MCP gate uses that build.
# Homebrew's openssl@3 is keg-only on macOS: neither its headers nor its libraries are on the default search paths.
# Sets MCP_SSL_CPP / MCP_SSL_LD (empty elsewhere) for the server build and the probes.
MCP_SSL_CPP=""; MCP_SSL_LD=""
mcp_ssl_flags() {
  [ "$(uname -s)" = Darwin ] || return 0
  local p=""
  command -v brew >/dev/null 2>&1 && p="$(brew --prefix openssl@3 2>/dev/null || true)"
  [ -n "$p" ] && [ -d "$p/include/openssl" ] || for p in /opt/homebrew/opt/openssl@3 /usr/local/opt/openssl@3; do [ -d "$p/include/openssl" ] && break; p=""; done
  [ -n "$p" ] && { MCP_SSL_CPP="-I$p/include"; MCP_SSL_LD="-L$p/lib"; }
  return 0
}

MCP_EXE=""
mcp_build() {
  [ -n "$MCP_EXE" ] && [ -x "$MCP_EXE" ] && return 0
  local cxx=""; for c in "${CXX:-}" g++ c++ clang++; do [ -n "$c" ] && command -v "$c" >/dev/null 2>&1 && { cxx="$c"; break; }; done
  [ -n "$cxx" ] || { skip "mcp (no C++ compiler on PATH)"; return 1; }
  mcp_ssl_flags
  printf '#include <openssl/ssl.h>\nint main(){return 0;}\n' > "$TMPROOT/ssl_probe.cpp"
  "$cxx" $MCP_SSL_CPP -fsyntax-only "$TMPROOT/ssl_probe.cpp" >/dev/null 2>&1 || { skip "mcp (OpenSSL development headers not found: libssl-dev / openssl-devel)"; return 1; }
  local san=""
  [ "$ASAN" -eq 1 ] && san="-fsanitize=address -fno-omit-frame-pointer -g"
  [ "$UBSAN" -eq 1 ] && san="-fsanitize=undefined -fno-sanitize-recover=undefined -g"
  [ "$TSAN" -eq 1 ] && san="-fsanitize=thread -g"
  [ "$COVERAGE" -eq 1 ] && san="--coverage -fprofile-update=atomic"
  local out="$TMPROOT/mcp/fractalsql-valkey-mcp"
  if ! make -C "$HERE/mcp" CXX="$cxx" CXXFLAGS="-O1" CPPEXTRA="$MCP_SSL_CPP" LDEXTRA="$MCP_SSL_LD" SANFLAGS="$san" BUILD="$TMPROOT/mcp/build" OUT="$out" >"$TMPROOT/mcp_build.log" 2>&1; then
    tail -20 "$TMPROOT/mcp_build.log" >&2; fail "22 mcp: the server did not build"; return 1
  fi
  MCP_EXE="$out"
  # The server refuses to start without its drift locks; they are looked up next to the executable.
  cp "$HERE/mcp/.mcp-schema-lock" "$HERE/mcp/.mcp-safety-annotations-lock" "$TMPROOT/mcp/"
  return 0
}

mcp_env_run() {   # mcp_env_run PORT cmd...   (runs a python test driver against the server on PORT)
  local port="$1"; shift
  mkdir -p "$TMPROOT/mcp_cwd"
  MCP_SERVER_EXE="$MCP_EXE" MCP_SERVER_CWD="$TMPROOT/mcp_cwd" MCP_HERE="$HERE/mcp" VALKEY_HOST=127.0.0.1 VALKEY_PORT="$port" \
    ASAN_OPTIONS="detect_leaks=0" "$@"
}

# The probes are built once per run (with the active sanitizer) by whichever probe gate runs first.
MCP_PROBES_BUILT=0
mcp_probes_build() {
  [ "$MCP_PROBES_BUILT" -eq 1 ] && return 0
  command -v g++ >/dev/null 2>&1 || command -v c++ >/dev/null 2>&1 || command -v clang++ >/dev/null 2>&1 || { skip "mcp probes (no C++ compiler on PATH)"; return 1; }
  mcp_ssl_flags
  printf '#include <openssl/ssl.h>\nint main(){return 0;}\n' > "$TMPROOT/ssl_probe.cpp"
  "$(command -v g++ || command -v c++ || command -v clang++)" $MCP_SSL_CPP -fsyntax-only "$TMPROOT/ssl_probe.cpp" >/dev/null 2>&1 || { skip "mcp probes (OpenSSL development headers not found: libssl-dev / openssl-devel)"; return 1; }
  mkdir -p "$TMPROOT/mcp"
  local san=""; [ "$ASAN" -eq 1 ] && san="-fsanitize=address -g"; [ "$UBSAN" -eq 1 ] && san="-fsanitize=undefined -g"; [ "$TSAN" -eq 1 ] && san="-fsanitize=thread -g"
  [ "$COVERAGE" -eq 1 ] && san="--coverage -fprofile-update=atomic"
  local cxx; cxx="$(command -v g++ || command -v c++ || command -v clang++)"
  if ! make -C "$HERE/mcp" probes CXX="$cxx" CXXFLAGS="-O1 $san" CPPEXTRA="$MCP_SSL_CPP" LDEXTRA="$MCP_SSL_LD" BUILD="$TMPROOT/mcp/build" >"$TMPROOT/mcp_probes_build.log" 2>&1; then
    tail -20 "$TMPROOT/mcp_probes_build.log" >&2; fail "mcp probes: they did not build"; return 1
  fi
  MCP_PROBES_BUILT=1
}

# mcp_probe_run NUM PROBE [live]: runs one standalone probe; "live" gives it a throwaway Valkey to talk to.
mcp_probe_run() {
  local num="$1" p="$2" live="${3:-}" out rc
  mcp_probes_build || return
  if [ "$live" = live ]; then
    spawn_server "mcp_probe_$p" || { fail "$num mcp: could not start a server for $p"; return; }
    out="$(VALKEY_HOST=127.0.0.1 VALKEY_PORT="$SP_PORT" ASAN_OPTIONS="detect_leaks=0" "$TMPROOT/mcp/build/$p" 2>&1)"; rc=$?
    kill "$SP_PID" 2>/dev/null
  else
    out="$(ASAN_OPTIONS="detect_leaks=0" "$TMPROOT/mcp/build/$p" 2>&1)"; rc=$?
  fi
  if [ "$rc" -eq 0 ]; then pass "$num mcp: $p ($(grep -cE '^OK|^  ok' <<< "$out") checks; $(tail -1 <<< "$out" | cut -c1-80))"
  else grep -E '^FAIL' <<< "$out" | head -5 >&2; fail "$num mcp: $p reported a failure"; fi
}

gate_22_mcp_probes() {   # server build, vectors_probe and sanitize_probe
  mcp_build || return
  pass "22 mcp: fractalsql-valkey-mcp built ($("$MCP_EXE" --version))"
  mcp_probe_run 22 vectors_probe
  mcp_probe_run 22 sanitize_probe
}
gate_26_mcp_db_probe() { mcp_probe_run 26 db_probe live; }
gate_27_mcp_concurrency_probe() { mcp_probe_run 27 concurrency_probe; }

# 28 mcp_faults (Linux): the server under tests/oom_preload.c with one allocation, file call or socket call
# failed at a time. It must keep answering (an uncaught std::bad_alloc is tolerated, any other death is not).
gate_28_mcp_faults() {
  [ "$(uname -s)" = Linux ] || { skip "28 mcp_faults (needs LD_PRELOAD and signals, Linux only)"; return; }
  if [ "$ASAN" -eq 1 ] || [ "$UBSAN" -eq 1 ] || [ "$TSAN" -eq 1 ]; then
    skip "28 mcp_faults (a sanitizer runtime and the fault shim cannot both interpose the allocator)"; return
  fi
  mcp_build || return
  local w="$TMPROOT/mcp_faults"; rm -rf "$w"; mkdir -p "$w"
  if ! cc -O2 -fPIC -shared -o "$w/oom_preload.so" tests/oom_preload.c -ldl 2>"$w/build.log"; then
    cat "$w/build.log" >&2; fail "28 mcp_faults: oom_preload.so failed to build"; return
  fi
  spawn_server mcp_faults || { fail "28 mcp_faults: could not start the server"; return; }
  local out rc
  out="$(MCP_OOM_PRELOAD="$w/oom_preload.so" MCP_SERVER_EXE="$MCP_EXE" MCP_SERVER_CWD="$w" VALKEY_HOST=127.0.0.1 VALKEY_PORT="$SP_PORT" \
         timeout 420 nice "$PY" -u "$HERE/mcp/tests/mcp_faults.py" 2>&1)"; rc=$?
  printf '%s\n' "$out" > "$TMPROOT/mcp_faults.log"
  if [ "$rc" -eq 0 ] && grep -q 'ALL GREEN' <<< "$out"; then
    pass "28 mcp_faults: $(grep -c '^ *PASS' <<< "$out") checks passed ($(grep -E '^PASS faults: [0-9]+ injected alloc' <<< "$out" | head -1 | cut -c1-110))"
  else
    grep -E '^ *FAIL|cause' <<< "$out" | head -10 >&2
    fail "28 mcp_faults: failed (exit $rc; full log $TMPROOT/mcp_faults.log)"
  fi
  kill "$SP_PID" 2>/dev/null
}

# 29 mcp_fuzz (--fuzz only): libFuzzer + ASan on the RESP reply parser, the JSON-RPC front door and the
# argument coercion. Builds its own binaries from mcp/src; no server, no module.
gate_29_mcp_fuzz() {
  local cc="" cxx c secs="${FSQL_FUZZ_TIME:-30}" w="$TMPROOT/mcp_fuzz" t rc
  for c in "${FSQL_FUZZ_CC:-}" clang-19 clang-18 clang-17 clang; do
    [ -n "$c" ] || continue
    command -v "$c" >/dev/null 2>&1 || continue
    printf 'int LLVMFuzzerTestOneInput(const unsigned char*d,unsigned long n){return d&&n;}\n' > "$TMPROOT/fz_probe29.c"
    "$c" -fsanitize=fuzzer "$TMPROOT/fz_probe29.c" -o "$TMPROOT/fz_probe29" >/dev/null 2>&1 && { cc="$c"; break; }
  done
  [ -n "$cc" ] || { skip "29 mcp_fuzz (no clang with libFuzzer: set FSQL_FUZZ_CC)"; return; }
  cxx="${cc/clang/clang++}"; command -v "$cxx" >/dev/null 2>&1 || { skip "29 mcp_fuzz (no $cxx next to $cc)"; return; }
  rm -rf "$w"; mkdir -p "$w"
  for t in resp vectors jsonrpc; do
    local srcs=(mcp/src/errors.cpp)
    case "$t" in
      resp)    srcs+=(mcp/src/db.cpp mcp/src/logging.cpp) ;;
      vectors) srcs+=(mcp/src/vectors.cpp) ;;
      jsonrpc) srcs+=(mcp/src/db.cpp mcp/src/jsonrpc_server.cpp mcp/src/logging.cpp mcp/src/prompts.cpp mcp/src/resources.cpp mcp/src/vectors.cpp) ;;
    esac
    if ! "$cxx" -std=c++17 -O1 -g -fsanitize=fuzzer,address -Imcp/src -Imcp/vendor "tests/fuzz/fuzz_mcp_$t.cpp" "${srcs[@]}" \
          -lssl -lcrypto -lpthread -o "$w/fuzz_mcp_$t" >"$w/$t.build.log" 2>&1; then
      fail "29 mcp_fuzz: fuzz_mcp_$t failed to build; see $w/$t.build.log"; continue
    fi
    mkdir -p "$w/seed_$t"; rc=0
    ASAN_OPTIONS="detect_leaks=0:symbolize=0:abort_on_error=1" "$w/fuzz_mcp_$t" "$w/seed_$t" "tests/fuzz/corpus_mcp_$t" \
      -max_total_time="$secs" -print_final_stats=1 >"$w/$t.run.log" 2>&1 || rc=$?
    if [ "$rc" -eq 0 ]; then pass "29 mcp_fuzz: fuzz_mcp_$t (${secs}s, corpus-seeded)"
    else fail "29 mcp_fuzz: fuzz_mcp_$t exited rc=$rc; see $w/$t.run.log"; tail -5 "$w/$t.run.log" 2>/dev/null | sed 's/^/         /'; fi
  done
}

mcp_env_run() {   # mcp_env_run PORT cmd...   (runs a python test driver against the server on PORT)
  local port="$1"; shift
  MCP_SERVER_EXE="$MCP_EXE" MCP_SERVER_CWD="$TMPROOT/mcp_cwd" MCP_HERE="$HERE/mcp" VALKEY_HOST=127.0.0.1 VALKEY_PORT="$port" \
    ASAN_OPTIONS="detect_leaks=0" "$@"
}

mcp_driver_gate() {   # mcp_driver_gate NUM NAME SCRIPT [extra env as VAR=value ...]
  local num="$1" name="$2" script="$3"; shift 3
  mkdir -p "$TMPROOT/mcp_cwd"   # the server runs here, so its logs and any test keys never land in the repo
  mcp_build || return
  spawn_server "mcp_$name" || { fail "$num mcp_$name: could not start the server"; return; }
  local out rc
  local skip_sweeps=""; { [ "$TSAN" -eq 1 ] || [ "$COVERAGE" -eq 1 ]; } && skip_sweeps=1   # the sweeps are 10-20x slower under TSan
  out="$(env "$@" ${skip_sweeps:+MCP_SMOKE_SKIP_SWEEPS=1} MCP_SERVER_EXE="$MCP_EXE" MCP_SERVER_CWD="$TMPROOT/mcp_cwd" MCP_HERE="$HERE/mcp" VALKEY_HOST=127.0.0.1 VALKEY_PORT="$SP_PORT" \
         ASAN_OPTIONS="detect_leaks=0" timeout 1500 "$PY" "$HERE/mcp/tests/$script" 2>&1)"; rc=$?
  printf '%s\n' "$out" > "$TMPROOT/mcp_$name.log"
  local nfail; nfail="$(grep -c '^ *FAIL' <<< "$out")"
  if [ "$rc" -eq 0 ] && grep -q 'ALL GREEN' <<< "$out"; then
    pass "$num mcp_$name: $(grep -c '^ *PASS' <<< "$out") checks passed ($(grep -E 'ALL GREEN' <<< "$out" | tail -1 | sed 's/^ *//' | cut -c1-70))"
  else
    grep -E '^ *FAIL|offender|Traceback|Error' <<< "$out" | head -12 >&2
    fail "$num mcp_$name: $nfail check(s) failed (exit $rc; full log $TMPROOT/mcp_$name.log)"
  fi
  kill "$SP_PID" 2>/dev/null
}

gate_23_mcp_smoke() { mcp_driver_gate 23 smoke mcp_smoke_client.py; }
gate_24_mcp_https() {
  command -v openssl >/dev/null 2>&1 || { skip "24 mcp_https (no openssl CLI for the throwaway certificate)"; return; }
  local w="$TMPROOT/mcp_tls"; mkdir -p "$w"
  # A minimal [req] section, not an empty file: macOS ships LibreSSL, which refuses an empty config even with -subj.
  printf '%s\n' '[req]' 'distinguished_name = dn' 'prompt = no' '[dn]' 'CN = 127.0.0.1' > "$w/empty.cnf"
  OPENSSL_CONF="$w/empty.cnf" openssl req -x509 -newkey rsa:2048 -nodes -days 2 -subj "/CN=127.0.0.1" \
    -keyout "$w/key.pem" -out "$w/cert.pem" >/dev/null 2>&1 || { skip "24 mcp_https (could not generate a certificate)"; return; }
  mcp_driver_gate 24 https mcp_https_smoke_client.py MCP_HTTPS_PORT="$(free_port)" MCP_HTTPS_CERT="$w/cert.pem" MCP_HTTPS_KEY="$w/key.pem"
}
gate_25_mcp_cli() { mcp_driver_gate 25 cli mcp_cli.py; }
gate_36_mcp_audit_http() { mcp_driver_gate 36 audit_http mcp_audit_http.py; }   # origin check and repeated headers (SECURITY_AUDIT.md 3.17, 3.18)
# 30 mcp_tls: the server's TLS connection to Valkey, against a TLS-terminating proxy in front of the plain server.
gate_30_mcp_tls() {
  command -v openssl >/dev/null 2>&1 || { skip "30 mcp_tls (no openssl CLI for the throwaway certificates)"; return; }
  mcp_driver_gate 30 tls mcp_tls.py
}

# 33 fake_daemon: the module's daemon client against a scripted daemon that misbehaves (tests/fake_daemon.py). The real fractalsqld always answers
# correctly, so a reply cut short, a bad tag, a wrong request id, an oversized length, a status to map, a result that is too short or lies about its
# length and a daemon that never answers can only be driven by a double. Every daemon command, the multimodal optimizers and every agent (through
# gates 15 and 18's own call sequences) run under each failure; the server must answer every one. Also the module's load options for the daemon:
# the ways a daemon-socket / daemon-key-file / daemon-timeout-s combination is refused.
gate_35_frame_auth() {
  # SECURITY_AUDIT.md 3.1, 3.9, 3.22 (ported from the Druid daemon). Frame authentication allocates nothing, so a forged
  # all-zero-tag frame is refused even when the buffer it once allocated is made to fail; a payload declared but not sent is not
  # allocated before the frame is authenticated; and a key under 16 bytes is refused at start-up.
  local d key conf sock pid log hex i
  [ "$(uname -s)" = Linux ] || { skip "35 frame_auth (needs LD_PRELOAD and /proc: Linux only)"; return; }
  have_cc=$(command -v gcc || command -v cc)
  [ -n "$have_cc" ] || { skip "35 frame_auth (no C compiler)"; return; }
  [ -n "$PY" ] || { skip "35 frame_auth (no python3)"; return; }
  d="$(mktemp -d /tmp/fsqa.XXXXXX)"; key="$d/k"; conf="$d/c.conf"; sock="$d/s"; log="$d/log"
  "$have_cc" -shared -fPIC -O1 -o "$d/fs.so" tests/robustness/fail_size_preload.c -ldl 2>"$d/build.log" \
    || { fail "35 frame_auth: the failure shim did not build"; rm -rf "$d"; return; }
  openssl rand -hex 32 > "$key"; chmod 600 "$key"
  printf 'socket_path = %s\nhmac_key_file = %s\nallowed_uids = %s\n' "$sock" "$key" "$(id -u)" > "$conf"; chmod 600 "$conf"
  FSQL_FAIL_SIZE=$((32 + 777)) LD_PRELOAD="$d/fs.so" "$HERE/service/build/fractalsqld" -c "$conf" </dev/null >"$log" 2>&1 &
  pid=$!
  for i in $(seq 1 100); do [ -S "$sock" ] && break; kill -0 "$pid" 2>/dev/null || break; sleep 0.05; done
  if "$PY" tests/robustness/frame_auth.py "$sock" "$key" "$pid" 777 >"$d/out" 2>&1; then
    pass "35 frame_auth: forged frames refused, genuine answered, declared-but-unsent payloads not allocated"
  else
    sed 's/^/         /' "$d/out" | tail -8; fail "35 frame_auth: see the output above"
  fi
  kill "$pid" 2>/dev/null; { wait "$pid"; } 2>/dev/null
  local bad=0
  for hex in 00 0011223344556677 00112233445566778899aabbccddee; do
    printf '%s\n' "$hex" > "$key"; rm -f "$sock"
    if [ "$(KEYTRY=1 bash -c '"$0" -c "$1" </dev/null >/dev/null 2>&1 & p=$!; for i in $(seq 1 40); do [ -S "$2" ] && { echo start; kill $p; exit; }; kill -0 $p 2>/dev/null || { echo refuse; exit; }; sleep 0.05; done; echo timeout; kill $p' "$HERE/service/build/fractalsqld" "$conf" "$sock")" != refuse ]; then bad=$((bad + 1)); fi
  done
  if [ "$bad" -eq 0 ]; then pass "35 frame_auth: keys of 1, 8 and 15 bytes are refused at start-up"
  else fail "35 frame_auth: $bad short key(s) were accepted"; fi
  # SECURITY_AUDIT.md 3.12: without FRACTALSQL_LEDGER_ALLOW_DESTRUCTIVE the destructive ledger calls are refused (and logged); flush is not gated.
  if true; then
    printf '%s\n' "$(openssl rand -hex 32)" > "$key"; rm -f "$sock"
    env -u FRACTALSQL_LEDGER_ALLOW_DESTRUCTIVE "$HERE/service/build/fractalsqld" -c "$conf" </dev/null >"$d/off.log" 2>&1 &
    pid=$!
    for i in $(seq 1 100); do [ -S "$sock" ] && break; kill -0 "$pid" 2>/dev/null || break; sleep 0.05; done
    "$PY" tests/destructive_calls.py "$sock" "$key"
    kill "$pid" 2>/dev/null; { wait "$pid"; } 2>/dev/null
    rm -f "$sock"
    FRACTALSQL_LEDGER_ALLOW_DESTRUCTIVE=1 "$HERE/service/build/fractalsqld" -c "$conf" </dev/null >"$d/on.log" 2>&1 &
    pid=$!
    for i in $(seq 1 100); do [ -S "$sock" ] && break; kill -0 "$pid" 2>/dev/null || break; sleep 0.05; done
    "$PY" tests/destructive_calls.py "$sock" "$key" reset_only
    kill "$pid" 2>/dev/null; { wait "$pid"; } 2>/dev/null
    if grep -q "reset_hard refused: FRACTALSQL_LEDGER_ALLOW_DESTRUCTIVE" "$d/off.log" && ! grep -q "reset_hard refused" "$d/on.log" \
       && ! grep -q "flush refused" "$d/off.log"; then
      pass "35 frame_auth: reset_hard is refused (and logged) with the destructive switch off, not with it on; flush is never gated"
    else
      fail "35 frame_auth: the destructive switch did not behave: off log '$(tail -c 160 "$d/off.log" | tr '\n' ' ')'"
    fi
  fi
  rm -rf "$d"
}

gate_33_fake_daemon() {
  local d="$TMPROOT/fake_daemon" key sock port pid log v bin
  mkdir -p "$d"; key="$d/key"; sock="$(short_tmp)/fd.sock"
  "$PY" -c 'import secrets; print(secrets.token_hex(32))' > "$key"; chmod 600 "$key"
  spawn_server fakedaemon daemon-socket "$sock" daemon-key-file "$key" daemon-timeout-s 2 \
    || { fail "33 fake_daemon: could not start the server with the daemon configured"; return; }
  port="$SP_PORT"; pid="$SP_PID"; log="$SP_LOG"
  FSQL_FAKE_SOCK="$sock" FSQL_FAKE_KEY="$key" run_py_gate 33 fake_daemon "$port" "$pid" "$log"
  stop_server "$pid"
  # load options the module must refuse (the server then does not start)
  bin="$(pick_server)"
  printf 'zz-not-hex\n' > "$d/key_nothex"; chmod 600 "$d/key_nothex"
  printf 'abc\n' > "$d/key_odd"; chmod 600 "$d/key_odd"
  : > "$d/key_empty"; chmod 600 "$d/key_empty"
  cp "$key" "$d/key_open"; chmod 644 "$d/key_open"
  local variants=("daemon-socket relative/path daemon-key-file $key" "daemon-socket $sock daemon-key-file $d/missing" "daemon-socket $sock daemon-key-file $d/key_nothex"
                  "daemon-socket $sock daemon-key-file $d/key_odd" "daemon-socket $sock daemon-key-file $d/key_empty" "daemon-socket $sock daemon-key-file $d/key_open"
                  "daemon-socket $sock" "daemon-key-file $key" "daemon-socket $sock daemon-key-file $key daemon-timeout-s 0"
                  "daemon-socket $sock daemon-key-file $key daemon-timeout-s 99999" "daemon-socket $sock daemon-key-file $key daemon-timeout-s abc"
                  "daemon-socket $(printf 'a%.0s' $(seq 1 400)) daemon-key-file $key" "daemon-socket '' daemon-key-file $key")
  local refused=0 total=0
  for v in "${variants[@]}"; do
    total=$((total + 1))
    # shellcheck disable=SC2086
    if spawn_server fakedaemon_bad $v >/dev/null 2>&1; then stop_server "$SP_PID"; else refused=$((refused + 1)); fi
  done
  [ "$refused" -ge $((total - 3)) ] && pass "33 fake_daemon: $refused of $total bad daemon load option combinations were refused at load" \
    || fail "33 fake_daemon: only $refused of $total bad daemon load option combinations were refused"
}

# 34 session_params: FRACTAL.DIVERSIFY.* / FRACTAL.FEEDBACK argument handling (the params JSON parser above all), the edition/version arity and the session
# limit, on a server loaded with explicit non-default load options; then every load option the module must refuse (the server then does not start).
gate_34_session_params() {
  spawn_server s34 threads 3 max-inflight-mb 64 max-sessions 50 session-ttl-sec 3600 agent-partial-reasoning-fail no \
    || { fail "34 session_params: could not start the server with explicit load options"; return; }
  run_py_gate 34 session_params "$SP_PORT" "$SP_PID" "$SP_LOG"
  stop_server "$SP_PID"
  local v refused=0 total=0 bad=("threads 0" "threads 65" "threads abc" "max-inflight-mb -1" "max-inflight-mb 2000000" "max-sessions 0" "max-sessions 5000" "session-ttl-sec -1"
                                  "session-ttl-sec 99999999" "threads" "unknown-option 1" "agent-partial-reasoning-fail maybe" "daemon-timeout-s 0")
  for v in "${bad[@]}"; do
    total=$((total + 1))
    # shellcheck disable=SC2086
    if spawn_server s34_bad $v >/dev/null 2>&1; then stop_server "$SP_PID"; else refused=$((refused + 1)); fi
  done
  [ "$refused" -ge $((total - 1)) ] && pass "34 session_params: $refused of $total bad load options were refused at load" \
    || fail "34 session_params: only $refused of $total bad load options were refused"
  for v in "agent-partial-reasoning-fail yes" "session-ttl-sec 0" "max-inflight-mb 0"; do
    # shellcheck disable=SC2086
    if spawn_server s34_ok $v >/dev/null 2>&1; then stop_server "$SP_PID"; pass "34 session_params: load option '$v' is accepted"; else fail "34 session_params: load option '$v' was refused"; fi
  done
}

# 31 mcp_conformance: the shared conformance driver (mcp/tests/mcp_conformance.py), which starts and
# stops the servers it needs and writes its own credential files. It needs no third-party SDK, only
# python3, and it is the same file, byte for byte, in every engine repository that ships an MCP
# plugin.
# Gate 32: every vendored reasoning plugin carries its detached signature. fractalsqld loads the plugin
# only when <plugin>.sig (exactly 64 raw Ed25519 bytes) sits beside it, so a plugin vendored without one
# (or with a truncated one) would ship an image whose reasoning tier refuses to start. A static check of
# include/<platform>/; it reads files only.
gate_32_plugin_sig_vendored() {
  local n=0 bad=0 f
  for f in "$HERE"/include/*/fractalsql-reasoning-http.so; do
    [ -f "$f" ] || continue
    n=$((n + 1))
    if [ ! -f "${f}.sig" ] || [ "$(wc -c < "${f}.sig" | tr -d ' ')" -ne 64 ]; then
      printf '      missing or not 64 bytes: %s.sig\n' "${f#"$HERE"/}"; bad=$((bad + 1))
    fi
  done
  if [ "$n" -eq 0 ]; then skip "32 plugin_sig_vendored (no vendored plugins under include/)"; return; fi
  if [ "$bad" -ne 0 ]; then fail "32 plugin_sig_vendored: $bad of $n vendored plugin(s) lack a valid .sig"; return; fi
  pass "32 plugin_sig_vendored: all $n vendored plugins have a 64-byte .sig beside them"
}

gate_31_mcp_conformance() {
  [ -f "$HERE/mcp/tests/mcp_conformance.py" ] || { skip "31 mcp_conformance (the conformance driver is not present)"; return; }
  local mcp_py="$PY"
  [ -n "$mcp_py" ] || { skip "31 mcp_conformance (no python3 on PATH -- the conformance driver needs it)"; return; }
  mcp_build || return
  mkdir -p "$TMPROOT/mcp_cwd"   # the servers the driver starts run here, so nothing lands in the repo
  local out rc
  out="$(env \
      MCP_SERVER_EXE="$MCP_EXE" \
      MCP_SERVER_CWD="$TMPROOT/mcp_cwd" \
      MCPC_TOKEN="conformance-token" \
      MCPC_GATE=31 \
      MCPC_LOCK_DIR="$HERE/mcp" \
      MCPC_PASSTHROUGH_ENV="VALKEY_HOST,VALKEY_PORT,VALKEY_PASSWORD,VALKEY_DB,FSQL_MCP_KEY_PREFIX" \
      MCPC_READ_TOOL="fractal_version" \
      MCPC_READ_ARGS='{}' \
      MCPC_STATE_TOOL="fractal_diversify_enable" \
      MCPC_STATE_ARGS='{}' \
      MCPC_STATE_READER="fractal_explain_result" \
      MCPC_DESTRUCTIVE_TOOL="fractal_diversify_enable" \
      MCPC_DESTRUCTIVE_ARGS='{}' \
      MCPC_SAFE_KEY="reasoning_model" \
      MCPC_UNSAFE_KEY="reasoning_url" \
      VALKEY_HOST=127.0.0.1 VALKEY_PORT="$PORT" \
      MCP_HERE="$HERE/mcp" \
      ASAN_OPTIONS="detect_leaks=0" \
      timeout 1800 "$mcp_py" -u "$HERE/mcp/tests/mcp_conformance.py" 2>&1)"; rc=$?
  printf '%s\n' "$out" > "$TMPROOT/mcp_conformance.log"
  if [ "$rc" -eq 0 ] && grep -q "ALL GREEN" <<< "$out"; then
    pass "31 mcp_conformance: $(grep -c '^ *PASS' <<< "$out") checks passed (protocol eras, authentication, framing, operator switches, catalog)"
  else
    grep -E '^ *FAIL|^ *SKIP' <<< "$out" | head -20 >&2
    fail "31 mcp_conformance: the conformance driver reported failures (exit $rc; full log $TMPROOT/mcp_conformance.log)"
  fi
}

gate_21_fuzz() {
  local cc="" c plat core secs="${FSQL_FUZZ_TIME:-30}" bin="$TMPROOT/fuzz_fv" work="$TMPROOT/fuzz_work"
  for c in "${FSQL_FUZZ_CC:-}" clang-19 clang-18 clang; do
    [ -n "$c" ] || continue
    command -v "$c" >/dev/null 2>&1 || continue
    printf 'int LLVMFuzzerTestOneInput(const unsigned char*d,unsigned long n){return d&&n;}\n' > "$TMPROOT/fz_probe.c"
    "$c" -fsanitize=fuzzer "$TMPROOT/fz_probe.c" -o "$TMPROOT/fz_probe" >/dev/null 2>&1 && { cc="$c"; break; }
  done
  [ -n "$cc" ] || { skip "21 fuzz (no clang with libFuzzer: set FSQL_FUZZ_CC)"; return; }
  plat="$(uname -s | tr '[:upper:]' '[:lower:]')-$(uname -m)"
  core="$HERE/include/$plat/libfractalsql-${CORE_VARIANT:-community-sovereign-c}.a"
  [ -f "$core" ] || { skip "21 fuzz (no core archive for $plat)"; return; }
  if ! "$cc" -g -O1 -fno-omit-frame-pointer -fsanitize=fuzzer,address,undefined -fno-sanitize-recover=undefined \
        -fcommon -I"$HERE/include" -I"$HERE/src" -DFRACTALSQL_VERSION=\"$(cat "$HERE/VERSION")\" \
        "$HERE/tests/fuzz/fuzz_fv.c" "$HERE/src/fv_pool.c" "$HERE/src/fv_vector.c" "$core" -lm -ldl -lpthread -o "$bin" \
        >"$TMPROOT/fuzz_build.log" 2>&1; then
    tail -8 "$TMPROOT/fuzz_build.log" >&2; fail "21 fuzz: the harness did not build"; return
  fi
  pass "21 fuzz: harness built with $cc (ASan + UBSan + libFuzzer)"
  mkdir -p "$work"
  if "$bin" -max_total_time="$secs" -max_len=4096 -rss_limit_mb=2048 -timeout=10 "$work" "$HERE/tests/fuzz/corpus_fv" \
        >"$TMPROOT/fuzz_run.log" 2>&1; then
    pass "21 fuzz: ${secs}s over five targets, no crash ($(grep -E '^#[0-9]+.*(DONE|pulse)' "$TMPROOT/fuzz_run.log" | tail -1 | sed -E 's/ +/ /g' | cut -c1-60))"
  else
    tail -25 "$TMPROOT/fuzz_run.log" >&2; fail "21 fuzz: a crash or sanitizer report (artifact in $PWD)"
  fi
}

gate_17_kits() {
  local f name out rc n ran=0 verts=0
  for f in "$HERE"/demo/*.py; do
    name="$(basename "$f" .py)"
    [ "$name" = fvlib ] && continue
    out="$(cd "$HERE/demo" && "$PY" "$name.py" --port "$PORT" 2>&1)"; rc=$?
    ran=$((ran + 1))
    if [ "$rc" -ne 0 ]; then
      fail "17 demos: $name failed (exit $rc): $(grep -E 'FAIL|Error|error' <<< "$out" | head -2 | tr '\n' ' ' | cut -c1-200)"; continue
    fi
    if grep -qiwE 'nan|inf' <<< "$out"; then fail "17 demos: $name printed a NaN/inf cell"; continue; fi
    n="$(grep -c '\[ok\]' <<< "$out")"
    case "$name" in
      demo-vertical-*)
        verts=$((verts + 1))
        if [ "$n" -lt 3 ]; then fail "17 demos: $name made only $n checks"; continue; fi ;;
    esac
    pass "17 demos: $name ran clean and reproduced its headline result ($n checks)"
  done
  [ "$verts" -eq 44 ] && pass "17 demos: all $verts demo-vertical-* files ran" || fail "17 demos: found $verts demo-vertical-* file(s), expected exactly 44 (42 verticals + 2 Valkey-native)"
  [ "$ran" -eq 50 ] && pass "17 demos: all $ran demo files ran (44 verticals, demo, demo-agents, demo-fractal-vector, demo-diversify, response-modes, enterprise-qtl-audit)" || fail "17 demos: found $ran demo file(s), expected exactly 50"
}

gate_10_lifecycle() {
  spawn_server lifecycle threads 2 || { fail "10 lifecycle: could not start the server"; return; }
  FSQL_MODULE_PATH="$HERE/fractalsql.so" run_py_gate 10 lifecycle "$SP_PORT" "$SP_PID" "$SP_LOG"
}


# --coverage: stop every server so the counters are written, then report.
coverage_report() {
  local p i
  for p in "${ALL_PIDS[@]+"${ALL_PIDS[@]}"}"; do kill "$p" 2>/dev/null; done
  for p in "${ALL_PIDS[@]+"${ALL_PIDS[@]}"}"; do
    for i in $(seq 1 100); do kill -0 "$p" 2>/dev/null || break; sleep 0.1; done
  done
  echo
  echo "coverage (module sources; allocation-failure branches are exercised by gate 11's separate build):"
  local f out lines branches total_l=0 total_lc=0 total_b=0 total_bc=0
  for f in module fv_pool fv_search fv_vector fv_analytics fv_daemon fv_agents fv_session; do
    out="$(cd "$HERE/src" && gcov -b -n -o . "$f.c" 2>/dev/null | awk -v f="$f.c" '
        $0 ~ "^File .*" f "\047" {hit=1; next}
        hit && /^Lines executed/ {l=$0}
        hit && /^Branches executed/ {b=$0; hit=0}
        END {print l "|" b}')"
    lines="${out%%|*}"; branches="${out##*|}"
    printf '  %-14s %s   %s\n' "$f.c" "${lines#Lines executed:}" "${branches#Branches executed:}"
    local pct="${lines#Lines executed:}"; pct="${pct%%%*}"
    if [ -z "$pct" ] || ! awk -v p="$pct" -v floor="${FSQL_COVERAGE_FLOOR:-85}" 'BEGIN{exit !(p+0 >= floor+0)}'; then
      fail "coverage: $f.c has ${pct:-no data}% line coverage, below the ${FSQL_COVERAGE_FLOOR:-85}% floor"
    else
      pass "coverage: $f.c line coverage ${pct}% meets the ${FSQL_COVERAGE_FLOOR:-85}% floor"
    fi
  done
  # The two module flavors unioned: what the whole suite reached, including the allocation-failure branches gate 11 drives.
  if command -v python3 >/dev/null 2>&1 && ls "$HERE"/.oomcov/fractalsql_oom.so-*.gcda >/dev/null 2>&1; then
    echo "coverage (module sources, shipped flavor + allocation-failure flavor unioned):"
    python3 -I "$HERE/tests/gcov_union.py" "$HERE" '(^|/)src/(module|fv_[a-z_]+)\.c$' "$HERE/src" "$HERE/.oomcov" | grep -v '^TOTAL ' | sed 's/^/  /'
  fi
  # The MCP server (gates 22-28), reported but with no floor: it shows what the MCP gates reached.
  if [ -d "$TMPROOT/mcp/build" ] && find "$TMPROOT/mcp/build" -name '*.gcda' 2>/dev/null | grep -q .; then
    echo "coverage (MCP server sources; best-effort goal ${FSQL_MCP_COVERAGE_GOAL:-80}% lines, reported, not a floor):"
    local o
    for o in $(find "$TMPROOT/mcp/build" -name '*.gcno' | sort); do
      ( cd "$(dirname "$o")" && gcov -n "$(basename "$o")" 2>/dev/null | awk -v f="$(basename "$o" .gcno)" '
          /^File / {hit = ($0 ~ /^File \047(.*\/)?src\/[^\/]+\.(cpp|cc|c)\047$/); next}
          hit && /^Lines executed/ {printf "  %-22s %s\n", f, $0; hit=0}' )
    done | awk '
      # Probe binaries recompile the same sources: key by source file, keep the best any object reached.
      match($0, /executed:[0-9.]+% of [0-9]+/) {
        split(substr($0, RSTART+9, RLENGTH-9), a, /% of /)
        k = $1; sub(/^.*-/, "", k)
        if (!(k in n) || a[1]+0 > p[k]+0) {p[k] = a[1]; n[k] = a[2]}
      }
      END {
        for (k in n) {printf "  %-22s Lines executed:%.2f%% of %d\n", k, p[k], n[k] | "sort"; t += n[k]; c += p[k]*n[k]/100}
        close("sort")
        if (t > 0) printf "  %-22s Lines executed:%.2f%% of %d  (best result per source across the server and probe binaries)\n", "TOTAL", 100*c/t, t
      }'
  else
    skip "coverage: no MCP .gcda produced (no MCP gate ran under --coverage)"
  fi
}

# ---------------------------------------------------------------------------
# main
# ---------------------------------------------------------------------------
[ -n "$PY" ] || { echo "ERROR: python3 is required" >&2; exit 2; }

echo "== fractalsql-valkey${SAN_NOTE:+ ($SAN_NOTE)} =="

need_server=0
for g in "${GATES[@]}"; do [ "$g" != "01" ] && [ "$g" != "21" ] && [ "$g" != "29" ] && need_server=1; done

# The build step runs when gate 01 is selected, and also whenever the module
# is missing or was built for a different flavor, whatever gates were chosen.
run_build=0
for g in "${GATES[@]}"; do [ "$g" = "01" ] && run_build=1; done
[ "$FUZZ" -eq 0 ] && [ "$run_build" -eq 0 ] && build_needed && run_build=1
[ "$run_build" -eq 1 ] && gate_01_build

if [ "$need_server" -eq 1 ]; then
  if [ "$FAIL" -gt 0 ]; then
    skip "gates 02+: the build failed, no server started"
  elif ! start_server; then
    fail "server: could not start a throwaway server with the module loaded"
  else
    pass "server: module loaded into $(basename "$(pick_server)") ($("$(pick_server)" --version | sed -E 's/.*v=([^ ]+).*/\1/') ) on port $PORT"
    gate_sanitizer_loaded
    for g in "${GATES[@]}"; do
      case "$g" in
        02) gate_02_smoke ;;
        03) gate_03_scout ;;
        04) gate_04_bounds ;;
        05) gate_05_survival ;;
        06) gate_06_vector ;;
        07) gate_07_analytics ;;
        08) gate_08_async ;;
        09) gate_09_limits ;;
        10) gate_10_lifecycle ;;
        11) gate_11_oom ;;
        12) gate_12_acl ;;
        13) gate_13_persist ;;
        14) gate_14_versions ;;
        15) gate_15_daemon ;;
        16) gate_16_enterprise ;;
        17) gate_17_kits ;;
        18) gate_18_agents ;;
        19) gate_19_analyst ;;
        20) gate_20_sessions ;;
        22) gate_22_mcp_probes ;;
        23) gate_23_mcp_smoke ;;
        24) gate_24_mcp_https ;;
        25) gate_25_mcp_cli ;;
        26) gate_26_mcp_db_probe ;;
        27) gate_27_mcp_concurrency_probe ;;
        28) gate_28_mcp_faults ;;
        30) gate_30_mcp_tls ;;
        31) gate_31_mcp_conformance ;;
        32) gate_32_plugin_sig_vendored ;;
        33) gate_33_fake_daemon ;;
        34) gate_34_session_params ;;
        35) gate_35_frame_auth ;;
        36) gate_36_mcp_audit_http ;;
      esac
    done
  fi
fi

[ "$FUZZ" -eq 1 ] && { gate_21_fuzz; gate_29_mcp_fuzz; }
[ "$COVERAGE" -eq 1 ] && coverage_report

echo
printf 'build_test: %d passed, %d failed, %d skipped, %d known\n' "$PASS" "$FAIL" "$SKIP" "$KNOWN"
if [ "$FAIL" -eq 0 ]; then echo "build_test: PASS"; exit 0; fi
echo "build_test: FAIL"
exit 1
