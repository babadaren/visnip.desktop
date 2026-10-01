#include "services/ImageTranslationService.h"
#include "services/OfflineTranslationService.h"

#include "core/PerfLog.h"
#include "core/TranslationLanguage.h"

#include <QBuffer>
#include <QFutureWatcher>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QJsonValue>
#include <QNetworkAccessManager>
#include <QNetworkProxy>
#include <QNetworkProxyFactory>
#include <QNetworkProxyQuery>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QPainter>
#include <QSslConfiguration>
#include <QSslSocket>
#include <QUrl>
#include <QtConcurrentRun>

#include <cmath>
#include <limits>
#include <utility>

namespace Visnip {

namespace {

constexpr qreal kScaleStep = 0.85;
constexpr int kJpegQualities[] = { 92, 85, 78, 70, 62 };

bool isUsableEndpoint(const QUrl& url)
{
    const QString scheme = url.scheme().toLower();
    return url.isValid()
        && !url.host().isEmpty()
        && (scheme == QStringLiteral("http") || scheme == QStringLiteral("https"));
}

QByteArray encodeImage(const QImage& image, const char* format, int quality = -1)
{
    QByteArray bytes;
    QBuffer buffer(&bytes);
    if (!buffer.open(QIODevice::WriteOnly)
        || !image.save(&buffer, format, quality)) {
        return {};
    }
    return bytes;
}

QImage opaqueImage(const QImage& source)
{
    QImage result(source.size(), QImage::Format_RGB32);
    result.fill(Qt::white);
    QPainter painter(&result);
    painter.drawImage(QPoint(), source);
    return result;
}

QString detailFromBody(const QByteArray& body)
{
    const QJsonDocument document = QJsonDocument::fromJson(body);
    if (!document.isObject()) {
        return {};
    }
    const QJsonValue detail = document.object().value(QStringLiteral("detail"));
    QString message = detail.toString().trimmed();
    if (message.isEmpty() && detail.isArray() && !detail.toArray().isEmpty()) {
        message = detail.toArray().first().toObject()
                      .value(QStringLiteral("msg"))
                      .toString()
                      .trimmed();
    }
    if (message.size() > 200) {
        message = message.left(200) + QStringLiteral("...");
    }
    return message;
}

bool isCanonicalBase64(const QByteArray& encoded)
{
    if (encoded.isEmpty() || encoded.size() % 4 != 0) {
        return false;
    }
    int padding = 0;
    if (encoded.endsWith("==")) {
        padding = 2;
    } else if (encoded.endsWith('=')) {
        padding = 1;
    }
    const qsizetype contentSize = encoded.size() - padding;
    for (qsizetype index = 0; index < contentSize; ++index) {
        const char ch = encoded.at(index);
        const bool valid = (ch >= 'A' && ch <= 'Z')
            || (ch >= 'a' && ch <= 'z')
            || (ch >= '0' && ch <= '9')
            || ch == '+'
            || ch == '/';
        if (!valid) {
            return false;
        }
    }
    for (qsizetype index = contentSize; index < encoded.size(); ++index) {
        if (encoded.at(index) != '=') {
            return false;
        }
    }
    return true;
}

} // namespace

ImageTranslationService::ImageTranslationService(AppConfig* config, QObject* parent)
    : QObject(parent)
    , config_(config)
    , nam_(new QNetworkAccessManager(this))
{
    if (config_) {
        connect(config_, &AppConfig::changed, this,
                [this, route = config_->settings().aiTranslate.translationMethodCacheKey()]() mutable {
            const QString next = config_->settings().aiTranslate.translationMethodCacheKey();
            if (next != route) {
                route = next;
                cancel();
                preconnectedEndpointKey_.clear();
                nam_->clearConnectionCache();
            }
        });
    }
}

bool ImageTranslationService::isBusy() const
{
    return !reply_.isNull() || (offline_ && offline_->isBusy());
}

void ImageTranslationService::preconnectConfiguredEndpoint()
{
    if (!config_ || isBusy()) {
        return;
    }
    if (!onlineTranslationEnabled()
        || config_->settings().aiTranslate.translationMethod != TranslationMethod::CloudImage
        || !config_->settings().aiTranslate.uploadConsented()) {
        // No connection to the service before the user has accepted uploads.
        preconnectedEndpointKey_.clear();
        return;
    }

    const QUrl endpoint(config_->settings().aiTranslate.fastImageTranslateEndpoint());
    if (!isUsableEndpoint(endpoint)) {
        return;
    }

    const bool encrypted = endpoint.scheme().compare(
        QStringLiteral("https"), Qt::CaseInsensitive) == 0;
    const quint16 port = static_cast<quint16>(
        endpoint.port(encrypted ? 443 : 80));
    const QString endpointKey = QStringLiteral("%1://%2:%3")
                                    .arg(endpoint.scheme().toLower(),
                                         endpoint.host().toLower())
                                    .arg(port);
    if (endpointKey == preconnectedEndpointKey_) {
        return;
    }

    if (!networkStackWarmed_) {
        if (networkStackWarmupPending_) {
            return;
        }
        networkStackWarmupPending_ = true;
        // The first use of the network stack resolves the system proxy (WPAD)
        // and loads the TLS backend plus the CA store — measured as ~1.4s of
        // GUI-thread blocking inside connectToHostEncrypted(), which queued
        // any hotkey pressed in that window. Pay that one-time cost on a
        // worker thread, then finish the actual preconnect back here.
        auto* watcher = new QFutureWatcher<void>(this);
        connect(watcher, &QFutureWatcher<void>::finished, this,
                [this, watcher]() {
            watcher->deleteLater();
            networkStackWarmupPending_ = false;
            networkStackWarmed_ = true;
            preconnectConfiguredEndpoint();
        });
        watcher->setFuture(QtConcurrent::run([endpoint]() {
            Perf::ScopedTimer timer(QStringLiteral("ImageTranslate.networkStackWarmup"));
#ifndef QT_NO_SSL
            QSslSocket::supportsSsl();
            const auto caCertificates =
                QSslConfiguration::defaultConfiguration().caCertificates();
            Q_UNUSED(caCertificates);
#endif
            const QNetworkProxyQuery proxyQuery(endpoint);
            const auto proxies =
                QNetworkProxyFactory::systemProxyForQuery(proxyQuery);
            Q_UNUSED(proxies);
        }));
        return;
    }

    preconnectedEndpointKey_ = endpointKey;
    if (encrypted) {
#ifndef QT_NO_SSL
        nam_->connectToHostEncrypted(endpoint.host(), port);
#else
        preconnectedEndpointKey_.clear();
        return;
#endif
    } else {
        nam_->connectToHost(endpoint.host(), port);
    }
    Perf::log(QStringLiteral("ImageTranslate.preconnectQueued endpoint=%1")
                  .arg(endpointKey));
}

QByteArray ImageTranslationService::buildRequestPayload(const QImage& image,
                                                        const QString& targetLanguage,
                                                        QString* error,
                                                        QSize* uploadedImageSize)
{
    const auto fail = [error](const QString& message) {
        if (error) {
            *error = message;
        }
        return QByteArray();
    };
    if (error) {
        error->clear();
    }
    if (uploadedImageSize) {
        *uploadedImageSize = {};
    }
    if (image.isNull() || !image.size().isValid()) {
        return fail(QStringLiteral("待翻译图片无效。"));
    }
    const QString normalizedTarget = targetLanguage.trimmed();
    if (normalizedTarget.isEmpty()) {
        return fail(QStringLiteral("目标语言不能为空。"));
    }
    if (!isSupportedTranslationLanguage(normalizedTarget)) {
        return fail(QStringLiteral("云端图片翻译暂不支持目标语言：%1。")
                        .arg(normalizedTarget));
    }

    QImage candidate = image;

    QByteArray encoded = encodeImage(candidate, "PNG");
    if (encoded.isEmpty()) {
        return fail(QStringLiteral("无法编码待翻译截图。"));
    }
    if (encoded.size() > kMaxUploadImageBytes) {
        while (encoded.isEmpty() || encoded.size() > kMaxUploadImageBytes) {
            const QImage jpegSource = opaqueImage(candidate);
            for (const int quality : kJpegQualities) {
                encoded = encodeImage(jpegSource, "JPEG", quality);
                if (!encoded.isEmpty() && encoded.size() <= kMaxUploadImageBytes) {
                    break;
                }
            }
            if (!encoded.isEmpty() && encoded.size() <= kMaxUploadImageBytes) {
                break;
            }
            const QSize nextSize(qMax(1, qRound(candidate.width() * kScaleStep)),
                                 qMax(1, qRound(candidate.height() * kScaleStep)));
            if (nextSize == candidate.size()) {
                break;
            }
            QImage scaled = candidate.scaled(nextSize, Qt::IgnoreAspectRatio,
                                             Qt::SmoothTransformation);
            if (scaled.isNull()) {
                return fail(QStringLiteral("压缩待翻译截图时内存不足。"));
            }
            candidate = std::move(scaled);
        }
    }
    if (encoded.isEmpty() || encoded.size() > kMaxUploadImageBytes) {
        return fail(QStringLiteral("无法在 %1 MB 上传上限内编码截图。")
                        .arg(kMaxUploadImageBytes / (1024 * 1024)));
    }

    QJsonObject request;
    request.insert(QStringLiteral("image_base64"),
                   QString::fromLatin1(encoded.toBase64()));
    request.insert(QStringLiteral("source_lang"), QStringLiteral("auto"));
    request.insert(QStringLiteral("target_lang"), normalizedTarget);
    if (uploadedImageSize) {
        *uploadedImageSize = candidate.size();
    }
    return QJsonDocument(request).toJson(QJsonDocument::Compact);
}

ImageTranslationResult ImageTranslationService::parseResponse(
    const QByteArray& body,
    const QSize& expectedImageSize,
    QString* error)
{
    const auto fail = [error](const QString& message) {
        if (error) {
            *error = message;
        }
        return ImageTranslationResult();
    };
    if (error) {
        error->clear();
    }
    if (!expectedImageSize.isValid()) {
        return fail(QStringLiteral("无法校验译图尺寸：原截图尺寸无效。"));
    }
    if (body.isEmpty()) {
        return fail(QStringLiteral("图片翻译服务返回了空响应。"));
    }
    if (body.size() > kMaxResponseBytes) {
        return fail(QStringLiteral("图片翻译响应超过 %1 MB 上限。")
                        .arg(kMaxResponseBytes / (1024 * 1024)));
    }

    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(body, &parseError);
    if (parseError.error != QJsonParseError::NoError) {
        return fail(QStringLiteral("图片翻译服务返回的 JSON 无效：%1")
                        .arg(parseError.errorString()));
    }
    if (!document.isObject()) {
        return fail(QStringLiteral("图片翻译响应的 JSON 根节点必须是对象。"));
    }
    const QJsonObject object = document.object();
    const QJsonValue provider = object.value(QStringLiteral("provider"));
    const QJsonValue sourceLanguage = object.value(QStringLiteral("source_lang"));
    const QJsonValue targetLanguage = object.value(QStringLiteral("target_lang"));
    const QJsonValue blocks = object.value(QStringLiteral("blocks"));
    const QJsonValue elapsed = object.value(QStringLiteral("elapsed_ms"));
    const QJsonValue mimeType = object.value(QStringLiteral("translated_image_mime_type"));
    const QJsonValue encodedImage = object.value(QStringLiteral("translated_image_base64"));
    if (!provider.isString() || provider.toString().trimmed().isEmpty()) {
        return fail(QStringLiteral("图片翻译响应缺少 provider。"));
    }
    // The configured endpoint owns provider selection. A self-hosted pipeline
    // must identify itself honestly rather than impersonating Baidu.
    if (!sourceLanguage.isString()) {
        return fail(QStringLiteral("图片翻译响应缺少 source_lang。"));
    }
    if (!targetLanguage.isString() || targetLanguage.toString().trimmed().isEmpty()) {
        return fail(QStringLiteral("图片翻译响应缺少 target_lang。"));
    }
    if (!blocks.isArray()) {
        return fail(QStringLiteral("图片翻译响应缺少 blocks 数组。"));
    }
    const double serverElapsedMs = elapsed.toDouble(-1.0);
    if (!elapsed.isDouble() || !std::isfinite(serverElapsedMs)
        || serverElapsedMs < 0.0
        || std::floor(serverElapsedMs) != serverElapsedMs
        || serverElapsedMs > static_cast<double>(std::numeric_limits<qint64>::max())) {
        return fail(QStringLiteral("图片翻译响应的 elapsed_ms 无效。"));
    }
    if (!mimeType.isString() || !encodedImage.isString()) {
        return fail(QStringLiteral("图片翻译响应缺少译图数据。"));
    }

    const QString mime = mimeType.toString().trimmed().toLower();
    QByteArray imageFormat;
    if (mime == QStringLiteral("image/jpeg")) {
        imageFormat = QByteArrayLiteral("JPEG");
    } else if (mime == QStringLiteral("image/png")) {
        imageFormat = QByteArrayLiteral("PNG");
    } else {
        return fail(QStringLiteral("图片翻译服务返回了不支持的图片类型：%1").arg(mime));
    }

    const QByteArray encoded = encodedImage.toString().toLatin1();
    constexpr qint64 maxEncodedImageChars =
        ((kMaxTranslatedImageBytes + 2) / 3) * 4;
    if (encoded.size() > maxEncodedImageChars || !isCanonicalBase64(encoded)) {
        return fail(QStringLiteral("图片翻译服务返回的译图 Base64 无效。"));
    }
    const auto decoded = QByteArray::fromBase64Encoding(
        encoded, QByteArray::AbortOnBase64DecodingErrors);
    if (!decoded || decoded.decoded.isEmpty()
        || decoded.decoded.size() > kMaxTranslatedImageBytes
        || decoded.decoded.toBase64() != encoded) {
        return fail(QStringLiteral("图片翻译服务返回的译图 Base64 无效。"));
    }

    QImage image = QImage::fromData(decoded.decoded, imageFormat.constData());
    if (image.isNull()) {
        return fail(QStringLiteral("图片翻译服务返回的数据无法解码为 %1 图片。")
                        .arg(mime));
    }
    if (image.size() != expectedImageSize) {
        return fail(QStringLiteral("译图尺寸与原截图不一致：期望 %1x%2，实际 %3x%4。")
                        .arg(expectedImageSize.width())
                        .arg(expectedImageSize.height())
                        .arg(image.width())
                        .arg(image.height()));
    }

    ImageTranslationResult result;
    result.image = std::move(image);
    result.provider = provider.toString().trimmed();
    result.sourceLanguage = sourceLanguage.toString().trimmed();
    result.targetLanguage = targetLanguage.toString().trimmed();
    result.blockCount = blocks.toArray().size();
    result.blocks = blocks.toArray();
    result.serverElapsedMs = static_cast<qint64>(serverElapsedMs);
    return result;
}

void ImageTranslationService::translate(const QImage& image,
                                         const QString& targetLanguage)
{
    QElapsedTimer preparationTimer;
    preparationTimer.start();
    if (isBusy()) {
        emit failed(QStringLiteral("已有图片翻译请求正在进行。"));
        return;
    }
    if (!config_) {
        emit failed(QStringLiteral("图片翻译服务缺少应用配置。"));
        return;
    }

    const AiTranslateSettings settings = config_->settings().aiTranslate;
    if (settings.isOffline()) {
        if (!offline_) {
            offline_ = new OfflineTranslationService(this);
            connect(offline_, &OfflineTranslationService::phaseChanged, this, &ImageTranslationService::phaseChanged);
            connect(offline_, &OfflineTranslationService::succeeded, this, &ImageTranslationService::succeeded);
            connect(offline_, &OfflineTranslationService::failed, this, &ImageTranslationService::failed);
            connect(offline_, &OfflineTranslationService::cancelled, this, &ImageTranslationService::cancelled);
        }
        offline_->translate(image, targetLanguage, settings.offlineResourceDirectory, QStringLiteral("precise"));
        return;
    }
    if (!onlineTranslationEnabled()) {
        emit failed(QStringLiteral("此版本只提供本机离线翻译，不包含联网翻译；没有发送任何内容。"));
        return;
    }
    if (!settings.usesCloudImageTranslation()) {
        emit failed(QStringLiteral("当前处理方式禁止上传截图；不会自动切换到云端。"));
        return;
    }
    if (!settings.uploadConsented()) {
        emit failed(QStringLiteral("尚未确认上传说明：请在首选项「翻译」中查看并确认后再使用联网翻译。没有上传任何内容。"));
        return;
    }

    QString payloadError;
    QSize uploadedImageSize;
    const QByteArray payload = buildRequestPayload(
        image, targetLanguage, &payloadError, &uploadedImageSize);
    if (payload.isEmpty()) {
        emit failed(payloadError);
        return;
    }

    const QUrl endpoint(settings.fastImageTranslateEndpoint());
    if (endpoint.isEmpty()) {
        emit failed(QStringLiteral("尚未填写翻译服务地址：请在首选项「翻译 → 联网服务设置」中填写。没有上传任何内容。"));
        return;
    }
    if (!isUsableEndpoint(endpoint)) {
        emit failed(QStringLiteral("图片翻译服务地址无效：%1")
                        .arg(endpoint.toString()));
        return;
    }
    QString authorizationProblem;
    const QByteArray authorization = settings.serviceAuthorization(endpoint, &authorizationProblem);
    if (!authorizationProblem.isEmpty()) {
        emit failed(authorizationProblem);
        return;
    }

    originalImageSize_ = image.size();
    expectedImageSize_ = uploadedImageSize;
    requestedTargetLanguage_ = targetLanguage.trimmed();
    responseBody_.clear();
    responseLimitExceeded_ = false;
    firstResponseChunkLogged_ = false;
    userCancelled_ = false;
    jobTimer_.start();

    QNetworkRequest request(endpoint);
    // Never follow a server redirect into a different receiver automatically.
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                         QNetworkRequest::ManualRedirectPolicy);
    nam_->setProxy(settings.translationMethod == TranslationMethod::Intranet
                       ? QNetworkProxy(QNetworkProxy::NoProxy)
                       : QNetworkProxy(QNetworkProxy::DefaultProxy));
    request.setHeader(QNetworkRequest::ContentTypeHeader,
                      QStringLiteral("application/json"));
    if (!authorization.isEmpty()) {
        request.setRawHeader("Authorization", authorization);
    }
    request.setTransferTimeout(qBound(15, settings.timeoutSeconds, 300) * 1000);

    Perf::log(QStringLiteral("ImageTranslate.requestPrepared endpoint=%1 target=%2 image=%3x%4 upload=%5x%6 payloadBytes=%7 prepare=%8ms")
                  .arg(endpoint.toString())
                  .arg(requestedTargetLanguage_)
                  .arg(originalImageSize_.width())
                  .arg(originalImageSize_.height())
                  .arg(expectedImageSize_.width())
                  .arg(expectedImageSize_.height())
                  .arg(payload.size())
                  .arg(preparationTimer.elapsed()));
    QElapsedTimer postTimer;
    postTimer.start();
    reply_ = nam_->post(request, payload);
    Perf::log(QStringLiteral("ImageTranslate.postReturned elapsed=%1ms requestElapsed=%2ms")
                  .arg(postTimer.elapsed())
                  .arg(jobTimer_.elapsed()));
    connect(reply_, &QNetworkReply::readyRead,
            this, &ImageTranslationService::drainResponseBody);
    connect(reply_, &QNetworkReply::finished,
            this, &ImageTranslationService::handleReplyFinished);
}

void ImageTranslationService::drainResponseBody()
{
    if (!reply_ || !reply_->isOpen() || responseLimitExceeded_) {
        return;
    }
    if (!firstResponseChunkLogged_ && reply_->bytesAvailable() > 0) {
        firstResponseChunkLogged_ = true;
        Perf::log(QStringLiteral("ImageTranslate.firstResponseChunk elapsed=%1ms available=%2")
                      .arg(jobTimer_.isValid() ? jobTimer_.elapsed() : -1)
                      .arg(reply_->bytesAvailable()));
    }
    const qint64 remaining = kMaxResponseBytes - responseBody_.size();
    if (remaining >= 0) {
        responseBody_.append(reply_->read(remaining + 1));
    }
    if (responseBody_.size() <= kMaxResponseBytes && reply_->bytesAvailable() == 0) {
        return;
    }
    responseLimitExceeded_ = true;
    responseBody_.truncate(kMaxResponseBytes);
    reply_->abort();
}

void ImageTranslationService::handleReplyFinished()
{
    QNetworkReply* reply = reply_.data();
    if (!reply) {
        return;
    }
    drainResponseBody();

    const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    const QString contentType = reply->header(QNetworkRequest::ContentTypeHeader)
                                    .toString()
                                    .toLower();
    const QNetworkReply::NetworkError networkError = reply->error();
    const QString networkMessage = mapNetworkError(reply, responseBody_);
    const QByteArray body = std::move(responseBody_);
    const bool responseLimitExceeded = responseLimitExceeded_;
    const bool userCancelled = userCancelled_;
    reply->deleteLater();
    reply_.clear();
    responseBody_.clear();
    responseLimitExceeded_ = false;
    userCancelled_ = false;

    if (userCancelled) {
        emit cancelled();
        return;
    }
    if (responseLimitExceeded) {
        emit failed(QStringLiteral("图片翻译响应超过 %1 MB 上限，已停止接收。")
                        .arg(kMaxResponseBytes / (1024 * 1024)));
        return;
    }
    if (networkError != QNetworkReply::NoError) {
        emit failed(networkMessage);
        return;
    }
    if (status < 200 || status >= 300) {
        const QString detail = detailFromBody(body);
        emit failed(detail.isEmpty()
                        ? QStringLiteral("图片翻译服务返回 HTTP %1。").arg(status)
                        : QStringLiteral("图片翻译服务返回 HTTP %1：%2")
                              .arg(status)
                              .arg(detail));
        return;
    }
    if (!contentType.startsWith(QStringLiteral("application/json"))) {
        emit failed(QStringLiteral("图片翻译服务返回了非 JSON 内容：%1")
                        .arg(contentType.isEmpty() ? QStringLiteral("未知类型")
                                                   : contentType));
        return;
    }

    QElapsedTimer parseTimer;
    parseTimer.start();
    QString protocolError;
    ImageTranslationResult result = parseResponse(body, expectedImageSize_, &protocolError);
    if (result.image.isNull()) {
        emit failed(protocolError);
        return;
    }
    result.uploadedImageSize = expectedImageSize_;
    if (result.image.size() != originalImageSize_) {
        result.image = result.image.scaled(originalImageSize_,
                                           Qt::IgnoreAspectRatio,
                                           Qt::SmoothTransformation);
        if (result.image.isNull()) {
            emit failed(QStringLiteral("无法将服务端译图恢复到原截图尺寸。"));
            return;
        }
    }
    result.requestedTargetLanguage = requestedTargetLanguage_;
    const qint64 elapsedMs = jobTimer_.isValid() ? jobTimer_.elapsed() : -1;
    Perf::log(QStringLiteral("ImageTranslate.succeeded provider=%1 source=%2 target=%3 requestedTarget=%4 blocks=%5 serverMs=%6 elapsed=%7ms parse=%8ms image=%9x%10")
                  .arg(result.provider)
                  .arg(result.sourceLanguage)
                  .arg(result.targetLanguage)
                  .arg(result.requestedTargetLanguage)
                  .arg(result.blockCount)
                  .arg(result.serverElapsedMs)
                  .arg(elapsedMs)
                  .arg(parseTimer.elapsed())
                  .arg(result.image.width())
                  .arg(result.image.height()));
    emit succeeded(result, elapsedMs);
}

QString ImageTranslationService::mapNetworkError(QNetworkReply* reply,
                                                 const QByteArray& body) const
{
    const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    const QString detail = detailFromBody(body);
    if (status == 401 || status == 403) {
        return QStringLiteral("图片翻译服务拒绝访问（HTTP %1）：请在首选项「翻译」中检查 API 令牌。").arg(status);
    }
    if (status >= 400) {
        return detail.isEmpty()
            ? QStringLiteral("图片翻译服务返回 HTTP %1。").arg(status)
            : QStringLiteral("图片翻译服务返回 HTTP %1：%2").arg(status).arg(detail);
    }
    switch (reply->error()) {
    case QNetworkReply::TimeoutError:
    case QNetworkReply::OperationCanceledError:
        return QStringLiteral("图片翻译请求超时。请稍后重试。");
    case QNetworkReply::HostNotFoundError:
        return QStringLiteral("无法解析图片翻译服务域名。");
    case QNetworkReply::ConnectionRefusedError:
        return QStringLiteral("图片翻译服务拒绝连接。");
    case QNetworkReply::RemoteHostClosedError:
        return QStringLiteral("图片翻译服务提前关闭了连接。");
    case QNetworkReply::SslHandshakeFailedError:
        return QStringLiteral("图片翻译服务 TLS 握手失败。");
    default:
        return QStringLiteral("图片翻译网络错误：%1").arg(reply->errorString());
    }
}

void ImageTranslationService::cancel()
{
    if (offline_) offline_->cancel();
    if (!reply_) {
        return;
    }
    userCancelled_ = true;
    reply_->abort();
}

} // namespace Visnip
