#pragma once

#include <QImage>
#include <QObject>
#include <QString>

namespace Visnip {
struct ImageTranslationResult;
class OfflineWorkerSession;

// Small client facade. One application-owned worker is shared across capture,
// provisioning and settings; destroying a capture does not discard idle models.
class OfflineTranslationService final : public QObject {
    Q_OBJECT
public:
    explicit OfflineTranslationService(QObject* parent = nullptr);
    ~OfflineTranslationService() override;
    static QString defaultResourceDirectory();
    static QString resourceProblem(const QString& root, const QString& quality);
    static void prewarm(const QString& root);
    static void releaseSharedEngine();
    static bool sharedEngineReady();
    static qint64 sharedEngineProcessId();
    bool isBusy() const { return busy_; }
    void translate(const QImage& image, const QString& language, const QString& root, const QString& quality);
    void cancel();
signals:
    void succeeded(const Visnip::ImageTranslationResult& result, qint64 elapsedMs);
    void failed(const QString& message);
    void cancelled();
    void phaseChanged(const QString& stage);
private:
    friend class OfflineWorkerSession;
    bool busy_ = false;
};
}
