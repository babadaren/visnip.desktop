#include "app/AppController.h"
#include "core/AnnotationModel.h"
#include "core/DesignTokens.h"
#include "core/PerfLog.h"

#include <QApplication>
#include <QCommandLineParser>
#include <QImage>
#include <QPainter>
#include <iostream>

#ifdef Q_OS_WIN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

namespace {
#ifdef Q_OS_WIN
// Older MinGW headers may lack the power-throttling declarations.
#ifndef PROCESS_POWER_THROTTLING_CURRENT_VERSION
typedef struct _PROCESS_POWER_THROTTLING_STATE {
    ULONG Version;
    ULONG ControlMask;
    ULONG StateMask;
} PROCESS_POWER_THROTTLING_STATE;
#define PROCESS_POWER_THROTTLING_CURRENT_VERSION 1
#define PROCESS_POWER_THROTTLING_EXECUTION_SPEED 0x1
#endif

// While idle in the tray the process gets power-throttled (EcoQoS): measured
// as pending timers firing seconds late, only woken by the hotkey message
// itself, which shows up as a first-F1 lag. Opt out so the hotkey wakes a
// normally-scheduled process.
void disableBackgroundPowerThrottling()
{
    PROCESS_POWER_THROTTLING_STATE state{};
    state.Version = PROCESS_POWER_THROTTLING_CURRENT_VERSION;
    state.ControlMask = PROCESS_POWER_THROTTLING_EXECUTION_SPEED;
    state.StateMask = 0;
    const BOOL ok = SetProcessInformation(GetCurrentProcess(),
                                          static_cast<PROCESS_INFORMATION_CLASS>(4) /* ProcessPowerThrottling */,
                                          &state,
                                          sizeof(state));
    Visnip::Perf::log(QStringLiteral("main.power_throttling optOut=%1 error=%2")
                           .arg(ok ? 1 : 0)
                           .arg(ok ? 0 : static_cast<int>(GetLastError())));
}
#endif

int runSelfTest()
{
    QImage image(120, 80, QImage::Format_ARGB32);
    image.fill(Qt::white);
    Visnip::AnnotationItem rect;
    rect.type = Visnip::AnnotationType::Rectangle;
    rect.rect = QRectF(10, 10, 60, 30);
    rect.style.stroke = Visnip::Design::colors().primary;
    QPainter painter(&image);
    Visnip::drawAnnotation(painter, rect);
    painter.end();
    Visnip::applyMosaic(image, QRect(20, 20, 30, 20), 6);
    std::cout << "Visnip self-test OK" << std::endl;
    return 0;
}
} // namespace

int main(int argc, char* argv[])
{
    QApplication app(argc, argv);
    QApplication::setApplicationName(QStringLiteral("Visnip"));
    QApplication::setApplicationVersion(QStringLiteral(VISNIP_VERSION));
    QApplication::setOrganizationName(QStringLiteral("Visnip"));
    Visnip::Perf::initialize();
    Visnip::Perf::log(QStringLiteral("main.qapplication.ready version=%1").arg(QStringLiteral(VISNIP_VERSION)));
#ifdef Q_OS_WIN
    disableBackgroundPowerThrottling();
#endif

    QCommandLineParser parser;
    parser.setApplicationDescription(QStringLiteral("Visnip lightweight screenshot and pin tool"));
    parser.addHelpOption();
    parser.addVersionOption();
    QCommandLineOption selfTest(QStringLiteral("self-test"), QStringLiteral("Run a non-interactive smoke test and exit."));
    parser.addOption(selfTest);
    parser.process(app);

    if (parser.isSet(selfTest)) {
        Visnip::Perf::log(QStringLiteral("main.self_test.start"));
        return runSelfTest();
    }

    Visnip::AppController controller;
    if (!controller.initialize()) {
        Visnip::Perf::log(QStringLiteral("main.controller.initialize.failed"));
        return 1;
    }

    Visnip::Perf::log(QStringLiteral("main.event_loop.start"));
    return QApplication::exec();
}
