#pragma once

#include "core/OcrPostProcess.h"
#include "core/TranslationCompositor.h"

#include <QImage>
#include <QRect>
#include <QSize>
#include <QString>
#include <QStringList>
#include <QVector>

namespace Visnip {

// Local diagnostic artifacts for one fast-translation request. Each request
// gets its own directory under AppLocalDataLocation so the source pixels, OCR
// geometry/grouping and final composition can be inspected together.
class FastTranslationDiagnostics final {
public:
    static QString begin(const QImage& original,
                         int overlayId,
                         quint64 jobSerial,
                         const QRect& selection,
                         const QString& targetLanguage,
                         const QString& requestedOcrPackId,
                         const QString& resolvedOcrPackId,
                         const QString& ocrPackCacheKey);
    static QString beginImageTranslation(const QImage& original,
                                         int overlayId,
                                         quint64 jobSerial,
                                         const QRect& selection,
                                         const QString& targetLanguage,
                                         const QString& endpoint);
    static void recordOcr(
        const QString& directory,
        const QVector<OcrTextLine>& rawLines,
        const QVector<Ocr::RejectedTextLine>& rejectedLines,
        const QVector<Translate::TextBlock>& contextBlocks,
        const QVector<Translate::TextBlock>& translationUnits,
        const QVector<Translate::TextBlock>& preservedUnits,
        const QImage& source);
    static void recordProvider(const QString& directory,
                               const QString& provider,
                               bool fallback);
    static void recordResult(const QString& directory,
                             const QImage& translated,
                             const QStringList& translations,
                             const Translate::CompositionReport& composition);
    static void recordImageResult(const QString& directory,
                                  const QImage& translated,
                                  const QString& provider,
                                  const QString& sourceLanguage,
                                  const QString& targetLanguage,
                                  const QSize& uploadedImageSize,
                                  int blockCount,
                                  qint64 serverElapsedMs,
                                  qint64 clientElapsedMs);
    static void recordFailure(const QString& directory,
                              const QString& stage,
                              const QString& message);
};

} // namespace Visnip
