#!/usr/bin/env python3
"""tests/gcov_union.py -- per-file line and branch coverage from gcov's JSON output, UNIONED over several builds of the same sources.

The module is built twice under --coverage (the shipped flavor and the allocation-failure flavor gate 11 runs); each has its own
.gcno/.gcda set, so gcov alone reports them separately. This merges them by source file and (line, branch) so the total says what the
whole suite reached.

usage: gcov_union.py <repo-root> <source-suffix-regex> <build-dir> [<build-dir> ...]

Runs `gcov --json-format --stdout --branch-probabilities` on every .gcno under <build-dir> that has a .gcda and
reports the source files whose path matches <source-suffix-regex> (e.g. 'mcp/src/[^/]+\\.cpp$'). Exception-edge
branches (gcov's "throw" flag: C++ implicit unwinding paths) are not counted: they are not decisions in the code.
Prints one row per file and a TOTAL row; the last line is machine-readable: TOTAL <lines%> <funcs%> <branches%>.
"""
import gzip, json, os, re, subprocess, sys

def main():
    repo, pat = sys.argv[1], re.compile(sys.argv[2])
    builds = sys.argv[3:]
    excl = None
    per = {}
    walk = [(root, files) for b in builds for root, _, files in os.walk(b)]
    for root, files in walk:
        for fn in files:
            if not fn.endswith(".gcno"): continue
            base = os.path.join(root, fn[:-5])
            if not os.path.exists(base + ".gcda"): continue
            r = subprocess.run(["gcov", "--json-format", "--stdout", "--branch-probabilities", os.path.basename(base) + ".gcno"],
                               cwd=root, capture_output=True)
            for doc in r.stdout.splitlines():
                doc = doc.strip()
                if not doc.startswith(b"{"): continue
                try: j = json.loads(doc)
                except ValueError: continue
                for f in j.get("files", []):
                    name = os.path.realpath(os.path.join(repo, f["file"]))   # one entry per source file however the object named it
                    if not pat.search(name) or (excl and excl.search(name)): continue
                    d = per.setdefault(name, {"lines": {}, "funcs": {}, "br": {}})
                    for ln in f.get("lines", []):
                        n = ln["line_number"]
                        d["lines"][n] = d["lines"].get(n, 0) + ln["count"]
                        for i, b in enumerate(ln.get("branches", [])):
                            if b.get("throw"): continue
                            k = (n, i)
                            d["br"][k] = d["br"].get(k, 0) + b["count"]
                    for fu in f.get("functions", []):
                        k = fu["name"]
                        d["funcs"][k] = d["funcs"].get(k, 0) + fu["execution_count"]
    def pc(h, t): return "%5.1f%%" % (100.0 * h / t) if t else "   n/a"
    tot = [0, 0, 0, 0, 0, 0]
    print("  %-24s %8s %8s %9s" % ("file", "lines", "funcs", "branches"))
    for name in sorted(per, key=os.path.basename):
        d = per[name]
        lh, lt = sum(1 for c in d["lines"].values() if c > 0), len(d["lines"])
        fh, ft = sum(1 for c in d["funcs"].values() if c > 0), len(d["funcs"])
        bh, bt = sum(1 for c in d["br"].values() if c > 0), len(d["br"])
        for i, v in enumerate((lh, lt, fh, ft, bh, bt)): tot[i] += v
        print("  %-24s %8s %8s %9s" % (os.path.basename(name), pc(lh, lt), pc(fh, ft), pc(bh, bt)))
    print("  %-24s %8s %8s %9s" % ("TOTAL", pc(tot[0], tot[1]), pc(tot[2], tot[3]), pc(tot[4], tot[5])))
    print("  lines %d/%d  functions %d/%d  branches %d/%d" % tuple(tot))
    print("TOTAL %.1f %.1f %.1f" % tuple(100.0 * tot[i] / tot[i + 1] if tot[i + 1] else 0.0 for i in (0, 2, 4)))

main()
