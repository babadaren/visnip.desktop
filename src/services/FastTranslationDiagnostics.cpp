#include "services/FastTranslationDiagnostics.h"

#include "core/PerfLog.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>
#include <QStandardPaths>

#include <algorithm>
#include <functional>

namespace Visnip {
namespace {

QString diagnosticsRoot()
{
    const QString base = QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation);
    return QDir(base.isEmpty() ? QDir::tempPath() : base)
        .filePath(QStringLiteral("fast-translation-diagnostics"));
}

QString availableDirectory(const QString& requested)
{
    if (!QFileInfo::exists(requested)) {
        return requested;
    }
    for (int suffix = 2; suffix < 10000; ++suffix) {
        const QString candidate = QStringLiteral("%1_%2").arg(requested).arg(suffix);
        if (!QFileInfo::exists(candidate)) {
            return candidate;
        }
    }
    return {};
}

QJsonObject rectObject(const QRect& rect)
{
    QJsonObject result;
    result.insert(QStringLiteral("x"), rect.x());
    result.insert(QStringLiteral("y"), rect.y());
    result.insert(QStringLiteral("width"), rect.width());
    result.insert(QStringLiteral("height"), rect.height());
    return result;
}

QJsonObject sizeObject(const QSize& size)
{
    QJsonObject result;
    result.insert(QStringLiteral("width"), size.width());
    result.insert(QStringLiteral("height"), size.height());
    return result;
}

bool saveImage(const QImage& image, const QString& path)
{
    if (image.isNull()) {
        return false;
    }
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly)) {
        return false;
    }
    if (!image.save(&file, "PNG")) {
        file.cancelWriting();
        return false;
    }
    return file.commit();
}

bool saveJson(const QJsonObject& object, const QString& path)
{
    const QByteArray data = QJsonDocument(object).toJson(QJsonDocument::Indented);
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly)) {
        return false;
    }
    if (file.write(data) != data.size()) {
        file.cancelWriting();
        return false;
    }
    return file.commit();
}

QJsonObject readJsonObject(const QString& path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        return {};
    }
    return QJsonDocument::fromJson(file.readAll()).object();
}

void updateAnalysis(const QString& directory, const std::function<void(QJsonObject&)>& update)
{
    if (directory.isEmpty()) {
        return;
    }
    const QString path = QDir(directory).filePath(QStringLiteral("analysis.json"));
    QJsonObject object = readJsonObject(path);
    update(object);
    if (!saveJson(object, path)) {
        Perf::log(QStringLiteral("FastTranslate.diagnostics_json_failed file=%1").arg(path));
    }
}

QString beginDiagnostics(const QImage& original,
                         int overlayId,
                         quint64 jobSerial,
                         const QRect& selection,
                         const QString& targetLanguage,
                         const QString& initialStatus,
                         const QJsonObject& details)
{
    // Save user pixels/text only with explicit opt-in; normal logs are metadata.
    if (qEnvironmentVariable("VISNIP_SAVE_TRANSLATION_IMAGES") != QStringLiteral("1")) return {};
    const QDateTime now = QDateTime::currentDateTime();
    const QString directoryName = QStringLiteral("%1_overlay%2_job%3")
        .arg(now.toString(QStringLiteral("yyyyMMdd_HHmmss_zzz")))
        .arg(overlayId)
        .arg(jobSerial);
    const QString directory = availableDirectory(
        QDir(diagnosticsRoot()).filePath(directoryName));
    if (directory.isEmpty() || !QDir().mkpath(directory)) {
        Perf::log(QStringLiteral("FastTranslate.diagnostics_dir_failed dir=%1")
                      .arg(directory));
        return {};
    }

    const QString originalPath = QDir(directory).filePath(QStringLiteral("original.png"));
    if (!saveImage(original, originalPath)) {
        Perf::log(QStringLiteral("FastTranslate.diagnostics_image_failed file=%1")
                      .arg(originalPath));
    }

    QJsonObject analysis = details;
    analysis.insert(QStringLiteral("createdAt"), now.toString(Qt::ISODateWithMs));
    analysis.insert(QStringLiteral("overlayId"), overlayId);
    analysis.insert(QStringLiteral("jobSerial"), static_cast<qint64>(jobSerial));
    analysis.insert(QStringLiteral("targetLanguage"), targetLanguage);
    analysis.insert(QStringLiteral("selection"), rectObject(selection));
    analysis.insert(QStringLiteral("originalFile"), QStringLiteral("original.png"));
    analysis.insert(QStringLiteral("status"), initialStatus);
    const QString analysisPath = QDir(directory).filePath(QStringLiteral("analysis.json"));
    if (!saveJson(analysis, analysisPath)) {
        Perf::log(QStringLiteral("FastTranslate.diagnostics_json_failed file=%1")
                      .arg(analysisPath));
    }
    Perf::log(QStringLiteral("FastTranslate.diagnostics dir=%1")
                  .arg(QDir::toNativeSeparators(directory)));
    return directory;
}

} // namespace

QString FastTranslationDiagnostics::begin(
    const QImage& original,
    int overlayId,
    quint64 jobSerial,
    const QRect& selection,
    const QString& targetLanguage,
    const QString& requestedOcrPackId,
    const QString& resolvedOcrPackId,
    const QString& ocrPackCacheKey)
{
    QJsonObject details;
    details.insert(QStringLiteral("pipeline"), QStringLiteral("local-ocr"));
    details.insert(QStringLiteral("requestedOcrPackId"), requestedOcrPackId);
    details.insert(QStringLiteral("resolvedOcrPackId"), resolvedOcrPackId);
    details.insert(QStringLiteral("ocrPackVersionKey"), ocrPackCacheKey);
    return beginDiagnostics(original, overlayId, jobSerial, selection,
                            targetLanguage, QStringLiteral("ocr-pending"),
                            details);
}

QString FastTranslationDiagnostics::beginImageTranslation(
    const QImage& original,
    int overlayId,
    quint64 jobSerial,
    const QRect& selection,
    const QString& targetLanguage,
    const QString& endpoint)
{
    QJsonObject details;
    details.insert(QStringLiteral("pipeline"), QStringLiteral("cloud-image"));
    details.insert(QStringLiteral("endpoint"), endpoint);
    return beginDiagnostics(original, overlayId, jobSerial, selection,
                            targetLanguage, QStringLiteral("service-pending"),
                            details);
}

void FastTranslationDiagnostics::recordOcr(
    const QString& directory,
    const QVector<OcrTextLine>& rawLines,
    const QVector<Ocr::RejectedTextLine>& rejectedLines,
    const QVector<Translate::TextBlock>& contextBlocks,
    const QVector<Translate::TextBlock>& translationUnits,
    const QVector<Translate::TextBlock>& preservedUnits,
    const QImage& source)
{
    updateAnalysis(directory, [&rawLines, &rejectedLines, &contextBlocks,
                               &translationUnits, &preservedUnits,
                               &source](QJsonObject& analysis) {
        QJsonArray lineArray;
        for (int lineIndex = 0; lineIndex < rawLines.size(); ++lineIndex) {
            const OcrTextLine& line = rawLines[lineIndex];
            QJsonObject object;
            object.insert(QStringLiteral("index"), lineIndex);
            object.insert(QStringLiteral("box"), rectObject(line.box));
            object.insert(QStringLiteral("detectedBox"),
                          rectObject(line.detectedBox.isValid()
                                         ? line.detectedBox
                                         : line.box));
            object.insert(QStringLiteral("leadingIconSeparated"),
                          line.leadingIconSeparated);
            if (!line.refinementReason.isEmpty()) {
                object.insert(QStringLiteral("refinementReason"),
                              line.refinementReason);
            }
            object.insert(QStringLiteral("text"), line.text);
            object.insert(QStringLiteral("score"), line.score);
            const auto rejected = std::find_if(
                rejectedLines.cbegin(), rejectedLines.cend(),
                [lineIndex](const Ocr::RejectedTextLine& item) {
                    return item.lineIndex == lineIndex;
                });
            const auto translated = std::find_if(
                translationUnits.cbegin(), translationUnits.cend(),
                [lineIndex](const Translate::TextBlock& unit) {
                    return unit.sourceLineIndices.contains(lineIndex);
                });
            const auto preserved = std::find_if(
                preservedUnits.cbegin(), preservedUnits.cend(),
                [lineIndex](const Translate::TextBlock& unit) {
                    return unit.sourceLineIndices.contains(lineIndex);
                });
            object.insert(QStringLiteral("acceptedForTranslation"),
                          translated != translationUnits.cend());
            object.insert(QStringLiteral("rejectedFragmentByOcrFilter"),
                          rejected != rejectedLines.cend());
            if (rejected != rejectedLines.cend()) {
                object.insert(QStringLiteral("rejectionReason"), rejected->reason);
                object.insert(QStringLiteral("rejectionPeerIndex"), rejected->peerIndex);
            }
            if (preserved != preservedUnits.cend()) {
                const Translate::BlockTranslationDecision decision =
                    Translate::classifyBlockForTranslation(*preserved);
                object.insert(QStringLiteral("preservationReason"), decision.reason);
                object.insert(QStringLiteral("lexicalGraphemes"),
                              decision.lexicalGraphemes);
            }
            const Translate::LineVisualStyle style =
                Translate::measureLineVisualStyle(source, line.box);
            object.insert(QStringLiteral("inkHeight"), style.inkHeight);
            if (style.backgroundLuma >= 0) {
                object.insert(QStringLiteral("backgroundLuma"), style.backgroundLuma);
            }
            if (style.foregroundMeasured) {
                object.insert(QStringLiteral("foregroundLuma"), style.foregroundLuma);
                object.insert(QStringLiteral("contrast"), style.contrast);
            }
            lineArray.append(object);
        }
        analysis.insert(QStringLiteral("ocrLines"), lineArray);

        const auto blockObject = [](const Translate::TextBlock& block,
                                    int index) {
            QJsonObject object;
            object.insert(QStringLiteral("index"), index);
            object.insert(QStringLiteral("box"), rectObject(block.box));
            object.insert(QStringLiteral("mergedText"), block.mergedText());
            QJsonArray sourceLineIndices;
            for (const int sourceLineIndex : block.sourceLineIndices) {
                sourceLineIndices.append(sourceLineIndex);
            }
            object.insert(QStringLiteral("sourceLineIndices"),
                          sourceLineIndices);
            object.insert(QStringLiteral("alignment"),
                          Translate::detectBlockAlignment(block) == Qt::AlignHCenter
                              ? QStringLiteral("center")
                              : Translate::detectBlockAlignment(block) == Qt::AlignRight
                                  ? QStringLiteral("right")
                                  : QStringLiteral("left"));
            QJsonArray sourceLines;
            for (int blockLineIndex = 0; blockLineIndex < block.lines.size(); ++blockLineIndex) {
                QJsonObject sourceLine;
                sourceLine.insert(QStringLiteral("text"), block.lines[blockLineIndex]);
                if (blockLineIndex < block.lineBoxes.size()) {
                    sourceLine.insert(QStringLiteral("box"), rectObject(block.lineBoxes[blockLineIndex]));
                }
                sourceLines.append(sourceLine);
            }
            object.insert(QStringLiteral("sourceLines"), sourceLines);
            return object;
        };

        QJsonArray blockArray;
        for (int blockIndex = 0; blockIndex < contextBlocks.size(); ++blockIndex) {
            const Translate::TextBlock& block = contextBlocks[blockIndex];
            QJsonObject object = blockObject(block, blockIndex);
            blockArray.append(object);
        }
        analysis.insert(QStringLiteral("contextBlocks"), blockArray);

        QJsonArray translationUnitArray;
        for (int unitIndex = 0; unitIndex < translationUnits.size(); ++unitIndex) {
            const Translate::TextBlock& unit = translationUnits[unitIndex];
            QJsonObject object = blockObject(unit, unitIndex);
            const Translate::BlockTranslationDecision decision =
                Translate::classifyBlockForTranslation(unit);
            object.insert(QStringLiteral("translationEligible"), true);
            object.insert(QStringLiteral("decisionReason"), decision.reason);
            object.insert(QStringLiteral("lexicalGraphemes"),
                          decision.lexicalGraphemes);
            translationUnitArray.append(object);
        }
        analysis.insert(QStringLiteral("translationUnits"), translationUnitArray);
        // Keep the historical blocks -> translations[].blockIndex contract.
        analysis.insert(QStringLiteral("blocks"), translationUnitArray);

        QJsonArray preservedUnitArray;
        for (int unitIndex = 0; unitIndex < preservedUnits.size(); ++unitIndex) {
            const Translate::TextBlock& unit = preservedUnits[unitIndex];
            QJsonObject object = blockObject(unit, unitIndex);
            const Translate::BlockTranslationDecision decision =
                Translate::classifyBlockForTranslation(unit);
            object.insert(QStringLiteral("translationEligible"), false);
            object.insert(QStringLiteral("decisionReason"), decision.reason);
            object.insert(QStringLiteral("lexicalGraphemes"),
                          decision.lexicalGraphemes);
            preservedUnitArray.append(object);
        }
        analysis.insert(QStringLiteral("preservedUnits"), preservedUnitArray);
        analysis.insert(QStringLiteral("status"), QStringLiteral("translation-pending"));
    });
}

void FastTranslationDiagnostics::recordProvider(const QString& directory,
                                                const QString& provider,
                                                bool fallback)
{
    updateAnalysis(directory, [&provider, fallback](QJsonObject& analysis) {
        QJsonArray attempts = analysis.value(QStringLiteral("translationProviders")).toArray();
        QJsonObject attempt;
        attempt.insert(QStringLiteral("provider"), provider);
        attempt.insert(QStringLiteral("fallback"), fallback);
        attempts.append(attempt);
        analysis.insert(QStringLiteral("translationProviders"), attempts);
    });
}

void FastTranslationDiagnostics::recordResult(const QString& directory,
                                              const QImage& translated,
                                              const QStringList& translations,
                                              const Translate::CompositionReport& composition)
{
    if (directory.isEmpty()) {
        return;
    }
    const QString translatedPath = QDir(directory).filePath(QStringLiteral("translated.png"));
    if (!saveImage(translated, translatedPath)) {
        Perf::log(QStringLiteral("FastTranslate.diagnostics_image_failed file=%1").arg(translatedPath));
    }
    updateAnalysis(directory, [&translations, &composition](QJsonObject& analysis) {
        QJsonArray array;
        for (int index = 0; index < translations.size(); ++index) {
            QJsonObject object;
            object.insert(QStringLiteral("blockIndex"), index);
            object.insert(QStringLiteral("text"), translations[index]);
            array.append(object);
        }
        analysis.insert(QStringLiteral("translations"), array);
        QJsonObject compositionObject;
        compositionObject.insert(QStringLiteral("appliedCount"),
                                 composition.appliedCount);
        QJsonArray equivalentIndices;
        for (const int index : composition.equivalentIndices) {
            equivalentIndices.append(index);
        }
        compositionObject.insert(QStringLiteral("equivalentIndices"),
                                 equivalentIndices);
        QJsonArray unfitIndices;
        for (const int index : composition.unfitIndices) {
            unfitIndices.append(index);
        }
        compositionObject.insert(QStringLiteral("unfitIndices"), unfitIndices);
        analysis.insert(QStringLiteral("composition"), compositionObject);
        analysis.insert(QStringLiteral("translatedFile"), QStringLiteral("translated.png"));
        analysis.insert(QStringLiteral("status"),
                        composition.unfitIndices.isEmpty()
                            ? QStringLiteral("succeeded")
                            : QStringLiteral("partial"));
    });
}

void FastTranslationDiagnostics::recordImageResult(
    const QString& directory,
    const QImage& translated,
    const QString& provider,
    const QString& sourceLanguage,
    const QString& targetLanguage,
    const QSize& uploadedImageSize,
    int blockCount,
    qint64 serverElapsedMs,
    qint64 clientElapsedMs)
{
    if (directory.isEmpty()) {
        return;
    }
    const QString translatedPath = QDir(directory).filePath(QStringLiteral("translated.png"));
    if (!saveImage(translated, translatedPath)) {
        Perf::log(QStringLiteral("FastTranslate.diagnostics_image_failed file=%1")
                      .arg(translatedPath));
    }
    updateAnalysis(directory, [&](QJsonObject& analysis) {
        analysis.insert(QStringLiteral("provider"), provider);
        analysis.insert(QStringLiteral("sourceLanguage"), sourceLanguage);
        analysis.insert(QStringLiteral("providerTargetLanguage"), targetLanguage);
        analysis.insert(QStringLiteral("uploadedImageSize"), sizeObject(uploadedImageSize));
        analysis.insert(QStringLiteral("blockCount"), blockCount);
        analysis.insert(QStringLiteral("serverElapsedMs"), serverElapsedMs);
        analysis.insert(QStringLiteral("clientElapsedMs"), clientElapsedMs);
        analysis.insert(QStringLiteral("translatedFile"), QStringLiteral("translated.png"));
        analysis.insert(QStringLiteral("status"), QStringLiteral("succeeded"));
    });
}

void FastTranslationDiagnostics::recordFailure(const QString& directory,
                                               const QString& stage,
                                               const QString& message)
{
    updateAnalysis(directory, [&stage, &message](QJsonObject& analysis) {
        analysis.insert(QStringLiteral("status"), QStringLiteral("failed"));
        analysis.insert(QStringLiteral("failedStage"), stage);
        analysis.insert(QStringLiteral("error"), message);
    });
}

} // namespace Visnip
