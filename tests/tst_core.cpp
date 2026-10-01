#include "core/AnnotationModel.h"
#include "core/AppConfig.h"
#include "core/DesignTokens.h"
#include "core/LongCaptureMatch.h"
#include "core/OcrLanguagePack.h"
#include "core/OcrPostProcess.h"
#include "core/OutputFilenamePattern.h"
#include "core/TranslationCompositor.h"
#include "core/TranslationLanguage.h"

#include <QPainter>
#include <QPainterPath>
#include <QSettings>
#include <QTemporaryDir>
#include <QtTest/QtTest>

namespace {

// A tall synthetic "document" with aperiodic per-row texture so row
// signatures are unambiguous, similar to a real page of text.
QImage makeDocumentImage(int width, int height)
{
    QImage image(width, height, QImage::Format_ARGB32);
    image.fill(Qt::white);
    QPainter painter(&image);
    for (int y = 8; y < height; y += 18) {
        const quint32 hash = static_cast<quint32>(y) * 2654435761u;
        const int shade = 30 + static_cast<int>(hash % 190u);
        const int left = 4 + static_cast<int>((hash >> 8) % 48u);
        const int reduce = static_cast<int>((hash >> 16) % 64u);
        painter.fillRect(QRect(left, y, qMax(24, width - left - 12 - reduce), 9),
                         QColor(shade, (shade * 3) % 255, 255 - shade));
    }
    painter.end();
    return image;
}

QImage makeNumberedDocumentImage(int width, int firstValue, int rows)
{
    constexpr int rowHeight = 36;
    constexpr int pixelSize = 3;
    static constexpr quint16 digitMasks[] = {
        0b111101101101111,
        0b010110010010111,
        0b111001111100111,
        0b111001111001111,
        0b101101111001001,
        0b111100111001111,
        0b111100111101111,
        0b111001001001001,
        0b111101111101111,
        0b111101111001111,
    };
    QImage image(width, rows * rowHeight, QImage::Format_ARGB32);
    image.fill(Qt::white);
    QPainter painter(&image);
    painter.setPen(Qt::NoPen);
    painter.setBrush(Qt::black);
    for (int row = 0; row < rows; ++row) {
        const QString text = QString::number(firstValue + row);
        const int glyphAdvance = 4 * pixelSize;
        const int textWidth = text.size() * glyphAdvance - pixelSize;
        const int textLeft = (width - textWidth) / 2;
        const int textTop = row * rowHeight + (rowHeight - 5 * pixelSize) / 2;
        for (int digitIndex = 0; digitIndex < text.size(); ++digitIndex) {
            const quint16 mask = digitMasks[text.at(digitIndex).digitValue()];
            for (int bit = 0; bit < 15; ++bit) {
                if ((mask & (1u << (14 - bit))) == 0) {
                    continue;
                }
                painter.drawRect(textLeft + digitIndex * glyphAdvance + (bit % 3) * pixelSize,
                                 textTop + (bit / 3) * pixelSize,
                                 pixelSize,
                                 pixelSize);
            }
        }
    }
    painter.end();
    return image;
}

} // namespace

class CoreTests : public QObject {
    Q_OBJECT
private slots:
    void legacySettingsAreCopiedOnlyIntoAnEmptyStore()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        QSettings legacy(dir.filePath(QStringLiteral("legacy.ini")), QSettings::IniFormat);
        legacy.setValue(QStringLiteral("general/autoStart"), true);
        legacy.setValue(QStringLiteral("aiTranslate/offlineResourceDirectory"), QStringLiteral("C:/old/offline/1"));
        QSettings fresh(dir.filePath(QStringLiteral("fresh.ini")), QSettings::IniFormat);
        QVERIFY(Visnip::copyLegacySettings(legacy, fresh));
        QCOMPARE(fresh.value(QStringLiteral("general/autoStart")).toBool(), true);
        QCOMPARE(fresh.value(QStringLiteral("aiTranslate/offlineResourceDirectory")).toString(),
                 QStringLiteral("C:/old/offline/1"));

        // Settings that already exist are never overwritten.
        legacy.setValue(QStringLiteral("general/autoStart"), false);
        QVERIFY(!Visnip::copyLegacySettings(legacy, fresh));
        QCOMPARE(fresh.value(QStringLiteral("general/autoStart")).toBool(), true);

        QSettings empty(dir.filePath(QStringLiteral("empty.ini")), QSettings::IniFormat);
        QSettings untouched(dir.filePath(QStringLiteral("untouched.ini")), QSettings::IniFormat);
        QVERIFY(!Visnip::copyLegacySettings(empty, untouched));
        QVERIFY(untouched.allKeys().isEmpty());
    }

    void toolbarTokensMatchCompactSpec()
    {
        const auto& t = Visnip::Design::toolbar();
        QCOMPARE(t.height, 30);
        QCOMPARE(t.button, 26);
        QCOMPARE(t.icon, 23);
        QCOMPARE(t.radius, 9);
        QCOMPARE(t.buttonRadius, 6);
    }

    void annotationUndoRedoWorks()
    {
        Visnip::AnnotationDocument doc;
        const quint64 initialRevision = doc.revision();
        Visnip::AnnotationItem item;
        item.type = Visnip::AnnotationType::Rectangle;
        item.rect = QRectF(1, 2, 30, 40);
        doc.add(item);
        QVERIFY(doc.revision() > initialRevision);
        QCOMPARE(doc.items().size(), 1);
        QVERIFY(doc.canUndo());
        QVERIFY(doc.undo());
        const quint64 undoRevision = doc.revision();
        QCOMPARE(doc.items().size(), 0);
        QVERIFY(doc.canRedo());
        QVERIFY(doc.redo());
        QVERIFY(doc.revision() > undoRevision);
        QCOMPARE(doc.items().size(), 1);
    }

    void annotationTranslatePreservesHistoryAndUpdatesRevision()
    {
        Visnip::AnnotationDocument doc;

        Visnip::AnnotationItem committed;
        committed.type = Visnip::AnnotationType::Rectangle;
        committed.rect = QRectF(1.25, 2.5, 30, 40);
        committed.points = {QPointF(3.5, 4.75), QPointF(8.25, 9.5)};
        doc.add(committed);

        Visnip::AnnotationItem undone;
        undone.type = Visnip::AnnotationType::Arrow;
        undone.rect = QRectF(12.5, 18.25, 6, 8);
        undone.points = {QPointF(10.25, 20.5), QPointF(40.75, 55.25)};
        doc.add(undone);
        QVERIFY(doc.undo());

        const auto originalItems = doc.items();
        const auto originalRedoItems = doc.redoItems();
        const quint64 revision = doc.revision();

        doc.translate(QPointF());
        QCOMPARE(doc.revision(), revision);
        QCOMPARE(doc.items().at(0).rect, originalItems.at(0).rect);
        QCOMPARE(doc.items().at(0).points, originalItems.at(0).points);
        QCOMPARE(doc.redoItems().at(0).rect, originalRedoItems.at(0).rect);
        QCOMPARE(doc.redoItems().at(0).points, originalRedoItems.at(0).points);

        const QPointF delta(17.25, -9.5);
        doc.translate(delta);
        QVERIFY(doc.revision() > revision);
        const quint64 translatedRevision = doc.revision();
        QCOMPARE(doc.items().at(0).rect, originalItems.at(0).rect.translated(delta));
        QCOMPARE(doc.items().at(0).points.at(0), originalItems.at(0).points.at(0) + delta);
        QCOMPARE(doc.items().at(0).points.at(1), originalItems.at(0).points.at(1) + delta);
        QCOMPARE(doc.redoItems().at(0).rect, originalRedoItems.at(0).rect.translated(delta));
        QCOMPARE(doc.redoItems().at(0).points.at(0), originalRedoItems.at(0).points.at(0) + delta);
        QCOMPARE(doc.redoItems().at(0).points.at(1), originalRedoItems.at(0).points.at(1) + delta);

        doc.translate(-delta);
        QVERIFY(doc.revision() > translatedRevision);
        QCOMPARE(doc.items().at(0).rect, originalItems.at(0).rect);
        QCOMPARE(doc.items().at(0).points, originalItems.at(0).points);
        QCOMPARE(doc.redoItems().at(0).rect, originalRedoItems.at(0).rect);
        QCOMPARE(doc.redoItems().at(0).points, originalRedoItems.at(0).points);

        QVERIFY(doc.redo());
        QCOMPARE(doc.items().at(1).rect, undone.rect);
        QCOMPARE(doc.items().at(1).points, undone.points);
    }

    void mosaicChangesPixels()
    {
        QImage image(16, 16, QImage::Format_ARGB32);
        for (int y = 0; y < image.height(); ++y) {
            for (int x = 0; x < image.width(); ++x) {
                image.setPixelColor(x, y, QColor(x * 10, y * 10, 32));
            }
        }
        const QColor before = image.pixelColor(1, 1);
        Visnip::applyMosaic(image, QRect(0, 0, 8, 8), 8);
        const QColor after = image.pixelColor(1, 1);
        QVERIFY(before != after);
    }

    void eraserRestoresSourcePixels()
    {
        QImage source(12, 12, QImage::Format_ARGB32);
        source.fill(QColor(20, 40, 60));
        QImage image = source.copy();
        image.fill(QColor(240, 80, 60));

        Visnip::AnnotationItem eraser;
        eraser.type = Visnip::AnnotationType::Eraser;
        eraser.rect = QRectF(2, 2, 5, 5);
        eraser.style.mosaicPaintMode = Visnip::MosaicPaintMode::Fill;
        Visnip::applyEraserAnnotation(image, source, eraser);

        QCOMPARE(image.pixelColor(3, 3), source.pixelColor(3, 3));
        QVERIFY(image.pixelColor(0, 0) != source.pixelColor(0, 0));
    }

    void configDefaultsAreUsable()
    {
        Visnip::AppConfig config;
        QVERIFY(config.settings().capture.showMagnifier);
        QVERIFY(config.settings().capture.showCursorColor);
        QVERIFY(config.settings().capture.showSizeLabel);
        QVERIFY(config.settings().capture.maskColor.alpha() >= 112);
        QCOMPARE(Visnip::keySequenceToText(config.settings().hotkeys.capture), QStringLiteral("F1"));
        QVERIFY(config.defaultSaveDirectory().contains(QStringLiteral("Visnip")));
        QCOMPARE(config.settings().aiTranslate.fastOcrPackId,
                 Visnip::Ocr::defaultLanguagePackId());
        QCOMPARE(config.settings().aiTranslate.translationMethod,
                 Visnip::TranslationMethod::Offline);
        QVERIFY(config.settings().aiTranslate.usesLiteOfflineEngine());
#if !VISNIP_ONLINE_TRANSLATION
        QVERIFY(!Visnip::onlineTranslationEnabled());
        QVERIFY(Visnip::aiTranslateDefaultFastServiceUrl().isEmpty());
#endif
        QVERIFY(!config.settings().aiTranslate.allowsTranslationNetwork());
        QVERIFY(!config.settings().aiTranslate.fastOcrPackCacheKey().isEmpty());
    }

    void translationLanguageCatalogUsesTheTwoMethodIntersection()
    {
        const auto& languages = Visnip::supportedTranslationLanguages();
        QCOMPARE(languages.size(), 10);
        QVERIFY(Visnip::isSupportedTranslationLanguage(QStringLiteral("zh-Hans")));
        QVERIFY(Visnip::isSupportedTranslationLanguage(QStringLiteral("en")));
        QVERIFY(!Visnip::isSupportedTranslationLanguage(QStringLiteral("zh-Hant")));
        QCOMPARE(Visnip::normalizedTranslationLanguageCode(QStringLiteral("ja")),
                 QStringLiteral("ja"));
        QCOMPARE(Visnip::normalizedTranslationLanguageCode(QStringLiteral("zh-Hant")),
                 QStringLiteral("zh-Hans"));
    }

    void longCaptureQuietPeriodUsesLatestObservedActivity()
    {
        using Visnip::LongCapture::remainingQuietPeriodMs;

        QCOMPARE(remainingQuietPeriodMs(1000, -1, -1, 160), 0);
        QCOMPARE(remainingQuietPeriodMs(1000, 800, 700, 160), 0);
        QCOMPARE(remainingQuietPeriodMs(1000, 900, 940, 160), 100);
        QCOMPARE(remainingQuietPeriodMs(1000, 1100, 900, 160), 160);
        QCOMPARE(remainingQuietPeriodMs(1000, 990, 995, 0), 0);
    }

    void longCaptureMatchFindsDownwardScroll()
    {
        const QImage document = makeDocumentImage(320, 1600);
        const QImage frameA = document.copy(0, 0, 320, 400);
        const QImage frameB = document.copy(0, 260, 320, 400);

        const auto docSig = Visnip::LongCapture::computeRowSignature(frameA);
        const auto frameSig = Visnip::LongCapture::computeRowSignature(frameB);
        const auto match = Visnip::LongCapture::matchSignatureInDocument(
            docSig, 0, frameSig, -400, 400, 48);
        QVERIFY(match.valid);
        QCOMPARE(match.documentY, 260);
        QVERIFY2(match.confidence > 0.5,
                 qPrintable(QStringLiteral("confidence=%1 score=%2 overlap=%3")
                                .arg(match.confidence)
                                .arg(match.score)
                                .arg(match.overlap)));
    }

    void longCaptureMatchFindsUpwardScroll()
    {
        const QImage document = makeDocumentImage(320, 1600);
        const QImage frameA = document.copy(0, 700, 320, 400);
        const QImage frameB = document.copy(0, 430, 320, 400);

        const auto docSig = Visnip::LongCapture::computeRowSignature(frameA);
        const auto frameSig = Visnip::LongCapture::computeRowSignature(frameB);
        // Document top of the reference frame is 700 in document coordinates.
        const auto match = Visnip::LongCapture::matchSignatureInDocument(
            docSig, 700, frameSig, 300, 1100, 48);
        QVERIFY(match.valid);
        QCOMPARE(match.documentY, 430);
        QVERIFY2(match.confidence > 0.5,
                 qPrintable(QStringLiteral("confidence=%1 score=%2 overlap=%3")
                                .arg(match.confidence)
                                .arg(match.score)
                                .arg(match.overlap)));
    }

    void longCaptureMatchWorksWithCoarseStep()
    {
        const QImage document = makeDocumentImage(320, 2400);
        const QImage frameA = document.copy(0, 0, 320, 500);
        const QImage frameB = document.copy(0, 353, 320, 500);

        const auto docSig = Visnip::LongCapture::computeRowSignature(frameA);
        const auto frameSig = Visnip::LongCapture::computeRowSignature(frameB);
        const auto match = Visnip::LongCapture::matchSignatureInDocument(
            docSig, 0, frameSig, -500, 500, 48, 4);
        QVERIFY(match.valid);
        QCOMPARE(match.documentY, 353);
    }

    void longCaptureMatchRejectsFlatContent()
    {
        QImage flat(320, 400, QImage::Format_ARGB32);
        flat.fill(Qt::white);
        const auto docSig = Visnip::LongCapture::computeRowSignature(flat);
        const auto frameSig = Visnip::LongCapture::computeRowSignature(flat);
        const auto match = Visnip::LongCapture::matchSignatureInDocument(
            docSig, 0, frameSig, -300, 300, 48);
        // Featureless content must not produce a confident offset.
        QVERIFY(!match.valid || match.confidence < 0.35);
    }

    void longCaptureMatchRejectsTinyExactOverlap()
    {
        Visnip::LongCapture::RowSignature document;
        Visnip::LongCapture::RowSignature frame;
        document.luma.resize(120);
        document.edge.fill(4.0f, 120);
        frame.luma.resize(120);
        frame.edge.fill(4.0f, 120);
        for (int i = 0; i < 120; ++i) {
            document.luma[i] = static_cast<float>((i * 73 + i * i * 11) % 251);
            frame.luma[i] = static_cast<float>((i * 47 + i * i * 19 + 31) % 251);
        }
        for (int i = 0; i < 20; ++i) {
            frame.luma[i] = document.luma[100 + i];
        }

        const auto match = Visnip::LongCapture::matchSignatureInDocument(
            document, 0, frame, 100, 100, 20);
        QVERIFY(match.valid);
        QCOMPARE(match.overlap, 20);
        QVERIFY(match.confidence < 0.35);
    }

    void longCaptureMatchRejectsPeriodicFrameWithoutOverlap()
    {
        constexpr int frameHeight = 550;
        const QImage document = makeNumberedDocumentImage(320, 390, 80);
        const QImage current = document.copy(0, 0, 320, frameHeight);
        const QImage jumped = document.copy(0, 720, 320, frameHeight);
        const auto currentSignature = Visnip::LongCapture::computeRowSignature(current);
        const auto jumpedSignature = Visnip::LongCapture::computeRowSignature(jumped);
        QVERIFY(Visnip::LongCapture::signatureDifference(currentSignature,
                                                          0,
                                                          jumpedSignature,
                                                          0,
                                                          frameHeight)
                > 1.5);

        const auto match = Visnip::LongCapture::matchSignatureInDocument(
            currentSignature,
            0,
            jumpedSignature,
            -17,
            413,
            frameHeight / 4,
            2);
        QVERIFY2(!match.valid || match.confidence < 0.35 || match.score > 1.5,
                 qPrintable(QStringLiteral("confidence=%1 score=%2 overlap=%3 y=%4")
                                .arg(match.confidence)
                                .arg(match.score)
                                .arg(match.overlap)
                                .arg(match.documentY)));
    }

    void longCaptureCommittedOverlapDetectsPoisonedAnchor()
    {
        constexpr int frameHeight = 550;
        constexpr int rowHeight = 36;
        constexpr int missingRows = 18;
        const QImage document = makeNumberedDocumentImage(320, 390, 100);
        const QImage liveCurrent = document.copy(0, 0, 320, frameHeight);
        const QImage liveNext = document.copy(0, rowHeight, 320, frameHeight);
        const QImage poisonedCanvas = document.copy(0,
                                                    missingRows * rowHeight,
                                                    320,
                                                    frameHeight);
        const auto currentSignature = Visnip::LongCapture::computeRowSignature(liveCurrent);
        const auto nextSignature = Visnip::LongCapture::computeRowSignature(liveNext);
        const auto canvasSignature = Visnip::LongCapture::computeRowSignature(poisonedCanvas);

        const auto local = Visnip::LongCapture::matchSignatureInDocument(
            currentSignature,
            0,
            nextSignature,
            -17,
            frameHeight - frameHeight / 4,
            frameHeight / 4,
            2);
        QVERIFY(local.valid);
        QCOMPARE(local.documentY, rowHeight);
        QVERIFY(local.score <= 1.5);

        const int overlap = frameHeight - local.documentY;
        const double committedScore = Visnip::LongCapture::signatureDifference(
            canvasSignature,
            local.documentY,
            nextSignature,
            0,
            overlap);
        QVERIFY2(committedScore > 1.5,
                 qPrintable(QStringLiteral("committedScore=%1 overlap=%2")
                                .arg(committedScore)
                                .arg(overlap)));
    }

    void longCapturePeriodicMatchesRemainAmbiguousAndPreferNearestAnchor()
    {
        constexpr int period = 36;
        constexpr int documentHeight = 900;
        constexpr int frameHeight = 240;
        constexpr int sourceY = 20;
        constexpr int preferredY = sourceY + period * 5;
        Visnip::LongCapture::RowSignature document;
        document.luma.resize(documentHeight);
        document.edge.resize(documentHeight);
        document.spatialA.resize(documentHeight);
        document.spatialB.resize(documentHeight);
        for (int y = 0; y < documentHeight; ++y) {
            const int phase = y % period;
            document.luma[y] = static_cast<float>(40 + phase * 3);
            document.edge[y] = static_cast<float>(2 + (phase * 7) % 19);
            document.spatialA[y] = static_cast<float>((phase * 11) % 31);
            document.spatialB[y] = static_cast<float>((phase * 13) % 37);
        }

        Visnip::LongCapture::RowSignature frame;
        frame.luma = document.luma.mid(sourceY, frameHeight);
        frame.edge = document.edge.mid(sourceY, frameHeight);
        frame.spatialA = document.spatialA.mid(sourceY, frameHeight);
        frame.spatialB = document.spatialB.mid(sourceY, frameHeight);

        const auto match = Visnip::LongCapture::matchSignatureInDocument(
            document,
            0,
            frame,
            0,
            400,
            frameHeight / 2,
            2,
            preferredY);
        QVERIFY(match.valid);
        QCOMPARE(match.documentY, preferredY);
        QVERIFY(match.ambiguous);
        QVERIFY(match.confidence < 0.35);
    }

    void longCaptureAmbiguityUsesFullResolutionRunnerUp()
    {
        constexpr int documentHeight = 600;
        constexpr int frameHeight = 240;
        constexpr int duplicateY = 300;
        Visnip::LongCapture::RowSignature document;
        document.luma.resize(documentHeight);
        document.edge.resize(documentHeight);
        document.spatialA.resize(documentHeight);
        document.spatialB.resize(documentHeight);
        for (int y = 0; y < documentHeight; ++y) {
            document.luma[y] = static_cast<float>((y * 73 + y * y * 11) % 251);
            document.edge[y] = static_cast<float>(2 + (y * 17) % 29);
            document.spatialA[y] = static_cast<float>((y * 31) % 47);
            document.spatialB[y] = static_cast<float>((y * 43) % 53);
        }

        Visnip::LongCapture::RowSignature frame;
        frame.luma = document.luma.mid(0, frameHeight);
        frame.edge = document.edge.mid(0, frameHeight);
        frame.spatialA = document.spatialA.mid(0, frameHeight);
        frame.spatialB = document.spatialB.mid(0, frameHeight);
        for (int i = 0; i < frameHeight; ++i) {
            document.luma[duplicateY + i] = frame.luma[i];
            document.edge[duplicateY + i] = frame.edge[i];
            document.spatialA[duplicateY + i] = frame.spatialA[i];
            document.spatialB[duplicateY + i] = frame.spatialB[i];
            if (i % 5 == 1) {
                document.luma[duplicateY + i] += 80.0f;
            }
        }

        const auto match = Visnip::LongCapture::matchSignatureInDocument(
            document,
            0,
            frame,
            0,
            duplicateY,
            frameHeight / 2,
            2,
            0);
        QVERIFY(match.valid);
        QCOMPARE(match.documentY, 0);
        QVERIFY(!match.ambiguous);
    }

    void longCaptureMatchFindsLowOverlapInNumberedPage()
    {
        constexpr int frameHeight = 550;
        constexpr int scrollRows = 11;
        constexpr int rowHeight = 36;
        constexpr int expectedY = scrollRows * rowHeight;
        const QImage document = makeNumberedDocumentImage(320, 390, 80);
        const QImage current = document.copy(0, 0, 320, frameHeight);
        const QImage moved = document.copy(0, expectedY, 320, frameHeight);
        const auto currentSignature = Visnip::LongCapture::computeRowSignature(current);
        const auto movedSignature = Visnip::LongCapture::computeRowSignature(moved);

        const auto match = Visnip::LongCapture::matchSignatureInDocument(
            currentSignature,
            0,
            movedSignature,
            -17,
            413,
            frameHeight / 4,
            2);
        QVERIFY(match.valid);
        QCOMPARE(match.documentY, expectedY);
        QVERIFY(match.score <= 1.5);
        QVERIFY2(match.confidence >= 0.35,
                 qPrintable(QStringLiteral("confidence=%1 score=%2 overlap=%3")
                                .arg(match.confidence)
                                .arg(match.score)
                                .arg(match.overlap)));
    }

    void longCaptureMatchRejectsMalformedSignature()
    {
        Visnip::LongCapture::RowSignature malformed;
        malformed.luma.append(42.0f);
        Visnip::LongCapture::RowSignature valid;
        valid.luma.append(42.0f);
        valid.edge.append(1.0f);

        const auto match = Visnip::LongCapture::matchSignatureInDocument(
            malformed, 0, valid, 0, 0, 1);
        QVERIFY(!match.valid);
        QCOMPARE(Visnip::LongCapture::signatureDifference(malformed, 0, valid, 0, 1), 255.0);
    }

    void longCaptureMatchScalesToLongDocument()
    {
        constexpr int documentHeight = 120000;
        constexpr int frameHeight = 1000;
        constexpr int expectedY = 91357;
        Visnip::LongCapture::RowSignature document;
        document.luma.resize(documentHeight);
        document.edge.resize(documentHeight);
        for (int i = 0; i < documentHeight; ++i) {
            const quint32 hash = static_cast<quint32>(i) * 2654435761u;
            document.luma[i] = static_cast<float>((hash >> 8) % 251u);
            document.edge[i] = 1.0f + static_cast<float>((hash >> 20) % 12u);
        }
        Visnip::LongCapture::RowSignature frame;
        frame.luma = document.luma.mid(expectedY, frameHeight);
        frame.edge = document.edge.mid(expectedY, frameHeight);

        const auto match = Visnip::LongCapture::matchSignatureInDocument(
            document, 0, frame, 0, documentHeight - frameHeight, frameHeight / 4, 4);
        QVERIFY(match.valid);
        QCOMPARE(match.documentY, expectedY);
        QVERIFY(match.score < 0.01);
    }

    void longCaptureSameFrameHasLowDifference()
    {
        const QImage document = makeDocumentImage(320, 800);
        const QImage frame = document.copy(0, 120, 320, 400);
        const auto a = Visnip::LongCapture::computeRowSignature(frame);
        const auto b = Visnip::LongCapture::computeRowSignature(frame);
        QVERIFY(Visnip::LongCapture::signatureDifference(a, 0, b, 0, a.height()) < 0.01);

        const QImage shifted = document.copy(0, 190, 320, 400);
        const auto c = Visnip::LongCapture::computeRowSignature(shifted);
        QVERIFY(Visnip::LongCapture::signatureDifference(a, 0, c, 0, a.height()) > 1.5);
    }

    void ctcGreedyDecodeCollapsesRepeatsAndBlanks()
    {
        const QVector<QString> charset = { QStringLiteral("你"), QStringLiteral("好"),
                                           QStringLiteral("A") };
        // 6 timesteps x 5 classes (blank, 你, 好, A, space).
        QVector<float> probs(6 * 5, 0.01f);
        auto set = [&probs](int t, int cls, float p) { probs[t * 5 + cls] = p; };
        set(0, 1, 0.90f); // 你
        set(1, 1, 0.80f); // repeat, collapsed
        set(2, 0, 0.90f); // blank
        set(3, 2, 0.95f); // 好
        set(4, 4, 0.70f); // space
        set(5, 3, 0.85f); // A

        float score = 0.0f;
        const QString text = Visnip::Ocr::ctcGreedyDecode(probs.constData(), 6, 5, charset, &score);
        QCOMPARE(text, QStringLiteral("你好 A"));
        QVERIFY(qAbs(score - 0.85f) < 0.001f);
    }

    void ocrLanguagePackCatalogIsStable()
    {
        QCOMPARE(Visnip::Ocr::defaultLanguagePackId(),
                 QStringLiteral("general-v5"));
        const auto& packs = Visnip::Ocr::languagePacks();
        QCOMPARE(packs.size(), 5);
        QStringList ids;
        for (const auto& pack : packs) {
            ids.append(pack.id);
            QVERIFY(!pack.modelRevision.isEmpty());
            QCOMPARE(pack.modelSha256.size(), 64);
            QCOMPARE(pack.dictionarySha256.size(), 64);
            QVERIFY(pack.dictionaryEntries > 0);
            QVERIFY(!pack.cacheKey().isEmpty());
        }
        QCOMPARE(ids, QStringList({ QStringLiteral("general-v5"),
                                    QStringLiteral("korean-v5"),
                                    QStringLiteral("latin-v5"),
                                    QStringLiteral("cyrillic-v5"),
                                    QStringLiteral("arabic-v5") }));
        QCOMPARE(Visnip::Ocr::normalizedLanguagePackId(
                     QStringLiteral("invalid")),
                 QStringLiteral("general-v5"));
    }

    void arabicPredictionPreservesLatinRunOrder()
    {
        QCOMPARE(Visnip::Ocr::reverseArabicPrediction(
                     QStringLiteral("ابABC 12جد")),
                 QStringLiteral("دجABC 12با"));
    }

    void translatableLineFilterRejectsCompactIconBesideLabel()
    {
        QVector<Visnip::OcrTextLine> lines = {
            { QRect(15, 17, 39, 20), QStringLiteral("Feed"), 0.995f },
            { QRect(851, 21, 18, 13), QStringLiteral("三"), 0.815f },
            { QRect(860, 17, 50, 22), QStringLiteral("Filter"), 0.866f },
        };
        QVector<Visnip::Ocr::RejectedTextLine> rejected;
        const auto filtered = Visnip::Ocr::filterTranslatableLines(
            lines, {}, &rejected);

        QCOMPARE(filtered.size(), 2);
        QCOMPARE(filtered[0].text, QStringLiteral("Feed"));
        QCOMPARE(filtered[1].text, QStringLiteral("Filter"));
        QCOMPARE(rejected.size(), 1);
        QCOMPARE(rejected[0].lineIndex, 1);
        QCOMPARE(rejected[0].peerIndex, 2);
        QCOMPARE(rejected[0].reason,
                 QStringLiteral("compact-low-confidence-near-label"));
    }

    void translatableLineFilterRejectsEarlierIconVariant()
    {
        QVector<Visnip::OcrTextLine> lines = {
            { QRect(863, 30, 26, 16), QStringLiteral("FF"), 0.523f },
            { QRect(880, 28, 41, 19), QStringLiteral("Filter"), 0.991f },
        };
        QCOMPARE(Visnip::Ocr::filterTranslatableLines(lines).size(), 1);
        QCOMPARE(Visnip::Ocr::filterTranslatableLines(lines)[0].text,
                 QStringLiteral("Filter"));
    }

    void translatableLineFilterRejectsRepeatedLinkIcon()
    {
        QImage source(220, 145, QImage::Format_ARGB32);
        source.fill(Qt::white);
        QPainter painter(&source);
        QVector<Visnip::OcrTextLine> lines;
        QRect recognizedIcon;
        for (int row = 0; row < 4; ++row) {
            const int iconTop = 10 + row * 32;
            const QRect iconBox(20, iconTop, 15, 14);
            painter.fillRect(QRect(21, iconTop + 2, 7, 3), Qt::black);
            painter.fillRect(QRect(25, iconTop + 4, 4, 6), Qt::black);
            painter.fillRect(QRect(28, iconTop + 8, 6, 3), Qt::black);
            painter.fillRect(QRect(56, iconTop + 1, 104, 10), Qt::black);
            if (row == 3) {
                recognizedIcon = iconBox;
                lines.append({ iconBox, QStringLiteral("e"), 0.96f });
            }
            lines.append({ QRect(52, iconTop - 3, 125, 20),
                           QStringLiteral("Link to social profile %1").arg(row + 1),
                           0.97f });
        }
        painter.end();

        QVector<Visnip::Ocr::RejectedTextLine> rejected;
        const auto filtered = Visnip::Ocr::filterTranslatableLines(
            lines, source, &rejected);
        QCOMPARE(filtered.size(), 4);
        QCOMPARE(rejected.size(), 1);
        QCOMPARE(rejected[0].line.text, QStringLiteral("e"));
        QCOMPARE(rejected[0].peerIndex, 4);
        QCOMPARE(rejected[0].reason,
                 QStringLiteral("repeated-compact-icon-near-label"));

        const auto blocks = Visnip::Translate::mergeLinesIntoBlocks(
            filtered, source);
        QStringList translations;
        translations.fill(QStringLiteral("社交资料链接"), blocks.size());
        const QImage composed = Visnip::Translate::composeTranslatedImage(
            source, blocks, translations);
        for (int y = recognizedIcon.top(); y <= recognizedIcon.bottom(); ++y) {
            for (int x = recognizedIcon.left(); x <= recognizedIcon.right(); ++x) {
                QCOMPARE(composed.pixel(x, y), source.pixel(x, y));
            }
        }
    }

    void translatableLineFilterPreservesRealLetterNearLabel()
    {
        QImage source(220, 145, QImage::Format_ARGB32);
        source.fill(Qt::white);
        QPainter painter(&source);
        QVector<Visnip::OcrTextLine> lines;
        for (int row = 0; row < 4; ++row) {
            const int top = 10 + row * 32;
            painter.fillRect(QRect(56, top + 1, 104, 10), Qt::black);
            lines.append({ QRect(52, top - 3, 125, 20),
                           QStringLiteral("Link to social profile %1").arg(row + 1),
                           0.97f });
        }
        painter.fillRect(QRect(21, 108, 9, 8), Qt::black);
        painter.end();
        lines.insert(3, { QRect(20, 106, 15, 14),
                          QStringLiteral("e"),
                          0.98f });

        QVector<Visnip::Ocr::RejectedTextLine> rejected;
        const auto filtered = Visnip::Ocr::filterTranslatableLines(
            lines, source, &rejected);
        QCOMPARE(filtered.size(), lines.size());
        QVERIFY(rejected.isEmpty());
        QCOMPARE(filtered[3].text, QStringLiteral("e"));
    }

    void translatableLineFilterStripsMergedLeadingIcon()
    {
        QImage source(100, 40, QImage::Format_ARGB32);
        source.fill(QColor(247, 247, 247));
        QPainter painter(&source);
        painter.fillRect(QRect(2, 12, 14, 10), QColor(42, 42, 42));
        painter.fillRect(QRect(24, 11, 34, 11), QColor(42, 42, 42));
        painter.end();

        QVector<Visnip::OcrTextLine> lines = {
            { QRect(0, 6, 63, 22), QStringLiteral("三Filter"), 0.759f },
        };
        QVector<Visnip::Ocr::RejectedTextLine> rejected;
        const auto filtered = Visnip::Ocr::filterTranslatableLines(
            lines, source, &rejected);

        QCOMPARE(filtered.size(), 1);
        QCOMPARE(filtered[0].text, QStringLiteral("Filter"));
        QCOMPARE(rejected.size(), 1);
        QCOMPARE(rejected[0].line.text, QStringLiteral("三"));
        QCOMPARE(rejected[0].reason,
                 QStringLiteral("merged-leading-icon-prefix"));
    }

    void isolatedButtonIconIsSeparatedFromLabel()
    {
        QImage source(90, 42, QImage::Format_ARGB32);
        source.fill(Qt::white);
        QPainter painter(&source);
        painter.fillRect(QRect(7, 12, 14, 13), Qt::black);
        painter.fillRect(QRect(27, 13, 30, 11), Qt::black);
        painter.end();

        Visnip::OcrTextLine line(
            QRect(2, 6, 60, 25), QStringLiteral(" Edit"), 0.91f);
        QVERIFY(Visnip::Ocr::refineIsolatedLeadingIcon(&line, source));
        QCOMPARE(line.detectedBox, QRect(2, 6, 60, 25));
        QCOMPARE(line.box.left(), 27);
        QCOMPARE(line.box.right(), 61);
        QVERIFY(line.leadingIconSeparated);
        QCOMPARE(line.refinementReason,
                 QStringLiteral("isolated-leading-icon-whitespace"));
    }

    void isolatedTextWithLeadingWhitespaceKeepsItsBox()
    {
        QImage source(180, 42, QImage::Format_ARGB32);
        source.fill(Qt::white);
        QPainter painter(&source);
        painter.fillRect(QRect(10, 13, 145, 11), Qt::black);
        painter.end();

        Visnip::OcrTextLine line(
            QRect(4, 6, 160, 25), QStringLiteral(" ordinary text"), 0.98f);
        QVERIFY(!Visnip::Ocr::refineIsolatedLeadingIcon(&line, source));
        QCOMPARE(line.box, QRect(4, 6, 160, 25));
        QVERIFY(!line.leadingIconSeparated);
    }

    void translatableLineFilterPreservesLegitimateMixedText()
    {
        QImage source(100, 40, QImage::Format_ARGB32);
        source.fill(Qt::white);
        QPainter painter(&source);
        painter.fillRect(QRect(4, 12, 16, 10), Qt::black);
        painter.fillRect(QRect(28, 11, 32, 11), Qt::black);
        painter.end();

        QVector<Visnip::OcrTextLine> lines = {
            // Confidence and width gates prevent ordinary mixed-language text
            // from being rewritten by the icon-specific heuristic.
            { QRect(0, 6, 63, 22), QStringLiteral("中Filter"), 0.95f },
            { QRect(0, 6, 90, 22), QStringLiteral("中Filter"), 0.70f },
        };
        const auto filtered = Visnip::Ocr::filterTranslatableLines(lines, source);
        QCOMPARE(filtered.size(), lines.size());
        QCOMPARE(filtered[0].text, lines[0].text);
        QCOMPARE(filtered[1].text, lines[1].text);
    }

    void translatableLineFilterPreservesLegitimateShortText()
    {
        QVector<Visnip::OcrTextLine> lines = {
            // Standalone small text has no adjacent label.
            { QRect(20, 20, 18, 13), QStringLiteral("三"), 0.70f },
            // A high-confidence compact badge is retained even near a label.
            { QRect(100, 20, 18, 13), QStringLiteral("3"), 0.95f },
            { QRect(112, 17, 60, 20), QStringLiteral("updates"), 0.98f },
            // A longer small label is not considered an icon candidate.
            { QRect(200, 20, 25, 14), QStringLiteral("API"), 0.70f },
            { QRect(220, 18, 50, 20), QStringLiteral("status"), 0.98f },
        };
        QVector<Visnip::Ocr::RejectedTextLine> rejected;
        const auto filtered = Visnip::Ocr::filterTranslatableLines(
            lines, {}, &rejected);
        QCOMPARE(filtered.size(), lines.size());
        QVERIFY(rejected.isEmpty());
    }

    void translationGatePreservesNonLinguisticRegions()
    {
        const QVector<Visnip::OcrTextLine> lines = {
            { QRect(12, 10, 20, 13), QStringLiteral("0"), 0.99f },
            { QRect(38, 10, 30, 13), QStringLiteral("2026"), 0.99f },
            { QRect(90, 10, 20, 13), QStringLiteral("□"), 0.99f },
            { QRect(116, 10, 20, 13), QStringLiteral("👍"), 0.99f },
            { QRect(142, 10, 30, 13), QStringLiteral("$42"), 0.99f },
            { QRect(178, 10, 24, 13), QStringLiteral("..."), 0.99f },
            { QRect(12, 35, 40, 18), QStringLiteral("❤️❤️"), 0.99f },
            { QRect(60, 35, 40, 18), QStringLiteral("☑️☑️"), 0.99f },
        };
        const auto units = Visnip::Translate::makeLineTranslationUnits(lines);
        QCOMPARE(units.size(), lines.size());
        for (const auto& unit : units) {
            const auto decision =
                Visnip::Translate::classifyBlockForTranslation(unit);
            QVERIFY(!decision.translatable);
            QCOMPARE(decision.lexicalGraphemes, 0);
            QCOMPARE(decision.reason,
                     QStringLiteral("no-linguistic-content"));
        }
    }

    void translationGatePreservesAmbiguousSingleGraphemes()
    {
        const QVector<Visnip::OcrTextLine> lines = {
            { QRect(10, 10, 20, 20), QStringLiteral("I"), 0.99f },
            { QRect(40, 10, 20, 20), QStringLiteral("四"), 0.99f },
            { QRect(70, 10, 20, 20), QStringLiteral("あ"), 0.99f },
            { QRect(100, 10, 20, 20), QStringLiteral("가"), 0.99f },
        };
        const auto units = Visnip::Translate::makeLineTranslationUnits(lines);
        for (const auto& unit : units) {
            const auto decision =
                Visnip::Translate::classifyBlockForTranslation(unit);
            QVERIFY(!decision.translatable);
            QCOMPARE(decision.lexicalGraphemes, 1);
            QCOMPARE(decision.reason,
                     QStringLiteral("insufficient-linguistic-evidence"));
        }
    }

    void translationGatePreservesAmbiguousShortAlphabeticTokens()
    {
        const QVector<Visnip::OcrTextLine> lines = {
            { QRect(10, 10, 40, 20), QStringLiteral("AI"), 0.99f },
            { QRect(60, 10, 40, 20), QStringLiteral("OK"), 0.99f },
            { QRect(110, 10, 40, 20), QStringLiteral("CC"), 0.99f },
            { QRect(160, 10, 40, 20), QStringLiteral("Да"), 0.99f },
        };
        const auto units = Visnip::Translate::makeLineTranslationUnits(lines);
        for (const auto& unit : units) {
            const auto decision =
                Visnip::Translate::classifyBlockForTranslation(unit);
            QVERIFY(!decision.translatable);
            QCOMPARE(decision.lexicalGraphemes, 2);
            QCOMPARE(decision.reason,
                     QStringLiteral("ambiguous-short-alphabetic-token"));
        }
    }

    void translationGateAcceptsUnicodeLanguageContent()
    {
        const QVector<Visnip::OcrTextLine> lines = {
            { QRect(10, 10, 120, 22), QStringLiteral("Release 2026"), 0.99f },
            { QRect(10, 40, 80, 22), QStringLiteral("设置"), 0.99f },
            { QRect(10, 70, 120, 22), QStringLiteral("مرحبا"), 0.99f },
        };
        const auto units = Visnip::Translate::makeLineTranslationUnits(lines);
        QCOMPARE(units.size(), lines.size());
        for (const auto& unit : units) {
            const auto decision =
                Visnip::Translate::classifyBlockForTranslation(unit);
            QVERIFY(decision.translatable);
            QVERIFY(decision.lexicalGraphemes >= 2);
            QCOMPARE(decision.reason, QStringLiteral("linguistic-content"));
        }
    }

    void preservedTranslationUnitLeavesSourceUnchanged()
    {
        QImage source(240, 180, QImage::Format_ARGB32);
        source.fill(Qt::white);
        const QRect artworkBox(40, 30, 137, 113);
        QPainter painter(&source);
        painter.fillRect(artworkBox, QColor(107, 219, 152));
        painter.fillRect(QRect(40, 72, 38, 46), QColor(240, 240, 240));
        painter.fillRect(QRect(130, 72, 47, 46), QColor(240, 240, 240));
        painter.end();

        const QVector<Visnip::OcrTextLine> lines = {
            { artworkBox, QStringLiteral("1"), 0.908f },
        };
        const auto filtered = Visnip::Ocr::filterTranslatableLines(
            lines, source);
        QCOMPARE(filtered.size(), 1);
        const auto units = Visnip::Translate::makeLineTranslationUnits(filtered);
        QCOMPARE(units.size(), 1);
        QVERIFY(!Visnip::Translate::classifyBlockForTranslation(units[0])
                     .translatable);
        const QImage composed = Visnip::Translate::composeTranslatedImage(
            source, {}, {});
        QCOMPARE(composed.size(), source.size());
        for (int y = artworkBox.top(); y <= artworkBox.bottom(); ++y) {
            for (int x = artworkBox.left(); x <= artworkBox.right(); ++x) {
                QCOMPARE(composed.pixel(x, y), source.pixel(x, y));
            }
        }
    }

    void translationGatePreservesRealSingleGlyphByPolicy()
    {
        QImage source(180, 180, QImage::Format_ARGB32);
        source.fill(Qt::white);
        const QRect textBox(40, 20, 80, 130);
        QPainter painter(&source);
        QFont font;
        font.setPixelSize(110);
        font.setBold(true);
        painter.setFont(font);
        painter.setPen(Qt::black);
        painter.drawText(textBox, Qt::AlignCenter, QStringLiteral("1"));
        painter.end();

        const QVector<Visnip::OcrTextLine> lines = {
            { textBox, QStringLiteral("1"), 0.99f },
        };
        const auto filtered = Visnip::Ocr::filterTranslatableLines(
            lines, source);
        QCOMPARE(filtered.size(), 1);
        QCOMPARE(filtered[0].text, QStringLiteral("1"));
        const auto units = Visnip::Translate::makeLineTranslationUnits(filtered);
        const auto decision =
            Visnip::Translate::classifyBlockForTranslation(units[0]);
        QVERIFY(!decision.translatable);
        QCOMPARE(decision.reason, QStringLiteral("no-linguistic-content"));
    }

    void translatableLineFilterRejectsEnclosedSmileyIcon()
    {
        QImage source(80, 60, QImage::Format_ARGB32);
        source.fill(Qt::white);
        const QRect iconBox(20, 20, 20, 20);
        QPainter painter(&source);
        QPen pen(Qt::black);
        pen.setWidth(2);
        painter.setPen(pen);
        painter.drawEllipse(QRect(22, 22, 16, 16));
        painter.fillRect(QRect(26, 28, 2, 2), Qt::black);
        painter.fillRect(QRect(32, 28, 2, 2), Qt::black);
        painter.fillRect(QRect(27, 33, 6, 2), Qt::black);
        painter.end();

        const QVector<Visnip::OcrTextLine> lines = {
            { iconBox, QStringLiteral("四"), 0.756f },
        };
        QVector<Visnip::Ocr::RejectedTextLine> rejected;
        const auto filtered = Visnip::Ocr::filterTranslatableLines(
            lines, source, &rejected);
        QVERIFY(filtered.isEmpty());
        QCOMPARE(rejected.size(), 1);
        QCOMPARE(rejected[0].line.text, QStringLiteral("四"));
        QCOMPARE(rejected[0].peerIndex, -1);
        QCOMPARE(rejected[0].reason,
                 QStringLiteral("enclosed-compact-icon"));
    }

    void translatableLineFilterPreservesRealCompactFour()
    {
        QImage source(80, 60, QImage::Format_ARGB32);
        source.fill(Qt::white);
        const QRect textBox(20, 20, 20, 20);
        QPainter painter(&source);
        painter.fillRect(QRect(23, 23, 14, 2), Qt::black);
        painter.fillRect(QRect(23, 23, 2, 14), Qt::black);
        painter.fillRect(QRect(35, 23, 2, 14), Qt::black);
        painter.fillRect(QRect(23, 35, 14, 2), Qt::black);
        painter.fillRect(QRect(27, 27, 2, 7), Qt::black);
        painter.fillRect(QRect(31, 27, 2, 7), Qt::black);
        painter.end();

        const QVector<Visnip::OcrTextLine> lines = {
            { textBox, QStringLiteral("四"), 0.756f },
        };
        QVector<Visnip::Ocr::RejectedTextLine> rejected;
        const auto filtered = Visnip::Ocr::filterTranslatableLines(
            lines, source, &rejected);
        QCOMPARE(filtered.size(), 1);
        QCOMPARE(filtered[0].text, QStringLiteral("四"));
        QVERIFY(rejected.isEmpty());
    }

    void nearThresholdFragmentStillRecoversFullSentence()
    {
        QImage source(660, 70, QImage::Format_ARGB32);
        source.fill(Qt::white);
        QPainter painter(&source);
        painter.fillRect(QRect(20, 27, 580, 12), QColor(98, 98, 98));
        painter.end();

        const QVector<QRect> boxes = {
            QRect(16, 20, 492, 21),
            QRect(504, 23, 87, 14),
        };
        const auto recovered = Visnip::Ocr::recoverFragmentedLineBoxes(
            boxes, source);
        QCOMPARE(recovered.size(), 1);
        QCOMPARE(recovered[0].left(), boxes[0].left());
        QVERIFY(recovered[0].right() >= boxes[1].right());
    }

    void fragmentedSingleLineBoxesRecoverFullSentence()
    {
        QImage source(680, 90, QImage::Format_ARGB32);
        source.fill(Qt::white);
        QPainter painter(&source);
        QFont font;
        font.setPixelSize(12);
        painter.setFont(font);
        painter.setPen(QColor(98, 98, 98));
        painter.drawText(QPoint(20, 43),
                         QStringLiteral("Your name may appear around GitHub where you contribute "
                                        "or are mentioned. You can remove it at any time."));
        painter.end();

        const QVector<QRect> boxes = {
            QRect(16, 27, 280, 25),
            QRect(286, 34, 185, 11),
            QRect(548, 34, 48, 11),
            QRect(20, 66, 90, 20),
        };
        const auto recovered = Visnip::Ocr::recoverFragmentedLineBoxes(
            boxes, source);
        QCOMPARE(recovered.size(), 2);
        QCOMPARE(recovered[0].left(), boxes[0].left());
        QVERIFY(recovered[0].right() > boxes[2].right());
        QVERIFY(recovered[0].right() < source.width() - 1);
        QCOMPARE(recovered[1], boxes[3]);
    }

    void wideSparseLineUsesTighterRecognitionCrop()
    {
        QImage source(640, 60, QImage::Format_ARGB32);
        source.fill(Qt::white);
        QPainter painter(&source);
        painter.fillRect(QRect(20, 25, 580, 10), QColor(98, 98, 98));
        painter.end();

        const QRect detectorBox(16, 17, 590, 26);
        const QRect crop = Visnip::Ocr::recognitionCropBox(
            detectorBox, source);
        QCOMPARE(crop.left(), detectorBox.left());
        QCOMPARE(crop.width(), detectorBox.width());
        QVERIFY(crop.top() > detectorBox.top());
        QVERIFY(crop.bottom() < detectorBox.bottom());
        QVERIFY(crop.contains(QRect(20, 25, 580, 10)));
    }

    void confusableAiTermsAreCorrectedWithoutChangingProse()
    {
        QCOMPARE(Visnip::Ocr::correctConfusableText(QStringLiteral("Al usage")),
                 QStringLiteral("AI usage"));
        QCOMPARE(Visnip::Ocr::correctConfusableText(
                     QStringLiteral("licenses and Al Credits")),
                 QStringLiteral("licenses and AI Credits"));
        QCOMPARE(Visnip::Ocr::correctConfusableText(QStringLiteral("Al models")),
                 QStringLiteral("AI models"));
        QCOMPARE(Visnip::Ocr::correctConfusableText(QStringLiteral("Al Pacino")),
                 QStringLiteral("Al Pacino"));
        QCOMPARE(Visnip::Ocr::correctConfusableText(QStringLiteral("Al usage policy")),
                 QStringLiteral("AI usage policy"));
        QCOMPARE(Visnip::Ocr::correctConfusableText(QStringLiteral("Public profi le")),
                 QStringLiteral("Public profile"));
        QCOMPARE(Visnip::Ocr::correctConfusableText(QStringLiteral("profile page")),
                 QStringLiteral("profile page"));
        QCOMPARE(Visnip::Ocr::correctConfusableText(QStringLiteral("0ct")),
                 QStringLiteral("Oct"));
        QCOMPARE(Visnip::Ocr::correctConfusableText(QStringLiteral("0ctober")),
                 QStringLiteral("0ctober"));
    }

    void compactSparseLabelUsesTighterRecognitionCrop()
    {
        QImage source(220, 60, QImage::Format_ARGB32);
        source.fill(Qt::white);
        QPainter painter(&source);
        painter.fillRect(QRect(24, 25, 130, 10), QColor(34, 34, 34));
        painter.end();

        const QRect detectorBox(18, 17, 145, 26);
        const QRect crop = Visnip::Ocr::recognitionCropBox(
            detectorBox, source);
        QCOMPARE(crop.left(), detectorBox.left());
        QCOMPARE(crop.width(), detectorBox.width());
        QVERIFY(crop.top() > detectorBox.top());
        QVERIFY(crop.bottom() < detectorBox.bottom());
        QVERIFY(crop.contains(QRect(24, 25, 130, 10)));
    }

    void narrowWeekdayUsesTighterRecognitionCrop()
    {
        QImage source(80, 50, QImage::Format_ARGB32);
        source.fill(Qt::white);
        QPainter painter(&source);
        painter.fillRect(QRect(24, 20, 5, 6), QColor(82, 82, 82));
        painter.fillRect(QRect(31, 20, 1, 9), QColor(82, 82, 82));
        painter.fillRect(QRect(35, 23, 6, 6), QColor(82, 82, 82));
        painter.end();

        const QRect detectorBox(20, 14, 21, 18);
        const QRect crop = Visnip::Ocr::recognitionCropBox(
            detectorBox, source);
        QCOMPARE(crop.left(), detectorBox.left());
        QCOMPARE(crop.width(), detectorBox.width());
        QVERIFY(crop.top() > detectorBox.top());
        QVERIFY(crop.bottom() < detectorBox.bottom());
        QVERIFY(crop.contains(QRect(24, 20, 17, 9)));
    }

    void headingKeepsItsRecognitionCrop()
    {
        QImage source(360, 80, QImage::Format_ARGB32);
        source.fill(Qt::white);
        QPainter painter(&source);
        painter.fillRect(QRect(20, 25, 250, 26), Qt::black);
        painter.end();
        const QRect detectorBox(16, 17, 270, 34);
        QCOMPARE(Visnip::Ocr::recognitionCropBox(detectorBox, source),
                 detectorBox);
    }

    void normalSameRowLabelsAreNotRecoveredAsFragments()
    {
        QImage source(420, 80, QImage::Format_ARGB32);
        source.fill(Qt::white);
        QPainter painter(&source);
        painter.fillRect(QRect(20, 33, 180, 10), Qt::black);
        painter.fillRect(QRect(220, 33, 90, 10), Qt::black);
        painter.end();
        const QVector<QRect> boxes = {
            QRect(16, 26, 190, 25),
            QRect(216, 26, 100, 25),
        };
        QCOMPARE(Visnip::Ocr::recoverFragmentedLineBoxes(boxes, source),
                 boxes);
    }

    void repeatedMenuIconColumnIsRemovedBeforeRecognition()
    {
        QImage source(360, 210, QImage::Format_ARGB32);
        source.fill(Qt::white);
        QPainter painter(&source);
        QVector<QRect> boxes;
        for (int row = 0; row < 4; ++row) {
            const int top = 12 + row * 38;
            const QRect box(40 + (row % 2), top, 190 - row * 12, 28);
            boxes.append(box);
            painter.fillRect(QRect(46, top + 7, 15, 14), Qt::black);
            // The stable 9px separator puts all labels at x=70.
            painter.fillRect(QRect(70, top + 8, box.right() - 76, 12), Qt::black);
        }
        // An unrelated paragraph has a leading word-like span and gap, but no
        // repeated peer column, so it must retain its full detector box.
        const QRect paragraph(220, 172, 130, 26);
        boxes.append(paragraph);
        painter.fillRect(QRect(224, 179, 14, 11), Qt::black);
        painter.fillRect(QRect(247, 179, 96, 11), Qt::black);
        painter.end();

        const auto refined = Visnip::Ocr::refineRecognitionBoxes(boxes, source);
        QCOMPARE(refined.size(), boxes.size());
        for (int index = 0; index < 4; ++index) {
            QCOMPARE(refined[index].detectedBox, boxes[index]);
            QCOMPARE(refined[index].textBox.left(), 70);
            QCOMPARE(refined[index].textBox.right(), boxes[index].right());
            QVERIFY(refined[index].leadingIconSeparated);
            QCOMPARE(refined[index].reason,
                     QStringLiteral("repeated-leading-icon-column"));
        }
        QCOMPARE(refined.last().textBox, paragraph);
        QVERIFY(!refined.last().leadingIconSeparated);
        QVERIFY(refined.last().reason.isEmpty());
    }

    void isolatedLeadingShapeDoesNotTrimOrdinaryText()
    {
        QImage source(260, 100, QImage::Format_ARGB32);
        source.fill(Qt::white);
        QPainter painter(&source);
        painter.fillRect(QRect(20, 18, 15, 12), Qt::black);
        painter.fillRect(QRect(44, 18, 180, 12), Qt::black);
        painter.fillRect(QRect(20, 58, 180, 12), Qt::black);
        painter.end();

        const QVector<QRect> boxes = {
            QRect(16, 10, 220, 28),
            QRect(16, 50, 220, 28),
        };
        const auto refined = Visnip::Ocr::refineRecognitionBoxes(boxes, source);
        QCOMPARE(refined.size(), boxes.size());
        QCOMPARE(refined[0].textBox, boxes[0]);
        QCOMPARE(refined[1].textBox, boxes[1]);
        QVERIFY(!refined[0].leadingIconSeparated);
        QVERIFY(!refined[1].leadingIconSeparated);
    }

    void repeatedParagraphLinesDoNotLookLikeIconColumns()
    {
        QImage source(300, 140, QImage::Format_ARGB32);
        source.fill(Qt::white);
        QPainter painter(&source);
        QVector<QRect> boxes;
        for (int row = 0; row < 3; ++row) {
            const int top = 10 + row * 38;
            boxes.append(QRect(20, top, 250, 28));
            // A short first word followed by an ordinary 5px word space.
            painter.fillRect(QRect(24, top + 8, 15, 12), Qt::black);
            painter.fillRect(QRect(44, top + 8, 210, 12), Qt::black);
        }
        painter.end();

        const auto refined = Visnip::Ocr::refineRecognitionBoxes(boxes, source);
        QCOMPARE(refined.size(), boxes.size());
        for (int index = 0; index < refined.size(); ++index) {
            QCOMPARE(refined[index].textBox, boxes[index]);
            QVERIFY(!refined[index].leadingIconSeparated);
        }
    }

    void tallNeighborDoesNotBridgeSeparateTextRows()
    {
        QVector<QRect> boxes = {
            QRect(10, 415, 58, 26),
            QRect(385, 428, 374, 24),
            QRect(386, 408, 399, 21),
            QRect(10, 446, 121, 30),
            QRect(385, 449, 123, 24),
        };
        Visnip::Ocr::sortBoxesInReadingOrder(boxes);
        QCOMPARE(boxes[0], QRect(386, 408, 399, 21));
        QCOMPARE(boxes[1], QRect(10, 415, 58, 26));
        QCOMPARE(boxes[2], QRect(385, 428, 374, 24));
        QCOMPARE(boxes[3], QRect(10, 446, 121, 30));
        QCOMPARE(boxes[4], QRect(385, 449, 123, 24));
    }

    void detBoxExtractionFindsTextRegions()
    {
        constexpr int mapW = 64;
        constexpr int mapH = 32;
        QVector<float> prob(mapW * mapH, 0.05f);
        auto fill = [&prob](int x0, int y0, int x1, int y1) {
            for (int y = y0; y <= y1; ++y) {
                for (int x = x0; x <= x1; ++x) {
                    prob[y * mapW + x] = 0.9f;
                }
            }
        };
        fill(10, 8, 29, 15);  // upper line
        fill(40, 20, 55, 26); // lower line

        const QVector<QRect> boxes = Visnip::Ocr::extractDetBoxes(
            prob.constData(), mapW, mapH, 2.0, 2.0, QSize(128, 64));
        QCOMPARE(boxes.size(), 2);
        // Reading order: upper region first. Boxes cover the raw text area
        // (map coords x2) plus the unclip margin.
        QVERIFY(boxes[0].contains(QRect(20, 16, 40, 16)));
        QVERIFY(boxes[0].top() < boxes[1].top());
        QVERIFY(boxes[1].contains(QRect(80, 40, 32, 14)));
    }

    void ocrLinesMergeIntoParagraphs()
    {
        QVector<Visnip::OcrTextLine> lines;
        // A two-line paragraph, left-aligned, tight spacing.
        lines.append({ QRect(20, 100, 300, 20), QStringLiteral("第一行文本"), 0.9f });
        lines.append({ QRect(20, 126, 280, 20), QStringLiteral("第二行文本"), 0.9f });
        // A side-by-side segment on the same row must stay separate.
        lines.append({ QRect(420, 126, 120, 20), QStringLiteral("右侧标签"), 0.9f });
        // A distant heading with a different height stays separate too.
        lines.append({ QRect(20, 240, 200, 34), QStringLiteral("大标题"), 0.9f });

        const auto blocks = Visnip::Translate::mergeLinesIntoBlocks(lines);
        QCOMPARE(blocks.size(), 3);
        QCOMPARE(blocks[0].lines.size(), 2);
        QCOMPARE(blocks[0].box, QRect(20, 100, 300, 46));
        QCOMPARE(blocks[0].mergedText(), QStringLiteral("第一行文本第二行文本"));
        QCOMPARE(blocks[1].lines.size(), 1);
        QCOMPARE(blocks[2].lines.size(), 1);
    }

    void interleavedRowsKeepParagraphsTogether()
    {
        // Reading order visits each visual row left to right, so a sidebar
        // column interleaves with the paragraph lines. The wide paragraph
        // keeps its context while short peer labels stay independent.
        QVector<Visnip::OcrTextLine> lines;
        lines.append({ QRect(20, 100, 300, 20), QStringLiteral("段落第一行"), 0.9f });
        lines.append({ QRect(420, 100, 120, 20), QStringLiteral("侧栏一"), 0.9f });
        lines.append({ QRect(20, 126, 280, 20), QStringLiteral("段落第二行"), 0.9f });
        lines.append({ QRect(420, 126, 120, 20), QStringLiteral("侧栏二"), 0.9f });

        const auto blocks = Visnip::Translate::mergeLinesIntoBlocks(lines);
        QCOMPARE(blocks.size(), 3);
        QCOMPARE(blocks[0].mergedText(), QStringLiteral("段落第一行段落第二行"));
        QCOMPARE(blocks[1].mergedText(), QStringLiteral("侧栏一"));
        QCOMPARE(blocks[2].mergedText(), QStringLiteral("侧栏二"));
    }

    void contextGroupingSearchesBeyondRecentBlocks()
    {
        QVector<Visnip::OcrTextLine> lines = {
            { QRect(20, 100, 300, 20), QStringLiteral("paragraph first line"), 0.9f },
            { QRect(400, 100, 55, 20), QStringLiteral("item one"), 0.9f },
            { QRect(470, 100, 55, 20), QStringLiteral("item two"), 0.9f },
            { QRect(540, 100, 55, 20), QStringLiteral("item three"), 0.9f },
            { QRect(610, 100, 55, 20), QStringLiteral("item four"), 0.9f },
            { QRect(680, 100, 55, 20), QStringLiteral("item five"), 0.9f },
            { QRect(20, 126, 260, 20), QStringLiteral("paragraph second line"), 0.9f },
        };

        const auto blocks = Visnip::Translate::mergeLinesIntoBlocks(lines);
        QCOMPARE(blocks.size(), 6);
        QCOMPARE(blocks[0].mergedText(),
                 QStringLiteral("paragraph first line paragraph second line"));
    }

    void translationUnitsNeverMergePeerLabelsOrParagraphLines()
    {
        const QVector<Visnip::OcrTextLine> lines = {
            { QRect(80, 381, 45, 25), QStringLiteral("Usage"), 0.99f },
            { QRect(220, 381, 300, 20), QStringLiteral("paragraph first line"), 0.99f },
            { QRect(220, 407, 270, 20), QStringLiteral("paragraph second line"), 0.99f },
            { QRect(80, 412, 57, 26), QStringLiteral("AI usage"), 0.99f },
        };

        const auto contextBlocks =
            Visnip::Translate::mergeLinesIntoBlocks(lines);
        QCOMPARE(contextBlocks.size(), 3);
        const auto units = Visnip::Translate::makeLineTranslationUnits(lines);
        QCOMPARE(units.size(), lines.size());
        for (int index = 0; index < units.size(); ++index) {
            QCOMPARE(units[index].lineBoxes.size(), 1);
            QCOMPARE(units[index].mergedText(), lines[index].text);
        }
    }

    void navigationItemsStartSeparateBlocksAndKeepWrappedText()
    {
        QVector<Visnip::OcrTextLine> lines = {
            { QRect(75, 100, 120, 28), QStringLiteral("Account"), 0.99f,
              QRect(45, 100, 150, 28), true,
              QStringLiteral("repeated-leading-icon-column") },
            { QRect(75, 132, 180, 28), QStringLiteral("Password and"), 0.99f,
              QRect(45, 132, 210, 28), true,
              QStringLiteral("repeated-leading-icon-column") },
            // A wrapped line has no icon and remains attached to its item.
            { QRect(75, 158, 110, 28), QStringLiteral("authentication"), 0.99f },
            { QRect(75, 194, 100, 28), QStringLiteral("Sessions"), 0.99f,
              QRect(45, 194, 130, 28), true,
              QStringLiteral("repeated-leading-icon-column") },
        };

        const auto blocks = Visnip::Translate::mergeLinesIntoBlocks(lines);
        QCOMPARE(blocks.size(), 3);
        QCOMPARE(blocks[0].mergedText(), QStringLiteral("Account"));
        QCOMPARE(blocks[1].mergedText(),
                 QStringLiteral("Password and authentication"));
        QCOMPARE(blocks[1].lines.size(), 2);
        QCOMPARE(blocks[2].mergedText(), QStringLiteral("Sessions"));
    }

    void verticallyOverlappingDetBoxesStillMerge()
    {
        // DB unclip margins expand each line box, so adjacent lines of a
        // tight paragraph can overlap vertically. They must still merge.
        QVector<Visnip::OcrTextLine> lines;
        lines.append({ QRect(20, 100, 300, 30), QStringLiteral("第一行"), 0.9f });
        lines.append({ QRect(20, 112, 300, 30), QStringLiteral("第二行"), 0.9f });

        const auto blocks = Visnip::Translate::mergeLinesIntoBlocks(lines);
        QCOMPARE(blocks.size(), 1);
        QCOMPARE(blocks[0].lines.size(), 2);
    }

    void inkHeightDifferenceSplitsHeadingFromBody()
    {
        // Real-world failure: the det unclip margin flattens box heights, so
        // a heading box (30px) and a body box (28px) look alike while the
        // glyphs actually render at 18px vs 12px. With the image available
        // the measured ink heights must split them.
        QImage source(420, 90, QImage::Format_ARGB32);
        source.fill(Qt::white);
        QPainter painter(&source);
        painter.fillRect(QRect(30, 16, 100, 18), Qt::black); // heading glyphs
        painter.fillRect(QRect(25, 52, 340, 12), Qt::black); // body glyphs
        painter.end();

        QVector<Visnip::OcrTextLine> lines;
        lines.append({ QRect(20, 10, 150, 30), QStringLiteral("Heading"), 0.9f });
        lines.append({ QRect(20, 44, 360, 28), QStringLiteral("body text"), 0.9f });

        // Box heights alone (30 vs 28) would merge them.
        QCOMPARE(Visnip::Translate::mergeLinesIntoBlocks(lines).size(), 1);
        // Ink heights (18 vs 12) keep the heading separate.
        QCOMPARE(Visnip::Translate::mergeLinesIntoBlocks(lines, source).size(), 2);
    }

    void modestlyLargerHeadingStaysSeparate()
    {
        // Headings are often only ~1.3x the body size; they must not fold
        // into the paragraph below (the heading would vanish into the body
        // patch).
        QVector<Visnip::OcrTextLine> lines;
        lines.append({ QRect(20, 60, 200, 26), QStringLiteral("小标题"), 0.9f });
        lines.append({ QRect(20, 100, 300, 20), QStringLiteral("正文第一行"), 0.9f });
        lines.append({ QRect(20, 126, 280, 20), QStringLiteral("正文第二行"), 0.9f });

        const auto blocks = Visnip::Translate::mergeLinesIntoBlocks(lines);
        QCOMPARE(blocks.size(), 2);
        QCOMPARE(blocks[0].mergedText(), QStringLiteral("小标题"));
        QCOMPARE(blocks[1].lines.size(), 2);
    }

    void equallySizedIndentedBodyStartsNewBlock()
    {
        // A standalone flush-left title may use the same font size as the
        // paragraph below. The first short body line can share the title's
        // right edge, but the repeated left indent still marks a new block.
        QVector<Visnip::OcrTextLine> lines;
        lines.append({ QRect(20, 60, 300, 24), QStringLiteral("独立标题"), 0.9f });
        lines.append({ QRect(50, 90, 270, 24), QStringLiteral("正文第一行"), 0.9f });
        lines.append({ QRect(50, 120, 220, 24), QStringLiteral("正文第二行"), 0.9f });

        const auto blocks = Visnip::Translate::mergeLinesIntoBlocks(lines);
        QCOMPARE(blocks.size(), 2);
        QCOMPARE(blocks[0].mergedText(), QStringLiteral("独立标题"));
        QCOMPARE(blocks[1].lines.size(), 2);
        QCOMPARE(blocks[1].mergedText(), QStringLiteral("正文第一行正文第二行"));
    }

    void separatedPeerTitlesStartNewBlocks()
    {
        // Real release-feed layout: an outer title and a same-size title inside
        // the card are separate elements. Their det boxes have a 22px gap;
        // merging them duplicates the source title and creates a tall patch.
        QVector<Visnip::OcrTextLine> lines;
        lines.append({ QRect(44, 136, 155, 28), QStringLiteral("LiveAgent v1.2.0"), 0.9f });
        lines.append({ QRect(62, 185, 169, 28), QStringLiteral("LiveAgent v1.2.0"), 0.9f });

        const auto blocks = Visnip::Translate::mergeLinesIntoBlocks(lines);
        QCOMPARE(blocks.size(), 2);
        QCOMPARE(blocks[0].mergedText(), QStringLiteral("LiveAgent v1.2.0"));
        QCOMPARE(blocks[1].mergedText(), QStringLiteral("LiveAgent v1.2.0"));
    }

    void mutedMetadataStaysSeparateFromWrappedTitle()
    {
        // The changelog cards use equal-size, flush-left text for a light-gray
        // timestamp and a dark two-line title. Geometry alone merges all three
        // lines, but the visual style boundary must leave the timestamp alone
        // while preserving the wrapped title as one translation unit.
        QImage source(340, 100, QImage::Format_ARGB32);
        source.fill(Qt::white);
        QPainter painter(&source);
        painter.fillRect(QRect(62, 24, 30, 10), QColor(98, 98, 98));
        painter.fillRect(QRect(62, 43, 100, 11), QColor(40, 40, 40));
        painter.fillRect(QRect(62, 65, 50, 10), QColor(41, 41, 41));
        painter.end();

        QVector<Visnip::OcrTextLine> lines;
        lines.append({ QRect(62, 19, 65, 20), QStringLiteral("2 days ago"), 0.9f });
        lines.append({ QRect(62, 38, 214, 21),
                       QStringLiteral("Claude Opus 5 is now available in"), 0.9f });
        lines.append({ QRect(62, 60, 101, 20), QStringLiteral("GitHub Copilot"), 0.9f });

        QCOMPARE(Visnip::Translate::mergeLinesIntoBlocks(lines).size(), 1);
        const auto blocks = Visnip::Translate::mergeLinesIntoBlocks(lines, source);
        QCOMPARE(blocks.size(), 2);
        QCOMPARE(blocks[0].mergedText(), QStringLiteral("2 days ago"));
        QCOMPARE(blocks[1].lines.size(), 2);
        QCOMPARE(blocks[1].mergedText(),
                 QStringLiteral("Claude Opus 5 is now available in GitHub Copilot"));
    }

    void wrappedTailMergesDespiteInflatedLinkInk()
    {
        QImage source(700, 90, QImage::Format_ARGB32);
        source.fill(Qt::white);
        QPainter painter(&source);
        // The first line includes a link/underline and descenders, producing a
        // taller measured ink span than the short wrapped tail.
        painter.fillRect(QRect(24, 18, 620, 10), Qt::black);
        painter.fillRect(QRect(420, 30, 120, 2), Qt::black);
        painter.fillRect(QRect(26, 39, 150, 9), Qt::black);
        painter.end();

        QVector<Visnip::OcrTextLine> lines = {
            { QRect(20, 10, 640, 27),
              QStringLiteral("You have set your email address to private. To toggle email privacy, go to email settings and uncheck Keep my"),
              0.95f },
            { QRect(22, 32, 160, 23),
              QStringLiteral("email address private."), 0.95f },
        };
        const auto blocks = Visnip::Translate::mergeLinesIntoBlocks(lines, source);
        QCOMPARE(blocks.size(), 1);
        QCOMPARE(blocks[0].lines.size(), 2);
    }

    void linkedPrivacyTextKeepsBodyFontAndColor()
    {
        QImage source(700, 90, QImage::Format_ARGB32);
        source.fill(Qt::white);
        QPainter painter(&source);
        const QColor body(98, 98, 98);
        const QColor link(9, 105, 218);
        painter.fillRect(QRect(24, 18, 390, 10), body);
        painter.fillRect(QRect(420, 18, 120, 10), link);
        painter.fillRect(QRect(546, 18, 98, 10), body);
        painter.fillRect(QRect(420, 30, 120, 2), link);
        painter.fillRect(QRect(26, 39, 150, 9), body);
        painter.end();

        QVector<Visnip::OcrTextLine> lines = {
            { QRect(20, 10, 640, 27),
              QStringLiteral("You have set your email address to private. To toggle email privacy, go to email settings and uncheck Keep my"),
              0.95f },
            { QRect(22, 32, 160, 23),
              QStringLiteral("email address private."), 0.95f },
        };
        const auto blocks = Visnip::Translate::mergeLinesIntoBlocks(
            lines, source);
        QCOMPARE(blocks.size(), 1);
        const auto style = Visnip::Translate::estimatePatchStyle(
            source, blocks[0].box);
        QVERIFY(qAbs(style.foreground.red() - body.red()) < 30);
        QVERIFY(qAbs(style.foreground.green() - body.green()) < 30);
        QVERIFY(qAbs(style.foreground.blue() - body.blue()) < 30);
        const auto visual = Visnip::Translate::measureLineVisualStyle(
            source, lines[0].box);
        QVERIFY(visual.inkHeight <= 12);
    }

    void shortPeerLabelsStillStaySeparateWhenInkDiffers()
    {
        QImage source(260, 100, QImage::Format_ARGB32);
        source.fill(Qt::white);
        QPainter painter(&source);
        painter.fillRect(QRect(24, 18, 55, 14), Qt::black);
        painter.fillRect(QRect(24, 45, 50, 8), Qt::black);
        painter.end();

        QVector<Visnip::OcrTextLine> lines = {
            { QRect(20, 10, 70, 28), QStringLiteral("Heading"), 0.95f },
            { QRect(20, 37, 65, 22), QStringLiteral("Caption"), 0.95f },
        };
        const auto blocks = Visnip::Translate::mergeLinesIntoBlocks(lines, source);
        QCOMPARE(blocks.size(), 2);
    }

    void gapBeyondTheBlockRhythmStartsANewBlock()
    {
        // Three tightly spaced lines establish a rhythm; a following line
        // whose gap is much larger is a new paragraph even though the gap
        // alone would pass the height-based check.
        QVector<Visnip::OcrTextLine> lines;
        lines.append({ QRect(20, 100, 300, 24), QStringLiteral("一"), 0.9f });
        lines.append({ QRect(20, 129, 300, 24), QStringLiteral("二"), 0.9f });
        lines.append({ QRect(20, 158, 300, 24), QStringLiteral("三"), 0.9f });
        lines.append({ QRect(20, 200, 300, 24), QStringLiteral("四"), 0.9f });

        const auto blocks = Visnip::Translate::mergeLinesIntoBlocks(lines);
        QCOMPARE(blocks.size(), 2);
        QCOMPARE(blocks[0].lines.size(), 3);
        QCOMPARE(blocks[1].lines.size(), 1);
    }

    void composeLeavesLeadingIconOutsideThePatch()
    {
        QImage source(300, 60, QImage::Format_ARGB32);
        source.fill(Qt::white);
        QPainter painter(&source);
        painter.fillRect(QRect(20, 22, 12, 12), Qt::red);    // leading icon
        painter.fillRect(QRect(48, 24, 150, 10), Qt::black); // glyph ink
        painter.end();

        // The det box carries the unclip margin, so it cuts into the icon.
        Visnip::Translate::TextBlock block;
        block.box = QRect(28, 14, 180, 30);
        block.lineBoxes = { block.box };
        block.lines = { QStringLiteral("text") };

        const QImage result = Visnip::Translate::composeTranslatedImage(
            source, { block }, { QStringLiteral("译") });
        // The patch pulls back to the measured ink, leaving the icon intact.
        QCOMPARE(result.pixel(30, 28), source.pixel(30, 28));
        // The original glyphs are still covered.
        QVERIFY(qGray(result.pixel(120, 29)) > 180);
    }

    void refinedMenuPatchCoversFirstGlyphAndKeepsIcon()
    {
        QImage source(220, 60, QImage::Format_ARGB32);
        source.fill(Qt::white);
        QPainter painter(&source);
        painter.fillRect(QRect(20, 22, 14, 14), Qt::red);      // icon
        painter.fillRect(QRect(50, 23, 7, 12), Qt::magenta);  // first glyph
        painter.fillRect(QRect(61, 23, 90, 12), Qt::black);   // remaining label
        painter.end();

        const Visnip::OcrTextLine line(
            QRect(50, 15, 110, 28), QStringLiteral("Account"), 0.99f,
            QRect(16, 15, 144, 28), true,
            QStringLiteral("repeated-leading-icon-column"));
        const auto blocks = Visnip::Translate::mergeLinesIntoBlocks({ line }, source);
        QCOMPARE(blocks.size(), 1);
        QVERIFY(blocks[0].leadingIconSeparated);

        const QImage result = Visnip::Translate::composeTranslatedImage(
            source, blocks, { QStringLiteral("账户") });
        // Recognition excluded the icon, and composition must keep it untouched.
        QCOMPARE(result.pixel(28, 28), source.pixel(28, 28));
        // The patch starts from the refined box edge rather than applying the
        // generic ink inset again, so the first original glyph is covered.
        QVERIFY(result.pixel(50, 29) != source.pixel(50, 29));
        // Translation begins at the original label column instead of moving
        // another 5-8px away from the icon.
        QVERIFY(result.pixel(52, 29) != source.pixel(52, 29));
    }

    void shortLabelUsesClearBackgroundForWiderTranslation()
    {
        QImage source(180, 60, QImage::Format_ARGB32);
        source.fill(Qt::white);
        QPainter painter(&source);
        painter.fillRect(QRect(24, 24, 21, 10), Qt::magenta);
        painter.end();

        Visnip::Translate::TextBlock block;
        block.box = QRect(20, 17, 29, 22);
        block.lineBoxes = { block.box };
        block.lines = { QStringLiteral("Bio") };

        const QImage result = Visnip::Translate::composeTranslatedImage(
            source, { block }, { QStringLiteral("个人简历") });
        // The source glyphs are covered, and the wider translation uses clear
        // background to the right instead of wrapping into a clipped row.
        QVERIFY(result.pixel(24, 29) != source.pixel(24, 29));
        bool changedRight = false;
        for (int y = 17; y <= 39; ++y) {
            for (int x = 50; x <= 72; ++x) {
                changedRight = changedRight
                    || result.pixel(x, y) != source.pixel(x, y);
            }
        }
        QVERIFY(changedRight);
        QCOMPARE(result.pixel(90, 29), source.pixel(90, 29));
    }

    void shortLabelDoesNotExpandAcrossNearbyArtwork()
    {
        QImage source(120, 60, QImage::Format_ARGB32);
        source.fill(Qt::white);
        QPainter painter(&source);
        painter.fillRect(QRect(24, 24, 21, 10), Qt::black);
        painter.fillRect(QRect(52, 17, 20, 22), Qt::red);
        painter.end();

        Visnip::Translate::TextBlock block;
        block.box = QRect(20, 17, 29, 22);
        block.lineBoxes = { block.box };
        block.lines = { QStringLiteral("Bio") };
        Visnip::Translate::CompositionReport report;
        const QImage result = Visnip::Translate::composeTranslatedImage(
            source, { block }, { QStringLiteral("个人简历") }, QString(),
            &report);
        QCOMPARE(result.pixel(60, 28), source.pixel(60, 28));
        QCOMPARE(report.appliedCount, 0);
        QCOMPARE(report.unfitIndices, QVector<int>({ 0 }));
        QCOMPARE(result, source.convertToFormat(QImage::Format_ARGB32_Premultiplied));
    }

    void composeSeparatesFilterLabelFromOverlappingIcon()
    {
        QImage source(949, 60, QImage::Format_ARGB32);
        source.fill(QColor(247, 247, 247));
        QPainter painter(&source);
        // Reproduce the latest diagnostic geometry: the OCR box starts inside
        // the icon, followed by an 8 px separator and the Filter glyphs.
        painter.fillRect(QRect(870, 23, 5, 10), QColor(41, 41, 41));
        painter.fillRect(QRect(883, 22, 32, 10), QColor(41, 41, 41));
        painter.end();

        Visnip::Translate::TextBlock block;
        block.box = QRect(870, 17, 49, 22);
        block.lineBoxes = { block.box };
        block.lines = { QStringLiteral("Filter") };

        const QImage result = Visnip::Translate::composeTranslatedImage(
            source, { block }, { QStringLiteral("过滤器") });
        QCOMPARE(result.pixel(874, 27), source.pixel(874, 27));
        int changedLabelPixels = 0;
        for (int y = 22; y <= 31; ++y) {
            for (int x = 883; x <= 914; ++x) {
                changedLabelPixels += result.pixel(x, y) != source.pixel(x, y);
            }
        }
        QVERIFY(changedLabelPixels > 0);
    }

    void mergedTextJoinsByScript()
    {
        Visnip::Translate::TextBlock cjk;
        cjk.lines = { QStringLiteral("你好"), QStringLiteral("世界") };
        QCOMPARE(cjk.mergedText(), QStringLiteral("你好世界"));

        Visnip::Translate::TextBlock latin;
        latin.lines = { QStringLiteral("Hello"), QStringLiteral("world") };
        QCOMPARE(latin.mergedText(), QStringLiteral("Hello world"));

        Visnip::Translate::TextBlock mixed;
        mixed.lines = { QStringLiteral("你好"), QStringLiteral("world") };
        QCOMPARE(mixed.mergedText(), QStringLiteral("你好 world"));

        Visnip::Translate::TextBlock splitAi;
        splitAi.lines = { QStringLiteral("licenses and Al"),
                          QStringLiteral("Credits.") };
        QCOMPARE(splitAi.mergedText(),
                 QStringLiteral("licenses and AI Credits."));
    }

    void patchStyleMatchesBackgroundAndInk()
    {
        QImage source(120, 60, QImage::Format_ARGB32);
        source.fill(QColor(240, 240, 240));
        const QRect box(30, 20, 60, 20);
        QPainter painter(&source);
        painter.fillRect(QRect(34, 26, 52, 8), QColor(20, 20, 20)); // fake glyphs
        painter.end();

        const auto style = Visnip::Translate::estimatePatchStyle(source, box);
        QVERIFY(qAbs(style.background.red() - 240) <= 8);
        QVERIFY(qAbs(style.background.green() - 240) <= 8);
        QVERIFY(qAbs(style.background.blue() - 240) <= 8);
        QVERIFY(style.foreground.lightness() < 100);
    }

    void composeCoversBlocksWithTranslatedPatches()
    {
        QImage source(200, 80, QImage::Format_ARGB32);
        source.fill(Qt::white);
        QPainter painter(&source);
        painter.fillRect(QRect(24, 30, 120, 10), Qt::black); // original text pixels
        painter.end();

        Visnip::Translate::TextBlock block;
        block.box = QRect(20, 26, 130, 18);
        block.lineBoxes = { block.box };
        block.lines = { QStringLiteral("original") };

        const QImage result = Visnip::Translate::composeTranslatedImage(
            source, { block }, { QStringLiteral("译文") });
        QCOMPARE(result.size(), source.size());
        // The black glyph pixels must be covered by the near-white patch.
        const QRgb covered = result.pixel(80, 35);
        QVERIFY(qGray(covered) > 180);
        // Pixels outside the patch stay untouched.
        QCOMPARE(result.pixel(5, 5), source.pixel(5, 5));
    }

    void sameColumnTitleAndBodyKeepIndependentFontSizes()
    {
        QImage source(260, 170, QImage::Format_ARGB32);
        source.fill(Qt::white);
        QPainter painter(&source);
        const QVector<QRect> boxes = {
            QRect(40, 10, 180, 24),
            QRect(40, 40, 150, 22),
            QRect(40, 70, 150, 22),
            QRect(40, 100, 150, 22),
        };
        const QVector<int> inkHeights = { 12, 9, 9, 9 };
        QVector<Visnip::Translate::TextBlock> blocks;
        QStringList translations;
        for (int index = 0; index < boxes.size(); ++index) {
            const QRect& box = boxes[index];
            const int inkHeight = inkHeights[index];
            painter.fillRect(QRect(box.left() + 5,
                                   box.center().y() - inkHeight / 2,
                                   box.width() - 12,
                                   inkHeight),
                             Qt::black);
            Visnip::Translate::TextBlock block;
            block.box = box;
            block.lineBoxes = { box };
            block.lines = { index == 0 ? QStringLiteral("Title")
                                       : QStringLiteral("Body") };
            blocks.append(block);
            translations.append(index == 0 ? QStringLiteral("标题")
                                            : QStringLiteral("正文")
                                                  + QString::number(index));
        }
        painter.end();

        const QImage result = Visnip::Translate::composeTranslatedImage(
            source, blocks, translations, QStringLiteral("Microsoft YaHei UI"));
        QVector<int> renderedInkHeights;
        for (const QRect& box : boxes) {
            int first = -1;
            int last = -1;
            for (int y = box.top(); y <= box.bottom(); ++y) {
                bool hasInk = false;
                for (int x = box.left(); x <= box.right(); ++x) {
                    if (qGray(result.pixel(x, y)) < 128) {
                        hasInk = true;
                        break;
                    }
                }
                if (hasInk) {
                    first = first < 0 ? y : first;
                    last = y;
                }
            }
            QVERIFY(first >= 0);
            renderedInkHeights.append(last - first + 1);
        }
        QCOMPARE(renderedInkHeights[1], renderedInkHeights[2]);
        QCOMPARE(renderedInkHeights[2], renderedInkHeights[3]);
        QVERIFY(renderedInkHeights[0] >= renderedInkHeights[1] + 2);
    }

    void blockAlignmentFollowsTheLineBoxes()
    {
        Visnip::Translate::TextBlock block;
        block.box = QRect(0, 0, 200, 66);
        // Ragged-right, shared left edge → left-aligned.
        block.lineBoxes = { QRect(0, 0, 200, 18), QRect(0, 24, 140, 18), QRect(0, 48, 180, 18) };
        QCOMPARE(Visnip::Translate::detectBlockAlignment(block), Qt::AlignLeft);
        // Symmetric ragged edges → centered.
        block.lineBoxes = { QRect(0, 0, 200, 18), QRect(30, 24, 140, 18), QRect(10, 48, 180, 18) };
        QCOMPARE(Visnip::Translate::detectBlockAlignment(block), Qt::AlignHCenter);
        // Shared right edge, ragged left → right-aligned.
        block.lineBoxes = { QRect(0, 0, 200, 18), QRect(60, 24, 140, 18), QRect(20, 48, 180, 18) };
        QCOMPARE(Visnip::Translate::detectBlockAlignment(block), Qt::AlignRight);
        // A single line carries no alignment information.
        block.lineBoxes = { QRect(40, 0, 120, 18) };
        QCOMPARE(Visnip::Translate::detectBlockAlignment(block), Qt::AlignLeft);
    }

    void composeRejectsMultiLineTranslationUnits()
    {
        QImage source(400, 200, QImage::Format_ARGB32);
        source.fill(Qt::white);
        Visnip::Translate::TextBlock block;
        block.lineBoxes = { QRect(20, 20, 200, 12), QRect(20, 62, 200, 12) };
        block.box = QRect(20, 20, 200, 54);
        block.lines = { QStringLiteral("first"), QStringLiteral("second") };

        QVERIFY(Visnip::Translate::composeTranslatedImage(
                    source, { block }, { QStringLiteral("translated") })
                    .isNull());
    }

    void composeRejectsTranslationCountMismatch()
    {
        QImage source(120, 50, QImage::Format_ARGB32);
        source.fill(Qt::white);
        Visnip::Translate::TextBlock block;
        block.box = QRect(10, 10, 80, 20);
        block.lineBoxes = { block.box };
        block.lines = { QStringLiteral("source") };

        QVERIFY(Visnip::Translate::composeTranslatedImage(
                    source, { block }, {}).isNull());
        QVERIFY(Visnip::Translate::composeTranslatedImage(
                    source, {}, { QStringLiteral("extra") }).isNull());
        QVERIFY(Visnip::Translate::composeTranslatedImage(
                    source, { block }, { QString() }).isNull());

        Visnip::Translate::CompositionReport report;
        const QImage unchanged = Visnip::Translate::composeTranslatedImage(
            source, { block }, { QStringLiteral("source") }, QString(),
            &report);
        QCOMPARE(report.appliedCount, 0);
        QCOMPARE(report.equivalentIndices, QVector<int>({ 0 }));
        QCOMPARE(unchanged,
                 source.convertToFormat(QImage::Format_ARGB32_Premultiplied));
    }

    void composeRejectsRegionsOutsideTheSourceImage()
    {
        QImage source(120, 50, QImage::Format_ARGB32);
        source.fill(Qt::white);
        Visnip::Translate::TextBlock block;
        block.box = QRect(-4, 10, 80, 20);
        block.lineBoxes = { block.box };
        block.lines = { QStringLiteral("source") };

        Visnip::Translate::CompositionReport report;
        report.appliedCount = 9;
        report.unfitIndices = { 4 };
        QVERIFY(Visnip::Translate::composeTranslatedImage(
                    source, { block }, { QStringLiteral("译文") }, QString(),
                    &report)
                    .isNull());
        QCOMPARE(report.appliedCount, 0);
        QVERIFY(report.equivalentIndices.isEmpty());
        QVERIFY(report.unfitIndices.isEmpty());
    }

    void fastServiceUrlNormalizesToTheServiceRoot()
    {
        const QString fallback = Visnip::aiTranslateDefaultFastServiceUrl();
        QCOMPARE(Visnip::aiTranslateNormalizedFastServiceUrl(QString()), fallback);
        QCOMPARE(Visnip::aiTranslateNormalizedFastServiceUrl(QStringLiteral("   ")), fallback);
        QCOMPARE(Visnip::aiTranslateNormalizedFastServiceUrl(
                     QStringLiteral(" https://host.example/visnip/ ")),
                 QStringLiteral("https://host.example/visnip"));
        // Pasting a full endpoint or the health probe still yields the root.
        QCOMPARE(Visnip::aiTranslateNormalizedFastServiceUrl(
                     QStringLiteral("https://host.example/visnip/v1/translate")),
                 QStringLiteral("https://host.example/visnip"));
        QCOMPARE(Visnip::aiTranslateNormalizedFastServiceUrl(
                     QStringLiteral("https://host.example/visnip/v1/image-translate")),
                 QStringLiteral("https://host.example/visnip"));
        QCOMPARE(Visnip::aiTranslateNormalizedFastServiceUrl(
                     QStringLiteral("https://host.example/visnip/healthz")),
                 QStringLiteral("https://host.example/visnip"));
    }

    void outputFilenamePatternNormalizesSafeRules()
    {
        auto result = Visnip::validateOutputFilenamePattern(
            QStringLiteral("  Visnip_%1  "));
        QVERIFY(result.isValid());
        QCOMPARE(result.normalizedPattern, QStringLiteral("Visnip_%1.png"));

        result = Visnip::validateOutputFilenamePattern(
            QStringLiteral("capture_%1.JPEG"));
        QVERIFY(result.isValid());
        QCOMPARE(result.normalizedPattern, QStringLiteral("capture_%1.JPEG"));

        result = Visnip::validateOutputFilenamePattern(
            QStringLiteral("legacy_$yyyy-MM-dd_HH-mm-ss$.jpg"));
        QVERIFY(result.isValid());
        QCOMPARE(result.normalizedPattern, QStringLiteral("legacy_%1.jpg"));
    }

    void outputFilenamePatternRejectsMissingOrUnknownPlaceholders()
    {
        auto result = Visnip::validateOutputFilenamePattern(
            QStringLiteral("capture.png"));
        QVERIFY(!result.isValid());
        QCOMPARE(result.error,
                 Visnip::OutputFilenamePatternError::MissingTimestampPlaceholder);
        QVERIFY(result.normalizedPattern.isEmpty());

        result = Visnip::validateOutputFilenamePattern(
            QStringLiteral("capture_%2.png"));
        QVERIFY(!result.isValid());
        QCOMPARE(result.error,
                 Visnip::OutputFilenamePatternError::UnsupportedPlaceholder);
    }

    void outputFilenamePatternRejectsPathsAndInvalidCharacters()
    {
        auto result = Visnip::validateOutputFilenamePattern(
            QStringLiteral("../capture_%1.png"));
        QCOMPARE(result.error,
                 Visnip::OutputFilenamePatternError::ParentTraversal);

        result = Visnip::validateOutputFilenamePattern(
            QStringLiteral("folder/capture_%1.png"));
        QCOMPARE(result.error,
                 Visnip::OutputFilenamePatternError::PathSeparator);

        result = Visnip::validateOutputFilenamePattern(
            QStringLiteral("folder\\capture_%1.png"));
        QCOMPARE(result.error,
                 Visnip::OutputFilenamePatternError::PathSeparator);

        result = Visnip::validateOutputFilenamePattern(
            QStringLiteral("capture:%1.png"));
        QCOMPARE(result.error,
                 Visnip::OutputFilenamePatternError::InvalidCharacter);

        result = Visnip::validateOutputFilenamePattern(
            QStringLiteral("capture_%1.webp"));
        QCOMPARE(result.error,
                 Visnip::OutputFilenamePatternError::UnsupportedExtension);
    }

    void serviceTokenOnlyReachesItsOwnReceiver()
    {
        using Visnip::TranslationMethod;
        Visnip::AiTranslateSettings settings;
        settings.translationMethod = TranslationMethod::CloudImage;
        settings.cloudServiceToken = QStringLiteral(" cloud-token_1.~ ");
        settings.intranetServiceToken = QStringLiteral("corp-token");
        QString problem;
        QCOMPARE(settings.serviceAuthorization(QUrl(QStringLiteral("https://cloud.example/v1/image-translate")), &problem),
                 QByteArray("Bearer cloud-token_1.~"));
        QVERIFY(problem.isEmpty());
        // Plain HTTP to a remote host would expose the cloud token.
        QVERIFY(settings.serviceAuthorization(QUrl(QStringLiteral("http://cloud.example/v1/image-translate")), &problem).isEmpty());
        QVERIFY(problem.contains(QStringLiteral("HTTPS")));
        QCOMPARE(settings.serviceAuthorization(QUrl(QStringLiteral("http://127.0.0.1:8000/v1/image-translate"))),
                 QByteArray("Bearer cloud-token_1.~"));
        QCOMPARE(settings.serviceAuthorization(QUrl(QStringLiteral("http://localhost/v1/translate"))),
                 QByteArray("Bearer cloud-token_1.~"));
        settings.translationMethod = TranslationMethod::LocalOcr;
        QCOMPARE(settings.serviceToken(), QStringLiteral("cloud-token_1.~"));
        // An administrator-provided intranet server may use explicit HTTP.
        settings.translationMethod = TranslationMethod::Intranet;
        QCOMPARE(settings.serviceAuthorization(QUrl(QStringLiteral("http://translate.corp/v1/image-translate")), &problem),
                 QByteArray("Bearer corp-token"));
        QVERIFY(problem.isEmpty());
        settings.intranetServiceToken.clear();
        QVERIFY(settings.serviceAuthorization(QUrl(QStringLiteral("https://translate.corp/v1/image-translate")), &problem).isEmpty());
        QVERIFY(problem.isEmpty()); // no token configured is not an error
        settings.translationMethod = TranslationMethod::Offline;
        QVERIFY(settings.serviceToken().isEmpty());
        QVERIFY(settings.serviceAuthorization(QUrl(QStringLiteral("https://cloud.example/x"))).isEmpty());
        // Header injection or non-ASCII is withheld, never sent.
        settings.translationMethod = TranslationMethod::CloudImage;
        for (const QString& invalid : {QStringLiteral("abc\r\nX-Evil: 1"), QStringLiteral("two words"),
                                       QStringLiteral("令牌")}) {
            settings.cloudServiceToken = invalid;
            QVERIFY(settings.serviceAuthorization(QUrl(QStringLiteral("https://cloud.example/x")), &problem).isEmpty());
            QVERIFY(!problem.isEmpty());
        }
    }

    void fastServiceEndpointsFollowTheConfiguredRoot()
    {
        Visnip::AiTranslateSettings settings;
        QCOMPARE(settings.fastProvider, QStringLiteral("official"));
        // Offline by default: no endpoint until the user picks an online mode.
        QCOMPARE(settings.translationMethod, Visnip::TranslationMethod::Offline);
        QVERIFY(settings.fastTranslateEndpoint().isEmpty());
        QVERIFY(settings.fastImageTranslateEndpoint().isEmpty());
        QVERIFY(settings.uploadConsented());
        settings.translationMethod = Visnip::TranslationMethod::CloudImage;
        QVERIFY(!settings.uploadConsented());
        const QString builtIn = Visnip::aiTranslateDefaultFastServiceUrl();
        if (builtIn.isEmpty()) {
            // No service address is compiled in: online modes need one entered.
            QVERIFY(settings.fastTranslateEndpoint().isEmpty());
            QVERIFY(settings.fastImageTranslateEndpoint().isEmpty());
            QVERIFY(settings.fastHealthEndpoint().isEmpty());
        } else {
            QCOMPARE(settings.fastTranslateEndpoint(), builtIn + QStringLiteral("/v1/translate"));
            QCOMPARE(settings.fastImageTranslateEndpoint(), builtIn + QStringLiteral("/v1/image-translate"));
            QCOMPARE(settings.fastHealthEndpoint(), builtIn + QStringLiteral("/healthz"));
        }

        settings.fastServiceUrl = QStringLiteral("http://127.0.0.1:8000/visnip");
        QCOMPARE(settings.fastTranslateEndpoint(),
                 QStringLiteral("http://127.0.0.1:8000/visnip/v1/translate"));
        QCOMPARE(settings.fastImageTranslateEndpoint(),
                 QStringLiteral("http://127.0.0.1:8000/visnip/v1/image-translate"));
        QCOMPARE(settings.fastHealthEndpoint(),
                 QStringLiteral("http://127.0.0.1:8000/visnip/healthz"));

        const QString cloudCacheKey = settings.translationMethodCacheKey();
        QVERIFY(cloudCacheKey.contains(QStringLiteral("cloud-image")));
        settings.translationMethod = Visnip::TranslationMethod::LocalOcr;
        QVERIFY(!settings.usesCloudImageTranslation());
        QVERIFY(settings.translationMethodCacheKey().contains(QStringLiteral("local-ocr")));

        settings.fastProvider = QStringLiteral("baidu");
        settings.baiduAppId = QStringLiteral("appid");
        settings.baiduSecretKey = QStringLiteral("secret-one");
        const QString firstKey = settings.fastProviderCacheKey();
        QVERIFY(!firstKey.contains(settings.baiduAppId));
        QVERIFY(!firstKey.contains(settings.baiduSecretKey));
        settings.baiduSecretKey = QStringLiteral("secret-two");
        QVERIFY(firstKey != settings.fastProviderCacheKey());
    }
};

QTEST_MAIN(CoreTests)
#include "tst_core.moc"
