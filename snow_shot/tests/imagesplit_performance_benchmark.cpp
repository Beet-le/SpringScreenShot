// SPDX-License-Identifier: GPL-3.0-only
#include "snow_shot/presentation/imagesplitdocument.h"
#include "snow_shot/presentation/components/imagespliteditorwidget.h"
#include "theme/theme_manager.h"
#include "../../test-support/memorysnapshot.h"

#include <QApplication>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMouseEvent>
#include <QTemporaryDir>
#include <QTimer>
#include <QWheelEvent>
#include <QtMath>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <future>
#include <iostream>
#include <stdexcept>
#include <thread>
#include <vector>

using namespace snow_shot::presentation;
using namespace snow_shot::presentation::components;
namespace {
void require(bool value, const char* message) {
    if (!value)
        throw std::runtime_error(message);
}
double milliseconds(const QElapsedTimer& timer) {
    return double(timer.nsecsElapsed()) / 1'000'000;
}
double percentile(std::vector<double> values, double fraction) {
    if (values.empty())
        return 0;
    std::sort(values.begin(), values.end());
    const size_t index =
        std::min(values.size() - 1, size_t(std::ceil(double(values.size()) * fraction) - 1));
    return values.at(index);
}

void latencyReport(QJsonObject& report, const QString& prefix, const std::vector<double>& values) {
    report.insert(prefix + QStringLiteral("_samples"), int(values.size()));
    report.insert(prefix + QStringLiteral("_p50_ms"), percentile(values, .50));
    report.insert(prefix + QStringLiteral("_p95_ms"), percentile(values, .95));
    report.insert(prefix + QStringLiteral("_max_ms"),
                  values.empty() ? 0 : *std::max_element(values.begin(), values.end()));
}

class MemorySampler final {
  public:
    MemorySampler()
        : m_peak(quint64(snow::test_support::memorySnapshot().residentBytes)), m_thread([this] {
              while (!m_stop.load()) {
                  m_peak.store(std::max(
                      m_peak.load(), quint64(snow::test_support::memorySnapshot().residentBytes)));
                  std::this_thread::sleep_for(std::chrono::milliseconds(5));
              }
          }) {}
    ~MemorySampler() {
        m_stop.store(true);
        m_thread.join();
    }
    quint64 peak() const {
        return m_peak.load();
    }

  private:
    std::atomic_bool m_stop{false};
    std::atomic<quint64> m_peak;
    std::thread m_thread;
};

void mouse(QWidget* target, QEvent::Type type, QPointF point, Qt::MouseButton button,
           Qt::MouseButtons buttons) {
    QMouseEvent event(type, point, target->mapToGlobal(point.toPoint()), button, buttons,
                      Qt::NoModifier);
    QApplication::sendEvent(target, &event);
}
void wheel(QWidget* target, QPointF point, int delta) {
    QWheelEvent event(point, target->mapToGlobal(point.toPoint()), {}, QPoint(0, delta),
                      Qt::NoButton, Qt::NoModifier, Qt::NoScrollPhase, false);
    QApplication::sendEvent(target, &event);
}

class CanvasSampler final {
  public:
    explicit CanvasSampler(ImageSplitPreviewCanvas* canvas) : m_canvas(canvas) {}
    void render() {
        const qreal ratio = m_canvas->devicePixelRatioF();
        const QSize size(qCeil(m_canvas->width() * ratio), qCeil(m_canvas->height() * ratio));
        if (m_frame.size() != size || m_frame.devicePixelRatio() != ratio) {
            m_frame = QImage(size, QImage::Format_ARGB32_Premultiplied);
            require(!m_frame.isNull(), "canvas benchmark render target allocation succeeds");
            m_frame.setDevicePixelRatio(ratio);
        }
        m_canvas->render(&m_frame);
    }
    std::vector<double> samplePaint(int count) {
        std::vector<double> values;
        values.reserve(size_t(count));
        for (int index = 0; index < count; ++index) {
            QElapsedTimer timer;
            timer.start();
            render();
            values.push_back(milliseconds(timer));
        }
        return values;
    }
    std::vector<double> samplePan(int count) {
        std::vector<double> values;
        values.reserve(size_t(count));
        const QPointF center = QRectF(m_canvas->rect()).center();
        mouse(m_canvas, QEvent::MouseButtonPress, center, Qt::MiddleButton, Qt::MiddleButton);
        for (int index = 0; index < count; ++index) {
            QElapsedTimer timer;
            timer.start();
            const QPointF next = center + (index % 2 == 0 ? QPointF(12, 8) : QPointF{});
            mouse(m_canvas, QEvent::MouseMove, next, Qt::NoButton, Qt::MiddleButton);
            render();
            values.push_back(milliseconds(timer));
        }
        mouse(m_canvas, QEvent::MouseButtonRelease, center, Qt::MiddleButton, Qt::NoButton);
        return values;
    }
    std::vector<double> sampleZoom(int count) {
        std::vector<double> values;
        values.reserve(size_t(count));
        const QPointF center = QRectF(m_canvas->rect()).center();
        for (int index = 0; index < count; ++index) {
            QElapsedTimer timer;
            timer.start();
            wheel(m_canvas, center, index % 2 == 0 ? 120 : -120);
            render();
            values.push_back(milliseconds(timer));
        }
        return values;
    }

  private:
    ImageSplitPreviewCanvas* m_canvas;
    QImage m_frame;
};

void canvasReport(QJsonObject& report, const QImage& image, const QImage& preview) {
    const auto baseline = snow::test_support::memorySnapshot();
    MemorySampler memory;
    ImageSplitDocument document;
    document.setImage(image, preview);
    ImageSplitEditorWidget editor(&document);
    editor.resize(1200, 800);
    editor.show();
    QApplication::processEvents(QEventLoop::AllEvents);
    auto* canvas = editor.canvas();
    canvas->fitToCanvas();
    CanvasSampler sampler(canvas);
    sampler.render();
    report.insert(QStringLiteral("canvas_width"), canvas->width());
    report.insert(QStringLiteral("canvas_height"), canvas->height());
    report.insert(QStringLiteral("canvas_device_pixel_ratio"), canvas->devicePixelRatioF());
    report.insert(QStringLiteral("canvas_preview_width"), preview.width());
    report.insert(QStringLiteral("canvas_preview_height"), preview.height());
    latencyReport(report, QStringLiteral("canvas_fit_paint"), sampler.samplePaint(96));
    latencyReport(report, QStringLiteral("canvas_fit_pan_and_paint"), sampler.samplePan(96));
    latencyReport(report, QStringLiteral("canvas_fit_zoom_and_paint"), sampler.sampleZoom(96));
    ImageSplitSettings settings;
    settings.direction = ImageSplitDirection::Grid;
    settings.rows = 1000;
    settings.columns = 1000;
    require(document.setSettings(settings), "dense canvas benchmark grid is accepted");
    canvas->fitToCanvas();
    sampler.render();
    const auto before = document.state();
    report.insert(QStringLiteral("canvas_dense_grid_outputs"),
                  double(document.layout().outputCount()));
    latencyReport(report, QStringLiteral("canvas_dense_fit_paint"), sampler.samplePaint(96));
    latencyReport(report, QStringLiteral("canvas_dense_fit_pan_and_paint"), sampler.samplePan(96));
    latencyReport(report, QStringLiteral("canvas_dense_fit_zoom_and_paint"),
                  sampler.sampleZoom(96));
    const QPointF center = QRectF(canvas->rect()).center();
    const qreal steps = std::log(1.0 / canvas->zoom()) / std::log(1.15);
    wheel(canvas, center, qRound(steps * 120));
    sampler.render();
    report.insert(QStringLiteral("canvas_detail_zoom"), canvas->zoom());
    latencyReport(report, QStringLiteral("canvas_dense_detail_paint"), sampler.samplePaint(96));
    latencyReport(report, QStringLiteral("canvas_dense_detail_pan_and_paint"),
                  sampler.samplePan(96));
    latencyReport(report, QStringLiteral("canvas_dense_detail_zoom_and_paint"),
                  sampler.sampleZoom(96));
    require(document.state() == before, "canvas pan, zoom and painting preserve all editing state");
    require(!document.canRedo(), "viewport interactions do not introduce redo operations");
    const auto after = snow::test_support::memorySnapshot();
    report.insert(QStringLiteral("canvas_baseline_resident_bytes"), double(baseline.residentBytes));
    report.insert(QStringLiteral("canvas_peak_sampled_resident_bytes"), double(memory.peak()));
    report.insert(QStringLiteral("canvas_peak_resident_delta_bytes"),
                  double(memory.peak() - quint64(baseline.residentBytes)));
    report.insert(QStringLiteral("canvas_after_resident_bytes"), double(after.residentBytes));
}

void extremeCanvasReport(QJsonObject& report, const QImage& source) {
    // Reinterpret the existing contiguous 64MP fixture as an elongated source. Its lifetime spans
    // this call, so the benchmark needs no second full-resolution allocation or preparation copy.
    require(source.bytesPerLine() == qsizetype(source.width()) * 4,
            "extreme canvas fixture has contiguous RGBA pixels");
    const int width = source.width() * source.height();
    const QImage image(source.constBits(), width, 1, source.sizeInBytes(), QImage::Format_RGBA8888);
    require(!image.isNull(), "extreme canvas fixture aliases the prepared source");
    const QImage preview = image.scaled({2048, 1}, Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
    require(!preview.isNull(), "extreme canvas preview is prepared before measurements");
    ImageSplitDocument document;
    document.setImage(image, preview);
    ImageSplitEditorWidget editor(&document);
    editor.resize(1200, 800);
    editor.show();
    QApplication::processEvents(QEventLoop::AllEvents);
    ImageSplitSettings settings;
    settings.count = width;
    QElapsedTimer configuration;
    configuration.start();
    require(document.setSettings(settings), "extreme canvas equal split accepts every pixel");
    report.insert(QStringLiteral("canvas_extreme_configuration_ms"), milliseconds(configuration));
    auto* canvas = editor.canvas();
    canvas->fitToCanvas();
    CanvasSampler sampler(canvas);
    sampler.render();
    const auto original = document.state();
    report.insert(QStringLiteral("canvas_extreme_source_width"), width);
    report.insert(QStringLiteral("canvas_extreme_source_height"), 1);
    report.insert(QStringLiteral("canvas_extreme_outputs"),
                  double(document.layout().outputCount()));
    report.insert(QStringLiteral("canvas_extreme_guide_storage_bytes"),
                  double(document.layout().verticalLines.storageBytes()));
    latencyReport(report, QStringLiteral("canvas_extreme_fit_paint"), sampler.samplePaint(96));
    latencyReport(report, QStringLiteral("canvas_extreme_fit_pan_and_paint"),
                  sampler.samplePan(96));
    latencyReport(report, QStringLiteral("canvas_extreme_fit_zoom_and_paint"),
                  sampler.sampleZoom(96));
    require(document.state() == original, "extreme viewport interactions preserve guide state");
    std::vector<double> edits;
    for (int sample = 0; sample < 96; ++sample) {
        QElapsedTimer timer;
        timer.start();
        require(document.removeLine(true, width / 2 - 1), "dense manual deletion succeeds");
        document.undo();
        require(document.state() == original, "dense undo restores compact geometry");
        edits.push_back(milliseconds(timer));
    }
    latencyReport(report, QStringLiteral("canvas_extreme_delete_and_undo"), edits);
}
QImage fixture(QSize size) {
    QImage image(size, QImage::Format_RGBA8888);
    require(!image.isNull(), "benchmark source allocation succeeds");
    for (int y = 0; y < size.height(); ++y) {
        auto* row = image.scanLine(y);
        for (int x = 0; x < size.width(); ++x) {
            row[qsizetype(x) * 4] = uchar((x * 13 + y * 7) % 256);
            row[qsizetype(x) * 4 + 1] = uchar((x * 3 + y * 17) % 256);
            row[qsizetype(x) * 4 + 2] = uchar((x * 29 + y) % 256);
            row[qsizetype(x) * 4 + 3] = uchar(64 + (x + y) % 192);
        }
    }
    return image;
}

void geometryReport(QJsonObject& report) {
    ImageSplitSettings settings;
    settings.direction = ImageSplitDirection::Grid;
    settings.rows = 2000;
    settings.columns = 32000;
    std::vector<double> elapsed;
    qint64 checksum = 0;
    for (int sample = 0; sample < 100; ++sample) {
        QElapsedTimer timer;
        timer.start();
        const auto layout = calculateImageSplitLayout({32000, 2000}, settings);
        checksum += layout.regionAt(63'999'999).x();
        elapsed.push_back(milliseconds(timer));
        require(layout.isValid() && layout.outputCount() == 64'000'000,
                "dense-grid geometry is valid");
    }
    report.insert(QStringLiteral("dense_grid_output_count"), 64'000'000);
    report.insert(QStringLiteral("dense_grid_geometry_p50_ms"), percentile(elapsed, .50));
    report.insert(QStringLiteral("dense_grid_geometry_p95_ms"), percentile(elapsed, .95));
    const auto dense = calculateImageSplitLayout({32000, 2000}, settings);
    report.insert(
        QStringLiteral("dense_grid_boundary_bytes"),
        double(dense.verticalLines.storageBytes() + dense.horizontalLines.storageBytes()));
    report.insert(QStringLiteral("dense_grid_boundary_runs"),
                  int(dense.verticalLines.runCount() + dense.horizontalLines.runCount()));
    elapsed.clear();
    for (int sample = 0; sample < 100; ++sample) {
        QElapsedTimer timer;
        timer.start();
        require(dense.isValid(), "dense-grid boundary validation succeeds");
        elapsed.push_back(milliseconds(timer));
    }
    report.insert(QStringLiteral("dense_grid_validation_p95_ms"), percentile(elapsed, .95));
    QElapsedTimer lookup;
    lookup.start();
    for (qint64 index = 0; index < 100'000; ++index)
        checksum += dense.regionAt(index * 619 % dense.outputCount()).x();
    report.insert(QStringLiteral("dense_grid_100000_region_lookups_ms"), milliseconds(lookup));
    settings.direction = ImageSplitDirection::Vertical;
    settings.count = 64'000'000;
    elapsed.clear();
    for (int sample = 0; sample < 100; ++sample) {
        QElapsedTimer timer;
        timer.start();
        const auto extreme = calculateImageSplitLayout({64'000'000, 1}, settings);
        require(extreme.isValid() && extreme.outputCount() == 64'000'000 &&
                    extreme.regionAt(63'999'999) == QRect(63'999'999, 0, 1, 1),
                "extreme aspect geometry is valid and reaches its final pixel");
        elapsed.push_back(milliseconds(timer));
    }
    latencyReport(report, QStringLiteral("extreme_axis_geometry"), elapsed);
    const auto extreme = calculateImageSplitLayout({64'000'000, 1}, settings);
    report.insert(QStringLiteral("extreme_axis_boundary_bytes"),
                  double(extreme.verticalLines.storageBytes()));
    report.insert(QStringLiteral("extreme_axis_boundary_runs"),
                  int(extreme.verticalLines.runCount()));
    lookup.restart();
    for (qint64 index = 0; index < 100'000; ++index)
        checksum += extreme.regionAt(index * 619 % extreme.outputCount()).x();
    report.insert(QStringLiteral("extreme_axis_100000_region_lookups_ms"), milliseconds(lookup));
    report.insert(QStringLiteral("geometry_checksum"), double(checksum));
}

void exportReport(QJsonObject& report, const QImage& image, const QString& parent,
                  const QString& prefix, ImageSplitSettings settings) {
    const auto layout = calculateImageSplitLayout(image.size(), settings);
    require(layout.isValid(), "benchmark export layout is valid");
    const auto baseline = snow::test_support::memorySnapshot();
    std::atomic_bool stop{false};
    std::atomic<quint64> peak{quint64(baseline.residentBytes)};
    std::thread sampler([&] {
        while (!stop.load()) {
            peak.store(
                std::max(peak.load(), quint64(snow::test_support::memorySnapshot().residentBytes)));
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
    });
    QElapsedTimer elapsed;
    elapsed.start();
    QElapsedTimer heartbeat;
    heartbeat.start();
    std::vector<double> gaps;
    QTimer timer;
    timer.setInterval(1);
    QObject::connect(&timer, &QTimer::timeout, [&] {
        gaps.push_back(milliseconds(heartbeat));
        heartbeat.restart();
    });
    timer.start();
    auto worker = std::async(std::launch::async,
                             [&] { return exportSplitImages(image, layout, parent, prefix); });
    while (worker.wait_for(std::chrono::milliseconds(0)) != std::future_status::ready) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    const auto result = worker.get();
    timer.stop();
    stop.store(true);
    sampler.join();
    require(result.succeeded(), "benchmark batch export succeeds");
    report.insert(prefix + QStringLiteral("_export_ms"), milliseconds(elapsed));
    report.insert(prefix + QStringLiteral("_outputs"), double(result.savedCount));
    report.insert(prefix + QStringLiteral("_gui_heartbeat_samples"), int(gaps.size()));
    report.insert(prefix + QStringLiteral("_gui_heartbeat_p95_ms"), percentile(gaps, .95));
    report.insert(prefix + QStringLiteral("_gui_heartbeat_max_ms"),
                  gaps.empty() ? 0 : *std::max_element(gaps.begin(), gaps.end()));
    report.insert(prefix + QStringLiteral("_baseline_resident_bytes"),
                  double(baseline.residentBytes));
    report.insert(prefix + QStringLiteral("_peak_resident_bytes"), double(peak.load()));
    report.insert(prefix + QStringLiteral("_peak_resident_delta_bytes"),
                  double(peak.load() - quint64(baseline.residentBytes)));
}
} // namespace

int main(int argc, char** argv) {
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QApplication application(argc, argv);
    adqt::theme::ThemeManager::instance().applyTo(application);
#ifndef NDEBUG
    std::cerr << "Build this benchmark with the windows-msvc-performance Release preset.\n";
    return 1;
#else
    try {
        QTemporaryDir temporary;
        require(temporary.isValid(), "benchmark output directory exists");
        QJsonObject report;
        report.insert(QStringLiteral("build_type"), QStringLiteral("Release"));
        geometryReport(report);
        const QImage image = fixture({8000, 8000});
        report.insert(QStringLiteral("source_pixels"),
                      double(qint64(image.width()) * image.height()));
        report.insert(QStringLiteral("source_bytes"), double(image.sizeInBytes()));
        const auto preview =
            image.scaled({2048, 2048}, Qt::KeepAspectRatio, Qt::SmoothTransformation);
        canvasReport(report, image, preview);
        extremeCanvasReport(report, image);
        ImageSplitSettings settings;
        exportReport(report, image, temporary.path(), QStringLiteral("large_image"), settings);
        settings.direction = ImageSplitDirection::Grid;
        settings.rows = 20;
        settings.columns = 20;
        exportReport(report, image, temporary.path(), QStringLiteral("many_regions"), settings);
        std::cout << QJsonDocument(report).toJson(QJsonDocument::Indented).constData();
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
#endif
}
