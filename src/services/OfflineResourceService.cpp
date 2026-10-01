#include "services/OfflineResourceService.h"
#include "services/LocalTextTranslationService.h"
#include "services/OfflineTranslationService.h"
#include "services/ImageTranslationService.h"
#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QFileInfo>
#include <QFutureWatcher>
#include <QGuiApplication>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QNetworkProxyFactory>
#include <QNetworkProxyQuery>
#include <QPainter>
#include <QSaveFile>
#include <QStorageInfo>
#include <QStringList>
#include <QtConcurrent/QtConcurrentRun>
#include <vector>
#ifdef Q_OS_WIN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace Visnip {
namespace {
constexpr qint64 reserveBytes = 1024LL * 1024 * 1024;
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
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::ManualRedirectPolicy);
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
}

OfflineResourceService::OfflineResourceService(QObject* parent) : QObject(parent),
    network_(new QNetworkAccessManager(this)), test_(new OfflineTranslationService(this)),
    liteTest_(new LocalTextTranslationService(this)), cacheRoot_(cacheDirectory())
{
    network_->setProxyFactory(new ResourceProxyFactory);
    installerPoll_.setInterval(250);
    connect(&installerPoll_, &QTimer::timeout, this, &OfflineResourceService::pollInstaller);
    connect(test_, &OfflineTranslationService::phaseChanged, this, [this](const QString& phase) {
        if (busy_) status(QStringLiteral("离线自检：%1").arg(phase));
    });
    connect(test_, &OfflineTranslationService::succeeded, this, [this](const ImageTranslationResult& result, qint64 ms) {
        if (!busy_) return;
        // This result is ONLY the built-in synthetic fixture, never a user screenshot.
        // Record its per-region reason so a rendering failure is not mistaken for a download failure.
        QSaveFile report(QDir(root_).filePath(QStringLiteral("selftest-report.json")));
        const auto diagnostic = QJsonDocument(QJsonObject{
            {QStringLiteral("client_version"), QStringLiteral(VISNIP_VERSION)},
            {QStringLiteral("platform"), QGuiApplication::platformName()},
            {QStringLiteral("fixture"), QStringLiteral("builtin-en-three-lines-v1")},
            {QStringLiteral("tested_at"), QDateTime::currentSecsSinceEpoch()},
            {QStringLiteral("elapsed_ms"), ms},
            {QStringLiteral("blocks"), result.blocks},
            {QStringLiteral("notice"), result.notice}}).toJson();
        bool diagnosticSaved = false;
        if (report.open(QIODevice::WriteOnly)) {
            if (report.write(diagnostic) == diagnostic.size()) diagnosticSaved = report.commit();
            else report.cancelWriting();
        }
        int applied = 0;
        QStringList failedRegions;
        for (int index = 0; index < result.blocks.size(); ++index) {
            const auto block = result.blocks[index].toObject();
            if (block.value(QStringLiteral("status")).toString() == QStringLiteral("applied")) { ++applied; continue; }
            const QString reason = block.value(QStringLiteral("reason")).toString();
            const QString explanation = reason == QStringLiteral("artwork_collision") ? QStringLiteral("文字残留与图形保护区域冲突")
                : reason == QStringLiteral("layout_unfit") ? QStringLiteral("译文未能排入区域")
                : reason == QStringLiteral("equivalent") ? QStringLiteral("测试文字没有翻译")
                : QStringLiteral("识别、分割或回填不完整");
            failedRegions.append(QStringLiteral("第 %1 段：%2").arg(index+1).arg(explanation));
        }
        if (result.blockCount != 3 || applied != 3 || !result.notice.isEmpty() || result.image.isNull()
            || result.image.convertToFormat(QImage::Format_RGB32) == selfTestImage_) {
            fail(QStringLiteral("资源已下载，但自检未通过（成功 %1/3 段）。%2。未启用，原有配置不变。%3 不需要重复下载相同模型。")
                .arg(applied).arg(failedRegions.isEmpty() ? QStringLiteral("返回区域数量或图片异常") : failedRegions.join(QStringLiteral("；")))
                .arg(diagnosticSaved ? QStringLiteral("诊断已记录在资源目录的 selftest-report.json。") : QStringLiteral("诊断报告无法写入资源目录。"))); return;
        }
        recordActivation(ms);
    });
    connect(test_, &OfflineTranslationService::failed, this, [this](const QString& message) {
        if (busy_) fail(QStringLiteral("安装后离线自检未通过：%1 原有资源和翻译配置未被替换。").arg(message));
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
    QSaveFile receipt(QDir(root_).filePath(QStringLiteral("vislate-managed.json")));
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
QString OfflineResourceService::cacheDirectory()
{
    return QDir::cleanPath(QDir(OfflineTranslationService::defaultResourceDirectory()).absoluteFilePath(QStringLiteral("../downloads")));
}
void OfflineResourceService::status(const QString& text) { status_ = text; emit statusChanged(text); }
void OfflineResourceService::releaseReply()
{
    if (!reply_) return;
    QNetworkReply* reply = reply_.data(); reply_.clear();
    disconnect(reply, nullptr, this, nullptr); reply->abort(); reply->deleteLater();
}
void OfflineResourceService::finish()
{
    const bool wasBusy = busy_; busy_ = false; awaitingApproval_ = false; ++serial_;
    releaseReply(); output_.close(); stopInstaller(); test_->cancel(); liteTest_->cancel(); lock_.reset();
    if (wasBusy) emit busyChanged(false);
}
void OfflineResourceService::fail(const QString& message) { status(message); finish(); emit failed(message); }
void OfflineResourceService::cancel()
{
    if (!busy_) return;
    status(QStringLiteral("已暂停资源准备；已下载的部分保留，下次点击可继续。没有启用未完成的资源。"));
    finish(); emit cancelled();
}
void OfflineResourceService::prepare(const QString& quality)
{
    if (busy_) return;
    if (quality != QStringLiteral("lite") && quality != QStringLiteral("precise")) {
        emit failed(QStringLiteral("不支持的离线资源类型。")); return;
    }
    if (!plainDirectory(cacheRoot_)) { emit failed(QStringLiteral("无法创建安全的资源缓存目录。")); return; }
    lock_ = std::make_unique<QLockFile>(QDir(cacheRoot_).filePath(QStringLiteral("install.lock")));
    lock_->setStaleLockTime(0);
    if (!lock_->tryLock()) { lock_.reset(); emit failed(QStringLiteral("另一个 Visnip 正在准备离线资源，请稍后重试。")); return; }
    quality_ = quality; manifestBody_.clear(); plan_.clear(); index_ = 0; retries_ = 0; completedBytes_ = 0;
    busy_ = true; ++serial_; emit busyChanged(true);
    status(QStringLiteral("正在获取并验证资源发布清单…")); emit phaseChanged(QStringLiteral("manifest"));
    reply_ = network_->get(requestFor(QUrl(QStringLiteral("https://vislate.ipxair.com/offline/client-manifest.json"))));
    reply_->setReadBufferSize(32769);
    connect(reply_, &QNetworkReply::readyRead, this, [this]() {
        if (!reply_) return;
        manifestBody_ += reply_->readAll();
        if (manifestBody_.size() > 32768) fail(QStringLiteral("资源清单大小异常，已停止。"));
    });
    connect(reply_, &QNetworkReply::finished, this, [this]() {
        if (!reply_) return;
        manifestBody_ += reply_->readAll();
        const bool ok = reply_->error() == QNetworkReply::NoError
            && reply_->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt() == 200;
        releaseReply();
        if (!ok) { fail(QStringLiteral("无法获取资源清单，请检查网络。未启动下载或安装。")); return; }
        QString error;
        if (!OfflineResourceManifest::parse(manifestBody_, &manifest_, &error)) { fail(error); return; }
        preparePlan();
    });
}
void OfflineResourceService::preparePlan()
{
    clientAdapter_.clear();
    clientModules_.clear();
    if (quality_ == QStringLiteral("lite")) {
        // The lite tier runs llama.cpp directly from the client; only the base
        // package (translation model and server) is needed, no engine code.
        plan_.clear();
        for (const auto& package : manifest_.packages) {
            if (package.id == QStringLiteral("base")) plan_.append(package);
        }
        if (plan_.size() != 1) { fail(QStringLiteral("资源清单缺少轻量离线所需的基础包，未开始下载。")); return; }
    } else {
        QFile adapter(QStringLiteral(":/visnip/offline/native_translation.py"));
        if (!adapter.open(QIODevice::ReadOnly)) { fail(QStringLiteral("客户端离线适配器缺失，请更新客户端。")); return; }
        clientAdapter_ = adapter.readAll();
        if (clientAdapter_.isEmpty()) { fail(QStringLiteral("客户端离线适配器为空，未开始安装。")); return; }
        for (const QString& name : {QStringLiteral("native_translation"),QStringLiteral("runtime"),QStringLiteral("adapters"),
                QStringLiteral("layout"),QStringLiteral("appearance"),QStringLiteral("text_tiles"),QStringLiteral("desktop_runner"),QStringLiteral("session_worker"),
                QStringLiteral("adaptive_vision"),QStringLiteral("translation_policy"),QStringLiteral("gpu_runtime"),QStringLiteral("onnx_ocr"),
                QStringLiteral("ui_structure"),QStringLiteral("gpu_attention"),QStringLiteral("full_gpu_vision"),QStringLiteral("device_profile"),QStringLiteral("failure"),QStringLiteral("unified_elements"),QStringLiteral("unified_composition")}) {
            QFile file(QStringLiteral(":/visnip/offline/%1.py").arg(name));
            if (!file.open(QIODevice::ReadOnly)) { fail(QStringLiteral("客户端精细引擎代码不完整，请重新安装客户端。")); return; }
            clientModules_[name] = file.readAll();
        }
        plan_ = manifest_.packages;
    }
    QByteArray identity;
    qint64 installedBytes = 0, remaining = 0; totalBytes_ = 0;
    for (const auto& package : plan_) {
        identity += package.sha256; installedBytes += package.installedBytes; totalBytes_ += package.size;
        const QFileInfo complete(QDir(cacheRoot_).filePath(QString::fromLatin1(package.sha256) + QStringLiteral(".exe")));
        const QFileInfo partial(complete.absoluteFilePath() + QStringLiteral(".part"));
        const qint64 cached = !complete.isSymLink() && complete.size() == package.size ? package.size
            : !partial.isSymLink() ? qBound<qint64>(0LL, partial.size(), package.size) : 0;
        remaining += package.size - cached;
    }
    // Application adapter fixes travel with this binary; unchanged GiB model
    // packages stay reusable. Include the adapter in the immutable directory ID.
    if (!clientAdapter_.isEmpty()) identity += QCryptographicHash::hash(clientAdapter_, QCryptographicHash::Sha256).toHex();
    for (auto it=clientModules_.cbegin();it!=clientModules_.cend();++it)
        identity += it.key().toUtf8() + QCryptographicHash::hash(it.value(),QCryptographicHash::Sha256).toHex();
    root_ = QDir::cleanPath(QDir(cacheRoot_).absoluteFilePath(QStringLiteral("../managed/%1-%2").arg(quality_,
        QString::fromLatin1(QCryptographicHash::hash(identity, QCryptographicHash::Sha256).toHex().left(20)))));
    if (!plainDirectory(QFileInfo(root_).absolutePath())) { fail(QStringLiteral("无法创建资源安装位置。")); return; }
    reuse_ = QFileInfo::exists(QDir(root_).filePath(QStringLiteral("vislate-managed.json")))
        && OfflineTranslationService::resourceProblem(root_, quality_).isEmpty();
    const qint64 required = reuse_ ? reserveBytes : remaining + installedBytes + reserveBytes;
    const QStorageInfo storage(cacheRoot_);
    if (!storage.isValid() || storage.bytesAvailable() < required) {
        fail(QStringLiteral("资源安装空间不足，至少需要 %1 GiB 可用空间；不会删除原有资源。")
            .arg(required / double(1024LL * 1024 * 1024), 0, 'f', 1)); return;
    }
    awaitingApproval_ = true;
    status(QStringLiteral("资源签名已验证，等待确认下载和自动启用。"));
    emit approvalRequired(reuse_ ? 0 : remaining, required);
}
void OfflineResourceService::installApproved()
{
    if (!busy_ || !awaitingApproval_) return;
    awaitingApproval_ = false;
    if (manifest_.expiresAt <= QDateTime::currentSecsSinceEpoch()) { fail(QStringLiteral("资源清单已过期，请重新获取。")); return; }
    if (!plainDirectory(root_)) { fail(QStringLiteral("无法创建资源目录。")); return; }
    if (reuse_) selfTest(); else nextPackage();
}
QString OfflineResourceService::packagePath(bool partial) const
{
    return QDir(cacheRoot_).filePath(QString::fromLatin1(plan_[index_].sha256)
        + (partial ? QStringLiteral(".exe.part") : QStringLiteral(".exe")));
}
void OfflineResourceService::nextPackage()
{
    if (!busy_) return;
    if (index_ >= plan_.size()) { selfTest(); return; }
    for (const bool partial : {false, true}) {
        QFileInfo info(packagePath(partial));
        if (info.isSymLink()) { fail(QStringLiteral("资源缓存不允许符号链接。")); return; }
    }
    if (QFileInfo(packagePath()).size() == plan_[index_].size) checkCached(true);
    else if (QFileInfo(packagePath(true)).size() == plan_[index_].size) checkCached(false);
    else requestPackage();
}
void OfflineResourceService::checkCached(bool completeFile)
{
    status(QStringLiteral("正在校验文件 %1/%2 的 SHA-256…").arg(index_+1).arg(plan_.size())); emit phaseChanged(QStringLiteral("verify"));
    const QString path = packagePath(!completeFile);
    const QByteArray expected = plan_[index_].sha256;
    const quint64 generation = serial_;
    auto* watcher = new QFutureWatcher<QByteArray>(this);
    connect(watcher, &QFutureWatcher<QByteArray>::finished, this, [this, watcher, generation, path, expected, completeFile]() {
        const QByteArray hash = watcher->result(); watcher->deleteLater();
        if (!busy_ || generation != serial_) return;
        if (hash != expected) {
            QFile::remove(path);
            // A corrupt complete cache was included as zero download bytes in
            // the consent dialog. Reconfirm the full transfer instead of quietly
            // downloading a GiB after the user approved only cache reuse.
            fail(QStringLiteral("资源校验失败，损坏的下载文件已移除。请重新点击“下载并启用”确认下载量，不会执行此文件。"));
            return;
        }
        if (!completeFile) {
            if (QFileInfo::exists(packagePath()) && !QFile::remove(packagePath())) { fail(QStringLiteral("无法替换无效缓存。")); return; }
            if (!QFile::rename(path, packagePath())) { fail(QStringLiteral("无法保存校验通过的资源包。")); return; }
        }
        installPackage();
    });
    watcher->setFuture(QtConcurrent::run([path]() { return hashFile(path); }));
}
void OfflineResourceService::requestPackage()
{
    if (!busy_) return;
    const auto& package = plan_[index_];
    output_.setFileName(packagePath(true));
    if (!output_.open(QIODevice::ReadWrite)) { fail(QStringLiteral("无法写入资源下载文件。")); return; }
    if (output_.size() > package.size && !output_.resize(0)) { fail(QStringLiteral("无法清理异常下载文件。")); return; }
    offset_ = output_.size(); output_.seek(offset_); headersChecked_ = false;
    QStorageInfo storage(cacheRoot_);
    if (storage.bytesAvailable() < package.size - offset_ + reserveBytes) { fail(QStringLiteral("剩余磁盘空间不足，下载已暂停。")); return; }
    auto request = requestFor(package.url);
    // Small verified ranges avoid long first-byte waits on large-file proxies.
    requestedEnd_ = qMin(offset_ + 4LL * 1024 * 1024, package.size) - 1;
    request.setRawHeader("Range", "bytes=" + QByteArray::number(offset_) + "-" + QByteArray::number(requestedEnd_));
    status(QStringLiteral("正在下载资源文件 %1/%2（支持暂停和续传）…").arg(index_+1).arg(plan_.size()));
    emit phaseChanged(QStringLiteral("download"));
    emit progress(completedBytes_ + offset_, totalBytes_);
    reply_ = network_->get(request); reply_->setReadBufferSize(1024 * 1024);
    connect(reply_, &QNetworkReply::readyRead, this, &OfflineResourceService::readPackage);
    connect(reply_, &QNetworkReply::finished, this, &OfflineResourceService::packageFinished);
}
void OfflineResourceService::readPackage()
{
    if (!reply_ || !busy_) return;
    if (!headersChecked_) {
        const int http = reply_->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        if (http == 0) return;
        if (http == 429 || http == 502 || http == 503 || http == 504) {
            reply_->readAll(); return; // Retry without saving a server error page into the package.
        }
        // Do not append an error page to the partial package. Let packageFinished
        // retry transient HTTP failures instead of rejecting them as corrupt ranges.
        if (http == 429 || http == 502 || http == 503 || http == 504) {
            reply_->readAll();
            return;
        }
        if (http == 200) {
            if (offset_ > 0 && (!output_.resize(0) || !output_.seek(0))) { fail(QStringLiteral("无法重置不支持续传的下载。")); return; }
            offset_ = 0;
            requestedEnd_ = plan_[index_].size - 1;
        }
        bool validLength = true;
        const auto lengthHeader = reply_->rawHeader("Content-Length");
        const qint64 length = lengthHeader.isEmpty() ? -1 : lengthHeader.toLongLong(&validLength);
        if (!validLength || !OfflineResourceManifest::acceptsRange(http, reply_->rawHeader("Content-Range"), offset_, plan_[index_].size, length, requestedEnd_)
            || (!reply_->rawHeader("Content-Encoding").isEmpty() && reply_->rawHeader("Content-Encoding") != "identity")) {
            fail(QStringLiteral("下载服务器返回异常状态或错误的续传范围；原有资源不受影响。")); return;
        }
        headersChecked_ = true;
    }
    while (reply_ && reply_->bytesAvailable() > 0) {
        const QByteArray part = reply_->read(512 * 1024);
        if (output_.pos() + part.size() > requestedEnd_ + 1) {
            output_.close(); QFile::remove(packagePath(true)); fail(QStringLiteral("下载内容超过签名清单中的大小，已拒绝。")); return;
        }
        if (output_.write(part) != part.size()) { fail(QStringLiteral("写入资源失败，请检查磁盘空间。")); return; }
    }
    emit progress(completedBytes_ + output_.pos(), totalBytes_);
}
void OfflineResourceService::packageFinished()
{
    if (!reply_) return;
    readPackage(); if (!reply_) return;
    const int networkCode = int(reply_->error());
    const int http = reply_->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    const bool ok = reply_->error() == QNetworkReply::NoError;
    releaseReply(); output_.flush(); const qint64 size = output_.size(); output_.close();
    if (ok && http == 206 && size == requestedEnd_ + 1 && size < plan_[index_].size) {
        retries_ = 0;
        const quint64 generation = serial_;
        // Keep a single fast client below the API's 120-request/minute limit.
        QTimer::singleShot(650, this, [this, generation]() { if (generation == serial_) requestPackage(); }); return;
    }
    if (!ok || size != plan_[index_].size) {
        if ((networkCode == int(QNetworkReply::OperationCanceledError)
             || networkCode == int(QNetworkReply::TimeoutError)
             || networkCode == int(QNetworkReply::TemporaryNetworkFailureError)
             || http == 429 || http == 502 || http == 503 || http == 504) && retries_ < 3) {
            ++retries_;
            network_->clearConnectionCache();
            status(QStringLiteral("网络暂时中断，正在从已下载位置重试（%1/3）…").arg(retries_));
            const quint64 generation = serial_;
            QTimer::singleShot(2000 * retries_, this, [this, generation]() {
                if (generation == serial_ && busy_) {
                    if (QFileInfo(packagePath(true)).size() == plan_[index_].size) checkCached(false);
                    else requestPackage();
                }
            });
            return;
        }
        fail(QStringLiteral("下载中断（网络 %1 / HTTP %2），已保留下载进度。点击重试会继续，不会启用不完整资源。")
             .arg(networkCode).arg(http)); return;
    }
    checkCached(false);
}
void OfflineResourceService::installPackage()
{
#ifdef Q_OS_WIN
    if (!busy_) return;
    status(QStringLiteral("正在安装资源 %1/%2，请稍候…").arg(index_+1).arg(plan_.size())); emit phaseChanged(QStringLiteral("install"));
    // The resource installers are per-user. Never use runas or disable Windows security.
    // NSIS /D must be the final unquoted argument, including when the path has spaces.
    const QString commandLine = QStringLiteral("\"%1\" /S /D=%2")
        .arg(QDir::toNativeSeparators(packagePath()), QDir::toNativeSeparators(root_));
    if (root_.contains(QLatin1Char('"')) || root_.contains(QLatin1Char('\n'))) { fail(QStringLiteral("无效安装路径。")); return; }
    HANDLE job = CreateJobObjectW(nullptr, nullptr);
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
    limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    if (!job || !SetInformationJobObject(job, JobObjectExtendedLimitInformation, &limits, sizeof(limits))) {
        if (job) CloseHandle(job);
        fail(QStringLiteral("无法创建资源安装的隔离进程组。")); return;
    }
    std::wstring native = commandLine.toStdWString();
    // A GUI app may have invalid/inherited console handles. The precision
    // installer invokes Python; give its stdin EOF and discard output rather
    // than allowing the helper to block on the launcher console or a full pipe.
    SECURITY_ATTRIBUTES security{sizeof(SECURITY_ATTRIBUTES), nullptr, TRUE};
    HANDLE nullStream = CreateFileW(L"NUL", GENERIC_READ | GENERIC_WRITE,
        FILE_SHARE_READ | FILE_SHARE_WRITE, &security, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (nullStream == INVALID_HANDLE_VALUE) {
        CloseHandle(job); fail(QStringLiteral("无法创建资源安装的受控输入输出。")); return;
    }
    SIZE_T attributeSize = 0;
    InitializeProcThreadAttributeList(nullptr, 1, 0, &attributeSize);
    std::vector<BYTE> attributeBuffer(attributeSize);
    STARTUPINFOEXW startup{}; startup.StartupInfo.cb = sizeof(startup);
    startup.lpAttributeList = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(attributeBuffer.data());
    if (!InitializeProcThreadAttributeList(startup.lpAttributeList, 1, 0, &attributeSize)) {
        CloseHandle(nullStream); CloseHandle(job);
        fail(QStringLiteral("无法隔离资源安装的进程句柄。")); return;
    }
    startup.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
    startup.StartupInfo.hStdInput = nullStream;
    startup.StartupInfo.hStdOutput = nullStream;
    startup.StartupInfo.hStdError = nullStream;
    const bool handlesReady = UpdateProcThreadAttribute(startup.lpAttributeList, 0,
        PROC_THREAD_ATTRIBUTE_HANDLE_LIST, &nullStream, sizeof(nullStream), nullptr, nullptr);
    PROCESS_INFORMATION process{};
    const bool started = handlesReady && CreateProcessW(reinterpret_cast<LPCWSTR>(packagePath().utf16()),
        native.data(), nullptr, nullptr, TRUE, CREATE_SUSPENDED | CREATE_NO_WINDOW | EXTENDED_STARTUPINFO_PRESENT,
        nullptr, nullptr, &startup.StartupInfo, &process);
    DeleteProcThreadAttributeList(startup.lpAttributeList); CloseHandle(nullStream);
    if (!started) {
        CloseHandle(job); fail(QStringLiteral("无法启动已校验的资源安装程序，可能被系统策略阻止。未自动提权或绕过保护。")); return;
    }
    if (!AssignProcessToJobObject(job, process.hProcess)) {
        TerminateProcess(process.hProcess, 1); CloseHandle(process.hThread); CloseHandle(process.hProcess); CloseHandle(job);
        fail(QStringLiteral("无法安全管理资源安装进程，已取消安装。")); return;
    }
    process_ = process.hProcess; job_ = job; ResumeThread(process.hThread); CloseHandle(process.hThread);
    installStarted_ = QDateTime::currentMSecsSinceEpoch(); installerPoll_.start();
#else
    fail(QStringLiteral("自动安装仅支持 Windows x64。"));
#endif
}
void OfflineResourceService::pollInstaller()
{
#ifdef Q_OS_WIN
    if (!busy_ || !process_) return;
    if (QDateTime::currentMSecsSinceEpoch() - installStarted_ > 20LL * 60 * 1000) {
        fail(QStringLiteral("资源安装超时，已停止，未启用此资源。")); return;
    }
    if (WaitForSingleObject(process_, 0) != WAIT_OBJECT_0) return;
    DWORD code = 1; GetExitCodeProcess(process_, &code); stopInstaller();
    if (code != 0) { fail(QStringLiteral("资源安装失败（退出码 %1），请检查磁盘或安全策略后重试。").arg(code)); return; }
    if (!stageClientCode()) return;
    const QDir installed(root_);
    const QString required = plan_[index_].id == QStringLiteral("base") ? QStringLiteral("basic.json") : QStringLiteral("precise.json");
    const bool complete = quality_ == QStringLiteral("lite")
        ? OfflineTranslationService::resourceProblem(root_, quality_).isEmpty()
        : QFileInfo(installed.filePath(required)).isFile() && QFileInfo(installed.filePath(QStringLiteral("python/python.exe"))).isFile();
    if (!complete) {
        fail(QStringLiteral("资源安装不完整，未启用。")); return;
    }
    completedBytes_ += plan_[index_].size; ++index_; retries_ = 0; nextPackage();
#endif
}
void OfflineResourceService::stopInstaller()
{
    installerPoll_.stop();
#ifdef Q_OS_WIN
    if (job_) { CloseHandle(job_); job_ = nullptr; }
    if (process_) { WaitForSingleObject(process_, 3000); CloseHandle(process_); process_ = nullptr; }
#endif
}
bool OfflineResourceService::stageClientCode()
{
    for (auto it=clientModules_.cbegin();it!=clientModules_.cend();++it) {
        const QString path=QDir(root_).filePath(QStringLiteral("vislate_engine/%1.py").arg(it.key()));
        if (!plainDirectory(QFileInfo(path).absolutePath()) || QFileInfo(path).isSymLink()) {
            fail(QStringLiteral("离线引擎目录无效，未启用。")); return false;
        }
        if (reuse_) {
            if (hashFile(path)!=QCryptographicHash::hash(it.value(),QCryptographicHash::Sha256).toHex()) {
                fail(QStringLiteral("已有引擎代码发生变化，请重新准备资源。"));return false;
            }
        } else {
            QSaveFile file(path);
            if (!file.open(QIODevice::WriteOnly) || file.write(it.value())!=it.value().size() || !file.commit()) {
                fail(QStringLiteral("无法写入客户端精细引擎，未启用。")); return false;
            }
        }
    }
    return true;
}
void OfflineResourceService::selfTest()
{
    if (quality_ == QStringLiteral("lite")) {
        status(QStringLiteral("安装完成，正在本机执行轻量翻译自检；通过后自动启用…"));
        emit phaseChanged(QStringLiteral("selftest"));
        LocalTextTranslationService::releaseSharedEngine(); // test exactly the new directory
        liteTest_->translate(LocalTextTranslationService::selfTestTexts(), QStringLiteral("zh-Hans"), root_);
        return;
    }
    if (!stageClientCode()) return;
    status(QStringLiteral("安装完成，正在本机执行精细翻译自检；通过后自动启用…"));
    emit phaseChanged(QStringLiteral("selftest"));
    QImage sample(800, 260, QImage::Format_RGB32); sample.fill(Qt::white);
    QPainter painter(&sample); QFont font(QStringLiteral("Arial")); font.setPixelSize(26);
    painter.setFont(font); painter.setPen(Qt::black);
    painter.drawText(40, 65, QStringLiteral("Project settings"));
    painter.drawText(40, 125, QStringLiteral("Keep 12 files in the local folder."));
    painter.drawText(40, 190, QStringLiteral("Save changes")); painter.end();
    selfTestImage_ = sample;
    test_->translate(sample, QStringLiteral("zh-Hans"), root_, quality_);
}
}
