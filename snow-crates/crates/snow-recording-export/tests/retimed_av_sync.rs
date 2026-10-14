//! Content-level synchronization checks, independent of tempo duration and pitch tests.
use std::f64::consts::TAU;
use std::path::{Path, PathBuf};

use ffmpeg::Rescale;
use ffmpeg_next as ffmpeg;
use snow_core::cancellation::CancellationToken;
use snow_recording_export::clip::{AudioReader, ClipSource};
use snow_recording_export::{
    ExportExecutionMode, ExportFormat, PlaybackSpeed, SoftwareH264Priority, StreamingAudioConfig,
    StreamingEncoder, StreamingEncoderConfig, VideoCodec, write_deferred_output_settings,
};
use snow_recording_model::{
    AudioSampleFormat, AudioTrackManifest, AudioTrackRole, BundleAssetKind, EffectsConfig,
    FinalizedTimeline, InputStoreWriter, IntermediateRecordingProfile, MouseStore, PlaybackOverlay,
    RecordingBundleAsset, RenderConfig, RenderMetadata, SessionManifest, VideoEncodeConfig,
    VideoEncodingSpeed, write_mouse_records, write_recording_bundle, write_render_metadata,
};

const FPS: u32 = 60;
const SAMPLE_RATE: usize = 48_000;
const SOURCE_MS: u64 = 3_000;
const TRIM_START_MS: u64 = 500;
const TRIM_END_MS: u64 = 2_500;
const SELECTED_CENTERS_MS: [u64; 3] = [700, 1_500, 2_300];
const CENTERS_MS: [u64; 5] = [250, 700, 1_500, 2_300, 2_750];
const COLORS: [[u8; 4]; 5] = [
    [220, 220, 30, 255], // Prefix guard: excluded by the selected source boundary.
    [220, 30, 30, 255],
    [30, 220, 30, 255],
    [30, 30, 220, 255],
    [220, 30, 220, 255], // Suffix guard: excluded by the selected source boundary.
];

struct Fixture {
    _directory: tempfile::TempDir,
    finalized: ClipSource,
    deferred: ClipSource,
}

fn config(path: &Path) -> StreamingEncoderConfig {
    StreamingEncoderConfig {
        output_path: path.into(),
        format: ExportFormat::Mp4,
        width: 32,
        height: 24,
        fps: FPS,
        codec: VideoCodec::H264,
        prefer_hardware_h264: false,
        execution_mode: ExportExecutionMode::SoftwareOnly,
        software_h264_priority: SoftwareH264Priority::X264First,
        video: VideoEncodeConfig {
            quality: 100,
            speed: VideoEncodingSpeed::UltraFast,
        },
        encode_threads: 1,
        loop_animated_images: false,
        audio: vec![
            StreamingAudioConfig {
                track_id: "system".into(),
                title: "Synchronization speaker".into(),
                ..Default::default()
            },
            StreamingAudioConfig {
                track_id: "microphone".into(),
                title: "Synchronization microphone".into(),
                default: false,
                ..Default::default()
            },
        ],
    }
}

fn marker_at(time_ms: u64) -> Option<usize> {
    CENTERS_MS
        .iter()
        .position(|center| (*center - 50..*center + 50).contains(&time_ms))
}

fn marker_audio(frequency: f64) -> Vec<i16> {
    let frames = SOURCE_MS as usize * SAMPLE_RATE / 1_000;
    (0..frames)
        .flat_map(|frame| {
            let time_ms = frame as f64 * 1_000.0 / SAMPLE_RATE as f64;
            let sample = CENTERS_MS
                .iter()
                .find_map(|center| {
                    let distance = (time_ms - *center as f64).abs();
                    if distance >= 50.0 {
                        return None;
                    }
                    // Five-ms raised-cosine edges limit AAC ringing while keeping a clear center.
                    let gain = if distance <= 45.0 {
                        1.0
                    } else {
                        (1.0 + ((distance - 45.0) * std::f64::consts::PI / 5.0).cos()) / 2.0
                    };
                    Some(
                        (12_000.0
                            * gain
                            * (TAU * frequency * frame as f64 / SAMPLE_RATE as f64).sin())
                            as i16,
                    )
                })
                .unwrap_or(0);
            [sample, sample]
        })
        .collect()
}

fn encode_source(path: &Path, audio: Option<&[Vec<i16>]>) -> StreamingEncoderConfig {
    let mut config = config(path);
    if audio.is_none() {
        config.audio.clear();
    }
    let mut encoder = StreamingEncoder::create(config.clone()).unwrap();
    let frame_count = SOURCE_MS * u64::from(FPS) / 1_000;
    for frame in 0..frame_count {
        let color = marker_at(frame * 1_000 / u64::from(FPS))
            .map_or([16, 16, 16, 255], |marker| COLORS[marker]);
        encoder
            .push_rgba_frame_at_pts(frame, &color.repeat(32 * 24))
            .unwrap();
        if let Some(audio) = audio {
            let first = frame as usize * SAMPLE_RATE / FPS as usize;
            let end = (frame as usize + 1) * SAMPLE_RATE / FPS as usize;
            for (track, samples) in config.audio.iter().zip(audio) {
                encoder
                    .push_audio_track_pcm_i16_at_frame(
                        &track.track_id,
                        first as u64,
                        &samples[first * 2..end * 2],
                    )
                    .unwrap();
            }
        }
    }
    encoder.finish_at_duration_ms(SOURCE_MS).unwrap();
    config
}

impl Fixture {
    fn new() -> Self {
        let directory = tempfile::tempdir().unwrap();
        let root = directory.path();
        let audio = [marker_audio(440.0), marker_audio(880.0)];
        let finalized_path = root.join("finalized.mp4");
        let finalized_config = encode_source(&finalized_path, Some(&audio));
        let finalized = ClipSource::open(&finalized_path, Some(finalized_config)).unwrap();

        let source_path = root.join("deferred.mp4");
        encode_source(&source_path, None);
        let output_config = config(&root.join("deferred-output.mp4"));
        let inputs = root.join("inputs.bin");
        InputStoreWriter::new(&inputs).unwrap().finish().unwrap();
        let mouse = root.join("mouse.bin");
        write_mouse_records(&mouse, &MouseStore::new()).unwrap();
        let metadata = RenderMetadata {
            render: RenderConfig {
                output_width: 32,
                output_height: 24,
                output_fps: FPS,
                effects: EffectsConfig {
                    show_cursor: false,
                    ..Default::default()
                },
                playback_overlay: PlaybackOverlay::None,
            },
            timeline: FinalizedTimeline::new(SOURCE_MS, FPS).unwrap(),
            coded_width: 32,
            coded_height: 24,
        };
        let render = root.join("render.bin");
        write_render_metadata(&render, &metadata).unwrap();
        let settings = root.join("settings.bin");
        write_deferred_output_settings(&settings, &output_config).unwrap();
        let index = root.join("index.bin");
        let mut bytes = b"SVIDX\0\0".to_vec();
        for frame in 0..SOURCE_MS * u64::from(FPS) / 1_000 {
            let start = frame * 1_000 / u64::from(FPS);
            let end = (frame + 1) * 1_000 / u64::from(FPS);
            bytes.extend(frame.to_le_bytes());
            bytes.extend(start.to_le_bytes());
            bytes.extend(((end - start) as u32).to_le_bytes());
        }
        std::fs::write(&index, bytes).unwrap();
        let mut tracks = Vec::new();
        let mut pcm_paths = Vec::new();
        for ((id, role), samples) in [
            ("system", AudioTrackRole::SystemOutput),
            ("microphone", AudioTrackRole::MicrophoneInput),
        ]
        .into_iter()
        .zip(&audio)
        {
            let path = root.join(format!("{id}.pcm"));
            std::fs::write(
                &path,
                samples
                    .iter()
                    .flat_map(|sample| sample.to_le_bytes())
                    .collect::<Vec<_>>(),
            )
            .unwrap();
            tracks.push(AudioTrackManifest {
                track_id: id.into(),
                role,
                asset_id: format!("{id}.pcm"),
                sample_rate_hz: SAMPLE_RATE as u32,
                channels: 2,
                sample_format: AudioSampleFormat::PcmS16Le,
                duration_frames: SOURCE_MS * SAMPLE_RATE as u64 / 1_000,
                recorded: true,
            });
            pcm_paths.push(path);
        }
        let manifest = SessionManifest {
            media: snow_recording_model::media::RecordedMedia::new(
                snow_media::ColorDescription::SRGB,
                snow_media::CursorMode::Separate,
                Vec::new(),
            ),
            video_codec: VideoCodec::H264,
            session_id: "retimed-av-sync".into(),
            output_dir: root.into(),
            keep_temp_files: false,
            fps: FPS,
            intermediate_profile: IntermediateRecordingProfile::EditFast,
            recording_video: output_config.video,
            width: 32,
            height: 24,
            capture_origin_x: 0,
            capture_origin_y: 0,
            audio_tracks: tracks.clone(),
            pause_intervals: Vec::new(),
        };
        let mut assets = vec![
            RecordingBundleAsset {
                kind: BundleAssetKind::VideoIndex,
                asset_id: None,
                path: &index,
            },
            RecordingBundleAsset {
                kind: BundleAssetKind::MouseStore,
                asset_id: None,
                path: &mouse,
            },
            RecordingBundleAsset {
                kind: BundleAssetKind::InputEvents,
                asset_id: None,
                path: &inputs,
            },
            RecordingBundleAsset {
                kind: BundleAssetKind::RenderMetadata,
                asset_id: None,
                path: &render,
            },
            RecordingBundleAsset {
                kind: BundleAssetKind::OutputSettings,
                asset_id: None,
                path: &settings,
            },
        ];
        for (track, path) in tracks.iter().zip(&pcm_paths) {
            assets.push(RecordingBundleAsset {
                kind: BundleAssetKind::AudioTrack,
                asset_id: Some(&track.asset_id),
                path,
            });
        }
        write_recording_bundle(&source_path, &manifest, &assets).unwrap();
        let bundle_path = root.join("deferred.snowrec");
        std::fs::rename(source_path, &bundle_path).unwrap();
        let deferred = ClipSource::open(&bundle_path, None).unwrap();
        Self {
            _directory: directory,
            finalized,
            deferred,
        }
    }
}

/// Decode encoded video PTS independently of the clip's source-to-output mapping.
fn video_markers(path: &Path) -> (u64, Vec<(usize, f64)>) {
    let mut input = ffmpeg::format::input(path).unwrap();
    let stream = input.streams().best(ffmpeg::media::Type::Video).unwrap();
    let index = stream.index();
    let time_base = stream.time_base();
    let duration_us = stream.duration().rescale(time_base, (1, 1_000_000)) as u64;
    let mut decoder = ffmpeg::codec::context::Context::from_parameters(stream.parameters())
        .unwrap()
        .decoder()
        .video()
        .unwrap();
    let mut conversion = ffmpeg::software::scaling::Context::get(
        decoder.format(),
        decoder.width(),
        decoder.height(),
        ffmpeg::format::Pixel::RGBA,
        decoder.width(),
        decoder.height(),
        ffmpeg::software::scaling::Flags::BICUBIC,
    )
    .unwrap();
    let mut frames = Vec::new();
    let mut decoded = ffmpeg::frame::Video::empty();
    let mut rgba = ffmpeg::frame::Video::empty();
    let mut drain = |decoder: &mut ffmpeg::decoder::Video| {
        while decoder.receive_frame(&mut decoded).is_ok() {
            conversion.run(&decoded, &mut rgba).unwrap();
            let offset =
                rgba.height() as usize / 2 * rgba.stride(0) + rgba.width() as usize / 2 * 4;
            let rgb = &rgba.data(0)[offset..offset + 3];
            let marker = COLORS.iter().position(|color| {
                rgb.iter().zip(color).all(|(actual, expected)| {
                    (i16::from(*actual) - i16::from(*expected)).abs() < 45
                })
            });
            let pts = decoded
                .timestamp()
                .expect("encoded marker frame has a timestamp")
                .rescale(time_base, (1, 1_000_000));
            frames.push((pts.max(0) as u64, marker));
        }
    };
    for (stream, packet) in input.packets() {
        if stream.index() == index {
            decoder.send_packet(&packet).unwrap();
            drain(&mut decoder);
        }
    }
    decoder.send_eof().unwrap();
    drain(&mut decoder);
    let mut markers = Vec::new();
    let mut frame = 0;
    while frame < frames.len() {
        let (start, marker) = frames[frame];
        let mut end = frame + 1;
        while end < frames.len() && frames[end].1 == marker {
            end += 1;
        }
        if let Some(marker) = marker {
            let stop = frames.get(end).map_or(duration_us, |frame| frame.0);
            markers.push((marker, (start + stop) as f64 / 2.0 / 1_000_000.0));
        }
        frame = end;
    }
    (duration_us, markers)
}

fn read_audio(source: &ClipSource) -> Vec<Vec<i16>> {
    let token = CancellationToken::default();
    let mut reader = AudioReader::open(source).unwrap();
    let mut samples = vec![Vec::new(); source.config.audio.len()];
    let total = source.duration_us * SAMPLE_RATE as u64 / 1_000_000;
    let mut first = 0;
    while first < total {
        let count = (total - first).min(960) as usize;
        for (destination, block) in samples
            .iter_mut()
            .zip(reader.read(first, count, &token).unwrap())
        {
            destination.extend_from_slice(block);
        }
        first += count as u64;
    }
    samples
}

/// Locate burst energy rather than comparing phase-sensitive, time-stretched PCM.
fn audio_markers(samples: &[i16], percent: u32) -> Vec<f64> {
    const BIN_FRAMES: usize = SAMPLE_RATE / 200; // Five ms, below one AAC frame.
    let gap_bins = (50_u32 * 100).div_ceil(percent * 5).max(3) as usize;
    let energy: Vec<_> = samples
        .chunks(BIN_FRAMES * 2)
        .map(|bin| {
            bin.iter()
                .step_by(2)
                .map(|sample| f64::from(*sample).powi(2))
                .sum::<f64>()
                / (bin.len() / 2) as f64
        })
        .collect();
    let audible: Vec<_> = energy
        .iter()
        .enumerate()
        .filter(|(_, energy)| **energy > 1_000.0_f64.powi(2))
        .map(|(bin, _)| bin)
        .collect();
    let mut centers = Vec::new();
    let mut first = 0;
    while first < audible.len() {
        let mut end = first + 1;
        // A slow WSOLA chain may split a burst within its 50-ms source analysis window.
        // Bridge that bounded window, which is much shorter than the 700-ms source gaps.
        while end < audible.len() && audible[end] - audible[end - 1] <= gap_bins + 1 {
            end += 1;
        }
        let bins = &audible[first..end];
        let total = bins.iter().map(|bin| energy[*bin]).sum::<f64>();
        let center = bins
            .iter()
            .map(|bin| (*bin as f64 + 0.5) * energy[*bin])
            .sum::<f64>()
            / total
            * BIN_FRAMES as f64
            / SAMPLE_RATE as f64;
        centers.push(center);
        first = end;
    }
    centers
}

fn tone_power(samples: &[i16], center: f64, frequency: f64) -> f64 {
    // A short central window fits even the 33-ms burst at 3.0x.
    let center = (center * SAMPLE_RATE as f64) as usize;
    let start = center.saturating_sub(400);
    let end = (center + 400).min(samples.len() / 2);
    let mut real = 0.0;
    let mut imaginary = 0.0;
    for frame in start..end {
        let phase = TAU * frequency * frame as f64 / SAMPLE_RATE as f64;
        let weight = (1.0 - (TAU * (frame - start) as f64 / (end - start) as f64).cos()) / 2.0;
        let sample = f64::from(samples[frame * 2]) * weight;
        real += sample * phase.cos();
        imaginary += sample * phase.sin();
    }
    real * real + imaginary * imaginary
}

#[test]
fn every_speed_keeps_selected_audio_and_video_markers_in_sync() {
    let fixture = Fixture::new();
    // Confirm that both guards really exist in the input instead of testing an empty cut.
    let (_, source_video) = video_markers(&fixture.finalized.path);
    assert_eq!(
        source_video
            .iter()
            .map(|marker| marker.0)
            .collect::<Vec<_>>(),
        [0, 1, 2, 3, 4]
    );
    for source in [&fixture.finalized, &fixture.deferred] {
        for audio in read_audio(source) {
            assert_eq!(audio_markers(&audio, 100).len(), 5);
        }
    }

    for (kind, source) in [
        ("finalized", &fixture.finalized),
        ("deferred", &fixture.deferred),
    ] {
        let range = source
            .range(
                TRIM_START_MS * u64::from(FPS) / 1_000,
                TRIM_END_MS * u64::from(FPS) / 1_000,
            )
            .unwrap();
        assert_eq!((range.start_us, range.end_us), (500_000, 2_500_000));
        for percent in [25, 50, 75, 100, 125, 150, 175, 200, 300] {
            let speed = PlaybackSpeed::from_percent(percent).unwrap();
            let destination: PathBuf = source
                .path
                .parent()
                .unwrap()
                .join(format!("{kind}-{percent}.mp4"));
            source
                .export_with_speed(
                    range,
                    speed,
                    &destination,
                    &CancellationToken::default(),
                    |_| {},
                )
                .unwrap();
            let output = ClipSource::open(&destination, Some(source.config.clone())).unwrap();
            let expected_duration = speed.output_duration_us(range.duration_us()).unwrap();
            let (video_duration, video) = video_markers(&destination);
            assert!(
                video_duration.abs_diff(expected_duration) <= 2_000,
                "{kind} {percent}%: video endpoint {video_duration} != {expected_duration}"
            );
            assert_eq!(
                video.iter().map(|marker| marker.0).collect::<Vec<_>>(),
                [1, 2, 3],
                "{kind} {percent}%: outside-trim video guard leaked, or a selected flash was lost: {video:?}"
            );

            let expected: Vec<_> = SELECTED_CENTERS_MS
                .iter()
                .map(|center| {
                    (center - TRIM_START_MS) as f64 / 1_000.0 * 100.0 / f64::from(percent)
                })
                .collect();
            // CFR sampling can move either flash edge by one 60-Hz output frame.
            let video_tolerance = 1.0 / f64::from(FPS) + 0.002;
            for ((_, actual), expected) in video.iter().zip(&expected) {
                assert!(
                    (actual - expected).abs() <= video_tolerance,
                    "{kind} {percent}%: video marker {actual:.6}s != {expected:.6}s"
                );
            }

            let tracks = read_audio(&output);
            assert_eq!(tracks.len(), 2, "{kind} {percent}%: separate audio routes");
            // Allow 50 ms in the source clock for bounded WSOLA analysis and one 1024-sample
            // AAC frame plus the 5-ms envelope bin in the output clock. This does not allow
            // a wrong source boundary, a missing guard cut, or accumulating marker drift.
            let audio_tolerance =
                0.050 * 100.0 / f64::from(percent) + 1024.0 / SAMPLE_RATE as f64 + 0.005;
            for (track, samples) in tracks.iter().enumerate() {
                let audio = audio_markers(samples, percent);
                assert_eq!(
                    audio.len(),
                    3,
                    "{kind} {percent}% track {track}: outside-trim audio guard leaked, or a selected burst was lost: {audio:?}"
                );
                for (marker, ((actual, expected), (_, video))) in
                    audio.iter().zip(&expected).zip(&video).enumerate()
                {
                    assert!(
                        (actual - expected).abs() <= audio_tolerance,
                        "{kind} {percent}% track {track} marker {marker}: audio {actual:.6}s != expected {expected:.6}s, tolerance {audio_tolerance:.6}s"
                    );
                    assert!(
                        (actual - video).abs() <= audio_tolerance + video_tolerance,
                        "{kind} {percent}% track {track} marker {marker}: audio {actual:.6}s != video {video:.6}s"
                    );
                    let frequency = if track == 0 { 440.0 } else { 880.0 };
                    let other = if track == 0 { 880.0 } else { 440.0 };
                    assert!(
                        tone_power(samples, *actual, frequency)
                            > tone_power(samples, *actual, other) * 10.0,
                        "{kind} {percent}% track {track} marker {marker}: pitch or route identity changed"
                    );
                }
                let measured_span = audio[2] - audio[0];
                let expected_span = expected[2] - expected[0];
                assert!(
                    (measured_span - expected_span).abs() <= audio_tolerance,
                    "{kind} {percent}% track {track}: first-to-last drift {measured_span:.6}s != {expected_span:.6}s"
                );
            }
        }
    }
}
