#pragma once

#include <QDateTime>
#include <QElapsedTimer>
#include <QImage>
#include <QList>
#include <QPoint>
#include <QRect>
#include <QSize>
#include <QString>
#include <QWidget>

#include <functional>

class QCloseEvent;
class QLabel;
class QPushButton;
class QShowEvent;
class QSizeGrip;
class QTextBrowser;
class QTimer;
class QToolButton;

namespace Visnip {

class AppConfig;
class QuestionAnswerService;

// Grabs a global logical rect from the screen that holds most of it, at that
// screen's physical resolution. `note` explains a partial or failed grab.
QImage grabScreenRegion(const QRect& globalRect, QString* note = nullptr);

// Always-on-top side panel that remembers one screen region. Each request
// grabs the region as it is at that moment and streams the model's answer.
class QuestionPanel : public QWidget {
    Q_OBJECT
public:
    using RegionGrabber = std::function<QImage(const QRect& globalRect, QString* note)>;

    explicit QuestionPanel(AppConfig* config, QWidget* parent = nullptr);
    ~QuestionPanel() override;

    // A region picked on the capture toolbar; `preview` shows it as it looked
    // when chosen. Moves the panel beside the region when needed.
    void setRegion(const QRect& globalRect, const QImage& preview);
    QRect region() const { return region_; }
    // Grabs the region and asks the model; stops a request in progress.
    void requestAnswer();
    // Hotkey shown on the ask button, or why it could not be registered.
    void setAskHotkey(const QString& text, const QString& problem = {});
    void setRegionGrabber(RegionGrabber grabber);
    QuestionAnswerService* service() const { return service_; }

signals:
    void reselectRequested();
    void settingsRequested();

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;
    void closeEvent(QCloseEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;
    void showEvent(QShowEvent* event) override;

private:
    struct Entry {
        QImage image;
        QSize regionSize;
        QDateTime capturedAt;
        QString answer;
        QString error;
        qint64 elapsedMs = 0;
        bool running = false;
        bool reasoning = false;
        bool truncated = false;
    };

    void buildUi();
    void placeBeside(const QRect& region);
    void captureAndAsk();
    void stop();
    void finishRunning(const QString& answer, const QString& error, bool truncated);
    void showEntry(int index);
    void refreshPreview();
    void refreshStatus();
    void refreshControls();
    void renderAnswer(bool resetScroll);
    // The copy button copies only the final answer when the response has one,
    // otherwise everything; the analysis is copied by selecting it.
    void refreshCopyButton();
    void copyText(const QString& text, const QString& done);
    void showAnswerMenu(const QPoint& position);
    void setCollapsed(bool collapsed);
    bool isRunning() const;

    AppConfig* config_ = nullptr;
    QuestionAnswerService* service_ = nullptr;
    RegionGrabber grabber_;
    QRect region_;
    QImage regionPreview_;
    QList<Entry> entries_;
    int current_ = -1; // -1 shows the region preview without an answer
    int running_ = -1;
    bool capturePending_ = false;
    QElapsedTimer runTimer_;
    QString captureNote_;
    QString idleError_;
    bool idleErrorNeedsSettings_ = false;
    QString askHotkeyText_;
    QString askHotkeyProblem_;
    bool placed_ = false;
    bool userMoved_ = false;
    bool collapsed_ = false;
    int expandedHeight_ = 0;
    bool dragging_ = false;
    QPoint dragOffset_;

    QWidget* header_ = nullptr;
    QWidget* body_ = nullptr;
    QToolButton* collapseButton_ = nullptr;
    QLabel* preview_ = nullptr;
    QLabel* meta_ = nullptr;
    QPushButton* askButton_ = nullptr;
    QLabel* status_ = nullptr;
    QPushButton* statusSettingsButton_ = nullptr;
    QTextBrowser* answer_ = nullptr;
    QToolButton* previousButton_ = nullptr;
    QToolButton* nextButton_ = nullptr;
    QLabel* position_ = nullptr;
    QPushButton* copyButton_ = nullptr;
    QSizeGrip* leftGrip_ = nullptr;
    QSizeGrip* rightGrip_ = nullptr;
    QTimer* renderTimer_ = nullptr;
    QTimer* tickTimer_ = nullptr;
    QTimer* copiedTimer_ = nullptr;
};

} // namespace Visnip
