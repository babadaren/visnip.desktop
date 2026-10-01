#include "services/OcrPackDownloadService.h"

#include "core/OcrLanguagePack.h"
#include "core/PerfLog.h"

#include <QDir>
#include <QFileInfo>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QUuid>

namespace Visnip {

OcrPackDownloadService::OcrPackDownloadService(QObject* parent)
    : QObject(parent)
    , nam_(new QNetworkAccessManager(this))
{
}

void OcrPackDownloadService::install(const QString& packId)
{
    if (isBusy()) {
        return;
    }
    const Ocr::LanguagePack* pack = Ocr::languagePack(packId);
    if (!pack || pack->legacy) {
        emit failed(packId, QStringLiteral("未知或不可下载的 OCR 语言包。"));
        return;
    }
    packId_ = pack->id;
    userCancelled_ = false;
    completedBytes_ = 0;
    temporaryDirectory_ = Ocr::userPackDirectory(packId_)
        + QStringLiteral(".download-")
        + QUuid::createUuid().toString(QUuid::Id128);
    if (!QDir().mkpath(temporaryDirectory_)) {
        fail(QStringLiteral("无法创建 OCR 下载目录。"));
        return;
    }
    Perf::log(QStringLiteral("OcrPack.download.begin pack=%1").arg(packId_));
    beginPart(Part::Model);
}

bool OcrPackDownloadService::remove(const QString& packId, QString* error)
{
    if (isBusy() && packId == packId_) {
        if (error) {
            *error = QStringLiteral("语言包正在下载。请先取消下载。");
        }
        return false;
    }
    const Ocr::LanguagePack* pack = Ocr::languagePack(packId);
    if (!pack || pack->legacy || pack->bundled) {
        if (error) {
            *error = QStringLiteral("内置语言包不能删除。");
        }
        return false;
    }
    QDir directory(Ocr::userPackDirectory(packId));
    if (!directory.exists()) {
        return true;
    }
    if (!directory.removeRecursively()) {
        if (error) {
            *error = QStringLiteral("无法删除语言包目录：%1").arg(directory.absolutePath());
        }
        return false;
    }
    Perf::log(QStringLiteral("OcrPack.remove pack=%1").arg(packId));
    return true;
}

void OcrPackDownloadService::cancel()
{
    if (!reply_) {
        return;
    }
    userCancelled_ = true;
    reply_->abort();
}

void OcrPackDownloadService::beginPart(Part part)
{
    const Ocr::LanguagePack* pack = Ocr::languagePack(packId_);
    if (!pack) {
        fail(QStringLiteral("OCR 语言包信息已失效。"));
        return;
    }
    part_ = part;
    urls_ = part == Part::Model ? pack->modelUrls() : pack->dictionaryUrls();
    urlIndex_ = 0;
    tryNextUrl();
}

void OcrPackDownloadService::tryNextUrl()
{
    const Ocr::LanguagePack* pack = Ocr::languagePack(packId_);
    if (!pack || urlIndex_ >= urls_.size()) {
        fail(QStringLiteral("下载 OCR %1失败，请检查网络后重试。")
                 .arg(part_ == Part::Model ? QStringLiteral("模型")
                                           : QStringLiteral("字典")));
        return;
    }
    const QString outputName = part_ == Part::Model
        ? QStringLiteral("model.onnx")
        : pack->dictionaryFile;
    output_.setFileName(QDir(temporaryDirectory_).filePath(outputName));
    if (!output_.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        fail(QStringLiteral("无法写入 OCR 下载文件。"));
        return;
    }

    QNetworkRequest request(QUrl(urls_[urlIndex_++]));
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                         QNetworkRequest::NoLessSafeRedirectPolicy);
    request.setTransferTimeout(120000);
    reply_ = nam_->get(request);
    connect(reply_, &QNetworkReply::readyRead, this, [this]() {
        if (reply_ && output_.write(reply_->readAll()) < 0) {
            reply_->abort();
        }
    });
    connect(reply_, &QNetworkReply::downloadProgress, this,
            [this](qint64 received, qint64 total) {
        const Ocr::LanguagePack* currentPack = Ocr::languagePack(packId_);
        if (!currentPack) {
            return;
        }
        const qint64 current = completedBytes_ + received;
        const qint64 combined = total > 0
            ? completedBytes_ + total
            : currentPack->modelBytes;
        emit progress(packId_, current, combined);
    });
    connect(reply_, &QNetworkReply::finished, this,
            &OcrPackDownloadService::handleFinished);
}

void OcrPackDownloadService::handleFinished()
{
    QNetworkReply* reply = reply_.data();
    if (!reply) {
        return;
    }
    output_.write(reply->readAll());
    output_.close();
    const bool ok = reply->error() == QNetworkReply::NoError;
    reply->deleteLater();
    reply_.clear();

    if (userCancelled_) {
        const QString cancelledPack = packId_;
        QDir(temporaryDirectory_).removeRecursively();
        reset();
        emit cancelled(cancelledPack);
        return;
    }
    if (!ok) {
        QFile::remove(output_.fileName());
        tryNextUrl();
        return;
    }

    const Ocr::LanguagePack* pack = Ocr::languagePack(packId_);
    QString error;
    const bool valid = part_ == Part::Model
        ? Ocr::verifyFile(output_.fileName(), pack->modelBytes,
                          pack->modelSha256, &error)
        : Ocr::verifyFile(output_.fileName(), 0,
                          pack->dictionarySha256, &error);
    if (!valid) {
        QFile::remove(output_.fileName());
        if (urlIndex_ < urls_.size()) {
            tryNextUrl();
        } else {
            fail(error);
        }
        return;
    }

    if (part_ == Part::Model) {
        completedBytes_ = pack->modelBytes;
        beginPart(Part::Dictionary);
    } else {
        finishInstall();
    }
}

void OcrPackDownloadService::finishInstall()
{
    QString error;
    const Ocr::LanguagePack* pack = Ocr::languagePack(packId_);
    if (!pack
        || !Ocr::verifyFile(QDir(temporaryDirectory_).filePath(QStringLiteral("model.onnx")),
                            pack->modelBytes, pack->modelSha256, &error)
        || !Ocr::verifyFile(QDir(temporaryDirectory_).filePath(pack->dictionaryFile),
                            0, pack->dictionarySha256, &error)) {
        fail(error.isEmpty() ? QStringLiteral("OCR 语言包校验失败。") : error);
        return;
    }

    const QString finalDirectory = Ocr::userPackDirectory(packId_);
    QDir finalDir(finalDirectory);
    if (finalDir.exists() && !finalDir.removeRecursively()) {
        fail(QStringLiteral("无法替换已有 OCR 语言包。"));
        return;
    }
    QDir parent(QFileInfo(finalDirectory).absolutePath());
    if (!parent.mkpath(QStringLiteral("."))
        || !parent.rename(temporaryDirectory_, finalDirectory)) {
        fail(QStringLiteral("无法完成 OCR 语言包安装。"));
        return;
    }
    const QString installedPack = packId_;
    Perf::log(QStringLiteral("OcrPack.download.succeeded pack=%1").arg(installedPack));
    reset();
    emit succeeded(installedPack);
}

void OcrPackDownloadService::fail(const QString& message)
{
    const QString failedPack = packId_;
    if (reply_) {
        reply_->deleteLater();
        reply_.clear();
    }
    output_.close();
    if (!temporaryDirectory_.isEmpty()) {
        QDir(temporaryDirectory_).removeRecursively();
    }
    Perf::log(QStringLiteral("OcrPack.download.failed pack=%1 message=\"%2\"")
                  .arg(failedPack, message));
    reset();
    emit failed(failedPack, message);
}

void OcrPackDownloadService::reset()
{
    output_.setFileName(QString());
    packId_.clear();
    temporaryDirectory_.clear();
    urls_.clear();
    urlIndex_ = 0;
    completedBytes_ = 0;
    userCancelled_ = false;
}

} // namespace Visnip
