#pragma once
#include "core/OfflineResourceManifest.h"
#include <QFile>
#include <QImage>
#include <QLockFile>
#include <QObject>
#include <QPointer>
#include <QMap>
#include <QTimer>
#include <memory>

class QNetworkAccessManager;
class QNetworkReply;
namespace Visnip {
class LocalTextTranslationService;
class OfflineTranslationService;
class OfflineResourceTests;

// Resource provisioning uses an explicit user action. Image translation never calls this service.
class OfflineResourceService final : public QObject {
    Q_OBJECT
public:
    explicit OfflineResourceService(QObject* parent = nullptr);
    ~OfflineResourceService() override;
    bool isBusy() const { return busy_; }
    QString statusText() const { return status_; }
    QString targetQuality() const { return quality_; }
    static QString cacheDirectory();
    void prepare(const QString& quality);
    void installApproved();
    void cancel();
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
    void finish();
    void fail(const QString& message);
    void preparePlan();
    void nextPackage();
    void checkCached(bool completeFile);
    void requestPackage();
    void readPackage();
    void packageFinished();
    void installPackage();
    void pollInstaller();
    void stopInstaller();
    void selfTest();
    void recordActivation(qint64 ms);
    bool stageClientCode();
    void releaseReply();
    QString packagePath(bool partial = false) const;
    QNetworkAccessManager* network_ = nullptr;
    QPointer<QNetworkReply> reply_;
    OfflineTranslationService* test_ = nullptr;
    LocalTextTranslationService* liteTest_ = nullptr;
    std::unique_ptr<QLockFile> lock_;
    QFile output_;
    QImage selfTestImage_;
    QTimer installerPoll_;
    void* process_ = nullptr;
    void* job_ = nullptr;
    qint64 installStarted_ = 0;
    QByteArray manifestBody_;
    QByteArray clientAdapter_;
    QMap<QString,QByteArray> clientModules_;
    OfflineResourceManifest manifest_;
    QVector<OfflineResourcePackage> plan_;
    QString root_, quality_, status_, cacheRoot_;
    int index_ = 0;
    int retries_ = 0;
    quint64 serial_ = 0;
    qint64 offset_ = 0, requestedEnd_ = 0, completedBytes_ = 0, totalBytes_ = 0;
    bool busy_ = false, awaitingApproval_ = false, headersChecked_ = false, reuse_ = false;
};
}
