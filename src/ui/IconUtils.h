#pragma once

#include <QColor>
#include <QIcon>
#include <QString>

namespace Visnip::Ui {

QIcon applicationIcon();
QIcon themedIcon(const QString& resourcePath, const QColor& color, QSize size = QSize(18, 18));
QIcon toolbarIcon(const QString& id, bool dark = false, bool accent = false);
QString iconPathForId(const QString& id);
void warmupToolbarIcons(bool dark = false);

} // namespace Visnip::Ui
