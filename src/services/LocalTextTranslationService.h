#pragma once

#include <QElapsedTimer>
#include <QHash>
#include <QObject>
#include <QString>
#include <QStringList>
#include <QVector>

class QNetworkReply;

namespace Visnip {

struct LocalTranslationStats {
    int requests = 0;
    int retries = 0;
    int cacheHits = 0;
    int unresolved = 0;
    int promptTokens = 0;
    int promptProcessed = 0; // prefilled, i.e. not served from the KV cache
    int completionTokens = 0;
    int concurrency = 0;
    double promptMs = 0.0;
    double generationMs = 0.0;
    qint64 waitForEngineMs = 0;
};

// Lite offline tier: text translation through a local llama.cpp server running
// Hy-MT. The server is a child process listening on loopback only, with a
// per-launch API key; no Python, PyTorch or network fallback is involved. One
// application-wide server is shared by every capture and kept warm while idle
// for a bounded time. Interface mirrors TextTranslationService so the capture
// overlay reuses its OCR and composition path unchanged.
class LocalTextTranslationService final : public QObject {
    Q_OBJECT
public:
    explicit LocalTextTranslationService(QObject* parent = nullptr);
    ~LocalTextTranslationService() override;

    static QString resourceProblem(const QString& root);
    // The official llama.cpp Windows build needs the Visual C++ 2015-2022
    // runtime, installed system-wide or placed beside llama-server.exe.
    static QString runtimeProblem(const QString& root = QString());
    static QString serverExecutable(const QString& root);
    static QString modelFile(const QString& root);
    // Starts the shared server in the background when resources exist and the
    // machine has enough free memory. Never downloads anything.
    static void prewarm(const QString& root);
    static void releaseSharedEngine();
    static bool sharedEngineReady();
    static qint64 sharedEngineProcessId();
    // "cpu", "vulkan" or "cuda" once the shared server is ready.
    static QString sharedEngineBackend();

    // Built-in English fixture used by installation and preference self-tests.
    static QStringList selfTestTexts();
    // Empty when every fixture line produced a validated Chinese translation.
    QString selfTestProblem(const QStringList& translations) const;

    bool isBusy() const { return busy_; }
    void translate(const QStringList& texts, const QString& targetLanguage, const QString& root);
    void cancel();

    // Valid after succeeded(): regions that kept their source text because
    // the model output failed validation twice.
    QVector<int> unresolvedIndices() const { return unresolved_.keys().toVector(); }
    const LocalTranslationStats& lastStats() const { return stats_; }

signals:
    void succeeded(const QStringList& translations, qint64 elapsedMs);
    void failed(const QString& message);
    void cancelled();
    void phaseChanged(const QString& phase);
    void progress(int completed, int total);

private:
    struct Task {
        int index = -1;
        bool retry = false;
        QString prompt;
        QString firstPrompt; // cache key, also for a successful retry
        QString firstValue;
        QString firstReason;
    };

    void engineReady(quint64 serial, const QString& error);
    void pump();
    void send(const Task& task);
    void handleReply(QNetworkReply* reply);
    void accept(int index, const QString& value, const QString& prompt);
    void reject(const Task& task, const QString& value, const QString& reason);
    void finishIfDone();
    void finishFailure(const QString& message);
    void abortRequests();

    bool busy_ = false;
    bool holdsEngine_ = false;
    quint64 serial_ = 0;
    QString target_;
    QStringList texts_;
    QStringList results_;
    QString pageContext_;
    QVector<Task> queue_;
    QHash<QNetworkReply*, Task> inflight_;
    QHash<int, QString> unresolved_;
    int concurrency_ = 1;
    int translatedCount_ = 0;
    int pendingTotal_ = 0;
    int completed_ = 0;
    LocalTranslationStats stats_;
    QElapsedTimer timer_;
};

} // namespace Visnip
