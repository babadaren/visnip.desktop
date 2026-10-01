#pragma once

#include <QString>

#include <QtGlobal>

namespace Visnip::Perf {

void initialize();
QString logPath();
qint64 elapsedMs();
void log(const QString& message);
void logDuration(const QString& name, qint64 elapsedMs);

class ScopedTimer {
public:
    explicit ScopedTimer(QString name);
    ~ScopedTimer();

    ScopedTimer(const ScopedTimer&) = delete;
    ScopedTimer& operator=(const ScopedTimer&) = delete;

private:
    QString name_;
    qint64 startMs_ = 0;
};

} // namespace Visnip::Perf
