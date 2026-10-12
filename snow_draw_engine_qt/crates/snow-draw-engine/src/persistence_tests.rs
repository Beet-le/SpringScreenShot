use serde_json::Value;

use crate::{
    Engine, ErrorCode, Point, RuntimeConfig, TextCommitTarget, TextDraftCommit, TextLayoutSize,
};

fn remove_speech_bubble_fields(value: &mut Value) {
    match value {
        Value::Object(fields) => {
            fields.remove("speechBubble");
            fields.remove("text_ink_reserve");
            for child in fields.values_mut() {
                remove_speech_bubble_fields(child);
            }
        }
        Value::Array(items) => {
            for child in items {
                remove_speech_bubble_fields(child);
            }
        }
        _ => {}
    }
}

fn text_history_fixture() -> Engine {
    let mut config = RuntimeConfig::default();
    config.style_defaults.editor.rectangle.stroke_width = 5.0;
    let mut engine = Engine::new(config);
    let viewport = engine.create_viewport(Default::default()).unwrap();
    engine
        .create_text_with_viewport_changes(
            viewport,
            Point::new(12.375, -23.125),
            "original text",
            TextLayoutSize::new(130.25, 36.5),
        )
        .unwrap();
    let first = engine.model.paint_order()[0];
    let info = engine.text_element_info(first).unwrap();
    let style = engine
        .viewport_style_toolbar_state(viewport)
        .unwrap()
        .text_style;
    engine
        .commit_text_draft_with_viewport_changes(
            viewport,
            TextDraftCommit::new(
                TextCommitTarget::Existing(first),
                info.center,
                "edited text",
                TextLayoutSize::new(info.width, info.height),
                style,
                true,
                false,
            ),
        )
        .unwrap();
    engine
        .create_text_with_viewport_changes(
            viewport,
            Point::new(240.625, 75.125),
            "second text",
            TextLayoutSize::new(100.5, 36.5),
        )
        .unwrap();
    let second = engine.model.paint_order()[1];
    for id in [first, second] {
        engine
            .select_element_with_viewport_changes(viewport, id)
            .unwrap();
        engine
            .delete_selected_with_viewport_changes(viewport)
            .unwrap();
    }
    engine.undo().unwrap();
    engine
}

#[test]
fn legacy_speech_bubble_persistence_migrates_document_editor_and_both_history_stacks() {
    let source = text_history_fixture();
    for history_only in [false, true] {
        let bytes = if history_only {
            source.serialize_document_history()
        } else {
            source.serialize_document_session()
        }
        .unwrap();
        let mut legacy: Value = serde_json::from_slice(&bytes).unwrap();
        remove_speech_bubble_fields(&mut legacy);
        let last_legacy_version = if history_only { 8 } else { 12 };
        for version in 1..=last_legacy_version {
            legacy["schemaVersion"] = version.into();
            let bytes = serde_json::to_vec(&legacy).unwrap();
            let mut restored = if history_only {
                Engine::from_serialized_document_history_with_config(
                    &bytes,
                    RuntimeConfig::default(),
                )
            } else {
                Engine::from_serialized_document_session_with_config(
                    &bytes,
                    RuntimeConfig::default(),
                )
            }
            .unwrap();
            assert_eq!(restored.model.document(), source.model.document());
            // Selection geometry is a runtime cache; history persists its IDs.
            assert_eq!(
                serde_json::to_value(&restored.history).unwrap(),
                serde_json::to_value(&source.history).unwrap()
            );
            if !history_only {
                let saved: Value =
                    serde_json::from_slice(&restored.serialize_document_session().unwrap())
                        .unwrap();
                assert_eq!(
                    saved["editor"]["speechBubble"],
                    legacy["editor"]["rectangle"]
                );
                assert_eq!(saved["editor"]["text"]["text_ink_reserve"], 0.0);
            }
            restored.redo().unwrap();
            assert!(restored.model.paint_order().is_empty());
            restored.undo().unwrap();
            restored.undo().unwrap();
            assert_eq!(restored.model.paint_order().len(), 2);
            let edited = restored.model.paint_order()[0];
            assert_eq!(restored.model.text(edited).unwrap().text, "edited text");
            restored.undo().unwrap();
            restored.undo().unwrap();
            assert_eq!(restored.model.text(edited).unwrap().text, "original text");
            restored.redo().unwrap();
            assert_eq!(restored.model.text(edited).unwrap().text, "edited text");
            let saved = restored.serialize_document_session().unwrap();
            let round_trip = Engine::from_serialized_document_session_with_config(
                &saved,
                RuntimeConfig::default(),
            )
            .unwrap();
            assert_eq!(round_trip.model.document(), restored.model.document());
            assert_eq!(
                serde_json::to_value(&round_trip.history).unwrap(),
                serde_json::to_value(&restored.history).unwrap()
            );
        }
    }
}

#[test]
fn legacy_speech_bubble_text_templates_import_and_replay_without_new_measurements() {
    let mut source = Engine::default();
    let viewport = source.create_viewport(Default::default()).unwrap();
    source
        .create_text_with_viewport_changes(
            viewport,
            Point::new(10.0, 20.0),
            "saved template",
            TextLayoutSize::new(140.0, 36.0),
        )
        .unwrap();
    let id = source.model.paint_order()[0];
    source
        .select_element_with_viewport_changes(viewport, id)
        .unwrap();
    let mut legacy: Value =
        serde_json::from_slice(&source.serialize_selected_draw_template().unwrap()).unwrap();
    remove_speech_bubble_fields(&mut legacy);
    for version in 1..=3 {
        legacy["schemaVersion"] = version.into();
        let mut target = Engine::default();
        let viewport = target.create_viewport(Default::default()).unwrap();
        target
            .insert_draw_template_with_viewport_changes(
                viewport,
                &serde_json::to_vec(&legacy).unwrap(),
                Point::new(250.0, 120.0),
            )
            .unwrap();
        let inserted = target.selected_ids()[0];
        let text = target.model.text(inserted).unwrap().clone();
        assert_eq!(text.text, "saved template");
        assert_eq!(text.center, Point::new(250.0, 120.0));
        assert_eq!(text.text_ink_reserve, 0.0);
        target.undo().unwrap();
        assert!(target.model.paint_order().is_empty());
        target.redo().unwrap();
        assert_eq!(target.model.text(inserted).unwrap(), &text);
    }
    legacy["schemaVersion"] = 4.into();
    let mut target = Engine::default();
    let viewport = target.create_viewport(Default::default()).unwrap();
    assert_eq!(
        target
            .insert_draw_template_with_viewport_changes(
                viewport,
                &serde_json::to_vec(&legacy).unwrap(),
                Point::default(),
            )
            .unwrap_err(),
        ErrorCode::InvalidArgument,
    );
}

#[test]
fn legacy_speech_bubble_empty_sessions_migrate_only_omitted_defaults() {
    let mut config = RuntimeConfig::default();
    config.style_defaults.editor.rectangle.stroke_width = 5.0;
    config.style_defaults.editor.speech_bubble.stroke_width = 7.0;
    let source = Engine::new(config);
    let saved: Value =
        serde_json::from_slice(&source.serialize_document_session().unwrap()).unwrap();
    for keep_bubble_style in [false, true] {
        let mut legacy = saved.clone();
        legacy["schemaVersion"] = 12.into();
        if !keep_bubble_style {
            legacy["editor"]
                .as_object_mut()
                .unwrap()
                .remove("speechBubble");
        }
        // A supplied measurement must survive even when another field migrates.
        legacy["editor"]["text"]["text_ink_reserve"] = 3.5.into();
        let restored = Engine::from_serialized_document_session_with_config(
            &serde_json::to_vec(&legacy).unwrap(),
            RuntimeConfig::default(),
        )
        .unwrap();
        assert!(!restored.has_document_content());
        let migrated: Value =
            serde_json::from_slice(&restored.serialize_document_session().unwrap()).unwrap();
        assert_eq!(migrated["editor"]["text"]["text_ink_reserve"], 3.5);
        assert_eq!(
            &migrated["editor"]["speechBubble"],
            if keep_bubble_style {
                &saved["editor"]["speechBubble"]
            } else {
                &saved["editor"]["rectangle"]
            }
        );
        legacy["editor"]["speechBubble"] = Value::Null;
        assert_eq!(
            Engine::from_serialized_document_session_with_config(
                &serde_json::to_vec(&legacy).unwrap(),
                RuntimeConfig::default(),
            )
            .unwrap_err(),
            ErrorCode::InvalidArgument
        );
    }
}

#[test]
fn speech_bubble_persistence_migration_keeps_current_and_malformed_payloads_strict() {
    let source = text_history_fixture();
    for history_only in [false, true] {
        let bytes = if history_only {
            source.serialize_document_history()
        } else {
            source.serialize_document_session()
        }
        .unwrap();
        let saved: Value = serde_json::from_slice(&bytes).unwrap();
        let restore = |value: &Value| {
            let bytes = serde_json::to_vec(value).unwrap();
            if history_only {
                Engine::from_serialized_document_history_with_config(
                    &bytes,
                    RuntimeConfig::default(),
                )
            } else {
                Engine::from_serialized_document_session_with_config(
                    &bytes,
                    RuntimeConfig::default(),
                )
            }
        };
        let mut current_missing = saved.clone();
        remove_speech_bubble_fields(&mut current_missing);
        assert_eq!(
            restore(&current_missing).unwrap_err(),
            ErrorCode::InvalidArgument
        );
        for invalid in [Value::Null, Value::from(-1.0), Value::from("invalid")] {
            let mut legacy = saved.clone();
            remove_speech_bubble_fields(&mut legacy);
            legacy["schemaVersion"] = if history_only { 8 } else { 12 }.into();
            let text = legacy["document"]["slots"]
                .as_array_mut()
                .unwrap()
                .iter_mut()
                .find_map(|slot| slot.get_mut("data").and_then(|data| data.get_mut("Text")))
                .unwrap();
            text["text_ink_reserve"] = invalid;
            assert_eq!(restore(&legacy).unwrap_err(), ErrorCode::InvalidArgument);
        }
        let mut malformed = saved.clone();
        remove_speech_bubble_fields(&mut malformed);
        malformed["schemaVersion"] = if history_only { 8 } else { 12 }.into();
        let text = malformed["document"]["slots"]
            .as_array_mut()
            .unwrap()
            .iter_mut()
            .find_map(|slot| slot.get_mut("data").and_then(|data| data.get_mut("Text")))
            .unwrap();
        text.as_object_mut().unwrap().remove("font_size");
        assert_eq!(restore(&malformed).unwrap_err(), ErrorCode::InvalidArgument);
    }
}
