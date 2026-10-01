#include "ui/pin/PinManager.h"

#include "services/ImageOutputService.h"
#include "ui/pin/PinWindow.h"

#include <QApplication>
#include <QClipboard>
#include <QEventLoop>
#include <QGuiApplication>
#include <QImage>
#include <QTimer>
#include <QWindow>

#ifdef Q_OS_WIN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace Visnip {

namespace {
void activatePinWindow(PinWindow* pin)
{
    if (!pin) {
        return;
    }
    pin->raise();
    pin->activateWindow();
#ifdef Q_OS_WIN
    SetForegroundWindow(reinterpret_cast<HWND>(pin->winId()));
#endif
}

#ifdef Q_OS_WIN
HWND rootWindow(HWND hwnd)
{
    return hwnd ? GetAncestor(hwnd, GA_ROOT) : nullptr;
}
#endif
} // namespace

PinManager::PinManager(AppConfig* config, ImageOutputService* output, QObject* parent)
    : QObject(parent)
    , config_(config)
    , output_(output)
{
    connect(qApp, &QGuiApplication::applicationStateChanged, this, [this](Qt::ApplicationState) { syncActiveVisual(); });
    connect(qApp, &QGuiApplication::focusWindowChanged, this, [this](QWindow*) {
        QTimer::singleShot(0, this, [this]() { syncActiveVisual(); });
    });
    connect(qApp, &QApplication::focusChanged, this, [this](QWidget*, QWidget*) {
        QTimer::singleShot(0, this, [this]() { syncActiveVisual(); });
    });
}

PinWindow* PinManager::createPin(const QImage& image)
{
    if (image.isNull()) {
        return nullptr;
    }
    auto* pin = new PinWindow(image, config_->settings().pin);
    pin->setOutputService(output_);
    connect(pin, &QObject::destroyed, this, [this, pin]() { removePin(pin); });
    connect(pin, &PinWindow::becameActive, this, &PinManager::setActivePin);
    connect(pin, &PinWindow::deactivated, this, [this](PinWindow*) {
        QTimer::singleShot(0, this, [this]() { syncActiveVisual(); });
    });
    pins_.append(pin);
    pin->show();
    activatePinWindow(pin);
    pin->repaint();
    QApplication::processEvents(QEventLoop::ExcludeUserInputEvents);
    setActivePin(pin);
    QTimer::singleShot(0, this, [this]() { syncActiveVisual(); });
    QTimer::singleShot(120, this, [this]() { syncActiveVisual(); });
    emit pinCreated(pin);
    return pin;
}

PinWindow* PinManager::createPin(const QImage& image, const QPoint& topLeft)
{
    if (image.isNull()) {
        return nullptr;
    }
    auto* pin = new PinWindow(image, config_->settings().pin);
    pin->setOutputService(output_);
    connect(pin, &QObject::destroyed, this, [this, pin]() { removePin(pin); });
    connect(pin, &PinWindow::becameActive, this, &PinManager::setActivePin);
    connect(pin, &PinWindow::deactivated, this, [this](PinWindow*) {
        QTimer::singleShot(0, this, [this]() { syncActiveVisual(); });
    });
    pins_.append(pin);
    pin->moveImageTopLeft(topLeft);
    pin->show();
    activatePinWindow(pin);
    pin->repaint();
    QApplication::processEvents(QEventLoop::ExcludeUserInputEvents);
    setActivePin(pin);
    emit pinCreated(pin);
    QTimer::singleShot(0, pin, [pin]() { activatePinWindow(pin); });
    QTimer::singleShot(0, this, [this]() { syncActiveVisual(); });
    QTimer::singleShot(120, this, [this]() { syncActiveVisual(); });
    return pin;
}

bool PinManager::createFromClipboard()
{
    const QImage image = QApplication::clipboard()->image();
    if (image.isNull()) {
        return false;
    }
    return createPin(image) != nullptr;
}

void PinManager::toggleAllVisible()
{
    hiddenByToggle_ = !hiddenByToggle_;
    for (const auto& pin : pins_) {
        if (!pin) {
            continue;
        }
        if (hiddenByToggle_) {
            pin->setActiveVisual(false);
            pin->hide();
        } else {
            pin->show();
            pin->raise();
        }
    }
}

void PinManager::closeAll()
{
    const auto pins = pins_;
    for (const auto& pin : pins) {
        if (pin) {
            pin->close();
        }
    }
}

void PinManager::toggleActiveMouseThrough()
{
    if (PinWindow* target = shortcutTarget()) {
        lastActivePin_ = target;
        target->toggleMouseThrough();
    }
}

void PinManager::setActivePin(PinWindow* window)
{
    if (!isManagedPin(window)) {
        return;
    }
    lastActivePin_ = window;
    syncActiveVisual();
}

void PinManager::syncActiveVisual()
{
    PinWindow* activePin = activePinFromApplication();
    if (activePin) {
        lastActivePin_ = activePin;
    }
    for (const auto& pin : pins_) {
        if (pin) {
            pin->setActiveVisual(pin.data() == activePin);
        }
    }
}

bool PinManager::isManagedPin(const PinWindow* window) const
{
    for (const auto& pin : pins_) {
        if (pin && pin.data() == window) {
            return true;
        }
    }
    return false;
}

PinWindow* PinManager::activePinFromApplication() const
{
#ifdef Q_OS_WIN
    const HWND foreground = GetForegroundWindow();
    const HWND foregroundRoot = rootWindow(foreground);
    for (const auto& pin : pins_) {
        if (!pin || !pin->isVisible()) {
            continue;
        }
        const HWND pinHwnd = reinterpret_cast<HWND>(pin->winId());
        if (foreground == pinHwnd || foregroundRoot == rootWindow(pinHwnd)) {
            return pin;
        }
    }
    return nullptr;
#else
    if (QGuiApplication::applicationState() != Qt::ApplicationActive) {
        return nullptr;
    }

    QWidget* activeWindow = QApplication::activeWindow();
    for (const auto& pin : pins_) {
        if (pin && activeWindow == pin.data()) {
            return pin;
        }
    }

    QWindow* focusWindow = QGuiApplication::focusWindow();
    for (const auto& pin : pins_) {
        if (pin && pin->windowHandle() == focusWindow) {
            return pin;
        }
    }
    return nullptr;
#endif
}

PinWindow* PinManager::shortcutTarget() const
{
    if (lastActivePin_ && lastActivePin_->isVisible() && lastActivePin_->mouseThrough()) {
        return lastActivePin_;
    }

    for (auto it = pins_.crbegin(); it != pins_.crend(); ++it) {
        if (*it && (*it)->isVisible() && (*it)->mouseThrough()) {
            return it->data();
        }
    }

    if (PinWindow* activePin = activePinFromApplication()) {
        return activePin;
    }
    return lastActivePin_ && lastActivePin_->isVisible() ? lastActivePin_.data() : nullptr;
}

void PinManager::removePin(PinWindow* window)
{
    const bool hadPins = !pins_.isEmpty();
    for (int i = pins_.size() - 1; i >= 0; --i) {
        if (pins_.at(i).isNull() || pins_.at(i).data() == window) {
            pins_.removeAt(i);
        }
    }
    if (lastActivePin_.isNull() || lastActivePin_.data() == window) {
        lastActivePin_.clear();
    }
    if (hadPins && pins_.isEmpty()) {
        emit allClosed();
    }
}

} // namespace Visnip
