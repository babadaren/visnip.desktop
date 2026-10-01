#pragma once

#include <QColor>
#include <QString>

namespace Visnip::Design {

struct ToolbarTokens {
    int height = 30;
    int button = 26;
    int icon = 23;
    int paddingX = 0;
    int paddingY = 2;
    int gap = 5;
    int separatorWidth = 1;
    int separatorHeight = 14;
    int separatorBlock = 12;
    int radius = 9;
    int buttonRadius = 6;
    int safeHitArea = 26;
};

struct ColorTokens {
    QColor primary = QColor(QStringLiteral("#4F7CFF"));
    QColor primaryHover = QColor(QStringLiteral("#416BDF"));
    QColor primaryPressed = QColor(QStringLiteral("#3558C4"));
    QColor primarySoft = QColor(QStringLiteral("#EAF1FF"));
    QColor accent = QColor(QStringLiteral("#45D6B4"));
    QColor text = QColor(QStringLiteral("#172033"));
    QColor textSecondary = QColor(QStringLiteral("#667085"));
    QColor surface = QColor(QStringLiteral("#FFFFFF"));
    QColor surface2 = QColor(QStringLiteral("#F8FAFD"));
    QColor border = QColor(QStringLiteral("#DCE5F2"));
    QColor darkSurface = QColor(QStringLiteral("#1F2430"));
    QColor darkText = QColor(QStringLiteral("#F7F9FC"));
};

const ToolbarTokens& toolbar();
const ColorTokens& colors();
QString appStyleSheet(bool darkMode = false);

} // namespace Visnip::Design
