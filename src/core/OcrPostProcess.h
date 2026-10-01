#pragma once

#include <QImage>
#include <QRect>
#include <QSize>
#include <QString>
#include <QVector>

namespace Visnip {

// One recognized text line in source-image pixel coordinates. Produced by
// OcrService, consumed by the translation compositor.
struct OcrTextLine {
    OcrTextLine() = default;
    OcrTextLine(const QRect& recognizedBox,
                const QString& recognizedText,
                float confidence,
                const QRect& originalBox = {},
                bool iconSeparated = false,
                const QString& reason = {},
                int originalIndex = -1)
        : box(recognizedBox)
        , text(recognizedText)
        , score(confidence)
        , detectedBox(originalBox.isValid() ? originalBox : recognizedBox)
        , leadingIconSeparated(iconSeparated)
        , refinementReason(reason)
        , sourceIndex(originalIndex)
    {
    }

    QRect box;
    QString text;
    float score = 0.0f;
    QRect detectedBox;
    bool leadingIconSeparated = false;
    QString refinementReason;
    int sourceIndex = -1;
};

} // namespace Visnip

namespace Visnip::Ocr {

struct DetParams {
    double binaryThreshold = 0.3;   // DB probability-map binarization
    double boxScoreThreshold = 0.6; // mean probability required to keep a region
    double unclipRatio = 1.6;       // DB shrink compensation
    int minMapSidePx = 3;           // reject tiny specks (probability-map pixels)
};

struct RejectedTextLine {
    OcrTextLine line;
    QString reason;
    int lineIndex = -1;
    int peerIndex = -1;
};

// Extracts axis-aligned text-line boxes from a DB probability map.
// prob is a row-major mapH x mapW float map (the det model output). Map
// coordinates are multiplied by scaleToSourceX/Y to land in source-image
// pixels; results are clamped to sourceSize.
QVector<QRect> extractDetBoxes(const float* prob,
                               int mapW,
                               int mapH,
                               double scaleToSourceX,
                               double scaleToSourceY,
                               const QSize& sourceSize,
                               const DetParams& params = {});

// Greedy CTC decoding over a timeSteps x numClasses softmax matrix (the rec
// model output). charset is the dictionary WITHOUT the CTC blank and space:
// class 0 is blank, classes 1..charset.size() map to charset entries, and
// class charset.size()+1 is the space character. Entries are QStrings because
// dictionary glyphs may live outside the BMP. meanScore (optional) receives
// the mean probability of the emitted characters.
QString ctcGreedyDecode(const float* probs,
                        int timeSteps,
                        int numClasses,
                        const QVector<QString>& charset,
                        float* meanScore = nullptr);

// Matches PaddleOCR's Arabic CTC decoder: keep Latin/number runs in their
// internal order while reversing the sequence of RTL characters and runs.
QString reverseArabicPrediction(const QString& text);

struct RefinedOcrBox {
    QRect detectedBox;
    QRect textBox;
    bool leadingIconSeparated = false;
    QString reason;
};

// Recovers a long single-line text region when DB detection emits one normal
// box plus overlapping, vertically compressed fragments for the same baseline.
// Source-pixel continuity determines the full right edge; absorbed fragment
// boxes are removed so the recognizer sees the sentence exactly once.
QVector<QRect> recoverFragmentedLineBoxes(const QVector<QRect>& boxes,
                                          const QImage& source);

// Tightens only the vertical recognition crop around the dominant glyph rows,
// including narrow UI labels such as weekday abbreviations. The original
// detector box remains the line/compositor geometry.
QRect recognitionCropBox(const QRect& textBox, const QImage& source);

// Corrects high-confidence character-shape ambiguities only when surrounding
// words establish an unambiguous UI/technical term (for example Al usage ->
// AI usage). Ordinary names and prose are left unchanged.
QString correctConfusableText(const QString& text);

// Refines detector boxes before recognition. When at least three lines share a
// stable leading component, separator gap, and text column, the leading
// components are treated as navigation icons and excluded from the recognition
// and compositor boxes. detectedBox retains the detector's original geometry.
QVector<RefinedOcrBox> refineRecognitionBoxes(const QVector<QRect>& boxes,
                                              const QImage& source);

// Refines a compact recognized line whose leading whitespace corresponds to an
// isolated icon separated from the decoded label by a visible gap. Returns true
// when line.box was moved to the label while detectedBox retained the full box.
bool refineIsolatedLeadingIcon(OcrTextLine* line, const QImage& source);

// Removes compact low-confidence OCR fragments that overlap or nearly touch a
// longer same-row label. A wider-gap fragment is also removed when source pixels
// prove the same leading shape repeats beside at least two aligned peer labels.
// A low-confidence compact region with one large rounded enclosure and multiple
// independent enclosed marks is rejected as an icon even without a nearby label.
// When detector unclip joins a leading icon and label in one line, source pixels
// are used to split the edge fragment from the text and remove its spurious
// decoded prefix.
QVector<OcrTextLine> filterTranslatableLines(
    const QVector<OcrTextLine>& lines,
    const QImage& source = {},
    QVector<RejectedTextLine>* rejected = nullptr);

// Orders line boxes top-to-bottom, breaking ties left-to-right. Two boxes
// whose vertical centers fall within each other's height are treated as the
// same visual row.
void sortBoxesInReadingOrder(QVector<QRect>& boxes);

} // namespace Visnip::Ocr
