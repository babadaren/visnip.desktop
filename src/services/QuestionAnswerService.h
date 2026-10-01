#pragma once

#include "core/AppConfig.h"
#include "core/QuestionAnswer.h"

#include <QByteArray>
#include <QImage>
#include <QObject>
#include <QPointer>
#include <QString>

#include <memory>

class QNetworkAccessManager;
class QNetworkReply;

namespace Visnip {

// Sends a screenshot to the configured vision model and streams the answer.
// One request at a time: a new one cancels the previous.
class QuestionAnswerService : public QObject {
    Q_OBJECT
public:
    explicit QuestionAnswerService(AppConfig* config, QObject* parent = nullptr);
    ~QuestionAnswerService() override;

    // Why the settings cannot be used yet (shown with a link to the settings
    // page), or empty when a request can be made.
    static QString configurationProblem(const QuestionSettings& settings);

    bool isBusy() const;
    // Answers the question shown in `image` using the saved settings.
    void ask(const QImage& image);
    // Sends a tiny image to verify address, key, model and image support.
    void testConnection();
    void cancel();

signals:
    void started();
    // Accumulated answer so far; `reasoning` is true while the model reports
    // thinking and has not written any answer text yet.
    void progress(const QString& answer, bool reasoning);
    void finished(const QString& answer, bool truncated);
    void failed(const QString& message);

private:
    void start(const QImage& image, const QString& systemPrompt, const QString& userText);
    void send(const Question::EncodedImage& image, const QString& systemPrompt,
              const QString& userText);
    void handleReadyRead();
    void handleFinished();
    void releaseReply();

    AppConfig* config_ = nullptr;
    QNetworkAccessManager* network_ = nullptr;
    QPointer<QNetworkReply> reply_;
    std::unique_ptr<Question::StreamParser> parser_;
    QByteArray errorBody_;
    quint64 serial_ = 0;
    bool encoding_ = false;
};

} // namespace Visnip
