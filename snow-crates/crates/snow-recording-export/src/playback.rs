//! Bounded, cancellable preview. A single worker owns every decoder and audio device.
use crate::clip::{ClipSource, VideoReader};
use crate::error::{RecordingExportError, Result};
use crate::tempo::RetimedAudioReader;
use snow_core::cancellation::CancellationToken;
use snow_memory::RasterBuffer;
use snow_recording_model::{PlaybackSpeed, TrimRange};
use std::sync::{Arc, Condvar, Mutex};
use std::thread::JoinHandle;
use std::time::{Duration, Instant};

mod output;

trait AudioSink {
    fn reset(&mut self);
    fn available(&self) -> u64;
    fn played(&self) -> u64;
    fn push(&mut self, samples: &[i16]) -> bool;
}
impl AudioSink for output::Output {
    fn reset(&mut self) {
        self.reset();
    }
    fn available(&self) -> u64 {
        self.available()
    }
    fn played(&self) -> u64 {
        self.played()
    }
    fn push(&mut self, samples: &[i16]) -> bool {
        self.push(samples)
    }
}

#[derive(Clone, Default)]
pub struct PreviewSnapshot {
    pub revision: u64,
    pub position_us: u64,
    pub playing: bool,
    pub pixels: Arc<RasterBuffer>,
    pub error: String,
}

struct Command {
    revision: u64,
    seek_revision: u64,
    speed: PlaybackSpeed,
    position_us: u64,
    end_us: u64,
    playing: bool,
    closed: bool,
    cancellation: CancellationToken,
}
struct State {
    command: Mutex<Command>,
    wake: Condvar,
    snapshot: Mutex<PreviewSnapshot>,
}
pub struct Playback {
    state: Arc<State>,
    worker: Option<JoinHandle<()>>,
}

impl Playback {
    pub fn open(source: ClipSource) -> Result<Self> {
        let epoch = Instant::now();
        let has_audio = !source.config.audio.is_empty();
        Self::open_with(
            source,
            move || epoch.elapsed(),
            move || {
                if has_audio {
                    output::Output::open().map(|output| Box::new(output) as Box<dyn AudioSink>)
                } else {
                    None
                }
            },
        )
    }

    fn open_with(
        source: ClipSource,
        clock: impl Fn() -> Duration + Send + 'static,
        output: impl FnOnce() -> Option<Box<dyn AudioSink>> + Send + 'static,
    ) -> Result<Self> {
        let state = Arc::new(State {
            command: Mutex::new(Command {
                revision: 1,
                seek_revision: 1,
                speed: PlaybackSpeed::default(),
                position_us: 0,
                end_us: source.duration_us,
                playing: false,
                closed: false,
                cancellation: Default::default(),
            }),
            wake: Condvar::new(),
            snapshot: Default::default(),
        });
        let shared = Arc::clone(&state);
        let worker = std::thread::Builder::new()
            .name("snow-recording-playback".into())
            .spawn(move || {
                snow_core::qos::apply_current_thread();
                let result = std::panic::catch_unwind(std::panic::AssertUnwindSafe(|| {
                    run(&source, &shared, &clock, output())
                }))
                .unwrap_or_else(|_| {
                    Err(RecordingExportError::Decode(
                        "preview worker panicked".into(),
                    ))
                });
                if let Err(error) = result {
                    let mut snapshot = shared.snapshot.lock().unwrap_or_else(|e| e.into_inner());
                    snapshot.error = error.to_string();
                    snapshot.playing = false;
                }
            })?;
        Ok(Self {
            state,
            worker: Some(worker),
        })
    }

    pub fn seek(&self, position_us: u64, end_us: u64, playing: bool) -> u64 {
        let mut command = self.state.command.lock().unwrap_or_else(|e| e.into_inner());
        command.cancellation.cancel();
        command.cancellation = Default::default();
        command.revision += 1;
        command.seek_revision = command.revision;
        command.position_us = position_us;
        command.end_us = end_us;
        command.playing = playing;
        self.state.wake.notify_one();
        command.revision
    }

    /// Change rate without taking a stale UI position or playback state as an anchor.
    pub fn set_speed(&self, speed: PlaybackSpeed) -> u64 {
        let mut command = self.state.command.lock().unwrap_or_else(|e| e.into_inner());
        if command.speed == speed {
            return command.revision;
        }
        command.cancellation.cancel();
        command.cancellation = Default::default();
        command.revision += 1;
        command.speed = speed;
        self.state.wake.notify_one();
        command.revision
    }

    pub fn snapshot(&self) -> PreviewSnapshot {
        self.state
            .snapshot
            .lock()
            .unwrap_or_else(|e| e.into_inner())
            .clone()
    }
}

impl Drop for Playback {
    fn drop(&mut self) {
        {
            let mut command = self.state.command.lock().unwrap_or_else(|e| e.into_inner());
            command.closed = true;
            command.cancellation.cancel();
            self.state.wake.notify_one();
        }
        if let Some(worker) = self.worker.take() {
            let _ = worker.join();
        }
    }
}

fn sample_duration_us(frames: u64) -> u64 {
    (u128::from(frames) * 1_000_000 / 48_000).min(u128::from(u64::MAX)) as u64
}

fn run(
    source: &ClipSource,
    state: &State,
    clock: &impl Fn() -> Duration,
    mut sink: Option<Box<dyn AudioSink>>,
) -> Result<()> {
    let mut video = VideoReader::open(source.clone(), true)?;
    if source.config.audio.is_empty() {
        sink = None;
    }
    let mut audio = None;
    let mut revision = 0;
    let mut seek_revision = 0;
    let mut speed = PlaybackSpeed::default();
    let mut origin = 0;
    let mut end = source.duration_us;
    let mut started = clock();
    let mut audio_advanced = clock();
    let mut audio_position = 0;
    let mut playing = false;
    let mut next_audio = 0;
    let mut pending_audio_frames = 0;
    let mut last_frame = None;
    let mut pixels = RasterBuffer::new();
    let mut mixed = vec![0i16; 960 * 2];
    loop {
        let command = state.command.lock().unwrap_or_else(|e| e.into_inner());
        let command = if command.revision == revision && !playing && !command.closed {
            state.wake.wait(command).unwrap_or_else(|e| e.into_inner())
        } else {
            command
        };
        if command.closed {
            return Ok(());
        }
        let token = command.cancellation.clone();
        let intent = (
            command.revision,
            command.seek_revision,
            command.position_us,
            command.end_us,
            command.playing,
            command.speed,
        );
        // Filesystem/decoder/filter/device work must never hold the GUI command lock.
        drop(command);
        if intent.0 != revision {
            revision = intent.0;
            if intent.1 != seek_revision {
                seek_revision = intent.1;
                origin = intent.2.min(source.duration_us.saturating_sub(1));
                end = intent.3.min(source.duration_us).max(origin + 1);
                playing = intent.4;
            } else if playing {
                // Freeze the precise old-rate source clock before clearing queued PCM.
                let elapsed = sink.as_ref().map_or_else(
                    || {
                        if last_frame.is_none() {
                            // Silent playback starts only after its first picture.
                            // A canceled priming decode has no elapsed media time.
                            0
                        } else {
                            clock()
                                .saturating_sub(started)
                                .as_micros()
                                .min(u128::from(u64::MAX)) as u64
                        }
                    },
                    |sink| sample_duration_us(sink.played()),
                );
                origin = origin
                    .saturating_add(speed.source_elapsed_us(elapsed))
                    .min(end);
                playing = origin < end;
            }
            speed = intent.5;
            started = clock();
            audio_advanced = started;
            audio_position = 0;
            next_audio = 0;
            pending_audio_frames = 0;
            last_frame = None;
            if let Some(sink) = &mut sink {
                sink.reset();
            }
            let opened = if sink.is_some() && playing {
                RetimedAudioReader::open(
                    source,
                    TrimRange {
                        start_us: origin,
                        end_us: end,
                    },
                    speed,
                )
                .map(Some)
            } else {
                Ok(None)
            };
            if token.is_canceled() {
                continue;
            }
            audio = opened?;
        }
        if playing && let Some(device) = &sink {
            let position = device.played();
            if position != audio_position {
                audio_position = position;
                audio_advanced = clock();
            } else if clock().saturating_sub(audio_advanced) > Duration::from_millis(500) {
                // A disconnected or stalled device must never prevent silent playback.
                started =
                    clock().saturating_sub(Duration::from_micros(sample_duration_us(position)));
                sink = None;
            }
        }
        let elapsed = if playing {
            sink.as_ref().map_or(
                clock()
                    .saturating_sub(started)
                    .as_micros()
                    .min(u128::from(u64::MAX)) as u64,
                |sink| sample_duration_us(sink.played()),
            )
        } else {
            0
        };
        let at = origin
            .saturating_add(speed.source_elapsed_us(elapsed))
            .min(end.saturating_sub(1));
        let frame = source.frame_at(at);
        let render = (|| -> Result<()> {
            if last_frame != Some(frame) {
                video.read(source.boundary(frame), &token)?;
                video.copy_rgba_buffer(&mut pixels)?;
                if last_frame.is_none() {
                    started = clock();
                    audio_advanced = started;
                }
            }
            if playing && let Some(device) = &mut sink {
                let total_audio = audio.as_ref().expect("audio device reader").total_samples();
                // Never queue more than the device's bounded buffer, or beyond the cut.
                while next_audio < total_audio && device.available() >= 960 {
                    if pending_audio_frames == 0 {
                        pending_audio_frames = (total_audio - next_audio).min(960) as usize;
                        mixed[..pending_audio_frames * 2].fill(0);
                        let tracks = audio
                            .as_mut()
                            .expect("audio device reader")
                            .read(pending_audio_frames, &token)?;
                        for samples in tracks {
                            for (destination, sample) in mixed.iter_mut().zip(samples) {
                                *destination = destination.saturating_add(*sample);
                            }
                        }
                    }
                    // A raced device refusal must not advance the sequential tempo reader.
                    // Retain the exact block until the device accepts it or a revision resets it.
                    if !device.push(&mixed[..pending_audio_frames * 2]) {
                        break;
                    }
                    next_audio += pending_audio_frames as u64;
                    pending_audio_frames = 0;
                }
            }
            Ok(())
        })();
        if token.is_canceled() {
            continue;
        }
        render?;
        if playing && speed.source_elapsed_us(elapsed) >= end - origin {
            playing = false;
            origin = end;
        }
        // Audio duration is rounded to samples; don't wait forever for a fractional sample.
        if playing
            && sink.as_ref().is_some_and(|sink| {
                sink.played() >= audio.as_ref().expect("audio device reader").total_samples()
            })
        {
            playing = false;
            origin = end;
        }
        let command = state.command.lock().unwrap_or_else(|e| e.into_inner());
        if command.revision == revision {
            let mut snapshot = state.snapshot.lock().unwrap_or_else(|e| e.into_inner());
            snapshot.revision = revision;
            snapshot.position_us = if playing {
                at
            } else if command.playing {
                end
            } else {
                origin
            };
            snapshot.playing = playing;
            if last_frame != Some(frame) {
                // Swap an unshared retired buffer back into the decoder when possible.
                let retired =
                    std::mem::replace(&mut snapshot.pixels, Arc::new(std::mem::take(&mut pixels)));
                pixels = Arc::try_unwrap(retired).unwrap_or_default();
            }
            last_frame = Some(frame);
        }
        if playing {
            let _ = state
                .wake
                .wait_timeout(command, Duration::from_millis(8))
                .unwrap_or_else(|e| e.into_inner());
        }
    }
}

#[derive(Clone, Default)]
pub struct ClipExportProgress {
    pub state: u32,
    pub percent: f32,
    pub error: String,
}
pub struct ClipExportTask {
    token: CancellationToken,
    progress: Arc<Mutex<ClipExportProgress>>,
    worker: Option<JoinHandle<()>>,
}
impl ClipExportTask {
    pub fn start(
        source: ClipSource,
        range: snow_recording_model::TrimRange,
        path: std::path::PathBuf,
    ) -> Result<Self> {
        Self::start_with_speed(source, range, path, PlaybackSpeed::default())
    }

    pub fn start_with_speed(
        source: ClipSource,
        range: snow_recording_model::TrimRange,
        path: std::path::PathBuf,
        speed: PlaybackSpeed,
    ) -> Result<Self> {
        let token = CancellationToken::default();
        let worker_token = token.clone();
        let progress = Arc::new(Mutex::new(ClipExportProgress::default()));
        let shared = Arc::clone(&progress);
        let worker = std::thread::Builder::new()
            .name("snow-recording-trim-export".into())
            .spawn(move || {
                snow_core::qos::apply_current_thread();
                let result = std::panic::catch_unwind(std::panic::AssertUnwindSafe(|| {
                    source.export_with_speed(range, speed, &path, &worker_token, |percent| {
                        shared.lock().unwrap_or_else(|e| e.into_inner()).percent = percent;
                    })
                }))
                .unwrap_or_else(|_| {
                    Err(RecordingExportError::Export("trim worker panicked".into()))
                });
                let mut progress = shared.lock().unwrap_or_else(|e| e.into_inner());
                match result {
                    Ok(()) => {
                        progress.state = 1;
                        progress.percent = 100.0;
                    }
                    Err(RecordingExportError::ExportCanceled) => progress.state = 2,
                    Err(error) => {
                        progress.state = 3;
                        progress.error = error.to_string();
                    }
                }
            })?;
        Ok(Self {
            token,
            progress,
            worker: Some(worker),
        })
    }
    pub fn cancel(&self) {
        self.token.cancel();
    }
    pub fn snapshot(&self) -> ClipExportProgress {
        self.progress
            .lock()
            .unwrap_or_else(|e| e.into_inner())
            .clone()
    }
}
impl Drop for ClipExportTask {
    fn drop(&mut self) {
        self.cancel();
        if let Some(worker) = self.worker.take() {
            let _ = worker.join();
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::{
        ExportExecutionMode, ExportFormat, SoftwareH264Priority, StreamingEncoder,
        StreamingEncoderConfig,
    };
    use snow_recording_model::{VideoCodec, VideoEncodeConfig, VideoEncodingSpeed};
    use std::sync::atomic::{AtomicU64, Ordering};

    fn fixture() -> (tempfile::TempDir, ClipSource) {
        fixture_with_audio(false)
    }
    fn fixture_with_audio(has_audio: bool) -> (tempfile::TempDir, ClipSource) {
        let directory = tempfile::tempdir().unwrap();
        let config = StreamingEncoderConfig {
            output_path: directory.path().join("preview.mp4"),
            format: ExportFormat::Mp4,
            width: 32,
            height: 24,
            fps: 10,
            codec: VideoCodec::H264,
            prefer_hardware_h264: false,
            execution_mode: ExportExecutionMode::SoftwareOnly,
            software_h264_priority: SoftwareH264Priority::X264First,
            video: VideoEncodeConfig {
                quality: 90,
                speed: VideoEncodingSpeed::UltraFast,
            },
            encode_threads: 1,
            audio: if has_audio {
                vec![Default::default()]
            } else {
                Vec::new()
            },
            loop_animated_images: false,
        };
        let mut encoder = StreamingEncoder::create(config.clone()).unwrap();
        for index in 0..10 {
            encoder
                .push_rgba_frame_at_pts(
                    index,
                    &[20 + index as u8 * 20, 50, 100, 255].repeat(32 * 24),
                )
                .unwrap();
            if has_audio {
                let samples: Vec<i16> = (index * 4800..(index + 1) * 4800)
                    .flat_map(|sample| {
                        let amplitude = 2000.0 + (sample / 960 % 5) as f64 * 1500.0;
                        let value = ((sample as f64 * std::f64::consts::TAU * 431.0 / 48_000.0)
                            .sin()
                            * amplitude) as i16;
                        [value, value]
                    })
                    .collect();
                encoder
                    .push_audio_track_pcm_i16_at_frame(
                        &config.audio[0].track_id,
                        index * 4800,
                        &samples,
                    )
                    .unwrap();
            }
        }
        encoder.finish_at_duration_ms(1000).unwrap();
        let source = ClipSource::open(&config.output_path, Some(config.clone())).unwrap();
        (directory, source)
    }
    fn wait(playback: &Playback, predicate: impl Fn(&PreviewSnapshot) -> bool) -> PreviewSnapshot {
        let deadline = Instant::now();
        loop {
            let snapshot = playback.snapshot();
            assert!(snapshot.error.is_empty(), "{}", snapshot.error);
            if predicate(&snapshot) {
                return snapshot;
            }
            assert!(
                deadline.elapsed() < Duration::from_secs(5),
                "preview did not reach the requested deterministic state"
            );
            playback.state.wake.notify_one();
            std::thread::yield_now();
        }
    }
    #[test]
    fn playback_autoplays_once_replays_the_range_and_rejects_stale_seeks() {
        let (_directory, source) = fixture();
        let clock = Arc::new(AtomicU64::new(0));
        let worker_clock = clock.clone();
        let mut expected = VideoReader::open(source.clone(), false).unwrap();
        expected
            .read(600_000, &CancellationToken::default())
            .unwrap();
        let mut last_included = Vec::new();
        expected.copy_rgba(&mut last_included).unwrap();
        let playback = Playback::open_with(
            source,
            move || Duration::from_micros(worker_clock.load(Ordering::SeqCst)),
            || None,
        )
        .unwrap();
        let revision = playback.seek(200_000, 700_000, true);
        wait(&playback, |s| s.revision == revision && s.playing);
        clock.store(600_000, Ordering::SeqCst);
        let ended = wait(&playback, |s| s.revision == revision && !s.playing);
        assert_eq!(ended.position_us, 700_000);
        assert_eq!(
            *ended.pixels, last_included,
            "playback must retain the final included picture, not the end boundary"
        );
        clock.store(5_000_000, Ordering::SeqCst);
        assert_eq!(playback.snapshot().revision, revision);
        assert!(!playback.snapshot().playing);
        let replay = playback.seek(200_000, 700_000, true);
        wait(&playback, |s| {
            s.revision == replay && s.playing && s.position_us == 200_000
        });
        let mut last = replay;
        for index in 0..100 {
            last = playback.seek((index % 6) * 100_000, 700_000, false);
        }
        let scrub = wait(&playback, |s| s.revision == last);
        assert_eq!(scrub.position_us, 300_000);
        assert!(!scrub.playing);
        // Teardown races a fresh request, while an independently held frame stays valid.
        playback.seek(0, 1_000_000, true);
        drop(playback);
        assert_eq!(scrub.pixels.len(), ended.pixels.len());
    }

    #[derive(Default)]
    struct SinkState {
        written: AtomicU64,
        played: AtomicU64,
        largest_queue: AtomicU64,
        peak: AtomicU64,
        dropped: std::sync::atomic::AtomicBool,
    }
    struct FakeSink(Arc<SinkState>);
    impl Drop for FakeSink {
        fn drop(&mut self) {
            self.0.dropped.store(true, Ordering::SeqCst);
        }
    }
    impl AudioSink for FakeSink {
        fn reset(&mut self) {
            self.0.written.store(0, Ordering::SeqCst);
            self.0.played.store(0, Ordering::SeqCst);
        }
        fn available(&self) -> u64 {
            1920u64.saturating_sub(
                self.0
                    .written
                    .load(Ordering::SeqCst)
                    .saturating_sub(self.played()),
            )
        }
        fn played(&self) -> u64 {
            self.0.played.load(Ordering::SeqCst)
        }
        fn push(&mut self, samples: &[i16]) -> bool {
            let frames = samples.len() as u64 / 2;
            assert!(frames <= self.available());
            let written = self.0.written.fetch_add(frames, Ordering::SeqCst) + frames;
            self.0
                .largest_queue
                .fetch_max(written - self.played(), Ordering::SeqCst);
            self.0.peak.fetch_max(
                samples
                    .iter()
                    .map(|sample| u64::from(sample.unsigned_abs()))
                    .max()
                    .unwrap_or(0),
                Ordering::SeqCst,
            );
            true
        }
    }
    #[test]
    fn refused_audio_block_is_retried_without_skipping_source_pcm() {
        struct RefusingSink(Arc<Mutex<Vec<Vec<i16>>>>);
        impl AudioSink for RefusingSink {
            fn reset(&mut self) {
                self.0.lock().unwrap().clear();
            }
            fn available(&self) -> u64 {
                if self.0.lock().unwrap().len() < 2 {
                    960
                } else {
                    0
                }
            }
            fn played(&self) -> u64 {
                0
            }
            fn push(&mut self, samples: &[i16]) -> bool {
                let mut attempts = self.0.lock().unwrap();
                attempts.push(samples.to_vec());
                attempts.len() > 1
            }
        }
        let (_directory, source) = fixture_with_audio(true);
        let attempts = Arc::new(Mutex::new(Vec::new()));
        let worker_attempts = attempts.clone();
        let playback = Playback::open_with(
            source,
            || Duration::ZERO,
            move || Some(Box::new(RefusingSink(worker_attempts))),
        )
        .unwrap();
        playback.set_speed(PlaybackSpeed::from_percent(125).unwrap());
        let revision = playback.seek(200_000, 700_000, true);
        wait(&playback, |snapshot| {
            snapshot.revision == revision && attempts.lock().unwrap().len() == 2
        });
        let attempts = attempts.lock().unwrap();
        assert!(attempts[0].iter().any(|sample| *sample != 0));
        assert_eq!(
            attempts[0], attempts[1],
            "a refused block must be replayed byte for byte"
        );
    }
    #[test]
    fn playback_audio_is_bounded_and_device_loss_falls_back_to_a_silent_clock() {
        let (_directory, mut source) = fixture();
        source.config.audio.push(Default::default());
        let clock = Arc::new(AtomicU64::new(0));
        let worker_clock = clock.clone();
        let audio = Arc::new(SinkState::default());
        let worker_audio = audio.clone();
        let playback = Playback::open_with(
            source,
            move || Duration::from_micros(worker_clock.load(Ordering::SeqCst)),
            move || Some(Box::new(FakeSink(worker_audio))),
        )
        .unwrap();
        let revision = playback.seek(100_000, 500_000, true);
        wait(&playback, |s| s.revision == revision && s.playing);
        assert_eq!(audio.written.load(Ordering::SeqCst), 1920);
        assert!(audio.largest_queue.load(Ordering::SeqCst) <= 1920);
        assert_eq!(
            audio.peak.load(Ordering::SeqCst),
            0,
            "tempo must preserve silent source tracks"
        );
        audio.played.store(960, Ordering::SeqCst);
        wait(&playback, |s| s.position_us == 120_000);
        clock.store(600_000, Ordering::SeqCst);
        // Let the worker observe the stalled device and switch clocks before advancing time.
        wait(&playback, |_| audio.dropped.load(Ordering::SeqCst));
        clock.store(1_200_000, Ordering::SeqCst);
        wait(&playback, |s| !s.playing && s.position_us == 500_000);
    }

    #[test]
    fn audio_sample_clock_tracks_source_speed_and_rate_changes_reset_queued_pcm() {
        let (_directory, mut source) = fixture();
        source.config.audio.push(Default::default());
        let audio = Arc::new(SinkState::default());
        let worker_audio = audio.clone();
        let playback = Playback::open_with(
            source,
            || Duration::ZERO,
            move || Some(Box::new(FakeSink(worker_audio))),
        )
        .unwrap();
        for percent in [25, 100, 300] {
            let paused = playback.seek(100_000, 700_000, false);
            wait(&playback, |s| s.revision == paused && !s.playing);
            let rate = playback.set_speed(PlaybackSpeed::from_percent(percent).unwrap());
            wait(&playback, |s| s.revision == rate && !s.playing);
            let play = playback.seek(100_000, 700_000, true);
            wait(&playback, |s| s.revision == play && s.playing);
            assert_eq!(audio.written.load(Ordering::SeqCst), 1920);
            audio.played.store(960, Ordering::SeqCst);
            let position = 100_000 + 20_000 * u64::from(percent) / 100;
            wait(&playback, |s| s.position_us == position);
            let changed = playback.set_speed(PlaybackSpeed::from_percent(175).unwrap());
            wait(&playback, |s| {
                s.revision == changed && s.playing && s.position_us == position
            });
            assert_eq!(
                audio.played.load(Ordering::SeqCst),
                0,
                "old-rate audio must be discarded"
            );
            assert_eq!(audio.written.load(Ordering::SeqCst), 1920);
            audio.played.store(960, Ordering::SeqCst);
            wait(&playback, |s| s.position_us == position + 35_000);
        }
        assert!(audio.largest_queue.load(Ordering::SeqCst) <= 1920);
        assert_eq!(
            audio.peak.load(Ordering::SeqCst),
            0,
            "tempo must preserve silent source tracks"
        );
    }

    #[test]
    fn speed_changes_preserve_precise_worker_position_pause_and_completed_state() {
        let (_directory, source) = fixture();
        let clock = Arc::new(AtomicU64::new(0));
        let worker_clock = clock.clone();
        let playback = Playback::open_with(
            source,
            move || Duration::from_micros(worker_clock.load(Ordering::SeqCst)),
            || None,
        )
        .unwrap();
        let seek = playback.seek(200_000, 700_000, true);
        wait(&playback, |s| s.revision == seek && s.playing);
        clock.store(53_333, Ordering::SeqCst);
        wait(&playback, |s| s.position_us == 253_333);
        let revision = playback.set_speed(PlaybackSpeed::from_percent(300).unwrap());
        wait(&playback, |s| {
            s.revision == revision && s.playing && s.position_us == 253_333
        });
        clock.store(83_333, Ordering::SeqCst);
        wait(&playback, |s| s.position_us == 343_333);

        let pause = playback.seek(380_555, 700_000, false);
        wait(&playback, |s| {
            s.revision == pause && !s.playing && s.position_us == 380_555
        });
        let revision = playback.set_speed(PlaybackSpeed::from_percent(25).unwrap());
        wait(&playback, |s| {
            s.revision == revision && !s.playing && s.position_us == 380_555
        });
        let play = playback.seek(380_555, 700_000, true);
        wait(&playback, |s| s.revision == play && s.playing);
        clock.store(2_083_333, Ordering::SeqCst);
        wait(&playback, |s| !s.playing && s.position_us == 700_000);
        let revision = playback.set_speed(PlaybackSpeed::from_percent(175).unwrap());
        wait(&playback, |s| {
            s.revision == revision && !s.playing && s.position_us == 700_000
        });
        let replay = playback.seek(200_000, 700_000, true);
        wait(&playback, |s| s.revision == replay && s.playing);
        clock.store(2_183_333, Ordering::SeqCst);
        wait(&playback, |s| s.position_us == 375_000);
    }

    #[test]
    fn latest_seek_and_speed_share_one_authoritative_command_revision() {
        let (_directory, source) = fixture();
        let playback = Playback::open_with(source, || Duration::ZERO, || None).unwrap();
        let mut last = 0;
        for index in 0..100 {
            playback.seek((index % 6) * 100_000, 700_000, false);
            last = playback.set_speed(
                PlaybackSpeed::from_percent(if index % 2 == 0 { 25 } else { 300 }).unwrap(),
            );
        }
        let snapshot = wait(&playback, |s| s.revision == last);
        assert_eq!(snapshot.position_us, 300_000);
        assert!(!snapshot.playing);
        assert_eq!(
            playback.set_speed(PlaybackSpeed::from_percent(300).unwrap()),
            last
        );
    }

    #[test]
    fn control_commands_stay_nonblocking_while_worker_resets_audio_device() {
        struct GatedSink {
            sink: FakeSink,
            resets: usize,
            entered: std::sync::mpsc::Sender<()>,
            resume: std::sync::mpsc::Receiver<()>,
        }
        impl AudioSink for GatedSink {
            fn reset(&mut self) {
                self.resets += 1;
                if self.resets == 2 {
                    self.entered.send(()).unwrap();
                    self.resume.recv().unwrap();
                }
                self.sink.reset();
            }
            fn available(&self) -> u64 {
                self.sink.available()
            }
            fn played(&self) -> u64 {
                self.sink.played()
            }
            fn push(&mut self, samples: &[i16]) -> bool {
                self.sink.push(samples)
            }
        }
        let (_directory, mut source) = fixture();
        source.config.audio.push(Default::default());
        let (entered_tx, entered_rx) = std::sync::mpsc::channel();
        let (resume_tx, resume_rx) = std::sync::mpsc::channel();
        let sink = GatedSink {
            sink: FakeSink(Arc::new(SinkState::default())),
            resets: 0,
            entered: entered_tx,
            resume: resume_rx,
        };
        let playback = Arc::new(
            Playback::open_with(source, || Duration::ZERO, move || Some(Box::new(sink))).unwrap(),
        );
        wait(&playback, |snapshot| snapshot.revision == 1);
        playback.seek(100_000, 700_000, true);
        entered_rx.recv_timeout(Duration::from_secs(5)).unwrap();
        let controls = playback.clone();
        let (done_tx, done_rx) = std::sync::mpsc::channel();
        let control = std::thread::spawn(move || {
            controls.set_speed(PlaybackSpeed::from_percent(300).unwrap());
            done_tx
                .send(controls.seek(400_000, 700_000, false))
                .unwrap();
        });
        let result = done_rx.recv_timeout(Duration::from_secs(1));
        // Always release the worker, including when this regression fails.
        resume_tx.send(()).unwrap();
        control.join().unwrap();
        let revision = result.expect("GUI controls waited for the worker's audio device");
        wait(&playback, |snapshot| {
            snapshot.revision == revision && snapshot.position_us == 400_000 && !snapshot.playing
        });
    }

    #[test]
    fn speed_change_during_silent_priming_does_not_count_unpublished_time() {
        let (_directory, source) = fixture();
        let clock = Arc::new(AtomicU64::new(0));
        let armed = Arc::new(std::sync::atomic::AtomicBool::new(false));
        let calls = Arc::new(std::sync::atomic::AtomicUsize::new(0));
        let (entered_tx, entered_rx) = std::sync::mpsc::channel();
        let (resume_tx, resume_rx) = std::sync::mpsc::channel();
        let worker_clock = clock.clone();
        let worker_armed = armed.clone();
        let playback = Playback::open_with(
            source,
            move || {
                // Hold the first playing iteration after its start anchor, before
                // any picture can publish, while the controlled clock advances.
                if worker_armed.load(Ordering::SeqCst) && calls.fetch_add(1, Ordering::SeqCst) == 1
                {
                    entered_tx.send(()).unwrap();
                    resume_rx.recv().unwrap();
                }
                Duration::from_micros(worker_clock.load(Ordering::SeqCst))
            },
            || None,
        )
        .unwrap();
        wait(&playback, |snapshot| snapshot.revision == 1);
        armed.store(true, Ordering::SeqCst);
        playback.seek(200_000, 700_000, true);
        entered_rx.recv_timeout(Duration::from_secs(5)).unwrap();
        clock.store(1_000_000, Ordering::SeqCst);
        let revision = playback.set_speed(PlaybackSpeed::from_percent(300).unwrap());
        resume_tx.send(()).unwrap();
        wait(&playback, |snapshot| {
            snapshot.revision == revision && snapshot.playing && snapshot.position_us == 200_000
        });
    }
}
