#include "snow_shot/presentation/screenshotselectionbackground.h"

#include <QApplication>
#include <QElapsedTimer>
#include <iostream>
#include <stdexcept>

namespace {
void require(bool value, const char* message) {
    if (!value)
        throw std::runtime_error(message);
}

QImage pattern(const QSize& size) {
    QImage image(size, QImage::Format_ARGB32_Premultiplied);
    image.setColorSpace(QColorSpace::SRgb);
    for (int y = 0; y < size.height(); ++y)
        for (int x = 0; x < size.width(); ++x)
            image.setPixelColor(x, y, QColor((x * 3) % 256, (y * 5) % 256, (x + y) % 256));
    return image;
}

void opacityPreservesForegroundAndShadow() {
    const auto capture = pattern({100, 90});
    const QRect selection(15, 18, 60, 50);
    QImage annotated = capture.copy(selection);
    annotated.fill(QColor(210, 100, 30));
    for (bool compound : {false, true}) {
        for (int shadow : {0, 6}) {
            ScreenshotResultStyle style{12, shadow, QColor(20, 30, 40, 160)};
            if (compound)
                style.region = QRegion(QRect(0, 0, 20, 50)) + QRect(40, 0, 20, 50);
            const auto foreground = ScreenshotResultCompositor::compose(annotated, style);
            for (int percent : {0, 1, 50, 100}) {
                style.backgroundOpacityPercent = percent;
                const auto background = renderScreenshotSelectionBackground(
                    {{capture, capture.rect()}}, selection, selection.size(), style);
                const auto actual =
                    ScreenshotResultCompositor::compose(annotated, style, 1, 1, background);
                require(actual.size() == foreground.size(),
                        "opacity must not change output bounds");
                if (percent == 0) {
                    require(
                        background.isNull() && actual == foreground,
                        "zero opacity must preserve the old output without a backdrop allocation");
                    continue;
                }
                QImage expected = foreground.copy();
                QPainter painter(&expected);
                painter.setCompositionMode(QPainter::CompositionMode_DestinationOver);
                painter.setOpacity(percent / 100.0);
                painter.drawImage(
                    QPoint(), capture.copy(selection.adjusted(-shadow, -shadow, shadow, shadow)));
                painter.end();
                require(actual == expected, "corners, gaps and shadow padding must retain capture "
                                            "pixels beneath the foreground");
                require(actual.pixelColor(shadow + 10, shadow + 25) == QColor(210, 100, 30),
                        "background opacity must not fade selected annotations");
                const auto faded =
                    ScreenshotResultCompositor::compose(annotated, style, 1, .5, background);
                require(std::abs(faded.pixelColor(shadow + 10, shadow + 25).alpha() - 128) <= 1,
                        "whole-result opacity must be applied after background composition");
            }
        }
    }
}

void compressionMapsOccupiedAndEmptyBands() {
    const auto capture = pattern({120, 100});
    const QRect selection(10, 10, 80, 60);
    const ScreenshotRegionGeometry region = QRegion(QRect(0, 0, 20, 20)) + QRect(60, 40, 20, 20);
    for (int spacing : {0, 3, 50}) {
        for (int shadow : {0, 4}) {
            ScreenshotResultStyle style{5, shadow, Qt::black, region};
            style.backgroundOpacityPercent = 100;
            const auto plan = makeScreenshotLayoutCompactionPlan(region, selection.size(), 1,
                                                                 {true, spacing}, shadow);
            const auto background = renderScreenshotSelectionBackground(
                {{capture, capture.rect()}}, selection, selection.size(), style, &plan);
            require(background.size() == plan.outputSize + QSize(2 * shadow, 2 * shadow),
                    "background uses compressed result dimensions");
            for (const auto& row : plan.rows) {
                for (const auto& column : plan.columns) {
                    for (int y = row.begin; y < row.end; ++y)
                        for (int x = column.begin; x < column.end; ++x)
                            require(background.pixelColor(
                                        column.destination + x - column.begin + shadow,
                                        row.destination + y - row.begin + shadow) ==
                                        capture.pixelColor(selection.x() + x, selection.y() + y),
                                    "every occupied band intersection must retain original pixels, "
                                    "including empty pockets");
                }
            }
            if (spacing > 0) {
                const int x = plan.columns[0].end + shadow;
                const auto color = background.pixelColor(x + spacing / 2, shadow + 10);
                require(
                    color.alpha() == 255 && color.red() > 90 && color.red() < 220,
                    "new spacing must resample the original gap rather than leave transparency");
            }
            require(background.pixelColor(0, 0) == capture.pixelColor(10 - shadow, 10 - shadow),
                    "outer padding must sample neighboring capture pixels");
        }
    }
}

void sourceAlphaCoverageAndScale() {
    QImage red(40, 40, QImage::Format_ARGB32_Premultiplied);
    red.fill(QColor(200, 20, 30, 128));
    QImage cursor(4, 4, QImage::Format_ARGB32_Premultiplied);
    cursor.fill(Qt::green);
    ScreenshotResultStyle style{8, 3};
    style.backgroundOpacityPercent = 50;
    for (qreal scale : {.5, 1.0, 1.25, 1.5, 2.0}) {
        const QRect selection(-5, -5, 50, 50);
        const QSize pixels(qRound(50 * scale), qRound(50 * scale));
        auto scaled = style;
        scaled.shadowWidth = qRound(3 * scale);
        const auto background = renderScreenshotSelectionBackground(
            {{red, QRectF(0, 0, 40, 40)}, {cursor, QRectF(10, 10, 4, 4)}}, selection, pixels,
            scaled);
        require(background.pixelColor(0, 0).alpha() == 0,
                "unavailable capture pixels must remain transparent");
        const int offset = scaled.shadowWidth;
        require(
            background.pixelColor(offset + qRound(8 * scale), offset + qRound(8 * scale)).alpha() ==
                128,
            "background source alpha must survive projection");
        require(background.pixelColor(offset + qRound(17 * scale), offset + qRound(17 * scale)) ==
                    Qt::green,
                "overlapping capture layers must flatten before applying opacity");
    }
    require(renderScreenshotSelectionBackground({{red, red.rect()}}, red.rect(), red.size(), style,
                                                nullptr, [] { return true; })
                .isNull(),
            "cancelled background rendering must stop without publishing partial pixels");
}

void compressedDisplaySeamsAtFractionalScale() {
    const auto capture = pattern({120, 100});
    const QRect selection(10, 10, 80, 60);
    const ScreenshotRegionGeometry region = QRegion(0, 0, 20, 20) + QRect(60, 40, 20, 20);
    const QList<CanvasExportSource> displays{
        {capture.copy(0, 0, 50, 100), QRectF(0, 0, 50, 100)},
        {capture.copy(50, 0, 70, 100), QRectF(50, 0, 70, 100)}};
    for (qreal scale : {1.0, 1.25, 1.5, 2.0}) {
        const QSize pixels(qRound(80 * scale), qRound(60 * scale));
        for (int spacing : {0, 3, 50}) {
            ScreenshotResultStyle style{0, qRound(4 * scale), Qt::black, region, scale, 100};
            const auto plan = makeScreenshotLayoutCompactionPlan(
                region, pixels, scale, {true, spacing}, style.shadowWidth);
            const auto one = renderScreenshotSelectionBackground({{capture, capture.rect()}},
                                                                 selection, pixels, style, &plan);
            const auto two =
                renderScreenshotSelectionBackground(displays, selection, pixels, style, &plan);
            require(one.size() == two.size(),
                    "display decomposition must not change output bounds");
            for (int y = 0; y < one.height(); ++y) {
                for (int x = 0; x < one.width(); ++x) {
                    const auto a = one.pixelColor(x, y), b = two.pixelColor(x, y);
                    require(
                        a.alpha() == b.alpha() && std::abs(a.red() - b.red()) <= 3 &&
                            std::abs(a.green() - b.green()) <= 3 &&
                            std::abs(a.blue() - b.blue()) <= 3,
                        "compressed display and band boundaries must stay aligned without seams");
                }
            }
        }
    }
}

void curvesHolesAndSparseComposition() {
    QPainterPath path;
    path.setFillRule(Qt::OddEvenFill);
    path.addEllipse(QRectF(0, 0, 200, 200));
    path.addEllipse(QRectF(50, 50, 100, 100));
    path.addEllipse(QRectF(1100, 0, 100, 100));
    ScreenshotResultStyle style{
        0, 8, QColor(0, 0, 0, 100),
        ScreenshotRegionGeometry::fromPath(path, ScreenshotRegionType::Curve)};
    style.backgroundOpacityPercent = 50;
    QImage content(1200, 200, QImage::Format_ARGB32_Premultiplied);
    content.fill(Qt::red);
    QImage capture(1240, 240, QImage::Format_ARGB32_Premultiplied);
    capture.fill(Qt::blue);
    const auto background = renderScreenshotSelectionBackground(
        {{capture, QRectF(-20, -20, 1240, 240)}}, content.rect(), content.size(), style);
    const auto result = ScreenshotResultCompositor::compose(content, style, 1, 1, background);
    for (const QPoint point : {QPoint(108, 108), QPoint(808, 108)}) {
        const auto color = result.pixelColor(point);
        require(color.blue() == 255 && std::abs(color.alpha() - 128) <= 1,
                "curve holes and sparse gaps must show translucent capture pixels");
    }
}

void selectedTransparencyIsNotFilledByTheBackground() {
    QImage content(80, 60, QImage::Format_ARGB32_Premultiplied);
    content.fill(QColor(100, 160, 220, 128));
    for (bool compound : {false, true}) {
        ScreenshotResultStyle style{12, 4, QColor(30, 30, 30, 140)};
        if (compound)
            style.region = QRegion(0, 0, 25, 60) + QRect(55, 0, 25, 60);
        const auto foreground = ScreenshotResultCompositor::compose(content, style);
        for (int percent : {1, 50, 100}) {
            style.backgroundOpacityPercent = percent;
            QImage background(foreground.size(), QImage::Format_ARGB32_Premultiplied);
            background.fill(QColor(100, 160, 220, 128));
            const auto result =
                ScreenshotResultCompositor::compose(content, style, 1, 1, background);
            require(result.pixel(16, 34) == content.pixel(12, 30),
                    "retained background must not increase selected source alpha");
            require(result.pixelColor(0, 0).alpha() >= foreground.pixelColor(0, 0).alpha(),
                    "translucent capture pixels must remain visible outside the selection");
        }
    }
}

void styleCompatibility() {
    ScreenshotResultStyle style{9, 4};
    style.backgroundOpacityPercent = 37;
    const auto bytes = encodeScreenshotResultStyle(style);
    const auto decoded = decodeScreenshotResultStyle(bytes);
    require(decoded && decoded->backgroundOpacityPercent == 37 && decoded->cornerRadius == 9,
            "version-two result styles must round trip opacity");
    require(!decodeScreenshotResultStyle(bytes.left(bytes.size() - 1)),
            "truncated opacity must reject the style");
    style.backgroundOpacityPercent = 101;
    require(!decodeScreenshotResultStyle(encodeScreenshotResultStyle(style)),
            "persisted opacity outside its range must be rejected");
    require(ScreenshotResultCompositor::normalizedStyle(style).backgroundOpacityPercent == 100,
            "runtime opacity must clamp at 100 percent");
}
} // namespace

int main(int argc, char** argv) {
    QApplication app(argc, argv);
    try {
        opacityPreservesForegroundAndShadow();
        compressionMapsOccupiedAndEmptyBands();
        sourceAlphaCoverageAndScale();
        compressedDisplaySeamsAtFractionalScale();
        curvesHolesAndSparseComposition();
        selectedTransparencyIsNotFilledByTheBackground();
        styleCompatibility();
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
    return 0;
}
