#include "presentation/capture/screenshotscrollingpipeline.h"
#include "presentation/capture/windowcaptureexclusion.h"
#include "snow_capture.h"

#include <QApplication>
#include <QElapsedTimer>
#include <QThread>
#include <atomic>
#include <condition_variable>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <mutex>
#include <vector>

struct NativeDestroyGate {
    std::mutex mutex;
    std::condition_variable changed;
    std::atomic_bool entered = false;
    bool released = false;
};
struct SnowCaptureStreamImpl {
    std::mutex mutex;
    std::condition_variable changed;
    bool stopped = false;
    std::shared_ptr<NativeDestroyGate> destroyGate;
};
namespace {
using namespace snow_shot::capture_detail;
using snow_shot::presentation::WindowCaptureExclusion;
std::mutex recordsMutex;
std::vector<QVector<std::uint32_t>> capturedIds;
std::vector<QRect> capturedRegions;
std::atomic<int> liveStreams = 0;
std::atomic<bool> failCreate = false;
std::shared_ptr<NativeDestroyGate> nextDestroyGate;
void require(bool condition, const char* message) {
    if (!condition) {
        std::cerr << message << '\n';
        std::exit(1);
    }
}
template <class Predicate> void until(Predicate ready) {
    QElapsedTimer deadline;
    deadline.start();
    while (!ready() && deadline.elapsed() < 5000) {
        QCoreApplication::processEvents();
        QThread::msleep(2);
    }
    require(ready(), "scrolling worker did not complete in time");
}
std::size_t creates() {
    std::lock_guard lock(recordsMutex);
    return capturedIds.size();
}
void streamRecreationPreservesSuccessfulExclusions(bool overlaySucceeds, bool toolbarSucceeds) {
    QWidget overlay, toolbar;
    overlay.show();
    toolbar.show();
    std::vector<QWidget*> restored;
    const auto baseline = creates();
    {
        WindowCaptureExclusion exclusion([&](QWidget* widget, bool excluded) {
            if (!excluded) {
                require(liveStreams == 0, "restore must happen after native streams are destroyed");
                restored.push_back(widget);
            }
            return widget == &overlay ? overlaySucceeds : toolbarSucceeds;
        });
        exclusion.exclude(&overlay);
        exclusion.exclude(&toolbar);
        const auto ids = exclusion.windowIds([&](QWidget* widget) {
            return std::optional<std::uint32_t>(widget == &overlay ? 17 : 23);
        });
        // The factory must own its arrays while queued on the capture thread.
        auto temporary = ids;
        auto source = nativeScrollingSource(QRect(0, 0, 64, 64), false, temporary);
        temporary.fill(999);
        temporary.clear();
        int errors = 0;
        ScreenshotScrollingPipeline pipeline({}, [&](quint64, QString) { ++errors; });
        pipeline.begin(1, QSize(64, 64), ScreenshotScrollingRecognitionMode::Vertical, source);
        until([&] { return creates() == baseline + 1 && liveStreams == 1; });
        pipeline.begin(2, QSize(64, 64), ScreenshotScrollingRecognitionMode::Horizontal,
                       nativeScrollingSource(QRect(0, 0, 64, 64), false, ids));
        until([&] { return creates() == baseline + 2 && liveStreams == 1; });
        pipeline.pause(2);
        until([] { return liveStreams == 0; });
        require(restored.empty(), "export pause must retain sharing policies");
        pipeline.resume(2, QSize(64, 64),
                        nativeScrollingSource(QRect(128, -256, 64, 64), false, ids));
        until([&] { return creates() == baseline + 3 && liveStreams == 1; });
        require(overlay.isVisible() && toolbar.isVisible(), "exclusion never hides capture UI");
        {
            std::lock_guard lock(recordsMutex);
            require(capturedRegions[baseline + 2] == QRect(128, -256, 64, 64),
                    "resumed native source must capture the moved physical rectangle");
            for (auto index = baseline; index < capturedIds.size(); ++index)
                require(capturedIds[index] == ids, "every recreated stream retains successful IDs");
        }
        failCreate = true;
        pipeline.begin(3, QSize(64, 64), ScreenshotScrollingRecognitionMode::Vertical, source);
        until([&] { return errors == 1; });
        require(liveStreams == 0, "failed replacement releases the previous native stream");
        failCreate = false;
        // Start again so terminal cleanup must stop a live receive operation.
        pipeline.begin(4, QSize(64, 64), ScreenshotScrollingRecognitionMode::Vertical, source);
        until([&] { return creates() == baseline + 5 && liveStreams == 1; });
        bool completed = false;
        pipeline.shutdown(qApp, [&] {
            exclusion.restore();
            completed = true;
        });
        until([&] { return completed; });
    }
    std::vector<QWidget*> expected;
    if (toolbarSucceeds)
        expected.push_back(&toolbar);
    if (overlaySucceeds)
        expected.push_back(&overlay);
    require(restored == expected, "teardown restores successful windows in reverse order");
    require(liveStreams == 0, "teardown joins every stream");
}

void delayedRetirementPreservesRestartedCaptureExclusions() {
    QWidget overlay, toolbar;
    overlay.show();
    toolbar.show();
    bool overlayExcluded = false, toolbarExcluded = false;
    int restores = 0;
    const auto setter = [&](QWidget* widget, bool excluded) {
        bool& state = widget == &overlay ? overlayExcluded : toolbarExcluded;
        state = excluded;
        if (!excluded) {
            require(liveStreams == 0, "final sharing restore must follow native destruction");
            ++restores;
        }
        return true;
    };
    const auto acquire = [&] {
        auto exclusion = std::make_shared<WindowCaptureExclusion>(setter);
        require(exclusion->exclude(&overlay) && exclusion->exclude(&toolbar),
                "each capture acquires both native sharing leases");
        return exclusion;
    };
    const auto ids = [&](const WindowCaptureExclusion& exclusion) {
        return exclusion.windowIds([&](QWidget* widget) {
            return std::optional<std::uint32_t>(widget == &overlay ? 17 : 23);
        });
    };
    auto gate = std::make_shared<NativeDestroyGate>();
    {
        std::lock_guard lock(recordsMutex);
        nextDestroyGate = gate;
    }
    auto retiredExclusion = acquire();
    auto retired = std::make_unique<ScreenshotScrollingPipeline>(
        ScreenshotScrollingPipeline::FrameCallback{}, ScreenshotScrollingPipeline::ErrorCallback{});
    retired->begin(1, QSize(64, 64), ScreenshotScrollingRecognitionMode::Vertical,
                   nativeScrollingSource(QRect(0, 0, 64, 64), false, ids(*retiredExclusion)));
    until([] { return liveStreams == 1; });
    bool retiredCompleted = false, guiWorkDelivered = false;
    retired->shutdown(qApp, [exclusion = std::move(retiredExclusion), &retiredCompleted] {
        exclusion->restore();
        retiredCompleted = true;
    });
    retired.reset();
    QMetaObject::invokeMethod(qApp, [&] { guiWorkDelivered = true; }, Qt::QueuedConnection);
    until([&] { return gate->entered && guiWorkDelivered; });
    require(!retiredCompleted && restores == 0,
            "GUI delivery continues while retired native destruction remains stalled");
    auto restartedExclusion = acquire();
    auto restarted = std::make_unique<ScreenshotScrollingPipeline>(
        ScreenshotScrollingPipeline::FrameCallback{}, ScreenshotScrollingPipeline::ErrorCallback{});
    restarted->begin(2, QSize(64, 64), ScreenshotScrollingRecognitionMode::Horizontal,
                     nativeScrollingSource(QRect(0, 0, 64, 64), false, ids(*restartedExclusion)));
    until([] { return liveStreams == 2; });
    {
        std::lock_guard lock(gate->mutex);
        gate->released = true;
    }
    gate->changed.notify_all();
    until([&] { return retiredCompleted; });
    require(liveStreams == 1 && restores == 0 && overlayExcluded && toolbarExcluded,
            "old completion cannot restore windows leased by a restarted capture");
    bool restartedCompleted = false;
    restarted->shutdown(qApp, [exclusion = std::move(restartedExclusion), &restartedCompleted] {
        exclusion->restore();
        restartedCompleted = true;
    });
    restarted.reset();
    until([&] { return restartedCompleted; });
    require(liveStreams == 0 && restores == 2 && !overlayExcluded && !toolbarExcluded,
            "final completion restores each successful native setting exactly once");
    require(overlay.isVisible() && toolbar.isVisible(),
            "deferred retirement must preserve both window surfaces");
}
} // namespace

extern "C" {
SnowCaptureStream* snow_capture_stream_create_region(const SnowCaptureStreamConfig* config) {
    require(config->version == SNOW_CAPTURE_STREAM_CONFIG_VERSION &&
                config->struct_size == sizeof(*config),
            "scrolling source must use the current capture ABI");
    QVector<std::uint32_t> ids;
    for (size_t i = 0; i < config->exclusions.window_count; ++i)
        ids.push_back(config->exclusions.windows[i]);
    std::shared_ptr<NativeDestroyGate> gate;
    {
        std::lock_guard lock(recordsMutex);
        capturedIds.push_back(ids);
        capturedRegions.emplace_back(config->x, config->y, static_cast<int>(config->width),
                                     static_cast<int>(config->height));
        gate = std::move(nextDestroyGate);
    }
    if (failCreate)
        return nullptr;
    ++liveStreams;
    auto* stream = new SnowCaptureStream;
    stream->destroyGate = std::move(gate);
    return stream;
}
uint8_t snow_capture_stream_receive(SnowCaptureStream* stream, uint32_t timeout,
                                    SnowCaptureStreamEvent* event) {
    std::unique_lock lock(stream->mutex);
    stream->changed.wait_for(lock, std::chrono::milliseconds(timeout),
                             [&] { return stream->stopped; });
    *event = {};
    event->kind =
        stream->stopped ? SNOW_CAPTURE_STREAM_EVENT_ENDED : SNOW_CAPTURE_STREAM_EVENT_TIMEOUT;
    return 1;
}
uint8_t snow_capture_stream_stop(SnowCaptureStream* stream) {
    std::lock_guard lock(stream->mutex);
    stream->stopped = true;
    stream->changed.notify_all();
    return 1;
}
void snow_capture_stream_destroy(SnowCaptureStream* stream) {
    if (stream->destroyGate) {
        auto& gate = *stream->destroyGate;
        std::unique_lock lock(gate.mutex);
        gate.entered = true;
        require(gate.changed.wait_for(lock, std::chrono::seconds(5), [&] { return gate.released; }),
                "native stream retirement blocked GUI work needed to release destruction");
    }
    delete stream;
    --liveStreams;
}
uint8_t snow_capture_stream_stats(const SnowCaptureStream*, SnowCaptureStreamStats* stats) {
    *stats = {};
    return 1;
}
uint8_t snow_capture_stream_set_target_fps(SnowCaptureStream*, uint32_t) {
    return 1;
}
uint8_t snow_capture_stream_frame_info(const SnowCaptureStreamFrame*, SnowCaptureStreamFrameInfo*) {
    return 0;
}
void snow_capture_stream_frame_release(SnowCaptureStreamFrame*) {}
const char* snow_capture_last_error_message() {
    return "injected stream creation failure";
}
}

int main(int argc, char** argv) {
    QApplication app(argc, argv);
    for (bool overlay : {false, true})
        for (bool toolbar : {false, true})
            streamRecreationPreservesSuccessfulExclusions(overlay, toolbar);
    delayedRetirementPreservesRestartedCaptureExclusions();
    return 0;
}
