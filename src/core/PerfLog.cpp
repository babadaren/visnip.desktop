#include "core/PerfLog.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QMutex>
#include <QMutexLocker>
#include <QStandardPaths>
#include <QTextStream>
#include <QThread>

#include <utility>

namespace Visnip::Perf {
namespace {

QMutex& logMutex()
{
    static QMutex mutex;
    return mutex;
}

QElapsedTimer& uptimeTimer()
{
    static QElapsedTimer timer;
    return timer;
}

QString& logFilePath()
{
    static QString path;
    return path;
}

QFile& logFile()
{
    static QFile file;
    return file;
}

QTextStream& logStream()
{
    static QTextStream stream(&logFile());
    return stream;
}

int& bufferedLineCount()
{
    static int count = 0;
    return count;
}

bool& initialized()
{
    static bool value = false;
    return value;
}

QString fallbackLogDirectory()
{
    const QString temp = QStandardPaths::writableLocation(QStandardPaths::TempLocation);
    return temp.isEmpty() ? QDir::currentPath() : temp;
}

QString cleanMessage(QString message)
{
    message.replace(QLatin1Char('\r'), QLatin1Char(' '));
    message.replace(QLatin1Char('\n'), QLatin1Char(' '));
    return message;
}

void writeLineUnlocked(const QString& line)
{
    if (!logFile().isOpen()) {
        return;
    }
    logStream() << line << '\n';
    if (++bufferedLineCount() >= 16 || line.contains(QStringLiteral("OfflineEngine."))) {
        logStream().flush();
        bufferedLineCount() = 0;
    }
}

qint64 elapsedMsUnlocked()
{
    return uptimeTimer().isValid() ? uptimeTimer().elapsed() : 0;
}

QString currentThreadText()
{
    return QString::number(reinterpret_cast<quintptr>(QThread::currentThreadId()));
}

} // namespace

void initialize()
{
    QMutexLocker locker(&logMutex());
    if (initialized()) {
        return;
    }

    QString dir = QDir(QCoreApplication::applicationDirPath()).filePath(QStringLiteral("logs"));
    QDir().mkpath(dir);
    QFile probe(QDir(dir).filePath(QStringLiteral(".visnip-write-check")));
    if (probe.open(QIODevice::WriteOnly)) { probe.close(); probe.remove(); }
    else dir = QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation);
    if (dir.isEmpty()) {
        dir = fallbackLogDirectory();
    }
    QDir().mkpath(dir);
    logFilePath() = QDir(dir).filePath(QStringLiteral("visnip-perf.log"));

    if (QFileInfo(logFilePath()).size() > 5 * 1024 * 1024) {
        const QString previous = logFilePath() + QStringLiteral(".1");
        QFile::remove(previous);
        QFile::rename(logFilePath(), previous);
    }

    uptimeTimer().start();
    initialized() = true;

    logFile().setFileName(logFilePath());
    if (!logFile().open(QIODevice::WriteOnly | QIODevice::Append | QIODevice::Text)) {
        return;
    }

    writeLineUnlocked(QStringLiteral("---- Visnip session %1 pid=%2 ----")
                          .arg(QDateTime::currentDateTime().toString(Qt::ISODateWithMs))
                          .arg(QCoreApplication::applicationPid()));
    writeLineUnlocked(QStringLiteral("[%1 ms] perf.log.path %2")
                          .arg(elapsedMsUnlocked(), 6)
                          .arg(logFilePath()));
    logStream().flush();
    bufferedLineCount() = 0;
}

QString logPath()
{
    initialize();
    QMutexLocker locker(&logMutex());
    return logFilePath();
}

qint64 elapsedMs()
{
    initialize();
    QMutexLocker locker(&logMutex());
    return elapsedMsUnlocked();
}

void log(const QString& message)
{
    initialize();
    QMutexLocker locker(&logMutex());
    writeLineUnlocked(QStringLiteral("[%1 ms] tid=%2 %3")
                          .arg(elapsedMsUnlocked(), 6)
                          .arg(currentThreadText())
                          .arg(cleanMessage(message)));
}

void logDuration(const QString& name, qint64 elapsedMs)
{
    log(QStringLiteral("%1 elapsed=%2ms").arg(name).arg(elapsedMs));
}

ScopedTimer::ScopedTimer(QString name)
    : name_(std::move(name))
{
    initialize();
    QMutexLocker locker(&logMutex());
    startMs_ = elapsedMsUnlocked();
    writeLineUnlocked(QStringLiteral("[%1 ms] tid=%2 %3.begin")
                          .arg(startMs_, 6)
                          .arg(currentThreadText())
                          .arg(cleanMessage(name_)));
}

ScopedTimer::~ScopedTimer()
{
    QMutexLocker locker(&logMutex());
    const qint64 now = elapsedMsUnlocked();
    writeLineUnlocked(QStringLiteral("[%1 ms] tid=%2 %3.end elapsed=%4ms")
                          .arg(now, 6)
                          .arg(currentThreadText())
                          .arg(cleanMessage(name_))
                          .arg(now - startMs_));
}

} // namespace Visnip::Perf
