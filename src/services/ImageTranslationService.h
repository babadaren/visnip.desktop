#pragma once

#include "core/AppConfig.h"

#include <QByteArray>
#include <QElapsedTimer>
#include <QImage>
#include <QJsonArray>
#include <QMetaType>
#include <QObject>
#include <QPointer>
#include <QSize>
#include <QString>

class QNetworkAccessManager;
class QNetworkReply;

namespace Visnip {
class OfflineTranslationService;

struct ImageTranslationResult {
    QImage image;
    QString provider;
    QString sourceLanguage;
    QString targetLanguage;
    QString requestedTargetLanguage;
    QSize uploadedImageSize;
    int blockCount = 0;
    QJsonArray blocks; // Kept in memory; only synthetic self-tests persist their diagnostic report.
    qint64 serverElapsedMs = -1;
    QString notice;
};

// Whole-image translation for fast capture mode. The service owns only the
// transport/protocol boundary; selecting the translated image as the overlay
// base remains the caller's responsibility.
class ImageTranslationService : public QObject {
    Q_OBJECT
public:
    static constexpr qint64 kMaxUploadImageBytes = 4LL * 1024 * 1024;
    static constexpr qint64 kMaxResponseBytes = 16LL * 1024 * 1024;
    static constexpr qint64 kMaxTranslatedImageBytes = 8LL * 1024 * 1024;

    explicit ImageTranslationService(AppConfig* config, QObject* parent = nullptr);

    bool isBusy() const;

    void preconnectConfiguredEndpoint();
    void translate(const QImage& image, const QString& targetLanguage);
    void cancel();

    static QByteArray buildRequestPayload(const QImage& image,
                                          const QString& targetLanguage,
                                          QString* error = nullptr,
                                          QSize* uploadedImageSize = nullptr);
    static ImageTranslationResult parseResponse(const QByteArray& body,
                                                const QSize& expectedImageSize,
                                                QString* error = nullptr);

signals:
    void phaseChanged(const QString& phase);
    void succeeded(const Visnip::ImageTranslationResult& result, qint64 elapsedMs);
    void failed(const QString& message);
    void cancelled();

private:
    void drainResponseBody();
    void handleReplyFinished();
    QString mapNetworkError(QNetworkReply* reply, const QByteArray& body) const;

    AppConfig* config_ = nullptr;
    OfflineTranslationService* offline_ = nullptr;
    QNetworkAccessManager* nam_ = nullptr;
    QPointer<QNetworkReply> reply_;
    QElapsedTimer jobTimer_;
    QByteArray responseBody_;
    QSize originalImageSize_;
    QSize expectedImageSize_;
    QString requestedTargetLanguage_;
    QString preconnectedEndpointKey_;
    bool networkStackWarmed_ = false;
    bool networkStackWarmupPending_ = false;
    bool responseLimitExceeded_ = false;
    bool firstResponseChunkLogged_ = false;
    bool userCancelled_ = false;
};

} // namespace Visnip

Q_DECLARE_METATYPE(Visnip::ImageTranslationResult)
