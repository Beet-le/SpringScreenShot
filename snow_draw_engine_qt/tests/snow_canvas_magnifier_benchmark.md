# Magnifier rendering benchmark

Build `snow-canvas-magnifier-benchmark` with `windows-msvc-performance` or
`snow-shot-macos-{arm64|x64}-performance` (Release).
Run only this benchmark; packaging and Debug presets omit benchmark targets.

The executable measures one lens, 24 mixed-shape lenses, independent destination
movement, source movement, and 24 lenses above 1,000 annotations. All lenses use
10× enlargement. It prints mean, median, and p95 milliseconds as CSV, verifies
that original images remain shared, and rejects any filter-path annotation replay.
The final frame's complete pixel hash is calculated after timing, allowing exact
output comparisons between implementations.

`creation` includes the dashed source contour on one lens, and `selected` includes
source contours and selectable dashed frames on 24 mixed-shape lenses. Compare
these with `single` and `many` to measure the guide painting cost.

Use `--scenario single|many|lens_drag|source_drag|annotations|creation|selected|layered_opacity|layered_large`
to select one case and `--iterations N` to set measured frames (100 by default,
after five warmups). Use `--dpr 0.25..4` (1 by default) to set device pixel ratio;
the logical surface remains 1920×1080 while the physical output scales with DPR.
The CSV reports DPR, composited strip count, and peak scratch pixels.
The default appearance uses `--connection gradient`
and `--source-outline on`; use `--connection none|leader-line|gradient` and
`--source-outline on|off` to isolate decoration costs. Compare the same scenario
with `--connection none --source-outline on` and
`--connection gradient --source-outline on` for equivalent source outlines.
The CSV includes contour and hull build counts for the last measured frame.
It also reports disjoint and masked bridge counts. Add `--timings` for optional
clip, gradient-fill, and source-outline microseconds in the last frame; timings
are disabled during normal rendering and standard throughput comparisons.
Static scenarios reject geometry rebuilds after warmup; movement scenarios
rebuild only changed geometry. Gradient paths are retained per owned display item,
with adaptive curve flattening bounded to 0.125 physical pixels and higher
resolution reused when zooming out. Decorations create no raster buffers and do
not change pristine image sampling or compositing scratch budgets.
Disjoint bodies use exact common tangents and original inward cubic arcs in one
antialiased fill path. Support maxima use quadratic derivative roots, including
flat rectangle edges and parallel one-times tangents. One original-contour
exterior mask for disjoint bounds also excludes subpixel differences in Qt's
rasterization of full and reparameterized cubic arcs. Overlapping bounds use two
masks around a conservative hull whose outward error is at most 0.125 physical
pixels; containment omits the bridge entirely. Mask containers are simple
rectangles with a two-physical-pixel guard in every transformed direction. A
larger guard is retained when a higher-resolution path is reused at lower scale.
The straight tangent edges use collinear segments no longer than 16 physical
pixels. This preserves the continuous boundary while allowing Qt's antialiased
band rasterizer to reject segments outside each band without walking a long edge
again. Segment counts share the bounded contour budget and scale cache.

`layered_opacity` measures 24 lenses at 50% opacity sampling
a screenshot and an overlapping translucent source layer. It verifies that
compositing scratch never exceeds 65,536 physical pixels at this surface width
and original images remain shared; the other scenarios verify that no compositing
scratch is used.
Single-source lenses paint immutable original images directly, without a sampled
crop or enlarged image surface. Layered lenses compose only exposed destination
pixels in horizontal strips before applying the lens mask and opacity. Keeping
each complete sampling row preserves Qt's interpolation across strip boundaries.
Scratch uses at most 65,536 physical pixels, or one complete row when the exposed
physical width exceeds that budget.

`layered_large` isolates compositing with one 1600×900 elliptical lens at 50%
opacity sampling two overlapping original layers. Its source contour lies inside
the lens, so it draws no exterior gradient connector. It exercises many strips
at each DPR while keeping the same source-sharing and scratch-budget checks.
The completed lens clip and device transform are retained across exposed strips;
they are installed lazily when the first strip is presented and restored once.
This removes repeated rasterization of the same mask without changing sampling,
antialiasing, opacity, or the scratch allocation.

For a focused Release comparison, run matching scenarios and DPRs against the
same benchmark source, alternate before/after process order, and compare complete
frame hashes alongside latency and memory diagnostics:

```sh
snow-canvas-magnifier-benchmark --scenario layered_opacity --dpr 2 --connection none --iterations 300
snow-canvas-magnifier-benchmark --scenario layered_opacity --dpr 2 --connection gradient --iterations 300
snow-canvas-magnifier-benchmark --scenario layered_large --dpr 2 --connection none --iterations 300
snow-canvas-magnifier-benchmark --scenario many --dpr 2 --connection gradient --iterations 300
```

Local validation on 2026-10-10 with `windows-msvc-performance`, 1920×1080,
300 measured frames: 24 lenses averaged 5.154 ms without guides and 5.146 ms
with selection guides (p95: 5.701 ms and 5.637 ms). The painting cost remained
within measurement variation; original image storage and source draw counts were
unchanged. These cases measure painting with retained guides, excluding overlay
composition and selection handles.

Local validation on 2026-10-11 with `snow-shot-macos-arm64-performance`, Qt 6.12.0,
1920×1080 and 300 measured frames: `many` averaged 2.232 ms (p95: 2.359 ms),
compared with 2.324 ms before the compositing change. `layered_opacity` averaged
5.159 ms (p95: 5.398 ms), drew both immutable layers once per lens, and stayed
within the scratch budget. These results cover offscreen painting.

Local validation on 2026-10-12 with `windows-msvc-performance`, Qt 6.12.0,
1920×1080 and 300 measured frames per mode used source outlines enabled in both
the None and Gradient runs. Bridge antialiasing remained enabled, and all eight
paired scenarios passed the source-sharing, source-draw and scratch-budget checks.
Each table cell reports mean / p50 / p95 milliseconds.

| Scenario | None | Gradient |
| --- | ---: | ---: |
| `single` | 0.135 / 0.127 / 0.188 | 0.567 / 0.565 / 0.678 |
| `many` | 5.967 / 5.806 / 7.045 | 16.158 / 16.160 / 19.069 |
| `lens_drag` | 6.087 / 6.014 / 6.916 | 17.622 / 17.694 / 21.055 |
| `source_drag` | 5.815 / 5.774 / 6.537 | 18.283 / 18.518 / 22.322 |
| `annotations` | 12.229 / 11.408 / 16.696 | 22.687 / 23.382 / 27.745 |
| `creation` | 0.125 / 0.121 / 0.167 | 0.454 / 0.441 / 0.592 |
| `selected` | 6.248 / 6.184 / 6.832 | 15.773 / 15.560 / 18.096 |
| `layered_opacity` | 13.030 / 13.181 / 15.697 | 21.984 / 22.586 / 25.886 |

The representative 24-lens `many` Gradient case remains 170.8% above the
equivalent None case, exceeding the 25% investigation threshold used during
tuning. Large antialiased bridge fills and exact original-contour clipping
account for the remaining cost. Diagnostic solid-color fills stayed close to
gradient fills, while disabling only bridge antialiasing sharply reduced fill
time. Product rendering retains full antialiasing and strict body exclusion.
Separate 100-frame diagnostic probes reduced the initial approximately 25.6 ms
Gradient mean to approximately 14–17 ms after retaining vector geometry, using
exact curved bridges with one disjoint contour mask, and splitting long tangent
edges into collinear 16-physical-pixel segments. Those probes are diagnostic
experiments, not a direct comparison with the final 300-frame measurements.

Static cases rebuilt no contours or hulls after warmup. The two movement cases
changed all 24 lenses each frame and rebuilt only their affected geometry;
Gradient also rebuilt their connector precision. Source draws remained one per
lens in both modes, or two per lens for `layered_opacity`. Decorations added no
raster surfaces or pristine-source image draws; the existing layered compositing
scratch bound remained unchanged.

Local validation of retained compositing clips on 2026-10-12 used Apple M4,
Qt 6.12.0, and `snow-shot-macos-arm64-performance` with strict Release compilation.
Before and after used the same benchmark source and compiler flags; the before
renderer retained the original per-strip clip installation. Each performance case
ran three independent processes per implementation, alternating process order,
with five warmups and 300 measured frames per process. The table reports the
median of the three process means and p95 values. Source outlines and bridge
antialiasing remained enabled.

| Scenario | DPR | Connection | Before mean / p95 (ms) | After mean / p95 (ms) | Mean reduction |
| --- | ---: | --- | ---: | ---: | ---: |
| `layered_opacity` | 2 | None | 14.917 / 15.282 | 14.141 / 14.490 | 5.2% |
| `layered_opacity` | 2 | Gradient | 25.013 / 25.633 | 24.061 / 24.617 | 3.8% |
| `layered_large` | 1 | None | 8.194 / 8.390 | 4.982 / 5.143 | 39.2% |
| `layered_large` | 2 | None | 38.211 / 38.579 | 17.890 / 18.183 | 53.2% |
| `many` | 2 | Gradient | 16.107 / 16.606 | 16.145 / 16.647 | -0.2% |
| `single` | 2 | Gradient | 0.440 / 0.511 | 0.437 / 0.485 | 0.7% |

The large DPR-2 case presents 90 strips with 64,000 peak scratch pixels. Retaining
one clip avoids 89 redundant lens-mask rasterizations. The direct-source controls
remained within 0.7%, while repeated layered cases exceeded the comparison's
expected reductions: 40% for the large DPR-2 lens, 3% for 24 layered lenses
without connectors, and 2% with gradients. These local checks are not timing
thresholds for CTest.

All 38 before/after final-frame comparisons matched complete pixel hashes and
source-draw, geometry-build, strip-count, and scratch-memory diagnostics. This
includes all nine scenarios at DPR 1 and 1.25, the repeated performance cases,
and layered stress cases at DPR 3. Both implementations passed the focused
magnifier rendering and incremental-damage tests. Those checks include canonical
sampling, sparse exposure, opacity and blend modes, outer transforms, caller
painter-state restoration, and clips whose bounds overlap a lens while their
actual mask intersection is empty. These measurements cover offscreen painting;
widget presentation and input handling are excluded.
