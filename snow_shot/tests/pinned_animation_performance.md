# Pinned animation validation and performance

The playback benchmark exercises the production deadline coordinator, decode workers, transforms
and bounded raster cache. Every published frame is painted with `QPainter` into a managed raster
before its presentation is acknowledged. It measures CPU work and scheduling, rather than native
window compositor latency or battery consumption.

## Run

Use the Release performance preset, never Debug or a packaging preset. Run measurements on an
idle machine, without concurrent builds or other performance tests. Keep hardware, Qt version,
platform plugin and benchmark sources consistent when comparing runs.

```powershell
. ./scripts/snow-build-environment.ps1
Set-SnowBuildEnvironment -Preset windows-msvc-performance | Out-Null
cmake --build --preset build-windows-msvc-performance `
    --target snow-shot-animation-playback-benchmark --parallel 12
$env:QT_QPA_PLATFORM = 'offscreen'
./build/windows-msvc-performance/snow_shot/test-bin/Release/snow-shot-animation-playback-benchmark.exe `
    | Set-Content -Encoding utf8 -LiteralPath build/pinned-animation-performance.json
```

The executable writes JSON to stdout and exits unsuccessfully if a settled idle scenario performs
animation work, a large visible animation fails to advance, or reverse stepping fails to exercise
an actual cache miss and decoder reset. To repeat only the reverse replay case:

```powershell
./build/windows-msvc-performance/snow_shot/test-bin/Release/snow-shot-animation-playback-benchmark.exe `
    --reverse-only | Set-Content -Encoding utf8 -LiteralPath build/pinned-animation-reverse.json
```

To compare codec and storage optimizations independently of GUI scheduling, run the same Release
benchmark sources against both production revisions:

```powershell
./build/windows-msvc-performance/snow_shot/test-bin/Release/snow-shot-animation-playback-benchmark.exe `
    --optimization-only | Set-Content -Encoding utf8 -LiteralPath build/pinned-animation-optimization.json
```

This mode preserves the ordinary full benchmark and uses existing codec APIs. Each cold import and
snapshot preparation scenario has three repetitions lasting at least 1200 ms. Fixture generation
is outside the measured interval. Cold imports include source indexing, first-frame composition
and display preparation, and report the logical sum of initial raster sizes alongside the sum of
distinct storage addresses. Snapshot throughput uses real 1080p and 4K GIF decoders and includes
the ARGB32 premultiplied conversion used for display.

Display P3 WebP and APNG fixtures compare a valid 2 MiB XMP payload against profile-only controls.
Each repetition retains 64 tiny original decoder snapshots and samples process resident and
Windows private memory before retention, during retention, and after release. Pixel/profile
checks after advancing and destroying the decoder validate retained ownership. The sampled
process figures include allocator behavior and other process storage; they are not exact metadata
allocation counts. Private memory is reported as unavailable on platforms without this probe.
A final case warms two controllers sharing one source, retires one, then measures twenty backward
steps on the survivor and reports cache hits, misses and decoder resets. No machine-dependent time
or memory threshold is asserted; correctness failures still make the benchmark fail.

## Fixtures and metrics

Ordinary scenarios warm up for 100 ms, apply the requested paused/suspended state, settle for
25 ms and then measure 750 ms. A 16 ms GUI heartbeat runs only during active measurements.

- Synthetic immutable 320 × 180 frames isolate 10 fps and 60 fps scheduling, twelve simultaneous
  pins, and hidden, thumbnail and paused states.
- Small GIF, WebP and APNG fixtures use real decoding. A generated GIF has twelve changing full
  960 × 540 frames with 20 ms delays and ordinary bounded-dictionary LZW compression. Cases cover
  one pin, four pins, rotation, suspension, pause and 400% speed. A 1920 × 1080 case exercises
  larger publications.
- Reverse replay first walks forty 960 × 540 GIF frames, exceeding the optional cache capacity,
  then measures forty backward steps. It reports decoder resets and replayed composition,
  including the cost of cache misses rather than measuring only retained frame lookup.

The JSON reports wall/process CPU time; per-controller deadline callbacks and shared coordinator
wakes; active deadlines at the end of the observation window; decode requests, publications and
raster repaints; cache hits/misses;
mean decode submission-to-completion latency; maximum decode latency including warm-up; adaptive
preparation lead; peak observed optional cache and mandatory working reservations; and sampled
process resident memory. Active cases include GUI heartbeat p95/maximum lateness. Reverse replay
also includes step p95/maximum latency, decoder resets and composited frame counts. Memory figures
are sampled observations, not an exhaustive allocation trace.

Settled hidden, thumbnail and paused cases must have **zero** controller callbacks, shared
coordinator wakes, active deadlines, decode requests, frame publications and raster repaints.
The idle heartbeat is disabled so the probe does not introduce recurring work. Process CPU time
may contain unrelated runtime work; zero animation work is checked with production counters.
Thumbnail suspension preserves the user's playback choice. Expansion resumes that choice, and
Play/Space on a thumbnail expands it and starts playback.

## Deterministic regression coverage

Run only the related tests; do not run the unfiltered suite. After building the corresponding
targets in `windows-msvc-debug`, the focused CTest selection is:

```powershell
ctest --preset test-windows-msvc-debug -R '^(snow_image_(animation|png)_tests|snow-shot-(animation-(codec|static-codec|playback)|pinned-animation|clipboard-mime-data|file-pin-batch|pinned-window-repository|pin-to-screen-shortcut-settings)-tests)$'
```

- `snow_image_animation_tests` covers GIF/WebP/APNG composition, frame rectangles, transparency,
  blend/disposal, interlacing, hidden posters, loop metadata, immutable snapshots, truncation and
  input/canvas/frame/metadata/output limits.
- Qt animation codec tests cover encoded source ownership, normalized timing, allocation admission,
  cancellation, decoder reset, palette transparency, hidden posters and EXIF orientation. The
  static codec target exercises the same checks through an in-executable backend.
- PNG recovery tests cover truncated headers, pixel data and endings, partial reads, failing
  sources/sinks, cancellation and metadata errors, followed by valid decoding and encoding.
  A separate C recovery boundary prevents libpng error jumps from crossing live C++ objects;
  RAII retains normal exception cleanup. This also protects APNG's ordinary-PNG frame views.
- Playback tests use an injected clock, deadlines and decode completions. They cover first-paint
  gating, remaining duration on suspension, stepping from the painted frame, wraparound, 10–400%
  speeds, finite completion/restart, stale source/transform cancellation, preparation across future
  cycles, refresh limits, backpressure and immutable publications through the real codecs. Both
  inline and delayed completions verify that a future occurrence survives when the refresh limit
  prevents painting the current same-index occurrence, avoiding recursive retries or decode bursts.
- Clipboard and batch tests retain exact GIF/WebP/APNG bytes, prefer animation over PNG/JPEG/QImage
  thumbnails, reject changed file snapshots, survive deletion after admission and keep one-frame
  GIF/APNG imports static without displaying an APNG poster.
- Repository tests cover exact private sources, legacy animated file restoration and composed
  frame-zero previews, playback defaults/preferences, stable preview revisions, payload demotion,
  state saves without payload rewrites, immutable-owner hash reuse and static source replacement.
- Pinned integration tests cover command ordering/labels, speed bounds, Space input guards,
  retranslation, unsupported OCR/annotation interfaces, dormant annotation history, frozen static
  frame export, suspension and no persistence writes on frame ticks. A gated initial PNG encoder
  verifies rapid static-to-animation replacement and restore at frame zero with paused/playing
  preferences and speed preserved. Shortcut settings tests cover availability and conflict policy.
- Transform preparation tests cover worker-side color normalization, rotation and flipping,
  immutable initial display frames, cancellation, overflow and allocation admission, rapid command
  supersession, delayed paint, geometry rejection and export snapshots during pending preparation.
  Real worker tests apply a scaled transform to successive frames and reject oversized or
  unrepresentable output bounds before Qt transformation while preserving the painted frame.
  Tiny-frame tests include cache-node and raster metadata in the cache budget and verify that
  retirement releases optional cache storage and mandatory reservations.
- An isolated subprocess finishes worker preparations without dispatching their queued results,
  then destroys the recipient and application. It verifies cancellation, result release and
  shutdown without re-entering Qt's event-queue lock through a queued QObject deleter.
- MCP animation fixtures exercise all three formats through creation, rotation, retained-source
  restoration, replacement and static frame export after deleting the admitted source files.

## Native and deployment coverage limits

Windows Debug and Release direct native animation integration (`--animation-only -platform windows`)
passed for GIF, WebP and APNG on 2026-10-11. They checked actual foreground/Qt focus ownership before
keyboard assertions, editable and modal input guards, Hide to Top, minimized geometry, source
replacement and asynchronous transforms. The existing focused Hide to Top checks also passed.
The native fixture emitted a timer-affinity cleanup warning. A debugger stack traced it to the
existing static `ApplicationStorage`/`ConfigurationStore` cleanup timer after fixture application
teardown; no animation decoder, playback service or retirement operation appears in that stack.
The trace is retained in `build/animation-qobject-timer-stack-cdb.log`.
The isolated native transform command (`--animation-transforms-only -platform windows`) passed
twenty consecutive runs across all three formats after the worker-delivery cleanup fix: sixty
fixture executions covering rapid rotations, original/rotated export snapshots, reset geometry
and superseding an unpainted transform. It retains the original geometry assertions and avoids
keyboard/pointer input. An earlier intermittent geometry assertion has not reproduced with the
expanded diagnostics; its cause is unconfirmed. A separate frame-progress assertion was traced
to a prepared frame awaiting native paint; persistence, canvas revision and history were stable.
That invariant test now explicitly renders during its original 360 ms observation interval.
Three complete native Debug runs also passed after the cleanup fix. A fourth failed later at
`SetPhysicalCursorPos`; post-exit inspection found an accessible Default input desktop and full
screen cursor clipping, which cannot establish input availability at the instant of failure.
These interactive input limits are retained rather than suppressed.
Offscreen tests exercise playback and export behavior but do not validate native window exposure,
compositor presentation or platform power management. Native macOS validation is unavailable in
this Windows environment.

Debug static Qt with bundled PNG and the in-executable animation codec, and Release static Qt
with the system PNG codec, have passed their codec test targets. A complete redistributable static
dependency tree is unavailable here, so those results do not establish full packaged-application
deployment coverage. Native macOS and Windows ARM64 validation remain unavailable.

The final production revision and full Debug application build passed all twenty-one selected
Debug tests in 94.67 seconds, including PNG recovery, worker-delivery teardown, the three-format
MCP fixture and translation-catalog validation. The final Release application build passed all
sixteen selected decoder, playback, pinned-window and MCP checks in 43.48 seconds. The direct
native Windows Release animation fixture also passed after the PNG recovery and worker-delivery
cleanup fixes in 8.40 seconds. Only feature-related tests were run.
After the final test-only static-call lint correction, the pinned animation selection passed
again in both Debug and Release, and the focused native transform command passed in both builds.

Affected C++ formatting and `git diff --check` passed. Strict changed-line analysis found no
actionable feature diagnostics. It still fails on three reviewed MSVC `shared_ptr` ownership
diagnostics: two in pinned export artifact ownership and one in the existing runtime-work lease
helper. The returned/captured owners escape and retain ordinary RAII ownership; no analyzer
suppression was added. The final pinned-window analysis log is
`build/animation-tidy-screenshotpinnedwindow-final-replacement-mtd.log`.

## Measured results

All eighteen scenarios passed on 2026-10-11 using `windows-msvc-performance`, Release,
Qt 6.12.0 static/system codecs, MSVC 14.51 and the offscreen platform plugin. The machine ran
Windows 11 Pro 10.0.26300 (build 26300) on an AMD Ryzen 9 5950X, with 12 cores and 24 logical
processors available to this environment. The benchmark completed at 22:14:58 Asia/Shanghai
without concurrent builds. The tested working tree contains the animation implementation on
base commit `0015840fc`; these changes are uncommitted. Raw measurements are retained at
`build/windows-msvc-performance/animation-playback-final-results.json`.

The following active measurements cover approximately 750 ms each, after warm-up and settling.
CPU is total process CPU time, quantized by Windows in 15.625 ms increments; an active result of
zero does not establish zero execution cost. Decode latency includes worker queueing and delivery
back to the GUI. The JSON also retains maximum decode latency including warm-up.

| Scenario | Pins | CPU ms | Decode requests | Paints | Shared wakes | Mean decode ms | GUI lateness p95 / max ms |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| Synthetic 10 fps | 1 | 0 | 7 | 7 | 7 | 0.0011 | 0.343 / 0.714 |
| Synthetic 60 fps | 1 | 0 | 34 | 34 | 36 | 0.0009 | 0.553 / 0.707 |
| Synthetic simultaneous pins | 12 | 15.625 | 390 | 333 | 141 | 0.0003 | 0.713 / 1.198 |
| Small GIF | 1 | 0 | 4 | 4 | 4 | 0.059 | 0.648 / 0.686 |
| Small WebP | 1 | 0 | 4 | 4 | 4 | 0.054 | 0.491 / 0.936 |
| Small APNG | 1 | 0 | 4 | 4 | 4 | 0.063 | 0.662 / 0.868 |
| GIF 960 × 540, 50 fps | 1 | 156.250 | 37 | 37 | 37 | 0.649 | 0.961 / 1.203 |
| GIF 960 × 540, simultaneous pins | 4 | 62.500 | 148 | 148 | 74 | 0.469 | 0.518 / 0.768 |
| GIF 960 × 540, rotated | 1 | 171.875 | 37 | 37 | 37 | 0.741 | 0.634 / 1.009 |
| GIF 960 × 540, 400% speed | 1 | 109.375 | 23 | 22 | 23 | 4.483 | 0.598 / 0.696 |
| GIF 1920 × 1080 | 1 | 250.000 | 10 | 9 | 10 | 25.331 | 0.559 / 0.784 |

The large fixture advances despite decode cost exceeding individual frame durations. Its final
observed preparation lead was 97.225 ms while the GUI remained responsive. Expired presentations
are discarded rather than producing catch-up paint bursts. These measurements do not imply that this
large compressed fixture can sustain every source frame at 50 fps.

All six settled suspension scenarios recorded zero active deadlines, controller callbacks, shared
wakes, decode requests, publications, repaints and measured process CPU. Optional caches and
mandatory worker reservations were also released. The encoded source, last-painted frame and
any retained export snapshots remain available:

| Scenario | Pins | Measured wall ms | Animation callbacks / requests / paints | CPU ms |
| --- | ---: | ---: | --- | ---: |
| Synthetic hidden | 12 | 750.600 | 0 / 0 / 0 | 0 |
| Synthetic thumbnail | 12 | 750.892 | 0 / 0 / 0 | 0 |
| Synthetic paused | 12 | 750.974 | 0 / 0 / 0 | 0 |
| Real GIF hidden | 4 | 751.066 | 0 / 0 / 0 | 0 |
| Real GIF paused | 4 | 750.583 | 0 / 0 / 0 | 0 |
| Real GIF thumbnail | 4 | 750.196 | 0 / 0 / 0 | 0 |

Cache and worker reservation counters are shared process-wide totals. Optional-cache accounting
includes raster and cache-node metadata. Working reservations include conservative decoder and
publication admission. Peak resident memory includes the process and is sampled, so it should
not be interpreted as an exact sum of those two budgets.

| Real-codec scenario | Optional cache MiB | Working reservation MiB | Observed process resident MiB |
| --- | ---: | ---: | ---: |
| Small GIF / WebP / APNG | 0.0014 | 8.00 | 17.54 / 17.75 / 17.93 |
| GIF 960 × 540 | 23.74 | 27.95 | 48.16 |
| GIF 960 × 540, four pins | 23.74 | 55.91 | 56.56 |
| GIF 960 × 540, rotated | 47.47 | 27.95 | 72.48 |
| GIF 960 × 540, 400% speed | 19.78 | 27.95 | 44.90 |
| GIF 1920 × 1080 | 55.38 | 87.80 | 111.11 |

The final reverse-replay scenario completed forty backward steps over forty frames in 285.244 ms,
with forty paints, 31 cache hits, nine misses, seven decoder resets and 73 composited frames.
Process CPU was 468.750 ms across the process's threads. Step latency was 22.458 ms at p95 and
115.522 ms maximum; GUI heartbeat lateness was 0.573 ms at p95 and 0.684 ms maximum.
Retained optional cache was 66,377,728 bytes (63.303 MiB), below the 64 MiB per-source limit;
mandatory working reservation was 28.370 MiB,
and observed resident memory was 95.379 MiB.

The scheduling counters and idle CPU measurements provide evidence of dormant animation work,
not native compositor or battery-energy measurements. Native window visibility and Hide to Top
are validated by the separate Windows integration fixture described above.

## Optimization comparison — 2026-10-12

Pixel-only decoder snapshots now fuse channel conversion into the owning pixel copy. The Qt
bridge reuses the parsed source color space and retains pixel storage without copying complete
frame metadata into each publication. Compatible source/display rasters share storage; Display P3
originals remain separate from their sRGB display conversion. GIF palette expansion writes into
the composition canvas directly, and shared-source caches survive until their last worker retires.
The existing library `Frame` snapshot API still preserves independently owned metadata.

The same `--optimization-only` benchmark sources were built against baseline and optimized
production code using `windows-msvc-performance`, Release, Qt 6.12.0 and the offscreen plugin.
No build or analysis process was active at the start or completion of these paired runs. The
following values are medians of three repetitions, each lasting at least 1200 ms:

| Workload | Before ms | After ms |
| --- | ---: | ---: |
| Fresh GIF decoder import, 1920 × 1080 | 19.217 | 12.088 |
| Fresh GIF decoder import, 3840 × 2160 | 86.476 | 57.591 |
| Snapshot plus ARGB32 premultiplied preparation, 1920 × 1080 | 4.947 | 1.873 |
| Snapshot plus ARGB32 premultiplied preparation, 3840 × 2160 | 19.365 | 8.066 |

Distinct initial source/display raster storage fell exactly 50%: from 16,588,800 to 8,294,400
bytes at 1080p, and from 66,355,200 to 33,177,600 bytes at 4K. These counts describe the admitted
initial rasters, rather than total process memory or decoder working reservations.

Retaining 64 tiny Display P3 snapshots with a 2 MiB XMP payload produced median private-memory
growth of 134,782,976 bytes for WebP and 134,754,304 bytes for APNG before optimization; both
optimized medians were sampled as zero. Approximately 128.5 MiB of excess sampled growth was
eliminated. Pixel snapshots still allocate storage, and allocator reuse/granularity affects this
process-level probe; zero sampled growth does not imply zero allocations or retained memory.
The sibling-retirement case improved from 16 hits/four misses to 19 hits/one miss over twenty
steps, with the survivor's cache preserved. Its remaining miss prepares frame zero, which the
initial publication did not place in the optional cache.

All eight focused checks passed: native animation decoding, dynamic and static Qt
animation codecs, playback, repository animation selection, file-pin batch, clipboard MIME and
pinned animation integration. Scoped clang-tidy passed for ten changed translation units.
The full eighteen-scenario offscreen playback benchmark passed three optimized runs, including
all six settled idle checks. Only animation-related checks were run.

Raw paired data and derived medians are retained in
`build/animation-optimization/{baseline,optimized}/optimization.json` and
`build/animation-optimization/comparison.json`; the three playback runs are in
`build/animation-optimization/optimized/playback-{1,2,3}.json`. Earlier `optimization-loaded.json`
diagnostic runs are excluded from these results. Fresh imports use resident encoded bytes and
measure new decoder/source admission rather than disk I/O. Profiled metadata microbenchmarks
exclude profile-to-sRGB conversion; offscreen measurements exclude native compositor latency.
