#pragma once

#include <QtGui/qwindowdefs.h>

namespace Visnip::Platform {

enum class WindowCaptureExclusion {
    Failed,
    MonitorOnly,
    Excluded,
};

WindowCaptureExclusion applyWindowCaptureExclusion(
    WId windowId,
    bool allowMonitorFallback = true);

} // namespace Visnip::Platform
