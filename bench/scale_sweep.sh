#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
# SPDX-FileCopyrightText: 2026 Daniel Gardiner d/b/a FractalSQLabs
#
# bench/scale_sweep.sh: run bench.py across a range of corpus sizes, one size at a time, so you can see how
# FRACTAL.SEARCH and FRACTAL.EXPLORE grow with rows x dimension on YOUR hardware. It reports; it does not pass or fail.
#
# Usage:
#   bench/scale_sweep.sh [--host H] [--port P] [--dim D] [--sizes "1000 5000 20000"] [--repeat N]
#
# Scout and Sniper scan the corpus on every call, so cost is linear in rows x dim. Start small. Do not run it on a
# machine other work is using; the numbers would be about the other work.
set -euo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
HOST=127.0.0.1; PORT=6379; DIM=64; SIZES="1000 5000 20000"; REPEAT=10
while [ $# -gt 0 ]; do
  case "$1" in
    --host)   HOST="${2:?}"; shift ;;
    --port)   PORT="${2:?}"; shift ;;
    --dim)    DIM="${2:?}"; shift ;;
    --sizes)  SIZES="${2:?}"; shift ;;
    --repeat) REPEAT="${2:?}"; shift ;;
    -h|--help) sed -n '2,12p' "$0"; exit 0 ;;
    *) echo "unknown argument '$1'" >&2; exit 2 ;;
  esac
  shift
done
case "$PORT$DIM$REPEAT" in *[!0-9]*) echo "--port, --dim and --repeat must be integers" >&2; exit 2 ;; esac
for n in $SIZES; do case "$n" in ''|*[!0-9]*) echo "--sizes holds integers only (got '$n')" >&2; exit 2 ;; esac; done
command -v python3 >/dev/null || { echo "python3 is required" >&2; exit 2; }
for n in $SIZES; do
  echo "=== ${n} x ${DIM}"
  python3 "$HERE/bench.py" --host "$HOST" --port "$PORT" --sizes "${n}x${DIM}" --repeat "$REPEAT"
  echo
done
