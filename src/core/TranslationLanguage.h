#pragma once

#include <QString>
#include <QVector>

namespace Visnip {

struct TranslationLanguage {
    QString code;
    QString nativeName;
};

// Languages supported by both the local OCR pipeline and Baidu cloud image
// translation. Keeping one catalog prevents a mode switch from producing an
// invalid target-language configuration.
const QVector<TranslationLanguage>& supportedTranslationLanguages();
bool isSupportedTranslationLanguage(const QString& code);
QString normalizedTranslationLanguageCode(const QString& code);

} // namespace Visnip
