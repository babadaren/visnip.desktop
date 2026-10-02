#pragma once
#include "core/OfflineResourceCatalog.h"
#include <QByteArray>
#include <QFile>
#include <QLockFile>
#include <QObject>
#include <QPointer>
#include <QProcess>
#include <QTimer>
#include <memory>

class QNetworkAccessManager;
class QNetworkReply;
namespace Visnip {
class LocalTextTranslationService;
class OfflineResourceTests;

// Resource provisioning uses an explicit user action. Image translation never calls this service.
// Only the lite tier is provisioned. Its files come straight from their
// publishers (core/OfflineResourceCatalog), checked against pinned SHA-256.
class OfflineResourceService final : public QObject {
    Q_OBJECT
public:
    explicit OfflineResourceService(QObject* parent = nullptr);
    ~OfflineResourceService() override;
    bool isBusy() const { return busy_; }
    QString statusText() const { return status_; }
    QString targetQuality() const { return quality_; }
    // Base folder that holds the download cache and the managed installations.
    // An empty value selects the default location in the user's local app data.
    static QString defaultStorageDirectory();
    static QString storageDirectory(const QString& configured = QString());
    static QString cacheDirectory(const QString& configured = QString());
    void prepare(const QString& quality, const QString& storageDirectory = QString());
    void installApproved();
    void cancel();
    // Removes the installed lite files, the enable receipt and the downloaded
    // archives. The caller owns the user-visible confirmation.
    static bool removeInstalled(const QString& root, const QString& storageRoot = QString(),
                                QString* error = nullptr);
signals:
    void busyChanged(bool busy);
    void statusChanged(const QString& status);
    void phaseChanged(const QString& phase);
    void progress(qint64 received, qint64 total);
    void approvalRequired(qint64 downloadBytes, qint64 requiredDiskBytes);
    void succeeded(const QString& directory, const QString& quality, qint64 testMs);
    void failed(const QString& message);
    void cancelled();
private:
    friend class OfflineResourceTests;
    void status(const QString& text);
    void reject(const QString& message);
    void finish();
    void fail(const QString& message);
    void preparePlan();
    void nextFile();
    void checkCached(bool completeFile);
    void requestFile();
    void readFile();
    void fileFinished();
    void retryOrSwitch(bool transient, const QString& reason);
    void switchSource(const QString& reason);
    void installFile();
    void extractionFinished(int exitCode, QProcess::ExitStatus exitStatus);
    void stopExtractor();
    void selfTest();
    void recordActivation(qint64 ms);
    void releaseReply();
    QString filePath(bool partial = false) const;
    QString sourceName() const;
    QNetworkAccessManager* network_ = nullptr;
    QPointer<QNetworkReply> reply_;
    LocalTextTranslationService* liteTest_ = nullptr;
    QPointer<QProcess> extractor_;
    QTimer extractTimeout_;
    // Watches the delivered bytes: an attempt that dries up or trickles is
    // handed to the retry or fallback source without a manual pause.
    QTimer transferCheck_;
    qint64 healthyBytes_ = 0;
    QByteArray pendingHead_;
    std::unique_ptr<QLockFile> lock_;
    QFile output_;
    QVector<OfflineResourceFile> plan_;
    QString root_, quality_, status_, cacheRoot_;
    int index_ = 0;
    int source_ = 0;
    int retries_ = 0;
    quint64 serial_ = 0;
    qint64 offset_ = 0, requestedEnd_ = 0, completedBytes_ = 0, totalBytes_ = 0;
    bool busy_ = false, awaitingApproval_ = false, headersChecked_ = false, reuse_ = false;
    bool redirectRejected_ = false, badResponse_ = false;
};
}
