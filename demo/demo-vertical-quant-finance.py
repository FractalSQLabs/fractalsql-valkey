#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Quantitative finance: a cardinality-constrained book and a volatility regime change.

25 synthetic assets under a 4-factor covariance model, optimised down to an
8-asset book (FRACTAL.OPTIMIZE.PORTFOLIO), and a return series whose volatility
triples at t=300, localised with FRACTAL.SERIES.CHANGEPOINT and confirmed with
FRACTAL.DIM.DRIFT (DFA)."""
import math, statistics
from fvlib import Kit, pack, flat

k = Kit("quant-finance")
rng, n, K = k.rng, 25, 8

k.section("1. An 8-asset book from 25 candidates")
load = [[rng.gauss(0, 0.2) for _ in range(4)] for _ in range(n)]
idio = [rng.uniform(0.01, 0.03) for _ in range(n)]
cov = [[sum(load[i][f] * load[j][f] for f in range(4)) * 0.0004 + (idio[i] ** 2 if i == j else 0.0)
        for j in range(n)] for i in range(n)]
mu = [rng.uniform(0.02, 0.14) for _ in range(n)]
res = k.js("FRACTAL.OPTIMIZE.PORTFOLIO", pack(mu), pack(flat(cov)), K, 7)
w = res["weights"]
held = sum(1 for x in w if x > 1e-9)
k.show("sharpe of the optimised book", "%.3f" % res["sharpe"])
k.show("assets held / weights sum", "%d / %.6f" % (held, sum(w)))
k.expect("the book holds at most %d assets" % K, held <= K)
k.expect("the weights sum to 1", abs(sum(w) - 1) < 1e-6)

def sharpe_of(wv):
    ret = sum(a * b for a, b in zip(wv, mu))
    var = sum(wv[i] * wv[j] * cov[i][j] for i in range(n) for j in range(n))
    return ret / math.sqrt(var)
rand_sharpes = []
for _ in range(300):
    pick = rng.sample(range(n), K)
    wv = [0.0] * n
    for p in pick:
        wv[p] = 1.0 / K
    rand_sharpes.append(sharpe_of(wv))
med = statistics.median(rand_sharpes)
k.show("median sharpe of 300 random equal-weight books", "%.3f" % med)
k.expect("the optimised book beats the median random book", res["sharpe"] > med, "%.3f vs %.3f" % (res["sharpe"], med))
again = k.js("FRACTAL.OPTIMIZE.PORTFOLIO", pack(mu), pack(flat(cov)), K, 7)
k.expect("the same seed reproduces the same book", again == res)

k.section("2. A regime change in the return series")
calm = [rng.gauss(0, 0.01) for _ in range(300)]
wild = [rng.gauss(0, 0.03) for _ in range(200)]
series = calm + wild
# Window 50: a variance ratio over 30-sample windows is statistically noisy.
cp = k.js("FRACTAL.SERIES.CHANGEPOINT", pack(series), 50, 2.0)["indices"]
k.show("change points found", cp)
k.expect("a change point lands within 40 samples of t=300", any(abs(i - 300) <= 40 for i in cp))
control = k.js("FRACTAL.SERIES.CHANGEPOINT", pack(calm + [rng.gauss(0, 0.01) for _ in range(200)]), 50, 2.0)["indices"]
k.expect("a series with no regime change is not flagged", len(control) == 0, str(control))
drift = k.js("FRACTAL.DIM.DRIFT", pack(series), 120)
k.show("DFA drift (recent window vs baseline)", "%.3f" % drift["drift"])
k.expect("the drift report carries both alphas", all(x in drift for x in ("recent_alpha", "baseline_alpha")))
k.done()
