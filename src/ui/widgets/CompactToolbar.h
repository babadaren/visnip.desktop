#pragma once

#include <QButtonGroup>
#include <QFrame>
#include <QHash>
#include <QToolButton>

namespace Visnip::Ui {

class CompactToolbar : public QFrame {
    Q_OBJECT
public:
    explicit CompactToolbar(QWidget* parent = nullptr);

    void setDarkMode(bool dark);
    void selectTool(const QString& id);
    void setUndoAvailable(bool available);
    void setRedoAvailable(bool available);
    void setLongCaptureMode(bool enabled);
    // Fast-translate feedback on the translate button: busy shows a
    // cancellable in-flight state, active marks "translation shown".
    void setTranslateState(bool active, bool busy);
    QString activeTool() const;

signals:
    void toolSelected(const QString& id);
    void actionTriggered(const QString& id);

private:
    QToolButton* addButton(const QString& id, const QString& tooltip, bool checkable, int group);
    void addSeparator();
    void refreshIcons();
    void styleSelf();

    bool darkMode_ = false;
    bool longCaptureMode_ = false;
    bool translateActive_ = false;
    QButtonGroup* tools_ = nullptr;
    QHash<QString, QToolButton*> buttons_;
};

} // namespace Visnip::Ui
