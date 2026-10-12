#pragma once

#include "pinned_animation_fixtures.h"
#include "snowimageqtcodec.h"
#include "snow_shot/presentation/pinnedwindowgroupmanager.h"
#include "snow_shot/presentation/screenshotanimationplayback.h"
#include "snow_shot/presentation/screenshotexportartifact.h"
#include "snow_shot/presentation/screenshotselectionexportuiservices.h"
#include "snow_shot/storage/applicationstorage.h"
#include "snow_shot/storage/pinnedwindowrepository.h"
#include "snow_shot/storage/settingsadapters.h"

#include <QCoreApplication>
#include <QEvent>
#include <QPointer>
#include <QScopeGuard>

#include <atomic>
#include <future>
#include <memory>

namespace pinned_animation_persistence_tests {
// The caller owns isolated application storage and supplies the same paint/wait
// hooks used by the pinned-window integration tests. The encoder gate makes
// replacement happen strictly after the first static save has started.
template <class Access, class Require, class Wait, class Paint>
void replaceWhileInitialPngIsSaving(const pinned_animation_fixtures::Fixture& fixture,
                                    QScreen* screen, const QRect& nativeGeometry, Require require,
                                    Wait wait, Paint paint) {
    using namespace snow_shot;
    auto& applicationStorage = storage::ApplicationStorage::instance();
    auto& repository = applicationStorage.pinnedWindows();
    require(storage::PinToScreenSettings().setAutomaticTextRecognition(false),
            "disable automatic recognition for the pending-save race");
    presentation::PinnedWindowGroupManager groups(&repository);
    ScreenshotSelectionExportUiServices services(nullptr, nullptr, nullptr, {}, {}, &groups);
    QObject connections;
    QObject::connect(&applicationStorage, &storage::ApplicationStorage::pinnedWindowsChanged,
                     &connections, [&] { groups.onPinnedRecordsChanged(); });
    QObject::connect(&groups,
                     &presentation::PinnedWindowGroupManager::restoreActiveGroupWindowsRequested,
                     &connections, [&] { services.restorePersistedWindows(); });

    const auto source = image_codec::decodeAnimationSource(fixture.bytes);
    require(source && source->isAnimated(), "decode replacement animation for pending save");

    auto releaseEncoder = std::make_shared<std::promise<void>>();
    const auto encoderGate = releaseEncoder->get_future().share();
    auto encodingStarted = std::make_shared<std::atomic_bool>(false);
    bool released = false;
    ScreenshotExportArtifact::PngCachePolicy policy;
    policy.encodingStarted = [encodingStarted, encoderGate] {
        encodingStarted->store(true, std::memory_order_release);
        encoderGate.wait();
    };
    QImage staticImage(source->canvasSize, QImage::Format_RGB32);
    staticImage.fill(Qt::blue);
    auto artifact = std::make_shared<ScreenshotExportArtifact>(
        ScreenshotExportSource::fromImage(staticImage), ScreenshotCompressionLevel::Medium,
        std::move(policy));
    const auto unblockEncoder = qScopeGuard([&] {
        if (!released)
            releaseEncoder->set_value();
    });
    require(
        services.presentPinnedImageArtifact(artifact, screen, nativeGeometry, staticImage.size()),
        "present static pin whose initial PNG save is gated");
    wait(
        [&] {
            return encodingStarted->load(std::memory_order_acquire) &&
                   groups.liveWindows().size() == 1 &&
                   groups.liveWindows().front()->sourcePinAvailable();
        },
        "initial PNG encoding begins while the static pin is usable");
    QPointer<ScreenshotPinnedWindow> window(groups.liveWindows().front());
    const auto windowDeleted = [&] {
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
        return window.isNull();
    };
    const QString id = window->persistenceId();
    require(repository.summaries().isEmpty(),
            "initial source stays pending until its gated encoder completes");

    ScreenshotClipboardContent replacement;
    replacement.image = source->firstFrame;
    replacement.animationSource = source;
    require(window->automationReplaceContent(std::move(replacement)),
            "replace pending static pin with animation");
    Access::animationCommand(*window, QStringLiteral("next_frame"));
    wait(
        [&] {
            paint(*window);
            return Access::playback(*window).frameIndex() == 1;
        },
        "replacement advances and paints a noninitial frame before old PNG completes");
    for (int i = 0; i < 7; ++i)
        Access::animationCommand(*window, QStringLiteral("increase_playback_speed"));
    require(Access::playback(*window).speedPercent() == 170 &&
                !Access::playback(*window).requestedPlaying(),
            "pending replacement retains paused playback and its chosen speed");
    releaseEncoder->set_value();
    released = true;
    wait([&] { return repository.summaries().size() == 1; },
         "obsolete static PNG completion commits the live animation source");
    require(repository.flush().success, "flush replacement animation after old PNG completion");
    const auto record = repository.loadRecord(id);
    require(record && record->animationSource &&
                record->animationSource->encoded == fixture.bytes &&
                record->image == source->firstFrame && record->animationSpeedPercent == 170 &&
                !record->animationPlaying,
            "initial-save race persists exact animation bytes, first-frame preview and playback");

    require(window->requestHide(), "hide paused animation before restore");
    wait(windowDeleted, "hidden animation releases its original shell");
    services.showAllWindows();
    wait(
        [&] {
            auto* restored = groups.liveWindow(id);
            return restored && restored->sourcePinAvailable();
        },
        "restore the paused animation through its repository config");
    window = groups.liveWindow(id);
    paint(*window);
    require(Access::playback(*window).frameIndex() == 0 &&
                Access::playback(*window).speedPercent() == 170 &&
                !Access::playback(*window).requestedPlaying(),
            "paused restore starts at frame zero with the persisted speed and playback choice");

    Access::animationCommand(*window, QStringLiteral("reset_playback_speed"));
    Access::animationCommand(*window, QStringLiteral("toggle_playback"));
    require(window->requestHide(), "hide requested-playing animation before another restore");
    wait(windowDeleted, "playing animation releases its original shell");
    services.showAllWindows();
    wait(
        [&] {
            auto* restored = groups.liveWindow(id);
            return restored && restored->sourcePinAvailable();
        },
        "restore requested-playing animation through its repository config");
    window = groups.liveWindow(id);
    require(Access::playback(*window).frameIndex() == 0 &&
                Access::playback(*window).speedPercent() == 100 &&
                Access::playback(*window).requestedPlaying(),
            "playing restore starts at frame zero with the persisted default speed and choice");
    Access::playback(*window).pause();
    services.destroyRecords({id});
    wait(windowDeleted, "destroy restored animation race fixture");
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
}
} // namespace pinned_animation_persistence_tests
