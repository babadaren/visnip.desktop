#pragma once

#include "core/AppConfig.h"
#include "platform/HotkeyManager.h"

#include <QObject>
#include <QPointer>
#include <QRect>
#include <QSystemTrayIcon>

class QAction;
class QImage;

namespace Visnip {

class CaptureOverlayWindow;
class ImageTranslationService;
class ImageOutputService;
class OcrPackDownloadService;
class OcrService;
class PinManager;
class QuestionPanel;
class SettingsDialog;

class AppController : public QObject {
    Q_OBJECT
public:
    explicit AppController(QObject* parent = nullptr);
    ~AppController() override;

    bool initialize();
    void showSettings();
    void startCapture();
    void repeatCapture();
    void pinClipboard();

private slots:
    void handleHotkey(GlobalHotkeyAction action);

private:
    void setupTray();
    void setupHotkeys();
    void applyConfiguredHotkeys();
    void applyRequestedHotkeys(const HotkeySettings& candidate);
    void updateTrayHotkeyText();
    void beginCapture(const QRect& initialSelection, bool notifyOutput);
    void showQuestionPanel(const QRect& region, const QImage& preview);
    void registerQuestionHotkey();
    void showQuestionSettings();
    SettingsDialog* ensureSettingsDialog();
    void presentSettingsDialog(SettingsDialog* dialog);
    void showTrayMessage(const QString& title, const QString& message, QSystemTrayIcon::MessageIcon icon = QSystemTrayIcon::Information);
    QIcon appIcon() const;

    AppConfig config_;
    QSystemTrayIcon* tray_ = nullptr;
    QAction* trayCaptureAction_ = nullptr;
    QAction* trayRepeatAction_ = nullptr;
    QAction* trayPinAction_ = nullptr;
    QAction* trayTogglePinsAction_ = nullptr;
    HotkeyManager* hotkeys_ = nullptr;
    ImageOutputService* output_ = nullptr;
    ImageTranslationService* imageTranslation_ = nullptr;
    OcrService* ocr_ = nullptr;
    OcrPackDownloadService* ocrPackDownloads_ = nullptr;
    PinManager* pins_ = nullptr;
    QPointer<SettingsDialog> settingsDialog_;
    QPointer<CaptureOverlayWindow> captureWindow_;
    QPointer<QuestionPanel> questionPanel_;
    // The panel was hidden for a capture and comes back when it ends.
    bool restoreQuestionPanel_ = false;
    HotkeySettings activeHotkeys_;
    bool hotkeysActive_ = false;
    QRect lastSelection_;
};

} // namespace Visnip
