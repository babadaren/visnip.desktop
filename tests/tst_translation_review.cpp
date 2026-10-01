#include "ui/widgets/TranslationReviewPanel.h"
#include <QtTest>

using namespace Visnip;

class TranslationReviewTests : public QObject {
    Q_OBJECT
private slots:
    void validUnplacedTextOnlyOpensAfterExplicitUserAction()
    {
        QWidget host; host.resize(1000, 700); host.show();
        TranslationReviewPanel panel(&host);
        QJsonArray blocks{QJsonObject{{"source", "Manage settings"}, {"translation", "管理设置"},
            {"status", "preserved"}, {"reason", "neighbor_collision"}, {"display_mode", "panel"},
            {"translation_status", "translated"}}};
        panel.setResult(blocks, QRect(100, 100, 500, 80));
        panel.setTranslationVisible(true);
        auto* text=panel.findChild<QPlainTextEdit*>(QStringLiteral("VisnipTranslationReviewText"));
        auto* toggle=panel.findChild<QToolButton*>(QStringLiteral("VisnipTranslationReviewToggle"));
        QVERIFY(text && toggle && !panel.isVisible() && !text->isVisible());
        panel.toggleByUser();
        QVERIFY(panel.isVisible() && text->isVisible());
        QVERIFY(text->toPlainText().contains(QStringLiteral("管理设置")));
        toggle->click(); QVERIFY(!text->isVisible());
        toggle->click(); QVERIFY(text->isVisible());
        panel.setTranslationVisible(false); QVERIFY(!panel.isVisible());
        panel.setTranslationVisible(true); QVERIFY(!panel.isVisible());
        panel.toggleByUser(); QVERIFY(text->isVisible());
        panel.clear(); QVERIFY(!panel.isVisible()); QVERIFY(text->toPlainText().isEmpty());
    }

    void fullyInlineResultStartsCompletelyHidden()
    {
        QWidget host; host.resize(1000, 700); host.show();
        TranslationReviewPanel panel(&host);
        panel.setResult(QJsonArray{QJsonObject{{"source", "Code"}, {"translation", "代码"},
            {"status", "applied"}, {"translation_status", "translated"}}}, QRect(10, 10, 300, 90));
        panel.setTranslationVisible(true);
        auto* text=panel.findChild<QPlainTextEdit*>(QStringLiteral("VisnipTranslationReviewText"));
        QVERIFY(!panel.isVisible()); QVERIFY(!text->isVisible());
        panel.setAnchor(QRect(100, 100, 300, 90));
        host.resize(900, 600); QCoreApplication::processEvents();
        QVERIFY(!panel.isVisible());
        panel.toggleByUser();
        QVERIFY(text->isVisible());
        panel.toggleByUser();
        QVERIFY(!panel.isVisible());
    }

    void clipboardRequiresClickAndSourceIsPlainText()
    {
        QWidget host; host.resize(1000, 700); host.show();
        TranslationReviewPanel panel(&host);
        const QString original=QApplication::clipboard()->text();
        panel.setResult(QJsonArray{QJsonObject{{"source", "<script>sample</script>"},
            {"translation", "完整译文 <b>不是HTML</b>"}, {"status", "preserved"},
            {"reason", "layout_unfit"}, {"translation_status", "translated"}}}, QRect(20, 20, 200, 50));
        panel.setTranslationVisible(true);
        QCOMPARE(QApplication::clipboard()->text(),original);
        QVERIFY(!panel.isVisible());
        panel.toggleByUser();
        auto* text=panel.findChild<QPlainTextEdit*>(QStringLiteral("VisnipTranslationReviewText"));
        QVERIFY(text->toPlainText().contains(QStringLiteral("<script>sample</script>")));
        panel.findChild<QPushButton*>(QStringLiteral("VisnipTranslationReviewCopy"))->click();
        QCOMPARE(QApplication::clipboard()->text(),QStringLiteral("完整译文 <b>不是HTML</b>"));
        QApplication::clipboard()->setText(original);
    }

    void failedTranslationIsNotCopiedAsSuccess()
    {
        QWidget host; host.resize(600, 500); host.show();
        TranslationReviewPanel panel(&host);
        panel.setResult(QJsonArray{QJsonObject{{"source", "Open this"}, {"translation", ""},
            {"status", "preserved"}, {"translation_status", "failed"}, {"reason", "sentence_not_translated"}}},
            QRect(200, 100, 200, 100));
        panel.setTranslationVisible(true);
        QVERIFY(!panel.isVisible());
        QVERIFY(!panel.findChild<QPushButton*>(QStringLiteral("VisnipTranslationReviewCopy"))->isEnabled());
        QVERIFY(panel.findChild<QPlainTextEdit*>(QStringLiteral("VisnipTranslationReviewText"))->toPlainText()
            .contains(QStringLiteral("未获得合格译文")));
    }

    void repositionWithinViewportAndDoNotUseIdentityAsTranslation()
    {
        QWidget host; host.resize(800, 600); host.show();
        TranslationReviewPanel panel(&host);
        panel.setResult(QJsonArray{QJsonObject{{"source", "account/name"}, {"translation", "account/name"},
            {"status", "preserved"}, {"reason", "equivalent"}},
            QJsonObject{{"source", "Exit"}, {"translation", "退出"}, {"status", "preserved"}, {"reason", "artwork_collision"}}},
            QRect(700, 500, 90, 80));
        panel.setTranslationVisible(true);
        QVERIFY(!panel.isVisible());
        panel.toggleByUser();
        QVERIFY(host.rect().contains(panel.geometry()));
        panel.setAnchor(QRect(780, 10, 10, 20));
        QVERIFY(host.rect().contains(panel.geometry()));
        host.resize(540, 380); QCoreApplication::processEvents();
        QVERIFY(host.rect().contains(panel.geometry()));
        QVERIFY(!panel.findChild<QPlainTextEdit*>(QStringLiteral("VisnipTranslationReviewText"))->toPlainText().contains(QStringLiteral("account/name")));
    }

    void newResultNeverInheritsAnOpenPanel()
    {
        QWidget host; host.resize(800, 600); host.show();
        TranslationReviewPanel panel(&host);
        const QJsonArray blocks{QJsonObject{{"source", "Exit"}, {"translation", "退出"},
            {"status", "preserved"}, {"reason", "artwork_collision"}}};
        panel.setResult(blocks, QRect(20, 20, 180, 90));
        panel.setTranslationVisible(true);
        panel.toggleByUser(); QVERIFY(panel.isVisible());
        panel.setResult(blocks, QRect(40, 40, 180, 90));
        panel.setTranslationVisible(true);
        QVERIFY(!panel.isVisible());
        panel.clear();
        panel.setTranslationVisible(true); panel.toggleByUser();
        QVERIFY(!panel.isVisible() && !panel.hasResult());
    }
};
QTEST_MAIN(TranslationReviewTests)
#include "tst_translation_review.moc"
