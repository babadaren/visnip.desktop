#include "services/QuestionAnswerService.h"

#include "core/PerfLog.h"

#include <QFutureWatcher>
#include <QNetworkAccessManager>
#include <QNetworkProxy>
#include <QNetworkProxyFactory>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QtConcurrent/QtConcurrentRun>

namespace Visnip {

namespace {

constexpr qsizetype kMaxErrorBodyBytes = 64 * 1024;

QString networkErrorMessage(QNetworkReply::NetworkError error, const QString& detail)
{
    switch (error) {
    case QNetworkReply::HostNotFoundError:
        return QStringLiteral("找不到接口地址对应的服务器，请检查地址或网络。");
    case QNetworkReply::ConnectionRefusedError:
        return QStringLiteral("服务器拒绝连接，请检查接口地址和端口。");
    case QNetworkReply::TimeoutError:
    case QNetworkReply::OperationCanceledError:
        // User cancellation never reaches here (the reply is released
        // first), so a cancelled reply means the transfer timeout fired.
        return QStringLiteral("请求超时：长时间没有收到模型服务的数据。");
    case QNetworkReply::SslHandshakeFailedError:
        return QStringLiteral("HTTPS 连接失败（证书或握手错误）。");
    case QNetworkReply::ProxyConnectionRefusedError:
    case QNetworkReply::ProxyConnectionClosedError:
    case QNetworkReply::ProxyNotFoundError:
    case QNetworkReply::ProxyTimeoutError:
    case QNetworkReply::ProxyAuthenticationRequiredError:
        return QStringLiteral("无法通过代理连接，请检查系统代理，或在设置中关闭「跟随系统代理」。");
    case QNetworkReply::RemoteHostClosedError:
        return QStringLiteral("连接被服务器中断，请重试。");
    default:
        return QStringLiteral("网络请求失败：%1").arg(detail);
    }
}

QNetworkProxy proxyFor(const QUrl& url, bool useSystemProxy)
{
    if (useSystemProxy) {
        const QList<QNetworkProxy> proxies =
            QNetworkProxyFactory::systemProxyForQuery(QNetworkProxyQuery(url));
        if (!proxies.isEmpty()) {
            return proxies.first();
        }
    }
    return QNetworkProxy(QNetworkProxy::NoProxy);
}

} // namespace

QuestionAnswerService::QuestionAnswerService(AppConfig* config, QObject* parent)
    : QObject(parent)
    , config_(config)
    , network_(new QNetworkAccessManager(this))
{
    Q_ASSERT(config_);
}

QuestionAnswerService::~QuestionAnswerService()
{
    ++serial_;
    releaseReply();
}

QString QuestionAnswerService::configurationProblem(const QuestionSettings& settings)
{
    if (settings.apiUrl.trimmed().isEmpty() && settings.apiKey.trimmed().isEmpty()
        && settings.model.trimmed().isEmpty()) {
        return QStringLiteral("还没有配置大模型，请先在设置中填写接口地址、API Key 和模型。");
    }
    if (!Question::endpointUrl(settings.apiFormat, settings.apiUrl).isValid()) {
        return QStringLiteral("接口地址无效，请在设置中检查。");
    }
    if (Question::effectiveModel(settings.apiFormat, settings.model).isEmpty()) {
        return QStringLiteral("请先在设置中填写模型名称（需要支持图片输入的多模态模型）。");
    }
    if (settings.apiFormat == QuestionApiFormat::Anthropic && settings.apiKey.trimmed().isEmpty()) {
        return QStringLiteral("请先在设置中填写 API Key。");
    }
    return {};
}

bool QuestionAnswerService::isBusy() const
{
    return encoding_ || reply_;
}

void QuestionAnswerService::ask(const QImage& image)
{
    const QuestionSettings& settings = config_->settings().question;
    start(image,
          Question::systemPrompt(settings.customPrompt, settings.subjectScope),
          Question::userInstruction());
}

void QuestionAnswerService::testConnection()
{
    QImage probe(64, 64, QImage::Format_RGB32);
    probe.fill(QColor(0xF2, 0xF4, 0xF7));
    start(probe,
          QStringLiteral("这是一次连接测试。"),
          QStringLiteral("这是一次连接测试，请只回复 OK。"));
}

void QuestionAnswerService::cancel()
{
    ++serial_;
    encoding_ = false;
    releaseReply();
}

void QuestionAnswerService::start(const QImage& image, const QString& systemPrompt,
                                  const QString& userText)
{
    cancel();
    const quint64 serial = serial_;
    const QString problem = configurationProblem(config_->settings().question);
    if (!problem.isEmpty()) {
        emit failed(problem);
        return;
    }
    if (image.isNull()) {
        emit failed(QStringLiteral("截图失败，没有可以发送的图片。"));
        return;
    }

    // PNG-encoding a large capture takes long enough to stall the panel.
    encoding_ = true;
    emit started();
    auto* watcher = new QFutureWatcher<Question::EncodedImage>(this);
    connect(watcher, &QFutureWatcher<Question::EncodedImage>::finished, this,
            [this, watcher, serial, systemPrompt, userText]() {
        watcher->deleteLater();
        if (serial != serial_) {
            return; // cancelled or superseded meanwhile
        }
        encoding_ = false;
        const Question::EncodedImage encoded = watcher->result();
        if (encoded.isNull()) {
            emit failed(QStringLiteral("截图编码失败。"));
            return;
        }
        send(encoded, systemPrompt, userText);
    });
    watcher->setFuture(QtConcurrent::run([image]() { return Question::encodeImage(image); }));
}

void QuestionAnswerService::send(const Question::EncodedImage& image,
                                 const QString& systemPrompt, const QString& userText)
{
    const QuestionSettings settings = config_->settings().question;
    Question::RequestInput input;
    input.format = settings.apiFormat;
    input.apiUrl = settings.apiUrl;
    input.apiKey = settings.apiKey;
    input.model = settings.model;
    input.systemPrompt = systemPrompt;
    input.userText = userText;
    input.image = image;
    const Question::HttpRequest http = Question::buildRequest(input);

    QNetworkRequest request(http.url);
    for (const auto& header : http.headers) {
        request.setRawHeader(header.first, header.second);
    }
    request.setTransferTimeout(qBound(30, settings.timeoutSeconds, 600) * 1000);
    network_->setProxy(proxyFor(http.url, settings.useSystemProxy));

    parser_ = std::make_unique<Question::StreamParser>(settings.apiFormat);
    errorBody_.clear();
    reply_ = network_->post(request, http.body);
    connect(reply_, &QNetworkReply::readyRead, this, &QuestionAnswerService::handleReadyRead);
    connect(reply_, &QNetworkReply::finished, this, &QuestionAnswerService::handleFinished);
    Perf::log(QStringLiteral("QuestionAnswer.request format=%1 host=%2 model=%3 image=%4x%5 bytes=%6 mime=%7")
                  .arg(questionApiFormatId(settings.apiFormat), http.url.host(),
                       Question::effectiveModel(settings.apiFormat, settings.model))
                  .arg(image.size.width())
                  .arg(image.size.height())
                  .arg(image.data.size())
                  .arg(image.mimeType));
}

void QuestionAnswerService::handleReadyRead()
{
    QNetworkReply* reply = reply_.data();
    if (!reply || sender() != reply || !parser_) {
        return;
    }
    const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    const QByteArray chunk = reply->readAll();
    if (status >= 400) {
        if (errorBody_.size() < kMaxErrorBodyBytes) {
            errorBody_ += chunk;
        }
        return;
    }
    parser_->feed(chunk);
    if (!parser_->text().isEmpty() || parser_->reasoning()) {
        emit progress(parser_->text(), parser_->reasoning() && parser_->text().isEmpty());
    }
}

void QuestionAnswerService::handleFinished()
{
    QNetworkReply* reply = reply_.data();
    if (!reply || sender() != reply || !parser_) {
        return;
    }
    const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    const QNetworkReply::NetworkError error = reply->error();
    const QString errorString = reply->errorString();
    const QByteArray rest = reply->readAll();
    const std::unique_ptr<Question::StreamParser> parser = std::move(parser_);
    const QByteArray errorBody = errorBody_ + rest;
    errorBody_.clear();
    releaseReply();

    Perf::log(QStringLiteral("QuestionAnswer.finished status=%1 error=%2").arg(status).arg(int(error)));
    if (status >= 400) {
        emit failed(Question::describeHttpError(status, errorBody));
        return;
    }
    if (error != QNetworkReply::NoError) {
        emit failed(networkErrorMessage(error, errorString));
        return;
    }
    parser->feed(rest);
    parser->finish();
    if (!parser->errorMessage().isEmpty()) {
        emit failed(QStringLiteral("模型服务返回错误：%1").arg(parser->errorMessage()));
    } else if (parser->refused()) {
        emit failed(QStringLiteral("模型拒绝回答这道题（可能触发了服务方的安全策略），可以换个模型再试。"));
    } else if (parser->text().trimmed().isEmpty()) {
        emit failed(QStringLiteral("模型没有返回答案。"));
    } else {
        emit finished(parser->text(), parser->truncated());
    }
}

void QuestionAnswerService::releaseReply()
{
    if (!reply_) {
        return;
    }
    QNetworkReply* reply = reply_.data();
    reply_.clear();
    parser_.reset();
    disconnect(reply, nullptr, this, nullptr);
    if (reply->isRunning()) {
        reply->abort();
    }
    reply->deleteLater();
}

} // namespace Visnip
