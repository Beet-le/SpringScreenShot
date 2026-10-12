use serde::de::DeserializeOwned;
use serde_json::Value;
use snow_draw_engine_core::ErrorCode;

use crate::session::MAX_DOCUMENT_SESSION_BYTES;

#[derive(Clone, Copy)]
pub(crate) enum DrawingPayloadKind {
    Session,
    History,
    Template,
}

impl DrawingPayloadKind {
    // These introduction versions stay fixed as the formats advance.
    fn speech_bubble_schema_version(self) -> u64 {
        match self {
            Self::Session => 13,
            Self::History => 9,
            Self::Template => 4,
        }
    }
}

/// Keep valid payloads on the single-pass typed reader. Only earlier schemas
/// may supply the speech-bubble fields that did not exist when they were saved.
pub(crate) fn deserialize_drawing<T: DeserializeOwned>(
    bytes: &[u8],
    kind: DrawingPayloadKind,
) -> Result<T, ErrorCode> {
    if bytes.is_empty() || bytes.len() > MAX_DOCUMENT_SESSION_BYTES {
        return Err(ErrorCode::InvalidArgument);
    }
    if let Ok(payload) = serde_json::from_slice(bytes) {
        return Ok(payload);
    }
    let mut payload: Value =
        serde_json::from_slice(bytes).map_err(|_| ErrorCode::InvalidArgument)?;
    let version = payload
        .get("schemaVersion")
        .and_then(Value::as_u64)
        .ok_or(ErrorCode::InvalidArgument)?;
    if !(1..kind.speech_bubble_schema_version()).contains(&version) {
        return Err(ErrorCode::InvalidArgument);
    }

    match kind {
        DrawingPayloadKind::Session | DrawingPayloadKind::History => {
            if let Some(document) = payload.get_mut("document")
                && let Some(slots) = document.get_mut("slots").and_then(Value::as_array_mut)
            {
                for record in slots {
                    migrate_element_record(record);
                }
            }
            if let Some(history) = payload.get_mut("history") {
                migrate_history(history);
            }
            if let DrawingPayloadKind::Session = kind
                && let Some(editor) = payload.get_mut("editor").and_then(Value::as_object_mut)
            {
                // Before independent bubble defaults existed, Shape was the
                // user's persisted appearance for this family of geometry.
                if !editor.contains_key("speechBubble")
                    && let Some(rectangle) = editor.get("rectangle").cloned()
                {
                    editor.insert("speechBubble".into(), rectangle);
                }
                if let Some(text) = editor.get_mut("text") {
                    migrate_text(text);
                }
            }
        }
        DrawingPayloadKind::Template => {
            if let Some(elements) = payload.get_mut("elements").and_then(Value::as_array_mut) {
                for record in elements {
                    migrate_element_record(record);
                }
            }
        }
    }
    // Migration supplies omitted fields only. The normal typed reader and
    // document/history validation still reject invalid supplied values.
    serde_json::from_value(payload).map_err(|_| ErrorCode::InvalidArgument)
}

fn migrate_text(text: &mut Value) {
    if let Some(fields) = text.as_object_mut() {
        fields.entry("text_ink_reserve").or_insert(Value::from(0.0));
    }
}

fn migrate_element_data(data: &mut Value) {
    if let Some(text) = data.get_mut("Text") {
        migrate_text(text);
    }
}

fn migrate_element_record(record: &mut Value) {
    if let Some(data) = record.get_mut("data") {
        migrate_element_data(data);
    }
}

fn migrate_history(history: &mut Value) {
    for stack in ["undoStack", "redoStack"] {
        let Some(entries) = history.get_mut(stack).and_then(Value::as_array_mut) else {
            continue;
        };
        for entry in entries {
            for direction in ["undo", "redo"] {
                let Some(operations) = entry
                    .get_mut(direction)
                    .and_then(|transaction| transaction.get_mut("operations"))
                    .and_then(Value::as_array_mut)
                else {
                    continue;
                };
                for operation in operations {
                    for variant in ["InsertElement", "UpdateElementData"] {
                        if let Some(data) = operation
                            .get_mut(variant)
                            .and_then(|fields| fields.get_mut("data"))
                        {
                            migrate_element_data(data);
                        }
                    }
                    if let Some(record) = operation
                        .get_mut("RestoreElement")
                        .and_then(|fields| fields.get_mut("element"))
                    {
                        migrate_element_record(record);
                    }
                }
            }
        }
    }
}
