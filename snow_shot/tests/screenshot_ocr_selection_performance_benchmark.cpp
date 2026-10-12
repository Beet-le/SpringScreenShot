#include "snow_shot/presentation/screenshotocrpresentation.h"
#include "snow_shot/presentation/screenshotocrtextlayer.h"
#include "snow_shot/presentation/screenshotocrtextlayout.h"

#include <QApplication>
#include <QElapsedTimer>
#include <QFontDatabase>
#include <QGlyphRun>
#include <QImage>
#include <QPainter>
#include <QRegion>

#include <algorithm>
#include <cmath>
#include <iostream>
#include <memory>
#include <vector>

namespace {
double percentile(std::vector<double> values, double fraction) {
    std::sort(values.begin(), values.end());
    return values.at(
        static_cast<std::size_t>(std::ceil(fraction * static_cast<double>(values.size()))) - 1);
}

std::shared_ptr<ScreenshotOcrPresentation> presentationFor(int count, const QSize& size,
                                                           bool paragraphs) {
    auto presentation = std::make_shared<ScreenshotOcrPresentation>();
    presentation->selection = QRect(QPoint(), size);
    const int columns = paragraphs ? 20 : (count > 500 ? 25 : 10);
    const int rows = (count + columns - 1) / columns;
    const qreal cellWidth = static_cast<qreal>(size.width()) / columns;
    const qreal cellHeight = static_cast<qreal>(size.height()) / rows;
    for (int index = 0; index < count; ++index) {
        const int row = index / columns;
        const qreal x = (index % columns) * cellWidth + 8;
        const qreal y = row * cellHeight + 2;
        const qreal width = cellWidth - 16;
        const qreal height = paragraphs ? cellHeight - 4 : std::min<qreal>(28, cellHeight - 4);
        const qreal skew = index % 7 == 0 ? height * 0.15 : 0;
        ScreenshotOcrLine line;
        line.text = QStringLiteral("Recognized translation block %1").arg(index);
        line.quad = {
            {x, y}, {x + width, y + skew}, {x + width, y + height}, {x, y + height - skew}};
        if (paragraphs) {
            line.text = QString(24, QChar(0x7530));
            line.paragraph = true;
            const qreal rowHeight = height / 4;
            for (int sourceRow = 0; sourceRow < 3; ++sourceRow) {
                QPolygonF quad(QRectF(x, y + sourceRow * height * 3 / 8,
                                      sourceRow == 2 ? width * 0.6 : width, rowHeight));
                quad.removeLast();
                line.sourceLineQuads.push_back(quad);
            }
        }
        presentation->lines.push_back(line);
    }
    presentation->prepareForRendering();
    return presentation;
}

QPointF originFor(int scenario, const QSize& size) {
    switch (scenario) {
    case 1:
        return QPointF(-10, -10);
    case 2:
        return QPointF(size.width() - 12, size.height() - 12);
    default:
        return QPointF(0, 0);
    }
}

QRectF rectangleFor(int scenario, int run, const QSize& size) {
    const QPointF origin = originFor(scenario, size);
    const qreal offset = run % 2 == 0 ? 0.0 : 0.25;
    switch (scenario) {
    case 0:
        return QRectF(origin, QPointF(120 + (run % 10) * 9, 65 + (run % 7) * 5));
    case 1:
        return QRectF(origin, QPointF(size.width() + 10 + offset, size.height() + 10));
    case 2:
        return QRectF(origin, QPointF(20, 20 + (run % 10) * size.height() / 12.0)).normalized();
    default:
        return QRectF(origin, QPointF(120, 65));
    }
}
} // namespace

int main(int argc, char** argv) {
    QApplication application(argc, argv);
#if defined(Q_OS_WIN)
    if (QFontDatabase::addApplicationFont(QStringLiteral("C:/Windows/Fonts/segoeui.ttf")) < 0 ||
        QFontDatabase::addApplicationFont(QStringLiteral("C:/Windows/Fonts/msyh.ttc")) < 0) {
        return 3;
    }
    QFont font(QStringLiteral("Segoe UI"));
    font.setFamilies({QStringLiteral("Segoe UI"), QStringLiteral("Microsoft YaHei")});
    QApplication::setFont(font);
    // Offscreen FreeType does not enumerate Windows fonts. Verify the paragraph fixture
    // uses real fallback glyphs rather than timing a row of missing-character boxes.
    QTextLayout glyphProbe(QString(24, QChar(0x7530)), font);
    glyphProbe.beginLayout();
    glyphProbe.createLine().setLineWidth(10000);
    glyphProbe.endLayout();
    qsizetype glyphCount = 0;
    for (const QGlyphRun& run : glyphProbe.glyphRuns()) {
        const auto indexes = run.glyphIndexes();
        if (std::any_of(indexes.cbegin(), indexes.cend(),
                        [](quint32 glyph) { return glyph == 0; })) {
            std::cerr << "the paragraph fixture requires real CJK fallback glyphs\n";
            return 3;
        }
        glyphCount += indexes.size();
    }
    if (glyphCount != 24) {
        std::cerr << "the paragraph fixture must shape all its CJK characters\n";
        return 3;
    }
#endif
    const bool representativeOnly =
        QApplication::arguments().contains(QStringLiteral("--representative-only"));
    const char* scenarios[] = {"sparse", "all", "reverse_shrink", "stationary"};
    const char* states[] = {"empty", "mixed", "sparse_mixed"};
    std::cout
        << "blocks,fixture,dpr,scenario,state,move_median_ms,move_p95_ms,full_frame_median_ms,"
           "full_frame_p95_ms,event_drain_median_ms,event_drain_p95_ms,frame_total_median_ms,"
           "frame_total_p95_ms,preview_median_ms,preview_p95_ms,marquee_median_ms,"
           "marquee_p95_ms,begin_median_ms,begin_p95_ms,commit_median_ms,commit_p95_ms,"
           "changed_items,overlay_updates,geometry_updates,glyph_builds,glyph_hits,"
           "glyph_direct_paints,glyph_background_paints,glyph_bytes,preview_builds,"
           "preview_hits,preview_bytes,background_raster_builds,background_raster_hits,"
           "background_raster_bytes,dirty_paint_median_ms,dirty_paint_p95_ms,"
           "interaction_frame_median_ms,interaction_frame_p95_ms,damage_area_dip2\n";
    for (int fixture = 0; fixture < 4; ++fixture) {
        const bool paragraphs = fixture == 3;
        const int count = fixture == 0 ? 20 : (fixture == 2 ? 5000 : 500);
        if (representativeOnly && count != 500) {
            continue;
        }
        const QSize size = paragraphs ? QSize(2560, 1440) : QSize(3840, 2160);
        const qreal dpr = paragraphs ? 1.5 : 1.0;
        auto presentation = presentationFor(count, size, paragraphs);
        if (paragraphs) {
            ScreenshotOcrTextLayout layout;
            QTransform transform;
            if (!configureScreenshotOcrTextLayout(layout, presentation->lines.at(1), QTransform(),
                                                  QApplication::font(), Qt::black, {},
                                                  &transform) ||
                !layout.usesSourceRows()) {
                std::cerr << "the paragraph fixture must exercise fitted source rows\n";
                return 4;
            }
        }
        // The application already owns its exposure paint. Keep this host hidden so event
        // draining resets scene dirtiness without adding a second native frame to the sample.
        QWidget host;
        host.resize(size);
        ScreenshotOcrTextLayer layer(&host);
        layer.setPresentation(presentation);
        layer.synchronize(QTransform(), QRect(QPoint(), size));
        QImage image(size * dpr, QImage::Format_ARGB32_Premultiplied);
        image.setDevicePixelRatio(dpr);
        for (int state = 0; state < 3; ++state) {
            const auto resetBaseline = [&] {
                presentation->clearTextSelection();
                if (state == 1) {
                    presentation->beginTextSelection(ScreenshotOcrTextPosition{count / 3, 1});
                    presentation->updateTextSelection(ScreenshotOcrTextPosition{2 * count / 3, 5});
                    presentation->finishTextSelection();
                } else if (state == 2) {
                    presentation->beginTextSelection(ScreenshotOcrTextPosition{count / 3, 1});
                    presentation->updateTextSelection(ScreenshotOcrTextPosition{count / 3, 5});
                    presentation->finishTextSelection();
                    // A preceding column box selects scattered reading-order entries
                    // while preserving a partial line in another column. Later commits
                    // use sparse state.
                    const QRectF column = presentation->lines.at(1).quad.boundingRect();
                    const QRectF rectangle(column.left() - 1, -10, column.width() + 2,
                                           size.height() + 20);
                    presentation->beginLineBoxSelection(rectangle.topLeft());
                    presentation->updateLineBoxSelection(rectangle);
                    presentation->finishLineBoxSelection();
                }
                layer.updateSelection();
            };
            resetBaseline();
            for (int scenario = 0; scenario < 4; ++scenario) {
                // The committed appearance was already visible before a real pointer press.
                image.fill(Qt::white);
                {
                    QPainter painter(&image);
                    painter.setRenderHints(layer.renderHints());
                    layer.viewport()->render(&painter);
                }
                QApplication::processEvents();
                QImage interactionImage = image.copy();
                static_cast<void>(layer.takeSelectionDamageForTesting());
                const quint64 geometryBefore = layer.geometrySynchronizationCount();
                presentation->beginLineBoxSelection(originFor(scenario, size));
                presentation->updateLineBoxSelection(rectangleFor(scenario, 0, size));
                layer.updateSelection();
                quint64 itemsBefore = layer.selectionItemUpdateCount();
                quint64 overlaysBefore = layer.overlayUpdateCount();
                quint64 glyphBuilds = 0;
                quint64 glyphHits = 0;
                quint64 glyphDirect = 0;
                quint64 glyphBackground = 0;
                quint64 previewBuilds = 0;
                quint64 previewHits = 0;
                quint64 backgroundBuilds = 0;
                quint64 backgroundHits = 0;
                std::vector<double> inputTimes;
                std::vector<double> paintTimes;
                std::vector<double> dirtyPaintTimes;
                std::vector<double> interactionFrameTimes;
                std::vector<double> eventDrainTimes;
                std::vector<double> frameTimes;
                std::vector<double> previewTimes;
                std::vector<double> marqueeTimes;
                qint64 damageArea = 0;
                for (int run = 0; run < 42; ++run) {
                    if (run == 2) {
                        itemsBefore = layer.selectionItemUpdateCount();
                        overlaysBefore = layer.overlayUpdateCount();
                    }
                    const quint64 glyphBuildsBefore = layer.glyphRasterBuildCount();
                    const quint64 glyphHitsBefore = layer.glyphRasterHitCount();
                    const quint64 glyphDirectBefore = layer.glyphDirectPaintCount();
                    const quint64 glyphBackgroundBefore = layer.glyphBackgroundPaintCount();
                    const quint64 previewBuildsBefore = layer.previewRasterBuildCount();
                    const quint64 previewHitsBefore = layer.previewRasterHitCount();
                    const quint64 backgroundBuildsBefore = layer.backgroundRasterBuildCount();
                    const quint64 backgroundHitsBefore = layer.backgroundRasterHitCount();
                    QElapsedTimer timer;
                    timer.start();
                    presentation->updateLineBoxSelection(rectangleFor(scenario, run, size));
                    layer.updateSelection();
                    const double input = static_cast<double>(timer.nsecsElapsed()) / 1000000.0;
                    const QRegion damage = layer.takeSelectionDamageForTesting();
                    timer.restart();
                    if (!damage.isEmpty()) {
                        QPainter interaction(&interactionImage);
                        interaction.setRenderHints(layer.renderHints());
                        interaction.setClipRegion(damage);
                        interaction.fillRect(QRect(QPoint(), size), Qt::white);
                        // Keep the source region at its original viewport position so font
                        // baselines and fractional-DPI phases match a normal widget paint.
                        layer.viewport()->render(&interaction, damage.boundingRect().topLeft(),
                                                 damage);
                    }
                    const double dirtyPaint = static_cast<double>(timer.nsecsElapsed()) / 1000000.0;
                    const quint64 previewBefore = layer.previewPaintNanoseconds();
                    const quint64 marqueeBefore = layer.marqueePaintNanoseconds();
                    timer.restart();
                    image.fill(Qt::white);
                    QPainter painter(&image);
                    painter.setRenderHints(layer.renderHints());
                    layer.viewport()->render(&painter);
                    painter.end();
                    const double paint = static_cast<double>(timer.nsecsElapsed()) / 1000000.0;
                    if (run >= 2) {
                        inputTimes.push_back(input);
                        paintTimes.push_back(paint);
                        dirtyPaintTimes.push_back(dirtyPaint);
                        for (const QRect& rectangle : damage) {
                            damageArea += qint64(rectangle.width()) * rectangle.height();
                        }
                        previewTimes.push_back(
                            static_cast<double>(layer.previewPaintNanoseconds() - previewBefore) /
                            1000000.0);
                        marqueeTimes.push_back(
                            static_cast<double>(layer.marqueePaintNanoseconds() - marqueeBefore) /
                            1000000.0);
                        glyphBuilds += layer.glyphRasterBuildCount() - glyphBuildsBefore;
                        glyphHits += layer.glyphRasterHitCount() - glyphHitsBefore;
                        glyphDirect += layer.glyphDirectPaintCount() - glyphDirectBefore;
                        glyphBackground +=
                            layer.glyphBackgroundPaintCount() - glyphBackgroundBefore;
                        previewBuilds += layer.previewRasterBuildCount() - previewBuildsBefore;
                        previewHits += layer.previewRasterHitCount() - previewHitsBefore;
                        backgroundBuilds +=
                            layer.backgroundRasterBuildCount() - backgroundBuildsBefore;
                        backgroundHits += layer.backgroundRasterHitCount() - backgroundHitsBefore;
                    }
                    // Real viewport frames drain queued scene dirtiness. Keeping it pending
                    // would let subsequent item.update() calls short-circuit unrealistically.
                    // Snapshot measured paint work first; event-loop paints are outside timing.
                    timer.restart();
                    QApplication::processEvents();
                    const double drain = static_cast<double>(timer.nsecsElapsed()) / 1000000.0;
                    if (run >= 2) {
                        eventDrainTimes.push_back(drain);
                        frameTimes.push_back(input + paint + drain);
                        interactionFrameTimes.push_back(input + dirtyPaint + drain);
                    }
                }
                if (interactionImage != image) {
                    std::cerr << "incremental selection painting must match the full frame\n";
                    return 5;
                }
                const quint64 geometryUpdates =
                    layer.geometrySynchronizationCount() - geometryBefore;
                const quint64 itemUpdates = layer.selectionItemUpdateCount() - itemsBefore;
                const quint64 overlayUpdates = layer.overlayUpdateCount() - overlaysBefore;
                const qsizetype glyphBytes = layer.glyphRasterBytes();
                const qsizetype previewBytes = layer.previewRasterBytes();
                const qsizetype backgroundBytes = layer.backgroundRasterBytes();
                if (geometryUpdates != 0 ||
                    (scenario == 3 &&
                     (damageArea != 0 || itemUpdates != 0 || overlayUpdates != 0 ||
                      glyphBuilds != 0 || previewBuilds != 0 || backgroundBuilds != 0))) {
                    std::cerr << "selection update performed unnecessary geometry or "
                                 "stationary work\n";
                    return 2;
                }
                presentation->cancelLineBoxSelection();
                layer.updateSelection();
                std::vector<double> beginTimes;
                std::vector<double> commitTimes;
                for (int run = 0; run < 22; ++run) {
                    // Restore empty, linear, or sparse committed selection outside timing.
                    resetBaseline();
                    QApplication::processEvents();
                    QElapsedTimer timer;
                    timer.start();
                    presentation->beginLineBoxSelection(originFor(scenario, size));
                    layer.updateSelection();
                    const double begin = static_cast<double>(timer.nsecsElapsed()) / 1000000.0;
                    presentation->updateLineBoxSelection(rectangleFor(scenario, run, size));
                    layer.updateSelection();
                    QApplication::processEvents();
                    timer.restart();
                    presentation->finishLineBoxSelection();
                    layer.updateSelection();
                    const double commit = static_cast<double>(timer.nsecsElapsed()) / 1000000.0;
                    QApplication::processEvents();
                    if (run >= 2) {
                        beginTimes.push_back(begin);
                        commitTimes.push_back(commit);
                    }
                }
                if (layer.geometrySynchronizationCount() != geometryBefore) {
                    std::cerr << "gesture begin or release resynchronized text geometry\n";
                    return 2;
                }
                std::cout << count << ',' << (paragraphs ? "source_rows" : "single_rows") << ','
                          << dpr << ',' << scenarios[scenario] << ',' << states[state] << ','
                          << percentile(inputTimes, 0.5) << ',' << percentile(inputTimes, 0.95)
                          << ',' << percentile(paintTimes, 0.5) << ','
                          << percentile(paintTimes, 0.95) << ',' << percentile(eventDrainTimes, 0.5)
                          << ',' << percentile(eventDrainTimes, 0.95) << ','
                          << percentile(frameTimes, 0.5) << ',' << percentile(frameTimes, 0.95)
                          << ',' << percentile(previewTimes, 0.5) << ','
                          << percentile(previewTimes, 0.95) << ',' << percentile(marqueeTimes, 0.5)
                          << ',' << percentile(marqueeTimes, 0.95) << ','
                          << percentile(beginTimes, 0.5) << ',' << percentile(beginTimes, 0.95)
                          << ',' << percentile(commitTimes, 0.5) << ','
                          << percentile(commitTimes, 0.95) << ',' << itemUpdates << ','
                          << overlayUpdates << ',' << geometryUpdates << ',' << glyphBuilds << ','
                          << glyphHits << ',' << glyphDirect << ',' << glyphBackground << ','
                          << glyphBytes << ',' << previewBuilds << ',' << previewHits << ','
                          << previewBytes << ',' << backgroundBuilds << ',' << backgroundHits << ','
                          << backgroundBytes << ',' << percentile(dirtyPaintTimes, 0.5) << ','
                          << percentile(dirtyPaintTimes, 0.95) << ','
                          << percentile(interactionFrameTimes, 0.5) << ','
                          << percentile(interactionFrameTimes, 0.95) << ',' << damageArea << '\n'
                          << std::flush;
                resetBaseline();
            }
        }
    }
    return 0;
}
