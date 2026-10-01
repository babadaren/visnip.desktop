#include "platform/WindowCaptureExclusion.h"

#ifdef Q_OS_WIN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#ifndef WDA_EXCLUDEFROMCAPTURE
#define WDA_EXCLUDEFROMCAPTURE 0x00000011
#endif
#endif

namespace Visnip::Platform {

WindowCaptureExclusion applyWindowCaptureExclusion(
    WId windowId,
    bool allowMonitorFallback)
{
#ifdef Q_OS_WIN
    const HWND hwnd = reinterpret_cast<HWND>(windowId);
    if (!hwnd) {
        return WindowCaptureExclusion::Failed;
    }
    if (SetWindowDisplayAffinity(hwnd, WDA_EXCLUDEFROMCAPTURE)) {
        return WindowCaptureExclusion::Excluded;
    }
    if (allowMonitorFallback
        && SetWindowDisplayAffinity(hwnd, WDA_MONITOR)) {
        return WindowCaptureExclusion::MonitorOnly;
    }
    return WindowCaptureExclusion::Failed;
#else
    Q_UNUSED(windowId)
    Q_UNUSED(allowMonitorFallback)
    return WindowCaptureExclusion::Failed;
#endif
}

} // namespace Visnip::Platform
