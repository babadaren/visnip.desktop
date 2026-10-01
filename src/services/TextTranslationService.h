#pragma once

#include "core/AppConfig.h"

#include <QElapsedTimer>
#include <QObject>
#include <QPointer>
#include <QStringList>
#include <QUrl>
#include <QVector>

class QNetworkAccessManager;
class QNetworkReply;

namespace Visnip {

// Batch text translation for the local OCR capture-translate mode. The selected
// provider receives stable one-line OCR units in order. A failed Baidu attempt
// fails the job; the text is never re-sent to a different receiver.
class TextTranslationService : public QObject {
    Q_OBJECT
public:
    static constexpr int kMaxTextsPerRequest = 64;
    static constexpr int kMaxCharsPerText = 2000;

    explicit TextTranslationService(AppConfig* config, QObject* parent = nullptr);

    bool isBusy() const { return !reply_.isNull(); }

    void translate(const QStringList& paragraphs, const QString& languageCode);
    void cancel();

    // Connectivity probe for the built-in Visnip service: GET <root>/healthz.
    void checkHealth(const QString& serviceUrl);

    static QByteArray buildRequestPayload(const QStringList& texts,
                                          const QString& targetLanguage);
    static QStringList parseTranslations(const QByteArray& body,
                                         QString* error,
                                         int expectedCount = -1);
    static QString normalizeTranslationForTarget(const QString& text,
                                                 const QString& targetLanguage);
    static QString restoreProtectedIdentifiers(const QString& source,
                                               const QString& translation);
    static QString preservePersonalName(const QString& source,
                                        const QString& translation);

    static QString baiduLanguageCode(const QString& languageCode);
    static QByteArray buildBaiduRequestPayload(const QStringList& texts,
                                               const QString& targetLanguage,
                                               const QString& appId,
                                               const QString& secretKey,
                                               const QString& salt,
                                               QString* error);
    static QStringList parseBaiduTranslations(const QByteArray& body,
                                              const QStringList& expectedTexts,
                                              QString* error);

signals:
    void succeeded(const QStringList& translations, qint64 elapsedMs);
    void failed(const QString& message);
    void cancelled();
    void healthChecked(bool ok, const QString& message);
    void providerChanged(const QString& provider, bool fallback);

private:
    enum class Provider {
        Official,
        Baidu,
    };

    void prepareBatches();
    void sendNextBatch();
    void handleReplyFinished();
    void handleHealthFinished();
    void finishFailure(const QString& message);
    QString mapNetworkError(QNetworkReply* reply, const QByteArray& body) const;

    AppConfig* config_ = nullptr;
    QNetworkAccessManager* nam_ = nullptr;
    QPointer<QNetworkReply> reply_;
    QPointer<QNetworkReply> healthReply_;
    QElapsedTimer jobTimer_;
    QUrl endpoint_;
    QUrl officialEndpoint_;
    QByteArray officialAuthorization_;
    QString targetLanguage_;
    QString baiduAppId_;
    QString baiduSecretKey_;
    QStringList paragraphs_;
    int timeoutMs_ = 120000;
    QVector<QStringList> batches_;
    int batchIndex_ = 0;
    QStringList collected_;
    Provider provider_ = Provider::Official;
    bool userCancelled_ = false;
};

} // namespace Visnip
