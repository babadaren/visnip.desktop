#pragma once
#include <QString>

namespace Visnip {
// Runs in the freshly unpacked new version (visnip.exe --apply-update): waits
// until the old process has exited, replaces the installation in target with
// this package and starts it again. On failure the old files are restored and
// the old version is started.
int runUpdateApplier(const QString& target, qint64 waitPid);
}
