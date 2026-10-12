#include "decoding/image_loader.h"
#include "render/rhi_image_window.h"

#include <QApplication>
#include <QCoreApplication>
#include <QImage>
#include <QResizeEvent>
#include <QTemporaryDir>
#include <QtTest/QTest>

#include <array>
#include <cstdlib>
#include <functional>
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

void loaderObserversMayDestroyLoader(QObject& observations, const QString& path) {
    {
        auto loader = std::make_unique<ImageLoader>();
        QObject::connect(loader.get(), &ImageLoader::loadingChanged, &observations,
                         [&](bool loading) {
                             if (loading) {
                                 loader.reset();
                             }
                         });
        loader->load(path);
        require(!loader, "loading-start observer may destroy the loader");
    }
    {
        auto loader = std::make_unique<ImageLoader>();
        int results = 0;
        QObject::connect(loader.get(), &ImageLoader::imageLoaded, &observations,
                         [&] { ++results; });
        QObject::connect(loader.get(), &ImageLoader::loadFailed, &observations, [&] { ++results; });
        QObject::connect(loader.get(), &ImageLoader::loadingChanged, &observations,
                         [&](bool loading) {
                             if (!loading) {
                                 loader.reset();
                             }
                         });
        loader->load(path);
        require(QTest::qWaitFor([&] { return !loader; }, 5000),
                "loading-completion observer must be called");
        require(results == 0, "a destroyed loader must not publish its completed result");
    }
}

void newerLoadsSupersedeNotifications(QObject& observations, const QString& first,
                                      const QString& second) {
    for (const bool replaceAtStart : {false, true}) {
        ImageLoader loader;
        bool replaced = false;
        QStringList delivered;
        QObject::connect(&loader, &ImageLoader::loadingChanged, &observations, [&](bool loading) {
            if (!replaced && loading == replaceAtStart) {
                replaced = true;
                loader.load(second);
            }
        });
        QObject::connect(&loader, &ImageLoader::imageLoaded, &observations,
                         [&](const DecodedImage& image) { delivered.append(image.filePath); });
        loader.load(first);
        require(QTest::qWaitFor([&] { return !loader.isLoading() && !delivered.isEmpty(); }, 5000),
                "replacement load must complete");
        require(replaced && delivered == QStringList{second},
                "a request replaced by its notification must not publish stale image data");
    }
}

void canvasObserversMayDestroyWindow(QObject& observations, const QString& path) {
    DecodedImage image;
    image.filePath = path;
    image.sourceSize = QSize(8, 8);
    image.pixels = QImage(image.sourceSize, QImage::Format_RGBA8888);
    image.pixels.fill(Qt::red);
    const std::array<std::function<void(RhiImageWindow&)>, 9> changes = {
        [&](RhiImageWindow& window) { window.setImage(image); },
        [&](RhiImageWindow& window) {
            window.setThumbnail({image.filePath, image.sourceSize, image.pixels});
        },
        [](RhiImageWindow& window) { window.clearImage(); },
        [](RhiImageWindow& window) { window.setActualSize(); },
        [](RhiImageWindow& window) { window.fitToWindow(); },
        [](RhiImageWindow& window) { window.zoomBy(1.2); },
        [](RhiImageWindow& window) { window.rotateLeft(); },
        [](RhiImageWindow& window) { window.rotateRight(); },
        [](RhiImageWindow& window) {
            QResizeEvent event(QSize(400, 300), QSize(200, 150));
            QCoreApplication::sendEvent(&window, &event);
        }};
    for (const auto& change : changes) {
        auto window = std::make_unique<RhiImageWindow>(nullptr, RhiBackend::null_backend);
        window->setImage(image);
        QObject::connect(window.get(), &RhiImageWindow::zoomChanged, &observations,
                         [&] { window.reset(); });
        change(*window);
        require(!window, "zoom observer may destroy the window after its update is committed");
        QCoreApplication::sendPostedEvents();
    }
    auto window = std::make_unique<RhiImageWindow>(nullptr, RhiBackend::null_backend);
    QObject::connect(window.get(), &RhiImageWindow::comparisonRatioChanged, &observations,
                     [&] { window.reset(); });
    window->setComparisonRatio(0.25);
    require(!window, "comparison observer may destroy the window");
    QCoreApplication::sendPostedEvents();
}

DecodedImage renderFixture(const QString& path) {
    DecodedImage image;
    image.filePath = path;
    image.sourceSize = QSize(8, 8);
    image.pixels = QImage(image.sourceSize, QImage::Format_RGBA8888);
    image.pixels.fill(Qt::red);
    return image;
}

void renderOneFrame(RhiImageWindow& window) {
    QEvent update(QEvent::UpdateRequest);
    QCoreApplication::sendEvent(&window, &update);
}

std::unique_ptr<RhiImageWindow> renderWindow(const DecodedImage& image) {
    auto window = std::make_unique<RhiImageWindow>(nullptr, RhiBackend::null_backend);
    window->resize(96, 64);
    window->setImage(image);
    window->show();
    return window;
}

void renderedNotificationsMayRetireOrReplaceWindow(QObject& observations, const QString& path) {
    const DecodedImage image = renderFixture(path);
    for (const bool comparison : {false, true}) {
        auto window = renderWindow(image);
        if (comparison) {
            auto preview = image;
            preview.filePath += QStringLiteral(".comparison");
            window->setComparisonImage(preview);
        }
        int submissions = 0;
        if (comparison) {
            QObject::connect(window.get(), &RhiImageWindow::comparisonFrameSubmitted, &observations,
                             [&] {
                                 ++submissions;
                                 window.reset();
                             });
        } else {
            QObject::connect(window.get(), &RhiImageWindow::imageFrameSubmitted, &observations,
                             [&] {
                                 ++submissions;
                                 window.reset();
                             });
        }
        renderOneFrame(*window);
        require(!window && submissions == 1,
                "frame observers may retire the window after all render state is committed");
    }

    auto window = renderWindow(image);
    auto preview = image;
    preview.filePath += QStringLiteral(".comparison");
    window->setComparisonImage(preview);
    int obsoleteComparisons = 0;
    QString deliveredPath;
    QObject::connect(window.get(), &RhiImageWindow::imageFrameSubmitted, &observations,
                     [&](const QString&, bool) { window->clearImage(); });
    QObject::connect(window.get(), &RhiImageWindow::imageFrameSubmitted, &observations,
                     [&](const QString& submittedPath, bool) { deliveredPath = submittedPath; });
    QObject::connect(window.get(), &RhiImageWindow::comparisonFrameSubmitted, &observations,
                     [&] { ++obsoleteComparisons; });
    renderOneFrame(*window);
    require(deliveredPath == path && obsoleteComparisons == 0,
            "source replacement preserves the current payload and retires remaining frame events");

    window = renderWindow(image);
    // A valid document without CPU pixels cannot create its initial image textures.
    auto invalid = image;
    invalid.pixels = {};
    window->setImage(invalid);
    int errors = 0;
    QObject::connect(window.get(), &RhiImageWindow::renderError, &observations,
                     [&](const QString&) {
                         ++errors;
                         window.reset();
                     });
    renderOneFrame(*window);
    require(!window && errors == 1, "resource errors may retire the window after frame cleanup");

#if defined(Q_OS_WIN)
    window = renderWindow(image);
    int frameFollowups = 0;
    QObject::connect(window.get(), &RhiImageWindow::staticTextureResident, &observations,
                     [&](const QString&) { window.reset(); });
    QObject::connect(window.get(), &RhiImageWindow::imageFrameSubmitted, &observations,
                     [&] { ++frameFollowups; });
    renderOneFrame(*window);
    require(!window && frameFollowups == 0,
            "GPU residency notification deletion must retire subsequent frame notifications");
#endif
}

void gpuNotificationsRespectFrameAndReadbackLifetime(QObject& observations, const QString& path) {
#if defined(Q_OS_WIN)
    const DecodedImage image = renderFixture(path);
    EditExportSettings settings;
    settings.sourceSize = image.sourceSize;
    settings.width = image.sourceSize.width();
    settings.height = image.sourceSize.height();
    for (const bool retireAtCache : {false, true}) {
        auto window = renderWindow(image);
        renderOneFrame(*window);
        require(window->hasGpuOnlyStaticImage(), "null renderer must retain the fixture texture");
        int mainReadbacks = 0;
        int performances = 0;
        QObject::connect(window.get(), &RhiImageWindow::mainImageReadbackReady, &observations,
                         [&](quint64, const QImage& pixels) {
                             require(pixels.size() == image.sourceSize,
                                     "cache observer can read back after the active frame ends");
                             ++mainReadbacks;
                         });
        QObject::connect(window.get(), &RhiImageWindow::editResizeResourceCacheResult,
                         &observations, [&](quint64, bool) {
                             window->requestMainImageReadback(10);
                             if (retireAtCache)
                                 window.reset();
                         });
        QObject::connect(window.get(), &RhiImageWindow::editPerformanceStageCompleted,
                         &observations, [&](quint64, const QString&, qint64) {
                             ++performances;
                             window.reset();
                         });
        window->requestEditResize(1, settings);
        renderOneFrame(*window);
        require(!window && mainReadbacks == 1 && performances == (retireAtCache ? 0 : 1),
                "GPU observers run after frame completion and may retire remaining notifications");
    }

    for (const bool retireAtPerformance : {false, true}) {
        auto window = renderWindow(image);
        renderOneFrame(*window);
        int readbacks = 0;
        QObject::connect(window.get(), &RhiImageWindow::editPerformanceStageCompleted,
                         &observations, [&](quint64, const QString& stage, qint64) {
                             if (retireAtPerformance &&
                                 stage == QStringLiteral("exact.gpu_readback"))
                                 window.reset();
                         });
        QObject::connect(window.get(), &RhiImageWindow::editResizeReadbackReady, &observations,
                         [&](quint64, const GpuRasterResult&) {
                             ++readbacks;
                             window.reset();
                         });
        window->requestEditResize(1, settings);
        renderOneFrame(*window);
        require(QTest::qWaitFor([&] { return !window; }, 5000),
                "readback callbacks must complete without fixed frame delays");
        require(readbacks == (retireAtPerformance ? 0 : 1),
                "readback performance retirement must stop its result publication");
    }
#else
    Q_UNUSED(observations)
    Q_UNUSED(path)
#endif
}

void comparisonChangesPreserveIndependentFrameNotifications(QObject& observations,
                                                            const QString& path) {
    const DecodedImage image = renderFixture(path);
    auto preview = image;
    preview.filePath += QStringLiteral(".comparison");
    auto replacement = image;
    replacement.filePath += QStringLiteral(".replacement");
    auto window = renderWindow(image);
    window->setComparisonImage(preview);
    int imageSubmissions = 0;
    QStringList comparisonSubmissions;
    QObject::connect(window.get(), &RhiImageWindow::imageFrameSubmitted, &observations,
                     [&](const QString&, bool) {
                         ++imageSubmissions;
                         window->setComparisonImage(replacement);
                     });
    QObject::connect(window.get(), &RhiImageWindow::comparisonFrameSubmitted, &observations,
                     [&](const QString& filePath) { comparisonSubmissions.append(filePath); });
    renderOneFrame(*window);
    require(imageSubmissions == 1 && comparisonSubmissions.isEmpty(),
            "image frame observers may supersede only the queued comparison submission");
    renderOneFrame(*window);
    require(imageSubmissions == 1 && comparisonSubmissions == QStringList{replacement.filePath},
            "replacement comparison retains its own pending frame submission");

#if defined(Q_OS_WIN)
    for (const bool clear : {false, true}) {
        window = renderWindow(image);
        window->setComparisonImage(preview);
        imageSubmissions = 0;
        comparisonSubmissions.clear();
        QObject::connect(window.get(), &RhiImageWindow::staticTextureResident, &observations,
                         [&](const QString&) {
                             if (clear)
                                 window->clearComparison();
                             else
                                 window->setComparisonImage(replacement);
                         });
        QObject::connect(window.get(), &RhiImageWindow::imageFrameSubmitted, &observations,
                         [&](const QString&, bool) { ++imageSubmissions; });
        QObject::connect(window.get(), &RhiImageWindow::comparisonFrameSubmitted, &observations,
                         [&](const QString& filePath) { comparisonSubmissions.append(filePath); });
        renderOneFrame(*window);
        require(imageSubmissions == 1 && comparisonSubmissions.isEmpty(),
                "comparison mutations during residency preserve the independent image event");
        renderOneFrame(*window);
        require(imageSubmissions == 1 &&
                    comparisonSubmissions ==
                        (clear ? QStringList{} : QStringList{replacement.filePath}),
                "residency comparison replacement publishes exactly its own submitted frame");
    }
#endif
}
} // namespace

int main(int argc, char** argv) {
    QApplication application(argc, argv);
    QTemporaryDir directory;
    require(directory.isValid(), "temporary image directory must exist");
    const QString first = directory.filePath(QStringLiteral("first.png"));
    const QString second = directory.filePath(QStringLiteral("second.png"));
    QImage image(8, 8, QImage::Format_RGBA8888);
    image.fill(Qt::red);
    require(image.save(first) && image.save(second), "notification fixtures must be saved");
    loaderObserversMayDestroyLoader(application, first);
    newerLoadsSupersedeNotifications(application, first, second);
    canvasObserversMayDestroyWindow(application, first);
    renderedNotificationsMayRetireOrReplaceWindow(application, first);
    comparisonChangesPreserveIndependentFrameNotifications(application, first);
    gpuNotificationsRespectFrameAndReadbackLifetime(application, first);
    return EXIT_SUCCESS;
}
