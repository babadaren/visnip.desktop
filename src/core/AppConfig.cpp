#include "core/AppConfig.h"

#include "core/OcrLanguagePack.h"
#include "core/TranslationLanguage.h"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QFileInfo>
#include <QDebug>
#include <QSettings>
#include <QStandardPaths>
#include <QUrl>
#include <QStringList>
#include <QtGlobal>

#ifdef Q_OS_WIN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <wincrypt.h>
#endif

namespace Visnip {

namespace {
constexpr auto kOrg = "Visnip";
constexpr auto kApp = "Visnip";
// Visnip was called Vislate before 0.4.0; its settings, auto-start entry and
// data folders are taken over on the first start.
constexpr auto kLegacyOrg = "Vislate";
constexpr auto kLegacyApp = "Vislate";
constexpr auto kProtectedSecretPrefix = "dpapi:";

TranslationMethod translationMethodFromStoredSettings(QSettings& settings,
                                                       bool* migrated)
{
    QString storedMethod;
    if (settings.contains(QStringLiteral("translationMethod"))) {
        storedMethod = settings.value(QStringLiteral("translationMethod")).toString();
    } else if (settings.contains(QStringLiteral("fastPipeline"))) {
        storedMethod = settings.value(QStringLiteral("fastPipeline")).toString();
        *migrated = true;
    } else if (settings.contains(QStringLiteral("mode"))) {
        // Before cloud image translation existed, fast meant local OCR while
        // hifi meant whole-image processing. Preserve that user choice.
        storedMethod = settings.value(QStringLiteral("mode")).toString()
                               .trimmed().compare(QStringLiteral("hifi"),
                                                  Qt::CaseInsensitive) == 0
            ? QStringLiteral("cloud-baidu")
            : QStringLiteral("local-ocr");
        *migrated = true;
    } else {
        return TranslationMethod::Offline; // fresh install: no uploads by default
    }

    storedMethod = storedMethod.trimmed().toLower();
    if (storedMethod == QStringLiteral("local-ocr")) {
        return TranslationMethod::LocalOcr;
    }
    if (storedMethod == QStringLiteral("intranet")) {
        return TranslationMethod::Intranet;
    }
    if (storedMethod == QStringLiteral("offline")) {
        return TranslationMethod::Offline;
    }
    if (storedMethod == QStringLiteral("cloud-image")) {
        return TranslationMethod::CloudImage;
    }
    *migrated = true;
    if (storedMethod == QStringLiteral("cloud-baidu")) {
        return TranslationMethod::CloudImage;
    }
    // Unknown/future/corrupted modes must not silently enable uploading.
    return TranslationMethod::Offline;
}

bool removeObsoleteAiTranslationSettings(QSettings& settings)
{
    static const QStringList obsoleteKeys = {
        QStringLiteral("useOfficialApi"),
        QStringLiteral("provider"),
        QStringLiteral("customBaseUrl"),
        QStringLiteral("apiKey"),
        QStringLiteral("model"),
        QStringLiteral("mode"),
        QStringLiteral("fastPipeline"),
    };
    bool removed = false;
    for (const QString& key : obsoleteKeys) {
        if (settings.contains(key)) {
            settings.remove(key);
            removed = true;
        }
    }
    return removed;
}

QString genericLocalDataDirectory()
{
    return qEnvironmentVariable("LOCALAPPDATA", QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation));
}

// Takes over the settings of an earlier Vislate installation once, while the
// Visnip store is still empty. Nothing is moved or deleted: offline resources
// keep running from the folder Vislate installed them into.
void migrateLegacyInstallation(QSettings& settings)
{
    if (!qEnvironmentVariableIsEmpty("VISNIP_TEST_SETTINGS_FILE")) {
        return;
    }
    QSettings legacy(QString::fromLatin1(kLegacyOrg), QString::fromLatin1(kLegacyApp));
    if (!copyLegacySettings(legacy, settings)) {
        return;
    }
    const QString legacyResources = QDir(genericLocalDataDirectory())
        .filePath(QStringLiteral("%1/offline/1").arg(QLatin1String(kLegacyOrg)));
    if (settings.value(QStringLiteral("aiTranslate/offlineResourceDirectory")).toString().trimmed().isEmpty()
        && QFileInfo::exists(QDir(legacyResources).filePath(QStringLiteral("vislate-managed.json")))) {
        settings.setValue(QStringLiteral("aiTranslate/offlineResourceDirectory"), legacyResources);
    }
    // Downloaded OCR language packs; when the move fails they are fetched again on demand.
    const QString appData = QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation);
    const QString legacyAppData = QDir(QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation))
        .filePath(QStringLiteral("%1/%2").arg(QLatin1String(kLegacyOrg), QLatin1String(kLegacyApp)));
    const QString legacyPacks = QDir(legacyAppData).filePath(QStringLiteral("ocr-packs"));
    const QString packs = QDir(appData).filePath(QStringLiteral("ocr-packs"));
    if (!appData.isEmpty() && QFileInfo(legacyPacks).isDir() && !QFileInfo::exists(packs)
        && QDir().mkpath(appData) && !QDir().rename(legacyPacks, packs)) {
        qWarning() << "Could not move the OCR language packs from" << legacyPacks;
    }
    settings.sync();
    qInfo() << "Took over the settings of the earlier Vislate installation";
}

QSettings makeSettings()
{
    const QByteArray testSettingsFile = qgetenv("VISNIP_TEST_SETTINGS_FILE");
    if (!testSettingsFile.isEmpty()) {
        return QSettings(QString::fromLocal8Bit(testSettingsFile), QSettings::IniFormat);
    }
    return QSettings(QString::fromLatin1(kOrg), QString::fromLatin1(kApp));
}

QString colorToString(const QColor& color, const QString& fallback)
{
    return color.isValid() ? color.name(QColor::HexRgb) : fallback;
}

QColor colorFromString(const QString& value, const QColor& fallback)
{
    const QColor color(value);
    return color.isValid() ? color : fallback;
}

int normalizedMaskAlpha(int value, int fallback)
{
    if (value <= 0) {
        return fallback;
    }
    return qBound(112, value, 180);
}
} // namespace

QString translationMethodId(TranslationMethod method)
{
    switch (method) {
    case TranslationMethod::LocalOcr: return QStringLiteral("local-ocr");
    case TranslationMethod::CloudImage: return QStringLiteral("cloud-image");
    case TranslationMethod::Intranet: return QStringLiteral("intranet");
    case TranslationMethod::Offline: return QStringLiteral("offline");
    }
    return QStringLiteral("offline");
}

QString defaultOfflineResourceDirectory()
{
    return QDir(genericLocalDataDirectory()).filePath(QStringLiteral("Visnip/offline/1"));
}

bool copyLegacySettings(const QSettings& from, QSettings& to)
{
    const QStringList keys = from.allKeys();
    if (keys.isEmpty() || !to.allKeys().isEmpty()) {
        return false;
    }
    for (const QString& key : keys) {
        to.setValue(key, from.value(key));
    }
    return true;
}

#ifndef VISNIP_ONLINE_TRANSLATION
#define VISNIP_ONLINE_TRANSLATION 0
#endif
#ifndef VISNIP_DEFAULT_SERVICE_URL
#define VISNIP_DEFAULT_SERVICE_URL ""
#endif

bool onlineTranslationEnabled()
{
    return VISNIP_ONLINE_TRANSLATION != 0;
}

QString aiTranslateDefaultFastServiceUrl()
{
    return QString::fromUtf8(VISNIP_DEFAULT_SERVICE_URL);
}

QString questionApiFormatId(QuestionApiFormat format)
{
    return format == QuestionApiFormat::Anthropic
        ? QStringLiteral("anthropic")
        : QStringLiteral("openai");
}

QString aiTranslateNormalizedFastServiceUrl(const QString& raw, bool useDefaultWhenEmpty)
{
    static const QStringList knownSuffixes = {
        QStringLiteral("/v1/image-translate"),
        QStringLiteral("/v1/translate"),
        QStringLiteral("/healthz"),
    };
    QString url = raw.trimmed();
    while (url.endsWith(QLatin1Char('/'))) {
        url.chop(1);
    }
    for (const QString& suffix : knownSuffixes) {
        if (url.endsWith(suffix, Qt::CaseInsensitive)) {
            url.chop(suffix.size());
            break;
        }
    }
    while (url.endsWith(QLatin1Char('/'))) {
        url.chop(1);
    }
    return url.isEmpty() && useDefaultWhenEmpty ? aiTranslateDefaultFastServiceUrl() : url;
}

QString AiTranslateSettings::fastServiceBaseUrl() const
{
    if (!allowsTranslationNetwork()) {
        return {};
    }
    if (translationMethod == TranslationMethod::Intranet) {
        return intranetServiceUrl.trimmed().isEmpty()
            ? QString()
            : aiTranslateNormalizedFastServiceUrl(intranetServiceUrl, false);
    }
    return aiTranslateNormalizedFastServiceUrl(fastServiceUrl);
}

QString AiTranslateSettings::fastTranslateEndpoint() const
{
    const QString base = fastServiceBaseUrl();
    return base.isEmpty() ? QString() : base + QStringLiteral("/v1/translate");
}

QString AiTranslateSettings::fastImageTranslateEndpoint() const
{
    const QString base = fastServiceBaseUrl();
    return base.isEmpty() ? QString() : base + QStringLiteral("/v1/image-translate");
}

QString AiTranslateSettings::fastHealthEndpoint() const
{
    const QString base = fastServiceBaseUrl();
    return base.isEmpty() ? QString() : base + QStringLiteral("/healthz");
}

QString AiTranslateSettings::serviceToken() const
{
    switch (translationMethod) {
    case TranslationMethod::Intranet:
        return intranetServiceToken.trimmed();
    case TranslationMethod::CloudImage:
    case TranslationMethod::LocalOcr:
        return cloudServiceToken.trimmed();
    default:
        return {};
    }
}

QByteArray AiTranslateSettings::serviceAuthorization(const QUrl& endpoint, QString* problem) const
{
    if (problem) {
        problem->clear();
    }
    const QString token = serviceToken();
    if (token.isEmpty() || !allowsTranslationNetwork()) {
        return {};
    }
    const auto withhold = [problem](const QString& reason) {
        if (problem) {
            *problem = reason;
        }
        return QByteArray();
    };
    for (const QChar ch : token) {
        if (ch.unicode() < 0x21 || ch.unicode() > 0x7E) {
            return withhold(QStringLiteral("API 令牌只能包含可见的 ASCII 字符，请在首选项「翻译」中重新填写。"));
        }
    }
    const QString scheme = endpoint.scheme().toLower();
    const QString host = endpoint.host().toLower();
    const bool loopback = host == QStringLiteral("localhost") || host == QStringLiteral("::1")
        || host.startsWith(QStringLiteral("127."));
    const bool intranet = translationMethod == TranslationMethod::Intranet;
    if (scheme != QStringLiteral("https") && !(scheme == QStringLiteral("http") && (loopback || intranet))) {
        return withhold(QStringLiteral("已配置 API 令牌，但服务地址不是 HTTPS；为防止泄露，云端令牌只通过 HTTPS 发送。"));
    }
    return QByteArrayLiteral("Bearer ") + token.toLatin1();
}

QString AiTranslateSettings::translationMethodCacheKey() const
{
    if (!allowsTranslationNetwork()) {
        return QStringLiteral("offline|") + (usesLiteOfflineEngine() ? QStringLiteral("lite-v1|") : QStringLiteral("engine-v1|"))
            + offlineResourceDirectory + QLatin1Char('|') + offlineQuality;
    }
    if (usesCloudImageTranslation()) {
        return translationMethodId(translationMethod)
            + QLatin1Char('|') + fastImageTranslateEndpoint();
    }
    return translationMethodId(translationMethod) + QLatin1Char('|')
        + fastProviderCacheKey()
        + QLatin1Char('|')
        + fastOcrPackCacheKey();
}

QString AiTranslateSettings::fastProviderCacheKey() const
{
    if (fastProvider != QStringLiteral("baidu")) {
        return QStringLiteral("official|") + fastServiceBaseUrl();
    }
    const QByteArray credentialFingerprint = QCryptographicHash::hash(
        (baiduAppId + QLatin1Char('\n') + baiduSecretKey).toUtf8(),
        QCryptographicHash::Sha256).toHex().left(16);
    return fastProvider
        + QLatin1Char('|')
        + fastServiceBaseUrl()
        + QLatin1Char('|')
        + QString::fromLatin1(credentialFingerprint);
}

QString AiTranslateSettings::fastOcrPackCacheKey() const
{
    return Ocr::languagePackCacheKey(fastOcrPackId);
}

AppConfig::AppConfig(QObject* parent)
    : QObject(parent)
{
    settings_.output.saveDirectory = defaultSaveDirectory();
}

QString AppConfig::defaultSaveDirectory() const
{
    const QString pictures = QStandardPaths::writableLocation(QStandardPaths::PicturesLocation);
    const QString base = pictures.isEmpty() ? QDir::homePath() : pictures;
    return QDir(base).filePath(QStringLiteral("Visnip"));
}

void AppConfig::resetDefaults()
{
    settings_ = AppSettings{};
    settings_.output.saveDirectory = defaultSaveDirectory();
}

void AppConfig::load()
{
    AppSettings defaults;
    defaults.output.saveDirectory = defaultSaveDirectory();

    auto s = makeSettings();
    migrateLegacyInstallation(s);
    s.beginGroup(QStringLiteral("general"));
    settings_.autoStart = s.value(QStringLiteral("autoStart"), defaults.autoStart).toBool();
    s.endGroup();

    s.beginGroup(QStringLiteral("ui"));
    settings_.ui.darkToolbar = s.value(QStringLiteral("darkToolbar"), defaults.ui.darkToolbar).toBool();
    s.endGroup();

    s.beginGroup(QStringLiteral("capture"));
    settings_.capture.showMagnifier = s.value(QStringLiteral("showMagnifier"), defaults.capture.showMagnifier).toBool();
    settings_.capture.showCursorColor = s.value(QStringLiteral("showCursorColor"), defaults.capture.showCursorColor).toBool();
    settings_.capture.showSizeLabel = s.value(QStringLiteral("showSizeLabel"), defaults.capture.showSizeLabel).toBool();
    settings_.capture.borderWidth = qBound(1, s.value(QStringLiteral("borderWidth"), defaults.capture.borderWidth).toInt(), 6);
    settings_.capture.borderColor = colorFromString(s.value(QStringLiteral("borderColor"), colorToString(defaults.capture.borderColor, QStringLiteral("#4F7CFF"))).toString(), defaults.capture.borderColor);
    const int maskAlpha = normalizedMaskAlpha(s.value(QStringLiteral("maskAlpha"), defaults.capture.maskColor.alpha()).toInt(), defaults.capture.maskColor.alpha());
    settings_.capture.maskColor = QColor(0, 0, 0, maskAlpha);
    s.endGroup();

    s.beginGroup(QStringLiteral("pin"));
    settings_.pin.alwaysOnTop = s.value(QStringLiteral("alwaysOnTop"), defaults.pin.alwaysOnTop).toBool();
    settings_.pin.shadow = s.value(QStringLiteral("shadow"), defaults.pin.shadow).toBool();
    settings_.pin.doubleClickHide = s.value(QStringLiteral("doubleClickHide"), defaults.pin.doubleClickHide).toBool();
    settings_.pin.defaultOpacity = qBound(
        0.2,
        s.value(QStringLiteral("defaultOpacity"), defaults.pin.defaultOpacity).toDouble(),
        1.0);
    settings_.pin.wheelScaleStep = qBound(
        0.01,
        s.value(QStringLiteral("wheelScaleStep"), defaults.pin.wheelScaleStep).toDouble(),
        0.5);
    s.endGroup();

    s.beginGroup(QStringLiteral("output"));
    settings_.output.saveDirectory = s.value(QStringLiteral("saveDirectory"), defaults.output.saveDirectory).toString();
    settings_.output.filenamePattern = s.value(QStringLiteral("filenamePattern"), defaults.output.filenamePattern).toString();
    settings_.output.copyThenExit = s.value(QStringLiteral("copyThenExit"), defaults.output.copyThenExit).toBool();
    settings_.output.showSaveNotification = s.value(QStringLiteral("showSaveNotification"), defaults.output.showSaveNotification).toBool();
    s.endGroup();

    s.beginGroup(QStringLiteral("hotkeys"));
    settings_.hotkeys.capture = keySequenceFromText(s.value(QStringLiteral("capture"), keySequenceToText(defaults.hotkeys.capture)).toString(), defaults.hotkeys.capture);
    settings_.hotkeys.pinClipboard = keySequenceFromText(s.value(QStringLiteral("pinClipboard"), keySequenceToText(defaults.hotkeys.pinClipboard)).toString(), defaults.hotkeys.pinClipboard);
    settings_.hotkeys.repeatCapture = keySequenceFromText(s.value(QStringLiteral("repeatCapture"), keySequenceToText(defaults.hotkeys.repeatCapture)).toString(), defaults.hotkeys.repeatCapture);
    settings_.hotkeys.togglePins = keySequenceFromText(s.value(QStringLiteral("togglePins"), keySequenceToText(defaults.hotkeys.togglePins)).toString(), defaults.hotkeys.togglePins);
    settings_.hotkeys.toggleMouseThrough = keySequenceFromText(s.value(QStringLiteral("toggleMouseThrough"), keySequenceToText(defaults.hotkeys.toggleMouseThrough)).toString(), defaults.hotkeys.toggleMouseThrough);
    settings_.hotkeys.askQuestion = keySequenceFromText(s.value(QStringLiteral("askQuestion"), keySequenceToText(defaults.hotkeys.askQuestion)).toString(), defaults.hotkeys.askQuestion);
    s.endGroup();

    s.beginGroup(QStringLiteral("tools"));
    {
        auto& t = settings_.tools;
        const auto& td = defaults.tools;
        t.shapeStrokeColor = colorFromString(s.value(QStringLiteral("shapeStrokeColor"), colorToString(td.shapeStrokeColor, QString())).toString(), td.shapeStrokeColor);
        t.shapeFillColor = colorFromString(s.value(QStringLiteral("shapeFillColor"), colorToString(td.shapeFillColor, QString())).toString(), td.shapeFillColor);
        t.shapeStrokeWidth = qBound(1, s.value(QStringLiteral("shapeStrokeWidth"), td.shapeStrokeWidth).toInt(), 12);
        t.shapeFilled = s.value(QStringLiteral("shapeFilled"), td.shapeFilled).toBool();
        t.shapeFillAlpha = qBound(16, s.value(QStringLiteral("shapeFillAlpha"), td.shapeFillAlpha).toInt(), 255);
        t.arrowColor = colorFromString(s.value(QStringLiteral("arrowColor"), colorToString(td.arrowColor, QString())).toString(), td.arrowColor);
        t.arrowWidth = qBound(1, s.value(QStringLiteral("arrowWidth"), td.arrowWidth).toInt(), 12);
        t.arrowHeadMode = static_cast<ArrowHeadMode>(qBound(0, s.value(QStringLiteral("arrowHeadMode"), static_cast<int>(td.arrowHeadMode)).toInt(), 2));
        t.penColor = colorFromString(s.value(QStringLiteral("penColor"), colorToString(td.penColor, QString())).toString(), td.penColor);
        t.penWidth = qBound(1, s.value(QStringLiteral("penWidth"), td.penWidth).toInt(), 12);
        t.textColor = colorFromString(s.value(QStringLiteral("textColor"), colorToString(td.textColor, QString())).toString(), td.textColor);
        t.textFontSize = qBound(6, s.value(QStringLiteral("textFontSize"), td.textFontSize).toInt(), 96);
        const QString family = s.value(QStringLiteral("textFontFamily"), td.textFontFamily).toString();
        t.textFontFamily = family.trimmed().isEmpty() ? td.textFontFamily : family;
        t.textBold = s.value(QStringLiteral("textBold"), td.textBold).toBool();
        t.textItalic = s.value(QStringLiteral("textItalic"), td.textItalic).toBool();
        t.textOutline = s.value(QStringLiteral("textOutline"), td.textOutline).toBool();
        t.numberColor = colorFromString(s.value(QStringLiteral("numberColor"), colorToString(td.numberColor, QString())).toString(), td.numberColor);
        t.mosaicPaintMode = static_cast<MosaicPaintMode>(qBound(0, s.value(QStringLiteral("mosaicPaintMode"), static_cast<int>(td.mosaicPaintMode)).toInt(), 1));
        t.mosaicEffectMode = static_cast<MosaicEffectMode>(qBound(0, s.value(QStringLiteral("mosaicEffectMode"), static_cast<int>(td.mosaicEffectMode)).toInt(), 1));
        t.mosaicStrength = qBound(3, s.value(QStringLiteral("mosaicStrength"), td.mosaicStrength).toInt(), 24);
        t.eraserPaintMode = static_cast<MosaicPaintMode>(qBound(0, s.value(QStringLiteral("eraserPaintMode"), static_cast<int>(td.eraserPaintMode)).toInt(), 1));
        t.eraserSize = qBound(4, s.value(QStringLiteral("eraserSize"), td.eraserSize).toInt(), 96);
    }
    s.endGroup();

    s.beginGroup(QStringLiteral("aiTranslate"));
    bool migratedTranslationSettings = false;
    {
        const QString lang = s.value(QStringLiteral("targetLanguage"), defaults.aiTranslate.targetLanguage).toString().trimmed();
        settings_.aiTranslate.targetLanguage = normalizedTranslationLanguageCode(lang);
        // Offline translation is qualified for Chinese and English only.
        if (!onlineTranslationEnabled() && settings_.aiTranslate.targetLanguage != QStringLiteral("en")) {
            settings_.aiTranslate.targetLanguage = QStringLiteral("zh-Hans");
        }
        if (lang != settings_.aiTranslate.targetLanguage) {
            s.setValue(QStringLiteral("targetLanguage"),
                       settings_.aiTranslate.targetLanguage);
            migratedTranslationSettings = true;
        }
    }
    settings_.aiTranslate.fastOcrPackId = Ocr::normalizedLanguagePackId(
        s.value(QStringLiteral("fastOcrPackId"), defaults.aiTranslate.fastOcrPackId)
            .toString());
    settings_.aiTranslate.timeoutSeconds = qBound(30, s.value(QStringLiteral("timeoutSeconds"), defaults.aiTranslate.timeoutSeconds).toInt(), 300);
    settings_.aiTranslate.translationMethod =
        translationMethodFromStoredSettings(s, &migratedTranslationSettings);
    if (!onlineTranslationEnabled()) {
        // Settings from a build with online modes fall back to offline here.
        settings_.aiTranslate.translationMethod = TranslationMethod::Offline;
    }
    {
        const QString fastProvider = s.value(QStringLiteral("fastProvider"),
                                             defaults.aiTranslate.fastProvider)
                                         .toString()
                                         .trimmed()
                                         .toLower();
        settings_.aiTranslate.fastProvider = fastProvider == QStringLiteral("baidu")
            ? fastProvider
            : QStringLiteral("official");
    }
    settings_.aiTranslate.fastServiceUrl = s.value(QStringLiteral("fastServiceUrl"),
                                                   defaults.aiTranslate.fastServiceUrl)
                                               .toString()
                                               .trimmed();
    settings_.aiTranslate.intranetServiceUrl =
        s.value(QStringLiteral("intranetServiceUrl")).toString().trimmed();
    settings_.aiTranslate.cloudServiceToken =
        secretFromStorage(s.value(QStringLiteral("cloudServiceToken")).toString()).trimmed();
    settings_.aiTranslate.intranetServiceToken =
        secretFromStorage(s.value(QStringLiteral("intranetServiceToken")).toString()).trimmed();
    settings_.aiTranslate.offlineResourceDirectory = s.value(QStringLiteral("offlineResourceDirectory")).toString().trimmed();
    settings_.aiTranslate.offlineStorageDirectory = s.value(QStringLiteral("offlineStorageDirectory")).toString().trimmed();
    // "basic" was the earlier name of the resource package that the lite tier
    // runs from. Unknown values select the lite tier, which needs no Python.
    // Earlier clients stored "precise" for everyone; without an installed
    // resource directory that value was never a user choice.
    settings_.aiTranslate.offlineQuality =
        s.value(QStringLiteral("offlineQuality")).toString().trimmed() == QStringLiteral("precise")
            && !settings_.aiTranslate.offlineResourceDirectory.isEmpty()
        ? QStringLiteral("precise")
        : QStringLiteral("lite");
    settings_.aiTranslate.baiduAppId = s.value(QStringLiteral("baiduAppId"),
                                               defaults.aiTranslate.baiduAppId)
                                           .toString()
                                           .trimmed();
    settings_.aiTranslate.baiduSecretKey =
        secretFromStorage(s.value(QStringLiteral("baiduSecretKey")).toString()).trimmed();
    settings_.aiTranslate.cloudUploadConsent =
        qMax(0, s.value(QStringLiteral("cloudUploadConsent"), 0).toInt());
    settings_.aiTranslate.intranetUploadConsent =
        qMax(0, s.value(QStringLiteral("intranetUploadConsent"), 0).toInt());
    const QString methodId = translationMethodId(settings_.aiTranslate.translationMethod);
    if (s.value(QStringLiteral("translationMethod")).toString() != methodId) {
        s.setValue(QStringLiteral("translationMethod"), methodId);
        migratedTranslationSettings = true;
    }
    migratedTranslationSettings = removeObsoleteAiTranslationSettings(s)
        || migratedTranslationSettings;
    s.endGroup();

    s.beginGroup(QStringLiteral("question"));
    {
        auto& q = settings_.question;
        const auto& qd = defaults.question;
        const QString format = s.value(QStringLiteral("apiFormat"),
                                       questionApiFormatId(qd.apiFormat))
                                   .toString().trimmed().toLower();
        q.apiFormat = format == QStringLiteral("anthropic")
            ? QuestionApiFormat::Anthropic
            : QuestionApiFormat::OpenAiCompatible;
        q.apiUrl = s.value(QStringLiteral("apiUrl"), qd.apiUrl).toString().trimmed();
        q.apiKey = secretFromStorage(s.value(QStringLiteral("apiKey")).toString()).trimmed();
        q.model = s.value(QStringLiteral("model"), qd.model).toString().trimmed();
        q.customPrompt = s.value(QStringLiteral("customPrompt"), qd.customPrompt).toString();
        q.subjectScope = s.value(QStringLiteral("subjectScope"), qd.subjectScope).toString().trimmed();
        q.timeoutSeconds = qBound(30, s.value(QStringLiteral("timeoutSeconds"), qd.timeoutSeconds).toInt(), 600);
        q.useSystemProxy = s.value(QStringLiteral("useSystemProxy"), qd.useSystemProxy).toBool();
    }
    s.endGroup();

    if (migratedTranslationSettings) {
        s.sync();
        if (s.status() != QSettings::NoError) {
            qWarning() << "Failed to migrate translation settings" << s.status();
        }
    }

    emit changed();
}

bool AppConfig::save()
{
    auto s = makeSettings();
    s.beginGroup(QStringLiteral("general"));
    s.setValue(QStringLiteral("autoStart"), settings_.autoStart);
    s.endGroup();

    s.beginGroup(QStringLiteral("ui"));
    s.setValue(QStringLiteral("darkToolbar"), settings_.ui.darkToolbar);
    s.endGroup();

    s.beginGroup(QStringLiteral("capture"));
    s.remove(QStringLiteral("detectWindows"));
    s.setValue(QStringLiteral("showMagnifier"), settings_.capture.showMagnifier);
    s.setValue(QStringLiteral("showCursorColor"), settings_.capture.showCursorColor);
    s.setValue(QStringLiteral("showSizeLabel"), settings_.capture.showSizeLabel);
    s.setValue(QStringLiteral("borderWidth"), settings_.capture.borderWidth);
    s.setValue(QStringLiteral("borderColor"), colorToString(settings_.capture.borderColor, QStringLiteral("#4F7CFF")));
    s.setValue(QStringLiteral("maskAlpha"), settings_.capture.maskColor.alpha());
    s.endGroup();

    s.beginGroup(QStringLiteral("pin"));
    s.setValue(QStringLiteral("alwaysOnTop"), settings_.pin.alwaysOnTop);
    s.setValue(QStringLiteral("shadow"), settings_.pin.shadow);
    s.setValue(QStringLiteral("doubleClickHide"), settings_.pin.doubleClickHide);
    s.setValue(QStringLiteral("defaultOpacity"), settings_.pin.defaultOpacity);
    s.setValue(QStringLiteral("wheelScaleStep"), settings_.pin.wheelScaleStep);
    s.endGroup();

    s.beginGroup(QStringLiteral("output"));
    s.remove(QStringLiteral("format"));
    s.setValue(QStringLiteral("saveDirectory"), settings_.output.saveDirectory);
    s.setValue(QStringLiteral("filenamePattern"), settings_.output.filenamePattern);
    s.setValue(QStringLiteral("copyThenExit"), settings_.output.copyThenExit);
    s.setValue(QStringLiteral("showSaveNotification"), settings_.output.showSaveNotification);
    s.endGroup();

    s.beginGroup(QStringLiteral("hotkeys"));
    s.setValue(QStringLiteral("capture"), keySequenceToText(settings_.hotkeys.capture));
    s.setValue(QStringLiteral("pinClipboard"), keySequenceToText(settings_.hotkeys.pinClipboard));
    s.setValue(QStringLiteral("repeatCapture"), keySequenceToText(settings_.hotkeys.repeatCapture));
    s.setValue(QStringLiteral("togglePins"), keySequenceToText(settings_.hotkeys.togglePins));
    s.setValue(QStringLiteral("toggleMouseThrough"), keySequenceToText(settings_.hotkeys.toggleMouseThrough));
    s.setValue(QStringLiteral("askQuestion"), keySequenceToText(settings_.hotkeys.askQuestion));
    s.endGroup();

    s.beginGroup(QStringLiteral("tools"));
    {
        const auto& t = settings_.tools;
        s.setValue(QStringLiteral("shapeStrokeColor"), colorToString(t.shapeStrokeColor, QStringLiteral("#EF4444")));
        s.setValue(QStringLiteral("shapeFillColor"), colorToString(t.shapeFillColor, QStringLiteral("#EF4444")));
        s.setValue(QStringLiteral("shapeStrokeWidth"), t.shapeStrokeWidth);
        s.setValue(QStringLiteral("shapeFilled"), t.shapeFilled);
        s.setValue(QStringLiteral("shapeFillAlpha"), t.shapeFillAlpha);
        s.setValue(QStringLiteral("arrowColor"), colorToString(t.arrowColor, QStringLiteral("#EF4444")));
        s.setValue(QStringLiteral("arrowWidth"), t.arrowWidth);
        s.setValue(QStringLiteral("arrowHeadMode"), static_cast<int>(t.arrowHeadMode));
        s.setValue(QStringLiteral("penColor"), colorToString(t.penColor, QStringLiteral("#EF4444")));
        s.setValue(QStringLiteral("penWidth"), t.penWidth);
        s.setValue(QStringLiteral("textColor"), colorToString(t.textColor, QStringLiteral("#EF4444")));
        s.setValue(QStringLiteral("textFontSize"), t.textFontSize);
        s.setValue(QStringLiteral("textFontFamily"), t.textFontFamily);
        s.setValue(QStringLiteral("textBold"), t.textBold);
        s.setValue(QStringLiteral("textItalic"), t.textItalic);
        s.setValue(QStringLiteral("textOutline"), t.textOutline);
        s.setValue(QStringLiteral("numberColor"), colorToString(t.numberColor, QStringLiteral("#EF4444")));
        s.setValue(QStringLiteral("mosaicPaintMode"), static_cast<int>(t.mosaicPaintMode));
        s.setValue(QStringLiteral("mosaicEffectMode"), static_cast<int>(t.mosaicEffectMode));
        s.setValue(QStringLiteral("mosaicStrength"), t.mosaicStrength);
        s.setValue(QStringLiteral("eraserPaintMode"), static_cast<int>(t.eraserPaintMode));
        s.setValue(QStringLiteral("eraserSize"), t.eraserSize);
    }
    s.endGroup();

    s.beginGroup(QStringLiteral("aiTranslate"));
    s.setValue(QStringLiteral("targetLanguage"), settings_.aiTranslate.targetLanguage);
    s.setValue(QStringLiteral("fastOcrPackId"), settings_.aiTranslate.fastOcrPackId);
    s.setValue(QStringLiteral("timeoutSeconds"), settings_.aiTranslate.timeoutSeconds);
    s.setValue(QStringLiteral("translationMethod"),
               translationMethodId(settings_.aiTranslate.translationMethod));
    s.setValue(QStringLiteral("fastProvider"), settings_.aiTranslate.fastProvider);
    s.setValue(QStringLiteral("fastServiceUrl"), settings_.aiTranslate.fastServiceUrl);
    s.setValue(QStringLiteral("intranetServiceUrl"), settings_.aiTranslate.intranetServiceUrl);
    s.setValue(QStringLiteral("cloudServiceToken"), protectSecretForStorage(settings_.aiTranslate.cloudServiceToken));
    s.setValue(QStringLiteral("intranetServiceToken"), protectSecretForStorage(settings_.aiTranslate.intranetServiceToken));
    s.setValue(QStringLiteral("offlineResourceDirectory"), settings_.aiTranslate.offlineResourceDirectory);
    s.setValue(QStringLiteral("offlineStorageDirectory"), settings_.aiTranslate.offlineStorageDirectory);
    s.setValue(QStringLiteral("offlineQuality"), settings_.aiTranslate.offlineQuality);
    s.setValue(QStringLiteral("baiduAppId"), settings_.aiTranslate.baiduAppId);
    s.setValue(QStringLiteral("baiduSecretKey"), protectSecretForStorage(settings_.aiTranslate.baiduSecretKey));
    s.setValue(QStringLiteral("cloudUploadConsent"), settings_.aiTranslate.cloudUploadConsent);
    s.setValue(QStringLiteral("intranetUploadConsent"), settings_.aiTranslate.intranetUploadConsent);
    removeObsoleteAiTranslationSettings(s);
    s.endGroup();

    s.beginGroup(QStringLiteral("question"));
    {
        const auto& q = settings_.question;
        s.setValue(QStringLiteral("apiFormat"), questionApiFormatId(q.apiFormat));
        s.setValue(QStringLiteral("apiUrl"), q.apiUrl);
        s.setValue(QStringLiteral("apiKey"), protectSecretForStorage(q.apiKey));
        s.setValue(QStringLiteral("model"), q.model);
        s.setValue(QStringLiteral("customPrompt"), q.customPrompt);
        s.setValue(QStringLiteral("subjectScope"), q.subjectScope);
        s.setValue(QStringLiteral("timeoutSeconds"), q.timeoutSeconds);
        s.setValue(QStringLiteral("useSystemProxy"), q.useSystemProxy);
    }
    s.endGroup();

    s.sync();
    const bool saved = s.status() == QSettings::NoError;
    if (!saved) {
        qWarning() << "Failed to persist application settings" << s.status();
    }
    if (saved) {
        emit changed();
    }
    return saved;
}

bool AppConfig::syncAutoStart() const
{
#ifdef Q_OS_WIN
    QSettings run(QStringLiteral("HKEY_CURRENT_USER\\Software\\Microsoft\\Windows\\CurrentVersion\\Run"), QSettings::NativeFormat);
    const QString key = QString::fromLatin1(kApp);
    run.remove(QString::fromLatin1(kLegacyApp));
    if (settings_.autoStart) {
        const QString exe = QDir::toNativeSeparators(QCoreApplication::applicationFilePath());
        run.setValue(key, QStringLiteral("\"%1\"").arg(exe));
    } else {
        run.remove(key);
    }
    run.sync();
    if (run.status() != QSettings::NoError) {
        qWarning() << "Failed to update auto-start registration" << run.status();
        return false;
    }
#endif
    return true;
}

QString keySequenceToText(const QKeySequence& sequence)
{
    return sequence.toString(QKeySequence::PortableText);
}

QKeySequence keySequenceFromText(const QString& text, const QKeySequence& fallback)
{
    const QKeySequence seq(text, QKeySequence::PortableText);
    return seq.isEmpty() ? fallback : seq;
}

QString protectSecretForStorage(const QString& secret)
{
    if (secret.isEmpty()) {
        return {};
    }
#ifdef Q_OS_WIN
    QByteArray plain = secret.toUtf8();
    DATA_BLOB input{static_cast<DWORD>(plain.size()),
                    reinterpret_cast<BYTE*>(plain.data())};
    DATA_BLOB output{};
    if (CryptProtectData(&input, L"Visnip", nullptr, nullptr, nullptr,
                         CRYPTPROTECT_UI_FORBIDDEN, &output)) {
        const QByteArray encrypted(reinterpret_cast<const char*>(output.pbData),
                                   static_cast<qsizetype>(output.cbData));
        LocalFree(output.pbData);
        return QString::fromLatin1(kProtectedSecretPrefix)
            + QString::fromLatin1(encrypted.toBase64());
    }
    qWarning() << "Failed to encrypt secret; storing it unprotected" << GetLastError();
#endif
    return secret;
}

QString secretFromStorage(const QString& stored)
{
    const QLatin1String prefix(kProtectedSecretPrefix);
    if (!stored.startsWith(prefix)) {
        return stored;
    }
#ifdef Q_OS_WIN
    QByteArray encrypted = QByteArray::fromBase64(stored.mid(prefix.size()).toLatin1());
    DATA_BLOB input{static_cast<DWORD>(encrypted.size()),
                    reinterpret_cast<BYTE*>(encrypted.data())};
    DATA_BLOB output{};
    if (CryptUnprotectData(&input, nullptr, nullptr, nullptr, nullptr,
                           CRYPTPROTECT_UI_FORBIDDEN, &output)) {
        const QString secret = QString::fromUtf8(
            reinterpret_cast<const char*>(output.pbData),
            static_cast<qsizetype>(output.cbData));
        SecureZeroMemory(output.pbData, output.cbData);
        LocalFree(output.pbData);
        return secret;
    }
    qWarning() << "Failed to decrypt stored secret" << GetLastError();
#endif
    // Encrypted for another Windows user or machine: unusable here.
    return {};
}

} // namespace Visnip
