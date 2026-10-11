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
after five warmups). `layered_opacity` measures 24 lenses at 50% opacity sampling
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
