<p align="center">
  <img src="FractalSQLforValkey.jpg" alt="FractalSQL for Valkey" width="720">
</p>

Third-Party Notices

This file contains licensing and attribution notices for third-party software components incorporated into FractalSQL.

### 1. SFS (Stochastic Fractal Search) Algorithms
Component: SFS Core Math & Stochastic Convergence Logic
Source: Based on "Stochastic Fractal Search" (Salimi 2014)
License: BSD-2-Clause

Copyright (c) 2014, Hamid Salimi. All rights reserved.

Redistribution and use in source and binary forms, with or without
modification, are permitted provided that the following conditions are
met:

    * Redistributions of source code must retain the above copyright
      notice, this list of conditions and the following disclaimer.
    * Redistributions in binary form must reproduce the above copyright
      notice, this list of conditions and the following disclaimer in
      the documentation and/or other materials provided with the distribution.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE
LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
POSSIBILITY OF SUCH DAMAGE.

-------------------------------------------------------------------------------

### 2. LuaJIT PRNG (lj_prng.c)
Component: Tausworthe-223 pseudo-random number generator, ported to C from LuaJIT's lj_prng.c and baked into the vendored core archive. The LuaJIT runtime itself is not linked.
Source: https://luajit.org/
License: MIT License

Copyright (C) 2005-2023 Mike Pall. All rights reserved.

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in
all copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
THE SOFTWARE.

-------------------------------------------------------------------------------

### 3. Redis module API header (include/redismodule.h)
Component: redismodule.h, the module API header, vendored unmodified from the
Redis 7.2 source tree (the last Redis release line under the BSD-3-Clause
license). It declares the stable module ABI that Valkey implements; the module
is compiled against it and does not link any Redis or Valkey code.
Source: https://github.com/redis/redis (branch 7.2)
License: BSD-3-Clause

Copyright (c) 2006-2020, Salvatore Sanfilippo
All rights reserved.

Redistribution and use in source and binary forms, with or without
modification, are permitted provided that the following conditions are met:

    * Redistributions of source code must retain the above copyright notice,
      this list of conditions and the following disclaimer.
    * Redistributions in binary form must reproduce the above copyright
      notice, this list of conditions and the following disclaimer in the
      documentation and/or other materials provided with the distribution.
    * Neither the name of Redis nor the names of its contributors may be used
      to endorse or promote products derived from this software without
      specific prior written permission.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE
LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
POSSIBILITY OF SUCH DAMAGE.

-------------------------------------------------------------------------------

### 4. Algorithm Attributions (Original Implementations)

The vendored core artifact (v2.x, community-sovereign) includes several
components that are ORIGINAL C implementations of published algorithms/
methods, not ports or incorporations of existing third-party source code,
so no third-party license text applies. Listed here as academic
attribution for the published method each implementation follows, per
standard practice for algorithm-based (rather than code-derived)
components. Mirrors core's own THIRD-PARTY-NOTICES-COMMUNITY.md.

- **HNSW**: Malkov, Y. A., & Yashunin, D. A. (2016, revised 2018).
  "Efficient and Robust Approximate Nearest Neighbor Search Using
  Hierarchical Navigable Small World Graphs." Implemented in the vendored
  core for a planned persistent-index feature; not currently used by
  `FRACTAL.SEARCH`/`FRACTAL.EXPLORE`, whose fitness is evaluated against the
  whole corpus on every call (see `bench/README.md`).
- **DFA (Detrended Fluctuation Analysis)** (`FRACTAL.DIM.DFA`,
  `FRACTAL.DIM.DRIFT`): Peng, C.-K., Buldyrev, S. V., Havlin, S.,
  Simons, M., Stanley, H. E., & Goldberger, A. L. (1994). "Mosaic
  organization of DNA nucleotides."
- **Box-counting (Minkowski-Bouligand) dimension**
  (`FRACTAL.DIM.BOXCOUNT`, and shared by the domain-geometry
  functions below): standard, widely-used fractal-dimension estimation
  technique; not attributed to a single originating paper.
- **Lacunarity, fixed-grid variant** (used by
  `FRACTAL.MORPH.COMPLEXITY`): Plotnick, R. E., Gardner, R. H.,
  & O'Neill, R. V. (1996). "Lacunarity indices as measures of landscape
  texture."
- **Gyrification Index** (`FRACTAL.MORPH.CORTICAL`): Zilles, K.,
  Armstrong, E., Schleicher, A., & Kretschmann, H. J. (1988). "The human
  pattern of gyrification in the cerebral cortex."
- **Vascular tortuosity (arc-chord ratio) / branch density**
  (`FRACTAL.MORPH.VASCULAR`): standard vascular morphometry measures,
  not attributed to a single originating paper.
- **Corneal Nerve Fractal Dimension (CNFrD) convention**
  (`FRACTAL.MORPH.NERVE`): parameter conventions match those
  used by corneal confocal microscopy (CCM) analysis tools such as
  ACCMetrics.
- **Cardinality-constrained portfolio optimization**
  (`FRACTAL.OPTIMIZE.PORTFOLIO`): a hardcoded objective template built
  on entry 1's SFS engine (project-then-evaluate against a Sharpe-ratio
  objective); not itself a port of a separate published algorithm.
- **SimHash / random-projection state fingerprint** (`FRACTAL.SERIES.FINGERPRINT`):
  Charikar, M. S. (2002). "Similarity estimation techniques from
  rounding algorithms." Proceedings of the 34th Annual ACM Symposium on
  Theory of Computing (STOC), pp. 380-388.
- **Brent's cycle detection** (`FRACTAL.CYCLE.DETECT`): Brent, R. P.
  (1980). "An improved Monte Carlo factorization algorithm." BIT
  Numerical Mathematics, 20(2), 176-184. This module implements Brent's
  tortoise-and-hare checkpoint schedule, adapted to run incrementally
  over a live stream of values rather than a freely-callable function --
  an extension of the published algorithm, not a direct port.
- **Periodogram** (`FRACTAL.SERIES.PERIODOGRAM`): Schuster, A. (1898). "On
  the investigation of hidden periodicities with application to a
  supposed 26 day period of meteorological phenomena." Terrestrial
  Magnetism, 3(1), 13-41. The classical periodogram, computed here via
  direct DFT.
- **TDA persistence diagram, 0-dimensional exact + graph Betti number**
  (`FRACTAL.TDA.PERSISTENCE`): Edelsbrunner, H., Letscher, D.,
  & Zomorodian, A. (2002). "Topological persistence and simplification."
  Discrete & Computational Geometry, 28(4), 511-533. The 0-dimensional
  persistence computation (single-linkage clustering via Kruskal's
  algorithm) is an exact instance of this theory. The reported first
  Betti number is the cycle rank of the underlying Vietoris-Rips graph
  (edges minus vertices plus connected components), not the full
  simplicial first homology group the cited paper's boundary-matrix
  reduction method would produce.
- **Vector L_p distance and scalar quantization** (`FRACTAL.VECTOR.LP`,
  `FRACTAL.VECTOR.QUANTIZE.INT8`, `FRACTAL.VECTOR.QUANTIZE.BINARY`,
  `FRACTAL.VECTOR.HAMMING`): standard, widely-used numerical
  techniques (the Minkowski/p-norm family; symmetric per-vector min-max
  int8 quantization; 1-bit sign quantization with Hamming-distance
  comparison), not attributed to a single originating paper.
- **Change-point detection** (`FRACTAL.SERIES.CHANGEPOINT`): a windowed
  two-sample location/scale test scanned over candidate split points,
  not attributed to a single originating paper. Deliberately NOT a CUSUM
  implementation -- it reports the argmax split location and per-window
  statistics directly, without the cumulative-sum decision scheme.
- **Cardinality-constrained subset optimization, generic objective**
  (core function `fsql_optimize_subset`; not yet exposed as a command): a value-weighted-allocation objective
  template built on entry 1's SFS engine (project-then-evaluate against
  a weighted-sum objective with per-item upper bounds and a hard item
  cap); generalizes the portfolio entry above beyond a
  Sharpe-ratio-specific objective, not itself a port of a separate
  published algorithm.

-------------------------------------------------------------------------------

### 5. Companion software (used, not distributed)
FractalSQL for Valkey does not depend on, link, load, or redistribute any of the
following. They are named here because the documentation, the `search` Docker
Compose profile, `bench/head_to_head.py` and build_test.sh gate 14 run FractalSQL
next to them, using the unmodified official images.

  - Valkey (https://github.com/valkey-io/valkey): BSD-3-Clause. The server the
    module runs in; the official `valkey/valkey` image is used for the default
    Compose service.
  - valkey-search (https://github.com/valkey-io/valkey-search): BSD-3-Clause.
    Provides FT.CREATE / FT.SEARCH (HNSW) for the head-to-head benchmark.
  - valkey-json (https://github.com/valkey-io/valkey-json): BSD-3-Clause.
  - valkey-bundle (https://github.com/valkey-io/valkey-bundle): BSD-3-Clause. The
    `valkey/valkey-bundle` image packages Valkey with valkey-search, valkey-json,
    valkey-bloom and valkey-ldap; its components keep their own licenses.

No source or binary from these projects is copied into this repository, the
module, or its packages. License identifiers above come from each project's
GitHub repository page; consult the project's own LICENSE file for the
authoritative text. The images are pulled by the user at run time.
