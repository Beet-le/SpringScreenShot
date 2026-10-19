//! Portable webcam styling. Coordinates follow the other fixed-pixel recording effects.
use serde::{Deserialize, Serialize};

#[derive(Clone, Copy, Debug, Default, PartialEq, Eq, Serialize, Deserialize)]
pub enum WebcamShape {
    #[default]
    Square,
    Circle,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq, Serialize, Deserialize)]
pub struct WebcamStyle {
    pub size: u32,
    pub shape: WebcamShape,
}

impl Default for WebcamStyle {
    fn default() -> Self {
        Self {
            size: 150,
            shape: WebcamShape::Square,
        }
    }
}

impl WebcamStyle {
    pub fn validate(self) -> Result<(), String> {
        if !(50..=500).contains(&self.size) {
            return Err("webcam size must be between 50 and 500 pixels".into());
        }
        Ok(())
    }

    /// Reduce the inset before reducing the image, retaining the complete square/circle.
    pub fn placement(self, output: (u32, u32)) -> (u32, u32, u32) {
        let side = self.size.min(output.0).min(output.1);
        let x = 48.min(output.0.saturating_sub(side));
        let bottom = 48.min(output.1.saturating_sub(side));
        (x, output.1.saturating_sub(bottom + side), side)
    }
}

#[derive(Clone, Debug, Serialize, Deserialize)]
pub struct WebcamMetadata {
    pub style: WebcamStyle,
    pub fps: u32,
    pub duration_ms: u64,
    /// State changes use source time, so trimming and speed changes preserve unavailable gaps.
    pub visibility: Vec<(u64, bool)>,
}
impl WebcamMetadata {
    pub fn validate(&self) -> Result<(), String> {
        self.style.validate()?;
        if !(1..=30).contains(&self.fps)
            || self.duration_ms == 0
            || self.visibility.len() > 100_000
            || self
                .visibility
                .iter()
                .any(|(at, _)| *at >= self.duration_ms)
            || self.visibility.windows(2).any(|v| v[0].0 >= v[1].0)
        {
            return Err("invalid webcam timeline".into());
        }
        Ok(())
    }
    pub fn visible_at(&self, at: u64) -> bool {
        let count = self.visibility.partition_point(|(time, _)| *time <= at);
        count != 0 && self.visibility[count - 1].1
    }
}

const MAGIC: &[u8; 9] = b"SNOWCAM\0\x01";
pub fn write_metadata(path: &std::path::Path, metadata: &WebcamMetadata) -> crate::Result<()> {
    use std::io::Write;
    metadata
        .validate()
        .map_err(crate::RecordingModelError::Decode)?;
    let mut file = std::io::BufWriter::new(std::fs::File::create(path)?);
    file.write_all(MAGIC)?;
    bincode::serialize_into(&mut file, metadata)
        .map_err(|e| crate::RecordingModelError::Decode(e.to_string()))?;
    file.flush()?;
    Ok(())
}
pub fn read_metadata(mut input: impl std::io::Read) -> crate::Result<WebcamMetadata> {
    use bincode::Options;
    let mut magic = [0; MAGIC.len()];
    input.read_exact(&mut magic)?;
    if &magic != MAGIC {
        return Err(crate::RecordingModelError::Decode(
            "unsupported webcam metadata".into(),
        ));
    }
    let metadata: WebcamMetadata = bincode::DefaultOptions::new()
        .with_fixint_encoding()
        .with_limit(4 * 1024 * 1024)
        .deserialize_from(input)
        .map_err(|e| crate::RecordingModelError::Decode(e.to_string()))?;
    metadata
        .validate()
        .map_err(crate::RecordingModelError::Decode)?;
    Ok(metadata)
}

#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn webcam_metadata_is_versioned_validated_and_preserves_visibility_boundaries() {
        let style = WebcamStyle::default();
        assert_eq!(
            style,
            WebcamStyle {
                size: 150,
                shape: WebcamShape::Square
            }
        );
        assert_eq!(style.placement((800, 600)), (48, 402, 150));
        assert_eq!(style.placement((160, 170)), (10, 0, 150));
        assert_eq!(style.placement((20, 30)), (0, 0, 20));
        let mut metadata = WebcamMetadata {
            style,
            fps: 30,
            duration_ms: 1000,
            visibility: vec![(100, true), (400, false), (800, true)],
        };
        let mut bytes = MAGIC.to_vec();
        bincode::serialize_into(&mut bytes, &metadata).unwrap();
        let restored = read_metadata(bytes.as_slice()).unwrap();
        for (at, expected) in [
            (0, false),
            (99, false),
            (100, true),
            (399, true),
            (400, false),
            (799, false),
            (800, true),
        ] {
            assert_eq!(restored.visible_at(at), expected);
        }
        bytes[8] = 2;
        assert!(read_metadata(bytes.as_slice()).is_err());
        metadata.visibility.push((800, false));
        assert!(metadata.validate().is_err());
        metadata.visibility = vec![(1000, true)];
        assert!(metadata.validate().is_err());
        metadata.visibility.clear();
        metadata.style.size = 501;
        assert!(metadata.validate().is_err());
    }
}
