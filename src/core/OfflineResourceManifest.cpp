#include "core/OfflineResourceManifest.h"
#include <QCryptographicHash>
#include <QDateTime>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <QVersionNumber>
#ifdef Q_OS_WIN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <bcrypt.h>
#endif

namespace Visnip {
namespace {
bool verify(const QByteArray& payload, const QByteArray& signature)
{
#ifdef Q_OS_WIN
    const QByteArray modulus = QByteArray::fromHex(
        "d4fae879375b853f196f7194d18df50bd8a7fd0ac6dd56b37592c649b0fef49e9b92ec22f6952710640cd1262a5382cabe07260f5130bd5d9b0ee8a4c8ccecc5dbb4f6ffd80440a857201824dc0c24adb1f1dfc399563ca5bc96b327b348e0afc55cd248de8426da11a74612e419d36ae97177be1103529691f474af265c4c3c4716e5ad061bd687e07aefe928ca7df7a0ec9022040e5c3ed1813afd24511cf70abf72e4ba55b95e88136f6378080ed306c9537da2c119828c9b8344fc229f1b6fdc6414d45505a30ed5c3ad87468a90a65d8b3f4494e95321b2f1144965349e5d4e8b5c803fb7aee6001b0db30b630f07e5b38f2e30aca3424d35db46f6224b8c451dd332dd3a98432be46b1482ccfb5978982e16e4096501b8f8be67f8522b6d70d03d8fa791cbcc19a3ef50a129a2be7b15e33f8ddaeab7d725c5d14fe19dddd75fe0d11f0c1e9867570b51f1769436500d38709bf04fe2288e5f28228df40f4efd46bc110d30c956c48aff540052ed3442fbcb11f7c12c4fc841ab05b0d7");
    const QByteArray exponent = QByteArray::fromHex("010001");
    if (signature.size() != modulus.size()) return false;
    BCRYPT_RSAKEY_BLOB header{BCRYPT_RSAPUBLIC_MAGIC, 3072, 3, 384, 0, 0};
    QByteArray blob(reinterpret_cast<const char*>(&header), sizeof(header));
    blob += exponent; blob += modulus;
    BCRYPT_ALG_HANDLE algorithm = nullptr;
    BCRYPT_KEY_HANDLE key = nullptr;
    bool valid = false;
    if (BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_RSA_ALGORITHM, nullptr, 0) >= 0) {
        if (BCryptImportKeyPair(algorithm, nullptr, BCRYPT_RSAPUBLIC_BLOB, &key,
                reinterpret_cast<PUCHAR>(blob.data()), ULONG(blob.size()), 0) >= 0) {
            QByteArray hash = QCryptographicHash::hash(payload, QCryptographicHash::Sha256);
            BCRYPT_PKCS1_PADDING_INFO padding{BCRYPT_SHA256_ALGORITHM};
            valid = BCryptVerifySignature(key, &padding, reinterpret_cast<PUCHAR>(hash.data()), ULONG(hash.size()),
                reinterpret_cast<PUCHAR>(const_cast<char*>(signature.constData())), ULONG(signature.size()), BCRYPT_PAD_PKCS1) >= 0;
            BCryptDestroyKey(key);
        }
        BCryptCloseAlgorithmProvider(algorithm, 0);
    }
    return valid;
#else
    Q_UNUSED(payload); Q_UNUSED(signature);
    return false; // No unauthenticated cross-platform fallback.
#endif
}
}

bool OfflineResourceManifest::isAllowedDownload(const QUrl& url)
{
    return url.isValid() && url.scheme() == QStringLiteral("https")
        && url.host() == QStringLiteral("vislate.ipxair.com") && url.port(443) == 443
        && url.userName().isEmpty() && url.password().isEmpty() && !url.hasQuery() && !url.hasFragment()
        && QRegularExpression(QStringLiteral("^/api/v1/downloads/[a-f0-9]{32}$")).match(url.path()).hasMatch();
}

bool OfflineResourceManifest::acceptsRange(int status, const QByteArray& range, qint64 offset,
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

bool OfflineResourceManifest::parse(const QByteArray& envelope, OfflineResourceManifest* result, QString* error, qint64 now)
{
    const auto fail = [error](const QString& message) { if (error) *error = message; return false; };
    if (!result || envelope.size() > 32768) return fail(QStringLiteral("资源清单过大或无效。"));
    const auto outer = QJsonDocument::fromJson(envelope).object();
    if (outer.value(QStringLiteral("algorithm")).toString() != QStringLiteral("rsa-sha256")
        || outer.value(QStringLiteral("key_id")).toString() != QStringLiteral("vislate-offline-rsa-2026-09"))
        return fail(QStringLiteral("资源清单签名类型不受信任。"));
    const auto payload = QByteArray::fromBase64Encoding(outer.value(QStringLiteral("payload")).toString().toLatin1(), QByteArray::AbortOnBase64DecodingErrors);
    const auto signature = QByteArray::fromBase64Encoding(outer.value(QStringLiteral("signature")).toString().toLatin1(), QByteArray::AbortOnBase64DecodingErrors);
    if (!payload || !signature || !verify(payload.decoded, signature.decoded))
        return fail(QStringLiteral("资源清单签名验证失败，已拒绝下载和执行。"));
    const auto object = QJsonDocument::fromJson(payload.decoded).object();
    if (now == 0) now = QDateTime::currentSecsSinceEpoch();
    const qint64 issued = object.value(QStringLiteral("issued_at")).toInteger();
    const qint64 expires = object.value(QStringLiteral("expires_at")).toInteger();
    if (issued < 1 || issued > now + 86400 || expires <= now || expires - issued > 366LL * 86400)
        return fail(QStringLiteral("资源清单已过期或系统时间不正确。"));
    if (object.value(QStringLiteral("schema_version")).toInt() != 1
        || object.value(QStringLiteral("engine_abi")).toInt() != 1
        || object.value(QStringLiteral("platform")).toString() != QStringLiteral("windows-x64")
        || object.value(QStringLiteral("channel")).toString() != QStringLiteral("offline-preview"))
        return fail(QStringLiteral("资源清单与当前平台不兼容。"));
    const auto required = QVersionNumber::fromString(object.value(QStringLiteral("minimum_client_version")).toString());
    if (required.isNull() || required > QVersionNumber::fromString(QStringLiteral(VISNIP_VERSION)))
        return fail(QStringLiteral("请先更新 Visnip 客户端，再下载此离线资源。"));
    const auto packages = object.value(QStringLiteral("packages")).toArray();
    if (packages.size() != 2) return fail(QStringLiteral("资源依赖清单不完整。"));
    OfflineResourceManifest parsed;
    for (int i = 0; i < packages.size(); ++i) {
        const auto p = packages[i].toObject();
        OfflineResourcePackage item;
        item.id = p.value(QStringLiteral("id")).toString(); item.version = p.value(QStringLiteral("version")).toString();
        item.filename = p.value(QStringLiteral("filename")).toString(); item.url = QUrl(p.value(QStringLiteral("download_url")).toString());
        item.sha256 = p.value(QStringLiteral("sha256")).toString().toLatin1();
        item.size = p.value(QStringLiteral("size_bytes")).toInteger(); item.installedBytes = p.value(QStringLiteral("installed_bytes")).toInteger();
        if (item.id != (i == 0 ? QStringLiteral("base") : QStringLiteral("precise"))
            || !QRegularExpression(QStringLiteral("^[0-9]+\\.[0-9]+\\.[0-9]+$")).match(item.version).hasMatch()
            || !QRegularExpression(QStringLiteral("^[A-Za-z0-9._-]+\\.exe$")).match(item.filename).hasMatch()
            || !isAllowedDownload(item.url) || !QRegularExpression(QStringLiteral("^[a-f0-9]{64}$")).match(QString::fromLatin1(item.sha256)).hasMatch()
            || item.size < 1 || item.size > 2LL * 1024 * 1024 * 1024 || item.installedBytes < item.size || item.installedBytes > 12LL * 1024 * 1024 * 1024
            || p.value(QStringLiteral("format")).toString() != QStringLiteral("nsis-per-user-v1")
            || p.value(QStringLiteral("requires")).toArray() != (i == 0 ? QJsonArray{} : QJsonArray{QStringLiteral("base")}))
            return fail(QStringLiteral("资源来源、依赖或校验字段无效。"));
        parsed.packages.append(item);
    }
    parsed.expiresAt = expires; *result = parsed;
    if (error) error->clear();
    return true;
}
}
