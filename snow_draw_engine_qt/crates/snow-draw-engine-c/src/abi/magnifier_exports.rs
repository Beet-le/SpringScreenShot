use crate::abi::handles::ffi_value;
use crate::abi::types::*;
use snow_draw_engine_core::{MagnifierPaintGeometry, Point};

/// Shared document-space bounds for the lens, source outline and Gradient
/// connection. The host unions these with its existing leader-path bounds.
/// # Safety
/// `item` may be null; non-null pointers must reference a readable display item.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn snow_scene_magnifier_body_paint_bounds(
    item: *const SnowSceneDisplayItem,
) -> SnowMagnifierPaintBounds {
    ffi_value(SnowMagnifierPaintBounds::default(), || {
        let Some(item) = (unsafe { item.as_ref() }) else {
            return SnowMagnifierPaintBounds::default();
        };
        if item.kind != SnowSceneDisplayItemKind::Magnifier {
            return SnowMagnifierPaintBounds::default();
        }
        let geometry = MagnifierPaintGeometry {
            lens_center: Point::new(item.center_x, item.center_y),
            lens_width: item.width,
            lens_height: item.height,
            source_center: Point::new(
                item.magnifier.source_center_x,
                item.magnifier.source_center_y,
            ),
            source_width: item.magnifier.source_width,
            source_height: item.magnifier.source_height,
            rotation: item.rotation,
            stroke_width: item.stroke_width,
            stroke_visible: item.stroke.a != 0,
            source_outline: item.magnifier.source_outline != 0,
            connection_type: item.magnifier.connection_type.into(),
        };
        let bounds = geometry.bounds().unwrap_or_default();
        SnowMagnifierPaintBounds {
            min_x: bounds.min_x,
            min_y: bounds.min_y,
            max_x: bounds.max_x,
            max_y: bounds.max_y,
        }
    })
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::abi::convert::snow_scene_display_item_from_rust;
    use snow_draw_engine::{MagnifierDisplayItem, RectangleDisplayItem, SceneDisplayItem};

    #[test]
    fn magnifier_host_bounds_match_scene_geometry_without_changing_the_display_abi() {
        let mut item = MagnifierDisplayItem {
            lens: RectangleDisplayItem {
                center_x: 200.0,
                center_y: -150.0,
                width: 120.0,
                height: 80.0,
                rotation: 0.63,
                stroke_width: 12.0,
                stroke: snow_draw_engine_core::ColorRgba8 {
                    a: 255,
                    ..Default::default()
                },
                ..Default::default()
            },
            source_center: Point::new(-180.0, 120.0),
            source_width: 60.0,
            source_height: 40.0,
            factor: 2.0,
            connection_type: snow_draw_engine_core::MagnifierConnectionType::Gradient,
            source_outline: true,
            leader: None,
        };
        for connection in [
            snow_draw_engine_core::MagnifierConnectionType::None,
            snow_draw_engine_core::MagnifierConnectionType::LeaderLine,
            snow_draw_engine_core::MagnifierConnectionType::Gradient,
        ] {
            for outline in [false, true] {
                item.connection_type = connection;
                item.source_outline = outline;
                let converted = snow_scene_display_item_from_rust(
                    &SceneDisplayItem::Magnifier(item.clone()),
                    false,
                    false,
                );
                let actual = unsafe { snow_scene_magnifier_body_paint_bounds(&converted.view) };
                let expected = item.paint_geometry().bounds().unwrap();
                assert_eq!(
                    [actual.min_x, actual.min_y, actual.max_x, actual.max_y],
                    [
                        expected.min_x,
                        expected.min_y,
                        expected.max_x,
                        expected.max_y
                    ]
                );
            }
        }
        assert_eq!(
            unsafe { snow_scene_magnifier_body_paint_bounds(std::ptr::null()) },
            SnowMagnifierPaintBounds::default()
        );
        assert_eq!(
            unsafe { snow_scene_magnifier_body_paint_bounds(&SnowSceneDisplayItem::default()) },
            SnowMagnifierPaintBounds::default()
        );
    }
}
