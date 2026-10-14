use super::*;
use snow_draw_engine_display::SceneDisplayItem;
use snow_draw_engine_editor::SpeechBubbleTextMeasurement;

fn directly_commit_label(
    engine: &mut Engine,
    viewport: ViewportId,
    owner: ElementId,
    edit_existing: bool,
) -> ElementId {
    if edit_existing {
        engine
            .set_active_text_draft_presentation_with_viewport_changes(
                viewport,
                ActiveTextDraftPresentation {
                    target: ActiveTextDraftTarget::NewSpeechBubble(owner),
                    revision: 1,
                    text: TextData {
                        center: body(engine, owner).body.center,
                        text: "initial bubble text".into(),
                        ..Default::default()
                    },
                },
            )
            .unwrap();
    }
    let label = bind(engine, viewport, owner, 30.0, 20.0, 3.0);
    if edit_existing {
        let (info, style) = engine
            .speech_bubble_text_target(viewport, None)
            .unwrap()
            .unwrap();
        let mut draft = TextDraftCommit::new(
            TextCommitTarget::Existing(label),
            info.center,
            "edited bubble text",
            engine.model.text(label).unwrap().layout,
            style,
            true,
            false,
        );
        draft.minimum_grapheme_width = 20.0;
        draft.text_ink_reserve = 3.0;
        engine
            .commit_text_draft_with_viewport_changes(viewport, draft)
            .unwrap();
    }
    assert!(engine.editor.active_text_draft_presentation().is_none());
    label
}

#[test]
fn speech_bubble_direct_text_commits_allow_later_move_previews() {
    for edit_existing in [false, true] {
        let (mut engine, viewport, owner) = create(HighlightShape::Rectangle, 180.0, 90.0);
        let label = directly_commit_label(&mut engine, viewport, owner, edit_existing);
        engine
            .set_viewport_active_tool(viewport, ActiveTool::Select)
            .unwrap();
        let original = body(&engine, owner);
        let start = original.body.center;
        let target = Point::new(start.x - 35.0, start.y - 25.0);
        pointer(&mut engine, viewport, PointerEventType::Down, start);
        pointer(&mut engine, viewport, PointerEventType::Move, target);

        let patch = engine.acquire_patch(viewport, None).unwrap();
        let mut displayed_body = None;
        let mut displayed_text = None;
        for item in patch.scene.ops.iter().flat_map(|op| &op.insert_items) {
            match item {
                SceneDisplayItem::SpeechBubble(item) if item.body.id.index == owner.index => {
                    displayed_body = Some(Point::new(item.body.center_x, item.body.center_y));
                }
                SceneDisplayItem::Text(item) if item.id.index == label.index => {
                    displayed_text = Some(Point::new(item.center_x, item.center_y));
                }
                _ => {}
            }
        }
        assert_eq!(
            displayed_body,
            Some(target),
            "existing label: {edit_existing}"
        );
        assert_eq!(displayed_text, Some(target));
        assert_eq!(body(&engine, owner), original);
        pointer(&mut engine, viewport, PointerEventType::Up, target);
        assert_eq!(body(&engine, owner).body.center, target);
    }
}

#[test]
fn speech_bubble_direct_text_commits_allow_passive_remeasurement() {
    for edit_existing in [false, true] {
        let (mut engine, viewport, owner) = create(HighlightShape::Rectangle, 180.0, 90.0);
        let label = directly_commit_label(&mut engine, viewport, owner, edit_existing);
        let original = body(&engine, owner);
        engine.invalidate_arrow_text_measurements(viewport).unwrap();
        let request = engine
            .speech_bubble_text_layout_requests(viewport)
            .unwrap()
            .into_iter()
            .find(|request| request.owner_id == owner)
            .unwrap();
        let constraints = speech_bubble_text_constraints(request.body, 20.0, 3.0, 160.0).unwrap();
        engine
            .apply_speech_bubble_text_layouts(
                viewport,
                &[(
                    label,
                    SpeechBubbleTextMeasurement {
                        key: request.key,
                        size: TextLayoutSize::with_content(
                            constraints.content_width,
                            160.0,
                            constraints.content_width - 1.0,
                            160.0,
                        ),
                        minimum_grapheme_width: 20.0,
                        text_ink_reserve: 3.0,
                    },
                )],
            )
            .unwrap();
        let patch = engine.acquire_patch(viewport, None).unwrap();
        let displayed = patch
            .scene
            .ops
            .iter()
            .flat_map(|op| &op.insert_items)
            .find_map(|item| match item {
                SceneDisplayItem::SpeechBubble(item) if item.body.id.index == owner.index => {
                    Some(&item.body)
                }
                _ => None,
            })
            .unwrap();
        assert_eq!(displayed.height, constraints.body_height);
        assert!(displayed.height > original.body.height);
        assert_eq!(
            displayed.center_y,
            original.body.center.y + (constraints.body_height - original.body.height) / 2.0
        );
        assert_eq!(body(&engine, owner), original);
    }
}
