#include "core/TranslationLanguage.h"

namespace Visnip {

const QVector<TranslationLanguage>& supportedTranslationLanguages()
{
    static const QVector<TranslationLanguage> languages = {
        { QStringLiteral("zh-Hans"), QStringLiteral("简体中文") },
        { QStringLiteral("en"), QStringLiteral("English") },
        { QStringLiteral("ja"), QStringLiteral("日本語") },
        { QStringLiteral("ko"), QStringLiteral("한국어") },
        { QStringLiteral("fr"), QStringLiteral("Français") },
        { QStringLiteral("de"), QStringLiteral("Deutsch") },
        { QStringLiteral("es"), QStringLiteral("Español") },
        { QStringLiteral("pt"), QStringLiteral("Português") },
        { QStringLiteral("it"), QStringLiteral("Italiano") },
        { QStringLiteral("ru"), QStringLiteral("Русский") },
    };
    return languages;
}

bool isSupportedTranslationLanguage(const QString& code)
{
    const QString normalized = code.trimmed();
    for (const TranslationLanguage& language : supportedTranslationLanguages()) {
        if (language.code == normalized) {
            return true;
        }
    }
    return false;
}

QString normalizedTranslationLanguageCode(const QString& code)
{
    const QString normalized = code.trimmed();
    return isSupportedTranslationLanguage(normalized)
        ? normalized
        : supportedTranslationLanguages().first().code;
}

} // namespace Visnip
