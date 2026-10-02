#pragma once
#include "core/AppUpdate.h"
#include <QByteArray>
#include <QFile>
#include <QObject>
#include <QPointer>
#include <QProcess>
#include <QTimer>

class QNetworkAccessManager;
class QNetworkReply;
namespace Visnip {
class UpdateTests;

// Checks GitHub for a newer published release and, when the user asks,
// replaces this portable installation with it. Nothing is downloaded or
// installed without an explicit click; the check itself sends no data besides
// an ordinary HTTPS request to the GitHub API.
class UpdateService final : public QObject {
    Q_OBJECT
public:
    enum class State { Idle, Checking, UpToDate, Available, Downloading, Preparing, Restarting, Failed };
    Q_ENUM(State)

    explicit UpdateService(QObject* parent = nullptr);
    ~UpdateService() override;
    State state() const { return state_; }
    QString statusText() const { return status_; }
    const AppRelease& release() const { return release_; }
    bool hasUpdate() const { return state_ == State::Available || busyInstalling(); }
    bool busyInstalling() const { return state_ == State::Downloading || state_ == State::Preparing; }
    // Empty when this copy can replace itself; otherwise why it cannot.
    static QString installProblem(const QString& installDirectory = QString());
    void check();
    void install();
    void cancel();
signals:
    void stateChanged(UpdateService::State state);
    void progress(qint64 received, qint64 total);
    // The new version has started and waits for this process to exit.
    void restartRequired();
private:
    friend class UpdateTests;
    void setState(State state, const QString& status);
    void fail(const QString& message);
    void fetchChecksum();
    void downloadPackage();
    void readPackage();
    void packageFinished();
    void verifyPackage();
    void extractPackage();
    void extracted(int exitCode, QProcess::ExitStatus status);
    void selfTested(int exitCode, QProcess::ExitStatus status);
    void releaseReply();
    void stopProcess();
    QString packagePath(bool partial = false) const;
    QString stagingPath() const;
    QNetworkAccessManager* network_ = nullptr;
    QPointer<QNetworkReply> reply_;
    QPointer<QProcess> process_;
    QTimer processTimeout_;
    QFile output_;
    QByteArray body_;
    AppRelease release_;
    QByteArray expectedSha256_;
    QString status_;
    State state_ = State::Idle;
    qint64 offset_ = 0;
    quint64 serial_ = 0;
    bool headersChecked_ = false, rejected_ = false;
};
}
