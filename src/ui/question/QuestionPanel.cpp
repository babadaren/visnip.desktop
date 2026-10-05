#include "ui/question/QuestionPanel.h"

#include "core/AppConfig.h"
#include "core/PerfLog.h"
#include "core/QuestionAnswer.h"
#include "services/QuestionAnswerService.h"
#include "ui/IconUtils.h"

#include <QApplication>
#include <QClipboard>
#include <QCloseEvent>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QLabel>
#include <QMenu>
#include <QMouseEvent>
#include <QPixmap>
#include <QPushButton>
#include <QScreen>
#include <QScrollBar>
#include <QSizeGrip>
#include <QStyle>
#include <QTextBrowser>
#include <QTimer>
#include <QToolButton>
#include <QVBoxLayout>
#include <QWindow>

#include <cmath>

#ifdef Q_OS_WIN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <dwmapi.h>
#endif

namespace Visnip {

namespace {

constexpr int kMaxHistory = 10;
constexpr int kDefaultWidth = 400;
constexpr int kMinWidth = 320;
constexpr int kMaxHeight = 820;
constexpr int kScreenMargin = 12;
constexpr int kPreviewMaxHeight = 200;
constexpr int kRenderIntervalMs = 120;
// Time for the compositor to drop the panel from the screen before a grab.
constexpr int kSelfHideDelayMs = 90;

qint64 overlapArea(const QRect& a, const QRect& b)
{
    const QRect overlap = a.intersected(b);
    return overlap.isEmpty() ? 0 : qint64(overlap.width()) * overlap.height();
}

QScreen* screenWithMostOf(const QRect& rect)
{
    QScreen* best = nullptr;
    qint64 bestArea = 0;
    for (QScreen* screen : QGuiApplication::screens()) {
        const qint64 area = overlapArea(screen->geometry(), rect);
        if (area > bestArea) {
            best = screen;
            bestArea = area;
        }
    }
    return best;
}

QString elapsedText(qint64 ms)
{
    return QStringLiteral("%1 秒").arg(qMax<qint64>(0, (ms + 500) / 1000));
}

QToolButton* headerButton(const QString& iconPath, const QString& tooltip, QWidget* parent)
{
    auto* button = new QToolButton(parent);
    button->setProperty("panelHeaderButton", true);
    button->setIcon(Ui::themedIcon(iconPath, QColor(QStringLiteral("#475467")), QSize(18, 18)));
    button->setIconSize(QSize(18, 18));
    button->setFixedSize(28, 28);
    button->setToolTip(tooltip);
    button->setCursor(Qt::PointingHandCursor);
    button->setFocusPolicy(Qt::NoFocus);
    return button;
}

void repolish(QWidget* widget)
{
    widget->style()->unpolish(widget);
    widget->style()->polish(widget);
    widget->update();
}

QString panelStyleSheet()
{
    return QStringLiteral(R"(
        QWidget#VisnipQuestionPanel {
            background: #FFFFFF;
            border: 1px solid #C9D4E5;
        }
        QWidget#QuestionPanelHeader {
            background: #F8FAFD;
            border-bottom: 1px solid #DCE5F2;
        }
        QLabel#QuestionPanelTitle {
            font-size: 13px;
            font-weight: 600;
            color: #172033;
            background: transparent;
        }
        QToolButton[panelHeaderButton="true"] {
            border: none;
            border-radius: 6px;
            background: transparent;
        }
        QToolButton[panelHeaderButton="true"]:hover { background: #E9EDF4; }
        QLabel#QuestionPanelPreview {
            background: #F1F4F9;
            border: 1px solid #DCE5F2;
            border-radius: 6px;
            color: #98A2B3;
        }
        QLabel#QuestionPanelMeta, QLabel#QuestionPanelPosition { color: #667085; }
        QPushButton#QuestionPanelAsk {
            min-height: 34px;
            font-size: 13px;
            font-weight: 600;
            color: #FFFFFF;
            background: #3567E8;
            border: 1px solid #3567E8;
            border-radius: 6px;
        }
        QPushButton#QuestionPanelAsk:hover { background: #2858D4; border-color: #2858D4; }
        QPushButton#QuestionPanelAsk:disabled { background: #A9BDF3; border-color: #A9BDF3; }
        QPushButton#QuestionPanelAsk[busy="true"] {
            color: #B42318;
            background: #FFFFFF;
            border: 1px solid #F5B5AF;
        }
        QPushButton#QuestionPanelAsk[busy="true"]:hover { background: #FEF3F2; }
        QLabel#QuestionPanelStatus { color: #667085; }
        QLabel#QuestionPanelStatus[error="true"] { color: #B42318; }
        QTextBrowser#QuestionPanelAnswer {
            border: 1px solid #DCE5F2;
            border-radius: 6px;
            background: #FFFFFF;
            padding: 4px;
            font-size: 13px;
        }
        QToolButton#QuestionPanelHistoryButton {
            border: 1px solid #DCE5F2;
            border-radius: 6px;
            background: #FFFFFF;
            min-width: 26px;
            min-height: 24px;
            font-size: 14px;
        }
        QToolButton#QuestionPanelHistoryButton:hover { background: #F5F8FD; }
        QToolButton#QuestionPanelHistoryButton:disabled { color: #C0C8D4; }
    )");
}

} // namespace

QImage grabScreenRegion(const QRect& globalRect, QString* note)
{
    QScreen* screen = screenWithMostOf(globalRect);
    if (!screen) {
        if (note) {
            *note = QStringLiteral("选区已不在任何屏幕上，请重新选定。");
        }
        return {};
    }
    const QRect screenGeometry = screen->geometry();
    const QRect visible = globalRect.intersected(screenGeometry);
    if (visible != globalRect && note) {
        *note = QStringLiteral("选区跨越了多块屏幕，只截取了其中一块屏幕上的部分。");
    }

    // Grab the whole screen and crop in physical pixels, the same way the
    // capture overlay's Qt fallback does; this stays sharp on scaled screens.
    const QPixmap pixmap = screen->grabWindow(0);
    if (pixmap.isNull()) {
        if (note) {
            *note = QStringLiteral("截取屏幕失败，请重试。");
        }
        return {};
    }
    const qreal scaleX = qreal(pixmap.width()) / qMax(1, screenGeometry.width());
    const qreal scaleY = qreal(pixmap.height()) / qMax(1, screenGeometry.height());
    const QRect relative = visible.translated(-screenGeometry.topLeft());
    const QRect source = QRect(int(std::floor(relative.x() * scaleX)),
                               int(std::floor(relative.y() * scaleY)),
                               int(std::ceil(relative.width() * scaleX)),
                               int(std::ceil(relative.height() * scaleY)))
                             .intersected(QRect(QPoint(0, 0), pixmap.size()));
    QImage image = pixmap.copy(source).toImage();
    image.setDevicePixelRatio(1.0);
    return image;
}

QuestionPanel::QuestionPanel(AppConfig* config, QWidget* parent)
    : QWidget(parent)
    , config_(config)
    , service_(new QuestionAnswerService(config, this))
    , renderTimer_(new QTimer(this))
    , tickTimer_(new QTimer(this))
{
    Q_ASSERT(config_);
    setObjectName(QStringLiteral("VisnipQuestionPanel"));
    setWindowTitle(QStringLiteral("Visnip 解题"));
    setWindowIcon(Ui::applicationIcon());
    setWindowFlags(Qt::Tool | Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint);
    setAttribute(Qt::WA_ShowWithoutActivating, true);
    setAttribute(Qt::WA_StyledBackground, true);
    setAttribute(Qt::WA_DeleteOnClose, true);
    setMinimumSize(kMinWidth, 44);
    resize(kDefaultWidth, 640);
    setStyleSheet(panelStyleSheet());

    renderTimer_->setSingleShot(true);
    renderTimer_->setInterval(kRenderIntervalMs);
    connect(renderTimer_, &QTimer::timeout, this, [this]() { renderAnswer(false); });
    tickTimer_->setInterval(1000);
    connect(tickTimer_, &QTimer::timeout, this, &QuestionPanel::refreshStatus);

    buildUi();

    connect(service_, &QuestionAnswerService::progress, this,
            [this](const QString& answer, bool reasoning) {
        if (running_ < 0) {
            return;
        }
        Entry& entry = entries_[running_];
        entry.answer = answer;
        entry.reasoning = reasoning;
        if (current_ == running_ && !renderTimer_->isActive()) {
            renderTimer_->start();
        }
        refreshStatus();
    });
    connect(service_, &QuestionAnswerService::finished, this,
            [this](const QString& answer, bool truncated) {
        finishRunning(answer, QString(), truncated);
    });
    connect(service_, &QuestionAnswerService::failed, this, [this](const QString& message) {
        if (running_ < 0) {
            idleError_ = message;
            refreshStatus();
            return;
        }
        finishRunning(QString(), message, false);
    });

    showEntry(-1);
}

QuestionPanel::~QuestionPanel()
{
    service_->cancel();
}

void QuestionPanel::buildUi()
{
    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(1, 1, 1, 1);
    root->setSpacing(0);

    header_ = new QWidget(this);
    header_->setObjectName(QStringLiteral("QuestionPanelHeader"));
    header_->setAttribute(Qt::WA_StyledBackground, true);
    header_->setFixedHeight(40);
    header_->setCursor(Qt::SizeAllCursor);
    header_->installEventFilter(this);
    auto* headerLayout = new QHBoxLayout(header_);
    headerLayout->setContentsMargins(12, 0, 6, 0);
    headerLayout->setSpacing(2);
    auto* title = new QLabel(QStringLiteral("解题"), header_);
    title->setObjectName(QStringLiteral("QuestionPanelTitle"));
    headerLayout->addWidget(title);
    headerLayout->addStretch(1);
    auto* settingsButton = headerButton(QStringLiteral(":/visnip/icons/nav-general.svg"),
                                        QStringLiteral("解题设置"), header_);
    settingsButton->setObjectName(QStringLiteral("QuestionPanelSettingsButton"));
    collapseButton_ = headerButton(QStringLiteral(":/visnip/icons/combo-arrow-up.svg"),
                                   QStringLiteral("收起"), header_);
    collapseButton_->setObjectName(QStringLiteral("QuestionPanelCollapseButton"));
    auto* closeButton = headerButton(QStringLiteral(":/visnip/icons/action-close.svg"),
                                     QStringLiteral("关闭"), header_);
    closeButton->setObjectName(QStringLiteral("QuestionPanelCloseButton"));
    headerLayout->addWidget(settingsButton);
    headerLayout->addWidget(collapseButton_);
    headerLayout->addWidget(closeButton);
    root->addWidget(header_);

    body_ = new QWidget(this);
    body_->setObjectName(QStringLiteral("QuestionPanelBody"));
    auto* bodyLayout = new QVBoxLayout(body_);
    bodyLayout->setContentsMargins(12, 12, 12, 8);
    bodyLayout->setSpacing(8);

    preview_ = new QLabel(body_);
    preview_->setObjectName(QStringLiteral("QuestionPanelPreview"));
    preview_->setAlignment(Qt::AlignCenter);
    preview_->setMinimumHeight(72);
    preview_->setMaximumHeight(kPreviewMaxHeight);
    preview_->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    bodyLayout->addWidget(preview_);

    auto* metaRow = new QHBoxLayout;
    metaRow->setSpacing(8);
    meta_ = new QLabel(body_);
    meta_->setObjectName(QStringLiteral("QuestionPanelMeta"));
    auto* reselectButton = new QPushButton(QStringLiteral("重新选定"), body_);
    reselectButton->setObjectName(QStringLiteral("QuestionPanelReselectButton"));
    reselectButton->setCursor(Qt::PointingHandCursor);
    reselectButton->setAutoDefault(false);
    metaRow->addWidget(meta_, 1);
    metaRow->addWidget(reselectButton);
    bodyLayout->addLayout(metaRow);

    askButton_ = new QPushButton(body_);
    askButton_->setObjectName(QStringLiteral("QuestionPanelAsk"));
    askButton_->setCursor(Qt::PointingHandCursor);
    askButton_->setAutoDefault(false);
    bodyLayout->addWidget(askButton_);

    auto* statusRow = new QHBoxLayout;
    statusRow->setSpacing(8);
    status_ = new QLabel(body_);
    status_->setObjectName(QStringLiteral("QuestionPanelStatus"));
    status_->setWordWrap(true);
    status_->setTextFormat(Qt::PlainText);
    status_->setTextInteractionFlags(Qt::TextSelectableByMouse);
    statusSettingsButton_ = new QPushButton(QStringLiteral("打开设置"), body_);
    statusSettingsButton_->setObjectName(QStringLiteral("QuestionPanelStatusSettings"));
    statusSettingsButton_->setCursor(Qt::PointingHandCursor);
    statusSettingsButton_->setAutoDefault(false);
    statusRow->addWidget(status_, 1);
    statusRow->addWidget(statusSettingsButton_, 0, Qt::AlignTop);
    bodyLayout->addLayout(statusRow);

    answer_ = new QTextBrowser(body_);
    answer_->setObjectName(QStringLiteral("QuestionPanelAnswer"));
    answer_->setOpenLinks(false);
    answer_->setOpenExternalLinks(false);
    answer_->setPlaceholderText(QStringLiteral("答案会显示在这里。可以选中任意文字复制。"));
    // Qt's own menu would be English (no Qt translations are shipped).
    answer_->setContextMenuPolicy(Qt::CustomContextMenu);
    bodyLayout->addWidget(answer_, 1);

    auto* footer = new QHBoxLayout;
    footer->setSpacing(6);
    previousButton_ = new QToolButton(body_);
    previousButton_->setObjectName(QStringLiteral("QuestionPanelHistoryButton"));
    previousButton_->setText(QStringLiteral("‹"));
    previousButton_->setToolTip(QStringLiteral("上一题"));
    nextButton_ = new QToolButton(body_);
    nextButton_->setObjectName(QStringLiteral("QuestionPanelHistoryButton"));
    nextButton_->setText(QStringLiteral("›"));
    nextButton_->setToolTip(QStringLiteral("下一题"));
    position_ = new QLabel(body_);
    position_->setObjectName(QStringLiteral("QuestionPanelPosition"));
    copyButton_ = new QPushButton(QStringLiteral("复制答案"), body_);
    copyButton_->setObjectName(QStringLiteral("QuestionPanelCopyButton"));
    copyButton_->setCursor(Qt::PointingHandCursor);
    copyButton_->setAutoDefault(false);
    copiedTimer_ = new QTimer(this);
    copiedTimer_->setSingleShot(true);
    copiedTimer_->setInterval(1500);
    connect(copiedTimer_, &QTimer::timeout, this, &QuestionPanel::refreshCopyButton);
    footer->addWidget(previousButton_);
    footer->addWidget(position_);
    footer->addWidget(nextButton_);
    footer->addStretch(1);
    footer->addWidget(copyButton_);
    bodyLayout->addLayout(footer);
    root->addWidget(body_, 1);

    // Both bottom corners resize: the panel may sit at either screen edge.
    leftGrip_ = new QSizeGrip(this);
    rightGrip_ = new QSizeGrip(this);
    leftGrip_->setFixedSize(12, 12);
    rightGrip_->setFixedSize(12, 12);

    connect(settingsButton, &QToolButton::clicked, this, &QuestionPanel::settingsRequested);
    connect(statusSettingsButton_, &QPushButton::clicked, this, &QuestionPanel::settingsRequested);
    connect(closeButton, &QToolButton::clicked, this, &QWidget::close);
    connect(collapseButton_, &QToolButton::clicked, this, [this]() { setCollapsed(!collapsed_); });
    connect(reselectButton, &QPushButton::clicked, this, &QuestionPanel::reselectRequested);
    connect(askButton_, &QPushButton::clicked, this, &QuestionPanel::requestAnswer);
    connect(previousButton_, &QToolButton::clicked, this, [this]() {
        showEntry(current_ < 0 ? int(entries_.size()) - 1 : current_ - 1);
    });
    connect(nextButton_, &QToolButton::clicked, this, [this]() { showEntry(current_ + 1); });
    connect(copyButton_, &QPushButton::clicked, this, [this]() {
        if (current_ < 0 || entries_[current_].answer.isEmpty()) return;
        const QString answer = Question::finalAnswer(entries_[current_].answer);
        if (!answer.isEmpty()) copyText(answer, QStringLiteral("已复制答案"));
        else copyText(answer_->toPlainText().trimmed(), QStringLiteral("已复制全部"));
    });
    connect(answer_, &QWidget::customContextMenuRequested, this, &QuestionPanel::showAnswerMenu);
}

void QuestionPanel::refreshCopyButton()
{
    if (copiedTimer_->isActive()) return; // keep "已复制" for a moment
    const bool has = current_ >= 0 && !entries_[current_].answer.isEmpty();
    const bool answerOnly = has && !Question::finalAnswer(entries_[current_].answer).isEmpty();
    copyButton_->setEnabled(has);
    copyButton_->setText(answerOnly || !has ? QStringLiteral("复制答案") : QStringLiteral("复制全部"));
    copyButton_->setToolTip(answerOnly
        ? QStringLiteral("只复制最后的“答案”部分。需要解析时，在上方选中文字后按 Ctrl+C 或右键复制。")
        : QStringLiteral("回答里没有单独的“答案：”一行，将复制全部内容；也可以在上方选中需要的文字复制。"));
}

void QuestionPanel::copyText(const QString& text, const QString& done)
{
    if (text.isEmpty()) return;
    QGuiApplication::clipboard()->setText(text);
    copyButton_->setText(done);
    copiedTimer_->start();
}

void QuestionPanel::showAnswerMenu(const QPoint& position)
{
    const bool has = current_ >= 0 && !entries_[current_].answer.isEmpty();
    const QString answer = has ? Question::finalAnswer(entries_[current_].answer) : QString();
    QMenu menu(answer_);
    QAction* selection = menu.addAction(QStringLiteral("复制所选内容"));
    selection->setShortcut(QKeySequence::Copy);
    selection->setEnabled(answer_->textCursor().hasSelection());
    connect(selection, &QAction::triggered, answer_, &QTextBrowser::copy);
    QAction* answerAction = menu.addAction(QStringLiteral("复制答案"));
    answerAction->setEnabled(!answer.isEmpty());
    connect(answerAction, &QAction::triggered, this, [this, answer]() { copyText(answer, QStringLiteral("已复制答案")); });
    QAction* everything = menu.addAction(QStringLiteral("复制全部"));
    everything->setEnabled(has);
    connect(everything, &QAction::triggered, this, [this]() {
        copyText(answer_->toPlainText().trimmed(), QStringLiteral("已复制全部"));
    });
    menu.addSeparator();
    QAction* all = menu.addAction(QStringLiteral("全选"));
    all->setShortcut(QKeySequence::SelectAll);
    all->setEnabled(has);
    connect(all, &QAction::triggered, answer_, &QTextBrowser::selectAll);
    menu.exec(answer_->viewport()->mapToGlobal(position));
}

void QuestionPanel::setRegion(const QRect& globalRect, const QImage& preview)
{
    region_ = globalRect;
    regionPreview_ = preview;
    captureNote_.clear();
    idleError_.clear();
    if (!placed_ || !userMoved_ || frameGeometry().intersects(globalRect)) {
        placeBeside(globalRect);
    }
    showEntry(-1);
}

void QuestionPanel::setAskHotkey(const QString& text, const QString& problem)
{
    askHotkeyText_ = text;
    askHotkeyProblem_ = problem;
    refreshControls();
    refreshStatus();
}

void QuestionPanel::setRegionGrabber(RegionGrabber grabber)
{
    grabber_ = std::move(grabber);
}

void QuestionPanel::requestAnswer()
{
    if (isRunning() || capturePending_) {
        stop();
        return;
    }
    if (!region_.isValid()) {
        return;
    }
    const QString problem = QuestionAnswerService::configurationProblem(config_->settings().question);
    if (!problem.isEmpty()) {
        idleError_ = problem;
        showEntry(-1);
        return;
    }
    if (collapsed_) {
        setCollapsed(false);
    }
    // Keep the panel out of its own screenshot when it was moved over the region.
    if (isVisible() && frameGeometry().intersects(region_)) {
        capturePending_ = true;
        refreshControls();
        setWindowOpacity(0.0);
        QTimer::singleShot(kSelfHideDelayMs, this, [this]() {
            if (capturePending_) {
                captureAndAsk();
            }
            setWindowOpacity(1.0);
        });
        return;
    }
    captureAndAsk();
}

void QuestionPanel::captureAndAsk()
{
    capturePending_ = false;
    QString note;
    const QImage image = grabber_ ? grabber_(region_, &note) : grabScreenRegion(region_, &note);
    captureNote_ = note;
    if (image.isNull()) {
        idleError_ = note.isEmpty() ? QStringLiteral("截取选区失败，请重新选定。") : note;
        showEntry(-1);
        return;
    }

    Entry entry;
    entry.image = image;
    entry.regionSize = region_.size();
    entry.capturedAt = QDateTime::currentDateTime();
    entry.running = true;
    entries_.append(entry);
    while (entries_.size() > kMaxHistory) {
        entries_.removeFirst();
    }
    running_ = int(entries_.size()) - 1;
    idleError_.clear();
    runTimer_.start();
    tickTimer_->start();
    Perf::log(QStringLiteral("QuestionPanel.ask region=%1x%2 image=%3x%4")
                  .arg(region_.width())
                  .arg(region_.height())
                  .arg(image.width())
                  .arg(image.height()));
    showEntry(running_);
    service_->ask(image);
}

void QuestionPanel::stop()
{
    capturePending_ = false;
    service_->cancel();
    if (running_ >= 0) {
        finishRunning(QString(), QStringLiteral("已停止。"), false);
    } else {
        refreshControls();
    }
}

void QuestionPanel::finishRunning(const QString& answer, const QString& error, bool truncated)
{
    if (running_ < 0) {
        return;
    }
    Entry& entry = entries_[running_];
    if (!answer.isEmpty()) {
        entry.answer = answer;
    }
    entry.error = error;
    entry.truncated = truncated;
    entry.running = false;
    entry.reasoning = false;
    entry.elapsedMs = runTimer_.elapsed();
    const bool shown = current_ == running_;
    running_ = -1;
    tickTimer_->stop();
    renderTimer_->stop();
    if (shown) {
        renderAnswer(false);
    }
    refreshStatus();
    refreshControls();
}

void QuestionPanel::showEntry(int index)
{
    current_ = index >= 0 && index < entries_.size() ? index : -1;
    refreshPreview();
    renderAnswer(true);
    refreshStatus();
    refreshControls();
}

void QuestionPanel::refreshPreview()
{
    const bool showingEntry = current_ >= 0;
    const QImage image = showingEntry ? entries_[current_].image : regionPreview_;
    if (showingEntry) {
        const Entry& entry = entries_[current_];
        meta_->setText(QStringLiteral("%1×%2 · %3 截取")
                           .arg(entry.regionSize.width())
                           .arg(entry.regionSize.height())
                           .arg(entry.capturedAt.toString(QStringLiteral("HH:mm:ss"))));
    } else if (region_.isValid()) {
        meta_->setText(QStringLiteral("%1×%2 · 选定区域").arg(region_.width()).arg(region_.height()));
    } else {
        meta_->setText(QStringLiteral("尚未选定区域"));
    }

    if (image.isNull()) {
        preview_->setPixmap(QPixmap());
        preview_->setText(QStringLiteral("选定区域的截图会显示在这里"));
        return;
    }
    const qreal ratio = devicePixelRatioF();
    const QSize box = QSize(qMax(1, preview_->width() - 2), kPreviewMaxHeight - 2);
    QImage scaled = image.scaled(box * ratio, Qt::KeepAspectRatio, Qt::SmoothTransformation);
    if (scaled.width() > image.width() || scaled.height() > image.height()) {
        scaled = image; // never enlarge small captures
    }
    QPixmap pixmap = QPixmap::fromImage(scaled);
    pixmap.setDevicePixelRatio(ratio);
    preview_->setText(QString());
    preview_->setPixmap(pixmap);
    preview_->setFixedHeight(qBound(72, int(std::ceil(pixmap.height() / ratio)) + 2, kPreviewMaxHeight));
}

void QuestionPanel::refreshStatus()
{
    QString text;
    bool error = false;
    bool needsSettings = false;
    if (current_ >= 0) {
        const Entry& entry = entries_[current_];
        if (entry.running) {
            const QString elapsed = elapsedText(runTimer_.elapsed());
            if (!entry.answer.isEmpty()) {
                text = QStringLiteral("正在作答… %1").arg(elapsed);
            } else if (entry.reasoning) {
                text = QStringLiteral("思考中… %1").arg(elapsed);
            } else {
                text = QStringLiteral("正在发送截图，等待模型回复… %1").arg(elapsed);
            }
        } else if (!entry.error.isEmpty()) {
            text = entry.error;
            error = true;
        } else {
            text = QStringLiteral("已完成 · 用时 %1").arg(elapsedText(entry.elapsedMs));
            if (entry.truncated) {
                text += QStringLiteral("（答案过长，已被截断）");
            }
        }
    } else if (!idleError_.isEmpty()) {
        text = idleError_;
        error = true;
        needsSettings = !QuestionAnswerService::configurationProblem(
                             config_->settings().question).isEmpty();
    } else {
        const QString problem = QuestionAnswerService::configurationProblem(config_->settings().question);
        if (!problem.isEmpty()) {
            text = problem;
            needsSettings = true;
        } else {
            text = askHotkeyText_.isEmpty()
                ? QStringLiteral("点击「获取答案」截取选区当前的画面并解题。")
                : QStringLiteral("点击「获取答案」或按 %1，截取选区当前的画面并解题。").arg(askHotkeyText_);
        }
    }
    if (!captureNote_.isEmpty() && (current_ < 0 || current_ == int(entries_.size()) - 1)) {
        text += QLatin1Char('\n') + captureNote_;
    }
    if (!askHotkeyProblem_.isEmpty() && !error && (current_ < 0 || !entries_[current_].running)) {
        text += QLatin1Char('\n') + askHotkeyProblem_;
    }
    status_->setText(text);
    if (status_->property("error").toBool() != error) {
        status_->setProperty("error", error);
        repolish(status_);
    }
    statusSettingsButton_->setVisible(needsSettings);
}

void QuestionPanel::refreshControls()
{
    const bool busy = isRunning() || capturePending_;
    askButton_->setText(busy ? QStringLiteral("停止")
                             : askHotkeyText_.isEmpty()
                                 ? QStringLiteral("获取答案")
                                 : QStringLiteral("获取答案  %1").arg(askHotkeyText_));
    askButton_->setEnabled(busy || region_.isValid());
    if (askButton_->property("busy").toBool() != busy) {
        askButton_->setProperty("busy", busy);
        repolish(askButton_);
    }
    const int count = int(entries_.size());
    previousButton_->setEnabled(count > 0 && current_ != 0);
    nextButton_->setEnabled(current_ >= 0 && current_ < count - 1);
    position_->setText(current_ >= 0 ? QStringLiteral("%1/%2").arg(current_ + 1).arg(count)
                                     : count > 0 ? QStringLiteral("新选区 · 共 %1 题").arg(count)
                                                 : QString());
    refreshCopyButton();
}

void QuestionPanel::renderAnswer(bool resetScroll)
{
    QScrollBar* bar = answer_->verticalScrollBar();
    const bool followTail = !resetScroll && bar->value() >= bar->maximum() - 4;
    const int previous = bar->value();
    const QString markdown = current_ >= 0 ? Question::unicodeMath(entries_[current_].answer) : QString();
    answer_->setMarkdown(markdown);
    if (resetScroll) {
        bar->setValue(0);
    } else if (followTail) {
        bar->setValue(bar->maximum());
    } else {
        bar->setValue(previous);
    }
    refreshCopyButton();
}

void QuestionPanel::setCollapsed(bool collapsed)
{
    if (collapsed_ == collapsed) {
        return;
    }
    collapsed_ = collapsed;
    if (collapsed) {
        expandedHeight_ = height();
    }
    body_->setVisible(!collapsed);
    leftGrip_->setVisible(!collapsed);
    rightGrip_->setVisible(!collapsed);
    collapseButton_->setIcon(Ui::themedIcon(
        collapsed ? QStringLiteral(":/visnip/icons/combo-arrow.svg")
                  : QStringLiteral(":/visnip/icons/combo-arrow-up.svg"),
        QColor(QStringLiteral("#475467")), QSize(18, 18)));
    collapseButton_->setToolTip(collapsed ? QStringLiteral("展开") : QStringLiteral("收起"));
    if (collapsed) {
        setFixedHeight(header_->height() + 2);
    } else {
        setMinimumHeight(44);
        setMaximumHeight(QWIDGETSIZE_MAX);
        resize(width(), qMax(expandedHeight_, 360));
    }
}

bool QuestionPanel::isRunning() const
{
    return running_ >= 0;
}

void QuestionPanel::placeBeside(const QRect& region)
{
    QScreen* screen = screenWithMostOf(region);
    if (!screen) {
        screen = QGuiApplication::primaryScreen();
    }
    if (!screen) {
        return;
    }
    const QRect available = screen->availableGeometry();
    const int panelWidth = qBound(kMinWidth, placed_ ? width() : kDefaultWidth,
                                  qMax(kMinWidth, available.width() / 2));
    const int panelHeight = collapsed_ ? height()
                                       : qMin(available.height() - 2 * kScreenMargin, kMaxHeight);
    const int top = available.top() + (available.height() - panelHeight) / 2;
    const QRect right(available.right() - kScreenMargin - panelWidth + 1, top, panelWidth, panelHeight);
    const QRect left(available.left() + kScreenMargin, top, panelWidth, panelHeight);
    QRect target = right;
    if (right.intersects(region)) {
        target = overlapArea(left, region) < overlapArea(right, region) ? left : right;
    }
    setGeometry(target);
    placed_ = true;
    userMoved_ = false;
}

bool QuestionPanel::eventFilter(QObject* watched, QEvent* event)
{
    if (watched == header_) {
        switch (event->type()) {
        case QEvent::MouseButtonPress: {
            auto* mouse = static_cast<QMouseEvent*>(event);
            if (mouse->button() != Qt::LeftButton) {
                break;
            }
            userMoved_ = true;
            // A native move keeps snapping and multi-monitor DPI handling.
            if (windowHandle() && windowHandle()->startSystemMove()) {
                return true;
            }
            dragging_ = true;
            dragOffset_ = mouse->globalPosition().toPoint() - frameGeometry().topLeft();
            return true;
        }
        case QEvent::MouseMove:
            if (dragging_) {
                move(static_cast<QMouseEvent*>(event)->globalPosition().toPoint() - dragOffset_);
                return true;
            }
            break;
        case QEvent::MouseButtonRelease:
            dragging_ = false;
            break;
        case QEvent::MouseButtonDblClick:
            setCollapsed(!collapsed_);
            return true;
        default:
            break;
        }
    }
    return QWidget::eventFilter(watched, event);
}

void QuestionPanel::closeEvent(QCloseEvent* event)
{
    service_->cancel();
    QWidget::closeEvent(event);
}

void QuestionPanel::resizeEvent(QResizeEvent* event)
{
    QWidget::resizeEvent(event);
    if (!leftGrip_) {
        return;
    }
    leftGrip_->move(1, height() - leftGrip_->height() - 1);
    rightGrip_->move(width() - rightGrip_->width() - 1, height() - rightGrip_->height() - 1);
    leftGrip_->raise();
    rightGrip_->raise();
    refreshPreview();
}

void QuestionPanel::showEvent(QShowEvent* event)
{
    QWidget::showEvent(event);
#ifdef Q_OS_WIN
    // Rounded corners on Windows 11; older systems ignore the attribute.
    constexpr DWORD kWindowCornerPreference = 33; // DWMWA_WINDOW_CORNER_PREFERENCE
    const int round = 2;                          // DWMWCP_ROUND
    DwmSetWindowAttribute(reinterpret_cast<HWND>(winId()), kWindowCornerPreference,
                          &round, sizeof(round));
#endif
    refreshPreview();
}

} // namespace Visnip
