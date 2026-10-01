#pragma once

#include <QString>

namespace Visnip {

enum class OutputFilenamePatternError {
    None,
    Empty,
    MissingTimestampPlaceholder,
    UnsupportedPlaceholder,
    ParentTraversal,
    PathSeparator,
    InvalidCharacter,
    UnsupportedExtension,
};

struct OutputFilenamePatternValidation {
    QString normalizedPattern;
    OutputFilenamePatternError error = OutputFilenamePatternError::None;
    QString message;

    [[nodiscard]] bool isValid() const noexcept
    {
        return error == OutputFilenamePatternError::None;
    }
};

// A valid pattern always produces one image filename inside the configured
// output directory. %1 is replaced with the capture timestamp by the caller.
[[nodiscard]] OutputFilenamePatternValidation validateOutputFilenamePattern(
    const QString& pattern);

} // namespace Visnip
