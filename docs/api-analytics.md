<p align="center">
  <img src="../FractalSQLforValkey.jpg" alt="FractalSQL for Valkey" width="720">
</p>

# Analytics API Reference

Pure-compute commands over packed float32 blobs. Every one runs on a worker thread (except the vector math, which takes
microseconds and runs inline), decodes its arguments on the main thread, and refuses NaN and Inf. All are read-only and are not
propagated to replicas.

Series, matrices and vectors are **packed little-endian float32**: in Python, `struct.pack("<%df" % n, *values)`. Index arrays
(`edges`, `faces`) are **packed little-endian uint32**. Errors name the offending argument; an argument over 16 MiB is refused. Every
example below was run against a live server and the reply shown is its real output; the series are seeded random data, so yours will
differ in the digits.

## Fractal Dimension Analysis

### `FRACTAL.DIM.DFA`

Detrended Fluctuation Analysis: the scaling exponent of a series. About 0.5 for uncorrelated noise, 1.0 for 1/f noise, 1.5 for a random
walk, so it separates noise, persistent trends and structured signals.

```
FRACTAL.DIM.DFA series
```

- `series`: packed float32, at least 16 samples. A **constant** series has no variance to scale and is refused.

Returns the exponent as a double.

```
> FRACTAL.DIM.DFA noise
0.6910716465466882
```

### `FRACTAL.DIM.BOXCOUNT`

The Minkowski-Bouligand (box-counting) dimension of a point trace: about 1 for a line, towards 2 for a path that fills the plane.

```
FRACTAL.DIM.BOXCOUNT points dim
```

- `points`: `N x dim` packed float32, flattened (`x, y, x, y, ...`), at least 8 points.
- `dim`: the number of coordinates per point, 1 to 1024.

The input should be **path-like**, such as a track, a coastline or a meander: independent random points are refused as invalid input.

```
> FRACTAL.DIM.BOXCOUNT trace 2   # a 200-point meandering trace
1.2512501702645915
```

### `FRACTAL.DIM.DRIFT`

Detects a regime change by comparing the DFA exponent of the last `window` samples against everything before.

```
FRACTAL.DIM.DRIFT series window
```

- `window`: at least 16. The series needs at least `window + 16` samples.

Returns `{"drift","recent_alpha","baseline_alpha"}`, where `drift` is the difference of the two exponents. It responds to a change in
**dynamics** (white noise turning into a random walk, or an accelerating ramp), not to a constant level shift, which DFA removes.

```
> FRACTAL.DIM.DRIFT ramp 32
{"drift":0.8624542933,"recent_alpha":2.2418604101,"baseline_alpha":1.3794061169}
```

## Time-Series and Topology

### `FRACTAL.SERIES.CHANGEPOINT`

Localizes where a series' mean or variance shifted, complementing DFA's overall characterization.

```
FRACTAL.SERIES.CHANGEPOINT series window threshold [max_points]
```

- `window`: samples on each side of a candidate split; use 50 or more to find a variance shift.
- `threshold`: how large the two-sample statistic must be, in standard units.
- `max_points`: at most this many change points, default 64.

Returns `{"indices":[...]}`.

```
> FRACTAL.SERIES.CHANGEPOINT shift 50 4   # variance jumps 5x at sample 150
{"indices":[150]}
```

### `FRACTAL.SERIES.PERIODOGRAM`

Exact power spectrum (a direct DFT) for cadence detection, such as network beaconing or retry loops.

```
FRACTAL.SERIES.PERIODOGRAM series [max_peaks]
```

Returns `{"freqs":[...],"power":[...]}`, strongest first, up to `max_peaks` (default 8). Frequencies are cycles per sample, so the
period is `1 / freq`.

```
> FRACTAL.SERIES.PERIODOGRAM sine 3   # a period-8 tone in noise
{"freqs":[0.125,0.0703125,0.28125],"power":[35.40291134,0.3378074588,0.2781065946]}
```

### `FRACTAL.SERIES.SPECTROGRAM`

Per-window spectral entropy of the power spectrum: low means a few strong frequencies (a beacon, a locked carrier), high means noise. Pair
it with change-point localization to find when order is lost.

```
FRACTAL.SERIES.SPECTROGRAM series window hop [max_frames]
```

- `window`: 8 to 512. `hop`: samples between frames. `max_frames`: default 64.

Returns an array of entropies in [0, 1]. A constant frame reports 0, never NaN.

```
> FRACTAL.SERIES.SPECTROGRAM sine 32 32
[0.4428024488,0.3792943952,0.4964818388,0.4848813682]
```

### `FRACTAL.TDA.PERSISTENCE`

Exact 0-dimensional persistence over a point cloud's Vietoris-Rips filtration (single-linkage clustering), plus the graph cycle rank as an
informational Betti-number estimate.

```
FRACTAL.TDA.PERSISTENCE points dim max_dim max_thresh [max_bars]
```

- `points`: 2 to 512 points. `max_dim`: 0 or 1; with 0, `betti1` is `null`. `max_thresh`: the filtration radius. `max_bars`: default 1024.

Returns `{"n_h0_bars","h0_bars":[{"birth","death"}],"betti1"}`. `betti1` is the cycle rank of the 1-skeleton graph, not the full simplicial H1.

```
> FRACTAL.TDA.PERSISTENCE points 2 1 2.0   # two well-separated clusters
{"n_h0_bars":28,"h0_bars":[{"birth":0.0,"death":0.0308452132},{"birth":0.0,"death":0.0473850952},{"birth":0.0,"death":0.0558404116},{"birth":0.0,"death":0.0698111782},{"birth":0.0,"death":0.0703736562},{"birth":0.0,"death":0.0822542927},{"birth":0.0,"death":0.0822737067},{"birth" …
```

### `FRACTAL.SERIES.FINGERPRINT`

A SimHash: folds a state vector into a Hamming-comparable fingerprint where nearly-identical states collide.

```
FRACTAL.SERIES.FINGERPRINT vector bits [seed]
```

`bits` is 1 to 4096; the reply is a blob of `bits / 8` bytes (rounded up). It captures a state's **direction**: two vectors pointing the
same way give the same fingerprint however long they are.

```
> FRACTAL.SERIES.FINGERPRINT state 64 1
<8 bytes: the 64-bit fingerprint>
```


### `FRACTAL.CYCLE.DETECT`

Brent's cycle detection over a stream of fingerprints, tolerating a few differing bits.

```
FRACTAL.CYCLE.DETECT fingerprints n_bytes [hamming]
```

- `fingerprints`: fingerprints concatenated, `n_bytes` each. `hamming`: how many bits two fingerprints may differ and still count as the same
  state, default 0.

Returns `{"detected":true,"cycle_len":N,"at_index":I}` or `{"detected":false}`.

```
> FRACTAL.CYCLE.DETECT fingerprints 8 0   # a 5-state loop repeated 4 times
{"detected":true,"cycle_len":5,"at_index":12}
```

## Domain-Specific Geometry

These take **pre-extracted** geometry (a vessel graph, a mesh, a skeleton), not raw imaging. The fractal-dimension step inside the vascular and nerve
commands needs on the order of 3,000 nodes to find enough scales, and smaller inputs are refused as invalid.

### `FRACTAL.MORPH.VASCULAR`

Vessel-network tortuosity and branch density.

```
FRACTAL.MORPH.VASCULAR coords edges arc_lengths
```

`coords` are x, y, z triples; `edges` are node-index pairs (packed uint32), with one arc length per edge. An edge naming a node that does not
exist is refused. Returns `{"mean_tortuosity","branch_density","fractal_dimension"}`; tortuosity is at least 1.

```
> FRACTAL.MORPH.VASCULAR coords edges arcs   # a 3,000-node vessel tree
{"mean_tortuosity":1.0499999988,"branch_density":0.0367021708,"fractal_dimension":2.8418482272}
```

### `FRACTAL.MORPH.CORTICAL`

The Gyrification Index of a cortical surface mesh: mesh area over convex-hull area, 1 for a convex mesh.

```
FRACTAL.MORPH.CORTICAL vertices faces
```

`vertices` are x, y, z triples; `faces` are vertex-index triples (packed uint32). Returns `{"mesh_area","hull_area","gyrification_index"}`.

```
> FRACTAL.MORPH.CORTICAL vertices faces   # a tetrahedron
{"mesh_area":2.3660254038,"hull_area":2.3660254038,"gyrification_index":1.0}
```

### `FRACTAL.MORPH.NERVE`

Nerve-fibre plexus density and dimension, in the convention used by corneal confocal microscopy.

```
FRACTAL.MORPH.NERVE coords dim edges
```

Returns `{"fiber_length_density","branch_density","fractal_dimension"}`.

```
> FRACTAL.MORPH.NERVE nodes 2 edges   # a 3,000-node skeleton
{"fiber_length_density":156.7461817439,"branch_density":7.428380209,"fractal_dimension":1.8515059127}
```

### `FRACTAL.MORPH.COMPLEXITY`

Morphological complexity of a segmented mask or a sensor layout: box-counting dimension plus lacunarity (higher for clumped, gappy placement).

```
FRACTAL.MORPH.COMPLEXITY points dim
```

It needs on the order of a hundred points or more. Returns `{"dimension","lacunarity"}`.

```
> FRACTAL.MORPH.COMPLEXITY grid 2   # a 400-sensor jittered grid
{"dimension":1.5484307696,"lacunarity":1.464375}
```

## Portfolio Optimization

### `FRACTAL.OPTIMIZE.PORTFOLIO`

Cardinality-constrained maximum-Sharpe portfolio selection with the SFS engine: the best `k` assets in a large universe without the exponential
cost of a brute-force search.

```
FRACTAL.OPTIMIZE.PORTFOLIO mu cov k [seed [use_obl [gaussian|levy]]]
```

- `mu`: expected returns, `n` values. `cov`: the `n x n` covariance matrix, row-major. At most 512 assets.
- `k`: 1 to `n`. At most `k` weights are nonzero and they sum to 1.
- `seed`: the same seed returns the same portfolio. `use_obl`: 0 or 1. The last argument selects the diffusion mode.

Returns `{"sharpe","weights":[...]}`.

```
> FRACTAL.OPTIMIZE.PORTFOLIO mu cov 3 7
{"sharpe":1.0567622504,"weights":[0.0,0.3130193854,0.0,0.4044321458,0.0,0.2825484688]}
```

### `FRACTAL.OPTIMIZE.MULTIMODAL` and `FRACTAL.OPTIMIZE.MULTIMODAL.PARETO` (Enterprise)

Several structurally different portfolios instead of one. They run in the enterprise library, which only `fractalsqld` loads, so on a
Community daemon they answer "enterprise tier not loaded". Each call is also written to the decision ledger as an audit record.

```
FRACTAL.OPTIMIZE.MULTIMODAL        mu cov k n_restarts overlap_threshold quality_frac [seed]
FRACTAL.OPTIMIZE.MULTIMODAL.PARETO mu cov k n_restarts [max_front [seed]]
```

- `n_restarts`: 1 to 64 independent searches. `overlap_threshold`: 0 to 1, how much two portfolios may share and still count as different.
  `quality_frac`: above 0 and at most 1, how close to the best Sharpe a candidate must be.
- `.PARETO` returns the non-dominated return-versus-risk front; `max_front` defaults to `n_restarts`.

Returns `{"n_found":N,"candidates":[{"sharpe","weights"}, ...]}` (the Pareto form adds `"return"` and `"risk"` to each candidate), best first.
`FRACTAL.AGENT.DIVERSE_PORTFOLIOS` wraps this with a reasoned comparison ([api-agency.md](api-agency.md)).

## Per-document feature vectors

Store a document's feature vector in a string key or a hash of its own, and search it with `FRACTAL.SEARCH`.

## Vector Math and Quantization

### The canonical vector form

A vector is a **packed little-endian float32 blob**: byte-stable across architectures and binary-safe in any Valkey string, 4 bytes per value
and no parsing. `FRACTAL.VECTOR.FROMTEXT` and `TOTEXT` convert to and from `[1,2,3]` text for interchange. Vector math is inline, float32,
and refuses a dimension mismatch and NaN/Inf. A zero-norm input to `COSINE` or `SIMILARITY` returns distance 1 and similarity 0 (the
core's documented convention), never NaN.

| Command | Returns |
| --- | --- |
| `FRACTAL.VECTOR.DOT a b` · `L2 a b` · `L2SQ a b` · `COSINE a b` · `SIMILARITY a b` | double |
| `FRACTAL.VECTOR.LP a b p` | the L_p distance (`p > 0`; for `0 < p < 1` it is not a proper metric, so use it deliberately) |
| `FRACTAL.VECTOR.NORM a` | double |
| `FRACTAL.VECTOR.NORMALIZE a` · `ADD a b` · `SUB a b` · `SCALE a s` | blob |
| `FRACTAL.VECTOR.CONCAT a alpha b beta` | blob: `alpha*a` followed by `beta*b` (a weighted blend for cross-modal search) |
| `FRACTAL.VECTOR.QUANTIZE.INT8 a` | `{"scale","codes":[...]}`: 4x smaller |
| `FRACTAL.VECTOR.QUANTIZE.BINARY a` | packed sign bits: 32x smaller |
| `FRACTAL.VECTOR.HAMMING a b` | integer bit distance between two binary codes |
| `FRACTAL.VECTOR.FROMTEXT "1,2,3"` / `"[1,2,3]"` · `TOTEXT a` | blob · `"[1,2,3]"` |

```
> FRACTAL.VECTOR.COSINE a b
0.3333333730697632
```

```
> FRACTAL.VECTOR.QUANTIZE.INT8 a
{"scale":0.03149606287,"codes":[32,64,95,127]}
```

```
> FRACTAL.VECTOR.TOTEXT a
[1,2,3,4]
```

### Quantization with a re-rank

The usual pattern for large catalogs: store a 1-bit code beside each full-precision vector, pre-filter with `FRACTAL.VECTOR.HAMMING` (a handful of
bytes per comparison), then re-rank the survivors at full precision. The `agentic-edge-swarm` kit shows the compression ratios and that
similar vectors stay close in Hamming distance.

