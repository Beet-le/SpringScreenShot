use super::{Config, DetectorResizePolicy, TextDetectionProcessing};
use simd_paddle_ocr_rs::{
    OcrModelSet,
    cpu::CpuContext,
    detector::DetResizePolicy,
    image::{ImageResult, ImageView, PixelFormat},
    ocr::Ocr,
    recognizer::{RecognitionScoreMode, RecognizerOptions},
};
use snow_memory::RasterBuffer;
use std::{path::Path, sync::Arc};

pub(super) struct InputImage {
    pub width: usize,
    pub height: usize,
    pub pixels: RasterBuffer,
}

impl InputImage {
    pub fn view(&self) -> ImageResult<ImageView<'_>> {
        ImageView::new(
            self.pixels.as_slice(),
            self.width,
            self.height,
            0,
            PixelFormat::Bgr24,
        )
    }
}

#[derive(Debug, PartialEq)]
pub(super) struct OcrLine {
    pub text: String,
    pub score: f32,
    pub points: [[f32; 2]; 4],
}

fn accepted_line(text: &str, score: f32, points: [[f32; 2]; 4]) -> Option<OcrLine> {
    (!text.trim().is_empty() && score.is_finite() && score >= 0.5).then(|| OcrLine {
        text: text.to_owned(),
        score,
        points,
    })
}

fn source_quad(mut points: [[f32; 2]; 4], width: usize, height: usize) -> [[f32; 2]; 4] {
    // The detector's enclosing rotated rectangle can extend beyond a clipped
    // contour. Preserve Snow Shot's source-image bounds at the IPC boundary.
    for point in &mut points {
        point[0] = point[0].clamp(0., width as f32);
        point[1] = point[1].clamp(0., height as f32);
    }
    points
}

fn detector_limit(policy: DetectorResizePolicy, width: usize, height: usize) -> usize {
    match policy {
        DetectorResizePolicy::Min => 736,
        DetectorResizePolicy::Max if width.max(height) < 960 => 960,
        DetectorResizePolicy::Max if width.max(height) < 1500 => 1500,
        DetectorResizePolicy::Max => 2000,
    }
}

pub(super) struct Engine {
    request: Ocr,
    policy: DetectorResizePolicy,
}

impl Engine {
    pub fn run(&mut self, input: &InputImage) -> ImageResult<Vec<OcrLine>> {
        self.request
            .extractor_mut()
            .detector_mut()
            .set_resize_options(
                match self.policy {
                    DetectorResizePolicy::Max => DetResizePolicy::Max,
                    DetectorResizePolicy::Min => DetResizePolicy::Min,
                },
                detector_limit(self.policy, input.width, input.height),
            )?;
        // The request owns reusable scratch. Copy only the accepted output before
        // it crosses the completion channel; never copy the input raster again.
        Ok(self
            .request
            .run(input.view()?, false)?
            .iter()
            .filter_map(|line| {
                accepted_line(
                    line.text,
                    line.recognition_score,
                    source_quad(line.detection.points, input.width, input.height),
                )
            })
            .collect())
    }
}

pub(super) fn make_engine(config: &Config, thread_budget: usize) -> ImageResult<Engine> {
    make_engine_for_models(
        &config.detector_model,
        &config.recognizer_model,
        &config.dictionary,
        thread_budget,
        config.detector_resize_policy,
        config.text_detection_processing,
    )
}

pub(super) fn make_engine_for_models(
    detector: &Path,
    recognizer: &Path,
    dictionary: &Path,
    thread_budget: usize,
    policy: DetectorResizePolicy,
    processing: TextDetectionProcessing,
) -> ImageResult<Engine> {
    let models = OcrModelSet::from_paths(detector, recognizer, dictionary, None)?;
    let request = models
        .builder()
        .cpu_context(Arc::new(CpuContext::new(thread_budget.max(1))?))
        .direction_classification(false)
        .recognizer_options(RecognizerOptions {
            min_width: processing.recognition_width(),
            score_mode: RecognitionScoreMode::Probability,
            ..Default::default()
        })
        .build()?;
    Ok(Engine { request, policy })
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn result_filter_keeps_geometry_and_owned_utf8_text() {
        let points = [[1., 2.], [3., 2.], [3., 4.], [1., 4.]];
        let mut text = String::from("文字 Snow");
        let line = accepted_line(&text, 0.5, points).unwrap();
        text.clear();
        assert_eq!(line.text, "文字 Snow");
        assert_eq!(line.points, points);
        assert!(accepted_line(" ", 1., points).is_none());
        assert!(accepted_line("text", 0.499, points).is_none());
        assert!(accepted_line("text", f32::NAN, points).is_none());
    }

    #[test]
    fn enclosing_quad_is_clipped_in_original_image_coordinates() {
        let quad = [[5., 4.], [133.72, 13.05], [130., 66.], [-1.28, 73.]];
        assert_eq!(
            source_quad(quad, 130, 70),
            [[5., 4.], [130., 13.05], [130., 66.], [0., 70.]]
        );
    }

    #[test]
    fn resize_limits_preserve_policy_boundaries() {
        for (width, expected) in [
            (959, 960),
            (960, 1500),
            (1499, 1500),
            (1500, 2000),
            (3840, 2000),
        ] {
            assert_eq!(
                detector_limit(DetectorResizePolicy::Max, width, 100),
                expected
            );
            assert_eq!(detector_limit(DetectorResizePolicy::Min, width, 100), 736);
        }
        assert_eq!(TextDetectionProcessing::default().recognition_width(), 320);
        assert_eq!(TextDetectionProcessing::SpeedFirst.recognition_width(), 48);
    }
}
