#pragma once
#include <QByteArray>
#include <QString>
#include <QUrl>
#include <QVector>

namespace Visnip {
struct OfflineResourcePackage {
    QString id;
    QString version;
    QString filename;
    QUrl url;
    QByteArray sha256;
    qint64 size = 0;
    qint64 installedBytes = 0;
};
struct OfflineResourceManifest {
    QVector<OfflineResourcePackage> packages;
    qint64 expiresAt = 0;
    static bool parse(const QByteArray& envelope, OfflineResourceManifest* result,
                      QString* error, qint64 now = 0);
    static bool isAllowedDownload(const QUrl& url);
    static bool acceptsRange(int status, const QByteArray& range, qint64 offset,
                             qint64 total, qint64 length, qint64 requestedEnd = -1);
};
}
