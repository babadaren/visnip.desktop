#include "core/OfflineResourceManifest.h"
#include "services/OfflineResourceService.h"
#include "services/OfflineTranslationService.h"
#include "services/ImageTranslationService.h"
#include "ui/settings/SettingsDialog.h"
#include <QBuffer>
#include <QAbstractButton>
#include <QComboBox>
#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMessageBox>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkProxyFactory>
#include <QPushButton>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTimer>
#include <QProgressBar>
#include <QLabel>
#include <QToolButton>
#include <QPainter>
#include <QStandardPaths>
#include <QtTest>

namespace Visnip {
namespace {
class FixtureReply final : public QNetworkReply {
public:
    QByteArray bytes;
    qint64 position = 0;
    FixtureReply(const QNetworkRequest& request, QByteArray body, int status, const QByteArray& range,
                 QNetworkReply::NetworkError error, QObject* parent) : QNetworkReply(parent), bytes(std::move(body)) {
        setRequest(request); setUrl(request.url()); open(QIODevice::ReadOnly | QIODevice::Unbuffered);
        setAttribute(QNetworkRequest::HttpStatusCodeAttribute, status);
        setRawHeader("Content-Length", QByteArray::number(bytes.size()));
        if (!range.isEmpty()) setRawHeader("Content-Range", range);
        QTimer::singleShot(0, this, [this, error]() {
            emit metaDataChanged(); emit readyRead();
            if (error != QNetworkReply::NoError) setError(error, QStringLiteral("test network interruption"));
            setFinished(true); emit finished();
        });
    }
    void abort() override { setFinished(true); }
    qint64 bytesAvailable() const override { return bytes.size() - position + QNetworkReply::bytesAvailable(); }
protected:
    qint64 readData(char* output, qint64 maximum) override {
        const qint64 size = qMin<qint64>(maximum, bytes.size() - position);
        if (size == 0) return -1;
        memcpy(output, bytes.constData() + position, size); position += size; return size;
    }
};
class FixtureNetwork final : public QNetworkAccessManager {
public:
    QByteArray data, contentRange;
    int status = 200, calls = 0;
    QNetworkReply::NetworkError failure = QNetworkReply::NoError;
    QNetworkRequest last;
protected:
    QNetworkReply* createRequest(Operation operation, const QNetworkRequest& request, QIODevice* upload) override {
        Q_UNUSED(operation); Q_UNUSED(upload);
        ++calls; last = request;
        return new FixtureReply(request, data, status, contentRange, failure, this);
    }
};
QByteArray envelope() {
    QFile file(QStringLiteral(VISNIP_TEST_SOURCE_DIR) + QStringLiteral("/fixtures-offline-manifest.json"));
    if (!file.open(QIODevice::ReadOnly)) return {};
    return file.readAll();
}
qint64 issuedAt() {
    const auto outside = QJsonDocument::fromJson(envelope()).object();
    return QJsonDocument::fromJson(QByteArray::fromBase64(outside.value(QStringLiteral("payload")).toString().toLatin1()))
        .object().value(QStringLiteral("issued_at")).toInteger();
}
}

class OfflineResourceTests : public QObject {
    Q_OBJECT
private slots:
    void partialSelfTestNeverActivatesAndRecordsRegionReason() {
        for (const auto& reason : {QStringLiteral("equivalent"), QStringLiteral("artwork_collision"), QStringLiteral("layout_unfit")}) {
            QTemporaryDir root; QVERIFY(root.isValid());
            OfflineResourceService manager;
            manager.root_=root.path(); manager.quality_=QStringLiteral("precise"); manager.busy_=true;
            manager.selfTestImage_=QImage(30,30,QImage::Format_RGB32); manager.selfTestImage_.fill(Qt::white);
            ImageTranslationResult result; result.image=manager.selfTestImage_; result.image.setPixelColor(0,0,Qt::black); result.blockCount=3;
            result.blocks=QJsonArray{QJsonObject{{QStringLiteral("status"),QStringLiteral("preserved")},{QStringLiteral("reason"),reason}},
                QJsonObject{{QStringLiteral("status"),QStringLiteral("applied")}},QJsonObject{{QStringLiteral("status"),QStringLiteral("applied")}}};
            QSignalSpy failed(&manager,&OfflineResourceService::failed); QSignalSpy done(&manager,&OfflineResourceService::succeeded);
            emit manager.test_->succeeded(result,200);
            QCOMPARE(failed.size(),1); QCOMPARE(done.size(),0);
            QVERIFY(failed.first().first().toString().contains(QStringLiteral("第 1 段")));
            QVERIFY(!QFileInfo::exists(root.filePath(QStringLiteral("vislate-managed.json"))));
            QFile report(root.filePath(QStringLiteral("selftest-report.json"))); QVERIFY(report.open(QIODevice::ReadOnly));
            QCOMPARE(QJsonDocument::fromJson(report.readAll()).object().value(QStringLiteral("blocks")).toArray().first().toObject().value(QStringLiteral("reason")).toString(),reason);
        }
    }
    void exportNativeSelfTestRasterWhenRequested() {
        const QString path=qEnvironmentVariable("VISNIP_SELFTEST_RASTER");
        if(path.isEmpty()) QSKIP("Explicit diagnostic: export the built-in synthetic image only.");
        QImage input(800,260,QImage::Format_RGB32); input.fill(Qt::white);
        QPainter painter(&input); QFont font(QStringLiteral("Arial")); font.setPixelSize(26);
        painter.setFont(font); painter.setPen(Qt::black);
        painter.drawText(40,65,QStringLiteral("Project settings"));
        painter.drawText(40,125,QStringLiteral("Keep 12 files in the local folder."));
        painter.drawText(40,190,QStringLiteral("Save changes")); painter.end();
        QVERIFY(input.save(path));
        qInfo()<<"Synthetic raster platform"<<QGuiApplication::platformName()<<"dpi"<<input.logicalDpiX()<<"dpr"<<input.devicePixelRatio();
    }
    void residentModelsSurviveNewCaptureAndReleaseWhenIdle() {
        const QString root = qEnvironmentVariable("VISNIP_RESIDENT_TEST_ROOT");
        if (root.isEmpty()) QSKIP("Explicit real resident-model acceptance only.");
        struct Cleanup { ~Cleanup() { OfflineTranslationService::releaseSharedEngine(); qunsetenv("VISNIP_TEST_IDLE_MS"); QStandardPaths::setTestModeEnabled(false); } } cleanup;
        QStandardPaths::setTestModeEnabled(true);
        OfflineTranslationService::releaseSharedEngine();
        OfflineTranslationService::prewarm(root);
        QTRY_VERIFY_WITH_TIMEOUT(OfflineTranslationService::sharedEngineReady(), 120000);
        const qint64 pid = OfflineTranslationService::sharedEngineProcessId();
        QVERIFY(pid > 0);
        for (int index = 0; index < 3; ++index) {
            // A newly constructed facade represents a new capture window.
            OfflineTranslationService service;
            QSignalSpy done(&service, &OfflineTranslationService::succeeded);
            QSignalSpy failed(&service, &OfflineTranslationService::failed);
            QImage input(900,330,QImage::Format_RGB32); input.fill(Qt::white);
            QPainter painter(&input);QFont font(QStringLiteral("Arial"));font.setPixelSize(26);painter.setFont(font);painter.setPen(Qt::black);
            painter.drawText(45,60,QStringLiteral("Project settings"));
            painter.drawText(45,120,QStringLiteral("Keep %1 files in the local folder.").arg(index == 0 ? 12 : 13));
            painter.drawText(45,180,QStringLiteral("Save changes"));painter.end();
            if (index == 2) qputenv("VISNIP_TEST_IDLE_MS", "500");
            service.translate(input,QStringLiteral("zh-Hans"),root,QStringLiteral("precise"));
            QTRY_VERIFY_WITH_TIMEOUT(!done.isEmpty() || !failed.isEmpty(), 120000);
            QVERIFY2(failed.isEmpty(), failed.isEmpty() ? "" : qPrintable(failed.first().at(0).toString()));
            const auto result=qvariant_cast<ImageTranslationResult>(done.first().at(0));
            QVERIFY(result.image != input); QVERIFY(result.notice.isEmpty());
            QCOMPARE(OfflineTranslationService::sharedEngineProcessId(), pid);
            QVERIFY(OfflineTranslationService::sharedEngineReady());
            qInfo()<<"Resident task"<<index<<"ms"<<done.first().at(1).toLongLong()<<"pid"<<pid;
        }
        QTRY_COMPARE_WITH_TIMEOUT(OfflineTranslationService::sharedEngineProcessId(), 0LL, 4000);
        QVERIFY(!OfflineTranslationService::sharedEngineReady());
        // Cancellation during preload must stop the process, not leave it working.
        OfflineTranslationService service;QSignalSpy cancelled(&service,&OfflineTranslationService::cancelled);
        QImage image(30,30,QImage::Format_RGB32);image.fill(Qt::white);
        service.translate(image,QStringLiteral("zh-Hans"),root,QStringLiteral("precise"));
        service.cancel();QCOMPARE(cancelled.count(),1);QCOMPARE(OfflineTranslationService::sharedEngineProcessId(),0LL);
    }
    void actualNetworkProgressCanPauseBeforeInstallation() {
        if (!qEnvironmentVariableIsSet("VISNIP_RESOURCE_NETWORK_TEST")) QSKIP("Explicit live bounded resource request only.");
        QTemporaryDir temp;QVERIFY(temp.isValid());OfflineResourceService service;
        service.cacheRoot_=temp.filePath(QStringLiteral("cache"));
        QSignalSpy paused(&service,&OfflineResourceService::cancelled);QSignalSpy failed(&service,&OfflineResourceService::failed);
        qint64 reported=0,total=0;
        connect(&service,&OfflineResourceService::approvalRequired,&service,[&service](qint64,qint64){service.installApproved();});
        connect(&service,&OfflineResourceService::progress,&service,[&](qint64 bytes,qint64 expected){
            if (bytes>=262144) {reported=bytes;total=expected;service.cancel();}
        });
        service.prepare(QStringLiteral("precise"));
        QTRY_VERIFY_WITH_TIMEOUT(!paused.isEmpty()||!failed.isEmpty(),180000);
        const QString error=failed.isEmpty()?QString():failed.first().at(0).toString();
        QVERIFY2(failed.isEmpty(),qPrintable(error));QCOMPARE(paused.size(),1);
        const auto files=QDir(service.cacheRoot_).entryInfoList({QStringLiteral("*.part")},QDir::Files);
        QCOMPARE(files.size(),1);QCOMPARE(files.first().size(),reported);QVERIFY(total>reported);
        QVERIFY(!service.process_);qInfo()<<"Real HTTP bytes == progress == partial file"<<reported<<"of"<<total;
    }
    void realPreferencesActivationWhenExplicitlyRequested() {
        if (qEnvironmentVariable("VISNIP_RESOURCE_PREFS_REAL_TEST").isEmpty())
            QSKIP("Explicit live acceptance: prepare resources through the preferences button.");
        QTemporaryDir settings; QVERIFY(settings.isValid());
        qputenv("VISNIP_TEST_SETTINGS_FILE", QFile::encodeName(settings.filePath(QStringLiteral("settings.ini"))));
        {
            AppConfig config; SettingsDialog dialog(&config);
            dialog.showPage(SettingsDialog::Page::Translation);
            auto* offline = dialog.findChild<QPushButton*>(QStringLiteral("VisnipSettingsTranslationOffline"));
            auto* button = dialog.findChild<QPushButton*>(QStringLiteral("VisnipSettingsOfflineDownload"));
            auto* service = dialog.findChild<OfflineResourceService*>();
            auto* quality = dialog.findChild<QComboBox*>(QStringLiteral("VisnipSettingsOfflineQuality"));
            QVERIFY(offline && button && service && quality);
            const QString requested = qEnvironmentVariable("VISNIP_RESOURCE_PREFS_QUALITY", QStringLiteral("precise"));
            quality->setCurrentIndex(quality->findData(requested));
            QCOMPARE(quality->currentData().toString(), requested);
            QSignalSpy done(service, &OfflineResourceService::succeeded);
            QSignalSpy failed(service, &OfflineResourceService::failed);
            QTimer answer; answer.setInterval(25);
            connect(&answer, &QTimer::timeout, &dialog, []() {
                for (auto* widget : QApplication::topLevelWidgets()) {
                    auto* message = qobject_cast<QMessageBox*>(widget);
                    if (message && message->isVisible() && message->standardButtons().testFlag(QMessageBox::Yes)) {
                        if (auto* yes = message->button(QMessageBox::Yes)) yes->click();
                    }
                }
            });
            answer.start(); offline->click(); button->click();
            QTRY_VERIFY_WITH_TIMEOUT(!done.isEmpty() || !failed.isEmpty(), 1800000);
            const QString error = failed.isEmpty() ? QString() : failed.first().at(0).toString();
            QVERIFY2(failed.isEmpty(), qPrintable(error));
            QCOMPARE(done.size(), 1);
            QCOMPARE(config.settings().aiTranslate.translationMethod, TranslationMethod::Offline);
            QCOMPARE(config.settings().aiTranslate.offlineResourceDirectory, done.first().at(0).toString());
            AppConfig saved; saved.load();
            QCOMPARE(saved.settings().aiTranslate.offlineResourceDirectory, config.settings().aiTranslate.offlineResourceDirectory);
            QCOMPARE(saved.settings().aiTranslate.offlineQuality, requested);
            qInfo() << "Preferences click -> installation/self-test -> persisted activation passed";
        }
        qunsetenv("VISNIP_TEST_SETTINGS_FILE");
    }
    void realProvisioningWhenExplicitlyRequested() {
        const QString quality = qEnvironmentVariable("VISNIP_RESOURCE_REAL_TEST");
        if (quality.isEmpty()) QSKIP("Set VISNIP_RESOURCE_REAL_TEST=basic/precise to download and install signed resources.");
        QNetworkProxyFactory::setUseSystemConfiguration(true);
        OfflineResourceService service;
        QSignalSpy done(&service, &OfflineResourceService::succeeded);
        QSignalSpy failed(&service, &OfflineResourceService::failed);
        connect(service.test_, &OfflineTranslationService::succeeded, &service,
            [&service](const ImageTranslationResult& result, qint64 ms) {
                qInfo() << "Self-test model result" << result.blockCount << result.notice
                    << "changed" << (result.image.convertToFormat(QImage::Format_RGB32) != service.selfTestImage_)
                    << "milliseconds" << ms;
                const QString prefix = qEnvironmentVariable("VISNIP_RESOURCE_TEST_IMAGES");
                if (!prefix.isEmpty()) {
                    service.selfTestImage_.save(prefix + QStringLiteral("-input.png"));
                    result.image.save(prefix + QStringLiteral("-result.png"));
                }
            });
        connect(&service, &OfflineResourceService::approvalRequired, &service,
                [&service](qint64 bytes, qint64 disk) { qInfo() << "Approved acceptance download bytes" << bytes << "disk" << disk; service.installApproved(); });
        connect(&service, &OfflineResourceService::statusChanged, &service,
                [](const QString& message) { qInfo().noquote() << message; });
        service.prepare(quality);
        QTRY_VERIFY_WITH_TIMEOUT(!done.isEmpty() || !failed.isEmpty(), 1800000);
        const QString error = failed.isEmpty() ? QString() : failed.first().at(0).toString();
        QVERIFY2(failed.isEmpty(), qPrintable(error));
        QCOMPARE(done.size(), 1);
        qInfo() << "Actual download-install-selftest directory" << done.first().at(0).toString() << "test milliseconds" << done.first().at(2).toLongLong();
    }
    void validPinnedSignatureAndExpiry() {
        QVERIFY(!envelope().isEmpty());
        OfflineResourceManifest result; QString error;
        QVERIFY2(OfflineResourceManifest::parse(envelope(), &result, &error, issuedAt() + 60), qPrintable(error));
        QCOMPARE(result.packages.size(), 2);
        QCOMPARE(result.packages[0].id, QStringLiteral("base"));
        QCOMPARE(result.packages[1].id, QStringLiteral("precise"));
        QVERIFY(!OfflineResourceManifest::parse(envelope(), &result, &error, result.expiresAt + 1));
        QVERIFY(error.contains(QStringLiteral("过期")));
    }
    void tamperingNeverAuthorizesExecution() {
        auto object = QJsonDocument::fromJson(envelope()).object();
        auto payload = QByteArray::fromBase64(object.value(QStringLiteral("payload")).toString().toLatin1());
        payload.replace("offline-preview", "offline-exploit");
        object[QStringLiteral("payload")] = QString::fromLatin1(payload.toBase64());
        OfflineResourceManifest result; QString error;
        QVERIFY(!OfflineResourceManifest::parse(QJsonDocument(object).toJson(), &result, &error, issuedAt() + 60));
        QVERIFY(error.contains(QStringLiteral("签名")));
        object = QJsonDocument::fromJson(envelope()).object();
        object[QStringLiteral("key_id")] = QStringLiteral("attacker");
        QVERIFY(!OfflineResourceManifest::parse(QJsonDocument(object).toJson(), &result, &error, issuedAt() + 60));
        QVERIFY(!OfflineResourceManifest::parse(QByteArray(40000, 'x'), &result, &error));
    }
    void sourceAllowlist() {
        const QString good = QStringLiteral("https://vislate.ipxair.com/api/v1/downloads/") + QString(32, 'a');
        QVERIFY(OfflineResourceManifest::isAllowedDownload(QUrl(good)));
        for (const QString& bad : {good + QStringLiteral("?redirect=1"), good + QStringLiteral("#fragment"),
             QString(good).replace(QStringLiteral("https:"), QStringLiteral("http:")),
             QString(good).replace(QStringLiteral("vislate.ipxair.com"), QStringLiteral("evil.example")),
             QString(good).replace(QStringLiteral("https://"), QStringLiteral("https://user:pass@"))})
            QVERIFY(!OfflineResourceManifest::isAllowedDownload(QUrl(bad)));
    }
    void rangeResponsesMustMatchSignedSize() {
        QVERIFY(OfflineResourceManifest::acceptsRange(200, {}, 0, 1000, 1000));
        QVERIFY(OfflineResourceManifest::acceptsRange(206, "bytes 500-999/1000", 500, 1000, 500));
        QVERIFY(!OfflineResourceManifest::acceptsRange(206, "bytes 0-499/1000", 500, 1000, 500));
        QVERIFY(!OfflineResourceManifest::acceptsRange(206, "bytes 500-999/9000", 500, 1000, 500));
        QVERIFY(!OfflineResourceManifest::acceptsRange(200, {}, 500, 1000, 500));
        QVERIFY(!OfflineResourceManifest::acceptsRange(302, {}, 0, 1000, 1000));
    }
    void selectingOfflineNeverDownloadsUntilButtonIsPressed() {
        QTemporaryDir temp; QVERIFY(temp.isValid());
        qputenv("VISNIP_TEST_SETTINGS_FILE", QFile::encodeName(temp.filePath(QStringLiteral("settings.ini"))));
        AppConfig config; SettingsDialog dialog(&config);
        dialog.showPage(SettingsDialog::Page::Translation);
        auto* offline = dialog.findChild<QPushButton*>(QStringLiteral("VisnipSettingsTranslationOffline"));
        auto* download = dialog.findChild<QPushButton*>(QStringLiteral("VisnipSettingsOfflineDownload"));
        auto* manager = dialog.findChild<OfflineResourceService*>();
        QVERIFY(offline && download && manager);
        auto* network = new FixtureNetwork; network->setParent(manager);
        delete manager->network_; manager->network_ = network;
        offline->click(); QTest::qWait(25);
        QCOMPARE(network->calls, 0); QVERIFY(!manager->isBusy());
        QCOMPARE(download->text(), QStringLiteral("下载并启用"));
        for (auto* button : dialog.findChildren<QPushButton*>()) QVERIFY(button->text() != QStringLiteral("资源下载页"));
        qunsetenv("VISNIP_TEST_SETTINGS_FILE");
    }
    void invalidCatalogFailsBeforeResourceDownload() {
        QTemporaryDir temp; OfflineResourceService service;
        auto* network = new FixtureNetwork; network->setParent(&service);
        delete service.network_; service.network_ = network;
        service.cacheRoot_ = temp.filePath(QStringLiteral("cache")); network->data = "{}";
        QSignalSpy failed(&service, &OfflineResourceService::failed);
        service.prepare(QStringLiteral("precise"));
        QTRY_COMPARE(failed.count(), 1); QCOMPARE(network->calls, 1); QVERIFY(!service.isBusy());
        QVERIFY(network->last.rawHeader("Authorization").isEmpty());
        QVERIFY(network->last.rawHeader("Cookie").isEmpty());
    }
    void preferencesEnableOnlyAfterSuccessfulSelfTestSignal() {
        QTemporaryDir temporary; QVERIFY(temporary.isValid());
        qputenv("VISNIP_TEST_SETTINGS_FILE", QFile::encodeName(temporary.filePath(QStringLiteral("settings.ini"))));
        {
            AppConfig config;
            config.mutableSettings().aiTranslate.translationMethod = TranslationMethod::CloudImage;
            SettingsDialog dialog(&config);
            dialog.showPage(SettingsDialog::Page::Translation);
            auto* service = dialog.findChild<OfflineResourceService*>();
            QVERIFY(service);
            auto* quality = dialog.findChild<QComboBox*>(QStringLiteral("VisnipSettingsOfflineQuality"));
            QVERIFY(quality);
            QCOMPARE(quality->currentData().toString(), QStringLiteral("lite"));
            QCOMPARE(config.settings().aiTranslate.offlineQuality, QStringLiteral("lite"));
            // Signal wiring test only; no mock result is a model-quality claim.
            emit service->failed(QStringLiteral("test: download failed"));
            QCOMPARE(config.settings().aiTranslate.translationMethod, TranslationMethod::CloudImage);
            QVERIFY(config.settings().aiTranslate.offlineResourceDirectory.isEmpty());
            const QString root = temporary.filePath(QStringLiteral("verified-resource-test"));
            emit service->succeeded(root, QStringLiteral("precise"), 8000);
            QCOMPARE(config.settings().aiTranslate.translationMethod, TranslationMethod::Offline);
            QCOMPARE(config.settings().aiTranslate.offlineResourceDirectory, root);
            QCOMPARE(config.settings().aiTranslate.offlineQuality, QStringLiteral("precise"));
            AppConfig saved; saved.load();
            QCOMPARE(saved.settings().aiTranslate.offlineResourceDirectory, root);
            QCOMPARE(saved.settings().aiTranslate.translationMethod, TranslationMethod::Offline);
            QCOMPARE(saved.settings().aiTranslate.offlineQuality, QStringLiteral("precise"));
            const QString liteRoot = temporary.filePath(QStringLiteral("verified-lite-test"));
            emit service->succeeded(liteRoot, QStringLiteral("lite"), 3000);
            QCOMPARE(config.settings().aiTranslate.offlineQuality, QStringLiteral("lite"));
            QCOMPARE(config.settings().aiTranslate.offlineResourceDirectory, liteRoot);
            QCOMPARE(quality->currentData().toString(), QStringLiteral("lite"));
            QVERIFY(config.settings().aiTranslate.usesLiteOfflineEngine());
            QVERIFY(!config.settings().aiTranslate.usesWholeImageTranslation());
        }
        qunsetenv("VISNIP_TEST_SETTINGS_FILE");
    }
    void existingAdapterIsNeverOverwrittenWhenItChanges() {
        QTemporaryDir temporary; QVERIFY(temporary.isValid());
        OfflineResourceService service;
        service.root_ = temporary.path(); service.reuse_ = true; service.busy_ = true;
        service.clientAdapter_ = "expected first-party adapter";
        QVERIFY(QDir().mkpath(temporary.filePath(QStringLiteral("vislate_engine"))));
        QFile adapter(temporary.filePath(QStringLiteral("vislate_engine/native_translation.py")));
        QVERIFY(adapter.open(QIODevice::WriteOnly)); adapter.write("modified adapter"); adapter.close();
        QSignalSpy failed(&service, &OfflineResourceService::failed);
        service.selfTest();
        QCOMPARE(failed.count(), 1);
        QVERIFY(!service.test_->isBusy());
        QVERIFY(adapter.open(QIODevice::ReadOnly)); QCOMPARE(adapter.readAll(), QByteArray("modified adapter"));
    }
    void transientHttpPageIsNeverAppendedAndCanRetry() {
        QTemporaryDir temporary; QVERIFY(temporary.isValid());
        OfflineResourceService service;
        auto* network = new FixtureNetwork; network->setParent(&service);
        delete service.network_; service.network_ = network;
        service.cacheRoot_ = temporary.path(); service.busy_ = true;
        OfflineResourcePackage package; package.id = QStringLiteral("base"); package.size = 6;
        package.sha256 = QByteArray(64, 'a');
        package.url = QUrl(QStringLiteral("https://vislate.ipxair.com/api/v1/downloads/") + QString(32, 'b'));
        service.plan_ = {package}; service.totalBytes_ = 6;
        QFile partial(service.packagePath(true));
        QVERIFY(partial.open(QIODevice::WriteOnly)); partial.write("abc"); partial.close();
        network->status = 429; network->data = "rate limit HTML is not package bytes";
        network->failure = QNetworkReply::UnknownContentError;
        QSignalSpy failed(&service, &OfflineResourceService::failed);
        service.requestPackage();
        QTRY_COMPARE(service.retries_, 1);
        QCOMPARE(failed.count(), 0);
        QVERIFY(partial.open(QIODevice::ReadOnly)); QCOMPARE(partial.readAll(), QByteArray("abc")); partial.close();
        network->status = 206; network->data = "def"; network->contentRange = "bytes 3-5/6";
        network->failure = QNetworkReply::NoError;
        QTRY_COMPARE_WITH_TIMEOUT(network->calls, 2, 5000);
        QTRY_COMPARE_WITH_TIMEOUT(failed.count(), 1, 5000); // Fixture intentionally has a wrong hash.
        QCOMPARE(network->last.rawHeader("Range"), QByteArray("bytes=3-5"));
        QVERIFY(failed.first().at(0).toString().contains(QStringLiteral("校验失败")));
        QVERIFY(!service.process_);
    }
    void offlineOffersTwoTiersWithoutNetworkControlsAndProgressUsesBytes() {
        QTemporaryDir temp; QVERIFY(temp.isValid());
        qputenv("VISNIP_TEST_SETTINGS_FILE",QFile::encodeName(temp.filePath(QStringLiteral("settings.ini"))));
        AppConfig config; config.mutableSettings().aiTranslate.translationMethod=TranslationMethod::Offline;
        SettingsDialog dialog(&config); dialog.resize(840,620); dialog.showPage(SettingsDialog::Page::Translation);
        dialog.show();QTest::qWait(50);
        auto* quality=dialog.findChild<QComboBox*>(QStringLiteral("VisnipSettingsOfflineQuality"));QVERIFY(quality);
        QCOMPARE(quality->count(),2);QCOMPARE(quality->itemData(0).toString(),QStringLiteral("lite"));
        QCOMPARE(quality->itemData(1).toString(),QStringLiteral("precise"));QCOMPARE(quality->currentIndex(),0);
        auto* remote=dialog.findChild<QWidget*>(QStringLiteral("VisnipSettingsRemotePanel"));QVERIFY(remote);QVERIFY(!remote->isVisible());
        auto* advanced=dialog.findChild<QToolButton*>(QStringLiteral("VisnipSettingsAdvancedMethods"));QVERIFY(advanced);QVERIFY(!advanced->isVisible());
        auto* bar=dialog.findChild<QProgressBar*>(QStringLiteral("VisnipSettingsOfflineProgress"));QVERIFY(bar);
        auto* manager=dialog.findChild<OfflineResourceService*>();QVERIFY(manager);
        emit manager->phaseChanged(QStringLiteral("download"));emit manager->progress(5242880,10485760);
        QVERIFY(bar->isVisible());QCOMPARE(bar->value(),500);QCOMPARE(bar->maximum(),1000);
        QVERIFY(bar->width()>300);
        emit manager->phaseChanged(QStringLiteral("verify"));emit manager->progress(0,0);
        QVERIFY(!bar->isVisible());QCOMPARE(bar->maximum(),1000);
        emit manager->phaseChanged(QStringLiteral("install"));QVERIFY(!bar->isVisible());
        emit manager->phaseChanged(QStringLiteral("selftest"));QVERIFY(!bar->isVisible());
        const auto output=qEnvironmentVariable("VISNIP_SETTINGS_QA_IMAGE");if(!output.isEmpty()) QVERIFY(dialog.grab().save(output));
        qunsetenv("VISNIP_TEST_SETTINGS_FILE");
    }
    void rateLimitPageNeverBecomesPackageBytes() {
        QTemporaryDir temp;QVERIFY(temp.isValid());OfflineResourceService service;
        auto* network=new FixtureNetwork;network->setParent(&service);delete service.network_;service.network_=network;
        service.cacheRoot_=temp.path();service.busy_=true;service.retries_=3;
        OfflineResourcePackage p;p.id=QStringLiteral("base");p.size=6;p.sha256=QByteArray(64,'a');p.url=QUrl(QStringLiteral("https://vislate.ipxair.com/api/v1/downloads/")+QString(32,'b'));
        service.plan_={p};service.totalBytes_=6;
        QFile file(service.packagePath(true));QVERIFY(file.open(QIODevice::WriteOnly));file.write("abc");file.close();
        network->status=429;network->data="slow down";
        QSignalSpy failed(&service,&OfflineResourceService::failed);service.requestPackage();QTRY_COMPARE(failed.size(),1);
        QVERIFY(file.open(QIODevice::ReadOnly));QCOMPARE(file.readAll(),QByteArray("abc"));QVERIFY(!service.process_);
    }
    void interruptedDownloadKeepsPartialAndResumeRange() {
        QTemporaryDir temp; QVERIFY(temp.isValid());
        OfflineResourceService service; auto* network = new FixtureNetwork; network->setParent(&service);
        delete service.network_; service.network_ = network;
        service.cacheRoot_ = temp.path(); service.busy_ = true;
        OfflineResourcePackage package; package.id = QStringLiteral("base"); package.size = 6;
        package.sha256 = QByteArray(64, 'a'); package.url = QUrl(QStringLiteral("https://vislate.ipxair.com/api/v1/downloads/") + QString(32, 'b'));
        service.plan_ = {package}; service.totalBytes_ = 6;
        QFile partial(service.packagePath(true)); QVERIFY(partial.open(QIODevice::WriteOnly)); partial.write("abc"); partial.close();
        network->status = 206; network->contentRange = "bytes 3-5/6"; network->data = "def";
        // The response body is complete but the simulated network disconnect must prevent execution.
        network->failure = QNetworkReply::RemoteHostClosedError;
        QSignalSpy failed(&service, &OfflineResourceService::failed);
        service.requestPackage(); QTRY_COMPARE(failed.count(), 1);
        QCOMPARE(network->last.rawHeader("Range"), QByteArray("bytes=3-5"));
        QVERIFY(partial.open(QIODevice::ReadOnly)); QCOMPARE(partial.readAll(), QByteArray("abcdef"));
        QVERIFY(!service.process_); QVERIFY(!service.isBusy());
    }
};
}
QTEST_MAIN(Visnip::OfflineResourceTests)
#include "tst_offline_resources.moc"
