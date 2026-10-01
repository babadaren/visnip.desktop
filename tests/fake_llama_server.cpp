// Test double for llama.cpp's llama-server. Speaks the small HTTP subset the
// lite offline client uses and returns deterministic "translations", so the
// client's process, auth, concurrency and retry handling can be tested without
// a model. Request statistics go to fake-stats.json beside the model file.
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHostAddress>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QPointer>
#include <QRegularExpression>
#include <QSaveFile>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTimer>

#include <cstdio>

namespace {

struct Stats {
    int active = 0;
    int maxActive = 0;
    int requests = 0;
    int retries = 0;
};

QString argument(const QStringList& arguments, const QString& name)
{
    const int index = arguments.indexOf(name);
    return index >= 0 && index + 1 < arguments.size() ? arguments.at(index + 1) : QString();
}

QString sourceText(const QString& prompt, bool* retry)
{
    const QString contextMarker = QStringLiteral("也不要额外解释：\n");
    const QString retryMarker = QStringLiteral("代码名称。\n\n");
    *retry = false;
    qsizetype at = prompt.lastIndexOf(contextMarker);
    if (at >= 0) {
        return prompt.mid(at + contextMarker.size());
    }
    at = prompt.lastIndexOf(retryMarker);
    *retry = at >= 0;
    return at >= 0 ? prompt.mid(at + retryMarker.size()) : prompt;
}

QString translate(const QString& source, bool retry)
{
    if (source == QStringLiteral("Copilot")) {
        return source; // A product name: kept by both prompts.
    }
    if (source.contains(QStringLiteral("DROPNUM"))) {
        QString value = source;
        value.remove(QRegularExpression(QStringLiteral("[0-9]")));
        return QStringLiteral("丢失数字：") + value;
    }
    if (source.contains(QStringLiteral("ECHO")) && !retry) {
        return source;
    }
    if (source.contains(QStringLiteral("QUOTE"))) {
        return QStringLiteral("“译：") + source + QStringLiteral("”");
    }
    return QStringLiteral("译：") + source;
}

class FakeServer final : public QObject {
public:
    FakeServer(const QString& token, const QString& statsFile)
        : token_(token), statsFile_(statsFile)
    {
        connect(&server_, &QTcpServer::newConnection, this, [this]() {
            while (QTcpSocket* socket = server_.nextPendingConnection()) {
                connect(socket, &QTcpSocket::readyRead, this, [this, socket]() { read(socket); });
                connect(socket, &QTcpSocket::disconnected, socket, &QObject::deleteLater);
            }
        });
    }

    bool listen(quint16 port) { return server_.listen(QHostAddress::LocalHost, port); }

private:
    void read(QTcpSocket* socket)
    {
        QByteArray& buffer = buffers_[socket];
        buffer += socket->readAll();
        for (;;) {
            const qsizetype headerEnd = buffer.indexOf("\r\n\r\n");
            if (headerEnd < 0) {
                return;
            }
            const QList<QByteArray> lines = buffer.left(headerEnd).split('\n');
            qsizetype length = 0;
            QByteArray authorization;
            for (const QByteArray& line : lines.mid(1)) {
                const qsizetype colon = line.indexOf(':');
                const QByteArray name = line.left(colon).trimmed().toLower();
                const QByteArray value = line.mid(colon + 1).trimmed();
                if (name == "content-length") {
                    length = value.toLongLong();
                } else if (name == "authorization") {
                    authorization = value;
                }
            }
            if (buffer.size() < headerEnd + 4 + length) {
                return;
            }
            const QList<QByteArray> request = lines.first().trimmed().split(' ');
            const QByteArray body = buffer.mid(headerEnd + 4, length);
            buffer.remove(0, headerEnd + 4 + length);
            handle(socket, request.value(0), request.value(1), authorization, body);
        }
    }

    void handle(QTcpSocket* socket, const QByteArray& method, const QByteArray& path,
                const QByteArray& authorization, const QByteArray& body)
    {
        if (method == "GET" && path == "/health") {
            respond(socket, 200, QJsonObject{{QStringLiteral("status"), QStringLiteral("ok")}});
            return;
        }
        if (authorization != "Bearer " + token_.toLatin1()) {
            respond(socket, 401, QJsonObject{{QStringLiteral("error"), QStringLiteral("unauthorized")}});
            return;
        }
        if (method != "POST" || path != "/v1/chat/completions") {
            respond(socket, 404, QJsonObject{});
            return;
        }
        const QJsonObject request = QJsonDocument::fromJson(body).object();
        const QString prompt = request.value(QStringLiteral("messages")).toArray().first()
                                   .toObject().value(QStringLiteral("content")).toString();
        bool retry = false;
        const QString source = sourceText(prompt, &retry);
        ++stats_.requests;
        stats_.retries += retry;
        stats_.maxActive = qMax(stats_.maxActive, ++stats_.active);
        writeStats();
        const QString content = translate(source, retry);
        const int delay = source.contains(QStringLiteral("SLOW")) ? 3000 : 80;
        QPointer<QTcpSocket> guarded(socket);
        QTimer::singleShot(delay, this, [this, guarded, content, prompt]() {
            --stats_.active;
            writeStats();
            if (!guarded) {
                return;
            }
            const QJsonObject message{{QStringLiteral("role"), QStringLiteral("assistant")},
                                      {QStringLiteral("content"), content}};
            const QJsonObject choice{{QStringLiteral("message"), message},
                                     {QStringLiteral("finish_reason"), QStringLiteral("stop")}};
            // Timings of a mainstream desktop CPU: 400 tokens/s prefill, 40 decode.
            const int promptTokens = int(prompt.size() / 3);
            const int completionTokens = int(content.size());
            respond(guarded, 200, QJsonObject{
                {QStringLiteral("choices"), QJsonArray{choice}},
                {QStringLiteral("usage"), QJsonObject{{QStringLiteral("prompt_tokens"), promptTokens},
                                                      {QStringLiteral("completion_tokens"), completionTokens}}},
                {QStringLiteral("timings"), QJsonObject{{QStringLiteral("prompt_n"), promptTokens},
                                                        {QStringLiteral("prompt_ms"), promptTokens * 2.5},
                                                        {QStringLiteral("predicted_ms"), completionTokens * 25.0}}},
            });
        });
    }

    void respond(QTcpSocket* socket, int status, const QJsonObject& object)
    {
        const QByteArray body = QJsonDocument(object).toJson(QJsonDocument::Compact);
        const QByteArray reason = status == 200 ? "OK" : status == 401 ? "Unauthorized" : "Not Found";
        socket->write("HTTP/1.1 " + QByteArray::number(status) + ' ' + reason + "\r\n"
                      "Content-Type: application/json\r\n"
                      "Content-Length: " + QByteArray::number(body.size()) + "\r\n"
                      "Connection: keep-alive\r\n\r\n" + body);
    }

    void writeStats()
    {
        QSaveFile file(statsFile_);
        if (file.open(QIODevice::WriteOnly)) {
            file.write(QJsonDocument(QJsonObject{{QStringLiteral("max_active"), stats_.maxActive},
                                                 {QStringLiteral("requests"), stats_.requests},
                                                 {QStringLiteral("retries"), stats_.retries}})
                           .toJson(QJsonDocument::Compact));
            file.commit();
        }
    }

    QTcpServer server_;
    QHash<QTcpSocket*, QByteArray> buffers_;
    QString token_;
    QString statsFile_;
    Stats stats_;
};

} // namespace

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    const QStringList arguments = app.arguments();
    if (arguments.contains(QStringLiteral("--list-devices"))) {
        std::puts("Available devices:");
        return 0;
    }
    const QString model = argument(arguments, QStringLiteral("--model"));
    const QString token = argument(arguments, QStringLiteral("--api-key"));
    const quint16 port = argument(arguments, QStringLiteral("--port")).toUShort();
    if (!QFileInfo(model).isFile() || token.isEmpty() || port == 0
        || argument(arguments, QStringLiteral("--host")) != QStringLiteral("127.0.0.1")
        || !arguments.contains(QStringLiteral("--offline"))) {
        return 2;
    }
    const QString statsFile = QFileInfo(model).dir().filePath(QStringLiteral("fake-stats.json"));
    QSaveFile argumentsFile(QFileInfo(model).dir().filePath(QStringLiteral("fake-arguments.txt")));
    if (argumentsFile.open(QIODevice::WriteOnly)) {
        QStringList recorded = arguments.mid(1);
        recorded.replace(recorded.indexOf(token), QStringLiteral("<token>"));
        argumentsFile.write(recorded.join(QLatin1Char(' ')).toUtf8());
        argumentsFile.commit();
    }
    FakeServer server(token, statsFile);
    if (!server.listen(port)) {
        return 3;
    }
    std::puts("main: server is listening");
    std::fflush(stdout);
    return app.exec();
}
