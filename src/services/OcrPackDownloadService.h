#pragma once

#include <QFile>
#include <QObject>
#include <QPointer>
#include <QStringList>

class QNetworkAccessManager;
class QNetworkReply;

namespace Visnip {

// Downloads one catalogued recognition pack into AppLocalDataLocation. Both
// files are SHA-256 verified before the temporary directory is atomically
// renamed into place; partial or mismatched assets are never exposed to OCR.
class OcrPackDownloadService : public QObject {
    Q_OBJECT
public:
    explicit OcrPackDownloadService(QObject* parent = nullptr);

    bool isBusy() const { return !reply_.isNull(); }
    QString activePackId() const { return packId_; }

    void install(const QString& packId);
    bool remove(const QString& packId, QString* error = nullptr);
    void cancel();

signals:
    void progress(const QString& packId, qint64 received, qint64 total);
    void succeeded(const QString& packId);
    void failed(const QString& packId, const QString& message);
    void cancelled(const QString& packId);

private:
    enum class Part {
        Model,
        Dictionary,
    };

    void beginPart(Part part);
    void tryNextUrl();
    void handleFinished();
    void finishInstall();
    void fail(const QString& message);
    void reset();

    QNetworkAccessManager* nam_ = nullptr;
    QPointer<QNetworkReply> reply_;
    QFile output_;
    QString packId_;
    QString temporaryDirectory_;
    QStringList urls_;
    int urlIndex_ = 0;
    Part part_ = Part::Model;
    qint64 completedBytes_ = 0;
    bool userCancelled_ = false;
};

} // namespace Visnip
