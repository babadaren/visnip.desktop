#pragma once

#include <QApplication>
#include <QClipboard>
#include <QEvent>
#include <QFrame>
#include <QJsonArray>
#include <QJsonObject>
#include <QLabel>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QRect>
#include <QStringList>
#include <QToolButton>
#include <QVBoxLayout>
#include <algorithm>

namespace Visnip {

// Capture-owned plain-text presentation. Never writes files, interprets markup,
// follows links or copies automatically. Closing the capture releases the text.
class TranslationReviewPanel final : public QFrame {
public:
    explicit TranslationReviewPanel(QWidget* parent) : QFrame(parent)
    {
        setObjectName(QStringLiteral("VisnipTranslationReview"));
        setFrameShape(QFrame::StyledPanel);
        setAutoFillBackground(true);
        auto* layout = new QVBoxLayout(this);
        layout->setContentsMargins(8, 8, 8, 8);
        toggle_ = new QToolButton(this);
        toggle_->setObjectName(QStringLiteral("VisnipTranslationReviewToggle"));
        toggle_->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
        layout->addWidget(toggle_);
        summary_ = new QLabel(this);
        summary_->setTextFormat(Qt::PlainText);
        summary_->setWordWrap(true);
        layout->addWidget(summary_);
        text_ = new QPlainTextEdit(this);
        text_->setObjectName(QStringLiteral("VisnipTranslationReviewText"));
        text_->setReadOnly(true);
        text_->setMinimumSize(0, 0);
        layout->addWidget(text_, 1);
        copy_ = new QPushButton(QStringLiteral("复制完整译文"), this);
        copy_->setObjectName(QStringLiteral("VisnipTranslationReviewCopy"));
        layout->addWidget(copy_);
        connect(toggle_, &QToolButton::clicked, this, [this]() {
            expanded_ = !expanded_;
            updatePresentation();
        });
        connect(copy_, &QPushButton::clicked, this, [this]() {
            if (!copyText_.isEmpty()) QApplication::clipboard()->setText(copyText_);
        });
        if (parent) parent->installEventFilter(this);
        hide();
    }

    void setResult(const QJsonArray& blocks, const QRect& selection)
    {
        clear();
        selection_ = selection;
        QStringList lines, translated;
        int pending = 0, failed = 0;
        for (int i = 0; i < std::min(qsizetype(256), blocks.size()); ++i) {
            const auto block = blocks.at(i).toObject();
            const auto source = block.value(QStringLiteral("source")).toString();
            const auto target = block.value(QStringLiteral("translation")).toString();
            if (source.size() > 8000 || target.size() > 8000) continue;
            const auto state = block.value(QStringLiteral("translation_status")).toString();
            const bool unchanged = block.value(QStringLiteral("reason")).toString() == QStringLiteral("equivalent");
            if (unchanged && state != QStringLiteral("failed")) continue;
            ++count_;
            const bool unavailable = state == QStringLiteral("failed") || target.trimmed().isEmpty();
            const bool fallback = !unavailable && block.value(QStringLiteral("status")).toString() != QStringLiteral("applied");
            pending += fallback;
            failed += unavailable;
            const QString status = unavailable ? QStringLiteral("未获得合格译文")
                : fallback ? QStringLiteral("未原位回填，译文如下") : QStringLiteral("已原位显示");
            lines << QStringLiteral("%1. %2\n原文：%3\n译文：%4")
                .arg(count_).arg(status, source, unavailable ? QStringLiteral("本区域翻译未完成") : target);
            if (!unavailable) translated << target;
        }
        copyText_ = translated.join(QStringLiteral("\n"));
        text_->setPlainText(lines.join(QStringLiteral("\n\n")));
        if (pending || failed) {
            summary_->setText(QStringLiteral("%1 个区域未原位显示，%2 个区域翻译未完成。此面板不写入导出的截图。")
                              .arg(pending).arg(failed));
        } else {
            summary_->setText(QStringLiteral("可查看和复制全部译文。此面板不写入导出的截图。"));
        }
        // Results never open a side panel, including partially composed ones.
        // The capture toolbar/shortcut is the explicit entry point.
        expanded_ = false;
        updatePresentation();
    }

    void setAnchor(const QRect& selection)
    {
        selection_ = selection;
        positionPanel();
    }

    void setTranslationVisible(bool visible)
    {
        translationVisible_ = visible;
        if (!visible) requested_ = false;
        setVisible(visible && requested_ && count_ > 0);
        if (isVisible()) { positionPanel(); raise(); }
    }

    bool hasResult() const { return count_ > 0; }

    void toggleByUser()
    {
        if (!translationVisible_ || !hasResult()) return;
        requested_ = !isVisible();
        expanded_ = true;
        updatePresentation();
        setTranslationVisible(translationVisible_);
    }

    void clear()
    {
        count_ = 0;
        copyText_.clear();
        text_->clear();
        summary_->clear();
        expanded_ = false;
        requested_ = false;
        translationVisible_ = false;
        hide();
    }

protected:
    bool eventFilter(QObject* watched, QEvent* event) override
    {
        if (watched == parentWidget() && event->type() == QEvent::Resize) positionPanel();
        return QFrame::eventFilter(watched, event);
    }

private:
    void updatePresentation()
    {
        toggle_->setText(QStringLiteral("%1译文（%2）").arg(expanded_ ? QStringLiteral("收起") : QStringLiteral("查看")).arg(count_));
        toggle_->setArrowType(expanded_ ? Qt::DownArrow : Qt::RightArrow);
        text_->setVisible(expanded_);
        summary_->setVisible(expanded_);
        copy_->setVisible(expanded_);
        copy_->setEnabled(!copyText_.isEmpty());
        positionPanel();
    }

    void positionPanel()
    {
        if (!parentWidget()) return;
        const auto viewport = parentWidget()->rect().adjusted(8, 8, -8, -8);
        if (viewport.width() <= 0 || viewport.height() <= 0) return;
        const int width = std::min(expanded_ ? 440 : 190, viewport.width());
        const int height = std::min(expanded_ ? 320 : 42, viewport.height());
        int x = selection_.right()+10, y = selection_.top();
        if (x+width > viewport.right()+1) {
            x = selection_.left()-width-10;
            if (x < viewport.left()) {
                x = selection_.left();
                // Leave room for the screenshot action toolbar below selection.
                y = selection_.bottom()+64;
                if (y+height > viewport.bottom()+1) y = selection_.top()-height-10;
            }
        }
        x = std::clamp(x, viewport.left(), std::max(viewport.left(), viewport.right()-width+1));
        y = std::clamp(y, viewport.top(), std::max(viewport.top(), viewport.bottom()-height+1));
        setGeometry(x, y, width, height);
    }

    QToolButton* toggle_ = nullptr;
    QLabel* summary_ = nullptr;
    QPlainTextEdit* text_ = nullptr;
    QPushButton* copy_ = nullptr;
    QString copyText_;
    QRect selection_;
    int count_ = 0;
    bool expanded_ = false;
    bool requested_ = false;
    bool translationVisible_ = false;
};
} // namespace Visnip
