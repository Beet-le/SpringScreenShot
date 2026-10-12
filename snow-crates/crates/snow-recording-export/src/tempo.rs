//! Bounded pitch-preserving audio on the output sample clock, shared by preview and export.
use crate::clip::{AudioReader, ClipSource, check_cancel};
use crate::error::{RecordingExportError, Result};
use ffmpeg_next as ffmpeg;
use snow_core::cancellation::CancellationToken;
use snow_recording_model::{PlaybackSpeed, TrimRange};

const BLOCK_FRAMES: usize = 960;
const MIN_ANALYSIS_FRAMES: u64 = 4096;

fn filter_error(error: impl std::fmt::Display) -> RecordingExportError {
    RecordingExportError::Encode(format!("audio tempo: {error}"))
}

struct TempoTrack {
    graph: ffmpeg::filter::Graph,
    input: ffmpeg::frame::Audio,
    frame: ffmpeg::frame::Audio,
    exhausted: bool,
}

impl TempoTrack {
    fn new(speed: PlaybackSpeed) -> Result<Self> {
        let mut graph = ffmpeg::filter::Graph::new();
        for (kind, name, args) in [
            (
                "abuffer",
                "in",
                "time_base=1/48000:sample_rate=48000:sample_fmt=s16:channel_layout=stereo",
            ),
            ("abuffersink", "out", ""),
        ] {
            let filter = ffmpeg::filter::find(kind)
                .ok_or_else(|| filter_error(format!("required filter {kind} is missing")))?;
            graph.add(&filter, name, args).map_err(filter_error)?;
        }
        // Matching packed PCM on both ends avoids conversion filters and extra buffers.
        let spec = match speed.percent() {
            25 => "atempo=0.5,atempo=0.5".into(),
            300 => "atempo=1.5,atempo=2".into(),
            percent => format!("atempo={}.{:02}", percent / 100, percent % 100),
        };
        graph
            .output("in", 0)
            .map_err(filter_error)?
            .input("out", 0)
            .map_err(filter_error)?
            .parse(&spec)
            .map_err(filter_error)?;
        graph.validate().map_err(filter_error)?;
        Ok(Self {
            graph,
            input: ffmpeg::frame::Audio::new(
                ffmpeg::format::Sample::I16(ffmpeg::format::sample::Type::Packed),
                BLOCK_FRAMES,
                ffmpeg::ChannelLayout::STEREO,
            ),
            frame: ffmpeg::frame::Audio::empty(),
            exhausted: false,
        })
    }

    fn push(&mut self, position: u64, samples: &[i16]) -> Result<()> {
        self.input.set_samples(BLOCK_FRAMES);
        let status = unsafe { ffmpeg::ffi::av_frame_make_writable(self.input.as_mut_ptr()) };
        if status < 0 {
            return Err(filter_error(ffmpeg::Error::from(status)));
        }
        self.input.set_samples(samples.len() / 2);
        self.input.set_rate(48_000);
        self.input
            .set_pts(Some(i64::try_from(position).map_err(filter_error)?));
        for (bytes, sample) in self.input.data_mut(0).chunks_exact_mut(2).zip(samples) {
            bytes.copy_from_slice(&sample.to_ne_bytes());
        }
        let status = unsafe {
            ffmpeg::ffi::av_buffersrc_add_frame_flags(
                self.graph.get("in").expect("tempo input").as_mut_ptr(),
                self.input.as_mut_ptr(),
                ffmpeg::ffi::AV_BUFFERSRC_FLAG_KEEP_REF as i32,
            )
        };
        if status < 0 {
            Err(filter_error(ffmpeg::Error::from(status)))
        } else {
            Ok(())
        }
    }

    fn finish(&mut self) -> Result<()> {
        self.graph
            .get("in")
            .expect("tempo input")
            .source()
            .flush()
            .map_err(filter_error)
    }

    fn pull(&mut self, output: &mut [i16]) -> Result<usize> {
        if self.exhausted {
            return Ok(0);
        }
        match self
            .graph
            .get("out")
            .expect("tempo output")
            .sink()
            .samples(&mut self.frame, output.len() / 2)
        {
            Ok(()) => {
                let count = self.frame.samples() * 2;
                for (sample, bytes) in output[..count]
                    .iter_mut()
                    .zip(self.frame.data(0)[..count * 2].chunks_exact(2))
                {
                    *sample = i16::from_ne_bytes([bytes[0], bytes[1]]);
                }
                Ok(count)
            }
            Err(ffmpeg::Error::Eof) => {
                self.exhausted = true;
                Ok(0)
            }
            Err(error) if crate::ffmpeg_util::is_eagain(&error) => Ok(0),
            Err(error) => Err(filter_error(error)),
        }
    }
}

pub(crate) struct RetimedAudioReader {
    reader: AudioReader,
    range: TrimRange,
    tracks: Vec<TempoTrack>,
    output: Vec<Vec<i16>>,
    written: Vec<usize>,
    short_pcm: Option<Vec<Vec<i16>>>,
    short_output: Option<Vec<Vec<i16>>>,
    source_position: u64,
    padding_position: u64,
    position: u64,
    total_samples: u64,
    eof: bool,
}

impl RetimedAudioReader {
    pub(crate) fn open(
        source: &ClipSource,
        range: TrimRange,
        speed: PlaybackSpeed,
    ) -> Result<Self> {
        let total_samples = if speed == PlaybackSpeed::default() {
            range.sample_count(48_000)
        } else {
            source
                .output_timing(range, speed)?
                .sample_count(48_000)
                .map_err(filter_error)?
                .max(u64::from(range.sample_count(48_000) != 0))
        };
        Ok(Self {
            reader: AudioReader::open(source)?,
            range,
            tracks: if speed == PlaybackSpeed::default() {
                Vec::new()
            } else {
                (0..source.config.audio.len())
                    .map(|_| TempoTrack::new(speed))
                    .collect::<Result<_>>()?
            },
            output: vec![Vec::new(); source.config.audio.len()],
            written: vec![0; source.config.audio.len()],
            short_pcm: if speed != PlaybackSpeed::default()
                && total_samples <= BLOCK_FRAMES as u64
                && range.sample_count(48_000) <= MIN_ANALYSIS_FRAMES
            {
                Some(vec![Vec::new(); source.config.audio.len()])
            } else {
                None
            },
            short_output: None,
            source_position: 0,
            padding_position: 0,
            position: 0,
            total_samples,
            eof: false,
        })
    }

    pub(crate) fn total_samples(&self) -> u64 {
        self.total_samples
    }

    pub(crate) fn position(&self) -> u64 {
        self.position
    }

    /// Sequential, bounded reads. Filter tails are drained before endpoint silence is added.
    pub(crate) fn read(&mut self, count: usize, token: &CancellationToken) -> Result<&[Vec<i16>]> {
        check_cancel(token)?;
        let requested = count
            .min(BLOCK_FRAMES)
            .min((self.total_samples - self.position) as usize);
        if self.short_output.is_some() {
            return Ok(self.read_cached(requested));
        }
        // Inspect the entire very short result before publishing any of its PCM.
        let count = if self.short_pcm.is_some() {
            self.total_samples as usize
        } else {
            requested
        };
        for output in &mut self.output {
            output.resize(count * 2, 0);
            output.fill(0);
        }
        if count == 0 {
            return Ok(&self.output);
        }
        if self.tracks.is_empty() {
            let tracks = self.reader.read(
                self.range.sample_offset(48_000) + self.position,
                count,
                token,
            )?;
            for (output, samples) in self.output.iter_mut().zip(tracks) {
                output.copy_from_slice(samples);
            }
        } else {
            self.written.fill(0);
            loop {
                check_cancel(token)?;
                let mut complete = true;
                let mut advanced = false;
                for ((track, output), done) in self
                    .tracks
                    .iter_mut()
                    .zip(&mut self.output)
                    .zip(&mut self.written)
                {
                    if *done < count * 2 && !track.exhausted {
                        let pulled = track.pull(&mut output[*done..])?;
                        advanced |= pulled != 0;
                        *done += pulled;
                    }
                    complete &= *done == count * 2 || track.exhausted;
                }
                if complete {
                    break;
                }
                // EOF may return a final short successful frame before the next
                // request reports Eof. Drain it fully before adding endpoint silence.
                if advanced {
                    continue;
                }
                let remaining = self.range.sample_count(48_000) - self.source_position;
                if remaining == 0 {
                    // WSOLA needs an analysis window even for a one-frame clip.
                    // Provide bounded silent context, never samples outside the cut,
                    // so flushing tiny selections does not discard their audible PCM.
                    let padding =
                        MIN_ANALYSIS_FRAMES.saturating_sub(self.range.sample_count(48_000));
                    if self.padding_position < padding {
                        let frames =
                            (padding - self.padding_position).min(BLOCK_FRAMES as u64) as usize;
                        let silence = [0i16; BLOCK_FRAMES * 2];
                        for track in &mut self.tracks {
                            track.push(
                                self.source_position + self.padding_position,
                                &silence[..frames * 2],
                            )?;
                        }
                        self.padding_position += frames as u64;
                    } else if !self.eof {
                        for track in &mut self.tracks {
                            track.finish()?;
                        }
                        self.eof = true;
                    } else {
                        return Err(filter_error("filter did not drain after EOF"));
                    }
                } else {
                    let frames = remaining.min(BLOCK_FRAMES as u64) as usize;
                    let samples = self.reader.read(
                        self.range.sample_offset(48_000) + self.source_position,
                        frames,
                        token,
                    )?;
                    if let Some(original) = &mut self.short_pcm {
                        for (saved, pcm) in original.iter_mut().zip(samples) {
                            saved.extend_from_slice(pcm);
                        }
                    }
                    for (track, pcm) in self.tracks.iter_mut().zip(samples) {
                        track.push(self.source_position, pcm)?;
                    }
                    self.source_position += frames as u64;
                }
            }
        }
        if let Some(original) = &self.short_pcm {
            for (output, pcm) in self.output.iter_mut().zip(original) {
                // A sub-window transient has insufficient WSOLA pitch context. A
                // fast stage can omit it entirely even with silent lookahead. Only
                // for this bounded, wholly silent result, retain the selected PCM
                // prefix at its native sample rate and truncate to the exact end.
                if output.iter().all(|sample| *sample == 0) && pcm.iter().any(|sample| *sample != 0)
                {
                    let len = output.len().min(pcm.len());
                    output[..len].copy_from_slice(&pcm[..len]);
                }
            }
            let cached = std::mem::take(&mut self.output);
            self.output = vec![Vec::new(); cached.len()];
            self.short_output = Some(cached);
            return Ok(self.read_cached(requested));
        }
        self.position += count as u64;
        Ok(&self.output)
    }

    fn read_cached(&mut self, count: usize) -> &[Vec<i16>] {
        let start = self.position as usize * 2;
        for (output, cached) in self
            .output
            .iter_mut()
            .zip(self.short_output.as_ref().expect("short tempo output"))
        {
            output.resize(count * 2, 0);
            output.copy_from_slice(&cached[start..start + count * 2]);
        }
        self.position += count as u64;
        &self.output
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

    fn fixture() -> (tempfile::TempDir, ClipSource) {
        let directory = tempfile::tempdir().unwrap();
        let config = StreamingEncoderConfig {
            output_path: directory.path().join("tone.mp4"),
            width: 32,
            height: 24,
            fps: 10,
            format: ExportFormat::Mp4,
            codec: VideoCodec::H264,
            video: VideoEncodeConfig {
                quality: 80,
                speed: VideoEncodingSpeed::UltraFast,
            },
            prefer_hardware_h264: false,
            execution_mode: ExportExecutionMode::SoftwareOnly,
            software_h264_priority: SoftwareH264Priority::X264First,
            encode_threads: 1,
            audio: vec![Default::default()],
            loop_animated_images: false,
        };
        let mut encoder = StreamingEncoder::create(config.clone()).unwrap();
        encoder
            .push_rgba_frame_at_pts(0, &[255; 32 * 24 * 4])
            .unwrap();
        let samples: Vec<i16> = (0..96_000)
            .flat_map(|index| {
                let sample = ((index as f64 * 440.0 * std::f64::consts::TAU / 48_000.0).sin()
                    * 12000.0) as i16;
                [sample, sample]
            })
            .collect();
        encoder
            .push_audio_track_pcm_i16_at_frame(&config.audio[0].track_id, 0, &samples)
            .unwrap();
        encoder.finish_at_duration_ms(2000).unwrap();
        let source = ClipSource::open(&config.output_path, Some(config.clone())).unwrap();
        (directory, source)
    }

    #[test]
    fn every_speed_preserves_tone_pitch_and_exact_bounded_output_sample_count() {
        let (_directory, source) = fixture();
        let range = TrimRange::new(250_000, 1_250_000, source.duration_us).unwrap();
        for percent in [25, 50, 75, 100, 125, 150, 175, 200, 300] {
            let speed = PlaybackSpeed::from_percent(percent).unwrap();
            let mut reader = RetimedAudioReader::open(&source, range, speed).unwrap();
            let total = reader.total_samples();
            let mut result = Vec::new();
            while reader.position() < total {
                let tracks = reader.read(960, &CancellationToken::default()).unwrap();
                assert!(tracks[0].len() <= 1920);
                result.extend(tracks[0].chunks_exact(2).map(|samples| samples[0]));
            }
            assert_eq!(result.len() as u64, total);
            let middle = &result[result.len() / 10..result.len() * 9 / 10];
            let crossings = middle
                .windows(2)
                .filter(|pair| pair[0] <= 0 && pair[1] > 0)
                .count();
            let frequency = crossings as f64 * 48_000.0 / middle.len() as f64;
            assert!(
                (frequency - 440.0).abs() < 12.0,
                "{percent}% changed pitch to {frequency} Hz"
            );
        }
    }

    #[test]
    fn tiny_selections_flush_tempo_eof_and_cancellation_interrupts_reads() {
        let (_directory, source) = fixture();
        for duration in [1_000, 7_000, 21_000] {
            let range = TrimRange::new(200_000, 200_000 + duration, source.duration_us).unwrap();
            for percent in [25, 75, 175, 300] {
                let mut reader = RetimedAudioReader::open(
                    &source,
                    range,
                    PlaybackSpeed::from_percent(percent).unwrap(),
                )
                .unwrap();
                let mut count = 0;
                let mut audible = false;
                while reader.position() < reader.total_samples() {
                    let tracks = reader.read(7, &CancellationToken::default()).unwrap();
                    count += tracks[0].len() as u64 / 2;
                    audible |= tracks[0].iter().any(|sample| sample.abs() > 100);
                }
                assert_eq!(count, reader.total_samples());
                assert!(audible, "{duration} us at {percent}% lost its audio");
            }
        }
        let token = CancellationToken::default();
        token.cancel();
        let mut reader = RetimedAudioReader::open(
            &source,
            source.range(0, 2).unwrap(),
            PlaybackSpeed::from_percent(25).unwrap(),
        )
        .unwrap();
        assert!(matches!(
            reader.read(960, &token),
            Err(RecordingExportError::ExportCanceled)
        ));
    }
}
