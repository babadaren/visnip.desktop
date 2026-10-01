#include "core/AppConfig.h"
#include "core/DesignTokens.h"
#include "core/OcrLanguagePack.h"
#include "core/OcrPostProcess.h"
#include "core/QuestionAnswer.h"
#include "core/TranslationCompositor.h"
#include "platform/HotkeyManager.h"
#include "services/FastTranslationDiagnostics.h"
#include "services/ImageTranslationService.h"
#include "services/OcrService.h"
#include "services/TextTranslationService.h"
#include "ui/IconUtils.h"
#include "ui/capture/CaptureOverlayWindow.h"
#include "ui/settings/SettingsDialog.h"
#include "ui/widgets/CompactToolbar.h"

#include <QApplication>
#include <QBuffer>
#include <QComboBox>
#include <QDialog>
#include <QDir>
#include <QElapsedTimer>
#include <QEvent>
#include <QFont>
#include <QImage>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QFile>
#include <QLabel>
#include <QLayout>
#include <QLineEdit>
#include <QListView>
#include <QListWidget>
#include <QPainter>
#include <QPalette>
#include <QKeySequenceEdit>
#include <QSignalSpy>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QScrollArea>
#include <QScrollBar>
#include <QSettings>
#include <QStandardPaths>
#include <QStringListModel>
#include <QTemporaryDir>
#include <QToolButton>
#include <QUrlQuery>
#include <QtTest>

#include <algorithm>
#include <limits>

using namespace Visnip;

namespace {
// Points AppConfig at a private settings file for one test and restores the
// suite's file afterwards, also when the test fails early.
class ScopedSettingsFile {
public:
    explicit ScopedSettingsFile(const QString& path)
        : previous_(qgetenv("VISNIP_TEST_SETTINGS_FILE"))
    {
        qputenv("VISNIP_TEST_SETTINGS_FILE", QFile::encodeName(path));
    }
    ~ScopedSettingsFile()
    {
        if (previous_.isNull()) {
            qunsetenv("VISNIP_TEST_SETTINGS_FILE");
        } else {
            qputenv("VISNIP_TEST_SETTINGS_FILE", previous_);
        }
    }
    ScopedSettingsFile(const ScopedSettingsFile&) = delete;
    ScopedSettingsFile& operator=(const ScopedSettingsFile&) = delete;

private:
    QByteArray previous_;
};
} // namespace

class UiTests : public QObject {
    Q_OBJECT

private slots:
    void initTestCase()
    {
        QStandardPaths::setTestModeEnabled(true);
        QVERIFY(settingsDir_.isValid());
        qputenv("VISNIP_TEST_SETTINGS_FILE",
                QFile::encodeName(settingsDir_.filePath(QStringLiteral("settings.ini"))));
    }

    void cleanupTestCase()
    {
        qunsetenv("VISNIP_TEST_SETTINGS_FILE");
    }

    void applicationIconUsesApprovedBrandAssets()
    {
        const QIcon icon = Ui::applicationIcon();
        QVERIFY(!icon.isNull());

        for (const int size : { 16, 24, 32, 48, 64, 128, 256 }) {
            const QPixmap pixmap = icon.pixmap(QSize(size, size));
            QVERIFY2(!pixmap.isNull(), qPrintable(QStringLiteral("Missing %1 px app icon").arg(size)));
            QCOMPARE(pixmap.size(), QSize(size, size));
        }

        const QImage smallIcon = icon.pixmap(QSize(16, 16)).toImage();
        QRect darkBounds;
        for (int y = 0; y < smallIcon.height(); ++y) {
            for (int x = 0; x < smallIcon.width(); ++x) {
                const QColor pixel = smallIcon.pixelColor(x, y);
                if (pixel.alpha() > 127 && pixel.red() < 80
                    && pixel.green() < 80 && pixel.blue() < 80) {
                    darkBounds |= QRect(x, y, 1, 1);
                }
            }
        }
        QCOMPARE(darkBounds, QRect(3, 3, 11, 10));

        QFile master(QStringLiteral(":/visnip/app/visnip-app-icon-white.svg"));
        QFile compact(QStringLiteral(":/visnip/app/visnip-app-icon-white-compact.svg"));
        QFile horizontal(QStringLiteral(":/visnip/app/visnip-logo-horizontal-black.svg"));
        QFile symbol(QStringLiteral(":/visnip/app/visnip-symbol-black.svg"));
        QVERIFY(master.open(QIODevice::ReadOnly));
        QVERIFY(compact.open(QIODevice::ReadOnly));
        QVERIFY(horizontal.open(QIODevice::ReadOnly));
        QVERIFY(symbol.open(QIODevice::ReadOnly));
        const QByteArray masterSvg = master.readAll();
        const QByteArray compactSvg = compact.readAll();
        const QByteArray horizontalSvg = horizontal.readAll();
        const QByteArray symbolSvg = symbol.readAll();
        for (const QByteArray& svg : { masterSvg, compactSvg, horizontalSvg, symbolSvg }) {
            QVERIFY(svg.contains("#0B0B0D"));
            QVERIFY(!svg.contains("linearGradient"));
            QVERIFY(!svg.contains("feDropShadow"));
        }
        QVERIFY(masterSvg.contains("#FFFFFF"));
        QVERIFY(compactSvg.contains("#FFFFFF"));

        AppConfig config;
        SettingsDialog dialog(&config);
        dialog.showPage(SettingsDialog::Page::About);
        auto* aboutLogo = dialog.findChild<QLabel*>(QStringLiteral("VisnipAboutLogo"));
        QVERIFY(aboutLogo);
        QVERIFY(!aboutLogo->pixmap(Qt::ReturnByValue).isNull());
    }

    void compactToolbarWidthMatchesContentWithSymmetricEdges()
    {
        // The toolbar is a child widget in the real overlay, so the layout
        // cannot grow it back if the size hint underestimates the content.
        QWidget host;
        host.resize(900, 120);
        auto* toolbar = new Ui::CompactToolbar(&host);
        // Mirror the sizing sequence used by CaptureOverlayWindow::placeToolbar().
        toolbar->adjustSize();
        toolbar->resize(toolbar->sizeHint());
        toolbar->move(10, 10);
        host.show();
        if (toolbar->layout()) {
            toolbar->layout()->activate();
        }

        int leftEdge = std::numeric_limits<int>::max();
        int rightEdge = std::numeric_limits<int>::min();
        int topEdge = std::numeric_limits<int>::max();
        int bottomEdge = std::numeric_limits<int>::min();
        for (const QObject* child : toolbar->children()) {
            const auto* widget = qobject_cast<const QWidget*>(child);
            if (!widget) {
                continue;
            }
            leftEdge = qMin(leftEdge, widget->geometry().left());
            rightEdge = qMax(rightEdge, widget->geometry().right());
            topEdge = qMin(topEdge, widget->geometry().top());
            bottomEdge = qMax(bottomEdge, widget->geometry().bottom());
        }
        QVERIFY(leftEdge != std::numeric_limits<int>::max());

        const int leftInset = leftEdge;
        const int rightInset = toolbar->width() - 1 - rightEdge;
        QCOMPARE(rightInset, leftInset);
        // No child may be clipped and no trailing blank strip may remain.
        QVERIFY(rightInset >= 0);
        QVERIFY(rightInset <= 2);
        // The 1px border rows above and below the buttons must stay visible.
        const int topInset = topEdge;
        const int bottomInset = toolbar->height() - 1 - bottomEdge;
        QCOMPARE(bottomInset, topInset);
        QVERIFY(topInset >= 1);
        QVERIFY(topInset <= 2);

        const auto buttonX = [toolbar](const QString& tooltip) {
            for (QToolButton* button : toolbar->findChildren<QToolButton*>()) {
                if (button->toolTip() == tooltip) {
                    return button->x();
                }
            }
            return -1;
        };
        const QStringList trailingOrder = {
            QStringLiteral("撤销 Ctrl+Z"),
            QStringLiteral("重做 Ctrl+Y"),
            QStringLiteral("翻译"),
            QStringLiteral("解题"),
            QStringLiteral("关闭 Esc"),
            QStringLiteral("贴图 P"),
            QStringLiteral("保存 Ctrl+S"),
            QStringLiteral("复制 Enter"),
        };
        int previousX = -1;
        for (const QString& tooltip : trailingOrder) {
            const int x = buttonX(tooltip);
            QVERIFY2(x > previousX, qPrintable(tooltip));
            previousX = x;
        }
    }

    void compactToolbarUsesThemedTranslationPageIcon()
    {
        Ui::CompactToolbar toolbar;
        QToolButton* translate = nullptr;
        for (QToolButton* button : toolbar.findChildren<QToolButton*>()) {
            if (button->toolTip() == QStringLiteral("翻译")) {
                translate = button;
                break;
            }
        }
        QVERIFY(translate);
        QCOMPARE(translate->iconSize(), QSize(Design::toolbar().icon,
                                             Design::toolbar().icon));
        QVERIFY(!translate->icon().isNull());

        QFile resource(QStringLiteral(":/visnip/icons/action-translate.svg"));
        QVERIFY(resource.open(QIODevice::ReadOnly));
        const QByteArray svg = resource.readAll();
        QVERIFY(svg.contains("currentColor"));
        QVERIFY(!svg.contains("#2c2c2c"));
        QVERIFY(!svg.contains("DOCTYPE"));

        const auto alphaBounds = [](const QIcon& icon) {
            const QImage raw = icon.pixmap(QSize(23, 23)).toImage();
            const QImage image = raw.scaled(
                QSize(23, 23), Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
            QRect bounds;
            for (int y = 0; y < image.height(); ++y) {
                for (int x = 0; x < image.width(); ++x) {
                    if (image.pixelColor(x, y).alpha() > 8) {
                        bounds |= QRect(x, y, 1, 1);
                    }
                }
            }
            return bounds;
        };

        const QRect normalBounds = alphaBounds(translate->icon());
        QVERIFY(normalBounds.isValid());
        QVERIFY(normalBounds.width() >= 17 && normalBounds.width() <= 19);
        QVERIFY(normalBounds.height() >= 17 && normalBounds.height() <= 19);

        toolbar.setTranslateState(false, true);
        QCOMPARE(translate->toolTip(), QStringLiteral("翻译中…点击取消"));
        QCOMPARE(alphaBounds(translate->icon()), normalBounds);
    }

    void fastTranslateGlassPreservesSizeAndBlursContent()
    {
        QImage source(120, 72, QImage::Format_RGB32);
        for (int y = 0; y < source.height(); ++y) {
            for (int x = 0; x < source.width(); ++x) {
                source.setPixelColor(
                    x, y, ((x / 2) + (y / 2)) % 2 == 0 ? Qt::black : Qt::white);
            }
        }

        const QImage glass = buildFastTranslateGlassImage(source);
        QCOMPARE(glass.size(), source.size());
        QVERIFY(!glass.isNull());

        int changedPixels = 0;
        int blendedPixels = 0;
        for (int y = 0; y < glass.height(); ++y) {
            for (int x = 0; x < glass.width(); ++x) {
                const QColor pixel = glass.pixelColor(x, y);
                changedPixels += pixel != source.pixelColor(x, y);
                blendedPixels += pixel.red() > 20 && pixel.red() < 245;
            }
        }
        const int pixelCount = glass.width() * glass.height();
        QVERIFY(changedPixels > pixelCount * 3 / 4);
        QVERIFY(blendedPixels > pixelCount / 2);
        QVERIFY(buildFastTranslateGlassImage(QImage()).isNull());
    }

    void magnifierCursorColorSettingControlsOnlyColorInformation()
    {
        const QColor sampledColor(QStringLiteral("#112233"));
        QCOMPARE(captureCursorColorDisplayText(sampledColor),
                 QStringLiteral("RGB(17, 34, 51)  #112233"));

        QImage source(48, 48, QImage::Format_ARGB32);
        source.fill(sampledColor);
        const QPoint localMouse(24, 24);
        const QPoint globalMouse(960, 540);
        const QColor accent(QStringLiteral("#4F7CFF"));

        const QImage withColor = buildCaptureMagnifierPanelImage(
            source, localMouse, globalMouse, accent, true);
        const QImage withoutColor = buildCaptureMagnifierPanelImage(
            source, localMouse, globalMouse, accent, false);

        QVERIFY(!withColor.isNull());
        QCOMPARE(withoutColor.size(), withColor.size());
        QCOMPARE(withColor.size(), QSize(174, 142));

        // The magnified preview is unchanged when colour information is off.
        QCOMPARE(withColor.pixelColor(24, 24), sampledColor);
        QCOMPARE(withoutColor.pixelColor(24, 24), sampledColor);

        // The same location is the colour swatch only in the enabled state.
        constexpr QPoint swatchCenter(16, 129);
        QCOMPARE(withColor.pixelColor(swatchCenter), sampledColor);
        QVERIFY(withoutColor.pixelColor(swatchCenter) != sampledColor);

        int colorRowDifferences = 0;
        const QRect colorRow(0, withColor.height() - 24,
                             withColor.width(), 24);
        for (int y = colorRow.top(); y <= colorRow.bottom(); ++y) {
            for (int x = colorRow.left(); x <= colorRow.right(); ++x) {
                colorRowDifferences += withColor.pixelColor(x, y)
                    != withoutColor.pixelColor(x, y);
            }
        }
        QVERIFY(colorRowDifferences > 40);
    }

    void captureOverlayUsesAnOpaqueNonLayeredSurface()
    {
        AppConfig config;
        CaptureOverlayWindow overlay(&config);

        // Per-pixel-alpha layering (WA_TranslucentBackground) breaks
        // SetWindowDisplayAffinity on Windows and forces full-surface flushes,
        // so the overlay must stay an opaque, fully painted window.
        QVERIFY(!overlay.testAttribute(Qt::WA_TranslucentBackground));
        QVERIFY(overlay.testAttribute(Qt::WA_NoSystemBackground));
        QVERIFY(overlay.testAttribute(Qt::WA_OpaquePaintEvent));
        QVERIFY(overlay.windowFlags().testFlag(Qt::FramelessWindowHint));
        QVERIFY(overlay.windowFlags().testFlag(Qt::Tool));
        QVERIFY(overlay.windowFlags().testFlag(Qt::WindowStaysOnTopHint));
    }

    void textToolUsesImmediateLightweightFontModel()
    {
        AppConfig config;
        CaptureOverlayWindow overlay(&config);

        QVERIFY(QMetaObject::invokeMethod(
            &overlay,
            "onToolSelected",
            Qt::DirectConnection,
            Q_ARG(QString, QStringLiteral("tool-text"))));

        QComboBox* fontCombo = nullptr;
        for (QComboBox* combo : overlay.findChildren<QComboBox*>()) {
            if (combo->toolTip() == QStringLiteral("字体")) {
                fontCombo = combo;
                break;
            }
        }
        QVERIFY(fontCombo);
        QVERIFY(qobject_cast<QStringListModel*>(fontCombo->model()));
        QVERIFY(fontCombo->findText(config.settings().tools.textFontFamily,
                                    Qt::MatchFixedString
                                        | Qt::MatchCaseSensitive)
                >= 0);
    }

    void fastTranslatePlacementAllowsMovementButRejectsResize()
    {
        const QRect source(120, 80, 640, 360);
        QVERIFY(fastTranslationPlacementIsCompatible(source, source));
        QVERIFY(fastTranslationPlacementIsCompatible(
            source, QRect(420, 260, 640, 360)));
        QVERIFY(!fastTranslationPlacementIsCompatible(
            source, QRect(420, 260, 641, 360)));
        QVERIFY(!fastTranslationPlacementIsCompatible(source, QRect()));
    }

    void compactToolbarOffersQuestionOutsideLongCapture()
    {
        Ui::CompactToolbar toolbar;
        QToolButton* question = nullptr;
        for (QToolButton* button : toolbar.findChildren<QToolButton*>()) {
            if (button->toolTip() == QStringLiteral("解题")) {
                question = button;
            }
        }
        QVERIFY(question);
        QVERIFY(!question->icon().isNull());
        QVERIFY(!question->isCheckable());
        QSignalSpy actions(&toolbar, &Ui::CompactToolbar::actionTriggered);
        question->click();
        QCOMPARE(actions.size(), 1);
        QCOMPARE(actions.first().at(0).toString(), QStringLiteral("action-question"));

        toolbar.setLongCaptureMode(true);
        QVERIFY(!question->isEnabled());
        toolbar.setLongCaptureMode(false);
        QVERIFY(question->isEnabled());
    }

    void questionSettingsEditTheModelConfiguration()
    {
        QTemporaryDir temp;
        QVERIFY(temp.isValid());
        const ScopedSettingsFile settingsFile(temp.filePath(QStringLiteral("settings.ini")));
        AppConfig config;
        SettingsDialog dialog(&config);
        dialog.showPage(SettingsDialog::Page::Question);
        auto* nav = dialog.findChild<QListWidget*>(QStringLiteral("VisnipSettingsNav"));
        QVERIFY(nav);
        QCOMPARE(nav->currentItem()->text(), QStringLiteral("解题"));

        auto* format = dialog.findChild<QComboBox*>(QStringLiteral("VisnipSettingsQuestionFormat"));
        auto* url = dialog.findChild<QLineEdit*>(QStringLiteral("VisnipSettingsQuestionUrl"));
        auto* key = dialog.findChild<QLineEdit*>(QStringLiteral("VisnipSettingsQuestionKey"));
        auto* model = dialog.findChild<QLineEdit*>(QStringLiteral("VisnipSettingsQuestionModel"));
        auto* scope = dialog.findChild<QLineEdit*>(QStringLiteral("VisnipSettingsQuestionScope"));
        auto* prompt = dialog.findChild<QPlainTextEdit*>(QStringLiteral("VisnipSettingsQuestionPrompt"));
        auto* reset = dialog.findChild<QPushButton*>(QStringLiteral("VisnipSettingsQuestionPromptReset"));
        QVERIFY(format && url && key && model && scope && prompt && reset);
        QCOMPARE(key->echoMode(), QLineEdit::Password);
        QCOMPARE(prompt->placeholderText(), Question::defaultPrompt());

        format->setCurrentIndex(1);
        QCOMPARE(config.settings().question.apiFormat, QuestionApiFormat::Anthropic);
        QVERIFY(model->placeholderText().contains(Question::anthropicDefaultModel()));
        url->setText(QStringLiteral(" https://api.example.com/anthropic "));
        key->setText(QStringLiteral("sk-test"));
        model->setText(QStringLiteral("claude-sonnet-5-5"));
        scope->setText(QStringLiteral("高中化学"));
        prompt->setPlainText(QStringLiteral("只写答案"));
        QCOMPARE(config.settings().question.apiUrl, QStringLiteral("https://api.example.com/anthropic"));
        QCOMPARE(config.settings().question.apiKey, QStringLiteral("sk-test"));
        QCOMPARE(config.settings().question.model, QStringLiteral("claude-sonnet-5-5"));
        QCOMPARE(config.settings().question.subjectScope, QStringLiteral("高中化学"));
        QCOMPARE(config.settings().question.customPrompt, QStringLiteral("只写答案"));
        reset->click();
        QVERIFY(config.settings().question.customPrompt.isEmpty());

        dialog.showPage(SettingsDialog::Page::Hotkey);
        const QList<QKeySequenceEdit*> hotkeys =
            dialog.findChildren<QKeySequenceEdit*>(QStringLiteral("VisnipSettingsHotkeyEdit"));
        QCOMPARE(hotkeys.size(), 6);
        QCOMPARE(hotkeys.last()->keySequence(), QKeySequence(Qt::Key_F4));
    }

    void translationSettingsPreserveLegacyAndCloudControls()
    {
        if (!onlineTranslationEnabled()) {
            QSKIP("Online translation is not built in (VISNIP_ONLINE_TRANSLATION=OFF).");
        }
        // An existing cloud user who already accepted the upload notice.
        AppConfig config;
        config.mutableSettings().aiTranslate.translationMethod = TranslationMethod::CloudImage;
        config.mutableSettings().aiTranslate.cloudUploadConsent = AiTranslateSettings::kUploadNoticeVersion;
        config.mutableSettings().aiTranslate.intranetUploadConsent = AiTranslateSettings::kUploadNoticeVersion;
        SettingsDialog dialog(&config);
        dialog.showPage(SettingsDialog::Page::Translation);

        auto* nav = dialog.findChild<QListWidget*>(QStringLiteral("VisnipSettingsNav"));
        auto* stack = dialog.findChild<QStackedWidget*>();
        QVERIFY(nav);
        QVERIFY(stack);
        QCOMPARE(nav->currentRow(), static_cast<int>(SettingsDialog::Page::Translation));
        QCOMPARE(stack->currentIndex(), static_cast<int>(SettingsDialog::Page::Translation));
        QCOMPARE(nav->currentItem()->text(), QStringLiteral("翻译"));

        auto* ocrPack = dialog.findChild<QComboBox*>(
            QStringLiteral("VisnipSettingsOcrPackCombo"));
        auto* local = dialog.findChild<QPushButton*>(
            QStringLiteral("VisnipSettingsTranslationLocal"));
        auto* cloud = dialog.findChild<QPushButton*>(
            QStringLiteral("VisnipSettingsTranslationCloud"));
        auto* details = dialog.findChild<QStackedWidget*>(
            QStringLiteral("VisnipSettingsTranslationDetails"));
        QVERIFY(ocrPack);
        QVERIFY(local);
        QVERIFY(cloud);
        QVERIFY(details);
        QVERIFY(cloud->isChecked());
        QVERIFY(!local->isChecked());
        QCOMPARE(details->currentIndex(), 1);
        local->click();
        QCOMPARE(config.settings().aiTranslate.translationMethod,
                 TranslationMethod::LocalOcr);
        QVERIFY(local->isChecked());
        QCOMPARE(details->currentIndex(), 0);
        const int localDetailsHeight = details->sizeHint().height();
        QCOMPARE(ocrPack->count(), Ocr::languagePacks().size());
        QCOMPARE(ocrPack->currentData().toString(),
                 Ocr::defaultLanguagePackId());
        QCOMPARE(ocrPack->itemData(1).toString(),
                 QStringLiteral("korean-v5"));
        ocrPack->setCurrentIndex(1);
        if (Ocr::languagePackInstalled(QStringLiteral("korean-v5"))) {
            QCOMPARE(config.settings().aiTranslate.fastOcrPackId,
                     QStringLiteral("korean-v5"));
        } else {
            QCOMPARE(config.settings().aiTranslate.fastOcrPackId,
                     Ocr::defaultLanguagePackId());
        }
        auto* deleteButton = dialog.findChild<QPushButton*>(
            QStringLiteral("VisnipSettingsOcrPackDeleteButton"));
        QVERIFY(deleteButton);
        cloud->click();
        QCOMPARE(config.settings().aiTranslate.translationMethod,
                 TranslationMethod::CloudBaidu);
        QCOMPARE(details->currentIndex(), 1);
        QVERIFY(details->sizeHint().height() < localDetailsHeight);

        QVERIFY(!dialog.findChild<QComboBox*>(
            QStringLiteral("VisnipSettingsTranslationMethodCombo")));
        auto* localAdvanced = dialog.findChild<QWidget*>(
            QStringLiteral("VisnipSettingsLocalAdvanced"));
        QVERIFY(localAdvanced);
        QVERIFY(localAdvanced->isHidden());
        QVERIFY(dialog.findChild<QLineEdit*>(
            QStringLiteral("VisnipSettingsBaiduAppId")));
        QVERIFY(dialog.findChild<QLineEdit*>(
            QStringLiteral("VisnipSettingsBaiduSecret")));
        QVERIFY(!QApplication::isEffectEnabled(Qt::UI_AnimateCombo));
        auto* comboView = qobject_cast<QListView*>(ocrPack->view());
        QVERIFY(comboView);
        QCOMPARE(comboView->palette().color(QPalette::Base), QColor("#FFFFFF"));
        QCOMPARE(comboView->palette().color(QPalette::Text), QColor("#182230"));
        QCOMPARE(comboView->palette().color(QPalette::Highlight), QColor("#E3EBFF"));
        QCOMPARE(comboView->viewport()->palette().color(QPalette::Window),
                 QColor("#FFFFFF"));
        QVERIFY(!comboView->testAttribute(Qt::WA_OpaquePaintEvent));
        QVERIFY(!comboView->viewport()->testAttribute(Qt::WA_OpaquePaintEvent));

        QCOMPARE(local->property("settingsSegment").toBool(), true);
        QCOMPARE(cloud->property("settingsSegment").toBool(), true);
        QCOMPARE(deleteButton->property("settingsRole").toString(),
                 QStringLiteral("danger"));
        auto* downloadButton = dialog.findChild<QPushButton*>(
            QStringLiteral("VisnipSettingsOcrPackDownloadButton"));
        QVERIFY(downloadButton);
        QCOMPARE(downloadButton->property("settingsRole").toString(),
                 QStringLiteral("primary"));
        QToolButton* revealButton = nullptr;
        const auto revealButtons = dialog.findChildren<QToolButton*>();
        for (QToolButton* button : revealButtons) {
            if (button->property("settingsRole").toString()
                == QStringLiteral("inline")) {
                revealButton = button;
                break;
            }
        }
        QVERIFY(revealButton);
        QVERIFY(!dialog.findChild<QLabel*>(QStringLiteral("SettingsSidebarCaption")));
    }

    void settingsPagesAreCreatedOnFirstVisitAndReused()
    {
        AppConfig config;
        SettingsDialog dialog(&config);
        auto* stack = dialog.findChild<QStackedWidget*>(QStringLiteral("SettingsStack"));
        QVERIFY(stack);
        QCOMPARE(stack->count(), static_cast<int>(SettingsDialog::Page::About) + 1);
        QCOMPARE(dialog.findChildren<QWidget*>(
                     QStringLiteral("SettingsPagePlaceholder")).size(), stack->count() - 1);
        QVERIFY(!dialog.findChild<QComboBox*>(
            QStringLiteral("VisnipSettingsOcrPackCombo")));

        dialog.showPage(SettingsDialog::Page::Translation);
        auto* firstTranslationCombo = dialog.findChild<QComboBox*>(
            QStringLiteral("VisnipSettingsOcrPackCombo"));
        QVERIFY(firstTranslationCombo);
        QCOMPARE(dialog.findChildren<QWidget*>(
                     QStringLiteral("SettingsPagePlaceholder")).size(), stack->count() - 2);

        dialog.showPage(SettingsDialog::Page::General);
        dialog.showPage(SettingsDialog::Page::Translation);
        QCOMPARE(dialog.findChild<QComboBox*>(
                     QStringLiteral("VisnipSettingsOcrPackCombo")),
                 firstTranslationCombo);
        QVERIFY(!dialog.testAttribute(Qt::WA_DeleteOnClose));
    }

    void obsoleteHighFidelitySettingsAreMigratedAndRemoved()
    {
        const QString settingsPath = settingsDir_.filePath(QStringLiteral("settings.ini"));
        {
            QSettings legacy(settingsPath, QSettings::IniFormat);
            legacy.clear();
            legacy.beginGroup(QStringLiteral("aiTranslate"));
            legacy.setValue(QStringLiteral("mode"), QStringLiteral("hifi"));
            legacy.setValue(QStringLiteral("provider"), QStringLiteral("gemini"));
            legacy.setValue(QStringLiteral("useOfficialApi"), false);
            legacy.setValue(QStringLiteral("customBaseUrl"),
                            QStringLiteral("https://legacy.example/v1"));
            legacy.setValue(QStringLiteral("apiKey"), QStringLiteral("legacy-secret"));
            legacy.setValue(QStringLiteral("model"), QStringLiteral("legacy-image-model"));
            legacy.setValue(QStringLiteral("targetLanguage"), QStringLiteral("zh-Hant"));
            legacy.setValue(QStringLiteral("baiduAppId"), QStringLiteral("keep-baidu-id"));
            legacy.endGroup();
            legacy.sync();
            QCOMPARE(legacy.status(), QSettings::NoError);
        }

        AppConfig migratedConfig;
        migratedConfig.load();
        const TranslationMethod migratedHifi = onlineTranslationEnabled()
            ? TranslationMethod::CloudBaidu : TranslationMethod::Offline;
        QCOMPARE(migratedConfig.settings().aiTranslate.translationMethod, migratedHifi);
        QCOMPARE(migratedConfig.settings().aiTranslate.targetLanguage,
                 QStringLiteral("zh-Hans"));
        QCOMPARE(migratedConfig.settings().aiTranslate.baiduAppId,
                 QStringLiteral("keep-baidu-id"));

        {
            QSettings migrated(settingsPath, QSettings::IniFormat);
            migrated.beginGroup(QStringLiteral("aiTranslate"));
            QCOMPARE(migrated.value(QStringLiteral("translationMethod")).toString(),
                     translationMethodId(migratedHifi));
            const QStringList obsoleteKeys = {
                QStringLiteral("mode"), QStringLiteral("provider"),
                QStringLiteral("useOfficialApi"), QStringLiteral("customBaseUrl"),
                QStringLiteral("apiKey"), QStringLiteral("model"),
                QStringLiteral("fastPipeline"),
            };
            for (const QString& key : obsoleteKeys) {
                QVERIFY2(!migrated.contains(key), qPrintable(key));
            }
            migrated.remove(QStringLiteral("translationMethod"));
            migrated.setValue(QStringLiteral("fastPipeline"),
                              QStringLiteral("local-ocr"));
            migrated.endGroup();
            migrated.sync();
        }

        AppConfig localConfig;
        localConfig.load();
        QCOMPARE(localConfig.settings().aiTranslate.translationMethod,
                 onlineTranslationEnabled() ? TranslationMethod::LocalOcr : TranslationMethod::Offline);
    }

    void settingsTextChangesAreDebouncedAndFlushed()
    {
        if (!onlineTranslationEnabled()) {
            QSKIP("Online translation is not built in (VISNIP_ONLINE_TRANSLATION=OFF).");
        }
        // The service address applies to online modes; offline is the default.
        AppConfig config;
        config.mutableSettings().aiTranslate.translationMethod = TranslationMethod::CloudImage;
        config.mutableSettings().aiTranslate.cloudUploadConsent = AiTranslateSettings::kUploadNoticeVersion;
        SettingsDialog dialog(&config);
        dialog.showPage(SettingsDialog::Page::Translation);

        auto* serviceUrl = dialog.findChild<QLineEdit*>(
            QStringLiteral("VisnipSettingsServiceUrl"));
        QVERIFY(serviceUrl);
        QSignalSpy changedSpy(&config, &AppConfig::changed);

        serviceUrl->setText(QStringLiteral("https://one.example/visnip"));
        serviceUrl->setText(QStringLiteral("https://two.example/visnip"));
        QCOMPARE(config.settings().aiTranslate.fastServiceUrl,
                 QStringLiteral("https://two.example/visnip"));
        QCOMPARE(changedSpy.count(), 0);
        QTRY_COMPARE_WITH_TIMEOUT(changedSpy.count(), 1, 1000);

        AppConfig reloaded;
        reloaded.load();
        QCOMPARE(reloaded.settings().aiTranslate.fastServiceUrl,
                 QStringLiteral("https://two.example/visnip"));
    }

    void settingsPagesFitWithoutHorizontalOverflow()
    {
        AppConfig config;
        SettingsDialog dialog(&config);
        dialog.resize(dialog.minimumSize());
        dialog.show();
        QApplication::processEvents();

        auto* stack = dialog.findChild<QStackedWidget*>(QStringLiteral("SettingsStack"));
        QVERIFY(stack);
        QCOMPARE(stack->count(), static_cast<int>(SettingsDialog::Page::About) + 1);
        for (int index = 0; index < stack->count(); ++index) {
            dialog.showPage(static_cast<SettingsDialog::Page>(index));
            QApplication::processEvents();
            auto* scroll = qobject_cast<QScrollArea*>(stack->currentWidget());
            QVERIFY(scroll);
            QVERIFY2(scroll->horizontalScrollBar()->maximum() == 0,
                     qPrintable(QStringLiteral("Settings page %1 overflows by %2 px")
                                    .arg(index)
                                    .arg(scroll->horizontalScrollBar()->maximum())));
        }
    }

    void hotkeyValidationMatchesTheWindowsBackend()
    {
        QVERIFY(HotkeyManager::validateSequence(QKeySequence(Qt::Key_F8)).valid);
        QVERIFY(HotkeyManager::validateSequence(
            QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_T)).valid);
        QVERIFY(!HotkeyManager::validateSequence(QKeySequence()).valid);
        QVERIFY(!HotkeyManager::validateSequence(
            QKeySequence(QStringLiteral("Ctrl+K, Ctrl+C"))).valid);
        QVERIFY(!HotkeyManager::validateSequence(
            QKeySequence(Qt::Key_PageDown)).valid);
    }

    void invalidOutputPatternDoesNotReplaceTheEffectiveRule()
    {
        AppConfig config;
        const QString original = config.settings().output.filenamePattern;
        SettingsDialog dialog(&config);
        dialog.showPage(SettingsDialog::Page::Output);

        auto* pattern = dialog.findChild<QLineEdit*>(
            QStringLiteral("VisnipSettingsFilenamePattern"));
        QVERIFY(pattern);
        pattern->setText(QStringLiteral("../outside_%1.png"));
        QCOMPARE(config.settings().output.filenamePattern, original);

        pattern->setText(QStringLiteral("Capture_%1.png"));
        QCOMPARE(config.settings().output.filenamePattern,
                 QStringLiteral("Capture_%1.png"));
    }

    void ocrRecognizesSyntheticScreenshot()
    {
        QString missing;
        if (!OcrService::assetsPresent(&missing)) {
            QSKIP(qPrintable(QStringLiteral("OCR assets missing: %1").arg(missing)));
        }

        // Paint a synthetic UI screenshot: mixed Chinese/English labels at
        // typical desktop font sizes on light and dark backgrounds.
        QImage screenshot(720, 320, QImage::Format_ARGB32);
        screenshot.fill(QColor(245, 246, 248));
        {
            QPainter painter(&screenshot);
            painter.setRenderHint(QPainter::TextAntialiasing);
            QFont font(QStringLiteral("Microsoft YaHei UI"));

            font.setPixelSize(24);
            painter.setFont(font);
            painter.setPen(QColor(20, 20, 24));
            painter.drawText(QRect(32, 24, 660, 40), Qt::AlignLeft | Qt::AlignVCenter,
                             QStringLiteral("文件下载完成"));

            font.setPixelSize(16);
            painter.setFont(font);
            painter.setPen(QColor(70, 74, 82));
            painter.drawText(QRect(32, 80, 660, 30), Qt::AlignLeft | Qt::AlignVCenter,
                             QStringLiteral("The quick brown fox jumps over the lazy dog"));
            painter.drawText(QRect(32, 120, 660, 30), Qt::AlignLeft | Qt::AlignVCenter,
                             QStringLiteral("请在设置中选择保存位置，然后点击确认按钮。"));

            painter.fillRect(QRect(32, 190, 656, 90), QColor(36, 41, 51));
            painter.setPen(Qt::white);
            painter.drawText(QRect(56, 210, 600, 26), Qt::AlignLeft | Qt::AlignVCenter,
                             QStringLiteral("Download complete: 3 files"));
            painter.drawText(QRect(56, 242, 600, 26), Qt::AlignLeft | Qt::AlignVCenter,
                             QStringLiteral("耗时 12 秒"));
        }

        OcrService service;
        QSignalSpy prewarmSpy(&service, &OcrService::prewarmFinished);
        service.prewarm();
        service.prewarm();
        QVERIFY2(prewarmSpy.wait(15000), "OCR prewarm did not finish");
        QCOMPARE(prewarmSpy.count(), 1);
        const QList<QVariant> prewarmResult = prewarmSpy.takeFirst();
        QVERIFY2(prewarmResult.at(1).toBool(), qPrintable(prewarmResult.at(2).toString()));
        service.prewarm();
        QVERIFY2(prewarmSpy.wait(1000), "Ready OCR prewarm did not report completion");
        QVERIFY(prewarmSpy.first().at(3).toLongLong() <= 2);

        QString error;
        QElapsedTimer timer;
        timer.start();
        const QVector<OcrTextLine> lines = service.recognizeSync(screenshot, &error);
        qInfo("OCR synthetic screenshot: %lld ms, %lld lines",
              static_cast<long long>(timer.elapsed()), static_cast<long long>(lines.size()));
        for (const OcrTextLine& line : lines) {
            qInfo("  box=(%d,%d %dx%d) score=%.2f text=%s", line.box.x(), line.box.y(),
                  line.box.width(), line.box.height(), static_cast<double>(line.score),
                  qPrintable(line.text));
        }

        QVERIFY2(error.isEmpty(), qPrintable(error));
        QVERIFY(lines.size() >= 5);

        QString allText;
        for (const OcrTextLine& line : lines) {
            allText += line.text;
        }
        QVERIFY2(allText.contains(QStringLiteral("下载完成")), qPrintable(allText));
        QVERIFY2(allText.contains(QStringLiteral("quick brown fox")), qPrintable(allText));
        QVERIFY2(allText.contains(QStringLiteral("Download complete")), qPrintable(allText));
        QVERIFY2(allText.contains(QStringLiteral("12")), qPrintable(allText));

        // A stale overlay must not be able to cancel the current request on
        // the application-scoped service.
        QSignalSpy finishedSpy(&service, &OcrService::finished);
        const quint64 currentRevision = service.recognize(screenshot);
        service.cancel(currentRevision - 1);
        QVERIFY2(finishedSpy.wait(15000), "OCR request was cancelled by a stale revision");
        QCOMPARE(finishedSpy.first().at(0).toULongLong(), currentRevision);

        // Boxes must land where the text was painted (source-image pixels).
        const OcrTextLine* title = nullptr;
        for (const OcrTextLine& line : lines) {
            if (line.text.contains(QStringLiteral("下载完成"))) {
                title = &line;
                break;
            }
        }
        QVERIFY(title);
        QVERIFY(title->box.top() >= 8 && title->box.bottom() <= 84);
        QVERIFY(title->box.left() >= 8 && title->box.left() <= 60);

        // Full local fast-translate chain: keep one stable unit per OCR line
        // and composite dummy translations back onto the screenshot.
        const auto units = Translate::makeLineTranslationUnits(lines);
        QVERIFY(!units.isEmpty());
        QStringList translations;
        for (int i = 0; i < units.size(); ++i) {
            translations.append(QStringLiteral("译文 %1").arg(i + 1));
        }
        const QImage composed = Translate::composeTranslatedImage(
            screenshot, units, translations, QStringLiteral("Microsoft YaHei UI"));
        QCOMPARE(composed.size(), screenshot.size());
        // The dark banner block must now be covered by a dark patch (its
        // sampled background), no longer showing the original white glyphs.
        const OcrTextLine* banner = nullptr;
        for (const OcrTextLine& line : lines) {
            if (line.text.contains(QStringLiteral("Download complete"))) {
                banner = &line;
                break;
            }
        }
        QVERIFY(banner);
        const QRect probe = banner->box.adjusted(2, 2, -2, -2);
        // The patch repaints the area: the probe region must differ from the
        // original, and its corner must now carry the dark sampled background
        // instead of untouched glyph pixels.
        QVERIFY(screenshot.copy(probe) != composed.copy(probe));
        QVERIFY(qGray(composed.pixel(probe.topLeft())) < 128);
    }

    void ocrRecoversFragmentedNameDescription()
    {
        QString missing;
        if (!OcrService::assetsPresent(&missing)) {
            QSKIP(qPrintable(QStringLiteral("OCR assets missing: %1").arg(missing)));
        }

        const QString diagnosticRoot = QString::fromLocal8Bit(
            qgetenv("VISNIP_REAL_DIAGNOSTIC_ROOT"));
        if (diagnosticRoot.isEmpty()) {
            QSKIP("Set VISNIP_REAL_DIAGNOSTIC_ROOT to run the real screenshot regression");
        }
        const QImage screenshot(
            QDir(diagnosticRoot).filePath(QStringLiteral("original.png")));
        QVERIFY2(!screenshot.isNull(), "Real diagnostic original.png is unavailable");

        OcrService service;
        QString error;
        const QVector<OcrTextLine> lines = service.recognizeSync(
            screenshot, &error);
        QVERIFY2(error.isEmpty(), qPrintable(error));

        const OcrTextLine* description = nullptr;
        int overlappingFragments = 0;
        QStringList nearbyText;
        for (const OcrTextLine& line : lines) {
            if (line.text.trimmed().startsWith(
                    QStringLiteral("Your name may appear"))) {
                description = &line;
            }
            if (line.box.top() >= 200 && line.box.top() <= 240) {
                nearbyText.append(line.text);
            }
            if (line.box.top() >= 210 && line.box.top() <= 222
                && line.box.left() > 620) {
                ++overlappingFragments;
            }
        }
        QVERIFY2(description, qPrintable(nearbyText.join(QStringLiteral(" | "))));
        QCOMPARE(overlappingFragments, 0);
        QVERIFY2(description->box.width() >= 570,
                 qPrintable(QStringLiteral("box=%1,%2 %3x%4 text=%5")
                                .arg(description->box.x())
                                .arg(description->box.y())
                                .arg(description->box.width())
                                .arg(description->box.height())
                                .arg(description->text)));
        QVERIFY2(description->text.contains(QStringLiteral("contribute or are mentioned")),
                 qPrintable(description->text));
        QVERIFY2(description->text.contains(QStringLiteral("remove it at any time")),
                 qPrintable(description->text));

        const OcrTextLine* privacy = nullptr;
        const OcrTextLine* privacyTail = nullptr;
        QStringList privacyAreaText;
        for (const OcrTextLine& line : lines) {
            if (line.box.top() >= 305 && line.box.top() <= 360) {
                privacyAreaText.append(line.text);
            }
            if (line.text.trimmed().startsWith(
                    QStringLiteral("You have set your email address"))) {
                privacy = &line;
            }
            if (line.text.contains(QStringLiteral("email address private"))) {
                privacyTail = &line;
            }
        }
        QVERIFY2(privacy, qPrintable(privacyAreaText.join(QStringLiteral(" | "))));
        QVERIFY2(privacyTail, qPrintable(privacyAreaText.join(QStringLiteral(" | "))));
        QVERIFY2(privacy->text.contains(QStringLiteral("email privacy")),
                 qPrintable(privacy->text));
        QVERIFY2(privacy->text.contains(QStringLiteral("email settings")),
                 qPrintable(privacy->text));
        QVERIFY2(privacy->text.contains(QStringLiteral("uncheck")),
                 qPrintable(privacy->text));
        QVERIFY2(privacyTail->text.contains(QStringLiteral("private")),
                 qPrintable(privacyTail->text));

        const auto blocks = Translate::mergeLinesIntoBlocks(lines, screenshot);
        const auto privacyBlock = std::find_if(
            blocks.cbegin(), blocks.cend(), [](const Translate::TextBlock& block) {
                return block.mergedText().startsWith(
                    QStringLiteral("You have set your email address"));
            });
        QVERIFY(privacyBlock != blocks.cend());
        QVERIFY(privacyBlock->mergedText().contains(
            QStringLiteral("email address private")));
    }

    void ocrRecognizesBillingMenuLabels()
    {
        QString missing;
        if (!OcrService::assetsPresent(&missing)) {
            QSKIP(qPrintable(QStringLiteral("OCR assets missing: %1").arg(missing)));
        }

        const QString diagnosticRoot = QString::fromLocal8Bit(
            qgetenv("VISNIP_REAL_BILLING_MENU_ROOT"));
        if (diagnosticRoot.isEmpty()) {
            QSKIP("Set VISNIP_REAL_BILLING_MENU_ROOT to run the billing menu regression");
        }
        const QImage screenshot(
            QDir(diagnosticRoot).filePath(QStringLiteral("original.png")));
        QVERIFY2(!screenshot.isNull(), "Billing menu original.png is unavailable");

        OcrService service;
        QString error;
        const QVector<OcrTextLine> lines = service.recognizeSync(
            screenshot, &error);
        QVERIFY2(error.isEmpty(), qPrintable(error));
        QStringList texts;
        for (const OcrTextLine& line : lines) {
            texts.append(line.text.trimmed());
        }
        QCOMPARE(texts.size(), 10);
        QVERIFY2(texts.contains(QStringLiteral("Billing and licensing")),
                 qPrintable(texts.join(QStringLiteral(" | "))));
        QVERIFY2(texts.contains(QStringLiteral("AI usage")),
                 qPrintable(texts.join(QStringLiteral(" | "))));
        QVERIFY(texts.contains(QStringLiteral("Payment information")));
        QVERIFY(texts.contains(QStringLiteral("Additional billing details")));
    }

    void ocrRecognizesBillingPageLabels()
    {
        QString missing;
        if (!OcrService::assetsPresent(&missing)) {
            QSKIP(qPrintable(QStringLiteral("OCR assets missing: %1").arg(missing)));
        }

        const QString diagnosticRoot = QString::fromLocal8Bit(
            qgetenv("VISNIP_REAL_BILLING_PAGE_ROOT"));
        if (diagnosticRoot.isEmpty()) {
            QSKIP("Set VISNIP_REAL_BILLING_PAGE_ROOT to run the billing page regression");
        }
        const QImage screenshot(
            QDir(diagnosticRoot).filePath(QStringLiteral("original.png")));
        QVERIFY2(!screenshot.isNull(), "Billing page original.png is unavailable");

        OcrService service;
        QString error;
        const QVector<OcrTextLine> lines = service.recognizeSync(
            screenshot, &error);
        QVERIFY2(error.isEmpty(), qPrintable(error));
        QStringList texts;
        for (const OcrTextLine& line : lines) {
            texts.append(line.text.trimmed());
        }
        const QString diagnostic = texts.join(QStringLiteral(" | "));
        QVERIFY2(texts.contains(QStringLiteral("Public profile")), qPrintable(diagnostic));
        QVERIFY2(texts.contains(QStringLiteral("Appearance")), qPrintable(diagnostic));
        QVERIFY2(texts.contains(QStringLiteral("Accessibility")), qPrintable(diagnostic));
        QVERIFY2(texts.contains(QStringLiteral("Billing and licensing")), qPrintable(diagnostic));
        QVERIFY2(texts.contains(QStringLiteral("Payment information")), qPrintable(diagnostic));
        QVERIFY2(texts.contains(QStringLiteral("Additional billing details")), qPrintable(diagnostic));
        QVERIFY2(texts.contains(QStringLiteral("Emails")), qPrintable(diagnostic));
        QVERIFY2(texts.contains(QStringLiteral("Password and authentication")), qPrintable(diagnostic));
    }

    void ocrRecognizesProfileWeekdaysAndRejectsSmiley()
    {
        QString missing;
        if (!OcrService::assetsPresent(&missing)) {
            QSKIP(qPrintable(QStringLiteral("OCR assets missing: %1").arg(missing)));
        }

        const QString diagnosticRoot = QString::fromLocal8Bit(
            qgetenv("VISNIP_REAL_PROFILE_ROOT"));
        if (diagnosticRoot.isEmpty()) {
            QSKIP("Set VISNIP_REAL_PROFILE_ROOT to run the profile page regression");
        }
        const QImage screenshot(
            QDir(diagnosticRoot).filePath(QStringLiteral("original.png")));
        QVERIFY2(!screenshot.isNull(), "Profile page original.png is unavailable");

        OcrService service;
        QString error;
        const QVector<OcrTextLine> lines = service.recognizeSync(
            screenshot, &error);
        QVERIFY2(error.isEmpty(), qPrintable(error));
        QVector<Ocr::RejectedTextLine> rejected;
        const QVector<OcrTextLine> filtered = Ocr::filterTranslatableLines(
            lines, screenshot, &rejected);
        const QVector<Translate::TextBlock> units =
            Translate::makeLineTranslationUnits(filtered);
        QStringList observedTexts;
        QStringList translationTexts;
        QVector<Translate::TextBlock> preservedUnits;
        QVector<Translate::TextBlock> translationUnits;
        for (const Translate::TextBlock& unit : units) {
            observedTexts.append(unit.mergedText().trimmed());
            const auto decision = Translate::classifyBlockForTranslation(unit);
            if (decision.translatable) {
                translationTexts.append(unit.mergedText().trimmed());
                translationUnits.append(unit);
            } else {
                preservedUnits.append(unit);
            }
        }
        const QString diagnostic = observedTexts.join(QStringLiteral(" | "));
        QVERIFY2(translationTexts.contains(QStringLiteral("Oct")), qPrintable(diagnostic));
        QVERIFY2(translationTexts.contains(QStringLiteral("Jun")), qPrintable(diagnostic));
        QVERIFY2(translationTexts.contains(QStringLiteral("Mon")), qPrintable(diagnostic));
        QVERIFY2(translationTexts.contains(QStringLiteral("Wed")), qPrintable(diagnostic));
        QVERIFY2(translationTexts.contains(QStringLiteral("Fri")), qPrintable(diagnostic));
        // Years remain recognized for diagnostics but never need an API call
        // or destructive patch.
        QVERIFY2(observedTexts.contains(QStringLiteral("2026")), qPrintable(diagnostic));
        QVERIFY2(observedTexts.contains(QStringLiteral("2025")), qPrintable(diagnostic));
        QVERIFY2(!translationTexts.contains(QStringLiteral("2026")), qPrintable(diagnostic));
        QVERIFY2(!translationTexts.contains(QStringLiteral("2025")), qPrintable(diagnostic));
        QVERIFY2(!preservedUnits.isEmpty(), qPrintable(diagnostic));

        const QRect avatarArtworkArea(137, 116, 137, 113);
        const auto translatedAvatarArtwork = std::find_if(
            translationUnits.cbegin(), translationUnits.cend(),
            [&avatarArtworkArea](const Translate::TextBlock& unit) {
                return avatarArtworkArea.contains(unit.box.center());
            });
        QVERIFY2(translatedAvatarArtwork == translationUnits.cend(),
                 qPrintable(diagnostic));

        const QRect smileyArea(300, 240, 50, 50);
        const auto translatedSmiley = std::find_if(
            translationUnits.cbegin(), translationUnits.cend(),
            [&smileyArea](const Translate::TextBlock& unit) {
                return smileyArea.contains(unit.box.center());
            });
        QVERIFY2(translatedSmiley == translationUnits.cend(),
                 qPrintable(diagnostic));
    }

    void ocrDiagnosticsRecordDetectedAndRefinedBoxes()
    {
        struct RestoreDiagnostics {
            QByteArray previous = qgetenv("VISNIP_SAVE_TRANSLATION_IMAGES");
            ~RestoreDiagnostics() {
                if (previous.isNull()) qunsetenv("VISNIP_SAVE_TRANSLATION_IMAGES");
                else qputenv("VISNIP_SAVE_TRANSLATION_IMAGES",previous);
            }
        } restore;
        qunsetenv("VISNIP_SAVE_TRANSLATION_IMAGES");
        QImage privateSample(40,20,QImage::Format_RGB32);privateSample.fill(Qt::white);
        QVERIFY(FastTranslationDiagnostics::begin(privateSample,1,1,privateSample.rect(),
            QStringLiteral("zh-Hans"),QStringLiteral("general-v5"),QStringLiteral("general-v5"),QStringLiteral("test")).isEmpty());
        qputenv("VISNIP_SAVE_TRANSLATION_IMAGES","1");
        QImage source(160, 80, QImage::Format_ARGB32);
        source.fill(Qt::white);
        const QRect detected(20, 18, 120, 28);
        const QRect refined(50, 18, 90, 28);
        const QRect numericBox(20, 55, 50, 18);
        const QVector<OcrTextLine> lines = {
            { refined, QStringLiteral("Account"), 0.99f, detected, true,
              QStringLiteral("repeated-leading-icon-column") },
            { numericBox, QStringLiteral("2026"), 0.99f },
        };
        Translate::TextBlock block;
        block.box = refined;
        block.lineBoxes = { refined };
        block.sourceLineIndices = { 0 };
        block.lines = { QStringLiteral("Account") };
        Translate::TextBlock numeric;
        numeric.box = numericBox;
        numeric.lineBoxes = { numericBox };
        numeric.sourceLineIndices = { 1 };
        numeric.lines = { QStringLiteral("2026") };

        const QString directory = FastTranslationDiagnostics::begin(
            source, 1, 1, source.rect(), QStringLiteral("zh-Hans"),
            QStringLiteral("general-v5"), QStringLiteral("general-v5"),
            QStringLiteral("test-cache-key"));
        QVERIFY(!directory.isEmpty());
        FastTranslationDiagnostics::recordOcr(
            directory, lines, {}, { block, numeric }, { block }, { numeric },
            source);

        QFile file(QDir(directory).filePath(QStringLiteral("analysis.json")));
        QVERIFY(file.open(QIODevice::ReadOnly));
        const QJsonObject root = QJsonDocument::fromJson(file.readAll()).object();
        const QJsonObject line = root.value(QStringLiteral("ocrLines"))
                                     .toArray().first().toObject();
        QCOMPARE(line.value(QStringLiteral("box")).toObject()
                     .value(QStringLiteral("x")).toInt(), refined.x());
        QCOMPARE(line.value(QStringLiteral("detectedBox")).toObject()
                     .value(QStringLiteral("x")).toInt(), detected.x());
        QVERIFY(line.value(QStringLiteral("leadingIconSeparated")).toBool());
        QCOMPARE(line.value(QStringLiteral("refinementReason")).toString(),
                 QStringLiteral("repeated-leading-icon-column"));
        QVERIFY(line.value(QStringLiteral("acceptedForTranslation")).toBool());
        const QJsonObject preservedLine = root.value(QStringLiteral("ocrLines"))
                                              .toArray().at(1).toObject();
        QVERIFY(!preservedLine.value(QStringLiteral("acceptedForTranslation")).toBool());
        QCOMPARE(preservedLine.value(QStringLiteral("preservationReason")).toString(),
                 QStringLiteral("no-linguistic-content"));
        QCOMPARE(root.value(QStringLiteral("translationUnits")).toArray().size(), 1);
        QCOMPARE(root.value(QStringLiteral("blocks")).toArray().size(), 1);
        QCOMPARE(root.value(QStringLiteral("contextBlocks")).toArray().size(), 2);
        QCOMPARE(root.value(QStringLiteral("preservedUnits")).toArray().size(), 1);
        file.close();

        Translate::CompositionReport composition;
        composition.unfitIndices = { 0 };
        FastTranslationDiagnostics::recordResult(
            directory, source, { QStringLiteral("Account") }, composition);
        QVERIFY(file.open(QIODevice::ReadOnly));
        const QJsonObject resultRoot =
            QJsonDocument::fromJson(file.readAll()).object();
        QCOMPARE(resultRoot.value(QStringLiteral("status")).toString(),
                 QStringLiteral("partial"));
        QCOMPARE(resultRoot.value(QStringLiteral("composition")).toObject()
                     .value(QStringLiteral("unfitIndices")).toArray().size(), 1);
        file.close();
        QVERIFY(QDir(directory).removeRecursively());
    }

    void imageTranslateRequestEncodesTheCompleteScreenshot()
    {
        QImage source(64, 36, QImage::Format_ARGB32);
        source.fill(QColor(24, 80, 160));
        QString error;
        QSize uploadedSize;
        const QByteArray payload = ImageTranslationService::buildRequestPayload(
            source, QStringLiteral("zh-Hans"), &error, &uploadedSize);
        QVERIFY2(!payload.isEmpty(), qPrintable(error));
        QCOMPARE(uploadedSize, source.size());

        const QJsonObject request = QJsonDocument::fromJson(payload).object();
        QCOMPARE(request.value(QStringLiteral("source_lang")).toString(),
                 QStringLiteral("auto"));
        QCOMPARE(request.value(QStringLiteral("target_lang")).toString(),
                 QStringLiteral("zh-Hans"));
        QVERIFY(!request.contains(QStringLiteral("image_mime_type")));
        const QByteArray encoded = QByteArray::fromBase64(
            request.value(QStringLiteral("image_base64")).toString().toLatin1());
        const QImage decoded = QImage::fromData(encoded);
        QCOMPARE(decoded.size(), source.size());
        QCOMPARE(decoded.pixelColor(4, 4), source.pixelColor(4, 4));
    }

    void imageTranslateRequestAdaptsNoisyScreenshotsToTheUploadLimit()
    {
        QImage source(1600, 1600, QImage::Format_RGB32);
        quint32 state = 0x5a17c9e3U;
        for (int y = 0; y < source.height(); ++y) {
            auto* row = reinterpret_cast<QRgb*>(source.scanLine(y));
            for (int x = 0; x < source.width(); ++x) {
                state = state * 1664525U + 1013904223U;
                row[x] = qRgb((state >> 16) & 0xff,
                              (state >> 8) & 0xff,
                              state & 0xff);
            }
        }

        QString error;
        QSize uploadedSize;
        const QByteArray payload = ImageTranslationService::buildRequestPayload(
            source, QStringLiteral("zh-Hans"), &error, &uploadedSize);
        QVERIFY2(!payload.isEmpty(), qPrintable(error));
        const QJsonObject request = QJsonDocument::fromJson(payload).object();
        const QByteArray encoded = QByteArray::fromBase64(
            request.value(QStringLiteral("image_base64")).toString().toLatin1());
        QVERIFY(encoded.size() <= ImageTranslationService::kMaxUploadImageBytes);
        const QImage decoded = QImage::fromData(encoded);
        QVERIFY(!decoded.isNull());
        QCOMPARE(decoded.size(), uploadedSize);
        QVERIFY(uploadedSize.width() <= source.width());
        QVERIFY(uploadedSize.height() <= source.height());
    }

    void imageTranslateResponseRequiresAValidSameSizeImage()
    {
        QImage translated(80, 48, QImage::Format_RGB32);
        translated.fill(QColor(230, 235, 240));
        QByteArray png;
        QBuffer buffer(&png);
        QVERIFY(buffer.open(QIODevice::WriteOnly));
        QVERIFY(translated.save(&buffer, "PNG"));

        QJsonObject response;
        response.insert(QStringLiteral("provider"), QStringLiteral("baidu-v1"));
        response.insert(QStringLiteral("source_lang"), QStringLiteral("en"));
        response.insert(QStringLiteral("target_lang"), QStringLiteral("zh"));
        response.insert(QStringLiteral("blocks"), QJsonArray{ QJsonObject{}, QJsonObject{} });
        response.insert(QStringLiteral("elapsed_ms"), 1234);
        response.insert(QStringLiteral("translated_image_mime_type"),
                        QStringLiteral("image/png"));
        response.insert(QStringLiteral("translated_image_base64"),
                        QString::fromLatin1(png.toBase64()));
        const QByteArray body = QJsonDocument(response).toJson(QJsonDocument::Compact);

        QString error;
        const ImageTranslationResult result = ImageTranslationService::parseResponse(
            body, translated.size(), &error);
        QVERIFY2(!result.image.isNull(), qPrintable(error));
        QCOMPARE(result.image.size(), translated.size());
        QCOMPARE(result.provider, QStringLiteral("baidu-v1"));
        QCOMPARE(result.sourceLanguage, QStringLiteral("en"));
        QCOMPARE(result.targetLanguage, QStringLiteral("zh"));
        QCOMPARE(result.blockCount, 2);
        QCOMPARE(result.serverElapsedMs, 1234);

        error.clear();
        QVERIFY(ImageTranslationService::parseResponse(
                    body, QSize(81, 48), &error).image.isNull());
        QVERIFY(error.contains(QStringLiteral("尺寸")));

        response.insert(QStringLiteral("provider"), QStringLiteral("other-provider"));
        error.clear();
        QVERIFY(!ImageTranslationService::parseResponse(
                    QJsonDocument(response).toJson(QJsonDocument::Compact),
                    translated.size(), &error).image.isNull());
        QVERIFY(error.isEmpty());

        response.insert(QStringLiteral("provider"), QStringLiteral(" "));
        QVERIFY(ImageTranslationService::parseResponse(
                    QJsonDocument(response).toJson(QJsonDocument::Compact),
                    translated.size(), &error).image.isNull());
        QVERIFY(error.contains(QStringLiteral("provider")));

        response.insert(QStringLiteral("provider"), QStringLiteral("baidu-v1"));
        response.insert(QStringLiteral("translated_image_base64"),
                        QStringLiteral("not-base64"));
        error.clear();
        QVERIFY(ImageTranslationService::parseResponse(
                    QJsonDocument(response).toJson(QJsonDocument::Compact),
                    translated.size(), &error).image.isNull());
        QVERIFY(error.contains(QStringLiteral("Base64")));
    }

    void cloudImageTranslationLiveSmoke()
    {
        if (!onlineTranslationEnabled()) {
            QSKIP("Online translation is not built in (VISNIP_ONLINE_TRANSLATION=OFF).");
        }
        const QString inputPath = qEnvironmentVariable(
            "VISNIP_LIVE_IMAGE_TRANSLATION_INPUT");
        if (inputPath.isEmpty()) {
            QSKIP("Set VISNIP_LIVE_IMAGE_TRANSLATION_INPUT to run the live service smoke test.");
        }
        const QImage source(inputPath);
        QVERIFY2(!source.isNull(), qPrintable(inputPath));

        AppConfig config;
        config.mutableSettings().aiTranslate.translationMethod =
            TranslationMethod::CloudBaidu;
        config.mutableSettings().aiTranslate.cloudUploadConsent = AiTranslateSettings::kUploadNoticeVersion;
        config.mutableSettings().aiTranslate.timeoutSeconds = 150;
        const QString serviceUrl = qEnvironmentVariable(
            "VISNIP_LIVE_IMAGE_TRANSLATION_SERVICE_URL");
        if (!serviceUrl.isEmpty()) {
            config.mutableSettings().aiTranslate.fastServiceUrl = serviceUrl;
        }

        qRegisterMetaType<ImageTranslationResult>();
        ImageTranslationService service(&config);
        QSignalSpy succeeded(&service, &ImageTranslationService::succeeded);
        QSignalSpy failed(&service, &ImageTranslationService::failed);
        service.translate(source, QStringLiteral("zh-Hans"));
        QTRY_VERIFY_WITH_TIMEOUT(!succeeded.isEmpty() || !failed.isEmpty(), 160000);
        QVERIFY2(failed.isEmpty(),
                 failed.isEmpty()
                     ? ""
                     : qPrintable(failed.first().first().toString()));
        QCOMPARE(succeeded.size(), 1);
        const ImageTranslationResult result =
            qvariant_cast<ImageTranslationResult>(succeeded.first().first());
        QCOMPARE(result.image.size(), source.size());
        QCOMPARE(result.provider, QStringLiteral("baidu-v1"));
        QVERIFY(result.blockCount > 0);

        const QString outputPath = qEnvironmentVariable(
            "VISNIP_LIVE_IMAGE_TRANSLATION_OUTPUT");
        if (!outputPath.isEmpty()) {
            QVERIFY2(result.image.save(outputPath), qPrintable(outputPath));
        }
    }

    void translateRequestNeverSilentlyTruncatesSourceText()
    {
        const QString overlong(TextTranslationService::kMaxCharsPerText + 40, QLatin1Char('a'));
        const QByteArray payload = TextTranslationService::buildRequestPayload(
            { QStringLiteral("文件下载完成"), overlong }, QStringLiteral("en"));

        const QJsonObject body = QJsonDocument::fromJson(payload).object();
        QCOMPARE(body.value(QStringLiteral("target_lang")).toString(), QStringLiteral("en"));
        QCOMPARE(body.value(QStringLiteral("source_lang")).toString(), QStringLiteral("auto"));
        const QJsonArray texts = body.value(QStringLiteral("texts")).toArray();
        QCOMPARE(texts.size(), 2);
        QCOMPARE(texts.at(0).toString(), QStringLiteral("文件下载完成"));
        QCOMPARE(texts.at(1).toString(), overlong);

        AppConfig config;
        TextTranslationService service(&config);
        QSignalSpy failed(&service, &TextTranslationService::failed);
        service.translate({ overlong }, QStringLiteral("en"));
        QCOMPARE(failed.size(), 1);
        QVERIFY(failed.first().first().toString().contains(
            QString::number(TextTranslationService::kMaxCharsPerText)));
    }

    void translateResponseParsesTranslationsAndErrors()
    {
        QString error;
        const QStringList ok = TextTranslationService::parseTranslations(
            QByteArray(R"({"translations":["The file download is completed.","The quick brown fox"],)"
                       R"("detected_langs":["zh","en"],"elapsed_ms":1807})"),
            &error);
        QCOMPARE(ok.size(), 2);
        QCOMPARE(ok.at(0), QStringLiteral("The file download is completed."));
        QVERIFY(error.isEmpty());

        error.clear();
        const QStringList rejected = TextTranslationService::parseTranslations(
            QByteArray(R"({"detail":"texts must contain at most 64 items"})"), &error);
        QVERIFY(rejected.isEmpty());
        QVERIFY2(error.contains(QStringLiteral("at most 64 items")), qPrintable(error));

        error.clear();
        const QStringList garbage = TextTranslationService::parseTranslations(
            QByteArray("<html>404</html>"), &error);
        QVERIFY(garbage.isEmpty());
        QVERIFY(!error.isEmpty());

        error.clear();
        const QStringList mismatched = TextTranslationService::parseTranslations(
            QByteArray(R"({"translations":["one"]})"), &error, 2);
        QVERIFY(mismatched.isEmpty());
        QVERIFY(error.contains(QStringLiteral("数量不匹配")));

        error.clear();
        const QStringList invalidElement = TextTranslationService::parseTranslations(
            QByteArray(R"({"translations":["one",null]})"), &error, 2);
        QVERIFY(invalidElement.isEmpty());
        QVERIFY(error.contains(QStringLiteral("类型无效")));
    }

    void simplifiedChineseTargetNormalizesServiceOutput()
    {
        QCOMPARE(TextTranslationService::normalizeTranslationForTarget(
                     QStringLiteral("我們的 changelog 最新消息；LiveAgent v1.2.0 發表"),
                     QStringLiteral("zh-Hans")),
                 QStringLiteral("我们的 changelog 最新消息；LiveAgent v1.2.0 发表"));
        QCOMPARE(TextTranslationService::normalizeTranslationForTarget(
                     QStringLiteral("我們的 changelog 最新消息"),
                     QStringLiteral("zh-Hant")),
                 QStringLiteral("我們的 changelog 最新消息"));
        QCOMPARE(TextTranslationService::normalizeTranslationForTarget(
                     QStringLiteral("我們的 changelog 最新消息"),
                     QStringLiteral("en")),
                 QStringLiteral("我們的 changelog 最新消息"));
    }

    void standalonePersonalNamesStayUntranslated()
    {
        QCOMPARE(TextTranslationService::preservePersonalName(
                     QStringLiteral("Avery Lindqvist"),
                     QStringLiteral("艾弗里·林奎斯特")),
                 QStringLiteral("Avery Lindqvist"));
        QCOMPARE(TextTranslationService::preservePersonalName(
                     QStringLiteral("Avery Lindqvist (averyl)"),
                     QStringLiteral("艾弗里·林奎斯特（艾弗里尔）")),
                 QStringLiteral("Avery Lindqvist (averyl)"));
        QCOMPARE(TextTranslationService::preservePersonalName(
                     QStringLiteral("Your personal account"),
                     QStringLiteral("您的个人账户")),
                 QStringLiteral("您的个人账户"));
        QCOMPARE(TextTranslationService::preservePersonalName(
                     QStringLiteral("Public Profile"),
                     QStringLiteral("公开资料")),
                 QStringLiteral("公开资料"));
    }

    void translatedNamesKeepParenthesizedIdentifiers()
    {
        QCOMPARE(TextTranslationService::restoreProtectedIdentifiers(
                     QStringLiteral("Avery Lindqvist (averyl)"),
                     QStringLiteral("艾弗里·林奎斯特（艾弗里尔）")),
                 QStringLiteral("艾弗里·林奎斯特（艾弗里尔） (averyl)"));
        QCOMPARE(TextTranslationService::restoreProtectedIdentifiers(
                     QStringLiteral("Avery Lindqvist (averyl)"),
                     QStringLiteral("艾弗里·林奎斯特。")),
                 QStringLiteral("艾弗里·林奎斯特 (averyl)。"));
        QCOMPARE(TextTranslationService::restoreProtectedIdentifiers(
                     QStringLiteral("Release notes (version 2)"),
                     QStringLiteral("版本 2 的发行说明")),
                 QStringLiteral("版本 2 的发行说明"));
        QCOMPARE(TextTranslationService::restoreProtectedIdentifiers(
                     QStringLiteral("Avery Lindqvist"),
                     QStringLiteral("艾弗里·林奎斯特")),
                 QStringLiteral("艾弗里·林奎斯特"));
    }

    void baiduRequestMatchesTheOfficialContract()
    {
        const QStringList texts = { QStringLiteral("hello"), QStringLiteral("world") };
        QString error;
        const QByteArray payload = TextTranslationService::buildBaiduRequestPayload(
            texts,
            QStringLiteral("zh-Hans"),
            QStringLiteral("app"),
            QStringLiteral("secret"),
            QStringLiteral("20260727"),
            &error);
        QVERIFY2(!payload.isEmpty(), qPrintable(error));

        const QUrlQuery form(QString::fromUtf8(payload));
        QCOMPARE(form.queryItemValue(QStringLiteral("q"), QUrl::FullyDecoded),
                 QStringLiteral("hello\nworld"));
        QCOMPARE(form.queryItemValue(QStringLiteral("from")), QStringLiteral("auto"));
        QCOMPARE(form.queryItemValue(QStringLiteral("to")), QStringLiteral("zh"));
        QCOMPARE(form.queryItemValue(QStringLiteral("appid")), QStringLiteral("app"));
        QCOMPARE(form.queryItemValue(QStringLiteral("salt")), QStringLiteral("20260727"));
        QCOMPARE(form.queryItemValue(QStringLiteral("sign")),
                 QStringLiteral("806691e9cf973c6721379f302c1451dd"));
        QVERIFY(!payload.contains("secret"));
    }

    void baiduRequestPercentEncodesFormDelimiters()
    {
        const QString query = QStringLiteral(
            "bypass permissions on (shift+tab to cycle) · ctrl+t & review=a/b");
        QString error;
        const QByteArray payload = TextTranslationService::buildBaiduRequestPayload(
            { query },
            QStringLiteral("zh-Hans"),
            QStringLiteral(" app+id "),
            QStringLiteral("secret"),
            QStringLiteral("salt+value"),
            &error);
        QVERIFY2(!payload.isEmpty(), qPrintable(error));
        QVERIFY(payload.contains("shift%2Btab"));
        QVERIFY(payload.contains("ctrl%2Bt"));
        QVERIFY(payload.contains("%26%20review%3Da%2Fb"));
        QVERIFY(payload.contains("appid=app%2Bid"));
        QVERIFY(payload.contains("salt=salt%2Bvalue"));
        QVERIFY(!payload.contains("shift+tab"));
        QVERIFY(!payload.contains("ctrl+t"));

        const QUrlQuery form(QString::fromUtf8(payload));
        QCOMPARE(form.queryItemValue(QStringLiteral("q"), QUrl::FullyDecoded), query);
        QCOMPARE(form.queryItemValue(QStringLiteral("appid"), QUrl::FullyDecoded),
                 QStringLiteral("app+id"));
        QCOMPARE(form.queryItemValue(QStringLiteral("salt"), QUrl::FullyDecoded),
                 QStringLiteral("salt+value"));
        QCOMPARE(form.queryItemValue(QStringLiteral("sign")),
                 QStringLiteral("40bf3f89a7d8ab0a8e585d3665a9db2d"));
    }

    void baiduLanguageCodesMatchTheProvider()
    {
        QCOMPARE(TextTranslationService::baiduLanguageCode(QStringLiteral("zh-Hans")),
                 QStringLiteral("zh"));
        QCOMPARE(TextTranslationService::baiduLanguageCode(QStringLiteral("zh-Hant")),
                 QStringLiteral("cht"));
        QCOMPARE(TextTranslationService::baiduLanguageCode(QStringLiteral("ja")),
                 QStringLiteral("jp"));
        QCOMPARE(TextTranslationService::baiduLanguageCode(QStringLiteral("ko")),
                 QStringLiteral("kor"));
        QCOMPARE(TextTranslationService::baiduLanguageCode(QStringLiteral("fr")),
                 QStringLiteral("fra"));
        QCOMPARE(TextTranslationService::baiduLanguageCode(QStringLiteral("es")),
                 QStringLiteral("spa"));
        QCOMPARE(TextTranslationService::baiduLanguageCode(QStringLiteral("ar")),
                 QStringLiteral("ara"));
        QCOMPARE(TextTranslationService::baiduLanguageCode(QStringLiteral("vi")),
                 QStringLiteral("vie"));
    }

    void baiduResponseParsesTranslationsAndErrors()
    {
        QString error;
        const QStringList translated = TextTranslationService::parseBaiduTranslations(
            QByteArray(R"({"from":"en","to":"zh","trans_result":[)"
                       R"({"src":"hello","dst":"你好"},{"src":"world","dst":"世界"}]})"),
            { QStringLiteral("hello"), QStringLiteral("world") },
            &error);
        QCOMPARE(translated, QStringList({ QStringLiteral("你好"), QStringLiteral("世界") }));
        QVERIFY(error.isEmpty());

        error.clear();
        const QStringList insufficient = TextTranslationService::parseBaiduTranslations(
            QByteArray(R"({"trans_result":[{"src":"hello","dst":"你好"}]})"),
            { QStringLiteral("hello"), QStringLiteral("world") }, &error);
        QVERIFY(insufficient.isEmpty());
        QVERIFY(error.contains(QStringLiteral("数量")));

        error.clear();
        const QStringList reordered = TextTranslationService::parseBaiduTranslations(
            QByteArray(R"({"trans_result":[{"src":"world","dst":"世界"},)"
                       R"({"src":"hello","dst":"你好"}]})"),
            { QStringLiteral("hello"), QStringLiteral("world") }, &error);
        QVERIFY(reordered.isEmpty());
        QVERIFY(error.contains(QStringLiteral("原文与请求不匹配")));

        error.clear();
        const QStringList blank = TextTranslationService::parseBaiduTranslations(
            QByteArray(R"({"trans_result":[{"src":"hello","dst":"   "}]})"),
            { QStringLiteral("hello") }, &error);
        QVERIFY(blank.isEmpty());
        QVERIFY(error.contains(QStringLiteral("空译文")));

        error.clear();
        const QStringList noBalance = TextTranslationService::parseBaiduTranslations(
            QByteArray(R"({"error_code":"54004","error_msg":"Insufficient balance"})"),
            { QStringLiteral("hello") },
            &error);
        QVERIFY(noBalance.isEmpty());
        QVERIFY(error.contains(QStringLiteral("余额不足")));
    }

private:
    QTemporaryDir settingsDir_;
};

QTEST_MAIN(UiTests)
#include "tst_ui.moc"
