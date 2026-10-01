#include "core/DesignTokens.h"

namespace Visnip::Design {

const ToolbarTokens& toolbar()
{
    static const ToolbarTokens tokens;
    return tokens;
}

const ColorTokens& colors()
{
    static const ColorTokens tokens;
    return tokens;
}

QString appStyleSheet(bool darkMode)
{
    const auto& t = toolbar();
    const auto& c = colors();
    const QString surface = darkMode ? QStringLiteral("rgba(31,36,48,0.96)") : QStringLiteral("rgba(255,255,255,0.94)");
    const QString text = darkMode ? c.darkText.name() : c.text.name();
    const QString border = darkMode ? QStringLiteral("#3B4659") : c.border.name();
    const QString hover = darkMode ? QStringLiteral("#333C4D") : QStringLiteral("#F5F8FD");
    const QString checked = darkMode ? QStringLiteral("rgba(79,124,255,0.28)") : c.primarySoft.name();
    const QString dialogBg = darkMode ? QStringLiteral("#1F2430") : QStringLiteral("#FFFFFF");
    const QString panelBg = darkMode ? QStringLiteral("#293140") : QStringLiteral("#F8FAFD");

    return QStringLiteral(R"(
        QWidget {
            font-family: "Microsoft YaHei UI", "Segoe UI", sans-serif;
            font-size: 12px;
            color: %1;
            background: %6;
        }
        QDialog, QMenu { background: %6; }
        QMenu { border: 1px solid %3; padding: 6px; border-radius: 8px; }
        QMenu::item { padding: 7px 28px 7px 12px; border-radius: 6px; }
        QMenu::item:selected { background: %4; color: %1; }
        QLabel { background: transparent; }
        QLabel#VisnipHintLabel { color: #667085; }
        QPushButton {
            min-height: 26px;
            padding: 2px 14px;
            border: 1px solid %3;
            border-radius: 6px;
            background: %7;
        }
        QPushButton:hover { background: %4; border-color: %2; }
        QPushButton:pressed { background: %5; color: %2; }
        QLineEdit, QComboBox, QSpinBox, QDoubleSpinBox, QKeySequenceEdit {
            min-height: 26px;
            border: 1px solid %3;
            border-radius: 6px;
            padding: 2px 8px;
            background: %6;
            selection-background-color: %2;
            selection-color: #FFFFFF;
        }
        QLineEdit:hover, QComboBox:hover, QSpinBox:hover, QDoubleSpinBox:hover { border-color: #A9BBD6; }
        QLineEdit:focus, QComboBox:focus, QSpinBox:focus, QDoubleSpinBox:focus, QKeySequenceEdit:focus { border: 1px solid %2; }
        QLineEdit:read-only { background: %7; color: #667085; }
        QComboBox { padding-right: 24px; }
        QComboBox::drop-down {
            subcontrol-origin: padding;
            subcontrol-position: top right;
            width: 22px;
            border: none;
            background: transparent;
        }
        QComboBox::down-arrow { image: url(:/visnip/icons/combo-arrow.svg); width: 12px; height: 12px; }
        QComboBox QAbstractItemView {
            border: 1px solid %3;
            border-radius: 8px;
            background: %6;
            padding: 4px;
            outline: none;
        }
        QComboBox QAbstractItemView::item {
            min-height: 26px;
            padding: 2px 8px;
            border-radius: 5px;
        }
        QComboBox QAbstractItemView::item:hover { background: %4; }
        QComboBox QAbstractItemView::item:selected { background: %5; color: %2; }
        QSpinBox, QDoubleSpinBox { padding-right: 20px; }
        QSpinBox::up-button, QDoubleSpinBox::up-button,
        QSpinBox::down-button, QDoubleSpinBox::down-button {
            subcontrol-origin: border;
            width: 18px;
            border: none;
            background: transparent;
            border-radius: 4px;
            margin: 1px;
        }
        QSpinBox::up-button, QDoubleSpinBox::up-button { subcontrol-position: top right; }
        QSpinBox::down-button, QDoubleSpinBox::down-button { subcontrol-position: bottom right; }
        QSpinBox::up-button:hover, QDoubleSpinBox::up-button:hover,
        QSpinBox::down-button:hover, QDoubleSpinBox::down-button:hover { background: %4; }
        QSpinBox::up-arrow, QDoubleSpinBox::up-arrow { image: url(:/visnip/icons/combo-arrow-up.svg); width: 10px; height: 10px; }
        QSpinBox::down-arrow, QDoubleSpinBox::down-arrow { image: url(:/visnip/icons/combo-arrow.svg); width: 10px; height: 10px; }
        QCheckBox { spacing: 8px; background: transparent; min-height: 24px; }
        QCheckBox::indicator {
            width: 16px;
            height: 16px;
            border: 1px solid #C3CFE0;
            border-radius: 4px;
            background: %6;
        }
        QCheckBox::indicator:hover { border-color: %2; }
        QCheckBox::indicator:checked {
            border-color: %2;
            background: %2;
            image: url(:/visnip/icons/check-white.svg);
        }
        QGroupBox {
            border: 1px solid %3;
            border-radius: 8px;
            margin-top: 10px;
            padding: 10px 12px 8px 12px;
            background: %6;
        }
        QGroupBox::title {
            subcontrol-origin: margin;
            subcontrol-position: top left;
            left: 10px;
            padding: 0 4px;
            color: %1;
            font-weight: 600;
            background: %6;
        }
        QListWidget {
            border: none;
            background: %7;
            border-radius: 8px;
            outline: none;
            padding: 4px;
        }
        QListWidget::item {
            min-height: 34px;
            padding: 4px 10px;
            border-radius: 8px;
        }
        QListWidget::item:hover { background: %4; }
        QListWidget::item:selected { background: %5; color: %2; }
        QScrollBar:vertical {
            width: 8px;
            background: transparent;
            margin: 2px;
        }
        QScrollBar::handle:vertical {
            background: #C9D4E5;
            border-radius: 3px;
            min-height: 24px;
        }
        QScrollBar::handle:vertical:hover { background: #A9BBD6; }
        QScrollBar::add-line:vertical, QScrollBar::sub-line:vertical { height: 0; }
        QScrollBar::add-page:vertical, QScrollBar::sub-page:vertical { background: transparent; }
        QToolButton#VisnipToolButton {
            min-width: %8px;
            min-height: %8px;
            max-width: %8px;
            max-height: %8px;
            border-radius: %9px;
            padding: 0;
            border: none;
            background: transparent;
        }
        QToolButton#VisnipToolButton:hover { background: %4; }
        QToolButton#VisnipToolButton:checked { background: %5; }
    )")
        .arg(text,
             c.primary.name(),
             border,
             hover,
             checked,
             dialogBg,
             panelBg,
             QString::number(t.button),
             QString::number(t.buttonRadius));
}

} // namespace Visnip::Design
