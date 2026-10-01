#pragma once

#include <QString>
#include <QStringList>
#include <QVector>

namespace Visnip::Ocr {

struct LanguagePack {
    QString id;
    QString displayName;
    QString description;
    QString modelRepository;
    QString modelRevision;
    QString modelSha256;
    qint64 modelBytes = 0;
    QString dictionaryFile;
    QString dictionarySha256;
    int dictionaryEntries = 0;
    bool bundled = false;
    bool rightToLeft = false;
    bool legacy = false;

    QString cacheKey() const;
    QStringList modelUrls() const;
    QStringList dictionaryUrls() const;
};

QString defaultLanguagePackId();
const QVector<LanguagePack>& languagePacks();
const LanguagePack* languagePack(const QString& id);
QString normalizedLanguagePackId(const QString& id);

QString bundledPackDirectory(const QString& id);
QString userPackDirectory(const QString& id);
QString packModelPath(const LanguagePack& pack);
QString packDictionaryPath(const LanguagePack& pack);

bool verifyFile(const QString& path,
                qint64 expectedBytes,
                const QString& expectedSha256,
                QString* error = nullptr);
bool languagePackInstalled(const QString& id,
                           QString* missingOrInvalid = nullptr,
                           bool verifyChecksum = false);

// The built-in V5 general pack is preferred. A legacy root-level V4 bundle is
// accepted only as a migration fallback when that default pack is unavailable.
QString resolvedLanguagePackId(const QString& requestedId);
QString languagePackCacheKey(const QString& requestedId);

} // namespace Visnip::Ocr
