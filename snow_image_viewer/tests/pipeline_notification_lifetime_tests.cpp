#include "editing/edit_pipeline_controller.h"
#include "core/image_raster_store.h"

#include <QApplication>
#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QImage>
#include <QProcess>
#include <QTemporaryDir>
#include <QTimer>
#include <QtTest/QTest>

#include <cstdlib>
#include <array>
#include <iostream>
#include <memory>

namespace {
using namespace snow::image_viewer;

void require(bool condition, const char* message) {
    if (!condition) {
        std::cerr << message << '\n';
        std::exit(EXIT_FAILURE);
    }
}

EditExportSettings settings() {
    EditExportSettings result;
    result.sourceSize = QSize(8, 8);
    result.width = 8;
    result.height = 8;
    result.format = snow::image::Format::png;
    result.encode.format = result.format;
    return result;
}

EditPipelineOptions options(const QString& directory) {
    EditPipelineOptions result;
    result.discreteDebounceMs = 60000;
    result.memoryBudgetBytes = 512U * 1024U * 1024U;
    result.workerExecutablePath = directory + QStringLiteral("/missing-worker.exe");
    return result;
}

EditPipelineOptions realWorkerOptions(const QString& directory) {
    auto result = options(directory);
    result.workerExecutablePath = QDir(QCoreApplication::applicationDirPath())
                                      .filePath(
#ifdef Q_OS_WIN
                                          QStringLiteral("snow_image_edit_worker.exe")
#else
                                          QStringLiteral("snow_image_edit_worker")
#endif
                                      );
    require(QFileInfo(result.workerExecutablePath).isExecutable(),
            "the real edit worker is available beside the protocol publication test");
    return result;
}

void startupObserversPreserveWorkerReadiness(QObject& observations, const QString& path,
                                             const QString& directory) {
    snow::image::DocumentDescriptor descriptor;
    descriptor.canvas_width = descriptor.canvas_height = 8;
    snow::image::RasterFrameDescriptor frame;
    frame.width = frame.height = 8;
    frame.layout.alpha = snow::image::kRgba8.alpha;
    frame.layout.planes.push_back(
        {snow::image::PlaneSemantic::packed, 8, 8, snow::image::kRgba8, 8});
    descriptor.frames.push_back(std::move(frame));
    QString backingError;
    const auto backing = ImageRasterStore::create(std::move(descriptor), {}, &backingError);
    require(backing && backingError.isEmpty(), "create the startup CPU source fixture");
    std::array<std::byte, 8 * 8 * 4> pixels;
    pixels.fill(std::byte{0xff});
    require(backing->store()->write_rows(0, 0, 0, 8, 32, pixels).has_value() &&
                backing->store()->commit().has_value(),
            "commit the startup CPU source before the worker handshake");
    backing->setVerifiedAlphaContent(snow::image::AlphaContent::opaque);
    enum class Action {
        Cancel,
        ReplaceEdit,
        ReplaceAndFlush,
        ReplaceSource,
        ReplaceWorker,
        Destroy,
    };
    for (const auto action : {Action::Cancel, Action::ReplaceEdit, Action::ReplaceAndFlush,
                              Action::ReplaceSource, Action::ReplaceWorker, Action::Destroy}) {
        auto controller =
            std::make_unique<EditPipelineController>(realWorkerOptions(directory), nullptr);
        auto requestedSettings = settings();
        int startupNotifications = 0;
        int dispatchedJobs = 0;
        bool exported = false;
        QString failure;
        EditRequestId exportId = 0;
        qint64 processId = 0;
        QObject::connect(
            controller.get(), &EditPipelineController::performanceStageCompleted, &observations,
            [&](auto requestId, const QString& stage, auto) {
                if (stage == QStringLiteral("worker.job_dispatched")) {
                    require(requestId == exportId, "only the surviving startup edit is dispatched");
                    ++dispatchedJobs;
                }
                if (stage != QStringLiteral("worker.startup"))
                    return;
                if (++startupNotifications != 1) {
                    require(action == Action::ReplaceWorker && startupNotifications == 2,
                            "each worker publishes its handshake once");
                    return;
                }
                processId = controller->workerProcessId();
                require(processId != 0, "the ready handshake belongs to a running worker");
                if (action == Action::Cancel) {
                    controller->cancel();
                } else if (action == Action::ReplaceEdit || action == Action::ReplaceAndFlush ||
                           action == Action::ReplaceWorker) {
                    if (action == Action::ReplaceWorker) {
                        auto* worker = controller->findChild<QProcess*>();
                        require(worker != nullptr, "find the worker whose handshake is publishing");
                        worker->kill();
                        require(worker->waitForFinished(1000),
                                "retire the original worker inside its startup observer");
                    }
                    const auto previousId = controller->latestRequestId();
                    requestedSettings.encode.compression_level = 9;
                    exportId = controller->requestEdit(requestedSettings);
                    require(exportId > previousId,
                            "the startup observer replaces the current edit");
                    if (action != Action::ReplaceEdit)
                        controller->flushPendingExact();
                } else if (action == Action::ReplaceSource) {
                    controller->setSource(path, backing);
                } else {
                    controller.reset();
                }
            });
        QObject::connect(controller.get(), &EditPipelineController::exactReady, &observations,
                         [&](const ExactEditResult& result) {
                             require(result.requestId == exportId &&
                                         result.settings == requestedSettings &&
                                         result.provenance != RasterProvenance::gpu_approximate &&
                                         controller->hasExactResult(requestedSettings),
                                     "the surviving edit publishes its actual CPU export");
                             exported = true;
                         });
        QObject::connect(controller.get(), &EditPipelineController::failed, &observations,
                         [&](const QString& message) { failure = message; });
        controller->setSource(path, backing);
        require(controller->requestEdit(settings()) != 0,
                "prepare an edit before the worker handshake");
        require(QTest::qWaitFor([&] { return startupNotifications != 0; }, 5000),
                "observe the real worker handshake");
        if (action == Action::Destroy) {
            require(!controller, "a startup observer can destroy the controller");
            continue;
        }
        if (exportId == 0)
            exportId = controller->requestEdit(requestedSettings);
        require(exportId != 0, "request an export after the startup observer returns");
        controller->flushPendingExact();
        require(QTest::qWaitFor([&] { return exported || !failure.isEmpty(); }, 5000) && exported &&
                    failure.isEmpty(),
                "startup request supersession must preserve worker readiness for CPU export");
        const bool workerReplaced = action == Action::ReplaceWorker;
        require(dispatchedJobs == 1 &&
                    (controller->workerProcessId() != processId) == workerReplaced &&
                    controller->workerRestartCount() == (workerReplaced ? 1 : 0) &&
                    startupNotifications == (workerReplaced ? 2 : 1),
                "only the surviving worker dispatches the current export once");
    }
}

void cancellationObserverDispatchesCurrentEdit(QObject& observations, const QString& path,
                                               const QString& directory) {
    auto workerOptions = realWorkerOptions(directory);
    workerOptions.workerTestMode = QStringLiteral("block-cooperative");
    EditPipelineController controller(workerOptions, nullptr);
    auto firstSettings = settings();
    firstSettings.format = firstSettings.encode.format = snow::image::Format::jpeg;
    firstSettings.encode.quality = 75;
    auto queuedSettings = firstSettings;
    queuedSettings.width = 7;
    auto currentSettings = firstSettings;
    currentSettings.width = 6;
    bool sourceDecoded = false;
    bool workerReady = false;
    bool cancellationObserved = false;
    bool exported = false;
    int dispatchedJobs = 0;
    int currentDispatches = 0;
    QString failure;
    EditRequestId firstId = 0;
    EditRequestId currentId = 0;
    QObject::connect(&controller, &EditPipelineController::sourceReady, &observations,
                     [&] { sourceDecoded = true; });
    QObject::connect(
        &controller, &EditPipelineController::performanceStageCompleted, &observations,
        [&](auto requestId, const QString& stage, auto) {
            if (stage == QStringLiteral("worker.startup"))
                workerReady = true;
            if (stage == QStringLiteral("worker.job_dispatched")) {
                ++dispatchedJobs;
                if (requestId == firstId) {
                    QTimer::singleShot(0, &controller, [&] {
                        require(controller.requestEdit(queuedSettings) > firstId,
                                "queue a replacement for the blocked cooperative export");
                        controller.flushPendingExact();
                    });
                } else {
                    require(requestId == currentId,
                            "the superseded queued edit must never reach the worker");
                    ++currentDispatches;
                }
            }
            if (stage == QStringLiteral("worker.cooperative_cancellation")) {
                require(!cancellationObserved, "the blocked export is acknowledged once");
                cancellationObserved = true;
                currentId = controller.requestEdit(currentSettings);
                controller.flushPendingExact();
            }
        });
    QObject::connect(&controller, &EditPipelineController::exactReady, &observations,
                     [&](const ExactEditResult& result) {
                         require(result.requestId == currentId &&
                                     result.settings == currentSettings &&
                                     controller.hasExactResult(currentSettings),
                                 "the cancellation observer's edit supersedes queued work");
                         exported = true;
                     });
    QObject::connect(&controller, &EditPipelineController::failed, &observations,
                     [&](const QString& message) { failure = message; });
    controller.setSource(path);
    require(QTest::qWaitFor([&] { return sourceDecoded && workerReady; }, 5000),
            "prepare the CPU source and cooperative worker");
    const auto processId = controller.workerProcessId();
    firstId = controller.requestEdit(firstSettings);
    require(firstId != 0, "request the blocked cooperative export");
    controller.flushPendingExact();
    require(QTest::qWaitFor([&] { return exported || !failure.isEmpty(); }, 5000) &&
                cancellationObserved && exported && failure.isEmpty() && dispatchedJobs == 2 &&
                currentDispatches == 1 && controller.workerProcessId() == processId &&
                controller.workerRestartCount() == 0,
            "a cancellation observer can replace work and reuse the acknowledged worker");
}

void requestObserversMayDestroyController(QObject& observations, const QString& directory) {
    for (int variant = 0; variant < 3; ++variant) {
        auto controller = std::make_unique<EditPipelineController>(options(directory), nullptr);
        controller->setGpuSource(QStringLiteral("gpu-source.png"));
        if (variant == 0)
            QObject::connect(controller.get(), &EditPipelineController::stateChanged, &observations,
                             [&](auto) { controller.reset(); });
        else if (variant == 1)
            QObject::connect(controller.get(), &EditPipelineController::visualRequested,
                             &observations, [&](auto, const auto&) { controller.reset(); });
        else
            QObject::connect(controller.get(), &EditPipelineController::performanceStageCompleted,
                             &observations, [&](auto, const auto&, auto) { controller.reset(); });
        require(controller->requestEdit(settings()) != 0 && !controller,
                "an edit request can retire its controller from each notification boundary");
    }
    for (int variant = 0; variant < 5; ++variant) {
        auto controller = std::make_unique<EditPipelineController>(options(directory), nullptr);
        controller->setGpuSource(QStringLiteral("gpu-source.png"));
        controller->requestEdit(settings());
        if (variant == 0 || variant == 1)
            QObject::connect(controller.get(), &EditPipelineController::performanceStageCompleted,
                             &observations, [&](auto, const QString& stage, auto) {
                                 if (stage == (variant == 0
                                                   ? QStringLiteral("exact.debounce_delay")
                                                   : QStringLiteral("exact.resource_planning")))
                                     controller.reset();
                             });
        else if (variant == 2)
            QObject::connect(controller.get(), &EditPipelineController::stateChanged, &observations,
                             [&](auto state) {
                                 if (state == EditPipelineState::EncodingPending)
                                     controller.reset();
                             });
        else if (variant == 3)
            QObject::connect(controller.get(), &EditPipelineController::busyChanged, &observations,
                             [&](bool busy) {
                                 if (busy)
                                     controller.reset();
                             });
        else
            QObject::connect(controller.get(), &EditPipelineController::exactRasterRequested,
                             &observations, [&](auto, const auto&) { controller.reset(); });
        controller->flushPendingExact();
        require(!controller, "exact scheduling stops when its notification retires the controller");
    }
    {
        auto controller = std::make_unique<EditPipelineController>(options(directory), nullptr);
        controller->setGpuSource(QStringLiteral("gpu-source.png"));
        const auto id = controller->requestEdit(settings());
        int visualCompletions = 0;
        QObject::connect(controller.get(), &EditPipelineController::visualReady, &observations,
                         [&](auto) { ++visualCompletions; });
        QObject::connect(controller.get(), &EditPipelineController::stateChanged, &observations,
                         [&](auto) { controller.reset(); });
        controller->submitVisualFrame(id);
        require(!controller && visualCompletions == 0,
                "a retired visual state does not publish another completion");
    }
}

void decodeObserversMayDestroyController(QObject& observations, const QString& path,
                                         const QString& directory) {
    for (int variant = 0; variant < 4; ++variant) {
        auto controller = std::make_unique<EditPipelineController>(options(directory), nullptr);
        if (variant == 0)
            QObject::connect(controller.get(), &EditPipelineController::sourceReady, &observations,
                             [&] { controller.reset(); });
        else if (variant == 1)
            QObject::connect(controller.get(), &EditPipelineController::performanceStageCompleted,
                             &observations, [&](auto, const auto&, auto) { controller.reset(); });
        else
            QObject::connect(controller.get(), &EditPipelineController::failed, &observations,
                             [&](const auto&) { controller.reset(); });
        if (variant == 3) {
            bool ready = false;
            QObject::connect(controller.get(), &EditPipelineController::sourceReady, &observations,
                             [&] { ready = true; });
            controller->setSource(path);
            require(QTest::qWaitFor([&] { return ready; }, 5000), "decode a real export source");
            controller->requestEdit(settings());
            controller->flushPendingExact();
        } else
            controller->setSource(variant == 2 ? directory + QStringLiteral("/absent.png") : path);
        require(QTest::qWaitFor([&] { return !controller; }, 5000),
                "decode completion, timing, failure and worker failure observers can retire the "
                "controller");
    }
}

void nestedEditSupersedesOuterPublication(QObject& observations, const QString& directory) {
    EditPipelineController controller(options(directory), nullptr);
    controller.setGpuSource(QStringLiteral("gpu-source.png"));
    auto replacement = settings();
    replacement.width = 6;
    bool replaced = false;
    quint64 replacementId = 0;
    QList<quint64> visualRequests;
    QObject::connect(&controller, &EditPipelineController::stateChanged, &observations, [&](auto) {
        if (!replaced) {
            replaced = true;
            replacementId = controller.requestEdit(replacement);
        }
    });
    QObject::connect(&controller, &EditPipelineController::visualRequested, &observations,
                     [&](auto id, const auto&) { visualRequests.append(id); });
    const auto originalId = controller.requestEdit(settings());
    require(originalId != replacementId && controller.latestRequestId() == replacementId &&
                visualRequests == QList<quint64>{replacementId},
            "a nested edit supersedes the outer request before its visual notification");
}

void busyNotificationsSurviveRequestSupersession(QObject& observations, const QString& directory) {
    for (int variant = 0; variant < 4; ++variant) {
        EditPipelineController controller(options(directory), nullptr);
        controller.setGpuSource(QStringLiteral("gpu-source.png"));
        QList<bool> busyNotifications;
        QObject::connect(&controller, &EditPipelineController::busyChanged, &observations,
                         [&](bool busy) {
                             require(busy == controller.isBusy(),
                                     "busy notifications describe the surviving pipeline state");
                             busyNotifications.append(busy);
                         });
        controller.requestEdit(settings());
        controller.flushPendingExact();
        require(controller.isBusy() && busyNotifications == QList<bool>{true},
                "the original GPU readback publishes its busy state");

        auto replacement = settings();
        replacement.width = 6;
        bool replaced = false;
        EditRequestId replacementId = 0;
        QList<EditRequestId> visualRequests;
        QObject::connect(&controller, &EditPipelineController::visualRequested, &observations,
                         [&](auto id, const auto&) { visualRequests.append(id); });
        QObject::connect(&controller, &EditPipelineController::stateChanged, &observations,
                         [&](auto state) {
                             if (replaced || state != EditPipelineState::VisualPending)
                                 return;
                             replaced = true;
                             if (variant == 0 || variant == 1) {
                                 replacementId = controller.requestEdit(replacement);
                                 if (variant == 1)
                                     controller.flushPendingExact();
                             } else if (variant == 2) {
                                 controller.setGpuSource(QStringLiteral("replacement-source.png"));
                             } else {
                                 controller.cancel();
                             }
                         });
        auto outerSettings = settings();
        outerSettings.width = 7;
        const auto outerId = controller.requestEdit(outerSettings);
        require(replaced, "the outer edit enters its reentrant state observer");
        if (variant == 1) {
            const bool publishedCurrentTransitions =
                busyNotifications == QList<bool>{true} ||
                busyNotifications == QList<bool>({true, false, true});
            require(controller.isBusy() && publishedCurrentTransitions &&
                        controller.latestRequestId() == replacementId &&
                        visualRequests == QList<EditRequestId>{replacementId},
                    "a nested return to encoding preserves the delivered busy value without "
                    "duplicates or an obsolete visual request");
        } else {
            require(!controller.isBusy() && busyNotifications == QList<bool>({true, false}),
                    "a surviving non-busy transition is published after request or source "
                    "supersession and cancellation");
            if (variant == 0)
                require(controller.latestRequestId() == replacementId && replacementId != outerId &&
                            visualRequests == QList<EditRequestId>{replacementId},
                        "a same-state nested edit keeps only the replacement visual request");
            else
                require(controller.latestRequestId() != outerId && visualRequests.isEmpty(),
                        "source reset and cancellation stop the obsolete visual request");
        }
    }
}

void busyEntryObserversMayReverseOrRetireState(QObject& observations, const QString& directory) {
    for (int variant = 0; variant < 3; ++variant) {
        auto controller = std::make_unique<EditPipelineController>(options(directory), nullptr);
        controller->setGpuSource(QStringLiteral("gpu-source.png"));
        controller->requestEdit(settings());
        QList<bool> busyNotifications;
        bool replaced = false;
        auto replacement = settings();
        replacement.width = 6;
        QObject::connect(controller.get(), &EditPipelineController::busyChanged, &observations,
                         [&](bool busy) {
                             require(controller && busy == controller->isBusy(),
                                     "busy entry observers receive the current busy value");
                             busyNotifications.append(busy);
                             if (variant == 1 && busy && !replaced) {
                                 replaced = true;
                                 controller->requestEdit(replacement);
                             }
                         });
        QObject::connect(controller.get(), &EditPipelineController::stateChanged, &observations,
                         [&](auto state) {
                             if (variant == 1 || replaced ||
                                 state != EditPipelineState::EncodingPending)
                                 return;
                             replaced = true;
                             if (variant == 2)
                                 controller.reset();
                             else
                                 controller->requestEdit(replacement);
                         });
        controller->flushPendingExact();
        require(replaced, "the encoding observer reverses or retires the pending request");
        if (variant == 2) {
            require(!controller && busyNotifications.isEmpty(),
                    "state observer destruction stops subsequent busy publication");
        } else if (variant == 0) {
            require(controller && !controller->isBusy() && busyNotifications.isEmpty(),
                    "an encoding state superseded before busy publication emits no duplicate "
                    "non-busy value");
        } else {
            require(controller && !controller->isBusy() &&
                        busyNotifications == QList<bool>({true, false}),
                    "a published busy entry and its nested replacement each publish one value");
        }
    }
}

void workerAndCacheObserversMayDestroyController(QObject& observations, const QString& path,
                                                 const QString& directory) {
    auto workerOptions = options(directory);
    workerOptions.workerExecutablePath = QDir(QCoreApplication::applicationDirPath())
                                             .filePath(
#ifdef Q_OS_WIN
                                                 QStringLiteral("snow_image_edit_worker.exe")
#else
                                                 QStringLiteral("snow_image_edit_worker")
#endif
                                             );
    require(QFileInfo(workerOptions.workerExecutablePath).isExecutable(),
            "the real edit worker is available beside the lifetime test");
    for (const bool cached : {false, true}) {
        for (int variant = 0; variant < 4; ++variant) {
            auto controller = std::make_unique<EditPipelineController>(workerOptions, nullptr);
            bool ready = false;
            QObject::connect(controller.get(), &EditPipelineController::sourceReady, &observations,
                             [&] { ready = true; });
            controller->setSource(path);
            require(QTest::qWaitFor([&] { return ready; }, 5000),
                    "decode the artifact publication source");
            if (cached) {
                controller->requestEdit(settings());
                controller->flushPendingExact();
                require(
                    QTest::qWaitFor([&] { return controller->hasExactResult(settings()); }, 10000),
                    "produce a real cached artifact and comparison preview");
            }
            int encodeNotifications = 0;
            int artifactNotifications = 0;
            int artifactStateNotifications = 0;
            QObject::connect(controller.get(), &EditPipelineController::performanceStageCompleted,
                             &observations, [&](auto, const QString& stage, auto) {
                                 if (stage == QStringLiteral("exact.encode"))
                                     ++encodeNotifications;
                             });
            QObject::connect(controller.get(), &EditPipelineController::stateChanged, &observations,
                             [&](auto state) {
                                 if (state == EditPipelineState::ArtifactReady) {
                                     ++artifactStateNotifications;
                                     require(cached || encodeNotifications == 1,
                                             "completed encoding is measured before artifact "
                                             "state observers can retire its preview");
                                 }
                             });
            QObject::connect(controller.get(), &EditPipelineController::artifactReady,
                             &observations, [&](const auto&) {
                                 ++artifactNotifications;
                                 require(cached || encodeNotifications == 1,
                                         "completed encoding is measured before artifact "
                                         "observers can replace the edit");
                             });
            if (variant == 0)
                QObject::connect(controller.get(), &EditPipelineController::stateChanged,
                                 &observations, [&](auto state) {
                                     if (state == (cached ? EditPipelineState::ExactReady
                                                          : EditPipelineState::ArtifactReady))
                                         controller.reset();
                                 });
            else if (variant == 1)
                QObject::connect(controller.get(), &EditPipelineController::artifactReady,
                                 &observations, [&](const auto&) { controller.reset(); });
            else if (variant == 2)
                QObject::connect(controller.get(), &EditPipelineController::exactReady,
                                 &observations, [&](const auto&) { controller.reset(); });
            else
                QObject::connect(controller.get(),
                                 &EditPipelineController::performanceStageCompleted, &observations,
                                 [&](auto, const QString& stage, auto) {
                                     if (stage == (cached ? QStringLiteral("exact.cache_hit")
                                                          : QStringLiteral("exact.encode")))
                                         controller.reset();
                                 });
            controller->requestEdit(settings());
            if (controller)
                controller->flushPendingExact();
            require(QTest::qWaitFor([&] { return !controller; }, 10000),
                    "real worker and cached publications stop after their observer retires the "
                    "controller");
            if (variant == 3)
                require(artifactStateNotifications == 0 && artifactNotifications == 0,
                        "retirement during completed timing publication suppresses obsolete "
                        "artifact state and result notifications");
        }
    }
}

void equivalentEditsKeepCurrentResultSettings(QObject& observations, const QString& path,
                                              const QString& directory) {
    enum class Boundary { Timing, State, Artifact, PreviewRecovery };
    const auto workerPath = QDir(QCoreApplication::applicationDirPath())
                                .filePath(
#ifdef Q_OS_WIN
                                    QStringLiteral("snow_image_edit_worker.exe")
#else
                                    QStringLiteral("snow_image_edit_worker")
#endif
                                );
    for (const bool cached : {false, true}) {
        for (const auto boundary :
             {Boundary::Timing, Boundary::State, Boundary::Artifact, Boundary::PreviewRecovery}) {
            if (!cached && boundary == Boundary::PreviewRecovery)
                continue;
            for (const bool previewFailure : {false, true}) {
                if (cached && previewFailure)
                    continue;
                auto workerOptions = options(directory);
                workerOptions.workerExecutablePath = workerPath;
                if (previewFailure)
                    workerOptions.workerTestMode = QStringLiteral("preview-failure");
                EditPipelineController controller(workerOptions, nullptr);
                bool sourceReady = false;
                QObject::connect(&controller, &EditPipelineController::sourceReady, &observations,
                                 [&] { sourceReady = true; });
                controller.setSource(path);
                require(QTest::qWaitFor([&] { return sourceReady; }, 5000),
                        "decode the equivalent edit publication source");
                const auto original = settings();
                auto replacement = original;
                replacement.maintainAspectRatio = false;
                if (cached) {
                    controller.requestEdit(original);
                    controller.flushPendingExact();
                    require(
                        QTest::qWaitFor([&] { return controller.hasExactResult(original); }, 10000),
                        "seed the equivalent edit artifact and preview cache");
                    if (boundary == Boundary::PreviewRecovery)
                        controller.clearExactPreviewCacheForBenchmark();
                }
                bool replaced = false;
                EditRequestId replacementId = 0;
                const auto replace = [&] {
                    if (!replaced) {
                        replaced = true;
                        replacementId = controller.requestEdit(replacement);
                    }
                };
                QObject::connect(&controller, &EditPipelineController::performanceStageCompleted,
                                 &observations, [&](auto, const QString& stage, auto) {
                                     if ((boundary == Boundary::Timing &&
                                          stage == (cached ? QStringLiteral("exact.cache_hit")
                                                           : QStringLiteral("exact.encode"))) ||
                                         (boundary == Boundary::PreviewRecovery &&
                                          stage == QStringLiteral("exact.raster_preview_recovery")))
                                         replace();
                                 });
                QObject::connect(&controller, &EditPipelineController::stateChanged, &observations,
                                 [&](auto state) {
                                     if (boundary == Boundary::State &&
                                         state == (cached ? EditPipelineState::ExactReady
                                                          : EditPipelineState::ArtifactReady))
                                         replace();
                                 });
                bool artifactReady = false;
                bool complete = false;
                QObject::connect(
                    &controller, &EditPipelineController::artifactReady, &observations,
                    [&](const EncodedEditResult& result) {
                        const auto expected = replaced ? replacement : original;
                        require(result.settings == expected,
                                "artifact payload settings include earlier equivalent edits");
                        if (boundary == Boundary::Artifact)
                            replace();
                    });
                QObject::connect(
                    &controller, &EditPipelineController::artifactReady, &observations,
                    [&](const EncodedEditResult& result) {
                        artifactReady = true;
                        const auto expected = replaced ? replacement : original;
                        require(controller.hasEncodedArtifact(expected),
                                "the published artifact remains usable for current settings");
                        if (boundary == Boundary::Artifact)
                            require(result.settings == original,
                                    "one artifact notification retains its owned payload for all "
                                    "observers while the current result can change");
                    });
                QObject::connect(
                    &controller, &EditPipelineController::exactReady, &observations,
                    [&](const ExactEditResult& result) {
                        require(result.settings == replacement &&
                                    controller.hasExactResult(replacement),
                                "exact readiness preserves settings changed by earlier observers");
                        complete = true;
                    });
                QObject::connect(&controller, &EditPipelineController::previewUnavailable,
                                 &observations, [&](auto, const auto&) {
                                     require(previewFailure &&
                                                 controller.hasEncodedArtifact(replacement),
                                             "preview failure retains the current usable artifact");
                                     complete = true;
                                 });
                const auto originalId = controller.requestEdit(original);
                controller.flushPendingExact();
                require(QTest::qWaitFor([&] { return artifactReady && complete; }, 10000),
                        "equivalent publication edits complete without replacing their worker");
                require(
                    replaced && replacementId == originalId &&
                        controller.latestRequestId() == originalId &&
                        controller.hasEncodedArtifact(replacement),
                    "equivalent edits retain their request identity and current result settings");
            }
        }
    }
}
} // namespace

int main(int argc, char* argv[]) {
    QApplication app(argc, argv);
    QTemporaryDir directory;
    require(directory.isValid(), "create pipeline lifetime fixture directory");
    const QString path = directory.filePath(QStringLiteral("source.png"));
    QImage image(8, 8, QImage::Format_RGBA8888);
    image.fill(Qt::white);
    require(image.save(path, "PNG"), "write a real source decode fixture");
    QObject observations;
    busyNotificationsSurviveRequestSupersession(observations, directory.path());
    busyEntryObserversMayReverseOrRetireState(observations, directory.path());
    if (app.arguments().contains(QStringLiteral("--busy-publication-only")))
        return 0;
    startupObserversPreserveWorkerReadiness(observations, path, directory.path());
    cancellationObserverDispatchesCurrentEdit(observations, path, directory.path());
    if (app.arguments().contains(QStringLiteral("--worker-protocol-publication-only")))
        return 0;
    requestObserversMayDestroyController(observations, directory.path());
    decodeObserversMayDestroyController(observations, path, directory.path());
    nestedEditSupersedesOuterPublication(observations, directory.path());
    workerAndCacheObserversMayDestroyController(observations, path, directory.path());
    equivalentEditsKeepCurrentResultSettings(observations, path, directory.path());
    std::cout << "snow_image_viewer pipeline notification lifetime tests passed\n";
    return 0;
}
