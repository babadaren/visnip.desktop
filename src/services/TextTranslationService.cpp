#include "services/TextTranslationService.h"

#include "core/PerfLog.h"
#include "core/TranslationLanguage.h"

#include <QCryptographicHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkProxy>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QRandomGenerator>
#include <QSet>

#ifdef Q_OS_WIN
#include <qt_windows.h>
#endif

namespace Visnip {

namespace {

constexpr qint64 kMaxResponseBytes = 8LL * 1024 * 1024;
constexpr int kHealthTimeoutMs = 10000;
constexpr int kBaiduMaxQueryBytes = 6000;
constexpr auto kBaiduEndpoint = "https://fanyi-api.baidu.com/api/trans/vip/translate";

bool isUsableEndpoint(const QUrl& url)
{
    const QString scheme = url.scheme().toLower();
    return url.isValid()
        && !url.host().isEmpty()
        && (scheme == QStringLiteral("http") || scheme == QStringLiteral("https"));
}

QString providerName(bool baidu)
{
    return baidu ? QStringLiteral("baidu") : QStringLiteral("official");
}

// Errors come back as {"detail": "..."} (or FastAPI's validation array).
QString detailFromBody(const QByteArray& body)
{
    const QJsonDocument doc = QJsonDocument::fromJson(body);
    if (!doc.isObject()) {
        return {};
    }
    const QJsonValue detail = doc.object().value(QStringLiteral("detail"));
    QString text = detail.toString();
    if (text.isEmpty() && detail.isArray() && !detail.toArray().isEmpty()) {
        text = detail.toArray().first().toObject().value(QStringLiteral("msg")).toString();
    }
    if (text.size() > 200) {
        text = text.left(200) + QStringLiteral("…");
    }
    return text;
}

QString baiduErrorMessage(const QString& code, const QString& providerMessage)
{
    static const QHash<QString, QString> messages = {
        { QStringLiteral("52001"), QStringLiteral("请求超时") },
        { QStringLiteral("52002"), QStringLiteral("系统错误") },
        { QStringLiteral("52003"), QStringLiteral("APP ID 未授权") },
        { QStringLiteral("54000"), QStringLiteral("必填参数为空") },
        { QStringLiteral("54001"), QStringLiteral("签名错误，请检查 APP ID 和开发者密钥") },
        { QStringLiteral("54003"), QStringLiteral("访问频率受限") },
        { QStringLiteral("54004"), QStringLiteral("账户余额不足或额度已用完") },
        { QStringLiteral("54005"), QStringLiteral("翻译文本过长") },
        { QStringLiteral("58000"), QStringLiteral("客户端 IP 不在允许范围内") },
        { QStringLiteral("58001"), QStringLiteral("不支持该翻译语言方向") },
        { QStringLiteral("58002"), QStringLiteral("通用文本翻译服务已关闭") },
        { QStringLiteral("58003"), QStringLiteral("客户端 IP 被封禁") },
    };
    QString message = messages.value(code);
    if (message.isEmpty()) {
        message = providerMessage.trimmed();
    }
    if (message.isEmpty()) {
        message = QStringLiteral("未知错误");
    }
    return QStringLiteral("百度翻译失败（%1）：%2").arg(code, message);
}

} // namespace

TextTranslationService::TextTranslationService(AppConfig* config, QObject* parent)
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
                if (healthReply_) {
                    healthReply_->abort();
                }
                nam_->clearConnectionCache();
            }
        });
    }
}

QByteArray TextTranslationService::buildRequestPayload(const QStringList& texts,
                                                       const QString& targetLanguage)
{
    QJsonArray array;
    for (const QString& text : texts) {
        array.append(text);
    }
    QJsonObject body;
    body.insert(QStringLiteral("texts"), array);
    body.insert(QStringLiteral("target_lang"), targetLanguage);
    body.insert(QStringLiteral("source_lang"), QStringLiteral("auto"));
    return QJsonDocument(body).toJson(QJsonDocument::Compact);
}

QString TextTranslationService::baiduLanguageCode(const QString& languageCode)
{
    static const QHash<QString, QString> codes = {
        { QStringLiteral("zh-Hans"), QStringLiteral("zh") },
        { QStringLiteral("zh-Hant"), QStringLiteral("cht") },
        { QStringLiteral("en"), QStringLiteral("en") },
        { QStringLiteral("ja"), QStringLiteral("jp") },
        { QStringLiteral("ko"), QStringLiteral("kor") },
        { QStringLiteral("fr"), QStringLiteral("fra") },
        { QStringLiteral("de"), QStringLiteral("de") },
        { QStringLiteral("es"), QStringLiteral("spa") },
        { QStringLiteral("pt"), QStringLiteral("pt") },
        { QStringLiteral("it"), QStringLiteral("it") },
        { QStringLiteral("ru"), QStringLiteral("ru") },
        { QStringLiteral("ar"), QStringLiteral("ara") },
        { QStringLiteral("vi"), QStringLiteral("vie") },
        { QStringLiteral("th"), QStringLiteral("th") },
    };
    return codes.value(languageCode);
}

QByteArray TextTranslationService::buildBaiduRequestPayload(const QStringList& texts,
                                                            const QString& targetLanguage,
                                                            const QString& appId,
                                                            const QString& secretKey,
                                                            const QString& salt,
                                                            QString* error)
{
    const auto fail = [error](const QString& message) {
        if (error) {
            *error = message;
        }
        return QByteArray{};
    };
    if (appId.trimmed().isEmpty() || secretKey.isEmpty()) {
        return fail(QStringLiteral("百度翻译需要填写 APP ID 和开发者密钥。"));
    }
    const QString target = baiduLanguageCode(targetLanguage);
    if (target.isEmpty()) {
        return fail(QStringLiteral("百度通用文本翻译不支持当前目标语言：%1").arg(targetLanguage));
    }

    const QString query = texts.join(QLatin1Char('\n'));
    if (query.isEmpty()) {
        return fail(QStringLiteral("没有可发送给百度翻译的文字。"));
    }
    if (query.toUtf8().size() > kBaiduMaxQueryBytes) {
        return fail(QStringLiteral("百度翻译单次请求超过 6000 字节限制。"));
    }

    const QString canonicalAppId = appId.trimmed();
    const QByteArray signatureInput = canonicalAppId.toUtf8()
        + query.toUtf8()
        + salt.toUtf8()
        + secretKey.toUtf8();
    const QString sign = QString::fromLatin1(
        QCryptographicHash::hash(signatureInput, QCryptographicHash::Md5).toHex());

    QByteArray form;
    const auto appendField = [&form](const QByteArray& name,
                                     const QString& value) {
        if (!form.isEmpty()) {
            form += '&';
        }
        form += name;
        form += '=';
        form += QUrl::toPercentEncoding(value);
    };
    appendField(QByteArrayLiteral("q"), query);
    appendField(QByteArrayLiteral("from"), QStringLiteral("auto"));
    appendField(QByteArrayLiteral("to"), target);
    appendField(QByteArrayLiteral("appid"), canonicalAppId);
    appendField(QByteArrayLiteral("salt"), salt);
    appendField(QByteArrayLiteral("sign"), sign);
    return form;
}

QString TextTranslationService::preservePersonalName(
    const QString& source,
    const QString& translation)
{
    const QString trimmed = source.trimmed();
    const int parenthesis = trimmed.indexOf(QLatin1Char('('));
    const QString name = (parenthesis >= 0
                              ? trimmed.left(parenthesis)
                              : trimmed)
                             .trimmed();
    const QStringList words = name.split(QLatin1Char(' '), Qt::SkipEmptyParts);
    if (words.size() < 2 || words.size() > 4) {
        return translation;
    }
    static const QSet<QString> commonUiWords = {
        QStringLiteral("Account"),
        QStringLiteral("Email"),
        QStringLiteral("Personal"),
        QStringLiteral("Picture"),
        QStringLiteral("Profile"),
        QStringLiteral("Public"),
        QStringLiteral("Settings"),
        QStringLiteral("Social"),
    };
    for (const QString& word : words) {
        if (word.size() < 2 || !word.front().isUpper()
            || commonUiWords.contains(word)) {
            return translation;
        }
        for (const QChar ch : word) {
            if (!ch.isLetter() && ch != QLatin1Char('-')
                && ch != QLatin1Char('\'')) {
                return translation;
            }
        }
    }
    return trimmed;
}

QString TextTranslationService::restoreProtectedIdentifiers(
    const QString& source,
    const QString& translation)
{
    QStringList identifiers;
    int index = 0;
    while (index < source.size()) {
        const QChar opening = source[index];
        const QChar closing = opening == QLatin1Char('(')
            ? QLatin1Char(')')
            : (opening == QLatin1Char('[') ? QLatin1Char(']') : QChar());
        if (closing.isNull()) {
            ++index;
            continue;
        }
        const int closeIndex = source.indexOf(closing, index + 1);
        if (closeIndex < 0) {
            break;
        }
        const QString identifier = source.sliced(index + 1,
                                                 closeIndex - index - 1)
                                       .trimmed();
        bool identifierLike = !identifier.isEmpty();
        for (const QChar ch : identifier) {
            if (!ch.isLetterOrNumber()
                && ch != QLatin1Char('-')
                && ch != QLatin1Char('_')
                && ch != QLatin1Char('.')
                && ch != QLatin1Char('@')) {
                identifierLike = false;
                break;
            }
        }
        if (identifierLike) {
            identifiers.append(identifier);
        }
        index = closeIndex + 1;
    }
    if (identifiers.isEmpty()) {
        return translation;
    }

    QString result = translation.trimmed();
    for (const QString& identifier : identifiers) {
        const QString suffix = QStringLiteral(" (%1)").arg(identifier);
        if (!result.contains(identifier, Qt::CaseInsensitive)) {
            const QChar last = result.isEmpty() ? QChar() : result.back();
            if (!last.isNull() && QStringLiteral("。！？.!?").contains(last)) {
                result.chop(1);
                result += suffix;
                result += last;
            } else {
                result += suffix;
            }
        }
    }
    return result;
}

QString TextTranslationService::normalizeTranslationForTarget(
    const QString& text,
    const QString& targetLanguage)
{
    if (targetLanguage.compare(QStringLiteral("zh-Hans"), Qt::CaseInsensitive) != 0
        || text.isEmpty()) {
        return text;
    }
#ifdef Q_OS_WIN
    const int required = LCMapStringEx(L"zh-CN",
                                       LCMAP_SIMPLIFIED_CHINESE,
                                       reinterpret_cast<LPCWCH>(text.utf16()),
                                       text.size(),
                                       nullptr,
                                       0,
                                       nullptr,
                                       nullptr,
                                       0);
    if (required <= 0) {
        Perf::log(QStringLiteral("TextTranslate.hans_normalize_failed error=%1")
                      .arg(GetLastError()));
        return text;
    }
    QString normalized(required, Qt::Uninitialized);
    const int written = LCMapStringEx(L"zh-CN",
                                      LCMAP_SIMPLIFIED_CHINESE,
                                      reinterpret_cast<LPCWCH>(text.utf16()),
                                      text.size(),
                                      reinterpret_cast<LPWSTR>(normalized.data()),
                                      normalized.size(),
                                      nullptr,
                                      nullptr,
                                      0);
    return written == required ? normalized : text;
#else
    return text;
#endif
}

QStringList TextTranslationService::parseTranslations(const QByteArray& body,
                                                       QString* error,
                                                       int expectedCount)
{
    const auto fail = [error](const QString& message) {
        if (error) {
            *error = message;
        }
        return QStringList{};
    };

    QJsonParseError parseError{};
    const QJsonDocument doc = QJsonDocument::fromJson(body, &parseError);
    if (parseError.error != QJsonParseError::NoError || !doc.isObject()) {
        return fail(QStringLiteral("翻译服务返回了无法解析的数据，请检查服务地址是否正确。"));
    }
    const QJsonValue translations = doc.object().value(QStringLiteral("translations"));
    if (!translations.isArray()) {
        const QString detail = detailFromBody(body);
        return fail(detail.isEmpty()
                        ? QStringLiteral("翻译服务返回结果中没有译文内容。")
                        : QStringLiteral("翻译服务返回错误：%1").arg(detail));
    }
    QStringList result;
    const QJsonArray array = translations.toArray();
    if (expectedCount >= 0 && array.size() != expectedCount) {
        return fail(QStringLiteral("翻译服务返回的译文数量不匹配：预期 %1，实际 %2。")
                        .arg(expectedCount)
                        .arg(array.size()));
    }
    result.reserve(array.size());
    for (int index = 0; index < array.size(); ++index) {
        const QJsonValue value = array.at(index);
        if (!value.isString() || value.toString().trimmed().isEmpty()) {
            return fail(QStringLiteral("翻译服务返回的第 %1 段译文为空或类型无效。")
                            .arg(index + 1));
        }
        result.append(value.toString());
    }
    if (result.isEmpty()) {
        return fail(QStringLiteral("翻译服务返回结果中没有译文内容。"));
    }
    return result;
}

QStringList TextTranslationService::parseBaiduTranslations(const QByteArray& body,
                                                           const QStringList& expectedTexts,
                                                           QString* error)
{
    const auto fail = [error](const QString& message) {
        if (error) {
            *error = message;
        }
        return QStringList{};
    };
    QJsonParseError parseError{};
    const QJsonDocument doc = QJsonDocument::fromJson(body, &parseError);
    if (parseError.error != QJsonParseError::NoError || !doc.isObject()) {
        return fail(QStringLiteral("百度翻译返回了无法解析的数据。"));
    }
    const QJsonObject root = doc.object();
    const QString code = root.value(QStringLiteral("error_code")).toVariant().toString();
    if (!code.isEmpty()) {
        return fail(baiduErrorMessage(code,
                                      root.value(QStringLiteral("error_msg")).toString()));
    }
    const QJsonArray array = root.value(QStringLiteral("trans_result")).toArray();
    if (array.isEmpty()) {
        return fail(QStringLiteral("百度翻译返回结果中没有译文内容。"));
    }
    if (array.size() != expectedTexts.size()) {
        return fail(QStringLiteral("百度翻译返回的译文数量不匹配：预期 %1，实际 %2。")
                        .arg(expectedTexts.size())
                        .arg(array.size()));
    }
    QStringList result;
    result.reserve(array.size());
    for (int index = 0; index < array.size(); ++index) {
        const QJsonObject item = array.at(index).toObject();
        const QString source = item.value(QStringLiteral("src")).toString();
        if (source != expectedTexts.at(index)) {
            return fail(QStringLiteral("百度翻译返回的第 %1 段原文与请求不匹配。")
                            .arg(index + 1));
        }
        const QString translated = item.value(QStringLiteral("dst")).toString();
        if (translated.trimmed().isEmpty()) {
            return fail(QStringLiteral("百度翻译返回了空译文。"));
        }
        result.append(translated);
    }
    return result;
}

void TextTranslationService::translate(const QStringList& paragraphs,
                                       const QString& languageCode)
{
    if (!config_) {
        emit failed(QStringLiteral("文字翻译服务缺少应用配置。"));
        return;
    }
    if (isBusy()) {
        return;
    }
    if (paragraphs.isEmpty()) {
        emit failed(QStringLiteral("没有可翻译的文字内容。"));
        return;
    }
    for (int index = 0; index < paragraphs.size(); ++index) {
        const QString& paragraph = paragraphs.at(index);
        if (paragraph.trimmed().isEmpty()) {
            emit failed(QStringLiteral("第 %1 段没有可翻译的文字内容。")
                            .arg(index + 1));
            return;
        }
        if (paragraph.size() > kMaxCharsPerText) {
            emit failed(QStringLiteral("第 %1 段超过 %2 字符限制，已保留原图。")
                            .arg(index + 1)
                            .arg(kMaxCharsPerText));
            return;
        }
    }
    const AiTranslateSettings settings = config_->settings().aiTranslate;
    if (!onlineTranslationEnabled()) {
        emit failed(QStringLiteral("此版本只提供本机离线翻译，不包含联网翻译；没有发送任何内容。"));
        return;
    }
    if (!settings.usesLegacyOnlineTextTranslation()) {
        emit failed(QStringLiteral("当前处理方式禁止调用旧版在线文字翻译。"));
        return;
    }
    if (!settings.uploadConsented()) {
        emit failed(QStringLiteral("尚未确认上传说明：请在首选项「翻译」中查看并确认后再使用联网翻译。没有发送任何文字。"));
        return;
    }
    const QUrl officialEndpoint(settings.fastTranslateEndpoint());
    if (!isUsableEndpoint(officialEndpoint)) {
        emit failed(QStringLiteral("翻译服务地址无效：%1\n请打开托盘菜单「首选项 → 翻译」检查。")
                        .arg(settings.fastServiceBaseUrl()));
        return;
    }
    QString authorizationProblem;
    officialAuthorization_ = settings.serviceAuthorization(officialEndpoint, &authorizationProblem);
    if (!authorizationProblem.isEmpty()) {
        emit failed(authorizationProblem);
        return;
    }

    userCancelled_ = false;
    targetLanguage_ = normalizedTranslationLanguageCode(languageCode);
    timeoutMs_ = qBound(15, settings.timeoutSeconds, 300) * 1000;
    paragraphs_ = paragraphs;
    baiduAppId_ = settings.baiduAppId;
    baiduSecretKey_ = settings.baiduSecretKey;
    officialEndpoint_ = officialEndpoint;
    endpoint_ = officialEndpoint_;
    provider_ = settings.fastProvider == QStringLiteral("baidu")
        ? Provider::Baidu
        : Provider::Official;
    if (provider_ == Provider::Baidu) {
        endpoint_ = QUrl(QString::fromLatin1(kBaiduEndpoint));
    }
    prepareBatches();
    jobTimer_.start();

    Perf::log(QStringLiteral("TextTranslate.job provider=%1 endpoint=%2 lang=%3 paragraphs=%4 batches=%5")
                  .arg(providerName(provider_ == Provider::Baidu))
                  .arg(endpoint_.toString())
                  .arg(targetLanguage_)
                  .arg(paragraphs_.size())
                  .arg(batches_.size()));
    emit providerChanged(providerName(provider_ == Provider::Baidu), false);
    if (provider_ == Provider::Baidu
        && (baiduAppId_.trimmed().isEmpty() || baiduSecretKey_.isEmpty())) {
        // Never re-route the text to another receiver without the user's choice.
        finishFailure(QStringLiteral("未配置百度 APP ID 或开发者密钥，请在首选项「翻译」中填写；没有改用其他翻译服务。"));
        return;
    }
    sendNextBatch();
}

void TextTranslationService::prepareBatches()
{
    batches_.clear();
    batchIndex_ = 0;
    collected_.clear();
    if (provider_ == Provider::Official) {
        for (qsizetype offset = 0; offset < paragraphs_.size(); offset += kMaxTextsPerRequest) {
            batches_.append(paragraphs_.mid(offset, kMaxTextsPerRequest));
        }
        return;
    }

    QStringList batch;
    int bytes = 0;
    for (const QString& paragraph : paragraphs_) {
        const QString& text = paragraph;
        const int textBytes = text.toUtf8().size();
        const int addedBytes = batch.isEmpty() ? textBytes : textBytes + 1;
        if (!batch.isEmpty()
            && (batch.size() >= kMaxTextsPerRequest
                || bytes + addedBytes > kBaiduMaxQueryBytes)) {
            batches_.append(batch);
            batch.clear();
            bytes = 0;
        }
        bytes += batch.isEmpty() ? textBytes : textBytes + 1;
        batch.append(text);
    }
    if (!batch.isEmpty()) {
        batches_.append(batch);
    }
}

void TextTranslationService::sendNextBatch()
{
    if (!config_ || !config_->settings().aiTranslate.usesLegacyOnlineTextTranslation()) {
        finishFailure(QStringLiteral("处理方式已改变，已停止在线文字翻译。"));
        return;
    }
    const QStringList& texts = batches_.at(batchIndex_);
    QByteArray payload;
    QString payloadError;
    QNetworkRequest request(endpoint_);
    request.setTransferTimeout(timeoutMs_);
    if (provider_ == Provider::Baidu) {
        const QString salt = QString::number(QRandomGenerator::global()->generate64());
        payload = buildBaiduRequestPayload(texts,
                                           targetLanguage_,
                                           baiduAppId_,
                                           baiduSecretKey_,
                                           salt,
                                           &payloadError);
        request.setHeader(QNetworkRequest::ContentTypeHeader,
                          QStringLiteral("application/x-www-form-urlencoded"));
    } else {
        payload = buildRequestPayload(texts, targetLanguage_);
        // Only our own service receives the token, never the Baidu endpoint.
        if (!officialAuthorization_.isEmpty()) {
            request.setRawHeader("Authorization", officialAuthorization_);
        }
        request.setHeader(QNetworkRequest::ContentTypeHeader,
                          QStringLiteral("application/json"));
    }
    if (payload.isEmpty()) {
        finishFailure(payloadError);
        return;
    }

    Perf::log(QStringLiteral("TextTranslate.request provider=%1 batch=%2/%3 texts=%4 payloadBytes=%5")
                  .arg(providerName(provider_ == Provider::Baidu))
                  .arg(batchIndex_ + 1)
                  .arg(batches_.size())
                  .arg(texts.size())
                  .arg(payload.size()));

    reply_ = nam_->post(request, payload);
    connect(reply_, &QNetworkReply::finished, this, &TextTranslationService::handleReplyFinished);
}

void TextTranslationService::handleReplyFinished()
{
    auto* reply = reply_.data();
    if (!reply) {
        return;
    }
    const QByteArray body = reply->bytesAvailable() <= kMaxResponseBytes
        ? reply->readAll()
        : QByteArray();
    const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    const QNetworkReply::NetworkError networkError = reply->error();
    const QString networkMessage = mapNetworkError(reply, body);
    reply->deleteLater();
    reply_.clear();

    if (userCancelled_) {
        emit cancelled();
        return;
    }

    const qint64 serverMs = provider_ == Provider::Official
        ? static_cast<qint64>(
              QJsonDocument::fromJson(body).object().value(QStringLiteral("elapsed_ms")).toDouble(-1))
        : -1;
    Perf::log(QStringLiteral("TextTranslate.response provider=%1 batch=%2/%3 status=%4 networkError=%5 bytes=%6 serverMs=%7 elapsed=%8ms")
                  .arg(providerName(provider_ == Provider::Baidu))
                  .arg(batchIndex_ + 1)
                  .arg(batches_.size())
                  .arg(status)
                  .arg(networkError)
                  .arg(body.size())
                  .arg(serverMs)
                  .arg(jobTimer_.isValid() ? jobTimer_.elapsed() : -1));

    if (networkError != QNetworkReply::NoError) {
        finishFailure(networkMessage);
        return;
    }
    if (body.isEmpty()) {
        const QString message = QStringLiteral("翻译服务返回内容为空或超过 %1 MB 上限。")
                                    .arg(kMaxResponseBytes / (1024 * 1024));
        finishFailure(message);
        return;
    }

    QString error;
    const qsizetype sentCount = batches_.at(batchIndex_).size();
    QStringList translations = provider_ == Provider::Baidu
        ? parseBaiduTranslations(body, batches_.at(batchIndex_), &error)
        : parseTranslations(body, &error, static_cast<int>(sentCount));
    if (translations.isEmpty()) {
        finishFailure(error);
        return;
    }

    const QStringList& sourceTexts = batches_.at(batchIndex_);
    for (int index = 0; index < translations.size(); ++index) {
        QString& translation = translations[index];
        translation = normalizeTranslationForTarget(translation, targetLanguage_);
        translation = preservePersonalName(sourceTexts.value(index),
                                           translation);
        translation = restoreProtectedIdentifiers(sourceTexts.value(index),
                                                    translation);
    }

    collected_ += translations;

    ++batchIndex_;
    if (batchIndex_ < batches_.size()) {
        sendNextBatch();
        return;
    }
    emit succeeded(collected_, jobTimer_.elapsed());
}

void TextTranslationService::finishFailure(const QString& message)
{
    paragraphs_.clear();
    batches_.clear();
    collected_.clear();
    emit failed(message);
}

QString TextTranslationService::mapNetworkError(QNetworkReply* reply,
                                                const QByteArray& body) const
{
    const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    const QString detail = detailFromBody(body);
    const QString suffix = detail.isEmpty() ? QString() : QStringLiteral("\n服务返回：%1").arg(detail);

    if (reply->error() == QNetworkReply::OperationCanceledError) {
        return QStringLiteral("请求超时：可在设置中调大超时时间后重试。");
    }
    switch (status) {
    case 400:
        return QStringLiteral("翻译服务拒绝了本次请求。") + suffix;
    case 401:
    case 403:
        return QStringLiteral("翻译服务拒绝访问（HTTP %1）：请在首选项「翻译」中检查 API 令牌。").arg(status) + suffix;
    case 404:
        return QStringLiteral("翻译接口不存在：%1。").arg(endpoint_.toString()) + suffix;
    case 429:
        return QStringLiteral("请求过于频繁，请稍后重试。") + suffix;
    default:
        break;
    }
    if (status >= 500) {
        return QStringLiteral("翻译服务暂时不可用（HTTP %1），请稍后重试。").arg(status) + suffix;
    }
    if (status >= 400) {
        return QStringLiteral("请求被拒绝（HTTP %1）。").arg(status) + suffix;
    }
    return QStringLiteral("网络错误：%1").arg(reply->errorString());
}

void TextTranslationService::cancel()
{
    if (!reply_) {
        return;
    }
    userCancelled_ = true;
    reply_->abort();
}

void TextTranslationService::checkHealth(const QString& serviceUrl)
{
    if (!onlineTranslationEnabled() || !config_ || !config_->settings().aiTranslate.allowsTranslationNetwork()) {
        emit healthChecked(false, QStringLiteral("本机离线模式不发送网络连接测试。"));
        return;
    }
    if (healthReply_) {
        return;
    }
    const bool intranet = config_->settings().aiTranslate.translationMethod
        == TranslationMethod::Intranet;
    const QString base = intranet
        ? config_->settings().aiTranslate.fastServiceBaseUrl()
        : aiTranslateNormalizedFastServiceUrl(serviceUrl);
    const QUrl url(base + QStringLiteral("/healthz"));
    if (!isUsableEndpoint(url)) {
        emit healthChecked(false, QStringLiteral("服务地址无效：%1").arg(base));
        return;
    }
    QNetworkRequest request(url);
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                         QNetworkRequest::ManualRedirectPolicy);
    nam_->setProxy(intranet ? QNetworkProxy(QNetworkProxy::NoProxy)
                           : QNetworkProxy(QNetworkProxy::DefaultProxy));
    request.setTransferTimeout(kHealthTimeoutMs);
    healthReply_ = nam_->get(request);
    connect(healthReply_, &QNetworkReply::finished, this,
            &TextTranslationService::handleHealthFinished);
}

void TextTranslationService::handleHealthFinished()
{
    auto* reply = healthReply_.data();
    if (!reply) {
        return;
    }
    const QByteArray body = reply->readAll();
    reply->deleteLater();
    healthReply_.clear();

    const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    Perf::log(QStringLiteral("TextTranslate.health url=%1 status=%2 networkError=%3")
                  .arg(reply->url().toString())
                  .arg(status)
                  .arg(reply->error()));

    if (reply->error() != QNetworkReply::NoError) {
        emit healthChecked(false, QStringLiteral("连接失败：%1").arg(reply->errorString()));
        return;
    }
    const QJsonObject root = QJsonDocument::fromJson(body).object();
    if (root.value(QStringLiteral("status")).toString() != QStringLiteral("ok")) {
        emit healthChecked(false, QStringLiteral("服务返回了异常状态，请确认地址指向 Visnip 翻译服务。"));
        return;
    }
    if (config_ && config_->settings().aiTranslate.usesCloudImageTranslation()) {
        const QJsonObject imageTranslation =
            root.value(QStringLiteral("image_translation")).toObject();
        if (!imageTranslation.value(QStringLiteral("configured")).toBool()) {
            emit healthChecked(false, QStringLiteral("服务已连接，但图片翻译尚未配置。"));
            return;
        }
        const QString provider = imageTranslation.value(QStringLiteral("provider"))
                                     .toString()
                                     .trimmed();
        emit healthChecked(true, provider.isEmpty()
                                     ? QStringLiteral("图片翻译服务连接正常。")
                                     : QStringLiteral("图片翻译服务连接正常（%1）。")
                                           .arg(provider));
        return;
    }
    const QString model = root.value(QStringLiteral("model")).toString();
    emit healthChecked(true, model.isEmpty()
                                 ? QStringLiteral("连接正常。")
                                 : QStringLiteral("连接正常（模型：%1）。").arg(model));
}

} // namespace Visnip
