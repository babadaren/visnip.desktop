#include "services/OcrService.h"

#include <QCoreApplication>
#include <QFile>
#include <QImageReader>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTextStream>

namespace {

QJsonObject rectJson(const QRect& rect)
{
    return {
        { QStringLiteral("x"), rect.x() },
        { QStringLiteral("y"), rect.y() },
        { QStringLiteral("width"), rect.width() },
        { QStringLiteral("height"), rect.height() },
    };
}

int fail(const QString& message)
{
    QTextStream(stderr) << message << Qt::endl;
    return 2;
}

} // namespace

int main(int argc, char* argv[])
{
    QCoreApplication application(argc, argv);
    const QStringList arguments = application.arguments();
    if (arguments.size() < 4 || arguments.at(1) != QStringLiteral("--output")) {
        return fail(QStringLiteral(
            "Usage: visnip_ocr_batch --output <results.json> [--pack <pack-id>] <image>..."));
    }

    const QString outputPath = arguments.at(2);
    QString packId = QStringLiteral("general-v5");
    int imageArgument = 3;
    if (arguments.size() >= 6 && arguments.at(3) == QStringLiteral("--pack")) {
        packId = arguments.at(4);
        imageArgument = 5;
    }
    if (imageArgument >= arguments.size()) {
        return fail(QStringLiteral("At least one image path is required."));
    }

    QString missingAsset;
    if (!Visnip::OcrService::assetsPresent(packId, &missingAsset)) {
        return fail(QStringLiteral("OCR asset is missing: %1").arg(missingAsset));
    }

    Visnip::OcrService service;
    QJsonArray results;
    bool failed = false;
    for (int index = imageArgument; index < arguments.size(); ++index) {
        const QString imagePath = arguments.at(index);
        QImageReader reader(imagePath);
        reader.setAutoTransform(true);
        const QImage image = reader.read();
        QJsonObject result {
            { QStringLiteral("path"), imagePath },
            { QStringLiteral("packId"), packId },
        };
        if (image.isNull()) {
            result.insert(QStringLiteral("error"), reader.errorString());
            result.insert(QStringLiteral("lines"), QJsonArray {});
            failed = true;
            results.append(result);
            continue;
        }

        QString error;
        const QVector<Visnip::OcrTextLine> lines =
            service.recognizeSync(image, packId, &error);
        QJsonArray lineValues;
        for (qsizetype lineIndex = 0; lineIndex < lines.size(); ++lineIndex) {
            const Visnip::OcrTextLine& line = lines.at(lineIndex);
            lineValues.append(QJsonObject {
                { QStringLiteral("index"), static_cast<qint64>(lineIndex) },
                { QStringLiteral("textBox"), rectJson(line.box) },
                { QStringLiteral("detectedBox"), rectJson(line.detectedBox) },
                { QStringLiteral("recognizedText"), line.text },
                { QStringLiteral("recognitionScore"), line.score },
                { QStringLiteral("leadingIconSeparated"), line.leadingIconSeparated },
                { QStringLiteral("refinementReason"), line.refinementReason },
            });
        }
        result.insert(QStringLiteral("width"), image.width());
        result.insert(QStringLiteral("height"), image.height());
        result.insert(QStringLiteral("error"), error);
        result.insert(QStringLiteral("lines"), lineValues);
        failed = failed || !error.isEmpty();
        results.append(result);
    }

    QFile output(outputPath);
    if (!output.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        return fail(QStringLiteral("Cannot write %1: %2")
                        .arg(outputPath, output.errorString()));
    }
    output.write(QJsonDocument(QJsonObject {
        { QStringLiteral("formatVersion"), 1 },
        { QStringLiteral("results"), results },
    }).toJson(QJsonDocument::Indented));
    return failed ? 1 : 0;
}
