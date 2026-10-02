#include "core/OfflineResourceCatalog.h"
#include <QRegularExpression>
#include <QStringList>

namespace Visnip {

QVector<OfflineResourceFile> OfflineResourceCatalog::liteFiles()
{
    // Official CPU build of the llama.cpp release the client is pinned to
    // (see LocalTextTranslationService); it needs the Visual C++ runtime.
    OfflineResourceFile server;
    server.id = QStringLiteral("llama-cpu");
    server.label = QStringLiteral("llama.cpp b10964");
    server.sources = {QUrl(QStringLiteral(
        "https://github.com/ggml-org/llama.cpp/releases/download/b10964/llama-b10964-bin-win-cpu-x64.zip"))};
    server.sha256 = "917f39c076402c421224824607397af20f53625a60defc20e8dd22446bf4c5d7";
    server.size = 18427629;
    server.installedBytes = 46735645;
    server.install = OfflineResourceFile::Install::ExtractZip;
    server.target = QStringLiteral("llama");

    // Tencent's own releases of the same file; ModelScope first because it is
    // reachable in mainland China, Hugging Face as the fallback.
    OfflineResourceFile model;
    model.id = QStringLiteral("hy-mt2-q4km");
    model.label = QStringLiteral("Hy-MT2-1.8B 翻译模型");
    model.sources = {
        QUrl(QStringLiteral("https://www.modelscope.cn/models/Tencent-Hunyuan/Hy-MT2-1.8B-GGUF/resolve/"
                            "ef1d40b8b315575d30d1eb6579996130b8fc0bb2/Hy-MT2-1.8B-Q4_K_M.gguf")),
        QUrl(QStringLiteral("https://huggingface.co/tencent/Hy-MT2-1.8B-GGUF/resolve/"
                            "a0c709d9fac510f2c807aa3af52872340dc37a4a/Hy-MT2-1.8B-Q4_K_M.gguf")),
    };
    model.sha256 = "dc5f44fcf1fa496ee7ad725982c0c8c553a4de00259b53af84c4b89fb0c06699";
    model.size = 1133080448;
    model.install = OfflineResourceFile::Install::Place;
    model.target = QStringLiteral("models/Hy-MT2-1.8B-Q4_K_M.gguf");
    return {server, model};
}

bool OfflineResourceCatalog::isAllowedDownload(const QUrl& url)
{
    if (!url.isValid() || url.scheme() != QStringLiteral("https") || url.port(443) != 443
        || !url.userName().isEmpty() || !url.password().isEmpty() || url.hasFragment()) {
        return false;
    }
    // Signed CDN redirects carry query strings; integrity comes from SHA-256.
    const QString host = url.host().toLower();
    static const QStringList domains{QStringLiteral("github.com"), QStringLiteral("githubusercontent.com"),
                                     QStringLiteral("modelscope.cn"), QStringLiteral("huggingface.co"),
                                     QStringLiteral("hf.co")};
    for (const QString& domain : domains) {
        if (host == domain || host.endsWith(QLatin1Char('.') + domain)) {
            return true;
        }
    }
    return false;
}

bool OfflineResourceCatalog::acceptsRange(int status, const QByteArray& range, qint64 offset,
                                          qint64 total, qint64 length, qint64 requestedEnd)
{
    if (offset < 0 || offset >= total || total <= 0) return false;
    if (status == 200) return offset == 0 && (length == -1 || length == total);
    if (status != 206) return false;
    const qint64 end = requestedEnd >= 0 ? requestedEnd : total - 1;
    if (end < offset || end >= total) return false;
    const auto match = QRegularExpression(QStringLiteral("^bytes ([0-9]+)-([0-9]+)/([0-9]+)$"))
        .match(QString::fromLatin1(range));
    return match.hasMatch() && match.captured(1).toLongLong() == offset
        && match.captured(2).toLongLong() == end && match.captured(3).toLongLong() == total
        && (length == -1 || length == end + 1 - offset);
}
}
