# Recording webcam validation

## Behavior and ownership

The toolbar's camera button follows **Show keystrokes in recording**. Fresh and
reset settings disable the camera; its style defaults to a 150 px square. Effect
Settings accepts integer sizes from 50 to 500 px and square/circle shapes.

`snow-webcam` owns one worker and native camera session shared by preview and
recording. The worker admits at most 30 fps (limited by recording fps), center
crops and scales admitted frames, and retains at most four immutable images.
Native callback queues hold one sample. Pause clears queued images. A failure
clears the overlay and stays latched until explicit re-enable or a new session.
Camera errors never become screen-recording errors. Stop retires the camera
before encoder draining or deferred-bundle packing, even while preview and
recording configurations still retain the shared source.

Preview uses physical display pixels; output uses video pixels. All renderers
use the same placement and antialiased mask implementation. The inset starts
at 48 px, shrinks before the image does, and never modifies the saved size.
The camera sits below cursor, keyboard, mouse, and playback effects. GPU capture
does not read the desktop back to the CPU; it uploads a compact camera texture
only when the camera image changes.

Direct C configuration v13 and preview v8 retain the camera source; older
layouts remain accepted with the camera disabled. Deferred bundles optionally
contain `WebcamVideo`, `WebcamIndex`, and versioned `WebcamMetadata` assets. Their
timestamps and availability transitions use active recording time. Playback,
trim, speed changes, and export reuse the same camera replay and renderer.

## Automated checks

Run only these affected targets, from the repository build environment:

```powershell
cmake --build --preset build-windows-msvc-debug --target snow-shot-screenshot-tool-palette-tests snow-shot-screen-recording-controller-tests snow-shot-screen-recording-interaction-settings-tests
ctest --preset test-windows-msvc-debug -R '^snow-shot-(screen-recording-toolbar|screen-recording-interaction-settings|screen-recording-controller|mcp-recording)-tests$' --output-on-failure
ctest --preset test-windows-msvc-debug -R '^snow-shot-recording-webcam-(preview|settings)-tests$' --output-on-failure
cmake --build --preset build-windows-msvc-debug --target snow_shot_update_translations
```

From `snow-crates`, with repository FFmpeg and libclang configured:

```powershell
cargo test -p snow-webcam --lib
cargo test -p snow-recording-model -p snow-recording-effects -p snow-recording-effects-c -p snow-recording-c --lib
cargo test -p snow-recording-runtime --no-default-features --lib direct::tests::
cargo test -p snow-recording-runtime --no-default-features --lib webcam::tests::
cargo test -p snow-recording-runtime --no-default-features --lib hardware_sparse_effects_and_webcam -- --ignored
cargo test -p snow-recording-export --lib deferred::tests::
```

The GPU test requires a D3D11 VideoProcessor and H.264 encoder, but renders
offscreen. Other new camera tests use synthetic sources. They cover crop and
orientation, positive/negative native pitch, padded rows, bounded ownership,
pause/fresh resume, late callbacks, failure latching, physical-pixel preview,
circle edges, small regions, CPU/GPU layer order and clearing, direct output
after disconnect, deferred output in all four formats, backward preview seeks,
trim/speed changes, retries, old configurations and old bundles.

The controller tests replace camera FFI calls with an inactive fake source. They
cover saved enablement on first open, live preview style changes, disable/re-enable,
close/reopen, and source teardown without accessing camera hardware. Preference
changes remain deferred through queued startup, countdown, native initialization,
recording, pause, and finalization, then reach idle preview and the next recording.
Interrupted startup and automation-to-manual transitions preserve the same rules.

From `snow_shot/rust/snow-shot-mcp`, run `cargo test recording_` for both-edition
manifest consistency and webcam option validation. New UI text is translated
in the annotation and recording catalogs for en_US, zh_CN, and zh_TW.
The two `MacOSBundleMetadata` product-metadata tests in
`scripts/test-macos-build-support.py` verify the camera usage key and all six
localized permission resources without requiring macOS.

## Windows Release performance results

Measured on 2026-10-19 using `windows-msvc-performance`, an AMD Radeon RX 6750 XT,
1280 x 720 output at 30 fps, and a synthetic moving 640 x 480 camera. The baseline
is commit `622fb1497`, built with the same Release profile and benchmark features.
The first matrix uses three 4-second samples per case after a warmup; disabled
comparisons were repeated in reverse order with four 8-second samples per case.
Raw CSVs and recordings remain under `build/webcam-performance/` and
`build/webcam-performance-repeat/` on the validation machine.

Median composition time in milliseconds (first matrix):

| Path | Screen | Size | Square | Circle |
| --- | --- | ---: | ---: | ---: |
| CPU | Static | 150 | 3.410 | 3.237 |
| CPU | Static | 500 | 3.784 | 3.834 |
| CPU | Moving | 150 | 3.563 | 3.615 |
| CPU | Moving | 500 | 4.137 | 4.327 |
| GPU | Static | 150 | 2.593 | 2.595 |
| GPU | Static | 500 | 3.304 | 3.080 |
| GPU | Moving | 150 | 3.056 | 2.949 |
| GPU | Moving | 500 | 3.675 | 3.552 |

All 72 first-matrix samples had zero missed output slots. Camera-enabled samples
encoded 29.75–30 fps, including camera-only motion over a static screen; startup
and timestamp coalescing account for the occasional one-frame difference.
The largest median p95 composition time was 5.623 ms. GPU samples had zero
desktop readbacks, zero host transfers, and zero hardware fallback. Camera
uploads were 2.7 MB/s at 150 px and 30 MB/s at 500 px, independent of screen size.
Screen-capture queue drops were 0–2 in 47 of the 48 camera-enabled samples; one
500 px square GPU sample reported 12 drops while retaining 30 fps and zero
missed output slots. These source-queue metrics are separate from output cadence.

Disabled-feature median composition changes, relative to the baseline:

| Path / screen | First comparison | Reverse-order repeat |
| --- | ---: | ---: |
| CPU / static | -5.36% | -1.68% |
| CPU / moving | +2.45% | -3.80% |
| GPU / static | +1.91% | +3.19% |
| GPU / moving | -2.04% | -9.41% |

No repeatable disabled-feature composition regression exceeded 5%. Process CPU
measurements varied substantially between samples, and apparent CPU increases
in the first comparison did not repeat. The benchmark's CPU percentage measures
one logical core, not total machine capacity. One disabled GPU repeat missed
two output slots; this did not occur in the original matrix. Unchanged screen
frames are intentionally held, so disabled-feature encoded-frame counts are not
a measure of output timeline duration.

The 30-second 500 px circle runs produced 29.967 fps on CPU and 30.000 fps on
GPU, with zero missed output slots. Median private memory at 5–10 seconds versus
25–30 seconds was 202.47 → 202.34 MiB (CPU) and 258.15 → 259.14 MiB (GPU), with
no progressive growth. Whole-process peak private memory was 234.35 / 422.09 MiB;
these peaks include encoder/driver startup and benchmark instrumentation.
CPU consumption was 79.72% / 35.99% of one logical core. The GPU run retained
zero desktop readbacks and 30 MB/s camera upload. Native capture has a bounded
one-sample queue; the shared camera service retains at most four resized frames.

Reproduce an enabled case with the performance benchmark target:

```powershell
cmake --build --preset build-windows-msvc-performance --target snow-shot-realtime-recording-performance-benchmark
& build/windows-msvc-performance/cargo/x86_64-pc-windows-msvc/release/examples/realtime_recording_benchmark.exe --duration-seconds 30 --warmup-seconds 1 --samples 1 --fps 30 --clarity 1080p --region-size 1280x720 --scenario no-effects --workload continuous --webcam-size 500 --webcam-shape circle --prefer-hardware --output build/webcam-performance-check
```

Omit `--prefer-hardware` for CPU encoding, omit webcam options for the disabled
case, and vary size, shape, and `--workload static` for the full matrix.

## Native checks still required before publication

On 2026-10-19, the Windows x64 test host reported no camera device
(`0x80070490`). The native smoke example verified unavailable-device handling,
but live camera operation, permission denial/recovery, busy-device handling,
and physical disconnect/reconnect still require a camera-equipped host.

```powershell
# Opens the default camera briefly; saves no images or audio.
cargo run -p snow-webcam --example smoke
```

On macOS, the active stream owns the device configuration lock until the native
session has stopped and its callback queue has drained. This prevents macOS from
overriding the chosen format during startup or while another app shares the
camera. Format selection prefers the requested cadence, then the smallest source
that covers the square crop; undersized sources prefer the largest crop. When an
exact rate is unavailable, selection uses the nearest supported rate at or below
the request when possible, and uses the device's exact boundary frame duration
for fixed rates such as 30000/1001 fps. The shared service still caps admitted fps.
Native BGRA frames copy directly into packed RGBA without intermediate frame copies.

The macOS compositor retains one immutable sRGB camera image by generation and
pixel layout. Unchanged output frames reuse that native source; movement changes
only its placement. New frames, shape/size changes and reappearance replace it.
Disappearance releases it even when desktop composition is bypassed or the output
pool is full. Foreground effects and HDR color conversion retain the same order.

Focused native checks run on ARM64 macOS 27.0.1, with repository FFmpeg configured:

```sh
# From snow-crates; all tests below run without camera hardware.
cargo test -p snow-macos --lib webcam::
cargo test -p snow-macos --lib compositor::tests::
cargo test -p snow-recording-runtime --no-default-features --lib webcam
cargo test -p snow-recording-runtime --no-default-features --lib camera_and_highlight
cargo clippy -p snow-macos -p snow-webcam -p snow-recording-runtime --no-default-features --all-targets -- -D warnings
```

These cover lock lifetime and startup-failure cleanup, native format selection,
channel order/row orientation, native image reuse/replacement/retirement, padded
rows, clipped and translucent overlays, HDR agreement, and cursor layering.
ARM64 Full and Mini Debug app builds, Intel macOS all-target checks for
`snow-macos`, formatting and Clippy pass. The focused verification passed 26 Rust
tests, five UI tests and ten bundle-metadata tests. Hardware acceptance still
requires a camera-equipped host; none was enumerated on this Mac.
Physical Intel/macOS 15 and Windows ARM64 verification remain required. Test both
editions' localized camera permission descriptions in a packaged app, then
preview, record, pause/resume, cancel, and repeatedly close/reopen recording.
Verify the camera indicator retires, no camera microphone track is created,
and screen recording continues after denied access or camera removal.
Use a manifested application for first-time macOS permission testing: the bare
`cargo run` smoke executable has no camera usage description and deliberately
does not request access without one. The Full and Mini bundles include that key
and their localized permission resources.

Native capture follows the platform APIs documented by
[Microsoft](https://learn.microsoft.com/en-us/windows/win32/medfound/audio-video-capture-in-media-foundation)
and [Apple](https://developer.apple.com/documentation/avfoundation/avcapturevideodataoutput).
