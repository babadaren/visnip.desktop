#include "core/AppConfig.h"
#include "core/QuestionAnswer.h"
#include "services/QuestionAnswerService.h"
#include "ui/question/QuestionPanel.h"

#include <QApplication>
#include <QBuffer>
#include <QImageReader>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QPointer>
#include <QPushButton>
#include <QScreen>
#include <QSettings>
#include <QSignalSpy>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTemporaryDir>
#include <QTextBrowser>
#include <QTimer>
#include <QToolButton>
#include <QtTest/QtTest>

using namespace Visnip;

namespace {

QByteArray header(const Question::HttpRequest& request, const QByteArray& name)
{
    for (const auto& entry : request.headers) {
        if (entry.first.compare(name, Qt::CaseInsensitive) == 0) {
            return entry.second;
        }
    }
    return {};
}

QJsonObject bodyObject(const Question::HttpRequest& request)
{
    return QJsonDocument::fromJson(request.body).object();
}

Question::EncodedImage sampleImage()
{
    QImage image(40, 20, QImage::Format_RGB32);
    image.fill(Qt::white);
    return Question::encodeImage(image);
}

// Minimal HTTP/1.1 server standing in for a model API: records the request
// and answers with a status line and body chunks, then closes.
class FakeModelServer : public QObject {
public:
    int status = 200;
    QByteArray contentType = "text/event-stream";
    QList<QByteArray> chunks;
    int chunkDelayMs = 0;
    bool hang = false;
    QByteArray requestHead;
    QByteArray requestBody;
    int requests = 0;

    FakeModelServer()
    {
        QObject::connect(&server_, &QTcpServer::newConnection, this, [this]() {
            while (QTcpSocket* socket = server_.nextPendingConnection()) {
                auto buffer = std::make_shared<QByteArray>();
                QObject::connect(socket, &QTcpSocket::readyRead, this, [this, socket, buffer]() {
                    *buffer += socket->readAll();
                    const qsizetype headEnd = buffer->indexOf("\r\n\r\n");
                    if (headEnd < 0) {
                        return;
                    }
                    const QByteArray head = buffer->left(headEnd);
                    qsizetype length = 0;
                    for (const QByteArray& line : head.split('\n')) {
                        if (line.toLower().startsWith("content-length:")) {
                            length = line.mid(15).trimmed().toLongLong();
                        }
                    }
                    if (buffer->size() < headEnd + 4 + length) {
                        return;
                    }
                    requestHead = head;
                    requestBody = buffer->mid(headEnd + 4, length);
                    buffer->clear();
                    ++requests;
                    respond(socket);
                });
            }
        });
    }

    bool listen() { return server_.listen(QHostAddress::LocalHost, 0); }
    QString baseUrl() const { return QStringLiteral("http://127.0.0.1:%1/v1").arg(server_.serverPort()); }

private:
    void respond(QTcpSocket* socket)
    {
        if (hang) {
            return;
        }
        socket->write(QByteArray("HTTP/1.1 ") + QByteArray::number(status) + " Test\r\n"
                      + "Content-Type: " + contentType + "\r\nConnection: close\r\n\r\n");
        QPointer<QTcpSocket> guarded(socket);
        for (int index = 0; index < chunks.size(); ++index) {
            const QByteArray chunk = chunks.at(index);
            const bool last = index == chunks.size() - 1;
            QTimer::singleShot(chunkDelayMs * index, this, [guarded, chunk, last]() {
                if (!guarded) {
                    return;
                }
                guarded->write(chunk);
                guarded->flush();
                if (last) {
                    guarded->disconnectFromHost();
                }
            });
        }
        if (chunks.isEmpty()) {
            socket->disconnectFromHost();
        }
    }

    QTcpServer server_;
};

QByteArray openAiChunk(const QString& text)
{
    const QJsonObject delta{{QStringLiteral("content"), text}};
    const QJsonObject choice{{QStringLiteral("delta"), delta}};
    const QJsonObject event{{QStringLiteral("choices"), QJsonArray{choice}}};
    return "data: " + QJsonDocument(event).toJson(QJsonDocument::Compact) + "\n\n";
}

void configureFor(AppConfig& config, const FakeModelServer& server)
{
    auto& q = config.mutableSettings().question;
    q.apiFormat = QuestionApiFormat::OpenAiCompatible;
    q.apiUrl = server.baseUrl();
    q.apiKey = QStringLiteral("test-key");
    q.model = QStringLiteral("vision-model");
    q.useSystemProxy = false;
}

QImage testCapture(QColor color = Qt::white)
{
    QImage image(420, 180, QImage::Format_RGB32);
    image.fill(color);
    return image;
}

template <typename T>
T* child(QWidget& parent, const char* name)
{
    return parent.findChild<T*>(QString::fromLatin1(name));
}

} // namespace

class QuestionTests : public QObject {
    Q_OBJECT

private slots:
    void systemPromptUsesDefaultAndAppendsScope()
    {
        const QString plain = Question::systemPrompt(QString(), QString());
        QCOMPARE(plain, Question::defaultPrompt());
        QVERIFY(plain.contains(QStringLiteral("不要使用 LaTeX")));

        const QString scoped = Question::systemPrompt(QStringLiteral("  "), QStringLiteral(" 大学物理 "));
        QVERIFY(scoped.startsWith(Question::defaultPrompt()));
        QVERIFY(scoped.contains(QStringLiteral("题目所属领域：大学物理。")));

        const QString custom = Question::systemPrompt(QStringLiteral("只给答案"), QStringLiteral("电路"));
        QVERIFY(custom.startsWith(QStringLiteral("只给答案")));
        QVERIFY(!custom.contains(Question::defaultPrompt()));
        QVERIFY(custom.contains(QStringLiteral("电路")));
    }

    void endpointAcceptsBaseUrlsAndFullEndpoints_data()
    {
        QTest::addColumn<int>("format");
        QTest::addColumn<QString>("input");
        QTest::addColumn<QString>("expected");
        const int openAi = static_cast<int>(QuestionApiFormat::OpenAiCompatible);
        const int anthropic = static_cast<int>(QuestionApiFormat::Anthropic);

        QTest::newRow("openai-default") << openAi << QString()
                                        << QStringLiteral("https://api.openai.com/v1/chat/completions");
        QTest::newRow("openai-v1") << openAi << QStringLiteral("https://api.openai.com/v1/")
                                   << QStringLiteral("https://api.openai.com/v1/chat/completions");
        QTest::newRow("openai-host-only") << openAi << QStringLiteral("http://localhost:11434")
                                          << QStringLiteral("http://localhost:11434/v1/chat/completions");
        QTest::newRow("openai-versioned-prefix")
            << openAi << QStringLiteral("https://dashscope.aliyuncs.com/compatible-mode/v1")
            << QStringLiteral("https://dashscope.aliyuncs.com/compatible-mode/v1/chat/completions");
        QTest::newRow("openai-full") << openAi
                                     << QStringLiteral("https://ark.cn-beijing.volces.com/api/v3/chat/completions")
                                     << QStringLiteral("https://ark.cn-beijing.volces.com/api/v3/chat/completions");
        QTest::newRow("openai-no-scheme") << openAi << QStringLiteral("api.moonshot.cn/v1")
                                          << QStringLiteral("https://api.moonshot.cn/v1/chat/completions");
        QTest::newRow("anthropic-default") << anthropic << QString()
                                           << QStringLiteral("https://api.anthropic.com/v1/messages");
        QTest::newRow("anthropic-host") << anthropic << QStringLiteral("https://api.anthropic.com")
                                        << QStringLiteral("https://api.anthropic.com/v1/messages");
        QTest::newRow("anthropic-v1") << anthropic << QStringLiteral("https://api.anthropic.com/v1")
                                      << QStringLiteral("https://api.anthropic.com/v1/messages");
        QTest::newRow("anthropic-prefix") << anthropic << QStringLiteral("https://api.example.com/anthropic")
                                          << QStringLiteral("https://api.example.com/anthropic/v1/messages");
        QTest::newRow("anthropic-full") << anthropic << QStringLiteral("https://proxy.example.com/v1/messages/")
                                        << QStringLiteral("https://proxy.example.com/v1/messages");
    }

    void endpointAcceptsBaseUrlsAndFullEndpoints()
    {
        QFETCH(int, format);
        QFETCH(QString, input);
        QFETCH(QString, expected);
        const QUrl url = Question::endpointUrl(static_cast<QuestionApiFormat>(format), input);
        QCOMPARE(url.toString(), expected);
    }

    void endpointRejectsUnusableAddresses()
    {
        QVERIFY(!Question::endpointUrl(QuestionApiFormat::OpenAiCompatible,
                                       QStringLiteral("ftp://example.com/v1")).isValid());
        QVERIFY(!Question::endpointUrl(QuestionApiFormat::OpenAiCompatible,
                                       QStringLiteral("https://")).isValid());
    }

    void modelDefaultsAndFallbackOnlyForOfficialAnthropicApi()
    {
        QCOMPARE(Question::effectiveModel(QuestionApiFormat::Anthropic, QStringLiteral(" ")),
                 Question::anthropicDefaultModel());
        QVERIFY(Question::effectiveModel(QuestionApiFormat::OpenAiCompatible, QString()).isEmpty());
        QCOMPARE(Question::effectiveModel(QuestionApiFormat::OpenAiCompatible, QStringLiteral(" qwen-vl-max ")),
                 QStringLiteral("qwen-vl-max"));

        const QUrl official(QStringLiteral("https://api.anthropic.com/v1/messages"));
        const QUrl proxy(QStringLiteral("https://api.example.com/anthropic/v1/messages"));
        QVERIFY(Question::usesServerSideFallback(official, QStringLiteral("claude-opus-5-5")));
        QVERIFY(Question::usesServerSideFallback(official, QStringLiteral("claude-sonnet-5-5")));
        QVERIFY(!Question::usesServerSideFallback(official, QStringLiteral("claude-haiku-4-5")));
        QVERIFY(!Question::usesServerSideFallback(proxy, QStringLiteral("claude-opus-5-5")));
    }

    void openAiRequestCarriesPromptAndImage()
    {
        Question::RequestInput input;
        input.format = QuestionApiFormat::OpenAiCompatible;
        input.apiUrl = QStringLiteral("https://api.example.com/v1");
        input.apiKey = QStringLiteral(" sk-test ");
        input.model = QStringLiteral("vision-model");
        input.systemPrompt = QStringLiteral("系统提示");
        input.userText = Question::userInstruction();
        input.image = sampleImage();
        QVERIFY(!input.image.isNull());

        const Question::HttpRequest request = Question::buildRequest(input);
        QCOMPARE(request.url.toString(), QStringLiteral("https://api.example.com/v1/chat/completions"));
        QCOMPARE(header(request, "Authorization"), QByteArray("Bearer sk-test"));
        QCOMPARE(header(request, "Accept"), QByteArray("text/event-stream"));
        QVERIFY(header(request, "x-api-key").isEmpty());

        const QJsonObject body = bodyObject(request);
        QCOMPARE(body.value(QStringLiteral("model")).toString(), QStringLiteral("vision-model"));
        QVERIFY(body.value(QStringLiteral("stream")).toBool());
        QVERIFY(!body.contains(QStringLiteral("max_tokens")));
        const QJsonArray messages = body.value(QStringLiteral("messages")).toArray();
        QCOMPARE(messages.size(), 2);
        QCOMPARE(messages.at(0).toObject().value(QStringLiteral("role")).toString(), QStringLiteral("system"));
        QCOMPARE(messages.at(0).toObject().value(QStringLiteral("content")).toString(), QStringLiteral("系统提示"));
        const QJsonArray content = messages.at(1).toObject().value(QStringLiteral("content")).toArray();
        QCOMPARE(content.size(), 2);
        const QString dataUrl = content.at(0).toObject().value(QStringLiteral("image_url")).toObject()
                                    .value(QStringLiteral("url")).toString();
        QVERIFY(dataUrl.startsWith(QStringLiteral("data:image/png;base64,")));
        QCOMPARE(QByteArray::fromBase64(dataUrl.mid(22).toLatin1()), input.image.data);
        QCOMPARE(content.at(1).toObject().value(QStringLiteral("text")).toString(), Question::userInstruction());
    }

    void openAiRequestWithoutKeySendsNoAuthorization()
    {
        Question::RequestInput input;
        input.apiUrl = QStringLiteral("http://localhost:11434");
        input.model = QStringLiteral("llava");
        input.image = sampleImage();
        QVERIFY(header(Question::buildRequest(input), "Authorization").isEmpty());
    }

    void anthropicRequestPutsImageFirstAndGatesFallback()
    {
        Question::RequestInput input;
        input.format = QuestionApiFormat::Anthropic;
        input.apiKey = QStringLiteral("sk-ant-test");
        input.systemPrompt = QStringLiteral("系统提示");
        input.userText = Question::userInstruction();
        input.image = sampleImage();

        const Question::HttpRequest official = Question::buildRequest(input);
        QCOMPARE(official.url.toString(), QStringLiteral("https://api.anthropic.com/v1/messages"));
        QCOMPARE(header(official, "x-api-key"), QByteArray("sk-ant-test"));
        QCOMPARE(header(official, "anthropic-version"), QByteArray("2023-06-01"));
        QCOMPARE(header(official, "anthropic-beta"), QByteArray("server-side-fallback-2026-07-01"));
        QVERIFY(header(official, "Authorization").isEmpty());
        const QJsonObject body = bodyObject(official);
        QCOMPARE(body.value(QStringLiteral("model")).toString(), QStringLiteral("claude-opus-5-5"));
        QCOMPARE(body.value(QStringLiteral("fallbacks")).toString(), QStringLiteral("default"));
        QCOMPARE(body.value(QStringLiteral("system")).toString(), QStringLiteral("系统提示"));
        QVERIFY(body.value(QStringLiteral("max_tokens")).toInt() >= 4096);
        QVERIFY(!body.contains(QStringLiteral("thinking")));
        const QJsonArray content = body.value(QStringLiteral("messages")).toArray().at(0).toObject()
                                       .value(QStringLiteral("content")).toArray();
        QCOMPARE(content.size(), 2);
        const QJsonObject image = content.at(0).toObject();
        QCOMPARE(image.value(QStringLiteral("type")).toString(), QStringLiteral("image"));
        const QJsonObject source = image.value(QStringLiteral("source")).toObject();
        QCOMPARE(source.value(QStringLiteral("type")).toString(), QStringLiteral("base64"));
        QCOMPARE(source.value(QStringLiteral("media_type")).toString(), QStringLiteral("image/png"));
        QCOMPARE(QByteArray::fromBase64(source.value(QStringLiteral("data")).toString().toLatin1()),
                 input.image.data);
        QCOMPARE(content.at(1).toObject().value(QStringLiteral("type")).toString(), QStringLiteral("text"));

        input.apiUrl = QStringLiteral("https://api.example.com/anthropic");
        input.model = QStringLiteral("claude-opus-5-5");
        const Question::HttpRequest proxied = Question::buildRequest(input);
        QVERIFY(header(proxied, "anthropic-beta").isEmpty());
        QVERIFY(!bodyObject(proxied).contains(QStringLiteral("fallbacks")));
    }

    void openAiStreamSurvivesArbitraryChunking()
    {
        const QByteArray stream =
            "data: {\"choices\":[{\"delta\":{\"role\":\"assistant\",\"reasoning_content\":\"想一想\"}}]}\r\n\r\n"
            ": keep-alive\n\n"
            "data: {\"choices\":[{\"delta\":{\"content\":\"答案\"}}]}\n\n"
            "data: {\"choices\":[{\"delta\":{\"content\":\"：B\"},\"finish_reason\":null}]}\n\n"
            "data: {\"choices\":[{\"delta\":{},\"finish_reason\":\"stop\"}]}\n\n"
            "data: [DONE]\n\n";
        for (int step : {1, 3, 7, 64, static_cast<int>(stream.size())}) {
            Question::StreamParser parser(QuestionApiFormat::OpenAiCompatible);
            for (int offset = 0; offset < stream.size(); offset += step) {
                parser.feed(stream.mid(offset, step));
            }
            parser.finish();
            QCOMPARE(parser.text(), QStringLiteral("答案：B"));
            QVERIFY(parser.reasoning());
            QVERIFY(parser.finished());
            QVERIFY(!parser.refused());
            QVERIFY(!parser.truncated());
            QVERIFY(parser.errorMessage().isEmpty());
        }
    }

    void openAiStreamReportsLengthAndErrors()
    {
        Question::StreamParser truncated(QuestionApiFormat::OpenAiCompatible);
        truncated.feed("data: {\"choices\":[{\"delta\":{\"content\":\"部分\"},\"finish_reason\":\"length\"}]}\n");
        truncated.finish();
        QVERIFY(truncated.truncated());
        QCOMPARE(truncated.text(), QStringLiteral("部分"));

        Question::StreamParser failed(QuestionApiFormat::OpenAiCompatible);
        failed.feed("data: {\"error\":{\"message\":\"model does not support image input\"}}\n\n");
        failed.finish();
        QCOMPARE(failed.errorMessage(), QStringLiteral("model does not support image input"));
    }

    void anthropicStreamCollectsTextAndFlagsRefusal()
    {
        const QByteArray stream =
            "event: message_start\n"
            "data: {\"type\":\"message_start\",\"message\":{\"id\":\"msg_1\",\"content\":[]}}\n\n"
            "event: content_block_start\n"
            "data: {\"type\":\"content_block_start\",\"index\":0,\"content_block\":{\"type\":\"thinking\",\"thinking\":\"\"}}\n\n"
            "event: content_block_delta\n"
            "data: {\"type\":\"content_block_delta\",\"index\":0,\"delta\":{\"type\":\"thinking_delta\",\"thinking\":\"\"}}\n\n"
            "event: ping\n"
            "data: {\"type\":\"ping\"}\n\n"
            "event: content_block_start\n"
            "data: {\"type\":\"content_block_start\",\"index\":1,\"content_block\":{\"type\":\"text\",\"text\":\"\"}}\n\n"
            "event: content_block_delta\n"
            "data: {\"type\":\"content_block_delta\",\"index\":1,\"delta\":{\"type\":\"text_delta\",\"text\":\"x = \"}}\n\n"
            "event: content_block_delta\n"
            "data: {\"type\":\"content_block_delta\",\"index\":1,\"delta\":{\"type\":\"text_delta\",\"text\":\"2\"}}\n\n"
            "event: message_delta\n"
            "data: {\"type\":\"message_delta\",\"delta\":{\"stop_reason\":\"end_turn\"}}\n\n"
            "event: message_stop\n"
            "data: {\"type\":\"message_stop\"}\n\n";
        Question::StreamParser parser(QuestionApiFormat::Anthropic);
        parser.feed(stream);
        parser.finish();
        QCOMPARE(parser.text(), QStringLiteral("x = 2"));
        QVERIFY(parser.reasoning());
        QVERIFY(!parser.refused());

        Question::StreamParser refused(QuestionApiFormat::Anthropic);
        refused.feed("data: {\"type\":\"content_block_delta\",\"index\":0,\"delta\":{\"type\":\"text_delta\",\"text\":\"部分\"}}\n"
                     "data: {\"type\":\"message_delta\",\"delta\":{\"stop_reason\":\"refusal\"}}\n");
        refused.finish();
        QVERIFY(refused.refused());

        Question::StreamParser failed(QuestionApiFormat::Anthropic);
        failed.feed("event: error\ndata: {\"type\":\"error\",\"error\":{\"type\":\"overloaded_error\",\"message\":\"Overloaded\"}}\n\n");
        failed.finish();
        QCOMPARE(failed.errorMessage(), QStringLiteral("Overloaded"));
    }

    void nonStreamingBodiesAreStillUnderstood()
    {
        Question::StreamParser openAi(QuestionApiFormat::OpenAiCompatible);
        openAi.feed("{\"choices\":[{\"message\":{\"role\":\"assistant\",\"content\":\"答案：C\"},"
                    "\"finish_reason\":\"stop\"}]}");
        openAi.finish();
        QCOMPARE(openAi.text(), QStringLiteral("答案：C"));
        QVERIFY(openAi.errorMessage().isEmpty());

        Question::StreamParser anthropic(QuestionApiFormat::Anthropic);
        anthropic.feed("{\"type\":\"message\",\"content\":[{\"type\":\"text\",\"text\":\"答\"},"
                       "{\"type\":\"text\",\"text\":\"案\"}],\"stop_reason\":\"max_tokens\"}");
        anthropic.finish();
        QCOMPARE(anthropic.text(), QStringLiteral("答案"));
        QVERIFY(anthropic.truncated());

        Question::StreamParser garbage(QuestionApiFormat::OpenAiCompatible);
        garbage.feed("<html>bad gateway</html>");
        garbage.finish();
        QVERIFY(!garbage.errorMessage().isEmpty());

        const Question::CompleteResponse wrapped = Question::parseCompleteResponse(
            "{\"code\":0,\"message\":\"success\",\"choices\":[{\"message\":{\"content\":\"好\"}}]}");
        QCOMPARE(wrapped.text, QStringLiteral("好"));
        QVERIFY(wrapped.error.isEmpty());
    }

    void httpErrorsBecomeReadableMessages()
    {
        const QString unauthorized = Question::describeHttpError(
            401, "{\"error\":{\"message\":\"Incorrect API key provided\",\"type\":\"invalid_request_error\"}}");
        QVERIFY(unauthorized.contains(QStringLiteral("API Key")));
        QVERIFY(unauthorized.contains(QStringLiteral("HTTP 401")));
        QVERIFY(unauthorized.contains(QStringLiteral("Incorrect API key provided")));

        const QString anthropic = Question::describeHttpError(
            404, "{\"type\":\"error\",\"error\":{\"type\":\"not_found_error\",\"message\":\"model: nope\"}}");
        QVERIFY(anthropic.contains(QStringLiteral("模型不存在")));
        QVERIFY(anthropic.contains(QStringLiteral("model: nope")));

        const QString vision = Question::describeHttpError(
            400, "{\"error\":{\"message\":\"This model does not support image input\"}}");
        QVERIFY(vision.contains(QStringLiteral("不支持图片")));

        const QString html = Question::describeHttpError(502, "<html><body>Bad Gateway</body></html>");
        QCOMPARE(html, QStringLiteral("模型服务暂时不可用（HTTP 502）"));
    }

    void unicodeMathRewritesCommonLatex_data()
    {
        QTest::addColumn<QString>("input");
        QTest::addColumn<QString>("expected");
        QTest::newRow("fraction") << QStringLiteral("结果 $\\frac{1}{2}$ 即可")
                                  << QStringLiteral("结果 1/2 即可");
        QTest::newRow("compound-fraction") << QStringLiteral("$\\dfrac{x+1}{2a}$")
                                           << QStringLiteral("(x+1)/(2a)");
        QTest::newRow("power-and-root") << QStringLiteral("$x^2 + \\sqrt{3} = y^{10}$")
                                        << QStringLiteral("x² + √3 = y¹⁰");
        QTest::newRow("root-of-sum") << QStringLiteral("$\\sqrt{b^2-4ac}$")
                                     << QStringLiteral("√(b²-4ac)");
        QTest::newRow("cube-root") << QStringLiteral("$\\sqrt[3]{8}$") << QStringLiteral("∛8");
        QTest::newRow("greek-and-ops") << QStringLiteral("\\(2\\pi r \\times h \\le \\alpha\\)")
                                       << QStringLiteral("2πr × h ≤ α");
        QTest::newRow("subscripts") << QStringLiteral("$a_1 + a_{n}$") << QStringLiteral("a₁ + aₙ");
        QTest::newRow("mapped-subscript") << QStringLiteral("$v_{max}$") << QStringLiteral("vₘₐₓ");
        QTest::newRow("unmappable-subscript") << QStringLiteral("$v_{avg}$")
                                              << QStringLiteral("v\\_(avg)");
        QTest::newRow("degrees") << QStringLiteral("$30^\\circ$") << QStringLiteral("30°");
        QTest::newRow("text-and-left-right")
            << QStringLiteral("$\\left(\\text{速度}\\right)$") << QStringLiteral("(速度)");
        QTest::newRow("boxed") << QStringLiteral("答案 $\\boxed{42}$") << QStringLiteral("答案 **42**");
        QTest::newRow("functions") << QStringLiteral("$\\sin x + \\log_2 8$")
                                   << QStringLiteral("sin x + log₂ 8");
        QTest::newRow("display") << QStringLiteral("解：\n$$\nx = \\frac{-b}{2a}\n$$\n完")
                                 << QStringLiteral("解：\n\n\nx = (-b)/(2a)\n\n\n完");
        QTest::newRow("aligned-rows")
            << QStringLiteral("\\[\\begin{aligned} a &= 1 \\\\ b &= 2 \\end{aligned}\\]")
            << QStringLiteral("\n\na = 1  \nb = 2\n\n");
        QTest::newRow("blackboard") << QStringLiteral("$x \\in \\mathbb{R}$") << QStringLiteral("x ∈ ℝ");
        QTest::newRow("vector") << QStringLiteral("$\\vec{F}$") << (QStringLiteral("F") + QChar(0x20D7));
    }

    void unicodeMathRewritesCommonLatex()
    {
        QFETCH(QString, input);
        QFETCH(QString, expected);
        QCOMPARE(Question::unicodeMath(input), expected);
    }

    void unicodeMathLeavesCodeAndPricesAlone()
    {
        const QString prices = QStringLiteral("苹果 $5 和梨 $10，共 $15");
        QCOMPARE(Question::unicodeMath(prices), prices);

        const QString inlineCode = QStringLiteral("用 `$x^2$` 表示");
        QCOMPARE(Question::unicodeMath(inlineCode), inlineCode);

        const QString fenced = QStringLiteral("```latex\n$\\frac{1}{2}$\n```\n外面 $\\frac{1}{2}$");
        QCOMPARE(Question::unicodeMath(fenced),
                 QStringLiteral("```latex\n$\\frac{1}{2}$\n```\n外面 1/2"));

        const QString escaped = QStringLiteral("价格 \\$3");
        QCOMPARE(Question::unicodeMath(escaped), escaped);
    }

    void encodeImageKeepsSizeReasonable()
    {
        QImage large(3000, 1500, QImage::Format_ARGB32);
        large.fill(Qt::white);
        const Question::EncodedImage reduced = Question::encodeImage(large);
        QCOMPARE(reduced.size, QSize(2000, 1000));
        QCOMPARE(reduced.mimeType, QStringLiteral("image/png"));
        QCOMPARE(QImage::fromData(reduced.data).size(), QSize(2000, 1000));

        QImage tiny(120, 30, QImage::Format_RGB32);
        tiny.fill(Qt::black);
        QCOMPARE(Question::encodeImage(tiny).size, QSize(240, 60));

        QImage regular(800, 600, QImage::Format_RGB32);
        regular.fill(Qt::gray);
        QCOMPARE(Question::encodeImage(regular).size, QSize(800, 600));

        QVERIFY(Question::encodeImage(QImage()).isNull());
    }

    void questionSettingsRoundTripAndProtectKey()
    {
        QTemporaryDir temp;
        QVERIFY(temp.isValid());
        const QString file = temp.filePath(QStringLiteral("settings.ini"));
        qputenv("VISNIP_TEST_SETTINGS_FILE", QFile::encodeName(file));

        {
            AppConfig config;
            config.load();
            const QuestionSettings defaults;
            QCOMPARE(config.settings().question.apiFormat, defaults.apiFormat);
            QCOMPARE(config.settings().question.timeoutSeconds, 120);
            QVERIFY(config.settings().question.useSystemProxy);
            QCOMPARE(config.settings().hotkeys.askQuestion, QKeySequence(Qt::Key_F4));

            auto& q = config.mutableSettings().question;
            q.apiFormat = QuestionApiFormat::Anthropic;
            q.apiUrl = QStringLiteral("https://api.example.com/anthropic");
            q.apiKey = QStringLiteral("secret-key");
            q.model = QStringLiteral("claude-sonnet-5-5");
            q.customPrompt = QStringLiteral("第一行\n第二行");
            q.subjectScope = QStringLiteral("高中数学");
            q.timeoutSeconds = 5000;
            q.useSystemProxy = false;
            config.mutableSettings().hotkeys.askQuestion = QKeySequence(Qt::CTRL | Qt::Key_F4);
            QVERIFY(config.save());
        }

        {
            QSettings raw(file, QSettings::IniFormat);
            const QString storedKey = raw.value(QStringLiteral("question/apiKey")).toString();
#ifdef Q_OS_WIN
            QVERIFY(storedKey.startsWith(QStringLiteral("dpapi:")));
            QVERIFY(!storedKey.contains(QStringLiteral("secret-key")));
#else
            QCOMPARE(storedKey, QStringLiteral("secret-key"));
#endif
            QCOMPARE(raw.value(QStringLiteral("question/apiFormat")).toString(), QStringLiteral("anthropic"));
        }

        {
            AppConfig config;
            config.load();
            const auto& q = config.settings().question;
            QCOMPARE(q.apiFormat, QuestionApiFormat::Anthropic);
            QCOMPARE(q.apiUrl, QStringLiteral("https://api.example.com/anthropic"));
            QCOMPARE(q.apiKey, QStringLiteral("secret-key"));
            QCOMPARE(q.model, QStringLiteral("claude-sonnet-5-5"));
            QCOMPARE(q.customPrompt, QStringLiteral("第一行\n第二行"));
            QCOMPARE(q.subjectScope, QStringLiteral("高中数学"));
            QCOMPARE(q.timeoutSeconds, 600);
            QVERIFY(!q.useSystemProxy);
            QCOMPARE(config.settings().hotkeys.askQuestion, QKeySequence(Qt::CTRL | Qt::Key_F4));
        }

        {
            // A plain value written by hand or an older build still loads.
            QSettings raw(file, QSettings::IniFormat);
            raw.setValue(QStringLiteral("question/apiKey"), QStringLiteral("plain-key"));
            raw.sync();
            AppConfig config;
            config.load();
            QCOMPARE(config.settings().question.apiKey, QStringLiteral("plain-key"));
        }
        qunsetenv("VISNIP_TEST_SETTINGS_FILE");
    }

    void serviceStreamsAnswerFromCompatibleServer()
    {
        FakeModelServer server;
        QVERIFY(server.listen());
        server.chunks = {openAiChunk(QStringLiteral("解：")), openAiChunk(QStringLiteral("答案：B")),
                         QByteArray("data: [DONE]\n\n")};
        server.chunkDelayMs = 40;
        AppConfig config;
        configureFor(config, server);
        config.mutableSettings().question.subjectScope = QStringLiteral("高中物理");

        QuestionAnswerService service(&config);
        QSignalSpy started(&service, &QuestionAnswerService::started);
        QSignalSpy progress(&service, &QuestionAnswerService::progress);
        QSignalSpy finished(&service, &QuestionAnswerService::finished);
        QSignalSpy failed(&service, &QuestionAnswerService::failed);
        service.ask(testCapture());
        QVERIFY(service.isBusy());
        QTRY_COMPARE_WITH_TIMEOUT(finished.size(), 1, 10000);
        QCOMPARE(failed.size(), 0);
        QCOMPARE(started.size(), 1);
        QVERIFY(progress.size() >= 2);
        QCOMPARE(finished.first().at(0).toString(), QStringLiteral("解：答案：B"));
        QVERIFY(!finished.first().at(1).toBool());
        QVERIFY(!service.isBusy());

        QVERIFY(server.requestHead.startsWith("POST /v1/chat/completions "));
        QVERIFY(server.requestHead.toLower().contains("authorization: bearer test-key"));
        const QJsonObject body = QJsonDocument::fromJson(server.requestBody).object();
        QCOMPARE(body.value(QStringLiteral("model")).toString(), QStringLiteral("vision-model"));
        const QJsonArray messages = body.value(QStringLiteral("messages")).toArray();
        QVERIFY(messages.at(0).toObject().value(QStringLiteral("content")).toString()
                    .contains(QStringLiteral("高中物理")));
        const QString dataUrl = messages.at(1).toObject().value(QStringLiteral("content")).toArray()
                                    .at(0).toObject().value(QStringLiteral("image_url")).toObject()
                                    .value(QStringLiteral("url")).toString();
        const QImage sent = QImage::fromData(QByteArray::fromBase64(dataUrl.section(QLatin1Char(','), 1).toLatin1()));
        QCOMPARE(sent.size(), QSize(420, 180));
    }

    void serviceExplainsHttpAndStreamErrors()
    {
        FakeModelServer server;
        QVERIFY(server.listen());
        server.status = 401;
        server.contentType = "application/json";
        server.chunks = {QByteArray("{\"error\":{\"message\":\"Invalid API key\"}}")};
        AppConfig config;
        configureFor(config, server);
        QuestionAnswerService service(&config);
        QSignalSpy failed(&service, &QuestionAnswerService::failed);
        service.ask(testCapture());
        QTRY_COMPARE_WITH_TIMEOUT(failed.size(), 1, 10000);
        QVERIFY(failed.first().at(0).toString().contains(QStringLiteral("API Key")));
        QVERIFY(failed.first().at(0).toString().contains(QStringLiteral("Invalid API key")));

        server.status = 200;
        server.contentType = "text/event-stream";
        server.chunks = {QByteArray("data: {\"choices\":[{\"delta\":{\"content\":\"部分\"},"
                                    "\"finish_reason\":\"content_filter\"}]}\n\n")};
        failed.clear();
        service.ask(testCapture());
        QTRY_COMPARE_WITH_TIMEOUT(failed.size(), 1, 10000);
        QVERIFY(failed.first().at(0).toString().contains(QStringLiteral("拒绝")));
    }

    void serviceRejectsMissingConfigurationWithoutNetwork()
    {
        AppConfig config;
        QuestionAnswerService service(&config);
        QSignalSpy failed(&service, &QuestionAnswerService::failed);
        QSignalSpy started(&service, &QuestionAnswerService::started);
        service.ask(testCapture());
        QCOMPARE(failed.size(), 1);
        QCOMPARE(started.size(), 0);
        QVERIFY(failed.first().at(0).toString().contains(QStringLiteral("还没有配置")));

        QuestionSettings settings;
        settings.apiUrl = QStringLiteral("https://api.example.com/v1");
        QVERIFY(QuestionAnswerService::configurationProblem(settings).contains(QStringLiteral("模型")));
        settings.model = QStringLiteral("m");
        QVERIFY(QuestionAnswerService::configurationProblem(settings).isEmpty());
        settings.apiFormat = QuestionApiFormat::Anthropic;
        QVERIFY(QuestionAnswerService::configurationProblem(settings).contains(QStringLiteral("API Key")));
        settings.apiKey = QStringLiteral("k");
        settings.model.clear();
        QVERIFY(QuestionAnswerService::configurationProblem(settings).isEmpty());
    }

    void serviceCancelIsSilent()
    {
        FakeModelServer server;
        QVERIFY(server.listen());
        server.hang = true;
        AppConfig config;
        configureFor(config, server);
        QuestionAnswerService service(&config);
        QSignalSpy finished(&service, &QuestionAnswerService::finished);
        QSignalSpy failed(&service, &QuestionAnswerService::failed);
        service.ask(testCapture());
        QTRY_COMPARE_WITH_TIMEOUT(server.requests, 1, 10000);
        service.cancel();
        QVERIFY(!service.isBusy());
        QTest::qWait(200);
        QCOMPARE(finished.size(), 0);
        QCOMPARE(failed.size(), 0);
    }

    void panelAsksWithAFreshCaptureAndShowsTheAnswer()
    {
        FakeModelServer server;
        QVERIFY(server.listen());
        server.chunks = {openAiChunk(QStringLiteral("**答案：** $x^2$")), QByteArray("data: [DONE]\n\n")};
        AppConfig config;
        configureFor(config, server);

        QuestionPanel panel(&config);
        panel.setAttribute(Qt::WA_DeleteOnClose, false);
        int grabs = 0;
        QRect grabbedRect;
        panel.setRegionGrabber([&grabs, &grabbedRect](const QRect& rect, QString*) {
            ++grabs;
            grabbedRect = rect;
            return testCapture(Qt::yellow);
        });
        const QRect region(40, 60, 300, 120);
        panel.setRegion(region, testCapture(Qt::gray).scaled(300, 120));
        panel.show();

        auto* ask = child<QPushButton>(panel, "QuestionPanelAsk");
        auto* status = child<QLabel>(panel, "QuestionPanelStatus");
        auto* answer = child<QTextBrowser>(panel, "QuestionPanelAnswer");
        QVERIFY(ask && status && answer);
        QVERIFY(ask->isEnabled());
        QCOMPARE(grabs, 0); // selecting a region never sends anything

        ask->click();
        QCOMPARE(grabs, 1);
        QCOMPARE(grabbedRect, region);
        QCOMPARE(ask->text(), QStringLiteral("停止"));
        QTRY_VERIFY_WITH_TIMEOUT(status->text().startsWith(QStringLiteral("已完成")), 10000);
        QVERIFY(answer->toPlainText().contains(QStringLiteral("答案：")));
        QVERIFY(answer->toPlainText().contains(QStringLiteral("x²")));
        QVERIFY(!answer->toPlainText().contains(QLatin1Char('$')));
        QVERIFY(ask->text().startsWith(QStringLiteral("获取答案")));
        QCOMPARE(child<QLabel>(panel, "QuestionPanelPosition")->text(), QStringLiteral("1/1"));
        QVERIFY(child<QPushButton>(panel, "QuestionPanelCopyButton")->isEnabled());
    }

    void panelKeepsRecentQuestionsBrowsable()
    {
        FakeModelServer server;
        QVERIFY(server.listen());
        AppConfig config;
        configureFor(config, server);
        QuestionPanel panel(&config);
        panel.setAttribute(Qt::WA_DeleteOnClose, false);
        panel.setRegionGrabber([](const QRect&, QString*) { return testCapture(); });
        panel.setRegion(QRect(10, 10, 200, 100), QImage());
        panel.show();
        auto* ask = child<QPushButton>(panel, "QuestionPanelAsk");
        auto* status = child<QLabel>(panel, "QuestionPanelStatus");
        auto* answer = child<QTextBrowser>(panel, "QuestionPanelAnswer");
        auto* position = child<QLabel>(panel, "QuestionPanelPosition");

        for (const QString& text : {QStringLiteral("第一题的答案"), QStringLiteral("第二题的答案")}) {
            server.chunks = {openAiChunk(text), QByteArray("data: [DONE]\n\n")};
            ask->click();
            QTRY_VERIFY_WITH_TIMEOUT(status->text().startsWith(QStringLiteral("已完成")), 10000);
        }
        QCOMPARE(position->text(), QStringLiteral("2/2"));
        QVERIFY(answer->toPlainText().contains(QStringLiteral("第二题")));
        const QList<QToolButton*> history = panel.findChildren<QToolButton*>(QStringLiteral("QuestionPanelHistoryButton"));
        QCOMPARE(history.size(), 2);
        history.at(0)->click();
        QCOMPARE(position->text(), QStringLiteral("1/2"));
        QVERIFY(answer->toPlainText().contains(QStringLiteral("第一题")));
        QVERIFY(!history.at(0)->isEnabled());
        QVERIFY(history.at(1)->isEnabled());
    }

    void panelStopEndsTheRequest()
    {
        FakeModelServer server;
        QVERIFY(server.listen());
        server.hang = true;
        AppConfig config;
        configureFor(config, server);
        QuestionPanel panel(&config);
        panel.setAttribute(Qt::WA_DeleteOnClose, false);
        panel.setRegionGrabber([](const QRect&, QString*) { return testCapture(); });
        panel.setRegion(QRect(10, 10, 200, 100), QImage());
        panel.show();
        auto* ask = child<QPushButton>(panel, "QuestionPanelAsk");
        ask->click();
        QTRY_COMPARE_WITH_TIMEOUT(server.requests, 1, 10000);
        QCOMPARE(ask->text(), QStringLiteral("停止"));
        ask->click();
        QVERIFY(ask->text().startsWith(QStringLiteral("获取答案")));
        QCOMPARE(child<QLabel>(panel, "QuestionPanelStatus")->text(), QStringLiteral("已停止。"));
        QVERIFY(!panel.service()->isBusy());
    }

    void panelPointsToSettingsWhenUnconfigured()
    {
        AppConfig config;
        QuestionPanel panel(&config);
        panel.setAttribute(Qt::WA_DeleteOnClose, false);
        int grabs = 0;
        panel.setRegionGrabber([&grabs](const QRect&, QString*) { ++grabs; return testCapture(); });
        panel.setRegion(QRect(10, 10, 200, 100), QImage());
        panel.show();
        QSignalSpy settings(&panel, &QuestionPanel::settingsRequested);
        auto* status = child<QLabel>(panel, "QuestionPanelStatus");
        auto* openSettings = child<QPushButton>(panel, "QuestionPanelStatusSettings");
        QVERIFY(status->text().contains(QStringLiteral("还没有配置")));
        QVERIFY(openSettings->isVisible());
        child<QPushButton>(panel, "QuestionPanelAsk")->click();
        QCOMPARE(grabs, 0);
        openSettings->click();
        QCOMPARE(settings.size(), 1);
        child<QToolButton>(panel, "QuestionPanelSettingsButton")->click();
        QCOMPARE(settings.size(), 2);
    }

    void panelStaysClearOfTheRegion()
    {
        AppConfig config;
        QuestionPanel panel(&config);
        panel.setAttribute(Qt::WA_DeleteOnClose, false);
        const QRect screen = QGuiApplication::primaryScreen()->availableGeometry();
        const QRect rightSide(screen.right() - screen.width() / 4, screen.top() + 40,
                              screen.width() / 5, screen.height() / 3);
        panel.setRegion(rightSide, QImage());
        QVERIFY(!panel.geometry().intersects(rightSide));
        QVERIFY(panel.geometry().left() < rightSide.left());

        const QRect leftSide(screen.left() + 20, screen.top() + 40, screen.width() / 5, screen.height() / 3);
        panel.setRegion(leftSide, QImage());
        QVERIFY(!panel.geometry().intersects(leftSide));
        QVERIFY(panel.geometry().left() > leftSide.right());
        QVERIFY(screen.contains(panel.geometry()));
    }

    void secretStorageRoundTrips()
    {
        QVERIFY(protectSecretForStorage(QString()).isEmpty());
        QCOMPARE(secretFromStorage(protectSecretForStorage(QStringLiteral("k-1"))), QStringLiteral("k-1"));
        QCOMPARE(secretFromStorage(QStringLiteral("legacy")), QStringLiteral("legacy"));
#ifndef Q_OS_WIN
        // Encrypted for another machine/user: unusable, never passed through.
        QVERIFY(secretFromStorage(QStringLiteral("dpapi:AAAA")).isEmpty());
#endif
    }
};

QTEST_MAIN(QuestionTests)
#include "tst_question.moc"
