#pragma once

#include "core/AppConfig.h"

#include <QDialog>
#include <QList>
#include <QListWidget>
#include <QPointer>
#include <QStackedWidget>

class QCloseEvent;
class QShowEvent;
class QLabel;
class QKeySequenceEdit;
class QTimer;

namespace Visnip {

class OcrPackDownloadService;
class OfflineResourceService;
class UpdateService;

class SettingsDialog : public QDialog {
    Q_OBJECT
public:
    enum class Page {
        General,
        Capture,
        Pin,
        Output,
        Translation,
        Question,
        Hotkey,
        About,
    };

    explicit SettingsDialog(
        AppConfig* config,
        OcrPackDownloadService* ocrPackDownloads = nullptr,
        QWidget* parent = nullptr);
    ~SettingsDialog() override;

    void showPage(Page page);
    void setHotkeyChangeResult(const HotkeySettings& effective,
                               const QString& errorMessage = {});

signals:
    void hotkeyChangeRequested(const HotkeySettings& candidate);

protected:
    void closeEvent(QCloseEvent* event) override;
    void showEvent(QShowEvent* event) override;

private:
    QWidget* createGeneralPage();
    QWidget* createCapturePage();
    QWidget* createPinPage();
    QWidget* createOutputPage();
    QWidget* createTranslationPage();
    QWidget* createQuestionPage();
    QWidget* createHotkeyPage();
    QWidget* createAboutPage();
    QWidget* createPage(Page pageId);
    void addPage(Page pageId, const QString& title);
    void ensurePage(Page pageId);
    void resetPageCache();
    void refreshUpdateBadge();
    void scheduleSave(bool syncAutoStart = false);
    bool flushPendingSave();

    AppConfig* config_ = nullptr;
    OcrPackDownloadService* ocrPackDownloads_ = nullptr;
    OfflineResourceService* offlineResources_ = nullptr;
    UpdateService* updates_ = nullptr;
    qint64 lastUpdateCheckMs_ = 0;
    QListWidget* nav_ = nullptr;
    QStackedWidget* stack_ = nullptr;
    QTimer* saveTimer_ = nullptr;
    QLabel* saveStatus_ = nullptr;
    QLabel* hotkeyStatus_ = nullptr;
    QList<QKeySequenceEdit*> hotkeyEdits_;
    bool savePending_ = false;
    bool autoStartSyncPending_ = false;
};

} // namespace Visnip
