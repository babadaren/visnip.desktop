#include "ui/settings/SettingsDialog.h"

#include "core/DesignTokens.h"
#include "core/OcrLanguagePack.h"
#include "core/PerfLog.h"
#include "core/QuestionAnswer.h"
#include "core/TranslationLanguage.h"
#include "platform/HotkeyManager.h"
#include "services/ImageOutputService.h"
#include "services/LocalTextTranslationService.h"
#include "services/OcrPackDownloadService.h"
#include "services/OfflineTranslationService.h"
#include "services/OfflineResourceService.h"
#include "services/QuestionAnswerService.h"
#include "services/ImageTranslationService.h"
#include "services/TextTranslationService.h"
#include "ui/IconUtils.h"

#include <QAbstractButton>
#include <QAbstractScrollArea>
#include <QApplication>
#include <QButtonGroup>
#include <QCloseEvent>
#include <QComboBox>
#include <QDesktopServices>
#include <QUrl>
#include <QFileDialog>
#include <QFrame>
#include <QHBoxLayout>
#include <QKeySequenceEdit>
#include <QLabel>
#include <QLineEdit>
#include <QListView>
#include <QMessageBox>
#include <QPainter>
#include <QPalette>
#include <QPlainTextEdit>
#include <QProgressBar>
#include <QPushButton>
#include <QScrollArea>
#include <QSignalBlocker>
#include <QSpinBox>
#include <QStyle>
#include <QTimer>
#include <QToolButton>
#include <QVBoxLayout>

namespace Visnip {

namespace {

constexpr int kSettingsSaveDelayMs = 180;

class SettingsSwitch final : public QAbstractButton {
public:
    explicit SettingsSwitch(bool checked, QWidget* parent = nullptr)
        : QAbstractButton(parent)
    {
        setCheckable(true);
        setChecked(checked);
        setCursor(Qt::PointingHandCursor);
        setFixedSize(42, 24);
        setAttribute(Qt::WA_Hover, true);
    }

    QSize sizeHint() const override { return QSize(42, 24); }

protected:
    void paintEvent(QPaintEvent*) override
    {
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing, true);

        QColor track = isChecked() ? QColor(QStringLiteral("#3567E8"))
                                   : QColor(QStringLiteral("#C8D0DC"));
        if (underMouse()) {
            track = isChecked() ? QColor(QStringLiteral("#2858D4"))
                                : QColor(QStringLiteral("#B6C0CF"));
        }
        if (!isEnabled()) {
            track.setAlpha(120);
        }

        painter.setPen(Qt::NoPen);
        painter.setBrush(track);
        painter.drawRoundedRect(rect().adjusted(1, 2, -1, -2), 10, 10);

        constexpr int knob = 16;
        const int knobX = isChecked() ? width() - knob - 4 : 4;
        painter.setBrush(Qt::white);
        painter.drawEllipse(QRect(knobX, 4, knob, knob));
    }
};

class StableComboBox final : public QComboBox {
public:
    explicit StableComboBox(QWidget* parent = nullptr)
        : QComboBox(parent)
    {
        auto* list = new QListView(this);
        list->setUniformItemSizes(true);
        list->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
        list->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        list->setStyleSheet(QStringLiteral(R"(
            QListView {
                background-color: #FFFFFF;
                color: #182230;
                border: 1px solid #CBD5E1;
                border-radius: 6px;
                outline: none;
                padding: 4px;
            }
            QListView::item {
                min-height: 30px;
                padding: 2px 9px;
                border-radius: 4px;
            }
            QListView::item:hover { background-color: #F1F5F9; }
            QListView::item:selected {
                background-color: #E3EBFF;
                color: #2858D4;
            }
            QListView::item:disabled {
                background-color: #FFFFFF;
                color: #98A2B3;
            }
        )"));
        applyLightPalette(list);
        setView(list);
        setMaxVisibleItems(10);
        setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
    }

protected:
    void showPopup() override
    {
        applyLightPalette(view());
        QWidget* popup = view() ? view()->window() : nullptr;
        if (popup != window()) {
            applyLightPalette(popup);
        }
        QComboBox::showPopup();
        applyLightPalette(view());
        popup = view() ? view()->window() : nullptr;
        if (popup != window()) {
            applyLightPalette(popup);
        }
    }

private:
    static void applyLightPalette(QWidget* widget)
    {
        if (!widget) {
            return;
        }
        QPalette palette = widget->palette();
        palette.setColor(QPalette::Window, QColor(QStringLiteral("#FFFFFF")));
        palette.setColor(QPalette::Base, QColor(QStringLiteral("#FFFFFF")));
        palette.setColor(QPalette::AlternateBase, QColor(QStringLiteral("#F8FAFC")));
        palette.setColor(QPalette::Text, QColor(QStringLiteral("#182230")));
        palette.setColor(QPalette::WindowText, QColor(QStringLiteral("#182230")));
        palette.setColor(QPalette::Highlight, QColor(QStringLiteral("#E3EBFF")));
        palette.setColor(QPalette::HighlightedText, QColor(QStringLiteral("#2858D4")));
        widget->setPalette(palette);
        widget->setAutoFillBackground(true);
        widget->setAttribute(Qt::WA_OpaquePaintEvent, false);
        if (auto* scrollArea = qobject_cast<QAbstractScrollArea*>(widget)) {
            QWidget* viewport = scrollArea->viewport();
            viewport->setPalette(palette);
            viewport->setAutoFillBackground(true);
            viewport->setAttribute(Qt::WA_OpaquePaintEvent, false);
        }
    }
};

class CurrentPageStackedWidget final : public QStackedWidget {
public:
    explicit CurrentPageStackedWidget(QWidget* parent = nullptr)
        : QStackedWidget(parent)
    {
        connect(this, &QStackedWidget::currentChanged,
                this, [this]() { updateGeometry(); });
    }

    QSize sizeHint() const override
    {
        return currentWidget() ? currentWidget()->sizeHint()
                               : QStackedWidget::sizeHint();
    }

    QSize minimumSizeHint() const override
    {
        return currentWidget() ? currentWidget()->minimumSizeHint()
                               : QStackedWidget::minimumSizeHint();
    }
};

QLabel* labelWithRole(const QString& text, const QString& role, QWidget* parent = nullptr)
{
    auto* label = new QLabel(text, parent);
    label->setObjectName(role);
    return label;
}

QLabel* hint(const QString& text, QWidget* parent = nullptr)
{
    auto* label = labelWithRole(text, QStringLiteral("VisnipHintLabel"), parent);
    label->setWordWrap(true);
    return label;
}

QFrame* divider()
{
    auto* line = new QFrame;
    line->setObjectName(QStringLiteral("SettingsDivider"));
    line->setFrameShape(QFrame::HLine);
    return line;
}

void addSectionTitle(QVBoxLayout* layout, const QString& title)
{
    if (layout->count() > 3) {
        layout->addSpacing(18);
    }
    auto* label = labelWithRole(title, QStringLiteral("SettingsSectionTitle"));
    layout->addWidget(label);
    layout->addSpacing(4);
}

void setSettingsRole(QAbstractButton* button, const QString& role)
{
    button->setProperty("settingsRole", role);
    button->setCursor(Qt::PointingHandCursor);
}

QString readableBytes(qint64 bytes)
{
    return bytes >= (1LL << 30)
        ? QStringLiteral("%1 GiB").arg(bytes / double(1LL << 30), 0, 'f', 2)
        : QStringLiteral("%1 MiB").arg(bytes / double(1LL << 20), 0, 'f', 1);
}

QWidget* controlRow(const QString& title, const QString& description, QWidget* control)
{
    auto* row = new QWidget;
    row->setObjectName(QStringLiteral("SettingsRow"));
    auto* rowLayout = new QHBoxLayout(row);
    rowLayout->setContentsMargins(0, 10, 0, 10);
    rowLayout->setSpacing(18);

    auto* copy = new QVBoxLayout;
    copy->setContentsMargins(0, 0, 0, 0);
    copy->setSpacing(3);
    copy->addWidget(labelWithRole(title, QStringLiteral("SettingsRowTitle")));
    if (!description.isEmpty()) {
        auto* descriptionLabel = labelWithRole(
            description, QStringLiteral("SettingsRowDescription"));
        descriptionLabel->setWordWrap(true);
        descriptionLabel->setMinimumWidth(0);
        copy->addWidget(descriptionLabel);
    }
    rowLayout->addLayout(copy, 1);
    rowLayout->addWidget(control, 0, Qt::AlignRight | Qt::AlignVCenter);
    return row;
}

void addControlRow(QVBoxLayout* layout, const QString& title,
                   const QString& description, QWidget* control,
                   bool addDivider = true)
{
    layout->addWidget(controlRow(title, description, control));
    if (addDivider) {
        layout->addWidget(divider());
    }
}

QWidget* makePage(const QString& title, const QString& subtitle,
                  QVBoxLayout*& layout, const QString& scrollObjectName = {})
{
    auto* content = new QWidget;
    content->setObjectName(QStringLiteral("SettingsPage"));
    layout = new QVBoxLayout(content);
    layout->setContentsMargins(34, 28, 38, 30);
    layout->setSpacing(0);

    layout->addWidget(labelWithRole(title, QStringLiteral("SettingsPageTitle")));
    auto* subtitleLabel = labelWithRole(subtitle, QStringLiteral("SettingsPageSubtitle"));
    subtitleLabel->setWordWrap(true);
    layout->addWidget(subtitleLabel);
    layout->addSpacing(24);

    auto* scroll = new QScrollArea;
    scroll->setObjectName(scrollObjectName.isEmpty()
                              ? QStringLiteral("SettingsPageScroll")
                              : scrollObjectName);
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    scroll->setWidgetResizable(true);
    scroll->setWidget(content);
    return scroll;
}

SettingsSwitch* makeSwitch(bool checked, const QString& accessibleName)
{
    auto* control = new SettingsSwitch(checked);
    control->setAccessibleName(accessibleName);
    return control;
}

QWidget* modeSummary(const QString& title, const QString& body)
{
    auto* panel = new QWidget;
    panel->setObjectName(QStringLiteral("SettingsModeSummary"));
    auto* layout = new QVBoxLayout(panel);
    layout->setContentsMargins(16, 14, 16, 14);
    layout->setSpacing(4);
    layout->addWidget(labelWithRole(title, QStringLiteral("SettingsModeTitle")));
    auto* description = labelWithRole(body, QStringLiteral("SettingsRowDescription"));
    description->setWordWrap(true);
    layout->addWidget(description);
    return panel;
}

// Non-blocking explanation shown before a translation mode may upload anything.
QMessageBox* uploadNotice(QWidget* parent, const AiTranslateSettings& candidate)
{
    QString receiver;
    QString body;
    if (candidate.translationMethod == TranslationMethod::Intranet) {
        receiver = QUrl(candidate.fastImageTranslateEndpoint()).host();
        body = QStringLiteral("内网服务器模式会把你选中的截图区域上传到管理员配置的服务器%1处理，并取回译图。")
                   .arg(receiver.isEmpty() ? QStringLiteral("（填写地址后才会发送）") : QStringLiteral("「%1」").arg(receiver));
    } else if (candidate.translationMethod == TranslationMethod::LocalOcr) {
        receiver = QUrl(candidate.fastTranslateEndpoint()).host();
        body = candidate.fastProvider == QStringLiteral("baidu")
            ? QStringLiteral("混合模式在本机识别文字，再把识别出的文字发送到百度翻译开放平台翻译。截图本身不上传。")
            : QStringLiteral("混合模式在本机识别文字，再把识别出的文字发送到「%1」翻译。截图本身不上传。").arg(receiver);
    } else {
        receiver = QUrl(candidate.fastImageTranslateEndpoint()).host();
        body = QStringLiteral("云端翻译会把你选中的截图区域上传到「%1」处理，并取回译图。").arg(receiver);
    }
    auto* box = new QMessageBox(QMessageBox::Information, QStringLiteral("确认上传说明"),
        body + QStringLiteral("\n\n• 只在你点击翻译时发送当前选区的内容，不会上传整个屏幕。\n"
                              "• 接收方如何保存和处理数据，以该服务的隐私说明为准。\n"
                              "• 随时可以改用「本机离线」，图片和文字都不离开这台电脑。\n\n"
                              "确认后才会启用；取消则保持原来的处理方式。"),
        QMessageBox::NoButton, parent);
    box->setObjectName(QStringLiteral("VisnipUploadNotice"));
    box->addButton(QStringLiteral("同意并启用"), QMessageBox::AcceptRole);
    box->addButton(QStringLiteral("取消"), QMessageBox::RejectRole);
    box->setAttribute(Qt::WA_DeleteOnClose);
    box->setWindowModality(Qt::WindowModal);
    return box;
}

void recordUploadConsent(AiTranslateSettings& settings, TranslationMethod method)
{
    (method == TranslationMethod::Intranet ? settings.intranetUploadConsent : settings.cloudUploadConsent) =
        AiTranslateSettings::kUploadNoticeVersion;
}

QString settingsStyleSheet()
{
    return QStringLiteral(R"(
        QDialog#VisnipSettingsDialog { background: #F5F7FA; }
        QFrame#SettingsSidebar { background: #F5F7FA; border: none; }
        QListWidget#VisnipSettingsNav {
            background: transparent;
            border: none;
            border-radius: 0;
            padding: 0;
        }
        QListWidget#VisnipSettingsNav::item {
            min-height: 38px;
            margin: 1px 0;
            padding: 2px 12px;
            border-radius: 6px;
            color: #2A3444;
        }
        QListWidget#VisnipSettingsNav::item:hover { background: #E9EDF4; }
        QListWidget#VisnipSettingsNav::item:selected {
            background: #E3EBFF;
            color: #2858D4;
            font-weight: 600;
        }
        QStackedWidget#SettingsStack { background: #FFFFFF; }
        QScrollArea#SettingsPageScroll,
        QScrollArea#VisnipTranslationScrollArea,
        QScrollArea#SettingsPageScroll > QWidget > QWidget,
        QScrollArea#VisnipTranslationScrollArea > QWidget > QWidget,
        QWidget#SettingsPage { background: #FFFFFF; }
        QLabel#SettingsPageTitle {
            color: #111827;
            font-size: 22px;
            font-weight: 650;
        }
        QLabel#SettingsPageSubtitle {
            color: #667085;
            font-size: 12px;
            margin-top: 5px;
        }
        QLabel#SettingsSectionTitle {
            color: #344054;
            font-size: 12px;
            font-weight: 650;
        }
        QLabel#SettingsRowTitle {
            color: #182230;
            font-size: 13px;
            font-weight: 500;
        }
        QLabel#SettingsRowDescription {
            color: #7A8494;
            font-size: 11px;
        }
        QWidget#SettingsRow { background: transparent; min-height: 52px; }
        QFrame#SettingsDivider {
            color: #E9EDF3;
            background: #E9EDF3;
            border: none;
            max-height: 1px;
        }
        QLineEdit, QComboBox, QSpinBox, QKeySequenceEdit {
            min-height: 32px;
            border-radius: 6px;
            background: #FFFFFF;
        }
        QComboBox QAbstractItemView {
            background: #FFFFFF;
            color: #182230;
            border-radius: 6px;
            padding: 4px;
        }
        QComboBox QAbstractItemView::item {
            min-height: 30px;
            padding: 2px 9px;
        }
        QFrame#SettingsSegmented {
            background: #EEF2F6;
            border: none;
            border-radius: 7px;
        }
        QPushButton[settingsSegment="true"] {
            min-height: 34px;
            border: none;
            border-radius: 5px;
            background: transparent;
            color: #596273;
            padding: 0 18px;
            font-weight: 500;
        }
        QPushButton[settingsSegment="true"]:hover { background: #E1E7EF; }
        QPushButton[settingsSegment="true"]:checked {
            background: #3567E8;
            color: #FFFFFF;
            border: none;
        }
        QPushButton[settingsSegment="true"]:checked:hover { background: #2858D4; }
        QPushButton[settingsSegment="true"]:checked:pressed { background: #234EC2; }
        QPushButton[settingsSegment="true"]:focus { border: 1px solid #9DB5F3; }
        QPushButton[settingsSegment="true"]:disabled { color: #98A2B3; }
        QPushButton[settingsRole="primary"] {
            min-height: 32px;
            background: #3567E8;
            border: 1px solid #3567E8;
            border-radius: 6px;
            color: #FFFFFF;
            padding: 0 14px;
            font-weight: 600;
        }
        QPushButton[settingsRole="primary"]:hover {
            background: #2858D4;
            border-color: #2858D4;
        }
        QPushButton[settingsRole="primary"]:pressed {
            background: #234EC2;
            border-color: #234EC2;
        }
        QPushButton[settingsRole="secondary"] {
            min-height: 32px;
            background: #FFFFFF;
            border: 1px solid #D6DEE9;
            border-radius: 6px;
            color: #344054;
            padding: 0 13px;
        }
        QPushButton[settingsRole="secondary"]:hover {
            background: #F3F6FA;
            border-color: #B9C4D3;
        }
        QPushButton[settingsRole="secondary"]:pressed { background: #E8EDF4; }
        QPushButton[settingsRole="danger"] {
            min-height: 32px;
            background: #FFF7F6;
            border: 1px solid #F5C9C5;
            border-radius: 6px;
            color: #B42318;
            padding: 0 13px;
        }
        QPushButton[settingsRole="danger"]:hover {
            background: #FEECEB;
            border-color: #E9AAA4;
        }
        QPushButton[settingsRole="primary"]:focus,
        QPushButton[settingsRole="secondary"]:focus,
        QPushButton[settingsRole="danger"]:focus,
        QToolButton[settingsRole="inline"]:focus {
            border: 1px solid #7C9CF0;
        }
        QPushButton[settingsRole="primary"]:disabled,
        QPushButton[settingsRole="secondary"]:disabled,
        QPushButton[settingsRole="danger"]:disabled {
            background: #F2F4F7;
            border-color: #E4E7EC;
            color: #98A2B3;
        }
        QToolButton[settingsRole="inline"] {
            min-width: 48px;
            max-width: 48px;
            min-height: 32px;
            max-height: 32px;
            background: #EEF4FF;
            border: 1px solid #D7E3FF;
            border-radius: 6px;
            color: #2858D4;
            padding: 0;
            font-weight: 600;
        }
        QToolButton[settingsRole="inline"]:hover { background: #E3EBFF; }
        QToolButton[settingsRole="inline"]:pressed { background: #D7E3FF; }
        QWidget#SettingsModeSummary {
            background: #F7F9FC;
            border: 1px solid #E5EAF1;
            border-radius: 7px;
        }
        QLabel#SettingsModeTitle { color: #182230; font-weight: 600; }
        QLabel#SettingsInlineError { color: #B42318; font-size: 11px; }
        QLabel#SettingsInlineSuccess { color: #087A55; font-size: 11px; }
        QLabel#SettingsSaveStatus { color: #7A8494; font-size: 10px; }
        QPushButton#SettingsResetButton {
            min-height: 30px;
            background: #FFFFFF;
            border: 1px solid #D9E0E9;
            border-radius: 6px;
            color: #596273;
            padding: 0 10px;
        }
        QPushButton#SettingsResetButton:hover {
            background: #EEF2F6;
            border-color: #C4CDD9;
        }
        QToolButton#SettingsDisclosure {
            min-height: 30px;
            border: none;
            background: transparent;
            color: #596273;
            padding: 0 4px;
        }
        QToolButton#SettingsDisclosure:hover { color: #2858D4; }
        QProgressBar {
            min-height: 6px;
            max-height: 6px;
            border: none;
            border-radius: 3px;
            background: #E8EDF4;
            text-align: center;
        }
        QProgressBar::chunk { background: #3567E8; border-radius: 3px; }
        /* The resource download paints its byte counter inside the bar, so it must
           not inherit the 6 px height of the compact indicators above. The fill is
           kept light because Qt draws that text in one colour across the whole bar. */
        QProgressBar#VisnipSettingsOfflineProgress {
            min-height: 22px;
            max-height: 22px;
            border-radius: 6px;
            font-size: 12px;
        }
        QProgressBar#VisnipSettingsOfflineProgress::chunk {
            background: #C3D6F9;
            border-radius: 6px;
        }
    )");
}

bool hotkeysAreUnique(const HotkeySettings& settings)
{
    const QList<QKeySequence> values = {
        settings.capture,
        settings.pinClipboard,
        settings.repeatCapture,
        settings.togglePins,
        settings.toggleMouseThrough,
        settings.askQuestion,
    };
    for (int left = 0; left < values.size(); ++left) {
        for (int right = left + 1; right < values.size(); ++right) {
            if (values[left] == values[right]) {
                return false;
            }
        }
    }
    return true;
}

} // namespace

SettingsDialog::SettingsDialog(
    AppConfig* config,
    OcrPackDownloadService* ocrPackDownloads,
    QWidget* parent)
    : QDialog(parent)
    , config_(config)
    , ocrPackDownloads_(ocrPackDownloads)
    , nav_(new QListWidget(this))
    , stack_(new CurrentPageStackedWidget(this))
    , saveTimer_(new QTimer(this))
{
    Perf::ScopedTimer timer(QStringLiteral("SettingsDialog.constructor"));
    Q_ASSERT(config_);
    offlineResources_ = new OfflineResourceService(this);
    QApplication::setEffectEnabled(Qt::UI_AnimateCombo, false);

    setObjectName(QStringLiteral("VisnipSettingsDialog"));
    setWindowTitle(QStringLiteral("Visnip 设置"));
    setWindowIcon(Ui::applicationIcon());
    setMinimumSize(780, 540);
    resize(820, 580);
    setStyleSheet(Design::appStyleSheet(false) + settingsStyleSheet());

    saveTimer_->setSingleShot(true);
    saveTimer_->setInterval(kSettingsSaveDelayMs);
    connect(saveTimer_, &QTimer::timeout, this, [this]() { flushPendingSave(); });

    auto* root = new QHBoxLayout(this);
    root->setContentsMargins(0, 0, 0, 0);
    root->setSpacing(0);

    auto* sidebar = new QFrame;
    sidebar->setObjectName(QStringLiteral("SettingsSidebar"));
    sidebar->setFixedWidth(174);
    auto* sidebarLayout = new QVBoxLayout(sidebar);
    sidebarLayout->setContentsMargins(14, 14, 14, 16);
    sidebarLayout->setSpacing(8);

    nav_->setObjectName(QStringLiteral("VisnipSettingsNav"));
    nav_->setIconSize(QSize(16, 16));
    nav_->setSpacing(0);
    nav_->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    sidebarLayout->addWidget(nav_, 1);

    saveStatus_ = labelWithRole(QStringLiteral("更改会自动保存"),
                                QStringLiteral("SettingsSaveStatus"));
    saveStatus_->setAlignment(Qt::AlignCenter);
    sidebarLayout->addWidget(saveStatus_);

    auto* reset = new QPushButton(QStringLiteral("恢复全部默认设置"));
    reset->setObjectName(QStringLiteral("SettingsResetButton"));
    reset->setAutoDefault(false);
    sidebarLayout->addWidget(reset);

    stack_->setObjectName(QStringLiteral("SettingsStack"));
    root->addWidget(sidebar);
    root->addWidget(stack_, 1);

    addPage(Page::General, QStringLiteral("常规"));
    addPage(Page::Capture, QStringLiteral("截图"));
    addPage(Page::Pin, QStringLiteral("贴图"));
    addPage(Page::Output, QStringLiteral("输出"));
    addPage(Page::Translation, QStringLiteral("翻译"));
    addPage(Page::Question, QStringLiteral("解题"));
    addPage(Page::Hotkey, QStringLiteral("快捷键"));
    addPage(Page::About, QStringLiteral("关于"));

    connect(nav_, &QListWidget::currentRowChanged,
            this, [this](int row) {
        if (row < 0 || row >= stack_->count()) {
            return;
        }
        ensurePage(static_cast<Page>(row));
        stack_->setCurrentIndex(row);
    });
    connect(reset, &QPushButton::clicked, this, [this]() {
        const auto answer = QMessageBox::question(
            this,
            QStringLiteral("恢复默认设置"),
            QStringLiteral("这会重置截图、贴图、输出、翻译、解题和快捷键设置。是否继续？"),
            QMessageBox::Yes | QMessageBox::Cancel,
            QMessageBox::Cancel);
        if (answer != QMessageBox::Yes) {
            return;
        }
        saveTimer_->stop();
        savePending_ = false;
        config_->resetDefaults();
        if (!config_->save()) {
            saveStatus_->setText(QStringLiteral("保存失败，请检查系统配置权限"));
            return;
        }
        if (!config_->syncAutoStart()) {
            saveStatus_->setText(QStringLiteral("设置已保存，但开机启动更新失败"));
            return;
        }
        emit hotkeyChangeRequested(config_->settings().hotkeys);
        resetPageCache();
        accept();
    });
    ensurePage(Page::General);
    nav_->setCurrentRow(0);
}

SettingsDialog::~SettingsDialog()
{
    flushPendingSave();
}

void SettingsDialog::closeEvent(QCloseEvent* event)
{
    if (!flushPendingSave()) {
        event->ignore();
        return;
    }
    QDialog::closeEvent(event);
}

void SettingsDialog::scheduleSave(bool syncAutoStart)
{
    savePending_ = true;
    autoStartSyncPending_ = autoStartSyncPending_ || syncAutoStart;
    saveStatus_->setText(QStringLiteral("正在保存..."));
    saveTimer_->start();
}

bool SettingsDialog::flushPendingSave()
{
    if (!savePending_) {
        return true;
    }
    saveTimer_->stop();
    const bool syncAutoStart = autoStartSyncPending_;
    savePending_ = false;
    autoStartSyncPending_ = false;
    if (!config_->save()) {
        saveStatus_->setText(QStringLiteral("保存失败，请检查系统配置权限"));
        return false;
    }
    if (syncAutoStart) {
        if (!config_->syncAutoStart()) {
            saveStatus_->setText(QStringLiteral("设置已保存，但开机启动更新失败"));
            return false;
        }
    }
    saveStatus_->setText(QStringLiteral("已保存"));
    return true;
}

void SettingsDialog::showPage(Page page)
{
    const int index = static_cast<int>(page);
    ensurePage(page);
    nav_->setCurrentRow(index);
    stack_->setCurrentIndex(index);
}

void SettingsDialog::setHotkeyChangeResult(const HotkeySettings& effective,
                                           const QString& errorMessage)
{
    const QList<QKeySequence> sequences = {
        effective.capture,
        effective.pinClipboard,
        effective.repeatCapture,
        effective.togglePins,
        effective.toggleMouseThrough,
        effective.askQuestion,
    };
    const int count = qMin(hotkeyEdits_.size(), sequences.size());
    for (int index = 0; index < count; ++index) {
        QSignalBlocker blocker(hotkeyEdits_[index]);
        hotkeyEdits_[index]->setKeySequence(sequences[index]);
    }
    if (!hotkeyStatus_) {
        return;
    }
    hotkeyStatus_->setText(errorMessage);
    hotkeyStatus_->setVisible(!errorMessage.isEmpty());
}

void SettingsDialog::addPage(Page pageId, const QString& title)
{
    Q_ASSERT(nav_->count() == static_cast<int>(pageId));
    Q_ASSERT(stack_->count() == static_cast<int>(pageId));
    auto* item = new QListWidgetItem(title, nav_);
    const QString iconId = [pageId]() {
        switch (pageId) {
        case Page::General: return QStringLiteral("nav-general");
        case Page::Capture: return QStringLiteral("nav-capture");
        case Page::Pin: return QStringLiteral("nav-pin");
        case Page::Output: return QStringLiteral("nav-output");
        case Page::Translation: return QStringLiteral("action-translate");
        case Page::Question: return QStringLiteral("action-question");
        case Page::Hotkey: return QStringLiteral("nav-hotkey");
        case Page::About: return QStringLiteral("nav-about");
        }
        return QStringLiteral("nav-about");
    }();
    item->setIcon(Ui::toolbarIcon(iconId, false, false));
    auto* placeholder = new QWidget;
    placeholder->setObjectName(QStringLiteral("SettingsPagePlaceholder"));
    stack_->addWidget(placeholder);
}

QWidget* SettingsDialog::createPage(Page pageId)
{
    switch (pageId) {
    case Page::General: return createGeneralPage();
    case Page::Capture: return createCapturePage();
    case Page::Pin: return createPinPage();
    case Page::Output: return createOutputPage();
    case Page::Translation: return createTranslationPage();
    case Page::Question: return createQuestionPage();
    case Page::Hotkey: return createHotkeyPage();
    case Page::About: return createAboutPage();
    }
    Q_UNREACHABLE();
    return nullptr;
}

void SettingsDialog::ensurePage(Page pageId)
{
    const int index = static_cast<int>(pageId);
    if (index < 0 || index >= stack_->count()) {
        return;
    }
    QWidget* placeholder = stack_->widget(index);
    if (!placeholder
        || placeholder->objectName() != QStringLiteral("SettingsPagePlaceholder")) {
        return;
    }

    Perf::ScopedTimer timer(
        QStringLiteral("SettingsDialog.createPage.%1").arg(index));
    QWidget* page = createPage(pageId);
    stack_->removeWidget(placeholder);
    stack_->insertWidget(index, page);
    delete placeholder;
}

void SettingsDialog::resetPageCache()
{
    offlineResources_->cancel();
    hotkeyEdits_.clear();
    hotkeyStatus_ = nullptr;
    while (stack_->count() > 0) {
        QWidget* page = stack_->widget(0);
        stack_->removeWidget(page);
        delete page;
    }
    for (int index = 0; index <= static_cast<int>(Page::About); ++index) {
        auto* placeholder = new QWidget;
        placeholder->setObjectName(QStringLiteral("SettingsPagePlaceholder"));
        stack_->addWidget(placeholder);
    }
    ensurePage(Page::General);
    nav_->setCurrentRow(static_cast<int>(Page::General));
    stack_->setCurrentIndex(static_cast<int>(Page::General));
}

QWidget* SettingsDialog::createGeneralPage()
{
    const auto& settings = config_->settings();
    QVBoxLayout* layout = nullptr;
    auto* page = makePage(
        QStringLiteral("常规"),
        QStringLiteral("管理 Visnip 的启动方式和截图界面外观。"),
        layout);

    addSectionTitle(layout, QStringLiteral("启动"));
    auto* autoStart = makeSwitch(settings.autoStart, QStringLiteral("开机自动启动"));
    connect(autoStart, &QAbstractButton::toggled, this, [this](bool checked) {
        config_->mutableSettings().autoStart = checked;
        scheduleSave(true);
    });
    addControlRow(layout,
                  QStringLiteral("开机自动启动"),
                  QStringLiteral("登录 Windows 后在系统托盘中运行"),
                  autoStart,
                  false);

    addSectionTitle(layout, QStringLiteral("截图工具栏"));
    auto* darkToolbar = makeSwitch(settings.ui.darkToolbar,
                                   QStringLiteral("使用深色截图工具栏"));
    connect(darkToolbar, &QAbstractButton::toggled, this, [this](bool checked) {
        config_->mutableSettings().ui.darkToolbar = checked;
        scheduleSave();
    });
    addControlRow(layout,
                  QStringLiteral("深色工具栏"),
                  QStringLiteral("下一次截图时使用深色工具栏"),
                  darkToolbar,
                  false);

    layout->addStretch();
    return page;
}

QWidget* SettingsDialog::createCapturePage()
{
    const auto& settings = config_->settings().capture;
    QVBoxLayout* layout = nullptr;
    auto* page = makePage(
        QStringLiteral("截图"),
        QStringLiteral("这些选区辅助项会在下一次开始截图时生效。"),
        layout);

    addSectionTitle(layout, QStringLiteral("选区辅助"));
    auto* magnifier = makeSwitch(settings.showMagnifier, QStringLiteral("显示放大镜"));
    connect(magnifier, &QAbstractButton::toggled, this, [this](bool checked) {
        config_->mutableSettings().capture.showMagnifier = checked;
        scheduleSave();
    });
    addControlRow(layout,
                  QStringLiteral("像素放大镜"),
                  QStringLiteral("移动光标和调整选区时显示局部放大预览"),
                  magnifier);

    auto* cursorColor = makeSwitch(settings.showCursorColor,
                                   QStringLiteral("显示 RGB 和 HEX 取色"));
    connect(cursorColor, &QAbstractButton::toggled, this, [this](bool checked) {
        config_->mutableSettings().capture.showCursorColor = checked;
        scheduleSave();
    });
    addControlRow(layout,
                  QStringLiteral("光标取色"),
                  QStringLiteral("在放大镜内或光标旁显示 RGB 与 HEX 色值"),
                  cursorColor);

    auto* sizeLabel = makeSwitch(settings.showSizeLabel,
                                 QStringLiteral("显示选区尺寸"));
    connect(sizeLabel, &QAbstractButton::toggled, this, [this](bool checked) {
        config_->mutableSettings().capture.showSizeLabel = checked;
        scheduleSave();
    });
    addControlRow(layout,
                  QStringLiteral("选区尺寸"),
                  QStringLiteral("在选区附近显示宽度和高度"),
                  sizeLabel);

    auto* borderWidth = new QSpinBox;
    borderWidth->setObjectName(QStringLiteral("VisnipSettingsBorderWidth"));
    borderWidth->setRange(1, 6);
    borderWidth->setSuffix(QStringLiteral(" px"));
    borderWidth->setValue(settings.borderWidth);
    borderWidth->setFixedWidth(104);
    connect(borderWidth, qOverload<int>(&QSpinBox::valueChanged),
            this, [this](int value) {
        config_->mutableSettings().capture.borderWidth = value;
        scheduleSave();
    });
    addControlRow(layout,
                  QStringLiteral("选区边框"),
                  QStringLiteral("调整选区轮廓和拖拽命中区域的宽度"),
                  borderWidth,
                  false);

    layout->addStretch();
    return page;
}

QWidget* SettingsDialog::createPinPage()
{
    const auto& settings = config_->settings().pin;
    QVBoxLayout* layout = nullptr;
    auto* page = makePage(
        QStringLiteral("贴图"),
        QStringLiteral("以下默认行为只应用于之后新建的贴图，不改变当前贴图。"),
        layout);

    addSectionTitle(layout, QStringLiteral("新建贴图默认行为"));
    auto* alwaysOnTop = makeSwitch(settings.alwaysOnTop, QStringLiteral("新建贴图保持置顶"));
    connect(alwaysOnTop, &QAbstractButton::toggled, this, [this](bool checked) {
        config_->mutableSettings().pin.alwaysOnTop = checked;
        scheduleSave();
    });
    addControlRow(layout,
                  QStringLiteral("保持置顶"),
                  QStringLiteral("让新贴图显示在其他窗口上方"),
                  alwaysOnTop);

    auto* shadow = makeSwitch(settings.shadow, QStringLiteral("新建贴图显示阴影"));
    connect(shadow, &QAbstractButton::toggled, this, [this](bool checked) {
        config_->mutableSettings().pin.shadow = checked;
        scheduleSave();
    });
    addControlRow(layout,
                  QStringLiteral("窗口阴影"),
                  QStringLiteral("为新贴图增加轻微的边缘层次"),
                  shadow);

    auto* doubleClickHide = makeSwitch(settings.doubleClickHide,
                                       QStringLiteral("双击新贴图时隐藏"));
    connect(doubleClickHide, &QAbstractButton::toggled, this, [this](bool checked) {
        config_->mutableSettings().pin.doubleClickHide = checked;
        scheduleSave();
    });
    addControlRow(layout,
                  QStringLiteral("双击隐藏"),
                  QStringLiteral("双击贴图后将其临时隐藏"),
                  doubleClickHide);

    auto* opacity = new QSpinBox;
    opacity->setObjectName(QStringLiteral("VisnipSettingsPinOpacity"));
    opacity->setRange(20, 100);
    opacity->setSuffix(QStringLiteral(" %"));
    opacity->setValue(qRound(settings.defaultOpacity * 100.0));
    opacity->setFixedWidth(104);
    connect(opacity, qOverload<int>(&QSpinBox::valueChanged), this, [this](int value) {
        config_->mutableSettings().pin.defaultOpacity = value / 100.0;
        scheduleSave();
    });
    addControlRow(layout,
                  QStringLiteral("默认不透明度"),
                  QStringLiteral("设置新贴图创建时的透明程度"),
                  opacity,
                  false);

    layout->addStretch();
    return page;
}

QWidget* SettingsDialog::createOutputPage()
{
    const auto& settings = config_->settings().output;
    QVBoxLayout* layout = nullptr;
    auto* page = makePage(
        QStringLiteral("输出"),
        QStringLiteral("设置截图保存位置、文件命名和完成后的行为。"),
        layout);

    addSectionTitle(layout, QStringLiteral("保存位置"));
    auto* directoryEdit = new QLineEdit(settings.saveDirectory);
    directoryEdit->setObjectName(QStringLiteral("VisnipSettingsOutputDirectory"));
    auto* browse = new QPushButton(QStringLiteral("浏览..."));
    browse->setAutoDefault(false);
    setSettingsRole(browse, QStringLiteral("secondary"));
    auto* directoryControl = new QWidget;
    directoryControl->setMinimumWidth(360);
    auto* directoryLayout = new QHBoxLayout(directoryControl);
    directoryLayout->setContentsMargins(0, 0, 0, 0);
    directoryLayout->setSpacing(8);
    directoryLayout->addWidget(directoryEdit, 1);
    directoryLayout->addWidget(browse);
    connect(directoryEdit, &QLineEdit::textChanged, this, [this](const QString& text) {
        const QString directory = text.trimmed();
        config_->mutableSettings().output.saveDirectory = directory.isEmpty()
            ? config_->defaultSaveDirectory()
            : directory;
        scheduleSave();
    });
    connect(directoryEdit, &QLineEdit::editingFinished,
            this, [this]() { flushPendingSave(); });
    connect(browse, &QPushButton::clicked, this, [this, directoryEdit]() {
        const QString directory = QFileDialog::getExistingDirectory(
            this, QStringLiteral("选择保存目录"), directoryEdit->text());
        if (!directory.isEmpty()) {
            directoryEdit->setText(directory);
            flushPendingSave();
        }
    });
    addControlRow(layout,
                  QStringLiteral("默认保存目录"),
                  QStringLiteral("常规保存操作会使用此目录"),
                  directoryControl);

    auto* patternEdit = new QLineEdit(settings.filenamePattern);
    patternEdit->setObjectName(QStringLiteral("VisnipSettingsFilenamePattern"));
    patternEdit->setMinimumWidth(280);
    auto* patternStatus = labelWithRole(QString(), QStringLiteral("SettingsInlineError"));
    patternStatus->setWordWrap(true);
    auto* patternControl = new QWidget;
    patternControl->setMinimumWidth(360);
    auto* patternLayout = new QVBoxLayout(patternControl);
    patternLayout->setContentsMargins(0, 0, 0, 0);
    patternLayout->setSpacing(4);
    patternLayout->addWidget(patternEdit);
    patternLayout->addWidget(patternStatus);
    const auto updatePattern = [this, patternEdit, patternStatus]() {
        const QString value = patternEdit->text().trimmed();
        const QString error = outputFilenamePatternError(value);
        patternStatus->setText(error.isEmpty()
                                   ? QStringLiteral("%1 会替换为截图时间")
                                   : error);
        patternStatus->setObjectName(error.isEmpty()
                                         ? QStringLiteral("SettingsInlineSuccess")
                                         : QStringLiteral("SettingsInlineError"));
        patternStatus->style()->unpolish(patternStatus);
        patternStatus->style()->polish(patternStatus);
        if (error.isEmpty()
            && config_->settings().output.filenamePattern != value) {
            config_->mutableSettings().output.filenamePattern = value;
            scheduleSave();
        }
    };
    updatePattern();
    connect(patternEdit, &QLineEdit::textChanged, this,
            [updatePattern](const QString&) { updatePattern(); });
    connect(patternEdit, &QLineEdit::editingFinished, this, [this, patternEdit]() {
        if (!outputFilenamePatternError(patternEdit->text().trimmed()).isEmpty()) {
            return;
        }
        flushPendingSave();
    });
    addControlRow(layout,
                  QStringLiteral("文件名规则"),
                  QStringLiteral("必须包含时间占位符 %1，支持 PNG 或 JPEG"),
                  patternControl,
                  false);

    addSectionTitle(layout, QStringLiteral("完成后"));
    auto* copyThenExit = makeSwitch(settings.copyThenExit,
                                    QStringLiteral("复制后关闭截图层"));
    connect(copyThenExit, &QAbstractButton::toggled, this, [this](bool checked) {
        config_->mutableSettings().output.copyThenExit = checked;
        scheduleSave();
    });
    addControlRow(layout,
                  QStringLiteral("复制后关闭截图层"),
                  QStringLiteral("下一次截图时生效"),
                  copyThenExit);

    auto* notification = makeSwitch(settings.showSaveNotification,
                                    QStringLiteral("常规截图保存后通知"));
    connect(notification, &QAbstractButton::toggled, this, [this](bool checked) {
        config_->mutableSettings().output.showSaveNotification = checked;
        scheduleSave();
    });
    addControlRow(layout,
                  QStringLiteral("保存通知"),
                  QStringLiteral("常规截图保存成功后显示系统托盘通知"),
                  notification,
                  false);

    layout->addStretch();
    return page;
}

QWidget* SettingsDialog::createTranslationPage()
{
    const auto& settings = config_->settings().aiTranslate;
    QVBoxLayout* layout = nullptr;
    // The open-source build offers offline translation only (no online modes,
    // no service address); the mode selector and online settings are hidden.
    const bool online = onlineTranslationEnabled();
    auto* page = makePage(
        QStringLiteral("翻译"),
        online ? QStringLiteral("云端翻译会上传选区；本机离线在当前电脑完成处理。")
               : QStringLiteral("翻译在本机离线完成，截图和文字不会离开这台电脑。"),
        layout,
        QStringLiteral("VisnipTranslationScrollArea"));

    if (online) {
        addSectionTitle(layout, QStringLiteral("处理方式"));
    }
    auto* segmented = new QFrame;
    segmented->setObjectName(QStringLiteral("SettingsSegmented"));
    auto* segmentedLayout = new QHBoxLayout(segmented);
    segmentedLayout->setContentsMargins(3, 3, 3, 3);
    segmentedLayout->setSpacing(3);
    auto* localButton = new QPushButton(QStringLiteral("混合（联网）"));
    localButton->setObjectName(QStringLiteral("VisnipSettingsTranslationLocal"));
    localButton->setProperty("settingsSegment", true);
    localButton->setCheckable(true);
    localButton->setAutoDefault(false);
    localButton->setMinimumWidth(90);
    auto* cloudButton = new QPushButton(QStringLiteral("云端翻译"));
    cloudButton->setObjectName(QStringLiteral("VisnipSettingsTranslationCloud"));
    cloudButton->setProperty("settingsSegment", true);
    cloudButton->setCheckable(true);
    cloudButton->setAutoDefault(false);
    cloudButton->setMinimumWidth(90);
    auto* intranetButton = new QPushButton(QStringLiteral("内网服务器"));
    intranetButton->setObjectName(QStringLiteral("VisnipSettingsTranslationIntranet"));
    auto* offlineButton = new QPushButton(QStringLiteral("本机离线"));
    offlineButton->setObjectName(QStringLiteral("VisnipSettingsTranslationOffline"));
    for (auto* button : {intranetButton, offlineButton}) {
        button->setProperty("settingsSegment", true);
        button->setCheckable(true);
        button->setAutoDefault(false);
        button->setMinimumWidth(90);
    }
    segmentedLayout->addWidget(cloudButton);
    segmentedLayout->addWidget(offlineButton);
    auto* methodGroup = new QButtonGroup(segmented);
    methodGroup->setExclusive(true);
    methodGroup->addButton(localButton, static_cast<int>(TranslationMethod::LocalOcr));
    methodGroup->addButton(cloudButton, static_cast<int>(TranslationMethod::CloudBaidu));
    methodGroup->addButton(intranetButton, static_cast<int>(TranslationMethod::Intranet));
    methodGroup->addButton(offlineButton, static_cast<int>(TranslationMethod::Offline));
    auto* selectedMethod = methodGroup->button(static_cast<int>(settings.translationMethod));
    (selectedMethod ? selectedMethod : offlineButton)->setChecked(true);
    if (online) {
        layout->addWidget(segmented, 0, Qt::AlignLeft);
        layout->addSpacing(14);
    } else {
        segmented->setParent(page);
        segmented->hide();
    }

    auto* methodDetails = new CurrentPageStackedWidget;
    methodDetails->setObjectName(QStringLiteral("VisnipSettingsTranslationDetails"));

    auto* localPage = new QWidget;
    auto* localLayout = new QVBoxLayout(localPage);
    localLayout->setContentsMargins(0, 0, 0, 0);
    localLayout->setSpacing(0);
    localLayout->addWidget(modeSummary(
        QStringLiteral("旧版混合模式，不是离线翻译"),
        QStringLiteral("图片在本机识别和回填；识别出的文字仍发送到在线翻译服务。")));
    localLayout->addSpacing(14);

    auto* ocrPack = new StableComboBox;
    ocrPack->setObjectName(QStringLiteral("VisnipSettingsOcrPackCombo"));
    ocrPack->setMinimumWidth(245);
    for (const Ocr::LanguagePack& pack : Ocr::languagePacks()) {
        ocrPack->addItem(pack.displayName, pack.id);
    }
    const int ocrPackIndex = ocrPack->findData(settings.fastOcrPackId);
    ocrPack->setCurrentIndex(ocrPackIndex >= 0 ? ocrPackIndex : 0);
    localLayout->addWidget(controlRow(
        QStringLiteral("源文字识别模型"),
        QStringLiteral("按截图中的主要文字选择识别模型"),
        ocrPack));
    localLayout->addWidget(divider());

    auto* ocrPackStatus = hint(QString());
    ocrPackStatus->setObjectName(QStringLiteral("VisnipSettingsOcrPackStatus"));
    auto* ocrPackProgress = new QProgressBar;
    ocrPackProgress->setObjectName(QStringLiteral("VisnipSettingsOcrPackProgress"));
    ocrPackProgress->setRange(0, 1000);
    // The status line above already names the state; a percentage inside a 6 px
    // bar would be clipped, so this indicator stays textless.
    ocrPackProgress->setTextVisible(false);
    ocrPackProgress->hide();
    auto* downloadOcrPack = new QPushButton(QStringLiteral("下载模型"));
    downloadOcrPack->setObjectName(QStringLiteral("VisnipSettingsOcrPackDownloadButton"));
    downloadOcrPack->setAutoDefault(false);
    setSettingsRole(downloadOcrPack, QStringLiteral("primary"));
    auto* cancelOcrPack = new QPushButton(QStringLiteral("取消"));
    cancelOcrPack->setObjectName(QStringLiteral("VisnipSettingsOcrPackCancelButton"));
    cancelOcrPack->setAutoDefault(false);
    setSettingsRole(cancelOcrPack, QStringLiteral("secondary"));
    auto* deleteOcrPack = new QPushButton(QStringLiteral("删除"));
    deleteOcrPack->setObjectName(QStringLiteral("VisnipSettingsOcrPackDeleteButton"));
    deleteOcrPack->setAutoDefault(false);
    setSettingsRole(deleteOcrPack, QStringLiteral("danger"));
    auto* ocrPackActions = new QHBoxLayout;
    ocrPackActions->setContentsMargins(0, 0, 0, 0);
    ocrPackActions->setSpacing(8);
    ocrPackActions->addWidget(downloadOcrPack);
    ocrPackActions->addWidget(cancelOcrPack);
    ocrPackActions->addWidget(deleteOcrPack);
    ocrPackActions->addStretch();
    localLayout->addSpacing(10);
    localLayout->addWidget(ocrPackStatus);
    localLayout->addSpacing(6);
    localLayout->addWidget(ocrPackProgress);
    localLayout->addLayout(ocrPackActions);

    auto* advancedButton = new QToolButton;
    advancedButton->setObjectName(QStringLiteral("SettingsDisclosure"));
    advancedButton->setText(QStringLiteral("在线文字翻译高级选项"));
    advancedButton->setCheckable(true);
    advancedButton->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    advancedButton->setArrowType(Qt::RightArrow);
    advancedButton->setChecked(settings.fastProvider == QStringLiteral("baidu"));
    localLayout->addSpacing(12);
    localLayout->addWidget(advancedButton, 0, Qt::AlignLeft);

    auto* advancedPanel = new QWidget;
    advancedPanel->setObjectName(QStringLiteral("VisnipSettingsLocalAdvanced"));
    auto* advancedLayout = new QVBoxLayout(advancedPanel);
    advancedLayout->setContentsMargins(0, 4, 0, 0);
    advancedLayout->setSpacing(0);

    auto* fastProvider = new StableComboBox;
    fastProvider->setObjectName(QStringLiteral("VisnipSettingsTextProvider"));
    fastProvider->addItem(QStringLiteral("Visnip 文字翻译服务"),
                          QStringLiteral("official"));
    fastProvider->addItem(QStringLiteral("百度通用文本翻译直连"),
                          QStringLiteral("baidu"));
    const int providerIndex = fastProvider->findData(settings.fastProvider);
    fastProvider->setCurrentIndex(providerIndex >= 0 ? providerIndex : 0);
    fastProvider->setMinimumWidth(245);
    advancedLayout->addWidget(controlRow(
        QStringLiteral("文字翻译来源"),
        QStringLiteral("只影响本地 OCR 识别后的文字翻译阶段"),
        fastProvider));
    advancedLayout->addWidget(divider());

    auto* credentialsPanel = new QWidget;
    credentialsPanel->setObjectName(QStringLiteral("VisnipSettingsBaiduCredentials"));
    auto* credentialsLayout = new QVBoxLayout(credentialsPanel);
    credentialsLayout->setContentsMargins(0, 0, 0, 0);
    credentialsLayout->setSpacing(0);

    auto* baiduAppId = new QLineEdit(settings.baiduAppId);
    baiduAppId->setObjectName(QStringLiteral("VisnipSettingsBaiduAppId"));
    baiduAppId->setPlaceholderText(QStringLiteral("百度翻译开放平台 APP ID"));
    baiduAppId->setMinimumWidth(245);
    credentialsLayout->addWidget(controlRow(
        QStringLiteral("百度 APP ID"),
        QStringLiteral("仅用于本机直连百度通用文本翻译"),
        baiduAppId));
    credentialsLayout->addWidget(divider());

    auto* baiduSecret = new QLineEdit(settings.baiduSecretKey);
    baiduSecret->setObjectName(QStringLiteral("VisnipSettingsBaiduSecret"));
    baiduSecret->setEchoMode(QLineEdit::Password);
    baiduSecret->setPlaceholderText(QStringLiteral("百度开发者密钥"));
    auto* revealSecret = new QToolButton;
    revealSecret->setText(QStringLiteral("显示"));
    revealSecret->setCheckable(true);
    revealSecret->setAutoRaise(false);
    setSettingsRole(revealSecret, QStringLiteral("inline"));
    auto* secretControl = new QWidget;
    secretControl->setMinimumWidth(295);
    auto* secretLayout = new QHBoxLayout(secretControl);
    secretLayout->setContentsMargins(0, 0, 0, 0);
    secretLayout->setSpacing(8);
    secretLayout->addWidget(baiduSecret, 1);
    secretLayout->addWidget(revealSecret);
    credentialsLayout->addWidget(controlRow(
        QStringLiteral("百度开发者密钥"),
        QStringLiteral("保存在当前 Windows 用户配置中"),
        secretControl));
    advancedLayout->addWidget(credentialsPanel);
    localLayout->addWidget(advancedPanel);

    const auto updateProviderUi = [fastProvider, credentialsPanel, methodDetails]() {
        credentialsPanel->setVisible(
            fastProvider->currentData().toString() == QStringLiteral("baidu"));
        methodDetails->updateGeometry();
    };
    advancedPanel->setVisible(advancedButton->isChecked());
    updateProviderUi();
    connect(advancedButton, &QToolButton::toggled, this,
            [advancedButton, advancedPanel, methodDetails](bool expanded) {
        advancedButton->setArrowType(expanded ? Qt::DownArrow : Qt::RightArrow);
        advancedPanel->setVisible(expanded);
        methodDetails->updateGeometry();
    });
    if (advancedButton->isChecked()) {
        advancedButton->setArrowType(Qt::DownArrow);
    }
    connect(fastProvider, qOverload<int>(&QComboBox::currentIndexChanged), this,
            [this, fastProvider, updateProviderUi](int index) {
        if (index < 0) {
            return;
        }
        config_->mutableSettings().aiTranslate.fastProvider =
            fastProvider->itemData(index).toString();
        updateProviderUi();
        scheduleSave();
    });
    connect(baiduAppId, &QLineEdit::textChanged, this,
            [this](const QString& text) {
        config_->mutableSettings().aiTranslate.baiduAppId = text.trimmed();
        scheduleSave();
    });
    connect(baiduSecret, &QLineEdit::textChanged, this,
            [this](const QString& text) {
        config_->mutableSettings().aiTranslate.baiduSecretKey = text.trimmed();
        scheduleSave();
    });
    connect(baiduAppId, &QLineEdit::editingFinished,
            this, [this]() { flushPendingSave(); });
    connect(baiduSecret, &QLineEdit::editingFinished,
            this, [this]() { flushPendingSave(); });
    connect(revealSecret, &QToolButton::toggled, this,
            [baiduSecret, revealSecret](bool shown) {
        baiduSecret->setEchoMode(shown ? QLineEdit::Normal : QLineEdit::Password);
        revealSecret->setText(shown ? QStringLiteral("隐藏") : QStringLiteral("显示"));
    });

    auto* cloudPage = new QWidget;
    auto* cloudLayout = new QVBoxLayout(cloudPage);
    cloudLayout->setContentsMargins(0, 0, 0, 0);
    cloudLayout->addWidget(modeSummary(
        QStringLiteral("完整译图由云端返回"),
        QStringLiteral("仅上传选中的截图区域，适合复杂界面、网页和游戏画面。")));
    cloudLayout->addStretch();

    auto* intranetPage = new QWidget;
    auto* intranetLayout = new QVBoxLayout(intranetPage);
    intranetLayout->setContentsMargins(0, 0, 0, 0);
    intranetLayout->addWidget(modeSummary(
        QStringLiteral("由指定服务器返回完整译图"),
        QStringLiteral("高级选项：需要自行部署可用的整图翻译服务。官方资源下载地址不是模型推理地址；只填写地址不代表服务已就绪。不会回退到云端，也不自动跟随重定向。")));
    intranetLayout->addStretch();

    auto* offlinePage = new QWidget;
    auto* offlineLayout = new QVBoxLayout(offlinePage);
    offlineLayout->setContentsMargins(0, 0, 0, 0);
    offlineLayout->setSpacing(10);
    auto* offlineSummary = modeSummary(QString(), QString());
    const auto summaryLabels = offlineSummary->findChildren<QLabel*>();
    offlineLayout->addWidget(offlineSummary);
    auto* offlineQuality = new StableComboBox;
    offlineQuality->setObjectName(QStringLiteral("VisnipSettingsOfflineQuality"));
    offlineQuality->setMinimumWidth(200);
    offlineQuality->addItem(QStringLiteral("轻量（推荐）"), QStringLiteral("lite"));
    // Precise resources have no official upstream package, so they can no
    // longer be downloaded; an installation from an earlier version still runs.
    if (settings.offlineQuality == QStringLiteral("precise")
        && OfflineTranslationService::resourceProblem(settings.offlineResourceDirectory, QStringLiteral("precise")).isEmpty()) {
        offlineQuality->addItem(QStringLiteral("精细（已安装）"), QStringLiteral("precise"));
    }
    offlineQuality->setCurrentIndex(qMax(0, offlineQuality->findData(settings.offlineQuality)));
    auto* qualityRow = controlRow(QStringLiteral("处理档位"),
        QStringLiteral("轻量适合大多数电脑；精细额外擦除复杂背景，建议有独立显卡"), offlineQuality);
    qualityRow->setVisible(offlineQuality->count() > 1);
    offlineLayout->addWidget(qualityRow);
    const auto selectedQuality = [offlineQuality]() { return offlineQuality->currentData().toString(); };
    const auto updateOfflineSummary = [summaryLabels, selectedQuality]() {
        if (summaryLabels.size() < 2) return;
        const bool lite = selectedQuality() == QStringLiteral("lite");
        summaryLabels[0]->setText(lite ? QStringLiteral("本机离线翻译 · 轻量") : QStringLiteral("本机离线翻译 · 精细处理"));
        summaryLabels[1]->setText(lite
            ? QStringLiteral("当前支持中英文。本机识别文字后，由本地翻译模型按上下文翻译并原位回填；不需要 Python 或显卡。资源约 1.1 GiB，直接从官方渠道下载：llama.cpp 来自 GitHub，腾讯 Hy-MT2 翻译模型来自魔搭社区（ModelScope），不可用时改用 Hugging Face。模型就绪后连续截图无需重复加载，空闲 10 分钟后释放。翻译不上传截图或文字。")
            : QStringLiteral("当前支持中英文，文字翻译优先使用可用显卡。简单背景直接分析文字笔画，复杂区域才加载 Hi-SAM 和修复模型。资源约 6 GiB，模型就绪后连续截图无需重复加载，空闲 5 分钟后释放。首次显卡预热可能较慢。翻译不上传截图或文字。"));
    };
    updateOfflineSummary();
    auto* offlineStatus = hint(QString());
    offlineStatus->setObjectName(QStringLiteral("VisnipSettingsOfflineStatus"));
    offlineStatus->setWordWrap(true);
    offlineStatus->setTextInteractionFlags(Qt::TextSelectableByMouse); // e.g. the runtime download link
    offlineStatus->setMinimumWidth(0);
    offlineLayout->addWidget(offlineStatus);
    auto* phaseLabel = new QLabel;
    phaseLabel->setObjectName(QStringLiteral("VisnipSettingsOfflinePhase"));
    phaseLabel->setWordWrap(true);
    offlineLayout->addWidget(phaseLabel);
    auto* resourceProgress = new QProgressBar;
    resourceProgress->setObjectName(QStringLiteral("VisnipSettingsOfflineProgress"));
    resourceProgress->setRange(0, 1000);
    resourceProgress->setMinimumHeight(22);
    resourceProgress->hide();
    offlineLayout->addWidget(resourceProgress);
    auto* offlineActions = new QHBoxLayout;
    auto* downloadOffline = new QPushButton(QStringLiteral("下载并启用"));
    downloadOffline->setObjectName(QStringLiteral("VisnipSettingsOfflineDownload"));
    setSettingsRole(downloadOffline, QStringLiteral("primary"));
    auto* cancelOffline = new QPushButton(QStringLiteral("暂停 / 取消"));
    cancelOffline->setObjectName(QStringLiteral("VisnipSettingsOfflineCancel"));
    cancelOffline->setEnabled(false);
    auto* testOffline = new QPushButton(QStringLiteral("重新自检"));
    testOffline->setObjectName(QStringLiteral("VisnipSettingsOfflineSelfTest"));
    for (auto* button : {downloadOffline, cancelOffline, testOffline}) {
        button->setAutoDefault(false); offlineActions->addWidget(button);
    }
    offlineActions->addStretch();
    offlineLayout->addLayout(offlineActions);
    // Each installed file and the address it came from, so the download stays
    // auditable after the fact; deletion is offered right below it.
    auto* offlineFiles = new QLabel;
    offlineFiles->setObjectName(QStringLiteral("VisnipSettingsOfflineFiles"));
    offlineFiles->setTextFormat(Qt::RichText);
    offlineFiles->setWordWrap(true);
    offlineFiles->setTextInteractionFlags(Qt::TextSelectableByMouse | Qt::LinksAccessibleByMouse);
    offlineFiles->setOpenExternalLinks(true);
    offlineFiles->setMinimumWidth(0);
    offlineLayout->addWidget(offlineFiles);
    auto* deleteOffline = new QPushButton(QStringLiteral("删除已下载资源"));
    deleteOffline->setObjectName(QStringLiteral("VisnipSettingsOfflineDelete"));
    deleteOffline->setAutoDefault(false);
    setSettingsRole(deleteOffline, QStringLiteral("danger"));
    offlineLayout->addWidget(deleteOffline, 0, Qt::AlignLeft);
    deleteOffline->hide();
    auto* resources = offlineResources_;
    auto* localTest = new OfflineTranslationService(page);
    auto* liteTest = new LocalTextTranslationService(page);
    // The directory is app-managed; there is no user-visible path to edit.
    const auto offlineRootDirectory = [this]() {
        return config_->settings().aiTranslate.offlineResourceDirectory;
    };
    const auto describeOfflineFiles = [offlineRootDirectory]() {
        const QString root = offlineRootDirectory();
        QString html;
        for (const auto& file : OfflineResourceCatalog::liteFiles()) {
            const QString path = root.isEmpty() ? QString() : QDir(root).filePath(file.target);
            const bool installed = !path.isEmpty() && QFileInfo::exists(path);
            html += QStringLiteral("<p style=\"margin:0 0 10px 0\"><b>%1</b> · %2 · %3<br/>")
                .arg(file.label.toHtmlEscaped(), readableBytes(file.size),
                     installed ? QStringLiteral("已下载") : QStringLiteral("未下载"));
            if (installed) {
                html += QStringLiteral("本地文件：%1<br/>").arg(QDir::toNativeSeparators(path).toHtmlEscaped());
            }
            html += QStringLiteral("</p>");
        }
        return html;
    };
    const auto refreshOfflineFiles = [offlineFiles, deleteOffline, describeOfflineFiles, offlineRootDirectory,
                                      selectedQuality]() {
        offlineFiles->setVisible(selectedQuality() == QStringLiteral("lite"));
        offlineFiles->setText(describeOfflineFiles());
        const QString root = offlineRootDirectory();
        bool installed = false;
        if (!root.isEmpty()) {
            for (const auto& file : OfflineResourceCatalog::liteFiles()) {
                if (QFileInfo::exists(QDir(root).filePath(file.target))) { installed = true; break; }
            }
        }
        deleteOffline->setVisible(installed);
    };
    const auto refreshOffline = [offlineRootDirectory, offlineStatus, resources, testOffline, downloadOffline,
                                 selectedQuality, refreshOfflineFiles]() {
        refreshOfflineFiles();
        if (resources->isBusy()) { offlineStatus->setText(resources->statusText()); return; }
        const bool lite = selectedQuality() == QStringLiteral("lite");
        const QString problem = OfflineTranslationService::resourceProblem(offlineRootDirectory(), selectedQuality());
        const bool ready = problem.isEmpty();
        testOffline->setEnabled(ready);
        downloadOffline->setText(ready ? QStringLiteral("检查并启用") : QStringLiteral("下载并启用"));
        offlineStatus->setText(ready
            ? (lite ? QStringLiteral("轻量资源已找到。自检通过后自动启用，无需联网。")
                    : QStringLiteral("完整资源已找到。检查并自检通过后自动启用精细离线翻译；已有下载会复用。"))
            : (lite ? (problem.contains(QStringLiteral("Visual C++")) ? problem
                        : QStringLiteral("轻量离线资源尚未就绪。点击“下载并启用”，客户端将从官方渠道下载约 1.1 GiB 并逐个核对 SHA-256。"))
                    : QStringLiteral("精细离线资源不完整，而且没有官方发布渠道可以重新下载。请改用“轻量”，或继续使用已安装好的精细资源。")));
    };
    const auto busyControls = [this,downloadOffline,cancelOffline,testOffline,deleteOffline,methodGroup,offlineQuality,
                               selectedQuality,offlineRootDirectory](bool busy) {
        downloadOffline->setEnabled(!busy); cancelOffline->setEnabled(busy);
        testOffline->setEnabled(!busy && OfflineTranslationService::resourceProblem(offlineRootDirectory(),selectedQuality()).isEmpty());
        deleteOffline->setEnabled(!busy);
        offlineQuality->setEnabled(!busy);
        for (auto* button : methodGroup->buttons()) button->setEnabled(!busy);
        if (auto* reset=findChild<QPushButton*>(QStringLiteral("SettingsResetButton"))) reset->setEnabled(!busy);
    };
    const auto startSelfTest = [offlineRootDirectory,offlineStatus,phaseLabel,localTest,liteTest,busyControls,selectedQuality]() {
        busyControls(true);
        phaseLabel->setText(QStringLiteral("自检"));
        if (selectedQuality() == QStringLiteral("lite")) {
            liteTest->setProperty("testedDirectory",offlineRootDirectory());
            offlineStatus->setText(QStringLiteral("正在本机验证轻量翻译，成功后启用，不上传任何内容…"));
            LocalTextTranslationService::releaseSharedEngine(); // the test must run the model, not the cache
            liteTest->translate(LocalTextTranslationService::selfTestTexts(),QStringLiteral("zh-Hans"),offlineRootDirectory());
            return;
        }
        localTest->setProperty("testedDirectory",offlineRootDirectory());
        QImage input(800,260,QImage::Format_RGB32);input.fill(Qt::white);
        QPainter painter(&input);QFont font(QStringLiteral("Arial"));font.setPixelSize(26);painter.setFont(font);painter.setPen(Qt::black);
        painter.drawText(40,65,QStringLiteral("Project settings"));painter.drawText(40,125,QStringLiteral("Keep 12 files in the local folder."));painter.drawText(40,190,QStringLiteral("Save changes"));painter.end();
        offlineStatus->setText(QStringLiteral("正在本机验证精细处理，成功后启用，不上传截图…"));
        localTest->translate(input,QStringLiteral("zh-Hans"),offlineRootDirectory(),QStringLiteral("precise"));
    };
    connect(offlineQuality, qOverload<int>(&QComboBox::currentIndexChanged), page,
            [updateOfflineSummary,refreshOffline,busyControls,resources]() {
        updateOfflineSummary(); refreshOffline(); busyControls(resources->isBusy());
    });
    connect(downloadOffline, &QPushButton::clicked, page, [resources,offlineRootDirectory,selectedQuality,startSelfTest]() {
        // Installed resources need no download: test and enable.
        if (OfflineTranslationService::resourceProblem(offlineRootDirectory(), selectedQuality()).isEmpty()) {
            startSelfTest();
            return;
        }
        resources->prepare(selectedQuality());
    });
    connect(cancelOffline, &QPushButton::clicked, page, [resources,localTest,liteTest]() {
        resources->cancel(); localTest->cancel(); liteTest->cancel();
    });
    connect(resources, &OfflineResourceService::statusChanged, page, [offlineStatus](const QString& text) { offlineStatus->setText(text); });
    connect(resources, &OfflineResourceService::phaseChanged, page, [phaseLabel,resourceProgress,resources](const QString& phase) {
        resourceProgress->setVisible(phase == QStringLiteral("download"));
        phaseLabel->setText(phase == QStringLiteral("download") ? QStringLiteral("下载：以下进度来自实际接收的资源字节（含已下载缓存）")
            : phase == QStringLiteral("verify") ? QStringLiteral("校验：正在核对文件 SHA-256，不代表仍在下载")
            : phase == QStringLiteral("install") ? QStringLiteral("安装：正在解压 llama.cpp，请稍候")
            : phase == QStringLiteral("selftest") ? (resources->targetQuality() == QStringLiteral("lite")
                ? QStringLiteral("自检：正在本机验证翻译模型")
                : QStringLiteral("自检：正在本机验证识别、翻译、分割和回填")) : QString());
    });
    connect(resources, &OfflineResourceService::progress, page, [resourceProgress](qint64 received, qint64 total) {
        if (total <= 0) return; // Never present a moving busy animation as download progress.
        resourceProgress->setRange(0,1000);
        resourceProgress->setValue(int(qBound<qint64>(0LL, received*1000/total, 1000LL)));
        resourceProgress->setFormat(QStringLiteral("%1 / %2 MiB · %p%")
            .arg(received/1048576.0,0,'f',1).arg(total/1048576.0,0,'f',1));
    });
    connect(resources, &OfflineResourceService::busyChanged, page, busyControls);
    connect(resources, &OfflineResourceService::approvalRequired, page, [this,resources](qint64 bytes,qint64 disk) {
        const auto answer=QMessageBox::question(this,QStringLiteral("准备轻量离线资源"),
            QStringLiteral("还需下载 %1 GiB，至少预留 %2 GiB 磁盘空间。不需要手动安装。\n\n"
                           "文件直接来自官方发布渠道，Visnip 的服务器不参与：\n"
                           "· llama.cpp b10964（GitHub，ggml-org/llama.cpp）\n"
                           "· 腾讯 Hy-MT2-1.8B 翻译模型（魔搭社区 ModelScope，不可用时改用 Hugging Face）\n"
                           "下载时这些网站会收到你的 IP 等常规访问信息，不会上传截图或文字。\n\n"
                           "下载后逐个核对 SHA-256，再自检，通过后启用；失败不会改为联网翻译。轻量翻译运行时约占用 2 GB 内存。")
            .arg(bytes/double(1024LL*1024*1024),0,'f',2).arg(disk/double(1024LL*1024*1024),0,'f',1),
            QMessageBox::Yes|QMessageBox::Cancel,QMessageBox::Cancel);
        if (answer==QMessageBox::Yes) resources->installApproved(); else resources->cancel();
    });
    const auto activateOffline = [this,offlineStatus,phaseLabel,resourceProgress,methodGroup,methodDetails,offlineQuality,refreshOfflineFiles](const QString& root,const QString& quality,qint64 ms) {
        const auto previous=config_->settings().aiTranslate;
        auto& current=config_->mutableSettings().aiTranslate;
        current.offlineResourceDirectory=root; current.offlineQuality=quality;
        current.translationMethod=TranslationMethod::Offline;
        if (current.targetLanguage!=QStringLiteral("en") && current.targetLanguage!=QStringLiteral("zh-Hans")) current.targetLanguage=QStringLiteral("zh-Hans");
        scheduleSave();
        if (!flushPendingSave()) { config_->mutableSettings().aiTranslate=previous; offlineStatus->setText(QStringLiteral("自检通过，但无法保存配置，未启用。")); return; }
        refreshOfflineFiles();
        {
            const QSignalBlocker blocker(offlineQuality);
            offlineQuality->setCurrentIndex(qMax(0, offlineQuality->findData(quality)));
        }
        methodGroup->button(static_cast<int>(TranslationMethod::Offline))->setChecked(true);
        methodDetails->setCurrentIndex(static_cast<int>(TranslationMethod::Offline));
        if (auto* target=findChild<QComboBox*>(QStringLiteral("VisnipSettingsTargetLanguage"))) {
            const QSignalBlocker blocker(target);target->setCurrentIndex(target->findData(current.targetLanguage));
        }
        if (auto* remote=findChild<QWidget*>(QStringLiteral("VisnipSettingsRemotePanel"))) remote->hide();
        resourceProgress->hide(); phaseLabel->clear();
        offlineStatus->setText(QStringLiteral("已启用%1离线翻译，自检 %2 秒。资源文件位于 %3。现在可直接截图翻译，图片和文字不上传。")
            .arg(quality==QStringLiteral("lite") ? QStringLiteral("轻量") : QStringLiteral("精细"))
            .arg(ms/1000.0,0,'f',1).arg(QDir::toNativeSeparators(root)));
    };
    connect(resources, &OfflineResourceService::succeeded, page, [activateOffline](const QString& root,const QString& quality,qint64 ms) {
        if (quality==QStringLiteral("precise") || quality==QStringLiteral("lite")) activateOffline(root,quality,ms);
    });
    connect(resources, &OfflineResourceService::failed, page, [resourceProgress,phaseLabel](const QString&) {resourceProgress->hide();phaseLabel->clear();});
    connect(resources, &OfflineResourceService::cancelled, page, [resourceProgress,phaseLabel]() {resourceProgress->hide();phaseLabel->clear();});
    connect(deleteOffline, &QPushButton::clicked, page, [this,offlineRootDirectory,refreshOffline,offlineStatus]() {
        const QString root=offlineRootDirectory();
        qint64 planned=0;
        for (const auto& file : OfflineResourceCatalog::liteFiles()) planned += file.size;
        const auto answer=QMessageBox::question(this,QStringLiteral("删除已下载的离线资源"),
            QStringLiteral("将删除已下载的 llama.cpp 运行文件、Hy-MT2 翻译模型和下载缓存（约 %1），删除后需要重新下载才能使用离线翻译；当前配置不会改为联网翻译。是否继续？")
                .arg(readableBytes(planned)),
            QMessageBox::Yes|QMessageBox::Cancel,QMessageBox::Cancel);
        if (answer!=QMessageBox::Yes) return;
        QString error;
        if (!OfflineResourceService::removeInstalled(root,&error)) {
            offlineStatus->setText(QStringLiteral("删除未完成：%1").arg(error)); return;
        }
        if (config_->settings().aiTranslate.offlineResourceDirectory==root) {
            config_->mutableSettings().aiTranslate.offlineResourceDirectory.clear();
            scheduleSave(); flushPendingSave();
        }
        refreshOffline();
        offlineStatus->setText(QStringLiteral("已删除本机下载的离线资源；需要时可再次点击“下载并启用”。"));
    });
    connect(testOffline, &QPushButton::clicked, page, startSelfTest);
    connect(localTest,&OfflineTranslationService::succeeded,page,[localTest,busyControls,activateOffline,offlineStatus](const ImageTranslationResult& result,qint64 ms){
        busyControls(false);
        if (result.blockCount<3 || !result.notice.isEmpty()) {offlineStatus->setText(QStringLiteral("自检未完全通过，未启用。%1").arg(result.notice));return;}
        activateOffline(localTest->property("testedDirectory").toString(),QStringLiteral("precise"),ms);
    });
    connect(localTest,&OfflineTranslationService::phaseChanged,page,[phaseLabel](const QString& text){phaseLabel->setText(text);});
    connect(localTest,&OfflineTranslationService::failed,page,[busyControls,offlineStatus,phaseLabel](const QString& text){busyControls(false);offlineStatus->setText(text);phaseLabel->clear();});
    connect(localTest,&OfflineTranslationService::cancelled,page,[busyControls,offlineStatus,phaseLabel](){busyControls(false);offlineStatus->setText(QStringLiteral("自检已取消，原有配置未改变。"));phaseLabel->clear();});
    connect(liteTest,&LocalTextTranslationService::succeeded,page,[liteTest,busyControls,activateOffline,offlineStatus](const QStringList& translations,qint64 ms){
        busyControls(false);
        const QString problem = liteTest->selfTestProblem(translations);
        if (!problem.isEmpty()) {offlineStatus->setText(QStringLiteral("自检未完全通过，未启用：%1。").arg(problem));return;}
        activateOffline(liteTest->property("testedDirectory").toString(),QStringLiteral("lite"),ms);
    });
    connect(liteTest,&LocalTextTranslationService::phaseChanged,page,[phaseLabel](const QString& text){phaseLabel->setText(text);});
    connect(liteTest,&LocalTextTranslationService::failed,page,[busyControls,offlineStatus,phaseLabel](const QString& text){busyControls(false);offlineStatus->setText(text);phaseLabel->clear();});
    connect(liteTest,&LocalTextTranslationService::cancelled,page,[busyControls,offlineStatus,phaseLabel](){busyControls(false);offlineStatus->setText(QStringLiteral("自检已取消，原有配置未改变。"));phaseLabel->clear();});
    refreshOffline();busyControls(resources->isBusy());

    methodDetails->addWidget(localPage);
    methodDetails->addWidget(cloudPage);
    methodDetails->addWidget(intranetPage);
    methodDetails->addWidget(offlinePage);
    methodDetails->setCurrentIndex(selectedMethod
        ? static_cast<int>(settings.translationMethod)
        : static_cast<int>(TranslationMethod::Offline));
    layout->addWidget(methodDetails);

    OcrPackDownloadService* packDownloads = ocrPackDownloads_;
    if (!packDownloads) {
        packDownloads = new OcrPackDownloadService(this);
        ocrPackDownloads_ = packDownloads;
    }
    const auto refreshOcrPackUi = [ocrPack, ocrPackStatus, ocrPackProgress,
                                   downloadOcrPack, cancelOcrPack,
                                   deleteOcrPack, packDownloads]() {
        const QString packId = ocrPack->currentData().toString();
        const Ocr::LanguagePack* pack = Ocr::languagePack(packId);
        if (!pack) {
            ocrPackStatus->setText(QStringLiteral("未知 OCR 语言包"));
            return;
        }
        const bool downloading = packDownloads->isBusy();
        const bool thisDownload = downloading
            && packDownloads->activePackId() == packId;
        const bool installed = Ocr::languagePackInstalled(packId);
        const QString state = thisDownload
            ? QStringLiteral("正在下载...")
            : installed
                ? (pack->bundled ? QStringLiteral("内置，已安装")
                                 : QStringLiteral("已安装"))
                : QStringLiteral("未安装，下载后才会成为当前模型");
        ocrPackStatus->setText(QStringLiteral("%1  %2（约 %3 MB）")
                                   .arg(state, pack->description)
                                   .arg((pack->modelBytes + 500000) / 1000000));
        ocrPackProgress->setVisible(thisDownload);
        downloadOcrPack->setVisible(!installed);
        downloadOcrPack->setEnabled(!downloading);
        cancelOcrPack->setVisible(thisDownload);
        cancelOcrPack->setEnabled(thisDownload);
        deleteOcrPack->setVisible(installed && !pack->bundled);
        deleteOcrPack->setEnabled(!downloading && installed && !pack->bundled);
    };
    connect(ocrPack, qOverload<int>(&QComboBox::currentIndexChanged), this,
            [this, ocrPack, refreshOcrPackUi](int index) {
        if (index < 0) {
            return;
        }
        const QString packId = ocrPack->itemData(index).toString();
        if (Ocr::languagePackInstalled(packId)) {
            config_->mutableSettings().aiTranslate.fastOcrPackId = packId;
            scheduleSave();
        }
        refreshOcrPackUi();
    });
    connect(downloadOcrPack, &QPushButton::clicked, this,
            [ocrPack, packDownloads]() {
        packDownloads->install(ocrPack->currentData().toString());
    });
    connect(cancelOcrPack, &QPushButton::clicked,
            packDownloads, &OcrPackDownloadService::cancel);
    connect(deleteOcrPack, &QPushButton::clicked, this,
            [this, ocrPack, packDownloads, refreshOcrPackUi]() {
        const auto answer = QMessageBox::question(
            this,
            QStringLiteral("删除 OCR 模型"),
            QStringLiteral("删除后需要重新下载才能使用。是否继续？"),
            QMessageBox::Yes | QMessageBox::Cancel,
            QMessageBox::Cancel);
        if (answer != QMessageBox::Yes) {
            return;
        }
        QString error;
        if (!packDownloads->remove(ocrPack->currentData().toString(), &error)) {
            return;
        }
        const int defaultIndex = ocrPack->findData(Ocr::defaultLanguagePackId());
        ocrPack->setCurrentIndex(defaultIndex >= 0 ? defaultIndex : 0);
        refreshOcrPackUi();
    });
    connect(packDownloads, &OcrPackDownloadService::progress, this,
            [ocrPackProgress, packDownloads](const QString& packId,
                                             qint64 received, qint64 total) {
        if (packId != packDownloads->activePackId()) {
            return;
        }
        if (total <= 0) {
            ocrPackProgress->setRange(0, 0);
        } else {
            ocrPackProgress->setRange(0, 1000);
            ocrPackProgress->setValue(static_cast<int>(
                qBound<qint64>(0LL, received * 1000 / total, 1000LL)));
        }
    });
    connect(packDownloads, &OcrPackDownloadService::succeeded, this,
            [this, ocrPack, refreshOcrPackUi](const QString& packId) {
        if (ocrPack->currentData().toString() == packId) {
            config_->mutableSettings().aiTranslate.fastOcrPackId = packId;
            scheduleSave();
        }
        refreshOcrPackUi();
    });
    connect(packDownloads, &OcrPackDownloadService::cancelled, this,
            [refreshOcrPackUi](const QString&) { refreshOcrPackUi(); });
    connect(packDownloads, &OcrPackDownloadService::failed, this,
            [ocrPackStatus, refreshOcrPackUi](const QString&,
                                             const QString& message) {
        refreshOcrPackUi();
        ocrPackStatus->setText(message);
    });
    refreshOcrPackUi();

    connect(methodGroup, &QButtonGroup::idClicked, this,
            [this, methodDetails, packDownloads, methodGroup](int methodId) {
        const auto method = static_cast<TranslationMethod>(methodId);
        auto& current = config_->mutableSettings().aiTranslate;
        AiTranslateSettings candidate = current;
        candidate.translationMethod = method;
        if (!candidate.uploadConsented()) {
            // Keep the previous mode until the upload notice is accepted.
            const auto previous = static_cast<int>(current.translationMethod);
            if (auto* button = methodGroup->button(previous)) {
                button->setChecked(true);
            }
            methodDetails->setCurrentIndex(previous);
            auto* notice = uploadNotice(this, candidate);
            connect(notice, &QMessageBox::finished, this, [this, notice, methodGroup, method, methodId](int) {
                if (notice->buttonRole(notice->clickedButton()) != QMessageBox::AcceptRole) {
                    return;
                }
                recordUploadConsent(config_->mutableSettings().aiTranslate, method);
                if (auto* button = methodGroup->button(methodId)) {
                    button->click(); // consent recorded: applies the mode normally
                }
            });
            notice->open();
            return;
        }
        current.translationMethod = method;
        methodDetails->setCurrentIndex(methodId);
        if (method == TranslationMethod::Offline) {
            packDownloads->cancel();
        }
        scheduleSave();
        flushPendingSave(); // Apply the network boundary immediately on a mode change.
    });

    addSectionTitle(layout, QStringLiteral("翻译结果"));
    auto* language = new StableComboBox;
    language->setObjectName(QStringLiteral("VisnipSettingsTargetLanguage"));
    language->setMinimumWidth(220);
    for (const TranslationLanguage& entry : supportedTranslationLanguages()) {
        // Offline translation is qualified for Chinese and English only.
        if (online || entry.code == QStringLiteral("zh-Hans") || entry.code == QStringLiteral("en")) {
            language->addItem(entry.nativeName, entry.code);
        }
    }
    language->setCurrentIndex(qMax(0, language->findData(settings.targetLanguage)));
    connect(language, qOverload<int>(&QComboBox::currentIndexChanged), this,
            [this, language](int index) {
        if (index < 0) {
            return;
        }
        config_->mutableSettings().aiTranslate.targetLanguage =
            language->itemData(index).toString();
        scheduleSave();
    });
    addControlRow(layout,
                  QStringLiteral("默认目标语言"),
                  QStringLiteral("每次新翻译默认使用的输出语言"),
                  language,
                  false);

    auto* remotePanel = new QWidget;
    remotePanel->setObjectName(QStringLiteral("VisnipSettingsRemotePanel"));
    auto* remoteLayout = new QVBoxLayout(remotePanel);
    remoteLayout->setContentsMargins(0,0,0,0);
    layout->addWidget(remotePanel);
    addSectionTitle(remoteLayout, QStringLiteral("联网服务设置"));
    auto* advancedMethods = new QToolButton;
    advancedMethods->setText(QStringLiteral("其他联网服务（内网 / 旧版混合）"));
    advancedMethods->setObjectName(QStringLiteral("VisnipSettingsAdvancedMethods"));
    advancedMethods->setCheckable(true); advancedMethods->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    auto* extraMethods = new QWidget;
    auto* extraLayout = new QHBoxLayout(extraMethods); extraLayout->setContentsMargins(0,0,0,0);
    extraLayout->addWidget(intranetButton); extraLayout->addWidget(localButton); extraLayout->addStretch();
    remoteLayout->addWidget(advancedMethods,0,Qt::AlignLeft);remoteLayout->addWidget(extraMethods);
    const bool legacy = settings.translationMethod==TranslationMethod::Intranet || settings.translationMethod==TranslationMethod::LocalOcr;
    advancedMethods->setChecked(legacy);extraMethods->setVisible(legacy);
    advancedMethods->setArrowType(legacy ? Qt::DownArrow : Qt::RightArrow);
    connect(advancedMethods,&QToolButton::toggled,page,[extraMethods,advancedMethods](bool shown){extraMethods->setVisible(shown);advancedMethods->setArrowType(shown ? Qt::DownArrow : Qt::RightArrow);});
    auto* consentRow = new QWidget;
    consentRow->setObjectName(QStringLiteral("VisnipSettingsUploadConsent"));
    auto* consentLayout = new QHBoxLayout(consentRow);
    consentLayout->setContentsMargins(0, 0, 0, 0);
    auto* consentText = hint(QStringLiteral("尚未确认上传说明：确认前，联网翻译不会发送任何内容。"));
    auto* consentButton = new QPushButton(QStringLiteral("查看并确认"));
    consentButton->setObjectName(QStringLiteral("VisnipSettingsUploadConsentButton"));
    consentButton->setAutoDefault(false);
    consentLayout->addWidget(consentText, 1);
    consentLayout->addWidget(consentButton);
    remoteLayout->addWidget(consentRow);
    connect(consentButton, &QPushButton::clicked, this, [this, consentRow]() {
        const AiTranslateSettings current = config_->settings().aiTranslate;
        auto* notice = uploadNotice(this, current);
        connect(notice, &QMessageBox::finished, this, [this, notice, consentRow, method = current.translationMethod](int) {
            if (notice->buttonRole(notice->clickedButton()) != QMessageBox::AcceptRole) {
                return;
            }
            recordUploadConsent(config_->mutableSettings().aiTranslate, method);
            scheduleSave();
            flushPendingSave();
            consentRow->setVisible(!config_->settings().aiTranslate.uploadConsented());
        });
        notice->open();
    });

    auto* serviceUrl = new QLineEdit(settings.fastServiceUrl);
    serviceUrl->setObjectName(QStringLiteral("VisnipSettingsServiceUrl"));
    serviceUrl->setPlaceholderText(aiTranslateDefaultFastServiceUrl());
    serviceUrl->setMinimumWidth(210);
    auto* testService = new QPushButton(QStringLiteral("测试连接"));
    testService->setObjectName(QStringLiteral("VisnipSettingsTestService"));
    testService->setAutoDefault(false);
    setSettingsRole(testService, QStringLiteral("primary"));
    auto* serviceControl = new QWidget;
    serviceControl->setMinimumWidth(330);
    auto* serviceControlLayout = new QHBoxLayout(serviceControl);
    serviceControlLayout->setContentsMargins(0, 0, 0, 0);
    serviceControlLayout->setSpacing(8);
    serviceControlLayout->addWidget(serviceUrl, 1);
    serviceControlLayout->addWidget(testService);
    addControlRow(remoteLayout,
                  QStringLiteral("当前远程服务地址"),
                  aiTranslateDefaultFastServiceUrl().isEmpty()
                      ? QStringLiteral("由服务提供方提供，必须填写；离线不联网")
                      : QStringLiteral("云端留空使用默认值；内网必须填写；离线不联网"),
                  serviceControl);

    auto* serviceToken = new QLineEdit;
    serviceToken->setObjectName(QStringLiteral("VisnipSettingsServiceToken"));
    serviceToken->setEchoMode(QLineEdit::Password);
    serviceToken->setMinimumWidth(210);
    auto* revealToken = new QToolButton;
    revealToken->setText(QStringLiteral("显示"));
    revealToken->setCheckable(true);
    revealToken->setAutoRaise(false);
    setSettingsRole(revealToken, QStringLiteral("inline"));
    auto* tokenControl = new QWidget;
    tokenControl->setMinimumWidth(330);
    auto* tokenLayout = new QHBoxLayout(tokenControl);
    tokenLayout->setContentsMargins(0, 0, 0, 0);
    tokenLayout->setSpacing(8);
    tokenLayout->addWidget(serviceToken, 1);
    tokenLayout->addWidget(revealToken);
    addControlRow(remoteLayout,
                  QStringLiteral("API 令牌"),
                  QStringLiteral("由服务提供方分配，以 Bearer 方式随翻译请求发送；云端只通过 HTTPS 发送，按当前 Windows 用户加密保存"),
                  tokenControl);
    connect(serviceToken, &QLineEdit::textChanged, this, [this](const QString& text) {
        auto& current = config_->mutableSettings().aiTranslate;
        if (!current.allowsTranslationNetwork()) {
            return;
        }
        // Each receiver keeps its own token; switching modes never moves it.
        (current.translationMethod == TranslationMethod::Intranet
             ? current.intranetServiceToken : current.cloudServiceToken) = text.trimmed();
        scheduleSave();
    });
    connect(serviceToken, &QLineEdit::editingFinished,
            this, [this]() { flushPendingSave(); });
    connect(revealToken, &QToolButton::toggled, this, [serviceToken, revealToken](bool shown) {
        serviceToken->setEchoMode(shown ? QLineEdit::Normal : QLineEdit::Password);
        revealToken->setText(shown ? QStringLiteral("隐藏") : QStringLiteral("显示"));
    });

    auto* serviceStatus = hint(QString());
    serviceStatus->setObjectName(QStringLiteral("SettingsInlineSuccess"));
    serviceStatus->hide();
    remoteLayout->addWidget(serviceStatus);
    connect(serviceUrl, &QLineEdit::textChanged, this, [this](const QString& text) {
        auto& current = config_->mutableSettings().aiTranslate;
        if (!current.allowsTranslationNetwork()) {
            return;
        }
        if (current.translationMethod == TranslationMethod::Intranet) {
            current.intranetServiceUrl = text.trimmed();
        } else {
            current.fastServiceUrl = text.trimmed();
        }
        scheduleSave();
    });
    connect(serviceUrl, &QLineEdit::editingFinished,
            this, [this]() { flushPendingSave(); });

    auto* probe = new TextTranslationService(config_, page);
    connect(probe, &TextTranslationService::healthChecked, this,
            [this, testService, serviceStatus](bool ok, const QString& message) {
        testService->setEnabled(config_->settings().aiTranslate.allowsTranslationNetwork());
        serviceStatus->setObjectName(ok ? QStringLiteral("SettingsInlineSuccess")
                                        : QStringLiteral("SettingsInlineError"));
        serviceStatus->setText(message);
        serviceStatus->style()->unpolish(serviceStatus);
        serviceStatus->style()->polish(serviceStatus);
        serviceStatus->show();
    });
    connect(testService, &QPushButton::clicked, this,
            [this, probe, serviceUrl, testService, serviceStatus]() {
        flushPendingSave();
        testService->setEnabled(false);
        serviceStatus->setText(QStringLiteral("正在测试连接..."));
        serviceStatus->show();
        probe->checkHealth(serviceUrl->text());
    });

    auto* timeout = new QSpinBox;
    timeout->setObjectName(QStringLiteral("VisnipSettingsRequestTimeout"));
    timeout->setRange(30, 300);
    timeout->setSuffix(QStringLiteral(" 秒"));
    timeout->setValue(settings.timeoutSeconds);
    timeout->setFixedWidth(104);
    connect(timeout, qOverload<int>(&QSpinBox::valueChanged),
            this, [this](int value) {
        config_->mutableSettings().aiTranslate.timeoutSeconds = value;
        scheduleSave();
    });
    addControlRow(remoteLayout,
                  QStringLiteral("单次网络请求超时"),
                  QStringLiteral("只影响之后发起的文字或图片翻译请求"),
                  timeout,
                  false);

    const auto updateConnectionUi = [this, serviceUrl, serviceControl, serviceToken, tokenControl, serviceStatus, testService, timeout, remotePanel, consentRow]() {
        const auto& current = config_->settings().aiTranslate;
        const bool network = current.allowsTranslationNetwork();
        consentRow->setVisible(network && !current.uploadConsented());
        const bool intranet = current.translationMethod == TranslationMethod::Intranet;
        const QSignalBlocker blocker(serviceUrl);
        serviceUrl->setText(!network ? QString()
                                    : intranet ? current.intranetServiceUrl : current.fastServiceUrl);
        {
            const QSignalBlocker tokenBlocker(serviceToken);
            serviceToken->setText(!network ? QString()
                                           : intranet ? current.intranetServiceToken : current.cloudServiceToken);
            serviceToken->setPlaceholderText(intranet ? QStringLiteral("内网服务的访问令牌；留空则不发送")
                                                      : QStringLiteral("云端服务的访问令牌；留空则不发送"));
        }
        tokenControl->setEnabled(network);
        serviceUrl->setPlaceholderText(intranet ? QStringLiteral("由管理员提供，例如 https://translate.corp/visnip")
                                               : !network ? QString()
                                               : aiTranslateDefaultFastServiceUrl().isEmpty()
                                                   ? QStringLiteral("由服务提供方提供，例如 https://translate.example.com")
                                                   : aiTranslateDefaultFastServiceUrl());
        remotePanel->setVisible(network && onlineTranslationEnabled());
        serviceControl->setEnabled(network);
        testService->setEnabled(network);
        timeout->setEnabled(network);
        serviceStatus->hide();
    };
    connect(methodGroup, &QButtonGroup::idClicked, this,
            [updateConnectionUi](int) { updateConnectionUi(); });
    updateConnectionUi();

    layout->addStretch();
    return page;
}

QWidget* SettingsDialog::createQuestionPage()
{
    const auto& settings = config_->settings().question;
    QVBoxLayout* layout = nullptr;
    auto* page = makePage(
        QStringLiteral("解题"),
        QStringLiteral("截图工具栏的「题」会把选区交给侧边的解题面板；点击「获取答案」时截取选区当前的画面，发送给这里配置的多模态大模型。"),
        layout,
        QStringLiteral("VisnipQuestionScrollArea"));

    addSectionTitle(layout, QStringLiteral("模型接口"));
    auto* format = new StableComboBox;
    format->setObjectName(QStringLiteral("VisnipSettingsQuestionFormat"));
    format->addItem(QStringLiteral("OpenAI 兼容"), questionApiFormatId(QuestionApiFormat::OpenAiCompatible));
    format->addItem(QStringLiteral("Anthropic（Claude）"), questionApiFormatId(QuestionApiFormat::Anthropic));
    format->setCurrentIndex(settings.apiFormat == QuestionApiFormat::Anthropic ? 1 : 0);
    format->setMinimumWidth(245);
    addControlRow(layout,
                  QStringLiteral("接口格式"),
                  QStringLiteral("通义千问、豆包、智谱、Kimi、Ollama 等大多兼容 OpenAI 格式"),
                  format);

    auto* apiUrl = new QLineEdit(settings.apiUrl);
    apiUrl->setObjectName(QStringLiteral("VisnipSettingsQuestionUrl"));
    apiUrl->setMinimumWidth(295);
    addControlRow(layout,
                  QStringLiteral("接口地址"),
                  QStringLiteral("基础地址或完整接口地址均可；留空使用官方地址"),
                  apiUrl);

    auto* apiKey = new QLineEdit(settings.apiKey);
    apiKey->setObjectName(QStringLiteral("VisnipSettingsQuestionKey"));
    apiKey->setEchoMode(QLineEdit::Password);
    apiKey->setPlaceholderText(QStringLiteral("API Key"));
    auto* revealKey = new QToolButton;
    revealKey->setText(QStringLiteral("显示"));
    revealKey->setCheckable(true);
    revealKey->setAutoRaise(false);
    setSettingsRole(revealKey, QStringLiteral("inline"));
    auto* keyControl = new QWidget;
    keyControl->setMinimumWidth(295);
    auto* keyLayout = new QHBoxLayout(keyControl);
    keyLayout->setContentsMargins(0, 0, 0, 0);
    keyLayout->setSpacing(8);
    keyLayout->addWidget(apiKey, 1);
    keyLayout->addWidget(revealKey);
    addControlRow(layout,
                  QStringLiteral("API Key"),
                  QStringLiteral("加密保存在当前 Windows 用户配置中"),
                  keyControl);

    auto* model = new QLineEdit(settings.model);
    model->setObjectName(QStringLiteral("VisnipSettingsQuestionModel"));
    model->setMinimumWidth(295);
    addControlRow(layout,
                  QStringLiteral("模型"),
                  QStringLiteral("需要支持图片输入的多模态模型"),
                  model);

    auto* testButton = new QPushButton(QStringLiteral("测试连接"));
    testButton->setObjectName(QStringLiteral("VisnipSettingsQuestionTest"));
    testButton->setAutoDefault(false);
    setSettingsRole(testButton, QStringLiteral("primary"));
    addControlRow(layout,
                  QStringLiteral("测试连接"),
                  QStringLiteral("发送一张很小的测试图片，检查地址、Key、模型和图片支持"),
                  testButton,
                  false);
    auto* testStatus = hint(QString());
    testStatus->setObjectName(QStringLiteral("SettingsInlineSuccess"));
    testStatus->hide();
    layout->addWidget(testStatus);

    addSectionTitle(layout, QStringLiteral("提示词"));
    auto* scope = new QLineEdit(settings.subjectScope);
    scope->setObjectName(QStringLiteral("VisnipSettingsQuestionScope"));
    scope->setPlaceholderText(QStringLiteral("例如：高中数学、大学物理、电路分析"));
    scope->setMinimumWidth(295);
    addControlRow(layout,
                  QStringLiteral("专业范围"),
                  QStringLiteral("追加到提示词中，让模型按对应领域的知识作答"),
                  scope);

    auto* resetPrompt = new QPushButton(QStringLiteral("恢复默认"));
    resetPrompt->setObjectName(QStringLiteral("VisnipSettingsQuestionPromptReset"));
    resetPrompt->setAutoDefault(false);
    setSettingsRole(resetPrompt, QStringLiteral("secondary"));
    addControlRow(layout,
                  QStringLiteral("自定义提示词"),
                  QStringLiteral("留空时使用下方灰色显示的默认提示词；填写后替换默认提示词"),
                  resetPrompt,
                  false);
    auto* prompt = new QPlainTextEdit(settings.customPrompt);
    prompt->setObjectName(QStringLiteral("VisnipSettingsQuestionPrompt"));
    prompt->setPlaceholderText(Question::defaultPrompt());
    prompt->setMinimumHeight(170);
    prompt->setStyleSheet(QStringLiteral(
        "QPlainTextEdit { border: 1px solid #CBD5E1; border-radius: 6px; padding: 4px 6px; background: #FFFFFF; }"
        "QPlainTextEdit:focus { border: 1px solid #4F7CFF; }"));
    layout->addWidget(prompt);

    addSectionTitle(layout, QStringLiteral("网络"));
    auto* timeout = new QSpinBox;
    timeout->setObjectName(QStringLiteral("VisnipSettingsQuestionTimeout"));
    timeout->setRange(30, 600);
    timeout->setSuffix(QStringLiteral(" 秒"));
    timeout->setValue(settings.timeoutSeconds);
    timeout->setFixedWidth(104);
    addControlRow(layout,
                  QStringLiteral("无响应超时"),
                  QStringLiteral("超过这段时间没有收到任何数据就停止请求"),
                  timeout);
    auto* proxy = makeSwitch(settings.useSystemProxy, QStringLiteral("跟随系统代理"));
    proxy->setObjectName(QStringLiteral("VisnipSettingsQuestionProxy"));
    addControlRow(layout,
                  QStringLiteral("跟随系统代理"),
                  QStringLiteral("只作用于解题请求；访问海外接口通常需要开启"),
                  proxy,
                  false);
    layout->addSpacing(10);
    layout->addWidget(hint(QStringLiteral("「获取答案」的快捷键默认 F4，可在「快捷键」页修改，只在解题面板打开时生效。")));

    const auto updatePlaceholders = [apiUrl, model](QuestionApiFormat current) {
        const bool anthropic = current == QuestionApiFormat::Anthropic;
        apiUrl->setPlaceholderText(anthropic ? QStringLiteral("https://api.anthropic.com")
                                             : QStringLiteral("https://api.openai.com/v1"));
        model->setPlaceholderText(anthropic
                                      ? QStringLiteral("留空使用 %1").arg(Question::anthropicDefaultModel())
                                      : QStringLiteral("例如 qwen-vl-max、glm-4v-plus、gpt-4o"));
    };
    updatePlaceholders(settings.apiFormat);

    connect(format, qOverload<int>(&QComboBox::currentIndexChanged), this,
            [this, format, updatePlaceholders](int index) {
        if (index < 0) {
            return;
        }
        const QuestionApiFormat current =
            format->itemData(index).toString() == questionApiFormatId(QuestionApiFormat::Anthropic)
                ? QuestionApiFormat::Anthropic
                : QuestionApiFormat::OpenAiCompatible;
        config_->mutableSettings().question.apiFormat = current;
        updatePlaceholders(current);
        scheduleSave();
    });
    const auto bindLine = [this](QLineEdit* edit, QString QuestionSettings::*field) {
        connect(edit, &QLineEdit::textChanged, this, [this, field](const QString& text) {
            config_->mutableSettings().question.*field = text.trimmed();
            scheduleSave();
        });
        connect(edit, &QLineEdit::editingFinished, this, [this]() { flushPendingSave(); });
    };
    bindLine(apiUrl, &QuestionSettings::apiUrl);
    bindLine(apiKey, &QuestionSettings::apiKey);
    bindLine(model, &QuestionSettings::model);
    bindLine(scope, &QuestionSettings::subjectScope);
    connect(revealKey, &QToolButton::toggled, this, [apiKey, revealKey](bool shown) {
        apiKey->setEchoMode(shown ? QLineEdit::Normal : QLineEdit::Password);
        revealKey->setText(shown ? QStringLiteral("隐藏") : QStringLiteral("显示"));
    });
    connect(prompt, &QPlainTextEdit::textChanged, this, [this, prompt]() {
        config_->mutableSettings().question.customPrompt = prompt->toPlainText();
        scheduleSave();
    });
    connect(resetPrompt, &QPushButton::clicked, this, [prompt]() { prompt->clear(); });
    connect(timeout, qOverload<int>(&QSpinBox::valueChanged), this, [this](int value) {
        config_->mutableSettings().question.timeoutSeconds = value;
        scheduleSave();
    });
    connect(proxy, &QAbstractButton::toggled, this, [this](bool enabled) {
        config_->mutableSettings().question.useSystemProxy = enabled;
        scheduleSave();
    });

    auto* probe = new QuestionAnswerService(config_, page);
    const auto showTestResult = [testButton, testStatus](bool ok, const QString& message) {
        testButton->setEnabled(true);
        testButton->setText(QStringLiteral("测试连接"));
        testStatus->setObjectName(ok ? QStringLiteral("SettingsInlineSuccess")
                                     : QStringLiteral("SettingsInlineError"));
        testStatus->setText(message);
        testStatus->style()->unpolish(testStatus);
        testStatus->style()->polish(testStatus);
        testStatus->show();
    };
    connect(probe, &QuestionAnswerService::finished, this,
            [showTestResult](const QString& answer, bool) {
        showTestResult(true, QStringLiteral("连接成功，模型回复：%1").arg(answer.simplified().left(60)));
    });
    connect(probe, &QuestionAnswerService::failed, this,
            [showTestResult](const QString& message) { showTestResult(false, message); });
    connect(testButton, &QPushButton::clicked, this, [this, probe, testButton, testStatus]() {
        flushPendingSave();
        if (probe->isBusy()) {
            probe->cancel();
            testButton->setText(QStringLiteral("测试连接"));
            testStatus->hide();
            return;
        }
        testButton->setText(QStringLiteral("取消测试"));
        testStatus->setObjectName(QStringLiteral("SettingsInlineSuccess"));
        testStatus->style()->unpolish(testStatus);
        testStatus->style()->polish(testStatus);
        testStatus->setText(QStringLiteral("正在测试连接..."));
        testStatus->show();
        probe->testConnection();
    });

    layout->addStretch();
    return page;
}

QWidget* SettingsDialog::createHotkeyPage()
{
    QVBoxLayout* layout = nullptr;
    auto* page = makePage(
        QStringLiteral("快捷键"),
        QStringLiteral("全局快捷键在 Visnip 位于后台时也可使用。"),
        layout);

    addSectionTitle(layout, QStringLiteral("全局操作"));
    hotkeyStatus_ = labelWithRole(QString(), QStringLiteral("SettingsInlineError"));
    hotkeyStatus_->hide();

    const auto addHotkeyRow = [this, layout](
                                  const QString& title,
                                  const QString& description,
                                  QKeySequence HotkeySettings::*field,
                                  bool addDivider) {
        auto* edit = new QKeySequenceEdit(config_->settings().hotkeys.*field);
        edit->setObjectName(QStringLiteral("VisnipSettingsHotkeyEdit"));
        edit->setClearButtonEnabled(true);
        edit->setMinimumWidth(220);
        hotkeyEdits_.append(edit);
        connect(edit, &QKeySequenceEdit::editingFinished, this,
                [this, edit, field, title]() {
            const QKeySequence previous = config_->settings().hotkeys.*field;
            const QKeySequence proposed = edit->keySequence();
            if (proposed == previous) {
                return;
            }
            const HotkeyValidationResult validation =
                HotkeyManager::validateSequence(proposed);
            if (!validation.valid) {
                hotkeyStatus_->setText(
                    QStringLiteral("%1：%2").arg(title, validation.errorMessage));
                hotkeyStatus_->show();
                edit->setKeySequence(previous);
                return;
            }
            HotkeySettings candidate = config_->settings().hotkeys;
            candidate.*field = proposed;
            if (!hotkeysAreUnique(candidate)) {
                hotkeyStatus_->setText(QStringLiteral("快捷键不能与其他 Visnip 操作重复。"));
                hotkeyStatus_->show();
                edit->setKeySequence(previous);
                return;
            }
            hotkeyStatus_->hide();
            emit hotkeyChangeRequested(candidate);
        });
        addControlRow(layout, title, description, edit, addDivider);
    };

    addHotkeyRow(QStringLiteral("开始截图"),
                 QStringLiteral("打开截图选区"),
                 &HotkeySettings::capture,
                 true);
    addHotkeyRow(QStringLiteral("贴出剪贴板图片"),
                 QStringLiteral("创建一个新的贴图窗口"),
                 &HotkeySettings::pinClipboard,
                 true);
    addHotkeyRow(QStringLiteral("重复上次截图区域"),
                 QStringLiteral("按上一次选区重新截图"),
                 &HotkeySettings::repeatCapture,
                 true);
    addHotkeyRow(QStringLiteral("隐藏或显示全部贴图"),
                 QStringLiteral("统一切换当前所有贴图"),
                 &HotkeySettings::togglePins,
                 true);
    addHotkeyRow(QStringLiteral("当前贴图鼠标穿透"),
                 QStringLiteral("切换当前贴图是否接收鼠标操作"),
                 &HotkeySettings::toggleMouseThrough,
                 true);
    addHotkeyRow(QStringLiteral("获取答案"),
                 QStringLiteral("解题面板打开时截取选区并发送；面板关闭后不占用该按键"),
                 &HotkeySettings::askQuestion,
                 false);
    layout->addSpacing(10);
    layout->addWidget(hotkeyStatus_);
    layout->addStretch();
    return page;
}

QWidget* SettingsDialog::createAboutPage()
{
    QVBoxLayout* layout = nullptr;
    auto* page = makePage(
        QStringLiteral("关于 Visnip"),
        QStringLiteral("轻量截图、贴图与截图翻译工具。"),
        layout);

    auto* logo = new QLabel;
    logo->setObjectName(QStringLiteral("VisnipAboutLogo"));
    logo->setPixmap(QIcon(QStringLiteral(":/visnip/app/visnip-logo-horizontal-black.svg"))
                        .pixmap(QSize(174, 52)));
    logo->setFixedSize(174, 52);
    logo->setAlignment(Qt::AlignLeft | Qt::AlignVCenter);
    layout->addWidget(logo);
    layout->addSpacing(18);

    addControlRow(layout,
                  QStringLiteral("版本"),
                  QStringLiteral("Windows 桌面版"),
                  labelWithRole(QStringLiteral(VISNIP_VERSION),
                                QStringLiteral("SettingsRowTitle")));
    addControlRow(layout,
                  QStringLiteral("配置"),
                  QStringLiteral("设置保存在当前 Windows 用户环境中"),
                  labelWithRole(QStringLiteral("本机"),
                                QStringLiteral("SettingsRowTitle")),
                  false);
    layout->addStretch();
    return page;
}

} // namespace Visnip
