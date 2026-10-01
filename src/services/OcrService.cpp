#include "services/OcrService.h"

#include "core/OcrLanguagePack.h"
#include "core/OcrPostProcess.h"
#include "core/PerfLog.h"

#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QPointer>
#include <QProcessEnvironment>
#include <QThread>

#include <algorithm>
#include <memory>
#include <utility>
#include <vector>

// onnxruntime_c_api.h spells the calling convention `_stdcall`, which GCC
// only accepts in gnu++ dialects; map it to the always-available
// double-underscore form (ignored on x64 anyway). The header must precede
// windows.h so the SAL macro clash resolves inside system headers, where GCC
// suppresses the redefinition warnings.
#if defined(__GNUC__) && !defined(_stdcall)
#define _stdcall __stdcall
#endif
#include <onnxruntime_c_api.h>

#include <windows.h>

namespace Visnip {

namespace {

constexpr int kDetMaxSide = 960;
constexpr int kDetSizeMultiple = 32;
constexpr int kRecHeight = 48;
constexpr int kRecMinWidth = 16;
constexpr int kRecMaxWidth = 3840;
constexpr int kRecPaddingPx = 2;
constexpr float kDetMean[3] = { 0.485f, 0.456f, 0.406f };
constexpr float kDetStd[3] = { 0.229f, 0.224f, 0.225f };
constexpr float kMinLineScore = 0.5f;

const char* const kDllName = "onnxruntime.dll";
const char* const kDetModelName = "ch_PP-OCRv4_det_infer.onnx";

QString ortMessage(const OrtApi* api, OrtStatus* status)
{
    if (!status) {
        return {};
    }
    const QString message = QString::fromUtf8(api->GetErrorMessage(status));
    api->ReleaseStatus(status);
    return message;
}

// Fills a CHW float tensor from an RGB888 image with per-channel
// (x/255 - mean) / std normalization. Hoisting the normalization constants
// and writing each plane directly keeps this per-pixel hot loop small.
void fillChwTensor(const QImage& rgb888,
                   const float* mean,
                   const float* std,
                   std::vector<float>& out,
                   bool bgr = false)
{
    const int width = rgb888.width();
    const int height = rgb888.height();
    const size_t plane = static_cast<size_t>(width) * height;
    out.resize(plane * 3);
    const float scale[3] = {
        1.0f / (255.0f * std[0]),
        1.0f / (255.0f * std[1]),
        1.0f / (255.0f * std[2]),
    };
    const float bias[3] = {
        -mean[0] / std[0],
        -mean[1] / std[1],
        -mean[2] / std[2],
    };
    float* channel0 = out.data();
    float* channel1 = channel0 + plane;
    float* channel2 = channel1 + plane;
    for (int y = 0; y < height; ++y) {
        const uchar* line = rgb888.constScanLine(y);
        const size_t rowBase = static_cast<size_t>(y) * width;
        for (int x = 0; x < width; ++x) {
            const uchar* px = line + x * 3;
            const size_t index = rowBase + static_cast<size_t>(x);
            channel0[index] = px[bgr ? 2 : 0] * scale[0] + bias[0];
            channel1[index] = px[1] * scale[1] + bias[1];
            channel2[index] = px[bgr ? 0 : 2] * scale[2] + bias[2];
        }
    }
}

} // namespace

struct OcrService::Runtime {
    struct Recognizer {
        OrtSession* session = nullptr;
        QByteArray inputName;
        QByteArray outputName;
        QVector<QString> charset;
        bool bgrInput = true;
        bool rightToLeft = false;
        int classCount = 0;
    };

    HMODULE library = nullptr;
    const OrtApi* api = nullptr;
    OrtEnv* env = nullptr;
    OrtSession* det = nullptr;
    OrtMemoryInfo* memoryInfo = nullptr;
    QByteArray detInputName;
    QByteArray detOutputName;
    QHash<QString, Recognizer> recognizers;

    ~Runtime()
    {
        if (api) {
            if (det) {
                api->ReleaseSession(det);
            }
            for (const Recognizer& recognizer : std::as_const(recognizers)) {
                if (recognizer.session) {
                    api->ReleaseSession(recognizer.session);
                }
            }
            if (memoryInfo) {
                api->ReleaseMemoryInfo(memoryInfo);
            }
            if (env) {
                api->ReleaseEnv(env);
            }
        }
        if (library) {
            FreeLibrary(library);
        }
    }

    bool createSession(const QString& modelPath,
                       OrtSessionOptions* options,
                       OrtSession** session,
                       QByteArray* inputName,
                       QByteArray* outputName,
                       QString* error)
    {
        OrtStatus* status = api->CreateSession(
            env, reinterpret_cast<const wchar_t*>(modelPath.utf16()), options, session);
        if (status) {
            *error = QStringLiteral("加载 OCR 模型失败（%1）：%2")
                         .arg(QFileInfo(modelPath).fileName(), ortMessage(api, status));
            return false;
        }
        OrtAllocator* allocator = nullptr;
        status = api->GetAllocatorWithDefaultOptions(&allocator);
        if (status) {
            *error = QStringLiteral("初始化 OCR 运行时分配器失败：%1").arg(ortMessage(api, status));
            return false;
        }
        char* name = nullptr;
        status = api->SessionGetInputName(*session, 0, allocator, &name);
        if (status) {
            *error = QStringLiteral("读取 OCR 模型输入失败：%1").arg(ortMessage(api, status));
            return false;
        }
        *inputName = QByteArray(name);
        allocator->Free(allocator, name);
        status = api->SessionGetOutputName(*session, 0, allocator, &name);
        if (status) {
            *error = QStringLiteral("读取 OCR 模型输出失败：%1").arg(ortMessage(api, status));
            return false;
        }
        *outputName = QByteArray(name);
        allocator->Free(allocator, name);
        return true;
    }

    bool sessionOutputClassCount(OrtSession* session,
                                 int* classCount,
                                 QString* error)
    {
        OrtTypeInfo* typeInfo = nullptr;
        OrtStatus* status = api->SessionGetOutputTypeInfo(session, 0, &typeInfo);
        if (status) {
            *error = QStringLiteral("读取 OCR 模型输出类型失败：%1")
                         .arg(ortMessage(api, status));
            return false;
        }
        const OrtTensorTypeAndShapeInfo* tensorInfo = nullptr;
        status = api->CastTypeInfoToTensorInfo(typeInfo, &tensorInfo);
        size_t dimensionCount = 0;
        if (!status) {
            status = api->GetDimensionsCount(tensorInfo, &dimensionCount);
        }
        std::vector<int64_t> dimensions(dimensionCount);
        if (!status) {
            status = api->GetDimensions(tensorInfo, dimensions.data(), dimensionCount);
        }
        api->ReleaseTypeInfo(typeInfo);
        if (status) {
            *error = QStringLiteral("读取 OCR 模型输出形状失败：%1")
                         .arg(ortMessage(api, status));
            return false;
        }
        if (dimensions.size() != 3 || dimensions.back() <= 1) {
            *error = QStringLiteral("OCR 识别模型输出形状不受支持。");
            return false;
        }
        *classCount = static_cast<int>(dimensions.back());
        return true;
    }

    // Runs a single-input single-output float inference. Returns the output
    // data copied into `output` along with its dimensions.
    bool run(OrtSession* session,
             const QByteArray& inputName,
             const QByteArray& outputName,
             const std::vector<float>& input,
             const std::vector<int64_t>& inputShape,
             std::vector<float>* output,
             std::vector<int64_t>* outputShape,
             QString* error)
    {
        OrtValue* inputTensor = nullptr;
        OrtStatus* status = api->CreateTensorWithDataAsOrtValue(
            memoryInfo, const_cast<float*>(input.data()), input.size() * sizeof(float),
            inputShape.data(), inputShape.size(),
            ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT, &inputTensor);
        if (status) {
            *error = QStringLiteral("构建 OCR 输入张量失败：%1").arg(ortMessage(api, status));
            return false;
        }

        const char* inputNames[] = { inputName.constData() };
        const char* outputNames[] = { outputName.constData() };
        OrtValue* outputValue = nullptr;
        status = api->Run(session, nullptr, inputNames, &inputTensor, 1, outputNames, 1, &outputValue);
        api->ReleaseValue(inputTensor);
        if (status) {
            *error = QStringLiteral("OCR 推理失败：%1").arg(ortMessage(api, status));
            return false;
        }

        bool ok = false;
        OrtTensorTypeAndShapeInfo* info = nullptr;
        status = api->GetTensorTypeAndShape(outputValue, &info);
        if (!status) {
            size_t dimCount = 0;
            status = api->GetDimensionsCount(info, &dimCount);
            if (!status) {
                outputShape->resize(dimCount);
                status = api->GetDimensions(info, outputShape->data(), dimCount);
            }
            api->ReleaseTensorTypeAndShapeInfo(info);
        }
        if (!status) {
            size_t total = 1;
            for (int64_t d : *outputShape) {
                total *= static_cast<size_t>(d > 0 ? d : 0);
            }
            float* data = nullptr;
            status = api->GetTensorMutableData(outputValue, reinterpret_cast<void**>(&data));
            if (!status) {
                output->assign(data, data + total);
                ok = true;
            }
        }
        if (status) {
            *error = QStringLiteral("读取 OCR 推理结果失败：%1").arg(ortMessage(api, status));
        }
        api->ReleaseValue(outputValue);
        return ok;
    }
};

OcrService::OcrService(QObject* parent)
    : QObject(parent)
{
    pool_.setMaxThreadCount(1);
}

OcrService::~OcrService()
{
    revision_.fetch_add(1, std::memory_order_release);
    pool_.clear();
    pool_.waitForDone();
    delete runtime_;
}

QString OcrService::assetDirectory()
{
    const QString override = QProcessEnvironment::systemEnvironment().value(QStringLiteral("VISNIP_OCR_DIR"));
    if (!override.isEmpty()) {
        return QDir(override).absolutePath();
    }
    return QCoreApplication::applicationDirPath() + QStringLiteral("/ocr");
}

bool OcrService::assetsPresent(const QString& packId, QString* missingFile)
{
    const QDir dir(assetDirectory());
    for (const char* name : { kDllName, kDetModelName }) {
        const QString path = dir.filePath(QString::fromLatin1(name));
        if (!QFile::exists(path)) {
            if (missingFile) {
                *missingFile = path;
            }
            return false;
        }
    }
    return Ocr::languagePackInstalled(Ocr::resolvedLanguagePackId(packId),
                                      missingFile);
}

bool OcrService::assetsPresent(QString* missingFile)
{
    return assetsPresent(Ocr::defaultLanguagePackId(), missingFile);
}

OcrService::Runtime* OcrService::ensureRuntime(const QString& requestedPackId,
                                               QString* error)
{
    const QString packId = Ocr::resolvedLanguagePackId(requestedPackId);
    const Ocr::LanguagePack* pack = Ocr::languagePack(packId);
    QString missing;
    if (!pack || !assetsPresent(packId, &missing)) {
        *error = QStringLiteral("缺少 OCR 组件：%1")
                     .arg(missing.isEmpty() ? packId : missing);
        return nullptr;
    }

    const QDir dir(assetDirectory());
    if (!runtime_) {
        Perf::ScopedTimer timer(QStringLiteral("Ocr.runtimeInit"));
        auto runtime = std::make_unique<Runtime>();
        const QString dllPath = dir.filePath(QString::fromLatin1(kDllName));
        runtime->library = LoadLibraryExW(
            reinterpret_cast<const wchar_t*>(dllPath.utf16()),
            nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
        if (!runtime->library) {
            *error = QStringLiteral("加载 onnxruntime.dll 失败（错误码 %1）。")
                         .arg(GetLastError());
            return nullptr;
        }
        const auto getApiBase = reinterpret_cast<const OrtApiBase* (ORT_API_CALL*)()>(
            reinterpret_cast<void*>(GetProcAddress(runtime->library, "OrtGetApiBase")));
        if (!getApiBase) {
            *error = QStringLiteral("onnxruntime.dll 中未找到 OrtGetApiBase 入口。");
            return nullptr;
        }
        runtime->api = getApiBase()->GetApi(ORT_API_VERSION);
        if (!runtime->api) {
            *error = QStringLiteral("onnxruntime 版本不兼容（需要 API %1）。")
                         .arg(ORT_API_VERSION);
            return nullptr;
        }

        OrtStatus* status = runtime->api->CreateEnv(
            ORT_LOGGING_LEVEL_ERROR, "visnip-ocr", &runtime->env);
        if (status) {
            *error = QStringLiteral("初始化 OCR 运行时失败：%1")
                         .arg(ortMessage(runtime->api, status));
            return nullptr;
        }
        status = runtime->api->CreateCpuMemoryInfo(
            OrtArenaAllocator, OrtMemTypeDefault, &runtime->memoryInfo);
        if (status) {
            *error = QStringLiteral("初始化 OCR 内存信息失败：%1")
                         .arg(ortMessage(runtime->api, status));
            return nullptr;
        }

        OrtSessionOptions* options = nullptr;
        status = runtime->api->CreateSessionOptions(&options);
        if (status) {
            *error = QStringLiteral("创建 OCR 会话选项失败：%1")
                         .arg(ortMessage(runtime->api, status));
            return nullptr;
        }
        runtime->api->SetIntraOpNumThreads(
            options, qMax(2, QThread::idealThreadCount() / 2));
        runtime->api->SetSessionGraphOptimizationLevel(options, ORT_ENABLE_ALL);
        const bool detectorReady = runtime->createSession(
            dir.filePath(QString::fromLatin1(kDetModelName)), options,
            &runtime->det, &runtime->detInputName, &runtime->detOutputName,
            error);
        runtime->api->ReleaseSessionOptions(options);
        if (!detectorReady) {
            return nullptr;
        }

        Perf::log(QStringLiteral("Ocr.runtimeInit version=%1")
                      .arg(QString::fromUtf8(getApiBase()->GetVersionString())));
        runtime_ = runtime.release();
    }

    if (runtime_->recognizers.contains(packId)) {
        return runtime_;
    }

    Perf::ScopedTimer timer(QStringLiteral("Ocr.recognizerInit.%1").arg(packId));
    Runtime::Recognizer recognizer;
    OrtSessionOptions* options = nullptr;
    OrtStatus* status = runtime_->api->CreateSessionOptions(&options);
    if (status) {
        *error = QStringLiteral("创建 OCR 会话选项失败：%1")
                     .arg(ortMessage(runtime_->api, status));
        return nullptr;
    }
    runtime_->api->SetIntraOpNumThreads(
        options, qMax(2, QThread::idealThreadCount() / 2));
    runtime_->api->SetSessionGraphOptimizationLevel(options, ORT_ENABLE_ALL);
    const bool sessionReady = runtime_->createSession(
        Ocr::packModelPath(*pack), options, &recognizer.session,
        &recognizer.inputName, &recognizer.outputName, error);
    runtime_->api->ReleaseSessionOptions(options);
    if (!sessionReady) {
        return nullptr;
    }

    QFile dictFile(Ocr::packDictionaryPath(*pack));
    if (!dictFile.open(QIODevice::ReadOnly)) {
        runtime_->api->ReleaseSession(recognizer.session);
        *error = QStringLiteral("无法读取 OCR 字典：%1").arg(dictFile.fileName());
        return nullptr;
    }
    const QList<QByteArray> dictionaryLines = dictFile.readAll().split('\n');
    recognizer.charset.reserve(dictionaryLines.size());
    for (QByteArray line : dictionaryLines) {
        if (line.endsWith('\r')) {
            line.chop(1);
        }
        if (!line.isEmpty()) {
            recognizer.charset.append(QString::fromUtf8(line));
        }
    }
    if (recognizer.charset.isEmpty()
        || (pack->dictionaryEntries > 0
            && recognizer.charset.size() != pack->dictionaryEntries)) {
        runtime_->api->ReleaseSession(recognizer.session);
        *error = QStringLiteral("OCR 字典条目数不匹配（%1，实际 %2，预期 %3）。")
                     .arg(dictFile.fileName())
                     .arg(recognizer.charset.size())
                     .arg(pack->dictionaryEntries);
        return nullptr;
    }
    if (!runtime_->sessionOutputClassCount(
            recognizer.session, &recognizer.classCount, error)) {
        runtime_->api->ReleaseSession(recognizer.session);
        return nullptr;
    }
    const int expectedClasses = recognizer.charset.size() + 2;
    if (recognizer.classCount != expectedClasses) {
        runtime_->api->ReleaseSession(recognizer.session);
        *error = QStringLiteral("OCR 模型与字典不匹配（模型类别 %1，字典要求 %2）。")
                     .arg(recognizer.classCount)
                     .arg(expectedClasses);
        return nullptr;
    }
    recognizer.bgrInput = !pack->legacy;
    recognizer.rightToLeft = pack->rightToLeft;
    runtime_->recognizers.insert(packId, recognizer);
    Perf::log(QStringLiteral("Ocr.recognizerInit pack=%1 classes=%2 charset=%3 rtl=%4")
                  .arg(packId)
                  .arg(recognizer.classCount)
                  .arg(recognizer.charset.size())
                  .arg(recognizer.rightToLeft));
    return runtime_;
}

QVector<OcrTextLine> OcrService::runPipeline(
    const QImage& image,
    const QString& requestedPackId,
    const std::function<bool()>& shouldAbort,
    QString* error)
{
    const QString packId = Ocr::resolvedLanguagePackId(requestedPackId);
    Runtime* rt = ensureRuntime(packId, error);
    if (!rt) {
        return {};
    }
    const auto recognizerIt = rt->recognizers.constFind(packId);
    if (recognizerIt == rt->recognizers.cend()) {
        *error = QStringLiteral("OCR 识别模型未就绪：%1").arg(packId);
        return {};
    }
    const Runtime::Recognizer& recognizer = recognizerIt.value();

    const QImage rgb = image.convertToFormat(QImage::Format_RGB888);
    if (rgb.isNull()) {
        *error = QStringLiteral("准备 OCR 输入图像失败，内存可能不足。");
        return {};
    }
    const QSize sourceSize = rgb.size();

    // --- Detection ---
    Perf::ScopedTimer timer(QStringLiteral("Ocr.pipeline"));
    QElapsedTimer phaseTimer;
    phaseTimer.start();
    const int maxSide = qMax(sourceSize.width(), sourceSize.height());
    const double ratio = maxSide > kDetMaxSide ? static_cast<double>(kDetMaxSide) / maxSide : 1.0;
    auto roundToMultiple = [](double value) {
        const int rounded = qRound(value / kDetSizeMultiple) * kDetSizeMultiple;
        return qMax(kDetSizeMultiple, rounded);
    };
    const int detW = roundToMultiple(sourceSize.width() * ratio);
    const int detH = roundToMultiple(sourceSize.height() * ratio);
    const QImage detInput = rgb.scaled(detW, detH, Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
    if (detInput.isNull()) {
        *error = QStringLiteral("缩放 OCR 检测输入失败，内存可能不足。");
        return {};
    }

    const qint64 detResizeMs = phaseTimer.restart();
    std::vector<float> detTensor;
    fillChwTensor(detInput, kDetMean, kDetStd, detTensor);
    const qint64 detNormalizeMs = phaseTimer.restart();
    std::vector<float> detOutput;
    std::vector<int64_t> detShape;
    if (!rt->run(rt->det, rt->detInputName, rt->detOutputName,
                 detTensor, { 1, 3, detH, detW }, &detOutput, &detShape, error)) {
        return {};
    }
    const qint64 detInferenceMs = phaseTimer.restart();
    if (detShape.size() != 4 || detShape[2] <= 0 || detShape[3] <= 0) {
        *error = QStringLiteral("OCR 检测输出形状异常。");
        return {};
    }
    const int mapH = static_cast<int>(detShape[2]);
    const int mapW = static_cast<int>(detShape[3]);
    QVector<QRect> boxes = Ocr::extractDetBoxes(
        detOutput.data(), mapW, mapH,
        static_cast<double>(sourceSize.width()) / mapW,
        static_cast<double>(sourceSize.height()) / mapH,
        sourceSize);
    boxes = Ocr::recoverFragmentedLineBoxes(boxes, rgb);
    const qint64 detPostProcessMs = phaseTimer.restart();
    Perf::log(QStringLiteral("Ocr.det source=%1x%2 input=%3x%4 boxes=%5 resize=%6ms normalize=%7ms inference=%8ms post=%9ms")
                  .arg(sourceSize.width())
                  .arg(sourceSize.height())
                  .arg(detW)
                  .arg(detH)
                  .arg(boxes.size())
                  .arg(detResizeMs)
                  .arg(detNormalizeMs)
                  .arg(detInferenceMs)
                  .arg(detPostProcessMs));

    // Exclude repeated navigation-icon columns before recognition. This uses
    // detector geometry and source pixels only, so the recognizer never sees a
    // decorative glyph it could decode as a character.
    const QVector<Ocr::RefinedOcrBox> refinedBoxes =
        Ocr::refineRecognitionBoxes(boxes, rgb);
    const qsizetype refinedCount = std::count_if(
        refinedBoxes.cbegin(), refinedBoxes.cend(),
        [](const Ocr::RefinedOcrBox& box) {
            return box.leadingIconSeparated;
        });
    Perf::log(QStringLiteral("Ocr.refine boxes=%1 leading_icons=%2")
                  .arg(refinedBoxes.size())
                  .arg(refinedCount));

    // --- Recognition ---
    QVector<OcrTextLine> lines;
    lines.reserve(refinedBoxes.size());
    std::vector<float> recTensor;
    std::vector<float> recOutput;
    std::vector<int64_t> recShape;
    qint64 recPrepareMs = 0;
    qint64 recInferenceMs = 0;
    qint64 recDecodeMs = 0;
    for (const Ocr::RefinedOcrBox& refined : refinedBoxes) {
        if (shouldAbort && shouldAbort()) {
            *error = QString();
            return {};
        }
        const QRect box = refined.textBox;
        const QRect recognitionBox = Ocr::recognitionCropBox(box, rgb);
        const int verticalPadding = recognitionBox == box
            ? kRecPaddingPx
            : 0;
        const QRect padded = recognitionBox.adjusted(
                                          -kRecPaddingPx, -verticalPadding,
                                          kRecPaddingPx, verticalPadding)
                                 .intersected(QRect(QPoint(0, 0), sourceSize));
        if (padded.height() < 4 || padded.width() < 4) {
            continue;
        }
        const int targetW = qBound(kRecMinWidth,
                                   qRound(static_cast<double>(kRecHeight) * padded.width() / padded.height()),
                                   kRecMaxWidth);
        const QImage crop = rgb.copy(padded).scaled(targetW, kRecHeight,
                                                    Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
        if (crop.isNull()) {
            continue;
        }
        constexpr float recMean[3] = { 0.5f, 0.5f, 0.5f };
        constexpr float recStd[3] = { 0.5f, 0.5f, 0.5f };
        // PP-OCRv5 recognition models are trained on normalized BGR input;
        // retain RGB for the migration-only V4 recognizer.
        fillChwTensor(crop, recMean, recStd, recTensor,
                      recognizer.bgrInput);
        recPrepareMs += phaseTimer.restart();
        if (!rt->run(recognizer.session, recognizer.inputName,
                     recognizer.outputName,
                     recTensor, { 1, 3, kRecHeight, targetW },
                     &recOutput, &recShape, error)) {
            return {};
        }
        recInferenceMs += phaseTimer.restart();
        if (recShape.size() != 3 || recShape[1] <= 0
            || recShape[2] != recognizer.classCount) {
            continue;
        }
        float score = 0.0f;
        QString text = Ocr::ctcGreedyDecode(
            recOutput.data(), static_cast<int>(recShape[1]),
            static_cast<int>(recShape[2]), recognizer.charset, &score);
        if (recognizer.rightToLeft) {
            text = Ocr::reverseArabicPrediction(text);
        }
        text = Ocr::correctConfusableText(text);
        recDecodeMs += phaseTimer.restart();
        if (text.trimmed().isEmpty() || score < kMinLineScore) {
            continue;
        }
        OcrTextLine line(box,
                         text,
                         score,
                         refined.detectedBox,
                         refined.leadingIconSeparated,
                         refined.reason);
        Ocr::refineIsolatedLeadingIcon(&line, rgb);
        lines.append(line);
    }
    Perf::log(QStringLiteral("Ocr.rec pack=%1 boxes=%2 lines=%3 prepare=%4ms inference=%5ms decode=%6ms")
                  .arg(packId)
                  .arg(boxes.size())
                  .arg(lines.size())
                  .arg(recPrepareMs)
                  .arg(recInferenceMs)
                  .arg(recDecodeMs));
    return lines;
}

QVector<OcrTextLine> OcrService::recognizeSync(const QImage& image,
                                                   const QString& packId,
                                                   QString* error)
{
    QString localError;
    return runPipeline(image, packId, {}, error ? error : &localError);
}

QVector<OcrTextLine> OcrService::recognizeSync(const QImage& image,
                                                   QString* error)
{
    return recognizeSync(image, Ocr::defaultLanguagePackId(), error);
}

void OcrService::prewarm(const QString& requestedPackId)
{
    const QString packId = Ocr::resolvedLanguagePackId(requestedPackId);
    if (prewarmQueued_.contains(packId)) {
        return;
    }
    prewarmQueued_.insert(packId);
    const QPointer<OcrService> guard(this);
    pool_.start([guard, this, packId]() {
        QElapsedTimer timer;
        timer.start();
        QString error;
        const bool ready = ensureRuntime(packId, &error) != nullptr;
        const qint64 elapsed = timer.elapsed();
        if (!guard) {
            return;
        }
        QMetaObject::invokeMethod(
            guard.data(),
            [guard, packId, ready, error, elapsed]() {
                if (guard) {
                    guard->prewarmQueued_.remove(packId);
                    emit guard->prewarmFinished(
                        packId, ready, error, elapsed);
                }
            },
            Qt::QueuedConnection);
    });
}

void OcrService::prewarm()
{
    prewarm(Ocr::defaultLanguagePackId());
}

quint64 OcrService::recognize(const QImage& image,
                              const QString& requestedPackId)
{
    const QString packId = Ocr::resolvedLanguagePackId(requestedPackId);
    const quint64 revision = revision_.fetch_add(1, std::memory_order_acq_rel) + 1;
    if (image.isNull()) {
        busy_ = false;
        QMetaObject::invokeMethod(this, [this, revision, packId]() {
            emit failed(revision, packId,
                        QStringLiteral("没有可识别的图像内容。"));
        }, Qt::QueuedConnection);
        return revision;
    }

    busy_ = true;
    const QPointer<OcrService> guard(this);
    const QImage source = image;
    pool_.start([guard, this, source, revision, packId]() {
        // `this` is only dereferenced through invokeMethod once guard is
        // validated; the destructor drains the pool before deleting.
        QElapsedTimer timer;
        timer.start();
        QString error;
        auto shouldAbort = [this, revision]() {
            return revision_.load(std::memory_order_acquire) != revision;
        };
        QVector<OcrTextLine> lines = runPipeline(
            source, packId, shouldAbort, &error);
        const qint64 elapsed = timer.elapsed();
        if (!guard || shouldAbort()) {
            return;
        }
        QMetaObject::invokeMethod(
            guard.data(),
            [guard, revision, packId, lines = std::move(lines),
             error, elapsed]() {
                if (!guard
                    || guard->revision_.load(std::memory_order_acquire)
                        != revision) {
                    return;
                }
                guard->busy_ = false;
                if (!error.isEmpty()) {
                    emit guard->failed(revision, packId, error);
                } else {
                    emit guard->finished(
                        revision, packId, lines, elapsed);
                }
            },
            Qt::QueuedConnection);
    });
    return revision;
}

quint64 OcrService::recognize(const QImage& image)
{
    return recognize(image, Ocr::defaultLanguagePackId());
}

void OcrService::cancel(quint64 revision)
{
    quint64 expected = revision;
    if (revision_.compare_exchange_strong(expected, revision + 1,
                                          std::memory_order_acq_rel)) {
        busy_ = false;
    }
}

} // namespace Visnip
