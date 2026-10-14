use crate::{DrawRect, MagnifierConnectionType, Point, rotated_rect_extents};

/// Geometry shared by document culling, scene damage and host rendering.
/// The leader is a separate path; these bounds cover the lens, source outline
/// and Gradient connection. Pixel antialias padding belongs to the host.
#[derive(Clone, Copy, Debug)]
pub struct MagnifierPaintGeometry {
    pub lens_center: Point<f64>,
    pub lens_width: f64,
    pub lens_height: f64,
    pub source_center: Point<f64>,
    pub source_width: f64,
    pub source_height: f64,
    pub rotation: f64,
    pub stroke_width: f64,
    pub stroke_visible: bool,
    pub source_outline: bool,
    pub connection_type: MagnifierConnectionType,
}

impl MagnifierPaintGeometry {
    pub fn bounds(self) -> Option<DrawRect> {
        let stroke_width = if self.stroke_visible && self.stroke_width.is_finite() {
            self.stroke_width.max(0.0)
        } else {
            0.0
        };
        let bounds = |center: Point<f64>, width: f64, height: f64, stroke: f64| {
            if !center.x.is_finite()
                || !center.y.is_finite()
                || !width.is_finite()
                || !height.is_finite()
                || !self.rotation.is_finite()
                || width < 0.0
                || height < 0.0
            {
                return None;
            }
            // All supported contours fit inside their rotated rectangle. Their
            // round-joined strokes also fit inside this padded rectangle.
            let (x, y) = rotated_rect_extents(width, height, self.rotation, stroke);
            let rect = DrawRect::new(center.x - x, center.y - y, center.x + x, center.y + y);
            [rect.min_x, rect.min_y, rect.max_x, rect.max_y]
                .iter()
                .all(|v| v.is_finite())
                .then_some(rect)
        };
        let mut result = bounds(
            self.lens_center,
            self.lens_width,
            self.lens_height,
            stroke_width,
        )?;
        let outline = self.source_outline && stroke_width > 0.0;
        if self.stroke_visible
            && (outline || self.connection_type == MagnifierConnectionType::Gradient)
            && let Some(source) = bounds(
                self.source_center,
                self.source_width,
                self.source_height,
                if outline { stroke_width } else { 0.0 },
            )
        {
            // A convex connection cannot extend outside the enclosing AABB
            // of its two bodies. Include the gap, even if both bodies are
            // outside the viewport; clip only after forming the union.
            result.min_x = result.min_x.min(source.min_x);
            result.min_y = result.min_y.min(source.min_y);
            result.max_x = result.max_x.max(source.max_x);
            result.max_y = result.max_y.max(source.max_y);
        }
        Some(result)
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn geometry() -> MagnifierPaintGeometry {
        MagnifierPaintGeometry {
            lens_center: Point::new(200.0, 100.0),
            lens_width: 80.0,
            lens_height: 60.0,
            source_center: Point::new(-200.0, -100.0),
            source_width: 40.0,
            source_height: 30.0,
            rotation: 0.0,
            stroke_width: 10.0,
            stroke_visible: true,
            source_outline: false,
            connection_type: MagnifierConnectionType::Gradient,
        }
    }

    #[test]
    fn paint_bounds_include_the_bridge_and_only_visible_source_decorations() {
        let mut value = geometry();
        assert_eq!(
            value.bounds(),
            Some(DrawRect::new(-220.0, -115.0, 245.0, 135.0))
        );
        value.source_outline = true;
        assert_eq!(
            value.bounds(),
            Some(DrawRect::new(-225.0, -120.0, 245.0, 135.0))
        );
        for connection in [
            MagnifierConnectionType::None,
            MagnifierConnectionType::LeaderLine,
        ] {
            value.connection_type = connection;
            assert_eq!(
                value.bounds(),
                Some(DrawRect::new(-225.0, -120.0, 245.0, 135.0))
            );
            value.source_outline = false;
            assert_eq!(
                value.bounds(),
                Some(DrawRect::new(155.0, 65.0, 245.0, 135.0))
            );
            value.source_outline = true;
        }
        value.connection_type = MagnifierConnectionType::Gradient;
        value.stroke_width = 0.0;
        assert_eq!(
            value.bounds(),
            Some(DrawRect::new(-220.0, -115.0, 240.0, 130.0))
        );
        value.stroke_visible = false;
        assert_eq!(
            value.bounds(),
            Some(DrawRect::new(160.0, 70.0, 240.0, 130.0))
        );
    }

    #[test]
    fn rotated_bounds_cover_both_contours_and_round_strokes() {
        for angle in [0.0, 0.37, 1.57, -2.3] {
            let value = MagnifierPaintGeometry {
                rotation: angle,
                source_outline: true,
                ..geometry()
            };
            let bounds = value.bounds().unwrap();
            let (sin, cos) = angle.sin_cos();
            for (center, width, height) in [
                (value.lens_center, value.lens_width, value.lens_height),
                (value.source_center, value.source_width, value.source_height),
            ] {
                for x in [-width / 2.0, width / 2.0] {
                    for y in [-height / 2.0, height / 2.0] {
                        for step in 0..32 {
                            let direction = step as f64 * std::f64::consts::TAU / 32.0;
                            let px = center.x + x * cos - y * sin + 5.0 * direction.cos();
                            let py = center.y + x * sin + y * cos + 5.0 * direction.sin();
                            assert!(
                                px >= bounds.min_x - 1e-9
                                    && px <= bounds.max_x + 1e-9
                                    && py >= bounds.min_y - 1e-9
                                    && py <= bounds.max_y + 1e-9
                            );
                        }
                    }
                }
            }
        }
    }

    #[test]
    fn invalid_geometry_never_publishes_nonfinite_bounds() {
        for invalid in [f64::NAN, f64::INFINITY, f64::NEG_INFINITY] {
            assert!(
                MagnifierPaintGeometry {
                    lens_width: invalid,
                    ..geometry()
                }
                .bounds()
                .is_none()
            );
            assert!(
                MagnifierPaintGeometry {
                    rotation: invalid,
                    ..geometry()
                }
                .bounds()
                .is_none()
            );
            let value = MagnifierPaintGeometry {
                source_center: Point::new(invalid, 0.0),
                ..geometry()
            };
            assert_eq!(
                value.bounds(),
                Some(DrawRect::new(155.0, 65.0, 245.0, 135.0))
            );
        }
    }
}
