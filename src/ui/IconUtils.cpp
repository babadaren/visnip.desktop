#include "ui/IconUtils.h"

#include "core/DesignTokens.h"
#include "core/PerfLog.h"

#include <QBuffer>
#include <QElapsedTimer>
#include <QFile>
#include <QHash>
#include <QPainter>
#include <QPixmap>
#include <QStringList>
#include <QSvgRenderer>

namespace Visnip::Ui {

QIcon applicationIcon()
{
    QIcon icon;
    constexpr int sizes[] = { 16, 20, 24, 32, 48, 64, 128, 256, 512 };
    for (const int size : sizes) {
        icon.addFile(
            QStringLiteral(":/visnip/app/visnip-app-icon-%1.png").arg(size),
            QSize(size, size));
    }
    return icon;
}

QIcon themedIcon(const QString& resourcePath, const QColor& color, QSize size)
{
    QElapsedTimer timer;
    timer.start();
    QFile file(resourcePath);
    if (!file.open(QIODevice::ReadOnly)) {
        Perf::log(QStringLiteral("themedIcon.open_failed path=%1 elapsed=%2ms").arg(resourcePath).arg(timer.elapsed()));
        return QIcon(resourcePath);
    }

    QByteArray svg = file.readAll();
    const QByteArray colorName = color.name(QColor::HexRgb).toUtf8();
    svg.replace("currentColor", colorName);
    svg.replace("#172033", colorName);

    QSvgRenderer renderer(svg);
    if (!renderer.isValid()) {
        Perf::log(QStringLiteral("themedIcon.invalid_svg path=%1 elapsed=%2ms").arg(resourcePath).arg(timer.elapsed()));
        return QIcon(resourcePath);
    }

    const int scale = 3;
    QPixmap pixmap(size * scale);
    pixmap.fill(Qt::transparent);
    pixmap.setDevicePixelRatio(scale);

    QPainter painter(&pixmap);
    painter.setRenderHint(QPainter::Antialiasing, true);
    renderer.render(&painter, QRectF(QPointF(0, 0), QSizeF(size)));
    painter.end();

    Perf::log(QStringLiteral("themedIcon.render path=%1 color=%2 size=%3x%4 elapsed=%5ms")
                  .arg(resourcePath)
                  .arg(color.name(QColor::HexRgb))
                  .arg(size.width())
                  .arg(size.height())
                  .arg(timer.elapsed()));
    return QIcon(pixmap);
}

QString iconPathForId(const QString& id)
{
    return QStringLiteral(":/visnip/icons/%1.svg").arg(id);
}

QIcon toolbarIcon(const QString& id, bool dark, bool accent)
{
    const QString key = QStringLiteral("%1|%2|%3").arg(id).arg(dark ? 1 : 0).arg(accent ? 1 : 0);
    static QHash<QString, QIcon> cache;
    const auto it = cache.constFind(key);
    if (it != cache.constEnd()) {
        return it.value();
    }

    QElapsedTimer timer;
    timer.start();
    const QColor color = accent ? QColor(255, 255, 255) : (dark ? Design::colors().darkText : Design::colors().text);
    const QIcon icon = themedIcon(iconPathForId(id), color, QSize(Design::toolbar().icon, Design::toolbar().icon));
    cache.insert(key, icon);
    Perf::log(QStringLiteral("toolbarIcon.cache_miss id=%1 dark=%2 accent=%3 elapsed=%4ms")
                  .arg(id)
                  .arg(dark)
                  .arg(accent)
                  .arg(timer.elapsed()));
    return icon;
}

void warmupToolbarIcons(bool dark)
{
    Perf::ScopedTimer total(QStringLiteral("warmupToolbarIcons.%1").arg(dark ? QStringLiteral("dark") : QStringLiteral("light")));
    const QStringList ids = {
        QStringLiteral("tool-rect"),
        QStringLiteral("tool-arrow"),
        QStringLiteral("tool-pen"),
        QStringLiteral("tool-mosaic"),
        QStringLiteral("tool-text"),
        QStringLiteral("tool-rubber"),
        QStringLiteral("tool-number"),
        QStringLiteral("tool-longshot"),
        QStringLiteral("action-undo"),
        QStringLiteral("action-redo"),
        QStringLiteral("action-pin"),
        QStringLiteral("action-save"),
        QStringLiteral("action-copy"),
        QStringLiteral("action-translate"),
        QStringLiteral("action-question"),
        QStringLiteral("action-close"),
    };

    for (const QString& id : ids) {
        toolbarIcon(id, dark, false);
        toolbarIcon(id, dark, true);
    }
}

} // namespace Visnip::Ui
