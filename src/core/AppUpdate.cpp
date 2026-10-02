#include "core/AppUpdate.h"

#include <QDateTime>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <QSet>
#include <QStandardPaths>
#include <QThread>

#ifndef VISNIP_UPDATE_REPOSITORY
#define VISNIP_UPDATE_REPOSITORY ""
#endif

namespace Visnip::AppUpdate {
namespace {
constexpr qint64 kMaxPackageBytes = 512LL * 1024 * 1024;
constexpr int kMaxNotesCharacters = 4000;
const QString kBackup = QStringLiteral(".visnip-update-backup");
const QString kRetiredPrefix = QStringLiteral(".visnip-update-old-");

bool fail(QString* error, const QString& message)
{
    if (error) *error = message;
    return false;
}

bool safeRelativePath(const QString& path)
{
    return !path.isEmpty() && !path.startsWith(QLatin1Char('/')) && !path.contains(QLatin1Char('\\'))
        && !path.contains(QLatin1Char(':')) && !path.split(QLatin1Char('/')).contains(QStringLiteral(".."));
}

bool skippedDirectory(const QString& relative)
{
    return relative.startsWith(QStringLiteral("logs/")) || relative.startsWith(kBackup + QLatin1Char('/'))
        || relative.startsWith(kRetiredPrefix);
}

bool ensureParent(const QString& path)
{
    return QDir().mkpath(QFileInfo(path).absolutePath());
}

// A file of a process that just exited can stay locked for a moment (for
// example while an anti-virus scanner reads it).
bool moveFile(const QString& from, const QString& to)
{
    if (!ensureParent(to)) return false;
    for (int attempt = 0; attempt < 40; ++attempt) {
        if (QFile::rename(from, to)) return true;
        QThread::msleep(250);
    }
    return false;
}

bool restoreBackup(const QString& target, QString* error)
{
    const QString backup = QDir(target).filePath(kBackup);
    QDirIterator files(backup, QDir::Files | QDir::Hidden | QDir::System, QDirIterator::Subdirectories);
    while (files.hasNext()) {
        const QString path = files.next();
        const QString destination = QDir(target).filePath(QDir(backup).relativeFilePath(path));
        if (QFileInfo::exists(destination) && !QFile::remove(destination)) {
            return fail(error, QStringLiteral("无法恢复 %1。").arg(QDir(backup).relativeFilePath(path)));
        }
        if (!moveFile(path, destination)) {
            return fail(error, QStringLiteral("无法恢复 %1。").arg(QDir(backup).relativeFilePath(path)));
        }
    }
    QDir(backup).removeRecursively();
    return true;
}
} // namespace

QString repository()
{
    return QString::fromUtf8(VISNIP_UPDATE_REPOSITORY).trimmed();
}

QUrl latestReleaseApi()
{
    return repository().isEmpty() ? QUrl()
        : QUrl(QStringLiteral("https://api.github.com/repos/%1/releases/latest").arg(repository()));
}

QUrl releasesPage()
{
    return repository().isEmpty() ? QUrl()
        : QUrl(QStringLiteral("https://github.com/%1/releases").arg(repository()));
}

bool isAllowedHost(const QUrl& url)
{
    if (!url.isValid() || url.scheme() != QStringLiteral("https") || url.port(443) != 443
        || !url.userName().isEmpty() || !url.password().isEmpty() || url.hasFragment()) {
        return false;
    }
    const QString host = url.host().toLower();
    return host == QStringLiteral("github.com") || host == QStringLiteral("api.github.com")
        || host.endsWith(QStringLiteral(".githubusercontent.com"));
}

bool isNewer(const QVersionNumber& candidate, const QString& current)
{
    const QVersionNumber running = QVersionNumber::fromString(current);
    return !candidate.isNull() && !running.isNull() && candidate > running;
}

QByteArray parseChecksumFile(const QByteArray& content)
{
    const QByteArray token = content.trimmed().split(' ').value(0).trimmed().toLower();
    return QRegularExpression(QStringLiteral("^[0-9a-f]{64}$")).match(QString::fromLatin1(token)).hasMatch()
        ? token : QByteArray();
}

bool parseLatestRelease(const QByteArray& json, AppRelease* release, QString* error)
{
    if (!release) return fail(error, QStringLiteral("内部错误。"));
    const QJsonObject object = QJsonDocument::fromJson(json).object();
    if (object.isEmpty()) return fail(error, QStringLiteral("无法读取 GitHub 返回的版本信息。"));
    if (object.value(QStringLiteral("draft")).toBool() || object.value(QStringLiteral("prerelease")).toBool()) {
        return fail(error, QStringLiteral("最新版本还没有正式发布。"));
    }
    const QString tag = object.value(QStringLiteral("tag_name")).toString();
    const auto match = QRegularExpression(QStringLiteral("^v(\\d{1,4})\\.(\\d{1,4})\\.(\\d{1,4})$")).match(tag);
    if (!match.hasMatch()) return fail(error, QStringLiteral("版本号格式无法识别：%1").arg(tag));

    AppRelease parsed;
    parsed.tag = tag;
    parsed.version = QVersionNumber::fromString(tag.mid(1));
    parsed.notes = object.value(QStringLiteral("body")).toString().left(kMaxNotesCharacters);
    const QUrl page(object.value(QStringLiteral("html_url")).toString());
    parsed.page = isAllowedHost(page) ? page : QUrl(releasesPage().toString() + QStringLiteral("/tag/") + tag);

    const QString name = QStringLiteral("Visnip-%1-windows-x64.zip").arg(parsed.version.toString());
    const QString downloads = QStringLiteral("/%1/releases/download/%2/").arg(repository(), tag);
    for (const QJsonValue& value : object.value(QStringLiteral("assets")).toArray()) {
        const QJsonObject asset = value.toObject();
        const QString assetName = asset.value(QStringLiteral("name")).toString();
        const QUrl url(asset.value(QStringLiteral("browser_download_url")).toString());
        // Only files in this repository's own release downloads are trusted.
        const bool ours = url.host().toLower() == QStringLiteral("github.com") && isAllowedHost(url)
            && !url.hasQuery() && url.path().compare(downloads + assetName, Qt::CaseInsensitive) == 0;
        if (assetName == name && ours) {
            parsed.package = url;
            parsed.packageSize = asset.value(QStringLiteral("size")).toInteger();
            const QString digest = asset.value(QStringLiteral("digest")).toString();
            if (digest.startsWith(QStringLiteral("sha256:"))) {
                parsed.sha256 = parseChecksumFile(digest.mid(7).toLatin1());
            }
        } else if (assetName == name + QStringLiteral(".sha256") && ours) {
            parsed.checksum = url;
        }
    }
    if (parsed.package.isEmpty()) {
        return fail(error, QStringLiteral("%1 没有 Windows 安装包（%2）。").arg(tag, name));
    }
    if (parsed.packageSize <= 0 || parsed.packageSize > kMaxPackageBytes) {
        return fail(error, QStringLiteral("安装包大小异常，已拒绝。"));
    }
    if (parsed.sha256.isEmpty() && parsed.checksum.isEmpty()) {
        return fail(error, QStringLiteral("%1 没有可核对的 SHA-256 校验值，已拒绝。").arg(tag));
    }
    *release = parsed;
    if (error) error->clear();
    return true;
}

QString packageListName()
{
    return QStringLiteral("package-files.txt");
}

bool isPackagedInstall(const QString& directory)
{
    return QFileInfo(QDir(directory).filePath(packageListName())).isFile();
}

QStringList packageFiles(const QString& directory, QString* error)
{
    QStringList files;
    QFile list(QDir(directory).filePath(packageListName()));
    if (list.exists()) {
        if (!list.open(QIODevice::ReadOnly) || list.size() > 1024 * 1024) {
            fail(error, QStringLiteral("无法读取 %1。").arg(packageListName()));
            return {};
        }
        for (const QByteArray& raw : list.readAll().split('\n')) {
            const QString line = QString::fromUtf8(raw).trimmed();
            if (line.isEmpty() || line.startsWith(QLatin1Char('#'))) continue;
            if (!safeRelativePath(line) || skippedDirectory(line)) {
                fail(error, QStringLiteral("%1 中有不安全的路径：%2").arg(packageListName(), line));
                return {};
            }
            files.append(line);
        }
    } else {
        QDirIterator entries(directory, QDir::Files | QDir::Hidden | QDir::System, QDirIterator::Subdirectories);
        while (entries.hasNext()) {
            const QString relative = QDir(directory).relativeFilePath(entries.next());
            if (!skippedDirectory(relative)) files.append(relative);
        }
    }
    files.removeDuplicates();
    files.sort();
    if (error) error->clear();
    return files;
}

bool applyPackage(const QString& source, const QString& target, QString* error)
{
    const QDir sourceDir(source), targetDir(target);
    if (!sourceDir.exists() || !targetDir.exists()) return fail(error, QStringLiteral("更新目录不存在。"));
    if (sourceDir.canonicalPath() == targetDir.canonicalPath()) return fail(error, QStringLiteral("更新包和安装目录相同。"));

    // Leftovers of earlier updates: a pending backup means an update stopped
    // half-way, so the last complete version is put back first.
    for (const QFileInfo& retired : targetDir.entryInfoList({kRetiredPrefix + QLatin1Char('*')},
                                                           QDir::Dirs | QDir::Hidden | QDir::NoDotAndDotDot)) {
        QDir(retired.absoluteFilePath()).removeRecursively();
    }
    if (QFileInfo::exists(targetDir.filePath(kBackup)) && !restoreBackup(target, error)) return false;

    QString listError;
    const QStringList incoming = packageFiles(source, &listError);
    if (!listError.isEmpty()) return fail(error, listError);
    if (!incoming.contains(QStringLiteral("visnip.exe")) || !incoming.contains(packageListName())) {
        return fail(error, QStringLiteral("更新包不完整（缺少 visnip.exe 或文件清单）。"));
    }
    const QStringList previous = isPackagedInstall(target) ? packageFiles(target, &listError) : QStringList();
    if (!listError.isEmpty()) return fail(error, listError);

    const QString backup = targetDir.filePath(kBackup);
    QStringList moved, written;
    const auto rollback = [&]() {
        for (const QString& relative : written) QFile::remove(targetDir.filePath(relative));
        for (const QString& relative : moved) {
            moveFile(QDir(backup).filePath(relative), targetDir.filePath(relative));
        }
        QDir(backup).removeRecursively();
    };
    for (const QString& relative : incoming) {
        const QString from = sourceDir.filePath(relative), to = targetDir.filePath(relative);
        if (!QFileInfo(from).isFile() || QFileInfo(from).isSymLink()) {
            rollback();
            return fail(error, QStringLiteral("更新包缺少 %1。").arg(relative));
        }
        if (QFileInfo::exists(to)) {
            if (!moveFile(to, QDir(backup).filePath(relative))) {
                rollback();
                return fail(error, QStringLiteral("无法替换 %1，文件可能正在使用。").arg(relative));
            }
            moved.append(relative);
        }
        if (!ensureParent(to) || !QFile::copy(from, to)) {
            rollback();
            return fail(error, QStringLiteral("无法写入 %1。").arg(relative));
        }
        written.append(relative);
    }
    const QSet<QString> keep(incoming.cbegin(), incoming.cend());
    for (const QString& relative : previous) {
        const QString path = targetDir.filePath(relative);
        if (keep.contains(relative) || !QFileInfo::exists(path)) continue;
        if (!moveFile(path, QDir(backup).filePath(relative))) {
            rollback();
            return fail(error, QStringLiteral("无法移除旧文件 %1。").arg(relative));
        }
        moved.append(relative);
    }
    // Committed. A backup that cannot be deleted right away is renamed so it is
    // never mistaken for an interrupted update.
    if (QFileInfo::exists(backup) && !QDir(backup).removeRecursively()) {
        QDir().rename(backup, targetDir.filePath(kRetiredPrefix + QString::number(QDateTime::currentMSecsSinceEpoch())));
    }
    if (error) error->clear();
    return true;
}

QString updatesDirectory()
{
    const QString local = qEnvironmentVariable("LOCALAPPDATA",
        QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation));
    return QDir(local).filePath(QStringLiteral("Visnip/updates"));
}

} // namespace Visnip::AppUpdate
