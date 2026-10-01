#include "services/OfflineTranslationService.h"
#include "services/ImageTranslationService.h"
#include "services/LocalTextTranslationService.h"
#include "core/AppConfig.h"
#include "core/PerfLog.h"
#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QPointer>
#include <QProcess>
#include <QProcessEnvironment>
#include <QRegularExpression>
#include <QStandardPaths>
#include <QSaveFile>
#include <QTemporaryDir>
#include <QTimer>
#include <QUuid>
#include <memory>
#ifdef Q_OS_WIN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace Visnip {
namespace {
qint64 availableMemory()
{
#ifdef Q_OS_WIN
    MEMORYSTATUSEX value{}; value.dwLength = sizeof(value);
    if (GlobalMemoryStatusEx(&value)) return qint64(value.ullAvailPhys);
#endif
    return -1;
}
QString canonicalRoot(const QString& directory)
{
    return QDir(directory.trimmed().isEmpty() ? OfflineTranslationService::defaultResourceDirectory() : directory).absolutePath();
}
QString stageText(const QString& stage)
{
    if (stage == QStringLiteral("loading_local_models")) return QStringLiteral("正在加载本机模型，完成后连续翻译无需重复加载…");
    if (stage == QStringLiteral("ocr")) return QStringLiteral("正在本机识别文字…");
    if (stage == QStringLiteral("translation")) return QStringLiteral("正在本机进行上下文翻译…");
    if (stage == QStringLiteral("segmentation")) return QStringLiteral("正在精细分割文字笔画…");
    if (stage == QStringLiteral("warming_vision_gpu")) return QStringLiteral("正在预热显卡分割和修复模型，首次启动需要等待…");
    if (stage == QStringLiteral("repair_gpu")) return QStringLiteral("正在显卡上修复背景…");
    if (stage == QStringLiteral("composition")) return QStringLiteral("正在保护背景并回填译文…");
    if (stage == QStringLiteral("encode")) return QStringLiteral("正在生成译图…");
    return QStringLiteral("本机离线处理中…");
}
}

class OfflineWorkerSession final : public QObject {
public:
    explicit OfflineWorkerSession(QObject* parent) : QObject(parent)
    {
        idle_.setSingleShot(true); deadline_.setSingleShot(true); pressure_.setInterval(5000);
        connect(&idle_, &QTimer::timeout, this, [this]() { if (!owner_) release(); });
        connect(&pressure_, &QTimer::timeout, this, [this]() {
            const qint64 free = availableMemory();
            if (!owner_ && ready_ && free >= 0 && free < 2LL * 1024 * 1024 * 1024) release();
        });
        connect(&deadline_, &QTimer::timeout, this, [this]() {
            fail(QStringLiteral("本机模型加载或翻译超时，已释放引擎；没有联网回退。"));
        });
        connect(&process_, &QProcess::readyReadStandardOutput, this, [this]() { readEvents(); });
        connect(&process_, &QProcess::readyReadStandardError, this, [this]() { process_.readAllStandardError(); });
        connect(&process_, &QProcess::errorOccurred, this, [this](QProcess::ProcessError error) {
            if (!stopping_ && error == QProcess::FailedToStart)
                fail(QStringLiteral("无法启动本机离线引擎，请检查资源或系统安全策略；没有联网回退。"));
        });
        connect(&process_, qOverload<int, QProcess::ExitStatus>(&QProcess::finished), this,
            [this](int, QProcess::ExitStatus) {
                if (!stopping_) fail(QStringLiteral("本机离线引擎已退出，原图保留；下次任务会重新加载。"));
            });
        connect(QCoreApplication::instance(), &QCoreApplication::aboutToQuit, this, [this]() { release(); });
    }
    ~OfflineWorkerSession() override { release(); }
    bool ready() const { return ready_; }
    qint64 pid() const { return process_.processId(); }
    void warm(const QString& root)
    {
        if (owner_ || (root_ == canonicalRoot(root) && process_.state() != QProcess::NotRunning)) return;
        const qint64 free = availableMemory();
        if (free >= 0 && free < 4LL * 1024 * 1024 * 1024) return; // Vision models are now lazy; prewarm OCR/translator only.
        if (!OfflineTranslationService::resourceProblem(root, QStringLiteral("precise")).isEmpty()) return;
        start(root);
    }
    void submit(OfflineTranslationService* owner, const QImage& image, const QString& language, const QString& root)
    {
        if (owner_) { emit owner->failed(QStringLiteral("已有本机离线任务正在运行，请等待或取消后重试。")); return; }
        if (root_ != canonicalRoot(root) || process_.state() == QProcess::NotRunning) {
            if (!start(root)) { emit owner->failed(QStringLiteral("无法准备本机模型的临时工作目录。")); return; }
        }
        owner_ = owner; owner->busy_ = true; elapsed_.start(); idle_.stop();
        identifier_ = QUuid::createUuid().toString(QUuid::Id128);
        language_ = language; expected_ = image.size(); submitted_ = false;
        if (!workspace_ || !image.save(workspace_->filePath(identifier_ + QStringLiteral(".png")), "PNG")) {
            fail(QStringLiteral("无法保存本机离线任务的临时图片。")); return;
        }
        deadline_.start(600000);
        if (ready_) dispatch();
        else emit owner->phaseChanged(stageText(QStringLiteral("loading_local_models")));
    }
    void cancel(OfflineTranslationService* caller)
    {
        if (owner_ != caller) return;
        owner_.clear(); caller->busy_ = false;
        // Cancellation must stop computation, not merely discard a later response.
        release(); emit caller->cancelled();
    }
    void release()
    {
        QPointer<OfflineTranslationService> previous = owner_; owner_.clear();
        if (previous) previous->busy_ = false;
        stopping_ = true; ready_ = false; submitted_ = false;
        idle_.stop(); deadline_.stop(); pressure_.stop();
        if (process_.state() != QProcess::NotRunning) { process_.kill(); process_.waitForFinished(3000); }
        events_.clear(); identifier_.clear(); root_.clear(); workspace_.reset(); stopping_ = false;
        if (previous) emit previous->cancelled();
    }
private:
    bool start(const QString& root)
    {
        release();
        workspace_ = std::make_unique<QTemporaryDir>(QDir::tempPath() + QStringLiteral("/visnip-session-XXXXXX"));
        if (!workspace_->isValid()) { workspace_.reset(); return false; }
        root_ = canonicalRoot(root); QDir directory(root_);
        QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
        for (const QString& name : env.keys()) {
            if (name.startsWith(QStringLiteral("PYTHON"), Qt::CaseInsensitive)
                || name.contains(QStringLiteral("PROXY"), Qt::CaseInsensitive)
                || name.startsWith(QStringLiteral("LLAMA"), Qt::CaseInsensitive)) env.remove(name);
        }
        env.insert(QStringLiteral("HF_HUB_OFFLINE"), QStringLiteral("1"));
        env.insert(QStringLiteral("TRANSFORMERS_OFFLINE"), QStringLiteral("1"));
        env.insert(QStringLiteral("PYTHONUTF8"), QStringLiteral("1"));
        env.insert(QStringLiteral("VISLATE_GPU_RUNTIME"), QDir(QCoreApplication::applicationDirPath()).filePath(QStringLiteral("engine-runtime")));
        // Run the engine embedded in THIS client, not stale Python code shipped
        // inside an unchanged GiB weight package. The isolated overlay is local.
        const QString overlay = workspace_->filePath(QStringLiteral("engine"));
        const QString package = QDir(overlay).filePath(QStringLiteral("vislate_engine"));
        if (!QDir().mkpath(package)) return false;
        for (const QString& name : {QStringLiteral("__init__"),QStringLiteral("native_translation"),QStringLiteral("runtime"),
             QStringLiteral("adapters"),QStringLiteral("layout"),QStringLiteral("appearance"),QStringLiteral("text_tiles"),
             QStringLiteral("desktop_runner"),QStringLiteral("session_worker"),QStringLiteral("adaptive_vision"),
             QStringLiteral("translation_policy"),QStringLiteral("gpu_runtime"),QStringLiteral("onnx_ocr"),
             QStringLiteral("ui_structure"),QStringLiteral("gpu_attention"),QStringLiteral("full_gpu_vision"),QStringLiteral("device_profile"),QStringLiteral("failure"),QStringLiteral("unified_elements"),QStringLiteral("unified_composition")}) {
            QByteArray content;
            if (name != QStringLiteral("__init__")) {
                QFile embedded(QStringLiteral(":/visnip/offline/%1.py").arg(name));
                if (!embedded.open(QIODevice::ReadOnly)) return false;
                content=embedded.readAll();
            }
            QSaveFile target(QDir(package).filePath(name+QStringLiteral(".py")));
            if (!target.open(QIODevice::WriteOnly) || target.write(content)!=content.size() || !target.commit()) return false;
        }
        process_.setProcessEnvironment(env); process_.setWorkingDirectory(root_);
        process_.setProgram(directory.filePath(QStringLiteral("python/python.exe")));
        QStringList arguments{QStringLiteral("-c"), QStringLiteral("import sys,runpy;sys.path.insert(0,sys.argv.pop(1));runpy.run_module('vislate_engine.session_worker',run_name='__main__')"), overlay,
            QStringLiteral("--config"), directory.filePath(QStringLiteral("precise.json")),
            QStringLiteral("--workspace"), workspace_->path()};
        QString gpuProfile = QDir(QCoreApplication::applicationDirPath()).filePath(QStringLiteral("validation/gpu-profile.json"));
        if (QStandardPaths::isTestModeEnabled() && qEnvironmentVariableIsSet("VISNIP_TEST_GPU_PROFILE"))
            gpuProfile = qEnvironmentVariable("VISNIP_TEST_GPU_PROFILE");
        if (QFileInfo::exists(gpuProfile)) arguments << QStringLiteral("--gpu-profile") << gpuProfile;
        process_.setArguments(arguments);
        deadline_.start(120000); process_.start(); return true;
    }
    void dispatch()
    {
        if (!owner_ || !ready_ || submitted_) return;
        const QByteArray command = QJsonDocument(QJsonObject{{QStringLiteral("op"), QStringLiteral("translate")},
            {QStringLiteral("id"), identifier_}, {QStringLiteral("input"), identifier_ + QStringLiteral(".png")},
            {QStringLiteral("output"), identifier_ + QStringLiteral(".json")}, {QStringLiteral("target"), language_}}).toJson(QJsonDocument::Compact) + '\n';
        submitted_ = true;
        if (process_.write(command) != command.size()) fail(QStringLiteral("无法发送本机离线任务。"));
    }
    void keepWarm()
    {
        deadline_.stop();
        int interval = 5 * 60 * 1000;
        if (QStandardPaths::isTestModeEnabled()) {
            bool valid = false; const int testInterval = qEnvironmentVariableIntValue("VISNIP_TEST_IDLE_MS", &valid);
            if (valid && testInterval >= 100) interval = testInterval;
        }
        idle_.start(interval); pressure_.start();
    }
    void fail(const QString& message)
    {
        QPointer<OfflineTranslationService> owner = owner_; owner_.clear();
        if (owner) owner->busy_ = false;
        release();
        if (owner) emit owner->failed(message);
        else Perf::log(QStringLiteral("OfflineEngine.prewarm_failed"));
    }
    void finishTaskFailure(const QJsonObject& event, bool fatal = false)
    {
        const auto safeCode = [](const QString& text, const QString& fallback) {
            static const QRegularExpression valid(QStringLiteral("^[a-z][a-z0-9_]{0,79}$"));
            return valid.match(text).hasMatch() ? text : fallback;
        };
        const QString code = safeCode(event.value(QStringLiteral("code")).toString(), QStringLiteral("engine_internal_error"));
        const QString stage = safeCode(event.value(QStringLiteral("stage")).toString(), lastStage_.isEmpty() ? QStringLiteral("initialization") : lastStage_);
        const QStringList reusableCodes{QStringLiteral("protected_token_mismatch"), QStringLiteral("sentence_not_translated"),
            QStringLiteral("translation_id_mismatch"), QStringLiteral("invalid_translation"), QStringLiteral("translation_truncated"),
            QStringLiteral("translation_context_limit"), QStringLiteral("translation_json_invalid"),
            QStringLiteral("ocr_empty_or_too_many_units"), QStringLiteral("no_safe_layout"), QStringLiteral("font_glyphs_missing"),
            QStringLiteral("invalid_unit_box"), QStringLiteral("invalid_image"), QStringLiteral("image_too_large"),
            QStringLiteral("pixel_limit"), QStringLiteral("image_format"), QStringLiteral("language_not_qualified")};
        const bool reuse = !fatal && ready_ && process_.state() == QProcess::Running
            && event.value(QStringLiteral("recoverable")).toBool() && reusableCodes.contains(code);
        QJsonObject diagnostic{{QStringLiteral("client"), QStringLiteral(VISNIP_VERSION)},
            {QStringLiteral("code"), code}, {QStringLiteral("stage"), stage},
            {QStringLiteral("worker_pid"), pid()}, {QStringLiteral("model_reused"), reuse},
            {QStringLiteral("width"), expected_.width()}, {QStringLiteral("height"), expected_.height()},
            {QStringLiteral("total_ms"), owner_ && elapsed_.isValid() ? elapsed_.elapsed() : 0}};
        QJsonArray frames;
        static const QRegularExpression modulePattern(QStringLiteral("^[a-zA-Z_][a-zA-Z0-9_]{0,60}\\.py$"));
        for (const auto& value : event.value(QStringLiteral("frames")).toArray()) {
            const auto item = value.toObject();
            const QString module = item.value(QStringLiteral("module")).toString();
            const int number = item.value(QStringLiteral("line")).toInt();
            if (frames.size() < 6 && modulePattern.match(module).hasMatch() && number > 0 && number < 100000)
                frames.append(QJsonObject{{QStringLiteral("module"),module},{QStringLiteral("line"),number}});
        }
        diagnostic.insert(QStringLiteral("frames"), frames);
        QJsonArray regions;
        static const QRegularExpression regionId(QStringLiteral("^[a-zA-Z0-9_-]{1,64}$"));
        const QStringList reasons{QStringLiteral("equivalent"), QStringLiteral("artwork_collision"),
            QStringLiteral("neighbor_collision"), QStringLiteral("no_confident_text_mask"),
            QStringLiteral("foreground_unavailable"), QStringLiteral("font_unavailable"),
            QStringLiteral("protected_region"), QStringLiteral("layout_unfit"),
            QStringLiteral("sentence_not_translated"), QStringLiteral("protected_token_mismatch")};
        for (const auto& value : event.value(QStringLiteral("regions")).toArray()) {
            const auto row = value.toObject();
            const QString identifier = row.value(QStringLiteral("id")).toString();
            const QString reason = row.value(QStringLiteral("reason")).toString();
            if (regions.size() < 64 && regionId.match(identifier).hasMatch() && reasons.contains(reason))
                regions.append(QJsonObject{{QStringLiteral("id"), identifier}, {QStringLiteral("reason"), reason}});
        }
        if (!regions.isEmpty()) diagnostic.insert(QStringLiteral("regions"), regions);
        const QString failedUnit = event.value(QStringLiteral("unit_id")).toString();
        static const QRegularExpression unitPattern(QStringLiteral("^[A-Za-z0-9_-]{1,64}$"));
        if (unitPattern.match(failedUnit).hasMatch()) diagnostic.insert(QStringLiteral("unit_id"), failedUnit);
        const auto numbers = [](const QJsonObject& row, const QStringList& keys) {
            QJsonObject output;
            for (const QString& key : keys) {
                const QJsonValue value = row.value(key);
                if (value.isDouble() && value.toDouble() >= 0 && value.toDouble() <= 1e10)
                    output.insert(key, value);
            }
            return output;
        };
        const auto stats = event.value(QStringLiteral("translation_stats")).toObject();
        QJsonObject safeStats = numbers(stats, {QStringLiteral("cache_hits"), QStringLiteral("requests"),
            QStringLiteral("prompt_tokens"), QStringLiteral("completion_tokens"), QStringLiteral("prompt_ms"),
            QStringLiteral("generation_ms"), QStringLiteral("retry_requests"), QStringLiteral("retried_regions"),
            QStringLiteral("recovered_regions"), QStringLiteral("offloaded_layers")});
        QJsonArray calls;
        const QStringList callOutcomes{QStringLiteral("response_received"), QStringLiteral("translation_truncated"),
            QStringLiteral("translation_context_limit"), QStringLiteral("translation_id_mismatch"),
            QStringLiteral("translation_timeout"), QStringLiteral("translation_transport_error"),
            QStringLiteral("translation_response_error")};
        for (const auto& value : stats.value(QStringLiteral("calls")).toArray()) {
            if (calls.size() >= 32) break;
            if (!value.isObject()) continue;
            const auto row = value.toObject();
            auto call = numbers(row, {QStringLiteral("retry"), QStringLiteral("regions"), QStringLiteral("source_chars"),
                QStringLiteral("tokenize_ms"), QStringLiteral("inference_wall_ms"), QStringLiteral("wall_ms"),
                QStringLiteral("prompt_tokens"), QStringLiteral("completion_tokens"),
                QStringLiteral("prompt_ms"), QStringLiteral("generation_ms")});
            const QString outcome = row.value(QStringLiteral("outcome")).toString();
            if (callOutcomes.contains(outcome)) call.insert(QStringLiteral("outcome"), outcome);
            calls.append(call);
        }
        if (!calls.isEmpty()) safeStats.insert(QStringLiteral("calls"), calls);
        QJsonArray validationFailures;
        for (const auto& value : stats.value(QStringLiteral("validation_failures")).toArray()) {
            if (validationFailures.size() >= 128) break;
            const auto row = value.toObject();
            const QString identifier = row.value(QStringLiteral("id")).toString();
            const QString reason = row.value(QStringLiteral("reason")).toString();
            if (!unitPattern.match(identifier).hasMatch() || !reusableCodes.contains(reason)) continue;
            auto item = numbers(row, {QStringLiteral("retry"), QStringLiteral("source_chars"), QStringLiteral("target_chars"),
                QStringLiteral("missing_numbers"), QStringLiteral("added_numbers"),
                QStringLiteral("missing_placeholders"), QStringLiteral("added_placeholders")});
            item.insert(QStringLiteral("id"), identifier);
            item.insert(QStringLiteral("reason"), reason);
            validationFailures.append(item);
        }
        if (!validationFailures.isEmpty()) safeStats.insert(QStringLiteral("validation_failures"), validationFailures);
        if (!safeStats.isEmpty()) diagnostic.insert(QStringLiteral("translation_stats"), safeStats);
        const QByteArray content = QJsonDocument(diagnostic).toJson(QJsonDocument::Compact);
        Perf::log(QStringLiteral("OfflineEngine.failure ") + QString::fromUtf8(content));
        const QString logs = QDir(QCoreApplication::applicationDirPath()).filePath(QStringLiteral("logs"));
        if (QDir().mkpath(logs)) {
            QSaveFile file(QDir(logs).filePath(QStringLiteral("last-offline-error.json")));
            if (file.open(QIODevice::WriteOnly) && file.write(content) == content.size()) file.commit();
        }
        QString detail = QStringLiteral("本机处理未完成");
        if (code == QStringLiteral("protected_token_mismatch")) detail = QStringLiteral("译文中的数字或占位符未通过完整性检查");
        else if (code == QStringLiteral("sentence_not_translated")) detail = QStringLiteral("部分文字未生成目标语言译文");
        else if (code == QStringLiteral("ocr_empty_or_too_many_units")) detail = QStringLiteral("未识别到有效文字，或文字区域数量超过上限");
        else if (code == QStringLiteral("gpu_out_of_memory")) detail = QStringLiteral("显卡可用内存不足");
        else if (code == QStringLiteral("no_safe_layout")) detail = QStringLiteral("译文未能完成安全回填");
        const QString message = QStringLiteral("%1（%2 / %3）。原图保留；%4没有联网回退。")
            .arg(detail, stage, code, reuse ? QStringLiteral("模型仍就绪，可重试；") : QStringLiteral("诊断已保存到日志；"));
        QPointer<OfflineTranslationService> owner = owner_; owner_.clear();
        if (owner) owner->busy_ = false;
        if (reuse) {
            if (workspace_) {
                QFile::remove(workspace_->filePath(identifier_ + QStringLiteral(".png")));
                QFile::remove(workspace_->filePath(identifier_ + QStringLiteral(".json")));
            }
            submitted_ = false; identifier_.clear(); keepWarm();
        } else release();
        if (owner) emit owner->failed(message);
    }
    void readEvents()
    {
        events_ += process_.readAllStandardOutput();
        if (events_.size() > 65536) { fail(QStringLiteral("本机引擎协议数据异常。")); return; }
        while (events_.contains('\n')) {
            const auto end = events_.indexOf('\n'); const auto line = events_.left(end); events_.remove(0, end + 1);
            const QJsonObject event = QJsonDocument::fromJson(line).object();
            const QString type = event.value(QStringLiteral("event")).toString();
            if (type == QStringLiteral("vision_ready")) {
                Perf::log(QStringLiteral("OfflineEngine.vision_ready device=%1 strategy=%2")
                    .arg(event.value(QStringLiteral("device")).toString(), event.value(QStringLiteral("strategy")).toString()));
                continue;
            }
            if (type == QStringLiteral("ready")) {
                if (event.value(QStringLiteral("protocol")).toInt() != 2 || event.value(QStringLiteral("quality")).toString() != QStringLiteral("precise")) {
                    fail(QStringLiteral("本机引擎协议或精细模式不兼容。")); return;
                }
                ready_ = true;
                Perf::log(QStringLiteral("OfflineEngine.ready load_ms=%1 pid=%2 backend=%3 layers=%4 reason=%5")
                    .arg(event.value(QStringLiteral("loading_ms")).toInteger()).arg(pid())
                    .arg(event.value(QStringLiteral("backend")).toString())
                    .arg(event.value(QStringLiteral("offloaded_layers")).toInt())
                    .arg(event.value(QStringLiteral("backend_reason")).toString()));
                if (owner_) dispatch(); else keepWarm();
                continue;
            }
            if (type == QStringLiteral("fatal")) { finishTaskFailure(event, true); return; }
            if (!owner_) continue;
            const QString id = event.value(QStringLiteral("id")).toString();
            if (!id.isEmpty() && id != identifier_) continue;
            if (type == QStringLiteral("stage")) {
                const QString stage = event.value(QStringLiteral("stage")).toString();
                static const QRegularExpression validStage(QStringLiteral("^[a-z_]{1,40}$"));
                if (validStage.match(stage).hasMatch()) {
                    lastStage_ = stage;
                    Perf::log(QStringLiteral("OfflineEngine.stage name=%1 elapsed_ms=%2 pid=%3").arg(stage).arg(elapsed_.elapsed()).arg(pid()));
                }
                emit owner_->phaseChanged(stageText(stage)); continue;
            }
            if (id != identifier_) continue;
            if (type == QStringLiteral("error")) { finishTaskFailure(event); return; }
            if (type == QStringLiteral("done")) finishResult();
        }
    }
    void finishResult()
    {
        QFile file(workspace_->filePath(identifier_ + QStringLiteral(".json")));
        if (!file.open(QIODevice::ReadOnly) || file.size() > ImageTranslationService::kMaxResponseBytes) {
            fail(QStringLiteral("离线引擎未返回有效结果或结果过大。")); return;
        }
        const QByteArray payload = file.readAll(); file.close(); file.remove();
        QFile::remove(workspace_->filePath(identifier_ + QStringLiteral(".png")));
        QString error;
        auto result = ImageTranslationService::parseResponse(payload, expected_, &error);
        result.requestedTargetLanguage = language_; result.uploadedImageSize = expected_;
        if (result.targetLanguage != language_) error = QStringLiteral("离线译文目标语言不匹配。");
        const auto object = QJsonDocument::fromJson(payload).object();
        QJsonObject metrics{{QStringLiteral("client"),QStringLiteral(VISNIP_VERSION)},
            {QStringLiteral("width"),expected_.width()},{QStringLiteral("height"),expected_.height()},
            {QStringLiteral("total_ms"),elapsed_.elapsed()},{QStringLiteral("worker_pid"),pid()},
            {QStringLiteral("backend"),object.value(QStringLiteral("compute_backend"))},
            {QStringLiteral("backend_reason"),object.value(QStringLiteral("backend_reason"))},
            {QStringLiteral("timings_ms"),object.value(QStringLiteral("timings_ms"))},
            {QStringLiteral("segmentation"),object.value(QStringLiteral("segmentation"))},
            {QStringLiteral("translation_stats"),object.value(QStringLiteral("translation_stats"))},
            {QStringLiteral("structure"),object.value(QStringLiteral("structure"))},
            {QStringLiteral("presentation"),object.value(QStringLiteral("presentation"))}};
        QJsonArray outcomes;
        for (const auto& item:object.value(QStringLiteral("blocks")).toArray()) {
            const auto block=item.toObject();
            outcomes.append(QJsonObject{{QStringLiteral("id"),block.value(QStringLiteral("id"))},
                {QStringLiteral("status"),block.value(QStringLiteral("status"))},{QStringLiteral("reason"),block.value(QStringLiteral("reason"))},
                {QStringLiteral("source_chars"),block.value(QStringLiteral("source")).toString().size()},
                {QStringLiteral("layout_policy"),block.value(QStringLiteral("layout_policy"))},
                {QStringLiteral("display_mode"),block.value(QStringLiteral("display_mode"))},
                {QStringLiteral("obstacle_pixels"),block.value(QStringLiteral("obstacle_pixels"))},
                {QStringLiteral("collision_pixels"),block.value(QStringLiteral("collision_pixels"))},
                {QStringLiteral("target_chars"),block.value(QStringLiteral("translation")).toString().size()}});
        }
        metrics.insert(QStringLiteral("regions"),outcomes);
        Perf::log(QStringLiteral("OfflineEngine.metrics ")+QString::fromUtf8(QJsonDocument(metrics).toJson(QJsonDocument::Compact)));
        int preserved = 0;
        int untranslated = 0;
        for (const auto& block : object.value(QStringLiteral("blocks")).toArray()) {
            const auto value = block.toObject();
            if (value.value(QStringLiteral("translation_status")).toString() == QStringLiteral("failed")) ++untranslated;
            if (value.value(QStringLiteral("status")).toString() == QStringLiteral("preserved")
                && value.value(QStringLiteral("reason")).toString() != QStringLiteral("equivalent")) ++preserved;
        }
        if (untranslated) result.notice = QStringLiteral("已显示可用译文；%1 个区域翻译未完成，另有 %2 个区域未原位回填。请查看译文面板；面板文字不会自动写入导出图片。")
            .arg(untranslated).arg(preserved - untranslated);
        else if (preserved) result.notice = QStringLiteral("%1 个区域未原位回填，完整译文已放入译文面板；面板文字不会自动写入导出图片。").arg(preserved);
        if (result.image.isNull() || !error.isEmpty()) { fail(error); return; }
        QPointer<OfflineTranslationService> owner = owner_; owner_.clear();
        const qint64 ms = elapsed_.elapsed(); submitted_ = false; identifier_.clear();
        if (owner) owner->busy_ = false;
        keepWarm();
        Perf::log(QStringLiteral("OfflineEngine.task_finished total_ms=%1 pid=%2").arg(ms).arg(pid()));
        if (owner) emit owner->succeeded(result, ms);
    }
    QProcess process_;
    QTimer deadline_, idle_, pressure_;
    QElapsedTimer elapsed_;
    std::unique_ptr<QTemporaryDir> workspace_;
    QPointer<OfflineTranslationService> owner_;
    QString root_, identifier_, language_, lastStage_;
    QSize expected_;
    QByteArray events_;
    bool ready_ = false, submitted_ = false, stopping_ = false;
};
namespace {
QPointer<OfflineWorkerSession> resident;
OfflineWorkerSession* session()
{
    if (!resident) resident = new OfflineWorkerSession(QCoreApplication::instance());
    return resident.data();
}
}
OfflineTranslationService::OfflineTranslationService(QObject* parent) : QObject(parent) {}
OfflineTranslationService::~OfflineTranslationService() { if (resident) resident->cancel(this); }
QString OfflineTranslationService::defaultResourceDirectory()
{
    return defaultOfflineResourceDirectory();
}
QString OfflineTranslationService::resourceProblem(const QString& directory, const QString& quality)
{
    if (quality == QStringLiteral("lite")) return LocalTextTranslationService::resourceProblem(directory);
    if (quality != QStringLiteral("precise")) return QStringLiteral("未知的本机离线处理类型，请在首选项中重新选择。");
    const QDir root(canonicalRoot(directory));
    for (const QString& relative : {QStringLiteral("python/python.exe"),
        QStringLiteral("precise.json"), QStringLiteral("models/Hy-MT2-1.8B-Q4_K_M.gguf"),
        QStringLiteral("llama/llama-server.exe"), QStringLiteral("models/hi_sam_b.pth")}) {
        if (!QFileInfo(root.filePath(relative)).isFile())
            return QStringLiteral("精细资源尚未就绪（缺少 %1），请点击“下载并启用”。").arg(relative);
    }
    return {};
}
void OfflineTranslationService::translate(const QImage& image, const QString& language, const QString& root, const QString& quality)
{
    if (busy_) { emit failed(QStringLiteral("已有本机离线任务正在运行。")); return; }
    if (image.isNull() || qint64(image.width()) * image.height() > 8000000) { emit failed(QStringLiteral("本机离线支持不超过 800 万像素的有效截图。")); return; }
    if (language != QStringLiteral("zh-Hans") && language != QStringLiteral("en")) { emit failed(QStringLiteral("当前离线资源只开放中英文。")); return; }
    const QString problem = resourceProblem(root, quality);
    if (!problem.isEmpty()) { emit failed(problem); return; }
    session()->submit(this, image, language, root);
}
void OfflineTranslationService::prewarm(const QString& root) { session()->warm(root); }
void OfflineTranslationService::releaseSharedEngine() { if (resident) resident->release(); }
bool OfflineTranslationService::sharedEngineReady() { return resident && resident->ready(); }
qint64 OfflineTranslationService::sharedEngineProcessId() { return resident ? resident->pid() : 0; }
void OfflineTranslationService::cancel() { if (resident) resident->cancel(this); }
} // namespace Visnip
