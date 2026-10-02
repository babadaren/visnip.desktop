#pragma once

#include "core/AnnotationModel.h"

#include <QByteArray>
#include <QColor>
#include <QKeySequence>
#include <QObject>
#include <QString>

class QSettings;
class QUrl;

namespace Visnip {

struct HotkeySettings {
    QKeySequence capture = QKeySequence(Qt::Key_F1);
    QKeySequence pinClipboard = QKeySequence(Qt::Key_F3);
    QKeySequence repeatCapture = QKeySequence(Qt::CTRL | Qt::Key_F1);
    QKeySequence togglePins = QKeySequence(Qt::SHIFT | Qt::Key_F3);
    QKeySequence toggleMouseThrough = QKeySequence(Qt::CTRL | Qt::Key_T);
    // Registered only while the question panel is open, so F4 stays with
    // other applications the rest of the time.
    QKeySequence askQuestion = QKeySequence(Qt::Key_F4);
};

struct CaptureSettings {
    bool showMagnifier = true;
    bool showCursorColor = true;
    bool showSizeLabel = true;
    int borderWidth = 2;
    QColor borderColor = QColor(QStringLiteral("#4F7CFF"));
    QColor maskColor = QColor(0, 0, 0, 118);
};

struct PinSettings {
    bool alwaysOnTop = true;
    bool shadow = true;
    bool doubleClickHide = true;
    double defaultOpacity = 1.0;
    double wheelScaleStep = 0.08;
};

struct OutputSettings {
    QString saveDirectory;
    QString filenamePattern = QStringLiteral("Visnip_%1.png");
    bool copyThenExit = true;
    bool showSaveNotification = true;
};

struct ToolStyleSettings {
    QColor shapeStrokeColor = QColor(239, 68, 68);
    QColor shapeFillColor = QColor(239, 68, 68);
    int shapeStrokeWidth = 2;
    bool shapeFilled = false;
    int shapeFillAlpha = 48;

    QColor arrowColor = QColor(239, 68, 68);
    int arrowWidth = 2;
    ArrowHeadMode arrowHeadMode = ArrowHeadMode::SingleArrow;

    QColor penColor = QColor(QStringLiteral("#EF4444"));
    int penWidth = 2;

    QColor textColor = QColor(QStringLiteral("#EF4444"));
    int textFontSize = 16;
    QString textFontFamily = QStringLiteral("Microsoft YaHei UI");
    bool textBold = false;
    bool textItalic = false;
    bool textOutline = false;

    QColor numberColor = QColor(QStringLiteral("#EF4444"));

    MosaicPaintMode mosaicPaintMode = MosaicPaintMode::Fill;
    MosaicEffectMode mosaicEffectMode = MosaicEffectMode::GaussianBlur;
    int mosaicStrength = 15;

    MosaicPaintMode eraserPaintMode = MosaicPaintMode::Brush;
    int eraserSize = 18;
};

struct UiSettings {
    bool darkToolbar = false;
};

enum class TranslationMethod {
    LocalOcr = 0, // Legacy hybrid: local OCR, ONLINE text translation.
    CloudImage = 1,
    CloudBaidu = CloudImage, // Source compatibility, not a provider restriction.
    Intranet = 2,
    Offline = 3,
};

QString translationMethodId(TranslationMethod method);

struct AiTranslateSettings {
    QString targetLanguage = QStringLiteral("zh-Hans");
    QString fastOcrPackId = QStringLiteral("general-v5");
    int timeoutSeconds = 120;
    // Nothing leaves the computer until the user picks an online mode and
    // accepts its upload notice.
    TranslationMethod translationMethod = TranslationMethod::Offline;
    // Local OCR uses the Visnip service by default. "baidu" sends OCR text
    // directly to Baidu's General Text Translation API; a failure is reported,
    // never retried through another receiver.
    QString fastProvider = QStringLiteral("official"); // "official" | "baidu"
    QString fastServiceUrl;
    // Kept separate: an empty private endpoint must never select the cloud.
    QString intranetServiceUrl;
    // Bearer tokens for our own translation service, one per receiver: the
    // cloud token is never sent to an intranet server and vice versa.
    QString cloudServiceToken;
    QString intranetServiceToken;
    // Version of the upload notice accepted per receiver (0 = never). The
    // hybrid mode sends recognized text to the cloud receiver.
    static constexpr int kUploadNoticeVersion = 1;
    int cloudUploadConsent = 0;
    int intranetUploadConsent = 0;
    QString offlineResourceDirectory;
    // Base folder for offline downloads and managed installations. Empty keeps
    // the default location under the user's local application data.
    QString offlineStorageDirectory;
    // "lite": local OCR + llama.cpp text translation + in-process refill.
    // "precise": the Python whole-image engine (Hi-SAM, LaMa, PyTorch).
    QString offlineQuality = QStringLiteral("lite");
    QString baiduAppId;
    QString baiduSecretKey;

    QString fastServiceBaseUrl() const;
    QString fastTranslateEndpoint() const;
    QString fastImageTranslateEndpoint() const;
    QString fastHealthEndpoint() const;
    // Token of the selected online mode; empty when offline.
    QString serviceToken() const;
    // "Bearer <token>" for a request to endpoint, or empty. A configured token
    // is withheld (and problem explains why) when it contains characters that
    // cannot appear in a header, or when a cloud token would travel over
    // plain HTTP to a remote host. Intranet servers may use explicit HTTP.
    QByteArray serviceAuthorization(const QUrl& endpoint, QString* problem = nullptr) const;
    bool usesCloudImageTranslation() const {
        return translationMethod == TranslationMethod::CloudImage
            || translationMethod == TranslationMethod::Intranet;
    }
    bool usesLegacyOnlineTextTranslation() const {
        return translationMethod == TranslationMethod::LocalOcr;
    }
    bool isOffline() const { return translationMethod == TranslationMethod::Offline; }
    bool usesLiteOfflineEngine() const { return isOffline() && offlineQuality != QStringLiteral("precise"); }
    bool usesPreciseOfflineEngine() const { return isOffline() && offlineQuality == QStringLiteral("precise"); }
    bool usesWholeImageTranslation() const { return usesCloudImageTranslation() || usesPreciseOfflineEngine(); }
    bool allowsTranslationNetwork() const {
        return usesCloudImageTranslation() || usesLegacyOnlineTextTranslation();
    }
    // True when the selected mode uploads nothing or its notice was accepted.
    bool uploadConsented() const {
        if (!allowsTranslationNetwork()) {
            return true;
        }
        return (translationMethod == TranslationMethod::Intranet ? intranetUploadConsent : cloudUploadConsent)
            >= kUploadNoticeVersion;
    }
    QString translationMethodCacheKey() const;
    QString fastProviderCacheKey() const;
    QString fastOcrPackCacheKey() const;
};

// Build-time switch (CMake VISNIP_ONLINE_TRANSLATION). Off in the open-source
// build: only offline translation is offered and nothing is uploaded.
bool onlineTranslationEnabled();
// Build-time default (CMake VISNIP_DEFAULT_SERVICE_URL); empty unless set.
QString aiTranslateDefaultFastServiceUrl();
// Managed offline resources live here unless a directory was imported.
QString defaultOfflineResourceDirectory();
// Copies every value of an earlier installation's store into an empty one;
// returns false and changes nothing when there is nothing to copy or the
// destination already has settings.
bool copyLegacySettings(const QSettings& from, QSettings& to);
// Accepts the service root as well as a pasted translation endpoint or
// /healthz URL and returns the root; falls back to the default when empty.
QString aiTranslateNormalizedFastServiceUrl(const QString& raw, bool useDefaultWhenEmpty = true);

enum class QuestionApiFormat {
    OpenAiCompatible,
    Anthropic,
};

QString questionApiFormatId(QuestionApiFormat format);

struct QuestionSettings {
    QuestionApiFormat apiFormat = QuestionApiFormat::OpenAiCompatible;
    // Base URL or full endpoint; empty uses the format's official endpoint.
    QString apiUrl;
    QString apiKey;
    QString model;
    // Replaces the built-in prompt when not empty.
    QString customPrompt;
    // Appended to whichever prompt is used, e.g. "高中数学、大学物理".
    QString subjectScope;
    // Maximum time without any data received, not the total answer time.
    int timeoutSeconds = 120;
    bool useSystemProxy = true;
};

struct AppSettings {
    bool autoStart = false;
    UiSettings ui;
    CaptureSettings capture;
    PinSettings pin;
    OutputSettings output;
    HotkeySettings hotkeys;
    ToolStyleSettings tools;
    AiTranslateSettings aiTranslate;
    QuestionSettings question;
};

class AppConfig : public QObject {
    Q_OBJECT
public:
    explicit AppConfig(QObject* parent = nullptr);

    const AppSettings& settings() const { return settings_; }
    AppSettings& mutableSettings() { return settings_; }

    void load();
    bool save();
    void resetDefaults();
    bool syncAutoStart() const;

    QString defaultSaveDirectory() const;

signals:
    void changed();

private:
    AppSettings settings_;
};

QString keySequenceToText(const QKeySequence& sequence);
QKeySequence keySequenceFromText(const QString& text, const QKeySequence& fallback);

// API keys are stored encrypted for the current Windows user (DPAPI) with a
// "dpapi:" prefix. Other platforms, and values that fail to encrypt, are stored
// as-is; plain values read back unchanged so older settings keep working.
QString protectSecretForStorage(const QString& secret);
QString secretFromStorage(const QString& stored);

} // namespace Visnip
