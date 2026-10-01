#include "ui/widgets/CompactToolbar.h"

#include "core/DesignTokens.h"
#include "ui/IconUtils.h"

#include <QHBoxLayout>
#include <QPainter>
#include <QStyle>
#include <QStringList>

namespace Visnip::Ui {

namespace {
constexpr const char* kToolIds[] = {
    "tool-rect",
    "tool-arrow",
    "tool-pen",
    "tool-mosaic",
    "tool-text",
    "tool-rubber",
    "tool-number",
    "tool-longshot",
};

bool isTool(const QString& id)
{
    for (const auto* tool : kToolIds) {
        if (id == QLatin1String(tool)) {
            return true;
        }
    }
    return false;
}
} // namespace

CompactToolbar::CompactToolbar(QWidget* parent)
    : QFrame(parent)
    , tools_(new QButtonGroup(this))
{
    setObjectName(QStringLiteral("VisnipToolbar"));
    setAttribute(Qt::WA_StyledBackground, true);
    setCursor(Qt::ArrowCursor);
    tools_->setExclusive(true);

    auto* layout = new QHBoxLayout(this);
    const auto& t = Design::toolbar();
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(t.gap);

    addButton(QStringLiteral("tool-rect"), QStringLiteral("矩形"), true, 1);
    addButton(QStringLiteral("tool-arrow"), QStringLiteral("箭头"), true, 1);
    addButton(QStringLiteral("tool-pen"), QStringLiteral("画笔"), true, 1);
    addButton(QStringLiteral("tool-mosaic"), QStringLiteral("马赛克 / 模糊"), true, 1);
    addButton(QStringLiteral("tool-text"), QStringLiteral("文字"), true, 1);
    addButton(QStringLiteral("tool-rubber"), QStringLiteral("橡皮擦"), true, 1);
    addButton(QStringLiteral("tool-number"), QStringLiteral("序号"), true, 1);
    addButton(QStringLiteral("tool-longshot"), QStringLiteral("长截图"), true, 1);
    addSeparator();
    addButton(QStringLiteral("action-undo"), QStringLiteral("撤销 Ctrl+Z"), false, 2);
    addButton(QStringLiteral("action-redo"), QStringLiteral("重做 Ctrl+Y"), false, 2);
    addSeparator();
    addButton(QStringLiteral("action-translate"), QStringLiteral("翻译"), false, 2);
    addButton(QStringLiteral("action-question"), QStringLiteral("解题"), false, 2);
    addSeparator();
    addButton(QStringLiteral("action-close"), QStringLiteral("关闭 Esc"), false, 3);
    addButton(QStringLiteral("action-pin"), QStringLiteral("贴图 P"), false, 3);
    addButton(QStringLiteral("action-save"), QStringLiteral("保存 Ctrl+S"), false, 3);
    addButton(QStringLiteral("action-copy"), QStringLiteral("复制 Enter"), false, 3);

    // No fixed height: the layout height (button row plus the 1px style-sheet
    // border on each side) keeps the full border visible above and below.
    styleSelf();
    refreshIcons();
}

QToolButton* CompactToolbar::addButton(const QString& id, const QString& tooltip, bool checkable, int)
{
    auto* button = new QToolButton(this);
    button->setObjectName(QStringLiteral("VisnipToolButton"));
    button->setAutoRaise(true);
    button->setCheckable(checkable);
    button->setToolTip(tooltip);
    button->setCursor(Qt::ArrowCursor);
    button->setIconSize(QSize(Design::toolbar().icon, Design::toolbar().icon));
    button->setFixedSize(Design::toolbar().height, Design::toolbar().height);
    button->setFocusPolicy(Qt::NoFocus);
    layout()->addWidget(button);
    buttons_.insert(id, button);

    if (checkable) {
        tools_->addButton(button);
    }

    connect(button, &QToolButton::clicked, this, [this, id, checkable]() {
        if (checkable || isTool(id)) {
            emit toolSelected(id);
        } else {
            emit actionTriggered(id);
        }
    });
    return button;
}

void CompactToolbar::addSeparator()
{
    auto* sep = new QFrame(this);
    sep->setObjectName(QStringLiteral("VisnipToolbarSeparator"));
    sep->setFrameShape(QFrame::VLine);
    sep->setFixedSize(Design::toolbar().separatorBlock, Design::toolbar().separatorHeight);
    sep->setCursor(Qt::ArrowCursor);
    sep->setStyleSheet(QStringLiteral("background: transparent; border-left: 1px solid %1; margin-left: 5px; margin-right: 5px;")
                           .arg(darkMode_ ? QStringLiteral("#3B4659") : QStringLiteral("#DCE5F2")));
    layout()->addWidget(sep);
}

void CompactToolbar::setDarkMode(bool dark)
{
    if (darkMode_ == dark) {
        return;
    }
    darkMode_ = dark;
    styleSelf();
    refreshIcons();
}

void CompactToolbar::selectTool(const QString& id)
{
    if (id.isEmpty()) {
        tools_->setExclusive(false);
        for (auto it = buttons_.begin(); it != buttons_.end(); ++it) {
            if (it.value()->isCheckable()) {
                it.value()->setChecked(false);
            }
        }
        tools_->setExclusive(true);
        refreshIcons();
        return;
    }

    auto* button = buttons_.value(id, nullptr);
    if (!button || !button->isCheckable()) {
        return;
    }
    button->setChecked(true);
    refreshIcons();
}

void CompactToolbar::setUndoAvailable(bool available)
{
    if (auto* b = buttons_.value(QStringLiteral("action-undo"))) {
        b->setEnabled(available);
    }
}

void CompactToolbar::setRedoAvailable(bool available)
{
    if (auto* b = buttons_.value(QStringLiteral("action-redo"))) {
        b->setEnabled(available);
    }
}

void CompactToolbar::setLongCaptureMode(bool enabled)
{
    longCaptureMode_ = enabled;
    const QStringList annotationToolIds = {
        QStringLiteral("tool-rect"),
        QStringLiteral("tool-arrow"),
        QStringLiteral("tool-pen"),
        QStringLiteral("tool-mosaic"),
        QStringLiteral("tool-text"),
        QStringLiteral("tool-rubber"),
        QStringLiteral("tool-number"),
    };

    for (const QString& id : annotationToolIds) {
        if (auto* button = buttons_.value(id, nullptr)) {
            button->setEnabled(true);
        }
    }
    if (auto* button = buttons_.value(QStringLiteral("tool-longshot"), nullptr)) {
        button->setEnabled(true);
        button->setProperty("longCaptureActive", enabled);
        button->style()->unpolish(button);
        button->style()->polish(button);
        button->update();
    }
    // A long capture has no fixed screen region to translate or re-grab.
    for (const QString& id : {QStringLiteral("action-translate"), QStringLiteral("action-question")}) {
        if (auto* button = buttons_.value(id, nullptr)) {
            button->setEnabled(!enabled);
        }
    }
    refreshIcons();
}

void CompactToolbar::setTranslateState(bool active, bool busy)
{
    auto* button = buttons_.value(QStringLiteral("action-translate"), nullptr);
    if (!button) {
        return;
    }
    translateActive_ = active || busy;
    button->setProperty("translateActive", translateActive_);
    button->setToolTip(busy
                           ? QStringLiteral("翻译中…点击取消")
                           : active ? QStringLiteral("已显示译文，点击切回原图；Shift+点击或 Ctrl+Shift+T 查看完整译文")
                                    : QStringLiteral("翻译"));
    button->style()->unpolish(button);
    button->style()->polish(button);
    button->update();
    refreshIcons();
}

QString CompactToolbar::activeTool() const
{
    for (auto it = buttons_.cbegin(); it != buttons_.cend(); ++it) {
        if (it.value()->isCheckable() && it.value()->isChecked()) {
            return it.key();
        }
    }
    return {};
}

void CompactToolbar::refreshIcons()
{
    for (auto it = buttons_.begin(); it != buttons_.end(); ++it) {
        const bool isTranslate = it.key() == QStringLiteral("action-translate");
        const bool highlighted = it.value()->isChecked()
            || (longCaptureMode_ && it.key() == QStringLiteral("tool-longshot"))
            || (translateActive_ && isTranslate);
        it.value()->setIcon(toolbarIcon(it.key(), darkMode_, highlighted));
    }
}

void CompactToolbar::styleSelf()
{
    const QString bg = darkMode_ ? QStringLiteral("rgba(31,36,48,0.96)") : QStringLiteral("rgba(255,255,255,0.94)");
    const QString border = darkMode_ ? QStringLiteral("#3B4659") : QStringLiteral("#DCE5F2");
    const QString hover = darkMode_ ? QStringLiteral("#333C4D") : QStringLiteral("#E1E5EA");
    const QString checked = darkMode_ ? QStringLiteral("rgba(79,124,255,0.48)") : QStringLiteral("#0B7CFF");
    setStyleSheet(QStringLiteral(R"(
        QFrame#VisnipToolbar {
            background: %1;
            border: 1px solid %2;
            border-radius: %3px;
        }
        QToolButton#VisnipToolButton {
            background: transparent;
            border: none;
            border-radius: 0px;
            padding: 0;
        }
        QToolButton#VisnipToolButton:hover { background: %4; }
        QToolButton#VisnipToolButton:checked,
        QToolButton#VisnipToolButton[longCaptureActive="true"],
        QToolButton#VisnipToolButton[translateActive="true"] { background: %5; }
        QToolButton#VisnipToolButton:disabled { opacity: 0.40; }
    )")
                      .arg(bg,
                           border,
                           QStringLiteral("0"),
                           hover,
                           checked));
}

} // namespace Visnip::Ui
