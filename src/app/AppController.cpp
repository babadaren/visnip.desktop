#include "app/AppController.h"

#include "core/DesignTokens.h"
#include "core/PerfLog.h"
#include "services/ImageOutputService.h"
#include "services/ImageTranslationService.h"
#include "services/LocalTextTranslationService.h"
#include "services/OcrPackDownloadService.h"
#include "services/OcrService.h"
#include "services/OfflineTranslationService.h"
#include "ui/IconUtils.h"
#include "ui/capture/CaptureOverlayWindow.h"
#include "ui/pin/PinManager.h"
#include "ui/question/QuestionPanel.h"
#include "ui/settings/SettingsDialog.h"

#include <QAction>
#include <QApplication>
#include <QClipboard>
#include <QDebug>
#include <QElapsedTimer>
#include <QFutureWatcher>
#include <QMenu>
#include <QScreen>
#include <QStringList>
#include <QTimer>

namespace Visnip {

namespace {
constexpr int kOcrWarmupDelayMs = 1400;
// Lets the compositor drop the hidden question panel before the desktop grab.
constexpr int kQuestionPanelHideSettleMs = 60;

QString hotkeyActionName(GlobalHotkeyAction action)
{
    switch (action) {
    case GlobalHotkeyAction::Capture:
        return QStringLiteral("Capture");
    case GlobalHotkeyAction::PinClipboard:
        return QStringLiteral("PinClipboard");
    case GlobalHotkeyAction::RepeatCapture:
        return QStringLiteral("RepeatCapture");
    case GlobalHotkeyAction::TogglePins:
        return QStringLiteral("TogglePins");
    case GlobalHotkeyAction::ToggleMouseThrough:
        return QStringLiteral("ToggleMouseThrough");
    case GlobalHotkeyAction::AskQuestion:
        return QStringLiteral("AskQuestion");
    }
    return QStringLiteral("Unknown");
}

GlobalHotkeyBindings hotkeyBindings(const HotkeySettings& settings)
{
    return {
        {GlobalHotkeyAction::Capture, settings.capture},
        {GlobalHotkeyAction::PinClipboard, settings.pinClipboard},
        {GlobalHotkeyAction::RepeatCapture, settings.repeatCapture},
        {GlobalHotkeyAction::TogglePins, settings.togglePins},
        {GlobalHotkeyAction::ToggleMouseThrough, settings.toggleMouseThrough},
    };
}

QString actionWithHotkey(const QString& action, const QKeySequence& sequence)
{
    return QStringLiteral("%1\t%2")
        .arg(action, sequence.toString(QKeySequence::NativeText));
}

bool sameHotkeys(const HotkeySettings& left, const HotkeySettings& right)
{
    return left.capture == right.capture
        && left.pinClipboard == right.pinClipboard
        && left.repeatCapture == right.repeatCapture
        && left.togglePins == right.togglePins
        && left.toggleMouseThrough == right.toggleMouseThrough
        && left.askQuestion == right.askQuestion;
}
} // namespace

AppController::AppController(QObject* parent)
    : QObject(parent)
{
}

AppController::~AppController()
{
    delete settingsDialog_.data();
    delete captureWindow_.data();
    delete questionPanel_.data();
}

bool AppController::initialize()
{
    Perf::ScopedTimer timer(QStringLiteral("AppController.initialize"));
    QElapsedTimer phase;

    phase.start();
    QApplication::setQuitOnLastWindowClosed(false);
    qApp->setApplicationName(QStringLiteral("Visnip"));
    qApp->setOrganizationName(QStringLiteral("Visnip"));
    qApp->setWindowIcon(appIcon());
    qApp->setStyleSheet(Design::appStyleSheet(false));
    Perf::logDuration(QStringLiteral("initialize.qt_app_setup"), phase.elapsed());

    phase.restart();
    config_.load();
    config_.syncAutoStart();
    Perf::logDuration(QStringLiteral("initialize.config_load"), phase.elapsed());

    phase.restart();
    output_ = new ImageOutputService(&config_, this);
    imageTranslation_ = new ImageTranslationService(&config_, this);
    // Share the preloaded session with settings self-tests and subsequent captures.
    // Models are never started for cloud-only users or uninstalled resources.
    connect(&config_, &AppConfig::changed, this,
        [this, route = config_.settings().aiTranslate.translationMethodCacheKey()]() mutable {
            const auto& current = config_.settings().aiTranslate;
            const QString next = current.translationMethodCacheKey();
            if (next == route) return;
            route = next;
            // Only the selected offline tier may hold model memory.
            if (!current.usesLiteOfflineEngine()) LocalTextTranslationService::releaseSharedEngine();
            if (!current.usesPreciseOfflineEngine()) OfflineTranslationService::releaseSharedEngine();
            if (current.usesLiteOfflineEngine()) {
                LocalTextTranslationService::prewarm(current.offlineResourceDirectory);
            } else if (current.usesPreciseOfflineEngine()
                && OfflineTranslationService::resourceProblem(current.offlineResourceDirectory, QStringLiteral("precise")).isEmpty()) {
                OfflineTranslationService::prewarm(current.offlineResourceDirectory);
            }
        });
    QTimer::singleShot(1500, this, [this]() {
        const auto& current = config_.settings().aiTranslate;
        if (current.usesLiteOfflineEngine()) LocalTextTranslationService::prewarm(current.offlineResourceDirectory);
        else if (current.usesPreciseOfflineEngine()) OfflineTranslationService::prewarm(current.offlineResourceDirectory);
    });
    connect(&config_, &AppConfig::changed, imageTranslation_,
            [this, previousEndpoint =
                       config_.settings().aiTranslate.fastServiceBaseUrl()]() mutable {
        const QString endpoint = config_.settings().aiTranslate.fastServiceBaseUrl();
        if (endpoint == previousEndpoint) {
            return;
        }
        previousEndpoint = endpoint;
        QTimer::singleShot(0, imageTranslation_,
                           &ImageTranslationService::preconnectConfiguredEndpoint);
    });
    ocr_ = new OcrService(this);
    ocrPackDownloads_ = new OcrPackDownloadService(this);
    connect(ocrPackDownloads_, &OcrPackDownloadService::succeeded, ocr_,
            [this](const QString& packId) {
        const auto& current = config_.settings().aiTranslate;
        if ((current.usesLegacyOnlineTextTranslation() || current.usesLiteOfflineEngine())
            && current.fastOcrPackId == packId) {
            ocr_->prewarm(packId);
        }
    });
    pins_ = new PinManager(&config_, output_, this);
    connect(&config_, &AppConfig::changed, ocr_,
            [this, previousMethod = config_.settings().aiTranslate.translationMethod,
             previousPackId = config_.settings().aiTranslate.fastOcrPackId,
             previousLite = config_.settings().aiTranslate.usesLiteOfflineEngine()]() mutable {
        const QString packId = config_.settings().aiTranslate.fastOcrPackId;
        const TranslationMethod method =
            config_.settings().aiTranslate.translationMethod;
        const bool lite = config_.settings().aiTranslate.usesLiteOfflineEngine();
        if (method == previousMethod && packId == previousPackId && lite == previousLite) {
            return;
        }
        previousMethod = method;
        previousPackId = packId;
        previousLite = lite;
        if (!config_.settings().aiTranslate.usesLegacyOnlineTextTranslation() && !lite) {
            return;
        }
        if (OcrService::assetsPresent(packId)) {
            ocr_->prewarm(packId);
        }
    });
    connect(ocr_, &OcrService::prewarmFinished, this,
            [](const QString& packId, bool ready,
               const QString& error, qint64 elapsedMs) {
        if (ready) {
            Perf::log(QStringLiteral("AppController.ocrWarmup pack=%1 ready=1 elapsed=%2ms")
                          .arg(packId)
                          .arg(elapsedMs));
        } else {
            Perf::log(QStringLiteral("AppController.ocrWarmup pack=%1 ready=0 elapsed=%2ms error=\"%3\"")
                          .arg(packId)
                          .arg(elapsedMs)
                          .arg(error));
        }
    });
    Perf::logDuration(QStringLiteral("initialize.services_create"), phase.elapsed());

    phase.restart();
    setupTray();
    Perf::logDuration(QStringLiteral("initialize.setup_tray"), phase.elapsed());

    phase.restart();
    setupHotkeys();
    Perf::logDuration(QStringLiteral("initialize.setup_hotkeys"), phase.elapsed());

    phase.restart();
    // Populate the font database on a worker thread first and run the
    // widget/raster warmups only once it is ready: running them synchronously
    // here blocked startup for the whole first font enumeration (0.7s-3.8s
    // measured in warmup.options_bar).
    auto* fontCatalogWatcher = new QFutureWatcher<QStringList>(this);
    connect(fontCatalogWatcher, &QFutureWatcher<QStringList>::finished, this,
            [this, fontCatalogWatcher]() {
        fontCatalogWatcher->deleteLater();
        if (captureWindow_) {
            Perf::log(QStringLiteral("AppController.captureWarmup skipped=capture_active"));
            return;
        }
        CaptureOverlayWindow::warmupCaptureBackend();
    });
    fontCatalogWatcher->setFuture(CaptureOverlayWindow::warmupFontCatalog());
    Perf::logDuration(QStringLiteral("initialize.capture_warmup_scheduled"), phase.elapsed());

    QTimer::singleShot(kOcrWarmupDelayMs, this, [this]() {
        if (!config_.settings().aiTranslate.allowsTranslationNetwork()
            && !config_.settings().aiTranslate.usesLiteOfflineEngine()) {
            return; // The precise engine runs its own OCR on demand.
        }
        if (config_.settings().aiTranslate.usesCloudImageTranslation()) {
            imageTranslation_->preconnectConfiguredEndpoint();
            Perf::log(QStringLiteral("AppController.ocrWarmup skipped=cloud_image_pipeline preconnect=requested"));
            return;
        }
        const QString packId = config_.settings().aiTranslate.fastOcrPackId;
        QString missing;
        if (!OcrService::assetsPresent(packId, &missing)) {
            Perf::log(QStringLiteral("AppController.ocrWarmup skipped=assets_missing file=%1")
                          .arg(missing));
            return;
        }
        Perf::log(QStringLiteral("AppController.ocrWarmup queued=1 pack=%1")
                      .arg(packId));
        ocr_->prewarm(packId);
    });
    return true;
}

QIcon AppController::appIcon() const
{
    return Ui::applicationIcon();
}

void AppController::setupTray()
{
    tray_ = new QSystemTrayIcon(appIcon(), this);
    auto* menu = new QMenu;

    trayCaptureAction_ = menu->addAction(QStringLiteral("截图"));
    trayRepeatAction_ = menu->addAction(QStringLiteral("重复上次截图区域"));
    trayPinAction_ = menu->addAction(QStringLiteral("贴图"));
    trayTogglePinsAction_ = menu->addAction(QStringLiteral("隐藏/显示所有贴图"));
    menu->addSeparator();
    QAction* settings = menu->addAction(QStringLiteral("首选项..."));
    menu->addSeparator();
    QAction* quit = menu->addAction(QStringLiteral("退出"));

    connect(trayCaptureAction_, &QAction::triggered, this, &AppController::startCapture);
    connect(trayRepeatAction_, &QAction::triggered, this, &AppController::repeatCapture);
    connect(trayPinAction_, &QAction::triggered, this, &AppController::pinClipboard);
    connect(trayTogglePinsAction_, &QAction::triggered, pins_, &PinManager::toggleAllVisible);
    connect(settings, &QAction::triggered, this, &AppController::showSettings);
    connect(quit, &QAction::triggered, qApp, &QCoreApplication::quit);

    tray_->setContextMenu(menu);
    updateTrayHotkeyText();
    connect(tray_, &QSystemTrayIcon::activated, this, [this](QSystemTrayIcon::ActivationReason reason) {
        if (reason == QSystemTrayIcon::DoubleClick || reason == QSystemTrayIcon::Trigger) {
            startCapture();
        }
    });
    tray_->show();
}

void AppController::setupHotkeys()
{
    hotkeys_ = new HotkeyManager(this);
    connect(hotkeys_, &HotkeyManager::activated, this, &AppController::handleHotkey);
    applyConfiguredHotkeys();
}

void AppController::applyConfiguredHotkeys()
{
    const auto& h = config_.settings().hotkeys;
    QString error;
    if (!hotkeys_->replaceHotkeys(hotkeyBindings(h), &error)) {
        showTrayMessage(QStringLiteral("快捷键注册失败"), error,
                        QSystemTrayIcon::Warning);
        return;
    }
    activeHotkeys_ = h;
    hotkeysActive_ = true;
    updateTrayHotkeyText();
}

void AppController::applyRequestedHotkeys(const HotkeySettings& candidate)
{
    const HotkeySettings previous = hotkeysActive_
        ? activeHotkeys_
        : config_.settings().hotkeys;
    QString error;
    if (!hotkeys_->replaceHotkeys(hotkeyBindings(candidate), &error)) {
        if (!sameHotkeys(config_.settings().hotkeys, previous)) {
            config_.mutableSettings().hotkeys = previous;
            config_.save();
        }
        if (settingsDialog_) {
            settingsDialog_->setHotkeyChangeResult(previous, error);
        }
        showTrayMessage(QStringLiteral("快捷键未更改"), error,
                        QSystemTrayIcon::Warning);
        return;
    }

    bool saved = true;
    if (!sameHotkeys(config_.settings().hotkeys, candidate)) {
        config_.mutableSettings().hotkeys = candidate;
        saved = config_.save();
    }
    if (!saved) {
        QString rollbackError;
        hotkeys_->replaceHotkeys(hotkeyBindings(previous), &rollbackError);
        config_.mutableSettings().hotkeys = previous;
        error = QStringLiteral("配置无法保存，已恢复原快捷键。");
        if (settingsDialog_) {
            settingsDialog_->setHotkeyChangeResult(previous, error);
        }
        showTrayMessage(QStringLiteral("快捷键未更改"), error,
                        QSystemTrayIcon::Warning);
        return;
    }

    activeHotkeys_ = candidate;
    hotkeysActive_ = true;
    updateTrayHotkeyText();
    registerQuestionHotkey();
    if (settingsDialog_) {
        settingsDialog_->setHotkeyChangeResult(candidate);
    }
}

void AppController::updateTrayHotkeyText()
{
    if (!tray_) {
        return;
    }
    const HotkeySettings& hotkeys = hotkeysActive_
        ? activeHotkeys_
        : config_.settings().hotkeys;
    if (trayCaptureAction_) {
        trayCaptureAction_->setText(actionWithHotkey(QStringLiteral("截图"), hotkeys.capture));
    }
    if (trayRepeatAction_) {
        trayRepeatAction_->setText(actionWithHotkey(
            QStringLiteral("重复上次截图区域"), hotkeys.repeatCapture));
    }
    if (trayPinAction_) {
        trayPinAction_->setText(actionWithHotkey(QStringLiteral("贴图"), hotkeys.pinClipboard));
    }
    if (trayTogglePinsAction_) {
        trayTogglePinsAction_->setText(actionWithHotkey(
            QStringLiteral("隐藏/显示所有贴图"), hotkeys.togglePins));
    }
    tray_->setToolTip(QStringLiteral("Visnip - %1 截图，%2 贴图")
                          .arg(hotkeys.capture.toString(QKeySequence::NativeText),
                               hotkeys.pinClipboard.toString(QKeySequence::NativeText)));
}

void AppController::handleHotkey(GlobalHotkeyAction action)
{
    Perf::log(QStringLiteral("hotkey.activated action=%1").arg(hotkeyActionName(action)));
    switch (action) {
    case GlobalHotkeyAction::Capture:
        startCapture();
        break;
    case GlobalHotkeyAction::PinClipboard:
        pinClipboard();
        break;
    case GlobalHotkeyAction::RepeatCapture:
        repeatCapture();
        break;
    case GlobalHotkeyAction::TogglePins:
        pins_->toggleAllVisible();
        break;
    case GlobalHotkeyAction::ToggleMouseThrough:
        pins_->toggleActiveMouseThrough();
        break;
    case GlobalHotkeyAction::AskQuestion:
        // Never grab while a capture overlay covers the screen.
        if (questionPanel_ && questionPanel_->isVisible() && !captureWindow_) {
            questionPanel_->requestAnswer();
        }
        break;
    }
}

void AppController::startCapture()
{
    beginCapture(QRect(), true);
}

void AppController::repeatCapture()
{
    if (!lastSelection_.isValid()) {
        Perf::log(QStringLiteral("repeatCapture.no_last_selection"));
        startCapture();
        return;
    }
    beginCapture(lastSelection_, false);
}

void AppController::beginCapture(const QRect& initialSelection, bool notifyOutput)
{
    const QString operation = initialSelection.isValid() ? QStringLiteral("repeatCapture") : QStringLiteral("startCapture");
    Perf::ScopedTimer total(QStringLiteral("AppController.%1").arg(operation));
    QElapsedTimer phase;
    Perf::log(QStringLiteral("%1.overlay_existing present=%2 visible=%3")
                  .arg(operation)
                  .arg(captureWindow_ != nullptr)
                  .arg(captureWindow_ ? captureWindow_->isVisible() : false));
    if (captureWindow_) {
        Perf::log(QStringLiteral("%1.existing_window visible=%2 active=%3")
                      .arg(operation)
                      .arg(captureWindow_->isVisible())
                      .arg(captureWindow_->isActiveWindow()));
        captureWindow_->raise();
        captureWindow_->activateWindow();
        return;
    }
    // Keep the always-on-top question panel out of the frozen desktop and from
    // under the overlay; it returns when the capture ends.
    if (questionPanel_ && questionPanel_->isVisible()) {
        questionPanel_->hide();
        restoreQuestionPanel_ = true;
        QTimer::singleShot(kQuestionPanelHideSettleMs, this, [this, initialSelection, notifyOutput]() {
            beginCapture(initialSelection, notifyOutput);
        });
        return;
    }

    phase.start();
    auto* overlay = new CaptureOverlayWindow(&config_, ocr_, imageTranslation_);
    captureWindow_ = overlay;
    if (config_.settings().aiTranslate.usesLiteOfflineEngine()) {
        // Selecting a region takes longer than loading a cached model; start
        // it after the overlay's first frame. No-op while already running.
        QTimer::singleShot(300, this, [this]() {
            if (config_.settings().aiTranslate.usesLiteOfflineEngine()) {
                LocalTextTranslationService::prewarm(config_.settings().aiTranslate.offlineResourceDirectory);
            }
        });
    }
    Perf::log(QStringLiteral("%1.overlay_created present=%2 visible=%3")
                  .arg(operation)
                  .arg(captureWindow_ != nullptr)
                  .arg(captureWindow_ ? captureWindow_->isVisible() : false));
    Perf::logDuration(QStringLiteral("%1.overlay_construct").arg(operation), phase.elapsed());

    phase.restart();
    connect(overlay, &CaptureOverlayWindow::copyRequested, this, [this, notifyOutput](const QImage& image) {
        const bool copied = output_->copyToClipboard(image);
        if (notifyOutput && copied) {
            showTrayMessage(QStringLiteral("已复制"), QStringLiteral("截图已复制到剪贴板。"));
        }
    });
    connect(overlay, &CaptureOverlayWindow::saveRequested, this, [this, notifyOutput, overlay](const QImage& image) {
        const QString path = output_->saveToDefaultPath(image);
        if (path.isEmpty()) {
            showTrayMessage(QStringLiteral("保存失败"),
                            QStringLiteral("无法写入截图文件，请检查保存目录和磁盘空间。"),
                            QSystemTrayIcon::Warning);
            return;
        }
        if (notifyOutput && config_.settings().output.showSaveNotification) {
            showTrayMessage(QStringLiteral("已保存"), path);
        }
        overlay->close();
    });
    connect(overlay, &CaptureOverlayWindow::pinRequested, this, [this](const QImage& image, const QRect& sourceRect) { pins_->createPin(image, sourceRect.topLeft()); });
    connect(overlay, &CaptureOverlayWindow::questionRequested, this, &AppController::showQuestionPanel);
    connect(overlay, &CaptureOverlayWindow::finished, this, [this](const QRect& selection) { lastSelection_ = selection; });
    connect(overlay, &QObject::destroyed, this, [this, overlay, operation]() {
        Perf::log(QStringLiteral("%1.overlay_destroyed currentPresent=%2").arg(operation).arg(captureWindow_ != nullptr));
        if (captureWindow_.data() == overlay) {
            captureWindow_.clear();
        }
        if (restoreQuestionPanel_ && questionPanel_ && !questionPanel_->isVisible()) {
            questionPanel_->show();
        }
        restoreQuestionPanel_ = false;
    });
    Perf::logDuration(QStringLiteral("%1.connect_signals").arg(operation), phase.elapsed());

    phase.restart();
    overlay->beginCapture(initialSelection);
    Perf::logDuration(QStringLiteral("%1.beginCapture").arg(operation), phase.elapsed());
}

void AppController::pinClipboard()
{
    Perf::ScopedTimer total(QStringLiteral("AppController.pinClipboard"));
    if (!pins_->createFromClipboard()) {
        showTrayMessage(QStringLiteral("没有可贴图内容"), QStringLiteral("剪贴板里没有图片。"), QSystemTrayIcon::Warning);
    }
}

void AppController::showSettings()
{
    presentSettingsDialog(ensureSettingsDialog());
}

void AppController::showQuestionPanel(const QRect& region, const QImage& preview)
{
    if (!questionPanel_) {
        questionPanel_ = new QuestionPanel(&config_);
        connect(questionPanel_, &QuestionPanel::reselectRequested, this, [this]() {
            if (questionPanel_) {
                beginCapture(questionPanel_->region(), false);
            }
        });
        connect(questionPanel_, &QuestionPanel::settingsRequested,
                this, &AppController::showQuestionSettings);
        connect(questionPanel_, &QObject::destroyed, this, [this]() {
            // Closing the panel ends the session and hands F4 back.
            if (hotkeys_) {
                hotkeys_->clearAuxiliaryHotkey(GlobalHotkeyAction::AskQuestion);
            }
            restoreQuestionPanel_ = false;
        });
    }
    restoreQuestionPanel_ = false;
    questionPanel_->setRegion(region, preview);
    questionPanel_->show();
    questionPanel_->raise();
    registerQuestionHotkey();
}

void AppController::registerQuestionHotkey()
{
    if (!questionPanel_ || !hotkeys_) {
        return;
    }
    const QKeySequence sequence = config_.settings().hotkeys.askQuestion;
    QString error;
    if (hotkeys_->setAuxiliaryHotkey(GlobalHotkeyAction::AskQuestion, sequence, &error)) {
        questionPanel_->setAskHotkey(sequence.toString(QKeySequence::NativeText));
    } else {
        questionPanel_->setAskHotkey(QString(), error);
    }
}

void AppController::showQuestionSettings()
{
    SettingsDialog* dialog = ensureSettingsDialog();
    dialog->showPage(SettingsDialog::Page::Question);
    // The panel stays on top of other windows: open the dialog beside it.
    if (questionPanel_ && questionPanel_->isVisible()) {
        const QRect panel = questionPanel_->frameGeometry();
        const QRect available = questionPanel_->screen()->availableGeometry();
        const QSize size = dialog->frameGeometry().size();
        const bool roomOnLeft = panel.left() - available.left() >= available.right() - panel.right();
        int x = roomOnLeft ? panel.left() - size.width() - 8 : panel.right() + 9;
        x = qBound(available.left(), x, qMax(available.left(), available.right() - size.width() + 1));
        const int y = qMax(available.top(), available.top() + (available.height() - size.height()) / 2);
        dialog->move(x, y);
    }
    presentSettingsDialog(dialog);
}

SettingsDialog* AppController::ensureSettingsDialog()
{
    if (!settingsDialog_) {
        Perf::ScopedTimer timer(QStringLiteral("AppController.createSettingsDialog"));
        settingsDialog_ = new SettingsDialog(&config_, ocrPackDownloads_);
        connect(settingsDialog_, &SettingsDialog::hotkeyChangeRequested,
                this, &AppController::applyRequestedHotkeys);
    }
    return settingsDialog_;
}

void AppController::presentSettingsDialog(SettingsDialog* dialog)
{
    Perf::ScopedTimer timer(QStringLiteral("AppController.presentSettingsDialog"));
    dialog->show();
    dialog->raise();
    dialog->activateWindow();
}

void AppController::showTrayMessage(const QString& title, const QString& message, QSystemTrayIcon::MessageIcon icon)
{
    if (tray_ && tray_->isVisible()) {
        tray_->showMessage(title, message, icon, 2500);
    }
}

} // namespace Visnip
