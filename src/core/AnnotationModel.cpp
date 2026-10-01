#include "core/AnnotationModel.h"

#include <QFont>
#include <QPainter>
#include <QPainterPathStroker>
#include <QtMath>

#include <algorithm>
#include <vector>

namespace Visnip {

bool AnnotationItem::isValid() const
{
    switch (type) {
    case AnnotationType::Rectangle:
    case AnnotationType::Ellipse:
        return rect.normalized().width() >= 2 && rect.normalized().height() >= 2;
    case AnnotationType::Text:
        return rect.normalized().width() >= 2 && rect.normalized().height() >= 2 && !text.trimmed().isEmpty();
    case AnnotationType::Mosaic:
        if (style.mosaicPaintMode == MosaicPaintMode::Brush) {
            return points.size() >= 2;
        }
        return rect.normalized().width() >= 2 && rect.normalized().height() >= 2;
    case AnnotationType::Eraser:
        if (style.mosaicPaintMode == MosaicPaintMode::Brush) {
            return points.size() >= 2;
        }
        return rect.normalized().width() >= 2 && rect.normalized().height() >= 2;
    case AnnotationType::Arrow:
        return points.size() >= 2 && QLineF(points.first(), points.last()).length() >= 2;
    case AnnotationType::Pen:
        return points.size() >= 2;
    case AnnotationType::Number:
        return rect.isValid() || !points.isEmpty();
    }
    return false;
}

QRectF AnnotationItem::boundingRect() const
{
    // Rect-based tools (rectangle, ellipse, fill mosaic, fill eraser) seed
    // points with the press anchor but track their extent in `rect`, so both
    // sources have to contribute: returning only the point cloud used to
    // collapse a dragged rectangle's bounds to a ~40x40 box around the anchor.
    QRectF bounds;
    if (!points.isEmpty()) {
        bounds = QRectF(points.first(), QSizeF(1, 1));
        for (const auto& point : points) {
            bounds = bounds.united(QRectF(point, QSizeF(1, 1)));
        }
    }
    const QRectF normalized = rect.normalized();
    if (!normalized.isEmpty()) {
        bounds = bounds.isNull() ? normalized : bounds.united(normalized);
    }
    const int pad = type == AnnotationType::Mosaic ? style.mosaicBlock + 8 : qMax(1, style.strokeWidth) + 8;
    return bounds.adjusted(-pad, -pad, pad, pad);
}

void AnnotationDocument::add(const AnnotationItem& item)
{
    if (!item.isValid()) {
        return;
    }
    items_.append(item);
    redoStack_.clear();
    ++revision_;
}

AnnotationItem AnnotationDocument::itemAt(int index) const
{
    if (index < 0 || index >= items_.size()) {
        return AnnotationItem{};
    }
    return items_.at(index);
}

bool AnnotationDocument::replaceAt(int index, const AnnotationItem& item)
{
    if (index < 0 || index >= items_.size() || !item.isValid()) {
        return false;
    }
    items_.replace(index, item);
    redoStack_.clear();
    ++revision_;
    return true;
}

bool AnnotationDocument::undo()
{
    if (items_.isEmpty()) {
        return false;
    }
    redoStack_.append(items_.takeLast());
    ++revision_;
    return true;
}

bool AnnotationDocument::redo()
{
    if (redoStack_.isEmpty()) {
        return false;
    }
    items_.append(redoStack_.takeLast());
    ++revision_;
    return true;
}

void AnnotationDocument::clear()
{
    const bool changed = !items_.isEmpty() || !redoStack_.isEmpty();
    items_.clear();
    redoStack_.clear();
    if (changed) {
        // canRedo() flipping is observable state too, so a redo-stack-only
        // clear must also invalidate revision-keyed caches.
        ++revision_;
    }
}

void AnnotationDocument::translate(const QPointF& delta)
{
    if (delta.isNull()) {
        return;
    }

    const auto translateItems = [&delta](QVector<AnnotationItem>& items) {
        for (auto& item : items) {
            item.rect.translate(delta);
            for (auto& point : item.points) {
                point += delta;
            }
        }
    };
    translateItems(items_);
    translateItems(redoStack_);
    // revision() is used as a cache key for rendered composites, so every
    // observable mutation has to bump it -- add/replaceAt/undo/redo/clear
    // already do.
    ++revision_;
}

int AnnotationDocument::nextNumber() const
{
    int maxNumber = 0;
    for (const auto& item : items_) {
        if (item.type == AnnotationType::Number) {
            maxNumber = qMax(maxNumber, item.number);
        }
    }
    return maxNumber + 1;
}

namespace {
QPolygonF arrowHead(const QPointF& start, const QPointF& end, double size)
{
    const double angle = std::atan2(end.y() - start.y(), end.x() - start.x());
    const double back = angle + M_PI;
    const QPointF p1 = end + QPointF(std::cos(back + M_PI / 7.0) * size, std::sin(back + M_PI / 7.0) * size);
    const QPointF p2 = end + QPointF(std::cos(back - M_PI / 7.0) * size, std::sin(back - M_PI / 7.0) * size);
    return QPolygonF({end, p1, p2});
}

QRectF offsetRect(QRectF rect, const QPointF& offset)
{
    rect.translate(-offset);
    return rect.normalized();
}

QPointF offsetPoint(QPointF point, const QPointF& offset)
{
    return point - offset;
}

QPainterPath smoothPath(const QVector<QPointF>& points, const QPointF& offset)
{
    QPainterPath path;
    if (points.isEmpty()) {
        return path;
    }
    path.moveTo(offsetPoint(points.first(), offset));
    for (int i = 1; i < points.size(); ++i) {
        const QPointF p = offsetPoint(points.at(i), offset);
        if (i + 1 < points.size()) {
            const QPointF next = offsetPoint(points.at(i + 1), offset);
            path.quadTo(p, (p + next) / 2.0);
        } else {
            path.lineTo(p);
        }
    }
    return path;
}

QPainterPath strokedPath(const QVector<QPointF>& points,
                         const QPointF& offset,
                         qreal width,
                         qreal minimumWidth = 3.0)
{
    QPainterPathStroker stroker;
    stroker.setCapStyle(Qt::RoundCap);
    stroker.setJoinStyle(Qt::RoundJoin);
    stroker.setWidth(qMax(minimumWidth, width));
    return stroker.createStroke(smoothPath(points, offset));
}
} // namespace

void drawAnnotation(QPainter& painter, const AnnotationItem& item, const QPointF& offset)
{
    painter.save();
    painter.setRenderHint(QPainter::Antialiasing, true);

    QPen pen(item.style.stroke, item.style.strokeWidth, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin);
    painter.setPen(pen);

    switch (item.type) {
    case AnnotationType::Rectangle: {
        const QRectF r = offsetRect(item.rect, offset);
        painter.setBrush(item.style.fill);
        painter.drawRoundedRect(r, 4, 4);
        break;
    }
    case AnnotationType::Ellipse: {
        const QRectF r = offsetRect(item.rect, offset);
        painter.setBrush(item.style.fill);
        painter.drawEllipse(r);
        break;
    }
    case AnnotationType::Arrow: {
        if (item.points.size() < 2) {
            break;
        }
        const QPointF start = offsetPoint(item.points.first(), offset);
        const QPointF end = offsetPoint(item.points.last(), offset);
        painter.setBrush(item.style.stroke);
        painter.drawLine(start, end);
        const int headSize = qMax(8, item.style.strokeWidth * 4);
        if (item.style.arrowHeadMode == ArrowHeadMode::SingleArrow || item.style.arrowHeadMode == ArrowHeadMode::DoubleArrow) {
            painter.drawPolygon(arrowHead(start, end, headSize));
        }
        if (item.style.arrowHeadMode == ArrowHeadMode::DoubleArrow) {
            painter.drawPolygon(arrowHead(end, start, headSize));
        }
        break;
    }
    case AnnotationType::Pen: {
        if (item.points.size() < 2) {
            break;
        }
        painter.setBrush(Qt::NoBrush);
        painter.drawPath(smoothPath(item.points, offset));
        break;
    }
    case AnnotationType::Text: {
        const QRectF r = offsetRect(item.rect, offset);
        QFont font = painter.font();
        if (!item.style.fontFamily.isEmpty()) {
            font.setFamily(item.style.fontFamily);
        }
        font.setPixelSize(item.style.fontSize);
        font.setBold(item.style.textBold);
        font.setItalic(item.style.textItalic);
        font.setStyleStrategy(QFont::PreferAntialias);
        painter.setFont(font);
        const QFontMetricsF metrics(font);
        const QStringList lines = item.text.split(QLatin1Char('\n'));
        if (item.style.textOutline) {
            const QVector<QPointF> offsets = {
                QPointF(-1, 0), QPointF(1, 0), QPointF(0, -1), QPointF(0, 1)
            };
            painter.setPen(QPen(item.style.textOutlineColor, qMax(1, item.style.textOutlineWidth)));
            for (int i = 0; i < lines.size(); ++i) {
                const QPointF baseline(r.left(), r.top() + metrics.ascent() + i * metrics.lineSpacing());
                for (const QPointF& delta : offsets) {
                    painter.drawText(baseline + delta, lines.at(i));
                }
            }
        }
        painter.setPen(item.style.text);
        for (int i = 0; i < lines.size(); ++i) {
            const QPointF baseline(r.left(), r.top() + metrics.ascent() + i * metrics.lineSpacing());
            painter.drawText(baseline, lines.at(i));
        }
        break;
    }
    case AnnotationType::Mosaic: {
        Q_UNUSED(offset)
        // Mosaic annotations are rendered by applyMosaicAnnotation() directly onto the image preview/result.
        // Do not draw an extra helper outline here, otherwise the brush path/filled area shows blue borders.
        break;
    }
    case AnnotationType::Eraser: {
        Q_UNUSED(offset)
        // Eraser annotations are applied by applyEraserAnnotation() while replaying the annotation stack.
        break;
    }
    case AnnotationType::Number: {
        QPointF center = item.points.isEmpty() ? item.rect.center() : item.points.first();
        center = offsetPoint(center, offset);
        const qreal radius = 10.0;
        const QRectF circle(center.x() - radius, center.y() - radius, radius * 2.0, radius * 2.0);
        painter.setPen(Qt::NoPen);
        painter.setBrush(item.style.stroke);
        painter.drawEllipse(circle);
        QFont font = painter.font();
        font.setPixelSize(12);
        font.setBold(true);
        painter.setFont(font);
        painter.setPen(Qt::white);
        painter.drawText(circle, Qt::AlignCenter, QString::number(item.number));
        break;
    }
    }

    painter.restore();
}

void applyMosaic(QImage& image, QRect rect, int blockSize, int minimumBlockSize)
{
    if (image.isNull()) {
        return;
    }
    rect = rect.normalized().intersected(image.rect());
    if (rect.isEmpty()) {
        return;
    }
    // The scanline fast paths below reinterpret rows as QRgb*, which is only
    // valid for 32bpp buffers. Convert defensively rather than reading past
    // bytesPerLine on 8/16/24bpp formats.
    if (image.depth() != 32) {
        image.convertTo(QImage::Format_ARGB32);
        if (image.isNull()) {
            return;
        }
    }
    blockSize = qBound(qMax(1, minimumBlockSize), blockSize, 64);

    uchar* const base = image.bits();
    const qsizetype stride = image.bytesPerLine();

    for (int y = rect.top(); y <= rect.bottom(); y += blockSize) {
        for (int x = rect.left(); x <= rect.right(); x += blockSize) {
            const QRect blockRect = QRect(x, y, blockSize, blockSize).intersected(rect);
            if (blockRect.isEmpty()) {
                continue;
            }
            const int left = blockRect.left();
            const int width = blockRect.width();
            int r = 0;
            int g = 0;
            int b = 0;
            int count = 0;
            for (int by = blockRect.top(); by <= blockRect.bottom(); ++by) {
                const QRgb* line = reinterpret_cast<const QRgb*>(base + stride * by) + left;
                for (int i = 0; i < width; ++i) {
                    const QRgb px = line[i];
                    r += qRed(px);
                    g += qGreen(px);
                    b += qBlue(px);
                }
                count += width;
            }
            if (count <= 0) {
                continue;
            }
            const QRgb avg = qRgb(r / count, g / count, b / count);
            for (int by = blockRect.top(); by <= blockRect.bottom(); ++by) {
                QRgb* line = reinterpret_cast<QRgb*>(base + stride * by) + left;
                std::fill_n(line, width, avg);
            }
        }
    }
}

namespace {
// Both blur passes walk raw 32bpp scanlines instead of QImage::pixel()/
// setPixel(). Those accessors are out-of-line and re-validate coordinates and
// format on every call, which dominated the cost of a filter that runs once
// per painted frame. Results are bit-identical to the previous implementation.
void blurHorizontal(const QImage& source, QImage& target, int radius)
{
    const int w = source.width();
    const int h = source.height();
    if (w <= 0 || h <= 0 || radius <= 0) {
        return;
    }
    const int span = radius * 2 + 1;

    const uchar* const srcBase = source.constBits();
    const qsizetype srcStride = source.bytesPerLine();
    uchar* const dstBase = target.bits();
    const qsizetype dstStride = target.bytesPerLine();

    for (int y = 0; y < h; ++y) {
        const QRgb* const src = reinterpret_cast<const QRgb*>(srcBase + srcStride * y);
        QRgb* const dst = reinterpret_cast<QRgb*>(dstBase + dstStride * y);
        int a = 0;
        int r = 0;
        int g = 0;
        int b = 0;
        for (int dx = -radius; dx <= radius; ++dx) {
            const QRgb px = src[qBound(0, dx, w - 1)];
            a += qAlpha(px);
            r += qRed(px);
            g += qGreen(px);
            b += qBlue(px);
        }
        for (int x = 0; x < w; ++x) {
            dst[x] = qRgba(r / span, g / span, b / span, a / span);
            const QRgb removePx = src[qBound(0, x - radius, w - 1)];
            const QRgb addPx = src[qBound(0, x + radius + 1, w - 1)];
            a += qAlpha(addPx) - qAlpha(removePx);
            r += qRed(addPx) - qRed(removePx);
            g += qGreen(addPx) - qGreen(removePx);
            b += qBlue(addPx) - qBlue(removePx);
        }
    }
}

// Column-major traversal used to miss the cache on every pixel (stride =
// bytesPerLine). This keeps one sliding-window accumulator per column and
// walks the image row by row, so every access is sequential.
void blurVertical(const QImage& source, QImage& target, int radius)
{
    const int w = source.width();
    const int h = source.height();
    if (w <= 0 || h <= 0 || radius <= 0) {
        return;
    }
    const int span = radius * 2 + 1;

    const uchar* const srcBase = source.constBits();
    const qsizetype srcStride = source.bytesPerLine();
    uchar* const dstBase = target.bits();
    const qsizetype dstStride = target.bytesPerLine();

    const auto sourceRow = [&](int y) {
        return reinterpret_cast<const QRgb*>(srcBase + srcStride * qBound(0, y, h - 1));
    };

    // Interleaved [a, r, g, b] per column keeps the four channels of one pixel
    // on the same cache line.
    std::vector<int> accumulator(static_cast<size_t>(w) * 4, 0);
    int* const acc = accumulator.data();

    for (int dy = -radius; dy <= radius; ++dy) {
        const QRgb* const line = sourceRow(dy);
        for (int x = 0; x < w; ++x) {
            const QRgb px = line[x];
            int* const c = acc + x * 4;
            c[0] += qAlpha(px);
            c[1] += qRed(px);
            c[2] += qGreen(px);
            c[3] += qBlue(px);
        }
    }

    for (int y = 0; y < h; ++y) {
        QRgb* const dst = reinterpret_cast<QRgb*>(dstBase + dstStride * y);
        const QRgb* const removeLine = sourceRow(y - radius);
        const QRgb* const addLine = sourceRow(y + radius + 1);
        for (int x = 0; x < w; ++x) {
            int* const c = acc + x * 4;
            dst[x] = qRgba(c[1] / span, c[2] / span, c[3] / span, c[0] / span);
            const QRgb removePx = removeLine[x];
            const QRgb addPx = addLine[x];
            c[0] += qAlpha(addPx) - qAlpha(removePx);
            c[1] += qRed(addPx) - qRed(removePx);
            c[2] += qGreen(addPx) - qGreen(removePx);
            c[3] += qBlue(addPx) - qBlue(removePx);
        }
    }
}

// Reuses a caller-owned scratch buffer instead of allocating two full-size
// images per pass (three passes used to mean six allocations), and writes the
// vertical pass straight back into `image` so no third buffer is needed.
// Returns false when the scratch buffer cannot be allocated -- the previous
// code would have run the whole filter against a null QImage, emitting one
// qWarning per pixel.
bool boxBlur(QImage& image, QImage& scratch, int radius)
{
    if (image.isNull() || radius <= 0) {
        return true;
    }
    if (image.depth() != 32) {
        return false;
    }
    if (scratch.size() != image.size() || scratch.format() != image.format()) {
        scratch = QImage(image.size(), image.format());
        if (scratch.isNull()) {
            return false;
        }
    }
    // The passes assume distinct buffers; an aliased scratch would silently
    // produce wrong output rather than crash.
    Q_ASSERT(image.constBits() != scratch.constBits());
    blurHorizontal(image, scratch, radius);
    blurVertical(scratch, image, radius);
    return true;
}

void applyEffect(QImage& image,
                 QRect rect,
                 MosaicEffectMode effectMode,
                 int strength,
                 int minimumStrength)
{
    if (effectMode == MosaicEffectMode::GaussianBlur) {
        applyGaussianBlur(image, rect, strength, minimumStrength);
    } else {
        applyMosaic(image, rect, strength, minimumStrength);
    }
}
} // namespace

void applyGaussianBlur(QImage& image, QRect rect, int radius, int minimumRadius)
{
    if (image.isNull()) {
        return;
    }
    rect = rect.normalized().intersected(image.rect());
    if (rect.isEmpty()) {
        return;
    }
    radius = qBound(qMax(1, minimumRadius), radius, 24);
    const int boxRadius = qMax(1, radius / 2);
    const QRect sourceRect = rect.adjusted(-radius, -radius, radius, radius).intersected(image.rect());
    QImage work = image.copy(sourceRect).convertToFormat(QImage::Format_ARGB32);
    if (work.isNull()) {
        return;
    }
    QImage scratch;
    for (int i = 0; i < 3; ++i) {
        if (!boxBlur(work, scratch, boxRadius)) {
            return;
        }
    }

    QPainter painter(&image);
    painter.setClipRect(rect);
    painter.drawImage(sourceRect.topLeft(), work);
}

void applyMosaicAnnotation(QImage& image,
                           const AnnotationItem& item,
                           const QPointF& offset,
                           qreal renderScale)
{
    if (image.isNull() || item.type != AnnotationType::Mosaic) {
        return;
    }

    const int minimumStrength = renderScale < 1.0 ? 1 : 3;
    const int strength = qBound(minimumStrength,
                                qRound(qBound(3, item.style.mosaicBlock, 24) * renderScale),
                                24);
    if (item.style.mosaicPaintMode == MosaicPaintMode::Brush) {
        if (item.points.size() < 2) {
            return;
        }
        const qreal brushWidth = qBound(3, item.style.mosaicBlock, 24) * renderScale;
        const QPainterPath clipPath = strokedPath(item.points,
                                                  offset,
                                                  brushWidth,
                                                  renderScale < 1.0 ? 0.25 : 3.0);
        const QRect bounds = clipPath.boundingRect().adjusted(-strength, -strength, strength, strength).toAlignedRect().intersected(image.rect());
        if (bounds.isEmpty()) {
            return;
        }

        QImage processed = image.copy(bounds).convertToFormat(QImage::Format_ARGB32);
        applyEffect(processed,
                    QRect(QPoint(0, 0), processed.size()),
                    item.style.mosaicEffectMode,
                    strength,
                    minimumStrength);

        QPainter painter(&image);
        painter.setRenderHint(QPainter::Antialiasing, true);
        painter.setClipPath(clipPath);
        painter.drawImage(bounds.topLeft(), processed);
        return;
    }

    const QRect rect = offsetRect(item.rect, offset).toAlignedRect().intersected(image.rect());
    applyEffect(image, rect, item.style.mosaicEffectMode, strength, minimumStrength);
}

void applyEraserAnnotation(QImage& image,
                           const QImage& source,
                           const AnnotationItem& item,
                           const QPointF& offset,
                           qreal renderScale)
{
    if (image.isNull() || source.isNull() || item.type != AnnotationType::Eraser || image.size() != source.size()) {
        return;
    }

    QPainterPath erasePath;
    if (item.style.mosaicPaintMode == MosaicPaintMode::Brush) {
        if (item.points.size() < 2) {
            return;
        }
        const qreal width = qBound(4, item.style.strokeWidth, 96) * renderScale;
        erasePath = strokedPath(item.points,
                                offset,
                                width,
                                renderScale < 1.0 ? 0.25 : 4.0);
    } else {
        const QRectF rect = offsetRect(item.rect, offset).intersected(QRectF(image.rect()));
        if (rect.isEmpty()) {
            return;
        }
        erasePath.addRect(rect);
    }

    const QRect bounds = erasePath.boundingRect().toAlignedRect().intersected(image.rect());
    if (bounds.isEmpty()) {
        return;
    }

    QPainter painter(&image);
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.setClipPath(erasePath);
    painter.drawImage(QPoint(0, 0), source);
}

void renderAnnotationItemToImage(QImage& image,
                                 const QImage& source,
                                 const AnnotationItem& item,
                                 const QPointF& offset)
{
    if (item.type == AnnotationType::Mosaic) {
        applyMosaicAnnotation(image, item, offset);
        return;
    }
    if (item.type == AnnotationType::Eraser) {
        applyEraserAnnotation(image, source, item, offset);
        return;
    }

    QPainter painter(&image);
    painter.setRenderHint(QPainter::Antialiasing, true);
    drawAnnotation(painter, item, offset);
}

} // namespace Visnip
