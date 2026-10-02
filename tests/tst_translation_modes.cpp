#include "core/AppConfig.h"
#include "core/PerfLog.h"
#include "services/ImageTranslationService.h"
#include "services/OfflineTranslationService.h"
#include "services/TextTranslationService.h"
#include "ui/settings/SettingsDialog.h"
#include "ui/widgets/TranslationReviewPanel.h"
#include <QComboBox>
#include <QApplication>
#include <QDir>

#include <QBuffer>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPushButton>
#include <QPainter>
#include <QSettings>
#include <QStandardPaths>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTemporaryDir>
#include <QtTest>

#include <memory>
#include <algorithm>

using namespace Visnip;

namespace {
// Deliberately loopback-only. No live provider or real screenshot is used.
class HttpFixture : public QObject {
public:
    QTcpServer server;
    QByteArray response;
    QByteArray request;
    bool completed = false;
    int connections = 0;
    bool holdResponse = false;

    HttpFixture()
    {
        connect(&server, &QTcpServer::newConnection, this, [this]() {
            ++connections;
            auto* socket = server.nextPendingConnection();
            auto bytes = std::make_shared<QByteArray>();
            auto done = std::make_shared<bool>(false);
            connect(socket, &QTcpSocket::readyRead, this, [this, socket, bytes, done]() {
                bytes->append(socket->readAll());
                if (*done) return;
                const auto headerEnd = bytes->indexOf("\r\n\r\n");
                if (headerEnd < 0) return;
                int contentLength = 0;
                for (const auto& header : bytes->left(headerEnd).split('\n')) {
                    if (header.toLower().startsWith("content-length:")) {
                        contentLength = header.mid(15).trimmed().toInt();
                    }
                }
                if (bytes->size() < headerEnd + 4 + contentLength) return;
                *done = true;
                request = *bytes;
                completed = true;
                if (!holdResponse) {
                    socket->write(response);
                    socket->disconnectFromHost();
                }
            });
            connect(socket, &QTcpSocket::disconnected, socket, &QObject::deleteLater);
        });
    }
    bool listen() { return server.listen(QHostAddress::LocalHost, 0); }
    QString root() const {
        return QStringLiteral("http://127.0.0.1:%1").arg(server.serverPort());
    }
    void setJson(const QByteArray& body) {
        response = "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nConnection: close\r\nContent-Length: "
            + QByteArray::number(body.size()) + "\r\n\r\n" + body;
    }
};

// Transport tests run as a user who already accepted the upload notice; the
// notice itself is covered by onlineModesNeedTheUploadNotice.
void acceptUploads(AppConfig& config)
{
    config.mutableSettings().aiTranslate.cloudUploadConsent = AiTranslateSettings::kUploadNoticeVersion;
    config.mutableSettings().aiTranslate.intranetUploadConsent = AiTranslateSettings::kUploadNoticeVersion;
}

QImage sampleImage()
{
    QImage image(32, 20, QImage::Format_RGB32);
    image.fill(Qt::white);
    return image;
}

QByteArray imageResponse()
{
    QByteArray png;
    QBuffer buffer(&png);
    buffer.open(QIODevice::WriteOnly);
    sampleImage().save(&buffer, "PNG");
    return QJsonDocument(QJsonObject{
        {QStringLiteral("provider"), QStringLiteral("visnip-selfhost-v1")},
        {QStringLiteral("source_lang"), QStringLiteral("en")},
        {QStringLiteral("target_lang"), QStringLiteral("zh-Hans")},
        {QStringLiteral("blocks"), QJsonArray{}},
        {QStringLiteral("elapsed_ms"), 1},
        {QStringLiteral("translated_image_mime_type"), QStringLiteral("image/png")},
        {QStringLiteral("translated_image_base64"), QString::fromLatin1(png.toBase64())}
    }).toJson(QJsonDocument::Compact);
}
} // namespace

class TranslationModeTests : public QObject {
    Q_OBJECT
private:
    QTemporaryDir settingsDirectory_;
private slots:
    void initTestCase()
    {
        QStandardPaths::setTestModeEnabled(true);
        QVERIFY(settingsDirectory_.isValid());
        qputenv("VISNIP_TEST_SETTINGS_FILE",
                QFile::encodeName(settingsDirectory_.filePath(QStringLiteral("settings.ini"))));
    }
    void cleanupTestCase() { qunsetenv("VISNIP_TEST_SETTINGS_FILE"); }
    void init()
    {
        QSettings settings(settingsDirectory_.filePath(QStringLiteral("settings.ini")), QSettings::IniFormat);
        settings.clear();
        settings.sync();
    }

    void endpointsAndCacheKeysStayInTheirSelectedMode()
    {
        AiTranslateSettings settings;
        settings.fastServiceUrl = QStringLiteral("https://cloud.example/visnip");
        const QString cloudKey = settings.translationMethodCacheKey();
        settings.translationMethod = TranslationMethod::Intranet;
        QVERIFY(settings.fastServiceBaseUrl().isEmpty());
        QVERIFY(settings.fastImageTranslateEndpoint().isEmpty());
        QVERIFY(settings.fastHealthEndpoint().isEmpty());
        for (const QString& invalid : {QStringLiteral("/"), QStringLiteral("/healthz"),
                                       QStringLiteral("/v1/image-translate/")}) {
            settings.intranetServiceUrl = invalid;
            QVERIFY(settings.fastImageTranslateEndpoint().isEmpty());
            QVERIFY(settings.fastHealthEndpoint().isEmpty());
        }
        settings.intranetServiceUrl = QStringLiteral("https://translate.corp/service/v1/image-translate/");
        QCOMPARE(settings.fastServiceBaseUrl(), QStringLiteral("https://translate.corp/service"));
        QVERIFY(settings.translationMethodCacheKey() != cloudKey);
        settings.translationMethod = TranslationMethod::Offline;
        QVERIFY(!settings.allowsTranslationNetwork());
        QVERIFY(settings.fastServiceBaseUrl().isEmpty());
        QVERIFY(settings.fastTranslateEndpoint().isEmpty());
        QVERIFY(settings.fastImageTranslateEndpoint().isEmpty());
        QVERIFY(settings.fastHealthEndpoint().isEmpty());
        const QString offlineKey = settings.translationMethodCacheKey();
        settings.fastServiceUrl.clear();
        settings.intranetServiceUrl.clear();
        QCOMPARE(settings.translationMethodCacheKey(), offlineKey);
        settings.translationMethod = static_cast<TranslationMethod>(999);
        QVERIFY(!settings.allowsTranslationNetwork());
        QVERIFY(settings.fastImageTranslateEndpoint().isEmpty());
    }

    void persistedModesMigrateWithoutEnablingUnexpectedUploads()
    {
        const QString path = settingsDirectory_.filePath(QStringLiteral("settings.ini"));
        const auto setMode = [&path](const QString& mode) {
            QSettings settings(path, QSettings::IniFormat);
            settings.setValue(QStringLiteral("aiTranslate/translationMethod"), mode);
            settings.sync();
        };
        setMode(QStringLiteral("cloud-baidu"));
        AppConfig config;
        acceptUploads(config);
        config.load();
        if (!onlineTranslationEnabled()) {
            // An offline-only build turns every stored online mode into offline.
            QCOMPARE(config.settings().aiTranslate.translationMethod, TranslationMethod::Offline);
            setMode(QStringLiteral("local-ocr"));
            config.load();
            QVERIFY(config.settings().aiTranslate.isOffline());
            setMode(QStringLiteral("intranet"));
            config.load();
            QVERIFY(config.settings().aiTranslate.isOffline());
            return;
        }
        QCOMPARE(config.settings().aiTranslate.translationMethod, TranslationMethod::CloudImage);
        QCOMPARE(translationMethodId(config.settings().aiTranslate.translationMethod), QStringLiteral("cloud-image"));
        setMode(QStringLiteral("local-ocr"));
        config.load();
        QVERIFY(config.settings().aiTranslate.usesLegacyOnlineTextTranslation());
        for (const auto method : {TranslationMethod::Intranet, TranslationMethod::Offline}) {
            config.mutableSettings().aiTranslate.translationMethod = method;
            config.mutableSettings().aiTranslate.intranetServiceUrl = QStringLiteral("https://translate.corp/base");
            QVERIFY(config.save());
            AppConfig restored;
            restored.load();
            QCOMPARE(restored.settings().aiTranslate.translationMethod, method);
            QCOMPARE(restored.settings().aiTranslate.intranetServiceUrl, QStringLiteral("https://translate.corp/base"));
        }
        setMode(QStringLiteral("unknown-future-mode"));
        config.load();
        QVERIFY(config.settings().aiTranslate.isOffline());
    }

    void settingsKeepSeparateAddressesAndShowOfflineResourceReadiness()
    {
        if (!onlineTranslationEnabled()) {
            QSKIP("Online translation is not built in (VISNIP_ONLINE_TRANSLATION=OFF).");
        }
        AppConfig config;
        acceptUploads(config);
        config.mutableSettings().aiTranslate.fastServiceUrl = QStringLiteral("https://cloud.example/base");
        config.mutableSettings().aiTranslate.offlineResourceDirectory = settingsDirectory_.filePath(QStringLiteral("missing-offline"));
        SettingsDialog dialog(&config);
        dialog.showPage(SettingsDialog::Page::Translation);
        auto* intranet = dialog.findChild<QPushButton*>(QStringLiteral("VisnipSettingsTranslationIntranet"));
        auto* offline = dialog.findChild<QPushButton*>(QStringLiteral("VisnipSettingsTranslationOffline"));
        auto* cloud = dialog.findChild<QPushButton*>(QStringLiteral("VisnipSettingsTranslationCloud"));
        auto* legacy = dialog.findChild<QPushButton*>(QStringLiteral("VisnipSettingsTranslationLocal"));
        auto* url = dialog.findChild<QLineEdit*>(QStringLiteral("VisnipSettingsServiceUrl"));
        auto* test = dialog.findChild<QPushButton*>(QStringLiteral("VisnipSettingsTestService"));
        auto* status = dialog.findChild<QLabel*>(QStringLiteral("VisnipSettingsOfflineStatus"));
        QVERIFY(intranet && offline && cloud && legacy && url && test && status);
        QVERIFY(legacy->text().contains(QStringLiteral("联网")));
        intranet->click();
        QVERIFY(url->text().isEmpty());
        url->setText(QStringLiteral("https://translate.corp/base"));
        QCOMPARE(config.settings().aiTranslate.intranetServiceUrl, url->text());
        QCOMPARE(config.settings().aiTranslate.fastServiceUrl, QStringLiteral("https://cloud.example/base"));
        offline->click();
        QVERIFY(config.settings().aiTranslate.isOffline());
        QVERIFY(!url->isEnabled());
        QVERIFY(!test->isEnabled());
        QVERIFY(status->text().contains(QStringLiteral("尚未就绪")));
        QVERIFY(status->text().contains(QStringLiteral("下载并启用")));
        // Resources are app-managed: no import field, but the file list and its
        // delete action exist for every installed download.
        QVERIFY(!dialog.findChild<QLineEdit*>(QStringLiteral("VisnipSettingsOfflineDirectory")));
        QVERIFY(dialog.findChild<QLabel*>(QStringLiteral("VisnipSettingsOfflineFiles")));
        QVERIFY(dialog.findChild<QPushButton*>(QStringLiteral("VisnipSettingsOfflineDelete")));
        QVERIFY(dialog.findChild<QPushButton*>(QStringLiteral("VisnipSettingsOfflineSelfTest")));
        cloud->click();
        QCOMPARE(url->text(), QStringLiteral("https://cloud.example/base"));
        intranet->click();
        QCOMPARE(url->text(), QStringLiteral("https://translate.corp/base"));
    }

    void offlineRejectsImageTextProbeAndPreconnect()
    {
        HttpFixture fixture;
        QVERIFY(fixture.listen());
        AppConfig config;
        acceptUploads(config);
        auto& settings = config.mutableSettings().aiTranslate;
        settings.fastServiceUrl = fixture.root();
        settings.intranetServiceUrl = fixture.root();
        settings.translationMethod = TranslationMethod::Offline;
        ImageTranslationService image(&config);
        settings.offlineResourceDirectory = settingsDirectory_.filePath(QStringLiteral("missing-offline"));
        TextTranslationService text(&config);
        QSignalSpy imageFailed(&image, &ImageTranslationService::failed);
        QSignalSpy textFailed(&text, &TextTranslationService::failed);
        QSignalSpy health(&text, &TextTranslationService::healthChecked);
        image.preconnectConfiguredEndpoint();
        image.translate(sampleImage(), QStringLiteral("zh-Hans"));
        text.translate({QStringLiteral("Hello")}, QStringLiteral("zh-Hans"));
        text.checkHealth(fixture.root());
        QCOMPARE(imageFailed.size(), 1);
        QCOMPARE(textFailed.size(), 1);
        QCOMPARE(health.size(), 1);
        QVERIFY(!health.first().at(0).toBool());
        QTest::qWait(100);
        QVERIFY(!fixture.completed);
        QVERIFY(!image.isBusy());
        QCOMPARE(fixture.connections, 0);
        QVERIFY(!text.isBusy());
    }

    void emptyIntranetEndpointNeverUsesTheCloudOrProbeArgument()
    {
        HttpFixture fixture;
        QVERIFY(fixture.listen());
        AppConfig config;
        acceptUploads(config);
        config.mutableSettings().aiTranslate.translationMethod = TranslationMethod::Intranet;
        config.mutableSettings().aiTranslate.fastServiceUrl = fixture.root();
        ImageTranslationService image(&config);
        TextTranslationService probe(&config);
        QSignalSpy failed(&image, &ImageTranslationService::failed);
        QSignalSpy health(&probe, &TextTranslationService::healthChecked);
        image.translate(sampleImage(), QStringLiteral("zh-Hans"));
        probe.checkHealth(fixture.root());
        QCOMPARE(failed.size(), 1);
        QCOMPARE(health.size(), 1);
        QVERIFY(!health.first().at(0).toBool());
        QTest::qWait(100);
        QVERIFY(!fixture.completed);
        QCOMPARE(fixture.connections, 0);
    }

    void intranetAcceptsACompleteSelfHostedImageResponse()
    {
        if (!onlineTranslationEnabled()) {
            QSKIP("Online translation is not built in (VISNIP_ONLINE_TRANSLATION=OFF).");
        }
        HttpFixture fixture;
        QVERIFY(fixture.listen());
        fixture.setJson(imageResponse());
        AppConfig config;
        acceptUploads(config);
        config.mutableSettings().aiTranslate.translationMethod = TranslationMethod::Intranet;
        config.mutableSettings().aiTranslate.intranetServiceUrl = fixture.root();
        ImageTranslationService service(&config);
        QSignalSpy succeeded(&service, &ImageTranslationService::succeeded);
        QSignalSpy failed(&service, &ImageTranslationService::failed);
        service.translate(sampleImage(), QStringLiteral("zh-Hans"));
        QTRY_VERIFY_WITH_TIMEOUT(succeeded.size() + failed.size() == 1, 3000);
        QCOMPARE(failed.size(), 0);
        QVERIFY(fixture.request.startsWith("POST /v1/image-translate "));
        const auto result = qvariant_cast<ImageTranslationResult>(succeeded.first().at(0));
        QCOMPARE(result.provider, QStringLiteral("visnip-selfhost-v1"));
        QCOMPARE(result.image.size(), sampleImage().size());
    }

    void serviceTokensTravelOnlyToTheirOwnReceiver()
    {
        if (!onlineTranslationEnabled()) {
            QSKIP("Online translation is not built in (VISNIP_ONLINE_TRANSLATION=OFF).");
        }
        HttpFixture fixture;
        QVERIFY(fixture.listen());
        fixture.setJson(imageResponse());
        AppConfig config;
        acceptUploads(config);
        auto& settings = config.mutableSettings().aiTranslate;
        settings.translationMethod = TranslationMethod::CloudImage;
        settings.fastServiceUrl = fixture.root(); // loopback HTTP is allowed for the cloud token
        settings.cloudServiceToken = QStringLiteral("cloud-secret");
        settings.intranetServiceToken = QStringLiteral("corp-secret");
        {
            ImageTranslationService service(&config);
            QSignalSpy succeeded(&service, &ImageTranslationService::succeeded);
            service.translate(sampleImage(), QStringLiteral("zh-Hans"));
            QTRY_COMPARE_WITH_TIMEOUT(succeeded.size(), 1, 3000);
            QVERIFY(fixture.request.contains("\r\nAuthorization: Bearer cloud-secret\r\n"));
            QVERIFY(!fixture.request.contains("corp-secret"));
        }
        HttpFixture intranet;
        QVERIFY(intranet.listen());
        intranet.setJson(imageResponse());
        settings.translationMethod = TranslationMethod::Intranet;
        settings.intranetServiceUrl = intranet.root();
        {
            ImageTranslationService service(&config);
            QSignalSpy succeeded(&service, &ImageTranslationService::succeeded);
            service.translate(sampleImage(), QStringLiteral("zh-Hans"));
            QTRY_COMPARE_WITH_TIMEOUT(succeeded.size(), 1, 3000);
            QVERIFY(intranet.request.contains("\r\nAuthorization: Bearer corp-secret\r\n"));
            QVERIFY(!intranet.request.contains("cloud-secret"));
        }
        // Without a token no Authorization header is sent at all.
        HttpFixture anonymous;
        QVERIFY(anonymous.listen());
        anonymous.setJson(imageResponse());
        settings.intranetServiceUrl = anonymous.root();
        settings.intranetServiceToken.clear();
        {
            ImageTranslationService service(&config);
            QSignalSpy succeeded(&service, &ImageTranslationService::succeeded);
            service.translate(sampleImage(), QStringLiteral("zh-Hans"));
            QTRY_COMPARE_WITH_TIMEOUT(succeeded.size(), 1, 3000);
            QVERIFY(!anonymous.request.toLower().contains("authorization:"));
        }
        // A cloud token is never sent over plain HTTP to a remote host.
        settings.translationMethod = TranslationMethod::CloudImage;
        settings.fastServiceUrl = QStringLiteral("http://cloud.invalid");
        ImageTranslationService refused(&config);
        QSignalSpy failed(&refused, &ImageTranslationService::failed);
        refused.translate(sampleImage(), QStringLiteral("zh-Hans"));
        QCOMPARE(failed.size(), 1);
        QVERIFY(failed.first().first().toString().contains(QStringLiteral("HTTPS")));
        // Tokens are stored per receiver and encrypted for the Windows user.
        settings.fastServiceUrl.clear();
        QVERIFY(config.save());
        AppConfig restored;
        restored.load();
        QCOMPARE(restored.settings().aiTranslate.cloudServiceToken, QStringLiteral("cloud-secret"));
        QVERIFY(restored.settings().aiTranslate.intranetServiceToken.isEmpty());
#ifdef Q_OS_WIN
        QSettings raw(settingsDirectory_.filePath(QStringLiteral("settings.ini")), QSettings::IniFormat);
        QVERIFY(!raw.value(QStringLiteral("aiTranslate/cloudServiceToken")).toString().contains(QStringLiteral("cloud-secret")));
#endif
    }

    void unauthorizedResponseAsksForTheToken()
    {
        if (!onlineTranslationEnabled()) {
            QSKIP("Online translation is not built in (VISNIP_ONLINE_TRANSLATION=OFF).");
        }
        HttpFixture fixture;
        QVERIFY(fixture.listen());
        const QByteArray body = R"({"detail":"invalid bearer token"})";
        fixture.response = "HTTP/1.1 401 Unauthorized\r\nContent-Type: application/json\r\nConnection: close\r\nContent-Length: "
            + QByteArray::number(body.size()) + "\r\n\r\n" + body;
        AppConfig config;
        acceptUploads(config);
        config.mutableSettings().aiTranslate.translationMethod = TranslationMethod::CloudImage;
        config.mutableSettings().aiTranslate.fastServiceUrl = fixture.root();
        ImageTranslationService service(&config);
        QSignalSpy failed(&service, &ImageTranslationService::failed);
        service.translate(sampleImage(), QStringLiteral("zh-Hans"));
        QTRY_COMPARE_WITH_TIMEOUT(failed.size(), 1, 3000);
        QVERIFY(failed.first().first().toString().contains(QStringLiteral("API 令牌")));
    }

    void offlineOnlyBuildNeverUploads()
    {
        if (onlineTranslationEnabled()) {
            QSKIP("This build includes online translation.");
        }
        QVERIFY(aiTranslateDefaultFastServiceUrl().isEmpty()); // no service address compiled in
        HttpFixture fixture;
        QVERIFY(fixture.listen());
        fixture.setJson(imageResponse());
        {
            // Settings saved by a build with online modes.
            QSettings stored(settingsDirectory_.filePath(QStringLiteral("settings.ini")), QSettings::IniFormat);
            stored.setValue(QStringLiteral("aiTranslate/translationMethod"), QStringLiteral("cloud-image"));
            stored.setValue(QStringLiteral("aiTranslate/fastServiceUrl"), fixture.root());
            stored.setValue(QStringLiteral("aiTranslate/cloudUploadConsent"), AiTranslateSettings::kUploadNoticeVersion);
            stored.setValue(QStringLiteral("aiTranslate/targetLanguage"), QStringLiteral("ja"));
            stored.sync();
        }
        AppConfig config;
        config.load();
        QVERIFY(config.settings().aiTranslate.isOffline());
        QCOMPARE(config.settings().aiTranslate.targetLanguage, QStringLiteral("zh-Hans"));
        // Even a programmatically selected online mode sends nothing.
        config.mutableSettings().aiTranslate.translationMethod = TranslationMethod::CloudImage;
        ImageTranslationService image(&config);
        QSignalSpy imageFailed(&image, &ImageTranslationService::failed);
        image.preconnectConfiguredEndpoint();
        image.translate(sampleImage(), QStringLiteral("zh-Hans"));
        QCOMPARE(imageFailed.size(), 1);
        QVERIFY(imageFailed.first().first().toString().contains(QStringLiteral("不包含联网翻译")));
        config.mutableSettings().aiTranslate.translationMethod = TranslationMethod::LocalOcr;
        TextTranslationService text(&config);
        QSignalSpy textFailed(&text, &TextTranslationService::failed);
        QSignalSpy health(&text, &TextTranslationService::healthChecked);
        text.translate({QStringLiteral("Hello")}, QStringLiteral("zh-Hans"));
        text.checkHealth(fixture.root());
        QCOMPARE(textFailed.size(), 1);
        QCOMPARE(health.size(), 1);
        QVERIFY(!health.first().first().toBool());
        QTest::qWait(100);
        QCOMPARE(fixture.connections, 0);
        // Preferences show only offline translation, for Chinese and English.
        config.mutableSettings().aiTranslate.translationMethod = TranslationMethod::Offline;
        SettingsDialog dialog(&config);
        dialog.showPage(SettingsDialog::Page::Translation);
        dialog.show();
        QApplication::processEvents();
        auto* cloud = dialog.findChild<QPushButton*>(QStringLiteral("VisnipSettingsTranslationCloud"));
        auto* remote = dialog.findChild<QWidget*>(QStringLiteral("VisnipSettingsRemotePanel"));
        auto* languages = dialog.findChild<QComboBox*>(QStringLiteral("VisnipSettingsTargetLanguage"));
        QVERIFY(cloud && remote && languages);
        QVERIFY(!cloud->isVisible());
        QVERIFY(!remote->isVisible());
        QCOMPARE(languages->count(), 2);
        QVERIFY(languages->findData(QStringLiteral("zh-Hans")) >= 0 && languages->findData(QStringLiteral("en")) >= 0);
    }

    void freshInstallDefaultsToOfflineAndUploadsNothing()
    {
        AppConfig config;
        config.load(); // empty settings file: a new installation
        const auto& settings = config.settings().aiTranslate;
        QCOMPARE(settings.translationMethod, TranslationMethod::Offline);
        QVERIFY(settings.usesLiteOfflineEngine());
        QVERIFY(!settings.allowsTranslationNetwork());
        QVERIFY(settings.fastImageTranslateEndpoint().isEmpty());
        QCOMPARE(settings.cloudUploadConsent, 0);
    }

    void onlineModesNeedTheUploadNotice()
    {
        if (!onlineTranslationEnabled()) {
            QSKIP("Online translation is not built in (VISNIP_ONLINE_TRANSLATION=OFF).");
        }
        HttpFixture fixture;
        QVERIFY(fixture.listen());
        fixture.setJson(imageResponse());
        AppConfig config;
        config.mutableSettings().aiTranslate.translationMethod = TranslationMethod::CloudImage;
        config.mutableSettings().aiTranslate.fastServiceUrl = fixture.root();
        {
            ImageTranslationService image(&config);
            QSignalSpy failed(&image, &ImageTranslationService::failed);
            image.preconnectConfiguredEndpoint();
            image.translate(sampleImage(), QStringLiteral("zh-Hans"));
            QCOMPARE(failed.size(), 1);
            QVERIFY(failed.first().first().toString().contains(QStringLiteral("上传说明")));
        }
        config.mutableSettings().aiTranslate.translationMethod = TranslationMethod::LocalOcr;
        {
            TextTranslationService text(&config);
            QSignalSpy failed(&text, &TextTranslationService::failed);
            text.translate({QStringLiteral("Hello")}, QStringLiteral("zh-Hans"));
            QCOMPARE(failed.size(), 1);
        }
        QTest::qWait(100);
        QCOMPARE(fixture.connections, 0); // not even a TLS preconnect
        // Accepting the cloud notice does not cover the intranet receiver.
        config.mutableSettings().aiTranslate.cloudUploadConsent = AiTranslateSettings::kUploadNoticeVersion;
        QVERIFY(config.settings().aiTranslate.uploadConsented());
        config.mutableSettings().aiTranslate.translationMethod = TranslationMethod::Intranet;
        QVERIFY(!config.settings().aiTranslate.uploadConsented());
        QVERIFY(config.save());
        AppConfig restored;
        restored.load();
        QCOMPARE(restored.settings().aiTranslate.cloudUploadConsent, AiTranslateSettings::kUploadNoticeVersion);
        QCOMPARE(restored.settings().aiTranslate.intranetUploadConsent, 0);
    }

    void settingsAskBeforeSwitchingToAnUploadingMode()
    {
        if (!onlineTranslationEnabled()) {
            QSKIP("Online translation is not built in (VISNIP_ONLINE_TRANSLATION=OFF).");
        }
        AppConfig config;
        config.load();
        SettingsDialog dialog(&config);
        dialog.showPage(SettingsDialog::Page::Translation);
        auto* cloud = dialog.findChild<QPushButton*>(QStringLiteral("VisnipSettingsTranslationCloud"));
        auto* offline = dialog.findChild<QPushButton*>(QStringLiteral("VisnipSettingsTranslationOffline"));
        QVERIFY(cloud && offline);
        QVERIFY(offline->isChecked());
        const auto answer = [&dialog](QMessageBox::ButtonRole role) {
            auto* notice = dialog.findChild<QMessageBox*>(QStringLiteral("VisnipUploadNotice"));
            QVERIFY(notice);
            QVERIFY(notice->text().contains(QStringLiteral("上传")));
            for (auto* button : notice->buttons()) {
                if (notice->buttonRole(button) == role) {
                    button->click();
                    return;
                }
            }
            QFAIL("notice button missing");
        };
        cloud->click();
        QCOMPARE(config.settings().aiTranslate.translationMethod, TranslationMethod::Offline);
        QVERIFY(offline->isChecked());
        answer(QMessageBox::RejectRole);
        QTRY_VERIFY(!dialog.findChild<QMessageBox*>(QStringLiteral("VisnipUploadNotice")));
        QCOMPARE(config.settings().aiTranslate.translationMethod, TranslationMethod::Offline);
        QCOMPARE(config.settings().aiTranslate.cloudUploadConsent, 0);
        cloud->click();
        answer(QMessageBox::AcceptRole);
        QTRY_COMPARE(config.settings().aiTranslate.translationMethod, TranslationMethod::CloudImage);
        QVERIFY(cloud->isChecked());
        QCOMPARE(config.settings().aiTranslate.cloudUploadConsent, AiTranslateSettings::kUploadNoticeVersion);
        QTRY_VERIFY(!dialog.findChild<QMessageBox*>(QStringLiteral("VisnipUploadNotice")));
        offline->click();
        cloud->click(); // already accepted: no second notice
        QCOMPARE(config.settings().aiTranslate.translationMethod, TranslationMethod::CloudImage);
        QVERIFY(!dialog.findChild<QMessageBox*>(QStringLiteral("VisnipUploadNotice")));
    }

    void failedBaiduRequestIsNeverResentElsewhere()
    {
        if (!onlineTranslationEnabled()) {
            QSKIP("Online translation is not built in (VISNIP_ONLINE_TRANSLATION=OFF).");
        }
        HttpFixture official;
        QVERIFY(official.listen());
        AppConfig config;
        acceptUploads(config);
        auto& settings = config.mutableSettings().aiTranslate;
        settings.translationMethod = TranslationMethod::LocalOcr;
        settings.fastServiceUrl = official.root();
        settings.fastProvider = QStringLiteral("baidu"); // no APP ID or key configured
        TextTranslationService text(&config);
        QSignalSpy failed(&text, &TextTranslationService::failed);
        text.translate({QStringLiteral("Hello")}, QStringLiteral("zh-Hans"));
        QTRY_COMPARE_WITH_TIMEOUT(failed.size(), 1, 3000);
        QVERIFY(failed.first().first().toString().contains(QStringLiteral("没有改用其他翻译服务")));
        QTest::qWait(100);
        QCOMPARE(official.connections, 0);
        // The Baidu key is stored like other secrets.
        settings.baiduSecretKey = QStringLiteral("baidu-secret-value");
        QVERIFY(config.save());
        AppConfig restored;
        restored.load();
        QCOMPARE(restored.settings().aiTranslate.baiduSecretKey, QStringLiteral("baidu-secret-value"));
#ifdef Q_OS_WIN
        QSettings raw(settingsDirectory_.filePath(QStringLiteral("settings.ini")), QSettings::IniFormat);
        QVERIFY(!raw.value(QStringLiteral("aiTranslate/baiduSecretKey")).toString().contains(QStringLiteral("baidu-secret-value")));
#endif
    }

    void remoteRedirectIsNotFollowed()
    {
        if (!onlineTranslationEnabled()) {
            QSKIP("Online translation is not built in (VISNIP_ONLINE_TRANSLATION=OFF).");
        }
        HttpFixture fixture;
        HttpFixture otherReceiver;
        QVERIFY(fixture.listen());
        QVERIFY(otherReceiver.listen());
        fixture.response = "HTTP/1.1 307 Temporary Redirect\r\nLocation: "
            + otherReceiver.root().toUtf8() + "/v1/image-translate\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
        AppConfig config;
        acceptUploads(config);
        config.mutableSettings().aiTranslate.translationMethod = TranslationMethod::Intranet;
        config.mutableSettings().aiTranslate.intranetServiceUrl = fixture.root();
        ImageTranslationService service(&config);
        QSignalSpy failed(&service, &ImageTranslationService::failed);
        service.translate(sampleImage(), QStringLiteral("zh-Hans"));
        QTRY_COMPARE_WITH_TIMEOUT(failed.size(), 1, 3000);
        QVERIFY(!otherReceiver.completed);
        QCOMPARE(otherReceiver.connections, 0);
    }

    void offlineResourceSettingsRoundTripAndDoNotEnableNetwork()
    {
        AppConfig config;
        acceptUploads(config);
        auto& settings = config.mutableSettings().aiTranslate;
        settings.translationMethod = TranslationMethod::Offline;
        settings.offlineResourceDirectory = QStringLiteral("C:/Visnip-test/offline");
        settings.offlineQuality = QStringLiteral("precise");
        const QString key = settings.translationMethodCacheKey();
        QVERIFY(settings.usesWholeImageTranslation());
        QVERIFY(!settings.usesCloudImageTranslation());
        QVERIFY(!settings.allowsTranslationNetwork());
        QVERIFY(config.save());
        AppConfig restored; restored.load();
        QCOMPARE(restored.settings().aiTranslate.offlineResourceDirectory, settings.offlineResourceDirectory);
        QCOMPARE(restored.settings().aiTranslate.offlineQuality, QStringLiteral("precise"));
        QCOMPARE(restored.settings().aiTranslate.translationMethodCacheKey(), key);
        restored.mutableSettings().aiTranslate.offlineQuality = QStringLiteral("lite");
        QVERIFY(restored.settings().aiTranslate.translationMethodCacheKey() != key);
        // The lite tier refills in the client: no whole-image route, no network.
        QVERIFY(restored.settings().aiTranslate.usesLiteOfflineEngine());
        QVERIFY(!restored.settings().aiTranslate.usesWholeImageTranslation());
        QVERIFY(!restored.settings().aiTranslate.allowsTranslationNetwork());
        QVERIFY(restored.settings().aiTranslate.fastImageTranslateEndpoint().isEmpty());
        QVERIFY(!OfflineTranslationService::resourceProblem(settingsDirectory_.path(), QStringLiteral("precise")).isEmpty());
        QVERIFY(!OfflineTranslationService::resourceProblem(settingsDirectory_.path(), QStringLiteral("lite")).isEmpty());
        // Earlier clients stored "precise" for everyone; without resources it was no choice.
        restored.mutableSettings().aiTranslate.offlineQuality = QStringLiteral("precise");
        restored.mutableSettings().aiTranslate.offlineResourceDirectory.clear();
        QVERIFY(restored.save());
        AppConfig migrated; migrated.load();
        QCOMPARE(migrated.settings().aiTranslate.offlineQuality, QStringLiteral("lite"));
    }

    void realOfflinePackageWhenProvided()
    {
        const QString root = qEnvironmentVariable("VISNIP_REAL_OFFLINE_ROOT");
        if (root.isEmpty()) QSKIP("Set VISNIP_REAL_OFFLINE_ROOT for explicit real-model acceptance.");
        Perf::initialize();
        QImage input(900, 330, QImage::Format_RGB32); input.fill(Qt::white);
        QPainter painter(&input); QFont font(QStringLiteral("Arial")); font.setPixelSize(26); painter.setFont(font); painter.setPen(Qt::black);
        painter.drawText(45, 60, QStringLiteral("Project settings"));
        painter.drawText(45, 120, QStringLiteral("Keep 12 files in the local folder."));
        painter.drawText(45, 180, QStringLiteral("Save changes")); painter.end();
        AppConfig config; config.mutableSettings().aiTranslate.translationMethod = TranslationMethod::Offline;
        config.mutableSettings().aiTranslate.offlineResourceDirectory = root;
        config.mutableSettings().aiTranslate.offlineQuality = qEnvironmentVariable("VISNIP_REAL_OFFLINE_QUALITY", QStringLiteral("basic"));
        ImageTranslationService service(&config);
        QSignalSpy done(&service, &ImageTranslationService::succeeded);
        QSignalSpy failure(&service, &ImageTranslationService::failed);
        service.translate(input, QStringLiteral("zh-Hans"));
        QTRY_VERIFY_WITH_TIMEOUT(!done.isEmpty() || !failure.isEmpty(), 600000);
        QVERIFY2(failure.isEmpty(), failure.isEmpty() ? "" : qPrintable(failure.first().first().toString()));
        const auto result = qvariant_cast<ImageTranslationResult>(done.first().first());
        QCOMPARE(result.image.size(), input.size());
        QVERIFY(result.blockCount >= 2);
        QVERIFY(result.image != input);
        int visibleInk = 0;
        for (int y = 0; y < result.image.height(); ++y)
            for (int x = 0; x < result.image.width(); ++x)
                if (qGray(result.image.pixel(x, y)) < 160) ++visibleInk;
        QVERIFY(visibleInk > 300);
        QVERIFY(!service.isBusy());
        qInfo() << "Offline actual elapsed(ms)" << done.first().at(1).toLongLong() << "regions" << result.blockCount << result.notice;
        const QString output = qEnvironmentVariable("VISNIP_REAL_OFFLINE_OUTPUT");
        if (!output.isEmpty() && !QFileInfo::exists(output)) QVERIFY(result.image.save(output, "PNG"));
    }

    void realCudaScreenshotWhenProvided()
    {
        const QString source = qEnvironmentVariable("VISNIP_REAL_GPU_SCREENSHOT");
        if (source.isEmpty()) QSKIP("Explicit local CUDA screenshot validation only.");
        const QString root = qEnvironmentVariable("VISNIP_REAL_OFFLINE_ROOT");
        QVERIFY(!root.isEmpty());
        Perf::initialize();
        const qint64 logStart = QFileInfo(Perf::logPath()).size();
        OfflineTranslationService::releaseSharedEngine();
        OfflineTranslationService::prewarm(root);
        QTRY_VERIFY_WITH_TIMEOUT(OfflineTranslationService::sharedEngineReady(), 180000);
        const qint64 pid = OfflineTranslationService::sharedEngineProcessId();
        QImage input(source); QVERIFY(!input.isNull());
        OfflineTranslationService service;
        QSignalSpy done(&service, &OfflineTranslationService::succeeded);
        QSignalSpy failure(&service, &OfflineTranslationService::failed);
        QImage blank(320, 120, QImage::Format_RGB32); blank.fill(Qt::white);
        service.translate(blank, QStringLiteral("zh-Hans"), root, QStringLiteral("precise"));
        QTRY_VERIFY_WITH_TIMEOUT(!done.isEmpty() || !failure.isEmpty(), 120000);
        QCOMPARE(done.size(), 0);
        QCOMPARE(failure.size(), 1);
        QVERIFY(failure.first().first().toString().contains(QStringLiteral("ocr_empty_or_too_many_units")));
        QVERIFY(OfflineTranslationService::sharedEngineReady());
        QCOMPARE(OfflineTranslationService::sharedEngineProcessId(), pid);
        qInfo() << "Recoverable blank-image failure retained CUDA worker" << pid;
        failure.clear();
        service.translate(input, QStringLiteral("zh-Hans"), root, QStringLiteral("precise"));
        QTRY_VERIFY_WITH_TIMEOUT(!done.isEmpty() || !failure.isEmpty(), 120000);
        QVERIFY2(failure.isEmpty(), failure.isEmpty() ? "" : qPrintable(failure.first().first().toString()));
        const auto result = qvariant_cast<ImageTranslationResult>(done.first().first());
        QCOMPARE(result.image.size(), input.size());
        QVERIFY(result.image != input);
        QCOMPARE(OfflineTranslationService::sharedEngineProcessId(), pid);
        QFile logfile(Perf::logPath()); QVERIFY(logfile.open(QIODevice::ReadOnly));
        QVERIFY(logfile.seek(logStart));
        const QByteArray log = logfile.readAll();
        QVERIFY(log.contains("\"code\":\"ocr_empty_or_too_many_units\""));
        QVERIFY(log.contains("\"model_reused\":true"));
        QVERIFY(log.contains("\"vision_strategy\":\"gpu-full\""));
        QVERIFY(log.contains("\"vision_device\":\"cuda\""));
        QVERIFY(log.contains("\"lama_device\":\"cuda\""));
        if (qEnvironmentVariableIsSet("VISNIP_VERIFY_MENUS_1010")) {
            const QStringList expected{QStringLiteral("Code"), QStringLiteral("Issues"), QStringLiteral("Pull requests"),
                QStringLiteral("Agents"), QStringLiteral("Actions"), QStringLiteral("Projects"),
                QStringLiteral("Security and quality"), QStringLiteral("Insights"), QStringLiteral("Settings")};
            QStringList observed;
            for (const auto& value : result.blocks) {
                const auto block = value.toObject();
                const QString sourceText = block.value(QStringLiteral("source")).toString();
                if (!expected.contains(sourceText)) continue;
                observed.append(sourceText);
                QCOMPARE(block.value(QStringLiteral("status")).toString(), QStringLiteral("applied"));
                QVERIFY(block.value(QStringLiteral("reason")).toString().isEmpty());
                const QString translated = block.value(QStringLiteral("translation")).toString();
                QVERIFY(!translated.isEmpty() && translated != sourceText);
                QVERIFY(std::any_of(translated.begin(), translated.end(), [](QChar c) { return c.unicode() >= 0x3400 && c.unicode() <= 0x9fff; }));
            }
            QCOMPARE(observed.size(), expected.size());
            for (const auto& label : expected) QVERIFY(observed.contains(label));
            QVERIFY(result.notice.isEmpty());
            const QImage originalRgb = input.convertToFormat(QImage::Format_RGB32);
            const QImage translatedRgb = result.image.convertToFormat(QImage::Format_RGB32);
            // The supplied fixture's nine icons and header remain unchanged.
            const QList<QRect> unchanged{QRect(0,0,1010,60), QRect(0,94,1010,19),
                QRect(22,65,19,19), QRect(104,65,18,19), QRect(190,65,18,19),
                QRect(318,65,20,20), QRect(410,64,20,21), QRect(506,64,20,22),
                QRect(604,64,18,22), QRect(778,64,20,22), QRect(877,64,19,22)};
            for (const auto& rect : unchanged) QCOMPARE(translatedRgb.copy(rect), originalRgb.copy(rect));
            qInfo() << "All nine menu labels applied; icons, header and underline preserved";
        }
        if (qEnvironmentVariableIsSet("VISNIP_VERIFY_DROPDOWN_282")) {
            QCOMPARE(input.size(), QSize(282,621));
            const QStringList labels{QStringLiteral("Set status"),QStringLiteral("Profile"),
                QStringLiteral("Repositories"),QStringLiteral("Stars"),QStringLiteral("Gists"),
                QStringLiteral("Organizations"),QStringLiteral("Enterprises"),QStringLiteral("Sponsors"),
                QStringLiteral("Settings"),QStringLiteral("Copilot settings"),QStringLiteral("Feature preview"),
                QStringLiteral("Appearance"),QStringLiteral("Accessibility"),QStringLiteral("Try Enterprise"),
                QStringLiteral("Free"),QStringLiteral("Sign out")};
            QStringList observed;
            int preserved = 0;
            for (const auto& value : result.blocks) {
                const auto block = value.toObject();
                const auto source = block.value(QStringLiteral("source")).toString();
                if (!labels.contains(source)) {
                    QCOMPARE(block.value(QStringLiteral("status")).toString(),QStringLiteral("preserved"));
                    QCOMPARE(block.value(QStringLiteral("reason")).toString(),QStringLiteral("equivalent"));
                    ++preserved;
                    continue;
                }
                observed.append(source);
                QCOMPARE(block.value(QStringLiteral("status")).toString(),QStringLiteral("applied"));
                const auto text = block.value(QStringLiteral("translation")).toString();
                QVERIFY(std::any_of(text.begin(),text.end(),[](QChar c){return c.unicode() >= 0x3400 && c.unicode() <= 0x9fff;}));
            }
            QCOMPARE(observed.size(),16);
            for (const auto& label : labels) QVERIFY(observed.contains(label));
            QCOMPARE(preserved,2);
            QVERIFY(result.notice.isEmpty());
            const auto original = input.convertToFormat(QImage::Format_RGB32);
            const auto translated = result.image.convertToFormat(QImage::Format_RGB32);
            const QList<QRect> unchanged{QRect(0,0,282,68),QRect(0,0,54,621),QRect(265,0,17,621),
                QRect(25,101,241,5),QRect(25,341,241,5),QRect(25,549,241,5),
                QRect(219,518,41,3),QRect(219,537,41,3),QRect(219,521,6,15),QRect(255,521,5,15)};
            for (const auto& area : unchanged) QCOMPARE(translated.copy(area),original.copy(area));
            qInfo() << "All 16 dropdown labels applied; identity, icon rail, separators and badge rim unchanged";
        }
        qInfo() << "Qt CUDA screenshot task ms" << done.first().at(1).toLongLong() << "regions" << result.blockCount << result.notice;
        if (qEnvironmentVariableIsSet("VISNIP_VERIFY_READABILITY_933")) {
            QCOMPARE(input.size(), QSize(933, 290));
            int applied = 0;
            for (const auto& item : result.blocks) {
                const auto block = item.toObject();
                if (block.value(QStringLiteral("status")).toString() == QStringLiteral("applied")) ++applied;
                else QCOMPARE(block.value(QStringLiteral("reason")).toString(), QStringLiteral("equivalent"));
            }
            QCOMPARE(applied, 6);
            QVERIFY(result.notice.isEmpty());
            for (const QRect& area : {QRect(27,18,23,24), QRect(190,20,9,20), QRect(29,56,23,22),
                    QRect(28,183,24,23), QRect(27,132,138,23), QRect(27,242,158,25),
                    QRect(799,56,21,22), QRect(799,188,21,22)}) {
                QCOMPARE(result.image.copy(area).convertToFormat(QImage::Format_RGB32),
                         input.copy(area).convertToFormat(QImage::Format_RGB32));
            }
        }
        const QString output = qEnvironmentVariable("VISNIP_REAL_OFFLINE_OUTPUT");
        if (!output.isEmpty()) QVERIFY(result.image.save(output, "PNG"));
        OfflineTranslationService::releaseSharedEngine();
    }

    void realUnifiedBatchWhenProvided()
    {
        const QString directory = qEnvironmentVariable("VISNIP_REAL_UNIFIED_FIXTURES");
        if (directory.isEmpty()) QSKIP("Explicit supplied-image unified GPU regression only.");
        const QString root = qEnvironmentVariable("VISNIP_REAL_OFFLINE_ROOT");
        const QString output = qEnvironmentVariable("VISNIP_REAL_UNIFIED_OUTPUT");
        QVERIFY(!root.isEmpty() && !output.isEmpty());
        QVERIFY(QDir().mkpath(output));
        Perf::initialize();
        OfflineTranslationService::releaseSharedEngine();
        OfflineTranslationService::prewarm(root);
        QTRY_VERIFY_WITH_TIMEOUT(OfflineTranslationService::sharedEngineReady(), 180000);
        const qint64 pid = OfflineTranslationService::sharedEngineProcessId();
        const QStringList files{QStringLiteral("actual-user-footer-963x83.png"),
            QStringLiteral("actual-user-menus-1010x113.png"), QStringLiteral("actual-user-dropdown-282x621.png"),
            QStringLiteral("actual-user-original-933x290.png"), QStringLiteral("actual-user-original-947x458.png")};
        QJsonArray report;
        for (const auto& name : files) {
            QImage image(QDir(directory).filePath(name));
            QVERIFY2(!image.isNull(), qPrintable(name));
            OfflineTranslationService service;
            QSignalSpy done(&service, &OfflineTranslationService::succeeded);
            QSignalSpy failed(&service, &OfflineTranslationService::failed);
            service.translate(image, QStringLiteral("zh-Hans"), root, QStringLiteral("precise"));
            QTRY_VERIFY_WITH_TIMEOUT(!done.isEmpty() || !failed.isEmpty(), 120000);
            QVERIFY2(failed.isEmpty(), failed.isEmpty() ? "" : qPrintable(failed.first().first().toString()));
            const auto result = qvariant_cast<ImageTranslationResult>(done.first().first());
            QCOMPARE(result.image.size(), image.size());
            QCOMPARE(OfflineTranslationService::sharedEngineProcessId(), pid);
            int placed = 0, panelCount = 0, unchanged = 0;
            for (const auto& value : result.blocks) {
                const auto b = value.toObject();
                const auto mode = b.value(QStringLiteral("display_mode")).toString();
                const auto state = b.value(QStringLiteral("translation_status")).toString();
                QVERIFY2(state != QStringLiteral("failed"), qPrintable(name));
                if (mode == QStringLiteral("unchanged")) { ++unchanged; continue; }
                const QString target = b.value(QStringLiteral("translation")).toString();
                QVERIFY(!target.trimmed().isEmpty());
                QVERIFY(mode == QStringLiteral("inline") || mode == QStringLiteral("panel"));
                placed += mode == QStringLiteral("inline");
                panelCount += mode == QStringLiteral("panel");
            }
            QVERIFY(placed + panelCount > 0);
            QWidget host; host.resize(1400, 900); host.show();
            TranslationReviewPanel panel(&host);
            panel.setResult(result.blocks, QRect(30, 30, image.width(), image.height()));
            panel.setTranslationVisible(true);
            auto* text = panel.findChild<QPlainTextEdit*>(QStringLiteral("VisnipTranslationReviewText"));
            QVERIFY(text && !panel.isVisible());
            panel.toggleByUser();
            QVERIFY(panel.isVisible() && text->isVisible());
            for (const auto& value : result.blocks) {
                const auto b = value.toObject();
                if (b.value(QStringLiteral("display_mode")).toString() != QStringLiteral("unchanged"))
                    QVERIFY(text->toPlainText().contains(b.value(QStringLiteral("translation")).toString()));
            }
            QVERIFY(result.image.save(QDir(output).filePath(name), "PNG"));
            report.append(QJsonObject{{"fixture", name}, {"milliseconds", done.first().at(1).toLongLong()},
                {"inline", placed}, {"panel", panelCount}, {"unchanged", unchanged}});
            qInfo() << name << "total ms" << done.first().at(1).toLongLong()
                    << "inline" << placed << "panel" << panelCount << "unchanged" << unchanged;
        }
        QFile reportFile(QDir(output).filePath(QStringLiteral("client-summary.json")));
        QVERIFY(reportFile.open(QIODevice::WriteOnly));
        QVERIFY(reportFile.write(QJsonDocument(report).toJson()) > 0);
        OfflineTranslationService::releaseSharedEngine();
    }

    void unchangedBitmapCanCarryValidPanelTranslation()
    {
        if (!onlineTranslationEnabled()) {
            QSKIP("Online translation is not built in (VISNIP_ONLINE_TRANSLATION=OFF).");
        }
        HttpFixture fixture;
        QVERIFY(fixture.listen());
        auto object = QJsonDocument::fromJson(imageResponse()).object();
        object.insert(QStringLiteral("blocks"), QJsonArray{QJsonObject{{"id", "u0"},
            {"source", "Keep all text"}, {"translation", "保留完整文字"}, {"status", "preserved"},
            {"reason", "layout_unfit"}, {"translation_status", "translated"}, {"display_mode", "panel"}}});
        fixture.setJson(QJsonDocument(object).toJson());
        AppConfig config;
        acceptUploads(config);
        config.mutableSettings().aiTranslate.translationMethod = TranslationMethod::Intranet;
        config.mutableSettings().aiTranslate.intranetServiceUrl = fixture.root();
        ImageTranslationService service(&config);
        QSignalSpy done(&service, &ImageTranslationService::succeeded);
        QSignalSpy failed(&service, &ImageTranslationService::failed);
        service.translate(sampleImage(), QStringLiteral("zh-Hans"));
        QTRY_VERIFY_WITH_TIMEOUT(!done.isEmpty() || !failed.isEmpty(), 3000);
        QVERIFY(failed.isEmpty());
        const auto result = qvariant_cast<ImageTranslationResult>(done.first().first());
        QCOMPARE(result.image.convertToFormat(QImage::Format_RGB32), sampleImage());
        QCOMPARE(result.blocks.first().toObject().value(QStringLiteral("translation")).toString(), QStringLiteral("保留完整文字"));
    }

    void switchingOfflineCancelsAnActiveUpload()
    {
        if (!onlineTranslationEnabled()) {
            QSKIP("Online translation is not built in (VISNIP_ONLINE_TRANSLATION=OFF).");
        }
        HttpFixture fixture;
        QVERIFY(fixture.listen());
        fixture.holdResponse = true;
        AppConfig config;
        acceptUploads(config);
        config.mutableSettings().aiTranslate.translationMethod = TranslationMethod::Intranet;
        config.mutableSettings().aiTranslate.intranetServiceUrl = fixture.root();
        ImageTranslationService service(&config);
        QSignalSpy cancelled(&service, &ImageTranslationService::cancelled);
        service.translate(sampleImage(), QStringLiteral("zh-Hans"));
        QTRY_VERIFY_WITH_TIMEOUT(fixture.completed, 3000);
        config.mutableSettings().aiTranslate.translationMethod = TranslationMethod::Offline;
        QVERIFY(config.save());
        QTRY_COMPARE_WITH_TIMEOUT(cancelled.size(), 1, 3000);
        QVERIFY(!service.isBusy());
    }
};

QTEST_MAIN(TranslationModeTests)
#include "tst_translation_modes.moc"
