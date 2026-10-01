#include "ui/pin/PinWindow.h"

#include "services/ImageOutputService.h"

#include <QApplication>
#include <QClipboard>
#include <QContextMenuEvent>
#include <QCursor>
#include <QEvent>
#include <QGuiApplication>
#include <QKeyEvent>
#include <QLinearGradient>
#include <QMenu>
#include <QMouseEvent>
#include <QPainter>
#include <QPaintEvent>
#include <QRadialGradient>
#include <QScreen>
#include <QWheelEvent>

#ifdef Q_OS_WIN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace Visnip {

namespace {
constexpr int kPinShadowMargin = 16;
constexpr int kPinShadowOffset = 0;

// Gaussian-like falloff sampled into gradient stops; t=0 sits at the content
// edge, t=1 at the outer rim of the shadow band. Shared by the edge strips
// and the corner radials so the profiles meet seamlessly.
QGradientStops pinShadowStops()
{
    static const QGradientStops stops = []() {
        const int alphas[] = {24, 17, 11, 6, 3, 1, 0};
        QGradientStops built;
        const int count = static_cast<int>(std::size(alphas));
        for (int i = 0; i < count; ++i) {
            built.append({i / qreal(count - 1), QColor(15, 23, 42, alphas[i])});
        }
        return built;
    }();
    return stops;
}
} // namespace

PinWindow::PinWindow(QImage image, const PinSettings& settings, QWidget* parent)
    : QWidget(parent)
    , image_(std::move(image))
    , settings_(settings)
    , opacity_(qBound(0.2, settings.defaultOpacity, 1.0))
    , onTop_(settings.alwaysOnTop)
    , shadowEnabled_(settings.shadow)
{
    setWindowTitle(QStringLiteral("Visnip 贴图"));
    setWindowFlags(Qt::FramelessWindowHint | Qt::Tool | (settings.alwaysOnTop ? Qt::WindowStaysOnTopHint : Qt::WindowFlags{}));
    setAttribute(Qt::WA_NoSystemBackground, true);
    setAttribute(Qt::WA_TranslucentBackground, true);
    setAttribute(Qt::WA_DeleteOnClose, true);
    setFocusPolicy(Qt::StrongFocus);
    setMouseTracking(true);
    if (opacity_ < 0.999) {
        setWindowOpacity(opacity_);
    }

    resizeToScale();
}

int PinWindow::shadowMargin() const
{
    return shadowEnabled_ ? kPinShadowMargin : 0;
}

QSize PinWindow::scaledImageSize() const
{
    if (image_.isNull()) {
        return QSize(240, 160);
    }
    return QSize(qMax(1, static_cast<int>(image_.width() * scale_)),
                 qMax(1, static_cast<int>(image_.height() * scale_)));
}

int PinWindow::maxViewportHeight(const QPoint& referenceGlobal) const
{
    QPoint reference = referenceGlobal;
    if (reference.isNull()) {
        reference = frameGeometry().isValid() ? frameGeometry().center() : QCursor::pos();
    }
    QScreen* screen = QGuiApplication::screenAt(reference);
    if (!screen) {
        screen = QGuiApplication::primaryScreen();
    }
    const int available = screen ? screen->availableGeometry().height() : 720;
    return qMax(160, available - shadowMargin() * 2 - 24);
}

QSize PinWindow::viewportImageSize(const QPoint& referenceGlobal) const
{
    const QSize full = scaledImageSize();
    return QSize(full.width(), qMin(full.height(), maxViewportHeight(referenceGlobal)));
}

QRect PinWindow::imageRect() const
{
    const int margin = shadowMargin();
    return QRect(QPoint(margin, margin), viewportImageSize());
}

void PinWindow::resizeToScale(const QPoint& referenceGlobal)
{
    const int margin = shadowMargin();
    const QSize content = viewportImageSize(referenceGlobal);
    // Extra bottom room keeps the offset shadow tail inside the window.
    resize(content.width() + margin * 2, content.height() + margin * 2 + (shadowEnabled_ ? kPinShadowOffset : 0));
    clampImageScroll();
}

void PinWindow::clampImageScroll()
{
    const int maxScroll = qMax(0, scaledImageSize().height() - imageRect().height());
    imageScrollY_ = qBound(0, imageScrollY_, maxScroll);
}

void PinWindow::moveImageTopLeft(const QPoint& topLeft)
{
    resizeToScale(topLeft);
    move(topLeft - imageRect().topLeft());
    clampImageScroll();
}

void PinWindow::drawViewportScrollbar(QPainter& painter, const QRect& content) const
{
    const int fullHeight = scaledImageSize().height();
    if (fullHeight <= content.height()) {
        return;
    }
    const qreal ratio = static_cast<qreal>(content.height()) / fullHeight;
    const int thumbHeight = qMax(28, static_cast<int>(content.height() * ratio));
    const int maxScroll = qMax(1, fullHeight - content.height());
    const int trackTop = content.top() + 6;
    const int trackHeight = qMax(1, content.height() - 12);
    const int thumbTop = trackTop + static_cast<int>((trackHeight - thumbHeight) * (static_cast<qreal>(imageScrollY_) / maxScroll));
    const QRect track(content.right() - 8, trackTop, 4, trackHeight);
    const QRect thumb(track.left() - 1, thumbTop, 6, thumbHeight);

    painter.setPen(Qt::NoPen);
    painter.setBrush(QColor(15, 23, 42, 72));
    painter.drawRoundedRect(track, 2, 2);
    painter.setBrush(QColor(255, 255, 255, 190));
    painter.drawRoundedRect(thumb, 3, 3);
}

bool PinWindow::event(QEvent* event)
{
    switch (event->type()) {
    case QEvent::WindowActivate:
    case QEvent::FocusIn:
        emit becameActive(this);
        break;
    case QEvent::WindowDeactivate:
    case QEvent::FocusOut:
        emit deactivated(this);
        break;
    case QEvent::ActivationChange:
        if (isActiveWindow()) {
            emit becameActive(this);
        } else {
            emit deactivated(this);
        }
        break;
    default:
        break;
    }
    return QWidget::event(event);
}

void PinWindow::paintEvent(QPaintEvent* event)
{
    QPainter painter(this);
    painter.setRenderHint(QPainter::SmoothPixmapTransform, true);
    painter.setRenderHint(QPainter::Antialiasing, true);

    const QRect content = imageRect();
    if (shadowEnabled_) {
        const QRectF shadowRect = QRectF(content).translated(0, kPinShadowOffset);
        const qreal margin = static_cast<qreal>(kPinShadowMargin);
        const QGradientStops stops = pinShadowStops();

        auto fillLinear = [&painter, &stops](const QRectF& area, const QPointF& innerEdge, const QPointF& outerEdge) {
            QLinearGradient gradient(innerEdge, outerEdge);
            gradient.setStops(stops);
            painter.fillRect(area, gradient);
        };
        auto fillCorner = [&painter, &stops, margin](const QRectF& area, const QPointF& center) {
            QRadialGradient gradient(center, margin);
            gradient.setStops(stops);
            painter.fillRect(area, gradient);
        };

        painter.setPen(Qt::NoPen);
        fillLinear(QRectF(shadowRect.left(), shadowRect.top() - margin, shadowRect.width(), margin),
                   QPointF(0, shadowRect.top()), QPointF(0, shadowRect.top() - margin));
        fillLinear(QRectF(shadowRect.left(), shadowRect.bottom(), shadowRect.width(), margin),
                   QPointF(0, shadowRect.bottom()), QPointF(0, shadowRect.bottom() + margin));
        fillLinear(QRectF(shadowRect.left() - margin, shadowRect.top(), margin, shadowRect.height()),
                   QPointF(shadowRect.left(), 0), QPointF(shadowRect.left() - margin, 0));
        fillLinear(QRectF(shadowRect.right(), shadowRect.top(), margin, shadowRect.height()),
                   QPointF(shadowRect.right(), 0), QPointF(shadowRect.right() + margin, 0));
        fillCorner(QRectF(shadowRect.left() - margin, shadowRect.top() - margin, margin, margin), shadowRect.topLeft());
        fillCorner(QRectF(shadowRect.right(), shadowRect.top() - margin, margin, margin), shadowRect.topRight());
        fillCorner(QRectF(shadowRect.left() - margin, shadowRect.bottom(), margin, margin), shadowRect.bottomLeft());
        fillCorner(QRectF(shadowRect.right(), shadowRect.bottom(), margin, margin), shadowRect.bottomRight());
    }

    painter.save();
    painter.setClipRect(content);
    const QRect scaledTarget(content.left(), content.top() - imageScrollY_, scaledImageSize().width(), scaledImageSize().height());
    const QRect exposed = event->rect().intersected(content).intersected(scaledTarget);
    if (exposed.isValid()) {
        const qreal sourceScaleX = static_cast<qreal>(image_.width()) / scaledTarget.width();
        const qreal sourceScaleY = static_cast<qreal>(image_.height()) / scaledTarget.height();
        const QRectF source((exposed.x() - scaledTarget.x()) * sourceScaleX,
                            (exposed.y() - scaledTarget.y()) * sourceScaleY,
                            exposed.width() * sourceScaleX,
                            exposed.height() * sourceScaleY);
        painter.drawImage(QRectF(exposed), image_, source);
    }
    painter.restore();

    if (shadowEnabled_) {
        // Hairline border keeps light screenshots readable without adding a
        // visible white frame around the pinned image.
        painter.setPen(QPen(QColor(15, 23, 42, activeVisual_ ? 36 : 38), 1));
        painter.setBrush(Qt::NoBrush);
        painter.drawRect(QRectF(content).adjusted(0.5, 0.5, -0.5, -0.5));

        if (activeVisual_) {
            painter.setPen(QPen(QColor(128, 165, 235, 24), 3));
            painter.drawRect(QRectF(content).adjusted(-1.5, -1.5, 1.5, 1.5));

            painter.setPen(QPen(QColor(128, 165, 235, 130), 1));
            painter.drawRect(QRectF(content).adjusted(-0.5, -0.5, 0.5, 0.5));
        }
    }

    drawViewportScrollbar(painter, content);

    if (mouseThrough_) {
        painter.setPen(QPen(QColor(79, 124, 255, 170), 2));
        painter.setBrush(Qt::NoBrush);
        painter.drawRect(content.adjusted(0, 0, -1, -1));

        painter.setPen(Qt::NoPen);
        painter.setBrush(QColor(79, 124, 255, 210));
        const QRect badge(content.right() - 53, content.top() + 6, 48, 20);
        painter.drawRoundedRect(badge, 10, 10);
        painter.setPen(Qt::white);
        painter.drawText(badge, Qt::AlignCenter, QStringLiteral("穿透"));
    }
}

void PinWindow::mousePressEvent(QMouseEvent* event)
{
    emit becameActive(this);
    if (event->button() == Qt::LeftButton) {
        dragging_ = true;
        dragStartGlobal_ = event->globalPosition().toPoint();
        dragStartPos_ = frameGeometry().topLeft();
        raise();
        event->accept();
        return;
    }
    QWidget::mousePressEvent(event);
}

void PinWindow::mouseMoveEvent(QMouseEvent* event)
{
    if (dragging_) {
        const QPoint delta = event->globalPosition().toPoint() - dragStartGlobal_;
        move(dragStartPos_ + delta);
        event->accept();
        return;
    }
    QWidget::mouseMoveEvent(event);
}

void PinWindow::mouseReleaseEvent(QMouseEvent* event)
{
    if (event->button() == Qt::LeftButton) {
        dragging_ = false;
        event->accept();
        return;
    }
    QWidget::mouseReleaseEvent(event);
}

void PinWindow::mouseDoubleClickEvent(QMouseEvent* event)
{
    if (settings_.doubleClickHide && event->button() == Qt::LeftButton) {
        hide();
        event->accept();
        return;
    }
    QWidget::mouseDoubleClickEvent(event);
}

void PinWindow::wheelEvent(QWheelEvent* event)
{
    const int delta = event->angleDelta().y();
    if (delta == 0) {
        return;
    }
    if (event->modifiers().testFlag(Qt::ControlModifier)) {
        opacity_ = qBound(0.2, opacity_ + (delta > 0 ? 0.05 : -0.05), 1.0);
        setWindowOpacity(opacity_ < 0.999 ? opacity_ : 1.0);
        event->accept();
        return;
    }

    const int maxScroll = qMax(0, scaledImageSize().height() - imageRect().height());
    if (maxScroll > 0 && !event->modifiers().testFlag(Qt::ShiftModifier)) {
        int steps = delta / 120;
        if (steps == 0) {
            steps = delta > 0 ? 1 : -1;
        }
        const int scrollStep = qMax(32, imageRect().height() / 8);
        imageScrollY_ = qBound(0, imageScrollY_ - steps * scrollStep, maxScroll);
        update();
        event->accept();
        return;
    }

    const QPoint globalBefore = event->globalPosition().toPoint();
    const QPoint localBefore = event->position().toPoint() - imageRect().topLeft();
    const double factor = delta > 0 ? (1.0 + settings_.wheelScaleStep) : (1.0 - settings_.wheelScaleStep);
    scale_ = qBound(0.1, scale_ * factor, 6.0);
    resizeToScale(globalBefore);
    move(globalBefore - imageRect().topLeft() - QPoint(static_cast<int>(localBefore.x() * factor), static_cast<int>(localBefore.y() * factor)));
    event->accept();
}

void PinWindow::contextMenuEvent(QContextMenuEvent* event)
{
    emit becameActive(this);
    QMenu menu(this);
    QAction* copy = menu.addAction(QStringLiteral("复制"));
    QAction* save = menu.addAction(QStringLiteral("保存为..."));
    menu.addSeparator();
    QAction* top = menu.addAction(onTop_ ? QStringLiteral("取消置顶") : QStringLiteral("置顶"));
    QAction* pass = menu.addAction(mouseThrough_ ? QStringLiteral("关闭鼠标穿透") : QStringLiteral("鼠标穿透"));
    QAction* shadow = menu.addAction(shadowEnabled_ ? QStringLiteral("取消阴影") : QStringLiteral("显示阴影"));
    menu.addSeparator();
    QAction* closeAction = menu.addAction(QStringLiteral("关闭"));

    QAction* chosen = menu.exec(event->globalPos());
    if (chosen == copy) {
        copyImage();
    } else if (chosen == save) {
        saveImage();
    } else if (chosen == top) {
        setPinnedOnTop(!onTop_);
    } else if (chosen == pass) {
        toggleMouseThrough();
    } else if (chosen == shadow) {
        toggleShadow();
    } else if (chosen == closeAction) {
        this->close();
    }
}

void PinWindow::keyPressEvent(QKeyEvent* event)
{
    if (event->key() == Qt::Key_Escape) {
        close();
        return;
    }
    if (event->matches(QKeySequence::Copy)) {
        copyImage();
        return;
    }
    if (event->matches(QKeySequence::Save)) {
        saveImage();
        return;
    }
    QWidget::keyPressEvent(event);
}

void PinWindow::setPinnedOnTop(bool onTop)
{
    onTop_ = onTop;
    const QPoint topLeft = frameGeometry().topLeft();
    Qt::WindowFlags flags = windowFlags();
    if (onTop) {
        flags |= Qt::WindowStaysOnTopHint;
    } else {
        flags &= ~Qt::WindowStaysOnTopHint;
    }
    setWindowFlags(flags);
    move(topLeft);
    show();
    if (opacity_ < 0.999) {
        setWindowOpacity(opacity_);
    }
}

void PinWindow::toggleMouseThrough()
{
    mouseThrough_ = !mouseThrough_;
    applyMouseThrough();
    update();
}

void PinWindow::toggleShadow()
{
    const QPoint imageTopLeft = geometry().topLeft() + imageRect().topLeft();
    shadowEnabled_ = !shadowEnabled_;
    resizeToScale();
    moveImageTopLeft(imageTopLeft);
    update();
}

void PinWindow::setActiveVisual(bool active)
{
    if (activeVisual_ == active) {
        return;
    }
    activeVisual_ = active;
    update();
}

void PinWindow::applyMouseThrough()
{
#ifdef Q_OS_WIN
    HWND hwnd = reinterpret_cast<HWND>(winId());
    LONG_PTR exStyle = GetWindowLongPtr(hwnd, GWL_EXSTYLE);
    if (mouseThrough_) {
        exStyle |= WS_EX_TRANSPARENT;
    } else {
        exStyle &= ~WS_EX_TRANSPARENT;
    }
    SetWindowLongPtr(hwnd, GWL_EXSTYLE, exStyle);
#else
    setAttribute(Qt::WA_TransparentForMouseEvents, mouseThrough_);
#endif
}

void PinWindow::copyImage()
{
    QApplication::clipboard()->setImage(image_);
}

void PinWindow::saveImage()
{
    if (output_) {
        output_->saveAs(image_, this);
    }
}

} // namespace Visnip
