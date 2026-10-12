# Magnifier rendering benchmark

Build `snow-canvas-magnifier-benchmark` with `windows-msvc-performance` or
`snow-shot-macos-{arm64|x64}-performance` (Release).
Run only this benchmark; packaging and Debug presets omit benchmark targets.

The executable measures one lens, 24 mixed-shape lenses, independent destination
movement, source movement, and 24 lenses above 1,000 annotations. All lenses use
10× enlargement. It prints mean, median, and p95 milliseconds as CSV, verifies
that original images remain shared, and rejects any filter-path annotation replay.

`creation` includes the dashed source contour on one lens, and `selected` includes
source contours and selectable dashed frames on 24 mixed-shape lenses. Compare
these with `single` and `many` to measure the guide painting cost.

Use `--scenario single|many|lens_drag|source_drag|annotations|creation|selected|layered_opacity`
to select one case and `--iterations N` to set measured frames (100 by default,
after five warmups). The default appearance uses `--connection gradient`
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
