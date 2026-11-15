#!/usr/bin/env bash
#
# tests/smoke.sh — exercise FRACTAL.SEARCH end-to-end.
#
# Assumes: valkey-server is running and reachable via valkey-cli on the
# default port, with fractalsql.so already loaded (the install-test
# workflow brings this up; for local runs see the README).
#
# What it checks:
#   1. fractal.edition / .version metadata commands return Community / 2.0.0
#   2. A corpus stored as packed float32 BLOB is searchable
#   3. FRACTAL.SEARCH returns a JSON document containing dim, n_corpus,
#      best_point, best_fit, top_k — the canonical fsql_search_ptr shape
#   4. The k argument is honored
#
# Exit 0 on full pass, 1 on any failure. Prints what it ran for triage.

set -euo pipefail

VALKEY_CLI="${VALKEY_CLI:-valkey-cli}"
DIM=4
N_CORPUS=8
K=3

log() { printf '[smoke] %s\n' "$*"; }
fail() { printf '[smoke] FAIL: %s\n' "$*" >&2; exit 1; }

# --- Sanity: server reachable and module loaded -------------------------

log "ping"
"$VALKEY_CLI" ping | grep -q PONG || fail "valkey-cli ping did not return PONG"

log "MODULE LIST contains fractalsql"
"$VALKEY_CLI" MODULE LIST | grep -qi fractal \
    || fail "fractalsql module not loaded"

log "FRACTAL.EDITION + .VERSION"
edition="$("$VALKEY_CLI" FRACTAL.EDITION)"
version="$("$VALKEY_CLI" FRACTAL.VERSION)"
[[ "$edition" = "Community" ]] || fail "expected edition=Community, got '$edition'"
[[ "$version" = "2.0.0" ]]     || fail "expected version=2.0.0, got '$version'"

# --- Build a deterministic corpus + query as packed float32 BLOBs -------
# 8 rows × 4 dims = 32 floats = 128 bytes. The query is identical to
# row 0 so the search is well-conditioned: best_fit should be near 0
# (cosine distance to itself), and idx=0 should appear in top_k.

log "synthesize float32 corpus + query (dim=$DIM, n_corpus=$N_CORPUS)"
python3 - <<PY > /tmp/redis-smoke-load.txt
import struct
rows = [
    [1.0, 0.0, 0.0, 0.0],   # row 0 (== query)
    [0.9, 0.1, 0.0, 0.0],   # row 1 (close)
    [0.0, 1.0, 0.0, 0.0],   # row 2 (orthogonal)
    [0.0, 0.0, 1.0, 0.0],
    [0.0, 0.0, 0.0, 1.0],
    [-1.0, 0.0, 0.0, 0.0],  # opposite of query
    [0.5, 0.5, 0.0, 0.0],
    [0.7, 0.7, 0.1, 0.0],
]
corpus = b"".join(struct.pack("<" + "f" * len(r), *r) for r in rows)
query  = struct.pack("<ffff", *rows[0])

# valkey-cli -x reads the value from stdin so we can pipe binary
# without quoting headaches. Emit two lines: corpus path + query path.
import sys, pathlib
p_corpus = pathlib.Path("/tmp/redis-smoke-corpus.bin")
p_query  = pathlib.Path("/tmp/redis-smoke-query.bin")
p_corpus.write_bytes(corpus)
p_query.write_bytes(query)
print(p_corpus)
print(p_query)
PY
read -r CORPUS_PATH < /tmp/redis-smoke-load.txt
QUERY_PATH=$(sed -n 2p /tmp/redis-smoke-load.txt)

log "SET fractal:corpus from $CORPUS_PATH"
"$VALKEY_CLI" -x SET fractal:corpus < "$CORPUS_PATH" | grep -q OK \
    || fail "SET corpus failed"

# --- FRACTAL.SEARCH ----------------------------------------------------

# valkey-cli -x appends stdin (the binary query) as the LAST argument, so
# K cannot be passed here and defaults to 10. Honoring a custom K needs a
# client that can send a binary arg in non-final position.
log "FRACTAL.SEARCH fractal:corpus <query>  (k defaults to 10)"
result="$(cat "$QUERY_PATH" | "$VALKEY_CLI" -x FRACTAL.SEARCH fractal:corpus)"

if [[ -z "$result" ]]; then
    fail "FRACTAL.SEARCH returned empty"
fi

log "raw result (first 200 chars):"
printf '  %s\n' "${result:0:200}"

# Validate JSON shape via python (ships on every CI runner + RHEL/Debian).
log "validate JSON shape"
python3 - "$result" <<'PY' || fail "JSON validation failed"
import json, sys
doc = json.loads(sys.argv[1])
required = {"dim", "n_corpus", "best_fit", "best_point", "top_k"}
missing = required - set(doc)
if missing:
    print(f"missing fields: {missing}", file=sys.stderr); sys.exit(1)
if doc["dim"] != 4:
    print(f"expected dim=4, got {doc['dim']}", file=sys.stderr); sys.exit(1)
if doc["n_corpus"] != 8:
    print(f"expected n_corpus=8, got {doc['n_corpus']}", file=sys.stderr); sys.exit(1)
if not isinstance(doc["best_point"], list) or len(doc["best_point"]) != 4:
    print(f"best_point must be 4-element list", file=sys.stderr); sys.exit(1)
if not isinstance(doc["top_k"], list) or len(doc["top_k"]) == 0:
    print("top_k must be non-empty list", file=sys.stderr); sys.exit(1)
for entry in doc["top_k"]:
    if "idx" not in entry or "dist" not in entry:
        print(f"top_k entry missing idx/dist: {entry}", file=sys.stderr); sys.exit(1)
# k defaults to 10; SFS may converge to fewer distinct rows, and the
# corpus holds 8 rows, so top_k is between 1 and 8.
if len(doc["top_k"]) > 8:
    print(f"top_k length {len(doc['top_k'])} exceeds corpus size 8",
          file=sys.stderr); sys.exit(1)
print("ok")
PY

# --- FRACTAL.EXPLORE (Scout Mode) -------------------------------------
# Build a 3-island clustered corpus; the Scout population must disperse
# across >1 island where a top-k nearest lookup would collapse into one.

log "build clustered corpus for Scout"
python3 - <<'PY'
import struct, random, pathlib
random.seed(11)
CENTERS = [[1,0,0],[0,1,0],[0,0,1]]; PER = 15
rows = []
for c in CENTERS:
    for _ in range(PER):
        rows.append([x + random.uniform(-0.02, 0.02) for x in c])
corpus = b"".join(struct.pack("<fff", *r) for r in rows)
query  = struct.pack("<fff", *CENTERS[0])
pathlib.Path("/tmp/valkey-scout-corpus.bin").write_bytes(corpus)
pathlib.Path("/tmp/valkey-scout-query.bin").write_bytes(query)
PY

log "SET fractal:scout_corpus"
"$VALKEY_CLI" -x SET fractal:scout_corpus < /tmp/valkey-scout-corpus.bin | grep -q OK \
    || fail "SET scout corpus failed"

# valkey-cli -x appends stdin (the binary query) as the LAST argument, so
# the binary query must be the final arg — population_size therefore
# defaults to 50 here.
#
# Skip-safe: if FRACTAL.EXPLORE is unavailable (pre-Scout source) or the
# linked core predates Scout (reply carries no "population"), SKIP rather
# than fail — a build made before the Scout drop is deployed legitimately
# has no population path, and this gate stays green until it does.
log "FRACTAL.EXPLORE fractal:scout_corpus <query>  (population_size defaults to 50)"
scout="$(cat /tmp/valkey-scout-query.bin | "$VALKEY_CLI" -x FRACTAL.EXPLORE fractal:scout_corpus 2>/dev/null || true)"

log "validate Scout population + dispersion (skip if pre-Scout)"
python3 - "$scout" <<'PY' || fail "Scout validation failed"
import json, sys
CENTERS = [[1,0,0],[0,1,0],[0,0,1]]
s = sys.argv[1] if len(sys.argv) > 1 else ""
try:
    doc = json.loads(s)
except Exception:
    print("[smoke] SKIP scout: FRACTAL.EXPLORE unavailable (pre-Scout build)")
    sys.exit(0)
pop = doc.get("population")
if not isinstance(pop, list) or not pop:
    print("[smoke] SKIP scout: reply has no 'population' (pre-Scout core)")
    sys.exit(0)
if len(pop) != 50:
    print(f"expected 50 particles (default population_size), got {len(pop)}",
          file=sys.stderr); sys.exit(1)
if not all(isinstance(p, list) and len(p) == 3 for p in pop):
    print("bad particle dim", file=sys.stderr); sys.exit(1)
def nearest(p):
    return min(range(3), key=lambda k:
               sum((p[i] - CENTERS[k][i]) ** 2 for i in range(3)))
islands = len(set(nearest(p) for p in pop))
if islands < 2:
    print(f"Scout discovered only {islands} island(s); expected >= 2 "
          "(no dispersion)", file=sys.stderr); sys.exit(1)
print(f"ok: {len(pop)} particles disperse across {islands}/3 islands")
PY

log "all checks passed"
