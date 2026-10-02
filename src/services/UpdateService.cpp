#include "services/UpdateService.h"
#include "core/OfflineResourceCatalog.h"
#include "core/PerfLog.h"
#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QDirIterator>
#include <QFileInfo>
#include <QFutureWatcher>
#include <QNetworkAccessManager>
#include <QNetworkProxyFactory>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QRegularExpression>
#include <QStorageInfo>
#include <QtConcurrent/QtConcurrentRun>

namespace Visnip {
namespace {
constexpr qint64 kMaxApiBytes = 1024 * 1024;
constexpr qint64 kMaxChecksumBytes = 4096;
constexpr int kProcessTimeoutMs = 3 * 60 * 1000;

class SystemProxyFactory final : public QNetworkProxyFactory {
public:
    QList<QNetworkProxy> queryProxy(const QNetworkProxyQuery& query) override {
        return QNetworkProxyFactory::systemProxyForQuery(query);
    }
};

QNetworkRequest requestFor(const QUrl& url)
{
    QNetworkRequest request(url);
    // Every redirect is checked against GitHub's own hosts before it is followed.
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::UserVerifiedRedirectPolicy);
    request.setMaximumRedirectsAllowed(5);
    request.setAttribute(QNetworkRequest::CookieLoadControlAttribute, QNetworkRequest::Manual);
    request.setAttribute(QNetworkRequest::CookieSaveControlAttribute, QNetworkRequest::Manual);
    request.setAttribute(QNetworkRequest::AuthenticationReuseAttribute, QNetworkRequest::Manual);
    request.setAttribute(QNetworkRequest::CacheLoadControlAttribute, QNetworkRequest::AlwaysNetwork);
    request.setRawHeader("User-Agent", "Visnip/" VISNIP_VERSION " updater");
    request.setRawHeader("Accept-Encoding", "identity");
    request.setTransferTimeout(60000);
    return request;
}

QByteArray hashFile(const QString& path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) return {};
    QCryptographicHash hash(QCryptographicHash::Sha256);
    if (!hash.addData(&file)) return {};
    return hash.result().toHex();
}

// Unpacked folders are named by version; packages carry it in their file name.
QVersionNumber versionOfEntry(const QString& name)
{
    const auto match = QRegularExpression(QStringLiteral("^(?:Visnip-)?(\\d+\\.\\d+\\.\\d+)")).match(name);
    return match.hasMatch() ? QVersionNumber::fromString(match.captured(1)) : QVersionNumber();
}
} // namespace

UpdateService::UpdateService(QObject* parent)
    : QObject(parent), network_(new QNetworkAccessManager(this))
{
    network_->setProxyFactory(new SystemProxyFactory);
    processTimeout_.setSingleShot(true);
    connect(&processTimeout_, &QTimer::timeout, this, [this]() {
        if (process_) fail(QStringLiteral("准备新版本超时，已停止；当前版本不受影响。"));
    });
}

UpdateService::~UpdateService()
{
    releaseReply();
    stopProcess();
}

QString UpdateService::installProblem(const QString& installDirectory)
{
#ifndef Q_OS_WIN
    Q_UNUSED(installDirectory);
    return QStringLiteral("自动更新只支持 Windows 版。");
#else
    if (AppUpdate::repository().isEmpty()) return QStringLiteral("此版本没有配置更新来源。");
    const QString directory = installDirectory.isEmpty() ? QCoreApplication::applicationDirPath() : installDirectory;
    if (!AppUpdate::isPackagedInstall(directory)) {
        return QStringLiteral("当前不是从发布包运行（缺少 %1），请从 GitHub 下载新版本后手动替换。")
            .arg(AppUpdate::packageListName());
    }
    QFile probe(QDir(directory).filePath(QStringLiteral(".visnip-write-test-%1").arg(QCoreApplication::applicationPid())));
    if (!probe.open(QIODevice::WriteOnly)) {
        return QStringLiteral("安装目录没有写入权限（例如位于 Program Files），请从 GitHub 下载新版本后手动替换。");
    }
    probe.close();
    probe.remove();
    return {};
#endif
}

void UpdateService::setState(State state, const QString& status)
{
    state_ = state;
    status_ = status;
    emit stateChanged(state);
}

void UpdateService::fail(const QString& message)
{
    ++serial_;
    releaseReply();
    stopProcess();
    output_.close();
    Perf::log(QStringLiteral("update.failed %1").arg(message));
    // A failed installation keeps the offer, so a second click resumes it.
    setState(release_.version.isNull() || !AppUpdate::isNewer(release_.version) ? State::Failed : State::Available, message);
}

void UpdateService::releaseReply()
{
    if (!reply_) return;
    QNetworkReply* reply = reply_.data();
    reply_.clear();
    disconnect(reply, nullptr, this, nullptr);
    reply->abort();
    reply->deleteLater();
}

void UpdateService::stopProcess()
{
    processTimeout_.stop();
    if (!process_) return;
    QProcess* process = process_.data();
    process_.clear();
    disconnect(process, nullptr, this, nullptr);
    if (process->state() != QProcess::NotRunning) {
        process->kill();
        process->waitForFinished(3000);
    }
    process->deleteLater();
}

QString UpdateService::packagePath(bool partial) const
{
    const QString path = QDir(AppUpdate::updatesDirectory()).filePath(release_.package.fileName());
    return partial ? path + QStringLiteral(".part") : path;
}

QString UpdateService::stagingPath() const
{
    return QDir(AppUpdate::updatesDirectory()).filePath(release_.version.toString());
}

void UpdateService::check()
{
    if (state_ == State::Checking || busyInstalling() || state_ == State::Restarting) return;
    const QUrl api = AppUpdate::latestReleaseApi();
    if (api.isEmpty()) {
        setState(State::Failed, QStringLiteral("此版本没有配置更新来源。"));
        return;
    }
    // Packages of versions that are already installed are no longer needed;
    // removal fails harmlessly while a just-finished update still runs.
    const QDir updates(AppUpdate::updatesDirectory());
    for (const QFileInfo& entry : updates.entryInfoList(QDir::AllEntries | QDir::NoDotAndDotDot)) {
        const QVersionNumber version = versionOfEntry(entry.fileName());
        if (version.isNull() || AppUpdate::isNewer(version)) continue;
        if (entry.isDir()) QDir(entry.absoluteFilePath()).removeRecursively();
        else QFile::remove(entry.absoluteFilePath());
    }
    releaseReply();
    body_.clear();
    const quint64 generation = ++serial_;
    setState(State::Checking, QStringLiteral("正在向 GitHub 查询新版本…"));
    auto request = requestFor(api);
    request.setRawHeader("Accept", "application/vnd.github+json");
    request.setRawHeader("X-GitHub-Api-Version", "2022-11-28");
    reply_ = network_->get(request);
    connect(reply_, &QNetworkReply::redirected, this, [this](const QUrl& target) {
        if (!reply_) return;
        if (AppUpdate::isAllowedHost(target)) emit reply_->redirectAllowed();
        else { rejected_ = true; reply_->abort(); }
    });
    connect(reply_, &QNetworkReply::readyRead, this, [this]() {
        if (!reply_) return;
        body_ += reply_->readAll();
        if (body_.size() > kMaxApiBytes) { rejected_ = true; reply_->abort(); }
    });
    connect(reply_, &QNetworkReply::finished, this, [this, generation]() {
        if (!reply_ || generation != serial_) return;
        body_ += reply_->readAll();
        const int http = reply_->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        const int network = int(reply_->error());
        const bool rejected = rejected_;
        rejected_ = false;
        releaseReply();
        if (rejected) { fail(QStringLiteral("GitHub 返回了异常响应，已停止检查。")); return; }
        if (http == 404) { fail(QStringLiteral("GitHub 上还没有正式发布的版本。")); return; }
        if (http == 403 || http == 429) { fail(QStringLiteral("GitHub 暂时限制了查询次数，请稍后再试。")); return; }
        if (http != 200) {
            fail(QStringLiteral("无法连接 GitHub 检查更新（网络 %1 / HTTP %2），请检查网络后重试。").arg(network).arg(http));
            return;
        }
        AppRelease release;
        QString error;
        if (!AppUpdate::parseLatestRelease(body_, &release, &error)) { fail(error); return; }
        release_ = release;
        if (AppUpdate::isNewer(release_.version)) {
            setState(State::Available, QStringLiteral("发现新版本 %1（当前 %2）。")
                .arg(release_.version.toString(), QStringLiteral(VISNIP_VERSION)));
        } else {
            setState(State::UpToDate, QStringLiteral("已是最新版本（%1）。").arg(QStringLiteral(VISNIP_VERSION)));
        }
    });
}

void UpdateService::install()
{
    if (state_ != State::Available) return;
    const QString problem = installProblem();
    if (!problem.isEmpty()) { setState(State::Available, problem); return; }
    const QString directory = AppUpdate::updatesDirectory();
    if (!QDir().mkpath(directory)) { fail(QStringLiteral("无法创建更新下载目录。")); return; }
    const QStorageInfo storage(directory);
    if (storage.isValid() && storage.bytesAvailable() < release_.packageSize * 4 + 256LL * 1024 * 1024) {
        fail(QStringLiteral("磁盘空间不足，无法下载新版本。"));
        return;
    }
    ++serial_;
    expectedSha256_ = release_.sha256;
    setState(State::Downloading, QStringLiteral("正在从 GitHub 下载 %1…").arg(release_.version.toString()));
    if (!release_.checksum.isEmpty()) fetchChecksum();
    else downloadPackage();
}

void UpdateService::cancel()
{
    if (!busyInstalling()) return;
    ++serial_;
    releaseReply();
    stopProcess();
    output_.close();
    setState(State::Available, QStringLiteral("已暂停更新；已下载的部分保留，再次点击会继续。当前版本不受影响。"));
}

void UpdateService::fetchChecksum()
{
    body_.clear();
    const quint64 generation = serial_;
    reply_ = network_->get(requestFor(release_.checksum));
    connect(reply_, &QNetworkReply::redirected, this, [this](const QUrl& target) {
        if (!reply_) return;
        if (AppUpdate::isAllowedHost(target)) emit reply_->redirectAllowed();
        else { rejected_ = true; reply_->abort(); }
    });
    connect(reply_, &QNetworkReply::readyRead, this, [this]() {
        if (!reply_) return;
        body_ += reply_->readAll();
        if (body_.size() > kMaxChecksumBytes) { rejected_ = true; reply_->abort(); }
    });
    connect(reply_, &QNetworkReply::finished, this, [this, generation]() {
        if (!reply_ || generation != serial_) return;
        body_ += reply_->readAll();
        const bool ok = reply_->error() == QNetworkReply::NoError && !rejected_;
        rejected_ = false;
        releaseReply();
        const QByteArray published = ok ? AppUpdate::parseChecksumFile(body_) : QByteArray();
        if (published.isEmpty()) { fail(QStringLiteral("无法读取新版本的 SHA-256 校验文件，请稍后重试。")); return; }
        if (!expectedSha256_.isEmpty() && expectedSha256_ != published) {
            fail(QStringLiteral("GitHub 记录的校验值与发布的校验文件不一致，已停止更新。"));
            return;
        }
        expectedSha256_ = published;
        downloadPackage();
    });
}

void UpdateService::downloadPackage()
{
    if (QFileInfo(packagePath()).size() == release_.packageSize) { verifyPackage(); return; }
    output_.setFileName(packagePath(true));
    if (!output_.open(QIODevice::ReadWrite)) { fail(QStringLiteral("无法写入更新下载文件。")); return; }
    if (output_.size() > release_.packageSize && !output_.resize(0)) { fail(QStringLiteral("无法清理异常的下载文件。")); return; }
    offset_ = output_.size();
    output_.seek(offset_);
    headersChecked_ = false;
    rejected_ = false;
    auto request = requestFor(release_.package);
    if (offset_ > 0) request.setRawHeader("Range", "bytes=" + QByteArray::number(offset_) + "-");
    emit progress(offset_, release_.packageSize);
    reply_ = network_->get(request);
    reply_->setReadBufferSize(1024 * 1024);
    connect(reply_, &QNetworkReply::redirected, this, [this](const QUrl& target) {
        if (!reply_) return;
        if (AppUpdate::isAllowedHost(target)) emit reply_->redirectAllowed();
        else { rejected_ = true; reply_->abort(); }
    });
    connect(reply_, &QNetworkReply::readyRead, this, &UpdateService::readPackage);
    connect(reply_, &QNetworkReply::finished, this, &UpdateService::packageFinished);
}

void UpdateService::readPackage()
{
    if (!reply_ || rejected_) return;
    if (!headersChecked_) {
        const int http = reply_->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        if (http == 0) return;
        if (http == 200 && offset_ > 0) {
            // The server ignored the range: start the file again.
            if (!output_.resize(0) || !output_.seek(0)) { fail(QStringLiteral("无法重新开始下载。")); return; }
            offset_ = 0;
        }
        bool validLength = true;
        const QByteArray lengthHeader = reply_->rawHeader("Content-Length");
        const qint64 length = lengthHeader.isEmpty() ? -1 : lengthHeader.toLongLong(&validLength);
        if (!validLength || !OfflineResourceCatalog::acceptsRange(http, reply_->rawHeader("Content-Range"), offset_,
                                                                  release_.packageSize, length, release_.packageSize - 1)) {
            rejected_ = true;
            reply_->abort();
            return;
        }
        headersChecked_ = true;
    }
    while (reply_ && reply_->bytesAvailable() > 0) {
        const QByteArray part = reply_->read(512 * 1024);
        if (output_.pos() + part.size() > release_.packageSize) {
            output_.close();
            QFile::remove(packagePath(true));
            fail(QStringLiteral("下载内容超过发布的文件大小，已删除。"));
            return;
        }
        if (output_.write(part) != part.size()) { fail(QStringLiteral("写入更新文件失败，请检查磁盘空间。")); return; }
    }
    emit progress(output_.pos(), release_.packageSize);
}

void UpdateService::packageFinished()
{
    if (!reply_) return;
    readPackage();
    if (!reply_) return;
    const bool ok = reply_->error() == QNetworkReply::NoError && !rejected_;
    const int http = reply_->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    const int network = int(reply_->error());
    rejected_ = false;
    releaseReply();
    output_.flush();
    const qint64 size = output_.size();
    output_.close();
    if (!ok || size != release_.packageSize) {
        fail(QStringLiteral("下载中断（网络 %1 / HTTP %2），已保留下载进度；再次点击“立即更新”会继续。")
                 .arg(network).arg(http));
        return;
    }
    QFile::remove(packagePath());
    if (!QFile::rename(packagePath(true), packagePath())) { fail(QStringLiteral("无法保存下载的安装包。")); return; }
    verifyPackage();
}

void UpdateService::verifyPackage()
{
    setState(State::Preparing, QStringLiteral("正在核对新版本的 SHA-256…"));
    const QString path = packagePath();
    const quint64 generation = serial_;
    auto* watcher = new QFutureWatcher<QByteArray>(this);
    connect(watcher, &QFutureWatcher<QByteArray>::finished, this, [this, watcher, path, generation]() {
        const QByteArray hash = watcher->result();
        watcher->deleteLater();
        if (generation != serial_ || state_ != State::Preparing) return;
        if (hash.isEmpty() || hash != expectedSha256_) {
            QFile::remove(path);
            fail(QStringLiteral("下载的安装包与发布的 SHA-256 不一致，已删除，没有安装。"));
            return;
        }
        extractPackage();
    });
    watcher->setFuture(QtConcurrent::run([path]() { return hashFile(path); }));
}

void UpdateService::extractPackage()
{
#ifdef Q_OS_WIN
    setState(State::Preparing, QStringLiteral("正在解压新版本…"));
    const QString staging = stagingPath();
    QDir(staging).removeRecursively();
    if (!QDir().mkpath(staging)) { fail(QStringLiteral("无法创建解压目录。")); return; }
    // Windows 10 1803+ ships bsdtar, which reads zip files. It runs inside the
    // folder with a relative archive path, so a non-ASCII user folder never
    // passes through its narrow-character command line.
    const QString tar = QDir(qEnvironmentVariable("SystemRoot", QStringLiteral("C:/Windows")))
                            .filePath(QStringLiteral("System32/tar.exe"));
    if (!QFileInfo(tar).isFile()) { fail(QStringLiteral("系统缺少 tar.exe，无法解压新版本。")); return; }
    auto* process = new QProcess(this);
    process_ = process;
    process->setProgram(tar);
    process->setArguments({QStringLiteral("-x"), QStringLiteral("-f"),
                           QDir::toNativeSeparators(QDir(staging).relativeFilePath(packagePath()))});
    process->setWorkingDirectory(staging);
    process->setStandardInputFile(QProcess::nullDevice());
    process->setStandardOutputFile(QProcess::nullDevice());
    process->setStandardErrorFile(QProcess::nullDevice());
    connect(process, &QProcess::finished, this, &UpdateService::extracted);
    connect(process, &QProcess::errorOccurred, this, [this](QProcess::ProcessError error) {
        if (error == QProcess::FailedToStart) fail(QStringLiteral("无法启动系统解压工具，可能被安全策略阻止。"));
    });
    processTimeout_.start(kProcessTimeoutMs);
    process->start();
#else
    fail(QStringLiteral("自动更新只支持 Windows 版。"));
#endif
}

void UpdateService::extracted(int exitCode, QProcess::ExitStatus status)
{
    stopProcess();
    if (state_ != State::Preparing) return;
    const QString staging = stagingPath();
    if (status != QProcess::NormalExit || exitCode != 0) {
        QDir(staging).removeRecursively();
        fail(QStringLiteral("解压新版本失败（退出码 %1）。").arg(exitCode));
        return;
    }
    QDirIterator entries(staging, QDir::AllEntries | QDir::Hidden | QDir::System | QDir::NoDotAndDotDot,
                         QDirIterator::Subdirectories);
    while (entries.hasNext()) {
        entries.next();
        if (entries.fileInfo().isSymLink()) {
            QDir(staging).removeRecursively();
            fail(QStringLiteral("安装包包含符号链接，已拒绝。"));
            return;
        }
    }
    QString error;
    const QStringList files = AppUpdate::packageFiles(staging, &error);
    if (!error.isEmpty() || !AppUpdate::isPackagedInstall(staging) || !files.contains(QStringLiteral("visnip.exe"))) {
        QDir(staging).removeRecursively();
        fail(error.isEmpty() ? QStringLiteral("安装包不完整（缺少 visnip.exe 或文件清单）。") : error);
        return;
    }
    // The new copy must start on this computer before it replaces the old one.
    setState(State::Preparing, QStringLiteral("正在检查新版本能否在这台电脑上运行…"));
    auto* process = new QProcess(this);
    process_ = process;
    process->setProgram(QDir(staging).filePath(QStringLiteral("visnip.exe")));
    process->setArguments({QStringLiteral("--self-test")});
    process->setWorkingDirectory(staging);
    process->setStandardInputFile(QProcess::nullDevice());
    process->setStandardOutputFile(QProcess::nullDevice());
    process->setStandardErrorFile(QProcess::nullDevice());
    connect(process, &QProcess::finished, this, &UpdateService::selfTested);
    connect(process, &QProcess::errorOccurred, this, [this](QProcess::ProcessError error) {
        if (error == QProcess::FailedToStart) fail(QStringLiteral("新版本无法启动，可能被安全软件拦截；当前版本不受影响。"));
    });
    processTimeout_.start(60000);
    process->start();
}

void UpdateService::selfTested(int exitCode, QProcess::ExitStatus status)
{
    stopProcess();
    if (state_ != State::Preparing) return;
    if (status != QProcess::NormalExit || exitCode != 0) {
        fail(QStringLiteral("新版本自检未通过（退出码 %1），没有安装；当前版本不受影响。").arg(exitCode));
        return;
    }
    // The new copy replaces this installation once this process has exited,
    // then starts it again. This argument contract is shared by all versions.
    const QString staging = stagingPath();
    const QStringList arguments{QStringLiteral("--apply-update"),
                                QStringLiteral("--target"), QDir::toNativeSeparators(QCoreApplication::applicationDirPath()),
                                QStringLiteral("--wait-pid"), QString::number(QCoreApplication::applicationPid())};
    if (!QProcess::startDetached(QDir(staging).filePath(QStringLiteral("visnip.exe")), arguments, staging)) {
        fail(QStringLiteral("无法启动更新程序，当前版本不受影响。"));
        return;
    }
    Perf::log(QStringLiteral("update.handover version=%1").arg(release_.version.toString()));
    setState(State::Restarting, QStringLiteral("新版本已准备好，Visnip 将关闭并自动重新打开…"));
    emit restartRequired();
}

} // namespace Visnip
