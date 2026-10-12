//! A clip is a half-open interval on the original, pause-free media clock.

/// The editor's exact, finite set of playback rates. Timing never uses floating point.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct PlaybackSpeed(u32);

impl Default for PlaybackSpeed {
    fn default() -> Self {
        Self(100)
    }
}

impl PlaybackSpeed {
    pub fn from_percent(percent: u32) -> Result<Self, String> {
        if matches!(percent, 25 | 50 | 75 | 100 | 125 | 150 | 175 | 200 | 300) {
            Ok(Self(percent))
        } else {
            Err("unsupported playback speed".into())
        }
    }

    pub fn percent(self) -> u32 {
        self.0
    }

    pub fn source_elapsed_us(self, elapsed_us: u64) -> u64 {
        (u128::from(elapsed_us) * u128::from(self.0) / 100).min(u128::from(u64::MAX)) as u64
    }

    pub fn output_duration_us(self, source_us: u64) -> Result<u64, String> {
        RetimedFrameTiming::from_duration_us(source_us, self, 1)?.duration_us()
    }

    /// Count presentation slots before rounding the retimed duration to microseconds.
    pub fn output_frame_count(self, source_us: u64, fps: u32) -> Result<u64, String> {
        RetimedFrameTiming::from_duration_us(source_us, self, fps)?.frame_count()
    }

    /// Direct rational mapping avoids first rounding an output slot to microseconds.
    pub fn source_time_at_frame(self, index: u64, fps: u32) -> Result<u64, String> {
        if fps == 0 {
            return Err("retimed frame rate must be positive".into());
        }
        u64::try_from(u128::from(index) * 1_000_000 * u128::from(self.0) / (u128::from(fps) * 100))
            .map_err(|_| "retimed source timestamp overflow".into())
    }
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
struct RationalTime {
    numerator: u128,
    denominator: u128,
}

fn gcd(mut a: u128, mut b: u128) -> u128 {
    while b != 0 {
        (a, b) = (b, a % b);
    }
    a
}

fn timing_overflow() -> String {
    "recording timing overflow".into()
}

impl RationalTime {
    fn new(numerator: u128, denominator: u128) -> Self {
        let divisor = gcd(numerator, denominator);
        Self {
            numerator: numerator / divisor,
            denominator: denominator / divisor,
        }
    }

    fn scale(self, numerator: u128, denominator: u128) -> Result<Self, String> {
        // Cancel cross factors before multiplying, including very long native timelines.
        let a = gcd(self.numerator, denominator);
        let b = gcd(numerator, self.denominator);
        Ok(Self::new(
            (self.numerator / a)
                .checked_mul(numerator / b)
                .ok_or_else(timing_overflow)?,
            (self.denominator / b)
                .checked_mul(denominator / a)
                .ok_or_else(timing_overflow)?,
        ))
    }

    fn subtract(self, start: Self) -> Result<Self, String> {
        let divisor = gcd(self.denominator, start.denominator);
        let end = self
            .numerator
            .checked_mul(start.denominator / divisor)
            .ok_or_else(timing_overflow)?;
        let start_value = start
            .numerator
            .checked_mul(self.denominator / divisor)
            .ok_or_else(timing_overflow)?;
        Ok(Self::new(
            end.checked_sub(start_value)
                .ok_or("trim endpoint precedes its exact start")?,
            self.denominator
                .checked_mul(start.denominator / divisor)
                .ok_or_else(timing_overflow)?,
        ))
    }

    fn ticks(self, rate: u32, round_up: bool) -> Result<u64, String> {
        let scaled = self.scale(u128::from(rate), 1)?;
        u64::try_from(if round_up {
            scaled.numerator.div_ceil(scaled.denominator)
        } else {
            scaled.numerator / scaled.denominator
        })
        .map_err(|_| timing_overflow())
    }
}

/// Fixed presentation slots and an exact source endpoint, independent of UI microseconds.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct FixedFrameTimeline {
    duration: RationalTime,
    duration_us: u64,
    fps: u32,
    frames: u64,
}

impl FixedFrameTimeline {
    pub fn new(duration_us: u64, fps: u32) -> Result<Self, String> {
        Self::from_time_base(duration_us, 1, 1_000_000, fps)
    }

    /// Retain the container's native endpoint, including a partial final frame.
    pub fn from_time_base(
        ticks: u64,
        numerator: u32,
        denominator: u32,
        fps: u32,
    ) -> Result<Self, String> {
        if ticks == 0 || numerator == 0 || denominator == 0 || fps == 0 {
            return Err(
                "fixed frame timeline requires positive duration, time base and FPS".into(),
            );
        }
        let duration = RationalTime::new(
            u128::from(ticks) * u128::from(numerator),
            u128::from(denominator),
        );
        let microseconds = duration.scale(1_000_000, 1)?;
        // Match native container import's nearest rounding for the visible endpoint.
        let duration_us = u64::try_from(
            (microseconds.numerator + microseconds.denominator / 2) / microseconds.denominator,
        )
        .map_err(|_| timing_overflow())?;
        if duration_us == 0 {
            return Err("fixed frame timeline is shorter than one microsecond".into());
        }
        Ok(Self {
            duration,
            duration_us,
            fps,
            frames: duration.ticks(fps, true)?,
        })
    }

    pub fn duration_us(self) -> u64 {
        self.duration_us
    }

    pub fn frame_count(self) -> u64 {
        self.frames
    }

    pub fn boundary_us(self, frame: u64) -> u64 {
        if frame >= self.frames {
            self.duration_us
        } else {
            (u128::from(frame) * 1_000_000 / u128::from(self.fps)) as u64
        }
    }

    pub fn frame_at(self, at_us: u64) -> u64 {
        (((u128::from(at_us) + 1) * u128::from(self.fps) - 1) / 1_000_000)
            .min(u128::from(self.frames - 1)) as u64
    }

    fn exact_boundary(self, at_us: u64) -> RationalTime {
        if at_us == self.duration_us {
            return self.duration;
        }
        let frame = (u128::from(at_us) * u128::from(self.fps)).div_ceil(1_000_000);
        if frame < u128::from(self.frames)
            && frame * 1_000_000 / u128::from(self.fps) == u128::from(at_us)
        {
            RationalTime::new(frame, u128::from(self.fps))
        } else {
            // Arbitrary microsecond cuts retain their requested time; only actual
            // editor frame boundaries recover the exact presentation-grid position.
            RationalTime::new(u128::from(at_us), 1_000_000)
        }
    }

    pub fn retime(
        self,
        range: TrimRange,
        speed: PlaybackSpeed,
    ) -> Result<RetimedFrameTiming, String> {
        TrimRange::new(range.start_us, range.end_us, self.duration_us)?;
        RetimedFrameTiming::new(
            self.exact_boundary(range.end_us)
                .subtract(self.exact_boundary(range.start_us))?,
            speed,
            self.fps,
        )
    }
}

/// All output endpoints derive from one exact interval, with rounding only at clock consumers.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct RetimedFrameTiming {
    duration: RationalTime,
    fps: u32,
}

impl RetimedFrameTiming {
    fn new(duration: RationalTime, speed: PlaybackSpeed, fps: u32) -> Result<Self, String> {
        if fps == 0 {
            return Err("retimed frame rate must be positive".into());
        }
        Ok(Self {
            duration: duration.scale(100, u128::from(speed.percent()))?,
            fps,
        })
    }

    pub fn from_duration_us(
        duration_us: u64,
        speed: PlaybackSpeed,
        fps: u32,
    ) -> Result<Self, String> {
        Self::new(
            RationalTime::new(u128::from(duration_us), 1_000_000),
            speed,
            fps,
        )
    }

    pub fn duration_us(self) -> Result<u64, String> {
        self.duration.ticks(1_000_000, true)
    }

    pub fn frame_count(self) -> Result<u64, String> {
        self.duration.ticks(self.fps, true)
    }

    pub fn sample_count(self, rate: u32) -> Result<u64, String> {
        self.duration.ticks(rate, false)
    }
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct TrimRange {
    pub start_us: u64,
    pub end_us: u64,
}

impl TrimRange {
    pub fn new(start_us: u64, end_us: u64, duration_us: u64) -> Result<Self, String> {
        if start_us >= end_us || end_us > duration_us {
            return Err("trim range must contain media and stay inside the source".into());
        }
        Ok(Self { start_us, end_us })
    }

    pub fn duration_us(self) -> u64 {
        self.end_us - self.start_us
    }

    pub fn sample_count(self, rate: u32) -> u64 {
        (u128::from(self.end_us) * u128::from(rate) / 1_000_000) as u64 - self.sample_offset(rate)
    }

    pub fn sample_offset(self, rate: u32) -> u64 {
        (u128::from(self.start_us) * u128::from(rate) / 1_000_000) as u64
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn playback_speed_accepts_only_the_editor_rates_and_maps_rational_time() {
        assert_eq!(PlaybackSpeed::default().percent(), 100);
        for percent in [25, 50, 75, 100, 125, 150, 175, 200, 300] {
            let speed = PlaybackSpeed::from_percent(percent).unwrap();
            assert_eq!(
                speed.source_elapsed_us(1_000_000),
                u64::from(percent) * 10_000
            );
            assert_eq!(
                speed.output_duration_us(1_000_001).unwrap(),
                (100_000_100u64).div_ceil(u64::from(percent))
            );
            assert_eq!(
                speed.source_time_at_frame(83, 83).unwrap(),
                u64::from(percent) * 10_000
            );
        }
        for percent in [0, 24, 26, 99, 101, 250, 301, u32::MAX] {
            assert!(PlaybackSpeed::from_percent(percent).is_err());
        }
        let slow = PlaybackSpeed::from_percent(25).unwrap();
        assert!(slow.output_duration_us(u64::MAX).is_err());
        assert_eq!(slow.output_duration_us(1).unwrap(), 4);
        assert_eq!(
            PlaybackSpeed::from_percent(300)
                .unwrap()
                .output_duration_us(1)
                .unwrap(),
            1
        );
        assert!(slow.source_time_at_frame(1, 0).is_err());
        assert!(
            PlaybackSpeed::from_percent(300)
                .unwrap()
                .source_time_at_frame(u64::MAX, 1)
                .is_err()
        );
    }

    #[test]
    fn half_open_range_rejects_empty_reversed_and_outside_media() {
        for (start, end) in [(0, 0), (10, 9), (0, 101)] {
            assert!(TrimRange::new(start, end, 100).is_err());
        }
        assert_eq!(TrimRange::new(0, 100, 100).unwrap().duration_us(), 100);
        assert_eq!(TrimRange::new(99, 100, 100).unwrap().duration_us(), 1);
    }

    #[test]
    fn output_frame_count_does_not_round_through_the_microsecond_clock() {
        let fast = PlaybackSpeed::from_percent(300).unwrap();
        assert_eq!(fast.output_duration_us(1_000_000).unwrap(), 333_334);
        assert_eq!(fast.output_frame_count(1_000_000, 60).unwrap(), 20);
        assert_eq!(
            PlaybackSpeed::from_percent(125)
                .unwrap()
                .output_frame_count(83_333, 60)
                .unwrap(),
            4
        );
        for percent in [25, 50, 75, 100, 125, 150, 175, 200, 300] {
            let speed = PlaybackSpeed::from_percent(percent).unwrap();
            assert_eq!(speed.output_frame_count(1, 60).unwrap(), 1);
            assert_eq!(speed.output_frame_count(0, 60).unwrap(), 0);
            assert_eq!(
                speed.output_frame_count(1_000_000, 60).unwrap(),
                6000u64.div_ceil(u64::from(percent))
            );
            assert!(speed.output_frame_count(1, 0).is_err());
        }
        assert!(
            PlaybackSpeed::from_percent(25)
                .unwrap()
                .output_frame_count(u64::MAX, u32::MAX)
                .is_err()
        );
    }

    #[test]
    fn frame_aligned_retiming_is_independent_of_microsecond_rounding_and_origin() {
        for fps in [24, 30, 60, 144, 1000] {
            let timeline = FixedFrameTimeline::from_time_base(129, 1, fps, fps).unwrap();
            for first in 0..7 {
                for count in [1, 2, 5] {
                    let range = TrimRange::new(
                        timeline.boundary_us(first),
                        timeline.boundary_us(first + count),
                        timeline.duration_us(),
                    )
                    .unwrap();
                    for percent in [25, 50, 75, 100, 125, 150, 175, 200, 300] {
                        let timing = timeline
                            .retime(range, PlaybackSpeed::from_percent(percent).unwrap())
                            .unwrap();
                        assert_eq!(
                            timing.frame_count().unwrap(),
                            (count * 100).div_ceil(u64::from(percent)),
                            "{fps} FPS [{first}, {}) at {percent}%",
                            first + count
                        );
                        assert_eq!(
                            timing.duration_us().unwrap(),
                            (count * 100 * 1_000_000).div_ceil(u64::from(fps) * u64::from(percent))
                        );
                        assert_eq!(
                            timing.sample_count(48_000).unwrap(),
                            count * 100 * 48_000 / (u64::from(fps) * u64::from(percent))
                        );
                    }
                }
            }
        }
    }

    #[test]
    fn native_endpoints_preserve_complete_and_partial_final_frame_intervals() {
        for fps in [60, 144] {
            let timeline = FixedFrameTimeline::from_time_base(7, 1, fps, fps).unwrap();
            let range = TrimRange::new(
                timeline.boundary_us(6),
                timeline.duration_us(),
                timeline.duration_us(),
            )
            .unwrap();
            let timing = timeline
                .retime(range, PlaybackSpeed::from_percent(25).unwrap())
                .unwrap();
            assert_eq!(timeline.frame_count(), 7);
            assert_eq!(timing.frame_count().unwrap(), 4);
        }
        let timeline = FixedFrameTimeline::new(305_000, 60).unwrap();
        assert_eq!(timeline.frame_count(), 19);
        let range = TrimRange::new(timeline.boundary_us(18), 305_000, 305_000).unwrap();
        let timing = timeline
            .retime(range, PlaybackSpeed::from_percent(25).unwrap())
            .unwrap();
        assert_eq!(timing.duration_us().unwrap(), 20_000);
        assert_eq!(timing.frame_count().unwrap(), 2);
        assert_eq!(timing.sample_count(48_000).unwrap(), 960);

        // Neither rounding the terminal microseconds nor treating the last frame
        // as a complete slot may change this native 1/3000-second final interval.
        let timeline = FixedFrameTimeline::from_time_base(1001, 1, 3000, 60).unwrap();
        assert_eq!(timeline.duration_us(), 333_667);
        let range = TrimRange::new(timeline.boundary_us(20), 333_667, 333_667).unwrap();
        let timing = timeline
            .retime(range, PlaybackSpeed::from_percent(25).unwrap())
            .unwrap();
        assert_eq!(timing.duration_us().unwrap(), 1334);
        assert_eq!(timing.frame_count().unwrap(), 1);
        assert_eq!(timing.sample_count(48_000).unwrap(), 64);
    }

    #[test]
    fn non_grid_cuts_keep_their_requested_times_and_large_timing_is_checked() {
        let timeline = FixedFrameTimeline::new(1_000_000, 60).unwrap();
        let range = TrimRange::new(16_665, 33_334, 1_000_000).unwrap();
        let timing = timeline
            .retime(range, PlaybackSpeed::from_percent(25).unwrap())
            .unwrap();
        assert_eq!(timing.duration_us().unwrap(), 66_676);
        assert_eq!(timing.frame_count().unwrap(), 5);
        assert_eq!(timing.sample_count(48_000).unwrap(), 3200);
        for (ticks, numerator, denominator, fps) in
            [(0, 1, 1, 60), (1, 0, 1, 60), (1, 1, 0, 60), (1, 1, 1, 0)]
        {
            assert!(
                FixedFrameTimeline::from_time_base(ticks, numerator, denominator, fps).is_err()
            );
        }
        assert!(FixedFrameTimeline::from_time_base(u64::MAX, u32::MAX, 1, 60).is_err());
        let timeline = FixedFrameTimeline::new(u64::MAX, 1000).unwrap();
        let range = TrimRange::new(0, u64::MAX, u64::MAX).unwrap();
        let timing = timeline
            .retime(range, PlaybackSpeed::from_percent(25).unwrap())
            .unwrap();
        assert!(timing.duration_us().is_err());
        assert!(
            timeline
                .retime(
                    TrimRange {
                        start_us: 2,
                        end_us: 1
                    },
                    PlaybackSpeed::default()
                )
                .is_err()
        );
    }

    #[test]
    fn audio_offsets_use_rational_media_time_without_overflow() {
        let range = TrimRange::new(1_234_567, 2_000_000, 2_000_000).unwrap();
        assert_eq!(range.sample_offset(48_000), 59_259);
        let range = TrimRange::new(u64::MAX / 2, u64::MAX, u64::MAX).unwrap();
        assert_eq!(range.sample_offset(1_000_000), range.start_us);
    }
}
