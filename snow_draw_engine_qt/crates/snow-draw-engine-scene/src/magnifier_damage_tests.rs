use super::*;
use snow_draw_engine_core::{Camera, MagnifierConnectionType, SurfaceSize};
use snow_draw_engine_display::{DisplayFilterType, FilterDisplayItem, FilterRenderSpec};
use snow_draw_engine_document::MagnifierData;

fn frame() -> FrameView {
    FrameView {
        surface: SurfaceSize {
            width: 1000,
            height: 1000,
        },
        camera: Camera {
            center: Point::default(),
            zoom: 1.0,
        },
        clear_color: ColorRgba8::default(),
    }
}

fn item(value: MagnifierData) -> SceneDisplayItem {
    item_conversions::scene_item_from_magnifier(
        ElementId {
            index: 1,
            generation: 1,
        },
        value,
    )
}

#[test]
fn magnifier_gradient_damage_survives_both_bodies_leaving_the_viewport() {
    let value = MagnifierData {
        source_center: Point::new(-700.0, 0.0),
        magnified_center: Point::new(700.0, 0.0),
        width: 80.0,
        height: 60.0,
        source_outline: false,
        ..Default::default()
    };
    let before = item(value);
    let after = item(MagnifierData {
        connection_type: MagnifierConnectionType::None,
        ..value
    });
    let (patch, _) = build_scene_layer_patch(&[before], 1, &[after], frame(), false, false);
    assert!(!patch.reset);
    assert_eq!(patch.dirty_regions.len(), 1);
    let bounds = patch.dirty_regions[0];
    assert_eq!(bounds.min_x, 0.0);
    assert_eq!(bounds.max_x, 1000.0);
    assert!(bounds.min_y <= 439.0 && bounds.max_y >= 561.0);
    assert!(
        bounds.min_y > 0.0 && bounds.max_y < 1000.0,
        "keep damage local to the connection"
    );
}

#[test]
fn magnifier_decorations_propagate_damage_into_later_filters() {
    let value = MagnifierData {
        source_center: Point::new(-200.0, 0.0),
        magnified_center: Point::new(200.0, 0.0),
        width: 40.0,
        height: 30.0,
        ..Default::default()
    };
    let before = item(value);
    let after = item(MagnifierData {
        connection_type: MagnifierConnectionType::None,
        source_outline: false,
        ..value
    });
    let filter = SceneDisplayItem::Filter(FilterDisplayItem {
        id: DisplayItemId {
            index: 2,
            generation: 1,
        },
        center_x: -200.0,
        center_y: 0.0,
        width: 120.0,
        height: 120.0,
        filter: FilterRenderSpec::resolve(DisplayFilterType::GaussianBlur, 1.0),
        opacity: 1.0,
        ..Default::default()
    });
    let base = scene_display_item_bounds(&before, frame()).unwrap();
    let affected =
        dirty_region_through_filters(&[before.clone(), filter.clone()], 0, base, frame());
    assert!(
        affected.min_x < base.min_x,
        "blur extends beyond the source decoration"
    );
    let (patch, _) = build_scene_layer_patch(
        &[before, filter.clone()],
        1,
        &[after, filter],
        frame(),
        false,
        false,
    );
    assert!(
        patch
            .dirty_regions
            .iter()
            .any(|bounds| bounds.min_x <= affected.min_x
                && bounds.min_y <= affected.min_y
                && bounds.max_x >= affected.max_x
                && bounds.max_y >= affected.max_y)
    );
}
