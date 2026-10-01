#pragma once

#include "core/AppConfig.h"

#include <QImage>
#include <QPoint>
#include <QRect>
#include <QWidget>

class QPainter;

namespace Visnip {

class ImageOutputService;

class PinWindow : public QWidget {
    Q_OBJECT
public:
    explicit PinWindow(QImage image, const PinSettings& settings, QWidget* parent = nullptr);

    const QImage& image() const { return image_; }
    void setOutputService(ImageOutputService* service) { output_ = service; }
    void toggleMouseThrough();
    bool mouseThrough() const { return mouseThrough_; }
    void setPinnedOnTop(bool onTop);
    void toggleShadow();
    void moveImageTopLeft(const QPoint& topLeft);
    void setActiveVisual(bool active);

signals:
    void becameActive(PinWindow* window);
    void deactivated(PinWindow* window);

protected:
    bool event(QEvent* event) override;
    void paintEvent(QPaintEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void mouseDoubleClickEvent(QMouseEvent* event) override;
    void wheelEvent(QWheelEvent* event) override;
    void contextMenuEvent(QContextMenuEvent* event) override;
    void keyPressEvent(QKeyEvent* event) override;

private:
    int shadowMargin() const;
    QSize scaledImageSize() const;
    QSize viewportImageSize(const QPoint& referenceGlobal = QPoint()) const;
    int maxViewportHeight(const QPoint& referenceGlobal = QPoint()) const;
    QRect imageRect() const;
    void resizeToScale(const QPoint& referenceGlobal = QPoint());
    void clampImageScroll();
    void drawViewportScrollbar(QPainter& painter, const QRect& content) const;
    void applyMouseThrough();
    void copyImage();
    void saveImage();

    QImage image_;
    PinSettings settings_;
    ImageOutputService* output_ = nullptr;
    double scale_ = 1.0;
    double opacity_ = 1.0;
    int imageScrollY_ = 0;
    bool dragging_ = false;
    bool mouseThrough_ = false;
    bool onTop_ = true;
    bool shadowEnabled_ = true;
    bool activeVisual_ = false;
    QPoint dragStartGlobal_;
    QPoint dragStartPos_;
};

} // namespace Visnip
