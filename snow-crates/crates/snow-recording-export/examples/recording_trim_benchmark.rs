//! Run only from the windows-msvc-performance preset (Release).
use anyhow::{Result, ensure};
use ffmpeg::Rescale;
use ffmpeg_next as ffmpeg;
use snow_core::cancellation::CancellationToken;
use snow_recording_export::{
    ExportExecutionMode, ExportFormat, SoftwareH264Priority, StreamingEncoder,
    StreamingEncoderConfig,
    clip::{ClipSource, VideoReader},
    playback::Playback,
};
use snow_recording_model::{
    PlaybackSpeed, TrimRange, VideoCodec, VideoEncodeConfig, VideoEncodingSpeed,
};
use std::path::Path;
use std::time::{Duration, Instant};

#[cfg(windows)]
fn memory_mib() -> f64 {
    use windows::Win32::System::{ProcessStatus::*, Threading::GetCurrentProcess};
    let mut counters = PROCESS_MEMORY_COUNTERS_EX::default();
    unsafe {
        let _ = K32GetProcessMemoryInfo(
            GetCurrentProcess(),
            (&mut counters as *mut PROCESS_MEMORY_COUNTERS_EX).cast(),
            std::mem::size_of_val(&counters) as u32,
        );
    }
    counters.PrivateUsage as f64 / (1024.0 * 1024.0)
}

fn measured_export(
    source: &ClipSource,
    range: TrimRange,
    speed: PlaybackSpeed,
    output: &Path,
    token: &CancellationToken,
) -> Result<(f64, f64, u64, u64)> {
    if output.exists() {
        std::fs::remove_file(output)?;
    }
    // Include encoder/tempo peaks, which progress-only sampling misses during finalization.
    let trace_memory = std::env::var_os("SNOW_TRIM_MEMORY_TRACE").is_some();
    let before = memory_mib();
    let (stop, stopped) = std::sync::mpsc::channel();
    let sampler = std::thread::spawn(move || {
        let started = Instant::now();
        let mut samples = Vec::new();
        let mut peak = memory_mib();
        let mut previous_sample = Duration::ZERO;
        while matches!(
            stopped.recv_timeout(Duration::from_millis(10)),
            Err(std::sync::mpsc::RecvTimeoutError::Timeout)
        ) {
            let memory = memory_mib();
            peak = peak.max(memory);
            if trace_memory
                && started.elapsed().saturating_sub(previous_sample) >= Duration::from_millis(100)
            {
                previous_sample = started.elapsed();
                samples.push((previous_sample.as_millis(), memory));
            }
        }
        (peak.max(memory_mib()), samples)
    });
    let start = Instant::now();
    let result = source.export_with_speed(range, speed, output, token, |_| {});
    let elapsed_ms = start.elapsed().as_secs_f64() * 1000.0;
    let _ = stop.send(());
    let (peak, samples) = sampler.join().expect("memory sampler");
    if trace_memory {
        eprintln!(
            "memory_export,{},{before:.2},{peak:.2},{:.2}",
            output.display(),
            memory_mib()
        );
        for (elapsed_ms, private_mib) in samples {
            eprintln!(
                "memory_sample,{},{elapsed_ms},{private_mib:.2}",
                output.display()
            );
        }
    }
    result?;
    let exported = ClipSource::open(output, Some(source.config.clone()))?;
    let timing = source.output_timing(range, speed)?;
    let expected_us = timing.duration_us().map_err(anyhow::Error::msg)?;
    ensure!(
        exported.duration_us.abs_diff(expected_us) <= 2000,
        "retimed output duration disagrees: {} vs {expected_us}",
        exported.duration_us
    );
    // Inspect encoded packet cadence independently of the cloned editor settings.
    let mut input = ffmpeg::format::input(output)?;
    let stream = input
        .streams()
        .best(ffmpeg::media::Type::Video)
        .ok_or_else(|| anyhow::anyhow!("export has no video stream"))?;
    let id = stream.index();
    let time_base = stream.time_base();
    let mut timestamps: Vec<_> = input
        .packets()
        .filter(|(stream, _)| stream.index() == id)
        .map(|(_, packet)| {
            packet
                .pts()
                .map(|pts| pts.rescale(time_base, (1, 1_000_000)))
        })
        .collect::<Option<_>>()
        .ok_or_else(|| anyhow::anyhow!("exported video packet has no presentation timestamp"))?;
    timestamps.sort_unstable();
    let frames = timing.frame_count().map_err(anyhow::Error::msg)?;
    ensure!(
        timestamps.len() as u64 == frames,
        "retiming omitted fixed-rate presentation slots"
    );
    for (index, timestamp) in timestamps.into_iter().enumerate() {
        let expected = (index as u128 * 1_000_000 / u128::from(source.config.fps)) as i64;
        ensure!(
            timestamp.abs_diff(expected) <= 1,
            "retiming changed the presentation grid"
        );
    }
    ensure!(
        exported.config.audio.len() == source.config.audio.len(),
        "retiming changed audio routes"
    );
    Ok((elapsed_ms, peak, exported.duration_us, frames))
}
#[cfg(not(windows))]
fn memory_mib() -> f64 {
    0.0
}

fn scenario(
    directory: &Path,
    name: &str,
    size: (u32, u32),
    seconds: u64,
    stride: u64,
    audio: bool,
    rates: &[u32],
) -> Result<()> {
    let config = StreamingEncoderConfig {
        output_path: directory.join(format!("{name}.mp4")),
        format: ExportFormat::Mp4,
        width: size.0,
        height: size.1,
        fps: 60,
        codec: VideoCodec::H264,
        prefer_hardware_h264: false,
        execution_mode: ExportExecutionMode::SoftwareOnly,
        software_h264_priority: SoftwareH264Priority::X264First,
        video: VideoEncodeConfig {
            quality: 80,
            speed: VideoEncodingSpeed::UltraFast,
        },
        encode_threads: 4,
        audio: if audio {
            ["System", "Microphone"]
                .into_iter()
                .enumerate()
                .map(
                    |(index, title)| snow_recording_export::StreamingAudioConfig {
                        track_id: title.into(),
                        title: title.into(),
                        default: index == 0,
                        ..Default::default()
                    },
                )
                .collect()
        } else {
            Vec::new()
        },
        loop_animated_images: false,
    };
    let mut pixels = vec![0; size.0 as usize * size.1 as usize * 4];
    if !config.output_path.exists() {
        let mut encoder = StreamingEncoder::create(config.clone())?;
        for frame in (0..seconds * 60).step_by(stride as usize) {
            for (index, pixel) in pixels.chunks_exact_mut(4).enumerate() {
                let x = index as u32 % size.0;
                let y = index as u32 / size.0;
                pixel.copy_from_slice(&[
                    40 + ((x / 16 + frame as u32) % 160) as u8,
                    40 + ((y / 16) % 160) as u8,
                    100,
                    255,
                ]);
            }
            encoder.push_rgba_frame_at_pts(frame, &pixels)?;
        }
        for start in (0..seconds * 48_000).step_by(960) {
            for (track, hz) in config.audio.iter().zip([440.0, 880.0]) {
                let samples: Vec<i16> = (start..(start + 960).min(seconds * 48_000))
                    .flat_map(|sample| {
                        let value = ((sample as f64 * std::f64::consts::TAU * hz / 48_000.0).sin()
                            * 6000.0) as i16;
                        [value, value]
                    })
                    .collect();
                encoder.push_audio_track_pcm_i16_at_frame(&track.track_id, start, &samples)?;
            }
        }
        encoder.finish_at_duration_ms(seconds * 1000)?;
    }
    let base = memory_mib();
    if std::env::var_os("SNOW_TRIM_MEMORY_TRACE").is_some() {
        eprintln!("memory_scenario,{name},baseline,{base:.2}");
    }
    let start = Instant::now();
    let source = ClipSource::open(&config.output_path, Some(config.clone()))?;
    let open_ms = start.elapsed().as_secs_f64() * 1000.0;
    let token = CancellationToken::default();
    let at = source.boundary((seconds * 60).saturating_sub(90));
    let mut software = VideoReader::open(source.clone(), false)?;
    software.read(at, &token)?;
    let mut reference = Vec::new();
    software.copy_rgba(&mut reference)?;
    drop(software);
    let start = Instant::now();
    let mut video = VideoReader::open(source.clone(), true)?;
    video.read(at, &token)?;
    video.copy_rgba(&mut pixels)?;
    let first_ms = start.elapsed().as_secs_f64() * 1000.0;
    let error = reference
        .iter()
        .zip(&pixels)
        .map(|(a, b)| f64::from(a.abs_diff(*b)))
        .sum::<f64>()
        / pixels.len() as f64;
    ensure!(
        error < 5.0,
        "hardware fallback changes pixels: mean error {error}"
    );
    let mut seeks = Vec::new();
    let mut peak = memory_mib();
    for index in [0, 3, 1, 4, 2, 5, 3, 0, 4, 5] {
        let start = Instant::now();
        video.read(source.boundary(source.frame_count() * index / 6), &token)?;
        video.copy_rgba(&mut pixels)?;
        seeks.push(start.elapsed().as_secs_f64() * 1000.0);
        peak = peak.max(memory_mib());
    }
    seeks.sort_by(f64::total_cmp);
    drop(video);
    // These independent pixel references validate decoding only. Retaining both
    // across export would add two full RGBA canvases to the measured encoder peak.
    drop(reference);
    drop(pixels);
    if std::env::var_os("SNOW_TRIM_MEMORY_TRACE").is_some() {
        eprintln!("memory_scenario,{name},after_seeks,{:.2}", memory_mib());
    }
    let playback = Playback::open(source.clone())?;
    let revision = playback.seek(at, (at + 1_000_000).min(source.duration_us), true);
    let start = Instant::now();
    let mut polls = 0;
    loop {
        let snapshot = playback.snapshot();
        ensure!(snapshot.error.is_empty(), "{}", snapshot.error);
        peak = peak.max(memory_mib());
        polls += 1;
        if snapshot.revision == revision && !snapshot.playing {
            break;
        }
        ensure!(
            start.elapsed() < Duration::from_secs(15),
            "playback stalled"
        );
        std::thread::sleep(Duration::from_millis(5));
    }
    let playback_ms = start.elapsed().as_secs_f64() * 1000.0;
    drop(playback);
    if std::env::var_os("SNOW_TRIM_MEMORY_TRACE").is_some() {
        eprintln!("memory_scenario,{name},after_playback,{:.2}", memory_mib());
    }
    let range = source.range(source.frame_at(at), source.frame_at(at) + 60)?;
    for &percent in rates {
        let speed = PlaybackSpeed::from_percent(percent).map_err(anyhow::Error::msg)?;
        let label = if percent == 100 {
            name.to_string()
        } else {
            format!("{name}-speed-{percent}")
        };
        let output = directory.join(format!("{label}-trim.mp4"));
        let (export_ms, export_peak, duration_us, output_frames) =
            measured_export(&source, range, speed, &output, &token)?;
        peak = peak.max(export_peak);
        ensure!(
            peak - base < 768.0,
            "unbounded export buffering: {} MiB",
            peak - base
        );
        println!(
            "{label},{open_ms:.2},{first_ms:.2},{:.2},{:.2},{playback_ms:.2},{polls},{export_ms:.2},{:.2},{error:.3},{percent},{},{duration_us},{output_frames},{}",
            seeks[5],
            seeks[9],
            peak - base,
            range.duration_us(),
            config.fps,
        );
    }
    if seconds >= 1800 {
        let range = source.range(source.frame_count() - 15 * 60, source.frame_count())?;
        let output = directory.join(format!("{name}-long-slow-trim.mp4"));
        let (export_ms, export_peak, duration_us, output_frames) = measured_export(
            &source,
            range,
            PlaybackSpeed::from_percent(25).map_err(anyhow::Error::msg)?,
            &output,
            &token,
        )?;
        peak = peak.max(export_peak);
        ensure!(
            peak - base < 768.0,
            "unbounded long slow export: {} MiB",
            peak - base
        );
        println!(
            "{name}-long-slow,{open_ms:.2},{first_ms:.2},{:.2},{:.2},{playback_ms:.2},{polls},{export_ms:.2},{:.2},{error:.3},25,{},{duration_us},{output_frames},{}",
            seeks[5],
            seeks[9],
            peak - base,
            range.duration_us(),
            config.fps,
        );
    }
    Ok(())
}
fn main() -> Result<()> {
    ensure!(
        !cfg!(debug_assertions),
        "Use windows-msvc-performance, never Debug"
    );
    let directory = std::env::args()
        .nth(1)
        .unwrap_or_else(|| "recording-trim-performance".into());
    let directory = Path::new(&directory);
    std::fs::create_dir_all(directory)?;
    let requested_scenario = std::env::args().nth(2);
    let rates = if let Some(percent) = std::env::args().nth(3) {
        let percent: u32 = percent.parse()?;
        PlaybackSpeed::from_percent(percent).map_err(anyhow::Error::msg)?;
        vec![percent]
    } else {
        vec![100, 25, 300]
    };
    println!(
        "scenario,open_ms,first_frame_ms,seek_p50_ms,seek_max_ms,playback_ms,polls,export_ms,peak_delta_mib,hardware_pixel_error,speed_percent,source_duration_us,output_duration_us,output_frames,output_fps"
    );
    let scenarios = [
        ("1080p60", (1920, 1080), 10, 1, false),
        ("4k60", (3840, 2160), 3, 1, false),
        ("long-30min", (1920, 1080), 1800, 60, false),
        ("separate-audio", (320, 180), 3, 1, true),
    ];
    ensure!(
        requested_scenario
            .as_deref()
            .is_none_or(|requested| scenarios.iter().any(|scenario| scenario.0 == requested)),
        "unknown benchmark scenario"
    );
    for (name, size, seconds, stride, audio) in scenarios {
        if requested_scenario
            .as_deref()
            .is_none_or(|requested| requested == name)
        {
            scenario(directory, name, size, seconds, stride, audio, &rates)?;
        }
    }
    Ok(())
}
