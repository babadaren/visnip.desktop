#pragma once

#include "core/AppConfig.h"

#include <QObject>
#include <QPoint>
#include <QPointer>
#include <QVector>

namespace Visnip {

class ImageOutputService;
class PinWindow;

class PinManager : public QObject {
    Q_OBJECT
public:
    explicit PinManager(AppConfig* config, ImageOutputService* output, QObject* parent = nullptr);

    PinWindow* createPin(const QImage& image);
    PinWindow* createPin(const QImage& image, const QPoint& topLeft);
    bool createFromClipboard();
    void toggleAllVisible();
    void closeAll();
    void toggleActiveMouseThrough();
    int count() const { return pins_.size(); }

signals:
    void pinCreated(PinWindow* window);
    void allClosed();

private:
    void setActivePin(PinWindow* window);
    void syncActiveVisual();
    bool isManagedPin(const PinWindow* window) const;
    PinWindow* activePinFromApplication() const;
    PinWindow* shortcutTarget() const;
    void removePin(PinWindow* window);

    AppConfig* config_ = nullptr;
    ImageOutputService* output_ = nullptr;
    QVector<QPointer<PinWindow>> pins_;
    QPointer<PinWindow> lastActivePin_;
    bool hiddenByToggle_ = false;
};

} // namespace Visnip
