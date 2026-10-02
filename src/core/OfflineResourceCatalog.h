#pragma once
#include <QByteArray>
#include <QString>
#include <QUrl>
#include <QVector>

namespace Visnip {
// One official upstream file of the lite offline tier. Download addresses and
// SHA-256 are pinned in the source; no Visnip server is involved.
struct OfflineResourceFile {
    enum class Install { ExtractZip, Place };
    QString id;
    QString label;
    QVector<QUrl> sources; // tried in order; every source serves identical bytes
    QByteArray sha256;
    qint64 size = 0;
    qint64 installedBytes = 0; // extra disk space once installed
    Install install = Install::Place;
    QString target; // relative to the resource root
};

struct OfflineResourceCatalog {
    static QVector<OfflineResourceFile> liteFiles();
    // The pinned sources and the CDN hosts they redirect to; HTTPS only.
    static bool isAllowedDownload(const QUrl& url);
    static bool acceptsRange(int status, const QByteArray& range, qint64 offset,
                             qint64 total, qint64 length, qint64 requestedEnd = -1);
};
}
