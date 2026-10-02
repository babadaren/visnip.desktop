#include "core/OfflineResourceCatalog.h"
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
#include <QRegularExpression>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTimer>
#include <QProgressBar>
#include <QLabel>
#include <QLineEdit>
#include <QToolButton>
#include <QPainter>
#include <QStandardPaths>
#include <QtTest>
#ifdef Q_OS_WIN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

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
    QString failingHost; // answers 404 for this host
    QNetworkRequest last;
protected:
    QNetworkReply* createRequest(Operation operation, const QNetworkRequest& request, QIODevice* upload) override {
        Q_UNUSED(operation); Q_UNUSED(upload);
        ++calls; last = request;
        if (request.url().host() == failingHost)
            return new FixtureReply(request, "not found", 404, {}, QNetworkReply::ContentNotFoundError, this);
        return new FixtureReply(request, data, status, contentRange, failure, this);
    }
};
// A source that accepts the request and then goes silent, like a CDN that
// keeps the connection open without ever sending the body.
class SilentReply final : public QNetworkReply {
public:
    SilentReply(const QNetworkRequest& request, QObject* parent) : QNetworkReply(parent) {
        setRequest(request); setUrl(request.url()); open(QIODevice::ReadOnly | QIODevice::Unbuffered);
    }
    void abort() override {
        setError(QNetworkReply::OperationCanceledError, QStringLiteral("stalled"));
        setFinished(true);
        QTimer::singleShot(0, this, [this]() { emit finished(); });
    }
    qint64 bytesAvailable() const override { return QNetworkReply::bytesAvailable(); }
protected:
    qint64 readData(char*, qint64) override { return -1; }
};
class SilentNetwork final : public QNetworkAccessManager {
public:
    int calls = 0;
protected:
    QNetworkReply* createRequest(Operation, const QNetworkRequest& request, QIODevice*) override {
        ++calls;
        return new SilentReply(request, this);
    }
};
OfflineResourceFile fixtureFile(const QStringList& sources)
{
    OfflineResourceFile file;
    file.id = QStringLiteral("fixture"); file.label = QStringLiteral("fixture");
    for (const QString& source : sources) file.sources.append(QUrl(source));
    file.size = 6; file.sha256 = QByteArray(64, 'a'); // intentionally wrong hash
    file.install = OfflineResourceFile::Install::ExtractZip; file.target = QStringLiteral("llama");
    return file;
}
const QString kGitHub = QStringLiteral("https://github.com/ggml-org/llama.cpp/releases/download/b1/fixture.zip");
}

class OfflineResourceTests : public QObject {
    Q_OBJECT
private slots:
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
        service.prepare(QStringLiteral("lite"));
        QTRY_VERIFY_WITH_TIMEOUT(!paused.isEmpty()||!failed.isEmpty(),180000);
        const QString error=failed.isEmpty()?QString():failed.first().at(0).toString();
        QVERIFY2(failed.isEmpty(),qPrintable(error));QCOMPARE(paused.size(),1);
        const auto files=QDir(service.cacheRoot_).entryInfoList({QStringLiteral("*.part")},QDir::Files);
        QCOMPARE(files.size(),1);QCOMPARE(files.first().size(),reported);QVERIFY(total>reported);
        QVERIFY(!service.extractor_);qInfo()<<"Real HTTP bytes == progress == partial file"<<reported<<"of"<<total;
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
            const QString requested = QStringLiteral("lite");
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
        if (quality.isEmpty()) QSKIP("Set VISNIP_RESOURCE_REAL_TEST=lite to download and install the official resources.");
        QNetworkProxyFactory::setUseSystemConfiguration(true);
        OfflineResourceService service;
        QSignalSpy done(&service, &OfflineResourceService::succeeded);
        QSignalSpy failed(&service, &OfflineResourceService::failed);
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
    void catalogPinsOfficialFilesOnly() {
        const auto files = OfflineResourceCatalog::liteFiles();
        QCOMPARE(files.size(), 2);
        QCOMPARE(files[0].target, QStringLiteral("llama"));
        QCOMPARE(files[0].install, OfflineResourceFile::Install::ExtractZip);
        QCOMPARE(files[1].target, QStringLiteral("models/Hy-MT2-1.8B-Q4_K_M.gguf"));
        QCOMPARE(files[1].install, OfflineResourceFile::Install::Place);
        QCOMPARE(files[1].sources.size(), 2);
        QCOMPARE(files[1].sources[0].host(), QStringLiteral("www.modelscope.cn"));
        QCOMPARE(files[1].sources[1].host(), QStringLiteral("huggingface.co"));
        for (const auto& file : files) {
            QVERIFY(QRegularExpression(QStringLiteral("^[a-f0-9]{64}$")).match(QString::fromLatin1(file.sha256)).hasMatch());
            QVERIFY(file.size > 0);
            for (const QUrl& source : file.sources) {
                QVERIFY2(OfflineResourceCatalog::isAllowedDownload(source), qPrintable(source.toString()));
                QVERIFY(!source.hasQuery());
                QVERIFY(!source.host().contains(QStringLiteral("ipxair")) && !source.host().contains(QStringLiteral("visnip")));
            }
        }
    }
    void downloadsFollowOnlyOfficialHosts() {
        for (const QString& good : {QStringLiteral("https://github.com/ggml-org/llama.cpp/releases/download/b1/a.zip"),
             QStringLiteral("https://release-assets.githubusercontent.com/x?sig=1&exp=2"),
             QStringLiteral("https://cdn-lfs-cn-1.modelscope.cn/prod/lfs-objects/dc/5f/abc"),
             QStringLiteral("https://us.aws.cdn.hf.co/x?Signature=1"),
             QStringLiteral("https://cdn-lfs.huggingface.co/x")})
            QVERIFY2(OfflineResourceCatalog::isAllowedDownload(QUrl(good)), qPrintable(good));
        for (const QString& bad : {QStringLiteral("http://github.com/a.zip"),
             QStringLiteral("https://evil.example/a.zip"), QStringLiteral("https://evilgithub.com/a.zip"),
             QStringLiteral("https://github.com.evil.example/a.zip"), QStringLiteral("https://user:pass@github.com/a.zip"),
             QStringLiteral("https://github.com:8443/a.zip"), QStringLiteral("https://github.com/a.zip#x"),
             QStringLiteral("https://vislate.ipxair.com/api/v1/downloads/") + QString(32, 'a')})
            QVERIFY2(!OfflineResourceCatalog::isAllowedDownload(QUrl(bad)), qPrintable(bad));
    }
    void rangeResponsesMustMatchSignedSize() {
        QVERIFY(OfflineResourceCatalog::acceptsRange(200, {}, 0, 1000, 1000));
        QVERIFY(OfflineResourceCatalog::acceptsRange(206, "bytes 500-999/1000", 500, 1000, 500));
        QVERIFY(!OfflineResourceCatalog::acceptsRange(206, "bytes 0-499/1000", 500, 1000, 500));
        QVERIFY(!OfflineResourceCatalog::acceptsRange(206, "bytes 500-999/9000", 500, 1000, 500));
        QVERIFY(!OfflineResourceCatalog::acceptsRange(200, {}, 500, 1000, 500));
        QVERIFY(!OfflineResourceCatalog::acceptsRange(302, {}, 0, 1000, 1000));
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
    void preciseTierIsNeverDownloaded() {
        QTemporaryDir temp; OfflineResourceService service;
        auto* network = new FixtureNetwork; network->setParent(&service);
        delete service.network_; service.network_ = network;
        service.cacheRoot_ = temp.filePath(QStringLiteral("cache"));
        QSignalSpy failed(&service, &OfflineResourceService::failed);
        service.prepare(QStringLiteral("precise"));
        QCOMPARE(failed.count(), 1); QCOMPARE(network->calls, 0); QVERIFY(!service.isBusy());
        QVERIFY(failed.first().first().toString().contains(QStringLiteral("官方")));
        QCOMPARE(service.statusText(), failed.first().first().toString());
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
    void transientHttpPageIsNeverAppendedAndCanRetry() {
        QTemporaryDir temporary; QVERIFY(temporary.isValid());
        OfflineResourceService service;
        auto* network = new FixtureNetwork; network->setParent(&service);
        delete service.network_; service.network_ = network;
        service.cacheRoot_ = temporary.path(); service.busy_ = true;
        service.plan_ = {fixtureFile({kGitHub})}; service.totalBytes_ = 6;
        QFile partial(service.filePath(true));
        QVERIFY(partial.open(QIODevice::WriteOnly)); partial.write("abc"); partial.close();
        network->status = 429; network->data = "rate limit HTML is not package bytes";
        network->failure = QNetworkReply::UnknownContentError;
        QSignalSpy failed(&service, &OfflineResourceService::failed);
        service.requestFile();
        QTRY_COMPARE(service.retries_, 1);
        QCOMPARE(failed.count(), 0);
        QVERIFY(partial.open(QIODevice::ReadOnly)); QCOMPARE(partial.readAll(), QByteArray("abc")); partial.close();
        network->status = 206; network->data = "def"; network->contentRange = "bytes 3-5/6";
        network->failure = QNetworkReply::NoError;
        QTRY_COMPARE_WITH_TIMEOUT(network->calls, 2, 5000);
        QTRY_COMPARE_WITH_TIMEOUT(failed.count(), 1, 5000); // Fixture intentionally has a wrong hash.
        QCOMPARE(network->last.rawHeader("Range"), QByteArray("bytes=3-"));
        QVERIFY(failed.first().at(0).toString().contains(QStringLiteral("校验失败")));
        QVERIFY(!QFileInfo::exists(service.filePath(true))); // never kept, never extracted
        QVERIFY(!service.extractor_);
    }
    void fallbackSourceContinuesFromTheSamePartialFile() {
        QTemporaryDir temporary; QVERIFY(temporary.isValid());
        OfflineResourceService service;
        auto* network = new FixtureNetwork; network->setParent(&service);
        delete service.network_; service.network_ = network;
        service.cacheRoot_ = temporary.path(); service.busy_ = true;
        service.plan_ = {fixtureFile({QStringLiteral("https://www.modelscope.cn/models/x/resolve/1/m.gguf"),
                                      QStringLiteral("https://huggingface.co/x/resolve/1/m.gguf")})};
        service.totalBytes_ = 6;
        QFile partial(service.filePath(true));
        QVERIFY(partial.open(QIODevice::WriteOnly)); partial.write("abc"); partial.close();
        network->failingHost = QStringLiteral("www.modelscope.cn");
        network->status = 206; network->data = "def"; network->contentRange = "bytes 3-5/6";
        QSignalSpy failed(&service, &OfflineResourceService::failed);
        service.requestFile();
        QTRY_COMPARE_WITH_TIMEOUT(network->calls, 2, 5000);
        QCOMPARE(network->last.url().host(), QStringLiteral("huggingface.co"));
        QCOMPARE(network->last.rawHeader("Range"), QByteArray("bytes=3-"));
        QTRY_COMPARE_WITH_TIMEOUT(failed.count(), 1, 5000); // the fixture hash is wrong
        QVERIFY(failed.first().at(0).toString().contains(QStringLiteral("校验失败")));
        QVERIFY(!service.extractor_);
    }
    void existingPreciseInstallationStaysSelectable() {
        QTemporaryDir temp; QVERIFY(temp.isValid());
        qputenv("VISNIP_TEST_SETTINGS_FILE",QFile::encodeName(temp.filePath(QStringLiteral("settings.ini"))));
        const QDir root(temp.filePath(QStringLiteral("precise")));
        for (const QString& relative : {QStringLiteral("python/python.exe"), QStringLiteral("precise.json"),
             QStringLiteral("models/Hy-MT2-1.8B-Q4_K_M.gguf"), QStringLiteral("llama/llama-server.exe"), QStringLiteral("models/hi_sam_b.pth")}) {
            QVERIFY(QDir().mkpath(QFileInfo(root.filePath(relative)).absolutePath()));
            QFile file(root.filePath(relative)); QVERIFY(file.open(QIODevice::WriteOnly)); file.write("x");
        }
        AppConfig config; auto& settings=config.mutableSettings().aiTranslate;
        settings.translationMethod=TranslationMethod::Offline; settings.offlineQuality=QStringLiteral("precise");
        settings.offlineResourceDirectory=root.absolutePath();
        SettingsDialog dialog(&config); dialog.showPage(SettingsDialog::Page::Translation);
        auto* quality=dialog.findChild<QComboBox*>(QStringLiteral("VisnipSettingsOfflineQuality"));QVERIFY(quality);
        QCOMPARE(quality->count(),2);QCOMPARE(quality->currentData().toString(),QStringLiteral("precise"));
        qunsetenv("VISNIP_TEST_SETTINGS_FILE");
    }
    void offlineOffersOnlyTheLiteTierWithoutNetworkControlsAndProgressUsesBytes() {
        QTemporaryDir temp; QVERIFY(temp.isValid());
        qputenv("VISNIP_TEST_SETTINGS_FILE",QFile::encodeName(temp.filePath(QStringLiteral("settings.ini"))));
        AppConfig config; config.mutableSettings().aiTranslate.translationMethod=TranslationMethod::Offline;
        SettingsDialog dialog(&config); dialog.resize(840,620); dialog.showPage(SettingsDialog::Page::Translation);
        dialog.show();QTest::qWait(50);
        auto* quality=dialog.findChild<QComboBox*>(QStringLiteral("VisnipSettingsOfflineQuality"));QVERIFY(quality);
        QCOMPARE(quality->count(),1);QCOMPARE(quality->itemData(0).toString(),QStringLiteral("lite"));
        QCOMPARE(quality->currentIndex(),0);
        auto* remote=dialog.findChild<QWidget*>(QStringLiteral("VisnipSettingsRemotePanel"));QVERIFY(remote);QVERIFY(!remote->isVisible());
        auto* advanced=dialog.findChild<QToolButton*>(QStringLiteral("VisnipSettingsAdvancedMethods"));QVERIFY(advanced);QVERIFY(!advanced->isVisible());
        auto* bar=dialog.findChild<QProgressBar*>(QStringLiteral("VisnipSettingsOfflineProgress"));QVERIFY(bar);
        auto* manager=dialog.findChild<OfflineResourceService*>();QVERIFY(manager);
        emit manager->phaseChanged(QStringLiteral("download"));emit manager->progress(5242880,10485760);
        QVERIFY(bar->isVisible());QCOMPARE(bar->value(),500);QCOMPARE(bar->maximum(),1000);
        QVERIFY(bar->width()>300);
        // The byte counter is painted inside the bar: a slim indicator clips it.
        QVERIFY(bar->isTextVisible());
        QVERIFY(bar->text().contains(QStringLiteral("MiB")));
        QVERIFY2(bar->height()>=16,qPrintable(QStringLiteral("download progress text needs a taller bar, got %1 px").arg(bar->height())));
        const auto output=qEnvironmentVariable("VISNIP_SETTINGS_QA_IMAGE");if(!output.isEmpty()) QVERIFY(dialog.grab().save(output));
        emit manager->phaseChanged(QStringLiteral("verify"));emit manager->progress(0,0);
        QVERIFY(!bar->isVisible());QCOMPARE(bar->maximum(),1000);
        emit manager->phaseChanged(QStringLiteral("install"));QVERIFY(!bar->isVisible());
        emit manager->phaseChanged(QStringLiteral("selftest"));QVERIFY(!bar->isVisible());
        qunsetenv("VISNIP_TEST_SETTINGS_FILE");
    }
    void installedFilesAreListedWithTheirSourcesAndCanBeDeleted()
    {
        QTemporaryDir temp; QVERIFY(temp.isValid());
        QTemporaryDir settings; QVERIFY(settings.isValid());
        const QByteArray previous = qgetenv("LOCALAPPDATA");
        struct Restore { QByteArray value; ~Restore() { if (value.isNull()) qunsetenv("LOCALAPPDATA"); else qputenv("LOCALAPPDATA", value); } } restore{previous};
        qputenv("LOCALAPPDATA", QFile::encodeName(temp.path()));
        qputenv("VISNIP_TEST_SETTINGS_FILE", QFile::encodeName(settings.filePath(QStringLiteral("settings.ini"))));
        const QString root = temp.filePath(QStringLiteral("Vislate/offline/managed/lite-fixture"));
        QVERIFY(QDir().mkpath(QDir(root).filePath(QStringLiteral("llama"))));
        QVERIFY(QDir().mkpath(QDir(root).filePath(QStringLiteral("models"))));
        for (const QString& name : {QStringLiteral("llama/llama-server.exe"),
                                    QStringLiteral("models/Hy-MT2-1.8B-Q4_K_M.gguf"),
                                    QStringLiteral("vislate-managed.json")}) {
            QFile file(QDir(root).filePath(name)); QVERIFY(file.open(QIODevice::WriteOnly)); file.write("x"); file.close();
        }
        const QString cache = OfflineResourceService::cacheDirectory();
        QVERIFY(QDir().mkpath(cache));
        QFile cached(QDir(cache).filePath(QStringLiteral("download.zip"))); QVERIFY(cached.open(QIODevice::WriteOnly)); cached.close();

        AppConfig config;
        config.mutableSettings().aiTranslate.offlineResourceDirectory = root;
        config.mutableSettings().aiTranslate.offlineQuality = QStringLiteral("lite");
        const QString customStorage = temp.filePath(QStringLiteral("custom-storage"));
        config.mutableSettings().aiTranslate.offlineStorageDirectory = customStorage;
        SettingsDialog dialog(&config);
        dialog.resize(840,620);
        dialog.showPage(SettingsDialog::Page::Translation);
        dialog.show(); QTest::qWait(50);

        // The download location is selectable and can be reset to the default.
        auto* storagePath = dialog.findChild<QLineEdit*>(QStringLiteral("VisnipSettingsOfflineStoragePath")); QVERIFY(storagePath);
        QCOMPARE(storagePath->text(), QDir::toNativeSeparators(customStorage));
        auto* storageChoose = dialog.findChild<QPushButton*>(QStringLiteral("VisnipSettingsOfflineStorageChoose"));
        QVERIFY(storageChoose && storageChoose->isEnabled());
        auto* storageReset = dialog.findChild<QPushButton*>(QStringLiteral("VisnipSettingsOfflineStorageReset")); QVERIFY(storageReset);
        QVERIFY(storageReset->isEnabled());
        storageReset->click(); QTest::qWait(10);
        QCOMPARE(storagePath->text(), QDir::toNativeSeparators(OfflineResourceService::storageDirectory()));
        QVERIFY(config.settings().aiTranslate.offlineStorageDirectory.isEmpty());

        // The list names the installed files and their state. Neither the
        // download address nor the local path is printed: the folder is opened
        // through its own button instead.
        auto* files = dialog.findChild<QLabel*>(QStringLiteral("VisnipSettingsOfflineFiles")); QVERIFY(files);
        QVERIFY(files->text().contains(QStringLiteral("llama.cpp")));
        QVERIFY(files->text().contains(QStringLiteral("Hy-MT2-1.8B")));
        QVERIFY(files->text().contains(QStringLiteral("已下载")));
        QVERIFY(!files->text().contains(QStringLiteral("http")));
        QVERIFY(!files->text().contains(QDir::toNativeSeparators(root)));
        QVERIFY(!dialog.findChild<QLineEdit*>(QStringLiteral("VisnipSettingsOfflineDirectory")));
        auto* open = dialog.findChild<QPushButton*>(QStringLiteral("VisnipSettingsOfflineOpenFolder")); QVERIFY(open);
        QVERIFY(open->isVisibleTo(&dialog) && open->isEnabled());
        auto* remove = dialog.findChild<QPushButton*>(QStringLiteral("VisnipSettingsOfflineDelete")); QVERIFY(remove);
        QVERIFY(remove->isVisibleTo(&dialog));
        const auto qaImage = qEnvironmentVariable("VISNIP_SETTINGS_QA_DELETE_IMAGE");
        if (!qaImage.isEmpty()) QVERIFY(dialog.grab().save(qaImage));

        QTimer answer; answer.setInterval(25);
        connect(&answer, &QTimer::timeout, &dialog, []() {
            for (auto* widget : QApplication::topLevelWidgets()) {
                auto* message = qobject_cast<QMessageBox*>(widget);
                if (message && message->isVisible() && message->standardButtons().testFlag(QMessageBox::Yes)) {
                    if (auto* yes = message->button(QMessageBox::Yes)) yes->click();
                }
            }
        });
        answer.start();
        remove->click();
        QTest::qWait(50);

        QVERIFY(!QFileInfo::exists(QDir(root).filePath(QStringLiteral("llama"))));
        QVERIFY(!QFileInfo::exists(QDir(root).filePath(QStringLiteral("models/Hy-MT2-1.8B-Q4_K_M.gguf"))));
        QVERIFY(!QFileInfo::exists(QDir(root).filePath(QStringLiteral("vislate-managed.json"))));
        QVERIFY(!QFileInfo::exists(QDir(cache).filePath(QStringLiteral("download.zip"))));
        QVERIFY(config.settings().aiTranslate.offlineResourceDirectory.isEmpty());
        QVERIFY(!remove->isVisibleTo(&dialog));
        QVERIFY(!open->isVisibleTo(&dialog));

        // A file a shutting-down engine still holds open is retried, reported
        // with its name, and removed once the handle is gone.
#ifdef Q_OS_WIN
        const QString busyRoot = temp.filePath(QStringLiteral("busy-root"));
        QVERIFY(QDir().mkpath(QDir(busyRoot).filePath(QStringLiteral("llama"))));
        QVERIFY(QDir().mkpath(QDir(busyRoot).filePath(QStringLiteral("models"))));
        const QString busyModel = QDir(busyRoot).filePath(QStringLiteral("models/Hy-MT2-1.8B-Q4_K_M.gguf"));
        { QFile file(busyModel); QVERIFY(file.open(QIODevice::WriteOnly)); file.write("x"); }
        HANDLE lock = CreateFileW(reinterpret_cast<const wchar_t*>(QDir::toNativeSeparators(busyModel).utf16()),
                                  GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
        QVERIFY(lock != INVALID_HANDLE_VALUE);
        QString busyError;
        QVERIFY(!OfflineResourceService::removeInstalled(busyRoot, QString(), &busyError));
        QVERIFY2(busyError.contains(QStringLiteral("Hy-MT2-1.8B-Q4_K_M.gguf")), qPrintable(busyError));
        CloseHandle(lock);
        QString retryError;
        QVERIFY2(OfflineResourceService::removeInstalled(busyRoot, QString(), &retryError), qPrintable(retryError));
        QVERIFY(!QFileInfo::exists(busyModel));
#endif
        qunsetenv("VISNIP_TEST_SETTINGS_FILE");
    }
    void silentSourceFailsOverInsteadOfHanging() {
        QTemporaryDir temp; QVERIFY(temp.isValid());
        OfflineResourceService service;
        auto* network = new SilentNetwork;
        network->setParent(&service);
        delete service.network_;
        service.network_ = network;
        service.busy_ = true;
        service.cacheRoot_ = temp.path();
        service.plan_ = OfflineResourceCatalog::liteFiles();
        service.index_ = 0; service.source_ = 0;
        service.transferCheck_.setInterval(150); // keep the test quick
        QSignalSpy failed(&service, &OfflineResourceService::failed);
        QSignalSpy status(&service, &OfflineResourceService::statusChanged);
        service.requestFile();
        QTRY_VERIFY_WITH_TIMEOUT(!failed.isEmpty(), 15000);
        bool reportedStall = false;
        for (const auto& arguments : status)
            if (arguments.at(0).toString().contains(QStringLiteral("下载停顿"))) reportedStall = true;
        QVERIFY2(reportedStall, "the stall watchdog never reported the silent source");
        const QString error = failed.first().at(0).toString();
        QVERIFY2(!error.contains(QStringLiteral("磁盘")), qPrintable(error));
        QVERIFY2(error.contains(QStringLiteral("无法完成下载")), qPrintable(error));
        QCOMPARE(network->calls, 1);
        QVERIFY(QFileInfo(service.filePath(true)).exists()); // the partial file stays for the next attempt
    }
    void rateLimitPageNeverBecomesPackageBytes() {
        QTemporaryDir temp;QVERIFY(temp.isValid());OfflineResourceService service;
        auto* network=new FixtureNetwork;network->setParent(&service);delete service.network_;service.network_=network;
        service.cacheRoot_=temp.path();service.busy_=true;service.retries_=3;
        service.plan_={fixtureFile({kGitHub})};service.totalBytes_=6;
        QFile file(service.filePath(true));QVERIFY(file.open(QIODevice::WriteOnly));file.write("abc");file.close();
        network->status=429;network->data="slow down";
        QSignalSpy failed(&service,&OfflineResourceService::failed);service.requestFile();QTRY_COMPARE(failed.size(),1);
        QVERIFY(failed.first().at(0).toString().contains(QStringLiteral("已保留下载进度")));
        QVERIFY(file.open(QIODevice::ReadOnly));QCOMPARE(file.readAll(),QByteArray("abc"));QVERIFY(!service.extractor_);
    }
    void interruptedDownloadKeepsPartialAndResumeRange() {
        QTemporaryDir temp; QVERIFY(temp.isValid());
        OfflineResourceService service; auto* network = new FixtureNetwork; network->setParent(&service);
        delete service.network_; service.network_ = network;
        service.cacheRoot_ = temp.path(); service.busy_ = true;
        service.plan_ = {fixtureFile({kGitHub})}; service.totalBytes_ = 6;
        service.retries_ = 3; // retries spent: the interruption ends the attempt
        QFile partial(service.filePath(true)); QVERIFY(partial.open(QIODevice::WriteOnly)); partial.write("abc"); partial.close();
        network->status = 206; network->contentRange = "bytes 3-5/6"; network->data = "def";
        // The response body is complete but the simulated network disconnect must prevent execution.
        network->failure = QNetworkReply::RemoteHostClosedError;
        QSignalSpy failed(&service, &OfflineResourceService::failed);
        service.requestFile(); QTRY_COMPARE(failed.count(), 1);
        QCOMPARE(network->last.rawHeader("Range"), QByteArray("bytes=3-"));
        QVERIFY(partial.open(QIODevice::ReadOnly)); QCOMPARE(partial.readAll(), QByteArray("abcdef"));
        QVERIFY(!service.extractor_); QVERIFY(!service.isBusy());
    }
};
}
QTEST_MAIN(Visnip::OfflineResourceTests)
#include "tst_offline_resources.moc"
