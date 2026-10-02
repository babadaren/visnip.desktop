#include "services/OfflineResourceService.h"
#include "services/LocalTextTranslationService.h"
#include "services/OfflineTranslationService.h"
#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QDirIterator>
#include <QFileInfo>
#include <QFutureWatcher>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QNetworkProxyFactory>
#include <QNetworkProxyQuery>
#include <QSaveFile>
#include <QStorageInfo>
#include <QThread>
#include <QtConcurrent/QtConcurrentRun>

namespace Visnip {
namespace {
constexpr qint64 reserveBytes = 1024LL * 1024 * 1024;
constexpr int kMaxRetriesPerSource = 3;
constexpr int kExtractTimeoutMs = 3 * 60 * 1000;
const QString kReceipt = QStringLiteral("vislate-managed.json");
class ResourceProxyFactory final : public QNetworkProxyFactory {
public:
    QList<QNetworkProxy> queryProxy(const QNetworkProxyQuery& query) override {
        return QNetworkProxyFactory::systemProxyForQuery(query);
    }
};
QByteArray hashFile(const QString& path)
{
    QFile file(path); if (!file.open(QIODevice::ReadOnly)) return {};
    QCryptographicHash hash(QCryptographicHash::Sha256);
    while (!file.atEnd()) {
        QByteArray part = file.read(1024 * 1024);
        if (part.isEmpty() && file.error() != QFileDevice::NoError) return {};
        hash.addData(part);
    }
    return hash.result().toHex();
}
QNetworkRequest requestFor(const QUrl& url)
{
    QNetworkRequest request(url);
    // Each redirect is checked against the catalog's hosts before it is followed.
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::UserVerifiedRedirectPolicy);
    request.setMaximumRedirectsAllowed(5);
    request.setAttribute(QNetworkRequest::Http2AllowedAttribute, false);
    request.setAttribute(QNetworkRequest::CookieLoadControlAttribute, QNetworkRequest::Manual);
    request.setAttribute(QNetworkRequest::CookieSaveControlAttribute, QNetworkRequest::Manual);
    request.setAttribute(QNetworkRequest::AuthenticationReuseAttribute, QNetworkRequest::Manual);
    request.setAttribute(QNetworkRequest::CacheLoadControlAttribute, QNetworkRequest::AlwaysNetwork);
    request.setRawHeader("User-Agent", "Visnip/" VISNIP_VERSION " resource-manager");
    request.setRawHeader("Accept-Encoding", "identity");
    request.setTransferTimeout(90000);
    return request;
}
bool plainDirectory(const QString& directory)
{
    QFileInfo cursor(QDir(directory).absolutePath());
    for (;;) {
        if (cursor.isSymLink()) return false;
        const QString parent = cursor.absolutePath();
        if (parent == cursor.absoluteFilePath()) break;
        cursor.setFile(parent);
    }
    return QDir().mkpath(directory);
}
bool transientNetworkError(int code)
{
    return code == int(QNetworkReply::OperationCanceledError) // transfer timeout
        || code == int(QNetworkReply::TimeoutError)
        || code == int(QNetworkReply::TemporaryNetworkFailureError)
        || code == int(QNetworkReply::RemoteHostClosedError)
        || code == int(QNetworkReply::NetworkSessionFailedError)
        || code == int(QNetworkReply::ProxyTimeoutError)
        || code == int(QNetworkReply::UnknownNetworkError);
}
bool transientHttp(int http)
{
    return http == 429 || http == 500 || http == 502 || http == 503 || http == 504;
}
}

OfflineResourceService::OfflineResourceService(QObject* parent) : QObject(parent),
    network_(new QNetworkAccessManager(this)), liteTest_(new LocalTextTranslationService(this)),
    cacheRoot_(cacheDirectory())
{
    network_->setProxyFactory(new ResourceProxyFactory);
    extractTimeout_.setSingleShot(true);
    connect(&extractTimeout_, &QTimer::timeout, this, [this]() {
        if (busy_ && extractor_) fail(QStringLiteral("解压 llama.cpp 超时，已停止，没有启用。"));
    });
    connect(liteTest_, &LocalTextTranslationService::phaseChanged, this, [this](const QString& phase) {
        if (busy_) status(QStringLiteral("离线自检：%1").arg(phase));
    });
    connect(liteTest_, &LocalTextTranslationService::succeeded, this, [this](const QStringList& translations, qint64 ms) {
        if (!busy_) return;
        // Only the built-in fixture is translated here, never a user screenshot.
        const QString problem = liteTest_->selfTestProblem(translations);
        if (!problem.isEmpty()) {
            fail(QStringLiteral("资源已下载，但轻量翻译自检未通过：%1。未启用，原有配置不变；不需要重复下载相同模型。").arg(problem));
            return;
        }
        recordActivation(ms);
    });
    connect(liteTest_, &LocalTextTranslationService::failed, this, [this](const QString& message) {
        if (busy_) fail(QStringLiteral("安装后离线自检未通过：%1 原有资源和翻译配置未被替换。").arg(message));
    });
}
void OfflineResourceService::recordActivation(qint64 ms)
{
    QSaveFile receipt(QDir(root_).filePath(kReceipt));
    const QByteArray json = QJsonDocument(QJsonObject{{QStringLiteral("quality"), quality_},
        {QStringLiteral("abi"), 1}, {QStringLiteral("tested_at"), QDateTime::currentSecsSinceEpoch()}}).toJson();
    if (!receipt.open(QIODevice::WriteOnly) || receipt.write(json) != json.size() || !receipt.commit()) {
        fail(QStringLiteral("资源自检通过，但无法保存启用记录。")); return;
    }
    const QString directory = root_, quality = quality_;
    status(QStringLiteral("离线资源下载、校验和真实翻译自检通过，已可启用。"));
    finish(); emit succeeded(directory, quality, ms);
}
OfflineResourceService::~OfflineResourceService() { finish(); }
QString OfflineResourceService::defaultStorageDirectory()
{
    return QDir::cleanPath(QDir(OfflineTranslationService::defaultResourceDirectory()).absoluteFilePath(QStringLiteral("..")));
}
QString OfflineResourceService::storageDirectory(const QString& configured)
{
    const QString trimmed = configured.trimmed();
    return trimmed.isEmpty() ? defaultStorageDirectory() : QDir::cleanPath(trimmed);
}
QString OfflineResourceService::cacheDirectory(const QString& configured)
{
    return QDir(storageDirectory(configured)).absoluteFilePath(QStringLiteral("downloads"));
}
bool OfflineResourceService::removeInstalled(const QString& root, const QString& storageRoot, QString* error)
{
    const auto reject = [error](const QString& message) {
        if (error) *error = message;
        return false;
    };
    const QString directory = QDir::cleanPath(root);
    if (directory.isEmpty()) return reject(QStringLiteral("没有已下载的离线资源。"));
    // An offline engine that is still shutting down keeps its executables open,
    // so give it a moment instead of failing the whole deletion.
    QString stuck;
    for (int attempt = 0; attempt < 4; ++attempt) {
        if (attempt) QThread::msleep(400);
        stuck.clear();
        for (const auto& file : OfflineResourceCatalog::liteFiles()) {
            const QString path = QDir(directory).filePath(file.target);
            const QFileInfo info(path);
            if (!info.exists() && !info.isSymLink()) continue;
            const bool removed = info.isDir() ? QDir(path).removeRecursively() : QFile::remove(path);
            if (!removed) { stuck = file.target; break; }
        }
        if (stuck.isEmpty()) break;
    }
    if (!stuck.isEmpty()) return reject(QStringLiteral("无法删除 %1：离线翻译进程仍在占用它，请退出翻译后重试。").arg(stuck));
    QFile::remove(QDir(directory).filePath(kReceipt));
    QDir(directory).rmdir(QStringLiteral("models")); // only succeeds when empty
    QDir().rmdir(directory);
    // The archives are only useful for reinstalling the exact same files.
    // The archives live in the configured storage root, not next to the install.
    QDir cache(cacheDirectory(storageRoot));
    if (cache.exists()) {
        const auto entries = cache.entryInfoList(QDir::Files | QDir::Hidden | QDir::System | QDir::NoDotAndDotDot);
        for (const auto& entry : entries) {
            if (entry.fileName() != QStringLiteral("install.lock")) {
                QFile::remove(entry.absoluteFilePath());
            }
        }
    }
    if (error) error->clear();
    return true;
}
void OfflineResourceService::status(const QString& text) { status_ = text; emit statusChanged(text); }
void OfflineResourceService::reject(const QString& message) { status(message); emit failed(message); }
void OfflineResourceService::releaseReply()
{
    if (!reply_) return;
    QNetworkReply* reply = reply_.data(); reply_.clear();
    disconnect(reply, nullptr, this, nullptr); reply->abort(); reply->deleteLater();
}
void OfflineResourceService::stopExtractor()
{
    extractTimeout_.stop();
    if (!extractor_) return;
    QProcess* process = extractor_.data(); extractor_.clear();
    disconnect(process, nullptr, this, nullptr);
    if (process->state() != QProcess::NotRunning) { process->kill(); process->waitForFinished(3000); }
    process->deleteLater();
}
void OfflineResourceService::finish()
{
    const bool wasBusy = busy_; busy_ = false; awaitingApproval_ = false; ++serial_;
    releaseReply(); output_.close(); stopExtractor(); liteTest_->cancel(); lock_.reset();
    if (wasBusy) emit busyChanged(false);
}
void OfflineResourceService::fail(const QString& message) { status(message); finish(); emit failed(message); }
void OfflineResourceService::cancel()
{
    if (!busy_) return;
    status(QStringLiteral("已暂停资源准备；已下载的部分保留，下次点击可继续。没有启用未完成的资源。"));
    finish(); emit cancelled();
}
void OfflineResourceService::prepare(const QString& quality, const QString& storageDirectory)
{
    if (busy_) return;
    if (quality != QStringLiteral("lite")) {
        reject(QStringLiteral("精细档的资源没有官方发布渠道，无法自动下载；本版本只提供轻量离线翻译的下载。"));
        return;
    }
    // Checked first so nobody downloads 1 GiB that cannot run on this computer.
    const QString runtime = LocalTextTranslationService::runtimeProblem();
    if (!runtime.isEmpty()) { reject(runtime); return; }
    cacheRoot_ = OfflineResourceService::cacheDirectory(storageDirectory);
    if (!plainDirectory(cacheRoot_)) { reject(QStringLiteral("无法创建安全的资源缓存目录。")); return; }
    lock_ = std::make_unique<QLockFile>(QDir(cacheRoot_).filePath(QStringLiteral("install.lock")));
    lock_->setStaleLockTime(0);
    if (!lock_->tryLock()) { lock_.reset(); reject(QStringLiteral("另一个 Visnip 正在准备离线资源，请稍后重试。")); return; }
    quality_ = quality; plan_ = OfflineResourceCatalog::liteFiles();
    index_ = 0; source_ = 0; retries_ = 0; completedBytes_ = 0;
    busy_ = true; ++serial_; emit busyChanged(true);
    status(QStringLiteral("正在核对官方资源和磁盘空间…"));
    const quint64 generation = serial_;
    QTimer::singleShot(0, this, [this, generation]() { if (generation == serial_ && busy_) preparePlan(); });
}
void OfflineResourceService::preparePlan()
{
    QByteArray identity;
    qint64 installedBytes = 0; totalBytes_ = 0;
    for (const auto& file : plan_) {
        identity += file.sha256; installedBytes += file.installedBytes; totalBytes_ += file.size;
    }
    root_ = QDir::cleanPath(QDir(cacheRoot_).absoluteFilePath(QStringLiteral("../managed/%1-%2").arg(quality_,
        QString::fromLatin1(QCryptographicHash::hash(identity, QCryptographicHash::Sha256).toHex().left(20)))));
    if (!plainDirectory(QFileInfo(root_).absolutePath())) { fail(QStringLiteral("无法创建资源安装位置。")); return; }
    qint64 remaining = 0;
    for (index_ = 0; index_ < plan_.size(); ++index_) {
        const qint64 size = plan_[index_].size;
        const QFileInfo complete(filePath()), partial(filePath(true));
        const qint64 cached = !complete.isSymLink() && complete.size() == size ? size
            : !partial.isSymLink() ? qBound<qint64>(0LL, partial.size(), size) : 0;
        remaining += size - cached;
    }
    index_ = 0;
    reuse_ = QFileInfo::exists(QDir(root_).filePath(kReceipt))
        && OfflineTranslationService::resourceProblem(root_, quality_).isEmpty();
    const qint64 required = reuse_ ? reserveBytes : remaining + installedBytes + reserveBytes;
    const QStorageInfo storage(cacheRoot_);
    if (!storage.isValid() || storage.bytesAvailable() < required) {
        fail(QStringLiteral("资源安装空间不足，至少需要 %1 GiB 可用空间；不会删除原有资源。")
            .arg(required / double(1024LL * 1024 * 1024), 0, 'f', 1)); return;
    }
    awaitingApproval_ = true;
    status(QStringLiteral("等待确认从官方渠道下载并自动启用。"));
    emit approvalRequired(reuse_ ? 0 : remaining, required);
}
void OfflineResourceService::installApproved()
{
    if (!busy_ || !awaitingApproval_) return;
    awaitingApproval_ = false;
    if (!plainDirectory(root_)) { fail(QStringLiteral("无法创建资源目录。")); return; }
    if (reuse_) selfTest(); else nextFile();
}
QString OfflineResourceService::filePath(bool partial) const
{
    const auto& file = plan_[index_];
    // Archives are cached; the model is downloaded straight to where it runs.
    const QString path = file.install == OfflineResourceFile::Install::ExtractZip
        ? QDir(cacheRoot_).filePath(QString::fromLatin1(file.sha256) + QStringLiteral(".zip"))
        : QDir(root_).filePath(file.target);
    return partial ? path + QStringLiteral(".part") : path;
}
QString OfflineResourceService::sourceName() const
{
    const QString host = plan_[index_].sources.value(source_).host();
    if (host.endsWith(QStringLiteral("modelscope.cn"))) return QStringLiteral("魔搭社区（ModelScope）");
    if (host.endsWith(QStringLiteral("huggingface.co"))) return QStringLiteral("Hugging Face");
    if (host.endsWith(QStringLiteral("github.com"))) return QStringLiteral("GitHub");
    return host;
}
void OfflineResourceService::nextFile()
{
    if (!busy_) return;
    if (index_ >= plan_.size()) { selfTest(); return; }
    source_ = 0; retries_ = 0;
    if (!plainDirectory(QFileInfo(filePath()).absolutePath())) { fail(QStringLiteral("无法创建资源目录。")); return; }
    for (const bool partial : {false, true}) {
        if (QFileInfo(filePath(partial)).isSymLink()) { fail(QStringLiteral("资源目录不允许符号链接。")); return; }
    }
    if (QFileInfo(filePath()).size() == plan_[index_].size) checkCached(true);
    else if (QFileInfo(filePath(true)).size() == plan_[index_].size) checkCached(false);
    else requestFile();
}
void OfflineResourceService::checkCached(bool completeFile)
{
    status(QStringLiteral("正在校验 %1 的 SHA-256（%2/%3）…").arg(plan_[index_].label).arg(index_+1).arg(plan_.size()));
    emit phaseChanged(QStringLiteral("verify"));
    const QString path = completeFile ? filePath() : filePath(true);
    const QByteArray expected = plan_[index_].sha256;
    const quint64 generation = serial_;
    auto* watcher = new QFutureWatcher<QByteArray>(this);
    connect(watcher, &QFutureWatcher<QByteArray>::finished, this, [this, watcher, generation, path, expected, completeFile]() {
        const QByteArray hash = watcher->result(); watcher->deleteLater();
        if (!busy_ || generation != serial_) return;
        if (hash != expected) {
            QFile::remove(path);
            // A corrupt cache counted as zero download bytes in the consent
            // dialog; reconfirm the full transfer instead of starting it here.
            fail(QStringLiteral("%1 的 SHA-256 校验失败，与官方公布的不一致；该文件已删除，没有使用。请重新点击“下载并启用”。")
                .arg(plan_[index_].label));
            return;
        }
        if (!completeFile) {
            if (QFileInfo::exists(filePath()) && !QFile::remove(filePath())) { fail(QStringLiteral("无法替换无效缓存。")); return; }
            if (!QFile::rename(path, filePath())) { fail(QStringLiteral("无法保存校验通过的资源文件。")); return; }
        }
        installFile();
    });
    watcher->setFuture(QtConcurrent::run([path]() { return hashFile(path); }));
}
void OfflineResourceService::requestFile()
{
    if (!busy_) return;
    const auto& file = plan_[index_];
    output_.setFileName(filePath(true));
    if (!output_.open(QIODevice::ReadWrite)) { fail(QStringLiteral("无法写入资源下载文件。")); return; }
    if (output_.size() > file.size && !output_.resize(0)) { fail(QStringLiteral("无法清理异常下载文件。")); return; }
    offset_ = output_.size(); output_.seek(offset_); headersChecked_ = false;
    redirectRejected_ = false; badResponse_ = false;
    QStorageInfo storage(cacheRoot_);
    if (storage.bytesAvailable() < file.size - offset_ + reserveBytes) { fail(QStringLiteral("剩余磁盘空间不足，下载已暂停。")); return; }
    auto request = requestFor(file.sources[source_]);
    // One open-ended range per attempt; an interruption resumes from the file size.
    requestedEnd_ = file.size - 1;
    if (offset_ > 0) request.setRawHeader("Range", "bytes=" + QByteArray::number(offset_) + "-");
    status(QStringLiteral("正在从 %1 下载 %2（%3/%4，支持暂停和续传）…")
        .arg(sourceName(), file.label).arg(index_+1).arg(plan_.size()));
    emit phaseChanged(QStringLiteral("download"));
    emit progress(completedBytes_ + offset_, totalBytes_);
    reply_ = network_->get(request); reply_->setReadBufferSize(1024 * 1024);
    connect(reply_, &QNetworkReply::redirected, this, [this](const QUrl& target) {
        if (!reply_) return;
        if (OfflineResourceCatalog::isAllowedDownload(target)) { emit reply_->redirectAllowed(); return; }
        redirectRejected_ = true; reply_->abort();
    });
    connect(reply_, &QNetworkReply::readyRead, this, &OfflineResourceService::readFile);
    connect(reply_, &QNetworkReply::finished, this, &OfflineResourceService::fileFinished);
}
void OfflineResourceService::readFile()
{
    if (!reply_ || !busy_ || redirectRejected_ || badResponse_) return;
    const qint64 size = plan_[index_].size;
    if (!headersChecked_) {
        const int http = reply_->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        if (http == 0) return;
        if (transientHttp(http)) {
            reply_->readAll(); return; // Retry without saving a server error page into the file.
        }
        if (http == 200 && offset_ > 0) {
            // This source ignored the range; start the file again from its beginning.
            if (!output_.resize(0) || !output_.seek(0)) { fail(QStringLiteral("无法重置不支持续传的下载。")); return; }
            offset_ = 0;
        }
        bool validLength = true;
        const auto lengthHeader = reply_->rawHeader("Content-Length");
        const qint64 length = lengthHeader.isEmpty() ? -1 : lengthHeader.toLongLong(&validLength);
        if (!validLength || !OfflineResourceCatalog::acceptsRange(http, reply_->rawHeader("Content-Range"), offset_, size, length, requestedEnd_)
            || (!reply_->rawHeader("Content-Encoding").isEmpty() && reply_->rawHeader("Content-Encoding") != "identity")) {
            badResponse_ = true; reply_->abort(); return;
        }
        headersChecked_ = true;
    }
    while (reply_ && reply_->bytesAvailable() > 0) {
        const QByteArray part = reply_->read(512 * 1024);
        if (output_.pos() + part.size() > size) {
            output_.close(); QFile::remove(filePath(true)); fail(QStringLiteral("下载内容超过官方文件的大小，已拒绝。")); return;
        }
        if (output_.write(part) != part.size()) { fail(QStringLiteral("写入资源失败，请检查磁盘空间。")); return; }
    }
    emit progress(completedBytes_ + output_.pos(), totalBytes_);
}
void OfflineResourceService::fileFinished()
{
    if (!reply_) return;
    readFile(); if (!reply_) return;
    const int networkCode = int(reply_->error());
    const int http = reply_->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    const bool ok = reply_->error() == QNetworkReply::NoError;
    const bool rejected = redirectRejected_, bad = badResponse_;
    releaseReply(); output_.flush(); const qint64 size = output_.size(); output_.close();
    if (ok && !rejected && !bad && size == plan_[index_].size) { checkCached(false); return; }
    if (rejected) { switchSource(QStringLiteral("下载地址跳转到了未知网站，已拒绝")); return; }
    if (bad) { switchSource(QStringLiteral("下载源返回了异常响应（HTTP %1）").arg(http)); return; }
    // A source that made progress and then dropped is retried; one that cannot
    // even start (blocked, refused, no answer) is skipped for the next source.
    const bool madeProgress = size > offset_;
    const bool transient = transientHttp(http) || (ok && size < plan_[index_].size)
        || (madeProgress && transientNetworkError(networkCode));
    retryOrSwitch(transient, QStringLiteral("下载中断（网络 %1 / HTTP %2）").arg(networkCode).arg(http));
}
void OfflineResourceService::retryOrSwitch(bool transient, const QString& reason)
{
    if (!transient || retries_ >= kMaxRetriesPerSource) { switchSource(reason); return; }
    ++retries_;
    network_->clearConnectionCache();
    status(QStringLiteral("网络暂时中断，正在从已下载位置重试（%1/%2）…").arg(retries_).arg(kMaxRetriesPerSource));
    const quint64 generation = serial_;
    QTimer::singleShot(2000 * retries_, this, [this, generation]() {
        if (generation != serial_ || !busy_) return;
        if (QFileInfo(filePath(true)).size() == plan_[index_].size) checkCached(false);
        else requestFile();
    });
}
void OfflineResourceService::switchSource(const QString& reason)
{
    const auto& file = plan_[index_];
    if (source_ + 1 >= file.sources.size()) {
        fail(QStringLiteral("%1。%2 的官方下载源都无法完成下载，已保留下载进度；检查网络后再点击“下载并启用”会继续。没有启用不完整的资源。")
            .arg(reason, file.label));
        return;
    }
    ++source_; retries_ = 0;
    network_->clearConnectionCache();
    // Every source serves identical bytes, so the partial file is kept.
    status(QStringLiteral("%1，改用 %2 继续下载（已下载部分保留）…").arg(reason, sourceName()));
    const quint64 generation = serial_;
    QTimer::singleShot(500, this, [this, generation]() { if (generation == serial_ && busy_) requestFile(); });
}
void OfflineResourceService::installFile()
{
    if (!busy_) return;
    const auto& file = plan_[index_];
    if (file.install == OfflineResourceFile::Install::Place) {
        completedBytes_ += file.size; ++index_; nextFile(); return;
    }
#ifdef Q_OS_WIN
    status(QStringLiteral("正在解压 %1（%2/%3）…").arg(file.label).arg(index_+1).arg(plan_.size()));
    emit phaseChanged(QStringLiteral("install"));
    const QString staging = QDir(root_).filePath(file.target + QStringLiteral(".extract"));
    QDir(staging).removeRecursively();
    if (!plainDirectory(staging)) { fail(QStringLiteral("无法创建解压目录。")); return; }
    // Windows 10 1803+ ships bsdtar, which reads zip files. It runs inside the
    // staging folder with a relative archive path, so a non-ASCII user folder
    // never has to pass through its narrow-character command line.
    const QString tar = QDir(qEnvironmentVariable("SystemRoot", QStringLiteral("C:/Windows")))
        .filePath(QStringLiteral("System32/tar.exe"));
    if (!QFileInfo(tar).isFile()) {
        fail(QStringLiteral("系统缺少 tar.exe（Windows 10 1803 及以上自带），无法解压 llama.cpp。")); return;
    }
    auto* process = new QProcess(this);
    extractor_ = process;
    process->setProgram(tar);
    process->setArguments({QStringLiteral("-x"), QStringLiteral("-f"),
                           QDir::toNativeSeparators(QDir(staging).relativeFilePath(filePath()))});
    process->setWorkingDirectory(staging);
    process->setStandardInputFile(QProcess::nullDevice());
    process->setStandardOutputFile(QProcess::nullDevice());
    process->setStandardErrorFile(QProcess::nullDevice());
    connect(process, &QProcess::finished, this, &OfflineResourceService::extractionFinished);
    connect(process, &QProcess::errorOccurred, this, [this](QProcess::ProcessError error) {
        if (error == QProcess::FailedToStart && busy_) fail(QStringLiteral("无法启动系统解压工具，可能被安全策略阻止。"));
    });
    extractTimeout_.start(kExtractTimeoutMs);
    process->start();
#else
    fail(QStringLiteral("自动安装仅支持 Windows x64。"));
#endif
}
void OfflineResourceService::extractionFinished(int exitCode, QProcess::ExitStatus exitStatus)
{
    stopExtractor();
    if (!busy_) return;
    const auto& file = plan_[index_];
    const QString staging = QDir(root_).filePath(file.target + QStringLiteral(".extract"));
    if (exitStatus != QProcess::NormalExit || exitCode != 0) {
        QDir(staging).removeRecursively();
        fail(QStringLiteral("解压 %1 失败（退出码 %2），没有启用。").arg(file.label).arg(exitCode)); return;
    }
    QDirIterator entries(staging, QDir::AllEntries | QDir::Hidden | QDir::System | QDir::NoDotAndDotDot,
                         QDirIterator::Subdirectories);
    while (entries.hasNext()) {
        entries.next();
        if (entries.fileInfo().isSymLink()) {
            QDir(staging).removeRecursively();
            fail(QStringLiteral("解压结果包含符号链接，已拒绝。")); return;
        }
    }
    if (!QFileInfo(QDir(staging).filePath(QStringLiteral("llama-server.exe"))).isFile()) {
        QDir(staging).removeRecursively();
        fail(QStringLiteral("解压结果缺少 llama-server.exe，没有启用。")); return;
    }
    const QString target = QDir(root_).filePath(file.target);
    if (QFileInfo::exists(target) && !QDir(target).removeRecursively()) { fail(QStringLiteral("无法替换旧的 llama.cpp 文件。")); return; }
    if (!QDir().rename(staging, target)) { fail(QStringLiteral("无法放置解压后的 llama.cpp。")); return; }
    completedBytes_ += file.size; ++index_; nextFile();
}
void OfflineResourceService::selfTest()
{
    status(QStringLiteral("安装完成，正在本机执行轻量翻译自检；通过后自动启用…"));
    emit phaseChanged(QStringLiteral("selftest"));
    LocalTextTranslationService::releaseSharedEngine(); // test exactly the new directory
    liteTest_->translate(LocalTextTranslationService::selfTestTexts(), QStringLiteral("zh-Hans"), root_);
}
}
