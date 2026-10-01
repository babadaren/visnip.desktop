#include "core/OutputFilenamePattern.h"

#include <QFileInfo>
#include <QRegularExpression>
#include <QStringList>

namespace {

using Visnip::OutputFilenamePatternError;
using Visnip::OutputFilenamePatternValidation;

OutputFilenamePatternValidation failure(OutputFilenamePatternError error,
                                        const QString& message)
{
    return { {}, error, message };
}

bool containsInvalidFilenameCharacter(const QString& pattern)
{
    static const QString invalid = QStringLiteral("<>:\"|?*");
    for (const QChar character : pattern) {
        if (character.unicode() < 0x20 || invalid.contains(character)) {
            return true;
        }
    }
    return false;
}

} // namespace

namespace Visnip {

OutputFilenamePatternValidation validateOutputFilenamePattern(
    const QString& pattern)
{
    QString normalized = pattern.trimmed();
    if (normalized.isEmpty()) {
        return failure(OutputFilenamePatternError::Empty,
                       QStringLiteral("文件名规则不能为空。"));
    }

    // Migrate the only previously documented legacy timestamp token. Other
    // patterns without %1 remain invalid instead of silently becoming a
    // different default filename.
    normalized.replace(QStringLiteral("$yyyy-MM-dd_HH-mm-ss$"),
                       QStringLiteral("%1"));

    QString pathLike = normalized;
    pathLike.replace(QChar(u'\\'), QChar(u'/'));
    const QStringList components = pathLike.split(QChar(u'/'), Qt::KeepEmptyParts);
    if (components.contains(QStringLiteral(".."))) {
        return failure(OutputFilenamePatternError::ParentTraversal,
                       QStringLiteral("文件名规则不能包含“..”路径段。"));
    }
    if (normalized.contains(QChar(u'/')) || normalized.contains(QChar(u'\\'))) {
        return failure(OutputFilenamePatternError::PathSeparator,
                       QStringLiteral("文件名规则不能包含路径分隔符。"));
    }
    if (containsInvalidFilenameCharacter(normalized)) {
        return failure(OutputFilenamePatternError::InvalidCharacter,
                       QStringLiteral("文件名规则包含系统不允许的字符。"));
    }

    static const QRegularExpression placeholderExpression(
        QStringLiteral("%([1-9][0-9]?)"));
    bool hasTimestampPlaceholder = false;
    auto matchIterator = placeholderExpression.globalMatch(normalized);
    while (matchIterator.hasNext()) {
        const auto match = matchIterator.next();
        if (match.captured(1) != QStringLiteral("1")) {
            return failure(OutputFilenamePatternError::UnsupportedPlaceholder,
                           QStringLiteral("文件名规则只支持时间占位符 %1。"));
        }
        hasTimestampPlaceholder = true;
    }
    if (!hasTimestampPlaceholder) {
        return failure(OutputFilenamePatternError::MissingTimestampPlaceholder,
                       QStringLiteral("文件名规则必须包含时间占位符 %1。"));
    }

    const QString suffix = QFileInfo(normalized).suffix();
    if (suffix.isEmpty()) {
        if (normalized.endsWith(QChar(u'.'))) {
            return failure(OutputFilenamePatternError::InvalidCharacter,
                           QStringLiteral("文件名规则不能以句点结尾。"));
        }
        normalized += QStringLiteral(".png");
    } else if (suffix.compare(QStringLiteral("png"), Qt::CaseInsensitive) != 0
               && suffix.compare(QStringLiteral("jpg"), Qt::CaseInsensitive) != 0
               && suffix.compare(QStringLiteral("jpeg"), Qt::CaseInsensitive) != 0) {
        return failure(OutputFilenamePatternError::UnsupportedExtension,
                       QStringLiteral("文件名扩展名只能是 .png、.jpg 或 .jpeg。"));
    }

    return { normalized, OutputFilenamePatternError::None, {} };
}

} // namespace Visnip
