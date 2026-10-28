#ifndef SNOW_SHOT_TELEMETRY_USAGEEVENTS_H
#define SNOW_SHOT_TELEMETRY_USAGEEVENTS_H

#include <QJsonObject>
#include <QString>
#include <QStringList>
#include <QtGlobal>

#include <optional>
#include <type_traits>

namespace snow_shot::telemetry {

// Samples travel from whatever thread recorded the diagnostic event to the uploader's ring
// buffer, so they must stay pointer-free and trivially copyable: the catalogue is a closed
// enum rather than free-form strings for exactly that reason.
enum class UsageOutcome : quint8 {
    Unknown,
    Succeeded,
    Failed,
    Cancelled,
    Rejected,
};

enum class UsageFeature : quint8 {
    None,
    Translation,
    TableExtraction,
    LatexExtraction,
    ImageConversion,
    Other,
};

// Annotation tools are a dimension rather than one event per tool: the diagnostics event name
// stays stable while the tool travels in a field, exactly like request_kind does for API calls.
enum class UsageTool : quint8 {
    Unknown,
    Select,
    Shape,
    Arrow,
    Line,
    FreeDraw,
    RectangleHighlight,
    PenHighlight,
    Eraser,
    RectangleFilter,
    Watermark,
    Text,
    SerialNumber,
    PenFilter,
    Spotlight,
    AutoFilter,
    RectangleEraser,
    BrushEraser,
    Distance,
    Angle,
    Magnifier,
    SpeechBubble,
    Count,
};

enum class UsageSurface : quint8 {
    Unknown,
    Capture,
    Pinned,
    ImageEditing,
    Count,
};

enum class UsageEvent : quint8 {
    SessionStart,
    SessionCleanShutdown,
    SessionUnexpectedTermination,
    CaptureFinished,
    ScrollingStarted,
    OcrSubmitted,
    OcrFinished,
    OcrCancelled,
    RecordingStarted,
    RecordingPaused,
    RecordingResumed,
    RecordingExportFinished,
    RecordingFailed,
    RecordingTrimOpened,
    ExportImageFinished,
    ExportCancelled,
    PinStarted,
    PinFinished,
    PinRejected,
    ClipboardFinished,
    PrintSnapshotRequested,
    PrintFailed,
    ApiRequestFinished,
    AnnotationToolSelected,
    AnnotationStrokeCommitted,
    Count,
};

struct UsageSample {
    qint64 epochMs = 0;
    UsageEvent event = UsageEvent::Count;
    UsageOutcome outcome = UsageOutcome::Unknown;
    UsageFeature feature = UsageFeature::None;
    UsageTool tool = UsageTool::Unknown;
    UsageSurface surface = UsageSurface::Unknown;
};
static_assert(std::is_trivially_copyable_v<UsageSample>);
static_assert(sizeof(UsageSample) <= 24, "samples must stay tiny; they live in a fixed ring");

// Maps a diagnostics event onto the usage catalogue. Returns nullopt for everything the
// catalogue does not care about, which is the overwhelmingly common case: the diagnostics
// message handler funnels every Qt log record through record() as "qt.message", and startup
// noise (display topology, permissions, shortcut registration, OCR engine lifecycle) must not
// drown out real feature usage.
//
// Filter order is part of the hot path contract; keep the cheap string tests first.
std::optional<UsageSample> usageSampleForDiagnosticEvent(QtMsgType level, const QString& category,
                                                         const QString& event,
                                                         const QJsonObject& fields,
                                                         qint64 epochMs) noexcept;

QString usageEventName(UsageEvent event);
QString usageOutcomeName(UsageOutcome outcome);
QString usageFeatureName(UsageFeature feature);
QString usageToolName(UsageTool tool);
QString usageSurfaceName(UsageSurface surface);

// Diagnostic event names the catalogue reacts to, in table order. Exposed so the mapping can
// be verified without a network stack and so the catalogue can be reviewed at a glance.
QStringList trackedDiagnosticEvents();

} // namespace snow_shot::telemetry

#endif
