#pragma once
#include <QByteArray>
#include <QString>
#include <QStringList>
#include <QUrl>
#include <QVersionNumber>

namespace Visnip {

// A published release of the Windows package on GitHub.
struct AppRelease {
    QVersionNumber version;
    QString tag;
    QString notes;
    QUrl page;
    QUrl package;         // Visnip-<version>-windows-x64.zip
    qint64 packageSize = 0;
    QByteArray sha256;    // GitHub's digest of the uploaded asset, when present
    QUrl checksum;        // the release's .sha256 file, when present
};

namespace AppUpdate {
// "owner/name" of the repository that publishes releases (CMake
// VISNIP_UPDATE_REPOSITORY); empty when the build has no update source.
QString repository();
QUrl latestReleaseApi();
QUrl releasesPage();
// Accepts only a published, non-pre-release version with a Windows package
// hosted in the repository's own release downloads.
bool parseLatestRelease(const QByteArray& json, AppRelease* release, QString* error);
bool isNewer(const QVersionNumber& candidate, const QString& current = QStringLiteral(VISNIP_VERSION));
// GitHub, its API and its release CDN, over HTTPS only.
bool isAllowedHost(const QUrl& url);
// First token of a "sha256sum" line, lower-case hex; empty when malformed.
QByteArray parseChecksumFile(const QByteArray& content);

// Every packaged installation lists its files here, relative and with "/".
QString packageListName();
bool isPackagedInstall(const QString& directory);
// Files of an installation: the package list when present, otherwise every
// file below the directory except logs and update leftovers.
QStringList packageFiles(const QString& directory, QString* error = nullptr);
// Replaces the installation in target with the package in source. Files the
// new package replaces or no longer has are moved to a backup folder first and
// restored when any step fails; files that are not part of either package
// (logs, user files) are never touched. A backup left by an interrupted
// update is restored before anything else.
bool applyPackage(const QString& source, const QString& target, QString* error);
// Where downloaded packages are unpacked before they are applied.
QString updatesDirectory();
}

} // namespace Visnip
