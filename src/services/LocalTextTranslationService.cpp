#include "services/LocalTextTranslationService.h"

#include "core/AppConfig.h"
#include "core/LocalTranslation.h"
#include "core/PerfLog.h"
#include "services/TextTranslationService.h"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QFutureWatcher>
#include <QHostAddress>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkProxy>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QPointer>
#include <QProcess>
#include <QProcessEnvironment>
#include <QRandomGenerator>
#include <QRegularExpression>
#include <QStandardPaths>
#include <QTcpServer>
#include <QThread>
#include <QTimer>
#include <QtConcurrent>

#include <functional>
#include <utility>
#include <vector>

#ifdef Q_OS_WIN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace Visnip {
namespace {

// Parallel decoding: on a CPU, generation is bound by memory bandwidth, so a
// step that advances four regions costs little more than a step for one.
constexpr int kSlots = 4;
constexpr int kContextPerSlot = 1536;
constexpr int kStartTimeoutMs = 120000;
constexpr int kHealthPollMs = 150;
constexpr int kDeviceProbeTimeoutMs = 20000;
constexpr int kRequestTimeoutMs = 120000;
constexpr int kRetryTimeoutMs = 30000;
// Restarting a memory-mapped model is cheap while the OS still caches it, so
// the server is kept long enough to cover a working session, not forever.
constexpr int kIdleReleaseMs = 10 * 60 * 1000;
constexpr qint64 kPrewarmMinimumFreeBytes = 3LL * 1024 * 1024 * 1024;
constexpr qint64 kPressureReleaseBytes = 1536LL * 1024 * 1024;
constexpr int kCacheEntries = 512;
const QString kServerRelativePath = QStringLiteral("llama/llama-server.exe");
const QString kModelRelativePath = QStringLiteral("models/Hy-MT2-1.8B-Q4_K_M.gguf");
const QString kPinnedRelease = QStringLiteral("b10964");

qint64 availableMemory()
{
#ifdef Q_OS_WIN
    MEMORYSTATUSEX value{};
    value.dwLength = sizeof(value);
    if (GlobalMemoryStatusEx(&value)) {
        return qint64(value.ullAvailPhys);
    }
#endif
    return -1;
}

int physicalCoreCount()
{
#ifdef Q_OS_WIN
    DWORD length = 0;
    GetLogicalProcessorInformation(nullptr, &length);
    std::vector<SYSTEM_LOGICAL_PROCESSOR_INFORMATION> entries(length / sizeof(SYSTEM_LOGICAL_PROCESSOR_INFORMATION));
    if (!entries.empty() && GetLogicalProcessorInformation(entries.data(), &length)) {
        const auto cores = std::count_if(entries.cbegin(), entries.cend(), [](const auto& entry) {
            return entry.Relationship == RelationProcessorCore;
        });
        if (cores > 0) {
            return static_cast<int>(cores);
        }
    }
#endif
    return qMax(1, QThread::idealThreadCount() / 2);
}

QString canonicalRoot(const QString& directory)
{
    return QDir(directory.trimmed().isEmpty() ? defaultOfflineResourceDirectory()
                                              : directory.trimmed())
        .absolutePath();
}

QString randomToken()
{
    QByteArray bytes(32, Qt::Uninitialized);
    QRandomGenerator::system()->fillRange(reinterpret_cast<quint32*>(bytes.data()), bytes.size() / 4);
    return QString::fromLatin1(bytes.toBase64(QByteArray::Base64UrlEncoding | QByteArray::OmitTrailingEquals));
}

quint16 freeLoopbackPort()
{
    QTcpServer probe;
    return probe.listen(QHostAddress::LocalHost, 0) ? probe.serverPort() : 0;
}

struct GpuRuntime {
    QString executable;
    QString backend;
    QString problem;
};

// Optional accelerated llama.cpp build placed beside the client by an
// operator. Same contract as resources/offline/gpu_runtime.py: pinned
// release, closed file list with SHA-256, files confined to runtime/.
GpuRuntime verifyGpuRuntime(const QString& directory)
{
    GpuRuntime result;
    const QDir root(directory);
    QFile manifest(root.filePath(QStringLiteral("runtime-manifest.json")));
    if (!manifest.exists()) {
        result.problem = QStringLiteral("not_installed");
        return result;
    }
    if (manifest.size() > 65536 || !manifest.open(QIODevice::ReadOnly)) {
        result.problem = QStringLiteral("manifest_missing");
        return result;
    }
    const QJsonObject info = QJsonDocument::fromJson(manifest.readAll()).object();
    const QString backend = info.value(QStringLiteral("backend")).toString();
    if (info.value(QStringLiteral("source")).toString() != QStringLiteral("ggml-org/llama.cpp")
        || info.value(QStringLiteral("release")).toString() != kPinnedRelease
        || (backend != QStringLiteral("vulkan") && backend != QStringLiteral("cuda"))) {
        result.problem = QStringLiteral("unsupported_runtime");
        return result;
    }
    const QString runtimeRoot = QFileInfo(root.filePath(QStringLiteral("runtime"))).canonicalFilePath();
    if (runtimeRoot.isEmpty()) {
        result.problem = QStringLiteral("file_missing");
        return result;
    }
    QStringList servers;
    for (const QJsonValue& value : info.value(QStringLiteral("files")).toArray()) {
        const QJsonObject record = value.toObject();
        const QFileInfo file(QDir(runtimeRoot).filePath(record.value(QStringLiteral("path")).toString()));
        const QString canonical = file.canonicalFilePath();
        if (file.isSymLink() || canonical.isEmpty() || !file.isFile()
            || !canonical.startsWith(runtimeRoot + QLatin1Char('/'))) {
            result.problem = QStringLiteral("file_missing");
            return result;
        }
        QFile stream(canonical);
        QCryptographicHash hash(QCryptographicHash::Sha256);
        if (!stream.open(QIODevice::ReadOnly) || !hash.addData(&stream)
            || hash.result().toHex() != record.value(QStringLiteral("sha256")).toString().toLatin1()) {
            result.problem = QStringLiteral("checksum_mismatch");
            return result;
        }
        if (file.fileName() == QStringLiteral("llama-server.exe")) {
            servers.append(canonical);
        }
    }
    if (servers.size() != 1) {
        result.problem = QStringLiteral("executable_missing");
        return result;
    }
    result.executable = servers.first();
    result.backend = backend;
    return result;
}

QProcessEnvironment serverEnvironment(const QString& executable)
{
    // No proxy, model-download or remote-provider variables are inherited.
    const QProcessEnvironment system = QProcessEnvironment::systemEnvironment();
    QProcessEnvironment env;
    for (const QString& name : {QStringLiteral("SYSTEMROOT"), QStringLiteral("WINDIR"), QStringLiteral("TEMP"),
                                QStringLiteral("TMP"), QStringLiteral("USERPROFILE"), QStringLiteral("LOCALAPPDATA"),
                                QStringLiteral("HOME"), QStringLiteral("CUDA_VISIBLE_DEVICES")}) {
        if (system.contains(name)) {
            env.insert(name, system.value(name));
        }
    }
    const QString directory = QDir::toNativeSeparators(QFileInfo(executable).absolutePath());
#ifdef Q_OS_WIN
    env.insert(QStringLiteral("PATH"), directory + QLatin1Char(';')
        + QDir::toNativeSeparators(system.value(QStringLiteral("SYSTEMROOT"), QStringLiteral("C:\\Windows"))
                                   + QStringLiteral("/System32")));
#else
    env.insert(QStringLiteral("PATH"), directory + QStringLiteral(":/usr/bin:/bin"));
    env.insert(QStringLiteral("LD_LIBRARY_PATH"), directory);
#endif
    if (QStandardPaths::isTestModeEnabled() && qEnvironmentVariableIsSet("VISNIP_TEST_LITE_EXTRA_PATH")) {
        // Lets the test double find its Qt libraries; never used in production.
        env.insert(QStringLiteral("PATH"), env.value(QStringLiteral("PATH")) + QDir::listSeparator()
            + qEnvironmentVariable("VISNIP_TEST_LITE_EXTRA_PATH"));
    }
    env.insert(QStringLiteral("HF_HUB_OFFLINE"), QStringLiteral("1"));
    env.insert(QStringLiteral("NO_PROXY"), QStringLiteral("127.0.0.1,localhost"));
    return env;
}

// Prompt-keyed translation memory shared by every capture in this process.
struct TranslationCache {
    QHash<QByteArray, QString> values;
    QList<QByteArray> order;

    static QByteArray key(const QString& target, const QString& prompt)
    {
        return QCryptographicHash::hash((target + QChar(0) + prompt).toUtf8(), QCryptographicHash::Sha256);
    }
    bool find(const QByteArray& key, QString* value)
    {
        const auto it = values.constFind(key);
        if (it == values.constEnd()) {
            return false;
        }
        *value = it.value();
        order.removeOne(key);
        order.append(key);
        return true;
    }
    void insert(const QByteArray& key, const QString& value)
    {
        if (!values.contains(key)) {
            order.append(key);
        }
        values.insert(key, value);
        while (order.size() > kCacheEntries) {
            values.remove(order.takeFirst());
        }
    }
    void clear()
    {
        values.clear();
        order.clear();
    }
};

TranslationCache& cache()
{
    static TranslationCache instance;
    return instance;
}

class LiteEngine final : public QObject {
public:
    using Callback = std::function<void(const QString& error)>;

    explicit LiteEngine(QObject* parent)
        : QObject(parent)
        , network_(new QNetworkAccessManager(this))
    {
        network_->setProxy(QNetworkProxy::NoProxy);
        network_->setAutoDeleteReplies(false);
        process_.setProcessChannelMode(QProcess::MergedChannels);
        idle_.setSingleShot(true);
        deadline_.setSingleShot(true);
        health_.setInterval(kHealthPollMs);
        pressure_.setInterval(5000);
        connect(&idle_, &QTimer::timeout, this, [this]() {
            if (activeJobs_ == 0) {
                Perf::log(QStringLiteral("LiteMt.release reason=idle"));
                release();
            }
        });
        connect(&pressure_, &QTimer::timeout, this, [this]() {
            const qint64 free = availableMemory();
            if (activeJobs_ == 0 && state_ == State::Ready && free >= 0 && free < kPressureReleaseBytes) {
                Perf::log(QStringLiteral("LiteMt.release reason=memory free_mib=%1").arg(free / 1048576));
                release();
            }
        });
        connect(&deadline_, &QTimer::timeout, this, [this]() { startFailed(QStringLiteral("start_timeout")); });
        connect(&health_, &QTimer::timeout, this, [this]() { pollHealth(); });
        connect(&process_, &QProcess::readyReadStandardOutput, this, [this]() { scanOutput(); });
        connect(&process_, &QProcess::errorOccurred, this, [this](QProcess::ProcessError error) {
            if (!stopping_ && error == QProcess::FailedToStart) {
                startFailed(QStringLiteral("failed_to_start"));
            }
        });
        connect(&process_, qOverload<int, QProcess::ExitStatus>(&QProcess::finished), this,
                [this](int code, QProcess::ExitStatus) {
            if (stopping_) {
                return;
            }
            if (state_ == State::Starting || state_ == State::WarmingUp) {
                startFailed(QStringLiteral("exited_%1").arg(code));
                return;
            }
            if (state_ == State::Ready) {
                Perf::log(QStringLiteral("LiteMt.exited code=%1 backend=%2").arg(code).arg(backend_));
                state_ = State::Stopped;
                idle_.stop();
                pressure_.stop();
            }
        });
        connect(QCoreApplication::instance(), &QCoreApplication::aboutToQuit, this, [this]() { release(); });
    }

    ~LiteEngine() override
    {
        release();
#ifdef Q_OS_WIN
        if (job_) {
            CloseHandle(job_);
        }
#endif
    }

    bool ready() const { return state_ == State::Ready; }
    qint64 pid() const { return state_ == State::Stopped ? 0 : process_.processId(); }
    QString backend() const { return state_ == State::Ready ? backend_ : QString(); }
    int slotCount() const { return kSlots; }
    double prefillRate() const { return prefillRate_; }
    double decodeRate() const { return decodeRate_; }

    void prewarm(const QString& root)
    {
        const QString canonical = canonicalRoot(root);
        if (state_ != State::Stopped && root_ == canonical) {
            return;
        }
        if (activeJobs_ > 0) {
            return;
        }
        const qint64 free = availableMemory();
        if (free >= 0 && free < kPrewarmMinimumFreeBytes) {
            Perf::log(QStringLiteral("LiteMt.prewarm skipped=memory free_mib=%1").arg(free / 1048576));
            return;
        }
        release();
        start(canonical);
    }

    void acquire(const QString& root, QObject* context, Callback callback)
    {
        const QString canonical = canonicalRoot(root);
        if (state_ != State::Stopped && root_ != canonical) {
            release();
        }
        idle_.stop();
        if (state_ == State::Ready) {
            QTimer::singleShot(0, context, [callback]() { callback(QString()); });
            return;
        }
        waiters_.append({QPointer<QObject>(context), std::move(callback)});
        if (state_ == State::Stopped) {
            start(canonical);
        }
    }

    void jobStarted()
    {
        ++activeJobs_;
        idle_.stop();
    }

    void jobFinished()
    {
        activeJobs_ = qMax(0, activeJobs_ - 1);
        if (activeJobs_ == 0 && state_ == State::Ready) {
            keepWarm();
        }
    }

    QNetworkReply* post(const QString& path, const QByteArray& body, int timeoutMs)
    {
        if (state_ != State::Ready && state_ != State::WarmingUp) {
            return nullptr;
        }
        QNetworkRequest request = requestFor(path, timeoutMs);
        request.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/json"));
        return network_->post(request, body);
    }

    void release()
    {
        ++generation_;
        const bool wasRunning = state_ != State::Stopped;
        state_ = State::Stopped;
        idle_.stop();
        deadline_.stop();
        health_.stop();
        pressure_.stop();
        if (healthReply_) {
            healthReply_->abort();
            healthReply_->deleteLater();
            healthReply_ = nullptr;
        }
        if (probe_) {
            probe_->disconnect(this);
            probe_->kill();
            probe_->deleteLater();
            probe_ = nullptr;
        }
        stopProcess();
        root_.clear();
        notifyWaiters(wasRunning ? QStringLiteral("本机翻译模型已释放，请重试。") : QString(), true);
    }

private:
    enum class State { Stopped, Verifying, Probing, Starting, WarmingUp, Ready };
    struct Waiter {
        QPointer<QObject> context;
        Callback callback;
    };

    QNetworkRequest requestFor(const QString& path, int timeoutMs) const
    {
        QNetworkRequest request(QUrl(QStringLiteral("http://127.0.0.1:%1%2").arg(port_).arg(path)));
        request.setRawHeader("Authorization", "Bearer " + token_.toLatin1());
        request.setAttribute(QNetworkRequest::Http2AllowedAttribute, false);
        request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::ManualRedirectPolicy);
        request.setTransferTimeout(timeoutMs);
        return request;
    }

    void start(const QString& root)
    {
        root_ = root;
        state_ = State::Verifying;
        deadline_.start(kStartTimeoutMs);
        const quint64 generation = ++generation_;
        const QString runtime = QDir(QCoreApplication::applicationDirPath()).filePath(QStringLiteral("engine-runtime"));
        if (!QFileInfo::exists(QDir(runtime).filePath(QStringLiteral("runtime-manifest.json")))) {
            launchCpu();
            return;
        }
        // Hashing the accelerated runtime once per process keeps the UI thread free.
        auto* watcher = new QFutureWatcher<GpuRuntime>(this);
        connect(watcher, &QFutureWatcher<GpuRuntime>::finished, this, [this, watcher, generation]() {
            watcher->deleteLater();
            if (generation != generation_ || state_ != State::Verifying) {
                return;
            }
            const GpuRuntime gpu = watcher->result();
            if (gpu.executable.isEmpty()) {
                Perf::log(QStringLiteral("LiteMt.gpu_runtime unavailable reason=%1").arg(gpu.problem));
                launchCpu();
                return;
            }
            probeDevices(gpu.executable, gpu.backend);
        });
        watcher->setFuture(QtConcurrent::run(verifyGpuRuntime, runtime));
    }

    void probeDevices(const QString& executable, const QString& backend)
    {
        state_ = State::Probing;
        probe_ = new QProcess(this);
        probe_->setProcessChannelMode(QProcess::MergedChannels);
        probe_->setProcessEnvironment(serverEnvironment(executable));
        probe_->setWorkingDirectory(QFileInfo(executable).absolutePath());
        auto* timeout = new QTimer(probe_);
        timeout->setSingleShot(true);
        connect(timeout, &QTimer::timeout, probe_, [this]() {
            if (probe_) {
                probe_->kill();
            }
        });
        connect(probe_, qOverload<int, QProcess::ExitStatus>(&QProcess::finished), this,
                [this, executable, backend](int, QProcess::ExitStatus status) {
            QProcess* probe = probe_;
            probe_ = nullptr;
            if (!probe) {
                return;
            }
            probe->deleteLater();
            if (state_ != State::Probing) {
                return;
            }
            if (status != QProcess::NormalExit) {
                Perf::log(QStringLiteral("LiteMt.gpu_runtime unavailable reason=device_query_failed"));
                launchCpu();
                return;
            }
            const QString text = QString::fromUtf8(probe->readAll());
            static const QRegularExpression devicePattern(
                QStringLiteral("^\\s*((?:Vulkan|CUDA)\\d+):\\s*([^\\n]+)"), QRegularExpression::MultilineOption);
            QString device;
            for (auto it = devicePattern.globalMatch(text); it.hasNext();) {
                const auto match = it.next();
                if (match.captured(2).contains(QStringLiteral("NVIDIA"))) {
                    device = match.captured(1);
                    break;
                }
            }
            launch(executable, backend, 99, device);
        });
        connect(probe_, &QProcess::errorOccurred, this, [this](QProcess::ProcessError error) {
            if (error == QProcess::FailedToStart && probe_ && state_ == State::Probing) {
                probe_->deleteLater();
                probe_ = nullptr;
                Perf::log(QStringLiteral("LiteMt.gpu_runtime unavailable reason=device_query_failed"));
                launchCpu();
            }
        });
        probe_->start(executable, {QStringLiteral("--list-devices")});
        timeout->start(kDeviceProbeTimeoutMs);
    }

    void launchCpu()
    {
        launch(LocalTextTranslationService::serverExecutable(root_), QStringLiteral("cpu"), 0, QString());
    }

    void launch(const QString& executable, const QString& backend, int gpuLayers, const QString& device)
    {
        stopProcess();
        state_ = State::Starting;
        backend_ = backend;
        gpuLayers_ = gpuLayers;
        offloadedLayers_ = 0;
        outputTail_.clear();
        port_ = freeLoopbackPort();
        token_ = randomToken();
        if (port_ == 0) {
            startFailed(QStringLiteral("no_loopback_port"));
            return;
        }
        const int threads = qBound(1, physicalCoreCount(), 8);
        QStringList arguments{
            QStringLiteral("--model"), LocalTextTranslationService::modelFile(root_),
            QStringLiteral("--host"), QStringLiteral("127.0.0.1"),
            QStringLiteral("--port"), QString::number(port_),
            QStringLiteral("--ctx-size"), QString::number(kSlots * kContextPerSlot),
            QStringLiteral("--parallel"), QString::number(kSlots),
            QStringLiteral("--threads"), QString::number(threads),
            QStringLiteral("--threads-batch"), QString::number(threads),
            QStringLiteral("--gpu-layers"), QString::number(gpuLayers),
            QStringLiteral("--jinja"), QStringLiteral("--offline"), QStringLiteral("--no-webui"),
            QStringLiteral("--poll"), QStringLiteral("0"),
            QStringLiteral("--api-key"), token_,
            QStringLiteral("--log-verbosity"), gpuLayers > 0 ? QStringLiteral("4") : QStringLiteral("3"),
        };
        if (gpuLayers > 0) {
            // Interactive translation does not need a 2048-token prefill buffer.
            arguments << QStringLiteral("--batch-size") << QStringLiteral("512")
                      << QStringLiteral("--ubatch-size") << QStringLiteral("128");
            if (!device.isEmpty()) {
                arguments << QStringLiteral("--device") << device;
            }
        }
        process_.setProcessEnvironment(serverEnvironment(executable));
        process_.setWorkingDirectory(QFileInfo(executable).absolutePath());
        process_.setProgram(executable);
        process_.setArguments(arguments);
        startedAt_.start();
        Perf::log(QStringLiteral("LiteMt.launch backend=%1 threads=%2 slots=%3 device=%4")
                      .arg(backend).arg(threads).arg(kSlots).arg(device.isEmpty() ? QStringLiteral("default") : device));
        process_.start();
        if (!process_.waitForStarted(10000)) {
            return; // errorOccurred(FailedToStart) reports the failure.
        }
        contain(process_.processId());
        health_.start();
    }

    void contain(qint64 pid)
    {
#ifdef Q_OS_WIN
        // The server dies with Visnip even after a crash of the client.
        if (!job_) {
            job_ = CreateJobObjectW(nullptr, nullptr);
            JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
            limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
            if (job_ && !SetInformationJobObject(job_, JobObjectExtendedLimitInformation, &limits, sizeof(limits))) {
                CloseHandle(job_);
                job_ = nullptr;
            }
        }
        if (job_) {
            HANDLE process = OpenProcess(PROCESS_SET_QUOTA | PROCESS_TERMINATE, FALSE, DWORD(pid));
            if (process) {
                AssignProcessToJobObject(job_, process);
                CloseHandle(process);
            }
        }
#else
        Q_UNUSED(pid);
#endif
    }

    void scanOutput()
    {
        // Consume without keeping prompts or responses; only the startup
        // layer-offload count is retained.
        outputTail_ += process_.readAllStandardOutput();
        if (state_ == State::Starting && gpuLayers_ > 0) {
            static const QRegularExpression offloaded(QStringLiteral("offloaded\\s+(\\d+)/(\\d+)\\s+layers"));
            const auto match = offloaded.match(QString::fromLatin1(outputTail_));
            if (match.hasMatch()) {
                offloadedLayers_ = match.captured(1).toInt();
            }
        }
        const qsizetype newline = outputTail_.lastIndexOf('\n');
        if (newline >= 0) {
            outputTail_.remove(0, newline + 1);
        }
        if (outputTail_.size() > 4096) {
            outputTail_.clear();
        }
    }

    void pollHealth()
    {
        if (state_ != State::Starting || healthReply_) {
            return;
        }
        healthReply_ = network_->get(requestFor(QStringLiteral("/health"), 1000));
        const quint64 generation = generation_;
        connect(healthReply_, &QNetworkReply::finished, this, [this, generation]() {
            QNetworkReply* reply = healthReply_;
            healthReply_ = nullptr;
            if (!reply) {
                return;
            }
            reply->deleteLater();
            const bool ok = reply->error() == QNetworkReply::NoError
                && reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt() == 200;
            if (!ok || generation != generation_ || state_ != State::Starting) {
                return;
            }
            health_.stop();
            if (gpuLayers_ > 0 && offloadedLayers_ == 0) {
                startFailed(QStringLiteral("gpu_not_actually_offloaded"));
                return;
            }
            warmup();
        });
    }

    void warmup()
    {
        state_ = State::WarmingUp;
        sendWarmup(0);
    }

    // Pass 0 loads the mapped weights and compiles GPU kernels before a user
    // waits. Pass 1 measures prefill and decode speed on this machine, which
    // decides how many regions are translated in parallel.
    void sendWarmup(int pass)
    {
        static const QString measurement = QStringLiteral(
            "Open the settings page to choose where new screenshots are saved, which keyboard shortcut "
            "starts a capture, and whether the translated image is copied to the clipboard automatically "
            "after the text recognition and translation have finished on this computer.");
        QJsonObject request = QJsonDocument::fromJson(LocalMt::buildChatRequest(
            LocalMt::buildRetryPrompt(pass == 0 ? QStringLiteral("Ready") : measurement, QStringLiteral("zh-Hans")),
            pass == 0 ? 4 : 16)).object();
        request.insert(QStringLiteral("cache_prompt"), false);
        QNetworkReply* reply = post(QStringLiteral("/v1/chat/completions"),
                                    QJsonDocument(request).toJson(QJsonDocument::Compact), 120000);
        const quint64 generation = generation_;
        connect(reply, &QNetworkReply::finished, this, [this, reply, generation, pass]() {
            reply->deleteLater();
            if (generation != generation_ || state_ != State::WarmingUp) {
                return;
            }
            if (reply->error() != QNetworkReply::NoError
                || reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt() != 200) {
                startFailed(QStringLiteral("warmup_failed"));
                return;
            }
            if (pass == 0) {
                sendWarmup(1);
                return;
            }
            LocalMt::ChatResult timing;
            if (LocalMt::parseChatResponse(reply->readAll(), &timing, nullptr)
                && timing.promptMs > 0 && timing.generationMs > 0) {
                prefillRate_ = qBound(1.0, timing.promptProcessed * 1000.0 / timing.promptMs, 100000.0);
                decodeRate_ = qBound(0.5, timing.completionTokens * 1000.0 / timing.generationMs, 10000.0);
            }
            state_ = State::Ready;
            deadline_.stop();
            Perf::log(QStringLiteral("OfflineEngine.lite_ready backend=%1 layers=%2 load_ms=%3 pid=%4 prefill_tps=%5 decode_tps=%6")
                          .arg(backend_).arg(offloadedLayers_).arg(startedAt_.elapsed()).arg(process_.processId())
                          .arg(prefillRate_, 0, 'f', 1).arg(decodeRate_, 0, 'f', 1));
            notifyWaiters(QString(), false);
            if (activeJobs_ == 0) {
                keepWarm();
            }
        });
    }

    void startFailed(const QString& code)
    {
        if (state_ == State::Stopped || state_ == State::Ready) {
            return;
        }
        Perf::log(QStringLiteral("OfflineEngine.lite_start_failed code=%1 backend=%2 elapsed_ms=%3")
                      .arg(code).arg(backend_).arg(startedAt_.isValid() ? startedAt_.elapsed() : -1));
        if (gpuLayers_ > 0) {
            // A failed accelerator never blocks translation; the CPU build is
            // part of the verified resources. Relaunch outside QProcess's own
            // signal emission.
            gpuLayers_ = 0;
            health_.stop();
            if (healthReply_) {
                healthReply_->abort();
            }
            state_ = State::Starting;
            const quint64 generation = generation_;
            QTimer::singleShot(0, this, [this, generation]() {
                if (generation == generation_ && state_ == State::Starting) {
                    launchCpu();
                }
            });
            return;
        }
        const QString message = code == QStringLiteral("start_timeout")
            ? QStringLiteral("本机翻译模型启动超时，原图保留；没有联网回退。")
            : QStringLiteral("本机翻译模型无法启动（%1），请在首选项中重新自检资源；没有联网回退。").arg(code);
        const QVector<Waiter> waiters = std::exchange(waiters_, {});
        release();
        for (const Waiter& waiter : waiters) {
            if (waiter.context) {
                waiter.callback(message);
            }
        }
    }

    void keepWarm()
    {
        int interval = kIdleReleaseMs;
        if (QStandardPaths::isTestModeEnabled()) {
            bool valid = false;
            const int value = qEnvironmentVariableIntValue("VISNIP_TEST_LITE_IDLE_MS", &valid);
            if (valid && value >= 50) {
                interval = value;
            }
        }
        idle_.start(interval);
        pressure_.start();
    }

    void stopProcess()
    {
        if (process_.state() == QProcess::NotRunning) {
            return;
        }
        stopping_ = true;
        process_.kill();
        process_.waitForFinished(3000);
        process_.readAll();
        stopping_ = false;
    }

    void notifyWaiters(const QString& error, bool onlyOnError)
    {
        if (onlyOnError && error.isEmpty()) {
            waiters_.clear();
            return;
        }
        const QVector<Waiter> waiters = std::exchange(waiters_, {});
        for (const Waiter& waiter : waiters) {
            if (waiter.context) {
                waiter.callback(error);
            }
        }
    }

    QNetworkAccessManager* network_ = nullptr;
    QProcess process_;
    QPointer<QProcess> probe_;
    QPointer<QNetworkReply> healthReply_;
    QTimer idle_;
    QTimer deadline_;
    QTimer health_;
    QTimer pressure_;
    QElapsedTimer startedAt_;
    QVector<Waiter> waiters_;
    QByteArray outputTail_;
    QString root_;
    QString backend_;
    QString token_;
    State state_ = State::Stopped;
    quint64 generation_ = 0;
    quint16 port_ = 0;
    int gpuLayers_ = 0;
    int offloadedLayers_ = 0;
    // Measured at start-up; the defaults describe a mainstream desktop CPU.
    double prefillRate_ = 200.0;
    double decodeRate_ = 20.0;
    int activeJobs_ = 0;
    bool stopping_ = false;
#ifdef Q_OS_WIN
    HANDLE job_ = nullptr;
#endif
};

QPointer<LiteEngine> resident;

LiteEngine* engine()
{
    if (!resident) {
        resident = new LiteEngine(QCoreApplication::instance());
    }
    return resident.data();
}

} // namespace

LocalTextTranslationService::LocalTextTranslationService(QObject* parent)
    : QObject(parent)
{
}

LocalTextTranslationService::~LocalTextTranslationService()
{
    if (busy_) {
        abortRequests();
        busy_ = false;
    }
    if (holdsEngine_ && resident) {
        resident->jobFinished();
    }
}

QString LocalTextTranslationService::serverExecutable(const QString& root)
{
    return QDir(canonicalRoot(root)).filePath(kServerRelativePath);
}

QString LocalTextTranslationService::modelFile(const QString& root)
{
    return QDir(canonicalRoot(root)).filePath(kModelRelativePath);
}

QString LocalTextTranslationService::runtimeProblem(const QString& root)
{
#ifdef Q_OS_WIN
    const QString beside = QFileInfo(serverExecutable(root)).absolutePath();
    const QString system = QDir(qEnvironmentVariable("SystemRoot", QStringLiteral("C:/Windows")))
        .filePath(QStringLiteral("System32"));
    QStringList missing;
    for (const QString& name : {QStringLiteral("msvcp140.dll"), QStringLiteral("vcruntime140.dll"),
                                QStringLiteral("vcruntime140_1.dll")}) {
        if (!QFileInfo(QDir(system).filePath(name)).isFile() && !QFileInfo(QDir(beside).filePath(name)).isFile()) {
            missing.append(name);
        }
    }
    if (!missing.isEmpty()) {
        return QStringLiteral("这台电脑缺少 Microsoft Visual C++ 运行库（%1），本机翻译引擎无法启动。"
                              "请安装微软官方的“Visual C++ 2015-2022 可再发行程序包（x64）”："
                              "https://aka.ms/vs/17/release/vc_redist.x64.exe ，然后重新点击“下载并启用”。")
            .arg(missing.join(QStringLiteral("、")));
    }
#else
    Q_UNUSED(root);
#endif
    return {};
}

QString LocalTextTranslationService::resourceProblem(const QString& root)
{
    // Every file was checked against its pinned SHA-256 when it was installed;
    // hashing the 1 GiB model again on each start would only add latency.
    for (const QString& relative : {kServerRelativePath, kModelRelativePath}) {
        const QFileInfo file(QDir(canonicalRoot(root)).filePath(relative));
        if (!file.isFile() || file.isSymLink()) {
            return QStringLiteral("轻量离线资源尚未就绪（缺少 %1），请在首选项中点击“下载并启用”。").arg(relative);
        }
    }
    return runtimeProblem(root);
}

void LocalTextTranslationService::prewarm(const QString& root)
{
    if (resourceProblem(root).isEmpty()) {
        engine()->prewarm(root);
    }
}

void LocalTextTranslationService::releaseSharedEngine()
{
    if (resident) {
        resident->release();
    }
    cache().clear();
}

bool LocalTextTranslationService::sharedEngineReady()
{
    return resident && resident->ready();
}

qint64 LocalTextTranslationService::sharedEngineProcessId()
{
    return resident ? resident->pid() : 0;
}

QString LocalTextTranslationService::sharedEngineBackend()
{
    return resident ? resident->backend() : QString();
}

QStringList LocalTextTranslationService::selfTestTexts()
{
    return {QStringLiteral("Project settings"), QStringLiteral("Keep 12 files in the local folder."),
            QStringLiteral("Save changes")};
}

QString LocalTextTranslationService::selfTestProblem(const QStringList& translations) const
{
    const QStringList sources = selfTestTexts();
    if (translations.size() != sources.size()) {
        return QStringLiteral("返回段数异常");
    }
    static const QRegularExpression han(QStringLiteral("[\\x{3400}-\\x{9FFF}]"));
    QStringList failures;
    for (int i = 0; i < sources.size(); ++i) {
        if (unresolved_.contains(i) || translations[i].trimmed() == sources[i]
            || !han.match(translations[i]).hasMatch()) {
            failures.append(QStringLiteral("第 %1 段未生成中文译文").arg(i + 1));
        }
    }
    return failures.join(QStringLiteral("；"));
}

void LocalTextTranslationService::translate(const QStringList& texts,
                                            const QString& targetLanguage,
                                            const QString& root)
{
    if (busy_) {
        emit failed(QStringLiteral("已有本机翻译任务正在运行。"));
        return;
    }
    if (!LocalMt::supportsTargetLanguage(targetLanguage)) {
        emit failed(QStringLiteral("轻量离线目前只支持中英文互译，请在首选项中切换目标语言。"));
        return;
    }
    const QString problem = resourceProblem(root);
    if (!problem.isEmpty()) {
        emit failed(problem);
        return;
    }
    busy_ = true;
    const quint64 serial = ++serial_;
    timer_.start();
    stats_ = {};
    unresolved_.clear();
    inflight_.clear();
    queue_.clear();
    target_ = targetLanguage;
    texts_ = texts;
    results_ = texts;
    translatedCount_ = 0;
    completed_ = 0;
    pageContext_ = LocalMt::buildPageContext(texts);

    QVector<int> order;
    QHash<int, QString> prompts;
    for (int i = 0; i < texts.size(); ++i) {
        const QString text = texts[i].trimmed();
        if (text.isEmpty() || LocalMt::isPreservedText(text) || LocalMt::alreadyInTarget(text, target_)) {
            continue;
        }
        const QString prompt = LocalMt::buildPrompt(
            pageContext_, LocalMt::glossaryTerms(text, texts, target_), text, target_);
        QString cached;
        if (cache().find(TranslationCache::key(target_, prompt), &cached)) {
            results_[i] = cached;
            ++translatedCount_;
            ++stats_.cacheHits;
            continue;
        }
        order.append(i);
        prompts.insert(i, prompt);
    }
    // Longest regions first: with parallel slots the slowest region decides
    // the total time, so it must not start last.
    std::stable_sort(order.begin(), order.end(),
                     [&texts](int a, int b) { return texts[a].size() > texts[b].size(); });
    for (const int index : std::as_const(order)) {
        Task task;
        task.index = index;
        task.prompt = prompts.value(index);
        task.firstPrompt = task.prompt;
        queue_.append(task);
    }
    pendingTotal_ = static_cast<int>(order.size());
    if (queue_.isEmpty()) {
        QTimer::singleShot(0, this, [this, serial]() {
            if (serial == serial_) {
                finishIfDone();
            }
        });
        return;
    }
    engine()->jobStarted();
    holdsEngine_ = true;
    if (!engine()->ready()) {
        emit phaseChanged(QStringLiteral("正在启动本机翻译模型…"));
    }
    QElapsedTimer waiting;
    waiting.start();
    engine()->acquire(root, this, [this, serial, waiting](const QString& error) {
        if (serial == serial_) {
            stats_.waitForEngineMs = waiting.elapsed();
        }
        engineReady(serial, error);
    });
}

void LocalTextTranslationService::engineReady(quint64 serial, const QString& error)
{
    if (serial != serial_ || !busy_) {
        return;
    }
    if (!error.isEmpty()) {
        finishFailure(error);
        return;
    }
    QVector<int> prompts;
    QVector<int> outputs;
    for (const Task& task : std::as_const(queue_)) {
        const int tokens = LocalMt::estimateTokens(texts_[task.index]);
        prompts.append(tokens + 12);   // own text plus chat template
        outputs.append(tokens + 3);
    }
    const int prefix = LocalMt::estimateTokens(LocalMt::buildPrompt(pageContext_, {}, QString(), target_)) + 8;
    concurrency_ = LocalMt::chooseConcurrency(prompts, outputs, prefix, engine()->prefillRate(),
                                              engine()->decodeRate(), engine()->slotCount());
    stats_.concurrency = concurrency_;
    emit phaseChanged(QStringLiteral("本机翻译中…（%1 段）").arg(pendingTotal_));
    pump();
}

void LocalTextTranslationService::pump()
{
    while (busy_ && inflight_.size() < concurrency_ && !queue_.isEmpty()) {
        send(queue_.takeFirst());
    }
}

void LocalTextTranslationService::send(const Task& task)
{
    const QString text = texts_[task.index].trimmed();
    QNetworkReply* reply = engine()->post(QStringLiteral("/v1/chat/completions"),
                                          LocalMt::buildChatRequest(task.prompt, LocalMt::maxOutputTokens(text)),
                                          task.retry ? kRetryTimeoutMs : kRequestTimeoutMs);
    if (!reply) {
        finishFailure(QStringLiteral("本机翻译模型已退出，原图保留；下次翻译会重新加载。"));
        return;
    }
    ++stats_.requests;
    if (task.retry) {
        ++stats_.retries;
    }
    inflight_.insert(reply, task);
    connect(reply, &QNetworkReply::finished, this, [this, reply]() { handleReply(reply); });
}

void LocalTextTranslationService::handleReply(QNetworkReply* reply)
{
    reply->deleteLater();
    if (!busy_ || !inflight_.contains(reply)) {
        return;
    }
    const Task task = inflight_.take(reply);
    const QString source = texts_[task.index].trimmed();
    const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    const QByteArray body = reply->readAll();
    if (reply->error() != QNetworkReply::NoError || status != 200) {
        if (!engine()->ready()) {
            finishFailure(QStringLiteral("本机翻译模型已退出，原图保留；下次翻译会重新加载。"));
            return;
        }
        // llama.cpp answers 400 when a prompt exceeds the slot context; the
        // context-free retry prompt is much shorter.
        const QString reason = status == 400 ? QStringLiteral("translation_context_limit")
            : reply->error() == QNetworkReply::OperationCanceledError ? QStringLiteral("translation_timeout")
                                                                      : QStringLiteral("translation_transport_error");
        reject(task, QString(), reason);
    } else {
        LocalMt::ChatResult chat;
        QString error;
        if (!LocalMt::parseChatResponse(body, &chat, &error)) {
            reject(task, QString(), error);
        } else {
            stats_.promptTokens += chat.promptTokens;
            stats_.promptProcessed += chat.promptProcessed;
            stats_.completionTokens += chat.completionTokens;
            stats_.promptMs += chat.promptMs;
            stats_.generationMs += chat.generationMs;
            if (chat.finishReason != QStringLiteral("stop")) {
                reject(task, chat.content, QStringLiteral("translation_truncated"));
            } else {
                const QString value = LocalMt::cleanOutput(chat.content, source, target_);
                const QString reason = LocalMt::rejectionReason(source, value, target_);
                if (reason.isEmpty()) {
                    accept(task.index, value, task.firstPrompt);
                } else {
                    reject(task, value, reason);
                }
            }
        }
    }
    if (!busy_) {
        return;
    }
    pump();
    finishIfDone();
}

void LocalTextTranslationService::accept(int index, const QString& value, const QString& prompt)
{
    QString translation = TextTranslationService::normalizeTranslationForTarget(value, target_);
    translation = TextTranslationService::restoreProtectedIdentifiers(texts_[index], translation);
    results_[index] = translation;
    ++translatedCount_;
    cache().insert(TranslationCache::key(target_, prompt), translation);
    emit progress(++completed_, pendingTotal_);
}

void LocalTextTranslationService::reject(const Task& task, const QString& value, const QString& reason)
{
    const QString source = texts_[task.index].trimmed();
    if (!task.retry) {
        Task retry;
        retry.index = task.index;
        retry.retry = true;
        retry.prompt = LocalMt::buildRetryPrompt(source, target_);
        retry.firstPrompt = task.firstPrompt;
        retry.firstValue = value;
        retry.firstReason = reason;
        queue_.append(retry);
        return;
    }
    if (reason == QStringLiteral("sentence_not_translated")
        && task.firstReason == QStringLiteral("sentence_not_translated")
        && LocalMt::looksLikeKeptName(source)
        && task.firstValue.trimmed() == source && value.trimmed() == source) {
        // Two independent prompts both kept a short capitalised label: a
        // product or proper name. Unchanged pixels are correct.
        emit progress(++completed_, pendingTotal_);
        return;
    }
    // Source pixels stay untouched; reported, never cached or labelled equivalent.
    unresolved_.insert(task.index, reason);
    emit progress(++completed_, pendingTotal_);
}

void LocalTextTranslationService::finishIfDone()
{
    if (!busy_ || !inflight_.isEmpty() || !queue_.isEmpty()) {
        return;
    }
    stats_.unresolved = static_cast<int>(unresolved_.size());
    const qint64 elapsed = timer_.elapsed();
    Perf::log(QStringLiteral("LiteMt.job units=%1 requests=%2 retries=%3 cache_hits=%4 unresolved=%5 "
                             "prompt_tokens=%6 prefilled=%7 completion_tokens=%8 prompt_ms=%9 generation_ms=%10 "
                             "wait_engine_ms=%11 total_ms=%12 backend=%13 concurrency=%14")
                  .arg(texts_.size()).arg(stats_.requests).arg(stats_.retries).arg(stats_.cacheHits)
                  .arg(stats_.unresolved).arg(stats_.promptTokens).arg(stats_.promptProcessed)
                  .arg(stats_.completionTokens).arg(qRound(stats_.promptMs)).arg(qRound(stats_.generationMs))
                  .arg(stats_.waitForEngineMs).arg(elapsed).arg(sharedEngineBackend()).arg(stats_.concurrency));
    if (!unresolved_.isEmpty() && translatedCount_ == 0) {
        finishFailure(QStringLiteral("本机模型未生成可用译文（%1），原图保留；没有联网回退。")
                          .arg(unresolved_.constBegin().value()));
        return;
    }
    busy_ = false;
    if (holdsEngine_) {
        holdsEngine_ = false;
        engine()->jobFinished();
    }
    emit succeeded(results_, elapsed);
}

void LocalTextTranslationService::finishFailure(const QString& message)
{
    abortRequests();
    busy_ = false;
    ++serial_;
    if (holdsEngine_) {
        holdsEngine_ = false;
        engine()->jobFinished();
    }
    emit failed(message);
}

void LocalTextTranslationService::cancel()
{
    if (!busy_) {
        return;
    }
    abortRequests();
    busy_ = false;
    ++serial_;
    if (holdsEngine_) {
        holdsEngine_ = false;
        engine()->jobFinished();
    }
    emit cancelled();
}

void LocalTextTranslationService::abortRequests()
{
    queue_.clear();
    const QList<QNetworkReply*> replies = inflight_.keys();
    inflight_.clear();
    for (QNetworkReply* reply : replies) {
        reply->abort();
    }
}

} // namespace Visnip
