#include "ui/capture/CaptureOverlayWindow.h"
#include "ui/widgets/TranslationReviewPanel.h"

#include "core/DesignTokens.h"
#include "core/OcrLanguagePack.h"
#include "core/PerfLog.h"
#include "platform/WindowCaptureExclusion.h"
#include "services/FastTranslationDiagnostics.h"
#include "services/ImageTranslationService.h"
#include "services/LocalTextTranslationService.h"
#include "services/OfflineTranslationService.h"
#include "services/OcrService.h"
#include "services/TextTranslationService.h"

#include <QScopeGuard>
#include "ui/IconUtils.h"
#include "ui/widgets/CompactToolbar.h"

#include <QApplication>
#include <QCloseEvent>
#include <QComboBox>
#include <QContextMenuEvent>
#include <QCursor>
#include <QElapsedTimer>
#include <QEvent>
#include <QEventLoop>
#include <QFontDatabase>
#include <QFrame>
#include <QFuture>
#include <QFutureWatcher>
#include <QIcon>
#include <QWheelEvent>
#include <QTimer>
#include <QGridLayout>
#include <QHideEvent>
#include <QHBoxLayout>
#include <QLayoutItem>
#include <QToolButton>
#include <QVector>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <condition_variable>
#include <cstring>
#include <functional>
#include <mutex>
#include <optional>
#include <thread>
#include <utility>
#include <QGuiApplication>
#include <QInputMethodEvent>
#include <QInputMethod>
#include <QKeyEvent>
#include <QLabel>
#include <QMenu>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPainterPathStroker>
#include <QPair>
#include <QPixmap>
#include <QRegion>
#include <QScreen>
#include <QSignalBlocker>
#include <QShowEvent>
#include <QSlider>
#include <QStringListModel>
#include <QThread>
#include <QThreadPool>
#include <QToolTip>
#include <QtConcurrentRun>

#ifdef Q_OS_WIN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <dwmapi.h>
#include <windows.h>
#endif

namespace Visnip {

namespace {
QRect allScreensGeometry()
{
    QRect result;
    for (QScreen* screen : QGuiApplication::screens()) {
        result = result.united(screen->geometry());
    }
    return result.isNull() ? QRect(0, 0, 1280, 720) : result;
}

bool screensUseUnitScale(const QRect& geometry)
{
    for (QScreen* screen : QGuiApplication::screens()) {
        if (screen->geometry().intersects(geometry) && !qFuzzyCompare(screen->devicePixelRatio(), 1.0)) {
            return false;
        }
    }
    return true;
}

// True when every screen shares one device pixel ratio. Only then is the
// logical-to-native mapping a single linear scale, which lets the GDI capture
// path (and the async grabber) work on scaled-DPI desktops.
bool allScreensUniformScale(qreal* scaleOut)
{
    qreal scale = 0.0;
    for (QScreen* screen : QGuiApplication::screens()) {
        const qreal dpr = screen->devicePixelRatio();
        if (scale == 0.0) {
            scale = dpr;
        } else if (!qFuzzyCompare(scale, dpr)) {
            return false;
        }
    }
    if (scale <= 0.0) {
        return false;
    }
    if (scaleOut) {
        *scaleOut = scale;
    }
    return true;
}

int nextOverlayId()
{
    static int id = 0;
    return ++id;
}

void flushWindowSystemChanges()
{
#ifdef Q_OS_WIN
    DwmFlush();
#else
    QGuiApplication::sync();
#endif
}

constexpr int kLongCaptureBurstIntervalMs = 16;
constexpr int kFastTranslateGlassScaleDivisor = 12;
constexpr int kFastTranslateGlassTintAlpha = 36;

QThreadPool& fastTranslateGlassThreadPool()
{
    class GlassThreadPool final : public QThreadPool {
    public:
        GlassThreadPool()
        {
            setMaxThreadCount(1);
            setExpiryTimeout(5000);
        }
    };
    static GlassThreadPool pool;
    return pool;
}

// Retry cadence while stitching is paused on unmatched content: frequent
// enough to notice the page settling back, cheap enough to idle at.
constexpr int kLongCapturePauseRetryIntervalMs = 200;
// Slack added around an annotation's bounding box when computing the repaint
// region: covers stroke width, arrow heads, text outline and the radius that
// mosaic/blur bleed beyond the item's own bounds (strength is capped at 24).
constexpr int kAnnotationDirtySlack = 96;
constexpr int kLongCaptureSettleDelayMs = 160;
constexpr int kLongCaptureQuietStopMs = 1500;
constexpr int kLongCapturePreviewRefreshMs = 50;
constexpr int kLongCapturePreviewMaxDeferredMs = 120;
constexpr int kLongCaptureMinDispatchIntervalMs = 32;
constexpr int kLongCaptureMaxWheelDispatchDelta = 120;
constexpr int kLongCaptureMaxDocumentRows = 120000;
// Leaves headroom for geometric growth, result compaction, the desktop frame,
// and image encoders instead of treating the canvas as the whole process budget.
constexpr qint64 kLongCaptureMaxCanvasBytes = 128LL * 1024 * 1024;
constexpr int kLongCapturePreviewWidth = 150;
constexpr int kLongCapturePixelWheelStep = 40;
constexpr int kLongCaptureWheelDelta = 120;
constexpr int kLongCaptureMinimumViewportHeight = 96;
constexpr int kLongCaptureAnnotationConfirmIntervalMs = 32;
constexpr int kLongCaptureAnnotationStableSamples = 2;
constexpr double kLongCaptureSameFrameScore = 1.5;
constexpr double kLongCaptureAcceptConfidence = 0.35;
constexpr double kLongCaptureMaxAlignmentScore = 1.5;
constexpr double kLongCaptureRelocalizeConfidence = 0.5;
// Noisy-render acceptance: some pages re-rasterize subtly while scrolling
// (sub-pixel AA, image resampling), keeping alignment scores permanently above
// the strict threshold even though the position is unambiguous. When the match
// is this confident (ambiguous matches are capped far below), tolerate the
// rendering noise instead of stuttering through reject/repair cycles.
constexpr double kLongCaptureNoisyAcceptConfidence = 0.65;
constexpr double kLongCaptureNoisyMaxAlignmentScore = 6.0;
// In-place repair of committed rows when the stationary live frame stops
// matching them (sub-pixel scroll settle, hover/theme changes, dynamic
// content). Requires this many consecutive stable-mismatch samples, and gives
// up beyond the score cap (content replaced wholesale is not repairable).
constexpr int kLongCaptureRepairStableSamples = 3;
constexpr double kLongCaptureRepairMaxScore = 48.0;
// Keep retrying the annotation confirmation while repair has a chance to
// kick in, instead of bouncing the user back to browse mode immediately.
constexpr int kLongCaptureAnnotationMaxConfirmRejects = 10;

int longCaptureMinRequiredOverlap(int height)
{
    if (height <= 1) {
        return 1;
    }
    return qMin(height - 1, qMax(8, height / 4));
}

std::atomic<bool> gLongCaptureHookInputPaused{false};

#ifdef Q_OS_WIN
QString hwndDebugText(HWND hwnd)
{
    if (!hwnd) {
        return QStringLiteral("hwnd=null");
    }

    wchar_t className[256]{};
    wchar_t title[256]{};
    GetClassNameW(hwnd, className, 255);
    GetWindowTextW(hwnd, title, 255);

    RECT rect{};
    GetWindowRect(hwnd, &rect);
    return QStringLiteral("hwnd=0x%1 class=\"%2\" title=\"%3\" visible=%4 enabled=%5 rect=%6,%7 %8x%9")
        .arg(static_cast<quintptr>(reinterpret_cast<quintptr>(hwnd)), 0, 16)
        .arg(QString::fromWCharArray(className))
        .arg(QString::fromWCharArray(title).left(80))
        .arg(IsWindowVisible(hwnd))
        .arg(IsWindowEnabled(hwnd))
        .arg(rect.left)
        .arg(rect.top)
        .arg(rect.right - rect.left)
        .arg(rect.bottom - rect.top);
}
#endif

#ifdef Q_OS_WIN
struct WindowAtPointSearch {
    POINT point{};
    HWND excludedOverlay = nullptr;
    HWND excludedViewport = nullptr;
    HWND result = nullptr;
};

BOOL CALLBACK enumWindowAtPointProc(HWND hwnd, LPARAM lParam)
{
    auto* search = reinterpret_cast<WindowAtPointSearch*>(lParam);
    if (!search || !hwnd || hwnd == search->excludedOverlay
        || hwnd == search->excludedViewport || !IsWindowVisible(hwnd)) {
        return TRUE;
    }
    if (GetAncestor(hwnd, GA_ROOT) != hwnd) {
        return TRUE;
    }
    RECT rect{};
    if (!GetWindowRect(hwnd, &rect)) {
        return TRUE;
    }
    if (search->point.x < rect.left || search->point.x >= rect.right
        || search->point.y < rect.top || search->point.y >= rect.bottom) {
        return TRUE;
    }
    search->result = hwnd;
    return FALSE;
}

HWND topLevelWindowBelowPoint(POINT globalPoint, HWND excludedOverlay, HWND excludedViewport)
{
    WindowAtPointSearch search;
    search.point = globalPoint;
    search.excludedOverlay = excludedOverlay;
    search.excludedViewport = excludedViewport;
    EnumWindows(enumWindowAtPointProc, reinterpret_cast<LPARAM>(&search));
    return search.result;
}

HWND deepestChildWindowAtPoint(HWND root, POINT globalPoint)
{
    HWND target = root;
    while (target) {
        POINT clientPoint = globalPoint;
        if (!ScreenToClient(target, &clientPoint)) {
            break;
        }
        const HWND child = ChildWindowFromPointEx(target,
                                                   clientPoint,
                                                   CWP_SKIPINVISIBLE | CWP_SKIPDISABLED | CWP_SKIPTRANSPARENT);
        if (!child || child == target) {
            break;
        }
        target = child;
    }
    return target;
}

std::atomic<HHOOK> gLongCaptureMouseHook{nullptr};
CaptureOverlayWindow* gLongCaptureMouseHookWindow = nullptr;
std::thread gLongCaptureMouseHookThread;
std::atomic<DWORD> gLongCaptureMouseHookThreadId{0};
std::atomic<DWORD> gLongCaptureMouseHookInstallError{0};
std::atomic<HWND> gLongCaptureWheelReceiver{nullptr};
std::atomic<bool> gLongCaptureHookRegionActive{false};
std::atomic<LONG> gLongCaptureHookSelectionLeft{0};
std::atomic<LONG> gLongCaptureHookSelectionTop{0};
std::atomic<LONG> gLongCaptureHookSelectionRight{0};
std::atomic<LONG> gLongCaptureHookSelectionBottom{0};
std::atomic<LONG> gLongCaptureHookToolbarLeft{0};
std::atomic<LONG> gLongCaptureHookToolbarTop{0};
std::atomic<LONG> gLongCaptureHookToolbarRight{0};
std::atomic<LONG> gLongCaptureHookToolbarBottom{0};
std::atomic<LONG> gLongCaptureHookPreviewLeft{0};
std::atomic<LONG> gLongCaptureHookPreviewTop{0};
std::atomic<LONG> gLongCaptureHookPreviewRight{0};
std::atomic<LONG> gLongCaptureHookPreviewBottom{0};
constexpr UINT kLongCaptureQueuedWheelMessage = WM_APP + 0x35a;

WORD currentMouseKeyState()
{
    WORD state = 0;
    if ((GetAsyncKeyState(VK_LBUTTON) & 0x8000) != 0) state |= MK_LBUTTON;
    if ((GetAsyncKeyState(VK_RBUTTON) & 0x8000) != 0) state |= MK_RBUTTON;
    if ((GetAsyncKeyState(VK_MBUTTON) & 0x8000) != 0) state |= MK_MBUTTON;
    if ((GetAsyncKeyState(VK_XBUTTON1) & 0x8000) != 0) state |= MK_XBUTTON1;
    if ((GetAsyncKeyState(VK_XBUTTON2) & 0x8000) != 0) state |= MK_XBUTTON2;
    if ((GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0) state |= MK_SHIFT;
    if ((GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0) state |= MK_CONTROL;
    return state;
}

bool atomicRectContains(const MSLLHOOKSTRUCT& info,
                        const std::atomic<LONG>& left,
                        const std::atomic<LONG>& top,
                        const std::atomic<LONG>& right,
                        const std::atomic<LONG>& bottom)
{
    return info.pt.x >= left.load(std::memory_order_relaxed)
        && info.pt.x < right.load(std::memory_order_relaxed)
        && info.pt.y >= top.load(std::memory_order_relaxed)
        && info.pt.y < bottom.load(std::memory_order_relaxed);
}

void setLongCaptureHookRegion(const QRect& selection, const QRect& toolbar, const QRect& preview)
{
    gLongCaptureHookRegionActive.store(false, std::memory_order_release);
    gLongCaptureHookSelectionLeft.store(selection.left(), std::memory_order_relaxed);
    gLongCaptureHookSelectionTop.store(selection.top(), std::memory_order_relaxed);
    gLongCaptureHookSelectionRight.store(selection.right() + 1, std::memory_order_relaxed);
    gLongCaptureHookSelectionBottom.store(selection.bottom() + 1, std::memory_order_relaxed);
    gLongCaptureHookToolbarLeft.store(toolbar.left(), std::memory_order_relaxed);
    gLongCaptureHookToolbarTop.store(toolbar.top(), std::memory_order_relaxed);
    gLongCaptureHookToolbarRight.store(toolbar.isValid() ? toolbar.right() + 1 : toolbar.left(), std::memory_order_relaxed);
    gLongCaptureHookToolbarBottom.store(toolbar.isValid() ? toolbar.bottom() + 1 : toolbar.top(), std::memory_order_relaxed);
    gLongCaptureHookPreviewLeft.store(preview.left(), std::memory_order_relaxed);
    gLongCaptureHookPreviewTop.store(preview.top(), std::memory_order_relaxed);
    gLongCaptureHookPreviewRight.store(preview.isValid() ? preview.right() + 1 : preview.left(), std::memory_order_relaxed);
    gLongCaptureHookPreviewBottom.store(preview.isValid() ? preview.bottom() + 1 : preview.top(), std::memory_order_relaxed);
    gLongCaptureHookRegionActive.store(selection.isValid(), std::memory_order_release);
}

void clearLongCaptureHookRegion()
{
    gLongCaptureHookRegionActive.store(false, std::memory_order_release);
}

bool isLongCaptureWheelHookActive(const CaptureOverlayWindow* window)
{
    return gLongCaptureMouseHook.load(std::memory_order_acquire) != nullptr
        && gLongCaptureMouseHookWindow == window;
}

LRESULT CALLBACK longCaptureMouseHookProc(int code, WPARAM wParam, LPARAM lParam)
{
    if (code == HC_ACTION && wParam == WM_MOUSEWHEEL) {
        const auto* info = reinterpret_cast<const MSLLHOOKSTRUCT*>(lParam);
        if (info && gLongCaptureHookRegionActive.load(std::memory_order_acquire)
            && (GetAsyncKeyState(VK_CONTROL) & 0x8000) == 0
            && (GetAsyncKeyState(VK_SHIFT) & 0x8000) == 0
            && (GetAsyncKeyState(VK_MENU) & 0x8000) == 0
            && atomicRectContains(*info,
                                  gLongCaptureHookSelectionLeft,
                                  gLongCaptureHookSelectionTop,
                                  gLongCaptureHookSelectionRight,
                                  gLongCaptureHookSelectionBottom)
            && !atomicRectContains(*info,
                                   gLongCaptureHookToolbarLeft,
                                   gLongCaptureHookToolbarTop,
                                   gLongCaptureHookToolbarRight,
                                   gLongCaptureHookToolbarBottom)
            && !atomicRectContains(*info,
                                   gLongCaptureHookPreviewLeft,
                                   gLongCaptureHookPreviewTop,
                                   gLongCaptureHookPreviewRight,
                                   gLongCaptureHookPreviewBottom)) {
            if (gLongCaptureHookInputPaused.load(std::memory_order_acquire)) {
                return 1;
            }
            const int delta = static_cast<SHORT>(HIWORD(info->mouseData));
            const HWND receiver = gLongCaptureWheelReceiver.load(std::memory_order_acquire);
            if (receiver && PostMessageW(receiver,
                                         kLongCaptureQueuedWheelMessage,
                                         MAKEWPARAM(currentMouseKeyState(),
                                                   static_cast<SHORT>(delta)),
                                         MAKELPARAM(static_cast<SHORT>(info->pt.x),
                                                    static_cast<SHORT>(info->pt.y)))) {
                return 1;
            }
        }
    }
    return CallNextHookEx(gLongCaptureMouseHook.load(std::memory_order_relaxed), code, wParam, lParam);
}
#else
bool isLongCaptureWheelHookActive(const CaptureOverlayWindow*)
{
    return false;
}
#endif

QSize optionsBarSizeForTool(const QString& toolId)
{
    if (toolId == QStringLiteral("tool-rect")) {
        return QSize(352, 42);
    }
    if (toolId == QStringLiteral("tool-arrow")) {
        return QSize(300, 42);
    }
    if (toolId == QStringLiteral("tool-pen")) {
        return QSize(252, 42);
    }
    if (toolId == QStringLiteral("tool-mosaic")) {
        return QSize(246, 42);
    }
    if (toolId == QStringLiteral("tool-rubber")) {
        return QSize(76, 42);
    }
    if (toolId == QStringLiteral("tool-text")) {
        return QSize(548, 42);
    }
    if (toolId == QStringLiteral("tool-number")) {
        return QSize(220, 42);
    }
    return QSize(172, 44);
}

#ifdef Q_OS_WIN
struct WinDesktopCaptureStats {
    qint64 totalMs = 0;
    qint64 setupMs = 0;
    qint64 bitBltMs = 0;
    qint64 copyMs = 0;
    DWORD error = ERROR_SUCCESS;
    bool resized = false;
};

class WinDesktopCaptureBuffer final {
public:
    ~WinDesktopCaptureBuffer()
    {
        releaseBuffer();
        if (memoryDc_) {
            DeleteDC(memoryDc_);
        }
    }

    QImage capture(const QRect& geometry, WinDesktopCaptureStats* stats)
    {
        WinDesktopCaptureStats localStats;
        QElapsedTimer totalTimer;
        QElapsedTimer phaseTimer;
        totalTimer.start();
        phaseTimer.start();

        HDC screenDc = GetDC(nullptr);
        if (!screenDc) {
            localStats.error = GetLastError();
            finishStats(localStats, totalTimer, stats);
            return {};
        }
        const auto screenDcGuard = qScopeGuard([screenDc]() { ReleaseDC(nullptr, screenDc); });
        if (!ensureBuffer(screenDc, geometry.size(), localStats)) {
            finishStats(localStats, totalTimer, stats);
            return {};
        }
        localStats.setupMs = phaseTimer.elapsed();

        phaseTimer.restart();
        SetLastError(ERROR_SUCCESS);
        const BOOL copied = BitBlt(memoryDc_,
                                   0,
                                   0,
                                   geometry.width(),
                                   geometry.height(),
                                   screenDc,
                                   geometry.left(),
                                   geometry.top(),
                                   SRCCOPY | CAPTUREBLT);
        localStats.bitBltMs = phaseTimer.elapsed();
        if (!copied) {
            localStats.error = GetLastError();
            finishStats(localStats, totalTimer, stats);
            return {};
        }

        phaseTimer.restart();
        QImage result(geometry.size(), QImage::Format_ARGB32);
        if (result.isNull()) {
            localStats.error = ERROR_NOT_ENOUGH_MEMORY;
            finishStats(localStats, totalTimer, stats);
            return {};
        }
        const qsizetype byteCount = static_cast<qsizetype>(geometry.width())
            * geometry.height() * 4;
        std::memcpy(result.bits(), bits_, static_cast<size_t>(byteCount));
        localStats.copyMs = phaseTimer.elapsed();
        finishStats(localStats, totalTimer, stats);
        return result;
    }

private:
    static void finishStats(WinDesktopCaptureStats& localStats,
                            const QElapsedTimer& totalTimer,
                            WinDesktopCaptureStats* stats)
    {
        localStats.totalMs = totalTimer.elapsed();
        if (stats) {
            *stats = localStats;
        }
    }

    bool ensureBuffer(HDC screenDc, const QSize& requested, WinDesktopCaptureStats& stats)
    {
        if (!memoryDc_) {
            memoryDc_ = CreateCompatibleDC(screenDc);
            if (!memoryDc_) {
                stats.error = GetLastError();
                return false;
            }
        }
        if (bitmap_ && capacity_.width() == requested.width()
            && capacity_.height() >= requested.height()) {
            return true;
        }

        const int capacityHeight = capacity_.width() == requested.width()
            ? qMax(requested.height(), capacity_.height() + capacity_.height() / 2)
            : requested.height();
        BITMAPINFO info{};
        info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        info.bmiHeader.biWidth = requested.width();
        info.bmiHeader.biHeight = -capacityHeight;
        info.bmiHeader.biPlanes = 1;
        info.bmiHeader.biBitCount = 32;
        info.bmiHeader.biCompression = BI_RGB;

        void* nextBits = nullptr;
        HBITMAP nextBitmap = CreateDIBSection(
            screenDc, &info, DIB_RGB_COLORS, &nextBits, nullptr, 0);
        if (!nextBitmap || !nextBits) {
            if (nextBitmap) {
                DeleteObject(nextBitmap);
            }
            stats.error = GetLastError();
            return false;
        }
        HGDIOBJ previous = SelectObject(memoryDc_, nextBitmap);
        if (!previous || previous == HGDI_ERROR) {
            stats.error = GetLastError();
            DeleteObject(nextBitmap);
            return false;
        }
        if (!originalBitmap_) {
            originalBitmap_ = previous;
        }
        if (bitmap_) {
            DeleteObject(bitmap_);
        }
        bitmap_ = nextBitmap;
        bits_ = nextBits;
        capacity_ = QSize(requested.width(), capacityHeight);
        stats.resized = true;
        return true;
    }

    void releaseBuffer()
    {
        if (memoryDc_ && originalBitmap_) {
            SelectObject(memoryDc_, originalBitmap_);
        }
        if (bitmap_) {
            DeleteObject(bitmap_);
        }
        bitmap_ = nullptr;
        originalBitmap_ = nullptr;
        bits_ = nullptr;
        capacity_ = QSize();
    }

    HDC memoryDc_ = nullptr;
    HBITMAP bitmap_ = nullptr;
    HGDIOBJ originalBitmap_ = nullptr;
    void* bits_ = nullptr;
    QSize capacity_;
};

QImage captureDesktopWin(const QRect& geometry, WinDesktopCaptureStats* stats = nullptr)
{
    if (!geometry.isValid()) {
        return {};
    }
    thread_local WinDesktopCaptureBuffer buffer;
    return buffer.capture(geometry, stats);
}

// Maps a logical client-area rect of `hwnd` to native (physical) screen
// coordinates. Valid only under a desktop-wide uniform scale factor, where the
// mapping is linear from the native client origin.
QRect nativeRectForClientRect(HWND hwnd, const QRect& logicalClientRect, qreal scale)
{
    POINT origin{0, 0};
    if (!hwnd || !ClientToScreen(hwnd, &origin) || !logicalClientRect.isValid()
        || scale <= 0.0) {
        return {};
    }
    return QRect(origin.x + static_cast<int>(std::floor(logicalClientRect.left() * scale)),
                 origin.y + static_cast<int>(std::floor(logicalClientRect.top() * scale)),
                 qMax(1, qRound(logicalClientRect.width() * scale)),
                 qMax(1, qRound(logicalClientRect.height() * scale)));
}
#endif

QImage captureDesktopQt(const QRect& geometry)
{
    if (!geometry.isValid()) {
        return {};
    }

    QImage image(geometry.size(), QImage::Format_ARGB32_Premultiplied);
    if (image.isNull()) {
        return {};
    }
    image.fill(Qt::transparent);

    QPainter painter(&image);
    for (QScreen* screen : QGuiApplication::screens()) {
        const QRect screenGeometry = screen->geometry();
        const QRect intersection = geometry.intersected(screenGeometry);
        if (intersection.isEmpty()) {
            continue;
        }
        const QPixmap pixmap = screen->grabWindow(0);
        if (pixmap.isNull()) {
            continue;
        }
        const qreal scaleX = static_cast<qreal>(pixmap.width()) / qMax(1, screenGeometry.width());
        const qreal scaleY = static_cast<qreal>(pixmap.height()) / qMax(1, screenGeometry.height());
        const QRect relative = intersection.translated(-screenGeometry.topLeft());
        const QRectF source(relative.x() * scaleX,
                            relative.y() * scaleY,
                            relative.width() * scaleX,
                            relative.height() * scaleY);
        painter.drawPixmap(intersection.translated(-geometry.topLeft()), pixmap, source);
    }
    painter.end();
    return image.convertToFormat(QImage::Format_ARGB32);
}

QRect clampRect(QRect rect, const QRect& bounds)
{
    rect = rect.normalized();
    rect = rect.intersected(bounds);
    return rect;
}

bool isShapeAnnotation(const AnnotationItem& item)
{
    return item.type == AnnotationType::Rectangle || item.type == AnnotationType::Ellipse;
}

bool isFilledMosaicAnnotation(const AnnotationItem& item)
{
    return item.type == AnnotationType::Mosaic && item.style.mosaicPaintMode == MosaicPaintMode::Fill;
}

bool isEditableRectAnnotationForTool(const AnnotationItem& item, const QString& toolId, MosaicPaintMode mosaicPaintMode)
{
    if (toolId == QStringLiteral("tool-rect")) {
        return isShapeAnnotation(item);
    }
    if (toolId == QStringLiteral("tool-mosaic")) {
        return mosaicPaintMode == MosaicPaintMode::Fill && isFilledMosaicAnnotation(item);
    }
    return false;
}

constexpr int kPinHandoffCloseDelayMs = 48;

QColor pixelColorAt(const QImage& image, QPoint point)
{
    if (image.isNull() || !image.rect().contains(point)) {
        return QColor(0, 0, 0);
    }
    return QColor::fromRgb(image.pixel(point));
}

QString rgbColorText(const QColor& color)
{
    return QStringLiteral("RGB(%1, %2, %3)")
        .arg(color.red())
        .arg(color.green())
        .arg(color.blue());
}

QString hexColorText(const QColor& color)
{
    return color.name(QColor::HexRgb).toUpper();
}

QFont captureInfoFont()
{
    QFont font(QStringLiteral("Microsoft YaHei UI"));
    font.setPixelSize(12);
    return font;
}

QImage renderMagnifierPanelImage(const QImage& sourceImage,
                                 const QPoint& localMouse,
                                 const QPoint& globalMouse,
                                 const QColor& accent,
                                 bool showCursorColor)
{
    constexpr int sample = 17;
    constexpr int previewW = 174;
    constexpr int previewH = 94;
    constexpr int infoH = 48;

    QImage panelImage(previewW, previewH + infoH, QImage::Format_ARGB32_Premultiplied);
    panelImage.fill(Qt::transparent);

    QPainter painter(&panelImage);
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.setRenderHint(QPainter::SmoothPixmapTransform, false);

    const QRect preview(0, 0, previewW, previewH);
    const QRect info(0, previewH, previewW, infoH);
    const QRect panel(0, 0, previewW, previewH + infoH);
    const QRect src(localMouse.x() - sample / 2, localMouse.y() - sample / 2, sample, sample);
    const QColor c = pixelColorAt(sourceImage, localMouse);

    painter.setPen(QPen(QColor(255, 255, 255, 225), 2));
    painter.setBrush(QColor(248, 250, 253, 238));
    painter.drawRect(preview.adjusted(0, 0, -1, -1));
    if (!sourceImage.isNull()) {
        painter.drawImage(preview.adjusted(1, 1, -1, -1), sourceImage, src.intersected(sourceImage.rect()));
    }

    const QPoint center = preview.center();
    QPen guide(accent, 4);
    guide.setCosmetic(true);
    painter.setPen(guide);
    painter.drawLine(center.x(), preview.top() + 2, center.x(), preview.bottom() - 2);
    painter.drawLine(preview.left() + 2, center.y(), preview.right() - 2, center.y());

    painter.setPen(Qt::NoPen);
    painter.setBrush(accent);
    painter.drawRoundedRect(QRect(center.x() - 17, center.y() - 17, 34, 34), 4, 4);
    painter.setBrush(QColor(255, 255, 255));
    painter.setPen(QPen(QColor(70, 80, 100), 1));
    painter.drawRect(QRect(center.x() - 4, center.y() - 4, 8, 8));

    painter.setPen(Qt::NoPen);
    painter.setBrush(QColor(23, 32, 51, 235));
    painter.drawRect(info);

    QFont font = painter.font();
    font.setPixelSize(showCursorColor ? 12 : 13);
    font.setBold(true);
    painter.setFont(font);
    painter.setPen(Qt::white);
    const QRect coordinateRect = showCursorColor
        ? QRect(info.left(), info.top() + 1, info.width(), 19)
        : info;
    painter.drawText(coordinateRect, Qt::AlignCenter,
                     QStringLiteral("(%1, %2)").arg(globalMouse.x()).arg(globalMouse.y()));

    if (showCursorColor) {
        const QRect swatch(info.left() + 10, info.top() + 27, 13, 13);
        painter.setPen(QPen(QColor(255, 255, 255, 210), 1));
        painter.setBrush(c);
        painter.drawRect(swatch);

        const int textLeft = swatch.right() + 7;
        const int textWidth = info.right() - textLeft - 7;
        const QRect rgbRect(textLeft, info.top() + 20, textWidth, 14);
        const QRect hexRect(textLeft, info.top() + 34, textWidth, 13);
        font.setPixelSize(10);
        font.setBold(true);
        painter.setFont(font);
        painter.setPen(Qt::white);
        painter.drawText(rgbRect, Qt::AlignVCenter | Qt::AlignLeft,
                         rgbColorText(c));
        painter.drawText(hexRect, Qt::AlignVCenter | Qt::AlignLeft,
                         hexColorText(c));
    }

    QPen panelBorder(accent, 1);
    panelBorder.setCosmetic(true);
    painter.setPen(panelBorder);
    painter.setBrush(Qt::NoBrush);
    painter.drawRect(panel.adjusted(0, 0, -1, -1));
    painter.end();
    return panelImage;
}

QImage renderSizeLabelImage(const QString& text, const QSize& size, const QFont& font)
{
    QImage image(size, QImage::Format_ARGB32_Premultiplied);
    image.fill(Qt::transparent);

    QPainter painter(&image);
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.setFont(font);
    painter.setPen(Qt::NoPen);
    painter.setBrush(QColor(23, 32, 51, 215));
    painter.drawRoundedRect(QRect(QPoint(0, 0), size), 6, 6);
    painter.setPen(Qt::white);
    painter.drawText(QRect(QPoint(0, 0), size), Qt::AlignCenter, text);
    painter.end();
    return image;
}

QImage renderCursorColorLabelImage(const QString& text, const QColor& color, const QSize& size, const QFont& font)
{
    QImage image(size, QImage::Format_ARGB32_Premultiplied);
    image.fill(Qt::transparent);

    QPainter painter(&image);
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.setFont(font);
    painter.setPen(Qt::NoPen);
    painter.setBrush(QColor(23, 32, 51, 225));
    painter.drawRoundedRect(QRect(QPoint(0, 0), size), 7, 7);
    painter.setBrush(color);
    painter.drawRect(10, 9, 12, 12);
    painter.setPen(Qt::white);
    painter.drawText(QRect(30, 0, size.width() - 36, size.height()), Qt::AlignVCenter | Qt::AlignLeft, text);
    painter.end();
    return image;
}

void warmupMagnifierRenderPath()
{
    QImage desktop(96, 96, QImage::Format_ARGB32);
    desktop.fill(QColor(128, 140, 156));
    const QImage panel = renderMagnifierPanelImage(
        desktop,
        QPoint(48, 48),
        QPoint(960, 540),
        QColor(79, 124, 255),
        true);

    QImage target(220, 170, QImage::Format_ARGB32_Premultiplied);
    target.fill(Qt::transparent);
    QPainter painter(&target);
    painter.drawImage(QPoint(8, 8), panel);
}

void warmupLabelRenderPath()
{
    const QFont font = captureInfoFont();
    const QFontMetrics metrics(font);
    const auto sizeLabel = [&font, &metrics](const QString& text) {
        return renderSizeLabelImage(
            text,
            QSize(metrics.horizontalAdvance(text) + 18, 24),
            font);
    };
    const auto colorLabel = [&font](const QColor& color) {
        const QString text = QStringLiteral("%1  %2")
                                 .arg(rgbColorText(color), hexColorText(color));
        return renderCursorColorLabelImage(text, color, QSize(198, 30), font);
    };

    const QImage normalSize = sizeLabel(QStringLiteral("1920 x 1080 px"));
    const QImage smallSize = sizeLabel(QStringLiteral("128 x 64 px"));
    const QImage pointSize = sizeLabel(QStringLiteral("1 x 1 px"));
    const QImage normalColor = colorLabel(QColor(96, 128, 180));
    const QImage blackColor = colorLabel(Qt::black);

    QImage target(240, 160, QImage::Format_ARGB32_Premultiplied);
    target.fill(Qt::transparent);
    QPainter painter(&target);
    painter.drawImage(QPoint(0, 0), normalSize);
    painter.drawImage(QPoint(0, 34), normalColor);
    painter.drawImage(QPoint(0, 68), smallSize);
    painter.drawImage(QPoint(0, 98), blackColor);
    painter.drawImage(QPoint(0, 132), pointSize);
}

const QFuture<QStringList>& textFontCatalogFuture()
{
    static const QFuture<QStringList> future = QtConcurrent::run([]() {
        Perf::ScopedTimer timer(QStringLiteral("TextFontCatalog.enumerate"));
        QStringList catalog;
        const QStringList families = QFontDatabase::families();
        catalog.reserve(families.size());
        for (const QString& family : families) {
            if (!family.isEmpty() && !QFontDatabase::isPrivateFamily(family)) {
                catalog.append(family);
            }
        }
        std::sort(catalog.begin(), catalog.end(), [](const QString& left,
                                                     const QString& right) {
            return QString::compare(left, right, Qt::CaseInsensitive) < 0;
        });
        Perf::log(QStringLiteral("TextFontCatalog.ready families=%1")
                      .arg(catalog.size()));
        return catalog;
    });
    return future;
}

void populateTextFontCombo(QComboBox* combo,
                           const QStringList& families,
                           const QString& preferredFamily)
{
    if (!combo) {
        return;
    }

    QString selectedFamily = preferredFamily.trimmed();
    if (selectedFamily.isEmpty()) {
        selectedFamily = QApplication::font().family();
    }

    QStringList choices = families;
    int selectedIndex = choices.indexOf(selectedFamily, Qt::CaseSensitive);
    if (selectedIndex < 0) {
        choices.prepend(selectedFamily);
        selectedIndex = 0;
    }

    const QSignalBlocker blocker(combo);
    auto* model = qobject_cast<QStringListModel*>(combo->model());
    if (!model) {
        model = new QStringListModel(combo);
        combo->setModel(model);
    }
    model->setStringList(choices);
    combo->setCurrentIndex(selectedIndex);
}

QCursor captureCursor(const QColor& accent)
{
    QPixmap pixmap(19, 19);
    pixmap.fill(Qt::transparent);

    QPainter painter(&pixmap);
    painter.setRenderHint(QPainter::Antialiasing, true);

    const QPoint center(9, 9);
    QPen halo(QColor(255, 255, 255, 230), 2.2);
    halo.setCapStyle(Qt::RoundCap);
    painter.setPen(halo);
    painter.drawLine(center.x(), 1, center.x(), 17);
    painter.drawLine(1, center.y(), 17, center.y());

    QPen line(accent, 1.1);
    line.setCapStyle(Qt::RoundCap);
    painter.setPen(line);
    painter.drawLine(center.x(), 1, center.x(), 7);
    painter.drawLine(center.x(), 11, center.x(), 17);
    painter.drawLine(1, center.y(), 7, center.y());
    painter.drawLine(11, center.y(), 17, center.y());

    painter.setPen(Qt::NoPen);
    painter.setBrush(accent);
    painter.drawEllipse(QRectF(center.x() - 1.4, center.y() - 1.4, 2.8, 2.8));
    painter.end();

    return QCursor(pixmap, center.x(), center.y());
}


QIcon drawnOptionIcon(const std::function<void(QPainter&, const QRectF&)>& draw)
{
    QPixmap pixmap(22, 22);
    pixmap.fill(Qt::transparent);
    QPainter painter(&pixmap);
    painter.setRenderHint(QPainter::Antialiasing, true);
    draw(painter, QRectF(2, 2, 18, 18));
    painter.end();
    return QIcon(pixmap);
}

QColor optionInk(bool checked)
{
    return checked ? QColor(255, 255, 255) : QColor(71, 83, 103);
}

QIcon shapeRectangleIcon(bool checked)
{
    return drawnOptionIcon([checked](QPainter& painter, const QRectF& rect) {
        painter.setPen(QPen(optionInk(checked), 1.7, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
        painter.setBrush(Qt::NoBrush);
        painter.drawRoundedRect(rect.adjusted(2, 4, -2, -4), 2.5, 2.5);
    });
}

QIcon shapeEllipseIcon(bool checked)
{
    return drawnOptionIcon([checked](QPainter& painter, const QRectF& rect) {
        painter.setPen(QPen(optionInk(checked), 1.7, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
        painter.setBrush(Qt::NoBrush);
        painter.drawEllipse(rect.adjusted(2, 4, -2, -4));
    });
}

QIcon strokePreviewIcon(const QColor& stroke, int width, bool checked)
{
    return drawnOptionIcon([stroke, width, checked](QPainter& painter, const QRectF& rect) {
        const QColor ink = checked ? QColor(255, 255, 255) : stroke;
        const qreal dot = qBound(3.0, static_cast<qreal>(width) + 2.0, 13.0);
        painter.setPen(Qt::NoPen);
        painter.setBrush(ink);
        painter.drawEllipse(QRectF(rect.center().x() - dot / 2.0, rect.center().y() - dot / 2.0, dot, dot));
    });
}

QIcon fillPreviewIcon(const QColor& stroke, const QColor& fill, int alpha, bool filled, bool checked)
{
    return drawnOptionIcon([stroke, fill, alpha, filled, checked](QPainter& painter, const QRectF& rect) {
        const QRectF box = rect.adjusted(3, 3, -3, -3);
        if (filled) {
            QColor color = fill;
            color.setAlpha(alpha);
            painter.setBrush(color);
            painter.setPen(QPen(checked ? QColor(255, 255, 255) : stroke, 1.5));
            painter.drawRoundedRect(box, 2, 2);
        } else {
            const int cell = 4;
            for (int y = 0; y < 3; ++y) {
                for (int x = 0; x < 3; ++x) {
                    painter.fillRect(QRectF(box.left() + x * cell, box.top() + y * cell, cell, cell),
                                     ((x + y) % 2 == 0) ? QColor(205, 211, 219) : QColor(244, 247, 250));
                }
            }
            painter.setBrush(Qt::NoBrush);
            painter.setPen(QPen(checked ? QColor(255, 255, 255) : QColor(104, 116, 134), 1.4));
            painter.drawRoundedRect(box, 2, 2);
        }
    });
}


class ColorSwatchButton final : public QToolButton {
public:
    ColorSwatchButton(const QColor& color, int innerSize, QWidget* parent = nullptr)
        : QToolButton(parent)
        , color_(color)
        , innerSize_(innerSize)
    {
        setFixedSize(15, 15);
        setCursor(Qt::ArrowCursor);
        setMouseTracking(true);
        setAutoRaise(false);
        setFocusPolicy(Qt::NoFocus);
        setStyleSheet(QStringLiteral("QToolButton { min-width:15px; max-width:15px; min-height:15px; max-height:15px; background: transparent; border: none; padding: 0; margin: 0; }"));
    }

protected:
    void paintEvent(QPaintEvent*) override
    {
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing, false);

        const int visualSize = qMin(width() - 3, underMouse() ? innerSize_ + 1 : innerSize_);
        const QRect swatch((width() - visualSize) / 2, (height() - visualSize) / 2, visualSize, visualSize);

        painter.setPen(QPen(QColor(184, 194, 212), 1));
        painter.setBrush(color_);
        painter.drawRect(swatch);

    }

private:
    QColor color_;
    int innerSize_ = 10;
};


class CurrentColorChip final : public QFrame {
public:
    CurrentColorChip(const QColor& color, QWidget* parent = nullptr)
        : QFrame(parent)
        , color_(color)
    {
        setAttribute(Qt::WA_StyledBackground, false);
        setCursor(Qt::ArrowCursor);
    }

    void setColor(const QColor& color)
    {
        if (color_ == color) {
            return;
        }
        color_ = color;
        update();
    }

protected:
    void paintEvent(QPaintEvent*) override
    {
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing, true);

        const QRectF chipRect = QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5);
        painter.setPen(Qt::NoPen);
        painter.setBrush(color_);
        painter.drawRoundedRect(chipRect, 3, 3);
    }

private:
    QColor color_;
};


class MosaicStrengthSlider final : public QSlider {
public:
    explicit MosaicStrengthSlider(QWidget* parent = nullptr)
        : QSlider(Qt::Horizontal, parent)
    {
        setFixedSize(78, 20);
        setCursor(Qt::ArrowCursor);
        setFocusPolicy(Qt::NoFocus);
        setSingleStep(1);
        setPageStep(1);
    }

protected:
    void wheelEvent(QWheelEvent* event) override
    {
        int steps = event->angleDelta().y() / 120;
        if (steps == 0 && event->angleDelta().y() != 0) {
            steps = event->angleDelta().y() > 0 ? 1 : -1;
        }
        if (steps != 0) {
            setValue(qBound(minimum(), value() + steps, maximum()));
        }
        event->accept();
    }

    void paintEvent(QPaintEvent*) override
    {
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing, true);

        const qreal centerY = height() / 2.0;
        const qreal left = 6.0;
        const qreal right = width() - 6.0;
        const qreal trackHeight = 3.0;
        const QRectF fullTrack(left, centerY - trackHeight / 2.0, right - left, trackHeight);

        const qreal range = qMax(1, maximum() - minimum());
        const qreal ratio = (value() - minimum()) / range;
        const qreal handleX = left + (right - left) * ratio;

        painter.setPen(Qt::NoPen);
        painter.setBrush(QColor(220, 229, 242));
        painter.drawRoundedRect(fullTrack, 1.5, 1.5);

        painter.setBrush(QColor(79, 124, 255));
        painter.drawRoundedRect(QRectF(left, centerY - trackHeight / 2.0, handleX - left, trackHeight), 1.5, 1.5);

        constexpr qreal radius = 4.5;
        const QRectF handle(handleX - radius, centerY - radius, radius * 2.0, radius * 2.0);
        painter.setBrush(Qt::white);
        painter.setPen(QPen(QColor(79, 124, 255), 1.2));
        painter.drawEllipse(handle);
    }
};


class InlineTextBox final : public QWidget {
public:
    explicit InlineTextBox(QWidget* parent = nullptr)
        : QWidget(parent)
    {
        setMouseTracking(true);
        setFocusPolicy(Qt::StrongFocus);
        setAttribute(Qt::WA_StyledBackground, false);
        setAttribute(Qt::WA_InputMethodEnabled, true);
        setAttribute(Qt::WA_NoSystemBackground, true);
        setAutoFillBackground(false);
        setCursor(Qt::IBeamCursor);
    }

    std::function<void()> commitRequested;
    std::function<void()> cancelRequested;
    std::function<void()> textChanged;

    QString text() const { return text_; }
    int handleMargin() const { return handleMargin_; }

    QPoint textCursorOriginOffset() const
    {
        return QPoint(handleMargin_ + textPadding_, handleMargin_ + textPadding_ + 10);
    }

    void setMoveBounds(const QRect& bounds)
    {
        bounds_ = bounds;
    }

    QRect contentRectInParent() const
    {
        const QRect textRect = contentRect().adjusted(textPadding_, textPadding_, -textPadding_, -textPadding_);
        return QRect(mapToParent(textRect.topLeft()), textRect.size());
    }

    void applyStyle(const AnnotationStyle& style)
    {
        font_ = QFont(style.fontFamily.isEmpty() ? QStringLiteral("Microsoft YaHei UI") : style.fontFamily);
        font_.setPixelSize(style.fontSize);
        font_.setBold(style.textBold);
        font_.setItalic(style.textItalic);
        font_.setStyleStrategy(QFont::PreferAntialias);
        textColor_ = style.text;
        outlineEnabled_ = style.textOutline;
        outlineColor_ = style.textOutlineColor;
        outlineWidth_ = qMax(1, style.textOutlineWidth);
        setFont(font_);
        update();
    }

    void fitToContent()
    {
        const QSize contentSize = measuredContentSize();
        const int maxWidth = bounds_.isValid() ? qMax(minimumContentWidth_, bounds_.right() - x() + 1 - handleMargin_ * 2) : 4096;
        const int maxHeight = bounds_.isValid() ? qMax(minimumContentHeight_, bounds_.bottom() - y() + 1 - handleMargin_ * 2) : 4096;
        const QSize next(qBound(minimumContentWidth_, contentSize.width(), maxWidth),
                         qBound(minimumContentHeight_, contentSize.height(), maxHeight));
        setGeometry(clampedGeometry(QRect(pos(), next + QSize(handleMargin_ * 2, handleMargin_ * 2))));
        update();
    }

protected:
    void paintEvent(QPaintEvent*) override
    {
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing, true);

        const QRectF box = QRectF(contentRect()).adjusted(-0.5, -0.5, 0.5, 0.5);
        QPen border(QColor(79, 124, 255), 1.2);
        border.setCosmetic(true);
        painter.setPen(border);
        painter.setBrush(Qt::NoBrush);
        painter.drawRect(box);

        painter.setFont(font_);
        drawText(painter, contentRect().adjusted(textPadding_, textPadding_, -textPadding_, -textPadding_));

        if (hasFocus()) {
            drawCursor(painter, contentRect().adjusted(textPadding_, textPadding_, -textPadding_, -textPadding_));
        }

        painter.setPen(QPen(QColor(255, 255, 255, 230), 1));
        painter.setBrush(QColor(79, 124, 255));
        const QVector<QPointF> handles = {box.topLeft(), box.topRight(), box.bottomLeft(), box.bottomRight()};
        for (const QPointF& point : handles) {
            painter.drawRoundedRect(QRectF(point.x() - 4, point.y() - 4, 8, 8), 2, 2);
        }
    }

    void inputMethodEvent(QInputMethodEvent* event) override
    {
        if (!event->commitString().isEmpty()) {
            insertText(event->commitString());
        }
        preeditText_ = event->preeditString();
        fitToContent();
        update();
        event->accept();
    }

    QVariant inputMethodQuery(Qt::InputMethodQuery query) const override
    {
        switch (query) {
        case Qt::ImEnabled:
            return true;
        case Qt::ImCursorRectangle:
            return QRect(cursorPoint(contentRect().adjusted(textPadding_, textPadding_, -textPadding_, -textPadding_)), QSize(1, QFontMetrics(font_).height()));
        case Qt::ImCursorPosition:
            return cursorPosition_;
        case Qt::ImSurroundingText:
            return text_;
        case Qt::ImCurrentSelection:
            return QString();
        default:
            return QWidget::inputMethodQuery(query);
        }
    }

    void keyPressEvent(QKeyEvent* event) override
    {
        if (event->key() == Qt::Key_Escape) {
            if (cancelRequested) {
                cancelRequested();
            }
            event->accept();
            return;
        }
        if ((event->key() == Qt::Key_Return || event->key() == Qt::Key_Enter)
            && event->modifiers().testFlag(Qt::ControlModifier)) {
            if (commitRequested) {
                commitRequested();
            }
            event->accept();
            return;
        }
        if (event->key() == Qt::Key_Return || event->key() == Qt::Key_Enter) {
            insertText(QStringLiteral("\n"));
            event->accept();
            return;
        }
        if (event->key() == Qt::Key_Backspace) {
            if (cursorPosition_ > 0) {
                text_.remove(cursorPosition_ - 1, 1);
                --cursorPosition_;
                notifyTextChanged();
            }
            event->accept();
            return;
        }
        if (event->key() == Qt::Key_Delete) {
            if (cursorPosition_ < text_.size()) {
                text_.remove(cursorPosition_, 1);
                notifyTextChanged();
            }
            event->accept();
            return;
        }
        if (event->key() == Qt::Key_Left) {
            cursorPosition_ = qMax(0, cursorPosition_ - 1);
            update();
            event->accept();
            return;
        }
        if (event->key() == Qt::Key_Right) {
            cursorPosition_ = qMin(text_.size(), cursorPosition_ + 1);
            update();
            event->accept();
            return;
        }
        if (event->key() == Qt::Key_Home) {
            cursorPosition_ = lineStart(cursorPosition_);
            update();
            event->accept();
            return;
        }
        if (event->key() == Qt::Key_End) {
            cursorPosition_ = lineEnd(cursorPosition_);
            update();
            event->accept();
            return;
        }
        if (!event->text().isEmpty() && !event->modifiers().testFlag(Qt::ControlModifier)) {
            insertText(event->text());
            event->accept();
            return;
        }
        QWidget::keyPressEvent(event);
    }

    void mousePressEvent(QMouseEvent* event) override
    {
        if (event->button() == Qt::LeftButton) {
            const DragMode hit = hitTest(event->position().toPoint());
            if (hit != DragMode::None) {
                dragMode_ = hit;
                dragStartGlobal_ = event->globalPosition().toPoint();
                dragStartGeometry_ = geometry();
                event->accept();
                return;
            }
            setFocus(Qt::MouseFocusReason);
            cursorPosition_ = textPositionAt(event->position().toPoint());
            preeditText_.clear();
            update();
            event->accept();
            return;
        }
        QWidget::mousePressEvent(event);
    }

    void mouseMoveEvent(QMouseEvent* event) override
    {
        if (dragMode_ != DragMode::None) {
            const QPoint delta = event->globalPosition().toPoint() - dragStartGlobal_;
            QRect next = dragStartGeometry_;
            if (dragMode_ == DragMode::Move) {
                next.translate(delta);
            } else {
                next.setSize(QSize(qMax(minimumContentWidth_ + handleMargin_ * 2, dragStartGeometry_.width() + delta.x()),
                                   qMax(minimumContentHeight_ + handleMargin_ * 2, dragStartGeometry_.height() + delta.y())));
            }
            setGeometry(clampedGeometry(next));
            event->accept();
            return;
        }

        updateCursor(event->position().toPoint());
        QWidget::mouseMoveEvent(event);
    }

    void mouseReleaseEvent(QMouseEvent* event) override
    {
        if (dragMode_ != DragMode::None && event->button() == Qt::LeftButton) {
            dragMode_ = DragMode::None;
            updateCursor(event->position().toPoint());
            event->accept();
            return;
        }
        QWidget::mouseReleaseEvent(event);
    }

private:
    enum class DragMode { None, Move, Resize };

    QRect contentRect() const
    {
        return rect().adjusted(handleMargin_, handleMargin_, -handleMargin_, -handleMargin_);
    }

    DragMode hitTest(const QPoint& pos) const
    {
        const QRect resizeHandle(width() - handleMargin_ - 8, height() - handleMargin_ - 8, 16, 16);
        if (resizeHandle.contains(pos)) {
            return DragMode::Resize;
        }
        if (!contentRect().contains(pos)) {
            return DragMode::Move;
        }
        return DragMode::None;
    }

    void updateCursor(const QPoint& pos)
    {
        const DragMode hit = hitTest(pos);
        if (hit == DragMode::Resize) {
            setCursor(Qt::SizeFDiagCursor);
        } else if (hit == DragMode::Move) {
            setCursor(Qt::SizeAllCursor);
        } else {
            setCursor(Qt::IBeamCursor);
        }
    }

    QRect clampedGeometry(QRect next) const
    {
        if (bounds_.isValid()) {
            next.setWidth(qMin(next.width(), bounds_.width()));
            next.setHeight(qMin(next.height(), bounds_.height()));
            if (next.left() < bounds_.left()) {
                next.moveLeft(bounds_.left());
            }
            if (next.top() < bounds_.top()) {
                next.moveTop(bounds_.top());
            }
            if (next.right() > bounds_.right()) {
                next.moveRight(bounds_.right());
            }
            if (next.bottom() > bounds_.bottom()) {
                next.moveBottom(bounds_.bottom());
            }
        }
        return next;
    }

    int lineStart(int position) const
    {
        const int start = text_.lastIndexOf(QLatin1Char('\n'), qMax(0, position - 1));
        return start < 0 ? 0 : start + 1;
    }

    int lineEnd(int position) const
    {
        const int end = text_.indexOf(QLatin1Char('\n'), position);
        return end < 0 ? text_.size() : end;
    }

    QString displayedText() const
    {
        QString value = text_;
        if (!preeditText_.isEmpty()) {
            value.insert(cursorPosition_, preeditText_);
        }
        return value;
    }

    QSize measuredContentSize() const
    {
        QFontMetrics metrics(font_);
        const QString measureText = displayedText();
        const QString display = measureText.isEmpty() ? QStringLiteral("文字输入") : measureText;
        const QStringList lines = display.split(QLatin1Char('\n'));
        int width = minimumContentWidth_;
        for (const QString& line : lines) {
            width = qMax(width, metrics.horizontalAdvance(line));
        }
        const int height = qMax(minimumContentHeight_, metrics.lineSpacing() * qMax(1, lines.size()));
        return QSize(width + textPadding_ * 2 + 4, height + textPadding_ * 2 + 4);
    }

    void drawText(QPainter& painter, const QRect& area) const
    {
        QFontMetrics metrics(font_);
        const QString visibleText = displayedText();
        if (visibleText.isEmpty()) {
            painter.setPen(QColor(79, 124, 255, 130));
            painter.drawText(QPointF(area.left(), area.top() + metrics.ascent()), QStringLiteral("文字输入"));
            return;
        }

        const QStringList lines = visibleText.split(QLatin1Char('\n'));
        for (int i = 0; i < lines.size(); ++i) {
            const QPointF baseline(area.left(), area.top() + metrics.ascent() + i * metrics.lineSpacing());
            if (outlineEnabled_) {
                painter.setPen(QPen(outlineColor_, outlineWidth_));
                const QVector<QPointF> offsets = {QPointF(-1, 0), QPointF(1, 0), QPointF(0, -1), QPointF(0, 1)};
                for (const QPointF& offset : offsets) {
                    painter.drawText(baseline + offset, lines.at(i));
                }
            }
            painter.setPen(textColor_);
            painter.drawText(baseline, lines.at(i));
        }
    }

    QPoint cursorPoint(const QRect& area) const
    {
        QFontMetrics metrics(font_);
        const QString value = displayedText();
        const int pos = qBound(0, cursorPosition_ + preeditText_.size(), value.size());
        int line = 0;
        int start = 0;
        for (int i = 0; i < pos; ++i) {
            if (value.at(i) == QLatin1Char('\n')) {
                ++line;
                start = i + 1;
            }
        }
        const QString prefix = value.mid(start, pos - start);
        return QPoint(area.left() + metrics.horizontalAdvance(prefix), area.top() + line * metrics.lineSpacing());
    }

    void drawCursor(QPainter& painter, const QRect& area) const
    {
        const QPoint top = cursorPoint(area);
        QFontMetrics metrics(font_);
        QPen cursorPen(QColor(79, 124, 255), 1.4);
        cursorPen.setCosmetic(true);
        painter.setPen(cursorPen);
        painter.drawLine(top, top + QPoint(0, metrics.height()));
    }

    int textPositionAt(const QPoint& point) const
    {
        QFontMetrics metrics(font_);
        const QRect area = contentRect().adjusted(textPadding_, textPadding_, -textPadding_, -textPadding_);
        const QStringList lines = text_.split(QLatin1Char('\n'));
        const int lineIndex = qBound(0, (point.y() - area.top()) / qMax(1, metrics.lineSpacing()), qMax(0, lines.size() - 1));
        int base = 0;
        for (int i = 0; i < lineIndex; ++i) {
            base += lines.at(i).size() + 1;
        }
        const QString line = lines.value(lineIndex);
        const int x = point.x() - area.left();
        for (int i = 0; i < line.size(); ++i) {
            const int mid = metrics.horizontalAdvance(line.left(i)) + metrics.horizontalAdvance(line.mid(i, 1)) / 2;
            if (x < mid) {
                return base + i;
            }
        }
        return qMin(text_.size(), base + line.size());
    }

    void insertText(const QString& value)
    {
        if (value.isEmpty()) {
            return;
        }
        preeditText_.clear();
        text_.insert(cursorPosition_, value);
        cursorPosition_ += value.size();
        notifyTextChanged();
    }

    void notifyTextChanged()
    {
        fitToContent();
        update();
        if (textChanged) {
            textChanged();
        }
    }

    QString text_;
    QString preeditText_;
    int cursorPosition_ = 0;
    QFont font_ = QFont(QStringLiteral("Microsoft YaHei UI"), 16);
    QColor textColor_ = QColor(239, 68, 68);
    bool outlineEnabled_ = false;
    QColor outlineColor_ = Qt::white;
    int outlineWidth_ = 2;
    QRect bounds_;
    DragMode dragMode_ = DragMode::None;
    QPoint dragStartGlobal_;
    QRect dragStartGeometry_;
    static constexpr int handleMargin_ = 8;
    static constexpr int textPadding_ = 2;
    static constexpr int minimumContentWidth_ = 24;
    static constexpr int minimumContentHeight_ = 24;
};

QString arrowModeTooltip(ArrowHeadMode mode)
{
    switch (mode) {
    case ArrowHeadMode::Line:
        return QStringLiteral("直线");
    case ArrowHeadMode::SingleArrow:
        return QStringLiteral("单向箭头");
    case ArrowHeadMode::DoubleArrow:
        return QStringLiteral("双向箭头");
    }
    return QStringLiteral("单向箭头");
}


QIcon arrowModeIcon(ArrowHeadMode mode)
{
    QPixmap pixmap(34, 22);
    pixmap.fill(Qt::transparent);
    QPainter painter(&pixmap);
    painter.setRenderHint(QPainter::Antialiasing, true);

    QPen pen(QColor(71, 83, 103), 1.8, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin);
    painter.setPen(pen);
    const QPointF start(5, 11);
    const QPointF end(21, 11);
    painter.drawLine(start, end);

    auto drawHead = [&](const QPointF& from, const QPointF& to) {
        const double angle = std::atan2(to.y() - from.y(), to.x() - from.x());
        const double back = angle + M_PI;
        const double size = 5.0;
        const QPointF p1 = to + QPointF(std::cos(back + M_PI / 6.0) * size, std::sin(back + M_PI / 6.0) * size);
        const QPointF p2 = to + QPointF(std::cos(back - M_PI / 6.0) * size, std::sin(back - M_PI / 6.0) * size);
        painter.drawLine(to, p1);
        painter.drawLine(to, p2);
    };

    if (mode == ArrowHeadMode::SingleArrow || mode == ArrowHeadMode::DoubleArrow) {
        drawHead(start, end);
    }
    if (mode == ArrowHeadMode::DoubleArrow) {
        drawHead(end, start);
    }

    painter.setPen(Qt::NoPen);
    painter.setBrush(QColor(100, 116, 139));
    painter.drawPolygon(QPolygonF({QPointF(27, 9), QPointF(31, 9), QPointF(29, 13)}));
    painter.end();
    return QIcon(pixmap);
}

QIcon mosaicBrushIcon(int strength, bool checked)
{
    return strokePreviewIcon(QColor(51, 65, 85), strength, checked);
}

QIcon mosaicFillIcon(bool checked)
{
    return Ui::themedIcon(QStringLiteral(":/visnip/icons/mosaic-fill.svg"), optionInk(checked), QSize(20, 20));
}

QIcon rubberBrushIcon(int size, bool checked)
{
    return drawnOptionIcon([size, checked](QPainter& painter, const QRectF& rect) {
        const QColor ink = checked ? QColor(255, 255, 255) : QColor(51, 65, 85);
        const qreal dot = qBound(4.0, static_cast<qreal>(size) / 3.5, 13.0);
        painter.setPen(Qt::NoPen);
        painter.setBrush(ink);
        painter.drawEllipse(QRectF(rect.center().x() - dot / 2.0, rect.center().y() - dot / 2.0, dot, dot));
    });
}

QIcon rubberFillIcon(bool checked)
{
    return drawnOptionIcon([checked](QPainter& painter, const QRectF& rect) {
        const QColor ink = optionInk(checked);
        const QRectF box = rect.adjusted(3, 4, -3, -4);
        painter.setPen(QPen(ink, 1.6, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
        painter.setBrush(Qt::NoBrush);
        painter.drawRoundedRect(box, 2, 2);
        painter.drawLine(box.topLeft() + QPointF(3, 3), box.bottomRight() - QPointF(3, 3));
        painter.drawLine(box.topRight() + QPointF(-3, 3), box.bottomLeft() + QPointF(3, -3));
    });
}

QIcon mosaicEffectIcon(MosaicEffectMode mode)
{
    const QString icon = mode == MosaicEffectMode::GaussianBlur
        ? QStringLiteral(":/visnip/icons/mosaic-blur.svg")
        : QStringLiteral(":/visnip/icons/mosaic-pixelate.svg");
    return Ui::themedIcon(icon, QColor(71, 83, 103), QSize(20, 20));
}

QIcon textOptionIcon(const QString& name, bool checked)
{
    return Ui::themedIcon(QStringLiteral(":/visnip/icons/%1.svg").arg(name), optionInk(checked), QSize(20, 20));
}

QVector<QColor> toolPalette()
{
    return {
        QColor(QStringLiteral("#EF4444")),
        QColor(QStringLiteral("#F97316")),
        QColor(QStringLiteral("#FACC15")),
        QColor(QStringLiteral("#22C55E")),
        QColor(QStringLiteral("#06B6D4")),
        QColor(QStringLiteral("#4F7CFF")),
        QColor(QStringLiteral("#8B5CF6")),
        QColor(QStringLiteral("#EC4899")),
        QColor(QStringLiteral("#F9A8D4")),
        QColor(QStringLiteral("#8B5A2B")),
        QColor(QStringLiteral("#9A3412")),
        QColor(QStringLiteral("#D97706")),
        QColor(QStringLiteral("#FEF08A")),
        QColor(QStringLiteral("#A3E635")),
        QColor(QStringLiteral("#15803D")),
        QColor(QStringLiteral("#5EEAD4")),
        QColor(QStringLiteral("#1D4ED8")),
        QColor(QStringLiteral("#581C87")),
        QColor(QStringLiteral("#737373")),
        QColor(QStringLiteral("#111827")),
    };
}

} // namespace

QString captureCursorColorDisplayText(const QColor& color)
{
    return QStringLiteral("%1  %2")
        .arg(rgbColorText(color), hexColorText(color));
}

QImage buildCaptureMagnifierPanelImage(const QImage& sourceImage,
                                       const QPoint& localMouse,
                                       const QPoint& globalMouse,
                                       const QColor& accent,
                                       bool showCursorColor)
{
    return renderMagnifierPanelImage(sourceImage, localMouse, globalMouse,
                                     accent, showCursorColor);
}

namespace {

QImage buildFastTranslateGlassImageImpl(
    const QImage& source,
    const std::shared_ptr<std::atomic_bool>& cancelFlag)
{
    const auto cancelled = [&cancelFlag]() {
        return cancelFlag
            && cancelFlag->load(std::memory_order_acquire);
    };
    if (source.isNull() || cancelled()) {
        return {};
    }

    const QSize coarseSize(
        qMax(1, source.width() / kFastTranslateGlassScaleDivisor),
        qMax(1, source.height() / kFastTranslateGlassScaleDivisor));
    const QImage coarse = source.scaled(
        coarseSize, Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
    if (coarse.isNull() || cancelled()) {
        return {};
    }

    QImage result = coarse.scaled(
        source.size(), Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
    if (cancelled()) {
        return {};
    }
    if (!result.isNull()) {
        {
            QPainter tint(&result);
            tint.fillRect(result.rect(),
                          QColor(255, 255, 255, kFastTranslateGlassTintAlpha));
        }
    }
    return result;
}

} // namespace

QImage buildFastTranslateGlassImage(const QImage& source)
{
    return buildFastTranslateGlassImageImpl(source, {});
}

bool fastTranslationPlacementIsCompatible(const QRect& sourceRect,
                                          const QRect& destinationRect)
{
    return sourceRect.isValid() && destinationRect.isValid()
        && sourceRect.size() == destinationRect.size();
}

struct LongCaptureFrameGrabResult {
    quint64 requestId = 0;
    QRect geometry;
    QImage image;
    LongCapture::RowSignature signature;
    qint64 totalMs = 0;
    qint64 setupMs = 0;
    qint64 bitBltMs = 0;
    qint64 copyMs = 0;
    qint64 signatureMs = 0;
    int error = 0;
    bool resized = false;
};

#ifdef Q_OS_WIN
class LongCaptureFrameGrabber final : public QThread {
public:
    using Callback = std::function<void(LongCaptureFrameGrabResult)>;

    LongCaptureFrameGrabber(QObject* receiver, Callback callback)
        : receiver_(receiver)
        , callback_(std::move(callback))
    {
    }

    ~LongCaptureFrameGrabber() override
    {
        stopAndWait();
    }

    bool request(quint64 requestId,
                 const QRect& geometry,
                 const QRect& grabRect,
                 const QSize& logicalSize)
    {
        std::lock_guard lock(mutex_);
        if (stopping_) {
            return false;
        }
        pending_ = Request{requestId, geometry, grabRect, logicalSize};
        condition_.notify_one();
        return true;
    }

    void stopAndWait()
    {
        {
            std::lock_guard lock(mutex_);
            if (stopping_) {
                return;
            }
            stopping_ = true;
            pending_.reset();
        }
        condition_.notify_one();
        wait();
    }

protected:
    void run() override
    {
        for (;;) {
            Request request;
            {
                std::unique_lock lock(mutex_);
                condition_.wait(lock, [this]() { return stopping_ || pending_.has_value(); });
                if (stopping_) {
                    return;
                }
                request = *pending_;
                pending_.reset();
            }

            LongCaptureFrameGrabResult result;
            result.requestId = request.requestId;
            result.geometry = request.geometry;
            WinDesktopCaptureStats stats;
            result.image = captureDesktopWin(request.grabRect, &stats);
            result.totalMs = stats.totalMs;
            result.setupMs = stats.setupMs;
            result.bitBltMs = stats.bitBltMs;
            result.copyMs = stats.copyMs;
            result.error = static_cast<int>(stats.error);
            result.resized = stats.resized;
            if (!result.image.isNull() && request.logicalSize.isValid()
                && result.image.size() != request.logicalSize) {
                // Scaled-DPI capture: bring the native-resolution frame back to
                // the logical selection size the stitching document uses.
                QElapsedTimer scaleTimer;
                scaleTimer.start();
                result.image = result.image.scaled(request.logicalSize,
                                                   Qt::IgnoreAspectRatio,
                                                   Qt::SmoothTransformation);
                result.totalMs += scaleTimer.elapsed();
            }
            if (!result.image.isNull()) {
                QElapsedTimer signatureTimer;
                signatureTimer.start();
                result.signature = LongCapture::computeRowSignature(result.image);
                result.signatureMs = signatureTimer.elapsed();
                result.totalMs += result.signatureMs;
            }

            const Callback callback = callback_;
            QMetaObject::invokeMethod(
                receiver_,
                [callback, result = std::move(result)]() mutable {
                    callback(std::move(result));
                },
                Qt::QueuedConnection);
        }
    }

private:
    struct Request {
        quint64 requestId = 0;
        QRect geometry;
        QRect grabRect;
        QSize logicalSize;
    };

    QObject* receiver_ = nullptr;
    Callback callback_;
    std::mutex mutex_;
    std::condition_variable condition_;
    std::optional<Request> pending_;
    bool stopping_ = false;
};
#endif

class LongCaptureViewportLayer final : public QWidget {
public:
    using NativeWheelHandler = std::function<bool(quint32, quintptr, qintptr)>;

    explicit LongCaptureViewportLayer(NativeWheelHandler nativeWheelHandler)
        : QWidget(nullptr)
        , nativeWheelHandler_(std::move(nativeWheelHandler))
    {
        setWindowFlags(Qt::FramelessWindowHint
                       | Qt::Tool
                       | Qt::WindowStaysOnTopHint
                       | Qt::WindowDoesNotAcceptFocus
                       | Qt::WindowTransparentForInput
                       | Qt::NoDropShadowWindowHint);
        setAttribute(Qt::WA_OpaquePaintEvent, true);
        setAttribute(Qt::WA_ShowWithoutActivating, true);
        setAttribute(Qt::WA_TransparentForMouseEvents, true);
        setFocusPolicy(Qt::NoFocus);
    }

    bool enableCaptureExclusion()
    {
#ifdef Q_OS_WIN
        const HWND hwnd = reinterpret_cast<HWND>(winId());
        const LONG_PTR exStyle = GetWindowLongPtrW(hwnd, GWL_EXSTYLE);
        SetWindowLongPtrW(hwnd,
                          GWL_EXSTYLE,
                          (exStyle & ~WS_EX_LAYERED) | WS_EX_TRANSPARENT
                              | WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW);
        SetWindowPos(hwnd,
                     nullptr,
                     0,
                     0,
                     0,
                     0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE
                         | SWP_NOZORDER | SWP_FRAMECHANGED);
        return Platform::applyWindowCaptureExclusion(winId(), false)
            == Platform::WindowCaptureExclusion::Excluded;
#else
        return false;
#endif
    }

    bool prepare(const QImage& image,
                 const QVector<AnnotationItem>& vectorAnnotations,
                 const QPointF& annotationOffset,
                 const QRect& globalGeometry,
                 const QColor& borderColor,
                 int borderWidth)
    {
        frame_ = image;
        vectorAnnotations_ = vectorAnnotations;
        annotationOffset_ = annotationOffset;
        borderColor_ = borderColor;
        borderWidth_ = qMax(1, borderWidth);
        if (frame_.isNull() || !globalGeometry.isValid()) {
            hide();
            return false;
        }
        setGeometry(globalGeometry);
        return !isVisible();
    }

    void reveal()
    {
        if (!isVisible()) {
            show();
        }
    }

protected:
    void paintEvent(QPaintEvent*) override
    {
        QPainter painter(this);
        painter.setCompositionMode(QPainter::CompositionMode_Source);
        painter.drawImage(rect(), frame_);
        painter.setCompositionMode(QPainter::CompositionMode_SourceOver);
        painter.setRenderHint(QPainter::Antialiasing, true);
        for (const AnnotationItem& item : vectorAnnotations_) {
            drawAnnotation(painter, item, annotationOffset_);
        }
        QPen border(borderColor_, borderWidth_);
        border.setCosmetic(true);
        painter.setPen(border);
        painter.setBrush(Qt::NoBrush);
        painter.drawRect(rect().adjusted(0, 0, -1, -1));
    }

    bool nativeEvent(const QByteArray& eventType, void* message, qintptr* result) override
    {
#ifdef Q_OS_WIN
        Q_UNUSED(eventType)
        auto* msg = static_cast<MSG*>(message);
        if (msg && (msg->message == WM_MOUSEWHEEL || msg->message == WM_POINTERWHEEL)
            && nativeWheelHandler_
            && nativeWheelHandler_(msg->message,
                                   static_cast<quintptr>(msg->wParam),
                                   static_cast<qintptr>(msg->lParam))) {
            if (result) {
                *result = 0;
            }
            return true;
        }
        if (msg && msg->message == WM_NCHITTEST) {
            if (result) {
                *result = HTTRANSPARENT;
            }
            return true;
        }
        if (msg && msg->message == WM_MOUSEACTIVATE) {
            if (result) {
                *result = MA_NOACTIVATE;
            }
            return true;
        }
#else
        Q_UNUSED(eventType)
        Q_UNUSED(message)
        Q_UNUSED(result)
#endif
        return QWidget::nativeEvent(eventType, message, result);
    }

private:
    QImage frame_;
    QVector<AnnotationItem> vectorAnnotations_;
    QPointF annotationOffset_;
    QColor borderColor_;
    int borderWidth_ = 1;
    NativeWheelHandler nativeWheelHandler_;
};

CaptureOverlayWindow::CaptureOverlayWindow(AppConfig* config,
                                           OcrService* ocrService,
                                           ImageTranslationService* imageTranslationService,
                                           QWidget* parent)
    : QWidget(parent)
    , config_(config)
    , captureSettings_(config ? config->settings().capture : CaptureSettings{})
    , uiSettings_(config ? config->settings().ui : UiSettings{})
    , copyThenExit_(!config || config->settings().output.copyThenExit)
{
    QElapsedTimer ctorTimer;
    ctorTimer.start();
    fastOcr_ = ocrService;
    fastImage_ = imageTranslationService;
    overlayId_ = nextOverlayId();
    Perf::log(QStringLiteral("CaptureOverlayWindow.ctor.id id=%1").arg(overlayId_));
    if (captureSettings_.maskColor.alpha() < 112) {
        captureSettings_.maskColor.setAlpha(118);
    }
    if (config_) {
        toolStyles_ = config_->settings().tools;
        connect(config_, &AppConfig::changed, this, [this]() {
            if (!fastTranslateRunning_
                || fastTranslationConfigurationMatchesPending()) {
                return;
            }
            Perf::log(QStringLiteral("FastTranslate.configChanged id=%1 job=%2 action=cancel")
                          .arg(overlayId_)
                          .arg(fastJobSerial_));
            cancelFastTranslation();
        });
    }

    setWindowTitle(QStringLiteral("Visnip 截图"));
    setWindowFlags(Qt::FramelessWindowHint | Qt::Tool | Qt::WindowStaysOnTopHint);
    setAttribute(Qt::WA_DeleteOnClose, true);
    // Keep the surface opaque and non-layered: WA_TranslucentBackground forces
    // a per-pixel-alpha layered window, which SetWindowDisplayAffinity rejects
    // with ERROR_NOT_ENOUGH_MEMORY (breaking long-capture exclusion) and which
    // re-uploads the whole surface on every flush. The flash-free reveal is
    // handled by the windowOpacity 0 -> 1 sequence in beginCapture().
    setAttribute(Qt::WA_NoSystemBackground, true);
    setAttribute(Qt::WA_OpaquePaintEvent, true);
    setContextMenuPolicy(Qt::PreventContextMenu);
    setMouseTracking(true);
    setFocusPolicy(Qt::StrongFocus);
    setCursor(captureCursor(captureSettings_.borderColor));

    shapeState_.strokeColor = toolStyles_.shapeStrokeColor;
    shapeState_.fillColor = toolStyles_.shapeFillColor;
    shapeState_.strokeWidth = qBound(1, toolStyles_.shapeStrokeWidth, 12);
    shapeState_.mode = toolStyles_.shapeFilled ? ShapePaintMode::Filled : ShapePaintMode::StrokeOnly;
    shapeState_.fillAlpha = qBound(16, toolStyles_.shapeFillAlpha, 255);
    arrowState_.strokeColor = toolStyles_.arrowColor;
    arrowState_.strokeWidth = qBound(1, toolStyles_.arrowWidth, 12);
    arrowState_.headMode = toolStyles_.arrowHeadMode;
    mosaicState_.paintMode = toolStyles_.mosaicPaintMode;
    mosaicState_.effectMode = toolStyles_.mosaicEffectMode;
    mosaicState_.strength = qBound(3, toolStyles_.mosaicStrength, 24);
    eraserState_.paintMode = toolStyles_.eraserPaintMode;
    eraserState_.size = qBound(4, toolStyles_.eraserSize, 96);

    currentStyle_.stroke = toolStyles_.penColor;
    currentStyle_.fill = QColor(toolStyles_.penColor.red(), toolStyles_.penColor.green(), toolStyles_.penColor.blue(), 34);
    currentStyle_.text = toolStyles_.penColor;
    currentStyle_.strokeWidth = qBound(1, toolStyles_.penWidth, 12);
    currentStyle_.mosaicBlock = mosaicState_.strength;
    currentStyle_.fontSize = qBound(6, toolStyles_.textFontSize, 96);
    currentStyle_.fontFamily = toolStyles_.textFontFamily;
    currentStyle_.textBold = toolStyles_.textBold;
    currentStyle_.textItalic = toolStyles_.textItalic;
    currentStyle_.textOutline = toolStyles_.textOutline;

    standardOptionsBar_ = new QFrame(this);
    optionsBar_ = standardOptionsBar_;
    optionsBar_->setObjectName(QStringLiteral("VisnipOptionsBar"));
    optionsBar_->setAttribute(Qt::WA_StyledBackground, true);
    optionsBar_->setContextMenuPolicy(Qt::PreventContextMenu);
    optionsBar_->setCursor(Qt::ArrowCursor);
    installRightButtonEventFilter(optionsBar_);
    optionsBar_->hide();

    toolbar_ = new Ui::CompactToolbar(this);
    toolbar_->setDarkMode(uiSettings_.darkToolbar);
    toolbar_->setContextMenuPolicy(Qt::PreventContextMenu);
    installRightButtonEventFilter(toolbar_);
    toolbar_->hide();
    connect(toolbar_, &Ui::CompactToolbar::toolSelected, this, &CaptureOverlayWindow::onToolSelected);
    connect(toolbar_, &Ui::CompactToolbar::actionTriggered, this, &CaptureOverlayWindow::onToolbarAction);
    longCaptureProgressTimer_.setTimerType(Qt::PreciseTimer);
    longCaptureProgressTimer_.setInterval(kLongCaptureBurstIntervalMs);
    connect(&longCaptureProgressTimer_, &QTimer::timeout, this, &CaptureOverlayWindow::captureLongFrameDuringScroll);
    longCaptureAnnotationModeTimer_.setSingleShot(true);
    connect(&longCaptureAnnotationModeTimer_, &QTimer::timeout, this, &CaptureOverlayWindow::tryEnterLongCaptureAnnotationMode);
    longCapturePreviewRefreshTimer_.setSingleShot(true);
    longCapturePreviewRefreshTimer_.setTimerType(Qt::PreciseTimer);
    connect(&longCapturePreviewRefreshTimer_, &QTimer::timeout, this, [this]() {
        if (!longCaptureActive_ || !selection_.isValid()) {
            return;
        }
        const qint64 now = Perf::elapsedMs();
        const qint64 dirtyAgeMs = longCapturePreviewDirtyAtMs_ >= 0
            ? now - longCapturePreviewDirtyAtMs_
            : kLongCapturePreviewMaxDeferredMs;
        const bool scrollingBusy = longCaptureCaptureBusy_
            || longCaptureWheelDispatchInFlight_
            || longCaptureWheelDispatchScheduled_
            || !longCaptureWheelQueue_.isEmpty()
            || (longCaptureLastMovementAtMs_ >= 0
                && now - longCaptureLastMovementAtMs_ < kLongCaptureSettleDelayMs);
        if (scrollingBusy && dirtyAgeMs < kLongCapturePreviewMaxDeferredMs) {
            const int retryMs = static_cast<int>(qMin<qint64>(
                kLongCaptureBurstIntervalMs,
                kLongCapturePreviewMaxDeferredMs - dirtyAgeMs));
            longCapturePreviewRefreshTimer_.start(qMax(1, retryMs));
            return;
        }
        longCapturePreviewDirtyAtMs_ = -1;
        refreshLongCapturePreviewFit();
        update(longCapturePreviewPanelRect().adjusted(-2, -2, 2, 2));
    });
    Perf::logDuration(QStringLiteral("CaptureOverlayWindow.ctor"), ctorTimer.elapsed());
}

CaptureOverlayWindow::~CaptureOverlayWindow()
{
    if (fastGlassCancelFlag_) {
        fastGlassCancelFlag_->store(true, std::memory_order_release);
    }
    fastGlassQueuedSource_ = QImage();
    fastGlassQueuedRect_ = QRect();
    fastGlassQueuedSerial_ = 0;
    if (fastImage_ && fastImage_->isBusy()) {
        fastImage_->cancel();
    }
    if (fastOcr_ && fastOcrRevision_ != 0) {
        fastOcr_->cancel(fastOcrRevision_);
        fastOcrRevision_ = 0;
    }
    if (fastText_) {
        fastText_->cancel();
    }
    if (fastLocalText_) {
        fastLocalText_->cancel();
    }
    cancelLongCaptureFrameRequest();
#ifdef Q_OS_WIN
    if (longCaptureFrameGrabber_) {
        longCaptureFrameGrabber_->stopAndWait();
        longCaptureFrameGrabber_.reset();
    }
#endif
    makeLongCaptureAnnotationsDocumentRelative();
    uninstallLongCaptureWheelHook();
    clearLongCaptureInputRegion();
    destroyLongCaptureLayers();
}

bool CaptureOverlayWindow::applyCaptureExclusion()
{
#ifdef Q_OS_WIN
    const HWND hwnd = reinterpret_cast<HWND>(winId());
    QElapsedTimer timer;
    timer.start();
    if (!IsWindowVisible(hwnd)) {
        longCaptureExclusionChecked_ = false;
        longCaptureExclusionAvailable_ = false;
        Perf::log(QStringLiteral("LongCapture.captureExclusion id=%1 ok=0 strict=0 skipped=hidden elapsed=%2ms overlay={%3}")
                      .arg(overlayId_)
                      .arg(timer.elapsed())
                      .arg(hwndDebugText(hwnd)));
        return false;
    }

    const Platform::WindowCaptureExclusion exclusion =
        Platform::applyWindowCaptureExclusion(winId(), true);
    const DWORD exclusionError = exclusion == Platform::WindowCaptureExclusion::Failed
        ? GetLastError()
        : ERROR_SUCCESS;
    longCaptureExclusionChecked_ = true;
    longCaptureExclusionAvailable_ =
        exclusion == Platform::WindowCaptureExclusion::Excluded;
    const bool protectedOk = exclusion != Platform::WindowCaptureExclusion::Failed;
    Perf::log(QStringLiteral("LongCapture.captureExclusion id=%1 ok=%2 mode=%3 error=%4 elapsed=%5ms overlay={%6}")
                  .arg(overlayId_)
                  .arg(protectedOk)
                  .arg(static_cast<int>(exclusion))
                  .arg(static_cast<int>(exclusionError))
                  .arg(timer.elapsed())
                  .arg(hwndDebugText(hwnd)));
    return protectedOk;
#else
    return false;
#endif
}

void CaptureOverlayWindow::beginCapture(const QRect& initialSelection)
{
    Perf::ScopedTimer total(QStringLiteral("CaptureOverlayWindow.beginCapture"));
    beginCaptureStartedAtMs_ = Perf::elapsedMs();
    firstPaintLogged_ = false;
    QElapsedTimer phase;

    phase.start();
    captureDesktop();
    Perf::logDuration(QStringLiteral("beginCapture.captureDesktop"), phase.elapsed());

    phase.restart();
    virtualGeometry_ = allScreensGeometry();
    setGeometry(virtualGeometry_);
    selection_ = clampRect(initialSelection.translated(-virtualGeometry_.topLeft()), QRect(QPoint(0, 0), virtualGeometry_.size()));
    mode_ = selection_.isValid() ? Mode::Ready : Mode::Idle;
    lastMouse_ = QCursor::pos() - virtualGeometry_.topLeft();
    if (!rect().contains(lastMouse_)) {
        lastMouse_ = rect().center();
    }
    Perf::log(QStringLiteral("beginCapture.geometry elapsed=%1ms virtual=%2,%3 %4x%5 initialValid=%6 selectionValid=%7")
                  .arg(phase.elapsed())
                  .arg(virtualGeometry_.x())
                  .arg(virtualGeometry_.y())
                  .arg(virtualGeometry_.width())
                  .arg(virtualGeometry_.height())
                  .arg(initialSelection.isValid())
                  .arg(selection_.isValid()));

    phase.restart();
    if (selection_.isValid()) {
        placeToolbar();
    } else {
        hideToolbar();
    }
    Perf::logDuration(QStringLiteral("beginCapture.toolbar_update"), phase.elapsed());

    phase.restart();
    // The window is opaque, so reveal it at opacity 0 and flip to 1 only after
    // the first frame has been painted; showing it directly would flash an
    // unpainted surface. Constant-alpha opacity keeps SetWindowDisplayAffinity
    // working (per-pixel-alpha layering would not).
    setWindowOpacity(0.0);
    show();
#ifdef Q_OS_WIN
    const HWND hwnd = reinterpret_cast<HWND>(winId());
    QElapsedTimer exclusionTimer;
    exclusionTimer.start();
    const bool captureExcluded = applyCaptureExclusion();
    const qint64 exclusionMs = exclusionTimer.elapsed();
    if (screensUseUnitScale(virtualGeometry_)) {
        SetWindowPos(hwnd,
                     HWND_TOPMOST,
                     virtualGeometry_.x(),
                     virtualGeometry_.y(),
                     virtualGeometry_.width(),
                     virtualGeometry_.height(),
                     SWP_NOACTIVATE | SWP_NOOWNERZORDER | SWP_NOSENDCHANGING | SWP_SHOWWINDOW);
    } else {
        setGeometry(virtualGeometry_);
        raise();
    }
#else
    raise();
    const bool captureExcluded = false;
    const qint64 exclusionMs = 0;
#endif
    const qint64 showMs = phase.elapsed();
    setFocus();
    grabKeyboard();
    QElapsedTimer frameTimer;
    frameTimer.start();
    repaint();
    QApplication::processEvents(QEventLoop::ExcludeUserInputEvents);
    setWindowOpacity(1.0);
    update();
    const qint64 finalFrameMs = frameTimer.elapsed();
    Perf::log(QStringLiteral("beginCapture.show_topmost_reveal elapsed=%1ms show=%2ms exclusion=%3ms finalFrame=%4ms excluded=%5 active=%6 hasFocus=%7 visible=%8")
                  .arg(phase.elapsed())
                  .arg(showMs)
                  .arg(exclusionMs)
                  .arg(finalFrameMs)
                  .arg(captureExcluded)
                  .arg(isActiveWindow())
                  .arg(hasFocus())
                  .arg(isVisible()));
}

void CaptureOverlayWindow::warmupCaptureBackend()
{
    Perf::ScopedTimer total(QStringLiteral("CaptureOverlayWindow.warmupCaptureBackend"));
    QElapsedTimer phase;

#ifdef Q_OS_WIN
    phase.start();
    const QRect geometry = allScreensGeometry();
    Perf::log(QStringLiteral("warmup.geometry elapsed=%1ms %2,%3 %4x%5")
                  .arg(phase.elapsed())
                  .arg(geometry.x())
                  .arg(geometry.y())
                  .arg(geometry.width())
                  .arg(geometry.height()));

    phase.restart();
    const QRect sample(geometry.topLeft(), QSize(1, 1));
    const QImage image = captureDesktopWin(sample);
    Perf::log(QStringLiteral("warmup.captureDesktopWin elapsed=%1ms null=%2 size=%3x%4")
                  .arg(phase.elapsed())
                  .arg(image.isNull())
                  .arg(image.width())
                  .arg(image.height()));
#else
    phase.start();
    const QImage image(1, 1, QImage::Format_ARGB32);
    Q_UNUSED(image);
    Perf::logDuration(QStringLiteral("warmup.non_win_image"), phase.elapsed());
#endif

    phase.restart();
    Ui::warmupToolbarIcons(false);
    Perf::logDuration(QStringLiteral("warmup.toolbar_icons_light"), phase.elapsed());

    phase.restart();
    Ui::warmupToolbarIcons(true);
    Perf::logDuration(QStringLiteral("warmup.toolbar_icons_dark"), phase.elapsed());

    phase.restart();
    warmupMagnifierRenderPath();
    Perf::logDuration(QStringLiteral("warmup.magnifier_render"), phase.elapsed());

    phase.restart();
    warmupLabelRenderPath();
    Perf::logDuration(QStringLiteral("warmup.labels_render"), phase.elapsed());

    phase.restart();
    {
        QImage sampleDoc(320, 240, QImage::Format_ARGB32);
        sampleDoc.fill(Qt::white);
        QPainter samplePainter(&sampleDoc);
        for (int y = 8; y < sampleDoc.height(); y += 16) {
            samplePainter.fillRect(
                QRect(6 + (y * 7) % 30, y, sampleDoc.width() - 40, 7),
                QColor(40 + (y * 11) % 160, 90, 200 - (y * 5) % 120));
        }
        samplePainter.end();
        const LongCapture::RowSignature docSignature =
            LongCapture::computeRowSignature(sampleDoc);
        const LongCapture::RowSignature frameSignature =
            LongCapture::computeRowSignature(
                sampleDoc.copy(0, 60, sampleDoc.width(), 120));
        const LongCapture::MatchResult match =
            LongCapture::matchSignatureInDocument(
                docSignature, 0, frameSignature, -120, 200, 24);
        const QImage scaled = sampleDoc.scaled(
            134, 100, Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
        Perf::log(QStringLiteral("warmup.long_capture elapsed=%1ms matchValid=%2 matchY=%3 scaled=%4x%5")
                      .arg(phase.elapsed())
                      .arg(match.valid)
                      .arg(match.documentY)
                      .arg(scaled.width())
                      .arg(scaled.height()));
    }

    phase.restart();
    {
        QImage textSample(420, 90, QImage::Format_ARGB32_Premultiplied);
        textSample.fill(Qt::white);
        QPainter textPainter(&textSample);
        const QString sample = QStringLiteral(
            "长截图：滚轮滚动页面，复制/贴图输出长图 马赛克模糊强度字号 0123456789");
        QFont uiFont(QStringLiteral("Microsoft YaHei UI"));
        for (int pixelSize : {12, 13, 16}) {
            uiFont.setPixelSize(pixelSize);
            uiFont.setBold(pixelSize == 13);
            textPainter.setFont(uiFont);
            textPainter.drawText(
                QRect(0, (pixelSize - 12) * 24, textSample.width(), 24),
                Qt::AlignLeft | Qt::AlignVCenter,
                sample);
        }
        textPainter.end();
    }
    Perf::logDuration(QStringLiteral("warmup.cjk_text_render"), phase.elapsed());

    phase.restart();
    {
        QImage effectSample(256, 256, QImage::Format_ARGB32);
        effectSample.fill(QColor(126, 148, 184));
        applyGaussianBlur(effectSample, effectSample.rect(), 15);
        applyMosaic(effectSample, effectSample.rect(), 15);
    }
    Perf::logDuration(QStringLiteral("warmup.mosaic_effects"), phase.elapsed());

    phase.restart();
    {
        QElapsedTimer optionsPhase;
        optionsPhase.start();
        QFrame warmBar;
        warmBar.setObjectName(QStringLiteral("VisnipOptionsBar"));
        warmBar.setAttribute(Qt::WA_StyledBackground, true);
        warmBar.setAttribute(Qt::WA_DontShowOnScreen, true);
        warmBar.setStyleSheet(QStringLiteral(
            "QFrame#VisnipOptionsBar { background: rgba(255,255,255,0.96); border: 1px solid #4F7CFF; border-radius: 4px; }"
            "QToolButton { min-width: 24px; min-height: 24px; max-width: 24px; max-height: 24px; border: 1px solid #DCE5F2; border-radius: 3px; background: #FFFFFF; color: #334155; font-weight: 600; padding: 0; }"
            "QToolButton:hover { background: #F3F7FF; }"
            "QToolButton:checked { background: #0B7CFF; color: #FFFFFF; border: none; }"));
        auto* warmLayout = new QHBoxLayout(&warmBar);
        auto* warmButton = new QToolButton(&warmBar);
        warmButton->setText(QStringLiteral("M"));
        warmButton->setCheckable(true);
        warmButton->setChecked(true);
        auto* warmSlider = new QSlider(Qt::Horizontal, &warmBar);
        warmSlider->setRange(3, 24);
        auto* warmLabel = new QLabel(QStringLiteral("15"), &warmBar);
        warmLabel->setStyleSheet(
            QStringLiteral("QLabel { font-weight:600; color:#334155; }"));
        warmLayout->addWidget(warmButton);
        warmLayout->addWidget(warmSlider);
        warmLayout->addWidget(warmLabel);
        const qint64 createMs = optionsPhase.restart();
        warmBar.ensurePolished();
        const qint64 polishMs = optionsPhase.restart();
        const QPixmap warmed = warmBar.grab();
        Q_UNUSED(warmed);
        Perf::log(QStringLiteral("warmup.options_bar.split create=%1ms polish=%2ms grab=%3ms")
                      .arg(createMs)
                      .arg(polishMs)
                      .arg(optionsPhase.elapsed()));
    }
    Perf::logDuration(QStringLiteral("warmup.options_bar"), phase.elapsed());

    const QFuture<QStringList>& fontCatalog = textFontCatalogFuture();
    Perf::log(QStringLiteral("warmup.font_catalog scheduled=1 finished=%1")
                  .arg(fontCatalog.isFinished()));
}

QFuture<QStringList> CaptureOverlayWindow::warmupFontCatalog()
{
    return textFontCatalogFuture();
}

void CaptureOverlayWindow::captureDesktop()
{
    Perf::ScopedTimer total(QStringLiteral("CaptureOverlayWindow.captureDesktop"));
    QElapsedTimer phase;

    phase.start();
    const QRect geometry = allScreensGeometry();
    Perf::log(QStringLiteral("captureDesktop.geometry elapsed=%1ms %2,%3 %4x%5")
                  .arg(phase.elapsed())
                  .arg(geometry.x())
                  .arg(geometry.y())
                  .arg(geometry.width())
                  .arg(geometry.height()));
#ifdef Q_OS_WIN
    if (screensUseUnitScale(geometry)) {
        phase.restart();
        const QImage fastCapture = captureDesktopWin(geometry);
        Perf::log(QStringLiteral("captureDesktop.fastCapture elapsed=%1ms null=%2 size=%3x%4")
                      .arg(phase.elapsed())
                      .arg(fastCapture.isNull())
                      .arg(fastCapture.width())
                      .arg(fastCapture.height()));
        if (!fastCapture.isNull()) {
            desktopImage_ = fastCapture.convertToFormat(QImage::Format_ARGB32);
            invalidateSelectionComposite();
            return;
        }
    }
#endif

    phase.restart();
    const QImage image = captureDesktopQt(geometry);
    Perf::logDuration(QStringLiteral("captureDesktop.fallbackGrabAll"), phase.elapsed());

    desktopImage_ = image;
    invalidateSelectionComposite();
}

QRect CaptureOverlayWindow::normalizedFromPoints(const QPoint& a, const QPoint& b) const
{
    return QRect(a, b).normalized().intersected(QRect(QPoint(0, 0), size()));
}

void CaptureOverlayWindow::paintEvent(QPaintEvent*)
{
    const bool logFirstPaint = !firstPaintLogged_;
    QElapsedTimer paintTimer;
    QElapsedTimer sectionTimer;
    paintTimer.start();
    if (logFirstPaint) {
        const qint64 sinceBegin = beginCaptureStartedAtMs_ >= 0 ? Perf::elapsedMs() - beginCaptureStartedAtMs_ : -1;
        Perf::log(QStringLiteral("CaptureOverlayWindow.firstPaint.begin sinceBeginCapture=%1ms selectionValid=%2 desktop=%3x%4")
                      .arg(sinceBegin)
                      .arg(selection_.isValid())
                      .arg(desktopImage_.width())
                      .arg(desktopImage_.height()));
    }

    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing, true);

    if (logFirstPaint) sectionTimer.start();
    if (!desktopImage_.isNull()) {
        painter.drawImage(rect(), desktopImage_);
    }
    painter.fillRect(rect(), captureSettings_.maskColor);
    if (logFirstPaint) {
        Perf::logDuration(QStringLiteral("paint.draw_dimmed_background"), sectionTimer.elapsed());
    }

    if (selection_.isValid()) {
        if (logFirstPaint) sectionTimer.restart();
        if (longCaptureActive_ && !longCaptureVisibleFrame_.isNull()) {
            QImage selectedImage = longCaptureResizePreviewFrame_;
            if (selectedImage.isNull()) {
                const QImage baseImage = longCaptureVisibleFrame_.convertToFormat(QImage::Format_ARGB32);
                selectedImage = baseImage.copy();
                const QPointF offset = longCaptureAnnotationsOverlayRelative_
                    ? QPointF(selection_.topLeft())
                    : QPointF(0, longCaptureCurrentY_);
                renderAnnotationDocumentToImage(selectedImage,
                                                baseImage,
                                                longCaptureAnnotations_,
                                                longCaptureAnnotationMode_,
                                                offset);
            }
            if (mode_ == Mode::ResizingSelection && selectionDragStartRect_.isValid()) {
                // Resize previews are document slices. Map the temporary
                // viewport back into that slice so previously cropped rows can
                // reappear immediately without stretching or committing the
                // output boundary before mouse release.
                const int selectionDocumentY = longCaptureCurrentY_
                    + selection_.top() - selectionDragStartRect_.top();
                const int overlapTopY = qMax(selectionDocumentY,
                                             longCaptureResizePreviewDocumentY_);
                const int overlapBottomY = qMin(
                    selectionDocumentY + selection_.height(),
                    longCaptureResizePreviewDocumentY_ + selectedImage.height());
                if (overlapBottomY > overlapTopY) {
                    const int rows = overlapBottomY - overlapTopY;
                    const int sourceWidth = qMin(selection_.width(), selectedImage.width());
                    const QRect source(0,
                                       overlapTopY - longCaptureResizePreviewDocumentY_,
                                       sourceWidth,
                                       rows);
                    const QPoint target(selection_.left(),
                                        selection_.top() + overlapTopY - selectionDocumentY);
                    painter.drawImage(target, selectedImage, source);
                }
            } else {
                painter.drawImage(selection_, selectedImage);
            }
        } else if (annotations_.count() > 0 || hasCurrentAnnotation_) {
            const QImage& committed = committedSelectionComposite();
            if (hasCurrentAnnotation_) {
                // Only the in-progress item is replayed per frame; everything
                // already committed comes from the cache.
                if (inProgressComposite_.size() != committed.size()
                    || inProgressComposite_.format() != committed.format()) {
                    inProgressComposite_ = QImage(committed.size(), committed.format());
                }
                if (!inProgressComposite_.isNull()) {
                    Q_ASSERT(inProgressComposite_.sizeInBytes() == committed.sizeInBytes());
                    std::memcpy(inProgressComposite_.bits(),
                                committed.constBits(),
                                static_cast<size_t>(committed.sizeInBytes()));
                    renderAnnotationItemToImage(inProgressComposite_,
                                                selectionCompositeBase_,
                                                currentAnnotation_,
                                                QPointF(selection_.topLeft()));
                    painter.drawImage(selection_.topLeft(), inProgressComposite_);
                }
            } else {
                painter.drawImage(selection_.topLeft(), committed);
            }
        } else if (fastTranslateShown()) {
            painter.drawImage(selection_.topLeft(), fastTranslatedImage_);
        } else if (fastTranslateRunning_
                   && fastTranslationPlacementIsCompatible(
                       fastPendingRect_, selection_)) {
            if (!fastPendingImage_.isNull()) {
                painter.drawImage(selection_.topLeft(), fastPendingImage_);
            } else {
                painter.drawImage(selection_, desktopImage_, fastPendingRect_);
            }
        } else {
            painter.drawImage(selection_, desktopImage_, selection_);
        }
        if (logFirstPaint) {
            Perf::logDuration(QStringLiteral("paint.selection_draw"), sectionTimer.elapsed());
        }
    }

    // Loading content belongs inside the selection. Paint the selection chrome
    // afterwards so the border and resize handles always remain visible.
    if (logFirstPaint) sectionTimer.restart();
    drawFastTranslateOverlay(painter);
    if (logFirstPaint) {
        Perf::logDuration(QStringLiteral("paint.drawFastTranslateOverlay"),
                          sectionTimer.elapsed());
    }

    if (logFirstPaint) sectionTimer.restart();
    drawSelection(painter);
    if (logFirstPaint) Perf::logDuration(QStringLiteral("paint.drawSelection"), sectionTimer.elapsed());

    if (logFirstPaint) sectionTimer.restart();
    drawEraserControlOverlay(painter);
    if (logFirstPaint) Perf::logDuration(QStringLiteral("paint.drawEraserControlOverlay"), sectionTimer.elapsed());

    drawActiveShapeEditOverlay(painter);
    drawActiveArrowEditOverlay(painter);

    if (logFirstPaint) sectionTimer.restart();
    drawMagnifier(painter);
    if (logFirstPaint) Perf::logDuration(QStringLiteral("paint.drawMagnifier"), sectionTimer.elapsed());

    if (logFirstPaint) sectionTimer.restart();
    drawLabels(painter);
    drawLongCapturePreview(painter);
    if (logFirstPaint) {
        Perf::logDuration(QStringLiteral("paint.drawLabels"), sectionTimer.elapsed());
        const qint64 sinceBeginEnd = beginCaptureStartedAtMs_ >= 0 ? Perf::elapsedMs() - beginCaptureStartedAtMs_ : -1;
        Perf::log(QStringLiteral("CaptureOverlayWindow.firstPaint.end total=%1ms sinceBeginCapture=%2ms")
                      .arg(paintTimer.elapsed())
                      .arg(sinceBeginEnd));
        firstPaintLogged_ = true;
    } else if (paintTimer.elapsed() >= 40) {
        Perf::log(QStringLiteral("paint.slow id=%1 elapsed=%2ms tool=%3 annotations=%4 hasCurrent=%5 selection=%6x%7")
                      .arg(overlayId_)
                      .arg(paintTimer.elapsed())
                      .arg(activeTool_)
                      .arg(activeAnnotationDocument().count())
                      .arg(hasCurrentAnnotation_)
                      .arg(selection_.width())
                      .arg(selection_.height()));
    }
}

void CaptureOverlayWindow::drawSelection(QPainter& painter) const
{
    const QRect r = selection_;
    if (!r.isValid()) {
        return;
    }

    painter.save();
    QPen pen(captureSettings_.borderColor, captureSettings_.borderWidth);
    pen.setCosmetic(true);
    painter.setPen(pen);
    painter.setBrush(Qt::NoBrush);
    painter.drawRect(r.adjusted(0, 0, -1, -1));

    if (selection_.isValid()) {
        painter.setPen(QPen(captureSettings_.borderColor, 1));
        painter.setBrush(Qt::white);
        const int h = Design::toolbar().safeHitArea > 0 ? captureSettings_.borderWidth + 5 : 7;
        QVector<QPoint> points;
        if (longCaptureActive_) {
            const bool resizingTop = mode_ == Mode::ResizingSelection
                && selectionDragMode_ == SelectionDragMode::ResizeTop;
            const bool resizingBottom = mode_ == Mode::ResizingSelection
                && selectionDragMode_ == SelectionDragMode::ResizeBottom;
            if (resizingTop || canResizeLongCaptureEdge(SelectionDragMode::ResizeTop)) {
                points.append(QPoint(r.center().x(), r.top()));
            }
            if (resizingBottom || canResizeLongCaptureEdge(SelectionDragMode::ResizeBottom)) {
                points.append(QPoint(r.center().x(), r.bottom()));
            }
        } else {
            points = {
                r.topLeft(), QPoint(r.center().x(), r.top()), r.topRight(),
                QPoint(r.left(), r.center().y()), QPoint(r.right(), r.center().y()),
                r.bottomLeft(), QPoint(r.center().x(), r.bottom()), r.bottomRight()
            };
        }
        for (const QPoint& p : points) {
            painter.setBrush(Qt::white);
            painter.drawEllipse(QRectF(p.x() - h / 2.0, p.y() - h / 2.0, h, h));
        }
    }
    painter.restore();
}

void CaptureOverlayWindow::drawLabels(QPainter& painter) const
{
    QFont font = captureInfoFont();
    QFontMetrics metrics(font);

    QRect r = pointerRect();
    if (captureSettings_.showSizeLabel && r.isValid()) {
        const QString text = QStringLiteral("%1 × %2 px").arg(r.width()).arg(r.height());
        const int textWidth = metrics.horizontalAdvance(text);
        const QSize textSize = textWidth > 0
            ? QSize(textWidth + 18, 24)
            : QSize(92, 24);
        QRect label(QPoint(r.left(), qMax(6, r.top() - textSize.height() - 7)), textSize);
        if (label.left() < 6) {
            label.moveLeft(6);
        }
        const QImage labelImage = renderSizeLabelImage(text, textSize, font);
        painter.drawImage(label.topLeft(), labelImage);
    }

    if (captureSettings_.showCursorColor && !captureSettings_.showMagnifier && shouldShowPointerInfo()) {
        const QColor c = pixelColorAt(desktopImage_, lastMouse_);
        const QString text = captureCursorColorDisplayText(c);
        QRect label(lastMouse_ + QPoint(16, 20), QSize(198, 30));
        label = adjustedFloatingRect(label);
        const QImage labelImage = renderCursorColorLabelImage(text, c, label.size(), font);
        painter.drawImage(label.topLeft(), labelImage);
    }
}

void CaptureOverlayWindow::drawMagnifier(QPainter& painter) const
{
    if (!captureSettings_.showMagnifier || !shouldShowPointerInfo()) {
        return;
    }

    constexpr int previewW = 174;
    constexpr int previewH = 94;
    constexpr int infoH = 48;

    const QSize panelSize(previewW, previewH + infoH);
    QRect panel(lastMouse_ + QPoint(18, 18), panelSize);
    if (panel.right() > width() - 8) {
        panel.moveLeft(lastMouse_.x() - panel.width() - 18);
    }
    if (panel.bottom() > height() - 8) {
        panel.moveTop(lastMouse_.y() - panel.height() - 18);
    }
    panel = adjustedFloatingRect(panel);

    const QPoint global = lastMouse_ + virtualGeometry_.topLeft();
    const QImage panelImage = buildCaptureMagnifierPanelImage(
        desktopImage_, lastMouse_, global, captureSettings_.borderColor,
        captureSettings_.showCursorColor);
    painter.drawImage(panel.topLeft(), panelImage);
}

void CaptureOverlayWindow::renderAnnotationsToImage(QImage& image, const QImage& base, bool includeCurrent, const QPointF& offset) const
{
    renderAnnotationDocumentToImage(image, base, annotations_, includeCurrent, offset);
}

void CaptureOverlayWindow::invalidateSelectionComposite()
{
    selectionCompositeValid_ = false;
}

QRect CaptureOverlayWindow::pointerOverlayDirtyRect(const QPoint& pointer) const
{
    if (!rect().contains(pointer)) {
        return QRect();
    }
    QRect dirty;
    // The magnifier panel (174x142) and the cursor colour chip (198x30) are
    // anchored to the pointer and flip to the opposite side near a screen
    // edge, so bound them symmetrically with a little slack.
    if (captureSettings_.showMagnifier || captureSettings_.showCursorColor) {
        constexpr int kPointerPanelReach = 240;
        dirty |= QRect(pointer, QSize(1, 1))
                     .adjusted(-kPointerPanelReach, -kPointerPanelReach,
                               kPointerPanelReach, kPointerPanelReach);
    }
    // While there is no committed selection the size label rides on
    // pointerRect(), i.e. it follows the pointer too.
    if (captureSettings_.showSizeLabel && !selection_.isValid()) {
        dirty |= QRect(pointer.x() - 16, pointer.y() - 48, 256, 60);
    }
    return dirty;
}

QRect CaptureOverlayWindow::selectionDirtyRect(const QRect& selection) const
{
    if (!selection.isValid()) {
        return QRect();
    }
    // Slack covers the cosmetic border, the round handles and the size label
    // that sits above the selection (and may overhang to the right when the
    // selection is narrower than the label).
    QRect dirty = selection.adjusted(-24, -56, 256, 24);
    // drawLabels() clamps the label's top to y=6, pushing it *below* its
    // natural spot when the selection hugs the screen's top edge; make sure
    // that clamped strip is inside the dirty rect whenever the clamp engages.
    if (captureSettings_.showSizeLabel && selection.top() < 40) {
        dirty |= QRect(qMax(6, selection.left() - 24), 6, 300, 26);
    }
    return dirty;
}

const QImage& CaptureOverlayWindow::committedSelectionComposite() const
{
    const qint64 desktopKey = desktopImage_.isNull() ? -1 : desktopImage_.cacheKey();
    const quint64 revision = annotations_.revision();
    if (selectionCompositeValid_
        && selectionCompositeRect_ == selection_
        && selectionCompositeDesktopKey_ == desktopKey
        && selectionCompositeRevision_ == revision
        && selectionCompositeTranslateEpoch_ == fastTranslateEpoch_) {
        return selectionCompositeCommitted_;
    }

    selectionCompositeBase_ = fastTranslateShown()
        ? fastTranslatedImage_
        : desktopImage_.copy(selection_);
    if (selectionCompositeBase_.format() != QImage::Format_ARGB32) {
        selectionCompositeBase_.convertTo(QImage::Format_ARGB32);
    }
    selectionCompositeCommitted_ = selectionCompositeBase_;
    // Force the copy now: the two images must not share a buffer, because
    // mosaic/eraser items read original pixels out of the base while writing
    // into the composite.
    selectionCompositeCommitted_.detach();
    renderAnnotationDocumentToImage(selectionCompositeCommitted_,
                                    selectionCompositeBase_,
                                    annotations_,
                                    /*includeCurrent=*/false,
                                    QPointF(selection_.topLeft()));

    selectionCompositeRect_ = selection_;
    selectionCompositeDesktopKey_ = desktopKey;
    selectionCompositeRevision_ = revision;
    selectionCompositeTranslateEpoch_ = fastTranslateEpoch_;
    selectionCompositeValid_ = true;
    return selectionCompositeCommitted_;
}

void CaptureOverlayWindow::renderAnnotationDocumentToImage(QImage& image,
                                                            const QImage& base,
                                                            const AnnotationDocument& document,
                                                            bool includeCurrent,
                                                            const QPointF& offset) const
{
    for (const auto& item : document.items()) {
        renderAnnotationItemToImage(image, base, item, offset);
    }
    if (includeCurrent && hasCurrentAnnotation_) {
        renderAnnotationItemToImage(image, base, currentAnnotation_, offset);
    }
}

void CaptureOverlayWindow::drawEraserControlOverlay(QPainter& painter) const
{
    if (!hasCurrentAnnotation_ || currentAnnotation_.type != AnnotationType::Eraser
        || currentAnnotation_.style.mosaicPaintMode != MosaicPaintMode::Fill) {
        return;
    }

    const QRectF r = currentAnnotation_.rect.normalized();
    if (r.isEmpty()) {
        return;
    }

    painter.save();
    QPen pen(QColor(79, 124, 255), 1.2, Qt::DashLine);
    pen.setCosmetic(true);
    painter.setPen(pen);
    painter.setBrush(QColor(79, 124, 255, 28));
    painter.drawRect(r);

    painter.setPen(QPen(QColor(255, 255, 255, 230), 1));
    painter.setBrush(QColor(79, 124, 255));
    const QVector<QPointF> handles = {r.topLeft(), r.topRight(), r.bottomLeft(), r.bottomRight()};
    for (const QPointF& point : handles) {
        painter.drawRoundedRect(QRectF(point.x() - 4, point.y() - 4, 8, 8), 2, 2);
    }
    painter.restore();
}

void CaptureOverlayWindow::drawActiveShapeEditOverlay(QPainter& painter) const
{
    if (!isActiveShapeEditable()) {
        return;
    }

    const AnnotationItem item = activeAnnotationDocument().itemAt(activeShapeEdit_.index);
    const QRectF r = item.rect.normalized();
    if (!r.isValid()) {
        return;
    }

    painter.save();

    QPen outline(QColor(79, 124, 255), 1.1, Qt::DashLine);
    outline.setCosmetic(true);
    painter.setPen(outline);
    painter.setBrush(Qt::NoBrush);
    painter.drawRect(r);

    const QPointF topMid(r.center().x(), r.top());
    const QPointF leftMid(r.left(), r.center().y());
    const QPointF rightMid(r.right(), r.center().y());
    const QPointF bottomMid(r.center().x(), r.bottom());
    const QVector<QPointF> handles = {
        r.topLeft(), topMid, r.topRight(), leftMid, rightMid, r.bottomLeft(), bottomMid, r.bottomRight()
    };

    painter.setPen(QPen(QColor(79, 124, 255), 1));
    painter.setBrush(Qt::white);
    for (const QPointF& point : handles) {
        painter.drawEllipse(QRectF(point.x() - 4.0, point.y() - 4.0, 8.0, 8.0));
    }

    painter.restore();
}

void CaptureOverlayWindow::drawActiveArrowEditOverlay(QPainter& painter) const
{
    if (!isActiveArrowEditable()) {
        return;
    }

    const AnnotationItem item = activeAnnotationDocument().itemAt(activeArrowEdit_.index);
    if (item.points.size() < 2) {
        return;
    }

    const QPointF start = item.points.first();
    const QPointF end = item.points.last();

    painter.save();

    QPen guide(QColor(79, 124, 255), 1.1, Qt::DashLine);
    guide.setCosmetic(true);
    painter.setPen(guide);
    painter.setBrush(Qt::NoBrush);
    painter.drawLine(start, end);

    painter.setPen(QPen(QColor(79, 124, 255), 1));
    painter.setBrush(Qt::white);
    painter.drawEllipse(QRectF(start.x() - 4.0, start.y() - 4.0, 8.0, 8.0));
    painter.drawEllipse(QRectF(end.x() - 4.0, end.y() - 4.0, 8.0, 8.0));

    painter.restore();
}

void CaptureOverlayWindow::mousePressEvent(QMouseEvent* event)
{
    lastMouse_ = mapFromGlobal(event->globalPosition().toPoint());

    if (longCaptureAnnotationModePending_) {
        event->accept();
        return;
    }

    if (event->button() == Qt::RightButton) {
        beginRightButtonAction();
        event->accept();
        return;
    }

    const bool insideToolbar = toolbar_ && toolbar_->isVisible() && toolbar_->geometry().contains(lastMouse_);
    const bool insideOptionsBar = optionsBar_ && optionsBar_->isVisible() && optionsBar_->geometry().contains(lastMouse_);
    if (insideToolbar || insideOptionsBar) {
        event->accept();
        return;
    }

    if (inlineTextBox_ && !inlineTextBox_->geometry().contains(lastMouse_)) {
        commitInlineTextEdit();
    }

    if (event->button() != Qt::LeftButton) {
        QWidget::mousePressEvent(event);
        return;
    }

    if (eraserFillPending_) {
        const EraserRectDragMode hit = eraserRectDragModeAt(lastMouse_);
        if (hit != EraserRectDragMode::None) {
            eraserRectDragMode_ = hit;
            eraserRectDragStart_ = lastMouse_;
            eraserRectDragStartRect_ = currentAnnotation_.rect.normalized();
            event->accept();
            return;
        }
        commitPendingEraserFill();
    }

    if (mode_ == Mode::Ready && selection_.isValid()) {
        SelectionDragMode hit = selectionDragModeAt(lastMouse_);
        if (!activeTool_.isEmpty() && !isLongCaptureToolActive() && hit == SelectionDragMode::Move) {
            hit = SelectionDragMode::None;
        }
        if (hit != SelectionDragMode::None) {
            if (fastTranslateRunning_ && hit != SelectionDragMode::Move) {
                cancelFastTranslation();
            }
            selectionDragMode_ = hit;
            selectionDragStartRect_ = selection_;
            dragStart_ = lastMouse_;
            clearActiveShapeEdit();
            clearActiveArrowEdit();
            if (hit == SelectionDragMode::Move) {
                moveStartSelectionTopLeft_ = selection_.topLeft();
                mode_ = Mode::MovingSelection;
            } else {
                mode_ = Mode::ResizingSelection;
                if (longCaptureActive_) {
                    longCaptureProgressTimer_.stop();
                    longCaptureWheelQueue_.clear();
                    longCaptureWheelDispatchScheduled_ = false;
                    gLongCaptureHookInputPaused.store(true, std::memory_order_release);
                    prepareLongCaptureResizePreview();
                    if (longCaptureViewportLayer_) {
                        longCaptureViewportLayer_->hide();
                    }
                    // Browse mode cuts a native hole over the page. Restore the
                    // overlay surface once at drag start so repainting can show
                    // the moving edge continuously; the hole is revealed again
                    // only after the resized viewport has been committed.
                    clearLongCaptureInputRegion(
                        LongCaptureInputRegionClearPolicy::CommitBeforeReveal);
                    QRegion resizeRegion(selectionDragStartRect_.adjusted(-40, -40, 40, 40));
                    resizeRegion += longCapturePreviewPanelRect().adjusted(-2, -2, 2, 2);
                    update(resizeRegion);
                    grabMouse();
                }
            }
            hideToolbar();
            event->accept();
            return;
        }
    }

    if (mode_ == Mode::Ready && isActiveShapeEditable()) {
        const ShapeEditDragMode hit = shapeEditDragModeAt(lastMouse_);
        if (hit != ShapeEditDragMode::None) {
            beginShapeEditDrag(hit, lastMouse_);
            event->accept();
            return;
        }
    }

    if (mode_ == Mode::Ready && isActiveArrowEditable()) {
        const ArrowEditDragMode hit = arrowEditDragModeAt(lastMouse_);
        if (hit != ArrowEditDragMode::None) {
            beginArrowEditDrag(hit, lastMouse_);
            event->accept();
            return;
        }
    }

    if (mode_ == Mode::Ready && !activeTool_.isEmpty() && !isLongCaptureToolActive() && rect().contains(lastMouse_)) {
        startAnnotation(lastMouse_);
        return;
    }

    if (longCaptureActive_) {
        event->accept();
        return;
    }

    if (fastTranslateRunning_) {
        cancelFastTranslation();
    }
    mode_ = Mode::Selecting;
    dragStart_ = lastMouse_;
    selection_ = QRect(dragStart_, QSize(1, 1));
    annotations_.clear();
    clearActiveShapeEdit();
    clearActiveArrowEdit();
    hideToolbar();
    update();
}

void CaptureOverlayWindow::mouseMoveEvent(QMouseEvent* event)
{
    const QPoint previousMouse = lastMouse_;
    lastMouse_ = event->position().toPoint();
    const QRect previousSelection = selection_;
    const QRect previousLongCapturePreview = longCaptureActive_
        ? longCapturePreviewPanelRect()
        : QRect();
    const QRect previousPointerOverlay = pointerOverlayDirtyRect(previousMouse);
    const QRect previousAnnotationBounds = hasCurrentAnnotation_
        ? currentAnnotation_.boundingRect().toAlignedRect()
        : QRect();
    if (eraserRectDragMode_ != EraserRectDragMode::None && eraserFillPending_) {
        const QPoint delta = lastMouse_ - eraserRectDragStart_;
        QRectF next = eraserRectDragStartRect_;
        switch (eraserRectDragMode_) {
        case EraserRectDragMode::Move:
            next.translate(delta);
            break;
        case EraserRectDragMode::ResizeTopLeft:
            next.setTopLeft(next.topLeft() + delta);
            break;
        case EraserRectDragMode::ResizeTopRight:
            next.setTopRight(next.topRight() + delta);
            break;
        case EraserRectDragMode::ResizeBottomLeft:
            next.setBottomLeft(next.bottomLeft() + delta);
            break;
        case EraserRectDragMode::ResizeBottomRight:
            next.setBottomRight(next.bottomRight() + delta);
            break;
        case EraserRectDragMode::None:
            break;
        }
        const QRect previousEraserRect = currentAnnotation_.rect.toAlignedRect();
        currentAnnotation_.rect = clampedEraserRect(next);
        QRegion dirty;
        if (!previousEraserRect.isNull()) {
            dirty += previousEraserRect.adjusted(-kAnnotationDirtySlack,
                                                 -kAnnotationDirtySlack,
                                                 kAnnotationDirtySlack,
                                                 kAnnotationDirtySlack);
        }
        dirty += currentAnnotation_.rect.toAlignedRect().adjusted(-kAnnotationDirtySlack,
                                                                  -kAnnotationDirtySlack,
                                                                  kAnnotationDirtySlack,
                                                                  kAnnotationDirtySlack);
        update(dirty);
        event->accept();
        return;
    }

    switch (mode_) {
    case Mode::Selecting:
        selection_ = normalizedFromPoints(dragStart_, lastMouse_);
        break;
    case Mode::MovingSelection: {
        const QPoint delta = lastMouse_ - dragStart_;
        QRect next(moveStartSelectionTopLeft_ + delta, selectionDragStartRect_.size());
        next.moveLeft(qBound(0, next.left(), width() - next.width()));
        next.moveTop(qBound(0, next.top(), height() - next.height()));
        selection_ = next;
        moveFastTranslationPresentation(previousSelection);
        break;
    }
    case Mode::ResizingSelection:
        selection_ = resizedSelectionRect(lastMouse_);
        break;
    case Mode::EditingShape:
        updateShapeEditDrag(lastMouse_);
        break;
    case Mode::EditingArrow:
        updateArrowEditDrag(lastMouse_);
        break;
    case Mode::DrawingAnnotation:
        updateAnnotation(lastMouse_);
        break;
    case Mode::Idle:
        break;
    case Mode::Ready:
        if (activeTool_.isEmpty()) {
            updateSelectionHoverCursor(lastMouse_);
        } else {
            const SelectionDragMode hit = selectionDragModeAt(lastMouse_);
            if (hit != SelectionDragMode::None && hit != SelectionDragMode::Move) {
                updateSelectionHoverCursor(lastMouse_);
            } else {
                const ShapeEditDragMode shapeHit = shapeEditDragModeAt(lastMouse_);
                if (shapeHit != ShapeEditDragMode::None) {
                    updateShapeEditHoverCursor(lastMouse_);
                } else if (arrowEditDragModeAt(lastMouse_) != ArrowEditDragMode::None) {
                    updateArrowEditHoverCursor(lastMouse_);
                } else {
                    syncCursorForTool();
                }
            }
        }
        break;
    }
    if (mode_ == Mode::ResizingSelection && longCaptureActive_) {
        QRegion dirty(previousSelection.adjusted(-40, -40, 40, 40));
        dirty += selection_.adjusted(-40, -40, 40, 40);
        dirty += previousLongCapturePreview.adjusted(-2, -2, 2, 2);
        dirty += longCapturePreviewPanelRect().adjusted(-2, -2, 2, 2);
        update(dirty);
        event->accept();
        return;
    }

    // A bare update() here repainted the whole virtual desktop on every mouse
    // move: on a three-monitor setup that is a 6.2 MPx drawImage plus a 6.2 MPx
    // alpha-blended mask fill, per event. Repaint only what actually moved.
    QRegion dirty;
    bool dirtyKnown = true;
    switch (mode_) {
    case Mode::Selecting:
    case Mode::MovingSelection:
    case Mode::ResizingSelection:
        dirty += selectionDirtyRect(previousSelection);
        dirty += selectionDirtyRect(selection_);
        break;
    case Mode::DrawingAnnotation:
        if (!previousAnnotationBounds.isNull()) {
            dirty += previousAnnotationBounds.adjusted(-kAnnotationDirtySlack,
                                                       -kAnnotationDirtySlack,
                                                       kAnnotationDirtySlack,
                                                       kAnnotationDirtySlack);
        }
        if (hasCurrentAnnotation_) {
            dirty += currentAnnotation_.boundingRect().toAlignedRect().adjusted(
                -kAnnotationDirtySlack, -kAnnotationDirtySlack,
                kAnnotationDirtySlack, kAnnotationDirtySlack);
        }
        break;
    case Mode::EditingShape:
    case Mode::EditingArrow:
        // These move an already committed item plus its handle overlay; the
        // affected extent is not tracked here, so stay on the safe path.
        dirtyKnown = false;
        break;
    case Mode::Ready:
        // Hover only moves the cursor and the pointer-anchored overlays.
        break;
    case Mode::Idle:
        break;
    }

    if (!dirtyKnown) {
        update();
        return;
    }

    dirty += previousPointerOverlay;
    dirty += pointerOverlayDirtyRect(lastMouse_);
    if (longCaptureActive_) {
        // The preview panel reports scroll progress; keep it refreshed on the
        // same cadence a full update() used to give it.
        dirty += previousLongCapturePreview.adjusted(-2, -2, 2, 2);
        dirty += longCapturePreviewPanelRect().adjusted(-2, -2, 2, 2);
    }
    if (!dirty.isEmpty()) {
        update(dirty);
    }
}

void CaptureOverlayWindow::mouseReleaseEvent(QMouseEvent* event)
{
    lastMouse_ = event->position().toPoint();
    if (event->button() == Qt::RightButton) {
        event->accept();
        finishRightButtonAction();
        return;
    }
    if (event->button() != Qt::LeftButton) {
        QWidget::mouseReleaseEvent(event);
        return;
    }

    if (eraserRectDragMode_ != EraserRectDragMode::None) {
        eraserRectDragMode_ = EraserRectDragMode::None;
        update();
        event->accept();
        return;
    }

    if (mode_ == Mode::Selecting) {
        finalizeSelection();
        return;
    }
    if (mode_ == Mode::MovingSelection) {
        selectionDragMode_ = SelectionDragMode::None;
        mode_ = Mode::Ready;
        placeToolbar();
        if (longCaptureActive_) {
            if (!resetLongCaptureFrames(QStringLiteral("selection_changed"))) {
                abortLongCaptureMode(QStringLiteral("selection_changed_reset_failed"));
            }
        }
        update();
        return;
    }
    if (mode_ == Mode::ResizingSelection) {
        selection_ = resizedSelectionRect(lastMouse_);
        const QRect previousSelection = selectionDragStartRect_;
        if (longCaptureActive_) {
            releaseMouse();
        }
        selectionDragMode_ = SelectionDragMode::None;
        clearActiveShapeEdit();
        clearActiveArrowEdit();
        if (longCaptureActive_) {
            finishLongCaptureViewportResize(previousSelection);
        } else {
            finalizeSelection();
        }
        return;
    }
    if (mode_ == Mode::EditingShape) {
        finishShapeEditDrag();
        return;
    }
    if (mode_ == Mode::EditingArrow) {
        finishArrowEditDrag();
        return;
    }
    if (mode_ == Mode::DrawingAnnotation) {
        finishAnnotation(lastMouse_);
        return;
    }
}

void CaptureOverlayWindow::keyPressEvent(QKeyEvent* event)
{
    if (event->key() == Qt::Key_T && event->modifiers() == (Qt::ControlModifier | Qt::ShiftModifier)
        && !fastTranslateRunning_ && fastTranslateShown() && fastReviewPanel_) {
        fastReviewPanel_->toggleByUser();
        event->accept();
        return;
    }
    if (event->key() == Qt::Key_Escape && fastReviewPanel_ && fastReviewPanel_->isVisible()) {
        fastReviewPanel_->toggleByUser();
        event->accept();
        return;
    }
    if (event->key() == Qt::Key_Escape) {
        if (longCaptureAnnotationMode_ || longCaptureAnnotationModePending_) {
            if (longCaptureAnnotationModePending_) {
                cancelPendingLongCaptureAnnotationMode();
            } else {
                leaveLongCaptureAnnotationMode(true);
            }
            return;
        }
        if (eraserFillPending_) {
            cancelPendingEraserFill();
            return;
        }
        if (inlineTextBox_) {
            commitInlineTextEdit();
            return;
        }
        Perf::log(QStringLiteral("CaptureOverlayWindow.closeRequested id=%1 reason=escape").arg(overlayId_));
        emit cancelled();
        close();
        return;
    }
    if (event->matches(QKeySequence::Undo)) {
        commitPendingEraserFill();
        activeAnnotationDocument().undo();
        clearActiveShapeEdit();
        clearActiveArrowEdit();
        updateAnnotationViews();
        return;
    }
    if (event->matches(QKeySequence::Redo)) {
        commitPendingEraserFill();
        activeAnnotationDocument().redo();
        clearActiveShapeEdit();
        clearActiveArrowEdit();
        updateAnnotationViews();
        return;
    }
    if (event->matches(QKeySequence::Copy) || event->key() == Qt::Key_Return || event->key() == Qt::Key_Enter) {
        requestCopy();
        return;
    }
    if (event->matches(QKeySequence::Save)) {
        requestSave();
        return;
    }
    if (event->key() == Qt::Key_P && event->modifiers() == Qt::NoModifier) {
        requestPin();
        return;
    }
    if (selection_.isValid() && !longCaptureActive_) {
        QPoint delta;
        if (event->key() == Qt::Key_Left) delta.setX(-1);
        if (event->key() == Qt::Key_Right) delta.setX(1);
        if (event->key() == Qt::Key_Up) delta.setY(-1);
        if (event->key() == Qt::Key_Down) delta.setY(1);
        if (!delta.isNull()) {
            if (event->modifiers().testFlag(Qt::ShiftModifier)) {
                delta *= 10;
            }
            const QRect previousSelection = selection_;
            QRect next = selection_.translated(delta);
            next.moveLeft(qBound(0, next.left(), width() - next.width()));
            next.moveTop(qBound(0, next.top(), height() - next.height()));
            selection_ = next;
            moveFastTranslationPresentation(previousSelection);
            placeToolbar();
            if (longCaptureActive_) {
                if (!resetLongCaptureFrames(QStringLiteral("selection_changed"))) {
                    abortLongCaptureMode(QStringLiteral("selection_changed_reset_failed"));
                }
            }
            update();
            return;
        }
    }
    QWidget::keyPressEvent(event);
}

void CaptureOverlayWindow::contextMenuEvent(QContextMenuEvent* event)
{
    event->accept();
}

bool CaptureOverlayWindow::eventFilter(QObject* watched, QEvent* event)
{
    if ((watched == toolbar_ || watched == optionsBar_ || (toolbar_ && toolbar_->isAncestorOf(qobject_cast<QWidget*>(watched)))
         || (optionsBar_ && optionsBar_->isAncestorOf(qobject_cast<QWidget*>(watched))))) {
        if (event->type() == QEvent::MouseButtonPress) {
            auto* mouseEvent = static_cast<QMouseEvent*>(event);
            if (mouseEvent->button() == Qt::RightButton) {
                lastMouse_ = mapFromGlobal(mouseEvent->globalPosition().toPoint());
                beginRightButtonAction();
                mouseEvent->accept();
                return true;
            }
        }
        if (event->type() == QEvent::MouseButtonRelease) {
            auto* mouseEvent = static_cast<QMouseEvent*>(event);
            if (mouseEvent->button() == Qt::RightButton) {
                mouseEvent->accept();
                finishRightButtonAction();
                return true;
            }
        }
        if (event->type() == QEvent::ContextMenu) {
            event->accept();
            return true;
        }
    }
    return QWidget::eventFilter(watched, event);
}

void CaptureOverlayWindow::wheelEvent(QWheelEvent* event)
{
    if (longCaptureActive_
        && gLongCaptureHookInputPaused.load(std::memory_order_acquire)) {
        event->accept();
        return;
    }
    if (longCaptureAnnotationModePending_) {
        event->accept();
        return;
    }
    if (isLongCaptureToolActive()) {
        if (mode_ != Mode::Ready) {
            event->accept();
            return;
        }
        const QPoint position = event->position().toPoint();
        if (!selection_.contains(position)) {
            longCapturePixelWheelRemainder_ = 0;
            event->accept();
            return;
        }
        if ((toolbar_ && toolbar_->isVisible() && toolbar_->geometry().contains(position))
            || longCapturePreviewPanelRect().adjusted(-2, -2, 2, 2).contains(position)) {
            longCapturePixelWheelRemainder_ = 0;
            event->accept();
            return;
        }
        int delta = event->angleDelta().y();
        if (delta == 0) {
            longCapturePixelWheelRemainder_ += event->pixelDelta().y();
            const int steps = longCapturePixelWheelRemainder_ / kLongCapturePixelWheelStep;
            if (steps != 0) {
                longCapturePixelWheelRemainder_ -= steps * kLongCapturePixelWheelStep;
                delta = steps * kLongCaptureWheelDelta;
            }
        }
        if (delta != 0) {
            QPoint globalPos = event->globalPosition().toPoint();
#ifdef Q_OS_WIN
            POINT nativeCursor{};
            if (longCaptureInputRegionApplied_ && GetCursorPos(&nativeCursor)) {
                globalPos = QPoint(nativeCursor.x, nativeCursor.y);
            }
#endif
            if (handleLongCaptureNativeWheel(delta, globalPos)) {
                if (isLongCaptureWheelHookActive(this) && !longCapturePointerWheelLogged_) {
                    longCapturePointerWheelLogged_ = true;
                    Perf::log(QStringLiteral("LongCapture.touchpad.qtWheel id=%1 angle=%2 pixel=%3 delta=%4")
                                  .arg(overlayId_)
                                  .arg(event->angleDelta().y())
                                  .arg(event->pixelDelta().y())
                                  .arg(delta));
                }
            } else {
                longCaptureProgressTimer_.stop();
                longCaptureStatus_ = QStringLiteral("无法将滚轮事件发送到目标窗口");
                update(longCapturePreviewPanelRect().adjusted(-2, -2, 2, 2));
            }
        }
        event->accept();
        return;
    }

    if (activeTool_.isEmpty() || !selection_.isValid() || mode_ != Mode::Ready) {
        QWidget::wheelEvent(event);
        return;
    }

    int steps = event->angleDelta().y() / 120;
    if (steps == 0 && event->angleDelta().y() != 0) {
        steps = event->angleDelta().y() > 0 ? 1 : -1;
    }
    if (steps == 0) {
        QWidget::wheelEvent(event);
        return;
    }

    QString tip;
    if (activeTool_ == QStringLiteral("tool-rect")) {
        const int next = qBound(1, shapeState_.strokeWidth + steps, 12);
        applyToolWidth(next);
        tip = QStringLiteral("形状边框 %1 px").arg(next);
    } else if (activeTool_ == QStringLiteral("tool-arrow")) {
        const int next = qBound(1, arrowState_.strokeWidth + steps, 12);
        applyToolWidth(next);
        tip = QStringLiteral("箭头线宽 %1 px").arg(next);
    } else if (activeTool_ == QStringLiteral("tool-mosaic")) {
        const int next = qBound(3, mosaicState_.strength + steps, 24);
        applyMosaicStrength(next);
        tip = QStringLiteral("马赛克/模糊强度 %1").arg(next);
    } else if (activeTool_ == QStringLiteral("tool-text")) {
        const int next = qBound(10, currentStyle_.fontSize + steps * 2, 48);
        applyToolWidth(next);
        tip = QStringLiteral("字号 %1").arg(next);
    } else if (activeTool_ == QStringLiteral("tool-rubber")) {
        if (eraserState_.paintMode == MosaicPaintMode::Brush) {
            const int next = qBound(4, eraserState_.size + steps * 2, 96);
            applyEraserSize(next);
            tip = QStringLiteral("橡皮擦 %1 px").arg(next);
        } else {
            event->accept();
            return;
        }
    } else if (activeTool_ == QStringLiteral("tool-number")) {
        event->accept();
        return;
    } else {
        const int next = qBound(1, currentStyle_.strokeWidth + steps, 12);
        applyToolWidth(next);
        tip = QStringLiteral("线宽 %1 px").arg(next);
    }

    if (!tip.isEmpty()) {
        QToolTip::showText(event->globalPosition().toPoint(), tip, this, QRect(), 700);
    }
    event->accept();
}

void CaptureOverlayWindow::resizeEvent(QResizeEvent*)
{
    placeToolbar();
    if (longCaptureActive_) {
        syncLongCaptureLayers();
        if (isLongCaptureBrowseMode()) {
            applyLongCaptureInputRegion();
        }
    }
}

void CaptureOverlayWindow::showEvent(QShowEvent* event)
{
    Perf::log(QStringLiteral("CaptureOverlayWindow.showEvent id=%1 spontaneous=%2")
                  .arg(overlayId_)
                  .arg(event->spontaneous()));
    QWidget::showEvent(event);
    if (isLongCaptureBrowseMode() && !longCaptureCaptureBusy_
        && mode_ != Mode::ResizingSelection) {
        refreshLongCaptureViewportLayer();
    }
}

void CaptureOverlayWindow::hideEvent(QHideEvent* event)
{
    Perf::log(QStringLiteral("CaptureOverlayWindow.hideEvent id=%1 spontaneous=%2")
                  .arg(overlayId_)
                  .arg(event->spontaneous()));
    if (longCaptureViewportLayer_) {
        longCaptureViewportLayer_->hide();
    }
    QWidget::hideEvent(event);
}

void CaptureOverlayWindow::closeEvent(QCloseEvent* event)
{
    Perf::log(QStringLiteral("CaptureOverlayWindow.closeEvent.begin id=%1 accepted=%2")
                  .arg(overlayId_)
                  .arg(event->isAccepted()));
    if (fastTranslateRunning_) {
        cancelFastTranslation();
    }
    cancelLongCaptureFrameRequest();
    longCaptureProgressTimer_.stop();
    longCaptureAnnotationModeTimer_.stop();
    longCaptureActive_ = false;
    uninstallLongCaptureWheelHook();
    clearLongCaptureInputRegion();
    destroyLongCaptureLayers();
    if (inlineTextBox_) {
        inlineTextBox_->releaseKeyboard();
    }
    releaseKeyboard();
    QWidget::closeEvent(event);
    Perf::log(QStringLiteral("CaptureOverlayWindow.closeEvent.end id=%1 accepted=%2")
                  .arg(overlayId_)
                  .arg(event->isAccepted()));
}

bool CaptureOverlayWindow::nativeEvent(const QByteArray& eventType, void* message, qintptr* result)
{
#ifdef Q_OS_WIN
    Q_UNUSED(eventType)
    auto* msg = static_cast<MSG*>(message);
    if (msg && msg->message == kLongCaptureQueuedWheelMessage) {
        if (longCaptureActive_
            && gLongCaptureHookInputPaused.load(std::memory_order_acquire)) {
            if (result) {
                *result = 0;
            }
            return true;
        }
        const QPoint globalPos(static_cast<SHORT>(LOWORD(msg->lParam)),
                               static_cast<SHORT>(HIWORD(msg->lParam)));
        enqueueLongCaptureWheel(static_cast<SHORT>(HIWORD(msg->wParam)),
                                globalPos,
                                LOWORD(msg->wParam));
        if (result) {
            *result = 0;
        }
        return true;
    }
    if (msg && msg->message == WM_MOUSEWHEEL && longCaptureActive_) {
        if (gLongCaptureHookInputPaused.load(std::memory_order_acquire)) {
            if (result) {
                *result = 0;
            }
            return true;
        }
        if (longCaptureAnnotationModePending_) {
            if (result) {
                *result = 0;
            }
            return true;
        }
        if (isLongCaptureBrowseMode()) {
            const int wheelDelta = static_cast<SHORT>(HIWORD(msg->wParam));
            const QPoint globalPos(static_cast<SHORT>(LOWORD(msg->lParam)),
                                   static_cast<SHORT>(HIWORD(msg->lParam)));
            if (handleLongCaptureNativeWheel(wheelDelta, globalPos, LOWORD(msg->wParam))) {
                if (!longCapturePointerWheelLogged_) {
                    longCapturePointerWheelLogged_ = true;
                    Perf::log(QStringLiteral("LongCapture.touchpad.legacyWheel id=%1 delta=%2 point=%3,%4")
                                  .arg(overlayId_)
                                  .arg(wheelDelta)
                                  .arg(globalPos.x())
                                  .arg(globalPos.y()));
                }
                if (result) {
                    *result = 0;
                }
                return true;
            }
        }
    }
    if (msg && msg->message == WM_POINTERWHEEL && longCaptureActive_) {
        const int wheelDelta = static_cast<SHORT>(HIWORD(msg->wParam));
        const QPoint globalPos(static_cast<SHORT>(LOWORD(msg->lParam)),
                               static_cast<SHORT>(HIWORD(msg->lParam)));
        if (gLongCaptureHookInputPaused.load(std::memory_order_acquire)) {
            if (result) {
                *result = 0;
            }
            return true;
        }
        if (longCaptureAnnotationModePending_) {
            if (result) {
                *result = 0;
            }
            return true;
        }
        if (isLongCaptureBrowseMode()) {
            if (handleLongCaptureNativeWheel(wheelDelta,
                                             globalPos,
                                             currentMouseKeyState())) {
                if (!longCapturePointerWheelLogged_) {
                    longCapturePointerWheelLogged_ = true;
                    Perf::log(QStringLiteral("LongCapture.touchpad.pointerWheel id=%1 delta=%2 point=%3,%4")
                                  .arg(overlayId_)
                                  .arg(wheelDelta)
                                  .arg(globalPos.x())
                                  .arg(globalPos.y()));
                }
                if (result) {
                    *result = 0;
                }
                return true;
            }
        }
    }
    if (msg && msg->message == WM_ERASEBKGND) {
        if (result) {
            *result = 1;
        }
        return true;
    }
#else
    Q_UNUSED(eventType)
    Q_UNUSED(message)
    Q_UNUSED(result)
#endif
    return QWidget::nativeEvent(eventType, message, result);
}

void CaptureOverlayWindow::finalizeSelection()
{
    QElapsedTimer timer;
    timer.start();
    qint64 toolbarMs = 0;
    selectionDragMode_ = SelectionDragMode::None;
    selection_ = selection_.normalized().intersected(rect());
    if (!selection_.isValid() || selection_.width() < 3 || selection_.height() < 3) {
        selection_ = QRect();
        mode_ = Mode::Idle;
        hideToolbar();
    } else {
        mode_ = Mode::Ready;
        QElapsedTimer toolbarTimer;
        toolbarTimer.start();
        placeToolbar();
        toolbarMs = toolbarTimer.elapsed();
        if (activeTool_ == QStringLiteral("tool-longshot") && !longCaptureActive_) {
            enterLongCaptureMode();
        } else if (longCaptureActive_) {
            if (!resetLongCaptureFrames(QStringLiteral("selection_changed"))) {
                abortLongCaptureMode(QStringLiteral("selection_changed_reset_failed"));
            }
        }
    }
    update();
    Perf::log(QStringLiteral("CaptureOverlayWindow.finalizeSelection id=%1 valid=%2 size=%3x%4 toolbar=%5ms total=%6ms")
                  .arg(overlayId_)
                  .arg(selection_.isValid())
                  .arg(selection_.width())
                  .arg(selection_.height())
                  .arg(toolbarMs)
                  .arg(timer.elapsed()));
}

void CaptureOverlayWindow::beginRightButtonAction()
{
    rightButtonAction_ = selection_.isValid() || mode_ == Mode::Selecting
        ? RightButtonAction::ResetSelection
        : RightButtonAction::Close;
    grabMouse();
}

void CaptureOverlayWindow::finishRightButtonAction()
{
    const RightButtonAction action = rightButtonAction_;
    rightButtonAction_ = RightButtonAction::None;
    releaseMouse();

    if (action == RightButtonAction::ResetSelection) {
        resetSelectionFromRightButton();
    } else if (action == RightButtonAction::Close) {
        QTimer::singleShot(0, this, [this]() { closeFromRightButton(); });
    }
}

void CaptureOverlayWindow::resetSelectionFromRightButton()
{
    if (fastTranslateRunning_) {
        cancelFastTranslation();
    }
    if (inlineTextBox_) {
        cancelInlineTextEdit();
    }
    cancelPendingEraserFill();
    leaveLongCaptureMode();
    selection_ = QRect();
    selectionDragMode_ = SelectionDragMode::None;
    selectionDragStartRect_ = QRect();
    dragStart_ = QPoint();
    moveStartSelectionTopLeft_ = QPoint();
    annotations_.clear();
    hasCurrentAnnotation_ = false;
    eraserRectDragMode_ = EraserRectDragMode::None;
    clearActiveShapeEdit();
    clearActiveArrowEdit();
    activeTool_.clear();
    if (toolbar_) {
        toolbar_->selectTool(QString());
    }
    mode_ = Mode::Idle;
    hideToolbar();
    syncCursorForTool();
    update();
}

void CaptureOverlayWindow::closeFromRightButton()
{
    if (inlineTextBox_) {
        cancelInlineTextEdit();
    }
    cancelPendingEraserFill();
    Perf::log(QStringLiteral("CaptureOverlayWindow.closeRequested id=%1 reason=right_button").arg(overlayId_));
    emit cancelled();
    close();
}

void CaptureOverlayWindow::installRightButtonEventFilter(QWidget* widget)
{
    if (!widget) {
        return;
    }
    widget->installEventFilter(this);
    const auto children = widget->findChildren<QWidget*>();
    for (QWidget* child : children) {
        child->installEventFilter(this);
        child->setContextMenuPolicy(Qt::PreventContextMenu);
    }
}

void CaptureOverlayWindow::placeToolbar()
{
    if (!toolbar_ || !selection_.isValid()) {
        return;
    }
    toolbar_->adjustSize();
    const QSize ts = toolbar_->sizeHint();
    toolbar_->resize(ts);

    int x = selection_.right() - ts.width() + 1;
    x = qBound(8, x, width() - ts.width() - 8);
    int y = selection_.bottom() + 6;
    if (y + ts.height() + 8 > height()) {
        y = selection_.top() - ts.height() - 6;
    }
    y = qBound(8, y, height() - ts.height() - 8);
    toolbar_->move(x, y);
    const AnnotationDocument& document = activeAnnotationDocument();
    toolbar_->setUndoAvailable(document.canUndo());
    toolbar_->setRedoAvailable(document.canRedo());
    toolbar_->show();
    toolbar_->raise();
    placeOptionsBar();
}

void CaptureOverlayWindow::hideToolbar()
{
    if (toolbar_) {
        toolbar_->hide();
    }
    hideOptionsBar();
}

void CaptureOverlayWindow::hideOptionsBar()
{
    if (standardOptionsBar_) {
        standardOptionsBar_->hide();
    }
    if (textOptionsBar_) {
        textOptionsBar_->hide();
    }
}

void CaptureOverlayWindow::requestTranslation()
{
    if (!selection_.isValid() || longCaptureActive_) {
        return;
    }
    Perf::log(QStringLiteral("FastTranslate.click id=%1 selection=%2x%3 running=%4 shown=%5")
                  .arg(overlayId_)
                  .arg(selection_.width())
                  .arg(selection_.height())
                  .arg(fastTranslateRunning_)
                  .arg(fastTranslateShown()));
    commitPendingEraserFill();
    commitInlineTextEdit();
    toggleFastTranslation();
}

bool CaptureOverlayWindow::fastTranslateShown() const
{
    return fastTranslateVisible_
        && !fastTranslatedImage_.isNull()
        && fastTranslatedRect_ == selection_;
}

void CaptureOverlayWindow::toggleFastTranslation()
{
    if (fastTranslateRunning_) {
        cancelFastTranslation();
        return;
    }
    // A selection change orphans both the shown state and the cache key.
    if (fastTranslateVisible_ && fastTranslatedRect_ != selection_) {
        fastTranslateVisible_ = false;
        updateFastTranslateToolbarState();
    }
    if (fastTranslateVisible_) {
        setFastTranslateVisible(false);
        return;
    }
    const QString language = config_ ? config_->settings().aiTranslate.targetLanguage
                                     : QStringLiteral("zh-Hans");
    const bool cloudImage = !config_
        || config_->settings().aiTranslate.usesWholeImageTranslation();
    const QString providerKey = config_
        ? config_->settings().aiTranslate.translationMethodCacheKey()
        : QStringLiteral("cloud-baidu");
    const QString ocrPackKey = cloudImage
        ? QString()
        : config_
            ? config_->settings().aiTranslate.fastOcrPackCacheKey()
            : Ocr::languagePackCacheKey(Ocr::defaultLanguagePackId());
    if (!fastTranslatedImage_.isNull()
        && fastTranslatedRect_ == selection_
        && fastTranslatedLanguage_ == language
        && fastTranslatedProviderKey_ == providerKey
        && fastTranslatedOcrPackKey_ == ocrPackKey) {
        setFastTranslateVisible(true);
        Perf::log(QStringLiteral("FastTranslate.cacheHit id=%1 selection=%2x%3")
                      .arg(overlayId_)
                      .arg(selection_.width())
                      .arg(selection_.height()));
        return;
    }
    startFastTranslation();
}

void CaptureOverlayWindow::ensureFastTranslateServices(bool cloudImage)
{
    if (!fastImage_) {
        fastImage_ = new ImageTranslationService(config_, this);
    }
    if (!fastImageConnected_) {
        fastImageConnected_ = true;
        connect(fastImage_, &ImageTranslationService::phaseChanged, this, [this](const QString& phase) {
            if (fastTranslateRunning_) {fastTranslateStatus_=phase; update();}
        });
        connect(fastImage_, &ImageTranslationService::succeeded, this,
                [this](const ImageTranslationResult& result, qint64 elapsedMs) {
            if (!fastTranslateRunning_) {
                return;
            }
            if (!fastTranslationPlacementIsCompatible(
                    fastPendingRect_, selection_)
                || result.requestedTargetLanguage != fastPendingLanguage_) {
                cancelFastTranslation();
                return;
            }

            fastTranslatedImage_ = result.image;
            fastTranslatedRect_ = selection_;
            fastTranslatedLanguage_ = fastPendingLanguage_;
            fastTranslatedProviderKey_ = fastPendingProviderKey_;
            fastTranslatedOcrPackKey_ = fastPendingOcrPackKey_;
            FastTranslationDiagnostics::recordImageResult(
                fastDiagnosticsDir_, fastTranslatedImage_, result.provider,
                result.sourceLanguage, result.targetLanguage,
                result.uploadedImageSize, result.blockCount,
                result.serverElapsedMs, elapsedMs);
            fastPendingImage_ = QImage();
            fastTranslationUnits_.clear();
            fastTranslateRunning_ = false;
            fastTranslateStatus_.clear();
            stopFastTranslateProgress();
            Perf::log(QStringLiteral("FastTranslate.cloudSucceeded id=%1 selection=%2x%3 provider=%4 blocks=%5 server=%6ms total=%7ms upload=%8x%9")
                          .arg(overlayId_)
                          .arg(fastTranslatedRect_.width())
                          .arg(fastTranslatedRect_.height())
                          .arg(result.provider)
                          .arg(result.blockCount)
                          .arg(result.serverElapsedMs)
                          .arg(fastJobTimer_.isValid() ? fastJobTimer_.elapsed() : -1)
                          .arg(result.uploadedImageSize.width())
                          .arg(result.uploadedImageSize.height()));
            if (!fastReviewPanel_) fastReviewPanel_ = new TranslationReviewPanel(this);
            fastReviewPanel_->setResult(result.blocks, selection_);
            setFastTranslateVisible(true);
            // Keep partial-result information in the existing capture status,
            // not an automatic popup. A user can request the full text explicitly.
            if (!result.notice.isEmpty()) {
                fastTranslateStatus_ = QStringLiteral("部分内容未原位显示；Shift+点击翻译查看详情");
                update();
            }
        });
        connect(fastImage_, &ImageTranslationService::failed, this,
                [this](const QString& message) {
            if (fastTranslateRunning_) {
                failFastTranslation(message);
            }
        });
    }
    if (cloudImage) {
        return;
    }
    if (!fastOcr_) {
        fastOcr_ = new OcrService(this);
    }
    if (!fastOcrConnected_) {
        fastOcrConnected_ = true;
        connect(fastOcr_, &OcrService::finished, this,
                [this](quint64 revision, const QString& packId,
                       const QVector<OcrTextLine>& lines,
                       qint64 elapsedMs) {
            if (!fastTranslateRunning_ || revision != fastOcrRevision_
                || packId != Ocr::resolvedLanguagePackId(fastPendingOcrPackId_)) {
                return;
            }
            fastOcrRevision_ = 0;
            QElapsedTimer postProcessTimer;
            postProcessTimer.start();
            QVector<Ocr::RejectedTextLine> rejectedLines;
            const QVector<OcrTextLine> translatableLines =
                Ocr::filterTranslatableLines(
                    lines, fastPendingImage_, &rejectedLines);
            const qint64 filterMs = postProcessTimer.restart();
            // Logs are written to disk in every mode; screenshot text never is.
            const auto loggedText = [](const QString& text) {
                return QStringLiteral("<%1 chars>").arg(text.size());
            };
            for (const Ocr::RejectedTextLine& rejected : rejectedLines) {
                Perf::log(QStringLiteral("FastTranslate.ocrFiltered id=%1 text=\"%2\" score=%3 box=%4,%5 %6x%7 reason=%8 peer=%9")
                              .arg(overlayId_)
                              .arg(loggedText(rejected.line.text))
                              .arg(rejected.line.score, 0, 'f', 3)
                              .arg(rejected.line.box.x())
                              .arg(rejected.line.box.y())
                              .arg(rejected.line.box.width())
                              .arg(rejected.line.box.height())
                              .arg(rejected.reason)
                              .arg(rejected.peerIndex));
            }
            const QVector<Translate::TextBlock> contextBlocks =
                Translate::mergeLinesIntoBlocks(
                    translatableLines, fastPendingImage_);
            const QVector<Translate::TextBlock> candidateUnits =
                Translate::makeLineTranslationUnits(translatableLines);
            QVector<Translate::TextBlock> preservedUnits;
            preservedUnits.reserve(candidateUnits.size());
            fastTranslationUnits_.clear();
            fastTranslationUnits_.reserve(candidateUnits.size());
            for (const Translate::TextBlock& unit : candidateUnits) {
                const Translate::BlockTranslationDecision decision =
                    Translate::classifyBlockForTranslation(unit);
                if (decision.translatable) {
                    fastTranslationUnits_.append(unit);
                    continue;
                }
                preservedUnits.append(unit);
                Perf::log(QStringLiteral("FastTranslate.regionPreserved id=%1 text=\"%2\" box=%3,%4 %5x%6 reason=%7 lexical=%8")
                              .arg(overlayId_)
                              .arg(loggedText(unit.mergedText()))
                              .arg(unit.box.x())
                              .arg(unit.box.y())
                              .arg(unit.box.width())
                              .arg(unit.box.height())
                              .arg(decision.reason)
                              .arg(decision.lexicalGraphemes));
            }
            const qint64 mergeMs = postProcessTimer.restart();
            FastTranslationDiagnostics::recordOcr(
                fastDiagnosticsDir_, lines, rejectedLines,
                contextBlocks, fastTranslationUnits_, preservedUnits,
                fastPendingImage_);
            const qint64 diagnosticsMs = postProcessTimer.elapsed();
            Perf::log(QStringLiteral("FastTranslate.ocr id=%1 lines=%2 accepted=%3 filtered=%4 contextBlocks=%5 units=%6 preserved=%7 elapsed=%8ms filter=%9ms merge=%10ms diagnostics=%11ms")
                          .arg(overlayId_)
                          .arg(lines.size())
                          .arg(translatableLines.size())
                          .arg(rejectedLines.size())
                          .arg(contextBlocks.size())
                          .arg(fastTranslationUnits_.size())
                          .arg(preservedUnits.size())
                          .arg(elapsedMs)
                          .arg(filterMs)
                          .arg(mergeMs)
                          .arg(diagnosticsMs));
            if (fastTranslationUnits_.isEmpty()) {
                failFastTranslation(QStringLiteral("未识别到可翻译的文字。"));
                return;
            }
            if (fastPendingLiteOffline_) {
                // Wrapped prose goes to the model as one paragraph; labels stay
                // single units. Pixels are still replaced line by line.
                fastParagraphPlan_ = LocalMt::planParagraphs(fastTranslationUnits_, contextBlocks);
                fastTranslateStatus_ = QStringLiteral("本机翻译中…（%1 段）").arg(fastParagraphPlan_.texts.size());
                fastTranslateStage_ = QStringLiteral("translation");
                update();
                fastLocalText_->translate(fastParagraphPlan_.texts, fastPendingLanguage_,
                                          fastPendingOfflineRoot_);
                return;
            }
            QStringList paragraphs;
            paragraphs.reserve(fastTranslationUnits_.size());
            for (const Translate::TextBlock& block : fastTranslationUnits_) {
                paragraphs.append(block.mergedText());
            }
            fastTranslateStatus_ = QStringLiteral("翻译中…（%1 段）").arg(paragraphs.size());
            fastTranslateStage_ = QStringLiteral("translation");
            update();
            fastText_->translate(paragraphs, fastPendingLanguage_);
        });
        connect(fastOcr_, &OcrService::failed, this,
                [this](quint64 revision, const QString&,
                       const QString& message) {
            if (fastTranslateRunning_ && revision == fastOcrRevision_) {
                fastOcrRevision_ = 0;
                failFastTranslation(message);
            }
        });
    }
    if (!fastText_) {
        fastText_ = new TextTranslationService(config_, this);
        connect(fastText_, &TextTranslationService::succeeded, this,
                [this](const QStringList& translations, qint64 elapsedMs) {
            if (fastTranslateRunning_) {
                finishFastTextTranslation(translations, elapsedMs, 0);
            }
        });
        connect(fastText_, &TextTranslationService::failed, this, [this](const QString& message) {
            if (fastTranslateRunning_) {
                failFastTranslation(message);
            }
        });
        connect(fastText_, &TextTranslationService::providerChanged, this,
                [this](const QString& provider, bool fallback) {
            if (!fastTranslateRunning_) {
                return;
            }
            FastTranslationDiagnostics::recordProvider(
                fastDiagnosticsDir_, provider, fallback);
            fastTranslateStatus_ = provider == QStringLiteral("baidu")
                ? QStringLiteral("正在使用百度翻译…")
                : fastTranslateStatus_;
            update();
        });
    }
    if (!fastLocalText_) {
        fastLocalText_ = new LocalTextTranslationService(this);
        connect(fastLocalText_, &LocalTextTranslationService::succeeded, this,
                [this](const QStringList& translations, qint64 elapsedMs) {
            if (!fastTranslateRunning_) {
                return;
            }
            QVector<int> unsplit;
            const QStringList perUnit = LocalMt::distributeTranslations(
                fastParagraphPlan_, fastTranslationUnits_, translations,
                fastPendingLanguage_, &unsplit);
            if (perUnit.size() != fastTranslationUnits_.size()) {
                failFastTranslation(QStringLiteral("合成译文图像失败：翻译结果与文字区域不一致。"));
                return;
            }
            int unresolvedRegions = 0;
            QVector<int> unresolvedEntries = fastLocalText_->unresolvedIndices();
            unresolvedEntries += unsplit;
            for (const int entry : std::as_const(unresolvedEntries)) {
                unresolvedRegions += static_cast<int>(fastParagraphPlan_.members.value(entry).size());
            }
            finishFastTextTranslation(perUnit, elapsedMs, unresolvedRegions);
        });
        connect(fastLocalText_, &LocalTextTranslationService::failed, this,
                [this](const QString& message) {
            if (fastTranslateRunning_) {
                failFastTranslation(message);
            }
        });
        connect(fastLocalText_, &LocalTextTranslationService::phaseChanged, this,
                [this](const QString& phase) {
            if (fastTranslateRunning_) {
                fastTranslateStatus_ = phase;
                update();
            }
        });
        connect(fastLocalText_, &LocalTextTranslationService::progress, this,
                [this](int completed, int total) {
            if (fastTranslateRunning_ && total > 0) {
                fastTranslateStatus_ = QStringLiteral("本机翻译中…（%1/%2 段）").arg(completed).arg(total);
                update();
            }
        });
    }
}

void CaptureOverlayWindow::finishFastTextTranslation(const QStringList& translations,
                                                     qint64 elapsedMs,
                                                     int unresolvedRegions)
{
    if (!fastTranslationPlacementIsCompatible(
            fastPendingRect_, selection_)) {
        // Resizing changes the source/output contract. Position-only
        // movement is safe because the request owns a frozen crop.
        cancelFastTranslation();
        return;
    }
    QElapsedTimer composeTimer;
    composeTimer.start();
    Translate::CompositionReport compositionReport;
    QImage composed = Translate::composeTranslatedImage(
        fastPendingImage_,
        fastTranslationUnits_,
        translations,
        toolStyles_.textFontFamily,
        &compositionReport);
    const qint64 composeMs = composeTimer.elapsed();
    if (composed.isNull()) {
        failFastTranslation(QStringLiteral("合成译文图像失败：翻译响应与文字区域不一致。"));
        return;
    }
    fastTranslatedImage_ = std::move(composed);
    fastTranslatedRect_ = selection_;
    fastTranslatedLanguage_ = fastPendingLanguage_;
    fastTranslatedProviderKey_ = fastPendingProviderKey_;
    fastTranslatedOcrPackKey_ = fastPendingOcrPackKey_;
    QElapsedTimer diagnosticsTimer;
    diagnosticsTimer.start();
    FastTranslationDiagnostics::recordResult(
        fastDiagnosticsDir_, fastTranslatedImage_, translations,
        compositionReport);
    const qint64 diagnosticsMs = diagnosticsTimer.elapsed();
    fastPendingImage_ = QImage();
    fastTranslationUnits_.clear();
    fastTranslateRunning_ = false;
    const int unfitCount = compositionReport.unfitIndices.size();
    QStringList notes;
    if (unfitCount > 0) {
        notes.append(QStringLiteral("%1 处空间不足").arg(unfitCount));
    }
    if (unresolvedRegions > 0) {
        notes.append(QStringLiteral("%1 处译文未通过校验").arg(unresolvedRegions));
    }
    fastTranslateStatus_ = notes.isEmpty()
        ? QString()
        : QStringLiteral("已翻译 %1 处，%2，已保留原文")
              .arg(compositionReport.appliedCount)
              .arg(notes.join(QStringLiteral("、")));
    stopFastTranslateProgress();
    Perf::log(QStringLiteral("FastTranslate.succeeded id=%1 selection=%2x%3 translate=%4ms compose=%5ms applied=%6 equivalent=%7 unfit=%8 diagnostics=%9ms total=%10ms unresolved=%11 engine=%12")
                  .arg(overlayId_)
                  .arg(fastTranslatedRect_.width())
                  .arg(fastTranslatedRect_.height())
                  .arg(elapsedMs)
                  .arg(composeMs)
                  .arg(compositionReport.appliedCount)
                  .arg(compositionReport.equivalentIndices.size())
                  .arg(unfitCount)
                  .arg(diagnosticsMs)
                  .arg(fastJobTimer_.isValid() ? fastJobTimer_.elapsed() : -1)
                  .arg(unresolvedRegions)
                  .arg(fastPendingLiteOffline_ ? QStringLiteral("lite-offline") : QStringLiteral("online-text")));
    setFastTranslateVisible(true);
    if (!notes.isEmpty()) {
        const quint64 completedJob = fastJobSerial_;
        QTimer::singleShot(5000, this, [this, completedJob]() {
            if (fastJobSerial_ == completedJob
                && !fastTranslateRunning_
                && !fastTranslateStatus_.isEmpty()) {
                fastTranslateStatus_.clear();
                update();
            }
        });
    }
}

bool CaptureOverlayWindow::fastTranslationConfigurationMatchesPending() const
{
    if (!config_) {
        return fastPendingCloudImage_;
    }

    const AiTranslateSettings& settings = config_->settings().aiTranslate;
    const bool cloudImage = settings.usesWholeImageTranslation();
    const QString ocrPackKey = cloudImage
        ? QString()
        : settings.fastOcrPackCacheKey();
    return cloudImage == fastPendingCloudImage_
        && settings.targetLanguage == fastPendingLanguage_
        && settings.translationMethodCacheKey() == fastPendingProviderKey_
        && ocrPackKey == fastPendingOcrPackKey_;
}

void CaptureOverlayWindow::startFastTranslation()
{
    if (fastReviewPanel_) fastReviewPanel_->clear();
    fastDiagnosticsDir_.clear();
    if (config_ && config_->settings().aiTranslate.isOffline()) {
        const auto& settings = config_->settings().aiTranslate;
        const QString problem = OfflineTranslationService::resourceProblem(settings.offlineResourceDirectory, settings.offlineQuality);
        if (!problem.isEmpty()) { failFastTranslation(problem); return; }
    }
    if (config_ && !config_->settings().aiTranslate.uploadConsented()) {
        failFastTranslation(QStringLiteral("尚未确认上传说明：请在首选项「翻译」中查看并确认，或改用本机离线。没有上传任何内容。"));
        return;
    }
    const bool cloudImage = !config_
        || config_->settings().aiTranslate.usesWholeImageTranslation();
    const QString requestedPackId = config_
        ? config_->settings().aiTranslate.fastOcrPackId
        : Ocr::defaultLanguagePackId();
    if (!cloudImage) {
        fastTranslateStage_ = QStringLiteral("assets");
        QString missing;
        if (!OcrService::assetsPresent(requestedPackId, &missing)) {
            failFastTranslation(QStringLiteral("缺少所选本地识别模型，请在设置中下载后重试。"));
            Perf::log(QStringLiteral("FastTranslate.assetsMissing file=%1").arg(missing));
            return;
        }
    }
    ++fastJobSerial_;
    fastTranslateRunning_ = true;
    fastPendingRect_ = selection_;
    fastPendingLanguage_ = config_ ? config_->settings().aiTranslate.targetLanguage
                                   : QStringLiteral("zh-Hans");
    fastPendingProviderKey_ = config_
        ? config_->settings().aiTranslate.translationMethodCacheKey()
        : QStringLiteral("cloud-baidu");
    fastPendingOcrPackId_ = cloudImage ? QString() : requestedPackId;
    fastPendingOcrPackKey_ = cloudImage
        ? QString()
        : Ocr::languagePackCacheKey(fastPendingOcrPackId_);
    fastPendingCloudImage_ = cloudImage;
    fastPendingLiteOffline_ = config_ && config_->settings().aiTranslate.usesLiteOfflineEngine();
    fastPendingOfflineRoot_ = config_ ? config_->settings().aiTranslate.offlineResourceDirectory : QString();
    fastParagraphPlan_ = {};
    fastTranslationUnits_.clear();
    fastTranslateStage_ = cloudImage
        ? QStringLiteral("cloud-image")
        : QStringLiteral("ocr");
    fastTranslateStatus_ = fastPendingLiteOffline_
        ? QStringLiteral("本机识别文字中，不上传截图…")
        : config_ && config_->settings().aiTranslate.isOffline()
        ? QStringLiteral("正在本机加载模型并翻译，不上传截图…") : cloudImage
        ? QStringLiteral("云端图片翻译中…")
        : QStringLiteral("识别文字中…");
    fastJobTimer_.start();
    fastPendingImage_ = QImage();
    fastGlassImage_ = QImage();
    fastWorkLaunchAwaitingPaintSerial_ = fastJobSerial_;
    if (!fastSpinnerTimer_) {
        fastSpinnerTimer_ = new QTimer(this);
        fastSpinnerTimer_->setInterval(66);
        connect(fastSpinnerTimer_, &QTimer::timeout, this, [this]() {
            if (fastTranslateRunning_ && selection_.isValid()) {
                update(selection_.adjusted(-2, -2, 2, 2));
            }
        });
    }
    fastSpinnerTimer_->start();
    updateFastTranslateToolbarState();
    update(selection_.adjusted(-2, -2, 2, 2));
    Perf::log(QStringLiteral("FastTranslate.stateReady id=%1 job=%2 pipeline=%3 selection=%4x%5 lang=%6")
                  .arg(overlayId_)
                  .arg(fastJobSerial_)
                  .arg(cloudImage ? QStringLiteral("cloud-image")
                                  : QStringLiteral("local-ocr"))
                  .arg(selection_.width())
                  .arg(selection_.height())
                  .arg(fastPendingLanguage_));
}

void CaptureOverlayWindow::prepareFastTranslationInputAndLaunch(quint64 serial)
{
    if (!fastTranslateRunning_ || fastJobSerial_ != serial) {
        return;
    }
    if (!fastTranslationPlacementIsCompatible(fastPendingRect_, selection_)) {
        cancelFastTranslation();
        return;
    }

    QElapsedTimer phaseTimer;
    phaseTimer.start();
    fastPendingImage_ = desktopImage_.copy(fastPendingRect_);
    const qint64 cropMs = phaseTimer.elapsed();
    if (fastPendingImage_.isNull()) {
        failFastTranslation(QStringLiteral("无法准备待翻译截图。"));
        return;
    }

    queueFastTranslateGlass(serial);
    Perf::log(QStringLiteral("FastTranslate.inputReady id=%1 job=%2 crop=%3ms afterState=%4ms")
                  .arg(overlayId_)
                  .arg(serial)
                  .arg(cropMs)
                  .arg(fastJobTimer_.isValid() ? fastJobTimer_.elapsed() : -1));
    beginFastTranslationWork(fastPendingCloudImage_);
}

void CaptureOverlayWindow::queueFastTranslateGlass(quint64 serial)
{
    if (fastGlassWatcher_) {
        if (fastGlassCancelFlag_) {
            fastGlassCancelFlag_->store(true, std::memory_order_release);
        }
        fastGlassQueuedSource_ = fastPendingImage_;
        fastGlassQueuedRect_ = fastPendingRect_;
        fastGlassQueuedSerial_ = serial;
        Perf::log(QStringLiteral("FastTranslate.glassQueued id=%1 job=%2 coalesced=1")
                      .arg(overlayId_)
                      .arg(serial));
        return;
    }
    startFastTranslateGlassJob(fastPendingImage_, fastPendingRect_, serial);
}

void CaptureOverlayWindow::startFastTranslateGlassJob(
    QImage source, QRect sourceRect, quint64 serial)
{
    auto* watcher = new QFutureWatcher<QImage>(this);
    auto cancelFlag = std::make_shared<std::atomic_bool>(false);
    fastGlassWatcher_ = watcher;
    fastGlassCancelFlag_ = cancelFlag;
    const qint64 queuedAtMs = Perf::elapsedMs();
    connect(watcher, &QFutureWatcher<QImage>::finished,
            this, [this, watcher, cancelFlag, serial, sourceRect, queuedAtMs]() {
        QImage result = watcher->result();
        if (fastGlassWatcher_ == watcher) {
            fastGlassWatcher_ = nullptr;
            if (fastGlassCancelFlag_ == cancelFlag) {
                fastGlassCancelFlag_.reset();
            }
        }
        watcher->deleteLater();

        const bool current = fastTranslateRunning_ && fastJobSerial_ == serial
            && fastPendingRect_ == sourceRect
            && fastTranslationPlacementIsCompatible(sourceRect, selection_);
        if (current && result.isNull()) {
            Perf::log(QStringLiteral("FastTranslate.glassFailed id=%1 job=%2 selection=%3x%4")
                          .arg(overlayId_)
                          .arg(serial)
                          .arg(sourceRect.width())
                          .arg(sourceRect.height()));
        } else if (current) {
            fastGlassImage_ = std::move(result);
            update(selection_.adjusted(-2, -2, 2, 2));
            Perf::log(QStringLiteral("FastTranslate.glassPrepared id=%1 job=%2 elapsed=%3ms afterState=%4ms")
                          .arg(overlayId_)
                          .arg(serial)
                          .arg(Perf::elapsedMs() - queuedAtMs)
                          .arg(fastJobTimer_.isValid()
                                   ? fastJobTimer_.elapsed()
                                   : -1));
        }

        if (fastGlassQueuedSerial_ == 0) {
            return;
        }
        QImage queuedSource = std::move(fastGlassQueuedSource_);
        const QRect queuedRect = fastGlassQueuedRect_;
        const quint64 queuedSerial = fastGlassQueuedSerial_;
        fastGlassQueuedSource_ = QImage();
        fastGlassQueuedRect_ = QRect();
        fastGlassQueuedSerial_ = 0;
        startFastTranslateGlassJob(
            std::move(queuedSource), queuedRect, queuedSerial);
    });
    watcher->setFuture(QtConcurrent::run(
        &fastTranslateGlassThreadPool(),
        [source = std::move(source), cancelFlag]() mutable {
            return buildFastTranslateGlassImageImpl(source, cancelFlag);
        }));
}

void CaptureOverlayWindow::beginFastTranslationWork(bool cloudImage)
{
    QElapsedTimer phaseTimer;
    phaseTimer.start();
    Perf::log(QStringLiteral("FastTranslate.launchBegin id=%1 job=%2 afterState=%3ms")
                  .arg(overlayId_)
                  .arg(fastJobSerial_)
                  .arg(fastJobTimer_.isValid() ? fastJobTimer_.elapsed() : -1));
    if (!fastTranslationConfigurationMatchesPending()) {
        cancelFastTranslation();
        return;
    }
    ensureFastTranslateServices(cloudImage);
    const qint64 serviceMs = phaseTimer.restart();
    if (!fastTranslateRunning_) {
        return;
    }
    if (!fastTranslationPlacementIsCompatible(fastPendingRect_, selection_)) {
        cancelFastTranslation();
        return;
    }
    if (fastPendingImage_.isNull()
        || fastPendingImage_.size() != fastPendingRect_.size()) {
        failFastTranslation(QStringLiteral("待翻译截图状态无效。"));
        return;
    }
    const qint64 imageReadyMs = phaseTimer.restart();
    if (cloudImage) {
        if (config_ && config_->settings().aiTranslate.isOffline()) {
            // Only temporary IPC files; no persistent screenshot diagnostics.
            fastImage_->translate(fastPendingImage_, fastPendingLanguage_);
            return;
        }
        const QString endpoint = config_
            ? config_->settings().aiTranslate.fastImageTranslateEndpoint()
            : aiTranslateDefaultFastServiceUrl() + QStringLiteral("/v1/image-translate");
        fastDiagnosticsDir_ = FastTranslationDiagnostics::beginImageTranslation(
            fastPendingImage_, overlayId_, fastJobSerial_, fastPendingRect_,
            fastPendingLanguage_, endpoint);
        const qint64 diagnosticsMs = phaseTimer.elapsed();
        Perf::log(QStringLiteral("FastTranslate.requestPreparation id=%1 job=%2 service=%3ms imageReady=%4ms diagnostics=%5ms")
                      .arg(overlayId_)
                      .arg(fastJobSerial_)
                      .arg(serviceMs)
                      .arg(imageReadyMs)
                      .arg(diagnosticsMs));
        fastImage_->translate(fastPendingImage_, fastPendingLanguage_);
        return;
    }
    fastDiagnosticsDir_ = FastTranslationDiagnostics::begin(
        fastPendingImage_, overlayId_, fastJobSerial_, fastPendingRect_,
        fastPendingLanguage_, fastPendingOcrPackId_,
        Ocr::resolvedLanguagePackId(fastPendingOcrPackId_),
        fastPendingOcrPackKey_);
    Perf::log(QStringLiteral("FastTranslate.requestPreparation id=%1 job=%2 service=%3ms imageReady=%4ms diagnostics=%5ms")
                  .arg(overlayId_)
                  .arg(fastJobSerial_)
                  .arg(serviceMs)
                  .arg(imageReadyMs)
                  .arg(phaseTimer.elapsed()));
    if (fastPendingLiteOffline_) {
        // Overlap model start-up (normally already warm) with OCR.
        LocalTextTranslationService::prewarm(fastPendingOfflineRoot_);
    }
    fastOcrRevision_ = fastOcr_->recognize(
        fastPendingImage_, fastPendingOcrPackId_);
}

void CaptureOverlayWindow::cancelFastTranslation()
{
    ++fastJobSerial_;
    fastWorkLaunchAwaitingPaintSerial_ = 0;
    fastTranslateRunning_ = false;
    if (fastImage_) {
        fastImage_->cancel();
    }
    if (fastOcr_ && fastOcrRevision_ != 0) {
        fastOcr_->cancel(fastOcrRevision_);
        fastOcrRevision_ = 0;
    }
    if (fastText_) {
        fastText_->cancel();
    }
    if (fastLocalText_) {
        fastLocalText_->cancel();
    }
    fastPendingImage_ = QImage();
    fastTranslationUnits_.clear();
    fastTranslateStatus_.clear();
    stopFastTranslateProgress();
    updateFastTranslateToolbarState();
    update();
    Perf::log(QStringLiteral("FastTranslate.cancelled id=%1").arg(overlayId_));
}

void CaptureOverlayWindow::failFastTranslation(const QString& message)
{
    FastTranslationDiagnostics::recordFailure(fastDiagnosticsDir_, fastTranslateStage_, message);
    fastWorkLaunchAwaitingPaintSerial_ = 0;
    fastTranslateRunning_ = false;
    if (fastImage_) {
        fastImage_->cancel();
    }
    if (fastOcr_ && fastOcrRevision_ != 0) {
        fastOcr_->cancel(fastOcrRevision_);
        fastOcrRevision_ = 0;
    }
    if (fastLocalText_) {
        fastLocalText_->cancel();
    }
    fastPendingImage_ = QImage();
    fastTranslationUnits_.clear();
    fastTranslateStatus_ = message;
    stopFastTranslateProgress();
    updateFastTranslateToolbarState();
    update();
    Perf::log(QStringLiteral("FastTranslate.failed id=%1 message=\"%2\"").arg(overlayId_).arg(message));

    const quint64 serial = ++fastJobSerial_;
    QTimer::singleShot(5000, this, [this, serial]() {
        if (fastJobSerial_ == serial && !fastTranslateRunning_ && !fastTranslateStatus_.isEmpty()) {
            fastTranslateStatus_.clear();
            update();
        }
    });
}

void CaptureOverlayWindow::stopFastTranslateProgress()
{
    if (fastSpinnerTimer_) {
        fastSpinnerTimer_->stop();
    }
    fastGlassImage_ = QImage();
    if (fastGlassCancelFlag_) {
        fastGlassCancelFlag_->store(true, std::memory_order_release);
    }
    fastGlassQueuedSource_ = QImage();
    fastGlassQueuedRect_ = QRect();
    fastGlassQueuedSerial_ = 0;
}

void CaptureOverlayWindow::setFastTranslateVisible(bool visible)
{
    if (fastReviewPanel_) {
        fastReviewPanel_->setAnchor(selection_);
        fastReviewPanel_->setTranslationVisible(visible);
    }
    if (fastTranslateVisible_ == visible) {
        return;
    }
    fastTranslateVisible_ = visible;
    ++fastTranslateEpoch_;
    invalidateSelectionComposite();
    updateFastTranslateToolbarState();
    update();
}

void CaptureOverlayWindow::moveFastTranslationPresentation(
    const QRect& previousSelection)
{
    if (!fastTranslateVisible_
        || fastTranslatedRect_ != previousSelection
        || !fastTranslationPlacementIsCompatible(
            previousSelection, selection_)) {
        return;
    }
    fastTranslatedRect_ = selection_;
    if (fastReviewPanel_) fastReviewPanel_->setAnchor(selection_);
}

void CaptureOverlayWindow::updateFastTranslateToolbarState()
{
    if (toolbar_) {
        toolbar_->setTranslateState(fastTranslateShown(), fastTranslateRunning_);
    }
}

void CaptureOverlayWindow::drawFastTranslateOverlay(QPainter& painter)
{
    if (!selection_.isValid() || longCaptureActive_) {
        return;
    }

    if (fastTranslateRunning_) {
        const bool launchesWorkAfterThisPaint =
            fastWorkLaunchAwaitingPaintSerial_ == fastJobSerial_;
        painter.save();
        if (!fastGlassImage_.isNull()
            && fastGlassImage_.size() == selection_.size()) {
            painter.drawImage(selection_.topLeft(), fastGlassImage_);
        } else {
            painter.fillRect(selection_, QColor(255, 255, 255, 52));
        }
        painter.setRenderHint(QPainter::Antialiasing, true);

        QFont font = painter.font();
        font.setPixelSize(13);
        const QFontMetrics metrics(font);
        const int spinnerD = 16;
        const int spinnerGap = 9;
        const int padX = 14;
        const bool showSpinner = selection_.height() >= 40;
        const int reserved = padX * 2 + (showSpinner ? spinnerD + spinnerGap : 0);
        const QString status = metrics.elidedText(fastTranslateStatus_, Qt::ElideRight,
                                                  qMax(24, selection_.width() - reserved - 8));
        QRect pill(0, 0, metrics.horizontalAdvance(status) + reserved, 32);
        pill.moveCenter(selection_.center());
        painter.setPen(Qt::NoPen);
        painter.setBrush(QColor(20, 24, 32, 196));
        painter.drawRoundedRect(pill, 16, 16);

        int x = pill.left() + padX;
        if (showSpinner) {
            const QRectF arcRect(x + 1.25, pill.center().y() - spinnerD / 2.0 + 1.25,
                                 spinnerD - 2.5, spinnerD - 2.5);
            const int angle = fastJobTimer_.isValid()
                ? static_cast<int>((fastJobTimer_.elapsed() % 1200) * 360 / 1200)
                : 0;
            painter.setBrush(Qt::NoBrush);
            painter.setPen(QPen(QColor(255, 255, 255, 64), 2.0));
            painter.drawEllipse(arcRect);
            QPen arc(QColor(255, 255, 255, 235), 2.0);
            arc.setCapStyle(Qt::RoundCap);
            painter.setPen(arc);
            painter.drawArc(arcRect, -angle * 16, 110 * 16);
            x += spinnerD + spinnerGap;
        }
        painter.setPen(QColor(245, 248, 252));
        painter.setFont(font);
        painter.drawText(QRect(x, pill.top(), pill.right() - x - padX + 1, pill.height()),
                         Qt::AlignLeft | Qt::AlignVCenter, status);
        painter.restore();

        if (fastProgressPaintSerial_ != fastJobSerial_) {
            fastProgressPaintSerial_ = fastJobSerial_;
            Perf::log(QStringLiteral("FastTranslate.firstProgressPaint id=%1 job=%2 elapsed=%3ms selection=%4x%5")
                          .arg(overlayId_)
                          .arg(fastJobSerial_)
                          .arg(fastJobTimer_.isValid() ? fastJobTimer_.elapsed() : -1)
                          .arg(selection_.width())
                          .arg(selection_.height()));
        }
        if (launchesWorkAfterThisPaint) {
            const quint64 serial = fastJobSerial_;
            fastWorkLaunchAwaitingPaintSerial_ = 0;
            QTimer::singleShot(0, this, [this, serial]() {
                if (!fastTranslateRunning_ || fastJobSerial_ != serial) {
                    return;
                }
                if (!fastTranslationPlacementIsCompatible(
                        fastPendingRect_, selection_)) {
                    cancelFastTranslation();
                    return;
                }
                prepareFastTranslationInputAndLaunch(serial);
            });
        }
        return;
    }

    if (fastTranslateStatus_.isEmpty()) {
        return;
    }
    // Failure notice: above the selection, where the toolbar (which sits
    // below) cannot occlude it; fall inside the selection at the screen top.
    QFont font = painter.font();
    font.setPixelSize(12);
    const QFontMetrics metrics(font);
    const int textWidth = metrics.horizontalAdvance(fastTranslateStatus_);
    QRect badge(selection_.left(), selection_.top() - 26 - 10, textWidth + 20, 26);
    if (badge.top() < 6) {
        badge.moveTop(selection_.top() + 10);
    }
    badge = adjustedFloatingRect(badge);

    painter.save();
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.setPen(Qt::NoPen);
    painter.setBrush(QColor(20, 24, 32, 216));
    painter.drawRoundedRect(badge, 6, 6);
    painter.setPen(QColor(240, 244, 250));
    painter.setFont(font);
    painter.drawText(badge, Qt::AlignCenter, fastTranslateStatus_);
    painter.restore();
}

void CaptureOverlayWindow::syncCursorForTool()
{
    if (selection_.isValid() && mode_ == Mode::Ready) {
        const SelectionDragMode hit = selectionDragModeAt(lastMouse_);
        if (activeTool_.isEmpty() || (hit != SelectionDragMode::None && hit != SelectionDragMode::Move)) {
            updateSelectionHoverCursor(lastMouse_);
            return;
        }
        if (activeTool_ == QStringLiteral("tool-rect") && shapeEditDragModeAt(lastMouse_) != ShapeEditDragMode::None) {
            updateShapeEditHoverCursor(lastMouse_);
            return;
        }
        if (activeTool_ == QStringLiteral("tool-mosaic") && shapeEditDragModeAt(lastMouse_) != ShapeEditDragMode::None) {
            updateShapeEditHoverCursor(lastMouse_);
            return;
        }
        if (activeTool_ == QStringLiteral("tool-arrow") && arrowEditDragModeAt(lastMouse_) != ArrowEditDragMode::None) {
            updateArrowEditHoverCursor(lastMouse_);
            return;
        }
    }

    if (activeTool_.isEmpty()) {
        setCursor(captureCursor(captureSettings_.borderColor));
    } else if (activeTool_ == QStringLiteral("tool-text")) {
        setCursor(Qt::IBeamCursor);
    } else if (activeTool_ == QStringLiteral("tool-number")) {
        setCursor(Qt::PointingHandCursor);
    } else {
        setCursor(Qt::CrossCursor);
    }
}

void CaptureOverlayWindow::applyShapeKind(ShapeKind kind)
{
    shapeState_.kind = kind;
    applyShapeStateToActiveShape();
    QTimer::singleShot(0, this, [this]() {
        rebuildOptionsBar();
        placeOptionsBar();
        update();
    });
}

void CaptureOverlayWindow::applyShapePaintMode(ShapePaintMode mode)
{
    shapeState_.mode = mode;
    if (shapeState_.mode == ShapePaintMode::Filled && shapeState_.fillAlpha <= 0) {
        shapeState_.fillAlpha = 48;
    }
    toolStyles_.shapeFilled = shapeState_.mode == ShapePaintMode::Filled;
    toolStyles_.shapeFillAlpha = shapeState_.fillAlpha;
    persistToolStyles();
    applyShapeStateToActiveShape();
    QTimer::singleShot(0, this, [this]() {
        rebuildOptionsBar();
        placeOptionsBar();
        update();
    });
}

void CaptureOverlayWindow::applyArrowHeadMode(ArrowHeadMode mode)
{
    arrowState_.headMode = mode;
    toolStyles_.arrowHeadMode = mode;
    persistToolStyles();
    applyArrowStateToActiveArrow();
    QTimer::singleShot(0, this, [this]() {
        rebuildOptionsBar();
        placeOptionsBar();
        update();
    });
}

void CaptureOverlayWindow::applyMosaicPaintMode(MosaicPaintMode mode)
{
    mosaicState_.paintMode = mode;
    toolStyles_.mosaicPaintMode = mode;
    persistToolStyles();
    if (mosaicState_.paintMode == MosaicPaintMode::Fill) {
        activateLatestShapeIfEditable();
    } else {
        clearActiveShapeEdit();
    }
    QTimer::singleShot(0, this, [this]() {
        rebuildOptionsBar();
        placeOptionsBar();
        update();
    });
}

void CaptureOverlayWindow::applyMosaicEffectMode(MosaicEffectMode mode)
{
    mosaicState_.effectMode = mode;
    toolStyles_.mosaicEffectMode = mode;
    persistToolStyles();
    applyMosaicStateToActiveRegion();
    QTimer::singleShot(0, this, [this]() {
        rebuildOptionsBar();
        placeOptionsBar();
        update();
    });
}

void CaptureOverlayWindow::applyMosaicStrength(int value)
{
    mosaicState_.strength = qBound(3, value, 24);
    currentStyle_.mosaicBlock = mosaicState_.strength;
    toolStyles_.mosaicStrength = mosaicState_.strength;
    persistToolStyles();
    applyMosaicStateToActiveRegion();
    refreshOptionsBarVisuals();
    update();
}

void CaptureOverlayWindow::applyMosaicStateToActiveRegion()
{
    if (!isActiveShapeEditable()) {
        return;
    }

    AnnotationItem item = activeAnnotationDocument().itemAt(activeShapeEdit_.index);
    if (!isFilledMosaicAnnotation(item)) {
        return;
    }
    item.style.mosaicPaintMode = MosaicPaintMode::Fill;
    item.style.mosaicEffectMode = mosaicState_.effectMode;
    item.style.mosaicBlock = mosaicState_.strength;
    activeAnnotationDocument().replaceAt(activeShapeEdit_.index, item);
    updateAnnotationViews();
}

void CaptureOverlayWindow::applyEraserPaintMode(MosaicPaintMode mode)
{
    commitPendingEraserFill();
    eraserState_.paintMode = mode;
    toolStyles_.eraserPaintMode = mode;
    persistToolStyles();
    QTimer::singleShot(0, this, [this]() {
        rebuildOptionsBar();
        placeOptionsBar();
        update();
    });
}

void CaptureOverlayWindow::applyEraserSize(int value)
{
    eraserState_.size = qBound(4, value, 96);
    toolStyles_.eraserSize = eraserState_.size;
    persistToolStyles();
    refreshOptionsBarVisuals();
    update();
}

void CaptureOverlayWindow::applyTextBold(bool enabled)
{
    currentStyle_.textBold = enabled;
    toolStyles_.textBold = enabled;
    persistToolStyles();
    applyTextStyleToInlineEditor();
    refreshOptionsBarVisuals();
    update();
}

void CaptureOverlayWindow::applyTextItalic(bool enabled)
{
    currentStyle_.textItalic = enabled;
    toolStyles_.textItalic = enabled;
    persistToolStyles();
    applyTextStyleToInlineEditor();
    refreshOptionsBarVisuals();
    update();
}

void CaptureOverlayWindow::applyTextOutline(bool enabled)
{
    currentStyle_.textOutline = enabled;
    toolStyles_.textOutline = enabled;
    persistToolStyles();
    applyTextStyleToInlineEditor();
    refreshOptionsBarVisuals();
    update();
}

void CaptureOverlayWindow::applyTextFontFamily(const QString& family)
{
    if (!family.trimmed().isEmpty()) {
        currentStyle_.fontFamily = family;
        toolStyles_.textFontFamily = family;
        persistToolStyles();
    }
    applyTextStyleToInlineEditor();
    refreshOptionsBarVisuals();
    update();
}

void CaptureOverlayWindow::applyTextFontSize(int value)
{
    currentStyle_.fontSize = qBound(6, value, 96);
    toolStyles_.textFontSize = currentStyle_.fontSize;
    persistToolStyles();
    applyTextStyleToInlineEditor();
    refreshOptionsBarVisuals();
    update();
}

QColor CaptureOverlayWindow::currentShapeColor() const
{
    return shapeState_.mode == ShapePaintMode::StrokeOnly ? shapeState_.strokeColor : shapeState_.fillColor;
}

void CaptureOverlayWindow::persistToolStyles()
{
    if (!config_) {
        return;
    }
    QElapsedTimer saveTimer;
    saveTimer.start();
    config_->mutableSettings().tools = toolStyles_;
    config_->save();
    if (saveTimer.elapsed() >= 5) {
        Perf::log(QStringLiteral("persistToolStyles.slow elapsed=%1ms").arg(saveTimer.elapsed()));
    }
}

void CaptureOverlayWindow::applyPersistedStyleForTool()
{
    const auto softFill = [](const QColor& c) { return QColor(c.red(), c.green(), c.blue(), 34); };
    if (activeTool_ == QStringLiteral("tool-pen")) {
        currentStyle_.stroke = toolStyles_.penColor;
        currentStyle_.text = toolStyles_.penColor;
        currentStyle_.fill = softFill(toolStyles_.penColor);
        currentStyle_.strokeWidth = qBound(1, toolStyles_.penWidth, 12);
    } else if (activeTool_ == QStringLiteral("tool-text")) {
        currentStyle_.stroke = toolStyles_.textColor;
        currentStyle_.text = toolStyles_.textColor;
        currentStyle_.fill = softFill(toolStyles_.textColor);
        currentStyle_.fontSize = qBound(6, toolStyles_.textFontSize, 96);
        currentStyle_.fontFamily = toolStyles_.textFontFamily;
        currentStyle_.textBold = toolStyles_.textBold;
        currentStyle_.textItalic = toolStyles_.textItalic;
        currentStyle_.textOutline = toolStyles_.textOutline;
    } else if (activeTool_ == QStringLiteral("tool-number")) {
        currentStyle_.stroke = toolStyles_.numberColor;
        currentStyle_.text = toolStyles_.numberColor;
        currentStyle_.fill = softFill(toolStyles_.numberColor);
    }
}

void CaptureOverlayWindow::applyToolColor(const QColor& color)
{
    if (!color.isValid()) {
        return;
    }
    if (activeTool_ == QStringLiteral("tool-rect")) {
        if (shapeState_.mode == ShapePaintMode::StrokeOnly) {
            shapeState_.strokeColor = color;
            toolStyles_.shapeStrokeColor = color;
        } else {
            shapeState_.fillColor = color;
            toolStyles_.shapeFillColor = color;
        }
        persistToolStyles();
        applyShapeStateToActiveShape();
    } else if (activeTool_ == QStringLiteral("tool-arrow")) {
        arrowState_.strokeColor = color;
        toolStyles_.arrowColor = color;
        persistToolStyles();
        applyArrowStateToActiveArrow();
        refreshOptionsBarVisuals();
        update();
        return;
    } else if (activeTool_ == QStringLiteral("tool-pen")) {
        currentStyle_.stroke = color;
        currentStyle_.text = color;
        currentStyle_.fill = QColor(color.red(), color.green(), color.blue(), 34);
        toolStyles_.penColor = color;
        persistToolStyles();
        refreshOptionsBarVisuals();
        update();
        return;
    } else if (activeTool_ == QStringLiteral("tool-text")) {
        currentStyle_.stroke = color;
        currentStyle_.text = color;
        currentStyle_.fill = QColor(color.red(), color.green(), color.blue(), 34);
        toolStyles_.textColor = color;
        persistToolStyles();
        applyTextStyleToInlineEditor();
        refreshOptionsBarVisuals();
        update();
        return;
    } else {
        currentStyle_.stroke = color;
        currentStyle_.text = color;
        currentStyle_.fill = QColor(color.red(), color.green(), color.blue(), 34);
        if (activeTool_ == QStringLiteral("tool-number")) {
            toolStyles_.numberColor = color;
            persistToolStyles();
        }
    }
    QTimer::singleShot(0, this, [this]() {
        rebuildOptionsBar();
        placeOptionsBar();
        update();
    });
}

void CaptureOverlayWindow::applyToolWidth(int value)
{
    if (activeTool_ == QStringLiteral("tool-rect")) {
        shapeState_.strokeWidth = qBound(1, value, 12);
        toolStyles_.shapeStrokeWidth = shapeState_.strokeWidth;
        persistToolStyles();
        applyShapeStateToActiveShape();
    } else if (activeTool_ == QStringLiteral("tool-arrow")) {
        arrowState_.strokeWidth = qBound(1, value, 12);
        toolStyles_.arrowWidth = arrowState_.strokeWidth;
        persistToolStyles();
        applyArrowStateToActiveArrow();
        refreshOptionsBarVisuals();
        update();
        return;
    } else if (activeTool_ == QStringLiteral("tool-pen")) {
        currentStyle_.strokeWidth = qBound(1, value, 12);
        toolStyles_.penWidth = currentStyle_.strokeWidth;
        persistToolStyles();
        refreshOptionsBarVisuals();
        update();
        return;
    } else if (activeTool_ == QStringLiteral("tool-mosaic")) {
        applyMosaicStrength(value);
        return;
    } else if (activeTool_ == QStringLiteral("tool-text")) {
        applyTextFontSize(value);
        return;
    } else {
        currentStyle_.strokeWidth = qBound(1, value, 12);
    }
    rebuildOptionsBar();
    placeOptionsBar();
    update();
}


void CaptureOverlayWindow::refreshOptionsBarVisuals()
{
    QColor selectedColor = currentStyle_.stroke;
    if (activeTool_ == QStringLiteral("tool-rect")) {
        selectedColor = currentShapeColor();
    } else if (activeTool_ == QStringLiteral("tool-arrow")) {
        selectedColor = arrowState_.strokeColor;
    } else if (activeTool_ == QStringLiteral("tool-text")) {
        selectedColor = currentStyle_.text;
    }

    if (optionCurrentColorButton_) {
        const QSize chipSize(26, 26);
        auto* colorChip = static_cast<CurrentColorChip*>(optionCurrentColorButton_.data());
        optionCurrentColorButton_->setFixedSize(chipSize);
        colorChip->setColor(selectedColor);
        optionCurrentColorButton_->setFixedSize(chipSize);
    }

    if (optionStrokePreviewButton_) {
        if (activeTool_ == QStringLiteral("tool-arrow")) {
            optionStrokePreviewButton_->setIcon(strokePreviewIcon(arrowState_.strokeColor, arrowState_.strokeWidth, true));
            optionStrokePreviewButton_->setToolTip(QStringLiteral("箭头线宽，滚轮调整 1–12px"));
        } else if (activeTool_ == QStringLiteral("tool-pen")) {
            optionStrokePreviewButton_->setIcon(strokePreviewIcon(currentStyle_.stroke, currentStyle_.strokeWidth, true));
            optionStrokePreviewButton_->setToolTip(QStringLiteral("画笔大小，滚轮调整 1–12px"));
        } else if (activeTool_ == QStringLiteral("tool-mosaic")) {
            optionStrokePreviewButton_->setIcon(mosaicBrushIcon(mosaicState_.strength, mosaicState_.paintMode == MosaicPaintMode::Brush));
            optionStrokePreviewButton_->setToolTip(QStringLiteral("手动涂抹马赛克 / 模糊，滚轮调整 3–24"));
        } else if (activeTool_ == QStringLiteral("tool-rubber")) {
            optionStrokePreviewButton_->setIcon(rubberBrushIcon(eraserState_.size, eraserState_.paintMode == MosaicPaintMode::Brush));
            optionStrokePreviewButton_->setToolTip(QStringLiteral("像素点擦除，滚轮调整 4–96px"));
        }
    }

    if (optionMosaicStrengthSlider_) {
        const QSignalBlocker blocker(optionMosaicStrengthSlider_);
        optionMosaicStrengthSlider_->setValue(mosaicState_.strength);
    }
    if (optionMosaicStrengthLabel_) {
        optionMosaicStrengthLabel_->setText(QString::number(mosaicState_.strength));
    }
    if (optionMosaicEffectButton_) {
        optionMosaicEffectButton_->setIcon(mosaicEffectIcon(mosaicState_.effectMode));
        optionMosaicEffectButton_->setToolTip(mosaicState_.effectMode == MosaicEffectMode::GaussianBlur
                                                  ? QStringLiteral("当前高斯模糊，点击切换为马赛克")
                                                  : QStringLiteral("当前马赛克，点击切换为高斯模糊"));
    }

    if (activeTool_ == QStringLiteral("tool-text")) {
        if (optionTextBoldButton_) {
            const QSignalBlocker blocker(optionTextBoldButton_);
            optionTextBoldButton_->setChecked(currentStyle_.textBold);
        }
        if (optionTextItalicButton_) {
            const QSignalBlocker blocker(optionTextItalicButton_);
            optionTextItalicButton_->setChecked(currentStyle_.textItalic);
        }
        if (optionTextOutlineButton_) {
            const QSignalBlocker blocker(optionTextOutlineButton_);
            optionTextOutlineButton_->setChecked(currentStyle_.textOutline);
        }
        if (optionTextFontCombo_) {
            const QSignalBlocker blocker(optionTextFontCombo_);
            int fontIndex = optionTextFontCombo_->findText(
                currentStyle_.fontFamily,
                Qt::MatchFixedString | Qt::MatchCaseSensitive);
            if (fontIndex < 0) {
                optionTextFontCombo_->insertItem(0, currentStyle_.fontFamily);
                fontIndex = 0;
            }
            optionTextFontCombo_->setCurrentIndex(fontIndex);
        }
        if (optionTextSizeCombo_) {
            const QSignalBlocker blocker(optionTextSizeCombo_);
            optionTextSizeCombo_->setCurrentText(QString::number(currentStyle_.fontSize));
        }
    }

    if (optionsBar_) {
        optionsBar_->update();
    }
}

void CaptureOverlayWindow::rebuildOptionsBar()
{
    if (!standardOptionsBar_) {
        return;
    }
    if (activeTool_.isEmpty() || activeTool_ == QStringLiteral("tool-longshot")) {
        hideOptionsBar();
        return;
    }

    const bool textTool = activeTool_ == QStringLiteral("tool-text");
    QFrame* targetBar = standardOptionsBar_;
    if (textTool) {
        if (!textOptionsBar_) {
            textOptionsBar_ = new QFrame(this);
            textOptionsBar_->setObjectName(QStringLiteral("VisnipOptionsBar"));
            textOptionsBar_->setAttribute(Qt::WA_StyledBackground, true);
            textOptionsBar_->setContextMenuPolicy(Qt::PreventContextMenu);
            textOptionsBar_->setCursor(Qt::ArrowCursor);
            installRightButtonEventFilter(textOptionsBar_);
            textOptionsBar_->hide();
        }
        targetBar = textOptionsBar_;
    }
    if (optionsBar_ && optionsBar_ != targetBar) {
        optionsBar_->hide();
    }
    optionsBar_ = targetBar;

    if (textTool && optionsBar_->layout()) {
        optionStrokePreviewButton_ = nullptr;
        optionMosaicStrengthSlider_ = nullptr;
        optionMosaicStrengthLabel_ = nullptr;
        optionMosaicEffectButton_ = nullptr;
        optionCurrentColorButton_ = optionTextCurrentColorButton_;
        refreshOptionsBarVisuals();
        return;
    }

    if (auto* oldLayout = optionsBar_->layout()) {
        QLayoutItem* item = nullptr;
        while ((item = oldLayout->takeAt(0)) != nullptr) {
            if (auto* widget = item->widget()) {
                delete widget;
            }
            delete item;
        }
        delete oldLayout;
    }
    optionStrokePreviewButton_ = nullptr;
    optionCurrentColorButton_ = nullptr;
    optionMosaicStrengthSlider_ = nullptr;
    optionMosaicStrengthLabel_ = nullptr;
    optionMosaicEffectButton_ = nullptr;
    if (textTool) {
        optionTextBoldButton_ = nullptr;
        optionTextItalicButton_ = nullptr;
        optionTextOutlineButton_ = nullptr;
        optionTextCurrentColorButton_ = nullptr;
        optionTextFontCombo_ = nullptr;
        optionTextSizeCombo_ = nullptr;
    }

    const bool shapeTool = activeTool_ == QStringLiteral("tool-rect");
    const bool arrowTool = activeTool_ == QStringLiteral("tool-arrow");
    const bool penTool = activeTool_ == QStringLiteral("tool-pen");
    const bool mosaicTool = activeTool_ == QStringLiteral("tool-mosaic");
    const bool rubberTool = activeTool_ == QStringLiteral("tool-rubber");
    const bool numberTool = activeTool_ == QStringLiteral("tool-number");
    const bool richOptionsTool = shapeTool || arrowTool || penTool || mosaicTool || textTool || numberTool;
    const QSize optionsSize = optionsBarSizeForTool(activeTool_);
    auto* layout = new QHBoxLayout(optionsBar_);
    layout->setContentsMargins(7, 3, 7, 3);
    layout->setSpacing(5);
    optionsBar_->setFixedSize(optionsSize);
    optionsBar_->setStyleSheet(QStringLiteral(
        "QFrame#VisnipOptionsBar { background: rgba(255,255,255,0.96); border: 1px solid #4F7CFF; border-radius: 4px; }"
        "QToolButton { min-width: 24px; min-height: 24px; max-width: 24px; max-height: 24px; border: 1px solid #DCE5F2; border-radius: 3px; background: #FFFFFF; color: #334155; font-weight: 600; padding: 0; }"
        "QToolButton:hover { background: #F3F7FF; }"
        "QToolButton:checked { background: #0B7CFF; color: #FFFFFF; border: none; }"
        "QToolButton:disabled { background: #FFFFFF; color: #334155; border: 1px solid #DCE5F2; }"));

    auto addSeparator = [&]() {
        auto* sep = new QFrame(optionsBar_);
        sep->setFrameShape(QFrame::VLine);
        sep->setFixedSize(1, richOptionsTool ? 24 : 28);
        sep->setStyleSheet(QStringLiteral("background:#DCE5F2; border:none;"));
        layout->addWidget(sep);
    };

    auto addIconButton = [&](const QIcon& icon, const QString& tooltip, bool checked, const std::function<void()>& action) {
        auto* button = new QToolButton(optionsBar_);
        button->setIcon(icon);
        button->setIconSize(QSize(20, 20));
        button->setToolTip(tooltip);
        button->setCheckable(true);
        button->setChecked(checked);
        button->setFixedSize(24, 24);
        button->setCursor(Qt::ArrowCursor);
        layout->addWidget(button);
        connect(button, &QToolButton::clicked, this, [action]() { action(); });
        return button;
    };

    QColor selectedColor = shapeTool ? currentShapeColor() : (arrowTool ? arrowState_.strokeColor : (textTool ? currentStyle_.text : currentStyle_.stroke));

    if (shapeTool) {
        addIconButton(strokePreviewIcon(shapeState_.strokeColor, shapeState_.strokeWidth, shapeState_.mode == ShapePaintMode::StrokeOnly),
                      QStringLiteral("边框颜色 / 粗细，滚轮调整 1–12px"),
                      shapeState_.mode == ShapePaintMode::StrokeOnly,
                      [this]() { applyShapePaintMode(ShapePaintMode::StrokeOnly); });
        addIconButton(fillPreviewIcon(shapeState_.strokeColor, shapeState_.fillColor, shapeState_.fillAlpha, shapeState_.mode == ShapePaintMode::Filled, shapeState_.mode == ShapePaintMode::Filled),
                      QStringLiteral("填充颜色，点击启用半透明填充"),
                      shapeState_.mode == ShapePaintMode::Filled,
                      [this]() { applyShapePaintMode(ShapePaintMode::Filled); });
        addSeparator();
        addIconButton(shapeRectangleIcon(shapeState_.kind == ShapeKind::Rectangle),
                      QStringLiteral("矩形"),
                      shapeState_.kind == ShapeKind::Rectangle,
                      [this]() { applyShapeKind(ShapeKind::Rectangle); });
        addIconButton(shapeEllipseIcon(shapeState_.kind == ShapeKind::Ellipse),
                      QStringLiteral("圆形 / 椭圆"),
                      shapeState_.kind == ShapeKind::Ellipse,
                      [this]() { applyShapeKind(ShapeKind::Ellipse); });
        addSeparator();
    } else if (arrowTool) {
        optionStrokePreviewButton_ = addIconButton(strokePreviewIcon(arrowState_.strokeColor, arrowState_.strokeWidth, true),
                                                   QStringLiteral("箭头线宽，滚轮调整 1–12px"),
                                                   true,
                                                   [this]() { applyToolWidth(arrowState_.strokeWidth); });

        auto* modeButton = new QToolButton(optionsBar_);
        modeButton->setIcon(arrowModeIcon(arrowState_.headMode));
        modeButton->setIconSize(QSize(34, 22));
        modeButton->setToolTip(arrowModeTooltip(arrowState_.headMode));
        modeButton->setCursor(Qt::ArrowCursor);
        modeButton->setFixedSize(42, 28);
        modeButton->setPopupMode(QToolButton::InstantPopup);
        modeButton->setStyleSheet(QStringLiteral(
            "QToolButton { min-width:42px; max-width:42px; min-height:28px; max-height:28px; border:1px solid #DCE5F2; border-radius:3px; background:#FFFFFF; padding:0; margin:0; }"
            "QToolButton:hover { background:#F3F7FF; }"
            "QToolButton::menu-indicator { image:none; width:0px; }"));
        auto* menu = new QMenu(modeButton);
        menu->setCursor(Qt::ArrowCursor);
        menu->setStyleSheet(QStringLiteral(
            "QMenu { background:#FFFFFF; border:1px solid #DCE5F2; padding:2px; }"
            "QMenu::item { min-height:24px; padding:4px 18px 4px 12px; color:#334155; }"
            "QMenu::item:selected { background:#D8ECFF; color:#172033; }"));
        auto addMode = [this, menu](ArrowHeadMode mode, const QString& text) {
            QAction* action = menu->addAction(text);
            connect(action, &QAction::triggered, this, [this, mode]() { applyArrowHeadMode(mode); });
        };
        addMode(ArrowHeadMode::Line, QStringLiteral("—  直线"));
        addMode(ArrowHeadMode::SingleArrow, QStringLiteral("→  单向箭头"));
        addMode(ArrowHeadMode::DoubleArrow, QStringLiteral("↔  双向箭头"));
        modeButton->setMenu(menu);
        layout->addWidget(modeButton);
        addSeparator();
    } else if (penTool) {
        optionStrokePreviewButton_ = addIconButton(strokePreviewIcon(currentStyle_.stroke, currentStyle_.strokeWidth, true),
                                                   QStringLiteral("画笔大小，滚轮调整 1–12px"),
                                                   true,
                                                   [this]() { applyToolWidth(currentStyle_.strokeWidth); });
        addSeparator();
    } else if (mosaicTool) {
        optionStrokePreviewButton_ = addIconButton(mosaicBrushIcon(mosaicState_.strength, mosaicState_.paintMode == MosaicPaintMode::Brush),
                                                   QStringLiteral("手动涂抹马赛克 / 模糊，滚轮调整 3–24"),
                                                   mosaicState_.paintMode == MosaicPaintMode::Brush,
                                                   [this]() { applyMosaicPaintMode(MosaicPaintMode::Brush); });
        addIconButton(mosaicFillIcon(mosaicState_.paintMode == MosaicPaintMode::Fill),
                      QStringLiteral("填充马赛克 / 模糊"),
                      mosaicState_.paintMode == MosaicPaintMode::Fill,
                      [this]() { applyMosaicPaintMode(MosaicPaintMode::Fill); });
        addSeparator();

        auto* effectButton = new QToolButton(optionsBar_);
        effectButton->setIcon(mosaicEffectIcon(mosaicState_.effectMode));
        effectButton->setIconSize(QSize(20, 20));
        effectButton->setToolTip(mosaicState_.effectMode == MosaicEffectMode::GaussianBlur
                                     ? QStringLiteral("当前高斯模糊，点击切换为马赛克")
                                     : QStringLiteral("当前马赛克，点击切换为高斯模糊"));
        effectButton->setCursor(Qt::ArrowCursor);
        effectButton->setFixedSize(24, 24);
        effectButton->setStyleSheet(QStringLiteral(
            "QToolButton { min-width:24px; max-width:24px; min-height:24px; max-height:24px; border:1px solid #DCE5F2; border-radius:3px; background:#FFFFFF; padding:0; margin:0; }"
            "QToolButton:hover { background:#F3F7FF; }"));
        connect(effectButton, &QToolButton::clicked, this, [this]() {
            applyMosaicEffectMode(mosaicState_.effectMode == MosaicEffectMode::GaussianBlur
                                      ? MosaicEffectMode::Pixelate
                                      : MosaicEffectMode::GaussianBlur);
        });
        layout->addWidget(effectButton);
        optionMosaicEffectButton_ = effectButton;
        addSeparator();

        auto* strengthPane = new QFrame(optionsBar_);
        strengthPane->setFixedSize(122, 28);
        strengthPane->setCursor(Qt::ArrowCursor);
        strengthPane->setStyleSheet(QStringLiteral("QFrame { background:#FFFFFF; border:1px solid #DCE5F2; border-radius:3px; }"));
        auto* strengthLayout = new QHBoxLayout(strengthPane);
        strengthLayout->setContentsMargins(7, 0, 6, 0);
        strengthLayout->setSpacing(6);

        auto* slider = new MosaicStrengthSlider(strengthPane);
        slider->setRange(3, 24);
        slider->setValue(mosaicState_.strength);
        auto* valueLabel = new QLabel(QString::number(mosaicState_.strength), strengthPane);
        valueLabel->setFixedSize(26, 24);
        valueLabel->setAlignment(Qt::AlignCenter);
        valueLabel->setStyleSheet(QStringLiteral("QLabel { background: transparent; border: none; color:#334155; font-weight:600; }"));
        strengthLayout->addWidget(slider);
        strengthLayout->addWidget(valueLabel);
        layout->addWidget(strengthPane);
        optionMosaicStrengthSlider_ = slider;
        optionMosaicStrengthLabel_ = valueLabel;
        connect(slider, &QSlider::valueChanged, this, [this](int value) { applyMosaicStrength(value); });
        return;
    } else if (rubberTool) {
        optionStrokePreviewButton_ = addIconButton(rubberBrushIcon(eraserState_.size, eraserState_.paintMode == MosaicPaintMode::Brush),
                                                   QStringLiteral("像素点擦除，滚轮调整 4–96px"),
                                                   eraserState_.paintMode == MosaicPaintMode::Brush,
                                                   [this]() { applyEraserPaintMode(MosaicPaintMode::Brush); });
        addIconButton(rubberFillIcon(eraserState_.paintMode == MosaicPaintMode::Fill),
                      QStringLiteral("矩形擦除，可拖动和缩放"),
                      eraserState_.paintMode == MosaicPaintMode::Fill,
                      [this]() { applyEraserPaintMode(MosaicPaintMode::Fill); });
        return;
    } else if (textTool) {
        optionTextBoldButton_ = addIconButton(textOptionIcon(QStringLiteral("text-bold"), currentStyle_.textBold),
                                             QStringLiteral("加粗"),
                                             currentStyle_.textBold,
                                             [this]() { applyTextBold(!currentStyle_.textBold); });
        optionTextItalicButton_ = addIconButton(textOptionIcon(QStringLiteral("text-italic"), currentStyle_.textItalic),
                                               QStringLiteral("斜体"),
                                               currentStyle_.textItalic,
                                               [this]() { applyTextItalic(!currentStyle_.textItalic); });
        optionTextOutlineButton_ = addIconButton(textOptionIcon(QStringLiteral("text-stroke"), currentStyle_.textOutline),
                                                QStringLiteral("文字描边"),
                                                currentStyle_.textOutline,
                                                [this]() { applyTextOutline(!currentStyle_.textOutline); });
        addSeparator();

        auto* fontCombo = new QComboBox(optionsBar_);
        fontCombo->setFixedSize(148, 24);
        fontCombo->setCursor(Qt::ArrowCursor);
        fontCombo->setToolTip(QStringLiteral("字体"));
        fontCombo->setMaxVisibleItems(18);
        fontCombo->setStyleSheet(QStringLiteral(
            "QComboBox { min-height:24px; max-height:24px; border:1px solid #DCE5F2; border-radius:3px; background:#FFFFFF; padding:0 22px 0 6px; color:#334155; }"
            "QComboBox::drop-down { subcontrol-origin: padding; subcontrol-position: top right; width:18px; border:none; }"
            "QComboBox::down-arrow { image:url(:/visnip/icons/combo-arrow.svg); width:12px; height:12px; }"));

        QStringList initialFonts = {
            currentStyle_.fontFamily,
            QStringLiteral("Microsoft YaHei UI"),
            QStringLiteral("Segoe UI"),
            QStringLiteral("Arial"),
        };
        initialFonts.removeAll(QString());
        initialFonts.removeDuplicates();
        populateTextFontCombo(fontCombo, initialFonts, currentStyle_.fontFamily);
        layout->addWidget(fontCombo);
        optionTextFontCombo_ = fontCombo;
        connect(fontCombo, &QComboBox::currentTextChanged, this,
                [this](const QString& family) {
            if (!family.trimmed().isEmpty()) {
                applyTextFontFamily(family);
            }
        });

        const QFuture<QStringList>& fontCatalog = textFontCatalogFuture();
        if (fontCatalog.isFinished()) {
            populateTextFontCombo(
                fontCombo, fontCatalog.result(), currentStyle_.fontFamily);
        } else {
            auto* watcher = new QFutureWatcher<QStringList>(fontCombo);
            connect(watcher, &QFutureWatcher<QStringList>::finished,
                    fontCombo, [watcher, fontCombo]() {
                populateTextFontCombo(
                    fontCombo, watcher->result(), fontCombo->currentText());
            });
            watcher->setFuture(fontCatalog);
        }

        auto* sizeCombo = new QComboBox(optionsBar_);
        sizeCombo->setFixedSize(58, 24);
        sizeCombo->setEditable(true);
        sizeCombo->setInsertPolicy(QComboBox::NoInsert);
        sizeCombo->setToolTip(QStringLiteral("字号"));
        const QStringList sizes = {QStringLiteral("6"), QStringLiteral("8"), QStringLiteral("9"), QStringLiteral("10"), QStringLiteral("11"), QStringLiteral("12"), QStringLiteral("14"), QStringLiteral("16"), QStringLiteral("18"), QStringLiteral("20"), QStringLiteral("24"), QStringLiteral("28"), QStringLiteral("32"), QStringLiteral("36"), QStringLiteral("48"), QStringLiteral("64")};
        sizeCombo->addItems(sizes);
        sizeCombo->setCurrentText(QString::number(currentStyle_.fontSize));
        sizeCombo->setStyleSheet(QStringLiteral(
            "QComboBox { min-height:24px; max-height:24px; border:1px solid #DCE5F2; border-radius:3px; background:#FFFFFF; padding:0 18px 0 4px; color:#334155; }"
            "QComboBox::drop-down { subcontrol-origin: padding; subcontrol-position: top right; width:16px; border:none; }"
            "QComboBox::down-arrow { image:url(:/visnip/icons/combo-arrow.svg); width:12px; height:12px; }"));
        layout->addWidget(sizeCombo);
        optionTextSizeCombo_ = sizeCombo;
        connect(sizeCombo, &QComboBox::currentTextChanged, this, [this](const QString& text) {
            bool ok = false;
            const int value = text.toInt(&ok);
            if (ok) {
                applyTextFontSize(value);
            }
        });
        addSeparator();
    } else if (!numberTool) {
        QString metric;
        QString metricTip;
        if (activeTool_ == QStringLiteral("tool-mosaic")) {
            metric = QStringLiteral("M%1").arg(currentStyle_.mosaicBlock);
            metricTip = QStringLiteral("滚轮调整马赛克块大小：4–32");
        } else if (activeTool_ == QStringLiteral("tool-text")) {
            metric = QString::number(currentStyle_.fontSize);
            metricTip = QStringLiteral("滚轮调整字号：10–48");
        } else {
            metric = QStringLiteral("%1px").arg(currentStyle_.strokeWidth);
            metricTip = QStringLiteral("滚轮调整线宽：1–12");
        }

        auto* indicator = new QToolButton(optionsBar_);
        indicator->setText(metric);
        indicator->setEnabled(false);
        indicator->setToolTip(metricTip);
        indicator->setFixedSize(38, 28);
        layout->addWidget(indicator);
    }

    auto* currentColor = new CurrentColorChip(selectedColor, optionsBar_);
    currentColor->setToolTip(shapeTool
                                 ? (shapeState_.mode == ShapePaintMode::StrokeOnly ? QStringLiteral("当前边框颜色") : QStringLiteral("当前填充颜色"))
                                 : (arrowTool ? QStringLiteral("当前箭头颜色") : (penTool ? QStringLiteral("当前画笔颜色") : (numberTool ? QStringLiteral("当前序号颜色") : QStringLiteral("当前颜色")))));
    const QSize currentColorChipSize(26, 26);
    currentColor->setFixedSize(currentColorChipSize);
    currentColor->setFixedSize(currentColorChipSize);
    layout->addWidget(currentColor);
    optionCurrentColorButton_ = currentColor;
    if (textTool) {
        optionTextCurrentColorButton_ = currentColor;
    }

    addSeparator();

    auto* colorPane = new QFrame(optionsBar_);
    colorPane->setCursor(Qt::ArrowCursor);
    auto* grid = new QGridLayout(colorPane);
    const bool richPalette = richOptionsTool;
    grid->setContentsMargins(richPalette ? 2 : 0, 0, richPalette ? 2 : 0, 0);
    grid->setHorizontalSpacing(richPalette ? 1 : 3);
    grid->setVerticalSpacing(richPalette ? 1 : 3);

    const auto allColors = toolPalette();
    const int colorCount = richPalette ? allColors.size() : qMin(10, allColors.size());
    const int columns = richPalette ? 10 : 5;
    colorPane->setCursor(Qt::ArrowCursor);
    colorPane->setFixedSize(richPalette ? QSize(163, 31) : QSize(92, 35));
    for (int i = 0; i < colorCount; ++i) {
        const QColor color = allColors.at(i);
        QToolButton* button = nullptr;
        if (richPalette) {
            button = new ColorSwatchButton(color, 11, colorPane);
        } else {
            auto* toolButton = new QToolButton(colorPane);
            toolButton->setFixedSize(16, 16);
            toolButton->setStyleSheet(QStringLiteral("QToolButton { background:%1; border:1px solid %2; border-radius:2px; padding:0; margin:0; }")
                                      .arg(color.name(QColor::HexRgb), QStringLiteral("#B8C2D4")));
            button = toolButton;
        }
        button->setToolTip(color.name(QColor::HexRgb).toUpper());
        button->setCursor(Qt::ArrowCursor);
        grid->addWidget(button, i / columns, i % columns);
        connect(button, &QToolButton::clicked, this, [this, color]() { applyToolColor(color); });
    }
    layout->addWidget(colorPane);
    installRightButtonEventFilter(optionsBar_);

}

void CaptureOverlayWindow::placeOptionsBar()
{
    if (!optionsBar_ || !toolbar_ || activeTool_.isEmpty() || activeTool_ == QStringLiteral("tool-longshot")) {
        hideOptionsBar();
        return;
    }
    const QSize os = optionsBarSizeForTool(activeTool_);
    optionsBar_->resize(os);

    const QRect toolbarRect = toolbar_->geometry();
    int x = toolbarRect.left();
    if (x + os.width() > width() - 8) {
        x = toolbarRect.right() - os.width() + 1;
    }
    x = qBound(8, x, width() - os.width() - 8);

    int y = toolbarRect.bottom() + 3;
    if (y + os.height() + 8 > height()) {
        y = toolbarRect.top() - os.height() - 3;
    }
    y = qBound(8, y, height() - os.height() - 8);
    QElapsedTimer placeTimer;
    placeTimer.start();
    const bool wasHidden = !optionsBar_->isVisible();
    optionsBar_->move(x, y);
    optionsBar_->show();
    const qint64 showMs = placeTimer.elapsed();
    optionsBar_->raise();
    if (placeTimer.elapsed() >= 10) {
        Perf::log(QStringLiteral("placeOptionsBar.slow tool=%1 wasHidden=%2 cachedText=%3 show=%4ms raise=%5ms")
                      .arg(activeTool_)
                      .arg(wasHidden)
                      .arg(activeTool_ == QStringLiteral("tool-text")
                           && textOptionsBar_ && textOptionsBar_->layout())
                      .arg(showMs)
                      .arg(placeTimer.elapsed() - showMs));
    }
}

void CaptureOverlayWindow::onToolSelected(const QString& id)
{
    QElapsedTimer toolTimer;
    toolTimer.start();
    commitPendingEraserFill();
    commitInlineTextEdit();

    if (longCaptureActive_) {
        if (id == QStringLiteral("tool-longshot")) {
            if (longCaptureAnnotationMode_) {
                leaveLongCaptureAnnotationMode(true);
            } else if (longCaptureAnnotationModePending_) {
                cancelPendingLongCaptureAnnotationMode();
            } else {
                leaveLongCaptureMode();
                activeTool_.clear();
                toolbar_->selectTool(activeTool_);
                rebuildOptionsBar();
                placeOptionsBar();
                syncCursorForTool();
                update();
            }
            return;
        }

        if (longCaptureAnnotationMode_ && activeTool_ == id) {
            leaveLongCaptureAnnotationMode(true);
            return;
        }
        if (longCaptureAnnotationModePending_ && pendingLongCaptureAnnotationTool_ == id) {
            cancelPendingLongCaptureAnnotationMode();
            return;
        }
        if (longCaptureAnnotationMode_) {
            enterLongCaptureAnnotationMode(id);
        } else {
            requestLongCaptureAnnotationMode(id);
        }
        return;
    }

    activeTool_ = activeTool_ == id ? QString() : id;
    if (activeTool_ == QStringLiteral("tool-longshot")) {
        clearActiveShapeEdit();
        clearActiveArrowEdit();
        enterLongCaptureMode();
    }
    if (activeTool_ == QStringLiteral("tool-rect")) {
        activateLatestShapeIfEditable();
        clearActiveArrowEdit();
    } else if (activeTool_ == QStringLiteral("tool-mosaic") && mosaicState_.paintMode == MosaicPaintMode::Fill) {
        activateLatestShapeIfEditable();
        clearActiveArrowEdit();
    } else if (activeTool_ == QStringLiteral("tool-arrow")) {
        clearActiveShapeEdit();
        activateLatestArrowIfEditable();
    } else {
        clearActiveShapeEdit();
        clearActiveArrowEdit();
    }
    applyPersistedStyleForTool();
    if (toolbar_) {
        toolbar_->selectTool(activeTool_);
    }
    syncCursorForTool();
    const qint64 beforeOptionsMs = toolTimer.elapsed();
    rebuildOptionsBar();
    const qint64 afterRebuildMs = toolTimer.elapsed();
    placeOptionsBar();
    update();
    Perf::log(QStringLiteral("onToolSelected id=%1 tool=%2 total=%3ms preOptions=%4ms rebuildOptions=%5ms placeAndUpdate=%6ms")
                  .arg(overlayId_)
                  .arg(activeTool_.isEmpty() ? QStringLiteral("(none)") : activeTool_)
                  .arg(toolTimer.elapsed())
                  .arg(beforeOptionsMs)
                  .arg(afterRebuildMs - beforeOptionsMs)
                  .arg(toolTimer.elapsed() - afterRebuildMs));
}

bool CaptureOverlayWindow::shouldShowPointerInfo() const
{
    if (!rect().contains(lastMouse_)) {
        return false;
    }
    if (!selection_.isValid()) {
        return true;
    }
    if (!activeTool_.isEmpty()) {
        return false;
    }
    if (mode_ == Mode::Selecting || mode_ == Mode::DrawingAnnotation || mode_ == Mode::MovingSelection || mode_ == Mode::ResizingSelection) {
        return true;
    }
    return selection_.contains(lastMouse_);
}

QRect CaptureOverlayWindow::pointerRect() const
{
    QRect r = selection_.isValid() ? selection_ : QRect();
    if (!r.isValid() && rect().contains(lastMouse_)) {
        r = QRect(lastMouse_, QSize(1, 1));
    }
    return r;
}

QRect CaptureOverlayWindow::adjustedFloatingRect(QRect floating, const QSize& margin) const
{
    const int left = margin.width();
    const int top = margin.height();
    const int right = width() - margin.width();
    const int bottom = height() - margin.height();

    if (floating.right() > right) {
        floating.moveRight(right);
    }
    if (floating.left() < left) {
        floating.moveLeft(left);
    }
    if (floating.bottom() > bottom) {
        floating.moveBottom(bottom);
    }
    if (floating.top() < top) {
        floating.moveTop(top);
    }
    return floating;
}

void CaptureOverlayWindow::onToolbarAction(const QString& id)
{
    if (id == QStringLiteral("action-close")) {
        cancelPendingEraserFill();
        Perf::log(QStringLiteral("CaptureOverlayWindow.closeRequested id=%1 reason=toolbar_close").arg(overlayId_));
        emit cancelled();
        close();
        return;
    }
    commitPendingEraserFill();

    if (id == QStringLiteral("action-undo")) {
        activeAnnotationDocument().undo();
        clearActiveShapeEdit();
        clearActiveArrowEdit();
    } else if (id == QStringLiteral("action-redo")) {
        activeAnnotationDocument().redo();
        clearActiveShapeEdit();
        clearActiveArrowEdit();
    } else if (id == QStringLiteral("action-copy")) {
        requestCopy();
        return;
    } else if (id == QStringLiteral("action-save")) {
        requestSave();
        return;
    } else if (id == QStringLiteral("action-pin")) {
        requestPin();
        return;
    } else if (id == QStringLiteral("action-translate")) {
        if ((QApplication::keyboardModifiers() & Qt::ShiftModifier) && !fastTranslateRunning_
            && fastTranslateShown() && fastReviewPanel_ && fastReviewPanel_->hasResult()) {
            fastReviewPanel_->toggleByUser();
            return;
        }
        requestTranslation();
        return;
    } else if (id == QStringLiteral("action-question")) {
        requestQuestion();
        return;
    }
    updateAnnotationViews();
}

AnnotationType CaptureOverlayWindow::typeFromToolId(const QString& id) const
{
    if (id == QStringLiteral("tool-arrow")) return AnnotationType::Arrow;
    if (id == QStringLiteral("tool-pen")) return AnnotationType::Pen;
    if (id == QStringLiteral("tool-text")) return AnnotationType::Text;
    if (id == QStringLiteral("tool-mosaic")) return AnnotationType::Mosaic;
    if (id == QStringLiteral("tool-rubber")) return AnnotationType::Eraser;
    if (id == QStringLiteral("tool-number")) return AnnotationType::Number;
    return AnnotationType::Rectangle;
}

bool CaptureOverlayWindow::canStartAnnotation(const QPoint& pos) const
{
    if (!selection_.isValid() || !rect().contains(pos)) {
        return false;
    }
    return !longCaptureActive_ || (longCaptureAnnotationMode_ && selection_.contains(pos));
}

bool CaptureOverlayWindow::isLongCaptureToolActive() const
{
    return isLongCaptureBrowseMode();
}

bool CaptureOverlayWindow::isLongCaptureBrowseMode() const
{
    return longCaptureActive_ && !longCaptureAnnotationMode_
        && !longCaptureAnnotationModePending_
        && !longCaptureBrowseRevealPending_
        && activeTool_ == QStringLiteral("tool-longshot");
}

bool CaptureOverlayWindow::isLongCaptureAnnotationMode() const
{
    return longCaptureActive_ && longCaptureAnnotationMode_;
}

AnnotationDocument& CaptureOverlayWindow::activeAnnotationDocument()
{
    return longCaptureActive_ ? longCaptureAnnotations_ : annotations_;
}

const AnnotationDocument& CaptureOverlayWindow::activeAnnotationDocument() const
{
    return longCaptureActive_ ? longCaptureAnnotations_ : annotations_;
}

QRectF CaptureOverlayWindow::activeAnnotationBounds() const
{
    return longCaptureActive_ ? QRectF(selection_) : QRectF(rect());
}

void CaptureOverlayWindow::updateAnnotationViews()
{
    const AnnotationDocument& document = activeAnnotationDocument();
    if (toolbar_) {
        toolbar_->setUndoAvailable(document.canUndo());
        toolbar_->setRedoAvailable(document.canRedo());
    }
    if (longCaptureActive_) {
        longCaptureViewportComposedFrame_ = QImage();
        longCaptureViewportComposedFrameKey_ = -1;
        longCaptureLastPreviewRefreshAtMs_ = -1;
        refreshLongCapturePreviewFit();
        refreshLongCaptureViewportLayer();
        update(selection_.adjusted(-12, -12, 12, 12)
               | longCapturePreviewPanelRect().adjusted(-2, -2, 2, 2));
        return;
    }
    update();
}

void CaptureOverlayWindow::makeLongCaptureAnnotationsOverlayRelative()
{
    if (longCaptureAnnotationsOverlayRelative_) {
        return;
    }
    longCaptureAnnotationOverlayDelta_ = QPointF(selection_.left(),
                                                  selection_.top() - longCaptureCurrentY_);
    longCaptureAnnotations_.translate(longCaptureAnnotationOverlayDelta_);
    longCaptureAnnotationsOverlayRelative_ = true;
    longCaptureViewportComposedFrame_ = QImage();
    longCaptureViewportComposedFrameKey_ = -1;
}

void CaptureOverlayWindow::makeLongCaptureAnnotationsDocumentRelative()
{
    if (!longCaptureAnnotationsOverlayRelative_) {
        return;
    }
    longCaptureAnnotations_.translate(-longCaptureAnnotationOverlayDelta_);
    longCaptureAnnotationOverlayDelta_ = QPointF();
    longCaptureAnnotationsOverlayRelative_ = false;
    longCaptureViewportComposedFrame_ = QImage();
    longCaptureViewportComposedFrameKey_ = -1;
}

void CaptureOverlayWindow::requestLongCaptureAnnotationMode(const QString& toolId)
{
    if (!longCaptureActive_ || toolId.isEmpty() || toolId == QStringLiteral("tool-longshot")) {
        return;
    }
    gLongCaptureHookInputPaused.store(true, std::memory_order_release);
    longCaptureBrowseRevealPending_ = false;
    pendingLongCaptureAnnotationTool_ = toolId;
    longCaptureAnnotationModePending_ = true;
    longCaptureAnnotationStableSamples_ = 0;
    longCaptureAnnotationConfirmRejects_ = 0;
    longCaptureWheelQueue_.clear();
    longCaptureWheelDispatchScheduled_ = false;
    longCaptureProgressTimer_.stop();
    longCapturePreviewRefreshTimer_.stop();
    longCapturePreviewDirtyAtMs_ = -1;
    cancelLongCaptureFrameRequest();
#ifdef Q_OS_WIN
    const QRect pausedHookSelection = longCaptureAppliedRegionRect_.isValid()
        ? longCaptureAppliedRegionRect_.translated(longCaptureAppliedWindowGlobalTopLeft_)
        : QRect();
    const QRect pausedHookToolbar = longCaptureAppliedToolbarRegionRect_.isValid()
        ? longCaptureAppliedToolbarRegionRect_.translated(longCaptureAppliedWindowGlobalTopLeft_)
        : QRect();
    const QRect pausedHookPreview = longCaptureAppliedPreviewRegionRect_.isValid()
        ? longCaptureAppliedPreviewRegionRect_.translated(longCaptureAppliedWindowGlobalTopLeft_)
        : QRect();
#endif
    clearLongCaptureInputRegion(LongCaptureInputRegionClearPolicy::CommitBeforeReveal);
#ifdef Q_OS_WIN
    // Keep an in-progress precision-touchpad gesture from reaching the page
    // during the short settle period before annotation mode takes ownership.
    if (isLongCaptureWheelHookActive(this) && pausedHookSelection.isValid()) {
        setLongCaptureHookRegion(pausedHookSelection, pausedHookToolbar, pausedHookPreview);
    }
#endif
    if (toolbar_) {
        toolbar_->selectTool(toolId);
    }
    longCaptureStatus_ = QStringLiteral("正在确认当前画面...");
    Perf::log(QStringLiteral("LongCapture.annotation.request id=%1 tool=%2 canvasReady=%3 doc=[%4,%5)")
                  .arg(overlayId_)
                  .arg(toolId)
                  .arg(!longCaptureCanvas_.isNull())
                  .arg(longCaptureDocTopY_)
                  .arg(longCaptureDocBottomY_));
    scheduleLongCaptureAnnotationConfirmation();
    update(selection_ | longCapturePreviewPanelRect().adjusted(-2, -2, 2, 2));
}

void CaptureOverlayWindow::cancelPendingLongCaptureAnnotationMode()
{
    if (!longCaptureActive_ || !longCaptureAnnotationModePending_) {
        return;
    }
    gLongCaptureHookInputPaused.store(true, std::memory_order_release);
    const bool preserveInFlightWheel = longCaptureWheelDispatchInFlight_;
    cancelLongCaptureFrameRequest();
    Perf::log(QStringLiteral("LongCapture.annotation.cancel id=%1 tool=%2 canvasReady=%3 doc=[%4,%5)")
                  .arg(overlayId_)
                  .arg(pendingLongCaptureAnnotationTool_)
                  .arg(!longCaptureCanvas_.isNull())
                  .arg(longCaptureDocTopY_)
                  .arg(longCaptureDocBottomY_));
    longCaptureAnnotationModeTimer_.stop();
    longCaptureAnnotationModePending_ = false;
    longCaptureBrowseRevealPending_ = true;
    longCaptureAnnotationStableSamples_ = 0;
    pendingLongCaptureAnnotationTool_.clear();
    longCaptureWheelQueue_.clear();
    longCaptureWheelDispatchScheduled_ = false;
    if (!preserveInFlightWheel) {
        longCaptureWheelDispatchInFlight_ = false;
        longCaptureDispatchMovementObserved_ = false;
        longCaptureWheelDispatchAtMs_ = -1;
        longCaptureDispatchedWheelDelta_ = 0;
    }
    activeTool_ = QStringLiteral("tool-longshot");
    if (toolbar_) {
        toolbar_->selectTool(activeTool_);
    }
    longCaptureStatus_ = QStringLiteral("长截图：滚轮滚动页面，选择标注工具可暂停滚动并标注");
    const bool viewportReady = refreshLongCaptureViewportLayer(
        LongCaptureViewportRefreshPolicy::CommitBeforeReveal);
    const bool directPageFallback = longCaptureViewportExclusionChecked_
        && !longCaptureViewportExclusionAvailable_
        && longCaptureAnnotations_.count() == 0
        && (!longCaptureViewportLayer_ || !longCaptureViewportLayer_->isVisible());
    if (viewportReady || directPageFallback) {
        applyLongCaptureInputRegion();
    }
#ifdef Q_OS_WIN
    const bool inputReady = longCaptureInputRegionApplied_;
#else
    const bool inputReady = true;
#endif
    if ((viewportReady || directPageFallback) && inputReady) {
        longCaptureBrowseRevealPending_ = false;
        gLongCaptureHookInputPaused.store(false, std::memory_order_release);
        if (!longCaptureProgressTimer_.isActive()) {
            longCaptureProgressTimer_.start(kLongCaptureBurstIntervalMs);
        }
    } else {
        longCaptureViewportStable_ = false;
        longCaptureStatus_ = QStringLiteral("浏览画面恢复失败，请重新选择标注工具后重试");
        Perf::log(QStringLiteral("LongCapture.annotation.cancel.viewport_not_ready id=%1 viewport=%2 input=%3")
                      .arg(overlayId_)
                      .arg(viewportReady || directPageFallback)
                      .arg(inputReady));
    }
    update(selection_ | longCapturePreviewPanelRect().adjusted(-2, -2, 2, 2));
}

void CaptureOverlayWindow::scheduleLongCaptureAnnotationConfirmation(int minimumDelayMs)
{
    if (!longCaptureActive_ || !longCaptureAnnotationModePending_) {
        return;
    }
    const int quietRemainingMs = LongCapture::remainingQuietPeriodMs(
        Perf::elapsedMs(),
        longCaptureLastWheelAtMs_,
        longCaptureLastMovementAtMs_,
        kLongCaptureSettleDelayMs);
    longCaptureAnnotationModeTimer_.start(qMax(minimumDelayMs, quietRemainingMs));
}

void CaptureOverlayWindow::tryEnterLongCaptureAnnotationMode()
{
    if (!longCaptureActive_ || !longCaptureAnnotationModePending_) {
        return;
    }
    if (mode_ == Mode::ResizingSelection) {
        longCaptureAnnotationStableSamples_ = 0;
        scheduleLongCaptureAnnotationConfirmation(kLongCaptureAnnotationConfirmIntervalMs);
        return;
    }
    if (longCaptureCaptureBusy_) {
        if (longCaptureActiveFrameRequestId_ != 0
            && longCaptureActiveFrameResume_ == LongCaptureFrameResume::AnnotationConfirm) {
            return;
        }
        Perf::log(QStringLiteral("LongCapture.annotation.wait id=%1 reason=capture_busy")
                      .arg(overlayId_));
        scheduleLongCaptureAnnotationConfirmation(kLongCaptureAnnotationConfirmIntervalMs);
        return;
    }
    const int previousY = longCaptureCurrentY_;
    const LongCaptureFrameResult result = processLongCaptureFrame(
        LongCaptureFrameResume::AnnotationConfirm);
    Perf::log(QStringLiteral("LongCapture.annotation.try id=%1 result=%2 previousY=%3 currentY=%4 canvasReady=%5")
                  .arg(overlayId_)
                  .arg(static_cast<int>(result))
                  .arg(previousY)
                  .arg(longCaptureCurrentY_)
                  .arg(!longCaptureCanvas_.isNull()));
    if (result == LongCaptureFrameResult::Deferred) {
        if (!longCaptureCaptureBusy_
            || longCaptureActiveFrameResume_ != LongCaptureFrameResume::AnnotationConfirm) {
            scheduleLongCaptureAnnotationConfirmation(kLongCaptureAnnotationConfirmIntervalMs);
        }
        return;
    }
    if (result == LongCaptureFrameResult::Accepted
        && longCaptureCurrentY_ != previousY) {
        longCaptureAnnotationStableSamples_ = 0;
        scheduleLongCaptureAnnotationConfirmation(kLongCaptureAnnotationConfirmIntervalMs);
        return;
    }
    if (result == LongCaptureFrameResult::Rejected || result == LongCaptureFrameResult::Failed) {
        if (result == LongCaptureFrameResult::Rejected
            && ++longCaptureAnnotationConfirmRejects_ <= kLongCaptureAnnotationMaxConfirmRejects) {
            // A stationary-but-drifted page needs a few samples before the
            // repair path rewrites the committed rows; keep confirming instead
            // of bouncing the user straight back to browse mode.
            longCaptureAnnotationStableSamples_ = 0;
            scheduleLongCaptureAnnotationConfirmation(kLongCaptureAnnotationConfirmIntervalMs);
            return;
        }
        const QString status = result == LongCaptureFrameResult::Rejected
            ? QStringLiteral("当前画面未能可靠定位，请稍慢滚动后再标注")
            : QStringLiteral("当前画面采集失败，请重试");
        cancelPendingLongCaptureAnnotationMode();
        longCaptureStatus_ = status;
        update(longCapturePreviewPanelRect().adjusted(-2, -2, 2, 2));
        return;
    }
    ++longCaptureAnnotationStableSamples_;
    const int quietRemainingMs = LongCapture::remainingQuietPeriodMs(
        Perf::elapsedMs(),
        longCaptureLastWheelAtMs_,
        longCaptureLastMovementAtMs_,
        kLongCaptureSettleDelayMs);
    if (longCaptureAnnotationStableSamples_ < kLongCaptureAnnotationStableSamples
        || quietRemainingMs > 0) {
        Perf::log(QStringLiteral("LongCapture.annotation.wait id=%1 reason=stable_confirm samples=%2 quietRemaining=%3")
                      .arg(overlayId_)
                      .arg(longCaptureAnnotationStableSamples_)
                      .arg(quietRemainingMs));
        scheduleLongCaptureAnnotationConfirmation(kLongCaptureAnnotationConfirmIntervalMs);
        return;
    }
    const QString toolId = pendingLongCaptureAnnotationTool_;
    enterLongCaptureAnnotationMode(toolId);
}

void CaptureOverlayWindow::enterLongCaptureAnnotationMode(const QString& toolId)
{
    if (!longCaptureActive_ || toolId.isEmpty() || toolId == QStringLiteral("tool-longshot")) {
        return;
    }

    if (!longCaptureAnnotationMode_) {
        longCaptureProgressTimer_.stop();
        longCaptureAnnotationModeTimer_.stop();
        longCaptureWheelQueue_.clear();
        longCaptureWheelDispatchInFlight_ = false;
        longCaptureWheelDispatchScheduled_ = false;
        longCaptureDispatchMovementObserved_ = false;
        longCaptureWheelDispatchAtMs_ = -1;
        longCaptureDispatchedWheelDelta_ = 0;
        clearLongCaptureInputRegion();
#ifdef Q_OS_WIN
        clearLongCaptureHookRegion();
#endif
        makeLongCaptureAnnotationsOverlayRelative();
        longCaptureAnnotationMode_ = true;
    }

    longCaptureAnnotationModePending_ = false;
    longCaptureBrowseRevealPending_ = false;
    longCaptureAnnotationStableSamples_ = 0;
    pendingLongCaptureAnnotationTool_.clear();
    gLongCaptureHookInputPaused.store(false, std::memory_order_release);
    hasCurrentAnnotation_ = false;
    eraserFillPending_ = false;
    mode_ = Mode::Ready;
    activeTool_ = toolId;
    clearActiveShapeEdit();
    clearActiveArrowEdit();
    if (activeTool_ == QStringLiteral("tool-rect")
        || (activeTool_ == QStringLiteral("tool-mosaic")
            && mosaicState_.paintMode == MosaicPaintMode::Fill)) {
        activateLatestShapeIfEditable();
    } else if (activeTool_ == QStringLiteral("tool-arrow")) {
        activateLatestArrowIfEditable();
    }
    applyPersistedStyleForTool();
    if (toolbar_) {
        toolbar_->selectTool(activeTool_);
    }
    rebuildOptionsBar();
    placeOptionsBar();
    syncCursorForTool();
    longCaptureStatus_ = QStringLiteral("标注模式：再次点击当前工具或按 Esc 继续滚动");
    Perf::log(QStringLiteral("LongCapture.annotation.enter id=%1 tool=%2 currentY=%3 doc=[%4,%5)")
                  .arg(overlayId_)
                  .arg(toolId)
                  .arg(longCaptureCurrentY_)
                  .arg(longCaptureDocTopY_)
                  .arg(longCaptureDocBottomY_));
    updateAnnotationViews();
}

void CaptureOverlayWindow::leaveLongCaptureAnnotationMode(bool cancelUnfinished)
{
    if (!longCaptureActive_ || !longCaptureAnnotationMode_) {
        return;
    }
    const QString annotationTool = activeTool_;
    gLongCaptureHookInputPaused.store(true, std::memory_order_release);
    longCaptureBrowseRevealPending_ = true;
    longCaptureAnnotationModeTimer_.stop();
    longCaptureProgressTimer_.stop();
    commitInlineTextEdit();
    if (cancelUnfinished) {
        if (mode_ == Mode::DrawingAnnotation) {
            hasCurrentAnnotation_ = false;
            eraserFillPending_ = false;
        } else if (mode_ == Mode::EditingShape && isActiveShapeEditable()) {
            AnnotationItem item = activeAnnotationDocument().itemAt(activeShapeEdit_.index);
            item.rect = activeShapeEdit_.dragStartRect;
            activeAnnotationDocument().replaceAt(activeShapeEdit_.index, item);
        } else if (mode_ == Mode::EditingArrow && isActiveArrowEditable()) {
            AnnotationItem item = activeAnnotationDocument().itemAt(activeArrowEdit_.index);
            item.points = activeArrowEdit_.dragStartPoints;
            activeAnnotationDocument().replaceAt(activeArrowEdit_.index, item);
        }
        if (eraserRectDragMode_ != EraserRectDragMode::None) {
            currentAnnotation_.rect = eraserRectDragStartRect_;
        }
    }
    if (eraserFillPending_) {
        commitPendingEraserFill();
    }
    clearActiveShapeEdit();
    clearActiveArrowEdit();
    eraserRectDragMode_ = EraserRectDragMode::None;
    mode_ = Mode::Ready;
    makeLongCaptureAnnotationsDocumentRelative();
    longCaptureAnnotationMode_ = false;
    longCaptureViewportStable_ = true;
    activeTool_ = QStringLiteral("tool-longshot");
    if (toolbar_) {
        toolbar_->selectTool(activeTool_);
    }
    hideOptionsBar();
    syncCursorForTool();
    longCaptureStatus_ = QStringLiteral("长截图：滚轮滚动页面，选择标注工具可暂停滚动并标注");
    QElapsedTimer transitionTimer;
    transitionTimer.start();
    const bool viewportReady = refreshLongCaptureViewportLayer(
        LongCaptureViewportRefreshPolicy::CommitBeforeReveal);
    const bool directPageFallback = longCaptureViewportExclusionChecked_
        && !longCaptureViewportExclusionAvailable_
        && longCaptureAnnotations_.count() == 0
        && (!longCaptureViewportLayer_ || !longCaptureViewportLayer_->isVisible());
    const bool canRevealViewport = viewportReady || directPageFallback;
    const qint64 viewportMs = transitionTimer.elapsed();
    transitionTimer.restart();
    if (canRevealViewport) {
        applyLongCaptureInputRegion();
    }
#ifdef Q_OS_WIN
    const bool inputReady = longCaptureInputRegionApplied_;
#else
    const bool inputReady = true;
#endif
    if (canRevealViewport && inputReady) {
        longCaptureBrowseRevealPending_ = false;
        gLongCaptureHookInputPaused.store(false, std::memory_order_release);
    } else {
        longCaptureViewportStable_ = false;
        longCaptureStatus_ = QStringLiteral("标注画面提交失败，请重新选择标注工具后重试");
        Perf::log(QStringLiteral("LongCapture.annotation.leave.viewport_not_ready id=%1 viewport=%2 input=%3")
                      .arg(overlayId_)
                      .arg(canRevealViewport)
                      .arg(inputReady));
    }
    const qint64 regionMs = transitionTimer.elapsed();
    Perf::log(QStringLiteral("LongCapture.annotation.leave.transition id=%1 ready=%2 direct=%3 viewport=%4ms region=%5ms total=%6ms")
                  .arg(overlayId_)
                  .arg(viewportReady)
                  .arg(directPageFallback)
                  .arg(viewportMs)
                  .arg(regionMs)
                  .arg(viewportMs + regionMs));
    Perf::log(QStringLiteral("LongCapture.annotation.leave id=%1 tool=%2 annotations=%3 currentY=%4")
                  .arg(overlayId_)
                  .arg(annotationTool)
                  .arg(longCaptureAnnotations_.count())
                  .arg(longCaptureCurrentY_));
    update(selection_.adjusted(-12, -12, 12, 12)
           | longCapturePreviewPanelRect().adjusted(-2, -2, 2, 2));
}

void CaptureOverlayWindow::createLongCaptureLayers()
{
    if (longCaptureViewportLayer_) {
        return;
    }
#ifdef Q_OS_WIN
    longCaptureViewportLayer_ = new LongCaptureViewportLayer(
        [this](quint32 message, quintptr rawWParam, qintptr rawLParam) {
            if (!longCaptureActive_) {
                return false;
            }
            if (gLongCaptureHookInputPaused.load(std::memory_order_acquire)) {
                return true;
            }
            if (longCaptureAnnotationModePending_) {
                return true;
            }
            if (!isLongCaptureBrowseMode()) {
                return false;
            }
            const WPARAM wParam = static_cast<WPARAM>(rawWParam);
            const LPARAM lParam = static_cast<LPARAM>(rawLParam);
            const int wheelDelta = static_cast<SHORT>(HIWORD(wParam));
            const QPoint globalPos(static_cast<SHORT>(LOWORD(lParam)),
                                   static_cast<SHORT>(HIWORD(lParam)));
            const quint16 keyState = message == WM_MOUSEWHEEL
                ? LOWORD(wParam)
                : currentMouseKeyState();
            if (!handleLongCaptureNativeWheel(wheelDelta, globalPos, keyState)) {
                return false;
            }
            if (!longCapturePointerWheelLogged_) {
                longCapturePointerWheelLogged_ = true;
                Perf::log(QStringLiteral("LongCapture.touchpad.viewportWheel id=%1 source=%2 delta=%3 point=%4,%5")
                              .arg(overlayId_)
                              .arg(message == WM_POINTERWHEEL ? QStringLiteral("pointer")
                                                             : QStringLiteral("legacy"))
                              .arg(wheelDelta)
                              .arg(globalPos.x())
                              .arg(globalPos.y()));
            }
            return true;
        });
#else
    longCaptureViewportLayer_ = new LongCaptureViewportLayer({});
#endif
    longCaptureViewportExclusionChecked_ = false;
    longCaptureViewportExclusionAvailable_ = false;
    longCaptureViewportExclusionAttempts_ = 0;
    Perf::log(QStringLiteral("LongCapture.viewportLayer.create id=%1").arg(overlayId_));
}

void CaptureOverlayWindow::destroyLongCaptureLayers()
{
    longCaptureViewportExclusionChecked_ = false;
    longCaptureViewportExclusionAvailable_ = false;
    longCaptureViewportExclusionAttempts_ = 0;
    if (!longCaptureViewportLayer_) {
        return;
    }
    LongCaptureViewportLayer* layer = longCaptureViewportLayer_.data();
    longCaptureViewportLayer_ = nullptr;
    layer->hide();
    delete layer;
}

void CaptureOverlayWindow::syncLongCaptureLayers()
{
    if (!longCaptureViewportLayer_) {
        return;
    }
    if (!longCaptureActive_ || !selection_.isValid()) {
        longCaptureViewportLayer_->hide();
        return;
    }
    const QRect globalGeometry = selection_.translated(virtualGeometry_.topLeft());
    longCaptureViewportLayer_->setGeometry(globalGeometry);
#ifdef Q_OS_WIN
    const HWND layerHwnd = reinterpret_cast<HWND>(longCaptureViewportLayer_->winId());
    const HWND overlayHwnd = reinterpret_cast<HWND>(winId());
    if (screensUseUnitScale(globalGeometry)) {
        SetWindowPos(layerHwnd,
                     overlayHwnd,
                     globalGeometry.x(),
                     globalGeometry.y(),
                     globalGeometry.width(),
                     globalGeometry.height(),
                     SWP_NOACTIVATE | SWP_NOOWNERZORDER | SWP_NOSENDCHANGING);
    } else {
        SetWindowPos(layerHwnd,
                     overlayHwnd,
                     0,
                     0,
                     0,
                     0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE
                         | SWP_NOOWNERZORDER | SWP_NOSENDCHANGING);
    }
#endif
}

bool CaptureOverlayWindow::refreshLongCaptureViewportLayer(
    LongCaptureViewportRefreshPolicy policy)
{
    QElapsedTimer commitTimer;
    if (policy == LongCaptureViewportRefreshPolicy::CommitBeforeReveal) {
        commitTimer.start();
    }
    if (!longCaptureActive_ || longCaptureVisibleFrame_.isNull() || !selection_.isValid()) {
        if (longCaptureViewportLayer_) {
            longCaptureViewportLayer_->hide();
        }
        return false;
    }
    // Keep the prepared viewport behind the fully opaque annotation surface.
    // Exiting annotation mode can then reveal it without a hide/show frame.
    if (longCaptureAnnotationMode_) {
        return longCaptureViewportLayer_ && longCaptureViewportLayer_->isVisible();
    }
    createLongCaptureLayers();
    if (longCaptureViewportExclusionChecked_ && !longCaptureViewportExclusionAvailable_
        && longCaptureAnnotations_.count() == 0) {
        longCaptureViewportLayer_->hide();
        return false;
    }
    QElapsedTimer prepareTimer;
    prepareTimer.start();
    QImage frame = longCaptureVisibleFrame_;
    QVector<AnnotationItem> vectorAnnotations;
    bool hasPixelEffects = false;
    const QRectF viewportDocumentRect(0,
                                      longCaptureCurrentY_,
                                      longCaptureVisibleFrame_.width(),
                                      longCaptureVisibleFrame_.height());
    for (const AnnotationItem& item : longCaptureAnnotations_.items()) {
        if (item.type != AnnotationType::Mosaic && item.type != AnnotationType::Eraser) {
            continue;
        }
        if (item.boundingRect().intersects(viewportDocumentRect)) {
            hasPixelEffects = true;
            break;
        }
    }
    if (hasPixelEffects) {
        // Composing mosaic/eraser effects over the frame costs two full-frame
        // copies plus the effect render; reuse the result while the visible
        // frame and anchor are unchanged (annotation edits invalidate the key).
        if (!longCaptureViewportComposedFrame_.isNull()
            && longCaptureViewportComposedFrameKey_ == longCaptureVisibleFrame_.cacheKey()
            && longCaptureViewportComposedY_ == longCaptureCurrentY_) {
            frame = longCaptureViewportComposedFrame_;
        } else {
            const QImage base = longCaptureVisibleFrame_.convertToFormat(QImage::Format_ARGB32);
            frame = base.copy();
            renderAnnotationDocumentToImage(frame,
                                            base,
                                            longCaptureAnnotations_,
                                            false,
                                            QPointF(0, longCaptureCurrentY_));
            longCaptureViewportComposedFrame_ = frame;
            longCaptureViewportComposedFrameKey_ = longCaptureVisibleFrame_.cacheKey();
            longCaptureViewportComposedY_ = longCaptureCurrentY_;
        }
    } else {
        vectorAnnotations = longCaptureAnnotations_.items();
    }
    const qint64 composeMs = prepareTimer.elapsed();
    const bool needsReveal = longCaptureViewportLayer_->prepare(
        frame,
        vectorAnnotations,
        QPointF(0, longCaptureCurrentY_),
        selection_.translated(virtualGeometry_.topLeft()),
        captureSettings_.borderColor,
        captureSettings_.borderWidth);
    const qint64 presentMs = prepareTimer.elapsed() - composeMs;
    syncLongCaptureLayers();
    if (prepareTimer.elapsed() >= 8) {
        Perf::log(QStringLiteral("LongCapture.viewport.prepare id=%1 total=%2ms compose=%3ms present=%4ms sync=%5ms annotations=%6 pixel=%7")
                      .arg(overlayId_)
                      .arg(prepareTimer.elapsed())
                      .arg(composeMs)
                      .arg(presentMs)
                      .arg(prepareTimer.elapsed() - composeMs - presentMs)
                      .arg(longCaptureAnnotations_.count())
                      .arg(hasPixelEffects));
    }
    // Capture exclusion changes the native window style and can recreate its
    // DWM surface. Complete that mutation before the final repaint/flush so a
    // caller never reveals a hole backed by the pre-exclusion surface.
    bool exclusionAttempted = false;
    if (!longCaptureViewportExclusionChecked_
        && (longCaptureViewportLayer_->isVisible() || needsReveal)) {
        exclusionAttempted = true;
        ++longCaptureViewportExclusionAttempts_;
        longCaptureViewportExclusionAvailable_ = longCaptureViewportLayer_->enableCaptureExclusion();
#ifdef Q_OS_WIN
        const int exclusionError = longCaptureViewportExclusionAvailable_
            ? ERROR_SUCCESS
            : static_cast<int>(GetLastError());
        const bool terminalExclusionError = exclusionError == ERROR_NOT_ENOUGH_MEMORY
            || exclusionError == ERROR_INVALID_PARAMETER
            || exclusionError == ERROR_ACCESS_DENIED
            || exclusionError == ERROR_NOT_SUPPORTED;
#else
        const int exclusionError = 0;
        const bool terminalExclusionError = true;
#endif
        const bool commitRequiresStableExclusion =
            policy == LongCaptureViewportRefreshPolicy::CommitBeforeReveal;
        longCaptureViewportExclusionChecked_ = longCaptureViewportExclusionAvailable_
            || terminalExclusionError || longCaptureViewportExclusionAttempts_ >= 3
            || commitRequiresStableExclusion;
        Perf::log(QStringLiteral("LongCapture.viewportLayer.exclusion id=%1 attempt=%2 ok=%3 error=%4")
                      .arg(overlayId_)
                      .arg(longCaptureViewportExclusionAttempts_)
                      .arg(longCaptureViewportExclusionAvailable_)
                      .arg(exclusionError));
        if (!longCaptureViewportExclusionChecked_) {
            QTimer::singleShot(30, this, [this]() {
                if (isLongCaptureBrowseMode()) {
                    refreshLongCaptureViewportLayer();
                }
            });
        } else if (!longCaptureViewportExclusionAvailable_
                   && longCaptureAnnotations_.count() == 0) {
            longCaptureViewportLayer_->hide();
        }
    }
    const bool directPageOnly = longCaptureViewportExclusionChecked_
        && !longCaptureViewportExclusionAvailable_
        && longCaptureAnnotations_.count() == 0;
    bool becameVisible = false;
    if (needsReveal && !directPageOnly) {
        longCaptureViewportLayer_->reveal();
        syncLongCaptureLayers();
        becameVisible = longCaptureViewportLayer_->isVisible();
    }
    bool commitSucceeded = true;
    if (policy == LongCaptureViewportRefreshPolicy::CommitBeforeReveal) {
        const qint64 prepareMs = commitTimer.elapsed();
        longCaptureViewportLayer_->repaint();
        const qint64 paintMs = commitTimer.elapsed() - prepareMs;
#ifdef Q_OS_WIN
        const HRESULT flushResult = DwmFlush();
        commitSucceeded = SUCCEEDED(flushResult);
#else
        QGuiApplication::sync();
        const qint64 flushResult = 0;
#endif
        const qint64 flushMs = commitTimer.elapsed() - prepareMs - paintMs;
        Perf::log(QStringLiteral("LongCapture.viewport.commit id=%1 visible=%2 prepare=%3ms paint=%4ms flush=%5ms total=%6ms result=%7")
                      .arg(overlayId_)
                      .arg(longCaptureViewportLayer_->isVisible())
                      .arg(prepareMs)
                      .arg(paintMs)
                      .arg(flushMs)
                      .arg(commitTimer.elapsed())
                      .arg(static_cast<qlonglong>(flushResult)));
    } else if (becameVisible || exclusionAttempted) {
        longCaptureViewportLayer_->repaint();
        if (!longCaptureCaptureBusy_) {
            flushWindowSystemChanges();
        }
    } else {
        longCaptureViewportLayer_->update();
    }
    return longCaptureViewportLayer_ && longCaptureViewportLayer_->isVisible()
        && commitSucceeded;
}

void CaptureOverlayWindow::applyLongCaptureInputRegion()
{
#ifdef Q_OS_WIN
    if (!longCaptureActive_ || longCaptureAnnotationMode_ || longCaptureAnnotationModePending_
        || !selection_.isValid()
        || width() <= 0 || height() <= 0) {
        clearLongCaptureInputRegion();
        return;
    }
    if (!isLongCaptureWheelHookActive(this)) {
        clearLongCaptureInputRegion();
        return;
    }

    const QRect logicalSelection = selection_.normalized().intersected(rect());
    if (logicalSelection.isEmpty()) {
        clearLongCaptureInputRegion();
        return;
    }

    const HWND hwnd = reinterpret_cast<HWND>(winId());
    RECT windowRect{};
    RECT clientRect{};
    POINT clientOrigin{};
    if (!GetWindowRect(hwnd, &windowRect) || !GetClientRect(hwnd, &clientRect)
        || !ClientToScreen(hwnd, &clientOrigin)) {
        Perf::log(QStringLiteral("LongCapture.inputRegion.failed id=%1 stage=geometry error=%2")
                      .arg(overlayId_)
                      .arg(static_cast<int>(GetLastError())));
        return;
    }

    const int windowWidth = windowRect.right - windowRect.left;
    const int windowHeight = windowRect.bottom - windowRect.top;
    const int clientWidth = clientRect.right - clientRect.left;
    const int clientHeight = clientRect.bottom - clientRect.top;
    if (windowWidth <= 0 || windowHeight <= 0 || clientWidth <= 0 || clientHeight <= 0) {
        return;
    }

    const auto scaledFloor = [](int value, int nativeExtent, int logicalExtent) {
        return static_cast<int>(static_cast<qint64>(value) * nativeExtent / logicalExtent);
    };
    const auto scaledCeil = [](int value, int nativeExtent, int logicalExtent) {
        return static_cast<int>((static_cast<qint64>(value) * nativeExtent + logicalExtent - 1) / logicalExtent);
    };
    const int clientOffsetX = clientOrigin.x - windowRect.left;
    const int clientOffsetY = clientOrigin.y - windowRect.top;
    const auto toNativeRect = [&](const QRect& logicalRect) {
        const QRect clipped = logicalRect.normalized().intersected(rect());
        if (clipped.isEmpty()) {
            return QRect();
        }
        const int left = qBound(0,
                                clientOffsetX + scaledFloor(clipped.left(), clientWidth, width()),
                                windowWidth);
        const int top = qBound(0,
                               clientOffsetY + scaledFloor(clipped.top(), clientHeight, height()),
                               windowHeight);
        const int right = qBound(0,
                                 clientOffsetX + scaledCeil(clipped.right() + 1, clientWidth, width()),
                                 windowWidth);
        const int bottom = qBound(0,
                                  clientOffsetY + scaledCeil(clipped.bottom() + 1, clientHeight, height()),
                                  windowHeight);
        return QRect(left, top, right - left, bottom - top);
    };
    const QRect nativeHole = toNativeRect(logicalSelection);
    const QRect nativeToolbar = toolbar_ && toolbar_->isVisible()
        ? toNativeRect(toolbar_->geometry())
        : QRect();
    const QRect nativePreview = toNativeRect(longCapturePreviewPanelRect().adjusted(-2, -2, 2, 2));
    const int handleHit = qMax(7, captureSettings_.borderWidth + 5);
    const QRect nativeTopHandle = hasLongCaptureResizeFrontier(SelectionDragMode::ResizeTop)
        ? toNativeRect(QRect(selection_.left(),
                             selection_.top() - handleHit,
                             selection_.width(),
                             handleHit * 2 + 1))
        : QRect();
    const QRect nativeBottomHandle = hasLongCaptureResizeFrontier(SelectionDragMode::ResizeBottom)
        ? toNativeRect(QRect(selection_.left(),
                             selection_.bottom() - handleHit,
                             selection_.width(),
                             handleHit * 2 + 1))
        : QRect();
    const QSize nativeWindowSize(windowWidth, windowHeight);
    if (!nativeHole.isValid()) {
        return;
    }
    if (longCaptureInputRegionApplied_ && longCaptureAppliedRegionRect_ == nativeHole
        && longCaptureAppliedToolbarRegionRect_ == nativeToolbar
        && longCaptureAppliedPreviewRegionRect_ == nativePreview
        && longCaptureAppliedTopHandleRegionRect_ == nativeTopHandle
        && longCaptureAppliedBottomHandleRegionRect_ == nativeBottomHandle
        && longCaptureAppliedRegionWindowSize_ == nativeWindowSize) {
        return;
    }

    HRGN fullRegion = CreateRectRgn(0, 0, windowWidth, windowHeight);
    HRGN holeRegion = CreateRectRgn(nativeHole.left(),
                                    nativeHole.top(),
                                    nativeHole.right() + 1,
                                    nativeHole.bottom() + 1);
    if (!fullRegion || !holeRegion) {
        if (fullRegion) {
            DeleteObject(fullRegion);
        }
        if (holeRegion) {
            DeleteObject(holeRegion);
        }
        Perf::log(QStringLiteral("LongCapture.inputRegion.failed id=%1 stage=create error=%2")
                      .arg(overlayId_)
                      .arg(static_cast<int>(GetLastError())));
        return;
    }

    const int combineResult = CombineRgn(fullRegion, fullRegion, holeRegion, RGN_DIFF);
    DeleteObject(holeRegion);
    if (combineResult == ERROR) {
        DeleteObject(fullRegion);
        Perf::log(QStringLiteral("LongCapture.inputRegion.failed id=%1 stage=combine error=%2")
                      .arg(overlayId_)
                      .arg(static_cast<int>(GetLastError())));
        return;
    }

    const auto keepOverlayRect = [&](const QRect& nativeRect) {
        if (!nativeRect.isValid()) {
            return true;
        }
        HRGN keepRegion = CreateRectRgn(nativeRect.left(),
                                        nativeRect.top(),
                                        nativeRect.right() + 1,
                                        nativeRect.bottom() + 1);
        if (!keepRegion) {
            return false;
        }
        const int result = CombineRgn(fullRegion, fullRegion, keepRegion, RGN_OR);
        DeleteObject(keepRegion);
        return result != ERROR;
    };
    if (!keepOverlayRect(nativeToolbar) || !keepOverlayRect(nativePreview)
        || !keepOverlayRect(nativeTopHandle) || !keepOverlayRect(nativeBottomHandle)) {
        DeleteObject(fullRegion);
        Perf::log(QStringLiteral("LongCapture.inputRegion.failed id=%1 stage=keep_ui error=%2")
                      .arg(overlayId_)
                      .arg(static_cast<int>(GetLastError())));
        return;
    }

    const bool inputWasPaused = gLongCaptureHookInputPaused.exchange(
        true,
        std::memory_order_acq_rel);
    const bool previousRegionApplied = longCaptureInputRegionApplied_;
    const QRect previousHole = longCaptureAppliedRegionRect_;
    const QRect previousToolbar = longCaptureAppliedToolbarRegionRect_;
    const QRect previousPreview = longCaptureAppliedPreviewRegionRect_;
    const QPoint previousWindowTopLeft = longCaptureAppliedWindowGlobalTopLeft_;
    const QPoint windowTopLeft(windowRect.left, windowRect.top);

    // Arm the paused hook before opening the native window hole. This keeps a
    // continuing touchpad gesture from reaching the page between the two APIs.
    setLongCaptureHookRegion(nativeHole.translated(windowTopLeft),
                             nativeToolbar.translated(windowTopLeft),
                             nativePreview.translated(windowTopLeft));
    const int result = SetWindowRgn(hwnd, fullRegion, TRUE);
    const DWORD setRegionError = result != 0 ? ERROR_SUCCESS : GetLastError();
    if (result == 0) {
        DeleteObject(fullRegion);
        if (previousRegionApplied) {
            setLongCaptureHookRegion(previousHole.translated(previousWindowTopLeft),
                                     previousToolbar.translated(previousWindowTopLeft),
                                     previousPreview.translated(previousWindowTopLeft));
        } else if (!inputWasPaused) {
            clearLongCaptureHookRegion();
        }
    } else {
        longCaptureInputRegionApplied_ = true;
        longCaptureAppliedRegionRect_ = nativeHole;
        longCaptureAppliedToolbarRegionRect_ = nativeToolbar;
        longCaptureAppliedPreviewRegionRect_ = nativePreview;
        longCaptureAppliedTopHandleRegionRect_ = nativeTopHandle;
        longCaptureAppliedBottomHandleRegionRect_ = nativeBottomHandle;
        longCaptureAppliedRegionWindowSize_ = nativeWindowSize;
        longCaptureAppliedWindowGlobalTopLeft_ = windowTopLeft;
    }
    gLongCaptureHookInputPaused.store(inputWasPaused, std::memory_order_release);
    Perf::log(QStringLiteral("LongCapture.inputRegion.apply id=%1 ok=%2 error=%3 hole=%4,%5 %6x%7")
                  .arg(overlayId_)
                  .arg(result != 0)
                  .arg(static_cast<int>(setRegionError))
                  .arg(nativeHole.x())
                  .arg(nativeHole.y())
                  .arg(nativeHole.width())
                  .arg(nativeHole.height()));
#endif
}

void CaptureOverlayWindow::clearLongCaptureInputRegion(
    LongCaptureInputRegionClearPolicy policy)
{
#ifdef Q_OS_WIN
    if (longCaptureInputRegionApplied_) {
        const HWND hwnd = reinterpret_cast<HWND>(winId());
        const int result = SetWindowRgn(hwnd, nullptr, FALSE);
        Perf::log(QStringLiteral("LongCapture.inputRegion.clear id=%1 ok=%2 error=%3")
                      .arg(overlayId_)
                      .arg(result != 0)
                      .arg(result != 0 ? 0 : static_cast<int>(GetLastError())));
        if (result == 0 && IsWindow(hwnd)) {
            return;
        }
    }
#endif
    longCaptureInputRegionApplied_ = false;
    longCaptureAppliedRegionRect_ = QRect();
    longCaptureAppliedToolbarRegionRect_ = QRect();
    longCaptureAppliedPreviewRegionRect_ = QRect();
    longCaptureAppliedTopHandleRegionRect_ = QRect();
    longCaptureAppliedBottomHandleRegionRect_ = QRect();
    longCaptureAppliedRegionWindowSize_ = QSize();
    longCaptureAppliedWindowGlobalTopLeft_ = QPoint();
#ifdef Q_OS_WIN
    clearLongCaptureHookRegion();
#endif

    if (policy != LongCaptureInputRegionClearPolicy::CommitBeforeReveal
        || !longCaptureActive_ || !selection_.isValid() || !isVisible()) {
        return;
    }

    QElapsedTimer commitTimer;
    commitTimer.start();
    const int paintMargin = qMax(12, captureSettings_.borderWidth + 6);
    const QRect commitRect = selection_.adjusted(-paintMargin,
                                                  -paintMargin,
                                                  paintMargin,
                                                  paintMargin)
                                 .intersected(rect());
    repaint(commitRect);
    const qint64 repaintMs = commitTimer.elapsed();
#ifdef Q_OS_WIN
    const HRESULT flushResult = DwmFlush();
#else
    QGuiApplication::sync();
    const qint64 flushResult = 0;
#endif
    const qint64 flushMs = commitTimer.elapsed() - repaintMs;
    Perf::log(QStringLiteral("LongCapture.inputRegion.clear.commit id=%1 repaint=%2ms flush=%3ms total=%4ms result=%5")
                  .arg(overlayId_)
                  .arg(repaintMs)
                  .arg(flushMs)
                  .arg(commitTimer.elapsed())
                  .arg(static_cast<qlonglong>(flushResult)));
}

void CaptureOverlayWindow::enterLongCaptureMode()
{
    Perf::log(QStringLiteral("LongCapture.enter id=%1 selectionValid=%2 selection=%3,%4 %5x%6 activeTool=%7 active=%8")
                  .arg(overlayId_)
                  .arg(selection_.isValid())
                  .arg(selection_.x())
                  .arg(selection_.y())
                  .arg(selection_.width())
                  .arg(selection_.height())
                  .arg(activeTool_)
                  .arg(longCaptureActive_));
    if (!selection_.isValid()) {
        longCaptureStatus_ = QStringLiteral("请先选择截图区域");
        update();
        return;
    }
    if (fastTranslateRunning_) {
        cancelFastTranslation();
    } else if (fastTranslateVisible_) {
        setFastTranslateVisible(false);
    }
    commitPendingEraserFill();
    commitInlineTextEdit();
    hasCurrentAnnotation_ = false;
    clearActiveShapeEdit();
    clearActiveArrowEdit();
    longCaptureActive_ = true;
    longCaptureAnnotationMode_ = false;
    longCaptureAnnotationModePending_ = false;
    longCaptureBrowseRevealPending_ = false;
    longCaptureViewportStable_ = false;
    longCaptureAnnotationsOverlayRelative_ = false;
    longCaptureAnnotations_.clear();
    pendingLongCaptureAnnotationTool_.clear();
    longCaptureAnnotationStableSamples_ = 0;
    gLongCaptureHookInputPaused.store(false, std::memory_order_release);
    longCapturePreviewPanelHeight_ = qBound(180,
                                            selection_.height(),
                                            qMax(180, height() - 16));
    longCaptureAnchorGlobal_ = selection_.translated(virtualGeometry_.topLeft()).topLeft();
    createLongCaptureLayers();
    installLongCaptureWheelHook();
    if (toolbar_) {
        toolbar_->setLongCaptureMode(true);
        toolbar_->setUndoAvailable(longCaptureAnnotations_.canUndo());
        toolbar_->setRedoAvailable(longCaptureAnnotations_.canRedo());
    }
    hideOptionsBar();
    syncCursorForTool();
    longCaptureStatus_ = QStringLiteral("长截图：滚轮滚动页面，复制/保存/贴图输出长图");
    // Repaint the preview panel plus the selection surround (cleared edit
    // handles may straddle the selection border); still far cheaper than a
    // full multi-screen overlay repaint.
    update(longCapturePreviewPanelRect().adjusted(-2, -2, 2, 2)
           | selection_.adjusted(-24, -24, 24, 24).intersected(rect()));
    // Seed while the capture overlay still owns the whole window. The native
    // input hole is applied only after a ready viewport layer can cover it.
    QTimer::singleShot(0, this, [this]() {
        if (!longCaptureActive_) {
            return;
        }
        if (!resetLongCaptureFrames(QStringLiteral("enter"))) {
            abortLongCaptureMode(QStringLiteral("initial_seed_failed"));
        }
    });
    Perf::log(QStringLiteral("LongCapture.enter.end id=%1 deferredInit=next_event status=\"%2\"")
                  .arg(overlayId_)
                  .arg(longCaptureStatus_));
}

void CaptureOverlayWindow::leaveLongCaptureMode()
{
    cancelLongCaptureFrameRequest();
    Perf::log(QStringLiteral("LongCapture.leave id=%1 currentY=%2 doc=[%3,%4) target=0x%5 wheel=%6/%7/%8 queued=%9 status=\"%10\"")
                  .arg(overlayId_)
                  .arg(longCaptureCurrentY_)
                  .arg(longCaptureDocTopY_)
                  .arg(longCaptureDocBottomY_)
                  .arg(longCaptureTargetHwnd_, 0, 16)
                  .arg(longCaptureWheelReceivedCount_)
                  .arg(longCaptureWheelDispatchedCount_)
                  .arg(longCaptureWheelCompletedCount_)
                  .arg(longCaptureWheelQueue_.size())
                  .arg(longCaptureStatus_));
    longCaptureProgressTimer_.stop();
    longCaptureAnnotationModeTimer_.stop();
    longCapturePreviewRefreshTimer_.stop();
    if (longCaptureAnnotationMode_) {
        commitInlineTextEdit();
        commitPendingEraserFill();
    }
    makeLongCaptureAnnotationsDocumentRelative();
    longCaptureAnnotationMode_ = false;
    longCaptureAnnotationModePending_ = false;
    longCaptureBrowseRevealPending_ = false;
    pendingLongCaptureAnnotationTool_.clear();
    longCaptureAnnotationStableSamples_ = 0;
    gLongCaptureHookInputPaused.store(false, std::memory_order_release);
    uninstallLongCaptureWheelHook();
    clearLongCaptureInputRegion();
    longCaptureExpectedDirection_ = 0;
    longCaptureMotionDirection_ = 0;
    longCaptureLastTrackedFrameMovement_ = 0;
    longCapturePixelWheelRemainder_ = 0;
    longCaptureDispatchedWheelDelta_ = 0;
    longCaptureLastWheelAtMs_ = -1;
    longCaptureLastMovementAtMs_ = -1;
    longCaptureWheelDispatchAtMs_ = -1;
    longCaptureLastPreviewRefreshAtMs_ = -1;
    longCapturePreviewDirtyAtMs_ = -1;
    longCaptureCaptureBusy_ = false;
    longCaptureWheelDispatchInFlight_ = false;
    longCaptureWheelDispatchScheduled_ = false;
    longCaptureDispatchMovementObserved_ = false;
    longCaptureLastFrameRejected_ = false;
    longCapturePointerWheelLogged_ = false;
    longCaptureViewportStable_ = false;
    longCaptureWheelReceivedCount_ = 0;
    longCaptureWheelDispatchedCount_ = 0;
    longCaptureWheelCompletedCount_ = 0;
    longCaptureWheelQueue_.clear();
    longCaptureTargetHwnd_ = 0;
    longCaptureActive_ = false;
    destroyLongCaptureLayers();
    longCaptureStatus_.clear();
    longCaptureCurrentSignature_.clear();
    longCaptureLastSampleSignature_.clear();
    longCaptureCanvas_ = QImage();
    longCaptureVisibleFrame_ = QImage();
    longCaptureViewportComposedFrame_ = QImage();
    longCaptureViewportComposedFrameKey_ = -1;
    longCaptureResizePreviewBaseFrame_ = QImage();
    longCaptureResizePreviewFrame_ = QImage();
    longCaptureResizePreviewDocumentY_ = 0;
    longCaptureDocSignature_.clear();
    longCapturePreviewFitted_ = QImage();
    invalidateLongCapturePreviewMip();
    longCaptureCanvasDocTopY_ = 0;
    longCaptureDocTopY_ = 0;
    longCaptureDocBottomY_ = 0;
    longCaptureOutputTopY_ = 0;
    longCaptureOutputBottomY_ = 0;
    longCaptureCurrentY_ = 0;
    longCapturePreviewPanelHeight_ = 0;
    longCaptureAnnotations_.clear();
    if (toolbar_) {
        toolbar_->setLongCaptureMode(false);
        toolbar_->setUndoAvailable(annotations_.canUndo());
        toolbar_->setRedoAvailable(annotations_.canRedo());
    }
    update();
}

void CaptureOverlayWindow::abortLongCaptureMode(const QString& reason)
{
    if (!longCaptureActive_) {
        return;
    }
    const QString failureMessage = longCaptureStatus_.isEmpty()
        ? QStringLiteral("长截图初始化失败，请重试")
        : longCaptureStatus_;
    Perf::log(QStringLiteral("LongCapture.abort id=%1 reason=%2 status=\"%3\"")
                  .arg(overlayId_)
                  .arg(reason)
                  .arg(failureMessage));
    leaveLongCaptureMode();
    activeTool_.clear();
    if (toolbar_) {
        toolbar_->selectTool(activeTool_);
    }
    hideOptionsBar();
    syncCursorForTool();
    update();
    QToolTip::showText(QCursor::pos(), failureMessage, this, QRect(), 2000);
}

bool CaptureOverlayWindow::resetLongCaptureFrames(const QString& reason)
{
    if (!longCaptureActive_ || !selection_.isValid() || longCaptureCaptureBusy_) {
        Perf::log(QStringLiteral("LongCapture.reset.skip id=%1 reason=%2 active=%3 selectionValid=%4 busy=%5")
                      .arg(overlayId_)
                      .arg(reason)
                      .arg(longCaptureActive_)
                      .arg(selection_.isValid())
                      .arg(longCaptureCaptureBusy_));
        return false;
    }
    cancelLongCaptureFrameRequest();
    longCaptureCaptureBusy_ = true;
    auto busyGuard = qScopeGuard([this]() { longCaptureCaptureBusy_ = false; });
    Perf::log(QStringLiteral("LongCapture.reset.begin id=%1 reason=%2").arg(overlayId_).arg(reason));
    const bool hadContent = longCaptureDocBottomY_ - longCaptureDocTopY_ > selection_.height();
    clearLongCaptureInputRegion();
    longCaptureProgressTimer_.stop();
    longCapturePreviewRefreshTimer_.stop();
    longCaptureExpectedDirection_ = 0;
    longCaptureMotionDirection_ = 0;
    longCaptureAnnotationStableSamples_ = 0;
    longCaptureAnchorMismatchSamples_ = 0;
    longCaptureAnchorMismatchY_ = 0;
    longCaptureLastTrackedFrameMovement_ = 0;
    longCapturePixelWheelRemainder_ = 0;
    longCaptureDispatchedWheelDelta_ = 0;
    longCaptureLastWheelAtMs_ = -1;
    longCaptureLastMovementAtMs_ = -1;
    longCaptureWheelDispatchAtMs_ = -1;
    longCaptureLastPreviewRefreshAtMs_ = -1;
    longCapturePreviewDirtyAtMs_ = -1;
    longCaptureWheelDispatchInFlight_ = false;
    longCaptureWheelDispatchScheduled_ = false;
    longCaptureDispatchMovementObserved_ = false;
    longCaptureLastFrameRejected_ = false;
    longCapturePointerWheelLogged_ = false;
    longCaptureViewportStable_ = false;
    longCaptureWheelReceivedCount_ = 0;
    longCaptureWheelDispatchedCount_ = 0;
    longCaptureWheelCompletedCount_ = 0;
    longCaptureWheelQueue_.clear();
    longCaptureCanvas_ = QImage();
    longCaptureVisibleFrame_ = QImage();
    longCaptureResizePreviewBaseFrame_ = QImage();
    longCaptureResizePreviewFrame_ = QImage();
    longCaptureResizePreviewDocumentY_ = 0;
    longCaptureDocSignature_.clear();
    longCaptureCurrentSignature_.clear();
    longCaptureLastSampleSignature_.clear();
    longCapturePreviewFitted_ = QImage();
    invalidateLongCapturePreviewMip();
    longCaptureCanvasDocTopY_ = 0;
    longCaptureDocTopY_ = 0;
    longCaptureDocBottomY_ = 0;
    longCaptureOutputTopY_ = 0;
    longCaptureOutputBottomY_ = 0;
    longCaptureCurrentY_ = 0;
    longCaptureAnchorGlobal_ = selection_.translated(virtualGeometry_.topLeft()).topLeft();
    resolveLongCaptureTarget(QCursor::pos());
    const QImage image = captureLongFrameImage();
    auto viewportGuard = qScopeGuard([this]() {
        if (longCaptureViewportLayer_) {
            longCaptureViewportLayer_->hide();
        }
    });
    if (image.isNull()) {
        longCaptureStatus_ = QStringLiteral("长截图初始采集失败");
        Perf::log(QStringLiteral("LongCapture.reset.capture_failed id=%1 reason=%2").arg(overlayId_).arg(reason));
        update();
        return false;
    }
    longCaptureVisibleFrame_ = image;
    longCaptureCurrentSignature_ = LongCapture::computeRowSignature(image);
    if (!appendLongCaptureContent(image, longCaptureCurrentSignature_, 0)) {
        update(selection_ | longCapturePreviewPanelRect());
        Perf::log(QStringLiteral("LongCapture.reset.append_failed id=%1 reason=%2")
                      .arg(overlayId_)
                      .arg(reason));
        return false;
    }
    longCaptureCurrentY_ = 0;
    longCaptureOutputTopY_ = longCaptureDocTopY_;
    longCaptureOutputBottomY_ = longCaptureDocBottomY_;
    longCaptureViewportStable_ = true;
    refreshLongCapturePreviewFit();
    if (reason == QStringLiteral("selection_changed") && hadContent) {
        longCaptureStatus_ = QStringLiteral("截图区域已调整，长截图数据已重置，请从当前位置继续滚动采集。");
    } else if (reason == QStringLiteral("selection_changed")) {
        longCaptureStatus_ = QStringLiteral("截图区域已更新，请继续滚动采集。");
    } else {
        longCaptureStatus_ = QStringLiteral("长截图：滚轮滚动页面，复制/保存/贴图输出长图");
    }
    Perf::log(QStringLiteral("LongCapture.reset id=%1 reason=%2 hadContent=%3 doc=[%4,%5) target=0x%6")
                  .arg(overlayId_)
                  .arg(reason)
                  .arg(hadContent)
                  .arg(longCaptureDocTopY_)
                  .arg(longCaptureDocBottomY_)
                  .arg(longCaptureTargetHwnd_, 0, 16));
    longCaptureCaptureBusy_ = false;
    busyGuard.dismiss();
    const bool viewportReady = refreshLongCaptureViewportLayer(
        LongCaptureViewportRefreshPolicy::CommitBeforeReveal);
    const bool directPageFallback = longCaptureViewportExclusionChecked_
        && !longCaptureViewportExclusionAvailable_
        && longCaptureAnnotations_.count() == 0
        && (!longCaptureViewportLayer_ || !longCaptureViewportLayer_->isVisible());
    const bool canRevealViewport = viewportReady || directPageFallback;
    if (canRevealViewport) {
        applyLongCaptureInputRegion();
    }
#ifdef Q_OS_WIN
    const bool inputReady = longCaptureInputRegionApplied_;
#else
    const bool inputReady = true;
#endif
    Perf::log(QStringLiteral("LongCapture.reset.reveal id=%1 reason=%2 viewport=%3 direct=%4 input=%5")
                  .arg(overlayId_)
                  .arg(reason)
                  .arg(viewportReady)
                  .arg(directPageFallback)
                  .arg(inputReady));
    if (!canRevealViewport || !inputReady) {
        longCaptureViewportStable_ = false;
        longCaptureStatus_ = QStringLiteral("长截图视口初始化失败，已保留原截图区域");
        update(selection_ | longCapturePreviewPanelRect().adjusted(-2, -2, 2, 2));
        return false;
    }
    viewportGuard.dismiss();
    update(selection_ | longCapturePreviewPanelRect().adjusted(-2, -2, 2, 2));
    return true;
}

// Forces a stable live frame back into the document at its best-evidence
// position. Called only after several consecutive identical samples: the page
// has settled but its rendering drifted away from the committed rows (font
// re-rasterization, hover/theme changes, sub-pixel settle after animated
// scrolling), so both the live-frame chain and the committed comparison can
// stay permanently above the acceptance thresholds. The live content is the
// truth the user sees: overwrite the committed overlap, append any frontier
// rows, and adopt the position so stitching and annotation can continue.
bool CaptureOverlayWindow::repairLongCaptureCommittedFrame(
    const QImage& image,
    const LongCapture::RowSignature& signature,
    int documentY)
{
    const int frameHeight = image.height();
    if (longCaptureCanvas_.isNull() || image.isNull() || !signature.isValid()
        || signature.height() != frameHeight
        || image.width() != longCaptureCanvas_.width()) {
        return false;
    }
    const int overlapStart = qMax(documentY, longCaptureDocTopY_);
    const int overlapEnd = qMin(documentY + frameHeight, longCaptureDocBottomY_);
    if (overlapEnd <= overlapStart) {
        return false;
    }
    if (documentY < longCaptureDocTopY_
        || documentY + frameHeight > longCaptureDocBottomY_) {
        // Grow the canvas and commit the frontier strips first; the committed
        // overlap keeps its old content until the overwrite below.
        if (!appendLongCaptureContent(image, signature, documentY)) {
            return false;
        }
    }
    const int canvasY = overlapStart - longCaptureCanvasDocTopY_;
    const int overlapRows = overlapEnd - overlapStart;
    const int signatureIndex = overlapStart - longCaptureDocTopY_;
    if (canvasY < 0 || canvasY + overlapRows > longCaptureCanvas_.height()
        || signatureIndex < 0
        || signatureIndex + overlapRows > longCaptureDocSignature_.height()) {
        return false;
    }
    QPainter painter(&longCaptureCanvas_);
    painter.setCompositionMode(QPainter::CompositionMode_Source);
    painter.drawImage(QPoint(0, canvasY),
                      image,
                      QRect(0, overlapStart - documentY, image.width(), overlapRows));
    painter.end();
    const int frameIndex = overlapStart - documentY;
    const bool copySpatial = longCaptureDocSignature_.spatialA.size() == longCaptureDocSignature_.luma.size()
        && signature.spatialA.size() == signature.luma.size();
    for (int i = 0; i < overlapRows; ++i) {
        longCaptureDocSignature_.luma[signatureIndex + i] = signature.luma[frameIndex + i];
        longCaptureDocSignature_.edge[signatureIndex + i] = signature.edge[frameIndex + i];
        if (copySpatial) {
            longCaptureDocSignature_.spatialA[signatureIndex + i] = signature.spatialA[frameIndex + i];
            longCaptureDocSignature_.spatialB[signatureIndex + i] = signature.spatialB[frameIndex + i];
        }
    }
    const int previousY = longCaptureCurrentY_;
    if (documentY != previousY) {
        longCaptureMotionDirection_ = documentY > previousY ? 1 : -1;
        longCaptureLastMovementAtMs_ = Perf::elapsedMs();
        longCaptureViewportStable_ = false;
    }
    longCaptureCurrentY_ = documentY;
    longCaptureVisibleFrame_ = image;
    longCaptureCurrentSignature_ = signature;
    longCaptureOutputTopY_ = qMin(longCaptureOutputTopY_, documentY);
    longCaptureOutputBottomY_ = qMax(longCaptureOutputBottomY_, documentY + frameHeight);
    longCaptureLastFrameRejected_ = false;
    // Repaired rows sit inside the already-covered preview range; the
    // incremental mip would keep the stale pixels without a full rebuild.
    invalidateLongCapturePreviewMip();
    longCaptureLastPreviewRefreshAtMs_ = -1;
    scheduleLongCapturePreviewRefresh();
    longCaptureStatus_ = QStringLiteral("页面内容有变化，已自动校正当前画面");
    Perf::log(QStringLiteral("LongCapture.frame.repair id=%1 y=%2->%3 rows=%4 overlap=%5 doc=[%6,%7)")
                  .arg(overlayId_)
                  .arg(previousY)
                  .arg(documentY)
                  .arg(frameHeight)
                  .arg(overlapRows)
                  .arg(longCaptureDocTopY_)
                  .arg(longCaptureDocBottomY_));
    update(selection_ | longCapturePreviewPanelRect().adjusted(-2, -2, 2, 2));
    return true;
}

bool CaptureOverlayWindow::canCaptureLongFrameAsync() const
{
#ifdef Q_OS_WIN
    if (!longCaptureActive_ || !selection_.isValid() || !longCaptureExclusionAvailable_) {
        return false;
    }
    const QRect globalRect = selection_.translated(virtualGeometry_.topLeft());
    if (!screensUseUnitScale(globalRect) && !allScreensUniformScale(nullptr)) {
        return false;
    }
    return !longCaptureViewportLayer_ || !longCaptureViewportLayer_->isVisible()
        || longCaptureViewportExclusionAvailable_;
#else
    return false;
#endif
}

bool CaptureOverlayWindow::requestLongCaptureFrameAsync(LongCaptureFrameResume resume)
{
#ifdef Q_OS_WIN
    if (resume == LongCaptureFrameResume::Synchronous || longCaptureCaptureBusy_
        || !canCaptureLongFrameAsync()) {
        return false;
    }
    if (!longCaptureFrameGrabber_) {
        longCaptureFrameGrabber_ = std::make_unique<LongCaptureFrameGrabber>(
            this,
            [this](LongCaptureFrameGrabResult result) {
                handleLongCaptureFrameGrabbed(std::move(result));
            });
        longCaptureFrameGrabber_->start();
    }

    const QRect globalRect = selection_.translated(virtualGeometry_.topLeft());
    QRect grabRect = globalRect;
    QSize logicalSize;
    if (!screensUseUnitScale(globalRect)) {
        qreal scale = 1.0;
        if (!allScreensUniformScale(&scale)) {
            return false;
        }
        grabRect = nativeRectForClientRect(reinterpret_cast<HWND>(winId()),
                                           selection_,
                                           scale);
        if (!grabRect.isValid()) {
            return false;
        }
        logicalSize = globalRect.size();
    }
    const quint64 requestId = ++longCaptureFrameRequestSerial_;
    longCaptureActiveFrameRequestId_ = requestId;
    longCaptureActiveFrameResume_ = resume;
    longCaptureActiveFrameGeometry_ = globalRect;
    longCaptureCaptureBusy_ = true;
    if (!longCaptureFrameGrabber_->request(requestId, globalRect, grabRect, logicalSize)) {
        longCaptureCaptureBusy_ = false;
        longCaptureActiveFrameRequestId_ = 0;
        longCaptureActiveFrameGeometry_ = QRect();
        return false;
    }
    return true;
#else
    Q_UNUSED(resume)
    return false;
#endif
}

void CaptureOverlayWindow::handleLongCaptureFrameGrabbed(LongCaptureFrameGrabResult result)
{
#ifdef Q_OS_WIN
    if (result.requestId != longCaptureActiveFrameRequestId_) {
        Perf::log(QStringLiteral("LongCapture.grab.stale id=%1 request=%2 active=%3")
                      .arg(overlayId_)
                      .arg(result.requestId)
                      .arg(longCaptureActiveFrameRequestId_));
        return;
    }

    const LongCaptureFrameResume resume = longCaptureActiveFrameResume_;
    const bool geometryMatches = result.geometry == longCaptureActiveFrameGeometry_
        && result.geometry == selection_.translated(virtualGeometry_.topLeft());
    longCaptureActiveFrameRequestId_ = 0;
    longCaptureActiveFrameGeometry_ = QRect();
    longCaptureCaptureBusy_ = false;
    if (!longCaptureActive_ || !geometryMatches || mode_ == Mode::ResizingSelection) {
        Perf::log(QStringLiteral("LongCapture.grab.stale id=%1 request=%2 active=%3 geometry=%4")
                      .arg(overlayId_)
                      .arg(result.requestId)
                      .arg(longCaptureActive_)
                      .arg(geometryMatches));
        if (resume == LongCaptureFrameResume::AnnotationConfirm
            && longCaptureActive_ && longCaptureAnnotationModePending_) {
            longCaptureAnnotationStableSamples_ = 0;
            scheduleLongCaptureAnnotationConfirmation(kLongCaptureAnnotationConfirmIntervalMs);
        }
        return;
    }

    if (result.totalMs > 32 || result.error != ERROR_SUCCESS || result.resized) {
        Perf::log(QStringLiteral("LongCapture.grab.worker id=%1 request=%2 total=%3ms setup=%4ms bitblt=%5ms copy=%6ms signature=%7ms resized=%8 error=%9")
                      .arg(overlayId_)
                      .arg(result.requestId)
                      .arg(result.totalMs)
                      .arg(result.setupMs)
                      .arg(result.bitBltMs)
                      .arg(result.copyMs)
                      .arg(result.signatureMs)
                      .arg(result.resized)
                      .arg(result.error));
    }

    longCaptureReadyFrame_ = std::move(result.image);
    longCaptureReadyFrameSignature_ = std::move(result.signature);
    longCaptureReadyFrameCaptureMs_ = result.totalMs;
    longCaptureReadyFrameResume_ = resume;
    longCaptureReadyFrameAvailable_ = true;
    switch (resume) {
    case LongCaptureFrameResume::Progress:
        captureLongFrameDuringScroll();
        break;
    case LongCaptureFrameResume::AnnotationConfirm:
        longCaptureAnnotationModeTimer_.stop();
        tryEnterLongCaptureAnnotationMode();
        break;
    case LongCaptureFrameResume::Synchronous:
        break;
    }
#else
    Q_UNUSED(result)
#endif
}

void CaptureOverlayWindow::cancelLongCaptureFrameRequest()
{
    ++longCaptureFrameRequestSerial_;
    longCaptureActiveFrameRequestId_ = 0;
    longCaptureActiveFrameGeometry_ = QRect();
    longCaptureActiveFrameResume_ = LongCaptureFrameResume::Synchronous;
    longCaptureReadyFrameResume_ = LongCaptureFrameResume::Synchronous;
    longCaptureReadyFrame_ = QImage();
    longCaptureReadyFrameSignature_.clear();
    longCaptureReadyFrameCaptureMs_ = 0;
    longCaptureReadyFrameAvailable_ = false;
    longCaptureCaptureBusy_ = false;
}

void CaptureOverlayWindow::captureLongFrameDuringScroll()
{
    if (!longCaptureActive_ || mode_ == Mode::ResizingSelection) {
        longCaptureProgressTimer_.stop();
        return;
    }
    const qint64 now = Perf::elapsedMs();
    const bool wheelQuiet = longCaptureLastWheelAtMs_ < 0
        || now - longCaptureLastWheelAtMs_ > kLongCaptureQuietStopMs;
    const bool movementQuiet = longCaptureLastMovementAtMs_ < 0
        || now - longCaptureLastMovementAtMs_ > kLongCaptureQuietStopMs;
    if (wheelQuiet && movementQuiet && !longCaptureWheelDispatchInFlight_
        && longCaptureWheelQueue_.isEmpty() && !longCaptureCaptureBusy_
        && !(longCaptureReadyFrameAvailable_
             && longCaptureReadyFrameResume_ == LongCaptureFrameResume::Progress)) {
        refreshLongCapturePreviewFit();
        longCaptureProgressTimer_.stop();
        Perf::log(QStringLiteral("LongCapture.burst.stop id=%1 reason=quiet wheel=%2/%3/%4")
                      .arg(overlayId_)
                      .arg(longCaptureWheelReceivedCount_)
                      .arg(longCaptureWheelDispatchedCount_)
                      .arg(longCaptureWheelCompletedCount_));
        return;
    }
    const int previousY = longCaptureCurrentY_;
    const LongCaptureFrameResult result = processLongCaptureFrame(
        LongCaptureFrameResume::Progress);
    if (result == LongCaptureFrameResult::Deferred) {
        return;
    }
    const qint64 dispatchAgeMs = longCaptureWheelDispatchInFlight_
            && longCaptureWheelDispatchAtMs_ >= 0
        ? qMax<qint64>(0, Perf::elapsedMs() - longCaptureWheelDispatchAtMs_)
        : -1;
    const int frameMovement = qAbs(longCaptureCurrentY_ - previousY);
    if (result == LongCaptureFrameResult::Accepted) {
        longCaptureLastTrackedFrameMovement_ = frameMovement;
    } else if (result == LongCaptureFrameResult::NoChange) {
        longCaptureLastTrackedFrameMovement_ = 0;
    }
    const int safeDispatchStep = longCaptureMinRequiredOverlap(selection_.height());
    const bool dispatchBackpressureSatisfied = dispatchAgeMs >= kLongCaptureMinDispatchIntervalMs
        && frameMovement <= safeDispatchStep;
    const bool dispatchSettleDeadlineReached = dispatchAgeMs >= kLongCaptureSettleDelayMs;
    if (result == LongCaptureFrameResult::Accepted
        && longCaptureCurrentY_ == previousY
        && !longCaptureWheelDispatchInFlight_ && longCaptureWheelQueue_.isEmpty()
        && (longCaptureLastWheelAtMs_ < 0
            || now - longCaptureLastWheelAtMs_ >= kLongCaptureSettleDelayMs)) {
        longCaptureViewportStable_ = true;
        applyLongCaptureInputRegion();
        update(selection_.adjusted(-12, -12, 12, 12));
    }
    if (result == LongCaptureFrameResult::NoChange
        && !longCaptureWheelDispatchInFlight_ && longCaptureWheelQueue_.isEmpty()
        && (longCaptureLastMovementAtMs_ < 0
            || now - longCaptureLastMovementAtMs_ >= kLongCaptureSettleDelayMs)) {
        longCaptureViewportStable_ = true;
        applyLongCaptureInputRegion();
        update(selection_.adjusted(-12, -12, 12, 12));
    }
    if (result == LongCaptureFrameResult::Failed && longCaptureWheelDispatchInFlight_) {
        abortLongCaptureWheelDispatch(QStringLiteral("frame_failed"));
    } else if (longCaptureWheelDispatchInFlight_ && result == LongCaptureFrameResult::Accepted) {
        longCaptureDispatchMovementObserved_ = longCaptureDispatchMovementObserved_
            || frameMovement > 0;
        if (longCaptureDispatchMovementObserved_ && dispatchBackpressureSatisfied) {
            completeLongCaptureWheelDispatch(true);
        }
    } else if (longCaptureWheelDispatchInFlight_
               && result == LongCaptureFrameResult::NoChange
               && !longCaptureLastFrameRejected_) {
        if (longCaptureDispatchMovementObserved_
            && dispatchAgeMs >= kLongCaptureMinDispatchIntervalMs) {
            completeLongCaptureWheelDispatch(true);
        } else if (dispatchSettleDeadlineReached) {
            // The progress stream owns the absolute settle deadline, so a busy
            // worker can never restart another full settle period.
            longCaptureViewportStable_ = true;
            completeLongCaptureWheelDispatch(false);
        }
    } else if (longCaptureWheelDispatchInFlight_
               && result == LongCaptureFrameResult::Rejected
               && dispatchSettleDeadlineReached) {
        longCaptureViewportStable_ = false;
        longCaptureWheelDispatchInFlight_ = false;
        longCaptureDispatchMovementObserved_ = false;
        longCaptureWheelDispatchAtMs_ = -1;
        longCaptureDispatchedWheelDelta_ = 0;
        longCaptureStatus_ = QStringLiteral("当前页面滚动跨度无法连续匹配，已暂停同方向滚动");
        // Keep sampling at a low cadence so the pause self-recovers once the
        // page settles back onto committed content, instead of staying stuck
        // until the user reverses direction.
        longCaptureProgressTimer_.start(kLongCapturePauseRetryIntervalMs);
        Perf::log(QStringLiteral("LongCapture.wheel.pause id=%1 reason=unmatched age=%2ms queued=%3")
                      .arg(overlayId_)
                      .arg(dispatchAgeMs)
                      .arg(longCaptureWheelQueue_.size()));
    } else if (!longCaptureWheelDispatchInFlight_ && !longCaptureWheelQueue_.isEmpty()
               && !longCaptureLastFrameRejected_) {
        dispatchNextLongCaptureWheel();
    }
}

CaptureOverlayWindow::LongCaptureFrameResult CaptureOverlayWindow::processLongCaptureFrame(
    LongCaptureFrameResume resume)
{
    if (!longCaptureActive_ || mode_ == Mode::ResizingSelection
        || !selection_.isValid()) {
        return LongCaptureFrameResult::Failed;
    }
    const bool readyFrame = longCaptureReadyFrameAvailable_
        && longCaptureReadyFrameResume_ == resume;
    if (longCaptureReadyFrameAvailable_ && !readyFrame) {
        return LongCaptureFrameResult::Deferred;
    }
    if (!readyFrame && longCaptureCaptureBusy_) {
        return LongCaptureFrameResult::Deferred;
    }
    if (longCaptureCanvas_.isNull() || longCaptureDocBottomY_ <= longCaptureDocTopY_) {
        if (resetLongCaptureFrames(QStringLiteral("reseed"))) {
            return LongCaptureFrameResult::NoChange;
        }
        abortLongCaptureMode(QStringLiteral("reseed_failed"));
        return LongCaptureFrameResult::Failed;
    }
    if (!readyFrame && resume != LongCaptureFrameResume::Synchronous
        && canCaptureLongFrameAsync() && requestLongCaptureFrameAsync(resume)) {
        return LongCaptureFrameResult::Deferred;
    }

    longCaptureCaptureBusy_ = true;
    const auto busyGuard = qScopeGuard([this]() { longCaptureCaptureBusy_ = false; });
    QElapsedTimer timer;
    QImage image;
    LongCapture::RowSignature signature;
    qint64 captureElapsedMs = 0;
    if (readyFrame) {
        image = std::move(longCaptureReadyFrame_);
        signature = std::move(longCaptureReadyFrameSignature_);
        captureElapsedMs = longCaptureReadyFrameCaptureMs_;
        longCaptureReadyFrameAvailable_ = false;
        longCaptureReadyFrameResume_ = LongCaptureFrameResume::Synchronous;
        longCaptureReadyFrameCaptureMs_ = 0;
    } else {
        QElapsedTimer captureTimer;
        captureTimer.start();
        image = captureLongFrameImage();
        captureElapsedMs = captureTimer.elapsed();
    }
    timer.start();
    const auto pipelineGuard = qScopeGuard([this, captureElapsedMs, &timer]() {
        const qint64 totalMs = captureElapsedMs + timer.elapsed();
        if (totalMs > 32) {
            Perf::log(QStringLiteral("LongCapture.frame.pipeline id=%1 total=%2ms capture=%3ms gui=%4ms")
                          .arg(overlayId_)
                          .arg(totalMs)
                          .arg(captureElapsedMs)
                          .arg(timer.elapsed()));
        }
    });
    auto viewportGuard = qScopeGuard([this]() { refreshLongCaptureViewportLayer(); });
    if (image.isNull()) {
        longCaptureStatus_ = QStringLiteral("滚动后采集失败");
        Perf::log(QStringLiteral("LongCapture.frame.capture_failed id=%1").arg(overlayId_));
        update(longCapturePreviewPanelRect().adjusted(-2, -2, 2, 2));
        return LongCaptureFrameResult::Failed;
    }
    const int frameHeight = image.height();
    if (signature.isEmpty()) {
        signature = LongCapture::computeRowSignature(image);
    }
    // Sample-to-sample stability: unlike the chain anchor (which only
    // refreshes on acceptance), this tracks whether the on-screen content
    // itself has settled, so recovery can trust a frame even when every
    // acceptance comparison is poisoned by drift.
    const double lastSampleScore = longCaptureLastSampleSignature_.isEmpty()
        ? 255.0
        : LongCapture::signatureDifference(longCaptureLastSampleSignature_,
                                           0,
                                           signature,
                                           0,
                                           frameHeight);
    longCaptureLastSampleSignature_ = signature;
    const auto frameElapsed = [&]() { return captureElapsedMs + timer.elapsed(); };
    const auto committedOverlapScore = [this, &signature, frameHeight](int documentY,
                                                                      int* overlapRows) {
        const int overlapStart = qMax(documentY, longCaptureDocTopY_);
        const int overlapEnd = qMin(documentY + frameHeight, longCaptureDocBottomY_);
        const int overlap = qMax(0, overlapEnd - overlapStart);
        if (overlapRows) {
            *overlapRows = overlap;
        }
        if (overlap <= 0) {
            return 255.0;
        }
        return LongCapture::signatureDifference(longCaptureDocSignature_,
                                                overlapStart - longCaptureDocTopY_,
                                                signature,
                                                overlapStart - documentY,
                                                overlap);
    };
    const double sameFrameScore = longCaptureCurrentSignature_.isEmpty()
        ? 255.0
        : LongCapture::signatureDifference(longCaptureCurrentSignature_,
                                           0,
                                           signature,
                                           0,
                                           frameHeight);
    int currentCommittedOverlap = 0;
    const double currentCommittedScore = committedOverlapScore(longCaptureCurrentY_,
                                                               &currentCommittedOverlap);
    if (sameFrameScore <= kLongCaptureSameFrameScore
        && currentCommittedOverlap == frameHeight
        && currentCommittedScore <= kLongCaptureNoisyMaxAlignmentScore) {
        if (longCaptureLastFrameRejected_) {
            Perf::log(QStringLiteral("LongCapture.frame.recover id=%1 reason=returned_to_committed_frame")
                          .arg(overlayId_));
        }
        longCaptureLastFrameRejected_ = false;
        longCaptureVisibleFrame_ = image;
        update(selection_);
        return LongCaptureFrameResult::NoChange;
    }
    if (sameFrameScore <= kLongCaptureSameFrameScore
        && currentCommittedScore > kLongCaptureMaxAlignmentScore) {
        Perf::log(QStringLiteral("LongCapture.frame.anchor_mismatch id=%1 y=%2 live=%3 doc=%4 overlap=%5")
                      .arg(overlayId_)
                      .arg(longCaptureCurrentY_)
                      .arg(sameFrameScore, 0, 'f', 2)
                      .arg(currentCommittedScore, 0, 'f', 2)
                      .arg(currentCommittedOverlap));
    }

    const int minOverlap = longCaptureMinRequiredOverlap(frameHeight);
    const int directionTolerance = qMax(12, frameHeight / 32);
    const int matchDirection = longCaptureExpectedDirection_ != 0
            && longCaptureMotionDirection_ != 0
            && longCaptureExpectedDirection_ != longCaptureMotionDirection_
        ? 0
        : longCaptureExpectedDirection_;
    int localMinY = longCaptureCurrentY_ - frameHeight + minOverlap;
    int localMaxY = longCaptureCurrentY_ + frameHeight - minOverlap;
    if (matchDirection > 0) {
        localMinY = qMax(localMinY, longCaptureCurrentY_ - directionTolerance);
    } else if (matchDirection < 0) {
        localMaxY = qMin(localMaxY, longCaptureCurrentY_ + directionTolerance);
    }
    // Local continuity limits the search cost. Every resulting anchor is also
    // checked against committed rows so a poisoned live-frame chain cannot
    // create a silent gap when it reaches a document frontier.
    LongCapture::MatchResult match = LongCapture::matchSignatureInDocument(
        longCaptureCurrentSignature_,
        longCaptureCurrentY_,
        signature,
        localMinY,
        localMaxY,
        minOverlap,
        qMax(2, frameHeight / 360),
        longCaptureCurrentY_);
    bool relocalized = false;
    const bool localExtendsDocument = match.valid
        && (match.documentY < longCaptureDocTopY_
            || match.documentY + frameHeight > longCaptureDocBottomY_);
    int matchCommittedOverlap = 0;
    double matchCommittedScore = match.valid
        ? committedOverlapScore(match.documentY, &matchCommittedOverlap)
        : 255.0;
    // Alignment quality gate, graduated by confidence: the strict score bound
    // applies at the base acceptance confidence; an unambiguous, textured
    // match (ambiguous candidates are capped at 0.34) may carry moderate
    // rendering noise. Committed rows stay the ground truth — both the chain
    // score and the committed score must clear the same bound.
    const auto alignmentAcceptable = [&](const LongCapture::MatchResult& candidate,
                                         double committedScore) {
        if (!candidate.valid) {
            return false;
        }
        if (candidate.score <= kLongCaptureMaxAlignmentScore
            && committedScore <= kLongCaptureMaxAlignmentScore) {
            return true;
        }
        return candidate.confidence >= kLongCaptureNoisyAcceptConfidence
            && candidate.score <= kLongCaptureNoisyMaxAlignmentScore
            && committedScore <= kLongCaptureNoisyMaxAlignmentScore;
    };
    const bool localAlignmentUnreliable = match.valid
        && (!alignmentAcceptable(match, matchCommittedScore)
            || matchCommittedOverlap < minOverlap);
    if (!match.valid
        || (match.confidence < kLongCaptureAcceptConfidence && !localExtendsDocument)
        || localAlignmentUnreliable) {
        // Re-localization scores directly against committed rows, so its
        // acceptance bar (relocalize confidence + alignment score) is itself
        // committed evidence. Candidates stay confined to the local physical
        // continuity window — searching the full document would let repeated
        // content teleport the anchor on a reversal — but frontier positions
        // keeping at least minOverlap committed rows are allowed so a stale or
        // poisoned live-frame chain cannot deadlock stitching at an edge.
        const int globalMinY = qMax(longCaptureDocTopY_ - frameHeight + minOverlap,
                                    localMinY);
        const int globalMaxY = qMin(longCaptureDocBottomY_ - minOverlap,
                                    localMaxY);
        const LongCapture::MatchResult global = LongCapture::matchSignatureInDocument(
            longCaptureDocSignature_,
            longCaptureDocTopY_,
            signature,
            globalMinY,
            globalMaxY,
            minOverlap,
            qMax(2, frameHeight / 256),
            longCaptureCurrentY_);
        const bool globalReliable = global.valid
            && global.confidence >= kLongCaptureRelocalizeConfidence
            && (global.score <= kLongCaptureMaxAlignmentScore
                || (global.confidence >= kLongCaptureNoisyAcceptConfidence
                    && global.score <= kLongCaptureNoisyMaxAlignmentScore));
        if (globalReliable
            && (!match.valid || localAlignmentUnreliable
                || global.confidence > match.confidence)) {
            match = global;
            relocalized = true;
            matchCommittedScore = committedOverlapScore(match.documentY,
                                                         &matchCommittedOverlap);
        }
    }
    const bool extendsDocument = match.valid
        && (match.documentY < longCaptureDocTopY_
            || match.documentY + frameHeight > longCaptureDocBottomY_);
    const int extensionOverlap = minOverlap;
    const double requiredConfidence = kLongCaptureAcceptConfidence;
    const bool insufficientExtensionEvidence = extendsDocument
        && (match.overlap < extensionOverlap || matchCommittedOverlap < extensionOverlap);
    const bool unreliableAlignment = match.valid
        && !alignmentAcceptable(match, matchCommittedScore);
    if (!match.valid || match.confidence < requiredConfidence
        || insufficientExtensionEvidence || unreliableAlignment) {
        // Stable-frame recovery: the page has settled (consecutive samples
        // identical) at a confidently located position, but rendering drift
        // keeps every acceptance comparison above its threshold. Without this,
        // stitching and annotation deadlock — the chain anchor and committed
        // rows only refresh on acceptance, so the mismatch never clears.
        const int quietRemainingMs = LongCapture::remainingQuietPeriodMs(
            Perf::elapsedMs(),
            longCaptureLastWheelAtMs_,
            longCaptureLastMovementAtMs_,
            kLongCaptureSettleDelayMs);
        const bool recoveryCandidate = lastSampleScore <= kLongCaptureSameFrameScore
            && match.valid
            && match.confidence >= kLongCaptureRelocalizeConfidence
            && matchCommittedOverlap >= minOverlap
            && matchCommittedScore <= kLongCaptureRepairMaxScore
            && !longCaptureWheelDispatchInFlight_
            && quietRemainingMs == 0;
        if (recoveryCandidate) {
            if (longCaptureAnchorMismatchY_ != match.documentY) {
                longCaptureAnchorMismatchY_ = match.documentY;
                longCaptureAnchorMismatchSamples_ = 1;
            } else {
                ++longCaptureAnchorMismatchSamples_;
            }
            if (longCaptureAnchorMismatchSamples_ >= kLongCaptureRepairStableSamples) {
                longCaptureAnchorMismatchSamples_ = 0;
                if (repairLongCaptureCommittedFrame(image, signature, match.documentY)) {
                    applyLongCaptureInputRegion();
                    update(selection_ | longCapturePreviewPanelRect().adjusted(-2, -2, 2, 2));
                    return LongCaptureFrameResult::NoChange;
                }
            }
        } else {
            longCaptureAnchorMismatchSamples_ = 0;
        }
        longCaptureLastFrameRejected_ = true;
        longCaptureViewportStable_ = false;
        longCaptureStatus_ = QStringLiteral("拼接置信度低，请稍慢滚动");
        Perf::log(QStringLiteral("LongCapture.frame.reject id=%1 confidence=%2 required=%3 score=%4 docScore=%5 overlap=%6 docOverlap=%7 requiredOverlap=%8 currentY=%9 elapsed=%10ms")
                      .arg(overlayId_)
                      .arg(match.confidence, 0, 'f', 3)
                      .arg(requiredConfidence, 0, 'f', 3)
                      .arg(match.score, 0, 'f', 2)
                      .arg(matchCommittedScore, 0, 'f', 2)
                      .arg(match.overlap)
                      .arg(matchCommittedOverlap)
                      .arg(extendsDocument ? extensionOverlap : minOverlap)
                      .arg(longCaptureCurrentY_)
                      .arg(frameElapsed()));
        update(selection_ | longCapturePreviewPanelRect().adjusted(-2, -2, 2, 2));
        return LongCaptureFrameResult::Rejected;
    }

    const int previousY = longCaptureCurrentY_;
    if (match.documentY != previousY) {
        longCaptureMotionDirection_ = match.documentY > previousY ? 1 : -1;
        longCaptureLastMovementAtMs_ = Perf::elapsedMs();
        longCaptureViewportStable_ = false;
    }
    const bool covered = match.documentY >= longCaptureDocTopY_
        && match.documentY + frameHeight <= longCaptureDocBottomY_;
    if (covered) {
        longCaptureStatus_ = QStringLiteral("已回看到 %1 px").arg(match.documentY - longCaptureDocTopY_);
    } else {
        if (!appendLongCaptureContent(image, signature, match.documentY)) {
            update(selection_ | longCapturePreviewPanelRect().adjusted(-2, -2, 2, 2));
            return LongCaptureFrameResult::Failed;
        }
        longCaptureStatus_ = QStringLiteral("已截取 %1 px，匹配 %2%")
            .arg(longCaptureDocBottomY_ - longCaptureDocTopY_)
            .arg(qRound(match.confidence * 100.0));
    }
    longCaptureLastFrameRejected_ = false;
    longCaptureAnchorMismatchSamples_ = 0;
    longCaptureVisibleFrame_ = image;
    longCaptureCurrentSignature_ = signature;
    longCaptureCurrentY_ = match.documentY;
    const int previousOutputTopY = longCaptureOutputTopY_;
    const int previousOutputBottomY = longCaptureOutputBottomY_;
    longCaptureOutputTopY_ = qMin(longCaptureOutputTopY_, match.documentY);
    longCaptureOutputBottomY_ = qMax(longCaptureOutputBottomY_, match.documentY + frameHeight);
    Perf::log(QStringLiteral("LongCapture.frame.accept id=%1 covered=%2 reloc=%3 y=%4->%5 confidence=%6 score=%7 docScore=%8 overlap=%9 doc=[%10,%11) elapsed=%12ms")
                  .arg(overlayId_)
                  .arg(covered)
                  .arg(relocalized)
                  .arg(previousY)
                  .arg(match.documentY)
                  .arg(match.confidence, 0, 'f', 3)
                  .arg(match.score, 0, 'f', 2)
                  .arg(matchCommittedScore, 0, 'f', 2)
                  .arg(match.overlap)
                  .arg(longCaptureDocTopY_)
                  .arg(longCaptureDocBottomY_)
                  .arg(frameElapsed()));
    if (!covered || longCaptureOutputTopY_ != previousOutputTopY
        || longCaptureOutputBottomY_ != previousOutputBottomY) {
        scheduleLongCapturePreviewRefresh();
    }
    applyLongCaptureInputRegion();
    QTimer::singleShot(0, this, [this]() {
        if (isLongCaptureBrowseMode()) {
            applyLongCaptureInputRegion();
            update(selection_.adjusted(-12, -12, 12, 12));
        }
    });
    update(selection_ | longCapturePreviewPanelRect().adjusted(-2, -2, 2, 2));
    return LongCaptureFrameResult::Accepted;
}

bool CaptureOverlayWindow::ensureLongCaptureCanvasRange(int topY, int bottomY)
{
    const int width = qMax(1, selection_.width());
    const int requiredRows = bottomY - topY;
    const qint64 bytesPerRow = static_cast<qint64>(width) * 4;
    const int maxRows = qMin(kLongCaptureMaxDocumentRows,
                             static_cast<int>(kLongCaptureMaxCanvasBytes / bytesPerRow));
    if (requiredRows <= 0 || requiredRows > maxRows) {
        longCaptureStatus_ = QStringLiteral("长截图已达内存上限");
        Perf::log(QStringLiteral("LongCapture.canvas.limit id=%1 width=%2 rows=%3 maxRows=%4")
                      .arg(overlayId_)
                      .arg(width)
                      .arg(requiredRows)
                      .arg(maxRows));
        return false;
    }

    if (longCaptureCanvas_.isNull()) {
        const qint64 preferredRows = qMax<qint64>(2048, static_cast<qint64>(selection_.height()) * 2);
        const int capacity = qMin(maxRows,
                                  static_cast<int>(qMax<qint64>(requiredRows, preferredRows)));
        QImage next(width, capacity, QImage::Format_ARGB32);
        if (next.isNull()) {
            longCaptureStatus_ = QStringLiteral("长截图内存分配失败");
            Perf::log(QStringLiteral("LongCapture.canvas.allocate_failed id=%1 size=%2x%3")
                          .arg(overlayId_)
                          .arg(width)
                          .arg(capacity));
            return false;
        }
        next.fill(Qt::transparent);
        longCaptureCanvas_ = std::move(next);
        longCaptureCanvasDocTopY_ = topY;
        return true;
    }
    const int canvasTop = longCaptureCanvasDocTopY_;
    const int canvasBottom = canvasTop + longCaptureCanvas_.height();
    if (topY >= canvasTop && bottomY <= canvasBottom) {
        return true;
    }

    const bool hasDocument = longCaptureDocBottomY_ > longCaptureDocTopY_;
    const int contentTop = hasDocument ? longCaptureDocTopY_ : canvasTop;
    const int contentBottom = hasDocument ? longCaptureDocBottomY_ : canvasBottom;
    const int requiredTop = qMin(topY, contentTop);
    const int requiredBottom = qMax(bottomY, contentBottom);
    const int requiredSpan = requiredBottom - requiredTop;
    if (requiredSpan > maxRows) {
        longCaptureStatus_ = QStringLiteral("长截图已达内存上限");
        return false;
    }
    const int targetRows = qMin(maxRows,
                                static_cast<int>(qMax<qint64>(requiredSpan,
                                                              static_cast<qint64>(longCaptureCanvas_.height()) * 2)));
    const int spareRows = targetRows - requiredSpan;
    const int newTop = requiredTop - spareRows / 2;
    QImage next(width, targetRows, QImage::Format_ARGB32);
    if (next.isNull()) {
        longCaptureStatus_ = QStringLiteral("长截图内存分配失败");
        Perf::log(QStringLiteral("LongCapture.canvas.grow_failed id=%1 old=%2x%3 next=%4x%5")
                      .arg(overlayId_)
                      .arg(longCaptureCanvas_.width())
                      .arg(longCaptureCanvas_.height())
                      .arg(width)
                      .arg(targetRows));
        return false;
    }
    next.fill(Qt::transparent);
    QPainter painter(&next);
    painter.drawImage(QPoint(0, contentTop - newTop),
                      longCaptureCanvas_,
                      QRect(0,
                            contentTop - canvasTop,
                            longCaptureCanvas_.width(),
                            contentBottom - contentTop));
    painter.end();
    longCaptureCanvas_ = std::move(next);
    longCaptureCanvasDocTopY_ = newTop;
    return true;
}

bool CaptureOverlayWindow::appendLongCaptureContent(const QImage& image, const LongCapture::RowSignature& signature, int documentY)
{
    if (image.isNull() || !signature.isValid() || image.width() != selection_.width()) {
        longCaptureStatus_ = QStringLiteral("长截图帧尺寸无效");
        return false;
    }
    const int frameHeight = image.height();
    const bool firstContent = longCaptureDocBottomY_ <= longCaptureDocTopY_;
    const int newTop = firstContent ? documentY : qMin(longCaptureDocTopY_, documentY);
    const int newBottom = firstContent ? documentY + frameHeight : qMax(longCaptureDocBottomY_, documentY + frameHeight);
    if (newBottom - newTop > kLongCaptureMaxDocumentRows) {
        longCaptureStatus_ = QStringLiteral("长截图已达最大长度");
        Perf::log(QStringLiteral("LongCapture.append.limit id=%1 doc=[%2,%3) request=[%4,%5)")
                      .arg(overlayId_)
                      .arg(longCaptureDocTopY_)
                      .arg(longCaptureDocBottomY_)
                      .arg(newTop)
                      .arg(newBottom));
        return false;
    }
    if (!ensureLongCaptureCanvasRange(newTop, newBottom)) {
        return false;
    }

    struct Strip { int docStart; int docEnd; };
    Strip strips[2];
    int stripCount = 0;
    if (firstContent) {
        strips[stripCount++] = {documentY, documentY + frameHeight};
    } else {
        if (documentY < longCaptureDocTopY_) {
            strips[stripCount++] = {documentY, qMin(longCaptureDocTopY_, documentY + frameHeight)};
        }
        if (documentY + frameHeight > longCaptureDocBottomY_) {
            strips[stripCount++] = {qMax(longCaptureDocBottomY_, documentY), documentY + frameHeight};
        }
    }
    if (stripCount == 0) {
        return true;
    }
    QPainter painter(&longCaptureCanvas_);
    for (int i = 0; i < stripCount; ++i) {
        const int sourceY = strips[i].docStart - documentY;
        const int rows = strips[i].docEnd - strips[i].docStart;
        painter.drawImage(QPoint(0, strips[i].docStart - longCaptureCanvasDocTopY_),
                          image,
                          QRect(0, sourceY, image.width(), rows));
    }
    painter.end();

    if (firstContent) {
        longCaptureDocSignature_ = signature;
    } else {
        if (documentY < longCaptureDocTopY_) {
            const int rows = longCaptureDocTopY_ - documentY;
            longCaptureDocSignature_.luma = signature.luma.mid(0, rows) + longCaptureDocSignature_.luma;
            longCaptureDocSignature_.edge = signature.edge.mid(0, rows) + longCaptureDocSignature_.edge;
            longCaptureDocSignature_.spatialA = signature.spatialA.mid(0, rows) + longCaptureDocSignature_.spatialA;
            longCaptureDocSignature_.spatialB = signature.spatialB.mid(0, rows) + longCaptureDocSignature_.spatialB;
        }
        if (documentY + frameHeight > longCaptureDocBottomY_) {
            const int sourceIndex = longCaptureDocBottomY_ - documentY;
            longCaptureDocSignature_.luma += signature.luma.mid(sourceIndex);
            longCaptureDocSignature_.edge += signature.edge.mid(sourceIndex);
            longCaptureDocSignature_.spatialA += signature.spatialA.mid(sourceIndex);
            longCaptureDocSignature_.spatialB += signature.spatialB.mid(sourceIndex);
        }
    }

    longCaptureDocTopY_ = newTop;
    longCaptureDocBottomY_ = newBottom;
    return true;
}

QImage CaptureOverlayWindow::longCaptureResultImage() const
{
    const int height = longCaptureOutputBottomY_ - longCaptureOutputTopY_;
    if (longCaptureCanvas_.isNull() || height <= 0) {
        return {};
    }
    const int sourceY = longCaptureOutputTopY_ - longCaptureCanvasDocTopY_;
    if (sourceY < 0 || sourceY + height > longCaptureCanvas_.height()) {
        return {};
    }
    QImage result = longCaptureCanvas_.copy(0, sourceY, longCaptureCanvas_.width(), height)
                        .convertToFormat(QImage::Format_ARGB32);
    const bool hasEraser = std::any_of(longCaptureAnnotations_.items().cbegin(),
                                       longCaptureAnnotations_.items().cend(),
                                       [](const AnnotationItem& item) {
                                           return item.type == AnnotationType::Eraser;
                                       });
    const QPointF offset = longCaptureAnnotationsOverlayRelative_
        ? longCaptureAnnotationOverlayDelta_ + QPointF(0, longCaptureOutputTopY_)
        : QPointF(0, longCaptureOutputTopY_);
    if (hasEraser) {
        const QImage base = result;
        result = base.copy();
        renderAnnotationDocumentToImage(result,
                                        base,
                                        longCaptureAnnotations_,
                                        false,
                                        offset);
    } else {
        renderAnnotationDocumentToImage(result,
                                        result,
                                        longCaptureAnnotations_,
                                        false,
                                        offset);
    }
    return result;
}

QRect CaptureOverlayWindow::longCapturePreviewPanelRect() const
{
    if (!selection_.isValid()) {
        return {};
    }
    const int previewHeight = longCapturePreviewPanelHeight_ > 0
        ? qMin(longCapturePreviewPanelHeight_, qMax(180, height() - 16))
        : qBound(180, selection_.height(), qMax(180, height() - 16));
    QRect panel(selection_.right() + 8, selection_.top(), kLongCapturePreviewWidth, previewHeight);
    if (panel.right() > width() - 8) {
        panel.moveRight(selection_.left() - 8);
    }
    if (panel.left() < 8) {
        panel.moveRight(qMin(width() - 8, selection_.right() - 8));
    }
    panel.moveTop(qBound(8, panel.top(), qMax(8, height() - panel.height() - 8)));
    return panel;
}

void CaptureOverlayWindow::scheduleLongCapturePreviewRefresh()
{
    if (!longCaptureActive_ || !selection_.isValid()) {
        return;
    }
    const qint64 now = Perf::elapsedMs();
    if (longCapturePreviewDirtyAtMs_ < 0) {
        longCapturePreviewDirtyAtMs_ = now;
    }
    const int delayMs = longCaptureLastPreviewRefreshAtMs_ < 0
        ? 0
        : static_cast<int>(qMax<qint64>(
              0,
              kLongCapturePreviewRefreshMs - (now - longCaptureLastPreviewRefreshAtMs_)));
    if (longCapturePreviewRefreshTimer_.isActive()
        && longCapturePreviewRefreshTimer_.remainingTime() <= delayMs) {
        return;
    }
    longCapturePreviewRefreshTimer_.start(delayMs);
}

void CaptureOverlayWindow::invalidateLongCapturePreviewMip()
{
    longCapturePreviewMip_ = QImage();
    longCapturePreviewMipShift_ = 0;
    longCapturePreviewMipAnchorY_ = 0;
    longCapturePreviewMipTopY_ = 0;
    longCapturePreviewMipBottomY_ = 0;
}

// Maintains a power-of-two downscaled cache of the committed output range so
// each preview refresh only rescales newly captured strips instead of the
// whole document. Returns false when the document is short enough (or the mip
// cannot be allocated) that scaling directly from the canvas stays cheap.
bool CaptureOverlayWindow::updateLongCapturePreviewMip(int outputTop,
                                                       int outputBottom,
                                                       int targetHeight)
{
    const int width = longCaptureCanvas_.width();
    const int docHeight = outputBottom - outputTop;
    if (width <= 0 || docHeight <= 0 || targetHeight <= 0) {
        return false;
    }
    int shift = 0;
    while ((docHeight >> shift) > targetHeight * 2 && (width >> (shift + 1)) >= 8) {
        ++shift;
    }
    if (shift == 0) {
        invalidateLongCapturePreviewMip();
        return false;
    }
    const int block = 1 << shift;
    const int mipWidth = qMax(1, width >> shift);
    const auto floorDiv = [](int value, int divisor) {
        return value >= 0 ? value / divisor : -((-value + divisor - 1) / divisor);
    };

    for (int attempt = 0; attempt < 2; ++attempt) {
        if (longCapturePreviewMip_.isNull() || longCapturePreviewMipShift_ != shift
            || longCapturePreviewMip_.width() != mipWidth) {
            const int alignedTop = floorDiv(outputTop, block) * block;
            const int neededRows = (outputBottom - alignedTop + block - 1) / block;
            const int capacity = qMax(neededRows * 2, 64);
            QImage mip(mipWidth, capacity, QImage::Format_ARGB32_Premultiplied);
            if (mip.isNull()) {
                invalidateLongCapturePreviewMip();
                return false;
            }
            mip.fill(Qt::transparent);
            longCapturePreviewMip_ = std::move(mip);
            longCapturePreviewMipShift_ = shift;
            longCapturePreviewMipAnchorY_ = alignedTop
                - ((capacity - neededRows) / 2) * block;
            longCapturePreviewMipTopY_ = outputTop;
            longCapturePreviewMipBottomY_ = outputTop;
        }
        const int anchor = longCapturePreviewMipAnchorY_;
        const int requiredRowTop = floorDiv(outputTop - anchor, block);
        const int requiredRowBottom = (outputBottom - anchor + block - 1) / block;
        if (requiredRowTop < 0 || requiredRowBottom > longCapturePreviewMip_.height()) {
            // Out of headroom; rebuild recentered with doubled capacity.
            invalidateLongCapturePreviewMip();
            continue;
        }

        QPainter painter(&longCapturePreviewMip_);
        painter.setRenderHint(QPainter::SmoothPixmapTransform, true);
        painter.setCompositionMode(QPainter::CompositionMode_Source);
        const auto drawDocRange = [&](int top, int bottom) {
            // Redraw whole blocks so frontier rows scaled from a partial block
            // get replaced once the rest of the block is captured.
            const int alignedRangeTop = floorDiv(top - anchor, block) * block + anchor;
            const int alignedRangeBottom = anchor
                + ((bottom - anchor + block - 1) / block) * block;
            const int canvasTop = longCaptureCanvasDocTopY_;
            const int sourceTop = qMax(alignedRangeTop, canvasTop);
            const int sourceBottom = qMin(alignedRangeBottom,
                                          canvasTop + longCaptureCanvas_.height());
            if (sourceBottom <= sourceTop) {
                return;
            }
            const QRectF target(0.0,
                                static_cast<qreal>(sourceTop - anchor) / block,
                                longCapturePreviewMip_.width(),
                                static_cast<qreal>(sourceBottom - sourceTop) / block);
            painter.drawImage(target,
                              longCaptureCanvas_,
                              QRectF(0,
                                     sourceTop - canvasTop,
                                     longCaptureCanvas_.width(),
                                     sourceBottom - sourceTop));
        };
        if (longCapturePreviewMipBottomY_ <= longCapturePreviewMipTopY_) {
            drawDocRange(outputTop, outputBottom);
            longCapturePreviewMipTopY_ = outputTop;
            longCapturePreviewMipBottomY_ = outputBottom;
        } else {
            if (outputTop < longCapturePreviewMipTopY_) {
                drawDocRange(outputTop, longCapturePreviewMipTopY_);
                longCapturePreviewMipTopY_ = outputTop;
            }
            if (outputBottom > longCapturePreviewMipBottomY_) {
                drawDocRange(longCapturePreviewMipBottomY_, outputBottom);
                longCapturePreviewMipBottomY_ = outputBottom;
            }
        }
        painter.end();
        return true;
    }
    return false;
}

void CaptureOverlayWindow::refreshLongCapturePreviewFit()
{
    longCapturePreviewRefreshTimer_.stop();
    longCapturePreviewDirtyAtMs_ = -1;
    const int documentHeight = longCaptureOutputBottomY_ - longCaptureOutputTopY_;
    if (longCaptureCanvas_.isNull() || documentHeight <= 0) {
        longCapturePreviewFitted_ = QImage();
        invalidateLongCapturePreviewMip();
        return;
    }
    const qint64 now = Perf::elapsedMs();
    if (!longCapturePreviewFitted_.isNull() && longCaptureLastPreviewRefreshAtMs_ >= 0
        && now - longCaptureLastPreviewRefreshAtMs_ < kLongCapturePreviewRefreshMs) {
        return;
    }
    QElapsedTimer previewTimer;
    previewTimer.start();
    longCaptureLastPreviewRefreshAtMs_ = now;
    longCapturePreviewFitted_ = QImage();
    const QRect imageRect = longCapturePreviewPanelRect().adjusted(8, 8, -8, -42);
    if (!imageRect.isValid()) {
        return;
    }
    const int sourceY = longCaptureOutputTopY_ - longCaptureCanvasDocTopY_;
    if (sourceY < 0 || sourceY + documentHeight > longCaptureCanvas_.height()) {
        return;
    }
    const QSize outputSize(longCaptureCanvas_.width(), documentHeight);
    const QSize fittedSize = outputSize
                                 .scaled(imageRect.size(), Qt::KeepAspectRatio)
                                 .expandedTo(QSize(1, 1));
    QImage fitted(fittedSize, QImage::Format_ARGB32_Premultiplied);
    if (fitted.isNull()) {
        return;
    }
    QPainter painter(&fitted);
    painter.setRenderHint(QPainter::SmoothPixmapTransform, true);
    if (updateLongCapturePreviewMip(longCaptureOutputTopY_,
                                    longCaptureOutputBottomY_,
                                    imageRect.height())) {
        const qreal invBlock = 1.0 / (1 << longCapturePreviewMipShift_);
        painter.drawImage(QRectF(fitted.rect()),
                          longCapturePreviewMip_,
                          QRectF(0.0,
                                 (longCaptureOutputTopY_ - longCapturePreviewMipAnchorY_)
                                     * invBlock,
                                 longCapturePreviewMip_.width(),
                                 documentHeight * invBlock));
    } else {
        painter.drawImage(fitted.rect(),
                          longCaptureCanvas_,
                          QRect(0, sourceY, longCaptureCanvas_.width(), documentHeight));
    }
    painter.end();

    const QImage base = fitted.copy();
    const qreal scaleX = static_cast<qreal>(fitted.width()) / outputSize.width();
    const qreal scaleY = static_cast<qreal>(fitted.height()) / outputSize.height();
    const qreal styleScale = (scaleX + scaleY) / 2.0;
    const QPointF offset = longCaptureAnnotationsOverlayRelative_
        ? longCaptureAnnotationOverlayDelta_ + QPointF(0, longCaptureOutputTopY_)
        : QPointF(0, longCaptureOutputTopY_);
    const auto scaledEffectItem = [&](const AnnotationItem& source) {
        AnnotationItem item = source;
        QRectF rect = item.rect.translated(-offset);
        item.rect = QRectF(rect.x() * scaleX,
                           rect.y() * scaleY,
                           rect.width() * scaleX,
                           rect.height() * scaleY);
        for (QPointF& point : item.points) {
            point = QPointF((point.x() - offset.x()) * scaleX,
                            (point.y() - offset.y()) * scaleY);
        }
        return item;
    };
    for (const AnnotationItem& item : longCaptureAnnotations_.items()) {
        if (item.type == AnnotationType::Mosaic) {
            applyMosaicAnnotation(fitted, scaledEffectItem(item), QPointF(), styleScale);
        } else if (item.type == AnnotationType::Eraser) {
            applyEraserAnnotation(fitted, base, scaledEffectItem(item), QPointF(), styleScale);
        } else {
            QPainter annotationPainter(&fitted);
            annotationPainter.setRenderHint(QPainter::Antialiasing, true);
            annotationPainter.scale(scaleX, scaleY);
            drawAnnotation(annotationPainter, item, offset);
        }
    }
    longCapturePreviewFitted_ = std::move(fitted);
    if (previewTimer.elapsed() >= 8) {
        Perf::log(QStringLiteral("LongCapture.preview.fit id=%1 total=%2ms rows=%3 target=%4x%5 annotations=%6")
                      .arg(overlayId_)
                      .arg(previewTimer.elapsed())
                      .arg(documentHeight)
                      .arg(longCapturePreviewFitted_.width())
                      .arg(longCapturePreviewFitted_.height())
                      .arg(longCaptureAnnotations_.count()));
    }
}

QImage CaptureOverlayWindow::captureLongFrameImage(const QRect& requestedRect)
{
    const QRect logicalRect = (requestedRect.isValid() ? requestedRect : selection_)
                                  .normalized()
                                  .intersected(rect());
    if (!logicalRect.isValid()) {
        return {};
    }
    QElapsedTimer timer;
    timer.start();
    const QRect globalRect = logicalRect.translated(virtualGeometry_.topLeft());
    const bool capturesCurrentSelection = logicalRect
        == selection_.normalized().intersected(rect());
    const bool wasVisible = isVisible();
    const bool viewportWasVisible = longCaptureViewportLayer_
        && longCaptureViewportLayer_->isVisible();
    const qreal oldOpacity = windowOpacity();
    bool overlayRegionSimplified = false;
#ifdef Q_OS_WIN
    const HWND overlayHwnd = reinterpret_cast<HWND>(winId());
    HRGN savedOverlayRegion = nullptr;
    bool overlayRegionAlreadyClean = false;
    if (!longCaptureExclusionChecked_) {
        longCaptureExclusionAvailable_ =
            Platform::applyWindowCaptureExclusion(winId(), false)
            == Platform::WindowCaptureExclusion::Excluded;
        longCaptureExclusionChecked_ = true;
    }
    const bool exclusionOk = longCaptureExclusionAvailable_;
    if (!exclusionOk && capturesCurrentSelection && longCaptureInputRegionApplied_) {
        const auto intersectsCapture = [this](const QRect& region) {
            return region.isValid() && region.intersects(longCaptureAppliedRegionRect_);
        };
        overlayRegionAlreadyClean = !intersectsCapture(longCaptureAppliedToolbarRegionRect_)
            && !intersectsCapture(longCaptureAppliedPreviewRegionRect_)
            && !intersectsCapture(longCaptureAppliedTopHandleRegionRect_)
            && !intersectsCapture(longCaptureAppliedBottomHandleRegionRect_);
    }
    if (!exclusionOk && capturesCurrentSelection && !overlayRegionAlreadyClean
        && longCaptureInputRegionApplied_
        && longCaptureAppliedRegionRect_.isValid()
        && longCaptureAppliedRegionWindowSize_.isValid()) {
        savedOverlayRegion = CreateRectRgn(0, 0, 0, 0);
        const int savedType = savedOverlayRegion
            ? GetWindowRgn(overlayHwnd, savedOverlayRegion)
            : ERROR;
        HRGN captureRegion = CreateRectRgn(0,
                                           0,
                                           longCaptureAppliedRegionWindowSize_.width(),
                                           longCaptureAppliedRegionWindowSize_.height());
        HRGN holeRegion = CreateRectRgn(longCaptureAppliedRegionRect_.left(),
                                        longCaptureAppliedRegionRect_.top(),
                                        longCaptureAppliedRegionRect_.right() + 1,
                                        longCaptureAppliedRegionRect_.bottom() + 1);
        if (savedType != ERROR && savedType != NULLREGION && captureRegion && holeRegion
            && CombineRgn(captureRegion, captureRegion, holeRegion, RGN_DIFF) != ERROR
            && SetWindowRgn(overlayHwnd, captureRegion, FALSE) != 0) {
            overlayRegionSimplified = true;
            captureRegion = nullptr;
        }
        if (captureRegion) {
            DeleteObject(captureRegion);
        }
        if (holeRegion) {
            DeleteObject(holeRegion);
        }
        if (!overlayRegionSimplified && savedOverlayRegion) {
            DeleteObject(savedOverlayRegion);
            savedOverlayRegion = nullptr;
        }
    }
    const bool shouldHideOverlay = wasVisible && !exclusionOk
        && (!capturesCurrentSelection
            || (!overlayRegionAlreadyClean && !overlayRegionSimplified));
    if (!exclusionOk && longCaptureDocBottomY_ <= longCaptureDocTopY_) {
        Perf::log(QStringLiteral("LongCapture.frameImage.exclusion_failed id=%1 error=%2 overlay={%3}")
                      .arg(overlayId_)
                      .arg(static_cast<int>(GetLastError()))
                      .arg(hwndDebugText(overlayHwnd)));
    }
#else
    const bool shouldHideOverlay = wasVisible;
#endif
    const bool shouldHideViewport = viewportWasVisible
        && !longCaptureViewportExclusionAvailable_;
    if (overlayRegionSimplified || shouldHideViewport) {
        if (shouldHideViewport) {
            longCaptureViewportLayer_->hide();
        }
        flushWindowSystemChanges();
    }
    if (wasVisible && shouldHideOverlay) {
        // Rare fallback for systems without capture exclusion: the overlay
        // must actually disappear for the duration of the grab.
        hide();
        flushWindowSystemChanges();
    }

    QImage image;
#ifdef Q_OS_WIN
    WinDesktopCaptureStats captureStats;
    if (screensUseUnitScale(globalRect)) {
        image = captureDesktopWin(globalRect, &captureStats);
    } else {
        // Scaled DPI: grab natively via GDI when a uniform scale allows the
        // linear mapping, then downscale to the logical selection size. Falls
        // back to the (much slower) per-screen Qt grab on mixed-DPI desktops.
        qreal scale = 1.0;
        QRect grabRect;
        if (allScreensUniformScale(&scale)) {
            grabRect = nativeRectForClientRect(reinterpret_cast<HWND>(winId()),
                                               logicalRect,
                                               scale);
        }
        if (grabRect.isValid()) {
            image = captureDesktopWin(grabRect, &captureStats);
            if (!image.isNull() && image.size() != globalRect.size()) {
                image = image.scaled(globalRect.size(),
                                     Qt::IgnoreAspectRatio,
                                     Qt::SmoothTransformation);
            }
        } else {
            image = captureDesktopQt(globalRect);
        }
    }
    if (captureStats.totalMs > 32 || captureStats.error != ERROR_SUCCESS
        || captureStats.resized) {
        Perf::log(QStringLiteral("LongCapture.grab.sync id=%1 total=%2ms setup=%3ms bitblt=%4ms copy=%5ms resized=%6 error=%7")
                      .arg(overlayId_)
                      .arg(captureStats.totalMs)
                      .arg(captureStats.setupMs)
                      .arg(captureStats.bitBltMs)
                      .arg(captureStats.copyMs)
                      .arg(captureStats.resized)
                      .arg(static_cast<int>(captureStats.error)));
    }
#else
    image = captureDesktopQt(globalRect);
#endif

#ifdef Q_OS_WIN
    if (overlayRegionSimplified && savedOverlayRegion) {
        if (SetWindowRgn(overlayHwnd, savedOverlayRegion, FALSE) == 0) {
            DeleteObject(savedOverlayRegion);
        }
        savedOverlayRegion = nullptr;
    }
#endif

    if (wasVisible && shouldHideOverlay) {
        show();
#ifdef Q_OS_WIN
        if (!longCaptureExclusionChecked_ || longCaptureExclusionAvailable_) {
            applyCaptureExclusion();
        }
        if (screensUseUnitScale(virtualGeometry_)) {
            SetWindowPos(reinterpret_cast<HWND>(winId()),
                         HWND_TOPMOST,
                         virtualGeometry_.x(),
                         virtualGeometry_.y(),
                         virtualGeometry_.width(),
                         virtualGeometry_.height(),
                         SWP_NOACTIVATE | SWP_NOOWNERZORDER | SWP_NOSENDCHANGING | SWP_SHOWWINDOW);
        } else {
            setGeometry(virtualGeometry_);
            raise();
        }
#else
        raise();
#endif
        setWindowOpacity(oldOpacity);
        setFocus();
        grabKeyboard();
        placeToolbar();
        flushWindowSystemChanges();
    }
    const QImage converted = image.convertToFormat(QImage::Format_ARGB32);
    if (converted.isNull() || timer.elapsed() > 50) {
        Perf::log(QStringLiteral("LongCapture.frameImage id=%1 elapsed=%2ms null=%3 size=%4x%5 hideFallback=%6")
                      .arg(overlayId_)
                      .arg(timer.elapsed())
                      .arg(converted.isNull())
                      .arg(converted.width())
                      .arg(converted.height())
                      .arg(shouldHideOverlay));
    }
    return converted;
}

void CaptureOverlayWindow::resolveLongCaptureTarget(const QPoint& globalPos)
{
#ifdef Q_OS_WIN
    const POINT global{globalPos.x(), globalPos.y()};
    const HWND overlay = reinterpret_cast<HWND>(winId());
    const HWND viewport = longCaptureViewportLayer_
        ? reinterpret_cast<HWND>(longCaptureViewportLayer_->winId())
        : nullptr;
    const HWND root = topLevelWindowBelowPoint(global, overlay, viewport);
    const HWND target = deepestChildWindowAtPoint(root, global);
    longCaptureTargetHwnd_ = reinterpret_cast<quintptr>(target);
#else
    Q_UNUSED(globalPos)
    longCaptureTargetHwnd_ = 0;
#endif
}

bool CaptureOverlayWindow::sendLongCaptureWheel(int wheelDelta,
                                                const QPoint& globalPos,
                                                quint16 keyState)
{
#ifdef Q_OS_WIN
    resolveLongCaptureTarget(globalPos);
    HWND target = reinterpret_cast<HWND>(longCaptureTargetHwnd_);
    if (!target || !IsWindow(target) || !IsWindowVisible(target) || !IsWindowEnabled(target)) {
        Perf::log(QStringLiteral("LongCapture.wheel.send.no_target id=%1 delta=%2")
                      .arg(overlayId_)
                      .arg(wheelDelta));
        return false;
    }

    constexpr UINT kWheelDeliveryTimeoutMs = 50;
    DWORD_PTR messageResult = 0;
    QElapsedTimer timer;
    timer.start();
    SetLastError(ERROR_SUCCESS);
    const LRESULT delivered = SendMessageTimeoutW(
        target,
        WM_MOUSEWHEEL,
        MAKEWPARAM(keyState, static_cast<SHORT>(wheelDelta)),
        MAKELPARAM(static_cast<SHORT>(globalPos.x()),
                   static_cast<SHORT>(globalPos.y())),
        SMTO_ABORTIFHUNG | SMTO_BLOCK | SMTO_ERRORONEXIT,
        kWheelDeliveryTimeoutMs,
        &messageResult);
    const qint64 elapsedMs = timer.elapsed();
    if (delivered == 0) {
        Perf::log(QStringLiteral("LongCapture.wheel.send.failed id=%1 delta=%2 elapsed=%3ms error=%4 target={%5}")
                      .arg(overlayId_)
                      .arg(wheelDelta)
                      .arg(elapsedMs)
                      .arg(static_cast<int>(GetLastError()))
                      .arg(hwndDebugText(target)));
        return false;
    }
    if (elapsedMs > kLongCaptureBurstIntervalMs) {
        Perf::log(QStringLiteral("LongCapture.wheel.send.slow id=%1 delta=%2 elapsed=%3ms target={%4}")
                      .arg(overlayId_)
                      .arg(wheelDelta)
                      .arg(elapsedMs)
                      .arg(hwndDebugText(target)));
    }
    return true;
#else
    Q_UNUSED(wheelDelta)
    Q_UNUSED(globalPos)
    Q_UNUSED(keyState)
    return false;
#endif
}

void CaptureOverlayWindow::enqueueLongCaptureWheel(int wheelDelta,
                                                   const QPoint& globalPos,
                                                   quint16 keyState)
{
    if (gLongCaptureHookInputPaused.load(std::memory_order_acquire)
        || !isLongCaptureBrowseMode() || mode_ != Mode::Ready || wheelDelta == 0) {
        return;
    }
    ++longCaptureWheelReceivedCount_;
    const qint64 delta = wheelDelta;
    if (longCaptureLastFrameRejected_) {
        const bool reversesRejectedDirection = (delta < 0 ? 1 : -1) != longCaptureExpectedDirection_;
        if (!reversesRejectedDirection) {
            if (!longCaptureWheelQueue_.isEmpty()
                && ((longCaptureWheelQueue_.back().delta > 0) == (delta > 0))) {
                LongCaptureWheelSegment& segment = longCaptureWheelQueue_.back();
                segment.delta += delta;
                segment.globalPos = globalPos;
                segment.keyState = keyState;
            } else {
                longCaptureWheelQueue_.enqueue({delta, globalPos, keyState});
            }
            // Dispatch stays paused while unmatched, but the retry sampler must
            // run so the queued input can flow once the page settles back.
            if (!longCaptureProgressTimer_.isActive()) {
                longCaptureProgressTimer_.start(kLongCapturePauseRetryIntervalMs);
            }
            return;
        }
        Perf::log(QStringLiteral("LongCapture.wheel.recover id=%1 discardedSegments=%2")
                      .arg(overlayId_)
                      .arg(longCaptureWheelQueue_.size()));
        longCaptureWheelQueue_.clear();
    }
    if (!longCaptureWheelQueue_.isEmpty()
        && ((longCaptureWheelQueue_.back().delta > 0) == (delta > 0))) {
        LongCaptureWheelSegment& segment = longCaptureWheelQueue_.back();
        segment.delta += delta;
        segment.globalPos = globalPos;
        segment.keyState = keyState;
    } else {
        longCaptureWheelQueue_.enqueue({delta, globalPos, keyState});
    }
    if (!longCaptureWheelDispatchInFlight_ && !longCaptureWheelDispatchScheduled_) {
        longCaptureWheelDispatchScheduled_ = true;
        QTimer::singleShot(0, this, &CaptureOverlayWindow::dispatchNextLongCaptureWheel);
    }
}

void CaptureOverlayWindow::dispatchNextLongCaptureWheel()
{
    longCaptureWheelDispatchScheduled_ = false;
    if (gLongCaptureHookInputPaused.load(std::memory_order_acquire)
        || !isLongCaptureBrowseMode() || longCaptureWheelDispatchInFlight_
        || longCaptureWheelQueue_.isEmpty() || longCaptureCanvas_.isNull()) {
        return;
    }

    const qint64 now = Perf::elapsedMs();
    const bool dispatchCadenceReady = longCaptureLastWheelAtMs_ < 0
        || now - longCaptureLastWheelAtMs_ >= kLongCaptureMinDispatchIntervalMs;
    const bool trackedMotionSafe = longCaptureLastTrackedFrameMovement_
        <= longCaptureMinRequiredOverlap(selection_.height());
    if (!dispatchCadenceReady || !trackedMotionSafe) {
        if (!longCaptureProgressTimer_.isActive()) {
            longCaptureProgressTimer_.start(kLongCaptureBurstIntervalMs);
        }
        return;
    }

    const QPoint globalPos = longCaptureWheelQueue_.head().globalPos;
#ifdef Q_OS_WIN
    if (longCaptureInputRegionApplied_) {
        const QPoint nativeLocal = globalPos - longCaptureAppliedWindowGlobalTopLeft_;
        const bool overOverlayUi = longCaptureAppliedToolbarRegionRect_.contains(nativeLocal)
            || longCaptureAppliedPreviewRegionRect_.contains(nativeLocal);
        if (!longCaptureAppliedRegionRect_.contains(nativeLocal) || overOverlayUi) {
            longCaptureStatus_ = QStringLiteral("滚轮输入已排队，请将鼠标移回截图区域");
            if (!longCaptureProgressTimer_.isActive()) {
                longCaptureProgressTimer_.start(kLongCaptureBurstIntervalMs);
            }
            return;
        }
    }
#endif

    LongCaptureWheelSegment& segment = longCaptureWheelQueue_.head();
    const quint16 keyState = segment.keyState;
    const int wheelDelta = segment.delta > 0
        ? static_cast<int>(qMin<qint64>(segment.delta, kLongCaptureMaxWheelDispatchDelta))
        : static_cast<int>(qMax<qint64>(segment.delta, -kLongCaptureMaxWheelDispatchDelta));
    segment.delta -= wheelDelta;
    if (segment.delta == 0) {
        longCaptureWheelQueue_.dequeue();
    }
    if (!sendLongCaptureWheel(wheelDelta, globalPos, keyState)) {
        longCaptureWheelQueue_.clear();
        longCaptureStatus_ = QStringLiteral("无法向目标窗口重放滚轮输入，长截图已暂停");
        update(longCapturePreviewPanelRect().adjusted(-2, -2, 2, 2));
        return;
    }

    longCaptureWheelDispatchInFlight_ = true;
    longCaptureDispatchMovementObserved_ = false;
    longCaptureViewportStable_ = false;
    applyLongCaptureInputRegion();
    update(selection_.adjusted(-12, -12, 12, 12));
    longCaptureDispatchedWheelDelta_ = wheelDelta;
    longCaptureExpectedDirection_ = wheelDelta < 0 ? 1 : -1;
    longCaptureLastWheelAtMs_ = Perf::elapsedMs();
    longCaptureWheelDispatchAtMs_ = longCaptureLastWheelAtMs_;
    ++longCaptureWheelDispatchedCount_;
    Perf::log(QStringLiteral("LongCapture.wheel.dispatch id=%1 seq=%2 delta=%3 key=0x%4 target=0x%5 point=%6,%7 queued=%8")
                  .arg(overlayId_)
                  .arg(longCaptureWheelDispatchedCount_)
                  .arg(wheelDelta)
                  .arg(keyState, 0, 16)
                  .arg(longCaptureTargetHwnd_, 0, 16)
                  .arg(globalPos.x())
                  .arg(globalPos.y())
                  .arg(longCaptureWheelQueue_.size()));
    if (!longCaptureProgressTimer_.isActive()
        || longCaptureProgressTimer_.interval() != kLongCaptureBurstIntervalMs) {
        longCaptureProgressTimer_.start(kLongCaptureBurstIntervalMs);
    }
}

void CaptureOverlayWindow::completeLongCaptureWheelDispatch(bool moved)
{
    if (!longCaptureWheelDispatchInFlight_) {
        return;
    }
    ++longCaptureWheelCompletedCount_;
    Perf::log(QStringLiteral("LongCapture.wheel.complete id=%1 seq=%2 delta=%3 moved=%4 latency=%5ms queued=%6")
                  .arg(overlayId_)
                  .arg(longCaptureWheelCompletedCount_)
                  .arg(longCaptureDispatchedWheelDelta_)
                  .arg(moved)
                  .arg(qMax<qint64>(0, Perf::elapsedMs() - longCaptureWheelDispatchAtMs_))
                  .arg(longCaptureWheelQueue_.size()));
    longCaptureWheelDispatchInFlight_ = false;
    longCaptureDispatchMovementObserved_ = false;
    longCaptureWheelDispatchAtMs_ = -1;
    longCaptureDispatchedWheelDelta_ = 0;
    if (!longCaptureWheelQueue_.isEmpty() && !longCaptureWheelDispatchScheduled_) {
        // Dispatch before returning to the event loop. This guarantees the next
        // progress sample is taken after the wheel message it acknowledges.
        dispatchNextLongCaptureWheel();
    } else if (longCaptureWheelQueue_.isEmpty()) {
        applyLongCaptureInputRegion();
        update(selection_.adjusted(-12, -12, 12, 12));
    }
}

void CaptureOverlayWindow::abortLongCaptureWheelDispatch(const QString& reason)
{
    const qsizetype queuedSegments = longCaptureWheelQueue_.size();
    longCaptureProgressTimer_.stop();
    longCaptureWheelQueue_.clear();
    longCaptureWheelDispatchInFlight_ = false;
    longCaptureWheelDispatchScheduled_ = false;
    longCaptureDispatchMovementObserved_ = false;
    longCaptureWheelDispatchAtMs_ = -1;
    longCaptureDispatchedWheelDelta_ = 0;
    Perf::log(QStringLiteral("LongCapture.wheel.abort id=%1 reason=%2 queued=%3")
                  .arg(overlayId_)
                  .arg(reason)
                  .arg(queuedSegments));
    update(longCapturePreviewPanelRect().adjusted(-2, -2, 2, 2));
}

void CaptureOverlayWindow::installLongCaptureWheelHook()
{
#ifdef Q_OS_WIN
    gLongCaptureHookInputPaused.store(false, std::memory_order_release);
    if (isLongCaptureWheelHookActive(this)) {
        return;
    }
    if (gLongCaptureMouseHookThread.joinable()) {
        const DWORD threadId = gLongCaptureMouseHookThreadId.load(std::memory_order_acquire);
        if (threadId != 0) {
            PostThreadMessageW(threadId, WM_QUIT, 0, 0);
        }
        gLongCaptureMouseHookThread.join();
    }

    gLongCaptureMouseHookWindow = this;
    gLongCaptureWheelReceiver.store(reinterpret_cast<HWND>(winId()), std::memory_order_release);
    clearLongCaptureHookRegion();
    gLongCaptureMouseHookInstallError.store(0, std::memory_order_relaxed);
    HANDLE readyEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!readyEvent) {
        gLongCaptureMouseHookWindow = nullptr;
        gLongCaptureWheelReceiver.store(nullptr, std::memory_order_release);
        Perf::log(QStringLiteral("LongCapture.wheel.hook.install id=%1 ok=0 error=%2")
                      .arg(overlayId_)
                      .arg(static_cast<int>(GetLastError())));
        return;
    }
    gLongCaptureMouseHookThread = std::thread([readyEvent]() {
        MSG message{};
        PeekMessageW(&message, nullptr, WM_USER, WM_USER, PM_NOREMOVE);
        gLongCaptureMouseHookThreadId.store(GetCurrentThreadId(), std::memory_order_release);
        HHOOK hook = SetWindowsHookExW(WH_MOUSE_LL,
                                       longCaptureMouseHookProc,
                                       GetModuleHandleW(nullptr),
                                       0);
        if (!hook) {
            gLongCaptureMouseHookInstallError.store(GetLastError(), std::memory_order_relaxed);
        }
        gLongCaptureMouseHook.store(hook, std::memory_order_release);
        SetEvent(readyEvent);
        if (hook) {
            while (GetMessageW(&message, nullptr, 0, 0) > 0) {
                TranslateMessage(&message);
                DispatchMessageW(&message);
            }
            UnhookWindowsHookEx(hook);
        }
        gLongCaptureMouseHook.store(nullptr, std::memory_order_release);
        gLongCaptureMouseHookThreadId.store(0, std::memory_order_release);
    });
    const DWORD waitResult = WaitForSingleObject(readyEvent, 2000);
    const bool ok = waitResult == WAIT_OBJECT_0
        && gLongCaptureMouseHook.load(std::memory_order_acquire) != nullptr;
    Perf::log(QStringLiteral("LongCapture.wheel.hook.install id=%1 ok=%2 error=%3")
                  .arg(overlayId_)
                  .arg(ok)
                  .arg(ok ? 0 : static_cast<int>(gLongCaptureMouseHookInstallError.load(std::memory_order_relaxed))));
    if (!ok) {
        const DWORD threadId = gLongCaptureMouseHookThreadId.load(std::memory_order_acquire);
        if (threadId != 0) {
            PostThreadMessageW(threadId, WM_QUIT, 0, 0);
        }
        if (gLongCaptureMouseHookThread.joinable()) {
            gLongCaptureMouseHookThread.join();
        }
        gLongCaptureMouseHookWindow = nullptr;
        gLongCaptureWheelReceiver.store(nullptr, std::memory_order_release);
    }
    CloseHandle(readyEvent);
#endif
}

void CaptureOverlayWindow::uninstallLongCaptureWheelHook()
{
#ifdef Q_OS_WIN
    gLongCaptureHookInputPaused.store(false, std::memory_order_release);
    if (gLongCaptureMouseHookWindow != this) {
        return;
    }
    clearLongCaptureHookRegion();
    gLongCaptureWheelReceiver.store(nullptr, std::memory_order_release);
    const DWORD threadId = gLongCaptureMouseHookThreadId.load(std::memory_order_acquire);
    const bool posted = threadId == 0 || PostThreadMessageW(threadId, WM_QUIT, 0, 0) != FALSE;
    if (gLongCaptureMouseHookThread.joinable()) {
        gLongCaptureMouseHookThread.join();
    }
    const bool ok = posted && gLongCaptureMouseHook.load(std::memory_order_acquire) == nullptr;
    Perf::log(QStringLiteral("LongCapture.wheel.hook.uninstall id=%1 ok=%2 error=%3")
                  .arg(overlayId_)
                  .arg(ok)
                  .arg(ok ? 0 : static_cast<int>(GetLastError())));
    gLongCaptureMouseHookWindow = nullptr;
#endif
}

bool CaptureOverlayWindow::handleLongCaptureNativeWheel(int wheelDelta,
                                                        const QPoint& globalPos,
                                                        quint16 keyState)
{
    if (!longCaptureActive_ || !selection_.isValid() || wheelDelta == 0) {
        return false;
    }
    if (gLongCaptureHookInputPaused.load(std::memory_order_acquire)) {
        return true;
    }

    bool insideSelection = false;
#ifdef Q_OS_WIN
    if ((GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0
        || (GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0
        || (GetAsyncKeyState(VK_MENU) & 0x8000) != 0) {
        return false;
    }
    if (longCaptureInputRegionApplied_) {
        const QPoint nativeLocal = globalPos - longCaptureAppliedWindowGlobalTopLeft_;
        if (longCaptureAppliedToolbarRegionRect_.contains(nativeLocal)
            || longCaptureAppliedPreviewRegionRect_.contains(nativeLocal)) {
            return false;
        }
        insideSelection = longCaptureAppliedRegionRect_.contains(nativeLocal);
    }
#else
    const QPoint logicalLocal = mapFromGlobal(globalPos);
#endif
    if (!longCaptureInputRegionApplied_) {
#ifdef Q_OS_WIN
        const QPoint logicalLocal = mapFromGlobal(globalPos);
#endif
        insideSelection = selection_.contains(logicalLocal);
    }
    if (!insideSelection) {
        return false;
    }

    enqueueLongCaptureWheel(wheelDelta, globalPos, keyState);
    return true;
}

void CaptureOverlayWindow::drawLongCapturePreview(QPainter& painter) const
{
    if (!longCaptureActive_ || !selection_.isValid()) {
        return;
    }
    const QRect panel = longCapturePreviewPanelRect();
    if (!panel.isValid()) {
        return;
    }

    painter.save();
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.setPen(QPen(QColor(79, 124, 255, 190), 1));
    painter.setBrush(QColor(15, 23, 42, 220));
    painter.drawRoundedRect(panel, 8, 8);

    const QRect imageRect = panel.adjusted(8, 8, -8, -42);
    if (!longCapturePreviewFitted_.isNull() && imageRect.isValid()) {
        QRect target(QPoint(0, 0), longCapturePreviewFitted_.size());
        target.moveCenter(imageRect.center());
        painter.drawImage(target.topLeft(), longCapturePreviewFitted_);
        const int docHeight = longCaptureOutputBottomY_ - longCaptureOutputTopY_;
        if (docHeight > 0) {
            const qreal scale = static_cast<qreal>(target.height()) / docHeight;
            const int currentImageY = longCaptureCurrentY_ - longCaptureOutputTopY_;
            QRectF viewport(target.left(), target.top() + currentImageY * scale, target.width(), selection_.height() * scale);
            viewport = viewport.intersected(QRectF(target));
            painter.setPen(QPen(QColor(255, 255, 255, 230), 1.2));
            painter.setBrush(QColor(79, 124, 255, 48));
            painter.drawRect(viewport);
        }
    }

    QFont font = painter.font();
    font.setPixelSize(11);
    font.setBold(true);
    painter.setFont(font);
    painter.setPen(Qt::white);
    const QString status = longCaptureStatus_.isEmpty() ? QStringLiteral("长截图") : longCaptureStatus_;
    painter.drawText(panel.adjusted(8, panel.height() - 34, -8, -8), Qt::AlignLeft | Qt::AlignVCenter | Qt::TextWordWrap, status);
    painter.restore();
}

CaptureOverlayWindow::SelectionDragMode CaptureOverlayWindow::selectionDragModeAt(const QPoint& pos) const
{
    if (!selection_.isValid()) {
        return SelectionDragMode::None;
    }

    const QRect r = selection_.normalized();
    const int hit = qMax(7, captureSettings_.borderWidth + 5);
    if (longCaptureActive_) {
        const QRect topEdge(r.left(), r.top() - hit, r.width(), hit * 2 + 1);
        const QRect bottomEdge(r.left(), r.bottom() - hit, r.width(), hit * 2 + 1);
        if (topEdge.contains(pos) && canResizeLongCaptureEdge(SelectionDragMode::ResizeTop)) {
            return SelectionDragMode::ResizeTop;
        }
        if (bottomEdge.contains(pos) && canResizeLongCaptureEdge(SelectionDragMode::ResizeBottom)) {
            return SelectionDragMode::ResizeBottom;
        }
        return SelectionDragMode::None;
    }
    auto hitBox = [hit](const QPoint& p) {
        return QRect(p.x() - hit, p.y() - hit, hit * 2 + 1, hit * 2 + 1);
    };

    const QPoint topLeft = r.topLeft();
    const QPoint topMid(r.center().x(), r.top());
    const QPoint topRight = r.topRight();
    const QPoint leftMid(r.left(), r.center().y());
    const QPoint rightMid(r.right(), r.center().y());
    const QPoint bottomLeft = r.bottomLeft();
    const QPoint bottomMid(r.center().x(), r.bottom());
    const QPoint bottomRight = r.bottomRight();

    if (hitBox(topLeft).contains(pos)) return SelectionDragMode::ResizeTopLeft;
    if (hitBox(topRight).contains(pos)) return SelectionDragMode::ResizeTopRight;
    if (hitBox(bottomLeft).contains(pos)) return SelectionDragMode::ResizeBottomLeft;
    if (hitBox(bottomRight).contains(pos)) return SelectionDragMode::ResizeBottomRight;
    if (hitBox(topMid).contains(pos)) return SelectionDragMode::ResizeTop;
    if (hitBox(bottomMid).contains(pos)) return SelectionDragMode::ResizeBottom;
    if (hitBox(leftMid).contains(pos)) return SelectionDragMode::ResizeLeft;
    if (hitBox(rightMid).contains(pos)) return SelectionDragMode::ResizeRight;

    const QRect topEdge(r.left() + hit, r.top() - hit, qMax(1, r.width() - hit * 2), hit * 2 + 1);
    const QRect bottomEdge(r.left() + hit, r.bottom() - hit, qMax(1, r.width() - hit * 2), hit * 2 + 1);
    const QRect leftEdge(r.left() - hit, r.top() + hit, hit * 2 + 1, qMax(1, r.height() - hit * 2));
    const QRect rightEdge(r.right() - hit, r.top() + hit, hit * 2 + 1, qMax(1, r.height() - hit * 2));
    if (topEdge.contains(pos)) return SelectionDragMode::ResizeTop;
    if (bottomEdge.contains(pos)) return SelectionDragMode::ResizeBottom;
    if (leftEdge.contains(pos)) return SelectionDragMode::ResizeLeft;
    if (rightEdge.contains(pos)) return SelectionDragMode::ResizeRight;

    if (r.contains(pos)) {
        return SelectionDragMode::Move;
    }
    return SelectionDragMode::None;
}

QRect CaptureOverlayWindow::resizedSelectionRect(const QPoint& pos) const
{
    QPoint p(qBound(0, pos.x(), width() - 1), qBound(0, pos.y(), height() - 1));
    QRect next = selectionDragStartRect_;
    if (longCaptureActive_) {
        if (selectionDragMode_ == SelectionDragMode::ResizeTop) {
            const int maxTop = next.bottom() - kLongCaptureMinimumViewportHeight + 1;
            int minTop = selectionDragStartRect_.top();
            if (!longCaptureResizePreviewFrame_.isNull()) {
                minTop = qBound(
                    0,
                    selectionDragStartRect_.top()
                        + longCaptureResizePreviewDocumentY_ - longCaptureCurrentY_,
                    height() - 1);
            }
            next.setTop(qBound(qMin(minTop, maxTop), p.y(), maxTop));
        } else if (selectionDragMode_ == SelectionDragMode::ResizeBottom) {
            const int minBottom = next.top() + kLongCaptureMinimumViewportHeight - 1;
            int maxBottom = selectionDragStartRect_.bottom();
            if (!longCaptureResizePreviewFrame_.isNull()) {
                const int previewBottomY = longCaptureResizePreviewDocumentY_
                    + longCaptureResizePreviewFrame_.height();
                maxBottom = qBound(
                    0,
                    selectionDragStartRect_.top()
                        + previewBottomY - longCaptureCurrentY_ - 1,
                    height() - 1);
            }
            next.setBottom(qBound(minBottom, p.y(), qMax(minBottom, maxBottom)));
        }
        return next.intersected(rect());
    }
    switch (selectionDragMode_) {
    case SelectionDragMode::ResizeLeft:
        next.setLeft(p.x());
        break;
    case SelectionDragMode::ResizeRight:
        next.setRight(p.x());
        break;
    case SelectionDragMode::ResizeTop:
        next.setTop(p.y());
        break;
    case SelectionDragMode::ResizeBottom:
        next.setBottom(p.y());
        break;
    case SelectionDragMode::ResizeTopLeft:
        next.setTopLeft(p);
        break;
    case SelectionDragMode::ResizeTopRight:
        next.setTopRight(p);
        break;
    case SelectionDragMode::ResizeBottomLeft:
        next.setBottomLeft(p);
        break;
    case SelectionDragMode::ResizeBottomRight:
        next.setBottomRight(p);
        break;
    case SelectionDragMode::Move:
    case SelectionDragMode::None:
        break;
    }
    return next.normalized().intersected(rect());
}

bool CaptureOverlayWindow::hasLongCaptureResizeFrontier(SelectionDragMode mode) const
{
    if (!isLongCaptureBrowseMode() || !selection_.isValid()
        || longCaptureCurrentSignature_.isEmpty()) {
        return false;
    }
    if (mode == SelectionDragMode::ResizeTop) {
        return longCaptureCurrentY_ == longCaptureOutputTopY_;
    }
    if (mode == SelectionDragMode::ResizeBottom) {
        return longCaptureCurrentY_ + selection_.height() == longCaptureOutputBottomY_;
    }
    return false;
}

bool CaptureOverlayWindow::canResizeLongCaptureEdge(SelectionDragMode mode) const
{
    if (mode_ != Mode::Ready || longCaptureCaptureBusy_
        || longCaptureWheelDispatchInFlight_ || longCaptureWheelDispatchScheduled_
        || !longCaptureWheelQueue_.isEmpty() || longCaptureLastFrameRejected_
        || !longCaptureViewportStable_) {
        return false;
    }
    return hasLongCaptureResizeFrontier(mode);
}

void CaptureOverlayWindow::prepareLongCaptureResizePreview()
{
    longCaptureResizePreviewBaseFrame_ = QImage();
    longCaptureResizePreviewFrame_ = QImage();
    longCaptureResizePreviewDocumentY_ = longCaptureCurrentY_;
    if (!longCaptureActive_ || !selectionDragStartRect_.isValid()
        || longCaptureCanvas_.isNull()
        || longCaptureDocBottomY_ <= longCaptureDocTopY_) {
        return;
    }

    QRect previewRect = selectionDragStartRect_.normalized().intersected(rect());
    if (selectionDragMode_ == SelectionDragMode::ResizeTop) {
        previewRect.setTop(rect().top());
    } else if (selectionDragMode_ == SelectionDragMode::ResizeBottom) {
        previewRect.setBottom(rect().bottom());
    } else {
        return;
    }

    int previewTopY = longCaptureCurrentY_
        + previewRect.top() - selectionDragStartRect_.top();
    const int requestedPreviewTopY = previewTopY;
    const int requestedPreviewBottomY = previewTopY + previewRect.height();
    const int canvasBottomY = longCaptureCanvasDocTopY_ + longCaptureCanvas_.height();
    const int committedTopY = qMax(requestedPreviewTopY,
                                   qMax(longCaptureDocTopY_,
                                        longCaptureCanvasDocTopY_));
    const int committedBottomY = qMin(requestedPreviewBottomY,
                                      qMin(longCaptureDocBottomY_,
                                           canvasBottomY));
    const int previewWidth = qMin(selectionDragStartRect_.width(),
                                  longCaptureCanvas_.width());
    const bool hasCommittedRows = committedBottomY > committedTopY
        && previewWidth > 0;
    const bool canvasCoversPreview = hasCommittedRows
        && committedTopY == requestedPreviewTopY
        && committedBottomY == requestedPreviewBottomY
        && previewWidth == previewRect.width();

    QImage resizeBase;
    QString source;
    if (canvasCoversPreview) {
        const QRect sourceRect(0,
                               requestedPreviewTopY - longCaptureCanvasDocTopY_,
                               previewWidth,
                               previewRect.height());
        resizeBase = longCaptureCanvas_.copy(sourceRect).convertToFormat(
            QImage::Format_ARGB32);
        source = QStringLiteral("canvas");
    } else {
        resizeBase = captureLongFrameImage(previewRect);
        if (!resizeBase.isNull() && resizeBase.size() != previewRect.size()) {
            resizeBase = resizeBase.scaled(previewRect.size(),
                                           Qt::IgnoreAspectRatio,
                                           Qt::SmoothTransformation);
        }
        if (!resizeBase.isNull()) {
            source = QStringLiteral("live+canvas");
            if (hasCommittedRows) {
                const int rows = committedBottomY - committedTopY;
                const int overlayWidth = qMin(resizeBase.width(), previewWidth);
                QPainter basePainter(&resizeBase);
                basePainter.setCompositionMode(QPainter::CompositionMode_Source);
                basePainter.drawImage(
                    QPoint(0, committedTopY - requestedPreviewTopY),
                    longCaptureCanvas_,
                    QRect(0,
                          committedTopY - longCaptureCanvasDocTopY_,
                          overlayWidth,
                          rows));
            }
        } else if (hasCommittedRows) {
            previewTopY = committedTopY;
            const QRect sourceRect(0,
                                   committedTopY - longCaptureCanvasDocTopY_,
                                   previewWidth,
                                   committedBottomY - committedTopY);
            resizeBase = longCaptureCanvas_.copy(sourceRect).convertToFormat(
                QImage::Format_ARGB32);
            source = QStringLiteral("canvas-limited");
            longCaptureStatus_ = QStringLiteral("实时预览采集失败，已限制可调整范围");
            Perf::log(QStringLiteral("LongCapture.resize.preview.capture_failed id=%1 requested=[%2,%3) committed=[%4,%5)")
                          .arg(overlayId_)
                          .arg(requestedPreviewTopY)
                          .arg(requestedPreviewBottomY)
                          .arg(committedTopY)
                          .arg(committedBottomY));
        }
    }

    if (resizeBase.isNull()) {
        Perf::log(QStringLiteral("LongCapture.resize.preview.unavailable id=%1 requested=[%2,%3) committed=[%4,%5)")
                      .arg(overlayId_)
                      .arg(requestedPreviewTopY)
                      .arg(requestedPreviewBottomY)
                      .arg(committedTopY)
                      .arg(committedBottomY));
        return;
    }

    longCaptureResizePreviewBaseFrame_ = resizeBase;
    longCaptureResizePreviewFrame_ = resizeBase.copy();
    if (longCaptureResizePreviewFrame_.isNull()) {
        longCaptureResizePreviewBaseFrame_ = QImage();
        return;
    }
    renderAnnotationDocumentToImage(longCaptureResizePreviewFrame_,
                                    resizeBase,
                                    longCaptureAnnotations_,
                                    false,
                                    QPointF(0, previewTopY));
    longCaptureResizePreviewDocumentY_ = previewTopY;
    Perf::log(QStringLiteral("LongCapture.resize.preview id=%1 edge=%2 source=%3 requested=[%4,%5) image=[%6,%7) size=%8x%9")
                  .arg(overlayId_)
                  .arg(selectionDragMode_ == SelectionDragMode::ResizeTop
                           ? QStringLiteral("top")
                           : QStringLiteral("bottom"))
                  .arg(source)
                  .arg(requestedPreviewTopY)
                  .arg(requestedPreviewBottomY)
                  .arg(previewTopY)
                  .arg(previewTopY + longCaptureResizePreviewFrame_.height())
                  .arg(longCaptureResizePreviewFrame_.width())
                  .arg(longCaptureResizePreviewFrame_.height()));
}

void CaptureOverlayWindow::finishLongCaptureViewportResize(const QRect& previousSelection)
{
    bool keepInputPaused = false;
    const auto inputGuard = qScopeGuard([&keepInputPaused]() {
        gLongCaptureHookInputPaused.store(keepInputPaused, std::memory_order_release);
    });
    const auto previewGuard = qScopeGuard([this]() {
        longCaptureResizePreviewBaseFrame_ = QImage();
        longCaptureResizePreviewFrame_ = QImage();
        longCaptureResizePreviewDocumentY_ = 0;
    });
    if (!longCaptureActive_ || !previousSelection.isValid() || !selection_.isValid()) {
        return;
    }

    QElapsedTimer transitionTimer;
    transitionTimer.start();
    const bool topChanged = selection_.top() != previousSelection.top();
    const bool bottomChanged = selection_.bottom() != previousSelection.bottom();
    const bool boundaryChanged = topChanged != bottomChanged;
    bool resizeCommitted = !boundaryChanged;
    if (topChanged == bottomChanged) {
        selection_ = previousSelection;
    } else {
        const int previousCurrentY = longCaptureCurrentY_;
        const int previousOutputTop = longCaptureOutputTopY_;
        const int previousOutputBottom = longCaptureOutputBottomY_;
        if (topChanged) {
            longCaptureCurrentY_ += selection_.top() - previousSelection.top();
            longCaptureOutputTopY_ = longCaptureCurrentY_;
        } else {
            longCaptureOutputBottomY_ = longCaptureCurrentY_ + selection_.height();
        }

        const QRect previewSourceRect(
            0,
            longCaptureCurrentY_ - longCaptureResizePreviewDocumentY_,
            selection_.width(),
            selection_.height());
        QImage image;
        if (!longCaptureResizePreviewBaseFrame_.isNull()
            && longCaptureResizePreviewBaseFrame_.rect().contains(previewSourceRect)) {
            image = longCaptureResizePreviewBaseFrame_.copy(previewSourceRect);
        } else {
            Perf::log(QStringLiteral("LongCapture.resize.commit.preview_unavailable id=%1 source=%2,%3 %4x%5 preview=%6x%7 docY=%8 currentY=%9")
                          .arg(overlayId_)
                          .arg(previewSourceRect.x())
                          .arg(previewSourceRect.y())
                          .arg(previewSourceRect.width())
                          .arg(previewSourceRect.height())
                          .arg(longCaptureResizePreviewBaseFrame_.width())
                          .arg(longCaptureResizePreviewBaseFrame_.height())
                          .arg(longCaptureResizePreviewDocumentY_)
                          .arg(longCaptureCurrentY_));
        }
        const LongCapture::RowSignature signature = LongCapture::computeRowSignature(image);
        if (image.isNull() || !signature.isValid()
            || !appendLongCaptureContent(image, signature, longCaptureCurrentY_)) {
            selection_ = previousSelection;
            longCaptureCurrentY_ = previousCurrentY;
            longCaptureOutputTopY_ = previousOutputTop;
            longCaptureOutputBottomY_ = previousOutputBottom;
            longCaptureStatus_ = QStringLiteral("调整长截图边界失败，已恢复原区域");
        } else {
            longCaptureVisibleFrame_ = image;
            longCaptureCurrentSignature_ = signature;
            longCaptureLastFrameRejected_ = false;
            longCaptureViewportStable_ = true;
            longCaptureStatus_ = QStringLiteral("已调整长截图输出边界");
            resizeCommitted = true;
        }
    }

    longCaptureLastPreviewRefreshAtMs_ = -1;
    if (boundaryChanged) {
        refreshLongCapturePreviewFit();
    }
    placeToolbar();
    const qint64 prepareMs = transitionTimer.elapsed();
    const bool viewportReady = refreshLongCaptureViewportLayer(
        LongCaptureViewportRefreshPolicy::CommitBeforeReveal);
    const bool directPageFallback = longCaptureViewportExclusionChecked_
        && !longCaptureViewportExclusionAvailable_
        && longCaptureAnnotations_.count() == 0
        && (!longCaptureViewportLayer_ || !longCaptureViewportLayer_->isVisible());
    const bool canRevealViewport = viewportReady || directPageFallback;
    const qint64 viewportMs = transitionTimer.elapsed() - prepareMs;
    if (canRevealViewport) {
        applyLongCaptureInputRegion();
    }
#ifdef Q_OS_WIN
    const bool inputReady = longCaptureInputRegionApplied_;
#else
    const bool inputReady = true;
#endif
    const bool transitionCommitted = canRevealViewport && inputReady;
    if (!transitionCommitted) {
        keepInputPaused = true;
        longCaptureViewportStable_ = false;
        longCaptureStatus_ = QStringLiteral("调整后的浏览画面提交失败，请重试");
    }
    mode_ = Mode::Ready;
    const qint64 regionMs = transitionTimer.elapsed() - prepareMs - viewportMs;
    Perf::log(QStringLiteral("LongCapture.resize.transition id=%1 changed=%2 resize=%3 viewport=%4 direct=%5 input=%6 prepare=%7ms viewportMs=%8ms region=%9ms total=%10ms")
                  .arg(overlayId_)
                  .arg(boundaryChanged)
                  .arg(resizeCommitted)
                  .arg(viewportReady)
                  .arg(directPageFallback)
                  .arg(inputReady)
                  .arg(prepareMs)
                  .arg(viewportMs)
                  .arg(regionMs)
                  .arg(transitionTimer.elapsed()));
    update(selection_.adjusted(-16, -16, 16, 16)
           | previousSelection.adjusted(-16, -16, 16, 16)
           | longCapturePreviewPanelRect().adjusted(-2, -2, 2, 2));
}

void CaptureOverlayWindow::updateSelectionHoverCursor(const QPoint& pos)
{
    switch (selectionDragModeAt(pos)) {
    case SelectionDragMode::Move:
        setCursor(Qt::SizeAllCursor);
        break;
    case SelectionDragMode::ResizeLeft:
    case SelectionDragMode::ResizeRight:
        setCursor(Qt::SizeHorCursor);
        break;
    case SelectionDragMode::ResizeTop:
    case SelectionDragMode::ResizeBottom:
        setCursor(Qt::SizeVerCursor);
        break;
    case SelectionDragMode::ResizeTopLeft:
    case SelectionDragMode::ResizeBottomRight:
        setCursor(Qt::SizeFDiagCursor);
        break;
    case SelectionDragMode::ResizeTopRight:
    case SelectionDragMode::ResizeBottomLeft:
        setCursor(Qt::SizeBDiagCursor);
        break;
    case SelectionDragMode::None:
        setCursor(captureCursor(captureSettings_.borderColor));
        break;
    }
}

bool CaptureOverlayWindow::isActiveShapeEditable() const
{
    if (!activeShapeEdit_.active || activeShapeEdit_.index < 0) {
        return false;
    }
    const AnnotationItem item = activeAnnotationDocument().itemAt(activeShapeEdit_.index);
    return isEditableRectAnnotationForTool(item, activeTool_, mosaicState_.paintMode);
}

void CaptureOverlayWindow::clearActiveShapeEdit()
{
    activeShapeEdit_ = ActiveShapeEditState{};
}

void CaptureOverlayWindow::activateShapeEditAt(int index)
{
    const AnnotationItem item = activeAnnotationDocument().itemAt(index);
    if (!isEditableRectAnnotationForTool(item, activeTool_, mosaicState_.paintMode)) {
        clearActiveShapeEdit();
        return;
    }
    activeShapeEdit_.active = true;
    activeShapeEdit_.index = index;
    activeShapeEdit_.dragMode = ShapeEditDragMode::None;
    syncShapeStateFromActiveShape();
}

void CaptureOverlayWindow::activateLatestShapeIfEditable()
{
    for (int i = activeAnnotationDocument().count() - 1; i >= 0; --i) {
        const AnnotationItem item = activeAnnotationDocument().itemAt(i);
        if (isEditableRectAnnotationForTool(item, activeTool_, mosaicState_.paintMode)) {
            activateShapeEditAt(i);
            return;
        }
    }
    clearActiveShapeEdit();
}

void CaptureOverlayWindow::syncShapeStateFromActiveShape()
{
    if (!activeShapeEdit_.active) {
        return;
    }
    const AnnotationItem item = activeAnnotationDocument().itemAt(activeShapeEdit_.index);
    if (isShapeAnnotation(item)) {
        shapeState_.kind = item.type == AnnotationType::Ellipse ? ShapeKind::Ellipse : ShapeKind::Rectangle;
        shapeState_.strokeWidth = item.style.strokeWidth;
        shapeState_.strokeColor = item.style.stroke;
        const int fillAlpha = item.style.fill.alpha();
        shapeState_.fillColor = fillAlpha > 0
            ? QColor(item.style.fill.red(), item.style.fill.green(), item.style.fill.blue())
            : item.style.stroke;
        shapeState_.fillAlpha = fillAlpha > 0 ? fillAlpha : 48;
        shapeState_.mode = fillAlpha > 0 ? ShapePaintMode::Filled : ShapePaintMode::StrokeOnly;
        return;
    }
    if (isFilledMosaicAnnotation(item)) {
        mosaicState_.paintMode = MosaicPaintMode::Fill;
        mosaicState_.effectMode = item.style.mosaicEffectMode;
        mosaicState_.strength = qBound(3, item.style.mosaicBlock, 24);
        currentStyle_.mosaicBlock = mosaicState_.strength;
        return;
    }
    clearActiveShapeEdit();
}

void CaptureOverlayWindow::applyShapeStateToActiveShape()
{
    if (!isActiveShapeEditable()) {
        return;
    }
    AnnotationItem item = activeAnnotationDocument().itemAt(activeShapeEdit_.index);
    if (!isShapeAnnotation(item)) {
        return;
    }
    item.type = shapeState_.kind == ShapeKind::Ellipse ? AnnotationType::Ellipse : AnnotationType::Rectangle;
    item.style.stroke = shapeState_.strokeColor;
    item.style.strokeWidth = shapeState_.strokeWidth;
    item.style.fill = shapeState_.mode == ShapePaintMode::Filled
        ? QColor(shapeState_.fillColor.red(), shapeState_.fillColor.green(), shapeState_.fillColor.blue(), shapeState_.fillAlpha)
        : QColor(shapeState_.fillColor.red(), shapeState_.fillColor.green(), shapeState_.fillColor.blue(), 0);
    activeAnnotationDocument().replaceAt(activeShapeEdit_.index, item);
    updateAnnotationViews();
}

CaptureOverlayWindow::ShapeEditDragMode CaptureOverlayWindow::shapeEditDragModeAt(const QPoint& pos) const
{
    if (!isActiveShapeEditable()) {
        return ShapeEditDragMode::None;
    }
    const AnnotationItem item = activeAnnotationDocument().itemAt(activeShapeEdit_.index);
    const QRectF r = item.rect.normalized();
    if (!r.isValid()) {
        return ShapeEditDragMode::None;
    }

    auto hitBox = [](const QPointF& p) {
        return QRectF(p.x() - 6.0, p.y() - 6.0, 12.0, 12.0);
    };
    const QPointF topMid(r.center().x(), r.top());
    const QPointF leftMid(r.left(), r.center().y());
    const QPointF rightMid(r.right(), r.center().y());
    const QPointF bottomMid(r.center().x(), r.bottom());
    const QPointF p(pos);

    if (hitBox(r.topLeft()).contains(p)) return ShapeEditDragMode::ResizeTopLeft;
    if (hitBox(r.topRight()).contains(p)) return ShapeEditDragMode::ResizeTopRight;
    if (hitBox(r.bottomLeft()).contains(p)) return ShapeEditDragMode::ResizeBottomLeft;
    if (hitBox(r.bottomRight()).contains(p)) return ShapeEditDragMode::ResizeBottomRight;
    if (hitBox(topMid).contains(p)) return ShapeEditDragMode::ResizeTop;
    if (hitBox(bottomMid).contains(p)) return ShapeEditDragMode::ResizeBottom;
    if (hitBox(leftMid).contains(p)) return ShapeEditDragMode::ResizeLeft;
    if (hitBox(rightMid).contains(p)) return ShapeEditDragMode::ResizeRight;

    if (isFilledMosaicAnnotation(item) && r.contains(p)) {
        return ShapeEditDragMode::Move;
    }

    QPainterPath path;
    if (item.type == AnnotationType::Ellipse) {
        path.addEllipse(r);
    } else {
        path.addRect(r);
    }
    QPainterPathStroker stroker;
    stroker.setCapStyle(Qt::RoundCap);
    stroker.setJoinStyle(Qt::RoundJoin);
    stroker.setWidth(qMax(8.0, static_cast<double>(item.style.strokeWidth) + 6.0));
    if (stroker.createStroke(path).contains(p)) {
        return ShapeEditDragMode::Move;
    }
    return ShapeEditDragMode::None;
}

QRectF CaptureOverlayWindow::resizedActiveShapeRect(const QPoint& pos) const
{
    QRectF next = activeShapeEdit_.dragStartRect.normalized();
    const QRectF bounds = activeAnnotationBounds();
    const QPointF p(qBound(bounds.left(), static_cast<qreal>(pos.x()), bounds.right()),
                    qBound(bounds.top(), static_cast<qreal>(pos.y()), bounds.bottom()));
    const QPointF delta = QPointF(pos - activeShapeEdit_.dragStart);

    if (activeShapeEdit_.dragMode == ShapeEditDragMode::Move) {
        next.translate(delta);
        if (next.left() < bounds.left()) next.moveLeft(bounds.left());
        if (next.top() < bounds.top()) next.moveTop(bounds.top());
        if (next.right() > bounds.right()) next.moveRight(bounds.right());
        if (next.bottom() > bounds.bottom()) next.moveBottom(bounds.bottom());
        return next;
    }

    switch (activeShapeEdit_.dragMode) {
    case ShapeEditDragMode::ResizeLeft:
        next.setLeft(p.x());
        break;
    case ShapeEditDragMode::ResizeRight:
        next.setRight(p.x());
        break;
    case ShapeEditDragMode::ResizeTop:
        next.setTop(p.y());
        break;
    case ShapeEditDragMode::ResizeBottom:
        next.setBottom(p.y());
        break;
    case ShapeEditDragMode::ResizeTopLeft:
        next.setTopLeft(p);
        break;
    case ShapeEditDragMode::ResizeTopRight:
        next.setTopRight(p);
        break;
    case ShapeEditDragMode::ResizeBottomLeft:
        next.setBottomLeft(p);
        break;
    case ShapeEditDragMode::ResizeBottomRight:
        next.setBottomRight(p);
        break;
    case ShapeEditDragMode::Move:
    case ShapeEditDragMode::None:
        break;
    }
    next = next.normalized().intersected(bounds);
    if (next.width() < 4.0 || next.height() < 4.0) {
        return activeShapeEdit_.dragStartRect.normalized();
    }
    return next;
}

void CaptureOverlayWindow::updateShapeEditHoverCursor(const QPoint& pos)
{
    switch (shapeEditDragModeAt(pos)) {
    case ShapeEditDragMode::Move:
        setCursor(Qt::SizeAllCursor);
        break;
    case ShapeEditDragMode::ResizeLeft:
    case ShapeEditDragMode::ResizeRight:
        setCursor(Qt::SizeHorCursor);
        break;
    case ShapeEditDragMode::ResizeTop:
    case ShapeEditDragMode::ResizeBottom:
        setCursor(Qt::SizeVerCursor);
        break;
    case ShapeEditDragMode::ResizeTopLeft:
    case ShapeEditDragMode::ResizeBottomRight:
        setCursor(Qt::SizeFDiagCursor);
        break;
    case ShapeEditDragMode::ResizeTopRight:
    case ShapeEditDragMode::ResizeBottomLeft:
        setCursor(Qt::SizeBDiagCursor);
        break;
    case ShapeEditDragMode::None:
        syncCursorForTool();
        break;
    }
}

void CaptureOverlayWindow::beginShapeEditDrag(ShapeEditDragMode dragMode, const QPoint& pos)
{
    if (!isActiveShapeEditable()) {
        return;
    }
    activeShapeEdit_.dragMode = dragMode;
    activeShapeEdit_.dragStart = pos;
    activeShapeEdit_.dragStartRect = activeAnnotationDocument().itemAt(activeShapeEdit_.index).rect.normalized();
    mode_ = Mode::EditingShape;
}

void CaptureOverlayWindow::updateShapeEditDrag(const QPoint& pos)
{
    if (!isActiveShapeEditable() || activeShapeEdit_.dragMode == ShapeEditDragMode::None) {
        return;
    }
    AnnotationItem item = activeAnnotationDocument().itemAt(activeShapeEdit_.index);
    item.rect = resizedActiveShapeRect(pos);
    activeAnnotationDocument().replaceAt(activeShapeEdit_.index, item);
}

void CaptureOverlayWindow::finishShapeEditDrag()
{
    activeShapeEdit_.dragMode = ShapeEditDragMode::None;
    mode_ = Mode::Ready;
    placeToolbar();
    updateAnnotationViews();
}

bool CaptureOverlayWindow::isActiveArrowEditable() const
{
    if (!activeArrowEdit_.active || activeArrowEdit_.index < 0 || activeTool_ != QStringLiteral("tool-arrow")) {
        return false;
    }
    const AnnotationItem item = activeAnnotationDocument().itemAt(activeArrowEdit_.index);
    return item.type == AnnotationType::Arrow && item.points.size() >= 2;
}

void CaptureOverlayWindow::clearActiveArrowEdit()
{
    activeArrowEdit_ = ActiveArrowEditState{};
}

void CaptureOverlayWindow::activateArrowEditAt(int index)
{
    const AnnotationItem item = activeAnnotationDocument().itemAt(index);
    if (item.type != AnnotationType::Arrow || item.points.size() < 2) {
        clearActiveArrowEdit();
        return;
    }
    activeArrowEdit_.active = true;
    activeArrowEdit_.index = index;
    activeArrowEdit_.dragMode = ArrowEditDragMode::None;
    syncArrowStateFromActiveArrow();
}

void CaptureOverlayWindow::activateLatestArrowIfEditable()
{
    for (int i = activeAnnotationDocument().count() - 1; i >= 0; --i) {
        const AnnotationItem item = activeAnnotationDocument().itemAt(i);
        if (item.type == AnnotationType::Arrow && item.points.size() >= 2) {
            activateArrowEditAt(i);
            return;
        }
    }
    clearActiveArrowEdit();
}

void CaptureOverlayWindow::syncArrowStateFromActiveArrow()
{
    if (!activeArrowEdit_.active) {
        return;
    }
    const AnnotationItem item = activeAnnotationDocument().itemAt(activeArrowEdit_.index);
    if (item.type != AnnotationType::Arrow || item.points.size() < 2) {
        clearActiveArrowEdit();
        return;
    }
    arrowState_.strokeWidth = item.style.strokeWidth;
    arrowState_.strokeColor = item.style.stroke;
    arrowState_.headMode = item.style.arrowHeadMode;
}

void CaptureOverlayWindow::applyArrowStateToActiveArrow()
{
    if (!isActiveArrowEditable()) {
        return;
    }
    AnnotationItem item = activeAnnotationDocument().itemAt(activeArrowEdit_.index);
    item.style.stroke = arrowState_.strokeColor;
    item.style.strokeWidth = arrowState_.strokeWidth;
    item.style.arrowHeadMode = arrowState_.headMode;
    activeAnnotationDocument().replaceAt(activeArrowEdit_.index, item);
    updateAnnotationViews();
}

CaptureOverlayWindow::ArrowEditDragMode CaptureOverlayWindow::arrowEditDragModeAt(const QPoint& pos) const
{
    if (!isActiveArrowEditable()) {
        return ArrowEditDragMode::None;
    }
    const AnnotationItem item = activeAnnotationDocument().itemAt(activeArrowEdit_.index);
    const QPointF start = item.points.first();
    const QPointF end = item.points.last();
    const QPointF p(pos);

    auto handleRect = [](const QPointF& point) {
        return QRectF(point.x() - 7.0, point.y() - 7.0, 14.0, 14.0);
    };
    if (handleRect(start).contains(p)) {
        return ArrowEditDragMode::ResizeStart;
    }
    if (handleRect(end).contains(p)) {
        return ArrowEditDragMode::ResizeEnd;
    }

    QPainterPath path;
    path.moveTo(start);
    path.lineTo(end);
    QPainterPathStroker stroker;
    stroker.setCapStyle(Qt::RoundCap);
    stroker.setJoinStyle(Qt::RoundJoin);
    stroker.setWidth(qMax(8.0, static_cast<double>(item.style.strokeWidth) + 6.0));
    if (stroker.createStroke(path).contains(p)) {
        return ArrowEditDragMode::Move;
    }
    return ArrowEditDragMode::None;
}

void CaptureOverlayWindow::updateArrowEditHoverCursor(const QPoint& pos)
{
    switch (arrowEditDragModeAt(pos)) {
    case ArrowEditDragMode::Move:
        setCursor(Qt::SizeAllCursor);
        break;
    case ArrowEditDragMode::ResizeStart:
    case ArrowEditDragMode::ResizeEnd:
        setCursor(Qt::CrossCursor);
        break;
    case ArrowEditDragMode::None:
        syncCursorForTool();
        break;
    }
}

void CaptureOverlayWindow::beginArrowEditDrag(ArrowEditDragMode dragMode, const QPoint& pos)
{
    if (!isActiveArrowEditable()) {
        return;
    }
    activeArrowEdit_.dragMode = dragMode;
    activeArrowEdit_.dragStart = pos;
    activeArrowEdit_.dragStartPoints = activeAnnotationDocument().itemAt(activeArrowEdit_.index).points;
    mode_ = Mode::EditingArrow;
}

void CaptureOverlayWindow::updateArrowEditDrag(const QPoint& pos)
{
    if (!isActiveArrowEditable() || activeArrowEdit_.dragMode == ArrowEditDragMode::None
        || activeArrowEdit_.dragStartPoints.size() < 2) {
        return;
    }

    const QRectF bounds = activeAnnotationBounds();
    auto clampPoint = [&bounds](QPointF point) {
        point.setX(qBound(bounds.left(), point.x(), bounds.right()));
        point.setY(qBound(bounds.top(), point.y(), bounds.bottom()));
        return point;
    };

    QVector<QPointF> nextPoints = activeArrowEdit_.dragStartPoints;
    const QPointF delta = QPointF(pos - activeArrowEdit_.dragStart);
    if (activeArrowEdit_.dragMode == ArrowEditDragMode::Move) {
        QPointF start = nextPoints.first() + delta;
        QPointF end = nextPoints.last() + delta;
        const qreal shiftLeft = qMin(0.0, qMin(start.x() - bounds.left(), end.x() - bounds.left()));
        const qreal shiftTop = qMin(0.0, qMin(start.y() - bounds.top(), end.y() - bounds.top()));
        const qreal shiftRight = qMax(0.0, qMax(start.x() - bounds.right(), end.x() - bounds.right()));
        const qreal shiftBottom = qMax(0.0, qMax(start.y() - bounds.bottom(), end.y() - bounds.bottom()));
        start -= QPointF(shiftLeft + shiftRight, shiftTop + shiftBottom);
        end -= QPointF(shiftLeft + shiftRight, shiftTop + shiftBottom);
        nextPoints.first() = start;
        nextPoints.last() = end;
    } else if (activeArrowEdit_.dragMode == ArrowEditDragMode::ResizeStart) {
        nextPoints.first() = clampPoint(QPointF(pos));
    } else if (activeArrowEdit_.dragMode == ArrowEditDragMode::ResizeEnd) {
        nextPoints.last() = clampPoint(QPointF(pos));
    }

    if (QLineF(nextPoints.first(), nextPoints.last()).length() < 2.0) {
        return;
    }
    AnnotationItem item = activeAnnotationDocument().itemAt(activeArrowEdit_.index);
    item.points = nextPoints;
    activeAnnotationDocument().replaceAt(activeArrowEdit_.index, item);
}

void CaptureOverlayWindow::finishArrowEditDrag()
{
    activeArrowEdit_.dragMode = ArrowEditDragMode::None;
    mode_ = Mode::Ready;
    placeToolbar();
    updateAnnotationViews();
}

void CaptureOverlayWindow::applyTextStyleToInlineEditor()
{
    if (!inlineTextBox_) {
        return;
    }
    auto* box = static_cast<InlineTextBox*>(inlineTextBox_.data());
    box->applyStyle(currentStyle_);
    box->fitToContent();
}

void CaptureOverlayWindow::resizeInlineTextEditorToDocument()
{
    if (!inlineTextBox_ || !selection_.isValid()) {
        return;
    }
    auto* box = static_cast<InlineTextBox*>(inlineTextBox_.data());
    box->fitToContent();
}

void CaptureOverlayWindow::beginInlineTextEdit(const QPoint& pos)
{
    commitInlineTextEdit();
    auto* box = new InlineTextBox(this);
    inlineTextBox_ = box;
    const QRect editBounds = activeAnnotationBounds().toAlignedRect();
    box->setMoveBounds(editBounds);
    box->commitRequested = [this]() { commitInlineTextEdit(); };
    box->cancelRequested = [this]() {
        commitInlineTextEdit();
        if (isLongCaptureAnnotationMode()) {
            leaveLongCaptureAnnotationMode(true);
        }
    };
    applyTextStyleToInlineEditor();
    const QPoint clampedCursor(qBound(editBounds.left(), pos.x(), editBounds.right()),
                               qBound(editBounds.top(), pos.y(), editBounds.bottom()));
    const QPoint topLeft = clampedCursor - box->textCursorOriginOffset();
    box->setGeometry(QRect(topLeft, QSize(180, 48)).intersected(editBounds));
    box->textChanged = [this]() { resizeInlineTextEditorToDocument(); };
    box->show();
    box->raise();
    box->setFocus(Qt::MouseFocusReason);
    box->grabKeyboard();
}

void CaptureOverlayWindow::commitInlineTextEdit()
{
    if (!inlineTextBox_) {
        return;
    }
    if (QGuiApplication::inputMethod()) {
        QGuiApplication::inputMethod()->commit();
        QApplication::processEvents(QEventLoop::ExcludeUserInputEvents);
    }
    auto* box = static_cast<InlineTextBox*>(inlineTextBox_.data());
    const QString text = box->text();
    const QRect editorRect = box->contentRectInParent();
    box->releaseKeyboard();
    box->hide();
    box->deleteLater();
    inlineTextBox_ = nullptr;

    if (!text.trimmed().isEmpty() && editorRect.isValid()) {
        AnnotationItem item;
        item.type = AnnotationType::Text;
        item.rect = editorRect;
        item.text = text;
        item.style = currentStyle_;
        item.style.text = currentStyle_.text;
        activeAnnotationDocument().add(item);
        if (toolbar_) {
            toolbar_->setUndoAvailable(activeAnnotationDocument().canUndo());
            toolbar_->setRedoAvailable(activeAnnotationDocument().canRedo());
        }
    }
    mode_ = Mode::Ready;
    if (isVisible()) {
        setFocus(Qt::OtherFocusReason);
        grabKeyboard();
    }
    updateAnnotationViews();
}

void CaptureOverlayWindow::cancelInlineTextEdit()
{
    if (!inlineTextBox_) {
        return;
    }
    QWidget* box = inlineTextBox_.data();
    box->releaseKeyboard();
    box->hide();
    box->deleteLater();
    inlineTextBox_ = nullptr;
    mode_ = Mode::Ready;
    if (isVisible()) {
        setFocus(Qt::OtherFocusReason);
        grabKeyboard();
    }
    update();
}

void CaptureOverlayWindow::startAnnotation(const QPoint& pos)
{
    if (!canStartAnnotation(pos)) {
        return;
    }
    clearActiveShapeEdit();
    clearActiveArrowEdit();
    currentAnnotation_ = AnnotationItem{};
    if (activeTool_ == QStringLiteral("tool-rect")) {
        currentAnnotation_.type = shapeState_.kind == ShapeKind::Ellipse ? AnnotationType::Ellipse : AnnotationType::Rectangle;
        currentAnnotation_.style.stroke = shapeState_.strokeColor;
        currentAnnotation_.style.strokeWidth = shapeState_.strokeWidth;
        currentAnnotation_.style.fill = shapeState_.mode == ShapePaintMode::Filled
            ? QColor(shapeState_.fillColor.red(), shapeState_.fillColor.green(), shapeState_.fillColor.blue(), shapeState_.fillAlpha)
            : QColor(shapeState_.fillColor.red(), shapeState_.fillColor.green(), shapeState_.fillColor.blue(), 0);
    } else if (activeTool_ == QStringLiteral("tool-arrow")) {
        currentAnnotation_.type = AnnotationType::Arrow;
        currentAnnotation_.style.stroke = arrowState_.strokeColor;
        currentAnnotation_.style.strokeWidth = arrowState_.strokeWidth;
        currentAnnotation_.style.arrowHeadMode = arrowState_.headMode;
    } else if (activeTool_ == QStringLiteral("tool-mosaic")) {
        currentAnnotation_.type = AnnotationType::Mosaic;
        currentAnnotation_.style = currentStyle_;
        currentAnnotation_.style.stroke = captureSettings_.borderColor;
        currentAnnotation_.style.mosaicBlock = mosaicState_.strength;
        currentAnnotation_.style.mosaicPaintMode = mosaicState_.paintMode;
        currentAnnotation_.style.mosaicEffectMode = mosaicState_.effectMode;
    } else if (activeTool_ == QStringLiteral("tool-rubber")) {
        currentAnnotation_.type = AnnotationType::Eraser;
        currentAnnotation_.style = currentStyle_;
        currentAnnotation_.style.strokeWidth = eraserState_.size;
        currentAnnotation_.style.mosaicPaintMode = eraserState_.paintMode;
    } else {
        currentAnnotation_.type = typeFromToolId(activeTool_);
        currentAnnotation_.style = currentStyle_;
        if (currentAnnotation_.type == AnnotationType::Text) {
            currentAnnotation_.style.text = currentStyle_.text;
        }
    }

    if (currentAnnotation_.type == AnnotationType::Text) {
        beginInlineTextEdit(pos);
        return;
    }

    if (currentAnnotation_.type == AnnotationType::Eraser) {
        currentAnnotation_.points = {pos};
        currentAnnotation_.rect = QRectF(pos, QSizeF(1, 1));
        hasCurrentAnnotation_ = true;
        eraserFillPending_ = false;
        eraserRectDragMode_ = EraserRectDragMode::None;
        mode_ = Mode::DrawingAnnotation;
        return;
    }

    if (currentAnnotation_.type == AnnotationType::Number) {
        currentAnnotation_.number = activeAnnotationDocument().nextNumber();
        currentAnnotation_.points = {pos};
        currentAnnotation_.rect = QRect(pos - QPoint(10, 10), QSize(20, 20));
        activeAnnotationDocument().add(currentAnnotation_);
        updateAnnotationViews();
        return;
    }

    if (currentAnnotation_.type == AnnotationType::Mosaic && currentAnnotation_.style.mosaicPaintMode == MosaicPaintMode::Brush) {
        currentAnnotation_.points = {pos};
        currentAnnotation_.rect = QRectF(pos, QSizeF(1, 1));
        hasCurrentAnnotation_ = true;
        mode_ = Mode::DrawingAnnotation;
        return;
    }

    currentAnnotation_.rect = QRect(pos, QSize(1, 1));
    currentAnnotation_.points = {pos};
    hasCurrentAnnotation_ = true;
    mode_ = Mode::DrawingAnnotation;
}

void CaptureOverlayWindow::updateAnnotation(const QPoint& pos)
{
    if (!hasCurrentAnnotation_) {
        return;
    }
    const QRect bounds = activeAnnotationBounds().toAlignedRect();
    const QPoint clamped(qBound(bounds.left(), pos.x(), bounds.right()),
                         qBound(bounds.top(), pos.y(), bounds.bottom()));
    if (currentAnnotation_.type == AnnotationType::Pen
        || (currentAnnotation_.type == AnnotationType::Mosaic && currentAnnotation_.style.mosaicPaintMode == MosaicPaintMode::Brush)
        || (currentAnnotation_.type == AnnotationType::Eraser && currentAnnotation_.style.mosaicPaintMode == MosaicPaintMode::Brush)) {
        currentAnnotation_.points.append(clamped);
    } else if (currentAnnotation_.type == AnnotationType::Arrow) {
        if (currentAnnotation_.points.size() < 2) {
            currentAnnotation_.points.append(clamped);
        } else {
            currentAnnotation_.points.last() = clamped;
        }
    } else {
        currentAnnotation_.rect = QRect(currentAnnotation_.points.first().toPoint(), clamped).normalized();
    }
}

void CaptureOverlayWindow::finishAnnotation(const QPoint& pos)
{
    updateAnnotation(pos);
    if (hasCurrentAnnotation_ && currentAnnotation_.type == AnnotationType::Eraser
        && currentAnnotation_.style.mosaicPaintMode == MosaicPaintMode::Fill) {
        currentAnnotation_.rect = clampedEraserRect(currentAnnotation_.rect);
        if (currentAnnotation_.isValid()) {
            eraserFillPending_ = true;
            mode_ = Mode::Ready;
            placeToolbar();
            update();
            return;
        }
        hasCurrentAnnotation_ = false;
        eraserFillPending_ = false;
        mode_ = Mode::Ready;
        update();
        return;
    }
    const bool finishedShape = hasCurrentAnnotation_
        && (currentAnnotation_.type == AnnotationType::Rectangle || currentAnnotation_.type == AnnotationType::Ellipse);
    const bool finishedMosaicRegion = hasCurrentAnnotation_
        && currentAnnotation_.type == AnnotationType::Mosaic
        && currentAnnotation_.style.mosaicPaintMode == MosaicPaintMode::Fill;
    const bool finishedArrow = hasCurrentAnnotation_ && currentAnnotation_.type == AnnotationType::Arrow;
    const int itemIndex = activeAnnotationDocument().count();
    if (hasCurrentAnnotation_) {
        activeAnnotationDocument().add(currentAnnotation_);
    }
    if ((finishedShape || finishedMosaicRegion) && activeAnnotationDocument().count() > itemIndex) {
        activateShapeEditAt(itemIndex);
        clearActiveArrowEdit();
    } else if (finishedArrow && activeAnnotationDocument().count() > itemIndex) {
        clearActiveShapeEdit();
        activateArrowEditAt(itemIndex);
    } else if (hasCurrentAnnotation_) {
        clearActiveShapeEdit();
        clearActiveArrowEdit();
    }
    hasCurrentAnnotation_ = false;
    mode_ = Mode::Ready;
    toolbar_->setUndoAvailable(activeAnnotationDocument().canUndo());
    toolbar_->setRedoAvailable(activeAnnotationDocument().canRedo());
    placeToolbar();
    updateAnnotationViews();
}

QImage CaptureOverlayWindow::renderResult() const
{
    if (longCaptureActive_) {
        const QImage longResult = longCaptureResultImage();
        if (!longResult.isNull()) {
            Perf::log(QStringLiteral("LongCapture.output.renderResult id=%1 mode=long image=%2x%3 currentY=%4 doc=[%5,%6)")
                          .arg(overlayId_)
                          .arg(longResult.width())
                          .arg(longResult.height())
                          .arg(longCaptureCurrentY_)
                          .arg(longCaptureDocTopY_)
                          .arg(longCaptureDocBottomY_));
        }
        return longResult;
    }
    if (!selection_.isValid()) {
        return {};
    }
    const QImage base = (fastTranslateShown() ? fastTranslatedImage_ : desktopImage_.copy(selection_))
                            .convertToFormat(QImage::Format_ARGB32);
    QImage result = base.copy();
    renderAnnotationsToImage(result, base, false, selection_.topLeft());
    return result;
}

void CaptureOverlayWindow::finalizeLongCaptureOutput()
{
    if (!longCaptureActive_) {
        return;
    }
    if (longCaptureAnnotationMode_) {
        return;
    }
    if (longCaptureAnnotationModePending_) {
        cancelPendingLongCaptureAnnotationMode();
    }
    longCaptureProgressTimer_.stop();
    longCaptureWheelQueue_.clear();
    longCaptureWheelDispatchScheduled_ = false;
    cancelLongCaptureFrameRequest();
    processLongCaptureFrame();
    longCaptureWheelDispatchInFlight_ = false;
    longCaptureDispatchMovementObserved_ = false;
}

QImage CaptureOverlayWindow::prepareOutputResult()
{
    finalizeLongCaptureOutput();
    QImage result = renderResult();
    if (longCaptureActive_ && result.isNull()) {
        if (!longCaptureCanvas_.isNull() && longCaptureDocBottomY_ > longCaptureDocTopY_) {
            longCaptureStatus_ = QStringLiteral("长截图结果内存分配失败");
        } else if (longCaptureStatus_.isEmpty()) {
            longCaptureStatus_ = QStringLiteral("没有可输出的长截图内容");
        }
        update(longCapturePreviewPanelRect().adjusted(-2, -2, 2, 2));
    }
    return result;
}

void CaptureOverlayWindow::requestCopy()
{
    commitPendingEraserFill();
    commitInlineTextEdit();
    const QImage image = prepareOutputResult();
    Perf::log(QStringLiteral("LongCapture.output.copy id=%1 active=%2 imageNull=%3 image=%4x%5")
                  .arg(overlayId_)
                  .arg(longCaptureActive_)
                  .arg(image.isNull())
                  .arg(image.width())
                  .arg(image.height()));
    if (!image.isNull()) {
        emit copyRequested(image);
        emit finished(selection_.translated(virtualGeometry_.topLeft()));
        if (copyThenExit_) {
            Perf::log(QStringLiteral("CaptureOverlayWindow.closeRequested id=%1 reason=copy_then_exit").arg(overlayId_));
            close();
        }
    }
}

void CaptureOverlayWindow::requestSave()
{
    commitPendingEraserFill();
    commitInlineTextEdit();
    const QImage image = prepareOutputResult();
    Perf::log(QStringLiteral("LongCapture.output.save id=%1 active=%2 imageNull=%3 image=%4x%5")
                  .arg(overlayId_)
                  .arg(longCaptureActive_)
                  .arg(image.isNull())
                  .arg(image.width())
                  .arg(image.height()));
    if (!image.isNull()) {
        emit finished(selection_.translated(virtualGeometry_.topLeft()));
        emit saveRequested(image);
    }
}

void CaptureOverlayWindow::requestPin()
{
    commitPendingEraserFill();
    commitInlineTextEdit();
    const QImage image = prepareOutputResult();
    setUpdatesEnabled(false);
    Perf::log(QStringLiteral("LongCapture.output.pin id=%1 active=%2 imageNull=%3 image=%4x%5 anchor=%6,%7")
                  .arg(overlayId_)
                  .arg(longCaptureActive_)
                  .arg(image.isNull())
                  .arg(image.width())
                  .arg(image.height())
                  .arg(longCaptureAnchorGlobal_.x())
                  .arg(longCaptureAnchorGlobal_.y()));
    if (!image.isNull()) {
        const bool longResult = longCaptureActive_
            && longCaptureOutputBottomY_ - longCaptureOutputTopY_ > 0
            && image.height() > selection_.height();
        const QRect sourceRect = longResult
            ? QRect(selection_.translated(virtualGeometry_.topLeft()).topLeft(), image.size())
            : selection_.translated(virtualGeometry_.topLeft());
        emit pinRequested(image, sourceRect);
        emit finished(selection_.translated(virtualGeometry_.topLeft()));
        QTimer::singleShot(kPinHandoffCloseDelayMs, this, [this]() {
            Perf::log(QStringLiteral("CaptureOverlayWindow.closeRequested id=%1 reason=pin_delay").arg(overlayId_));
            close();
        });
        return;
    }

    setUpdatesEnabled(true);
}

void CaptureOverlayWindow::requestQuestion()
{
    if (!selection_.isValid() || longCaptureActive_) {
        return;
    }
    commitPendingEraserFill();
    commitInlineTextEdit();
    // The panel re-grabs the live screen on every request, so annotations
    // never reach the model; the preview shows the plain selection.
    const QRect globalSelection = selection_.translated(virtualGeometry_.topLeft());
    Perf::log(QStringLiteral("CaptureOverlayWindow.questionRequested id=%1 selection=%2,%3 %4x%5")
                  .arg(overlayId_)
                  .arg(globalSelection.x())
                  .arg(globalSelection.y())
                  .arg(globalSelection.width())
                  .arg(globalSelection.height()));
    emit questionRequested(globalSelection, desktopImage_.copy(selection_));
    emit finished(globalSelection);
    close();
}

void CaptureOverlayWindow::commitPendingEraserFill()
{
    if (!eraserFillPending_ || !hasCurrentAnnotation_ || currentAnnotation_.type != AnnotationType::Eraser) {
        return;
    }

    currentAnnotation_.rect = clampedEraserRect(currentAnnotation_.rect);
    if (currentAnnotation_.isValid()) {
        activeAnnotationDocument().add(currentAnnotation_);
    }
    hasCurrentAnnotation_ = false;
    eraserFillPending_ = false;
    eraserRectDragMode_ = EraserRectDragMode::None;
    mode_ = Mode::Ready;
    if (toolbar_) {
        toolbar_->setUndoAvailable(activeAnnotationDocument().canUndo());
        toolbar_->setRedoAvailable(activeAnnotationDocument().canRedo());
    }
    updateAnnotationViews();
}

void CaptureOverlayWindow::cancelPendingEraserFill()
{
    if (!eraserFillPending_) {
        return;
    }
    hasCurrentAnnotation_ = false;
    eraserFillPending_ = false;
    eraserRectDragMode_ = EraserRectDragMode::None;
    mode_ = Mode::Ready;
    update();
}

CaptureOverlayWindow::EraserRectDragMode CaptureOverlayWindow::eraserRectDragModeAt(const QPoint& pos) const
{
    if (!eraserFillPending_ || !hasCurrentAnnotation_ || currentAnnotation_.type != AnnotationType::Eraser) {
        return EraserRectDragMode::None;
    }

    const QRectF r = currentAnnotation_.rect.normalized();
    const QVector<QPair<QPointF, EraserRectDragMode>> handles = {
        {r.topLeft(), EraserRectDragMode::ResizeTopLeft},
        {r.topRight(), EraserRectDragMode::ResizeTopRight},
        {r.bottomLeft(), EraserRectDragMode::ResizeBottomLeft},
        {r.bottomRight(), EraserRectDragMode::ResizeBottomRight},
    };
    for (const auto& handle : handles) {
        const QRectF hit(handle.first.x() - 7, handle.first.y() - 7, 14, 14);
        if (hit.contains(pos)) {
            return handle.second;
        }
    }
    if (r.contains(pos)) {
        return EraserRectDragMode::Move;
    }
    return EraserRectDragMode::None;
}

QRectF CaptureOverlayWindow::clampedEraserRect(QRectF rect) const
{
    const QRectF bounds = activeAnnotationBounds();
    rect = rect.normalized();
    if (rect.width() > bounds.width()) {
        rect.setWidth(bounds.width());
    }
    if (rect.height() > bounds.height()) {
        rect.setHeight(bounds.height());
    }
    if (rect.left() < bounds.left()) {
        rect.moveLeft(bounds.left());
    }
    if (rect.top() < bounds.top()) {
        rect.moveTop(bounds.top());
    }
    if (rect.right() > bounds.right()) {
        rect.moveRight(bounds.right());
    }
    if (rect.bottom() > bounds.bottom()) {
        rect.moveBottom(bounds.bottom());
    }
    return rect.intersected(bounds).normalized();
}

} // namespace Visnip
