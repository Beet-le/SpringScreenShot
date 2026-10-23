//! One mask and placement implementation for preview, live compositors, and deferred replay.
use crate::surface::Surface;
use snow_recording_model::{WebcamShape, WebcamStyle};
use snow_webcam::Frame;

pub struct WebcamImage<'a> {
    pub rect: (u32, u32, u32),
    pub pixels: &'a [u8],
    pub generation: u64,
}

#[derive(Default)]
pub struct WebcamOverlay {
    key: Option<(u64, u32, WebcamShape)>,
    rect: (u32, u32, u32),
    mask: Vec<u8>,
    mask_key: Option<(u32, WebcamShape)>,
    pixels: Vec<u8>,
    generation: u64,
}

impl WebcamOverlay {
    pub fn prepare(
        &mut self,
        frame: Option<&Frame>,
        style: WebcamStyle,
        output: (u32, u32),
    ) -> bool {
        let rect = style.placement(output);
        let moved = self.rect != rect;
        self.rect = rect;
        let next = frame.map(|frame| (frame.revision, rect.2, style.shape));
        if next == self.key {
            return moved && self.visible();
        }
        self.key = next;
        self.generation = self.generation.wrapping_add(1);
        self.pixels.clear();
        let Some(frame) = frame else {
            return true;
        };
        let size = self.rect.2;
        if size == 0 {
            return true;
        }
        if self.mask_key != Some((size, style.shape)) {
            self.mask_key = Some((size, style.shape));
            self.mask.resize(size as usize * size as usize, 255);
            let radius = size as f32 / 2.0;
            for y in 0..size {
                for x in 0..size {
                    self.mask[(y * size + x) as usize] = if style.shape == WebcamShape::Circle {
                        let dx = x as f32 + 0.5 - radius;
                        let dy = y as f32 + 0.5 - radius;
                        ((radius + 0.5 - (dx * dx + dy * dy).sqrt()).clamp(0.0, 1.0) * 255.0)
                            .round() as u8
                    } else {
                        255
                    };
                }
            }
        }
        self.pixels.resize(size as usize * size as usize * 4, 0);
        for y in 0..size {
            for x in 0..size {
                let sx = u64::from(x) * u64::from(frame.size) / u64::from(size);
                let sy = u64::from(y) * u64::from(frame.size) / u64::from(size);
                let from = (sy as usize * frame.size as usize + sx as usize) * 4;
                let to = (y as usize * size as usize + x as usize) * 4;
                let alpha = self.mask[to / 4];
                for channel in 0..3 {
                    self.pixels[to + channel] =
                        ((u32::from(frame.rgba[from + channel]) * u32::from(alpha) + 127) / 255)
                            as u8;
                }
                self.pixels[to + 3] = alpha;
            }
        }
        true
    }

    /// Premultiplied RGBA in a compact square, shared by the native compositors.
    pub fn image(&self) -> Option<WebcamImage<'_>> {
        self.visible().then_some(WebcamImage {
            rect: self.rect,
            pixels: &self.pixels,
            generation: self.generation,
        })
    }

    pub fn visible(&self) -> bool {
        !self.pixels.is_empty()
    }
    pub fn draw_to<S: Surface>(&self, surface: &mut S) {
        if self.pixels.is_empty() {
            return;
        }
        let (x, y, size) = self.rect;
        for row in 0..size {
            surface.span(x, y + row, size, |target, offset| {
                let begin = (row as usize * size as usize + offset) * 4;
                for (to, from) in target
                    .chunks_exact_mut(4)
                    .zip(self.pixels[begin..].chunks_exact(4))
                {
                    let inverse = 255 - u32::from(from[3]);
                    for channel in 0..3 {
                        to[channel] = (u32::from(from[channel])
                            + (u32::from(to[channel]) * inverse + 127) / 255)
                            .min(255) as u8;
                    }
                    to[3] = if S::OPAQUE {
                        255
                    } else {
                        (u32::from(from[3]) + (u32::from(to[3]) * inverse + 127) / 255).min(255)
                            as u8
                    };
                }
            });
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::surface::{RgbaSurface, TileSurface};
    use std::{sync::Arc, time::Instant};
    #[test]
    fn webcam_movement_invalidates_placement_without_a_new_camera_frame() {
        let frame = Frame {
            revision: 1,
            at: Instant::now(),
            size: 50,
            rgba: Arc::new([220, 40, 10, 255].repeat(50 * 50)),
        };
        for shape in [WebcamShape::Square, WebcamShape::Circle] {
            let mut overlay = WebcamOverlay::default();
            let style = WebcamStyle {
                size: 50,
                shape,
                ..Default::default()
            };
            overlay.prepare(Some(&frame), style, (800, 600));
            let old = overlay.image().unwrap().rect;
            let pixels = overlay.image().unwrap().pixels.as_ptr();
            let generation = overlay.image().unwrap().generation;
            let moved = WebcamStyle {
                offset_left: 300,
                offset_bottom: 200,
                ..style
            };
            assert!(overlay.prepare(Some(&frame), moved, (800, 600)));
            assert_ne!(old, overlay.image().unwrap().rect);
            assert_eq!(overlay.image().unwrap().rect, (300, 350, 50));
            assert_eq!(overlay.image().unwrap().pixels.as_ptr(), pixels);
            assert_eq!(
                overlay.image().unwrap().generation,
                generation,
                "movement reuses the rasterized image and native texture"
            );
            assert!(!overlay.prepare(Some(&frame), moved, (800, 600)));
            let mut surface = TileSurface::new((800, 600));
            overlay.draw_to(&mut surface);
            assert!(overlay.prepare(None, moved, (800, 600)));
            assert!(!overlay.visible());
        }
    }
    #[test]
    fn webcam_placement_masks_layering_and_stale_clearing_agree_for_all_surfaces() {
        let frame = Frame {
            revision: 1,
            at: Instant::now(),
            size: 150,
            rgba: Arc::new([220, 40, 10, 255].repeat(150 * 150)),
        };
        for size in [50, 150, 500] {
            for shape in [WebcamShape::Square, WebcamShape::Circle] {
                for output in [(800, 600), (160, 170), (20, 30)] {
                    let style = WebcamStyle {
                        size,
                        shape,
                        ..Default::default()
                    };
                    let (x, y, side) = style.placement(output);
                    assert!(x + side <= output.0 && y + side <= output.1);
                    let mut overlay = WebcamOverlay::default();
                    assert!(overlay.prepare(Some(&frame), style, output));
                    assert!(!overlay.prepare(Some(&frame), style, output));
                    let mut rgba = [0, 0, 0, 255].repeat((output.0 * output.1) as usize);
                    overlay.draw_to(&mut RgbaSurface {
                        pixels: &mut rgba,
                        dimensions: output,
                    });
                    let center = (((y + side / 2) * output.0 + x + side / 2) * 4) as usize;
                    assert_eq!(&rgba[center..center + 4], &[220, 40, 10, 255]);
                    let corner = ((y * output.0 + x) * 4) as usize;
                    assert_eq!(
                        rgba[corner],
                        if shape == WebcamShape::Square { 220 } else { 0 }
                    );
                    let mut tiles = TileSurface::new(output);
                    overlay.draw_to(&mut tiles);
                    assert!(tiles.allocated_bytes() <= 36 * 128 * 128 * 4);
                    if shape == WebcamShape::Circle {
                        assert!(overlay.mask.iter().any(|a| *a > 0 && *a < 255));
                    }
                    assert!(overlay.prepare(None, style, output));
                    assert!(!overlay.visible());
                    assert!(!overlay.prepare(None, style, output));
                }
            }
        }
    }
}
