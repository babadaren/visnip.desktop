#include "services/ImageOutputService.h"

#include "core/OutputFilenamePattern.h"
#include "core/PerfLog.h"

#include <QApplication>
#include <QClipboard>
#include <QDateTime>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QSaveFile>

namespace {

constexpr int kMaxCollisionSuffix = 10000;

bool saveImageAtomically(const QImage& image, const QString& path, QByteArray format)
{
    if (format.isEmpty()) {
        format = QFileInfo(path).suffix().toLatin1();
    }
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly)) {
        return false;
    }
    if (!image.save(&file, format.constData())) {
        file.cancelWriting();
        return false;
    }
    return file.commit();
}

QString availablePath(const QString& requestedPath)
{
    if (!QFileInfo::exists(requestedPath)) {
        return requestedPath;
    }
    const QFileInfo info(requestedPath);
    const QString suffix = info.suffix();
    const QString extension = suffix.isEmpty() ? QString() : QStringLiteral(".%1").arg(suffix);
    const QString basePath = info.dir().filePath(info.completeBaseName());
    for (int index = 2; index < kMaxCollisionSuffix; ++index) {
        const QString candidate = QStringLiteral("%1_%2%3").arg(basePath).arg(index).arg(extension);
        if (!QFileInfo::exists(candidate)) {
            return candidate;
        }
    }
    return {};
}

} // namespace

namespace Visnip {

QString outputFilenamePatternError(const QString& pattern)
{
    return validateOutputFilenamePattern(pattern).message;
}

ImageOutputService::ImageOutputService(AppConfig* config, QObject* parent)
    : QObject(parent)
    , config_(config)
{
}

bool ImageOutputService::copyToClipboard(const QImage& image) const
{
    if (image.isNull()) {
        return false;
    }
    QApplication::clipboard()->setImage(image);
    return true;
}

QString ImageOutputService::makeDefaultPath() const
{
    const auto& settings = config_->settings().output;
    const auto pattern = validateOutputFilenamePattern(settings.filenamePattern);
    if (!pattern.isValid()) {
        Perf::log(QStringLiteral("ImageOutput.invalid_filename_pattern error=\"%1\"")
                      .arg(pattern.message));
        return {};
    }

    QDir dir(settings.saveDirectory.isEmpty() ? config_->defaultSaveDirectory() : settings.saveDirectory);
    if (!dir.exists() && !dir.mkpath(QStringLiteral("."))) {
        return {};
    }
    const QString stamp = QDateTime::currentDateTime().toString(QStringLiteral("yyyy-MM-dd_HH-mm-ss"));
    const QString name = pattern.normalizedPattern.arg(stamp);
    return availablePath(dir.filePath(name));
}

QString ImageOutputService::saveToDefaultPath(const QImage& image) const
{
    if (image.isNull()) {
        return {};
    }
    const QString path = makeDefaultPath();
    if (path.isEmpty() || !saveImageAtomically(image, path, {})) {
        Perf::log(QStringLiteral("ImageOutput.save_default_failed path=%1").arg(path));
        return {};
    }
    return path;
}

QString ImageOutputService::saveAs(const QImage& image, QWidget* parent) const
{
    if (image.isNull()) {
        return {};
    }
    QString path = QFileDialog::getSaveFileName(parent,
                                                QStringLiteral("保存截图"),
                                                makeDefaultPath(),
                                                QStringLiteral("PNG 图片 (*.png);;JPEG 图片 (*.jpg *.jpeg)"));
    if (path.isEmpty()) {
        return {};
    }
    if (QFileInfo(path).suffix().isEmpty()) {
        path += QStringLiteral(".png");
    }
    if (!saveImageAtomically(image, path, QFileInfo(path).suffix().toLatin1())) {
        Perf::log(QStringLiteral("ImageOutput.save_as_failed path=%1").arg(path));
        return {};
    }
    return path;
}

} // namespace Visnip
