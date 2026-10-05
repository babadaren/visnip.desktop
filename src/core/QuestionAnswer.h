#pragma once

#include "core/AppConfig.h"

#include <QByteArray>
#include <QImage>
#include <QList>
#include <QPair>
#include <QSize>
#include <QString>
#include <QUrl>

class QJsonObject;

namespace Visnip::Question {

// Built-in instructions used when the user has not written their own prompt.
QString defaultPrompt();
// The custom prompt (or the default one) plus the optional subject scope.
QString systemPrompt(const QString& customPrompt, const QString& subjectScope);
// Text sent next to the screenshot in the user turn.
QString userInstruction();

// Accepts a base URL (with or without /v1) as well as a full endpoint; empty
// input selects the format's official endpoint. Invalid input yields an
// invalid QUrl.
QUrl endpointUrl(QuestionApiFormat format, const QString& rawUrl);
QString anthropicDefaultModel();
// Anthropic falls back to a default model; OpenAI-compatible has none.
QString effectiveModel(QuestionApiFormat format, const QString& model);
// Server-side refusal fallback is only sent to Anthropic's own API for the
// models that accept it; compatible third-party endpoints reject the field.
bool usesServerSideFallback(const QUrl& endpoint, const QString& model);

struct EncodedImage {
    QByteArray data;
    QString mimeType;
    QSize size;

    bool isNull() const { return data.isEmpty(); }
};

// Scales a capture into a size vision models read well (large captures are
// reduced, tiny ones enlarged) and encodes it as PNG, or JPEG when the PNG
// would be too large to upload comfortably.
EncodedImage encodeImage(const QImage& image);

struct HttpRequest {
    QUrl url;
    QList<QPair<QByteArray, QByteArray>> headers;
    QByteArray body;
};

struct RequestInput {
    QuestionApiFormat format = QuestionApiFormat::OpenAiCompatible;
    QString apiUrl;
    QString apiKey;
    QString model;
    QString systemPrompt;
    QString userText;
    EncodedImage image;
};

// Streaming chat request carrying one screenshot and one instruction.
HttpRequest buildRequest(const RequestInput& input);

// Incremental parser for the server-sent events of both formats. A response
// that turns out not to be a stream (a server ignoring "stream") is parsed as
// one complete JSON answer by finish().
class StreamParser {
public:
    explicit StreamParser(QuestionApiFormat format);

    void feed(const QByteArray& chunk);
    void finish();

    const QString& text() const { return text_; }
    // True once the model reported thinking/reasoning output.
    bool reasoning() const { return reasoning_; }
    bool finished() const { return finished_; }
    // The model or a safety system declined; partial text must be discarded.
    bool refused() const { return refused_; }
    // The answer hit the output length limit.
    bool truncated() const { return truncated_; }
    const QString& errorMessage() const { return error_; }

private:
    void processLine(const QByteArray& line);
    void processData(const QByteArray& data);
    void processOpenAi(const QJsonObject& object);
    void processAnthropic(const QJsonObject& object);

    QuestionApiFormat format_;
    QByteArray pending_;
    QByteArray raw_;
    bool sawData_ = false;
    QString text_;
    bool reasoning_ = false;
    bool finished_ = false;
    bool refused_ = false;
    bool truncated_ = false;
    QString error_;
};

struct CompleteResponse {
    QString text;
    QString error;
    bool refused = false;
    bool truncated = false;
};

// Answer carried by a non-streaming response body of either format.
CompleteResponse parseCompleteResponse(const QByteArray& body);
// User-facing explanation of a failed HTTP request.
QString describeHttpError(int status, const QByteArray& body);

// Rewrites common LaTeX inside math delimiters ($...$, $$...$$, \(...\),
// \[...\]) as Unicode text, because the answer view renders Markdown but not
// LaTeX. Code spans and fenced code blocks are left untouched.
QString unicodeMath(const QString& markdown);

// The final answer of a response, as plain text: the part after its last
// "答案：" (also "最终答案", "Answer:", "Final answer:") line, up to the next
// blank line. Markdown emphasis is removed, numbered lines are kept and math
// is rewritten as Unicode. Empty when the response has no such line.
QString finalAnswer(const QString& markdown);

} // namespace Visnip::Question
