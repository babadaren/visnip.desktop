#pragma once

#include "core/OcrPostProcess.h"

#include <QImage>
#include <QObject>
#include <QRect>
#include <QSet>
#include <QString>
#include <QThreadPool>
#include <QVector>

#include <atomic>
#include <functional>

namespace Visnip {

// Local PaddleOCR detection plus selectable PP-OCRv5 recognition packs on
// top of onnxruntime. The detector is shared; recognizers and their exact
// dictionaries are loaded on the serial worker as selected.
class OcrService : public QObject {
    Q_OBJECT
public:
    explicit OcrService(QObject* parent = nullptr);
    ~OcrService() override;

    // Directory holding onnxruntime.dll, the det/rec models and dictionary.
    // Defaults to <application dir>/ocr; VISNIP_OCR_DIR overrides it.
    static QString assetDirectory();
    static bool assetsPresent(const QString& packId,
                              QString* missingFile = nullptr);
    static bool assetsPresent(QString* missingFile = nullptr);

    bool isBusy() const { return busy_; }

    // Loads onnxruntime, the shared detector, and the selected recognizer on
    // the same serial worker used for inference. Duplicate pack requests
    // coalesce; a different pack is never dropped.
    void prewarm(const QString& packId);
    void prewarm();

    quint64 recognize(const QImage& image, const QString& packId);
    quint64 recognize(const QImage& image);

    // Invalidates only this request. A stale caller must not cancel a newer
    // request submitted by another capture overlay.
    void cancel(quint64 revision);

    // Runs the full pipeline on the calling thread; used by tests.
    QVector<OcrTextLine> recognizeSync(const QImage& image,
                                       const QString& packId,
                                       QString* error);
    QVector<OcrTextLine> recognizeSync(const QImage& image, QString* error);

signals:
    void prewarmFinished(const QString& packId, bool ready,
                         const QString& error, qint64 elapsedMs);
    void finished(quint64 revision, const QString& packId,
                  const QVector<Visnip::OcrTextLine>& lines, qint64 elapsedMs);
    void failed(quint64 revision, const QString& packId, const QString& message);

private:
    struct Runtime;

    Runtime* ensureRuntime(const QString& packId, QString* error);
    QVector<OcrTextLine> runPipeline(const QImage& image,
                                     const QString& packId,
                                     const std::function<bool()>& shouldAbort,
                                     QString* error);

    QThreadPool pool_;
    Runtime* runtime_ = nullptr;
    std::atomic<quint64> revision_{ 0 };
    bool busy_ = false;
    QSet<QString> prewarmQueued_;
};

} // namespace Visnip
