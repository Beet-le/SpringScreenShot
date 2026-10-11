# Screenshot backing-store startup benchmark

The screenshot overlay's native window is created before capture, but Qt allocates and commits
its raster pixels lazily. The first visible paint previously paid this cost alongside source
attachment, drawing, and backing-store flush. Releasing the native surface after a capture
makes the cost recur even when the overlay QObject is reused.

After capture dispatch and native/Qt layout validation, the visible-capture workflow now asks
each active overlay to prepare its hidden backing store. A low-priority event allocates the
store and writes one byte per 4 KiB, covering at most 1 MiB of pixel storage per dispatch.
Subsequent slices reacquire the paint device inside a matched `beginPaint`/`endPaint` pair.
There is no screenshot render, window show, backing-store flush, or extra image buffer during
preparation. Capture results outrank preparation events and cancel unfinished work. Geometry,
screen, DPR, native-surface, and presentation changes also invalidate queued work.

This moves allocation and page faults into the capture interval. It does not reduce the final
backing-store size. Allocation alone is insufficient on Cocoa: `beginPaint` creates an IOSurface,
but physical pages still fault during the first visible drawing. Sparse page writes avoid an
extra complete image fill while making that memory resident earlier. Cocoa still clears the
existing alpha surface at first visible paint, so reduced reveal cost can require more total
GUI CPU.

## Run

Use the native Release performance preset. The benchmark requires Cocoa and is intentionally
opt-in rather than a timed CTest test. Do not run builds or other CPU-heavy tests concurrently.

```sh
cmake --preset snow-shot-macos-arm64-performance
cmake --build --preset build-snow-shot-macos-arm64-performance \
  --target snow-shot-backing-store-startup-benchmark --parallel 4
python3 snow_shot/scripts/run-backing-store-startup-perf.py \
  --binary build/snow-shot-macos-arm64-performance/snow_shot/test-bin/snow-shot-backing-store-startup-benchmark \
  --output build/backing-store-startup-controlled
python3 snow_shot/scripts/run-backing-store-startup-perf.py \
  --binary build/snow-shot-macos-arm64-performance/snow_shot/test-bin/snow-shot-backing-store-startup-benchmark \
  --native-capture --native-width 3840 --native-height 2160 \
  --output build/backing-store-startup-native
```

The native dimensions must match the primary display's actual captured pixel dimensions.
Native capture checks existing Screen Recording permission without requesting it and never
saves captured pixels. The controlled run requires no capture permission.

The runner resolves `--binary`, finds the nearest enclosing `CMakeCache.txt`, checks that
build's Release configuration, and pins its configured Cocoa plugin. Both arm64 and x64
performance builds are supported; symlinks use the cache belonging to their resolved binary.
Each output directory contains loader qualification, binary/plugin/source hashes, balanced
process orders, the selected cache path/hash, raw per-process JSON/logs, and `summary.json`.
It verifies that the measured binary, plugin, and source snapshot remain unchanged during a run.

Defaults compare disabled preparation, allocation only, and production page preparation in
three balanced process rounds. Four cold/recreated pairs per process produce 12 samples per
cell. The controlled suite covers 1080p and 4K source pixels with 0, 20, and 100 ms worker
deadlines (432 captures). The native suite acquires the primary display through the production
ScreenCaptureKit FFI and image lease wrapper (72 captures).

## Results: 2026-10-11

Measured on macOS 27.0.1 arm64, Qt 6.12.0, DPR 2, using the installed Cocoa plugin and the
Release performance preset. These are final-run medians for the reused overlay after native
surface destruction, with 12 samples per mode/cell. The disabled mode turns off preparation
in the same current-source binary; it does not substitute archived code or a modified plugin.

| Case / metric | Disabled | Allocation only | Page preparation |
| --- | ---: | ---: | ---: |
| Controlled 4K, 20 ms: first native-dispatch GUI CPU | 2.537 ms | 2.881 ms | 1.225 ms |
| Controlled 4K, 20 ms: ready to first dispatch | 14.865 ms | 15.333 ms | 13.465 ms |
| Controlled 4K, 20 ms: total GUI CPU to first frame | 15.007 ms | 15.697 ms | 15.960 ms |
| Actual ScreenCaptureKit 4K: first native-dispatch GUI CPU | 2.212 ms | 2.530 ms | 1.180 ms |
| Actual ScreenCaptureKit 4K: ready to first dispatch | 12.192 ms | 12.394 ms | 11.127 ms |
| Actual ScreenCaptureKit 4K: total GUI CPU to first frame | 14.017 ms | 14.376 ms | 14.991 ms |
| Actual ScreenCaptureKit 4K: total GUI CPU to moving frame | 14.635 ms | 15.019 ms | 15.582 ms |
| Controlled 4K, 20 ms: delivery to first-frame footprint growth | 31.672 MiB | 31.703 MiB | 0.000 MiB |

Production page preparation reduces the native reveal CPU burst by approximately 47% in the
actual-capture comparison and 52% in the controlled 20 ms case. The corresponding ready-to-frame
median improves by 1.07 ms and 1.40 ms. Total GUI CPU to the first frame increases by roughly
0.97 ms in both cases; this optimization deliberately moves work away from reveal rather than
reducing total CPU or memory consumption. Allocation-only preparation does not deliver this
benefit.

The controlled 4K, 20 ms comparison has a 0.221 ms maximum observed preparation dispatch,
including allocation, over its 12 recreated samples. At zero acquisition lead, no preparation
allocations or page writes occur. Cold process-first captures remain included and can arrive
before preparation starts; benefits depend on available acquisition time. Controlled 1080p,
20 ms reused-overlay native-dispatch CPU also improves from 1.025 ms to 0.536 ms.

All 432 controlled and 72 native captures passed pixel freshness, canvas geometry, hidden
behavior, and preparation cancellation checks. All eight related regression tests passed.
Changed production translation units also compile without benchmark internals, and changed
C++ files pass `clang-format --dry-run --Werror`.

The final measurement files remain under the ignored `build/` directory:

- `build/backing-store-startup-performance-20261011-final-controlled/summary.json`
- `build/backing-store-startup-performance-20261011-final-native/summary.json`
- `build/backing-store-startup-performance-20261011-final-analysis.json`

Binary SHA-256 for both suites:
`cb405cbdf51cbf4de226a4407a098c5006607dab2ba24f9ee4ce28042853c0a0`.

## Interpretation

First-frame dispatch ends after the enclosing native Qt expose/update handler, including
backing-store flush. It is not a compositor presentation fence. Canvas-paint CPU is nested
inside native-dispatch CPU; do not add them. GUI CPU through the first and moving frame also
includes creation, preparation, source attachment, and shared instrumentation overhead.
Source-ready timestamps come from the worker, so delivery delay is included in ready-to-frame
latency. All process-first captures remain in the results.

Memory measurements include Mach physical footprint, resident checkpoints, a 1 ms sampler,
and process peak RSS. Compare deltas calculated within each record: delivery-to-first-frame
shows reveal-time growth; start-to-first-frame and maximum-minus-start show the broader cost.
Sampling can miss transient peaks. Native captures create a fresh session per sample; their
absolute footprint drifts across successive samples, so absolute footprint medians alone
cannot attribute an optimization cost. Controlled sources are allocated before timing and
provide the cleaner backing-store memory comparison.

The native test exercises acquisition contention, but a fresh session provides more preparation
time than an already prepared production capture session. The controlled zero-deadline case
checks the other boundary: ready image delivery cancels queued preparation before allocation.
Single-display Cocoa results do not establish Windows or multi-display performance. Twelve
samples give a coarse p95, not a confidence interval or a reliable CI timing threshold.

## Regression checks

The runner's deterministic script tests cover build-cache selection and Qt-kit qualification
without launching Qt or capturing the screen:

```sh
python3 -B snow_shot/scripts/test-run-backing-store-startup-perf.py
```

The deterministic offscreen tests verify hidden behavior, physical dimensions at DPR 1/2,
bounded page preparation, capture-result priority, cancellation, destruction, and native reuse.
Workflow tests cover validated layout ordering, duplicate/stale callbacks, external drag, and
silent captures. Run only the related tests:

```sh
cmake --build --preset build-snow-shot-macos-arm64-performance \
  --target snow-shot-capture-workflow-tests snow-shot-screenshot-canvas-renderer-tests \
           snow-shot-overlay-frame-presenter-tests --parallel 4
ctest --preset test-snow-shot-macos-arm64-performance \
  -R '^snow-shot-(capture-workflow|overlay-backing-store|overlay-backing-store-dpr2|overlay-pool-prewarm|overlay-frame-presenter|overlay-close-release|background-coverage|overlay-boundary-input)-tests$' \
  --output-on-failure
```
