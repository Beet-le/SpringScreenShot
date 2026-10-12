#include "snow_canvas_magnifier_renderer.h"

#include "snow_canvas_render_geometry.h"
#include "snow_canvas_renderer.h"
#include "snow_canvas_filter_tile_cache.h"
#include "snow_draw_engine_qt/snow_canvas_image.h"

#include <QPainter>
#include <QPaintEngine>
#include <QTransform>
#include <QLinearGradient>
#include <QBrush>
#include <QPolygonF>
#include <QElapsedTimer>

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <utility>

namespace snow_canvas_magnifier_renderer {
namespace {

thread_local RenderDiagnostics g_magnifierDiagnostics;
thread_local bool g_stageTimingsEnabled = false;
thread_local bool g_benchmarkSolidFill = false;
thread_local bool g_benchmarkDisableAntialiasing = false;
constexpr std::size_t kMaximumContourPoints = 8192;

double pointSegmentDistanceSquared(const QPointF& point, const QPointF& start, const QPointF& end) {
    const QPointF delta = end - start;
    const double lengthSquared = QPointF::dotProduct(delta, delta);
    const double fraction =
        lengthSquared > 0.0
            ? std::clamp(QPointF::dotProduct(point - start, delta) / lengthSquared, 0.0, 1.0)
            : 0.0;
    const QPointF distance = point - (start + delta * fraction);
    return QPointF::dotProduct(distance, distance);
}

bool flattenCubic(std::vector<QPointF>& points, const QPointF& start, const QPointF& control1,
                  const QPointF& control2, const QPointF& end, double toleranceSquared,
                  unsigned depth = 0) {
    if (points.size() > kMaximumContourPoints - 3)
        return false;
    // The Bezier convex hull bounds its distance from the complete chord.
    // Limit recursion for pathological finite dimensions; normal display scales
    // reach the physical-pixel tolerance well before this guard.
    if (std::max(pointSegmentDistanceSquared(control1, start, end),
                 pointSegmentDistanceSquared(control2, start, end)) <= toleranceSquared) {
        // Leaf control points form a conservative hull around the original
        // curve. Their outward error is bounded by the same chord tolerance.
        points.push_back(control1);
        points.push_back(control2);
        points.push_back(end);
        return true;
    }
    if (depth == 16)
        return false;
    const QPointF a = (start + control1) / 2.0;
    const QPointF b = (control1 + control2) / 2.0;
    const QPointF c = (control2 + end) / 2.0;
    const QPointF d = (a + b) / 2.0;
    const QPointF e = (b + c) / 2.0;
    const QPointF middle = (d + e) / 2.0;
    return flattenCubic(points, start, a, d, middle, toleranceSquared, depth + 1) &&
           flattenCubic(points, middle, e, c, end, toleranceSquared, depth + 1);
}

bool appendContourPoints(std::vector<QPointF>& points, const QPainterPath& path,
                         double physicalScale) {
    constexpr double physicalTolerance = 0.125;
    const double tolerance = physicalTolerance / physicalScale;
    QPointF previous;
    for (int index = 0; index < path.elementCount(); ++index) {
        const auto element = path.elementAt(index);
        const QPointF point(element.x, element.y);
        // Squared distances and hull cross products must remain representable.
        if (!std::isfinite(point.x()) || !std::isfinite(point.y()) || std::abs(point.x()) > 1e150 ||
            std::abs(point.y()) > 1e150)
            return false;
        if (element.isCurveTo() && index + 2 < path.elementCount()) {
            const auto control2 = path.elementAt(++index);
            const auto end = path.elementAt(++index);
            previous = QPointF(end.x, end.y);
            if (!std::isfinite(control2.x) || !std::isfinite(control2.y) || !std::isfinite(end.x) ||
                !std::isfinite(end.y) ||
                std::max({std::abs(control2.x), std::abs(control2.y), std::abs(end.x),
                          std::abs(end.y)}) > 1e150 ||
                !flattenCubic(points, points.back(), point, QPointF(control2.x, control2.y),
                              previous, tolerance * tolerance))
                return false;
        } else {
            if (points.size() >= kMaximumContourPoints)
                return false;
            previous = point;
            points.push_back(previous);
        }
    }
    return true;
}

double crossProduct(const QPointF& origin, const QPointF& a, const QPointF& b) {
    const QPointF first = a - origin;
    const QPointF second = b - origin;
    return first.x() * second.y() - first.y() * second.x();
}

struct ContourSegment {
    std::array<QPointF, 4> points;
    bool cubic = false;
};

struct ContourPoint {
    QPointF point;
    std::size_t segment = 0;
    double fraction = 0.0;
};

std::vector<ContourSegment> contourSegments(const QPainterPath& path) {
    std::vector<ContourSegment> segments;
    QPointF previous;
    for (int index = 0; index < path.elementCount(); ++index) {
        const auto element = path.elementAt(index);
        const QPointF point(element.x, element.y);
        if (element.isMoveTo()) {
            previous = point;
        } else if (element.isCurveTo() && index + 2 < path.elementCount()) {
            const auto control2 = path.elementAt(++index);
            const auto end = path.elementAt(++index);
            segments.push_back(
                {{previous, point, QPointF(control2.x, control2.y), QPointF(end.x, end.y)}, true});
            previous = QPointF(end.x, end.y);
        } else if (element.isLineTo()) {
            segments.push_back({{previous, previous, point, point}, false});
            previous = point;
        }
    }
    return segments;
}

std::pair<ContourSegment, ContourSegment> splitSegment(const ContourSegment& segment,
                                                       double fraction) {
    const auto interpolate = [fraction](const QPointF& a, const QPointF& b) {
        return a + (b - a) * fraction;
    };
    const auto& p = segment.points;
    const QPointF a = interpolate(p[0], p[1]);
    const QPointF b = interpolate(p[1], p[2]);
    const QPointF c = interpolate(p[2], p[3]);
    const QPointF d = interpolate(a, b);
    const QPointF e = interpolate(b, c);
    const QPointF middle = interpolate(d, e);
    return {{{p[0], a, d, middle}, true}, {{middle, e, c, p[3]}, true}};
}

QPointF segmentPoint(const ContourSegment& segment, double fraction) {
    if (fraction == 0.0)
        return segment.points[0];
    if (fraction == 1.0)
        return segment.points[3];
    if (!segment.cubic)
        return segment.points[0] + (segment.points[3] - segment.points[0]) * fraction;
    return splitSegment(segment, fraction).first.points[3];
}

struct ContourSupport {
    ContourPoint tag;
    double distance = -std::numeric_limits<double>::infinity();
};

ContourSupport contourSupport(const std::vector<ContourSegment>& segments, const QPointF& center,
                              const QPointF& normal) {
    ContourSupport best;
    for (std::size_t index = 0; index < segments.size(); ++index) {
        ContourSegment local = segments[index];
        for (QPointF& point : local.points)
            point -= center;
        const auto consider = [&](double fraction) {
            if (fraction < 0.0 || fraction > 1.0 || !std::isfinite(fraction))
                return;
            const QPointF point = segmentPoint(local, fraction);
            const double distance = QPointF::dotProduct(normal, point);
            if (distance > best.distance) {
                best.distance = distance;
                best.tag = {point + center, index, fraction};
                if (fraction == 1.0) {
                    best.tag.segment = (index + 1) % segments.size();
                    best.tag.fraction = 0.0;
                    best.tag.point = segments[best.tag.segment].points[0];
                }
            }
        };
        consider(0.0);
        consider(1.0);
        if (!local.cubic)
            continue;
        const auto& p = local.points;
        double a = QPointF::dotProduct(normal, p[3] - p[2] * 3.0 + p[1] * 3.0 - p[0]);
        double b = QPointF::dotProduct(normal, (p[2] - p[1] * 2.0 + p[0]) * 2.0);
        double c = QPointF::dotProduct(normal, p[1] - p[0]);
        const double normalization = std::max({std::abs(a), std::abs(b), std::abs(c)});
        if (normalization == 0.0 || !std::isfinite(normalization))
            continue;
        a /= normalization;
        b /= normalization;
        c /= normalization;
        constexpr double epsilon = 16.0 * std::numeric_limits<double>::epsilon();
        if (std::abs(a) <= epsilon) {
            if (std::abs(b) > epsilon)
                consider(-c / b);
            continue;
        }
        const double discriminant = b * b - 4.0 * a * c;
        if (discriminant < -epsilon)
            continue;
        const double root = std::sqrt(std::max(0.0, discriminant));
        const double q = -0.5 * (b + std::copysign(root, b));
        consider(q / a);
        if (q != 0.0)
            consider(c / q);
    }
    return best;
}

bool positiveContour(const std::vector<ContourSegment>& segments) {
    if (segments.empty())
        return false;
    const QPointF origin = segments.front().points[0];
    double area = 0.0;
    for (const auto& segment : segments)
        area += crossProduct(origin, segment.points[0], segment.points[3]);
    return std::isfinite(area) && area > 0.0;
}

void appendReverseSegment(QPainterPath& path, ContourSegment segment, double from, double to) {
    if (to <= from)
        return;
    if (!segment.cubic) {
        path.lineTo(segment.points[0] + (segment.points[3] - segment.points[0]) * from);
        return;
    }
    if (to < 1.0)
        segment = splitSegment(segment, to).first;
    if (from > 0.0)
        segment = splitSegment(segment, from / to).second;
    path.cubicTo(segment.points[2], segment.points[1], segment.points[0]);
}

void appendReverseContour(QPainterPath& path, const std::vector<ContourSegment>& segments,
                          const ContourPoint& start, const ContourPoint& end) {
    std::size_t index = start.segment;
    if (index == end.segment && start.fraction >= end.fraction) {
        appendReverseSegment(path, segments[index], end.fraction, start.fraction);
        return;
    }
    appendReverseSegment(path, segments[index], 0.0, start.fraction);
    index = (index + segments.size() - 1) % segments.size();
    while (index != end.segment) {
        appendReverseSegment(path, segments[index], 0.0, 1.0);
        index = (index + segments.size() - 1) % segments.size();
    }
    appendReverseSegment(path, segments[index], end.fraction, 1.0);
}

bool appendRasterFriendlyLine(QPainterPath& path, const QPointF& end, double physicalScale,
                              std::size_t reservedElements) {
    const QPointF start = path.currentPosition();
    const QPointF delta = end - start;
    constexpr double maximumPhysicalLength = 16.0;
    const double required =
        std::ceil(std::hypot(delta.x(), delta.y()) * physicalScale / maximumPhysicalLength);
    const std::size_t existing = static_cast<std::size_t>(path.elementCount());
    if (!std::isfinite(required) || existing + reservedElements >= kMaximumContourPoints ||
        required > static_cast<double>(kMaximumContourPoints - existing - reservedElements))
        return false;
    const auto count = static_cast<std::size_t>(std::max(1.0, required));
    // Qt's antialiased rasterizer walks each edge again for every vertical band.
    // Collinear pieces let it reject edges outside that band without traversing
    // a long diagonal. The continuous vector boundary and cap curves stay exact.
    for (std::size_t index = 1; index < count; ++index)
        path.lineTo(start + delta * (static_cast<double>(index) / static_cast<double>(count)));
    path.lineTo(end);
    return true;
}

QPainterPath curvedBridge(const SnowSceneDisplayItem& item, const QPainterPath& source,
                          const QPainterPath& lens, double physicalScale) {
    const double factor = item.magnifier.magnification_factor;
    if (factor < 1.0 || item.width != item.magnifier.source_width * factor ||
        item.height != item.magnifier.source_height * factor)
        return {};
    const auto sourceSegments = contourSegments(source);
    const auto lensSegments = contourSegments(lens);
    if (sourceSegments.size() != lensSegments.size() || !positiveContour(sourceSegments) ||
        !positiveContour(lensSegments))
        return {};
    const QPointF lensCenter(item.center_x, item.center_y);
    const QPointF sourceCenter(item.magnifier.source_center_x, item.magnifier.source_center_y);
    const QPointF displacement = lensCenter - sourceCenter;
    const double length = std::hypot(displacement.x(), displacement.y());
    if (!std::isfinite(length) || length == 0.0)
        return {};
    std::array<ContourPoint, 2> lensTangents;
    if (factor == 1.0) {
        const QPointF normal(displacement.y() / length, -displacement.x() / length);
        lensTangents[0] = contourSupport(lensSegments, lensCenter, normal).tag;
        lensTangents[1] = contourSupport(lensSegments, lensCenter, -normal).tag;
    } else {
        const QRectF sourceBounds = source.boundingRect();
        const QRectF lensBounds = lens.boundingRect();
        QPointF separatingNormal;
        if (sourceBounds.right() <= lensBounds.left())
            separatingNormal = QPointF(-1.0, 0.0);
        else if (sourceBounds.left() >= lensBounds.right())
            separatingNormal = QPointF(1.0, 0.0);
        else if (sourceBounds.bottom() <= lensBounds.top())
            separatingNormal = QPointF(0.0, -1.0);
        else if (sourceBounds.top() >= lensBounds.bottom())
            separatingNormal = QPointF(0.0, 1.0);
        else
            return {};
        const double seed = std::atan2(separatingNormal.y(), separatingNormal.x());
        const double scaleDifference = (factor - 1.0) / factor;
        constexpr double pi = 3.14159265358979323846;
        for (std::size_t side = 0; side < lensTangents.size(); ++side) {
            double inside = seed;
            double outside = seed + (side == 0 ? pi : -pi);
            // Support equality describes the exact common tangent. A separating
            // AABB axis brackets each of the two roots in opposite half-circles.
            for (unsigned iteration = 0; iteration < 48; ++iteration) {
                const double middle = (inside + outside) / 2.0;
                const QPointF normal(std::cos(middle), std::sin(middle));
                const auto support = contourSupport(lensSegments, lensCenter, normal);
                const double residual =
                    QPointF::dotProduct(normal, displacement) + scaleDifference * support.distance;
                if (residual < 0.0)
                    inside = middle;
                else
                    outside = middle;
            }
            const double angle = (inside + outside) / 2.0;
            lensTangents[side] =
                contourSupport(lensSegments, lensCenter, QPointF(std::cos(angle), std::sin(angle)))
                    .tag;
        }
    }
    auto sourceTangents = lensTangents;
    for (auto& tangent : sourceTangents)
        tangent.point = segmentPoint(sourceSegments[tangent.segment], tangent.fraction);
    QPainterPath bridge;
    const auto reservedElements =
        static_cast<std::size_t>(source.elementCount() + lens.elementCount());
    bridge.moveTo(sourceTangents[0].point);
    if (!appendRasterFriendlyLine(bridge, lensTangents[0].point, physicalScale, reservedElements))
        return {};
    appendReverseContour(bridge, lensSegments, lensTangents[0], lensTangents[1]);
    if (!appendRasterFriendlyLine(bridge, sourceTangents[1].point, physicalScale, reservedElements))
        return {};
    appendReverseContour(bridge, sourceSegments, sourceTangents[1], sourceTangents[0]);
    bridge.closeSubpath();
    return bridge;
}

QPainterPath convexHull(std::vector<QPointF> points) {
    std::sort(points.begin(), points.end(), [](const QPointF& a, const QPointF& b) {
        return a.x() < b.x() || (a.x() == b.x() && a.y() < b.y());
    });
    points.erase(std::unique(points.begin(), points.end()), points.end());
    if (points.size() < 3)
        return {};
    QPolygonF hull;
    hull.reserve(static_cast<qsizetype>(points.size() * 2));
    for (const QPointF& point : points) {
        while (hull.size() >= 2 && crossProduct(hull[hull.size() - 2], hull.back(), point) <= 0.0)
            hull.removeLast();
        hull.append(point);
    }
    const qsizetype lowerSize = hull.size();
    for (auto it = points.rbegin() + 1; it != points.rend(); ++it) {
        while (hull.size() > lowerSize &&
               crossProduct(hull[hull.size() - 2], hull.back(), *it) <= 0.0)
            hull.removeLast();
        hull.append(*it);
    }
    QPainterPath path;
    path.addPolygon(hull);
    path.closeSubpath();
    return path;
}

double maximumLinearScale(const QTransform& transform) {
    const double normalization = std::max({std::abs(transform.m11()), std::abs(transform.m12()),
                                           std::abs(transform.m21()), std::abs(transform.m22())});
    if (!std::isfinite(normalization) || normalization == 0.0)
        return normalization;
    const double a = transform.m11() / normalization;
    const double b = transform.m12() / normalization;
    const double c = transform.m21() / normalization;
    const double d = transform.m22() / normalization;
    const double first = a * a + b * b;
    const double second = c * c + d * d;
    const double mixed = a * c + b * d;
    return normalization *
           std::sqrt((first + second + std::hypot(first - second, 2.0 * mixed)) / 2.0);
}

double minimumLinearScale(const QTransform& transform, double maximum) {
    const double normalization = std::max({std::abs(transform.m11()), std::abs(transform.m12()),
                                           std::abs(transform.m21()), std::abs(transform.m22())});
    if (!std::isfinite(normalization) || normalization == 0.0)
        return normalization;
    const double determinant =
        std::abs((transform.m11() / normalization) * (transform.m22() / normalization) -
                 (transform.m12() / normalization) * (transform.m21() / normalization));
    return determinant / (maximum / normalization) * normalization;
}

void rebuildExteriorMasks(SnowCanvasSceneItem::MagnifierGeometryCache& cache, double guard) {
    QPainterPath enclosing;
    enclosing.addRect(cache.hull.boundingRect().adjusted(-guard, -guard, guard, guard));
    enclosing.setFillRule(Qt::OddEvenFill);
    cache.outsideSource = {};
    cache.outsideLens = {};
    cache.outsideBoth = {};
    if (!cache.source.boundingRect().intersects(cache.lens.boundingRect())) {
        cache.outsideBoth = enclosing;
        cache.outsideBoth.addPath(cache.source);
        cache.outsideBoth.addPath(cache.lens);
    } else {
        cache.outsideSource = enclosing;
        cache.outsideSource.addPath(cache.source);
        cache.outsideLens = enclosing;
        cache.outsideLens.addPath(cache.lens);
    }
    cache.exteriorGuard = guard;
}

QPainterPath scaledSourcePath(const SnowSceneDisplayItem& item, const QPainterPath& lens) {
    const auto& geometry = item.magnifier;
    const double factor = geometry.magnification_factor;
    if (!std::isfinite(factor) || factor < 1.0 || factor > 10.0 ||
        !std::isfinite(geometry.source_center_x) || !std::isfinite(geometry.source_center_y) ||
        !std::isfinite(geometry.source_width) || !std::isfinite(geometry.source_height) ||
        geometry.source_width <= 0.0 || geometry.source_height <= 0.0)
        return {};
    if (item.width == geometry.source_width * factor &&
        item.height == geometry.source_height * factor) {
        QTransform source;
        source.translate(geometry.source_center_x, geometry.source_center_y);
        source.scale(1.0 / factor, 1.0 / factor);
        source.translate(-item.center_x, -item.center_y);
        return source.map(lens);
    }
    // Manually supplied snapshots may have independent source extents. Their
    // contour remains renderable, but exact paired-tangent construction rejects
    // this nonhomothetic geometry and uses the guarded hull instead.
    SnowSceneDisplayItem source = item;
    source.center_x = geometry.source_center_x;
    source.center_y = geometry.source_center_y;
    source.width = geometry.source_width;
    source.height = geometry.source_height;
    source.corner_radii.top_left /= factor;
    source.corner_radii.top_right /= factor;
    source.corner_radii.bottom_right /= factor;
    source.corner_radii.bottom_left /= factor;
    return lensPath(source);
}

const SnowCanvasSceneItem::MagnifierGeometryCache& cachedGeometry(const SnowCanvasSceneItem& item,
                                                                  double physicalScale,
                                                                  double exteriorGuard,
                                                                  bool needsHull) {
    const std::array<double, 14> key{item.center_x,
                                     item.center_y,
                                     item.width,
                                     item.height,
                                     item.rotation,
                                     item.magnifier.source_center_x,
                                     item.magnifier.source_center_y,
                                     item.magnifier.source_width,
                                     item.magnifier.source_height,
                                     item.magnifier.magnification_factor,
                                     item.corner_radii.top_left,
                                     item.corner_radii.top_right,
                                     item.corner_radii.bottom_right,
                                     item.corner_radii.bottom_left};
    auto& cache = item.magnifierGeometryCache();
    if (!cache.valid || cache.shape != item.rect_shape || cache.key != key) {
        cache = {};
        cache.key = key;
        cache.shape = item.rect_shape;
        cache.valid = true;
        cache.lens = lensPath(item);
        cache.source = scaledSourcePath(item, cache.lens);
        cache.sourceContained = !cache.source.isEmpty() && cache.source == cache.lens;
        const double factor = item.magnifier.magnification_factor;
        if (!cache.sourceContained && !cache.source.isEmpty() && factor > 1.0) {
            if (item.width == item.magnifier.source_width * factor &&
                item.height == item.magnifier.source_height * factor) {
                // For homothetic convex bodies, source containment is exactly
                // containment of the external homothety center in the source.
                const QPointF center(
                    item.magnifier.source_center_x +
                        (item.magnifier.source_center_x - item.center_x) / (factor - 1.0),
                    item.magnifier.source_center_y +
                        (item.magnifier.source_center_y - item.center_y) / (factor - 1.0));
                cache.sourceContained = cache.source.contains(center);
            } else {
                cache.sourceContained = cache.lens.contains(cache.source);
            }
        }
        ++g_magnifierDiagnostics.geometryBuildCount;
    } else {
        ++g_magnifierDiagnostics.geometryReuseCount;
    }
    if (needsHull && !cache.sourceContained && !cache.lens.isEmpty() && !cache.source.isEmpty()) {
        const double scale =
            std::isfinite(physicalScale) && physicalScale > 0.0 ? physicalScale : 1.0;
        const double bucket = std::exp2(std::ceil(std::log2(scale)));
        if (cache.physicalScale < bucket && cache.failedPhysicalScale != bucket) {
            std::vector<QPointF> points;
            if (appendContourPoints(points, cache.lens, bucket) &&
                appendContourPoints(points, cache.source, bucket)) {
                cache.hull = convexHull(std::move(points));
                rebuildExteriorMasks(cache, std::max(cache.exteriorGuard, exteriorGuard));
                cache.disjointBridge = {};
                if (!cache.source.boundingRect().intersects(cache.lens.boundingRect()))
                    cache.disjointBridge = curvedBridge(item, cache.source, cache.lens, bucket);
                cache.physicalScale = bucket;
                cache.failedPhysicalScale = 0.0;
            } else {
                // Suppress an unrepresentable bridge at this scale, retaining
                // the valid lower-resolution cache for a later zoom-out.
                cache.failedPhysicalScale = bucket;
            }
            ++g_magnifierDiagnostics.bridgeHullBuildCount;
        } else {
            ++g_magnifierDiagnostics.bridgeHullReuseCount;
        }
        if (!cache.hull.isEmpty() && exteriorGuard > cache.exteriorGuard)
            rebuildExteriorMasks(cache, exteriorGuard);
    }
    return cache;
}

bool finiteRect(const QRectF& rect) {
    return std::isfinite(rect.x()) && std::isfinite(rect.y()) && std::isfinite(rect.width()) &&
           std::isfinite(rect.height());
}

struct SourceLayer {
    const SnowCanvasBaseImageSource* source;
    QPainterPath coveragePath;
};

struct SamplingPlan {
    QList<SourceLayer> exposedLayers;
    bool usesGroupMask = false;
};

SamplingPlan samplingPlan(const QList<SnowCanvasBaseImageSource>& sources,
                          const QTransform& sourceToView, const QTransform& viewToDevice,
                          const QRectF& lensBounds, const QRectF& visible) {
    SamplingPlan plan;
    plan.exposedLayers.reserve(sources.size());
    const QTransform viewToSource = sourceToView.inverted();
    const QRectF lensSamples = viewToSource.mapRect(lensBounds);
    const QRectF exposedSamples = viewToSource.mapRect(visible);
    const QRectF devicePixel =
        (sourceToView * viewToDevice).inverted().mapRect(QRectF(0.0, 0.0, 1.0, 1.0));
    bool hasLensLayer = false;
    for (const auto& source : sources) {
        if (source.image.isNull() || !finiteRect(source.canvasRect) ||
            !source.canvasRect.isValid() || !finiteRect(source.coverage))
            continue;
        const QRectF coverage = source.coverage.isEmpty()
                                    ? source.canvasRect
                                    : source.canvasRect.intersected(source.coverage);
        // Keep culling conservative at interpolation and device clip boundaries.
        const double guardX =
            qMax(source.canvasRect.width() / source.image.width(), devicePixel.width());
        const double guardY =
            qMax(source.canvasRect.height() / source.image.height(), devicePixel.height());
        if (!lensSamples.adjusted(-guardX, -guardY, guardX, guardY).intersects(coverage))
            continue;
        // Masking belongs to the complete lens. Exposure may cull a layer, but
        // must not switch the remaining pixels between direct and group masks.
        plan.usesGroupMask |= hasLensLayer;
        hasLensLayer = true;
        if (!exposedSamples.adjusted(-guardX, -guardY, guardX, guardY).intersects(coverage))
            continue;
        SourceLayer layer{&source, {}};
        if (!source.coverage.isEmpty()) {
            QPainterPath coveragePath;
            coveragePath.addRect(coverage);
            layer.coveragePath = sourceToView.map(coveragePath);
        }
        plan.exposedLayers.append(std::move(layer));
    }
    return plan;
}

void drawLayer(QPainter& painter, const SourceLayer& layer, const QTransform& sourceToView,
               const QPainterPath* lensClip = nullptr) {
    painter.save();
    if (lensClip != nullptr) {
        // Resolve geometric coverage before raster clipping so partial exposure
        // cannot change rectangle-versus-path edge rounding.
        const QPainterPath clip =
            layer.coveragePath.isEmpty() ? *lensClip : lensClip->intersected(layer.coveragePath);
        painter.setClipPath(clip, Qt::IntersectClip);
    } else if (!layer.coveragePath.isEmpty()) {
        painter.setClipPath(layer.coveragePath, Qt::IntersectClip);
    }
    // Keep the complete image-to-canvas transform. Cropping a source rectangle
    // would clamp bilinear neighbors independently at each exposure or tile edge.
    painter.setWorldTransform(sourceToView, true);
    painter.drawImage(layer.source->canvasRect, layer.source->image,
                      QRectF(layer.source->image.rect()));
    painter.restore();
    ++g_magnifierDiagnostics.sourceLayerDrawCount;
}

void drawCompositedLayers(QPainter& painter, const QList<SourceLayer>& layers,
                          const QTransform& sourceToView, const QPainterPath& viewPath,
                          const QRectF& visible) {
    const QTransform viewToDevice = painter.deviceTransform();
    QRectF deviceBounds = viewToDevice.mapRect(visible);
    // Align both edges to the physical pixel grid, including export scale, DPR,
    // and enclosing filter-tile translations. Device metrics are not uniformly
    // physical (QWidget uses logical dimensions). A raster engine's paintDevice
    // is its physical backing image, including redirected widget/pixmap paints.
    const auto* engine = painter.paintEngine();
    QRegion systemClip;
    if (engine != nullptr && engine->type() == QPaintEngine::Raster) {
        if (const auto* target = engine->paintDevice())
            deviceBounds =
                deviceBounds.intersected(QRectF(0, 0, target->width(), target->height()));
        systemClip = engine->systemClip();
        if (!systemClip.isEmpty())
            deviceBounds = deviceBounds.intersected(QRectF(systemClip.boundingRect()));
    }
    constexpr double minCoordinate = std::numeric_limits<int>::min() + 1.0;
    constexpr double maxCoordinate = std::numeric_limits<int>::max() - 1.0;
    constexpr double maxExtent = std::numeric_limits<int>::max() - 2.0;
    if (!finiteRect(deviceBounds) || deviceBounds.isEmpty() ||
        deviceBounds.left() < minCoordinate || deviceBounds.top() < minCoordinate ||
        deviceBounds.right() > maxCoordinate || deviceBounds.bottom() > maxCoordinate ||
        deviceBounds.width() > maxExtent || deviceBounds.height() > maxExtent)
        return;
    const QRect pixels = deviceBounds.toAlignedRect();
    QPainterPath deviceClip = viewToDevice.map(viewPath);
    if (painter.hasClipping())
        deviceClip = deviceClip.intersected(viewToDevice.map(painter.clipPath()));
    if (!systemClip.isEmpty()) {
        QPainterPath systemPath;
        systemPath.addRegion(systemClip);
        deviceClip = deviceClip.intersected(systemPath);
    }
    constexpr int stripeMaxHeight = snow_canvas_filter_tile_cache::kTilePhysicalSize;
    constexpr int scratchPixelBudget = stripeMaxHeight * stripeMaxHeight;
    // Qt's downsampling rasterizer uses different precision for SIMD batches
    // and their scalar tails. Splitting a horizontal span introduces new tails
    // and visible seams, even with the complete original source rectangle.
    // Full-width stripes preserve each source/coverage span's actual endpoints.
    // Bound their height by a fixed pixel budget, with one full row as the minimum.
    const int stripeHeight =
        qMin(pixels.height(), qMin(stripeMaxHeight, qMax(1, scratchPixelBudget / pixels.width())));
    QImage scratch = snowCanvasAllocateImage(QSize(pixels.width(), stripeHeight),
                                             QImage::Format_ARGB32_Premultiplied);
    if (scratch.isNull())
        return;
    const auto scratchPixels =
        static_cast<std::size_t>(scratch.width()) * static_cast<std::size_t>(scratch.height());
    g_magnifierDiagnostics.peakCompositingPixelCount =
        qMax(g_magnifierDiagnostics.peakCompositingPixelCount, scratchPixels);
    bool compositingStateSaved = false;
    for (qint64 top = pixels.top(); top <= pixels.bottom(); top += stripeHeight) {
        const QRect tile(pixels.left(), static_cast<int>(top), pixels.width(),
                         qMin(stripeHeight, static_cast<int>(pixels.bottom() - top + 1)));
        if (!deviceClip.intersects(QRectF(tile).adjusted(-1, -1, 1, 1)))
            continue;
        scratch.fill(Qt::transparent);
        {
            QPainter layersPainter(&scratch);
            snow_canvas_renderer::copyPainterFontAndHints(layersPainter, painter);
            layersPainter.setRenderHint(QPainter::SmoothPixmapTransform, true);
            layersPainter.setCompositionMode(QPainter::CompositionMode_SourceOver);
            layersPainter.setClipRect(QRect(0, 0, tile.width(), tile.height()));
            layersPainter.setWorldTransform(
                viewToDevice * QTransform::fromTranslate(-static_cast<double>(pixels.left()),
                                                         -static_cast<double>(top)));
            for (const auto& layer : layers)
                drawLayer(layersPainter, layer, sourceToView);
        }
        if (!compositingStateSaved) {
            painter.save();
            // Retain the complete group mask across stripes. Reinstalling it would
            // rasterize the same lens contour for each strip. Wait until an exposed
            // strip is ready so empty sparse clips do not create a mask.
            painter.setClipPath(viewPath, Qt::IntersectClip);
            painter.setWorldTransform(QTransform());
            painter.setWorldTransform(painter.deviceTransform().inverted());
            painter.setRenderHint(QPainter::SmoothPixmapTransform, false);
            compositingStateSaved = true;
        }
        painter.drawImage(QRectF(tile), scratch, QRectF(0, 0, tile.width(), tile.height()));
        ++g_magnifierDiagnostics.compositedTileCount;
        g_magnifierDiagnostics.compositedPixelCount +=
            static_cast<std::size_t>(tile.width()) * static_cast<std::size_t>(tile.height());
    }
    if (compositingStateSaved)
        painter.restore();
}

} // namespace

RenderDiagnostics diagnosticsForCurrentThread() {
    return g_magnifierDiagnostics;
}

void resetDiagnosticsForCurrentThread() {
    g_magnifierDiagnostics = {};
}

void setStageTimingsEnabledForCurrentThread(bool enabled) {
    g_stageTimingsEnabled = enabled;
}

void setBridgeBenchmarkOptionsForCurrentThread(bool solidFill, bool disableAntialiasing) {
    g_benchmarkSolidFill = solidFill;
    g_benchmarkDisableAntialiasing = disableAntialiasing;
}

QPainterPath lensPath(const SnowSceneDisplayItem& item) {
    QPainterPath path;
    const QRectF rect(-item.width / 2.0, -item.height / 2.0, item.width, item.height);
    if (!finiteRect(rect) || !rect.isValid() || !std::isfinite(item.rotation) ||
        !std::isfinite(item.center_x) || !std::isfinite(item.center_y))
        return path;
    if (item.rect_shape == SNOW_DISPLAY_RECT_SHAPE_ELLIPSE) {
        path.addEllipse(rect);
    } else if (item.rect_shape == SNOW_DISPLAY_RECT_SHAPE_DIAMOND) {
        path = snow_canvas_render_geometry::roundedDiamondPath(rect, item.corner_radii);
    } else {
        path = snow_canvas_render_geometry::roundedRectPath(
            rect, snow_canvas_render_geometry::toViewCornerRadii(item.corner_radii, 1.0, rect));
    }
    QTransform element;
    element.translate(item.center_x, item.center_y);
    element.rotateRadians(item.rotation);
    return element.map(path);
}

QPainterPath sourcePath(const SnowSceneDisplayItem& item) {
    return scaledSourcePath(item, lensPath(item));
}

void render(QPainter& painter, const SceneDisplayInfo& displayInfo, const SnowCanvasSceneItem& item,
            const QList<SnowCanvasBaseImageSource>* sources) {
    const double zoom = displayInfo.camera_zoom;
    if (!std::isfinite(zoom) || zoom <= 0.0)
        return;
    const double deviceScale = maximumLinearScale(painter.deviceTransform());
    const double physicalScale = zoom * deviceScale;
    const double minimumPhysicalScale =
        zoom * minimumLinearScale(painter.deviceTransform(), deviceScale);
    const double exteriorGuard = std::max(1.0, 2.0 / minimumPhysicalScale);
    const bool gradient = item.magnifier.connection_type == SNOW_MAGNIFIER_CONNECTION_GRADIENT &&
                          item.stroke.a != 0 && std::isfinite(physicalScale) &&
                          physicalScale > 0.0 && std::isfinite(exteriorGuard) &&
                          exteriorGuard < 1e150 &&
                          std::isfinite(std::exp2(std::ceil(std::log2(physicalScale))));
    const auto& paths = cachedGeometry(item, physicalScale, exteriorGuard, gradient);
    const QPainterPath& canvasPath = paths.lens;
    if (canvasPath.isEmpty())
        return;
    const QTransform canvasToView(
        zoom, 0.0, 0.0, zoom, displayInfo.surface_width / 2.0 - displayInfo.camera_center_x * zoom,
        displayInfo.surface_height / 2.0 - displayInfo.camera_center_y * zoom);
    const QPainterPath viewPath = canvasToView.map(canvasPath);
    const QRectF exposed = painter.hasClipping() ? painter.clipBoundingRect()
                                                 : QRectF(0.0, 0.0, displayInfo.surface_width,
                                                          displayInfo.surface_height);
    const QRectF visible = viewPath.boundingRect().intersected(exposed);
    const double strokeWidth = item.stroke_width * zoom;
    const bool hasStroke = item.stroke.a != 0 && std::isfinite(strokeWidth) && strokeWidth > 0.0;
    // Source sampling needs the interior; the border can be exposed outside it.
    const bool sourceOutline = item.magnifier.source_outline != 0 && hasStroke &&
                               !paths.source.isEmpty() && !paths.sourceContained;
    if (visible.isEmpty() && !hasStroke && !sourceOutline && !gradient)
        return;
    const double opacity = painter.opacity() * qBound(0.0, item.opacity, 1.0);
    if (opacity == 0.0)
        return;
    ++g_magnifierDiagnostics.renderedMagnifierCount;
    painter.save();
    painter.setOpacity(opacity);
    if (gradient && !paths.sourceContained && paths.physicalScale >= physicalScale &&
        !paths.hull.isEmpty()) {
        QElapsedTimer timer;
        if (g_stageTimingsEnabled)
            timer.start();
        painter.save();
        // Original contour masks also guard subpixel differences between Qt's
        // rasterized full cubics and the exact reparameterized inward cap arcs.
        // Two masks preserve overlap exclusion without parity refilling it.
        if (paths.disjointBridge.isEmpty()) {
            ++g_magnifierDiagnostics.maskedBridgeDrawCount;
        } else {
            ++g_magnifierDiagnostics.disjointBridgeDrawCount;
        }
        if (!paths.outsideBoth.isEmpty()) {
            painter.setClipPath(canvasToView.map(paths.outsideBoth), Qt::IntersectClip);
        } else {
            painter.setClipPath(canvasToView.map(paths.outsideSource), Qt::IntersectClip);
            painter.setClipPath(canvasToView.map(paths.outsideLens), Qt::IntersectClip);
        }
        if (g_stageTimingsEnabled) {
            g_magnifierDiagnostics.connectionClipNanoseconds +=
                static_cast<std::uint64_t>(timer.nsecsElapsed());
            timer.start();
        }
        QLinearGradient connection(canvasToView.map(QPointF(item.magnifier.source_center_x,
                                                            item.magnifier.source_center_y)),
                                   canvasToView.map(QPointF(item.center_x, item.center_y)));
        QColor sourceColor = snow_canvas_renderer::toQColor(item.stroke);
        QColor lensColor = sourceColor;
        sourceColor.setAlphaF(static_cast<float>(static_cast<double>(sourceColor.alphaF()) * 0.04));
        lensColor.setAlphaF(static_cast<float>(static_cast<double>(lensColor.alphaF()) * 0.16));
        connection.setColorAt(0.0, sourceColor);
        connection.setColorAt(1.0, lensColor);
        if (g_benchmarkDisableAntialiasing)
            painter.setRenderHint(QPainter::Antialiasing, false);
        QBrush brush(connection);
        if (g_benchmarkSolidFill) {
            QColor solid = snow_canvas_renderer::toQColor(item.stroke);
            solid.setAlphaF(static_cast<float>(static_cast<double>(solid.alphaF()) * 0.10));
            brush = QBrush(solid);
        }
        painter.fillPath(
            canvasToView.map(paths.disjointBridge.isEmpty() ? paths.hull : paths.disjointBridge),
            brush);
        if (g_stageTimingsEnabled)
            g_magnifierDiagnostics.connectionFillNanoseconds +=
                static_cast<std::uint64_t>(timer.nsecsElapsed());
        painter.restore();
    }
    if (sourceOutline) {
        QElapsedTimer timer;
        if (g_stageTimingsEnabled)
            timer.start();
        painter.save();
        const QPainterPath viewSource = canvasToView.map(paths.source);
        const QRectF sourceStrokeBounds = viewSource.boundingRect().adjusted(
            -strokeWidth / 2.0, -strokeWidth / 2.0, strokeWidth / 2.0, strokeWidth / 2.0);
        if (sourceStrokeBounds.intersects(viewPath.boundingRect())) {
            QPainterPath outsideLens;
            outsideLens.addRect(
                sourceStrokeBounds.united(viewPath.boundingRect())
                    .adjusted(-strokeWidth, -strokeWidth, strokeWidth, strokeWidth));
            outsideLens.addPath(viewPath);
            outsideLens.setFillRule(Qt::OddEvenFill);
            painter.setClipPath(outsideLens, Qt::IntersectClip);
        }
        painter.setPen(QPen(snow_canvas_renderer::toQColor(item.stroke), strokeWidth, Qt::SolidLine,
                            Qt::RoundCap, Qt::RoundJoin));
        painter.setBrush(Qt::NoBrush);
        painter.drawPath(viewSource);
        if (g_stageTimingsEnabled)
            g_magnifierDiagnostics.sourceOutlineNanoseconds +=
                static_cast<std::uint64_t>(timer.nsecsElapsed());
        painter.restore();
    }
    const auto& geometry = item.magnifier;
    const double factor = geometry.magnification_factor;
    if (!visible.isEmpty() && sources != nullptr && std::isfinite(factor) && factor >= 1.0 &&
        factor <= 10.0 && std::isfinite(geometry.source_center_x) &&
        std::isfinite(geometry.source_center_y)) {
        const double scale = factor * zoom;
        const QPointF destinationCenter = canvasToView.map(QPointF(item.center_x, item.center_y));
        const QTransform sourceToView(scale, 0.0, 0.0, scale,
                                      destinationCenter.x() - geometry.source_center_x * scale,
                                      destinationCenter.y() - geometry.source_center_y * scale);
        painter.save();
        painter.setRenderHint(QPainter::SmoothPixmapTransform, true);
        const auto plan = samplingPlan(*sources, sourceToView, painter.deviceTransform(),
                                       viewPath.boundingRect(), visible);
        if (!plan.exposedLayers.isEmpty()) {
            if (plan.usesGroupMask) {
                drawCompositedLayers(painter, plan.exposedLayers, sourceToView, viewPath, visible);
            } else {
                for (const auto& layer : plan.exposedLayers)
                    drawLayer(painter, layer, sourceToView, &viewPath);
            }
        }
        painter.restore();
    }
    if (hasStroke) {
        painter.setPen(QPen(snow_canvas_renderer::toQColor(item.stroke), strokeWidth, Qt::SolidLine,
                            Qt::RoundCap, Qt::RoundJoin));
        painter.setBrush(Qt::NoBrush);
        painter.drawPath(viewPath);
    }
    painter.restore();
}

} // namespace snow_canvas_magnifier_renderer
