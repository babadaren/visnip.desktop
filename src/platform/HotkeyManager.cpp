#include "platform/HotkeyManager.h"

#include "core/PerfLog.h"

#include <QApplication>
#include <QDebug>
#include <QSet>

#include <array>
#include <utility>

#ifdef Q_OS_WIN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace Visnip {

namespace {
#ifdef Q_OS_WIN
QString actionName(GlobalHotkeyAction action)
{
    switch (action) {
    case GlobalHotkeyAction::Capture:
        return QStringLiteral("Capture");
    case GlobalHotkeyAction::PinClipboard:
        return QStringLiteral("PinClipboard");
    case GlobalHotkeyAction::RepeatCapture:
        return QStringLiteral("RepeatCapture");
    case GlobalHotkeyAction::TogglePins:
        return QStringLiteral("TogglePins");
    case GlobalHotkeyAction::ToggleMouseThrough:
        return QStringLiteral("ToggleMouseThrough");
    case GlobalHotkeyAction::AskQuestion:
        return QStringLiteral("AskQuestion");
    }
    return QStringLiteral("Unknown");
}
#endif

QString actionDisplayName(GlobalHotkeyAction action)
{
    switch (action) {
    case GlobalHotkeyAction::Capture:
        return QStringLiteral("开始截图");
    case GlobalHotkeyAction::PinClipboard:
        return QStringLiteral("贴出剪贴板图片");
    case GlobalHotkeyAction::RepeatCapture:
        return QStringLiteral("重复上次截图区域");
    case GlobalHotkeyAction::TogglePins:
        return QStringLiteral("隐藏或显示所有贴图");
    case GlobalHotkeyAction::ToggleMouseThrough:
        return QStringLiteral("当前贴图鼠标穿透");
    case GlobalHotkeyAction::AskQuestion:
        return QStringLiteral("获取答案");
    }
    return QStringLiteral("未知操作");
}

constexpr std::array<GlobalHotkeyAction, 5> kAllHotkeyActions = {
    GlobalHotkeyAction::Capture,
    GlobalHotkeyAction::PinClipboard,
    GlobalHotkeyAction::RepeatCapture,
    GlobalHotkeyAction::TogglePins,
    GlobalHotkeyAction::ToggleMouseThrough,
};

bool isSupportedQtKey(int key)
{
    if (key >= Qt::Key_A && key <= Qt::Key_Z) {
        return true;
    }
    if (key >= Qt::Key_0 && key <= Qt::Key_9) {
        return true;
    }
    if (key >= Qt::Key_F1 && key <= Qt::Key_F24) {
        return true;
    }
    switch (key) {
    case Qt::Key_Escape:
    case Qt::Key_Return:
    case Qt::Key_Enter:
    case Qt::Key_Space:
    case Qt::Key_Tab:
        return true;
    default:
        return false;
    }
}

int normalizedQtKey(int key)
{
    // The current Windows backend maps both Qt enter keys to VK_RETURN.
    return key == Qt::Key_Enter ? Qt::Key_Return : key;
}

bool mapsToSameHotkey(const QKeySequence& left, const QKeySequence& right)
{
    const QKeyCombination leftCombination = left[0];
    const QKeyCombination rightCombination = right[0];
    return leftCombination.keyboardModifiers() == rightCombination.keyboardModifiers()
        && normalizedQtKey(leftCombination.key()) == normalizedQtKey(rightCombination.key());
}

void setError(QString* destination, const QString& message)
{
    if (destination) {
        *destination = message;
    }
}

#ifdef Q_OS_WIN

UINT qtModifiersToWin(Qt::KeyboardModifiers modifiers)
{
    UINT result = 0;
    if (modifiers.testFlag(Qt::ControlModifier)) {
        result |= MOD_CONTROL;
    }
    if (modifiers.testFlag(Qt::ShiftModifier)) {
        result |= MOD_SHIFT;
    }
    if (modifiers.testFlag(Qt::AltModifier)) {
        result |= MOD_ALT;
    }
    if (modifiers.testFlag(Qt::MetaModifier)) {
        result |= MOD_WIN;
    }
    return result;
}

UINT qtKeyToVirtualKey(int key)
{
    if (key >= Qt::Key_A && key <= Qt::Key_Z) {
        return static_cast<UINT>('A' + (key - Qt::Key_A));
    }
    if (key >= Qt::Key_0 && key <= Qt::Key_9) {
        return static_cast<UINT>('0' + (key - Qt::Key_0));
    }
    if (key >= Qt::Key_F1 && key <= Qt::Key_F24) {
        return static_cast<UINT>(VK_F1 + (key - Qt::Key_F1));
    }
    switch (key) {
    case Qt::Key_Escape:
        return VK_ESCAPE;
    case Qt::Key_Return:
    case Qt::Key_Enter:
        return VK_RETURN;
    case Qt::Key_Space:
        return VK_SPACE;
    case Qt::Key_Tab:
        return VK_TAB;
    default:
        return 0;
    }
}
#endif
} // namespace

HotkeyManager::HotkeyManager(QObject* parent)
    : QObject(parent)
{
    qApp->installNativeEventFilter(this);
}

HotkeyManager::~HotkeyManager()
{
    unregisterAll();
    if (qApp) {
        qApp->removeNativeEventFilter(this);
    }
}

HotkeyValidationResult HotkeyManager::validateSequence(const QKeySequence& sequence)
{
    if (sequence.isEmpty()) {
        return {false, QStringLiteral("快捷键不能为空。")};
    }
    if (sequence.count() != 1) {
        return {false, QStringLiteral("仅支持单段快捷键，请不要输入连续按键。")};
    }

    const QKeyCombination combination = sequence[0];
    constexpr Qt::KeyboardModifiers supportedModifiers = Qt::ControlModifier | Qt::ShiftModifier
        | Qt::AltModifier | Qt::MetaModifier;
    if ((combination.keyboardModifiers() & ~supportedModifiers) != Qt::NoModifier) {
        return {false, QStringLiteral("该组合包含不支持的修饰键。")};
    }
    if (!isSupportedQtKey(combination.key())) {
        return {false, QStringLiteral("该按键不支持注册为全局快捷键。")};
    }
    return {true, {}};
}

bool HotkeyManager::registerHotkey(GlobalHotkeyAction action, const QKeySequence& sequence)
{
#ifdef Q_OS_WIN
    const HotkeyValidationResult validation = validateSequence(sequence);
    if (!validation.valid) {
        emit registrationFailed(action, sequence);
        return false;
    }
    const QKeyCombination combination = sequence[0];
    const UINT modifiers = qtModifiersToWin(combination.keyboardModifiers());
    const UINT vk = qtKeyToVirtualKey(combination.key());
    if (vk == 0) {
        emit registrationFailed(action, sequence);
        return false;
    }

    const int id = nextId_++;
    if (!RegisterHotKey(nullptr, id, modifiers, vk)) {
        qWarning() << "Failed to register global hotkey" << sequence.toString();
        Perf::log(QStringLiteral("hotkey.register_failed action=%1 sequence=%2 id=%3")
                      .arg(actionName(action), sequence.toString(QKeySequence::NativeText))
                      .arg(id));
        emit registrationFailed(action, sequence);
        return false;
    }
    idToAction_.insert(id, action);
    idToSequence_.insert(id, sequence);
    Perf::log(QStringLiteral("hotkey.registered action=%1 sequence=%2 id=%3")
                  .arg(actionName(action), sequence.toString(QKeySequence::NativeText))
                  .arg(id));
    return true;
#else
    Q_UNUSED(action)
    Q_UNUSED(sequence)
    return false;
#endif
}

bool HotkeyManager::replaceHotkeys(const GlobalHotkeyBindings& bindings, QString* errorMessage)
{
    setError(errorMessage, {});
    if (bindings.size() != static_cast<qsizetype>(kAllHotkeyActions.size())) {
        setError(errorMessage, QStringLiteral("快捷键配置不完整，必须为全部 5 个操作分别设置快捷键。"));
        return false;
    }

    for (const GlobalHotkeyAction action : kAllHotkeyActions) {
        const auto it = bindings.constFind(action);
        if (it == bindings.cend()) {
            setError(errorMessage, QStringLiteral("快捷键配置缺少“%1”。").arg(actionDisplayName(action)));
            return false;
        }
        const HotkeyValidationResult validation = validateSequence(it.value());
        if (!validation.valid) {
            setError(errorMessage,
                     QStringLiteral("“%1”：%2").arg(actionDisplayName(action), validation.errorMessage));
            emit registrationFailed(action, it.value());
            return false;
        }
    }

    for (qsizetype i = 0; i < static_cast<qsizetype>(kAllHotkeyActions.size()); ++i) {
        const GlobalHotkeyAction left = kAllHotkeyActions[static_cast<std::size_t>(i)];
        for (qsizetype j = i + 1; j < static_cast<qsizetype>(kAllHotkeyActions.size()); ++j) {
            const GlobalHotkeyAction right = kAllHotkeyActions[static_cast<std::size_t>(j)];
            if (mapsToSameHotkey(bindings.value(left), bindings.value(right))) {
                setError(errorMessage,
                         QStringLiteral("“%1”和“%2”不能使用同一个全局快捷键。")
                             .arg(actionDisplayName(left), actionDisplayName(right)));
                return false;
            }
        }
    }

#ifdef Q_OS_WIN
    QHash<int, GlobalHotkeyAction> replacementActions;
    QHash<int, QKeySequence> replacementSequences;
    QSet<int> reusedIds;
    QList<int> stagedIds;

    for (const GlobalHotkeyAction action : kAllHotkeyActions) {
        const QKeySequence sequence = bindings.value(action);
        int registrationId = -1;
        for (auto it = idToSequence_.cbegin(); it != idToSequence_.cend(); ++it) {
            if (mapsToSameHotkey(it.value(), sequence) && !reusedIds.contains(it.key())) {
                registrationId = it.key();
                reusedIds.insert(registrationId);
                break;
            }
        }

        if (registrationId < 0) {
            registrationId = nextId_++;
            const QKeyCombination combination = sequence[0];
            if (!RegisterHotKey(nullptr,
                                registrationId,
                                qtModifiersToWin(combination.keyboardModifiers()),
                                qtKeyToVirtualKey(combination.key()))) {
                for (const int stagedId : stagedIds) {
                    UnregisterHotKey(nullptr, stagedId);
                }
                const QString message = QStringLiteral("“%1”（%2）无法注册，可能已被其他程序占用。")
                                            .arg(actionDisplayName(action),
                                                 sequence.toString(QKeySequence::NativeText));
                setError(errorMessage, message);
                qWarning() << message;
                Perf::log(QStringLiteral("hotkey.replace_failed action=%1 sequence=%2 id=%3")
                              .arg(actionName(action), sequence.toString(QKeySequence::NativeText))
                              .arg(registrationId));
                emit registrationFailed(action, sequence);
                return false;
            }
            stagedIds.append(registrationId);
        }

        replacementActions.insert(registrationId, action);
        replacementSequences.insert(registrationId, sequence);
    }

    for (auto it = idToAction_.cbegin(); it != idToAction_.cend(); ++it) {
        if (!reusedIds.contains(it.key())) {
            UnregisterHotKey(nullptr, it.key());
        }
    }

    idToAction_ = std::move(replacementActions);
    idToSequence_ = std::move(replacementSequences);
    Perf::log(QStringLiteral("hotkey.replaced count=%1").arg(idToAction_.size()));
    return true;
#else
    setError(errorMessage, QStringLiteral("当前平台不支持全局快捷键。"));
    return false;
#endif
}

bool HotkeyManager::setAuxiliaryHotkey(GlobalHotkeyAction action,
                                       const QKeySequence& sequence,
                                       QString* errorMessage)
{
    setError(errorMessage, {});
    clearAuxiliaryHotkey(action);
    const HotkeyValidationResult validation = validateSequence(sequence);
    if (!validation.valid) {
        setError(errorMessage,
                 QStringLiteral("“%1”：%2").arg(actionDisplayName(action), validation.errorMessage));
        return false;
    }
#ifdef Q_OS_WIN
    const QKeyCombination combination = sequence[0];
    const int id = nextId_++;
    if (!RegisterHotKey(nullptr,
                        id,
                        qtModifiersToWin(combination.keyboardModifiers()),
                        qtKeyToVirtualKey(combination.key()))) {
        setError(errorMessage,
                 QStringLiteral("%1 已被其他程序占用，可在设置中更换“%2”的快捷键。")
                     .arg(sequence.toString(QKeySequence::NativeText), actionDisplayName(action)));
        Perf::log(QStringLiteral("hotkey.auxiliary_failed action=%1 sequence=%2 id=%3")
                      .arg(actionName(action), sequence.toString(QKeySequence::NativeText))
                      .arg(id));
        emit registrationFailed(action, sequence);
        return false;
    }
    auxiliaryIds_.insert(action, id);
    Perf::log(QStringLiteral("hotkey.auxiliary_registered action=%1 sequence=%2 id=%3")
                  .arg(actionName(action), sequence.toString(QKeySequence::NativeText))
                  .arg(id));
    return true;
#else
    setError(errorMessage, QStringLiteral("当前平台不支持全局快捷键。"));
    return false;
#endif
}

void HotkeyManager::clearAuxiliaryHotkey(GlobalHotkeyAction action)
{
    const auto it = auxiliaryIds_.constFind(action);
    if (it == auxiliaryIds_.cend()) {
        return;
    }
#ifdef Q_OS_WIN
    UnregisterHotKey(nullptr, it.value());
#endif
    auxiliaryIds_.erase(it);
}

void HotkeyManager::unregisterAll()
{
#ifdef Q_OS_WIN
    for (auto it = idToAction_.cbegin(); it != idToAction_.cend(); ++it) {
        UnregisterHotKey(nullptr, it.key());
    }
    for (auto it = auxiliaryIds_.cbegin(); it != auxiliaryIds_.cend(); ++it) {
        UnregisterHotKey(nullptr, it.value());
    }
#endif
    idToAction_.clear();
    idToSequence_.clear();
    auxiliaryIds_.clear();
}

bool HotkeyManager::nativeEventFilter(const QByteArray& eventType, void* message, qintptr* result)
{
    Q_UNUSED(result)
#ifdef Q_OS_WIN
    if (eventType != "windows_generic_MSG" && eventType != "windows_dispatcher_MSG") {
        return false;
    }
    MSG* msg = static_cast<MSG*>(message);
    if (msg && msg->message == WM_HOTKEY) {
        const int id = static_cast<int>(msg->wParam);
        const GlobalHotkeyAction* auxiliary = nullptr;
        for (auto it = auxiliaryIds_.cbegin(); it != auxiliaryIds_.cend(); ++it) {
            if (it.value() == id) {
                auxiliary = &it.key();
                break;
            }
        }
        if (idToAction_.contains(id) || auxiliary) {
            const GlobalHotkeyAction action = auxiliary ? *auxiliary : idToAction_.value(id);
            // msg->time is the tick the key event was posted; the difference
            // exposes how long the process took to wake up and dispatch it.
            const DWORD queueDelayMs = GetTickCount() - msg->time;
            Perf::log(QStringLiteral("hotkey.native_received action=%1 id=%2 queueDelay=%3ms")
                          .arg(actionName(action))
                          .arg(id)
                          .arg(queueDelayMs));
            emit activated(action);
            return true;
        }
    }
#else
    Q_UNUSED(eventType)
    Q_UNUSED(message)
#endif
    return false;
}

} // namespace Visnip
