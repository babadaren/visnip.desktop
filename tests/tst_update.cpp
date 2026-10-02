#include "core/AppUpdate.h"
#include "services/UpdateService.h"

#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTimer>
#include <QtTest>

namespace Visnip {
namespace {
const QString kRepo = QStringLiteral(VISNIP_UPDATE_REPOSITORY);

QByteArray releaseJson(const QString& tag, bool withDigest = true, bool withChecksum = true,
                       const QString& host = QStringLiteral("github.com"))
{
    const QString version = tag.mid(1);
    const QString name = QStringLiteral("Visnip-%1-windows-x64.zip").arg(version);
    const QString base = QStringLiteral("https://%1/%2/releases/download/%3/").arg(host, kRepo, tag);
    QJsonObject zip{{QStringLiteral("name"), name}, {QStringLiteral("size"), 1234},
                    {QStringLiteral("browser_download_url"), base + name}};
    if (withDigest) zip.insert(QStringLiteral("digest"), QStringLiteral("sha256:") + QString(64, QLatin1Char('a')));
    QJsonArray assets{zip};
    if (withChecksum) {
        assets.append(QJsonObject{{QStringLiteral("name"), name + QStringLiteral(".sha256")}, {QStringLiteral("size"), 90},
                                  {QStringLiteral("browser_download_url"), base + name + QStringLiteral(".sha256")}});
    }
    return QJsonDocument(QJsonObject{{QStringLiteral("tag_name"), tag}, {QStringLiteral("draft"), false},
                                     {QStringLiteral("prerelease"), false},
                                     {QStringLiteral("html_url"), QStringLiteral("https://github.com/%1/releases/tag/%2").arg(kRepo, tag)},
                                     {QStringLiteral("body"), QStringLiteral("## Changes\n- **Faster** capture")},
                                     {QStringLiteral("assets"), assets}}).toJson();
}

bool write(const QString& path, const QByteArray& content)
{
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile file(path);
    return file.open(QIODevice::WriteOnly) && file.write(content) == content.size();
}

QByteArray read(const QString& path)
{
    QFile file(path);
    return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray("<missing>");
}

// A package folder with the given files plus its file list.
void makePackage(const QString& root, const QMap<QString, QByteArray>& files, const QStringList& extraListed = {})
{
    QStringList listed;
    for (auto it = files.cbegin(); it != files.cend(); ++it) {
        QVERIFY(write(QDir(root).filePath(it.key()), it.value()));
        listed.append(it.key());
    }
    listed += extraListed;
    listed.append(AppUpdate::packageListName());
    QVERIFY(write(QDir(root).filePath(AppUpdate::packageListName()), listed.join(QLatin1Char('\n')).toUtf8()));
}

class FixtureReply final : public QNetworkReply {
public:
    FixtureReply(const QNetworkRequest& request, QByteArray body, int status, QObject* parent)
        : QNetworkReply(parent), bytes_(std::move(body))
    {
        setRequest(request); setUrl(request.url()); open(QIODevice::ReadOnly | QIODevice::Unbuffered);
        setAttribute(QNetworkRequest::HttpStatusCodeAttribute, status);
        QTimer::singleShot(0, this, [this, status]() {
            emit metaDataChanged(); emit readyRead();
            if (status != 200) setError(QNetworkReply::ContentNotFoundError, QStringLiteral("fixture"));
            setFinished(true); emit finished();
        });
    }
    void abort() override { setFinished(true); }
    qint64 bytesAvailable() const override { return bytes_.size() - position_ + QNetworkReply::bytesAvailable(); }
protected:
    qint64 readData(char* output, qint64 maximum) override
    {
        const qint64 size = qMin<qint64>(maximum, bytes_.size() - position_);
        if (size == 0) return -1;
        memcpy(output, bytes_.constData() + position_, size); position_ += size; return size;
    }
private:
    QByteArray bytes_;
    qint64 position_ = 0;
};

class FixtureNetwork final : public QNetworkAccessManager {
public:
    QByteArray body;
    int status = 200, calls = 0;
    QNetworkRequest last;
protected:
    QNetworkReply* createRequest(Operation, const QNetworkRequest& request, QIODevice*) override
    {
        ++calls; last = request;
        return new FixtureReply(request, body, status, this);
    }
};
} // namespace

class UpdateTests : public QObject {
    Q_OBJECT
private slots:
    void acceptsOnlyThisRepositorysPublishedPackage()
    {
        QVERIFY(!AppUpdate::repository().isEmpty());
        AppRelease release; QString error;
        QVERIFY2(AppUpdate::parseLatestRelease(releaseJson(QStringLiteral("v9.8.7")), &release, &error), qPrintable(error));
        QCOMPARE(release.version, QVersionNumber(9, 8, 7));
        QCOMPARE(release.package.fileName(), QStringLiteral("Visnip-9.8.7-windows-x64.zip"));
        QCOMPARE(release.sha256, QByteArray(64, 'a'));
        QVERIFY(release.checksum.toString().endsWith(QStringLiteral(".zip.sha256")));
        QCOMPARE(release.packageSize, qint64(1234));

        // Only one of the two checksums is enough.
        QVERIFY(AppUpdate::parseLatestRelease(releaseJson(QStringLiteral("v9.8.7"), false, true), &release, &error));
        QVERIFY(AppUpdate::parseLatestRelease(releaseJson(QStringLiteral("v9.8.7"), true, false), &release, &error));
        QVERIFY(!AppUpdate::parseLatestRelease(releaseJson(QStringLiteral("v9.8.7"), false, false), &release, &error));
        QVERIFY(error.contains(QStringLiteral("SHA-256")));
        // Packages outside the repository's release downloads are refused.
        QVERIFY(!AppUpdate::parseLatestRelease(releaseJson(QStringLiteral("v9.8.7"), true, true,
                                                           QStringLiteral("evil.example")), &release, &error));
        QVERIFY(!AppUpdate::parseLatestRelease(releaseJson(QStringLiteral("9.8.7")), &release, &error));

        auto object = QJsonDocument::fromJson(releaseJson(QStringLiteral("v9.8.7"))).object();
        object[QStringLiteral("prerelease")] = true;
        QVERIFY(!AppUpdate::parseLatestRelease(QJsonDocument(object).toJson(), &release, &error));
        object = QJsonDocument::fromJson(releaseJson(QStringLiteral("v9.8.7"))).object();
        auto assets = object.value(QStringLiteral("assets")).toArray();
        auto zip = assets[0].toObject(); zip[QStringLiteral("size")] = 900LL * 1024 * 1024; assets[0] = zip;
        object[QStringLiteral("assets")] = assets;
        QVERIFY(!AppUpdate::parseLatestRelease(QJsonDocument(object).toJson(), &release, &error));
        QVERIFY(!AppUpdate::parseLatestRelease("not json", &release, &error));
    }

    void versionsAndHosts()
    {
        QVERIFY(AppUpdate::isNewer(QVersionNumber(0, 4, 3), QStringLiteral("0.4.2")));
        QVERIFY(AppUpdate::isNewer(QVersionNumber(0, 10, 0), QStringLiteral("0.9.9")));
        QVERIFY(!AppUpdate::isNewer(QVersionNumber(0, 4, 2), QStringLiteral("0.4.2")));
        QVERIFY(!AppUpdate::isNewer(QVersionNumber(0, 4, 1), QStringLiteral("0.4.2")));
        for (const QString& good : {QStringLiteral("https://github.com/x"), QStringLiteral("https://api.github.com/x"),
                                    QStringLiteral("https://release-assets.githubusercontent.com/x?sig=1")}) {
            QVERIFY2(AppUpdate::isAllowedHost(QUrl(good)), qPrintable(good));
        }
        for (const QString& bad : {QStringLiteral("http://github.com/x"), QStringLiteral("https://evilgithub.com/x"),
                                   QStringLiteral("https://github.com.evil.example/x"), QStringLiteral("https://u:p@github.com/x"),
                                   QStringLiteral("https://github.com:444/x")}) {
            QVERIFY2(!AppUpdate::isAllowedHost(QUrl(bad)), qPrintable(bad));
        }
        QCOMPARE(AppUpdate::parseChecksumFile(QByteArray(64, 'A') + "  Visnip.zip\n"), QByteArray(64, 'a'));
        QVERIFY(AppUpdate::parseChecksumFile("<html>").isEmpty());
    }

    void packageListRejectsEscapingPaths()
    {
        QTemporaryDir dir; QVERIFY(dir.isValid());
        for (const QByteArray& bad : {QByteArray("../evil.dll"), QByteArray("/abs.dll"), QByteArray("C:/x.dll"),
                                      QByteArray("a\\b.dll"), QByteArray("logs/x.txt")}) {
            QVERIFY(write(dir.filePath(AppUpdate::packageListName()), "visnip.exe\n" + bad + "\n"));
            QString error;
            QVERIFY(AppUpdate::packageFiles(dir.path(), &error).isEmpty());
            QVERIFY2(!error.isEmpty(), bad.constData());
        }
    }

    void applyReplacesPackageFilesAndKeepsUserFiles()
    {
        QTemporaryDir source, target; QVERIFY(source.isValid() && target.isValid());
        makePackage(target.path(), {{QStringLiteral("visnip.exe"), "old exe"}, {QStringLiteral("old-plugin.dll"), "gone"},
                                    {QStringLiteral("platforms/qwindows.dll"), "old qpa"}});
        QVERIFY(write(target.filePath(QStringLiteral("logs/visnip-perf.log")), "user log"));
        QVERIFY(write(target.filePath(QStringLiteral("notes.txt")), "user file"));
        makePackage(source.path(), {{QStringLiteral("visnip.exe"), "new exe"}, {QStringLiteral("platforms/qwindows.dll"), "new qpa"},
                                    {QStringLiteral("new.dll"), "added"}});
        QString error;
        QVERIFY2(AppUpdate::applyPackage(source.path(), target.path(), &error), qPrintable(error));
        QCOMPARE(read(target.filePath(QStringLiteral("visnip.exe"))), QByteArray("new exe"));
        QCOMPARE(read(target.filePath(QStringLiteral("platforms/qwindows.dll"))), QByteArray("new qpa"));
        QCOMPARE(read(target.filePath(QStringLiteral("new.dll"))), QByteArray("added"));
        QVERIFY(!QFileInfo::exists(target.filePath(QStringLiteral("old-plugin.dll")))); // dropped by the new package
        QCOMPARE(read(target.filePath(QStringLiteral("logs/visnip-perf.log"))), QByteArray("user log"));
        QCOMPARE(read(target.filePath(QStringLiteral("notes.txt"))), QByteArray("user file"));
        QVERIFY(read(target.filePath(AppUpdate::packageListName())).contains("new.dll"));
        QVERIFY(!QFileInfo::exists(target.filePath(QStringLiteral(".visnip-update-backup"))));
    }

    void failedApplyRestoresTheOldVersion()
    {
        QTemporaryDir source, target; QVERIFY(source.isValid() && target.isValid());
        makePackage(target.path(), {{QStringLiteral("a.dll"), "old a"}, {QStringLiteral("visnip.exe"), "old exe"}});
        // The list names a file the package does not contain: the copy fails half-way.
        makePackage(source.path(), {{QStringLiteral("a.dll"), "new a"}, {QStringLiteral("visnip.exe"), "new exe"}},
                    {QStringLiteral("zz-missing.dll")});
        QString error;
        QVERIFY(!AppUpdate::applyPackage(source.path(), target.path(), &error));
        QVERIFY(error.contains(QStringLiteral("zz-missing.dll")));
        QCOMPARE(read(target.filePath(QStringLiteral("a.dll"))), QByteArray("old a"));
        QCOMPARE(read(target.filePath(QStringLiteral("visnip.exe"))), QByteArray("old exe"));
        QVERIFY(!read(target.filePath(AppUpdate::packageListName())).contains("zz-missing"));
        QVERIFY(!QFileInfo::exists(target.filePath(QStringLiteral(".visnip-update-backup"))));
    }

    void interruptedUpdateIsRolledBackFirst()
    {
        QTemporaryDir source, target; QVERIFY(source.isValid() && target.isValid());
        makePackage(target.path(), {{QStringLiteral("visnip.exe"), "half new"}});
        QVERIFY(write(target.filePath(QStringLiteral(".visnip-update-backup/visnip.exe")), "old exe"));
        // An unusable package still restores the last complete version.
        QVERIFY(write(source.filePath(AppUpdate::packageListName()), "readme.txt\n"));
        QVERIFY(write(source.filePath(QStringLiteral("readme.txt")), "x"));
        QString error;
        QVERIFY(!AppUpdate::applyPackage(source.path(), target.path(), &error));
        QCOMPARE(read(target.filePath(QStringLiteral("visnip.exe"))), QByteArray("old exe"));
        QVERIFY(!QFileInfo::exists(target.filePath(QStringLiteral(".visnip-update-backup"))));
    }

    void checkOffersOnlyNewerReleases()
    {
        UpdateService service;
        auto* network = new FixtureNetwork; network->setParent(&service);
        delete service.network_; service.network_ = network;
        QSignalSpy changed(&service, &UpdateService::stateChanged);

        network->body = releaseJson(QStringLiteral("v999.0.0"));
        service.check();
        QCOMPARE(service.state(), UpdateService::State::Checking);
        QTRY_COMPARE(service.state(), UpdateService::State::Available);
        QVERIFY(service.hasUpdate());
        QCOMPARE(service.release().version, QVersionNumber(999, 0, 0));
        QCOMPARE(network->last.url(), AppUpdate::latestReleaseApi());
        QVERIFY(network->last.rawHeader("Authorization").isEmpty());
        QVERIFY(network->last.rawHeader("Cookie").isEmpty());

        network->body = releaseJson(QStringLiteral("v") + QStringLiteral(VISNIP_VERSION));
        service.check();
        QTRY_COMPARE(service.state(), UpdateService::State::UpToDate);
        QVERIFY(!service.hasUpdate());

        network->status = 404; network->body = "{\"message\":\"Not Found\"}";
        service.check();
        QTRY_COMPARE(service.state(), UpdateService::State::Failed);
        QVERIFY(service.statusText().contains(QStringLiteral("还没有正式发布")));
        QVERIFY(changed.count() >= 6);
    }

    void installNeedsAnAvailableUpdate()
    {
        UpdateService service;
        auto* network = new FixtureNetwork; network->setParent(&service);
        delete service.network_; service.network_ = network;
        service.install(); // nothing offered yet
        QCOMPARE(service.state(), UpdateService::State::Idle);
        QCOMPARE(network->calls, 0);
        // A build directory is not a packaged installation and never replaces itself.
        QTemporaryDir dir; QVERIFY(dir.isValid());
        QVERIFY(!UpdateService::installProblem(dir.path()).isEmpty());
    }
};
} // namespace Visnip

QTEST_MAIN(Visnip::UpdateTests)
#include "tst_update.moc"
