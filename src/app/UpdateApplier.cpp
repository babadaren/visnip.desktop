#include "app/UpdateApplier.h"
#include "core/AppUpdate.h"
#include "core/PerfLog.h"

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QMessageBox>
#include <QProcess>

#ifdef Q_OS_WIN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

namespace Visnip {
namespace {
bool waitForExit(qint64 pid)
{
#ifdef Q_OS_WIN
    if (pid <= 0) return true;
    HANDLE process = OpenProcess(SYNCHRONIZE, FALSE, static_cast<DWORD>(pid));
    if (!process) return true; // already gone
    const DWORD result = WaitForSingleObject(process, 60000);
    CloseHandle(process);
    return result == WAIT_OBJECT_0;
#else
    Q_UNUSED(pid);
    return true;
#endif
}
}

int runUpdateApplier(const QString& target, qint64 waitPid)
{
    const QString source = QCoreApplication::applicationDirPath();
    const QString directory = QDir::fromNativeSeparators(target);
    // Automated checks: no dialogs and no restart, the exit code tells the result.
    const bool quiet = qEnvironmentVariableIsSet("VISNIP_APPLY_UPDATE_QUIET");
    const auto warn = [quiet](const QString& title, const QString& text) {
        if (!quiet) QMessageBox::warning(nullptr, title, text);
    };
    Perf::log(QStringLiteral("update.apply.start target=%1").arg(directory));
    if (directory.isEmpty() || !QFileInfo(QDir(directory).filePath(QStringLiteral("visnip.exe"))).isFile()) {
        warn(QStringLiteral("Visnip 更新"),
                             QStringLiteral("找不到要更新的 Visnip 安装目录，没有做任何改动。"));
        return 2;
    }
    if (!waitForExit(waitPid)) {
        warn(QStringLiteral("Visnip 更新"),
                             QStringLiteral("旧版本 Visnip 没有退出，更新已取消，没有做任何改动。请从托盘退出后，在首选项中重新更新。"));
        return 3;
    }
    QString error;
    const bool applied = AppUpdate::applyPackage(source, directory, &error);
    Perf::log(QStringLiteral("update.apply.%1 %2").arg(applied ? QStringLiteral("done") : QStringLiteral("failed"), error));
    if (!applied) {
        warn(QStringLiteral("Visnip 更新失败"),
                             QStringLiteral("%1\n\n已恢复原来的版本。也可以从 GitHub 下载新版本后手动替换。").arg(error));
    }
    if (!quiet) QProcess::startDetached(QDir(directory).filePath(QStringLiteral("visnip.exe")), {}, directory);
    return applied ? 0 : 1;
}
} // namespace Visnip
