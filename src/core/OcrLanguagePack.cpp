#include "core/OcrLanguagePack.h"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QProcessEnvironment>
#include <QStandardPaths>

namespace Visnip::Ocr {
namespace {

constexpr auto kPaddleOcrRevision =
    "2661c7c0ef5c613e8f93c6e93b2e052399f0f854";
constexpr auto kLegacyPackId = "legacy-v4";
constexpr auto kLegacyModel = "ch_PP-OCRv4_rec_infer.onnx";
constexpr auto kLegacyDictionary = "ppocr_keys_v1.txt";

QString ocrRoot()
{
    const QString override = QProcessEnvironment::systemEnvironment().value(
        QStringLiteral("VISNIP_OCR_DIR"));
    return override.isEmpty()
        ? QCoreApplication::applicationDirPath() + QStringLiteral("/ocr")
        : QDir(override).absolutePath();
}

QString userRoot()
{
    const QString base = QStandardPaths::writableLocation(
        QStandardPaths::AppLocalDataLocation);
    return QDir(base.isEmpty() ? QDir::homePath() : base)
        .filePath(QStringLiteral("ocr-packs"));
}

QString dictionaryUrl(const QString& file)
{
    return QStringLiteral(
               "https://raw.githubusercontent.com/PaddlePaddle/PaddleOCR/%1/ppocr/utils/dict/%2")
        .arg(QString::fromLatin1(kPaddleOcrRevision), file);
}

LanguagePack makePack(const char* id,
                      const char* displayName,
                      const char* description,
                      const char* repository,
                      const char* revision,
                      const char* modelSha256,
                      qint64 modelBytes,
                      const char* dictionaryFile,
                      const char* dictionarySha256,
                      int dictionaryEntries,
                      bool bundled,
                      bool rightToLeft = false)
{
    return {
        QString::fromLatin1(id),
        QString::fromUtf8(displayName),
        QString::fromUtf8(description),
        QString::fromLatin1(repository),
        QString::fromLatin1(revision),
        QString::fromLatin1(modelSha256),
        modelBytes,
        QString::fromLatin1(dictionaryFile),
        QString::fromLatin1(dictionarySha256),
        dictionaryEntries,
        bundled,
        rightToLeft,
        false,
    };
}

LanguagePack legacyPack()
{
    LanguagePack pack;
    pack.id = QString::fromLatin1(kLegacyPackId);
    pack.displayName = QStringLiteral("兼容模型（PP-OCRv4 中英文）");
    pack.description = QStringLiteral("现有安装中的旧版中英文识别模型");
    pack.dictionaryFile = QString::fromLatin1(kLegacyDictionary);
    pack.bundled = true;
    pack.legacy = true;
    return pack;
}

QString pathFor(const LanguagePack& pack, const QString& file)
{
    if (pack.legacy) {
        return QDir(ocrRoot()).filePath(file);
    }
    const QString userPath = QDir(userPackDirectory(pack.id)).filePath(file);
    if (QFileInfo::exists(userPath)) {
        return userPath;
    }
    return QDir(bundledPackDirectory(pack.id)).filePath(file);
}

} // namespace

QString LanguagePack::cacheKey() const
{
    return id + QLatin1Char('|')
        + (legacy ? QStringLiteral("legacy") : modelRevision.left(12))
        + QLatin1Char('|') + modelSha256.left(16)
        + QLatin1Char('|') + dictionarySha256.left(16);
}

QStringList LanguagePack::modelUrls() const
{
    if (legacy || modelRepository.isEmpty()) {
        return {};
    }
    const QString suffix = QStringLiteral("/PaddlePaddle/%1/resolve/%2/inference.onnx")
                               .arg(modelRepository, modelRevision);
    return {
        QStringLiteral("https://huggingface.co") + suffix,
        QStringLiteral("https://hf-mirror.com") + suffix,
    };
}

QStringList LanguagePack::dictionaryUrls() const
{
    if (legacy || dictionaryFile.isEmpty()) {
        return {};
    }
    return {
        dictionaryUrl(dictionaryFile),
        QStringLiteral("https://cdn.jsdelivr.net/gh/PaddlePaddle/PaddleOCR@%1/ppocr/utils/dict/%2")
            .arg(QString::fromLatin1(kPaddleOcrRevision), dictionaryFile),
    };
}

QString defaultLanguagePackId()
{
    return QStringLiteral("general-v5");
}

const QVector<LanguagePack>& languagePacks()
{
    static const QVector<LanguagePack> packs = {
        makePack("general-v5", "通用（中文 / 英文 / 日文）",
                 "PP-OCRv5 通用模型，适合中文、繁体中文、英文和日文",
                 "PP-OCRv5_mobile_rec_onnx",
                 "ed152b8b495f84de93cda5709d768548a9127622",
                 "da72dc72ca4dc220df0dfde68c1dedc31c58d3e76a25871122e5056227d50092",
                 16534782, "ppocrv5_dict.txt",
                 "d1979e9f794c464c0d2e0b70a7fe14dd978e9dc644c0e71f14158cdf8342af1b",
                 18383, true),
        makePack("korean-v5", "韩文（韩文 / 英文）",
                 "PP-OCRv5 韩文模型，支持韩文和英文",
                 "korean_PP-OCRv5_mobile_rec_onnx",
                 "5c6f574b8e2230adf4287b33e736d71b9fabd28e",
                 "92f0b7785e64fc9090106a241cf4c1eb97472824558272751b88a2a4476d3a08",
                 13418787, "ppocrv5_korean_dict.txt",
                 "a88071c68c01707489baa79ebe0405b7beb5cca229f4fc94cc3ef992328802d7",
                 11945, false),
        makePack("latin-v5", "拉丁字母（欧洲 / 越南语等）",
                 "PP-OCRv5 拉丁模型，支持法德西葡意、越南语等拉丁文字",
                 "latin_PP-OCRv5_mobile_rec_onnx",
                 "89d3a50e2c27e2e7cceeab0e944c25c807d5db4f",
                 "7888113072263cb471b93f66dd5e2ad70548dc526fa1ace760d0d973dd121498",
                 8042023, "ppocrv5_latin_dict.txt",
                 "ccbcc45730b3fbbd9050c5bc74db6a99067141ef1035e3d14889a84a6b9b1aff",
                 836, false),
        makePack("cyrillic-v5", "斯拉夫字母（俄文 / 乌克兰文等）",
                 "PP-OCRv5 西里尔模型，支持俄文、乌克兰文等",
                 "cyrillic_PP-OCRv5_mobile_rec_onnx",
                 "2cef88145434beb8afa9dd82d77d799eb1ad7b29",
                 "5371ee1ddaa7983cc62d0818d99e982b6804638c85e4f960d59a574094e172e5",
                 8048799, "ppocrv5_cyrillic_dict.txt",
                 "db40aa52ceb112055be80c694afdf655d5d2c4f7873704524cc16a447ca913ba",
                 850, false),
        makePack("arabic-v5", "阿拉伯字母（阿拉伯 / 波斯 / 乌尔都等）",
                 "PP-OCRv5 阿拉伯模型，支持阿拉伯文、波斯文和乌尔都文等",
                 "arabic_PP-OCRv5_mobile_rec_onnx",
                 "14aaedcd75825982689ecf5cd64ab33ee083215a",
                 "799113ebf267fbe742deb99eb36e8d42c9ddc5291ceacf92add41b4d52a59110",
                 7998947, "ppocrv5_arabic_dict.txt",
                 "7f92f7dbb9b75a4787a83bfb4f6d14a8ab515525130c9d40a9036f61cf6999e9",
                 747, false, true),
    };
    return packs;
}

const LanguagePack* languagePack(const QString& id)
{
    const QString normalized = id.trimmed().toLower();
    for (const LanguagePack& pack : languagePacks()) {
        if (pack.id == normalized) {
            return &pack;
        }
    }
    static const LanguagePack legacy = legacyPack();
    return normalized == legacy.id ? &legacy : nullptr;
}

QString normalizedLanguagePackId(const QString& id)
{
    const LanguagePack* pack = languagePack(id);
    return pack && !pack->legacy ? pack->id : defaultLanguagePackId();
}

QString bundledPackDirectory(const QString& id)
{
    return QDir(ocrRoot()).filePath(QStringLiteral("rec/%1").arg(id));
}

QString userPackDirectory(const QString& id)
{
    return QDir(userRoot()).filePath(id);
}

QString packModelPath(const LanguagePack& pack)
{
    return pack.legacy
        ? QDir(ocrRoot()).filePath(QString::fromLatin1(kLegacyModel))
        : pathFor(pack, QStringLiteral("model.onnx"));
}

QString packDictionaryPath(const LanguagePack& pack)
{
    return pathFor(pack, pack.dictionaryFile);
}

bool verifyFile(const QString& path,
                qint64 expectedBytes,
                const QString& expectedSha256,
                QString* error)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        if (error) {
            *error = QStringLiteral("无法读取：%1").arg(path);
        }
        return false;
    }
    if (expectedBytes > 0 && file.size() != expectedBytes) {
        if (error) {
            *error = QStringLiteral("文件大小不匹配：%1").arg(path);
        }
        return false;
    }
    if (!expectedSha256.isEmpty()) {
        QCryptographicHash hash(QCryptographicHash::Sha256);
        if (!hash.addData(&file)
            || QString::fromLatin1(hash.result().toHex()) != expectedSha256) {
            if (error) {
                *error = QStringLiteral("文件校验失败：%1").arg(path);
            }
            return false;
        }
    }
    return true;
}

bool languagePackInstalled(const QString& id,
                           QString* missingOrInvalid,
                           bool verifyChecksum)
{
    const LanguagePack* pack = languagePack(id);
    if (!pack) {
        if (missingOrInvalid) {
            *missingOrInvalid = id;
        }
        return false;
    }
    const QString model = packModelPath(*pack);
    const QString dictionary = packDictionaryPath(*pack);
    if (!QFileInfo(model).isFile()) {
        if (missingOrInvalid) {
            *missingOrInvalid = model;
        }
        return false;
    }
    if (!QFileInfo(dictionary).isFile()) {
        if (missingOrInvalid) {
            *missingOrInvalid = dictionary;
        }
        return false;
    }
    if (verifyChecksum && !pack->legacy) {
        QString error;
        if (!verifyFile(model, pack->modelBytes, pack->modelSha256, &error)
            || !verifyFile(dictionary, 0, pack->dictionarySha256, &error)) {
            if (missingOrInvalid) {
                *missingOrInvalid = error;
            }
            return false;
        }
    }
    return true;
}

QString resolvedLanguagePackId(const QString& requestedId)
{
    const QString requested = normalizedLanguagePackId(requestedId);
    if (languagePackInstalled(requested)) {
        return requested;
    }
    if (requested == defaultLanguagePackId()
        && languagePackInstalled(QString::fromLatin1(kLegacyPackId))) {
        return QString::fromLatin1(kLegacyPackId);
    }
    return requested;
}

QString languagePackCacheKey(const QString& requestedId)
{
    const QString resolved = resolvedLanguagePackId(requestedId);
    const LanguagePack* pack = languagePack(resolved);
    return pack ? pack->cacheKey() : resolved + QStringLiteral("|missing");
}

} // namespace Visnip::Ocr
