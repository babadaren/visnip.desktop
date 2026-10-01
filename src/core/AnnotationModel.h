#pragma once

#include <QColor>
#include <QImage>
#include <QPainterPath>
#include <QPointF>
#include <QRectF>
#include <QString>
#include <QtGlobal>
#include <QVector>

namespace Visnip {

enum class AnnotationType {
    Rectangle,
    Ellipse,
    Arrow,
    Pen,
    Text,
    Mosaic,
    Eraser,
    Number
};

enum class ArrowHeadMode {
    Line,
    SingleArrow,
    DoubleArrow
};

enum class MosaicPaintMode {
    Brush,
    Fill
};

enum class MosaicEffectMode {
    Pixelate,
    GaussianBlur
};

struct AnnotationStyle {
    QColor stroke = QColor(QStringLiteral("#EF4444"));
    QColor fill = QColor(239, 68, 68, 32);
    QColor text = QColor(QStringLiteral("#EF4444"));
    int strokeWidth = 2;
    int mosaicBlock = 8;
    int fontSize = 16;
    QString fontFamily = QStringLiteral("Microsoft YaHei UI");
    bool textBold = false;
    bool textItalic = false;
    bool textOutline = false;
    QColor textOutlineColor = QColor(255, 255, 255);
    int textOutlineWidth = 2;
    ArrowHeadMode arrowHeadMode = ArrowHeadMode::SingleArrow;
    MosaicPaintMode mosaicPaintMode = MosaicPaintMode::Fill;
    MosaicEffectMode mosaicEffectMode = MosaicEffectMode::Pixelate;
};

struct AnnotationItem {
    AnnotationType type = AnnotationType::Rectangle;
    QRectF rect;
    QVector<QPointF> points;
    QString text;
    int number = 1;
    AnnotationStyle style;

    bool isValid() const;
    QRectF boundingRect() const;
};

class AnnotationDocument {
public:
    const QVector<AnnotationItem>& items() const { return items_; }
    const QVector<AnnotationItem>& redoItems() const { return redoStack_; }
    int count() const { return items_.size(); }
    quint64 revision() const { return revision_; }
    AnnotationItem itemAt(int index) const;

    void add(const AnnotationItem& item);
    bool replaceAt(int index, const AnnotationItem& item);
    bool canUndo() const { return !items_.isEmpty(); }
    bool canRedo() const { return !redoStack_.isEmpty(); }
    bool undo();
    bool redo();
    void clear();
    void translate(const QPointF& delta);
    int nextNumber() const;

private:
    QVector<AnnotationItem> items_;
    QVector<AnnotationItem> redoStack_;
    quint64 revision_ = 0;
};

void drawAnnotation(QPainter& painter, const AnnotationItem& item, const QPointF& offset = QPointF());
void applyMosaic(QImage& image, QRect rect, int blockSize, int minimumBlockSize = 3);
void applyGaussianBlur(QImage& image, QRect rect, int radius, int minimumRadius = 3);
void applyMosaicAnnotation(QImage& image,
                           const AnnotationItem& item,
                           const QPointF& offset = QPointF(),
                           qreal renderScale = 1.0);
void applyEraserAnnotation(QImage& image,
                           const QImage& source,
                           const AnnotationItem& item,
                           const QPointF& offset = QPointF(),
                           qreal renderScale = 1.0);
void renderAnnotationItemToImage(QImage& image,
                                 const QImage& source,
                                 const AnnotationItem& item,
                                 const QPointF& offset = QPointF());

} // namespace Visnip
