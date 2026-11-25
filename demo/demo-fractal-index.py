#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""The native IVF index: build one, search it, and check that its answers are the exact ones.

A corpus is one Valkey key of packed little-endian float32 rows. The index partitions those rows into
k-means cells and gives each row a compact code, so a search reads a few cells instead of the whole corpus.
The index only chooses WHICH rows to look at: every candidate is read back from the corpus and scored with
the same double-precision cosine, so the distances are exact and the rows the index skipped do not change
the answer (they can change how many rows are considered, which is what `nprobe` controls).

Run it against a server that has fractalsql.so loaded:

    python3 demo/demo-fractal-index.py --host 127.0.0.1 --port 6379
"""
import sys, os, json, random, struct
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__))))
from fvlib import Kit, pack

k = Kit("demo-fractal-index")

DIM, ROWS, CLUSTERS = 16, 3000, 24
KEY = "demo:index:corpus"
NAME = "demo_index"

rng = random.Random(2026)
centres = [[rng.gauss(0, 1) for _ in range(DIM)] for _ in range(CLUSTERS)]
rows = [[x + 0.3 * rng.gauss(0, 1) for x in rng.choice(centres)] for _ in range(ROWS)]

k.section("1. The corpus")
k.call("SET", KEY, b"".join(pack(r) for r in rows))
k.show("corpus key", KEY)
k.show("rows x dimension", "%d x %d" % (ROWS, DIM))
k.show("bytes", str(len(b"".join(pack(r) for r in rows))))

k.section("2. The exact scan (no index)")
q = pack(rows[7])
exact = k.js("FRACTAL.SEARCH", KEY, q, 5, "USE_INDEX", 0)
k.show("rows scanned", str(exact["rows_scanned"]))
for row in exact["top_k"]:
    k.show("  row %d" % row["idx"], "dist %.12g" % row["dist"])

k.section("3. Build the index")
built = k.js("FRACTAL.INDEX.CREATE", KEY, NAME, "DIM", str(DIM), "VECTOR_FORMAT", "FP32")
k.show("format", built["format"])
k.show("rows indexed", str(built["rows"]))
k.show("cells (nlist)", str(built["nlist"]))
k.show("generation", str(built["generation"]))

k.section("4. The indexed search")
idx = k.js("FRACTAL.SEARCH", KEY, q, 5, "INDEX", NAME)
k.show("rows scanned", str(idx["rows_scanned"]))
k.show("cells probed", str(idx["cells_probed"]))
for row in idx["top_k"]:
    k.show("  row %d" % row["idx"], "dist %.12g" % row["dist"])

k.expect("the index read far fewer rows than the corpus holds",
         idx["rows_scanned"] < ROWS // 10, "%d of %d" % (idx["rows_scanned"], ROWS))
k.expect("the index was used", idx.get("used_index") is True)

k.section("5. The distances agree with the exact scan")
em = {r["idx"]: r["dist"] for r in exact["top_k"]}
same = [r for r in idx["top_k"] if r["idx"] in em]
k.expect("every returned row is one the exact scan also returned", len(same) == len(idx["top_k"]))
k.expect("every distance is bit-identical to the exact scan's",
         all(struct.pack("<d", r["dist"]) == struct.pack("<d", em[r["idx"]]) for r in same))
cos = float(k.call("FRACTAL.VECTOR.COSINE", q, pack(rows[idx["top_k"][0]["idx"]])))
k.expect("the distance is the module's own cosine, bit for bit",
         struct.pack("<d", cos) == struct.pack("<d", idx["top_k"][0]["dist"]))

k.section("6. What the index knows")
info = k.js("FRACTAL.INDEX.INFO", KEY, NAME)
k.show("format / dim / cells", "%s, dim %d, %d cells" % (info["format"], info["dim"], info["nlist"]))
k.show("status", info["status"])
k.show("advice", "; ".join(info["advice"]) or "(none)")
k.show("last search", "%d rows scanned, %d cells probed" %
       (info["last_search"]["rows_scanned"], info["last_search"]["cells_probed"]))
k.expect("a fresh index is ready", info["status"] == "ready")
for ix in k.js("FRACTAL.INDEX.LIST", KEY):
    k.show("index %s" % ix["name"], "%s, %d rows, %s" % (ix["format"], ix["rows"], ix["status"]))

k.section("7. Rows are appended to the corpus")
new_row = [rng.gauss(0, 1) for _ in range(DIM)]
k.call("SET", KEY, k.call("GET", KEY) + pack(new_row))
info = k.js("FRACTAL.INDEX.INFO", KEY, NAME)
k.show("status", info["status"])
k.show("rows added since the build", str(info["rows_added_since_build"]))
k.show("advice", "; ".join(info["advice"]))
k.expect("the index reports itself stale and advises a refresh",
         info["status"] == "stale" and any("refresh" in a for a in info["advice"]))
hit = k.js("FRACTAL.SEARCH", KEY, pack(new_row), 3, "INDEX", NAME, "NPROBE", 1)
k.expect("the appended row is found at once (rows after the build are scanned exactly)",
         hit["top_k"][0]["idx"] == ROWS and hit["top_k"][0]["dist"] < 1e-9)

k.section("8. Refresh folds the new rows in, without retraining")
rf = k.js("FRACTAL.INDEX.REFRESH", KEY, NAME)
k.show("added / rows / generation", "%d / %d / %d" % (rf["added"], rf["rows"], rf["generation"]))
info = k.js("FRACTAL.INDEX.INFO", KEY, NAME)
k.expect("after the refresh the index is ready", info["status"] == "ready" and info["rows_indexed"] == ROWS + 1)

k.section("9. One command that keeps the index fit: ENSURE")
k.call("SET", KEY, k.call("GET", KEY) + pack([rng.gauss(0, 1) for _ in range(DIM)]))
en = k.js("FRACTAL.INDEX.ENSURE", KEY, NAME, "DIM", str(DIM), "VECTOR_FORMAT", "FP32")
k.show("one row was added: action", "%s (generation %d, %d rows)" % (en["action"], en["generation"], en["rows"]))
k.expect("a few added rows are folded in (refreshed)", en["action"] == "refreshed")
en = k.js("FRACTAL.INDEX.ENSURE", KEY, NAME, "DIM", str(DIM), "VECTOR_FORMAT", "FP32")
k.show("nothing added: action", en["action"])
k.expect("an index that is fit is left alone (unchanged)", en["action"] == "unchanged" and en["status"] == "ready")
ROWS_NOW = en["rows"]

k.section("10. Search only some rows (IDS)")
subset = [3, 7, 11, 13, ROWS]
sub = k.js("FRACTAL.SEARCH", KEY, pack(rows[7]), 3, "IDS", ",".join(str(i) for i in subset))
k.show("rows scanned", str(sub["rows_scanned"]))
for row in sub["top_k"]:
    k.show("  row %d" % row["idx"], "dist %.12g" % row["dist"])
k.expect("only the listed rows were considered", sub["rows_scanned"] == len(subset)
         and all(r["idx"] in subset for r in sub["top_k"]) and sub["top_k"][0]["idx"] == 7)

k.section("11. A rebuild retrains from the current corpus")
rebuilt = k.js("FRACTAL.INDEX.REBUILD", KEY, NAME, "VECTOR_FORMAT", "FP32")
k.show("generation after the rebuild", str(rebuilt["generation"]))
info = k.js("FRACTAL.INDEX.INFO", KEY, NAME)
k.expect("after the rebuild nothing is stale", info["stale"] is False)

k.section("12. Drop the index")
k.show("drop", str(k.call("FRACTAL.INDEX.DROP", KEY, NAME)))
k.show("the index keys left", str(len(k.call("KEYS", KEY + ":fsql-idx:*"))))
k.call("DEL", KEY)

k.done()
