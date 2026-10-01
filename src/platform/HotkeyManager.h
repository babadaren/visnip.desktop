#pragma once

#include <QAbstractNativeEventFilter>
#include <QHash>
#include <QKeySequence>
#include <QMap>
#include <QObject>
#include <QString>

namespace Visnip {

enum class GlobalHotkeyAction {
    Capture,
    PinClipboard,
    RepeatCapture,
    TogglePins,
    ToggleMouseThrough,
    // Auxiliary: registered only while the question panel is open.
    AskQuestion
};

struct HotkeyValidationResult {
    bool valid = false;
    QString errorMessage;
};

using GlobalHotkeyBindings = QMap<GlobalHotkeyAction, QKeySequence>;

class HotkeyManager : public QObject, public QAbstractNativeEventFilter {
    Q_OBJECT
public:
    explicit HotkeyManager(QObject* parent = nullptr);
    ~HotkeyManager() override;

    [[nodiscard]] static HotkeyValidationResult validateSequence(const QKeySequence& sequence);

    bool registerHotkey(GlobalHotkeyAction action, const QKeySequence& sequence);
    bool replaceHotkeys(const GlobalHotkeyBindings& bindings, QString* errorMessage = nullptr);
    // Auxiliary hotkeys live outside the core set passed to replaceHotkeys():
    // each is registered and released on its own, and failing to register
    // one never affects the core bindings.
    bool setAuxiliaryHotkey(GlobalHotkeyAction action, const QKeySequence& sequence,
                            QString* errorMessage = nullptr);
    void clearAuxiliaryHotkey(GlobalHotkeyAction action);
    void unregisterAll();
    bool nativeEventFilter(const QByteArray& eventType, void* message, qintptr* result) override;

signals:
    void activated(GlobalHotkeyAction action);
    void registrationFailed(GlobalHotkeyAction action, QKeySequence sequence);

private:
    int nextId_ = 100;
    QHash<int, GlobalHotkeyAction> idToAction_;
    QHash<int, QKeySequence> idToSequence_;
    QMap<GlobalHotkeyAction, int> auxiliaryIds_;
};

} // namespace Visnip
