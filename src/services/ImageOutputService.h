#pragma once

#include "core/AppConfig.h"

#include <QImage>
#include <QObject>
#include <QString>

namespace Visnip {

// Returns an empty string when the pattern is valid. Settings UI and output
// execution share this entry point so they cannot disagree about validity.
[[nodiscard]] QString outputFilenamePatternError(const QString& pattern);

class ImageOutputService : public QObject {
    Q_OBJECT
public:
    explicit ImageOutputService(AppConfig* config, QObject* parent = nullptr);

    bool copyToClipboard(const QImage& image) const;
    QString saveToDefaultPath(const QImage& image) const;
    QString saveAs(const QImage& image, QWidget* parent = nullptr) const;
    QString makeDefaultPath() const;

private:
    AppConfig* config_ = nullptr;
};

} // namespace Visnip
